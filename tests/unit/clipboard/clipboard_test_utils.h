/**
 * @file tests/unit/clipboard/clipboard_test_utils.h
 * @brief Fakes shared by the clipboard service and endpoint tests.
 */
#pragma once

// standard includes
#include <map>
#include <memory>
#include <string>
#include <vector>

// local includes
#include <src/clipboard/service.h>

namespace clipboard_test {
  /**
   * @brief Clipboard backend that records calls and serves programmable content.
   */
  struct fake_backend_t: platf::clipboard_t {
    bool start(platf::clipboard_observer_t &clipboard_observer) override {
      observer = &clipboard_observer;
      started = start_result;
      return start_result;
    }

    void stop() override {
      ++stops;
    }

    [[nodiscard]] std::vector<std::string> formats() const override {
      return supported;
    }

    platf::clipboard_read_t read(std::uint64_t read_revision, const std::string &mime, std::uint64_t max_size) override {
      ++reads;
      platf::clipboard_read_t result;
      if (read_revision != revision) {
        result.result = platf::clipboard_result_e::stale;
        return result;
      }
      const auto it = content.find(mime);
      if (it == content.end()) {
        result.result = platf::clipboard_result_e::io_error;
        return result;
      }
      if (it->second.size() > max_size) {
        result.result = platf::clipboard_result_e::too_large;
        return result;
      }
      result.result = platf::clipboard_result_e::ok;
      result.bytes = it->second;
      return result;
    }

    platf::clipboard_result_e take_ownership(std::uint64_t expected_revision, const std::map<std::string, std::string, std::less<>> &installed, const std::vector<std::string> &file_paths) override {
      ownership_revisions.push_back(expected_revision);
      if (expected_revision != revision) {
        return platf::clipboard_result_e::stale;
      }
      installs.push_back(installed);
      installed_paths.push_back(file_paths);
      return ownership_result;
    }

    void release() override {
      ++releases;
    }

    /**
     * @brief Simulate another application changing the clipboard.
     *
     * @param formats New canonical formats.
     * @param new_content New content by canonical format.
     */
    void change(std::vector<std::string> formats, std::map<std::string, std::string> new_content = {}) {
      ++revision;
      content = std::move(new_content);
      observer->on_clipboard_changed(revision, std::move(formats));
    }

    bool start_result = true;  ///< Result of `start`.
    bool started = false;  ///< `start` succeeded.
    int stops = 0;  ///< Calls of `stop`.
    int reads = 0;  ///< Calls of `read`.
    int releases = 0;  ///< Calls of `release`.
    platf::clipboard_observer_t *observer = nullptr;  ///< Observer given to `start`.
    std::vector<std::string> supported {"text/plain;charset=utf-8", "text/html", "text/rtf", "image/png"};  ///< Supported formats.
    std::uint64_t revision = 0;  ///< Current revision.
    std::map<std::string, std::string> content;  ///< Current content.
    platf::clipboard_result_e ownership_result = platf::clipboard_result_e::ok;  ///< Result of a fresh `take_ownership`.
    std::vector<std::uint64_t> ownership_revisions;  ///< Expected revisions passed to `take_ownership`.
    std::vector<std::map<std::string, std::string, std::less<>>> installs;  ///< Installed content.
    std::vector<std::vector<std::string>> installed_paths;  ///< Installed file paths.
  };
}  // namespace clipboard_test
