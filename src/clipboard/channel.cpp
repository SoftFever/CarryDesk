/**
 * @file src/clipboard/channel.cpp
 * @brief Definitions for the host side of one clipboard channel.
 */
// standard includes
#include <algorithm>
#include <limits>
#include <utility>

// local includes
#include "channel.h"

namespace clipboard {
  namespace {
    constexpr std::size_t pull_chunk = 256 * 1024;  ///< Most bytes asked from a producer at once.
    constexpr std::uint64_t credit_batch = 256 * 1024;  ///< Consumed bytes collected before sending CREDIT.

    /**
     * @brief Check whether a type is a request that opens a stream.
     *
     * @param type Message type.
     * @return `true` for GET, LOCK, GET_FILE_LIST and GET_FILE.
     */
    bool is_request(const msg_e type) {
      return type == msg_e::get || type == msg_e::lock || type == msg_e::get_file_list || type == msg_e::get_file;
    }

    /**
     * @brief Check whether a type travels only on stream 0.
     *
     * @param type Message type.
     * @return `true` for channel-level messages.
     */
    bool is_channel_message(const msg_e type) {
      switch (type) {
        case msg_e::hello:
        case msg_e::hello_reply:
        case msg_e::announce:
        case msg_e::announce_ack:
        case msg_e::clear:
        case msg_e::unlock:
        case msg_e::ping:
        case msg_e::pong:
          return true;
        default:
          return false;
      }
    }

    /**
     * @brief Check whether a list contains a value.
     *
     * @param list List to search.
     * @param value Value to find.
     * @return `true` when found.
     */
    bool contains(const std::vector<std::string> &list, const std::string_view value) {
      return std::ranges::find(list, value) != list.end();
    }
  }  // namespace

  channel_t::channel_t(const std::uint64_t id, host_settings_t settings, channel_delegate_t &delegate, clock_fn now):
      id_ {id},
      settings_ {std::move(settings)},
      delegate_ {delegate},
      now_ {std::move(now)} {
    opened_ = last_received_ = last_ping_ = now_();
  }

  void channel_t::feed(const std::string_view bytes) {
    if (state_ != state_e::handshake && state_ != state_e::open) {
      return;
    }
    decoder_.feed(bytes);
    frame_t frame;
    while (state_ == state_e::handshake || state_ == state_e::open) {
      const auto result = decoder_.next(frame);
      if (result == frame_decoder_t::result_e::need_more) {
        return;
      }
      if (result == frame_decoder_t::result_e::bad_length) {
        fail(error_e::limit_exceeded, "invalid frame length");
        return;
      }
      last_received_ = now_();
      handle_frame(frame);
    }
  }

  void channel_t::handle_frame(frame_t &frame) {
    const auto type = static_cast<msg_e>(frame.type);
    if (is_channel_message(type) && frame.stream_id != 0) {
      fail(error_e::bad_request, "channel message on a stream");
      return;
    }
    if (state_ == state_e::handshake) {
      if (type == msg_e::hello) {
        handle_hello(frame);
      } else {
        fail(error_e::bad_request, "expected HELLO");
      }
      return;
    }
    switch (type) {
      case msg_e::hello:
        fail(error_e::bad_request, "second HELLO");
        return;
      case msg_e::announce:
        handle_announce(frame);
        return;
      case msg_e::announce_ack:
        if (!parse_announce_ack(frame.payload)) {
          fail(error_e::bad_request, "malformed ANNOUNCE_ACK");
        }
        return;
      case msg_e::clear:
        handle_clear(frame);
        return;
      case msg_e::get:
      case msg_e::lock:
      case msg_e::get_file_list:
      case msg_e::get_file:
        handle_request(frame);
        return;
      case msg_e::lock_reply:
      case msg_e::data:
        handle_response(frame);
        return;
      case msg_e::error:
        if (frame.stream_id == 0) {
          enter_closed(false);
        } else {
          handle_response(frame);
        }
        return;
      case msg_e::credit:
        handle_credit(frame);
        return;
      case msg_e::cancel:
        handle_cancel(frame);
        return;
      case msg_e::unlock:
        handle_unlock(frame);
        return;
      case msg_e::ping:
        if (const auto nonce = parse_single(frame.payload, "nonce")) {
          send_control(msg_e::pong, 0, 0, serialize_single("nonce", *nonce));
        } else {
          fail(error_e::bad_request, "malformed PING");
        }
        return;
      case msg_e::pong:
        if (!parse_single(frame.payload, "nonce")) {
          fail(error_e::bad_request, "malformed PONG");
        }
        return;
      default:
        // HELLO_REPLY travels the other way and unknown types are ignored for forward compatibility.
        return;
    }
  }

  void channel_t::handle_hello(const frame_t &frame) {
    const auto hello = parse_hello(frame.payload);
    if (!hello) {
      fail(error_e::bad_request, "malformed HELLO");
      return;
    }
    if (hello->version != protocol_version) {
      fail(error_e::bad_request, "unsupported version");
      return;
    }
    client_os_ = hello->os;
    policy_ = hello->policy & settings_.policy;
    for (const auto &format : settings_.formats) {
      if (contains(hello->formats, format)) {
        formats_.push_back(format);
      }
    }
    limits_.max_item_size = std::min({hello->max_item_size.value_or(settings_.limits.max_item_size), settings_.limits.max_item_size, limit::max_item_size});
    limits_.max_transfer_size = std::min({hello->max_transfer_size.value_or(settings_.limits.max_transfer_size), settings_.limits.max_transfer_size, limit::max_transfer_size});
    state_ = state_e::open;

    hello_reply_t reply;
    reply.os = settings_.os;
    reply.formats = formats_;
    reply.policy = policy_;
    reply.limits = limits_;
    std::optional<host_clipboard_t> clipboard;
    if (policy_.host_to_client) {
      clipboard = delegate_.current_host_clipboard(*this);
      reply.host_clipboard = clipboard && !transferable(clipboard->formats).empty();
    }
    send_control(msg_e::hello_reply, 0, 0, serialize(reply));
    if (reply.host_clipboard) {
      announce_host(*clipboard);
    }
    delegate_.on_open(*this);
  }

  std::vector<std::string> channel_t::transferable(const std::vector<std::string> &formats) const {
    std::vector<std::string> result;
    for (const auto &format : formats) {
      if (contains(formats_, format) && (format != mime::files || policy_.files) && !contains(result, format)) {
        result.push_back(format);
      }
    }
    return result;
  }

  void channel_t::handle_announce(const frame_t &frame) {
    const auto announce = parse_announce(frame.payload);
    if (!announce) {
      fail(error_e::bad_request, "malformed ANNOUNCE");
      return;
    }
    const auto generation = announce->generation;
    if (generation <= client_generation_) {
      send_control(msg_e::announce_ack, 0, 0, serialize(announce_ack_t {generation, std::string {to_string(error_e::bad_request)}}));
      return;
    }
    client_generation_ = generation;
    client_cleared_ = false;
    for (const auto pending : pending_acks_) {
      send_control(msg_e::announce_ack, 0, 0, serialize(announce_ack_t {pending, std::string {to_string(error_e::stale_generation)}}));
    }
    pending_acks_.clear();

    const auto too_many = announce->formats.size() > limit::max_formats;
    const auto too_long = std::ranges::any_of(announce->formats, [](const format_t &format) {
      return format.mime.size() > limit::max_mime_length;
    });
    if (too_many || too_long) {
      send_control(msg_e::announce_ack, 0, 0, serialize(announce_ack_t {generation, std::string {to_string(error_e::bad_request)}}));
      return;
    }
    if (!policy_.client_to_host) {
      send_control(msg_e::announce_ack, 0, 0, serialize(announce_ack_t {generation, std::string {to_string(error_e::not_permitted)}}));
      return;
    }

    std::vector<format_t> formats;
    for (const auto &format : announce->formats) {
      const auto allowed = contains(formats_, format.mime) && (format.mime != mime::files || policy_.files);
      const auto duplicate = std::ranges::any_of(formats, [&](const format_t &kept) {
        return kept.mime == format.mime;
      });
      if (allowed && !duplicate) {
        formats.push_back(format);
      }
    }
    pending_acks_.insert(generation);
    delegate_.on_client_announce(*this, generation, formats);
  }

  void channel_t::handle_clear(const frame_t &frame) {
    const auto generation = parse_single(frame.payload, "generation");
    if (!generation) {
      fail(error_e::bad_request, "malformed CLEAR");
      return;
    }
    if (*generation == 0 || *generation != client_generation_ || client_cleared_) {
      return;
    }
    client_cleared_ = true;
    for (const auto pending : pending_acks_) {
      send_control(msg_e::announce_ack, 0, 0, serialize(announce_ack_t {pending, std::string {to_string(error_e::stale_generation)}}));
    }
    pending_acks_.clear();
    delegate_.on_client_clear(*this, *generation);
  }

  bool channel_t::claim_stream_id(const std::uint32_t stream) {
    if (stream % 2 == 0) {
      return false;
    }
    auto next = client_ids_.upper_bound(stream);
    if (next != client_ids_.begin()) {
      const auto previous = std::prev(next);
      if (stream <= previous->second) {
        return false;
      }
      if (previous->second + 2 == stream) {
        previous->second = stream;
        if (next != client_ids_.end() && next->first == stream + 2) {
          previous->second = next->second;
          client_ids_.erase(next);
        }
        return true;
      }
    }
    if (next != client_ids_.end() && next->first == stream + 2) {
      const auto last = next->second;
      client_ids_.erase(next);
      client_ids_.emplace(stream, last);
      return true;
    }
    client_ids_.emplace(stream, stream);
    return true;
  }

  void channel_t::handle_request(const frame_t &frame) {
    const auto stream = frame.stream_id;
    if (!claim_stream_id(stream)) {
      fail(error_e::bad_request, "invalid request stream id");
      return;
    }
    if (served_.size() >= limit::max_streams) {
      send_error(stream, error_e::busy, "too many concurrent requests");
      return;
    }
    auto &served = served_[stream];
    served.request = static_cast<msg_e>(frame.type);
    served.progress = now_();
    switch (served.request) {
      case msg_e::get:
        serve_get(stream, frame.payload);
        return;
      case msg_e::lock:
        serve_lock(stream, frame.payload);
        return;
      case msg_e::get_file_list:
        serve_file_list(stream, frame.payload);
        return;
      default:
        serve_file(stream, frame.payload);
        return;
    }
  }

  void channel_t::serve_get(const std::uint32_t stream, const std::string &payload) {
    const auto get = parse_get(payload);
    if (!get) {
      respond_error(stream, error_e::bad_request, "malformed GET");
    } else if (!policy_.host_to_client) {
      respond_error(stream, error_e::not_permitted, "host to client is not allowed");
    } else if (get->generation != host_generation_ || !host_live_) {
      respond_error(stream, error_e::stale_generation, "not the current generation");
    } else if (get->mime == mime::files || !contains(host_formats_, get->mime)) {
      respond_error(stream, error_e::unsupported_format, "format not announced");
    } else {
      delegate_.serve_get(*this, stream, host_revision_, get->mime);
    }
  }

  void channel_t::serve_lock(const std::uint32_t stream, const std::string &payload) {
    const auto generation = parse_single(payload, "generation");
    if (!generation) {
      respond_error(stream, error_e::bad_request, "malformed LOCK");
    } else if (!policy_.host_to_client || !policy_.files) {
      respond_error(stream, error_e::not_permitted, "files are not allowed");
    } else if (*generation != host_generation_ || !host_live_) {
      respond_error(stream, error_e::stale_generation, "not the current generation");
    } else if (!contains(host_formats_, mime::files)) {
      respond_error(stream, error_e::unsupported_format, "files not announced");
    } else if (leases_.size() >= limit::max_leases) {
      respond_error(stream, error_e::busy, "too many leases");
    } else {
      delegate_.serve_lock(*this, stream, host_revision_);
    }
  }

  void channel_t::serve_file_list(const std::uint32_t stream, const std::string &payload) {
    const auto lease_id = parse_single(payload, "lease");
    if (!lease_id) {
      respond_error(stream, error_e::bad_request, "malformed GET_FILE_LIST");
      return;
    }
    auto *lease = find_lease(*lease_id);
    if (!lease) {
      respond_error(stream, error_e::unknown_lease, "unknown lease");
      return;
    }
    lease->used = now_();
    std::string body;
    for (const auto &entry : lease->manifest->entries) {
      body += serialize(entry);
    }
    respond_bytes(stream, std::move(body));
  }

  void channel_t::serve_file(const std::uint32_t stream, const std::string &payload) {
    const auto request = parse_get_file(payload);
    if (!request) {
      respond_error(stream, error_e::bad_request, "malformed GET_FILE");
      return;
    }
    auto *lease = find_lease(request->lease);
    if (!lease || request->index >= lease->manifest->entries.size()) {
      respond_error(stream, error_e::unknown_lease, "unknown lease or index");
      return;
    }
    const auto &entry = lease->manifest->entries[request->index];
    if (entry.dir || request->offset > entry.size || request->length > entry.size - request->offset) {
      respond_error(stream, error_e::bad_request, "range outside the file");
      return;
    }
    lease->used = now_();
    served_[stream].exact = request->length;
    if (request->length == 0) {
      respond_bytes(stream, {});
      return;
    }
    delegate_.serve_file(*this, stream, lease->manifest, request->index, request->offset, request->length);
  }

  channel_t::lease_t *channel_t::find_lease(const std::uint64_t lease) {
    const auto it = leases_.find(lease);
    return it == leases_.end() ? nullptr : &it->second;
  }

  void channel_t::handle_response(const frame_t &frame) {
    const auto stream = frame.stream_id;
    auto it = requested_.find(stream);
    if (it == requested_.end() || !it->second.sent) {
      return;
    }
    auto &request = it->second;
    request.progress = now_();
    const auto type = static_cast<msg_e>(frame.type);
    if (type == msg_e::error) {
      const auto error = parse_error(frame.payload);
      const auto code = error ? error_from_string(error->code).value_or(error_e::io_error) : error_e::io_error;
      fail_requested(stream, code, error ? error->message : std::string {}, false);
      return;
    }
    if ((type == msg_e::lock_reply) != (request.request == msg_e::lock)) {
      fail(error_e::bad_request, "unexpected response type");
      return;
    }
    if (type == msg_e::lock_reply) {
      auto callbacks = std::move(request.callbacks);
      requested_.erase(it);
      send_queued_requests();
      callbacks.on_data(frame.payload);
      if (state_ == state_e::open) {
        callbacks.on_end();
      }
      return;
    }
    const auto size = frame.payload.size();
    if (size > request.window) {
      fail(error_e::limit_exceeded, "DATA beyond granted credit");
      return;
    }
    request.window -= size;
    request.received += size;
    if (request.options.max_bytes && request.received > *request.options.max_bytes) {
      fail_requested(stream, error_e::too_large, "response exceeds the limit", true);
      return;
    }
    if (request.options.exact_bytes && request.received > *request.options.exact_bytes) {
      fail_requested(stream, error_e::io_error, "response longer than requested", true);
      return;
    }
    const auto auto_credit = request.options.auto_credit;
    if (size > 0) {
      auto on_data = request.callbacks.on_data;
      on_data(frame.payload);
      it = requested_.find(stream);
      if (it == requested_.end() || state_ != state_e::open) {
        return;
      }
      if (auto_credit) {
        grant(stream, size);
      }
    }
    if (frame.end()) {
      auto &finished = it->second;
      if (finished.options.exact_bytes && finished.received != *finished.options.exact_bytes) {
        fail_requested(stream, error_e::io_error, "response shorter than requested", false);
        return;
      }
      auto callbacks = std::move(finished.callbacks);
      requested_.erase(it);
      send_queued_requests();
      callbacks.on_end();
    }
  }

  void channel_t::handle_credit(const frame_t &frame) {
    const auto bytes = parse_single(frame.payload, "bytes");
    if (!bytes) {
      fail(error_e::bad_request, "malformed CREDIT");
      return;
    }
    const auto it = served_.find(frame.stream_id);
    if (it == served_.end()) {
      return;
    }
    it->second.credit = std::min(it->second.credit + *bytes, limit::max_json_integer);
    pump();
  }

  void channel_t::handle_cancel(const frame_t &frame) {
    finish_served(frame.stream_id, true);
  }

  void channel_t::handle_unlock(const frame_t &frame) {
    const auto lease = parse_single(frame.payload, "lease");
    if (!lease) {
      fail(error_e::bad_request, "malformed UNLOCK");
      return;
    }
    if (const auto it = leases_.find(*lease); it != leases_.end()) {
      release_lease(it);
    }
  }

  void channel_t::tick() {
    const auto now = now_();
    if (state_ == state_e::handshake) {
      if (now - opened_ >= timing::hello_timeout) {
        fail(error_e::bad_request, "HELLO timeout");
      }
      return;
    }
    if (state_ != state_e::open) {
      return;
    }
    if (now - last_received_ >= timing::idle_timeout) {
      enter_closed(false);
      return;
    }
    if (now - last_ping_ >= timing::ping_interval) {
      last_ping_ = now;
      send_control(msg_e::ping, 0, 0, serialize_single("nonce", ++ping_nonce_));
    }

    std::vector<std::uint32_t> stalled;
    for (const auto &[stream, served] : served_) {
      if (now - served.progress >= timing::stream_timeout) {
        stalled.push_back(stream);
      }
    }
    for (const auto stream : stalled) {
      send_error(stream, error_e::io_error, "no progress");
      finish_served(stream, true);
    }

    stalled.clear();
    for (const auto &[stream, request] : requested_) {
      if (request.sent && now - request.progress >= timing::stream_timeout) {
        stalled.push_back(stream);
      }
    }
    for (const auto stream : stalled) {
      if (state_ != state_e::open) {
        return;
      }
      fail_requested(stream, error_e::io_error, "no progress", true);
    }

    for (auto it = leases_.begin(); it != leases_.end() && state_ == state_e::open;) {
      if (now - it->second.used >= timing::lease_timeout) {
        const auto expired = it++;
        release_lease(expired);
      } else {
        ++it;
      }
    }
  }

  std::string channel_t::take_output(const std::size_t max) {
    std::string out;
    while (!control_.empty() && (out.empty() || out.size() + control_.front().size() <= max)) {
      out += control_.front();
      control_.pop_front();
    }
    bool took_data = false;
    while (!data_.empty() && (out.empty() || out.size() + data_.front().size() <= max)) {
      data_bytes_ -= data_.front().size();
      out += data_.front();
      data_.pop_front();
      took_data = true;
    }
    if (took_data) {
      pump();
    }
    return out;
  }

  bool channel_t::has_output() const {
    return !control_.empty() || !data_.empty();
  }

  void channel_t::close() {
    enter_closed(false);
  }

  void channel_t::fail(const error_e error, const std::string &message) {
    if (state_ != state_e::handshake && state_ != state_e::open) {
      return;
    }
    send_control(msg_e::error, 0, 0, serialize(error_msg_t {std::string {to_string(error)}, message}));
    enter_closed(true);
  }

  std::uint64_t channel_t::announce_host(const host_clipboard_t &clipboard) {
    if (state_ != state_e::open || !policy_.host_to_client) {
      return 0;
    }
    auto formats = transferable(clipboard.formats);
    if (formats.empty()) {
      if (host_live_) {
        host_live_ = false;
        send_control(msg_e::clear, 0, 0, serialize_single("generation", host_generation_));
      }
      return 0;
    }
    ++host_generation_;
    host_revision_ = clipboard.revision;
    host_live_ = true;
    announce_t announce {host_generation_, {}};
    for (const auto &format : formats) {
      announce.formats.push_back({format, std::nullopt});
    }
    host_formats_ = std::move(formats);
    send_control(msg_e::announce, 0, 0, serialize(announce));
    return host_generation_;
  }

  void channel_t::clear_host() {
    if (state_ != state_e::open) {
      return;
    }
    host_live_ = false;
    send_control(msg_e::clear, 0, 0, serialize_single("generation", host_generation_));
  }

  void channel_t::host_superseded() {
    host_live_ = false;
  }

  void channel_t::ack_client(const std::uint64_t generation, const std::string_view status) {
    if (state_ != state_e::open || pending_acks_.erase(generation) == 0) {
      return;
    }
    send_control(msg_e::announce_ack, 0, 0, serialize(announce_ack_t {generation, std::string {status}}));
  }

  void channel_t::respond_bytes(const std::uint32_t stream, std::string bytes) {
    const auto it = served_.find(stream);
    if (it == served_.end() || it->second.answered) {
      return;
    }
    auto &served = it->second;
    if (served.exact && bytes.size() != *served.exact) {
      respond_error(stream, error_e::io_error, "file size changed");
      return;
    }
    served.answered = true;
    served.complete = true;
    served.buffer = std::move(bytes);
    served.progress = now_();
    pump();
  }

  void channel_t::respond_stream(const std::uint32_t stream, std::function<void(std::size_t)> pull) {
    const auto it = served_.find(stream);
    if (it == served_.end() || it->second.answered) {
      return;
    }
    it->second.answered = true;
    it->second.pull = std::move(pull);
    it->second.progress = now_();
    pump();
  }

  void channel_t::respond_data(const std::uint32_t stream, std::string bytes, const bool eof) {
    const auto it = served_.find(stream);
    if (it == served_.end() || !it->second.pull) {
      return;
    }
    auto &served = it->second;
    served.pull_pending = false;
    const auto total = served.sent + (served.buffer.size() - served.offset) + bytes.size();
    if (served.exact && (total > *served.exact || (eof && total != *served.exact))) {
      respond_error(stream, error_e::io_error, "file size changed");
      return;
    }
    served.buffer += bytes;
    served.complete = eof || (served.exact && total == *served.exact);
    served.progress = now_();
    pump();
  }

  void channel_t::respond_error(const std::uint32_t stream, const error_e error, const std::string &message) {
    if (!served_.contains(stream)) {
      return;
    }
    send_error(stream, error, message);
    finish_served(stream, false);
  }

  void channel_t::grant_lease(const std::uint32_t stream, std::shared_ptr<const manifest_t> manifest) {
    if (!served_.contains(stream)) {
      return;
    }
    if (leases_.size() >= limit::max_leases) {
      respond_error(stream, error_e::busy, "too many leases");
      return;
    }
    const auto lease = next_lease_++;
    const lock_reply_t reply {lease, manifest->entries.size(), manifest->total_bytes};
    leases_[lease] = {std::move(manifest), now_()};
    send_control(msg_e::lock_reply, flag_end, stream, serialize(reply));
    finish_served(stream, false);
  }

  std::uint32_t channel_t::request(const msg_e type, std::string payload, request_options_t options, request_callbacks_t callbacks) {
    if (state_ != state_e::open) {
      return 0;
    }
    const auto stream = next_stream_;
    next_stream_ += 2;
    auto &request = requested_[stream];
    request.request = type;
    request.options = std::move(options);
    request.callbacks = std::move(callbacks);
    queued_requests_.push_back({stream, type, std::move(payload)});
    send_queued_requests();
    return stream;
  }

  void channel_t::send_queued_requests() {
    while (state_ == state_e::open && !queued_requests_.empty() && requested_.size() - queued_requests_.size() < limit::max_streams) {
      auto queued = std::move(queued_requests_.front());
      queued_requests_.pop_front();
      auto &request = requested_.at(queued.stream);
      request.sent = true;
      request.progress = now_();
      send_control(queued.request, 0, queued.stream, queued.payload);
    }
  }

  void channel_t::grant(const std::uint32_t stream, const std::uint64_t bytes) {
    const auto it = requested_.find(stream);
    if (state_ != state_e::open || it == requested_.end() || !it->second.sent || bytes == 0) {
      return;
    }
    auto &request = it->second;
    request.unacknowledged += bytes;
    if (request.unacknowledged >= credit_batch || request.window < credit_batch) {
      request.window += request.unacknowledged;
      send_control(msg_e::credit, 0, stream, serialize_single("bytes", request.unacknowledged));
      request.unacknowledged = 0;
    }
  }

  void channel_t::cancel_request(const std::uint32_t stream) {
    const auto it = requested_.find(stream);
    if (it == requested_.end()) {
      return;
    }
    if (it->second.sent) {
      send_control(msg_e::cancel, 0, stream, "{}");
    } else {
      std::erase_if(queued_requests_, [stream](const queued_t &queued) {
        return queued.stream == stream;
      });
    }
    requested_.erase(it);
    send_queued_requests();
  }

  void channel_t::unlock(const std::uint64_t lease) {
    if (state_ == state_e::open) {
      send_control(msg_e::unlock, 0, 0, serialize_single("lease", lease));
    }
  }

  void channel_t::send_control(const msg_e type, const std::uint16_t flags, const std::uint32_t stream, const std::string_view payload) {
    control_.push_back(encode_frame(type, flags, stream, payload));
    notify_output();
  }

  void channel_t::send_error(const std::uint32_t stream, const error_e error, const std::string &message) {
    send_control(msg_e::error, flag_end, stream, serialize(error_msg_t {std::string {to_string(error)}, message}));
  }

  void channel_t::finish_served(const std::uint32_t stream, const bool cancelled) {
    if (served_.erase(stream) > 0 && cancelled) {
      delegate_.cancel_serving(*this, stream);
    }
  }

  void channel_t::fail_requested(const std::uint32_t stream, const error_e error, const std::string &message, const bool send_cancel) {
    const auto it = requested_.find(stream);
    if (it == requested_.end()) {
      return;
    }
    if (send_cancel) {
      send_control(msg_e::cancel, 0, stream, "{}");
    }
    auto callbacks = std::move(it->second.callbacks);
    requested_.erase(it);
    send_queued_requests();
    callbacks.on_error(error, message);
  }

  void channel_t::pump() {
    if (pumping_ || state_ != state_e::open) {
      return;
    }
    pumping_ = true;
    for (auto it = served_.begin(); it != served_.end();) {
      const auto stream = it->first;
      auto &served = it->second;
      bool finished = false;
      while (true) {
        const auto pending = served.buffer.size() - served.offset;
        if (pending > 0) {
          if (served.credit == 0 || data_bytes_ >= limit::write_queue) {
            break;
          }
          const auto size = static_cast<std::size_t>(std::min<std::uint64_t>({pending, served.credit, limit::max_payload}));
          const auto last = served.complete && size == pending;
          data_.push_back(encode_frame(msg_e::data, last ? flag_end : 0, stream, std::string_view {served.buffer}.substr(served.offset, size)));
          data_bytes_ += data_.back().size();
          served.offset += size;
          served.credit -= size;
          served.sent += size;
          served.progress = now_();
          if (last) {
            finished = true;
            break;
          }
          continue;
        }
        served.buffer.clear();
        served.offset = 0;
        if (served.complete) {
          data_.push_back(encode_frame(msg_e::data, flag_end, stream, {}));
          data_bytes_ += data_.back().size();
          finished = true;
        } else if (served.pull && !served.pull_pending && served.credit > 0 && data_bytes_ < limit::write_queue) {
          auto want = std::min<std::uint64_t>({served.credit, limit::write_queue - data_bytes_, pull_chunk});
          if (served.exact) {
            want = std::min(want, *served.exact - served.sent);
          }
          served.pull_pending = true;
          served.pull(static_cast<std::size_t>(want));
        }
        break;
      }
      it = finished ? served_.erase(it) : std::next(it);
    }
    pumping_ = false;
    notify_output();
  }

  void channel_t::release_lease(const std::map<std::uint64_t, lease_t>::iterator it) {
    auto manifest = std::move(it->second.manifest);
    leases_.erase(it);
    delegate_.on_lease_released(*this, manifest);
  }

  void channel_t::enter_closed(const bool flush) {
    if (state_ == state_e::closing || state_ == state_e::closed) {
      return;
    }
    state_ = flush ? state_e::closing : state_e::closed;
    data_.clear();
    data_bytes_ = 0;
    queued_requests_.clear();
    if (!flush) {
      control_.clear();
    }

    auto served = std::move(served_);
    served_.clear();
    for (const auto &[stream, ignored] : served) {
      delegate_.cancel_serving(*this, stream);
    }
    auto requested = std::move(requested_);
    requested_.clear();
    for (auto &[stream, request] : requested) {
      request.callbacks.on_error(error_e::io_error, "channel closed");
    }
    auto leases = std::move(leases_);
    leases_.clear();
    for (auto &[lease, entry] : leases) {
      delegate_.on_lease_released(*this, entry.manifest);
    }
    pending_acks_.clear();
    delegate_.on_closed(*this);
    notify_output();
  }

  void channel_t::notify_output() {
    if (output_callback_ && (has_output() || wants_close())) {
      output_callback_();
    }
  }
}  // namespace clipboard
