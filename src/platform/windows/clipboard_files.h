/**
 * @file src/platform/windows/clipboard_files.h
 * @brief Declarations for the Windows file access behind clipboard file transfers.
 * @details The clipboard's containment rules (docs/clipboard_protocol.md) need entries opened
 *          relative to an opened directory, links never followed and file identities compared. On
 *          Windows that is `NtCreateFile` with a root directory handle and
 *          `FILE_OPEN_REPARSE_POINT`. Name-surrogate reparse points (symbolic links, junctions,
 *          mount points) count as links; other reparse points, such as cloud-file placeholders, are
 *          ordinary files that their file system driver must see opened normally.
 *
 *          Sunshine usually runs as SYSTEM in the console session. File access for the clipboard
 *          then impersonates the user logged on to that session, so host files are read with that
 *          user's rights and drive mappings, and staged files belong to that user in their profile.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// platform includes
#include <Windows.h>

namespace clipboard::win {
  /**
   * @brief Closes a handle.
   */
  struct handle_closer_t {
    /**
     * @brief Close a handle.
     *
     * @param handle Handle to close; null and `INVALID_HANDLE_VALUE` are ignored.
     */
    void operator()(HANDLE handle) const;
  };

  using handle_t = std::unique_ptr<void, handle_closer_t>;  ///< Owned handle.

  /**
   * @brief Convert UTF-8 to UTF-16.
   *
   * @param utf8 UTF-8 text.
   * @return UTF-16 text, or no value when the input is not valid UTF-8.
   */
  std::optional<std::wstring> widen(std::string_view utf8);

  /**
   * @brief Convert UTF-16 to UTF-8.
   *
   * @param wide UTF-16 text.
   * @return UTF-8 text, or no value when the input holds an unpaired surrogate.
   */
  std::optional<std::string> narrow(std::wstring_view wide);

  /**
   * @brief Make a path from UTF-8.
   *
   * @param utf8 UTF-8 path.
   * @return Path; empty when the input is not valid UTF-8.
   */
  std::filesystem::path to_path(std::string_view utf8);

  /**
   * @brief Get a path as UTF-8.
   *
   * @param path Path.
   * @return UTF-8 path with native separators.
   */
  std::string from_path(const std::filesystem::path &path);

  /**
   * @brief Prefix an absolute path so that Win32 calls accept it at any length.
   *
   * @param path Absolute path with drive letter or UNC prefix.
   * @return `\\?\` or `\\?\UNC\` path; other paths are returned unchanged.
   */
  std::wstring extended_path(const std::wstring &path);

  /**
   * @brief Check whether Sunshine runs as SYSTEM.
   *
   * @return `true` for the LocalSystem account.
   */
  bool running_as_system();

  /**
   * @brief Get the token of the user logged on to Sunshine's session.
   *
   * @return Token, owned by this module, when Sunshine runs as SYSTEM and a user is logged on;
   *         otherwise null.
   */
  HANDLE session_user_token();

  /**
   * @brief Act as the clipboard's file user for the lifetime of the scope.
   * @details When Sunshine runs as SYSTEM, the calling thread impersonates the user logged on to
   *          Sunshine's session until the scope ends. Otherwise Sunshine already is that user and
   *          nothing changes. Scopes must not be nested.
   */
  class user_scope_t {
  public:
    user_scope_t();
    ~user_scope_t();

    user_scope_t(const user_scope_t &) = delete;
    user_scope_t &operator=(const user_scope_t &) = delete;

    /**
     * @brief Check whether file access may go ahead.
     *
     * @return `false` when Sunshine runs as SYSTEM and nobody is logged on, or impersonation failed.
     */
    explicit operator bool() const {
      return ready_;
    }

  private:
    bool ready_ = false;  ///< The thread acts as the file user.
    bool impersonating_ = false;  ///< `RevertToSelf` is due when the scope ends.
  };

  /**
   * @brief Get the local application data folder of the clipboard's file user.
   *
   * @return `%LOCALAPPDATA%` of that user, or no value when nobody is logged on.
   */
  std::optional<std::filesystem::path> user_local_app_data();

  /**
   * @brief Identity of an opened file: its volume and file id.
   */
  struct identity_t {
    std::uint64_t volume = 0;  ///< Volume serial number.
    std::array<std::uint8_t, 16> id {};  ///< File id on the volume.

    /**
     * @brief Compare two identities.
     *
     * @param other Identity to compare with.
     * @return `true` for the same file.
     */
    bool operator==(const identity_t &other) const = default;
  };

  /**
   * @brief Facts about an opened file.
   */
  struct file_info_t {
    identity_t identity;  ///< Volume and file id.
    bool directory = false;  ///< The file is a directory.
    bool device = false;  ///< The file is a device rather than a file or directory.
    std::uint64_t size = 0;  ///< File size; 0 for directories.
    std::int64_t mtime = 0;  ///< Last write time in unix seconds.
  };

  /**
   * @brief Query an opened file.
   *
   * @param file Opened file or directory.
   * @return Facts, or no value when they cannot be read.
   */
  std::optional<file_info_t> query(HANDLE file);

  /**
   * @brief Read the attributes and reparse tag of an opened entry.
   *
   * @param entry Opened entry.
   * @return Attributes and tag, or no value.
   */
  std::optional<FILE_ATTRIBUTE_TAG_INFO> attribute_tag(HANDLE entry);

  /**
   * @brief Check whether a reparse point is a link to another location.
   *
   * @param attributes File attributes.
   * @param tag Reparse tag, meaningful only with `FILE_ATTRIBUTE_REPARSE_POINT`.
   * @return `true` for symbolic links, junctions, mount points and other name surrogates.
   */
  bool is_link(DWORD attributes, ULONG tag);

  /**
   * @brief What an opened entry is for.
   */
  enum class open_e {
    inspect,  ///< Any entry, to query it.
    list,  ///< A directory to list and to open entries in.
    read,  ///< A file to read.
    write,  ///< A file to write and to set the modification time of.
    remove,  ///< Any entry to delete.
  };

  /**
   * @brief Open an existing entry of an opened directory without following links.
   * @details A name-surrogate reparse point is refused. When `plain_only` is false, other reparse
   *          points are reopened normally after checking that the reopened file is the same one.
   *
   * @param directory Opened directory.
   * @param name Entry name, without separators.
   * @param purpose What the handle is for; `list` also requires a directory, `read` and `write` a
   *                non-directory. `remove` keeps the handle of any reparse point, so a link is
   *                deleted itself.
   * @param plain_only Refuse every reparse point, for trees Sunshine created itself.
   * @return Handle, or null.
   */
  handle_t open_entry(HANDLE directory, std::wstring_view name, open_e purpose, bool plain_only);

  /**
   * @brief Create a new entry in an opened directory; an existing entry of that name fails.
   *
   * @param directory Opened directory.
   * @param name Entry name, without separators.
   * @param dir Create a directory, opened for listing, instead of a file opened for writing.
   * @param security Security descriptor for the entry, or null to inherit.
   * @return Handle, or null.
   */
  handle_t create_entry(HANDLE directory, std::wstring_view name, bool dir, PSECURITY_DESCRIPTOR security = nullptr);

  /**
   * @brief Open a directory by absolute path.
   *
   * @param path Absolute path.
   * @param follow Follow a link at the path itself; otherwise any reparse point is refused.
   * @param change_permissions Also open it for `private_security_t::apply` and `owned_by_user`.
   * @return Handle opened for listing, or null.
   */
  handle_t open_directory(const std::filesystem::path &path, bool follow, bool change_permissions = false);

  /**
   * @brief One entry of a directory listing.
   */
  struct dir_entry_t {
    std::wstring name;  ///< Entry name.
    DWORD attributes = 0;  ///< File attributes.
    ULONG reparse_tag = 0;  ///< Reparse tag when `attributes` has `FILE_ATTRIBUTE_REPARSE_POINT`.
  };

  /**
   * @brief List an opened directory from its start.
   *
   * @param directory Directory opened for listing.
   * @return Entries without `.` and `..`, or no value when listing failed.
   */
  std::optional<std::vector<dir_entry_t>> list(HANDLE directory);

  /**
   * @brief Delete an opened entry once its handle closes.
   *
   * @param entry Entry opened for removal.
   * @return `false` when the entry cannot be deleted, such as a directory that is not empty.
   */
  bool mark_deleted(HANDLE entry);

  /**
   * @brief Wrap a handle in a C runtime descriptor.
   *
   * @param handle Handle; owned by the descriptor from now on, or closed on failure.
   * @param write Open the descriptor for writing instead of reading.
   * @return Descriptor, or -1.
   */
  int to_fd(handle_t handle, bool write);

  /**
   * @brief Security that grants full control to the file user and SYSTEM only.
   * @details Entries created inside inherit it, the Windows counterpart of an owner-only directory.
   */
  class private_security_t {
  public:
    private_security_t();
    ~private_security_t();

    private_security_t(const private_security_t &) = delete;
    private_security_t &operator=(const private_security_t &) = delete;

    /**
     * @brief Get the security descriptor.
     *
     * @return Descriptor, or null when the file user is unknown.
     */
    [[nodiscard]] PSECURITY_DESCRIPTOR descriptor() const {
      return descriptor_;
    }

    /**
     * @brief Replace an opened entry's permissions with these.
     *
     * @param entry Entry opened with `WRITE_DAC`.
     * @return `false` on failure.
     */
    bool apply(HANDLE entry) const;

  private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;  ///< Self-relative descriptor from `LocalAlloc`.
  };

  /**
   * @brief Check that an opened entry is owned by the file user.
   *
   * @param entry Entry opened with `READ_CONTROL`.
   * @return `true` when its owner is the file user or that user's default owner.
   */
  bool owned_by_user(HANDLE entry);

  /**
   * @brief Convert a Windows file time to unix seconds.
   *
   * @param ticks 100-nanosecond intervals since 1601.
   * @return Unix seconds.
   */
  std::int64_t unix_seconds(std::int64_t ticks);

  /**
   * @brief Convert unix seconds to a Windows file time.
   *
   * @param seconds Unix seconds.
   * @return File time.
   */
  FILETIME file_time(std::int64_t seconds);
}  // namespace clipboard::win
