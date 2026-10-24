/**
 * @file src/platform/linux/clipboard_portal.h
 * @brief Declarations for the clipboard backend on the XDG desktop portal.
 */
#pragma once

// standard includes
#include <filesystem>
#include <memory>

// local includes
#include "src/platform/common.h"

namespace portal {
  /**
   * @brief Create a clipboard backend that shares the clipboard through the XDG desktop portal.
   * @details This serves compositors without `ext-data-control-v1`, such as GNOME's, which give
   *          a background service the clipboard only through a remote desktop session. The backend
   *          opens that session on the bus in `DBUS_SESSION_BUS_ADDRESS` when it starts. It asks
   *          for the keyboard too, since GNOME remembers a permission only for a session with an
   *          input device, but never uses it. The desktop asks the user to allow the session; when
   *          they let the desktop remember the choice, the restore token kept in `token_file` lets
   *          later starts skip the question. Until the user answers, the clipboard is neither read
   *          nor written.
   *
   * @param token_file File that keeps the portal's restore token.
   * @return Backend instance.
   */
  std::unique_ptr<platf::clipboard_t> make_clipboard(std::filesystem::path token_file);
}  // namespace portal
