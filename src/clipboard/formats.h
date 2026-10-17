/**
 * @file src/clipboard/formats.h
 * @brief Declarations for clipboard format conversions and native format mapping.
 * @details Pure functions shared by the service and the platform backends.
 */
#pragma once

// standard includes
#include <map>
#include <memory>
#include <optional>
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
   * @brief Names of Windows clipboard formats.
   * @details Registered formats go by their registered names; the predefined ones the backends use
   *          go by the names of their `CF_` constants.
   */
  namespace windows {
    inline constexpr std::string_view text = "CF_UNICODETEXT";  ///< NUL-terminated UTF-16LE text.
    inline constexpr std::string_view dib = "CF_DIB";  ///< Bitmap with a `BITMAPINFOHEADER`.
    inline constexpr std::string_view dibv5 = "CF_DIBV5";  ///< Bitmap with a `BITMAPV5HEADER`.
    inline constexpr std::string_view drop_files = "CF_HDROP";  ///< `DROPFILES` list of paths.
    inline constexpr std::string_view html = "HTML Format";  ///< CF_HTML: UTF-8 HTML with an offset header.
    inline constexpr std::string_view rtf = "Rich Text Format";  ///< NUL-terminated RTF.
    inline constexpr std::string_view png = "PNG";  ///< PNG image, as most Windows applications name it.
    inline constexpr std::string_view png_mime = "image/png";  ///< PNG image, as some toolkits name it.
    inline constexpr std::string_view drop_effect = "Preferred DropEffect";  ///< `DWORD` telling a paste to copy or move files.
    inline constexpr std::string_view exclude_monitoring = "ExcludeClipboardContentFromMonitorProcessing";  ///< Marker that keeps content out of clipboard monitors.
    inline constexpr std::string_view history_opt_in = "CanIncludeInClipboardHistory";  ///< `DWORD`; 0 keeps content out of the clipboard history.
    inline constexpr std::string_view cloud_opt_in = "CanUploadToCloudClipboard";  ///< `DWORD`; 0 keeps content off the cloud clipboard.
    inline constexpr std::string_view viewer_ignore = "Clipboard Viewer Ignore";  ///< Older marker that asks clipboard viewers to look away.
  }  // namespace windows

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
   * @brief Canonical view of the Windows clipboard.
   */
  struct windows_offer_t {
    bool sensitive = false;  ///< The content is marked as not to be shared.
    std::vector<native_format_t> formats;  ///< Canonical formats in canonical order, each with the Windows format to read.
  };

  /**
   * @brief Map the formats the Windows clipboard offers to canonical formats.
   * @details PNG is read from a `PNG` format when there is one, otherwise from whichever of
   *          `CF_DIBV5` and `CF_DIB` comes first, which is the one the application put there itself.
   *
   * @param native_formats Format names in clipboard order.
   * @param opted_out The content opted out of the clipboard history or the cloud clipboard.
   * @return Canonical formats with the formats to read, or a sensitive marker.
   */
  windows_offer_t map_windows_offer(const std::vector<std::string> &native_formats, bool opted_out);

  /**
   * @brief Convert Windows text to canonical text.
   *
   * @param bytes UTF-16LE code units, ending at the first NUL if there is one.
   * @return Canonical UTF-8 text.
   */
  std::string text_from_utf16(std::string_view bytes);

  /**
   * @brief Convert canonical text to Windows text.
   *
   * @param text Canonical text.
   * @return UTF-16LE with CRLF line endings and a terminating NUL.
   */
  std::string utf16_from_text(std::string_view text);

  /**
   * @brief Extract HTML from Windows `HTML Format` data.
   * @details Uses the whole document between `StartHTML` and `EndHTML` so that styles in its head
   *          survive, the fragment when the document range is missing, and everything from the first
   *          tag when the header cannot be used.
   *
   * @param bytes CF_HTML data.
   * @return UTF-8 HTML.
   */
  std::string html_from_cf_html(std::string_view bytes);

  /**
   * @brief Build Windows `HTML Format` data.
   * @details Fragment markers already in the HTML are used as they are. Otherwise the fragment is
   *          the content of the document's body, or the whole HTML wrapped in a minimal document.
   *
   * @param html Canonical HTML.
   * @return CF_HTML data without a terminating NUL.
   */
  std::string make_cf_html(std::string_view html);

  /**
   * @brief Build a `DROPFILES` structure for paths.
   *
   * @param paths Absolute UTF-8 paths.
   * @return `DROPFILES` with wide, NUL-separated paths ending in an empty one.
   */
  std::string make_drop_files(const std::vector<std::string> &paths);

  /**
   * @brief Read the paths of a `DROPFILES` structure.
   *
   * @param bytes `DROPFILES` data.
   * @return UTF-8 paths, or no value when the paths are not wide characters.
   */
  std::optional<std::vector<std::string>> paths_from_drop_files(std::string_view bytes);

  /**
   * @brief Length of a PNG without the bytes that follow its `IEND` chunk.
   *
   * @param bytes PNG data, possibly padded.
   * @return Bytes up to the end of `IEND`, or the whole size when the chunks cannot be followed.
   */
  std::size_t png_length(std::string_view bytes);

  /**
   * @brief Convert bytes read from the Windows clipboard to canonical bytes.
   *
   * @param canonical Canonical mime being read; for PNG, the bytes are a `PNG` format.
   * @param bytes Bytes read from the format chosen by `map_windows_offer`.
   * @return Canonical bytes.
   */
  std::string windows_to_canonical(std::string_view canonical, std::string_view bytes);

  /**
   * @brief Build the Windows formats that hold a client's content.
   * @details Every clipboard also carries the markers that keep client content out of clipboard
   *          monitors, the clipboard history and the cloud clipboard. Bitmap formats for a PNG are
   *          left to the backend, which needs an image codec to make them.
   *
   * @param canonical Canonical mime to bytes; `files` is ignored here.
   * @param staged_paths Absolute staged paths of the top-level file entries; empty when no files.
   * @return Format names and bytes to put on the clipboard, in order.
   */
  native_offers_t windows_native_offers(const std::map<std::string, std::string, std::less<>> &canonical, const std::vector<std::string> &staged_paths);

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
