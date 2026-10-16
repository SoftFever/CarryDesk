/**
 * @file src/platform/linux/clipboard_wayland.h
 * @brief Declarations for the Wayland clipboard backend on `ext-data-control-v1`.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

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
   * @brief Read a pipe until EOF without blocking indefinitely.
   *
   * @param fd Read end of the pipe; the caller keeps ownership.
   * @param max_size Largest acceptable size.
   * @param stall_timeout Longest time without any byte arriving.
   * @param bytes Receives the bytes read.
   * @return `ok` at EOF, `too_large` beyond `max_size`, `io_error` on a read error or stall.
   */
  platf::clipboard_result_e drain_pipe(int fd, std::uint64_t max_size, std::chrono::milliseconds stall_timeout, std::string &bytes);
}  // namespace wl
