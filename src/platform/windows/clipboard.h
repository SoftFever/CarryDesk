/**
 * @file src/platform/windows/clipboard.h
 * @brief Declarations for the Windows clipboard backend.
 */
#pragma once

// standard includes
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// local includes
#include "src/platform/common.h"

namespace clipboard::win {
  inline constexpr std::string_view agent_argument = "--clipboard-agent";  ///< Argument that runs `sunshine.exe` as a clipboard agent.

  /**
   * @brief Pipes to a running clipboard agent.
   */
  struct agent_link_t {
    void *requests = nullptr;  ///< Write end of the request pipe; owned by the backend from now on.
    void *replies = nullptr;  ///< Read end of the reply pipe; owned by the backend from now on.
    std::function<void()> close;  ///< Ends the agent once the backend closed both pipes.
  };

  using agent_factory_t = std::function<std::optional<agent_link_t>()>;  ///< Starts an agent, or gives no value when none can run.

  /**
   * @brief Create the Windows clipboard backend.
   * @details The clipboard belongs to the window station, so the backend shares it with every
   *          application on the desktops of Sunshine's window station, whichever desktop has input.
   *          SunshineService runs Sunshine as SYSTEM in `WinSta0` of the console session, the window
   *          station of the logged-on user's applications. Windows shows such a process only the
   *          predefined text and bitmap formats of other users' content, so with an agent factory
   *          the backend inspects and reads the clipboard through an agent; it watches the clipboard
   *          and puts content on it itself, and falls back to its own view while no agent runs.
   *
   * @param desktop Desktop of the process window station that the backend's thread joins; empty for
   *                the process's own desktop. Tests use a private window station and desktop.
   * @param agent Starts an agent; empty to read the clipboard in Sunshine's own process.
   * @return Backend instance; it fails to start when its window cannot be created.
   */
  std::unique_ptr<platf::clipboard_t> make_clipboard(const std::wstring &desktop = {}, agent_factory_t agent = {});

  /**
   * @brief Start `sunshine.exe --clipboard-agent` as the user logged on to Sunshine's session.
   *
   * @return Pipes to the agent, or no value when nobody is logged on or the agent cannot start.
   */
  std::optional<agent_link_t> spawn_agent();

  /**
   * @brief Inspect and read the clipboard for a backend until the request pipe closes.
   *
   * @param requests Read end of the request pipe, a `HANDLE`.
   * @param replies Write end of the reply pipe, a `HANDLE`.
   * @param desktop Desktop of the process window station to join first; empty for the process's own.
   * @return Exit code.
   */
  int run_agent(void *requests, void *replies, const std::wstring &desktop = {});

  /**
   * @brief Convert a PNG to the bitmaps Windows applications paste.
   *
   * @param png PNG image.
   * @return `CF_DIBV5` bytes with straight alpha and `CF_DIB` bytes composited on white, or no value
   *         when the image cannot be decoded or is larger than 16384×16384 or 256 MiB decoded.
   */
  std::optional<std::pair<std::string, std::string>> bitmaps_from_png(std::string_view png);

  /**
   * @brief Convert a `CF_DIB` or `CF_DIBV5` bitmap to PNG.
   * @details A bitmap whose alpha channel is entirely zero is taken as opaque, as applications that
   *          ignore alpha leave it that way.
   *
   * @param dib Bitmap header, color table or masks, and pixels.
   * @return PNG image, or no value when the bitmap cannot be decoded or exceeds the image limits.
   */
  std::optional<std::string> png_from_bitmap(std::string_view dib);
}  // namespace clipboard::win
