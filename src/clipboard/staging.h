/**
 * @file src/clipboard/staging.h
 * @brief Declarations for staging files received from clients before they are offered on the host.
 * @details Each paste is written into a private root: a randomly named directory with owner-only
 *          permissions under the staging area. Every file is created relative to an opened directory
 *          without following links. A root stays until one hour after it is released, which is
 *          recorded in a marker file beside it, so a paste still reading it is never cut off. On
 *          Windows, where Sunshine usually runs as SYSTEM, staging acts as the user logged on to
 *          Sunshine's session, so the files are that user's and native pastes can read them.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "protocol.h"

namespace clipboard::staging {
  inline constexpr std::chrono::hours retention {1};  ///< Time a released root is kept.
  inline constexpr std::string_view marker_suffix = ".released";  ///< Suffix of a root's release marker.

#ifdef _WIN32
  using directory_t = void *;  ///< Opened directory: a `HANDLE`.
  inline constexpr directory_t no_directory = nullptr;  ///< No directory.
#else
  using directory_t = int;  ///< Opened directory: a descriptor.
  inline constexpr directory_t no_directory = -1;  ///< No directory.
#endif

  /**
   * @brief Get the default staging area for this user.
   * @details On Windows the area belongs to the user logged on to Sunshine's session, who may log on
   *          only after Sunshine started, so it is found when the area is opened.
   *
   * @return `$XDG_CACHE_HOME/sunshine/clipboard`, or `~/.cache/sunshine/clipboard`; on Windows an
   *         empty path, which `area_t` opens as `%LOCALAPPDATA%\Sunshine\clipboard` of that user.
   */
  std::filesystem::path default_area();

  /**
   * @brief One paste staged into a private directory.
   */
  class root_t {
  public:
    /**
     * @brief Take over an opened root directory.
     *
     * @param path Absolute path of the root.
     * @param directory Opened root directory; owned from now on.
     */
    root_t(std::filesystem::path path, directory_t directory);

    root_t(const root_t &) = delete;
    root_t &operator=(const root_t &) = delete;
    ~root_t();

    /**
     * @brief Get the root's absolute path.
     *
     * @return Root path.
     */
    [[nodiscard]] const std::filesystem::path &path() const {
      return path_;
    }

    /**
     * @brief Validate a received manifest, rename collisions and create its directories.
     *
     * @param entries Manifest entries in order.
     * @return No value on success, `bad_request` for a refused path or order, `io_error` when a
     *         directory cannot be created.
     */
    std::optional<error_e> prepare(const std::vector<file_entry_t> &entries);

    /**
     * @brief Create one prepared file entry for writing.
     *
     * @param index Entry index.
     * @return Write-only descriptor owned by the caller, or -1 on failure.
     */
    int create_file(std::size_t index);

    /**
     * @brief Get the final relative path of a prepared entry.
     *
     * @param index Entry index.
     * @return Path below the root after collision renaming.
     */
    [[nodiscard]] const std::string &final_path(std::size_t index) const {
      return final_paths_.at(index);
    }

    /**
     * @brief Get the absolute paths of the top-level entries, in manifest order.
     *
     * @return Paths to offer on the host clipboard.
     */
    [[nodiscard]] std::vector<std::string> top_level_paths() const;

    /**
     * @brief Delete the root and everything in it without following links.
     */
    void remove();

  private:
    std::filesystem::path path_;  ///< Absolute path.
    directory_t directory_ = no_directory;  ///< Opened root directory.
    std::vector<std::string> final_paths_;  ///< Final relative path per entry.
    std::vector<bool> top_level_;  ///< Whether each entry is top-level.
    std::vector<bool> dirs_;  ///< Whether each entry is a directory.
  };

  /**
   * @brief The directory that holds every staged root.
   */
  class area_t {
  public:
    /**
     * @brief Use a staging area directory.
     *
     * @param path Directory; created on first use. On Windows an empty path stands for the default
     *             area, found when the area is opened.
     */
    explicit area_t(std::filesystem::path path);

    area_t(const area_t &) = delete;
    area_t &operator=(const area_t &) = delete;
    ~area_t();

    /**
     * @brief Create the area if needed and check that it is a real directory owned by this user.
     * @details The area is then made private: mode 0700, or on Windows full control for this user
     *          and SYSTEM only.
     *
     * @return `false` when the area cannot be used.
     */
    bool open();

    /**
     * @brief Adopt roots of a previous run and clean up.
     * @details Roots without a marker are marked released now; markers without a root are deleted;
     *          roots released more than the retention ago are deleted.
     *
     * @param now Current time.
     */
    void sweep(std::chrono::system_clock::time_point now);

    /**
     * @brief Delete roots released more than the retention ago, and stray markers.
     *
     * @param now Current time.
     */
    void reap(std::chrono::system_clock::time_point now);

    /**
     * @brief Get the free space of the area's file system.
     *
     * @return Bytes available to this user, or no value when unknown.
     */
    [[nodiscard]] std::optional<std::uint64_t> free_bytes() const;

    /**
     * @brief Create a new private root.
     *
     * @return The root, or null on failure.
     */
    std::shared_ptr<root_t> create_root();

    /**
     * @brief Mark a root released; it is deleted once the retention has passed.
     *
     * @param root Root to release.
     * @param now Release time.
     */
    void release(const root_t &root, std::chrono::system_clock::time_point now);

    /**
     * @brief Get the area's path.
     *
     * @return Area directory.
     */
    [[nodiscard]] const std::filesystem::path &path() const {
      return path_;
    }

  private:
    /**
     * @brief Visit the area's roots and markers.
     *
     * @param adopt Whether roots without a marker are marked released.
     * @param now Current time.
     */
    void clean(bool adopt, std::chrono::system_clock::time_point now);

    std::filesystem::path path_;  ///< Area directory.
    directory_t directory_ = no_directory;  ///< Opened area directory.
  };
}  // namespace clipboard::staging
