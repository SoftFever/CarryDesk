/**
 * @file src/clipboard/staging.cpp
 * @brief Definitions for staging files received from clients.
 * @details The file system work is a small layer per platform, below the shared logic. Every
 *          function of that layer works relative to directories it opened and never follows links.
 */
// standard includes
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

#ifdef _WIN32
  // platform includes
  #include <Windows.h>
#else
  #include <dirent.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/statvfs.h>
  #include <unistd.h>
#endif

// local includes
#include "file_list.h"
#include "src/logging.h"
#include "staging.h"

#ifdef _WIN32
  #include "src/platform/windows/clipboard_files.h"
#endif

using namespace std::literals;

namespace clipboard::staging {
  namespace {
    constexpr std::size_t root_name_size = 32;  ///< Hex digits in a root name.

    /**
     * @brief Check whether a name is a root name: hex digits of the root-name size.
     *
     * @param name Directory entry name.
     * @return `true` for a root name.
     */
    bool is_root_name(const std::string_view name) {
      return name.size() == root_name_size && name.find_first_not_of("0123456789abcdef") == std::string_view::npos;
    }

    /**
     * @brief Seconds since the unix epoch.
     *
     * @param time Time point.
     * @return Unix seconds.
     */
    std::int64_t unix_seconds(const std::chrono::system_clock::time_point time) {
      return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    }

    /**
     * @brief Acts as the clipboard's file user for its lifetime on Windows; nothing elsewhere.
     */
    class file_user_t {
    public:
      /**
       * @brief Check whether file access may go ahead.
       *
       * @return `false` when there is no user to act as.
       */
      explicit operator bool() const {
#ifdef _WIN32
        return static_cast<bool>(scope_);
#else
        return true;
#endif
      }

#ifdef _WIN32
    private:
      win::user_scope_t scope_;  ///< Impersonation of the logged-on user.
#endif
    };

#ifdef _WIN32
    /**
     * @brief Close an opened directory.
     *
     * @param directory Directory, or `no_directory`.
     */
    void close_directory(const directory_t directory) {
      win::handle_closer_t {}(directory);
    }

    /**
     * @brief List a directory's entries.
     *
     * @param directory Opened directory; not consumed.
     * @return UTF-8 entry names; names that are not valid UTF-16 are left out.
     */
    std::vector<std::string> list(const directory_t directory) {
      std::vector<std::string> names;
      if (const auto entries = win::list(directory)) {
        for (const auto &entry : *entries) {
          if (auto name = win::narrow(entry.name)) {
            names.push_back(std::move(*name));
          }
        }
      }
      return names;
    }

    /**
     * @brief Check whether an entry is a directory and not a link.
     *
     * @param parent Directory containing the entry.
     * @param name Entry name.
     * @return `true` for a real directory.
     */
    bool is_directory(const directory_t parent, const std::string &name) {
      const auto wide = win::widen(name);
      const auto entry = wide ? win::open_entry(parent, *wide, win::open_e::inspect, true) : nullptr;
      const auto info = entry ? win::query(entry.get()) : std::nullopt;
      return info && info->directory;
    }

    /**
     * @brief Delete an entry and, for a directory, everything below it, never following links.
     *
     * @param parent Directory containing the entry.
     * @param name Entry name.
     */
    void remove_tree(const directory_t parent, const std::wstring &name) {
      const auto entry = win::open_entry(parent, name, win::open_e::remove, true);
      if (!entry) {
        return;
      }
      const auto tag = win::attribute_tag(entry.get());
      if (tag && (tag->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (const auto children = win::list(entry.get())) {
          for (const auto &child : *children) {
            remove_tree(entry.get(), child.name);
          }
        }
      }
      win::mark_deleted(entry.get());
    }

    /**
     * @brief Delete an entry and, for a directory, everything below it, never following links.
     *
     * @param parent Directory containing the entry.
     * @param name Entry name.
     */
    void remove_tree(const directory_t parent, const std::string &name) {
      if (const auto wide = win::widen(name)) {
        remove_tree(parent, *wide);
      }
    }

    /**
     * @brief Delete an entry that is not a directory; a link is deleted itself.
     *
     * @param parent Directory containing the entry.
     * @param name Entry name.
     */
    void remove_file(const directory_t parent, const std::string &name) {
      const auto wide = win::widen(name);
      const auto entry = wide ? win::open_entry(parent, *wide, win::open_e::remove, true) : nullptr;
      const auto tag = entry ? win::attribute_tag(entry.get()) : std::nullopt;
      if (tag && (!(tag->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
        win::mark_deleted(entry.get());
      }
    }

    /**
     * @brief Open a directory below a root by walking its components without following links.
     *
     * @param root Opened root directory; not consumed.
     * @param relative `/`-separated relative path; empty for the root itself.
     * @return Directory owned by the caller, or `no_directory`.
     */
    directory_t open_below(const directory_t root, const std::string &relative) {
      HANDLE copy = nullptr;
      if (!DuplicateHandle(GetCurrentProcess(), root, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return no_directory;
      }
      win::handle_t directory {copy};
      std::size_t start = 0;
      while (directory && start < relative.size()) {
        const auto end = relative.find('/', start);
        const auto component = win::widen(relative.substr(start, end == std::string::npos ? std::string::npos : end - start));
        directory = component ? win::open_entry(directory.get(), *component, win::open_e::list, true) : nullptr;
        start = end == std::string::npos ? relative.size() : end + 1;
      }
      return directory.release();
    }

    /**
     * @brief Create a directory in an opened directory; it inherits the root's permissions.
     *
     * @param parent Directory to create it in.
     * @param name Directory name.
     * @return `false` when it exists or cannot be created.
     */
    bool make_directory(const directory_t parent, const std::string &name) {
      const auto wide = win::widen(name);
      return wide && win::create_entry(parent, *wide, true);
    }

    /**
     * @brief Create a new file in an opened directory.
     *
     * @param parent Directory to create it in.
     * @param name File name.
     * @return Write-only descriptor, or -1 when it exists or cannot be created.
     */
    int create_file_in(const directory_t parent, const std::string &name) {
      const auto wide = win::widen(name);
      return wide ? win::to_fd(win::create_entry(parent, *wide, false), true) : -1;
    }

    /**
     * @brief Read a release marker.
     *
     * @param area Opened area directory.
     * @param name Marker name.
     * @return Release time in unix seconds, or no value when unreadable.
     */
    std::optional<std::int64_t> read_marker(const directory_t area, const std::string &name) {
      const auto wide = win::widen(name);
      const auto file = wide ? win::open_entry(area, *wide, win::open_e::read, true) : nullptr;
      char buffer[32];
      DWORD size = 0;
      if (!file || !ReadFile(file.get(), buffer, sizeof(buffer), &size, nullptr)) {
        return std::nullopt;
      }
      std::int64_t value = 0;
      if (size == 0 || std::from_chars(buffer, buffer + size, value).ec != std::errc {}) {
        return std::nullopt;
      }
      return value;
    }

    /**
     * @brief Write a release marker.
     *
     * @param area Opened area directory.
     * @param root Root name.
     * @param seconds Release time in unix seconds.
     */
    void write_marker(const directory_t area, const std::string &root, const std::int64_t seconds) {
      const auto name = root + std::string {marker_suffix};
      remove_file(area, name);
      const auto wide = win::widen(name);
      const auto file = wide ? win::create_entry(area, *wide, false) : nullptr;
      const auto text = std::to_string(seconds);
      DWORD written = 0;
      if (!file || !WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr)) {
        BOOST_LOG(warning) << "[clipboard] Cannot mark a staged paste released"sv;
      }
    }

    /**
     * @brief Create the area if needed, check it and make it private.
     *
     * @param path Area directory; an empty path is set to the default area.
     * @return Opened area, or `no_directory`.
     */
    directory_t open_area_directory(std::filesystem::path &path) {
      if (path.empty()) {
        const auto local = win::user_local_app_data();
        if (!local) {
          return no_directory;
        }
        path = *local / L"Sunshine" / L"clipboard";
      }
      std::error_code ec;
      std::filesystem::create_directories(path, ec);
      auto directory = win::open_directory(path, false, true);
      if (!directory || !win::owned_by_user(directory.get()) || !win::private_security_t {}.apply(directory.get())) {
        return no_directory;
      }
      return directory.release();
    }

    /**
     * @brief Create a private root directory in the area.
     *
     * @param area Opened area directory.
     * @param name Root name.
     * @return Opened root, or `no_directory`.
     */
    directory_t make_root_directory(const directory_t area, const std::string &name) {
      const win::private_security_t security;
      const auto wide = win::widen(name);
      if (!security.descriptor() || !wide) {
        return no_directory;
      }
      return win::create_entry(area, *wide, true, security.descriptor()).release();
    }

    /**
     * @brief Get the free space of the area's volume.
     *
     * @param path Area directory.
     * @return Bytes available to this user, or no value when unknown.
     */
    std::optional<std::uint64_t> free_space(directory_t, const std::filesystem::path &path) {
      ULARGE_INTEGER available {};
      if (!GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr)) {
        return std::nullopt;
      }
      return available.QuadPart;
    }

    /**
     * @brief Open the directory containing a path, refusing a link there.
     *
     * @param path Absolute path.
     * @return Opened parent, or `no_directory`.
     */
    directory_t open_parent(const std::filesystem::path &path) {
      return win::open_directory(path.parent_path(), false).release();
    }

    /**
     * @brief Turn a received name into one Windows can store.
     * @details Names from other systems may contain characters Windows file names cannot; each
     *          becomes `_`, and the collision renaming that follows keeps the results apart.
     *
     * @param name Received name.
     * @return Name to stage.
     */
    std::string local_name(std::string name) {
      std::ranges::replace_if(
        name,
        [](const char c) {
          return std::string_view {"<>\"|?*"}.find(c) != std::string_view::npos;
        },
        '_'
      );
      return name;
    }

    /**
     * @brief Absolute path of an entry below a root, in UTF-8.
     *
     * @param root Root path.
     * @param relative `/`-separated relative path.
     * @return Native path.
     */
    std::string absolute(const std::filesystem::path &root, const std::string &relative) {
      return win::from_path((root / win::to_path(relative)).make_preferred());
    }
#else
    /**
     * @brief Close an opened directory.
     *
     * @param directory Directory, or `no_directory`.
     */
    void close_directory(const directory_t directory) {
      if (directory >= 0) {
        ::close(directory);
      }
    }

    /**
     * @brief List a directory's entries.
     *
     * @param fd Opened directory; not consumed.
     * @return Entry names without `.` and `..`.
     */
    std::vector<std::string> list(const int fd) {
      std::vector<std::string> names;
      const auto copy = dup(fd);
      if (copy < 0) {
        return names;
      }
      auto *stream = fdopendir(copy);
      if (!stream) {
        ::close(copy);
        return names;
      }
      rewinddir(stream);
      while (const auto *entry = readdir(stream)) {
        if (std::string_view name {entry->d_name}; name != "." && name != "..") {
          names.emplace_back(name);
        }
      }
      closedir(stream);
      return names;
    }

    /**
     * @brief Check whether an entry is a directory and not a link.
     *
     * @param parent Directory containing the entry.
     * @param name Entry name.
     * @return `true` for a real directory.
     */
    bool is_directory(const int parent, const std::string &name) {
      struct stat st {};
      return fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
    }

    /**
     * @brief Delete an entry and, for a directory, everything below it, never following links.
     *
     * @param dirfd Directory containing the entry.
     * @param name Entry name.
     */
    void remove_tree(const int dirfd, const std::string &name) {
      struct stat st {};
      if (fstatat(dirfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return;
      }
      if (!S_ISDIR(st.st_mode)) {
        unlinkat(dirfd, name.c_str(), 0);
        return;
      }
      const auto fd = openat(dirfd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd >= 0) {
        for (const auto &child : list(fd)) {
          remove_tree(fd, child);
        }
        ::close(fd);
      }
      unlinkat(dirfd, name.c_str(), AT_REMOVEDIR);
    }

    /**
     * @brief Delete an entry that is not a directory; a link is deleted itself.
     *
     * @param parent Directory containing the entry.
     * @param name Entry name.
     */
    void remove_file(const int parent, const std::string &name) {
      unlinkat(parent, name.c_str(), 0);
    }

    /**
     * @brief Open a directory below a root by walking its components without following links.
     *
     * @param root Opened root directory; not consumed.
     * @param relative `/`-separated relative path; empty for the root itself.
     * @return Descriptor owned by the caller, or -1.
     */
    int open_below(const int root, const std::string &relative) {
      auto fd = dup(root);
      std::size_t start = 0;
      while (fd >= 0 && start < relative.size()) {
        const auto end = relative.find('/', start);
        const auto component = relative.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto next = openat(fd, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(fd);
        fd = next;
        start = end == std::string::npos ? relative.size() : end + 1;
      }
      return fd;
    }

    /**
     * @brief Create a directory in an opened directory with the user's normal permissions.
     *
     * @param parent Directory to create it in.
     * @param name Directory name.
     * @return `false` when it exists or cannot be created.
     */
    bool make_directory(const int parent, const std::string &name) {
      return mkdirat(parent, name.c_str(), 0755) == 0;
    }

    /**
     * @brief Create a new file in an opened directory with the user's normal permissions.
     *
     * @param parent Directory to create it in.
     * @param name File name.
     * @return Write-only descriptor, or -1 when it exists or cannot be created.
     */
    int create_file_in(const int parent, const std::string &name) {
      return openat(parent, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    }

    /**
     * @brief Check that an opened directory is a real directory owned by this user.
     *
     * @param fd Opened directory.
     * @return `true` when it may be used for staging.
     */
    bool owned_directory(const int fd) {
      struct stat st {};
      return fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid();
    }

    /**
     * @brief Read a release marker.
     *
     * @param area Opened area directory.
     * @param name Marker name.
     * @return Release time in unix seconds, or no value when unreadable.
     */
    std::optional<std::int64_t> read_marker(const int area, const std::string &name) {
      const auto fd = openat(area, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        return std::nullopt;
      }
      char buffer[32];
      const auto size = ::read(fd, buffer, sizeof(buffer));
      ::close(fd);
      std::int64_t value = 0;
      if (size <= 0 || std::from_chars(buffer, buffer + size, value).ec != std::errc {}) {
        return std::nullopt;
      }
      return value;
    }

    /**
     * @brief Write a release marker.
     *
     * @param area Opened area directory.
     * @param root Root name.
     * @param seconds Release time in unix seconds.
     */
    void write_marker(const int area, const std::string &root, const std::int64_t seconds) {
      const auto name = root + std::string {marker_suffix};
      const auto fd = openat(area, name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd < 0) {
        BOOST_LOG(warning) << "[clipboard] Cannot mark a staged paste released"sv;
        return;
      }
      const auto text = std::to_string(seconds);
      [[maybe_unused]] const auto written = ::write(fd, text.data(), text.size());
      ::close(fd);
    }

    /**
     * @brief Create the area if needed, check it and make it private.
     *
     * @param path Area directory.
     * @return Opened area, or `no_directory`.
     */
    int open_area_directory(const std::filesystem::path &path) {
      std::error_code ec;
      std::filesystem::create_directories(path, ec);
      const auto fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (!owned_directory(fd) || fchmod(fd, 0700) != 0) {
        close_directory(fd);
        return no_directory;
      }
      return fd;
    }

    /**
     * @brief Create a private root directory in the area.
     *
     * @param area Opened area directory.
     * @param name Root name.
     * @return Opened root, or `no_directory`.
     */
    int make_root_directory(const int area, const std::string &name) {
      if (mkdirat(area, name.c_str(), 0700) != 0) {
        return no_directory;
      }
      const auto fd = openat(area, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (!owned_directory(fd)) {
        close_directory(fd);
        return no_directory;
      }
      return fd;
    }

    /**
     * @brief Get the free space of the area's file system.
     *
     * @param area Opened area directory.
     * @return Bytes available to this user, or no value when unknown.
     */
    std::optional<std::uint64_t> free_space(const int area, const std::filesystem::path &) {
      struct statvfs st {};
      if (fstatvfs(area, &st) != 0) {
        return std::nullopt;
      }
      return static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize;
    }

    /**
     * @brief Open the directory containing a path, refusing a link there.
     *
     * @param path Absolute path.
     * @return Opened parent, or `no_directory`.
     */
    int open_parent(const std::filesystem::path &path) {
      return ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }

    /**
     * @brief Turn a received name into one this system can store.
     *
     * @param name Received name.
     * @return The name unchanged.
     */
    std::string local_name(std::string name) {
      return name;
    }

    /**
     * @brief Absolute path of an entry below a root.
     *
     * @param root Root path.
     * @param relative `/`-separated relative path.
     * @return Path.
     */
    std::string absolute(const std::filesystem::path &root, const std::string &relative) {
      return (root / relative).string();
    }
#endif
  }  // namespace

  std::filesystem::path default_area() {
#ifdef _WIN32
    return {};
#else
    if (const auto *cache = std::getenv("XDG_CACHE_HOME"); cache && *cache == '/') {
      return std::filesystem::path {cache} / "sunshine" / "clipboard";
    }
    const auto *home = std::getenv("HOME");
    return std::filesystem::path {home ? home : "/tmp"} / ".cache" / "sunshine" / "clipboard";
#endif
  }

  root_t::root_t(std::filesystem::path path, const directory_t directory):
      path_ {std::move(path)},
      directory_ {directory} {
  }

  root_t::~root_t() {
    close_directory(directory_);
  }

  std::optional<error_e> root_t::prepare(const std::vector<file_entry_t> &entries) {
    const file_user_t user;
    if (!user) {
      return error_e::io_error;
    }
    files::unique_names_t names;
    std::map<std::string, std::string> directories {{"", ""}};
    final_paths_.assign(entries.size(), {});
    top_level_.assign(entries.size(), false);
    dirs_.assign(entries.size(), false);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto &entry = entries[i];
      if (files::check_received_path(entry.path)) {
        return error_e::bad_request;
      }
      const auto slash = entry.path.rfind('/');
      const auto parent = slash == std::string::npos ? std::string {} : entry.path.substr(0, slash);
      const auto name = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
      const auto directory = directories.find(parent);
      if (directory == directories.end()) {
        return error_e::bad_request;
      }
      const auto &final_parent = directory->second;
      const auto final_name = names.claim(final_parent, local_name(name), entry.dir);
      final_paths_[i] = final_parent.empty() ? final_name : final_parent + "/" + final_name;
      top_level_[i] = parent.empty();
      dirs_[i] = entry.dir;
      if (entry.dir) {
        const auto opened = open_below(directory_, final_parent);
        const auto created = opened != no_directory && make_directory(opened, final_name);
        close_directory(opened);
        if (!created) {
          return error_e::io_error;
        }
        directories[entry.path] = final_paths_[i];
      }
    }
    return std::nullopt;
  }

  int root_t::create_file(const std::size_t index) {
    if (index >= final_paths_.size() || dirs_[index]) {
      return -1;
    }
    const file_user_t user;
    if (!user) {
      return -1;
    }
    const auto &path = final_paths_[index];
    const auto slash = path.rfind('/');
    const auto parent = open_below(directory_, slash == std::string::npos ? std::string {} : path.substr(0, slash));
    if (parent == no_directory) {
      return -1;
    }
    const auto fd = create_file_in(parent, slash == std::string::npos ? path : path.substr(slash + 1));
    close_directory(parent);
    return fd;
  }

  std::vector<std::string> root_t::top_level_paths() const {
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < final_paths_.size(); ++i) {
      if (top_level_[i]) {
        paths.push_back(absolute(path_, final_paths_[i]));
      }
    }
    return paths;
  }

  void root_t::remove() {
    if (directory_ == no_directory) {
      return;
    }
    close_directory(directory_);
    directory_ = no_directory;
    const file_user_t user;
    if (!user) {
      return;
    }
    const auto parent = open_parent(path_);
    if (parent != no_directory) {
      remove_tree(parent, path_.filename().string());
      close_directory(parent);
    }
  }

  area_t::area_t(std::filesystem::path path):
      path_ {std::move(path)} {
  }

  area_t::~area_t() {
    close_directory(directory_);
  }

  bool area_t::open() {
    if (directory_ != no_directory) {
      return true;
    }
    const file_user_t user;
    const auto directory = user ? open_area_directory(path_) : no_directory;
    if (directory == no_directory) {
      BOOST_LOG(warning) << "[clipboard] Cannot use the staging directory "sv << (path_.empty() ? "of the logged-on user"s : path_.string());
      return false;
    }
    directory_ = directory;
    return true;
  }

  void area_t::sweep(const std::chrono::system_clock::time_point now) {
    clean(true, now);
  }

  void area_t::reap(const std::chrono::system_clock::time_point now) {
    clean(false, now);
  }

  void area_t::clean(const bool adopt, const std::chrono::system_clock::time_point now) {
    if (directory_ == no_directory) {
      return;
    }
    const file_user_t user;
    if (!user) {
      return;
    }
    std::set<std::string> roots;
    std::set<std::string> markers;
    for (const auto &name : list(directory_)) {
      if (is_root_name(name) && is_directory(directory_, name)) {
        roots.insert(name);
      } else if (name.ends_with(marker_suffix) && is_root_name(std::string_view {name}.substr(0, name.size() - marker_suffix.size()))) {
        markers.insert(name);
      }
    }
    const auto current = unix_seconds(now);
    const auto keep = std::chrono::duration_cast<std::chrono::seconds>(retention).count();
    for (const auto &marker : markers) {
      const auto root = marker.substr(0, marker.size() - marker_suffix.size());
      if (!roots.contains(root)) {
        remove_file(directory_, marker);
        continue;
      }
      roots.erase(root);
      const auto released = read_marker(directory_, marker);
      if (!released) {
        write_marker(directory_, root, current);
      } else if (*released + keep <= current) {
        remove_tree(directory_, root);
        remove_file(directory_, marker);
      }
    }
    if (adopt) {
      for (const auto &root : roots) {
        write_marker(directory_, root, current);
      }
    }
  }

  std::optional<std::uint64_t> area_t::free_bytes() const {
    if (directory_ == no_directory) {
      return std::nullopt;
    }
    const file_user_t user;
    if (!user) {
      return std::nullopt;
    }
    return free_space(directory_, path_);
  }

  std::shared_ptr<root_t> area_t::create_root() {
    if (directory_ == no_directory) {
      return nullptr;
    }
    const file_user_t user;
    if (!user) {
      return nullptr;
    }
    std::random_device device;
    std::uniform_int_distribution<int> digit {0, 15};
    std::string name;
    for (std::size_t i = 0; i < root_name_size; ++i) {
      name.push_back("0123456789abcdef"[digit(device)]);
    }
    const auto root = make_root_directory(directory_, name);
    if (root == no_directory) {
      return nullptr;
    }
    return std::make_shared<root_t>(path_ / name, root);
  }

  void area_t::release(const root_t &root, const std::chrono::system_clock::time_point now) {
    if (directory_ == no_directory) {
      return;
    }
    const file_user_t user;
    if (user) {
      write_marker(directory_, root.path().filename().string(), unix_seconds(now));
    }
  }
}  // namespace clipboard::staging
