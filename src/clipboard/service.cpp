/**
 * @file src/clipboard/service.cpp
 * @brief Definitions for the host clipboard service.
 */
// standard includes
#include <algorithm>
#include <utility>

// lib includes
#include <boost/asio/post.hpp>

// local includes
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
  }  // namespace

  service_t::service_t(service_config_t config, std::unique_ptr<platf::clipboard_t> backend, boost::asio::any_io_executor io, boost::asio::any_io_executor worker, clock_fn clock, std::string os):
      config_ {config},
      backend_ {std::move(backend)},
      io_ {std::move(io)},
      worker_ {std::move(worker)},
      clock_ {std::move(clock)},
      os_ {std::move(os)} {
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
    running_ = true;
    BOOST_LOG(info) << "Clipboard sync is available"sv;
    return true;
  }

  void service_t::stop(const std::chrono::milliseconds wait) {
    const auto was_running = running_.exchange(false);
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
    BOOST_LOG(debug) << "Client clipboard generation "sv << generation << " offers "sv << formats.size() << " formats"sv;

    std::weak_ptr<bool> alive = alive_;
    for (const auto &format : formats) {
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
            install->content.try_emplace(mime);
            --install->pending;
            check_install(install);
          },
          [this, alive, install, mime](const error_e error, const std::string &) {
            if (alive.expired()) {
              return;
            }
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

  void service_t::serve_lock(channel_t &channel, const std::uint32_t stream, std::uint64_t) {
    channel.respond_error(stream, error_e::io_error, "files are not supported");
  }

  void service_t::serve_file(channel_t &channel, const std::uint32_t stream, const std::shared_ptr<const manifest_t> &, std::uint64_t, std::uint64_t, std::uint64_t) {
    channel.respond_error(stream, error_e::io_error, "files are not supported");
  }

  void service_t::cancel_serving(channel_t &, std::uint32_t) {
    // Pending backend reads finish within their deadline; the channel drops answers for ended streams.
  }

  void service_t::on_lease_released(channel_t &, const std::shared_ptr<const manifest_t> &) {
    // The manifest's context releases its resources when the last reference goes away.
  }

  void service_t::on_closed(channel_t &channel) {
    const auto id = channel.id();
    cancel_install(id);
    if (owner_ == id) {
      owner_.reset();
      host_.formats.clear();
      std::weak_ptr<bool> alive = alive_;
      boost::asio::post(worker_, [this, alive]() {
        if (!alive.expired()) {
          backend_->release();
        }
      });
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
    BOOST_LOG(debug) << "Host clipboard revision "sv << revision << " offers "sv << formats.size() << " formats"sv;
    host_ = {revision, std::move(formats)};
    owner_.reset();

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
    if (const auto it = installs_.find(install->channel); it != installs_.end() && it->second == install) {
      installs_.erase(it);
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
        const auto result = backend_->take_ownership(install->revision, install->content, {});
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
        owner_.reset();
        host_.formats.clear();
        std::weak_ptr<bool> alive = alive_;
        boost::asio::post(worker_, [this, alive]() {
          if (!alive.expired()) {
            backend_->release();
          }
        });
      } else {
        owner_ = install->channel;
        host_.formats.clear();
        for (const auto &other : connections()) {
          if (other == connection) {
            other->channel().host_superseded();
          } else {
            other->channel().clear_host();
          }
        }
        BOOST_LOG(debug) << "Installed client clipboard generation "sv << install->generation;
        finish_install(install, status_installed);
      }
    } else if (!install->cancelled) {
      finish_install(install, to_string(error_for(result)));
    }
    run_commits();
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
