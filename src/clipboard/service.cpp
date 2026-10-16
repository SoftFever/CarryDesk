/**
 * @file src/clipboard/service.cpp
 * @brief Definitions for the host clipboard service.
 */
// standard includes
#include <algorithm>
#include <utility>

#ifndef _WIN32
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

// lib includes
#include <boost/asio/post.hpp>

// local includes
#include "file_list.h"
#include "service.h"
#include "src/logging.h"

using namespace std::literals;

namespace clipboard {
  namespace {
    constexpr std::size_t max_tombstones = 64;  ///< Stopped-before-started sessions remembered.

    std::mutex instance_mutex;  ///< Guards `instance`.
    std::shared_ptr<service_t> instance;  ///< Process-wide service.

    /**
     * @brief ACK status for a failed pull.
     *
     * @param error Error that ended the pull.
     * @return ACK status.
     */
    std::string_view status_for(const error_e error) {
      switch (error) {
        case error_e::stale_generation:
        case error_e::too_large:
        case error_e::not_permitted:
          return to_string(error);
        default:
          return to_string(error_e::io_error);
      }
    }

    /**
     * @brief Protocol error for a backend outcome.
     *
     * @param result Backend outcome other than `ok`.
     * @return Protocol error code.
     */
    error_e error_for(const platf::clipboard_result_e result) {
      switch (result) {
        case platf::clipboard_result_e::stale:
          return error_e::stale_generation;
        case platf::clipboard_result_e::too_large:
          return error_e::too_large;
        case platf::clipboard_result_e::not_permitted:
          return error_e::not_permitted;
        default:
          return error_e::io_error;
      }
    }

    /**
     * @brief Join mime types for a log line.
     *
     * @param mimes Mime types.
     * @return Comma-separated list, or `none`.
     */
    std::string join(const std::vector<std::string> &mimes) {
      std::string text;
      for (const auto &mime : mimes) {
        text += (text.empty() ? "" : ", ") + mime;
      }
      return text.empty() ? "none"s : text;
    }

    /**
     * @brief Join announced formats for a log line.
     *
     * @param formats Announced formats.
     * @return Comma-separated mime list, or `none`.
     */
    std::string join(const std::vector<format_t> &formats) {
      std::vector<std::string> mimes;
      for (const auto &format : formats) {
        mimes.push_back(format.mime);
      }
      return join(mimes);
    }

    /**
     * @brief Write a whole buffer to a descriptor.
     *
     * @param fd Destination.
     * @param data Bytes.
     * @param size Byte count.
     * @return `false` when a write failed.
     */
    bool write_all(const int fd, const char *data, std::size_t size) {
#ifndef _WIN32
      while (size > 0) {
        const auto written = ::write(fd, data, size);
        if (written < 0 && errno == EINTR) {
          continue;
        }
        if (written <= 0) {
          return false;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
      }
      return true;
#else
      return false;
#endif
    }

    /**
     * @brief Read bytes at an offset.
     *
     * @param fd Source.
     * @param data Destination buffer.
     * @param size Most bytes.
     * @param offset File offset.
     * @return Bytes read, 0 at end of file, or -1 on failure.
     */
    long long read_at(const int fd, char *data, const std::size_t size, const std::uint64_t offset) {
#ifndef _WIN32
      while (true) {
        const auto read = ::pread(fd, data, size, static_cast<off_t>(offset));
        if (read < 0 && errno == EINTR) {
          continue;
        }
        return read;
      }
#else
      return -1;
#endif
    }

    /**
     * @brief Set a file's modification time.
     *
     * @param fd File.
     * @param mtime Unix seconds.
     */
    void set_mtime(const int fd, const std::int64_t mtime) {
#ifndef _WIN32
      const timespec times[2] {{0, UTIME_OMIT}, {static_cast<time_t>(mtime), 0}};
      futimens(fd, times);
#endif
    }
  }  // namespace

  service_t::shared_fd_t::~shared_fd_t() {
#ifndef _WIN32
    if (fd >= 0) {
      ::close(fd);
    }
#endif
  }

  service_t::service_t(service_config_t config, std::unique_ptr<platf::clipboard_t> backend, boost::asio::any_io_executor io, boost::asio::any_io_executor worker, clock_fn clock, std::string os):
      config_ {config},
      backend_ {std::move(backend)},
      io_ {std::move(io)},
      worker_ {std::move(worker)},
      clock_ {std::move(clock)},
      os_ {std::move(os)},
      reaper_ {std::make_shared<boost::asio::steady_timer>(io_)} {
  }

  service_t::~service_t() {
    alive_.reset();
  }

  bool service_t::start() {
    if (!config_.policy.host_to_client && !config_.policy.client_to_host) {
      BOOST_LOG(info) << "Clipboard sync is disabled"sv;
      return false;
    }
    if (!backend_) {
      BOOST_LOG(warning) << "Clipboard sync is unavailable: no clipboard backend works in this session"sv;
      return false;
    }
    if (!backend_->start(*this)) {
      BOOST_LOG(warning) << "Clipboard sync is unavailable: the clipboard backend failed to start"sv;
      return false;
    }
    formats_ = backend_->formats();
    area_ = std::make_shared<staging::area_t>(config_.staging_area);
    if (area_->open()) {
      area_->sweep(std::chrono::system_clock::now());
    }
    running_ = true;
    std::weak_ptr<bool> alive = alive_;
    boost::asio::post(io_, [this, alive]() {
      if (!alive.expired()) {
        schedule_reaper();
      }
    });
    BOOST_LOG(info) << "Clipboard sync is available"sv;
    return true;
  }

  void service_t::stop(const std::chrono::milliseconds wait) {
    const auto was_running = running_.exchange(false);
    boost::asio::post(io_, [timer = reaper_]() {
      timer->cancel();
    });
    for (const auto &connection : connections()) {
      post_close(connection);
    }
    {
      std::unique_lock lock {mutex_};
      if (!closed_.wait_for(lock, wait, [this]() {
            return connections_.empty();
          })) {
        BOOST_LOG(warning) << "Clipboard channels did not close in time"sv;
      }
    }
    if (was_running) {
      backend_->stop();
    }
  }

  bool service_t::available() const {
    return running_;
  }

  bool service_t::feature_enabled(const bool client_allowed) const {
    return running_ && client_allowed;
  }

  void service_t::session_started(const std::uint64_t session, const std::string &cert, const std::function<bool()> &still_running) {
    std::lock_guard lock {mutex_};
    if (const auto it = std::ranges::find(tombstones_, session); it != tombstones_.end()) {
      tombstones_.erase(it);
      return;
    }
    if (still_running()) {
      sessions_[session] = cert;
    }
  }

  void service_t::session_stopped(const std::uint64_t session) {
    std::shared_ptr<connection_t> closing;
    {
      std::lock_guard lock {mutex_};
      const auto it = sessions_.find(session);
      if (it == sessions_.end()) {
        tombstones_.push_back(session);
        if (tombstones_.size() > max_tombstones) {
          tombstones_.pop_front();
        }
        return;
      }
      const auto cert = it->second;
      sessions_.erase(it);
      if (!has_session_locked(cert)) {
        if (const auto channel = cert_channels_.find(cert); channel != cert_channels_.end()) {
          closing = connections_.at(channel->second);
        }
      }
    }
    if (closing) {
      post_close(closing);
    }
  }

  admission_t service_t::admit(const std::string &cert, const bool client_allowed, const connection_factory_t &factory) {
    std::shared_ptr<connection_t> replaced;
    admission_t admission;
    {
      std::lock_guard lock {mutex_};
      if (!running_) {
        return {admission_e::disabled, nullptr};
      }
      if (!client_allowed) {
        return {admission_e::not_permitted, nullptr};
      }
      if (!has_session_locked(cert)) {
        return {admission_e::no_session, nullptr};
      }
      const auto existing = cert_channels_.find(cert);
      if (existing == cert_channels_.end() && connections_.size() >= limit::max_channels) {
        return {admission_e::too_many, nullptr};
      }
      if (existing != cert_channels_.end()) {
        const auto old_id = existing->second;
        replaced = connections_.at(old_id);
        connections_.erase(old_id);
        channel_certs_.erase(old_id);
        cert_channels_.erase(existing);
      }
      const auto id = next_channel_++;
      auto connection = factory(id, host_settings_t {config_.policy, config_.limits, formats_, os_}, *this);
      connections_[id] = connection;
      channel_certs_[id] = cert;
      cert_channels_[cert] = id;
      admission = {admission_e::admitted, std::move(connection)};
    }
    if (replaced) {
      BOOST_LOG(info) << "Replacing the clipboard channel of a reconnecting client"sv;
      post_close(replaced);
    }
    return admission;
  }

  void service_t::revoke(const std::string &cert) {
    std::shared_ptr<connection_t> closing;
    {
      std::lock_guard lock {mutex_};
      if (const auto it = cert_channels_.find(cert); it != cert_channels_.end()) {
        closing = connections_.at(it->second);
      }
    }
    if (closing) {
      post_close(closing);
    }
  }

  void service_t::revoke_all() {
    for (const auto &connection : connections()) {
      post_close(connection);
    }
  }

  std::size_t service_t::channel_count() const {
    std::lock_guard lock {mutex_};
    return connections_.size();
  }

  std::size_t service_t::session_count(const std::string &cert) const {
    std::lock_guard lock {mutex_};
    return static_cast<std::size_t>(std::ranges::count_if(sessions_, [&cert](const auto &session) {
      return session.second == cert;
    }));
  }

  std::shared_ptr<connection_t> service_t::find_connection(const std::uint64_t id) const {
    std::lock_guard lock {mutex_};
    const auto it = connections_.find(id);
    return it == connections_.end() ? nullptr : it->second;
  }

  std::vector<std::shared_ptr<connection_t>> service_t::connections() const {
    std::lock_guard lock {mutex_};
    std::vector<std::shared_ptr<connection_t>> result;
    for (const auto &[id, connection] : connections_) {
      result.push_back(connection);
    }
    return result;
  }

  void service_t::post_close(const std::shared_ptr<connection_t> &connection) {
    boost::asio::post(io_, [connection]() {
      connection->close();
    });
  }

  bool service_t::has_session_locked(const std::string &cert) const {
    return std::ranges::any_of(sessions_, [&cert](const auto &session) {
      return session.second == cert;
    });
  }

  std::optional<host_clipboard_t> service_t::current_host_clipboard(channel_t &) {
    if (owner_ || host_.formats.empty()) {
      return std::nullopt;
    }
    return host_;
  }

  void service_t::on_open(channel_t &channel) {
    BOOST_LOG(info) << "Clipboard channel opened with a "sv << (channel.client_os().empty() ? "client"s : channel.client_os() + " client"s);
  }

  void service_t::on_client_announce(channel_t &channel, const std::uint64_t generation, const std::vector<format_t> &formats) {
    cancel_install(channel.id());
    auto install = std::make_shared<install_t>();
    install->channel = channel.id();
    install->generation = generation;
    install->sequence = ++announce_sequence_;
    install->revision = host_.revision;
    install->formats = formats;
    installs_[channel.id()] = install;
    BOOST_LOG(debug) << "Client ANNOUNCE generation "sv << generation << ": "sv << join(formats);

    std::weak_ptr<bool> alive = alive_;
    for (const auto &format : formats) {
      if (format.mime == mime::files) {
        start_files(install, channel);
        continue;
      }
      ++install->pending;
      const auto &mime = format.mime;
      const auto stream = channel.request(
        msg_e::get,
        serialize(get_t {generation, mime}),
        {channel.limits().max_item_size, std::nullopt, true},
        {
          [install, mime](const std::string_view bytes) {
            install->content[mime].append(bytes);
          },
          [this, alive, install, mime]() {
            if (alive.expired()) {
              return;
            }
            const auto &bytes = install->content[mime];
            BOOST_LOG(debug) << "Pulled "sv << mime << " of client generation "sv << install->generation << ": "sv << bytes.size() << " bytes"sv;
            --install->pending;
            check_install(install);
          },
          [this, alive, install, mime](const error_e error, const std::string &) {
            if (alive.expired()) {
              return;
            }
            BOOST_LOG(debug) << "Pulling "sv << mime << " of client generation "sv << install->generation << " failed: "sv << to_string(error);
            --install->pending;
            if (error == error_e::too_large) {
              ++install->too_large;
              install->content.erase(mime);
            } else if (!install->failure) {
              install->failure = error;
            }
            check_install(install);
          },
        }
      );
      install->streams.push_back(stream);
    }
    check_install(install);
  }

  void service_t::on_client_clear(channel_t &channel, std::uint64_t) {
    cancel_install(channel.id());
    if (owner_ == channel.id()) {
      BOOST_LOG(debug) << "The owning client cleared its clipboard; emptying the host clipboard"sv;
      release_ownership();
    }
  }

  void service_t::serve_get(channel_t &channel, const std::uint32_t stream, const std::uint64_t revision, const std::string &mime) {
    std::weak_ptr<connection_t> weak = find_connection(channel.id());
    std::weak_ptr<bool> alive = alive_;
    const auto max_size = channel.limits().max_item_size;
    boost::asio::post(worker_, [this, alive, weak, stream, revision, mime, max_size]() {
      if (alive.expired()) {
        return;
      }
      auto read = std::make_shared<platf::clipboard_read_t>(backend_->read(revision, mime, max_size));
      if (read->result == platf::clipboard_result_e::ok) {
        BOOST_LOG(debug) << "Serving GET "sv << mime << " of host revision "sv << revision << ": "sv << read->bytes.size() << " bytes"sv;
      } else {
        BOOST_LOG(debug) << "Serving GET "sv << mime << " of host revision "sv << revision << " failed: "sv << to_string(error_for(read->result));
      }
      boost::asio::post(io_, [weak, stream, read]() {
        const auto connection = weak.lock();
        if (!connection) {
          return;
        }
        if (read->result == platf::clipboard_result_e::ok) {
          connection->channel().respond_bytes(stream, std::move(read->bytes));
        } else {
          connection->channel().respond_error(stream, error_for(read->result));
        }
      });
    });
  }

  void service_t::serve_lock(channel_t &channel, const std::uint32_t stream, const std::uint64_t revision) {
    std::weak_ptr<connection_t> weak = find_connection(channel.id());
    std::weak_ptr<bool> alive = alive_;
    const auto max_total = channel.limits().max_transfer_size;
    boost::asio::post(worker_, [this, alive, weak, stream, revision, max_total]() {
      if (alive.expired()) {
        return;
      }
      const auto read = backend_->read(revision, std::string {mime::files}, limit::max_manifest_bytes);
      auto built = std::make_shared<files::build_result_t>();
      if (read.result != platf::clipboard_result_e::ok) {
        built->error = error_for(read.result);
      } else {
        *built = files::build_manifest(read.paths, max_total);
      }
      if (built->skipped > 0) {
        BOOST_LOG(debug) << "Left "sv << built->skipped << " links, special files or unshareable names out of a file copy"sv;
      }
      if (built->error) {
        BOOST_LOG(debug) << "Serving LOCK of host revision "sv << revision << " failed: "sv << to_string(*built->error);
      } else {
        BOOST_LOG(debug) << "Serving LOCK of host revision "sv << revision << ": "sv << built->manifest->entries.size() << " entries, "sv << built->manifest->total_bytes << " bytes"sv;
      }
      boost::asio::post(io_, [weak, stream, built]() {
        const auto connection = weak.lock();
        if (!connection) {
          return;
        }
        if (built->error) {
          connection->channel().respond_error(stream, *built->error);
        } else {
          connection->channel().grant_lease(stream, built->manifest);
        }
      });
    });
  }

  void service_t::serve_file(channel_t &channel, const std::uint32_t stream, const std::shared_ptr<const manifest_t> &manifest, const std::uint64_t index, const std::uint64_t offset, const std::uint64_t length) {
    std::weak_ptr<connection_t> weak = find_connection(channel.id());
    std::weak_ptr<bool> alive = alive_;
    boost::asio::post(worker_, [this, alive, weak, stream, manifest, index, offset, length]() {
      if (alive.expired()) {
        return;
      }
      auto file = std::make_shared<shared_fd_t>();
      file->fd = files::open_entry(*manifest, index);
      file->offset = offset;
      file->remaining = length;
      BOOST_LOG(debug) << "Serving GET_FILE entry "sv << index << " bytes "sv << offset << "+"sv << length << (file->fd < 0 ? ": the file changed or cannot be read"sv : ""sv);
      boost::asio::post(io_, [this, alive, weak, stream, file]() {
        const auto connection = weak.lock();
        if (!connection || alive.expired()) {
          return;
        }
        if (file->fd < 0) {
          connection->channel().respond_error(stream, error_e::io_error, "the file changed or cannot be read");
          return;
        }
        connection->channel().respond_stream(stream, [this, alive, weak, stream, file](const std::size_t max) {
          boost::asio::post(worker_, [this, alive, weak, stream, file, max]() {
            if (alive.expired()) {
              return;
            }
            auto bytes = std::make_shared<std::string>(static_cast<std::size_t>(std::min<std::uint64_t>(max, file->remaining)), '\0');
            const auto read = read_at(file->fd, bytes->data(), bytes->size(), file->offset);
            if (read >= 0) {
              bytes->resize(static_cast<std::size_t>(read));
              file->offset += static_cast<std::uint64_t>(read);
              file->remaining -= static_cast<std::uint64_t>(read);
            }
            const auto eof = read == 0 || file->remaining == 0;
            boost::asio::post(io_, [weak, stream, bytes, read, eof]() {
              const auto connection = weak.lock();
              if (!connection) {
                return;
              }
              if (read < 0) {
                connection->channel().respond_error(stream, error_e::io_error, "reading the file failed");
              } else {
                connection->channel().respond_data(stream, std::move(*bytes), eof);
              }
            });
          });
        });
      });
    });
  }

  void service_t::cancel_serving(channel_t &, std::uint32_t) {
    // Pending backend reads and file reads finish on their own; the channel drops answers for ended
    // streams, and a file closes when its stream's producer is dropped.
  }

  void service_t::on_lease_released(channel_t &, const std::shared_ptr<const manifest_t> &) {
    // The manifest's context releases its resources when the last reference goes away.
  }

  void service_t::on_closed(channel_t &channel) {
    const auto id = channel.id();
    cancel_install(id);
    if (owner_ == id) {
      release_ownership();
    }
    {
      std::lock_guard lock {mutex_};
      if (connections_.erase(id) > 0) {
        if (const auto cert = channel_certs_.find(id); cert != channel_certs_.end()) {
          cert_channels_.erase(cert->second);
          channel_certs_.erase(cert);
        }
      }
    }
    closed_.notify_all();
    BOOST_LOG(info) << "Clipboard channel closed"sv;
  }

  void service_t::on_clipboard_changed(const std::uint64_t revision, std::vector<std::string> formats) {
    std::weak_ptr<bool> alive = alive_;
    boost::asio::post(io_, [this, alive, revision, formats = std::move(formats)]() mutable {
      if (!alive.expired()) {
        handle_host_change(revision, std::move(formats));
      }
    });
  }

  void service_t::handle_host_change(const std::uint64_t revision, std::vector<std::string> formats) {
    BOOST_LOG(debug) << "Host clipboard revision "sv << revision << ": "sv << join(formats);
    host_ = {revision, std::move(formats)};
    owner_.reset();
    release_root(current_root_);
    current_root_.reset();

    std::vector<std::shared_ptr<install_t>> superseded;
    for (const auto &[id, install] : installs_) {
      superseded.push_back(install);
    }
    for (const auto &install : superseded) {
      if (const auto connection = find_connection(install->channel)) {
        connection->channel().ack_client(install->generation, to_string(error_e::stale_generation));
      }
      cancel_install(install->channel);
    }

    for (const auto &connection : connections()) {
      connection->channel().announce_host(host_);
    }
  }

  void service_t::cancel_install(const std::uint64_t channel_id) {
    const auto it = installs_.find(channel_id);
    if (it == installs_.end()) {
      return;
    }
    const auto install = it->second;
    installs_.erase(it);
    install->cancelled = true;
    if (const auto connection = find_connection(channel_id)) {
      for (const auto stream : install->streams) {
        connection->channel().cancel_request(stream);
      }
      if (install->lease) {
        connection->channel().unlock(install->lease);
      }
    }
    install->lease = 0;
    if (install->root != current_root_) {
      discard_root(install->root);
    }
  }

  void service_t::check_install(const std::shared_ptr<install_t> &install) {
    if (install->cancelled || install->pending > 0) {
      return;
    }
    if (install->failure) {
      finish_install(install, status_for(*install->failure));
      return;
    }
    if (!install->formats.empty() && install->too_large == install->formats.size()) {
      finish_install(install, to_string(error_e::too_large));
      return;
    }
    commits_.push_back(install);
    run_commits();
  }

  void service_t::finish_install(const std::shared_ptr<install_t> &install, const std::string_view status) {
    BOOST_LOG(debug) << "ANNOUNCE_ACK generation "sv << install->generation << ": "sv << status;
    if (const auto it = installs_.find(install->channel); it != installs_.end() && it->second == install) {
      installs_.erase(it);
    }
    if (status != status_installed && install->root != current_root_) {
      discard_root(install->root);
    }
    if (const auto connection = find_connection(install->channel)) {
      connection->channel().ack_client(install->generation, status);
    }
  }

  void service_t::run_commits() {
    while (!committing_ && !commits_.empty()) {
      const auto install = commits_.front();
      commits_.pop_front();
      if (install->cancelled) {
        continue;
      }
      const auto connection = find_connection(install->channel);
      if (!connection) {
        continue;
      }
      auto &channel = connection->channel();
      if (!channel.client_generation_current(install->generation) || install->sequence < committed_sequence_) {
        finish_install(install, to_string(error_e::stale_generation));
        continue;
      }
      committing_ = true;
      std::weak_ptr<bool> alive = alive_;
      boost::asio::post(worker_, [this, alive, install]() {
        if (alive.expired()) {
          return;
        }
        const auto result = backend_->take_ownership(install->revision, install->content, install->staged_paths);
        boost::asio::post(io_, [this, alive, install, result]() {
          if (!alive.expired()) {
            commit_done(install, result);
          }
        });
      });
    }
  }

  void service_t::commit_done(const std::shared_ptr<install_t> &install, const platf::clipboard_result_e result) {
    committing_ = false;
    const auto connection = find_connection(install->channel);
    if (result == platf::clipboard_result_e::ok) {
      committed_sequence_ = install->sequence;
      if (install->cancelled || !connection) {
        // The install was superseded while it took ownership. Unless the host clipboard changed
        // since the pull began (the newer host content then stays as it is), Sunshine's content
        // replaced what the host held, so releasing it leaves the host clipboard empty.
        if (host_.revision == install->revision) {
          release_ownership();
        } else {
          post_release();
        }
      } else {
        release_root(current_root_);
        current_root_ = install->root;
        owner_ = install->channel;
        host_.formats.clear();
        for (const auto &other : connections()) {
          if (other == connection) {
            other->channel().host_superseded();
          } else {
            other->channel().clear_host();
          }
        }
        BOOST_LOG(debug) << "Installed client generation "sv << install->generation << ": "sv << install->content.size() << " formats, "sv << install->staged_paths.size() << " staged top-level entries"sv;
        finish_install(install, status_installed);
      }
    } else if (!install->cancelled) {
      finish_install(install, to_string(error_for(result)));
    }
    run_commits();
  }

  void service_t::start_files(const std::shared_ptr<install_t> &install, channel_t &channel) {
    install->files_pending = true;
    ++install->pending;
    std::weak_ptr<bool> alive = alive_;
    const auto stream = channel.request(
      msg_e::lock,
      serialize_single("generation", install->generation),
      {4096, std::nullopt, true},
      {
        [install](const std::string_view payload) {
          if (const auto reply = parse_lock_reply(payload)) {
            install->lock = *reply;
            install->lease = reply->lease;
          }
        },
        [this, alive, install]() {
          if (!alive.expired()) {
            files_locked(install);
          }
        },
        [this, alive, install](const error_e error, const std::string &) {
          if (!alive.expired()) {
            files_finished(install, error);
          }
        },
      }
    );
    install->streams.push_back(stream);
  }

  void service_t::files_locked(const std::shared_ptr<install_t> &install) {
    const auto connection = find_connection(install->channel);
    if (install->cancelled || !connection) {
      return;
    }
    if (install->lease == 0) {
      files_finished(install, error_e::io_error);
      return;
    }
    BOOST_LOG(debug) << "Client LOCK_REPLY lease "sv << install->lease << ": "sv << install->lock.count << " entries, "sv << install->lock.total_bytes << " bytes"sv;
    if (install->lock.count > limit::max_manifest_entries || install->lock.total_bytes > connection->channel().limits().max_transfer_size) {
      files_finished(install, error_e::too_large);
      return;
    }
    const auto total = install->lock.total_bytes;
    std::weak_ptr<bool> alive = alive_;
    boost::asio::post(worker_, [this, alive, install, total]() {
      if (alive.expired()) {
        return;
      }
      std::shared_ptr<staging::root_t> root;
      auto error = error_e::io_error;
      if (area_ && area_->open()) {
        area_->reap(std::chrono::system_clock::now());
        if (const auto free = area_->free_bytes(); free && *free < total) {
          BOOST_LOG(warning) << "Not enough free space to stage the client's files"sv;
          error = error_e::too_large;
        } else if (free) {
          root = area_->create_root();
        }
      }
      boost::asio::post(io_, [this, alive, install, root, error]() {
        if (!alive.expired()) {
          files_root(install, root, error);
        }
      });
    });
  }

  void service_t::files_root(const std::shared_ptr<install_t> &install, std::shared_ptr<staging::root_t> root, const error_e error) {
    const auto connection = find_connection(install->channel);
    if (install->cancelled || !connection) {
      discard_root(root);
      return;
    }
    if (!root) {
      files_finished(install, error);
      return;
    }
    install->root = std::move(root);
    std::weak_ptr<bool> alive = alive_;
    const auto stream = connection->channel().request(
      msg_e::get_file_list,
      serialize_single("lease", install->lease),
      {limit::max_manifest_bytes, std::nullopt, true},
      {
        [install](const std::string_view bytes) {
          install->manifest.append(bytes);
        },
        [this, alive, install]() {
          boost::asio::post(worker_, [this, alive, install]() {
            if (alive.expired()) {
              return;
            }
            auto entries = std::make_shared<std::optional<std::vector<file_entry_t>>>(files::parse_manifest(install->manifest));
            std::uint64_t total = 0;
            if (*entries) {
              for (const auto &entry : **entries) {
                total += entry.size;
              }
            }
            const auto valid = *entries && (*entries)->size() == install->lock.count && total == install->lock.total_bytes && !install->root->prepare(**entries);
            boost::asio::post(io_, [this, alive, install, entries, valid]() {
              if (alive.expired() || install->cancelled) {
                return;
              }
              if (!valid) {
                BOOST_LOG(warning) << "Refused a client's file manifest"sv;
                files_finished(install, error_e::io_error);
                return;
              }
              install->entries = std::move(**entries);
              install->manifest.clear();
              next_file(install);
            });
          });
        },
        [this, alive, install](const error_e error, const std::string &) {
          if (!alive.expired()) {
            files_finished(install, error);
          }
        },
      }
    );
    install->streams.push_back(stream);
  }

  void service_t::next_file(const std::shared_ptr<install_t> &install) {
    if (install->cancelled) {
      return;
    }
    while (install->next_entry < install->entries.size() && install->entries[install->next_entry].dir) {
      ++install->next_entry;
    }
    if (install->next_entry >= install->entries.size()) {
      if (const auto connection = find_connection(install->channel)) {
        connection->channel().unlock(install->lease);
      }
      install->lease = 0;
      install->staged_paths = install->root->top_level_paths();
      files_finished(install, std::nullopt);
      return;
    }
    const auto index = install->next_entry;
    auto file = std::make_shared<shared_fd_t>();
    file->remaining = install->entries[index].size;
    file->mtime = install->entries[index].mtime;
    std::weak_ptr<bool> alive = alive_;
    boost::asio::post(worker_, [this, alive, install, index, file]() {
      if (alive.expired()) {
        return;
      }
      file->fd = install->root->create_file(index);
      if (file->fd >= 0 && file->remaining == 0) {
        set_mtime(file->fd, file->mtime);
      }
      boost::asio::post(io_, [this, alive, install, index, file]() {
        if (alive.expired() || install->cancelled) {
          return;
        }
        if (file->fd < 0) {
          files_finished(install, error_e::io_error);
        } else if (file->remaining == 0) {
          ++install->next_entry;
          next_file(install);
        } else {
          transfer_file(install, index, file);
        }
      });
    });
  }

  void service_t::transfer_file(const std::shared_ptr<install_t> &install, const std::size_t index, const std::shared_ptr<shared_fd_t> &file) {
    const auto connection = find_connection(install->channel);
    if (!connection) {
      return;
    }
    std::weak_ptr<connection_t> weak = connection;
    std::weak_ptr<bool> alive = alive_;
    auto stream = std::make_shared<std::uint32_t>(0);
    const auto size = install->entries[index].size;
    *stream = connection->channel().request(
      msg_e::get_file,
      serialize(get_file_t {install->lease, index, 0, size}),
      {std::nullopt, size, false},
      {
        [this, alive, weak, stream, file](const std::string_view bytes) {
          auto chunk = std::make_shared<std::string>(bytes);
          boost::asio::post(worker_, [this, alive, weak, stream, file, chunk]() {
            if (alive.expired()) {
              return;
            }
            if (!file->failed && !write_all(file->fd, chunk->data(), chunk->size())) {
              file->failed = true;
            }
            boost::asio::post(io_, [weak, stream, size = chunk->size()]() {
              if (const auto connection = weak.lock()) {
                connection->channel().grant(*stream, size);
              }
            });
          });
        },
        [this, alive, install, file]() {
          boost::asio::post(worker_, [this, alive, install, file]() {
            if (alive.expired()) {
              return;
            }
            set_mtime(file->fd, file->mtime);
            boost::asio::post(io_, [this, alive, install, file]() {
              if (alive.expired() || install->cancelled) {
                return;
              }
              if (file->failed) {
                files_finished(install, error_e::io_error);
                return;
              }
              ++install->next_entry;
              next_file(install);
            });
          });
        },
        [this, alive, install](const error_e error, const std::string &) {
          if (!alive.expired()) {
            files_finished(install, error);
          }
        },
      }
    );
    install->streams.push_back(*stream);
  }

  void service_t::files_finished(const std::shared_ptr<install_t> &install, const std::optional<error_e> error) {
    if (!install->files_pending) {
      return;
    }
    install->files_pending = false;
    if (error) {
      BOOST_LOG(debug) << "Staging the files of client generation "sv << install->generation << " failed: "sv << to_string(*error);
    } else {
      BOOST_LOG(debug) << "Staged the files of client generation "sv << install->generation;
    }
    if (error) {
      if (!install->failure) {
        install->failure = error;
      }
      if (const auto connection = find_connection(install->channel); connection && install->lease) {
        connection->channel().unlock(install->lease);
      }
      install->lease = 0;
    }
    --install->pending;
    check_install(install);
  }

  void service_t::release_ownership() {
    owner_.reset();
    host_.formats.clear();
    release_root(current_root_);
    current_root_.reset();
    post_release();
  }

  void service_t::post_release() {
    std::weak_ptr<bool> alive = alive_;
    boost::asio::post(worker_, [this, alive]() {
      if (!alive.expired()) {
        backend_->release();
      }
    });
  }

  void service_t::release_root(const std::shared_ptr<staging::root_t> &root) {
    if (!root) {
      return;
    }
    std::weak_ptr<bool> alive = alive_;
    boost::asio::post(worker_, [this, alive, root]() {
      if (!alive.expired() && area_) {
        area_->release(*root, std::chrono::system_clock::now());
      }
    });
  }

  void service_t::discard_root(const std::shared_ptr<staging::root_t> &root) {
    if (!root) {
      return;
    }
    boost::asio::post(worker_, [root]() {
      root->remove();
    });
  }

  void service_t::schedule_reaper() {
    std::weak_ptr<bool> alive = alive_;
    reaper_->expires_after(std::chrono::minutes {10});
    reaper_->async_wait([this, alive, timer = reaper_](const boost::system::error_code &ec) {
      if (ec || alive.expired() || !running_) {
        return;
      }
      boost::asio::post(worker_, [this, alive]() {
        if (!alive.expired() && area_) {
          area_->reap(std::chrono::system_clock::now());
        }
      });
      schedule_reaper();
    });
  }

  void set_service(std::shared_ptr<service_t> service) {
    std::lock_guard lock {instance_mutex};
    instance = std::move(service);
  }

  std::shared_ptr<service_t> get_service() {
    std::lock_guard lock {instance_mutex};
    return instance;
  }
}  // namespace clipboard
