/**
 * @file src/clipboard/staging.cpp
 * @brief Definitions for staging files received from clients.
 */
// standard includes
#include <charconv>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

#ifndef _WIN32
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

#ifndef _WIN32
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
#endif
  }  // namespace

  std::filesystem::path default_area() {
    if (const auto *cache = std::getenv("XDG_CACHE_HOME"); cache && *cache == '/') {
      return std::filesystem::path {cache} / "sunshine" / "clipboard";
    }
    const auto *home = std::getenv("HOME");
    return std::filesystem::path {home ? home : "/tmp"} / ".cache" / "sunshine" / "clipboard";
  }

  root_t::root_t(std::filesystem::path path, const int fd):
      path_ {std::move(path)},
      fd_ {fd} {
  }

  root_t::~root_t() {
#ifndef _WIN32
    if (fd_ >= 0) {
      ::close(fd_);
    }
#endif
  }

  std::optional<error_e> root_t::prepare(const std::vector<file_entry_t> &entries) {
#ifndef _WIN32
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
      const auto final_name = names.claim(final_parent, name, entry.dir);
      final_paths_[i] = final_parent.empty() ? final_name : final_parent + "/" + final_name;
      top_level_[i] = parent.empty();
      dirs_[i] = entry.dir;
      if (entry.dir) {
        const auto fd = open_below(fd_, final_parent);
        const auto created = fd >= 0 && mkdirat(fd, final_name.c_str(), 0755) == 0;
        if (fd >= 0) {
          ::close(fd);
        }
        if (!created) {
          return error_e::io_error;
        }
        directories[entry.path] = final_paths_[i];
      }
    }
    return std::nullopt;
#else
    return error_e::io_error;
#endif
  }

  int root_t::create_file(const std::size_t index) {
#ifndef _WIN32
    if (index >= final_paths_.size() || dirs_[index]) {
      return -1;
    }
    const auto &path = final_paths_[index];
    const auto slash = path.rfind('/');
    const auto parent = open_below(fd_, slash == std::string::npos ? std::string {} : path.substr(0, slash));
    if (parent < 0) {
      return -1;
    }
    const auto name = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto fd = openat(parent, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    ::close(parent);
    return fd;
#else
    return -1;
#endif
  }

  std::vector<std::string> root_t::top_level_paths() const {
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < final_paths_.size(); ++i) {
      if (top_level_[i]) {
        paths.push_back((path_ / final_paths_[i]).string());
      }
    }
    return paths;
  }

  void root_t::remove() {
#ifndef _WIN32
    if (fd_ < 0) {
      return;
    }
    ::close(fd_);
    fd_ = -1;
    const auto parent = ::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent >= 0) {
      remove_tree(parent, path_.filename().string());
      ::close(parent);
    }
#endif
  }

  area_t::area_t(std::filesystem::path path):
      path_ {std::move(path)} {
  }

  area_t::~area_t() {
#ifndef _WIN32
    if (fd_ >= 0) {
      ::close(fd_);
    }
#endif
  }

  bool area_t::open() {
#ifndef _WIN32
    if (fd_ >= 0) {
      return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
    const auto fd = ::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!owned_directory(fd) || fchmod(fd, 0700) != 0) {
      if (fd >= 0) {
        ::close(fd);
      }
      BOOST_LOG(warning) << "[clipboard] Cannot use the staging directory "sv << path_.string();
      return false;
    }
    fd_ = fd;
    return true;
#else
    return false;
#endif
  }

  void area_t::sweep(const std::chrono::system_clock::time_point now) {
    clean(true, now);
  }

  void area_t::reap(const std::chrono::system_clock::time_point now) {
    clean(false, now);
  }

  void area_t::clean(const bool adopt, const std::chrono::system_clock::time_point now) {
#ifndef _WIN32
    if (fd_ < 0) {
      return;
    }
    std::set<std::string> roots;
    std::set<std::string> markers;
    for (const auto &name : list(fd_)) {
      struct stat st {};
      if (is_root_name(name) && fstatat(fd_, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode)) {
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
        unlinkat(fd_, marker.c_str(), 0);
        continue;
      }
      roots.erase(root);
      const auto released = read_marker(fd_, marker);
      if (!released) {
        write_marker(fd_, root, current);
      } else if (*released + keep <= current) {
        remove_tree(fd_, root);
        unlinkat(fd_, marker.c_str(), 0);
      }
    }
    if (adopt) {
      for (const auto &root : roots) {
        write_marker(fd_, root, current);
      }
    }
#endif
  }

  std::optional<std::uint64_t> area_t::free_bytes() const {
#ifndef _WIN32
    struct statvfs st {};
    if (fd_ < 0 || fstatvfs(fd_, &st) != 0) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize;
#else
    return std::nullopt;
#endif
  }

  std::shared_ptr<root_t> area_t::create_root() {
#ifndef _WIN32
    if (fd_ < 0) {
      return nullptr;
    }
    std::random_device device;
    std::uniform_int_distribution<int> digit {0, 15};
    std::string name;
    for (std::size_t i = 0; i < root_name_size; ++i) {
      name.push_back("0123456789abcdef"[digit(device)]);
    }
    if (mkdirat(fd_, name.c_str(), 0700) != 0) {
      return nullptr;
    }
    const auto fd = openat(fd_, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!owned_directory(fd)) {
      if (fd >= 0) {
        ::close(fd);
      }
      return nullptr;
    }
    return std::make_shared<root_t>(path_ / name, fd);
#else
    return nullptr;
#endif
  }

  void area_t::release(const root_t &root, const std::chrono::system_clock::time_point now) {
#ifndef _WIN32
    if (fd_ >= 0) {
      write_marker(fd_, root.path().filename().string(), unix_seconds(now));
    }
#endif
  }
}  // namespace clipboard::staging
