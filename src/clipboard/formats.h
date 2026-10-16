/**
 * @file src/clipboard/formats.h
 * @brief Declarations for clipboard format conversions and native format mapping.
 * @details Pure functions shared by the service and the platform backends.
 */
#pragma once

// standard includes
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace clipboard::formats {
  inline constexpr std::string_view password_hint = "x-kde-passwordManagerHint";  ///< KDE's marker for sensitive clipboard content.
  inline constexpr std::string_view password_hint_value = "secret";  ///< Marker value that keeps content out of clipboard managers.
  inline constexpr std::string_view uri_list = "text/uri-list";  ///< Native list of URIs.
  inline constexpr std::string_view gnome_copied_files = "x-special/gnome-copied-files";  ///< Native file-manager copy list.

  /**
   * @brief Check whether bytes are well-formed UTF-8.
   *
   * @param bytes Bytes to check.
   * @return `true` when every sequence is valid UTF-8 (no overlong forms, surrogates or values above U+10FFFF).
   */
  bool is_valid_utf8(std::string_view bytes);

  /**
   * @brief Replace every invalid UTF-8 sequence with U+FFFD.
   *
   * @param bytes Bytes to clean.
   * @return Valid UTF-8.
   */
  std::string sanitize_utf8(std::string_view bytes);

  /**
   * @brief Convert native text to canonical text.
   *
   * @param bytes Text bytes, expected to be UTF-8.
   * @return UTF-8 with CRLF and lone CR replaced by LF and invalid sequences replaced.
   */
  std::string normalize_text(std::string_view bytes);

  /**
   * @brief Convert native HTML to canonical HTML.
   *
   * @param bytes HTML bytes: UTF-16 with a byte-order mark, or UTF-8.
   * @return UTF-8 HTML without a UTF-16 byte-order mark.
   */
  std::string html_to_utf8(std::string_view bytes);

  /**
   * @brief Percent-encode an absolute path for a `file://` URI.
   *
   * @param path Absolute path bytes.
   * @return Path with every byte outside `A-Z a-z 0-9 - . _ ~ /` encoded as uppercase `%XX`.
   */
  std::string percent_encode_path(std::string_view path);

  /**
   * @brief Build a `file://` URI for an absolute path.
   *
   * @param path Absolute path bytes.
   * @return `file://` followed by the encoded path.
   */
  std::string file_uri(std::string_view path);

  /**
   * @brief Extract local file paths from a `text/uri-list`.
   * @details Comment lines are skipped, and only `file://` URIs with an empty or `localhost` host are
   *          used. Paths are percent-decoded.
   *
   * @param list Native URI list.
   * @return Absolute local paths in list order.
   */
  std::vector<std::string> file_paths_from_uri_list(std::string_view list);

  /**
   * @brief Build the `text/uri-list` selection for staged files.
   *
   * @param paths Absolute staged paths, one per top-level entry.
   * @return Each URI followed by CRLF.
   */
  std::string make_uri_list(const std::vector<std::string> &paths);

  /**
   * @brief Build the `x-special/gnome-copied-files` selection for staged files.
   *
   * @param paths Absolute staged paths, one per top-level entry.
   * @return `copy` followed by a newline and URI per entry, with no trailing newline.
   */
  std::string make_gnome_copied_files(const std::vector<std::string> &paths);

  /**
   * @brief Build the text selection for staged files.
   *
   * @param paths Absolute staged paths, one per top-level entry.
   * @return Unencoded paths joined with LF.
   */
  std::string make_path_text(const std::vector<std::string> &paths);

  /**
   * @brief One canonical format the host clipboard offers and the native type to read it from.
   */
  struct native_format_t {
    std::string canonical;  ///< Canonical mime.
    std::string native;  ///< Native mime to read.

    /**
     * @brief Compare two mappings.
     *
     * @param other Mapping to compare with.
     * @return `true` when both fields match.
     */
    bool operator==(const native_format_t &other) const = default;
  };

  /**
   * @brief Canonical view of a Linux selection.
   */
  struct linux_offer_t {
    bool sensitive = false;  ///< The selection carries the password-manager hint and must not be shared.
    std::vector<native_format_t> formats;  ///< Canonical formats in canonical order.
  };

  /**
   * @brief Map the native types a Linux selection offers to canonical formats.
   *
   * @param native_mimes Types the selection offers.
   * @param uri_list_has_files Whether the selection's `text/uri-list` holds at least one `file://` URI.
   * @return Canonical formats with their preferred native types, or a sensitive marker.
   */
  linux_offer_t map_linux_offer(const std::vector<std::string> &native_mimes, bool uri_list_has_files);

  /**
   * @brief Convert bytes read from a Linux selection to canonical bytes.
   *
   * @param canonical Canonical mime being read.
   * @param bytes Bytes read from the native type chosen by `map_linux_offer`.
   * @return Canonical bytes.
   */
  std::string linux_to_canonical(std::string_view canonical, std::string_view bytes);

  /**
   * @brief Native types and bytes the host offers for a client's content.
   */
  using native_offers_t = std::vector<std::pair<std::string, std::shared_ptr<const std::string>>>;

  /**
   * @brief Build the native offers of a Linux selection that holds a client's content.
   * @details Every selection also offers the password-manager hint so clipboard managers neither
   *          record nor restore client content.
   *
   * @param canonical Canonical mime to bytes; `files` is ignored here.
   * @param staged_paths Absolute staged paths of the top-level file entries; empty when no files.
   * @return Native types and bytes to offer.
   */
  native_offers_t linux_native_offers(const std::map<std::string, std::string, std::less<>> &canonical, const std::vector<std::string> &staged_paths);
}  // namespace clipboard::formats
