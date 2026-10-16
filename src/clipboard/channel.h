/**
 * @file src/clipboard/channel.h
 * @brief Declarations for the host side of one clipboard channel.
 * @details The channel is a protocol state machine without I/O: received bytes go in through `feed`,
 *          bytes to send come out of `take_output`, and time comes from an injected clock. Clipboard
 *          operations are delegated to a `channel_delegate_t`, which answers asynchronously through
 *          the channel's `respond_*` and `ack_client` methods. All calls happen on one thread.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// local includes
#include "protocol.h"

namespace clipboard {
  using time_point = std::chrono::steady_clock::time_point;  ///< Time used by the channel's timers.
  using clock_fn = std::function<time_point()>;  ///< Source of the current time.

  /**
   * @brief Host settings that bound what a channel may negotiate.
   */
  struct host_settings_t {
    policy_t policy;  ///< Host global setting intersected with the client's permission.
    limits_t limits;  ///< Host's configured limits, already within the hard caps.
    std::vector<std::string> formats;  ///< Canonical formats the host backend supports.
    std::string os;  ///< Host operating system name sent in HELLO_REPLY.
  };

  /**
   * @brief The host clipboard as the channel announces it.
   */
  struct host_clipboard_t {
    std::uint64_t revision = 0;  ///< Native clipboard revision the formats were read from.
    std::vector<std::string> formats;  ///< Canonical formats available; empty when nothing is.
  };

  /**
   * @brief A file manifest pinned by a lease.
   */
  struct manifest_t {
    std::vector<file_entry_t> entries;  ///< Entries in manifest order.
    std::uint64_t total_bytes = 0;  ///< Summed file sizes.
    std::shared_ptr<void> context;  ///< Data the delegate needs to serve the files, such as opened handles.
  };

  /**
   * @brief Callbacks of a request the host sends to the client.
   * @details Exactly one of `on_end` and `on_error` is called, unless the request is cancelled by the
   *          host. Callbacks are never called after the channel closed them with `on_error`.
   */
  struct request_callbacks_t {
    std::function<void(std::string_view)> on_data;  ///< Payload of each DATA frame, or the LOCK_REPLY payload.
    std::function<void()> on_end;  ///< The response ended successfully.
    std::function<void(error_e, const std::string &)> on_error;  ///< The response failed.
  };

  /**
   * @brief Options of a request the host sends to the client.
   */
  struct request_options_t {
    std::optional<std::uint64_t> max_bytes;  ///< Response larger than this fails with `too_large`.
    std::optional<std::uint64_t> exact_bytes;  ///< Response of another size fails with `io_error`.
    bool auto_credit = true;  ///< Grant credit as soon as DATA is delivered; otherwise call `grant`.
  };

  class channel_t;

  /**
   * @brief Host operations a channel needs.
   */
  class channel_delegate_t {
  public:
    virtual ~channel_delegate_t() = default;

    /**
     * @brief Get the host clipboard to announce when the channel opens.
     *
     * @param channel Channel being opened.
     * @return The current host clipboard, or no value when the host does not own transferable content.
     */
    virtual std::optional<host_clipboard_t> current_host_clipboard(channel_t &channel) = 0;

    /**
     * @brief The handshake completed.
     *
     * @param channel Channel now open.
     */
    virtual void on_open(channel_t &channel) = 0;

    /**
     * @brief The client announced a new generation that passed validation.
     * @details The delegate installs it and answers with `channel_t::ack_client`.
     *
     * @param channel Announcing channel.
     * @param generation Client's generation.
     * @param formats Announced formats within the effective formats.
     */
    virtual void on_client_announce(channel_t &channel, std::uint64_t generation, const std::vector<format_t> &formats) = 0;

    /**
     * @brief The client cleared its current generation.
     *
     * @param channel Clearing channel.
     * @param generation Cleared generation.
     */
    virtual void on_client_clear(channel_t &channel, std::uint64_t generation) = 0;

    /**
     * @brief The client requested one format of the host's current generation.
     * @details The delegate answers with `respond_bytes` or `respond_error`.
     *
     * @param channel Requesting channel.
     * @param stream Response stream.
     * @param revision Native revision of the requested generation.
     * @param mime Requested canonical format.
     */
    virtual void serve_get(channel_t &channel, std::uint32_t stream, std::uint64_t revision, const std::string &mime) = 0;

    /**
     * @brief The client requested a lease on the host's current files.
     * @details The delegate answers with `grant_lease` or `respond_error`.
     *
     * @param channel Requesting channel.
     * @param stream Response stream.
     * @param revision Native revision of the requested generation.
     */
    virtual void serve_lock(channel_t &channel, std::uint32_t stream, std::uint64_t revision) = 0;

    /**
     * @brief The client requested a byte range of a leased file.
     * @details The delegate answers with `respond_stream` and `respond_data`, or `respond_error`.
     *
     * @param channel Requesting channel.
     * @param stream Response stream.
     * @param manifest Lease's manifest.
     * @param index Entry index; the channel validated it and the range.
     * @param offset First byte.
     * @param length Exact number of bytes to send.
     */
    virtual void serve_file(channel_t &channel, std::uint32_t stream, const std::shared_ptr<const manifest_t> &manifest, std::uint64_t index, std::uint64_t offset, std::uint64_t length) = 0;

    /**
     * @brief A served stream ended early (client CANCEL, timeout, or close); stop producing it.
     *
     * @param channel Channel of the stream.
     * @param stream Stream that ended.
     */
    virtual void cancel_serving(channel_t &channel, std::uint32_t stream) = 0;

    /**
     * @brief A lease the host granted ended.
     *
     * @param channel Channel of the lease.
     * @param manifest Manifest the lease pinned.
     */
    virtual void on_lease_released(channel_t &channel, const std::shared_ptr<const manifest_t> &manifest) = 0;

    /**
     * @brief The channel closed or started closing; it accepts no more operations.
     *
     * @param channel Closed channel.
     */
    virtual void on_closed(channel_t &channel) = 0;
  };

  /**
   * @brief Host side of one clipboard channel.
   */
  class channel_t {
  public:
    /**
     * @brief Lifecycle state.
     */
    enum class state_e {
      handshake,  ///< Waiting for HELLO.
      open,  ///< HELLO answered.
      closing,  ///< Remaining output is flushed, then the transport closes.
      closed,  ///< Nothing more is sent.
    };

    /**
     * @brief Create a channel right after the upgrade.
     *
     * @param id Identifier the delegate uses for this channel.
     * @param settings Host settings for this client.
     * @param delegate Host operations; must outlive the channel.
     * @param now Clock.
     */
    channel_t(std::uint64_t id, host_settings_t settings, channel_delegate_t &delegate, clock_fn now);

    channel_t(const channel_t &) = delete;
    channel_t &operator=(const channel_t &) = delete;

    /**
     * @brief Get the channel's identifier.
     *
     * @return Identifier given at construction.
     */
    [[nodiscard]] std::uint64_t id() const {
      return id_;
    }

    /**
     * @brief Get the lifecycle state.
     *
     * @return Current state.
     */
    [[nodiscard]] state_e state() const {
      return state_;
    }

    /**
     * @brief Get the effective policy negotiated in HELLO.
     *
     * @return Effective policy; all false before HELLO.
     */
    [[nodiscard]] const policy_t &policy() const {
      return policy_;
    }

    /**
     * @brief Get the effective limits negotiated in HELLO.
     *
     * @return Effective limits.
     */
    [[nodiscard]] const limits_t &limits() const {
      return limits_;
    }

    /**
     * @brief Get the effective formats negotiated in HELLO.
     *
     * @return Effective formats.
     */
    [[nodiscard]] const std::vector<std::string> &formats() const {
      return formats_;
    }

    /**
     * @brief Get the client's operating system from HELLO.
     *
     * @return Client OS name.
     */
    [[nodiscard]] const std::string &client_os() const {
      return client_os_;
    }

    /**
     * @brief Process bytes received from the client.
     *
     * @param bytes Received bytes.
     */
    void feed(std::string_view bytes);

    /**
     * @brief Run timers.
     */
    void tick();

    /**
     * @brief Take bytes to send, control frames first.
     *
     * @param max Most bytes to take; whole frames are always returned, at least one when any is queued.
     * @return Encoded frames.
     */
    std::string take_output(std::size_t max = 256 * 1024);

    /**
     * @brief Check whether output is queued.
     *
     * @return `true` when `take_output` would return bytes.
     */
    [[nodiscard]] bool has_output() const;

    /**
     * @brief Check whether the transport should close once output is flushed.
     *
     * @return `true` while closing or closed.
     */
    [[nodiscard]] bool wants_close() const {
      return state_ == state_e::closing || state_ == state_e::closed;
    }

    /**
     * @brief Set the callback run whenever output becomes available.
     *
     * @param callback Callback; it must not call back into the channel synchronously.
     */
    void set_output_callback(std::function<void()> callback) {
      output_callback_ = std::move(callback);
    }

    /**
     * @brief Close the channel without an error frame, e.g. when the transport failed or the host revoked it.
     */
    void close();

    /**
     * @brief Close the channel with a fatal ERROR on stream 0.
     *
     * @param error Error code.
     * @param message Detail for the peer and the log.
     */
    void fail(error_e error, const std::string &message);

    /**
     * @brief Announce new host clipboard content, or clear the client's view of it.
     * @details Formats outside the effective formats are dropped, and `files` unless files are allowed.
     *          When nothing is left, the client gets CLEAR if it still holds the host's previous
     *          generation. Nothing is sent when the policy forbids host to client.
     *
     * @param clipboard New host clipboard.
     * @return The new host generation, or 0 when nothing was announced.
     */
    std::uint64_t announce_host(const host_clipboard_t &clipboard);

    /**
     * @brief Send CLEAR because another channel took the host clipboard.
     */
    void clear_host();

    /**
     * @brief Mark the host's previous generation as superseded by this client's own installed content.
     */
    void host_superseded();

    /**
     * @brief Get the host's latest generation on this channel.
     *
     * @return Latest host generation, 0 before the first.
     */
    [[nodiscard]] std::uint64_t host_generation() const {
      return host_generation_;
    }

    /**
     * @brief Get the client's latest announced generation.
     *
     * @return Latest client generation, 0 before the first.
     */
    [[nodiscard]] std::uint64_t client_generation() const {
      return client_generation_;
    }

    /**
     * @brief Check whether a client generation is still the client's current clipboard.
     *
     * @param generation Client generation.
     * @return `true` when it is the latest announced generation and was not cleared.
     */
    [[nodiscard]] bool client_generation_current(std::uint64_t generation) const {
      return generation != 0 && generation == client_generation_ && !client_cleared_;
    }

    /**
     * @brief Answer a client announcement.
     * @details Ignored when the generation was already answered.
     *
     * @param generation Client generation.
     * @param status `installed` or an error code.
     */
    void ack_client(std::uint64_t generation, std::string_view status);

    /**
     * @brief Answer a served stream with a complete body.
     *
     * @param stream Served stream.
     * @param bytes Whole response body.
     */
    void respond_bytes(std::uint32_t stream, std::string bytes);

    /**
     * @brief Start answering a served stream with a body produced on demand.
     *
     * @param stream Served stream.
     * @param pull Called with the most bytes wanted; the producer answers with `respond_data`.
     */
    void respond_stream(std::uint32_t stream, std::function<void(std::size_t)> pull);

    /**
     * @brief Provide body bytes requested by a pull.
     *
     * @param stream Served stream.
     * @param bytes Bytes produced, at most the pulled amount.
     * @param eof Whether these are the last bytes.
     */
    void respond_data(std::uint32_t stream, std::string bytes, bool eof);

    /**
     * @brief Answer a served stream with an error.
     *
     * @param stream Served stream.
     * @param error Error code.
     * @param message Detail for the peer.
     */
    void respond_error(std::uint32_t stream, error_e error, const std::string &message = {});

    /**
     * @brief Answer a LOCK with a new lease.
     * @details Answers `busy` when the lease limit is reached.
     *
     * @param stream LOCK stream.
     * @param manifest Manifest the lease pins.
     */
    void grant_lease(std::uint32_t stream, std::shared_ptr<const manifest_t> manifest);

    /**
     * @brief Send a request to the client.
     * @details Requests beyond the concurrency limit wait until a stream finishes.
     *
     * @param type Request type (GET, LOCK, GET_FILE_LIST or GET_FILE).
     * @param payload JSON payload.
     * @param options Size checks and credit mode.
     * @param callbacks Response callbacks.
     * @return Stream id of the request, or 0 when the channel is not open.
     */
    std::uint32_t request(msg_e type, std::string payload, request_options_t options, request_callbacks_t callbacks);

    /**
     * @brief Grant the client more DATA credit on a request without automatic credit.
     *
     * @param stream Request stream.
     * @param bytes Bytes consumed.
     */
    void grant(std::uint32_t stream, std::uint64_t bytes);

    /**
     * @brief Abort a request; its callbacks are not called.
     *
     * @param stream Request stream.
     */
    void cancel_request(std::uint32_t stream);

    /**
     * @brief Release a lease the client granted the host.
     *
     * @param lease Client's lease.
     */
    void unlock(std::uint64_t lease);

    /**
     * @brief Get the number of requests waiting for a free stream.
     *
     * @return Queued request count.
     */
    [[nodiscard]] std::size_t queued_requests() const {
      return queued_requests_.size();
    }

    /**
     * @brief Get the number of leases the host granted on this channel.
     *
     * @return Live lease count.
     */
    [[nodiscard]] std::size_t lease_count() const {
      return leases_.size();
    }

  private:
    /**
     * @brief A response the host is sending.
     */
    struct served_t {
      msg_e request {};  ///< Request being answered.
      std::uint64_t credit = limit::initial_credit;  ///< DATA bytes the client still accepts.
      std::string buffer;  ///< Body bytes, framed from `offset` on.
      std::size_t offset = 0;  ///< First byte of `buffer` not yet framed.
      bool answered = false;  ///< A body or error source was set.
      bool complete = false;  ///< All body bytes are in `buffer` or sent.
      std::function<void(std::size_t)> pull;  ///< Producer of a streamed body.
      bool pull_pending = false;  ///< A pull awaits `respond_data`.
      std::optional<std::uint64_t> exact;  ///< Bytes the body must have in total.
      std::uint64_t sent = 0;  ///< Body bytes framed so far.
      time_point progress;  ///< Last time the stream advanced.
    };

    /**
     * @brief A request the host sent or queued.
     */
    struct requested_t {
      msg_e request {};  ///< Request type.
      request_options_t options;  ///< Size checks and credit mode.
      request_callbacks_t callbacks;  ///< Response callbacks.
      std::uint64_t window = limit::initial_credit;  ///< DATA bytes the client may still send.
      std::uint64_t unacknowledged = 0;  ///< Consumed bytes not yet returned as CREDIT.
      std::uint64_t received = 0;  ///< DATA bytes received.
      bool sent = false;  ///< The request left the queue.
      time_point progress;  ///< Last time the stream advanced.
    };

    /**
     * @brief A request waiting for a free stream.
     */
    struct queued_t {
      std::uint32_t stream = 0;  ///< Stream id reserved for it.
      msg_e request {};  ///< Request type.
      std::string payload;  ///< JSON payload.
    };

    /**
     * @brief A lease the host granted.
     */
    struct lease_t {
      std::shared_ptr<const manifest_t> manifest;  ///< Pinned manifest.
      time_point used;  ///< Last request on the lease.
    };

    /**
     * @brief Dispatch one received frame.
     *
     * @param frame Received frame.
     */
    void handle_frame(frame_t &frame);

    /**
     * @brief Negotiate the channel from HELLO and answer HELLO_REPLY.
     *
     * @param frame HELLO frame.
     */
    void handle_hello(const frame_t &frame);

    /**
     * @brief Validate a client ANNOUNCE and hand it to the delegate or reject it.
     *
     * @param frame ANNOUNCE frame.
     */
    void handle_announce(const frame_t &frame);

    /**
     * @brief Handle a client CLEAR.
     *
     * @param frame CLEAR frame.
     */
    void handle_clear(const frame_t &frame);

    /**
     * @brief Open a served stream for a client request.
     *
     * @param frame Request frame.
     */
    void handle_request(const frame_t &frame);

    /**
     * @brief Handle DATA, LOCK_REPLY or ERROR on a host request's stream.
     *
     * @param frame Response frame.
     */
    void handle_response(const frame_t &frame);

    /**
     * @brief Add credit to a served stream.
     *
     * @param frame CREDIT frame.
     */
    void handle_credit(const frame_t &frame);

    /**
     * @brief Stop a served stream the client cancelled.
     *
     * @param frame CANCEL frame.
     */
    void handle_cancel(const frame_t &frame);

    /**
     * @brief Release a lease the client no longer needs.
     *
     * @param frame UNLOCK frame.
     */
    void handle_unlock(const frame_t &frame);

    /**
     * @brief Record a client request stream id.
     *
     * @param stream Stream id of a client request.
     * @return `false` when the id has the host's parity or was used before.
     */
    bool claim_stream_id(std::uint32_t stream);

    /**
     * @brief Validate a GET and forward it to the delegate.
     *
     * @param stream Served stream.
     * @param payload Request payload.
     */
    void serve_get(std::uint32_t stream, const std::string &payload);

    /**
     * @brief Validate a LOCK and forward it to the delegate.
     *
     * @param stream Served stream.
     * @param payload Request payload.
     */
    void serve_lock(std::uint32_t stream, const std::string &payload);

    /**
     * @brief Answer GET_FILE_LIST from a lease's manifest.
     *
     * @param stream Served stream.
     * @param payload Request payload.
     */
    void serve_file_list(std::uint32_t stream, const std::string &payload);

    /**
     * @brief Validate a GET_FILE and forward it to the delegate.
     *
     * @param stream Served stream.
     * @param payload Request payload.
     */
    void serve_file(std::uint32_t stream, const std::string &payload);

    /**
     * @brief Find a lease the host granted.
     *
     * @param lease Lease id.
     * @return The lease, or null when unknown.
     */
    lease_t *find_lease(std::uint64_t lease);

    /**
     * @brief Keep the formats that may be announced on this channel.
     *
     * @param formats Canonical formats of the host clipboard.
     * @return Formats within the effective formats, `files` only when files are allowed.
     */
    [[nodiscard]] std::vector<std::string> transferable(const std::vector<std::string> &formats) const;

    /**
     * @brief Queue a control frame.
     *
     * @param type Message type.
     * @param flags Frame flags.
     * @param stream Stream id.
     * @param payload Payload.
     */
    void send_control(msg_e type, std::uint16_t flags, std::uint32_t stream, std::string_view payload);

    /**
     * @brief Queue an ERROR frame; on a stream it carries END.
     *
     * @param stream Stream id, 0 for the channel.
     * @param error Error code.
     * @param message Detail for the peer.
     */
    void send_error(std::uint32_t stream, error_e error, const std::string &message);

    /**
     * @brief Forget a served stream.
     *
     * @param stream Served stream.
     * @param cancelled Whether the delegate must stop producing it.
     */
    void finish_served(std::uint32_t stream, bool cancelled);

    /**
     * @brief End a host request with an error and run its error callback.
     *
     * @param stream Request stream.
     * @param error Error code.
     * @param message Detail.
     * @param send_cancel Whether to tell the client with CANCEL.
     */
    void fail_requested(std::uint32_t stream, error_e error, const std::string &message, bool send_cancel);

    /**
     * @brief Send queued requests while streams are free.
     */
    void send_queued_requests();

    /**
     * @brief Frame buffered response bytes within credit and the write-queue cap, and pull more.
     * @details Re-entry from a producer that answers synchronously is deferred to another pass, so
     *          its bytes are framed before `pump` returns.
     */
    void pump();

    /**
     * @brief Frame one served stream's buffered bytes, or pull more for it.
     *
     * @param stream Served stream.
     */
    void pump_stream(std::uint32_t stream);

    /**
     * @brief Remove a lease and tell the delegate.
     *
     * @param it Lease to release.
     */
    void release_lease(std::map<std::uint64_t, lease_t>::iterator it);

    /**
     * @brief Stop the channel: end every stream, request and lease, and tell the delegate.
     *
     * @param flush Whether queued control frames are still sent before the transport closes.
     */
    void enter_closed(bool flush);

    /**
     * @brief Run the output callback when there is something to send or the transport should close.
     */
    void notify_output();

    std::uint64_t id_;  ///< Delegate's identifier.
    host_settings_t settings_;  ///< Host bounds for negotiation.
    channel_delegate_t &delegate_;  ///< Host operations.
    clock_fn now_;  ///< Clock.
    std::function<void()> output_callback_;  ///< Run when output becomes available.

    state_e state_ = state_e::handshake;  ///< Lifecycle state.
    frame_decoder_t decoder_;  ///< Incoming frame decoder.
    std::deque<std::string> control_;  ///< Queued control frames.
    std::deque<std::string> data_;  ///< Queued DATA frames.
    std::size_t data_bytes_ = 0;  ///< Bytes in `data_`.
    time_point opened_;  ///< Time of the upgrade.
    time_point last_received_;  ///< Time the last frame arrived.
    time_point last_ping_;  ///< Time the last PING was sent.
    std::uint64_t ping_nonce_ = 0;  ///< Nonce of the last PING.

    policy_t policy_;  ///< Effective policy.
    limits_t limits_;  ///< Effective limits.
    std::vector<std::string> formats_;  ///< Effective formats.
    std::string client_os_;  ///< Client OS from HELLO.

    std::uint64_t host_generation_ = 0;  ///< Host's latest generation on this channel.
    std::uint64_t host_revision_ = 0;  ///< Native revision of the host's latest generation.
    std::vector<std::string> host_formats_;  ///< Formats announced in the host's latest generation.
    bool host_live_ = false;  ///< The client may still pull the host's latest generation.

    std::uint64_t client_generation_ = 0;  ///< Client's latest generation.
    bool client_cleared_ = false;  ///< The client cleared its latest generation.
    std::set<std::uint64_t> pending_acks_;  ///< Client generations not yet answered.

    std::map<std::uint32_t, served_t> served_;  ///< Responses the host is sending.
    std::map<std::uint32_t, std::uint32_t> client_ids_;  ///< Ranges of client stream ids used: first id to last id.
    std::map<std::uint32_t, requested_t> requested_;  ///< Requests sent to the client.
    std::deque<queued_t> queued_requests_;  ///< Requests waiting for a free stream.
    std::uint32_t next_stream_ = 2;  ///< Next host stream id.

    std::map<std::uint64_t, lease_t> leases_;  ///< Leases the host granted.
    std::uint64_t next_lease_ = 1;  ///< Next lease id.
    bool pumping_ = false;  ///< Guards `pump` against re-entry.
    bool repump_ = false;  ///< `pump` was called during a pass and must run another.
  };
}  // namespace clipboard
