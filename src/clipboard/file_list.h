/**
 * @file src/clipboard/file_list.h
 * @brief Declarations for file manifests: received-path rules, collision renaming, building a
 *        manifest from local paths, and opening its entries safely.
 * @details Local file access follows the containment rules in docs/clipboard_protocol.md: walks start
 *          from opened directories, links are never followed, special files are skipped, and an
 *          entry is reopened only through its parent directories and only when its identity still
 *          matches the one recorded in the manifest.
 */
#pragma once

// standard includes
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "channel.h"
#include "protocol.h"

namespace clipboard::files {
  /**
   * @brief Check a path received in a peer's manifest.
   *
   * @param path `/`-separated relative UTF-8 path.
   * @return A reason the path is refused, or no value when it is acceptable.
   */
  std::optional<std::string> check_received_path(std::string_view path);

  /**
   * @brief Key under which two names collide: case-folded and NFC-normalised.
   *
   * @param name UTF-8 name.
   * @return Collision key.
   */
  std::string collision_key(std::string_view name);

  /**
   * @brief Hands out names that do not collide within their directory.
   */
  class unique_names_t {
  public:
    /**
     * @brief Claim a name in a directory, renaming it with a ` (n)` suffix when it collides.
     * @details The suffix goes before a file's extension and at the end of a directory's name.
     *
     * @param parent Final path of the directory.
     * @param name Wanted name.
     * @param dir Whether the entry is a directory.
     * @return The name to use.
     */
    std::string claim(const std::string &parent, const std::string &name, bool dir);

  private:
    std::map<std::string, std::map<std::string, bool>> taken_;  ///< Collision keys taken per directory.
  };

  /**
   * @brief Result of building a manifest from local paths.
   */
  struct build_result_t {
    std::optional<error_e> error;  ///< Why the manifest could not be built.
    std::shared_ptr<manifest_t> manifest;  ///< The manifest when built.
    std::size_t skipped = 0;  ///< Links, special files and names that are not UTF-8.
  };

  /**
   * @brief Build the manifest of local files and folders.
   * @details Each path becomes a top-level entry named after its last component. Links and special
   *          files are skipped, as are names that are not valid UTF-8. The manifest's context keeps
   *          the parent directories of the top-level entries open for `open_entry`.
   *
   * @param paths Absolute local paths.
   * @param max_total Largest summed file size accepted.
   * @return The manifest, or `too_large` beyond the limits, or `io_error` when nothing can be read.
   */
  build_result_t build_manifest(const std::vector<std::string> &paths, std::uint64_t max_total);

  /**
   * @brief Open a file entry of a manifest built by `build_manifest`.
   *
   * @param manifest Manifest of the lease.
   * @param index Entry index.
   * @return Read-only descriptor owned by the caller, or -1 when the entry was replaced or is unreadable.
   */
  int open_entry(const manifest_t &manifest, std::uint64_t index);

  /**
   * @brief Parse a peer's NDJSON manifest.
   *
   * @param ndjson Manifest lines, each ending in a newline.
   * @return Entries, or no value when a line is malformed or the indexes are not in order from 0.
   */
  std::optional<std::vector<file_entry_t>> parse_manifest(std::string_view ndjson);
}  // namespace clipboard::files
