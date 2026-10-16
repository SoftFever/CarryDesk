/**
 * @file src/clipboard/file_list.cpp
 * @brief Definitions for file manifests.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cctype>

#ifndef _WIN32
  #include <dirent.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>

  // lib includes
  #include <boost/locale.hpp>
#endif

// local includes
#include "file_list.h"
#include "formats.h"
#include "src/logging.h"

using namespace std::literals;

namespace clipboard::files {
  namespace {
    constexpr std::array<std::string_view, 22> reserved_names {
      "CON",
      "PRN",
      "AUX",
      "NUL",
      "COM1",
      "COM2",
      "COM3",
      "COM4",
      "COM5",
      "COM6",
      "COM7",
      "COM8",
      "COM9",
      "LPT1",
      "LPT2",
      "LPT3",
      "LPT4",
      "LPT5",
      "LPT6",
      "LPT7",
      "LPT8",
      "LPT9",
    };

    /**
     * @brief Check a single path segment.
     *
     * @param segment Segment between slashes.
     * @return A reason it is refused, or no value.
     */
    std::optional<std::string> check_segment(const std::string_view segment) {
      if (segment.empty()) {
        return "empty segment"s;
      }
      if (segment == "." || segment == "..") {
        return "dot segment"s;
      }
      if (segment.size() > limit::max_segment) {
        return "segment too long"s;
      }
      for (const auto c : segment) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
          return "control character"s;
        }
        if (c == '\\' || c == ':') {
          return "backslash or colon"s;
        }
      }
      if (segment.back() == '.' || segment.back() == ' ') {
        return "trailing dot or space"s;
      }
      std::string stem {segment.substr(0, segment.find('.'))};
      std::ranges::transform(stem, stem.begin(), [](const char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      });
      if (std::ranges::find(reserved_names, stem) != reserved_names.end()) {
        return "reserved name"s;
      }
      return std::nullopt;
    }

    /**
     * @brief Split a `/`-separated path.
     *
     * @param path Path to split.
     * @return Segments, empty ones included.
     */
    std::vector<std::string_view> split(const std::string_view path) {
      std::vector<std::string_view> segments;
      std::size_t start = 0;
      while (true) {
        const auto end = path.find('/', start);
        segments.push_back(path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
          return segments;
        }
        start = end + 1;
      }
    }

    /**
     * @brief Cut a UTF-8 string to at most a byte count without splitting a character.
     *
     * @param text Text to cut.
     * @param size Most bytes.
     * @return Cut text.
     */
    std::string cut_utf8(const std::string &text, std::size_t size) {
      if (text.size() <= size) {
        return text;
      }
      while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xC0) == 0x80) {
        --size;
      }
      return text.substr(0, size);
    }

#ifndef _WIN32
    /**
     * @brief Opened directories and identities that `open_entry` checks against.
     */
    struct posix_context_t {
      /**
       * @brief Where one manifest entry lives.
       */
      struct item_t {
        std::size_t root = 0;  ///< Index of the top-level entry's parent directory.
        std::vector<std::string> components;  ///< Path components from that parent.
        dev_t device = 0;  ///< Device at enumeration.
        ino_t inode = 0;  ///< Inode at enumeration.
      };

      posix_context_t() = default;
      posix_context_t(const posix_context_t &) = delete;
      posix_context_t &operator=(const posix_context_t &) = delete;

      ~posix_context_t() {
        for (const auto fd : parents) {
          ::close(fd);
        }
      }

      std::vector<int> parents;  ///< Parent directories of the top-level entries.
      std::vector<item_t> items;  ///< One item per manifest entry.
    };

    /**
     * @brief Builds a manifest by walking opened directories.
     */
    class walker_t {
    public:
      /**
       * @brief Start a walk.
       *
       * @param max_total Largest summed file size.
       */
      explicit walker_t(const std::uint64_t max_total):
          max_total_ {max_total},
          context_ {std::make_shared<posix_context_t>()},
          manifest_ {std::make_shared<manifest_t>()} {
      }

      /**
       * @brief Add one top-level path.
       *
       * @param path Absolute local path.
       * @return `false` when a limit was exceeded.
       */
      bool add_root(std::string path) {
        while (path.size() > 1 && path.back() == '/') {
          path.pop_back();
        }
        const auto slash = path.rfind('/');
        if (slash == std::string::npos || slash + 1 >= path.size()) {
          ++skipped_;
          return true;
        }
        const auto name = path.substr(slash + 1);
        const auto parent = ::open(slash == 0 ? "/" : path.substr(0, slash).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0) {
          ++skipped_;
          return true;
        }
        const auto root = context_->parents.size();
        context_->parents.push_back(parent);
        return add_child(root, parent, {}, name, 1);
      }

      /**
       * @brief Finish the walk.
       *
       * @return The result.
       */
      build_result_t finish() {
        build_result_t result;
        result.skipped = skipped_;
        if (error_) {
          result.error = error_;
          return result;
        }
        if (manifest_->entries.empty() && skipped_ > 0) {
          result.error = error_e::io_error;
          return result;
        }
        manifest_->context = context_;
        result.manifest = manifest_;
        return result;
      }

    private:
      /**
       * @brief Add an entry found in an opened directory, descending into directories.
       *
       * @param root Top-level index.
       * @param dirfd Directory containing the entry.
       * @param parent Components of the directory.
       * @param name Entry name.
       * @param depth Depth of the entry.
       * @return `false` when a limit was exceeded.
       */
      bool add_child(const std::size_t root, const int dirfd, const std::vector<std::string> &parent, const std::string &name, const std::size_t depth) {
        struct stat st {};
        if (fstatat(dirfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) || !formats::is_valid_utf8(name) || check_segment(name)) {
          ++skipped_;
          return true;
        }
        auto components = parent;
        components.push_back(name);
        std::string path;
        for (const auto &component : components) {
          path += (path.empty() ? "" : "/") + component;
        }
        const auto dir = S_ISDIR(st.st_mode);
        const auto size = dir ? std::uint64_t {0} : static_cast<std::uint64_t>(st.st_size);
        manifest_bytes_ += path.size() + 96;
        if (depth > limit::max_depth || path.size() > limit::max_path || manifest_->entries.size() >= limit::max_manifest_entries || manifest_bytes_ > limit::max_manifest_bytes || size > max_total_ - std::min(max_total_, manifest_->total_bytes)) {
          error_ = error_e::too_large;
          return false;
        }
        manifest_->entries.push_back({manifest_->entries.size(), path, size, static_cast<std::int64_t>(st.st_mtime), dir});
        manifest_->total_bytes += size;
        context_->items.push_back({root, components, st.st_dev, st.st_ino});
        if (!dir) {
          return true;
        }

        const auto child = openat(dirfd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) {
          return true;
        }
        std::vector<std::string> names;
        if (auto *stream = fdopendir(dup(child))) {
          while (const auto *entry = readdir(stream)) {
            if (std::string_view name_view {entry->d_name}; name_view != "." && name_view != "..") {
              names.emplace_back(name_view);
            }
          }
          closedir(stream);
        }
        std::ranges::sort(names);
        bool ok = true;
        for (const auto &child_name : names) {
          if (!add_child(root, child, components, child_name, depth + 1)) {
            ok = false;
            break;
          }
        }
        ::close(child);
        return ok;
      }

      std::uint64_t max_total_;  ///< Largest summed file size.
      std::shared_ptr<posix_context_t> context_;  ///< Context being built.
      std::shared_ptr<manifest_t> manifest_;  ///< Manifest being built.
      std::size_t manifest_bytes_ = 0;  ///< Estimated encoded size.
      std::size_t skipped_ = 0;  ///< Entries left out.
      std::optional<error_e> error_;  ///< Limit that stopped the walk.
    };
#endif
  }  // namespace

  std::optional<std::string> check_received_path(const std::string_view path) {
    if (path.empty()) {
      return "empty path"s;
    }
    if (path.size() > limit::max_path) {
      return "path too long"s;
    }
    if (!formats::is_valid_utf8(path)) {
      return "not UTF-8"s;
    }
    const auto segments = split(path);
    if (segments.size() > limit::max_depth) {
      return "path too deep"s;
    }
    for (const auto segment : segments) {
      if (auto reason = check_segment(segment)) {
        return reason;
      }
    }
    return std::nullopt;
  }

  std::string collision_key(const std::string_view name) {
#ifndef _WIN32
    static const auto locale = []() -> std::optional<std::locale> {
      try {
        return boost::locale::generator {}("en_US.UTF-8");
      } catch (const std::exception &) {
        return std::nullopt;
      }
    }();
    if (locale) {
      try {
        return boost::locale::fold_case(boost::locale::normalize(std::string {name}, boost::locale::norm_nfc, *locale), *locale);
      } catch (const std::exception &) {
        // Fall back to ASCII folding below.
      }
    }
#endif
    std::string key {name};
    std::ranges::transform(key, key.begin(), [](const char c) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return key;
  }

  std::string unique_names_t::claim(const std::string &parent, const std::string &name, const bool dir) {
    auto &taken = taken_[parent];
    if (taken.emplace(collision_key(name), true).second) {
      return name;
    }
    auto stem = name;
    std::string extension;
    if (!dir) {
      if (const auto dot = name.rfind('.'); dot != std::string::npos && dot > 0) {
        stem = name.substr(0, dot);
        extension = name.substr(dot);
      }
    }
    for (std::size_t n = 1;; ++n) {
      const auto suffix = " (" + std::to_string(n) + ")" + extension;
      auto candidate = cut_utf8(stem, limit::max_segment > suffix.size() ? limit::max_segment - suffix.size() : 0) + suffix;
      if (taken.emplace(collision_key(candidate), true).second) {
        return candidate;
      }
    }
  }

  build_result_t build_manifest(const std::vector<std::string> &paths, const std::uint64_t max_total) {
#ifndef _WIN32
    walker_t walker {max_total};
    for (const auto &path : paths) {
      if (!walker.add_root(path)) {
        break;
      }
    }
    return walker.finish();
#else
    return {error_e::io_error, nullptr, paths.size()};
#endif
  }

  int open_entry(const manifest_t &manifest, const std::uint64_t index) {
#ifndef _WIN32
    const auto context = std::static_pointer_cast<const posix_context_t>(manifest.context);
    if (!context || index >= context->items.size() || manifest.entries[index].dir) {
      return -1;
    }
    const auto &item = context->items[index];
    auto directory = context->parents[item.root];
    for (std::size_t i = 0; i + 1 < item.components.size(); ++i) {
      const auto next = openat(directory, item.components[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (directory != context->parents[item.root]) {
        ::close(directory);
      }
      if (next < 0) {
        return -1;
      }
      directory = next;
    }
    const auto fd = openat(directory, item.components.back().c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (directory != context->parents[item.root]) {
      ::close(directory);
    }
    if (fd < 0) {
      return -1;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_dev != item.device || st.st_ino != item.inode) {
      ::close(fd);
      return -1;
    }
    return fd;
#else
    return -1;
#endif
  }

  std::optional<std::vector<file_entry_t>> parse_manifest(const std::string_view ndjson) {
    std::vector<file_entry_t> entries;
    std::size_t start = 0;
    while (start < ndjson.size()) {
      const auto end = ndjson.find('\n', start);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      auto entry = parse_file_entry(ndjson.substr(start, end - start));
      if (!entry || entry->index != entries.size() || (entry->dir && entry->size != 0)) {
        return std::nullopt;
      }
      entries.push_back(std::move(*entry));
      start = end + 1;
    }
    return entries;
  }
}  // namespace clipboard::files
