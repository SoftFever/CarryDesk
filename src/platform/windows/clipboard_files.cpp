/**
 * @file src/platform/windows/clipboard_files.cpp
 * @brief Definitions for the Windows file access behind clipboard file transfers.
 */
// standard includes
#include <cstdlib>
#include <cstring>
#include <mutex>

// platform includes
#include <aclapi.h>
#include <fcntl.h>
#include <io.h>
#include <knownfolders.h>
#include <sddl.h>
#include <shlobj.h>
#include <winternl.h>
#include <wtsapi32.h>

// local includes
#include "clipboard_files.h"
#include "src/logging.h"

using namespace std::literals;

namespace clipboard::win {
  namespace {
    constexpr std::int64_t epoch_ticks = 116444736000000000LL;  ///< File time of the unix epoch.
    constexpr std::int64_t ticks_per_second = 10000000LL;  ///< File time ticks per second.

    std::mutex token_mutex;  ///< Guards `session_token`.
    HANDLE session_token = nullptr;  ///< Token of the user logged on to Sunshine's session, once known.

    /**
     * @brief Get the token of the user whose files the clipboard works with.
     * @details Sunshine's session keeps its user for Sunshine's lifetime: SunshineService restarts
     *          Sunshine when the console moves to another session. The token is therefore kept once
     *          a user has logged on.
     *
     * @return Token of the session's user when running as SYSTEM, the process token otherwise, or
     *         null when running as SYSTEM and nobody is logged on.
     */
    HANDLE file_user_token() {
      std::lock_guard lock {token_mutex};
      if (session_token) {
        return session_token;
      }
      if (!running_as_system()) {
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, &token)) {
          session_token = token;
        }
        return session_token;
      }
      DWORD session = 0;
      HANDLE token = nullptr;
      if (ProcessIdToSessionId(GetCurrentProcessId(), &session) && WTSQueryUserToken(session, &token)) {
        session_token = token;
      }
      return session_token;
    }

    /**
     * @brief Read a SID from a token.
     *
     * @param token Token to query.
     * @param kind `TokenUser` or `TokenOwner`.
     * @return SID bytes, or no value.
     */
    std::optional<std::vector<std::uint8_t>> token_sid(HANDLE token, const TOKEN_INFORMATION_CLASS kind) {
      DWORD size = 0;
      GetTokenInformation(token, kind, nullptr, 0, &size);
      if (size == 0) {
        return std::nullopt;
      }
      std::vector<std::uint8_t> buffer(size);
      if (!GetTokenInformation(token, kind, buffer.data(), size, &size)) {
        return std::nullopt;
      }
      PSID sid = kind == TokenUser ? reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid : reinterpret_cast<TOKEN_OWNER *>(buffer.data())->Owner;
      const auto length = GetLengthSid(sid);
      std::vector<std::uint8_t> bytes(length);
      if (!CopySid(length, bytes.data(), sid)) {
        return std::nullopt;
      }
      return bytes;
    }

    /**
     * @brief Open or create an entry relative to an opened directory.
     *
     * @param directory Root directory of the name.
     * @param name Entry name, without separators.
     * @param access Desired access.
     * @param disposition `FILE_OPEN`, `FILE_CREATE`, ...
     * @param options `FILE_*` create options.
     * @param share Share mode.
     * @param security Security descriptor for a created entry, or null.
     * @return Handle, or null with the Win32 error set.
     */
    handle_t nt_create(HANDLE directory, const std::wstring_view name, const ACCESS_MASK access, const ULONG disposition, const ULONG options, const ULONG share, PSECURITY_DESCRIPTOR security) {
      if (name.empty() || name == L"." || name == L".." || name.find_first_of(L"\\/") != std::wstring_view::npos || name.size() > 32767) {
        SetLastError(ERROR_INVALID_NAME);
        return nullptr;
      }
      UNICODE_STRING object_name {};
      object_name.Buffer = const_cast<PWSTR>(name.data());
      object_name.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
      object_name.MaximumLength = object_name.Length;
      OBJECT_ATTRIBUTES attributes {};
      attributes.Length = sizeof(attributes);
      attributes.RootDirectory = directory;
      attributes.ObjectName = &object_name;
      attributes.Attributes = OBJ_CASE_INSENSITIVE;
      attributes.SecurityDescriptor = security;
      IO_STATUS_BLOCK status_block {};
      HANDLE handle = nullptr;
      const auto status = NtCreateFile(&handle, access | SYNCHRONIZE, &attributes, &status_block, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition, options | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
      if (!NT_SUCCESS(status)) {
        SetLastError(RtlNtStatusToDosError(status));
        return nullptr;
      }
      return handle_t {handle};
    }

    /**
     * @brief Access and options for opening an entry for a purpose.
     */
    struct purpose_t {
      ACCESS_MASK access;  ///< Desired access.
      ULONG options;  ///< Create options.
      ULONG share;  ///< Share mode.
    };

    /**
     * @brief Get the access and options a purpose needs.
     *
     * @param purpose What the handle is for.
     * @return Access, options and share mode.
     */
    purpose_t purpose_of(const open_e purpose) {
      constexpr ULONG share_all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
      switch (purpose) {
        case open_e::inspect:
          return {FILE_READ_ATTRIBUTES, 0, share_all};
        case open_e::list:
          return {FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES, FILE_DIRECTORY_FILE, share_all};
        case open_e::read:
          return {FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY, share_all};
        case open_e::write:
          return {FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY, FILE_SHARE_READ | FILE_SHARE_DELETE};
        case open_e::remove:
        default:
          return {DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, 0, share_all};
      }
    }
  }  // namespace

  bool running_as_system() {
    static const bool system = []() {
      HANDLE token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
      }
      const handle_t owned {token};
      std::array<std::uint8_t, 256> buffer {};
      DWORD size = 0;
      if (!GetTokenInformation(token, TokenUser, buffer.data(), buffer.size(), &size)) {
        return false;
      }
      return IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, WinLocalSystemSid) != FALSE;
    }();
    return system;
  }

  HANDLE session_user_token() {
    return running_as_system() ? file_user_token() : nullptr;
  }

  void handle_closer_t::operator()(HANDLE handle) const {
    if (handle && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }

  std::optional<std::wstring> widen(const std::string_view utf8) {
    if (utf8.empty()) {
      return std::wstring {};
    }
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
      return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
  }

  std::optional<std::string> narrow(const std::wstring_view wide) {
    if (wide.empty()) {
      return std::string {};
    }
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
      return std::nullopt;
    }
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
  }

  std::filesystem::path to_path(const std::string_view utf8) {
    auto wide = widen(utf8);
    return wide ? std::filesystem::path {std::move(*wide)} : std::filesystem::path {};
  }

  std::string from_path(const std::filesystem::path &path) {
    return narrow(path.native()).value_or(std::string {});
  }

  std::wstring extended_path(const std::wstring &path) {
    if (path.starts_with(L"\\\\?\\")) {
      return path;
    }
    const auto size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (size == 0) {
      return path;
    }
    std::wstring full(size, L'\0');
    const auto written = GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
    if (written == 0 || written >= size) {
      return path;
    }
    full.resize(written);
    if (full.starts_with(L"\\\\")) {
      return L"\\\\?\\UNC\\" + full.substr(2);
    }
    if (full.size() >= 3 && full[1] == L':' && full[2] == L'\\') {
      return L"\\\\?\\" + full;
    }
    return path;
  }

  user_scope_t::user_scope_t() {
    if (!running_as_system()) {
      ready_ = true;
      return;
    }
    const auto token = file_user_token();
    if (!token) {
      return;
    }
    if (!ImpersonateLoggedOnUser(token)) {
      BOOST_LOG(warning) << "[clipboard] Cannot act as the logged-on user for file access: "sv << GetLastError();
      return;
    }
    impersonating_ = true;
    ready_ = true;
  }

  user_scope_t::~user_scope_t() {
    if (impersonating_ && !RevertToSelf()) {
      // The thread would keep running as the user; nothing it does afterwards could be trusted.
      BOOST_LOG(fatal) << "[clipboard] Cannot stop acting as the logged-on user: "sv << GetLastError();
      std::abort();
    }
  }

  std::optional<std::filesystem::path> user_local_app_data() {
    HANDLE token = nullptr;
    if (running_as_system()) {
      token = file_user_token();
      if (!token) {
        return std::nullopt;
      }
    }
    PWSTR folder = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, token, &folder);
    std::optional<std::filesystem::path> path;
    if (SUCCEEDED(result) && folder) {
      path = std::filesystem::path {folder};
    }
    CoTaskMemFree(folder);
    return path;
  }

  std::optional<file_info_t> query(HANDLE file) {
    FILE_BASIC_INFO basic {};
    FILE_STANDARD_INFO standard {};
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) || !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard))) {
      return std::nullopt;
    }
    file_info_t info;
    FILE_ID_INFO id {};
    if (GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id))) {
      info.identity.volume = id.VolumeSerialNumber;
      std::memcpy(info.identity.id.data(), &id.FileId, info.identity.id.size());
    } else {
      // File systems without 128-bit ids, such as FAT, still have the 64-bit index.
      BY_HANDLE_FILE_INFORMATION legacy {};
      if (!GetFileInformationByHandle(file, &legacy)) {
        return std::nullopt;
      }
      info.identity.volume = legacy.dwVolumeSerialNumber;
      const std::uint64_t index = (static_cast<std::uint64_t>(legacy.nFileIndexHigh) << 32) | legacy.nFileIndexLow;
      std::memcpy(info.identity.id.data(), &index, sizeof(index));
    }
    info.directory = standard.Directory != FALSE;
    info.device = (basic.FileAttributes & FILE_ATTRIBUTE_DEVICE) != 0 || GetFileType(file) != FILE_TYPE_DISK;
    info.size = info.directory ? 0 : static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    info.mtime = unix_seconds(basic.LastWriteTime.QuadPart);
    return info;
  }

  std::optional<FILE_ATTRIBUTE_TAG_INFO> attribute_tag(HANDLE entry) {
    FILE_ATTRIBUTE_TAG_INFO info {};
    if (!GetFileInformationByHandleEx(entry, FileAttributeTagInfo, &info, sizeof(info))) {
      return std::nullopt;
    }
    return info;
  }

  bool is_link(const DWORD attributes, const ULONG tag) {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag);
  }

  handle_t open_entry(HANDLE directory, const std::wstring_view name, const open_e purpose, const bool plain_only) {
    const auto [access, options, share] = purpose_of(purpose);
    auto entry = nt_create(directory, name, access, FILE_OPEN, options | FILE_OPEN_REPARSE_POINT, share, nullptr);
    if (!entry || purpose == open_e::remove) {
      // Removal deletes a link itself, so it keeps the handle of the reparse point.
      return entry;
    }
    const auto tag = attribute_tag(entry.get());
    if (!tag) {
      return nullptr;
    }
    if (!(tag->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      return entry;
    }
    if (plain_only || is_link(tag->FileAttributes, tag->ReparseTag)) {
      SetLastError(ERROR_CANT_ACCESS_FILE);
      return nullptr;
    }
    // Placeholders of cloud files and similar reparse points only work when their driver sees an
    // ordinary open. The reopened entry must be the one that was checked.
    auto reopened = nt_create(directory, name, access, FILE_OPEN, options, share, nullptr);
    const auto first = query(entry.get());
    const auto second = reopened ? query(reopened.get()) : std::nullopt;
    if (!first || !second || first->identity != second->identity) {
      return nullptr;
    }
    return reopened;
  }

  handle_t create_entry(HANDLE directory, const std::wstring_view name, const bool dir, PSECURITY_DESCRIPTOR security) {
    const auto [access, options, share] = purpose_of(dir ? open_e::list : open_e::write);
    return nt_create(directory, name, access, FILE_CREATE, options | FILE_OPEN_REPARSE_POINT, share, security);
  }

  handle_t open_directory(const std::filesystem::path &path, const bool follow, const bool change_permissions) {
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
    const DWORD access = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE | (change_permissions ? WRITE_DAC : 0);
    const auto handle = CreateFileW(extended_path(path.native()).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return nullptr;
    }
    handle_t owned {handle};
    const auto tag = attribute_tag(handle);
    if (!tag || !(tag->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (!follow && (tag->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
      return nullptr;
    }
    return owned;
  }

  std::optional<std::vector<dir_entry_t>> list(HANDLE directory) {
    std::vector<std::uint64_t> buffer(8192);
    std::vector<dir_entry_t> entries;
    auto kind = FileFullDirectoryRestartInfo;
    while (true) {
      if (!GetFileInformationByHandleEx(directory, kind, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(buffer[0])))) {
        if (GetLastError() == ERROR_NO_MORE_FILES) {
          return entries;
        }
        return std::nullopt;
      }
      kind = FileFullDirectoryInfo;
      auto *bytes = reinterpret_cast<const std::uint8_t *>(buffer.data());
      while (true) {
        const auto *info = reinterpret_cast<const FILE_FULL_DIR_INFO *>(bytes);
        const std::wstring_view name {info->FileName, info->FileNameLength / sizeof(wchar_t)};
        if (name != L"." && name != L"..") {
          // For reparse points the EA size field holds the reparse tag (MS-FSCC 2.4.14).
          entries.push_back({std::wstring {name}, info->FileAttributes, (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? info->EaSize : 0});
        }
        if (info->NextEntryOffset == 0) {
          break;
        }
        bytes += info->NextEntryOffset;
      }
    }
  }

  bool mark_deleted(HANDLE entry) {
    FILE_DISPOSITION_INFO_EX disposition {FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    if (SetFileInformationByHandle(entry, FileDispositionInfoEx, &disposition, sizeof(disposition))) {
      return true;
    }
    // File systems without POSIX deletion delete the entry once the last handle closes.
    FILE_DISPOSITION_INFO legacy {TRUE};
    return SetFileInformationByHandle(entry, FileDispositionInfo, &legacy, sizeof(legacy)) != FALSE;
  }

  int to_fd(handle_t handle, const bool write) {
    if (!handle) {
      return -1;
    }
    const auto fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle.get()), (write ? _O_WRONLY : _O_RDONLY) | _O_BINARY);
    if (fd >= 0) {
      handle.release();
    }
    return fd;
  }

  private_security_t::private_security_t() {
    const auto token = file_user_token();
    const auto user = token ? token_sid(token, TokenUser) : std::nullopt;
    if (!user) {
      return;
    }
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<PSID>(const_cast<std::uint8_t *>(user->data())), &sid)) {
      return;
    }
    const auto sddl = L"D:P(A;OICI;FA;;;"s + sid + L")(A;OICI;FA;;;SY)";
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr)) {
      descriptor_ = nullptr;
    }
  }

  private_security_t::~private_security_t() {
    if (descriptor_) {
      LocalFree(descriptor_);
    }
  }

  bool private_security_t::apply(HANDLE entry) const {
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!descriptor_ || !GetSecurityDescriptorDacl(descriptor_, &present, &dacl, &defaulted) || !present) {
      return false;
    }
    return SetSecurityInfo(entry, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
  }

  bool owned_by_user(HANDLE entry) {
    const auto token = file_user_token();
    if (!token) {
      return false;
    }
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(entry, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS) {
      return false;
    }
    bool owned = false;
    for (const auto kind : {TokenUser, TokenOwner}) {
      if (const auto sid = token_sid(token, kind); sid && EqualSid(owner, reinterpret_cast<PSID>(const_cast<std::uint8_t *>(sid->data())))) {
        owned = true;
      }
    }
    LocalFree(descriptor);
    return owned;
  }

  std::int64_t unix_seconds(const std::int64_t ticks) {
    const auto since_epoch = ticks - epoch_ticks;
    return since_epoch >= 0 ? since_epoch / ticks_per_second : -((-since_epoch + ticks_per_second - 1) / ticks_per_second);
  }

  FILETIME file_time(const std::int64_t seconds) {
    ULARGE_INTEGER ticks {};
    ticks.QuadPart = static_cast<std::uint64_t>(seconds * ticks_per_second + epoch_ticks);
    return {ticks.LowPart, ticks.HighPart};
  }
}  // namespace clipboard::win
