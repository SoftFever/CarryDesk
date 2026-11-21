/**
 * @file src/platform/macos/clipboard.h
 * @brief Declarations for the macOS clipboard backend.
 */
#pragma once

// standard includes
#include <memory>
#include <string>

// local includes
#include "src/platform/common.h"

namespace clipboard::mac {
  /**
   * @brief Create the macOS clipboard backend.
   * @details Sunshine runs as an application in the user's login session, so it shares the general
   *          pasteboard with the user's applications. macOS reports no pasteboard changes, so the
   *          backend watches the pasteboard's change count.
   *
   * @param pasteboard Name of the pasteboard to use; empty for the general pasteboard, the one
   *                   applications copy to and paste from. Tests use a pasteboard of their own.
   * @return Backend instance.
   */
  std::unique_ptr<platf::clipboard_t> make_clipboard(const std::string &pasteboard = {});
}  // namespace clipboard::mac
