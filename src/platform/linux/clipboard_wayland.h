/**
 * @file src/platform/linux/clipboard_wayland.h
 * @brief Declarations for the Wayland clipboard backend on `ext-data-control-v1`.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "src/platform/common.h"

namespace wl {
  /**
   * @brief Create a clipboard backend for the Wayland session in `WAYLAND_DISPLAY`.
   * @details The backend connects when it starts; it fails to start when the compositor does not
   *          offer `ext_data_control_manager_v1` or a seat.
   *
   * @return Backend instance.
   */
  std::unique_ptr<platf::clipboard_t> make_clipboard();

  /**
   * @brief Check whether the compositor in `WAYLAND_DISPLAY` offers `ext_data_control_manager_v1` and a seat.
   *
   * @return `true` when a backend from `make_clipboard` can start.
   */
  bool offers_data_control();

  /**
   * @brief Read a pipe until EOF without blocking indefinitely.
   *
   * @param fd Read end of the pipe; the caller keeps ownership.
   * @param max_size Largest acceptable size.
   * @param stall_timeout Longest time without any byte arriving.
   * @param bytes Receives the bytes read.
   * @return `ok` at EOF, `too_large` beyond `max_size`, `io_error` on a read error or stall.
   */
  platf::clipboard_result_e drain_pipe(int fd, std::uint64_t max_size, std::chrono::milliseconds stall_timeout, std::string &bytes);

  /**
   * @brief Writes Sunshine's clipboard content to applications that requested it.
   * @details One thread serves every write with non-blocking descriptors, so an application that
   *          stops reading holds up only its own write, which is abandoned after a stall.
   */
  class writer_t {
  public:
    /**
     * @brief Start the writer thread.
     */
    writer_t();

    writer_t(const writer_t &) = delete;
    writer_t &operator=(const writer_t &) = delete;

    /**
     * @brief Stop the writer thread, abandoning the writes still pending.
     */
    ~writer_t();

    /**
     * @brief Write bytes to a descriptor and close it.
     *
     * @param fd Write end handed over by the compositor; owned from now on.
     * @param bytes Bytes to write.
     * @param done Called on the writer thread once the descriptor is closed, with whether every
     *             byte was written; may be empty.
     */
    void add(int fd, std::shared_ptr<const std::string> bytes, std::function<void(bool)> done = {});

  private:
    /**
     * @brief One pending write.
     */
    struct job_t {
      int fd;  ///< Destination.
      std::shared_ptr<const std::string> bytes;  ///< Content.
      std::size_t offset;  ///< Bytes already written.
      std::chrono::steady_clock::time_point deadline;  ///< Time after which the job is abandoned.
      std::function<void(bool)> done;  ///< Completion callback; may be empty.
    };

    /**
     * @brief Wake the writer thread.
     */
    void signal() const;

    /**
     * @brief Writer thread body.
     */
    void run();

    /**
     * @brief Close a job's descriptor and report its outcome.
     *
     * @param job Finished job.
     */
    static void finish(job_t &job);

    int wake_ = -1;  ///< Wakes the thread for new jobs or stop.
    std::mutex mutex_;  ///< Guards `incoming_` and `stop_`.
    std::vector<job_t> incoming_;  ///< Jobs not yet picked up.
    bool stop_ = false;  ///< The thread should exit.
    std::thread thread_;  ///< Writer thread.
  };
}  // namespace wl
