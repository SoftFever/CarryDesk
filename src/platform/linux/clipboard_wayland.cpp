/**
 * @file src/platform/linux/clipboard_wayland.cpp
 * @brief Definitions for the Wayland clipboard backend on `ext-data-control-v1`.
 * @details The backend owns its own Wayland connection and dispatch thread. Every protocol request
 *          runs on that thread; other threads hand it tasks through a queue woken by an eventfd.
 *          The thread never blocks on clipboard data: offers are read through pipes drained by the
 *          caller of `read`, and Sunshine's own content is written to requesting applications by a
 *          separate writer thread with non-blocking descriptors and a stall deadline.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>

// lib includes
#include <wayland-client.h>

// generated includes
#include <ext-data-control-v1.h>

// local includes
#include "clipboard_wayland.h"
#include "src/clipboard/formats.h"
#include "src/clipboard/protocol.h"
#include "src/logging.h"

using namespace std::literals;

namespace wl {
  namespace {
    constexpr auto stall_timeout = 5s;  ///< Longest time a pipe may go without progress.
    constexpr auto task_timeout = 5s;  ///< Longest wait for the Wayland thread to run a task.
    constexpr std::uint64_t max_uri_list = 1048576;  ///< Largest `text/uri-list` read to detect files.

    using clock = std::chrono::steady_clock;

    /**
     * @brief Close a descriptor if it is open.
     *
     * @param fd Descriptor to close; set to -1.
     */
    void close_fd(int &fd) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }

    /**
     * @brief Writes Sunshine's clipboard content to applications that requested it.
     */
    class writer_t {
    public:
      writer_t() {
        wake_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        thread_ = std::thread([this]() {
          run();
        });
      }

      writer_t(const writer_t &) = delete;
      writer_t &operator=(const writer_t &) = delete;

      ~writer_t() {
        {
          std::lock_guard lock {mutex_};
          stop_ = true;
        }
        signal();
        thread_.join();
        close_fd(wake_);
      }

      /**
       * @brief Write bytes to a descriptor and close it.
       *
       * @param fd Write end handed over by the compositor; owned from now on.
       * @param bytes Bytes to write.
       */
      void add(const int fd, std::shared_ptr<const std::string> bytes) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        {
          std::lock_guard lock {mutex_};
          incoming_.push_back({fd, std::move(bytes), 0, clock::now() + stall_timeout});
        }
        signal();
      }

    private:
      /**
       * @brief One pending write.
       */
      struct job_t {
        int fd;  ///< Destination.
        std::shared_ptr<const std::string> bytes;  ///< Content.
        std::size_t offset;  ///< Bytes already written.
        clock::time_point deadline;  ///< Time after which the job is abandoned.
      };

      /**
       * @brief Wake the writer thread.
       */
      void signal() const {
        const std::uint64_t one = 1;
        [[maybe_unused]] const auto ignored = ::write(wake_, &one, sizeof(one));
      }

      /**
       * @brief Writer thread body.
       */
      void run() {
        sigset_t pipe_signal;
        sigemptyset(&pipe_signal);
        sigaddset(&pipe_signal, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipe_signal, nullptr);

        std::vector<job_t> jobs;
        while (true) {
          {
            std::lock_guard lock {mutex_};
            if (stop_) {
              break;
            }
            for (auto &job : incoming_) {
              jobs.push_back(std::move(job));
            }
            incoming_.clear();
          }

          std::vector<pollfd> fds {{wake_, POLLIN, 0}};
          auto deadline = clock::time_point::max();
          for (const auto &job : jobs) {
            fds.push_back({job.fd, POLLOUT, 0});
            deadline = std::min(deadline, job.deadline);
          }
          const auto timeout = deadline == clock::time_point::max() ? -1 : static_cast<int>(std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count()) + 1);
          if (poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR) {
            break;
          }
          if (fds[0].revents & POLLIN) {
            std::uint64_t value;
            [[maybe_unused]] const auto ignored = ::read(wake_, &value, sizeof(value));
          }

          const auto now = clock::now();
          for (std::size_t i = 0; i < jobs.size();) {
            auto &job = jobs[i];
            bool done = job.offset == job.bytes->size();
            if (!done && (fds[i + 1].revents & (POLLOUT | POLLERR | POLLHUP))) {
              const auto written = ::write(job.fd, job.bytes->data() + job.offset, job.bytes->size() - job.offset);
              if (written > 0) {
                job.offset += static_cast<std::size_t>(written);
                job.deadline = now + stall_timeout;
                done = job.offset == job.bytes->size();
              } else if (written < 0 && errno != EAGAIN && errno != EINTR) {
                done = true;
                if (errno == EPIPE) {
                  const timespec zero {};
                  sigtimedwait(&pipe_signal, nullptr, &zero);
                }
              }
            }
            if (!done && now >= job.deadline) {
              BOOST_LOG(warning) << "[clipboard] An application stopped reading clipboard data"sv;
              done = true;
            }
            if (done) {
              close_fd(job.fd);
              jobs.erase(jobs.begin() + static_cast<std::ptrdiff_t>(i));
              fds.erase(fds.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            } else {
              ++i;
            }
          }
        }
        for (auto &job : jobs) {
          close_fd(job.fd);
        }
        std::lock_guard lock {mutex_};
        for (auto &job : incoming_) {
          close_fd(job.fd);
        }
      }

      int wake_ = -1;  ///< Wakes the thread for new jobs or stop.
      std::mutex mutex_;  ///< Guards `incoming_` and `stop_`.
      std::vector<job_t> incoming_;  ///< Jobs not yet picked up.
      bool stop_ = false;  ///< The thread should exit.
      std::thread thread_;  ///< Writer thread.
    };

    /**
     * @brief Clipboard backend on `ext-data-control-v1`.
     */
    class data_control_clipboard_t: public platf::clipboard_t {
    public:
      ~data_control_clipboard_t() override {
        stop();
      }

      bool start(platf::clipboard_observer_t &observer) override {
        observer_ = &observer;
        display_ = wl_display_connect(nullptr);
        if (!display_) {
          BOOST_LOG(info) << "[clipboard] No Wayland display to connect to"sv;
          return false;
        }
        registry_ = wl_display_get_registry(display_);
        wl_registry_add_listener(registry_, &registry_listener, this);
        wl_display_roundtrip(display_);
        if (!manager_ || !seat_) {
          BOOST_LOG(info) << "[clipboard] The compositor does not offer ext_data_control_manager_v1"sv;
          disconnect();
          return false;
        }
        device_ = ext_data_control_manager_v1_get_data_device(manager_, seat_);
        ext_data_control_device_v1_add_listener(device_, &device_listener, this);
        wl_display_flush(display_);

        wake_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        writer_ = std::make_unique<writer_t>();
        thread_ = std::thread([this]() {
          run();
        });
        thread_id_ = thread_.get_id();
        started_ = true;
        BOOST_LOG(info) << "[clipboard] Using the Wayland ext-data-control clipboard"sv;
        return true;
      }

      void stop() override {
        if (!started_.exchange(false)) {
          return;
        }
        run_task([this]() {
          release_locked();
        });
        stopping_ = true;
        signal();
        thread_.join();
        writer_.reset();
        close_fd(wake_);
      }

      [[nodiscard]] std::vector<std::string> formats() const override {
        return {std::string {clipboard::mime::text}, std::string {clipboard::mime::html}, std::string {clipboard::mime::rtf}, std::string {clipboard::mime::png}};
      }

      platf::clipboard_read_t read(const std::uint64_t revision, const std::string &mime, const std::uint64_t max_size) override {
        platf::clipboard_read_t result;
        const auto canonical = mime;
        auto fd = std::make_shared<int>(-1);
        auto status = std::make_shared<platf::clipboard_result_e>(platf::clipboard_result_e::io_error);
        const auto ran = run_task([this, revision, canonical, fd, status]() {
          if (revision != revision_ || !current_offer_) {
            *status = platf::clipboard_result_e::stale;
            return;
          }
          const auto offer = clipboard::formats::map_linux_offer(current_mimes_, current_has_files_);
          const auto format = std::ranges::find(offer.formats, canonical, &clipboard::formats::native_format_t::canonical);
          if (offer.sensitive || format == offer.formats.end()) {
            *status = platf::clipboard_result_e::stale;
            return;
          }
          *fd = open_receive(current_offer_, format->native);
          *status = *fd >= 0 ? platf::clipboard_result_e::ok : platf::clipboard_result_e::io_error;
        });
        if (!ran || *status != platf::clipboard_result_e::ok) {
          close_fd(*fd);
          result.result = ran ? *status : platf::clipboard_result_e::io_error;
          return result;
        }

        const auto files = canonical == clipboard::mime::files;
        std::string bytes;
        result.result = drain_pipe(*fd, files ? max_uri_list : max_size, stall_timeout, bytes);
        close_fd(*fd);
        if (result.result == platf::clipboard_result_e::ok && revision_ != revision) {
          result.result = platf::clipboard_result_e::stale;
        }
        if (result.result != platf::clipboard_result_e::ok) {
          return result;
        }
        if (files) {
          result.paths = clipboard::formats::file_paths_from_uri_list(bytes);
        } else {
          result.bytes = clipboard::formats::linux_to_canonical(canonical, bytes);
        }
        return result;
      }

      platf::clipboard_result_e take_ownership(const std::uint64_t expected_revision, const std::map<std::string, std::string, std::less<>> &content, const std::vector<std::string> &file_paths) override {
        auto offers = std::make_shared<clipboard::formats::native_offers_t>(clipboard::formats::linux_native_offers(content, file_paths));
        auto status = std::make_shared<platf::clipboard_result_e>(platf::clipboard_result_e::io_error);
        const auto ran = run_task([this, expected_revision, offers, status]() {
          if (!device_) {
            return;
          }
          if (expected_revision != revision_) {
            *status = platf::clipboard_result_e::stale;
            return;
          }
          auto *source = ext_data_control_manager_v1_create_data_source(manager_);
          ext_data_control_source_v1_add_listener(source, &source_listener, this);
          for (const auto &[type, bytes] : *offers) {
            ext_data_control_source_v1_offer(source, type.c_str());
          }
          ext_data_control_device_v1_set_selection(device_, source);
          if (source_) {
            sources_.erase(source_);
            ext_data_control_source_v1_destroy(source_);
          }
          source_ = source;
          sources_[source] = offers;
          expect_own_null_ = false;
          *status = platf::clipboard_result_e::ok;
        });
        return ran ? *status : platf::clipboard_result_e::io_error;
      }

      void release() override {
        run_task([this]() {
          release_locked();
        });
      }

    private:
      /**
       * @brief Clear the selection if Sunshine still owns it; runs on the Wayland thread.
       */
      void release_locked() {
        if (!source_ || !device_) {
          return;
        }
        ext_data_control_device_v1_set_selection(device_, nullptr);
        sources_.erase(source_);
        ext_data_control_source_v1_destroy(source_);
        source_ = nullptr;
        expect_own_null_ = true;
      }

      /**
       * @brief Start reading one type of an offer.
       *
       * @param offer Offer to read.
       * @param native Native type.
       * @return Read end of the pipe, or -1 on failure.
       */
      int open_receive(ext_data_control_offer_v1 *offer, const std::string &native) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) {
          return -1;
        }
        ext_data_control_offer_v1_receive(offer, native.c_str(), fds[1]);
        wl_display_flush(display_);
        ::close(fds[1]);
        return fds[0];
      }

      /**
       * @brief Run a task on the Wayland thread and wait for it.
       *
       * @param task Task to run.
       * @return `false` when the thread did not run it in time or is stopping.
       */
      bool run_task(std::function<void()> task) {
        if (std::this_thread::get_id() == thread_id_) {
          task();
          return true;
        }
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        {
          std::lock_guard lock {tasks_mutex_};
          if (stopping_ || thread_id_ == std::thread::id {}) {
            return false;
          }
          tasks_.push_back([task = std::move(task), done]() {
            task();
            done->set_value(true);
          });
        }
        signal();
        return future.wait_for(task_timeout) == std::future_status::ready && future.get();
      }

      /**
       * @brief Wake the Wayland thread.
       */
      void signal() const {
        const std::uint64_t one = 1;
        [[maybe_unused]] const auto ignored = ::write(wake_, &one, sizeof(one));
      }

      /**
       * @brief Run queued tasks on the Wayland thread.
       */
      void run_tasks() {
        std::deque<std::function<void()>> tasks;
        {
          std::lock_guard lock {tasks_mutex_};
          tasks.swap(tasks_);
        }
        for (auto &task : tasks) {
          task();
        }
      }

      /**
       * @brief Wayland thread body: dispatch events, run tasks and finish `text/uri-list` probes.
       */
      void run() {
        const auto fd = wl_display_get_fd(display_);
        while (!stopping_) {
          run_tasks();
          while (wl_display_prepare_read(display_) != 0) {
            wl_display_dispatch_pending(display_);
          }
          wl_display_flush(display_);

          std::vector<pollfd> fds {{fd, POLLIN, 0}, {wake_, POLLIN, 0}};
          int timeout = -1;
          if (probe_fd_ >= 0) {
            fds.push_back({probe_fd_, POLLIN, 0});
            timeout = static_cast<int>(std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(probe_deadline_ - clock::now()).count()) + 1);
          }
          const auto ready = poll(fds.data(), fds.size(), timeout);
          if (ready > 0 && (fds[0].revents & POLLIN)) {
            if (wl_display_read_events(display_) < 0) {
              break;
            }
          } else {
            wl_display_cancel_read(display_);
          }
          if (ready < 0 && errno != EINTR) {
            break;
          }
          if (fds[0].revents & (POLLERR | POLLHUP)) {
            BOOST_LOG(warning) << "[clipboard] Lost the Wayland connection"sv;
            break;
          }
          if (wl_display_dispatch_pending(display_) < 0) {
            BOOST_LOG(warning) << "[clipboard] Wayland dispatch failed"sv;
            break;
          }
          if (fds[1].revents & POLLIN) {
            std::uint64_t value;
            [[maybe_unused]] const auto ignored = ::read(wake_, &value, sizeof(value));
          }
          if (probe_fd_ >= 0) {
            continue_probe(fds.size() > 2 ? fds[2].revents : 0);
          }
        }

        {
          std::lock_guard lock {tasks_mutex_};
          stopping_ = true;
        }
        run_tasks();
        if (device_ && source_) {
          release_locked();
        }
        disconnect();
      }

      /**
       * @brief Destroy every protocol object and close the connection.
       */
      void disconnect() {
        close_fd(probe_fd_);
        if (current_offer_) {
          ext_data_control_offer_v1_destroy(current_offer_);
          current_offer_ = nullptr;
        }
        for (const auto &[offer, mimes] : pending_offers_) {
          ext_data_control_offer_v1_destroy(offer);
        }
        pending_offers_.clear();
        for (const auto &[source, offers] : sources_) {
          ext_data_control_source_v1_destroy(source);
        }
        sources_.clear();
        source_ = nullptr;
        if (device_) {
          ext_data_control_device_v1_destroy(device_);
          device_ = nullptr;
        }
        if (manager_) {
          ext_data_control_manager_v1_destroy(manager_);
          manager_ = nullptr;
        }
        if (seat_) {
          wl_seat_destroy(seat_);
          seat_ = nullptr;
        }
        if (registry_) {
          wl_registry_destroy(registry_);
          registry_ = nullptr;
        }
        if (display_) {
          wl_display_flush(display_);
          wl_display_disconnect(display_);
          display_ = nullptr;
        }
      }

      /**
       * @brief Report the current selection to the observer.
       */
      void report() {
        if (current_probe_pending_) {
          return;
        }
        auto offer = clipboard::formats::map_linux_offer(current_mimes_, current_has_files_);
        std::vector<std::string> formats;
        if (!offer.sensitive) {
          for (auto &format : offer.formats) {
            formats.push_back(std::move(format.canonical));
          }
        }
        observer_->on_clipboard_changed(revision_, std::move(formats));
      }

      /**
       * @brief Read more of the `text/uri-list` probe of the current selection.
       *
       * @param revents Poll result of the probe descriptor.
       */
      void continue_probe(const short revents) {
        bool finished = false;
        if (revents & (POLLIN | POLLHUP | POLLERR)) {
          char buffer[4096];
          const auto size = ::read(probe_fd_, buffer, sizeof(buffer));
          if (size > 0) {
            probe_bytes_.append(buffer, static_cast<std::size_t>(size));
            probe_deadline_ = clock::now() + stall_timeout;
            finished = probe_bytes_.size() > max_uri_list;
          } else if (size == 0 || (errno != EAGAIN && errno != EINTR)) {
            finished = true;
          }
        }
        if (!finished && clock::now() >= probe_deadline_) {
          finished = true;
        }
        if (finished) {
          close_fd(probe_fd_);
          current_has_files_ = !clipboard::formats::file_paths_from_uri_list(probe_bytes_).empty();
          probe_bytes_.clear();
          current_probe_pending_ = false;
          report();
        }
      }

      /**
       * @brief Handle a new selection.
       *
       * @param offer Offer of the selection, or null when the clipboard was emptied.
       */
      void on_selection(ext_data_control_offer_v1 *offer) {
        std::vector<std::string> mimes;
        if (offer) {
          if (const auto it = pending_offers_.find(offer); it != pending_offers_.end()) {
            mimes = std::move(it->second);
            pending_offers_.erase(it);
          }
        }
        if (source_ || (!offer && expect_own_null_)) {
          // The selection is Sunshine's own: either the echo of a source it set, or the empty
          // selection after it released one. Neither is announced.
          expect_own_null_ = false;
          if (offer) {
            ext_data_control_offer_v1_destroy(offer);
          }
          drop_current();
          return;
        }

        drop_current();
        ++revision_;
        current_offer_ = offer;
        current_mimes_ = std::move(mimes);
        current_has_files_ = false;
        if (offer && std::ranges::find(current_mimes_, clipboard::formats::uri_list) != current_mimes_.end()) {
          probe_fd_ = open_receive(offer, std::string {clipboard::formats::uri_list});
          if (probe_fd_ >= 0) {
            fcntl(probe_fd_, F_SETFL, fcntl(probe_fd_, F_GETFL) | O_NONBLOCK);
            probe_deadline_ = clock::now() + stall_timeout;
            current_probe_pending_ = true;
            return;
          }
        }
        report();
      }

      /**
       * @brief Forget the current foreign selection.
       */
      void drop_current() {
        close_fd(probe_fd_);
        probe_bytes_.clear();
        current_probe_pending_ = false;
        if (current_offer_) {
          ext_data_control_offer_v1_destroy(current_offer_);
          current_offer_ = nullptr;
        }
        current_mimes_.clear();
        current_has_files_ = false;
      }

      static void handle_global(void *data, wl_registry *registry, const std::uint32_t name, const char *interface, const std::uint32_t version) {
        auto *self = static_cast<data_control_clipboard_t *>(data);
        if (std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0 && !self->manager_) {
          self->manager_ = static_cast<ext_data_control_manager_v1 *>(wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
        } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !self->seat_) {
          self->seat_ = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, std::min<std::uint32_t>(version, 1)));
        }
      }

      static void handle_global_remove(void *, wl_registry *, std::uint32_t) {
      }

      static void handle_data_offer(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer) {
        auto *self = static_cast<data_control_clipboard_t *>(data);
        self->pending_offers_[offer] = {};
        ext_data_control_offer_v1_add_listener(offer, &offer_listener, self);
      }

      static void handle_selection(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer) {
        static_cast<data_control_clipboard_t *>(data)->on_selection(offer);
      }

      static void handle_finished(void *data, ext_data_control_device_v1 *) {
        auto *self = static_cast<data_control_clipboard_t *>(data);
        BOOST_LOG(warning) << "[clipboard] The Wayland data device is no longer valid"sv;
        self->drop_current();
        ext_data_control_device_v1_destroy(self->device_);
        self->device_ = nullptr;
      }

      static void handle_primary_selection(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer) {
        auto *self = static_cast<data_control_clipboard_t *>(data);
        if (offer) {
          self->pending_offers_.erase(offer);
          ext_data_control_offer_v1_destroy(offer);
        }
      }

      static void handle_offer(void *data, ext_data_control_offer_v1 *offer, const char *mime_type) {
        auto *self = static_cast<data_control_clipboard_t *>(data);
        if (const auto it = self->pending_offers_.find(offer); it != self->pending_offers_.end()) {
          it->second.emplace_back(mime_type);
        }
      }

      static void handle_send(void *data, ext_data_control_source_v1 *source, const char *mime_type, const std::int32_t fd) {
        auto *self = static_cast<data_control_clipboard_t *>(data);
        const auto it = self->sources_.find(source);
        if (it != self->sources_.end()) {
          for (const auto &[type, bytes] : *it->second) {
            if (type == mime_type) {
              self->writer_->add(fd, bytes);
              return;
            }
          }
        }
        ::close(fd);
      }

      static void handle_cancelled(void *data, ext_data_control_source_v1 *source) {
        auto *self = static_cast<data_control_clipboard_t *>(data);
        self->sources_.erase(source);
        if (self->source_ == source) {
          self->source_ = nullptr;
        }
        ext_data_control_source_v1_destroy(source);
      }

      static constexpr wl_registry_listener registry_listener {&handle_global, &handle_global_remove};  ///< Registry callbacks.
      static constexpr ext_data_control_device_v1_listener device_listener {&handle_data_offer, &handle_selection, &handle_finished, &handle_primary_selection};  ///< Device callbacks.
      static constexpr ext_data_control_offer_v1_listener offer_listener {&handle_offer};  ///< Offer callbacks.
      static constexpr ext_data_control_source_v1_listener source_listener {&handle_send, &handle_cancelled};  ///< Source callbacks.

      platf::clipboard_observer_t *observer_ = nullptr;  ///< Receives foreign changes.
      wl_display *display_ = nullptr;  ///< Own Wayland connection.
      wl_registry *registry_ = nullptr;  ///< Registry of globals.
      wl_seat *seat_ = nullptr;  ///< First seat.
      ext_data_control_manager_v1 *manager_ = nullptr;  ///< Data-control manager.
      ext_data_control_device_v1 *device_ = nullptr;  ///< Data-control device of the seat.

      std::map<ext_data_control_offer_v1 *, std::vector<std::string>> pending_offers_;  ///< Offers whose types are still arriving.
      ext_data_control_offer_v1 *current_offer_ = nullptr;  ///< Offer of the current foreign selection.
      std::vector<std::string> current_mimes_;  ///< Types of the current foreign selection.
      bool current_has_files_ = false;  ///< The current selection's `text/uri-list` holds a local file.
      bool current_probe_pending_ = false;  ///< The `text/uri-list` probe is still reading.
      std::atomic<std::uint64_t> revision_ {0};  ///< Revision of the current foreign selection.
      int probe_fd_ = -1;  ///< Read end of the `text/uri-list` probe.
      std::string probe_bytes_;  ///< Bytes read by the probe.
      clock::time_point probe_deadline_;  ///< Stall deadline of the probe.

      ext_data_control_source_v1 *source_ = nullptr;  ///< Sunshine's current selection source.
      std::map<ext_data_control_source_v1 *, std::shared_ptr<clipboard::formats::native_offers_t>> sources_;  ///< Content of live sources.
      bool expect_own_null_ = false;  ///< The next empty selection is Sunshine's own release.

      std::unique_ptr<writer_t> writer_;  ///< Writes content to requesting applications.
      int wake_ = -1;  ///< Wakes the Wayland thread.
      std::mutex tasks_mutex_;  ///< Guards `tasks_`.
      std::deque<std::function<void()>> tasks_;  ///< Tasks for the Wayland thread.
      std::atomic<bool> stopping_ {false};  ///< The Wayland thread should exit.
      std::atomic<bool> started_ {false};  ///< `start` succeeded and `stop` has not run.
      std::thread thread_;  ///< Wayland thread.
      std::thread::id thread_id_;  ///< Id of the Wayland thread, set once at start.
    };
  }  // namespace

  std::unique_ptr<platf::clipboard_t> make_clipboard() {
    return std::make_unique<data_control_clipboard_t>();
  }

  platf::clipboard_result_e drain_pipe(const int fd, const std::uint64_t max_size, const std::chrono::milliseconds stall_timeout, std::string &bytes) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    auto deadline = clock::now() + stall_timeout;
    std::vector<char> buffer(65536);
    while (true) {
      const auto ssize = ::read(fd, buffer.data(), buffer.size());
      if (ssize > 0) {
        const auto size = static_cast<std::size_t>(ssize);
        if (bytes.size() + size > max_size) {
          return platf::clipboard_result_e::too_large;
        }
        bytes.append(buffer.data(), size);
        deadline = clock::now() + stall_timeout;
        continue;
      }
      if (ssize == 0) {
        return platf::clipboard_result_e::ok;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno != EAGAIN) {
        return platf::clipboard_result_e::io_error;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
      if (remaining <= 0) {
        return platf::clipboard_result_e::io_error;
      }
      pollfd pfd {fd, POLLIN, 0};
      if (poll(&pfd, 1, static_cast<int>(remaining)) < 0 && errno != EINTR) {
        return platf::clipboard_result_e::io_error;
      }
    }
  }
}  // namespace wl
