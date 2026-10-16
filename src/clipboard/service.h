/**
 * @file src/clipboard/service.h
 * @brief Declarations for the host clipboard service.
 * @details The service admits channels, tracks which clients have a running stream, owns the host
 *          clipboard on behalf of one channel at a time, and runs the clipboard backend. Clipboard
 *          state lives on the I/O executor that runs the channels; the session registry and the
 *          channel table are guarded by one mutex so admission, session changes and revocation
 *          from other threads never interleave. Blocking backend calls run on a worker executor.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

// lib includes
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

// local includes
#include "channel.h"
#include "src/platform/common.h"
#include "staging.h"

namespace clipboard {
  /**
   * @brief Host clipboard settings.
   */
  struct service_config_t {
    policy_t policy;  ///< Directions and files the host allows globally.
    limits_t limits;  ///< Configured limits in bytes, within the hard caps.
    std::filesystem::path staging_area = staging::default_area();  ///< Directory for files received from clients.
  };

  /**
   * @brief Outcome of admitting an upgrade request.
   */
  enum class admission_e {
    admitted,  ///< A channel was created.
    disabled,  ///< Clipboard sync is disabled on the host or the service is stopping.
    not_permitted,  ///< The client is not allowed to use the clipboard.
    no_session,  ///< The client has no running stream.
    too_many,  ///< The host already runs the most channels it allows.
  };

  /**
   * @brief A channel's transport as the service sees it.
   */
  class connection_t {
  public:
    virtual ~connection_t() = default;

    /**
     * @brief Get the channel carried by this connection.
     *
     * @return Channel; used only on the I/O executor.
     */
    virtual channel_t &channel() = 0;

    /**
     * @brief Close the channel and its transport; runs on the I/O executor.
     */
    virtual void close() = 0;
  };

  /**
   * @brief Creates the connection for an admitted channel.
   * @details Called under the service's mutex; it must not start I/O or call back into the service.
   */
  using connection_factory_t = std::function<std::shared_ptr<connection_t>(std::uint64_t id, host_settings_t settings, channel_delegate_t &delegate)>;

  /**
   * @brief Result of an admission.
   */
  struct admission_t {
    admission_e result = admission_e::disabled;  ///< Outcome.
    std::shared_ptr<connection_t> connection;  ///< The new connection when admitted.
  };

  /**
   * @brief Host clipboard service.
   */
  class service_t: public channel_delegate_t, public platf::clipboard_observer_t {
  public:
    /**
     * @brief Create the service.
     *
     * @param config Host clipboard settings.
     * @param backend Clipboard backend, or null when none works here.
     * @param io Executor that runs the channels.
     * @param worker Executor for blocking backend calls.
     * @param clock Clock for the channels' timers.
     * @param os Host operating system name sent to clients.
     */
    service_t(service_config_t config, std::unique_ptr<platf::clipboard_t> backend, boost::asio::any_io_executor io, boost::asio::any_io_executor worker, clock_fn clock, std::string os);

    service_t(const service_t &) = delete;
    service_t &operator=(const service_t &) = delete;
    ~service_t() override;

    /**
     * @brief Start the backend when clipboard sync is enabled.
     *
     * @return `true` when the service can admit channels.
     */
    bool start();

    /**
     * @brief Close every channel, wait for them to finish closing, and stop the backend.
     * @details Must not run on the I/O executor, which has to process the closes.
     *
     * @param wait Longest time to wait for channels to close.
     */
    void stop(std::chrono::milliseconds wait);

    /**
     * @brief Check whether the service admits channels.
     *
     * @return `true` when clipboard sync is enabled, the backend runs, and the service is not stopping.
     */
    [[nodiscard]] bool available() const;

    /**
     * @brief Check whether a client should see the clipboard feature flag.
     *
     * @param client_allowed Whether the client's per-client permission allows the clipboard.
     * @return `true` when the service is available and the client is allowed.
     */
    [[nodiscard]] bool feature_enabled(bool client_allowed) const;

    /**
     * @brief Record a stream session that started successfully.
     * @details A session already reported stopped is ignored.
     *
     * @param session Launch-session id.
     * @param cert PEM certificate of the session's client.
     * @param still_running Checked under the registry mutex; the session counts only when it returns true.
     */
    void session_started(std::uint64_t session, const std::string &cert, const std::function<bool()> &still_running);

    /**
     * @brief Record that a stream session stopped.
     * @details When the client's last session stops, its channel closes. A stop for a session never
     *          reported started leaves a tombstone so a late start is ignored.
     *
     * @param session Launch-session id.
     */
    void session_stopped(std::uint64_t session);

    /**
     * @brief Admit an upgrade request and create its channel.
     * @details Runs on the I/O executor. A previous channel of the same certificate is closed.
     *
     * @param cert PEM certificate of the verified client.
     * @param client_allowed Whether the client's per-client permission allows the clipboard.
     * @param factory Creates the connection.
     * @return Outcome and, when admitted, the connection to start.
     */
    admission_t admit(const std::string &cert, bool client_allowed, const connection_factory_t &factory);

    /**
     * @brief Close the channel of a client whose permission ended.
     *
     * @param cert PEM certificate of the client.
     */
    void revoke(const std::string &cert);

    /**
     * @brief Close every channel.
     */
    void revoke_all();

    /**
     * @brief Get the number of registered channels.
     *
     * @return Channel count.
     */
    [[nodiscard]] std::size_t channel_count() const;

    /**
     * @brief Get the number of running sessions recorded for a certificate.
     *
     * @param cert PEM certificate.
     * @return Session count.
     */
    [[nodiscard]] std::size_t session_count(const std::string &cert) const;

    /**
     * @brief Get the staged root offered on the host clipboard.
     * @details Read on the I/O executor.
     *
     * @return The root, or null when the host clipboard holds no staged files.
     */
    [[nodiscard]] std::shared_ptr<staging::root_t> current_root() const {
      return current_root_;
    }

    /**
     * @brief Get the channel that owns the host clipboard.
     * @details Read on the I/O executor.
     *
     * @return Owning channel id, or no value when the host owns it.
     */
    [[nodiscard]] std::optional<std::uint64_t> owner() const {
      return owner_;
    }

    std::optional<host_clipboard_t> current_host_clipboard(channel_t &channel) override;
    void on_open(channel_t &channel) override;
    void on_client_announce(channel_t &channel, std::uint64_t generation, const std::vector<format_t> &formats) override;
    void on_client_clear(channel_t &channel, std::uint64_t generation) override;
    void serve_get(channel_t &channel, std::uint32_t stream, std::uint64_t revision, const std::string &mime) override;
    void serve_lock(channel_t &channel, std::uint32_t stream, std::uint64_t revision) override;
    void serve_file(channel_t &channel, std::uint32_t stream, const std::shared_ptr<const manifest_t> &manifest, std::uint64_t index, std::uint64_t offset, std::uint64_t length) override;
    void cancel_serving(channel_t &channel, std::uint32_t stream) override;
    void on_lease_released(channel_t &channel, const std::shared_ptr<const manifest_t> &manifest) override;
    void on_closed(channel_t &channel) override;
    void on_clipboard_changed(std::uint64_t revision, std::vector<std::string> formats) override;

  private:
    /**
     * @brief A client announcement being pulled and installed.
     */
    struct install_t {
      std::uint64_t channel = 0;  ///< Announcing channel.
      std::uint64_t generation = 0;  ///< Client generation.
      std::uint64_t sequence = 0;  ///< Order of the announcement among all channels.
      std::uint64_t revision = 0;  ///< Host revision when the pull began.
      std::vector<format_t> formats;  ///< Formats to pull.
      std::map<std::string, std::string, std::less<>> content;  ///< Pulled canonical bytes.
      std::vector<std::uint32_t> streams;  ///< Outstanding requests.
      std::size_t pending = 0;  ///< Pulls not finished.
      std::size_t too_large = 0;  ///< Formats dropped for size.
      std::optional<error_e> failure;  ///< First failure that ends the install.
      bool cancelled = false;  ///< The install was superseded or its channel closed.
      bool files_pending = false;  ///< The `files` part is still being staged.
      std::uint64_t lease = 0;  ///< Client lease on the files, 0 when none is held.
      lock_reply_t lock;  ///< The client's LOCK_REPLY.
      std::string manifest;  ///< NDJSON manifest received so far.
      std::vector<file_entry_t> entries;  ///< Parsed manifest.
      std::shared_ptr<staging::root_t> root;  ///< Private root the files are staged into.
      std::size_t next_entry = 0;  ///< Next manifest entry to stage.
      std::vector<std::string> staged_paths;  ///< Top-level staged paths once staging finished.
    };

    /**
     * @brief A descriptor shared by the jobs of one file transfer, closed with its last owner.
     */
    struct shared_fd_t {
      shared_fd_t() = default;
      shared_fd_t(const shared_fd_t &) = delete;
      shared_fd_t &operator=(const shared_fd_t &) = delete;
      ~shared_fd_t();

      int fd = -1;  ///< Descriptor.
      std::uint64_t offset = 0;  ///< Next byte to read or write.
      std::uint64_t remaining = 0;  ///< Bytes left to read.
      std::int64_t mtime = 0;  ///< Modification time to set when a staged file is complete.
      bool failed = false;  ///< A write failed.
    };

    /**
     * @brief Find a registered connection.
     *
     * @param id Channel id.
     * @return The connection, or null when it is gone.
     */
    std::shared_ptr<connection_t> find_connection(std::uint64_t id) const;

    /**
     * @brief Get every registered connection.
     *
     * @return Snapshot of the connections.
     */
    std::vector<std::shared_ptr<connection_t>> connections() const;

    /**
     * @brief Ask a connection to close on the I/O executor.
     *
     * @param connection Connection to close.
     */
    void post_close(const std::shared_ptr<connection_t> &connection);

    /**
     * @brief Check whether a certificate has a running session; the caller holds the mutex.
     *
     * @param cert PEM certificate.
     * @return `true` when at least one session is recorded.
     */
    bool has_session_locked(const std::string &cert) const;

    /**
     * @brief Apply a host clipboard change on the I/O executor.
     *
     * @param revision New revision.
     * @param formats New canonical formats.
     */
    void handle_host_change(std::uint64_t revision, std::vector<std::string> formats);

    /**
     * @brief Stop an install and its outstanding pulls without answering it.
     *
     * @param channel_id Channel whose install to cancel.
     */
    void cancel_install(std::uint64_t channel_id);

    /**
     * @brief Continue an install after one of its pulls finished.
     *
     * @param install Install to check.
     */
    void check_install(const std::shared_ptr<install_t> &install);

    /**
     * @brief End an install with an ACK status.
     *
     * @param install Install to end.
     * @param status ACK status.
     */
    void finish_install(const std::shared_ptr<install_t> &install, std::string_view status);

    /**
     * @brief Start the next queued commit when none is running.
     */
    void run_commits();

    /**
     * @brief Apply a commit's outcome on the I/O executor.
     *
     * @param install Committed install.
     * @param result Backend outcome.
     */
    void commit_done(const std::shared_ptr<install_t> &install, platf::clipboard_result_e result);

    /**
     * @brief Start staging the client's files: request a lease.
     *
     * @param install Install with `files`.
     * @param channel Announcing channel.
     */
    void start_files(const std::shared_ptr<install_t> &install, channel_t &channel);

    /**
     * @brief Check the lease and create a private root once there is room for the files.
     *
     * @param install Install with a lease.
     */
    void files_locked(const std::shared_ptr<install_t> &install);

    /**
     * @brief Request the manifest into a new root.
     *
     * @param install Install with a lease.
     * @param root Created root, or null when there is no room or the root could not be created.
     * @param error Why there is no root.
     */
    void files_root(const std::shared_ptr<install_t> &install, std::shared_ptr<staging::root_t> root, error_e error);

    /**
     * @brief Stage the next file of the manifest, or finish.
     *
     * @param install Install whose manifest was prepared.
     */
    void next_file(const std::shared_ptr<install_t> &install);

    /**
     * @brief Transfer one file into its created descriptor.
     *
     * @param install Install being staged.
     * @param index Manifest index.
     * @param file Created file.
     */
    void transfer_file(const std::shared_ptr<install_t> &install, std::size_t index, const std::shared_ptr<shared_fd_t> &file);

    /**
     * @brief End the `files` part of an install.
     *
     * @param install Install being staged.
     * @param error Failure, or no value when every file was staged.
     */
    void files_finished(const std::shared_ptr<install_t> &install, std::optional<error_e> error);

    /**
     * @brief Give up the host clipboard: empty it if Sunshine still owns it and release its staged root.
     */
    void release_ownership();

    /**
     * @brief Ask the backend to empty the clipboard if Sunshine still owns it.
     */
    void post_release();

    /**
     * @brief Release a staged root on the worker; it is deleted after the retention.
     *
     * @param root Root to release; may be null.
     */
    void release_root(const std::shared_ptr<staging::root_t> &root);

    /**
     * @brief Delete a staged root that was never offered.
     *
     * @param root Root to delete; may be null.
     */
    void discard_root(const std::shared_ptr<staging::root_t> &root);

    /**
     * @brief Schedule the next periodic reap of released roots.
     */
    void schedule_reaper();

    service_config_t config_;  ///< Host clipboard settings.
    std::unique_ptr<platf::clipboard_t> backend_;  ///< Clipboard backend.
    boost::asio::any_io_executor io_;  ///< Executor running the channels and clipboard state.
    boost::asio::any_io_executor worker_;  ///< Executor for blocking backend calls.
    clock_fn clock_;  ///< Clock for channel timers.
    std::string os_;  ///< Host OS name.
    std::vector<std::string> formats_;  ///< Formats the backend supports.
    std::atomic<bool> running_ {false};  ///< The backend runs and the service admits channels.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  ///< Expires when the service is destroyed.

    mutable std::mutex mutex_;  ///< Guards the registry below.
    std::condition_variable closed_;  ///< Signalled when a channel leaves the registry.
    std::map<std::uint64_t, std::string> sessions_;  ///< Running sessions: launch-session id to certificate.
    std::deque<std::uint64_t> tombstones_;  ///< Sessions stopped before they were reported started.
    std::map<std::uint64_t, std::shared_ptr<connection_t>> connections_;  ///< Registered channels by id.
    std::map<std::uint64_t, std::string> channel_certs_;  ///< Certificate of each registered channel.
    std::map<std::string, std::uint64_t> cert_channels_;  ///< Channel of each certificate.
    std::uint64_t next_channel_ = 1;  ///< Next channel id.

    std::optional<std::uint64_t> owner_;  ///< Channel owning the host clipboard; I/O executor only.
    host_clipboard_t host_;  ///< Latest host-owned clipboard content; I/O executor only.
    std::map<std::uint64_t, std::shared_ptr<install_t>> installs_;  ///< Current install per channel.
    std::deque<std::shared_ptr<install_t>> commits_;  ///< Installs waiting to take ownership.
    bool committing_ = false;  ///< A commit runs on the worker.
    std::uint64_t announce_sequence_ = 0;  ///< Last assigned announcement order.
    std::uint64_t committed_sequence_ = 0;  ///< Order of the last committed announcement.
    std::shared_ptr<staging::area_t> area_;  ///< Staging area; used on the worker.
    std::shared_ptr<staging::root_t> current_root_;  ///< Staged root offered on the host clipboard.
    std::shared_ptr<boost::asio::steady_timer> reaper_;  ///< Periodic reap of released roots.
  };

  /**
   * @brief Set the process-wide service used by the HTTP, RTSP and web UI code.
   *
   * @param service Service, or null to remove it.
   */
  void set_service(std::shared_ptr<service_t> service);

  /**
   * @brief Get the process-wide service.
   *
   * @return The service, or null when none runs.
   */
  std::shared_ptr<service_t> get_service();
}  // namespace clipboard
