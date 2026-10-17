/**
 * @file src/clipboard/formats.cpp
 * @brief Definitions for clipboard format conversions and native format mapping.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <optional>

// local includes
#include "formats.h"
#include "protocol.h"

namespace clipboard::formats {
  namespace {
    constexpr std::string_view replacement = "\xEF\xBF\xBD";

    /**
     * @brief Length of the valid UTF-8 sequence starting at a position.
     *
     * @param bytes Text being decoded.
     * @param pos Position of the sequence's first byte.
     * @return Sequence length, or 0 when the bytes at `pos` do not start a valid sequence.
     */
    std::size_t utf8_sequence_length(const std::string_view bytes, const std::size_t pos) {
      const auto byte = [&](const std::size_t i) {
        return static_cast<unsigned char>(bytes[pos + i]);
      };
      const auto in = [&](const std::size_t i, const unsigned char low, const unsigned char high) {
        return pos + i < bytes.size() && byte(i) >= low && byte(i) <= high;
      };
      const auto lead = byte(0);
      if (lead < 0x80) {
        return 1;
      }
      if (lead >= 0xC2 && lead <= 0xDF) {
        return in(1, 0x80, 0xBF) ? 2 : 0;
      }
      if (lead >= 0xE0 && lead <= 0xEF) {
        const unsigned char low = lead == 0xE0 ? 0xA0 : 0x80;
        const unsigned char high = lead == 0xED ? 0x9F : 0xBF;
        return in(1, low, high) && in(2, 0x80, 0xBF) ? 3 : 0;
      }
      if (lead >= 0xF0 && lead <= 0xF4) {
        const unsigned char low = lead == 0xF0 ? 0x90 : 0x80;
        const unsigned char high = lead == 0xF4 ? 0x8F : 0xBF;
        return in(1, low, high) && in(2, 0x80, 0xBF) && in(3, 0x80, 0xBF) ? 4 : 0;
      }
      return 0;
    }

    /**
     * @brief Append a code point as UTF-8.
     *
     * @param out Output text.
     * @param code_point Unicode scalar value.
     */
    void append_utf8(std::string &out, const char32_t code_point) {
      if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
      } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
      } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
      }
    }

    /**
     * @brief Convert UTF-16 to UTF-8, replacing unpaired surrogates and a trailing odd byte.
     *
     * @param bytes UTF-16 code units without the byte-order mark.
     * @param big_endian Whether the code units are big-endian.
     * @return UTF-8 text.
     */
    std::string utf16_to_utf8(const std::string_view bytes, const bool big_endian) {
      std::string out;
      out.reserve(bytes.size());
      const auto unit = [&](const std::size_t i) -> char16_t {
        const auto first = static_cast<unsigned char>(bytes[i]);
        const auto second = static_cast<unsigned char>(bytes[i + 1]);
        return big_endian ? static_cast<char16_t>((first << 8) | second) : static_cast<char16_t>((second << 8) | first);
      };
      std::size_t i = 0;
      for (; i + 1 < bytes.size(); i += 2) {
        const auto value = unit(i);
        if (value >= 0xD800 && value <= 0xDBFF && i + 3 < bytes.size()) {
          const auto low = unit(i + 2);
          if (low >= 0xDC00 && low <= 0xDFFF) {
            append_utf8(out, 0x10000 + ((static_cast<char32_t>(value) - 0xD800) << 10) + (low - 0xDC00));
            i += 2;
            continue;
          }
        }
        if (value >= 0xD800 && value <= 0xDFFF) {
          out.append(replacement);
        } else {
          append_utf8(out, value);
        }
      }
      if (i < bytes.size()) {
        out.append(replacement);
      }
      return out;
    }

    /**
     * @brief Value of a hexadecimal digit.
     *
     * @param c Character to convert.
     * @return Digit value, or no value for a non-hex character.
     */
    std::optional<int> hex_value(const char c) {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
      }
      return std::nullopt;
    }

    /**
     * @brief Decode `%XX` escapes; malformed escapes are kept literally.
     *
     * @param text Encoded text.
     * @return Decoded bytes.
     */
    std::string percent_decode(const std::string_view text) {
      std::string out;
      out.reserve(text.size());
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
          const auto high = hex_value(text[i + 1]);
          const auto low = hex_value(text[i + 2]);
          if (high && low) {
            out.push_back(static_cast<char>((*high << 4) | *low));
            i += 2;
            continue;
          }
        }
        out.push_back(text[i]);
      }
      return out;
    }

    /**
     * @brief Normalise a mime for comparison: lowercase without spaces.
     *
     * @param mime Mime as offered.
     * @return Comparable form.
     */
    std::string comparable_mime(const std::string_view mime) {
      std::string out;
      for (const auto c : mime) {
        if (c != ' ' && c != '\t') {
          out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
      }
      return out;
    }

    /**
     * @brief Find the first offered native type among candidates, in candidate order.
     *
     * @param offered Types the selection offers.
     * @param candidates Acceptable types in preference order.
     * @return The offered spelling of the first match, or no value.
     */
    std::optional<std::string> first_offered(const std::vector<std::string> &offered, const std::initializer_list<std::string_view> candidates) {
      for (const auto candidate : candidates) {
        const auto wanted = comparable_mime(candidate);
        for (const auto &mime : offered) {
          if (comparable_mime(mime) == wanted) {
            return mime;
          }
        }
      }
      return std::nullopt;
    }

    /**
     * @brief Make a shared byte buffer.
     *
     * @param bytes Bytes to share.
     * @return Shared immutable buffer.
     */
    std::shared_ptr<const std::string> shared(std::string bytes) {
      return std::make_shared<const std::string>(std::move(bytes));
    }

    /**
     * @brief Decode one code point of valid UTF-8.
     *
     * @param bytes Valid UTF-8.
     * @param pos Position of the sequence; moved past it.
     * @return Code point.
     */
    char32_t decode_utf8(const std::string_view bytes, std::size_t &pos) {
      const auto length = utf8_sequence_length(bytes, pos);
      const auto lead = static_cast<unsigned char>(bytes[pos]);
      char32_t code_point = length == 1 ? lead : length == 2 ? (lead & 0x1F) : length == 3 ? (lead & 0x0F) : (lead & 0x07);
      for (std::size_t i = 1; i < length; ++i) {
        code_point = (code_point << 6) | (static_cast<unsigned char>(bytes[pos + i]) & 0x3F);
      }
      pos += length;
      return code_point;
    }

    /**
     * @brief Append a UTF-16 code unit in little-endian order.
     *
     * @param out Output bytes.
     * @param unit Code unit.
     */
    void append_utf16le(std::string &out, const char16_t unit) {
      out.push_back(static_cast<char>(unit & 0xFF));
      out.push_back(static_cast<char>(unit >> 8));
    }

    /**
     * @brief Convert UTF-8 to UTF-16LE, replacing invalid sequences.
     *
     * @param text UTF-8 text.
     * @return UTF-16LE bytes without a terminator.
     */
    std::string utf8_to_utf16le(const std::string_view text) {
      const auto valid = sanitize_utf8(text);
      std::string out;
      out.reserve(valid.size() * 2);
      for (std::size_t pos = 0; pos < valid.size();) {
        const auto code_point = decode_utf8(valid, pos);
        if (code_point >= 0x10000) {
          append_utf16le(out, static_cast<char16_t>(0xD800 + ((code_point - 0x10000) >> 10)));
          append_utf16le(out, static_cast<char16_t>(0xDC00 + ((code_point - 0x10000) & 0x3FF)));
        } else {
          append_utf16le(out, static_cast<char16_t>(code_point));
        }
      }
      return out;
    }

    /**
     * @brief Bytes before the first NUL code unit of UTF-16LE text.
     *
     * @param bytes UTF-16LE bytes.
     * @return Bytes up to the terminator, or all of them.
     */
    std::string_view until_utf16_nul(const std::string_view bytes) {
      for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        if (bytes[i] == '\0' && bytes[i + 1] == '\0') {
          return bytes.substr(0, i);
        }
      }
      return bytes;
    }

    /**
     * @brief Read a little-endian 32-bit value.
     *
     * @param bytes Source; at least `offset + 4` bytes.
     * @param offset Byte offset.
     * @return Value.
     */
    std::uint32_t read_u32le(const std::string_view bytes, const std::size_t offset) {
      std::uint32_t value = 0;
      for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
      }
      return value;
    }

    /**
     * @brief Append a little-endian 32-bit value.
     *
     * @param out Output bytes.
     * @param value Value.
     */
    void append_u32le(std::string &out, const std::uint32_t value) {
      for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
      }
    }

    /**
     * @brief A 32-bit value as clipboard bytes.
     *
     * @param value Value.
     * @return Four little-endian bytes.
     */
    std::string dword(const std::uint32_t value) {
      std::string out;
      append_u32le(out, value);
      return out;
    }

    /**
     * @brief Find text ignoring ASCII case.
     *
     * @param haystack Text to search.
     * @param needle Lowercase ASCII text to find.
     * @param from Position to start at.
     * @return Position, or `npos`.
     */
    std::size_t find_ascii_ci(const std::string_view haystack, const std::string_view needle, const std::size_t from = 0) {
      for (std::size_t i = from; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size() && match; ++j) {
          match = std::tolower(static_cast<unsigned char>(haystack[i + j])) == needle[j];
        }
        if (match) {
          return i;
        }
      }
      return std::string_view::npos;
    }

    /**
     * @brief Find a tag of an element, such as `<body` or `</body`, not a longer name.
     *
     * @param html HTML to search.
     * @param tag Lowercase start of the tag.
     * @param last Find the last occurrence instead of the first.
     * @return Position of `<`, or `npos`.
     */
    std::size_t find_tag(const std::string_view html, const std::string_view tag, const bool last) {
      auto found = std::string_view::npos;
      for (auto pos = find_ascii_ci(html, tag); pos != std::string_view::npos; pos = find_ascii_ci(html, tag, pos + 1)) {
        const auto next = pos + tag.size() < html.size() ? html[pos + tag.size()] : '\0';
        if (next == '>' || next == '/' || std::isspace(static_cast<unsigned char>(next))) {
          found = pos;
          if (!last) {
            break;
          }
        }
      }
      return found;
    }

    /**
     * @brief Read one numeric CF_HTML header field.
     *
     * @param header Header text.
     * @param key Field name followed by a colon.
     * @return Value, or no value when missing or not a number.
     */
    std::optional<long long> cf_html_field(const std::string_view header, const std::string_view key) {
      for (std::size_t start = 0; start < header.size();) {
        auto end = header.find_first_of("\r\n", start);
        if (end == std::string_view::npos) {
          end = header.size();
        }
        const auto line = header.substr(start, end - start);
        start = end + 1;
        if (!line.starts_with(key)) {
          continue;
        }
        auto value = line.substr(key.size());
        const auto negative = value.starts_with('-');
        if (negative) {
          value.remove_prefix(1);
        }
        long long number = 0;
        if (value.empty() || value.find_first_not_of("0123456789") != std::string_view::npos || value.size() > 18) {
          return std::nullopt;
        }
        for (const auto c : value) {
          number = number * 10 + (c - '0');
        }
        return negative ? -number : number;
      }
      return std::nullopt;
    }
  }  // namespace

  bool is_valid_utf8(const std::string_view bytes) {
    for (std::size_t pos = 0; pos < bytes.size();) {
      const auto length = utf8_sequence_length(bytes, pos);
      if (length == 0) {
        return false;
      }
      pos += length;
    }
    return true;
  }

  std::string sanitize_utf8(const std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t pos = 0; pos < bytes.size();) {
      const auto length = utf8_sequence_length(bytes, pos);
      if (length == 0) {
        out.append(replacement);
        ++pos;
      } else {
        out.append(bytes.substr(pos, length));
        pos += length;
      }
    }
    return out;
  }

  std::string normalize_text(const std::string_view bytes) {
    const auto text = sanitize_utf8(bytes);
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\r') {
        out.push_back('\n');
        if (i + 1 < text.size() && text[i + 1] == '\n') {
          ++i;
        }
      } else {
        out.push_back(text[i]);
      }
    }
    return out;
  }

  std::string html_to_utf8(const std::string_view bytes) {
    if (bytes.size() >= 2) {
      const auto first = static_cast<unsigned char>(bytes[0]);
      const auto second = static_cast<unsigned char>(bytes[1]);
      if (first == 0xFF && second == 0xFE) {
        return utf16_to_utf8(bytes.substr(2), false);
      }
      if (first == 0xFE && second == 0xFF) {
        return utf16_to_utf8(bytes.substr(2), true);
      }
    }
    return sanitize_utf8(bytes);
  }

  std::string percent_encode_path(const std::string_view path) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (const auto c : path) {
      const auto byte = static_cast<unsigned char>(c);
      if (std::isalnum(byte) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
        out.push_back(c);
      } else {
        out.push_back('%');
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0xF]);
      }
    }
    return out;
  }

  std::string file_uri(const std::string_view path) {
    return "file://" + percent_encode_path(path);
  }

  std::vector<std::string> file_paths_from_uri_list(const std::string_view list) {
    std::vector<std::string> paths;
    std::size_t start = 0;
    while (start < list.size()) {
      auto end = list.find('\n', start);
      if (end == std::string_view::npos) {
        end = list.size();
      }
      auto line = list.substr(start, end - start);
      start = end + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
      }
      if (line.empty() || line.front() == '#' || line.size() < 6 || comparable_mime(line.substr(0, 5)) != "file:") {
        continue;
      }
      auto rest = line.substr(5);
      if (rest.starts_with("//")) {
        rest.remove_prefix(2);
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos) {
          continue;
        }
        const auto host = comparable_mime(rest.substr(0, slash));
        if (!host.empty() && host != "localhost") {
          continue;
        }
        rest.remove_prefix(slash);
      }
      if (!rest.starts_with('/')) {
        continue;
      }
      paths.push_back(percent_decode(rest));
    }
    return paths;
  }

  std::string make_uri_list(const std::vector<std::string> &paths) {
    std::string out;
    for (const auto &path : paths) {
      out += file_uri(path);
      out += "\r\n";
    }
    return out;
  }

  std::string make_gnome_copied_files(const std::vector<std::string> &paths) {
    std::string out = "copy";
    for (const auto &path : paths) {
      out += '\n';
      out += file_uri(path);
    }
    return out;
  }

  std::string make_path_text(const std::vector<std::string> &paths) {
    std::string out;
    for (const auto &path : paths) {
      if (!out.empty()) {
        out += '\n';
      }
      out += path;
    }
    return out;
  }

  linux_offer_t map_linux_offer(const std::vector<std::string> &native_mimes, const bool uri_list_has_files) {
    linux_offer_t offer;
    if (first_offered(native_mimes, {password_hint})) {
      offer.sensitive = true;
      return offer;
    }
    const auto add = [&](const std::string_view canonical, const std::initializer_list<std::string_view> candidates) {
      if (const auto native = first_offered(native_mimes, candidates)) {
        offer.formats.push_back({std::string {canonical}, *native});
      }
    };
    add(mime::text, {mime::text, "UTF8_STRING", "text/plain"});
    add(mime::html, {mime::html});
    add(mime::rtf, {mime::rtf, "application/rtf"});
    add(mime::png, {mime::png});
    if (uri_list_has_files) {
      add(mime::files, {uri_list});
    }
    return offer;
  }

  std::string linux_to_canonical(const std::string_view canonical, const std::string_view bytes) {
    if (canonical == mime::text) {
      return normalize_text(bytes);
    }
    if (canonical == mime::html) {
      return html_to_utf8(bytes);
    }
    return std::string {bytes};
  }

  native_offers_t linux_native_offers(const std::map<std::string, std::string, std::less<>> &canonical, const std::vector<std::string> &staged_paths) {
    native_offers_t offers;
    if (!staged_paths.empty()) {
      offers.emplace_back(std::string {uri_list}, shared(make_uri_list(staged_paths)));
      offers.emplace_back(std::string {gnome_copied_files}, shared(make_gnome_copied_files(staged_paths)));
    }
    if (const auto it = canonical.find(mime::html); it != canonical.end()) {
      offers.emplace_back(std::string {mime::html}, shared(it->second));
    }
    if (const auto it = canonical.find(mime::rtf); it != canonical.end()) {
      const auto bytes = shared(it->second);
      offers.emplace_back(std::string {mime::rtf}, bytes);
      offers.emplace_back("application/rtf", bytes);
    }
    if (const auto it = canonical.find(mime::png); it != canonical.end()) {
      offers.emplace_back(std::string {mime::png}, shared(it->second));
    }
    std::shared_ptr<const std::string> text;
    if (const auto it = canonical.find(mime::text); it != canonical.end()) {
      text = shared(it->second);
    } else if (!staged_paths.empty()) {
      text = shared(make_path_text(staged_paths));
    }
    if (text) {
      for (const std::string_view type : {mime::text, std::string_view {"text/plain"}, std::string_view {"UTF8_STRING"}}) {
        offers.emplace_back(std::string {type}, text);
      }
    }
    offers.emplace_back(std::string {password_hint}, shared(std::string {password_hint_value}));
    return offers;
  }
  windows_offer_t map_windows_offer(const std::vector<std::string> &native_formats, const bool opted_out) {
    windows_offer_t offer;
    if (opted_out || first_offered(native_formats, {windows::exclude_monitoring, windows::viewer_ignore})) {
      offer.sensitive = true;
      return offer;
    }
    const auto add = [&](const std::string_view canonical, const std::initializer_list<std::string_view> candidates) {
      if (const auto native = first_offered(native_formats, candidates)) {
        offer.formats.push_back({std::string {canonical}, *native});
      }
    };
    add(mime::text, {windows::text});
    add(mime::html, {windows::html});
    add(mime::rtf, {windows::rtf});
    if (const auto png = first_offered(native_formats, {windows::png, windows::png_mime})) {
      offer.formats.push_back({std::string {mime::png}, *png});
    } else {
      const auto bitmap = std::ranges::find_if(native_formats, [](const std::string &format) {
        return format == windows::dibv5 || format == windows::dib;
      });
      if (bitmap != native_formats.end()) {
        offer.formats.push_back({std::string {mime::png}, *bitmap});
      }
    }
    add(mime::files, {windows::drop_files});
    return offer;
  }

  std::string text_from_utf16(const std::string_view bytes) {
    return normalize_text(utf16_to_utf8(until_utf16_nul(bytes), false));
  }

  std::string utf16_from_text(const std::string_view text) {
    std::string crlf;
    for (const auto c : normalize_text(text)) {
      if (c == '\n') {
        crlf.push_back('\r');
      }
      crlf.push_back(c);
    }
    auto out = utf8_to_utf16le(crlf);
    append_utf16le(out, 0);
    return out;
  }

  std::string html_from_cf_html(const std::string_view bytes) {
    const auto data = bytes.substr(0, bytes.find('\0'));
    const auto first_tag = data.find('<');
    const auto header = data.substr(0, first_tag == std::string_view::npos ? data.size() : first_tag);
    const auto size = static_cast<long long>(data.size());
    const auto range = [&](const std::string_view start_key, const std::string_view end_key) -> std::optional<std::string_view> {
      const auto start = cf_html_field(header, start_key);
      const auto end = cf_html_field(header, end_key);
      if (!start || !end || *start < static_cast<long long>(header.size()) || *start >= size || *end <= *start) {
        return std::nullopt;
      }
      return data.substr(static_cast<std::size_t>(*start), static_cast<std::size_t>(std::min(*end, size) - *start));
    };
    if (const auto document = range("StartHTML:", "EndHTML:")) {
      return sanitize_utf8(*document);
    }
    if (const auto fragment = range("StartFragment:", "EndFragment:")) {
      return sanitize_utf8(*fragment);
    }
    return first_tag == std::string_view::npos ? std::string {} : sanitize_utf8(data.substr(first_tag));
  }

  std::string make_cf_html(const std::string_view html) {
    constexpr std::string_view start_marker = "<!--StartFragment-->";
    constexpr std::string_view end_marker = "<!--EndFragment-->";
    const auto text = sanitize_utf8(html);
    std::string document;
    std::size_t fragment_start = 0;
    std::size_t fragment_end = 0;
    const auto marked_start = text.find(start_marker);
    const auto marked_end = text.rfind(end_marker);
    const auto body_open = find_tag(text, "<body", false);
    const auto body_open_end = body_open == std::string::npos ? std::string::npos : text.find('>', body_open);
    const auto body_close = find_tag(text, "</body", true);
    if (marked_start != std::string::npos && marked_end != std::string::npos && marked_end >= marked_start + start_marker.size()) {
      document = text;
      fragment_start = marked_start + start_marker.size();
      fragment_end = marked_end;
    } else if (body_open_end != std::string::npos && body_close != std::string::npos && body_open_end < body_close) {
      const auto content_start = body_open_end + 1;
      document = text.substr(0, content_start);
      document += start_marker;
      fragment_start = document.size();
      document += text.substr(content_start, body_close - content_start);
      fragment_end = document.size();
      document += end_marker;
      document += text.substr(body_close);
    } else {
      document = "<html>\r\n<body>\r\n";
      document += start_marker;
      fragment_start = document.size();
      document += text;
      fragment_end = document.size();
      document += end_marker;
      document += "\r\n</body>\r\n</html>";
    }
    const auto field = [](const std::string_view key, const std::size_t value) {
      auto digits = std::to_string(value);
      return std::string {key} + std::string(10 - std::min<std::size_t>(10, digits.size()), '0') + digits + "\r\n";
    };
    constexpr std::string_view version = "Version:0.9\r\n";
    const auto header_size = version.size() + field("StartHTML:", 0).size() + field("EndHTML:", 0).size() + field("StartFragment:", 0).size() + field("EndFragment:", 0).size();
    std::string out {version};
    out += field("StartHTML:", header_size);
    out += field("EndHTML:", header_size + document.size());
    out += field("StartFragment:", header_size + fragment_start);
    out += field("EndFragment:", header_size + fragment_end);
    out += document;
    return out;
  }

  std::string make_drop_files(const std::vector<std::string> &paths) {
    constexpr std::uint32_t header_size = 20;
    std::string out;
    append_u32le(out, header_size);  // pFiles
    append_u32le(out, 0);  // pt.x
    append_u32le(out, 0);  // pt.y
    append_u32le(out, 0);  // fNC
    append_u32le(out, 1);  // fWide
    for (const auto &path : paths) {
      out += utf8_to_utf16le(path);
      append_utf16le(out, 0);
    }
    append_utf16le(out, 0);
    return out;
  }

  std::optional<std::vector<std::string>> paths_from_drop_files(const std::string_view bytes) {
    constexpr std::size_t header_size = 20;
    if (bytes.size() < header_size || read_u32le(bytes, 16) == 0) {
      return std::nullopt;
    }
    std::vector<std::string> paths;
    const auto offset = read_u32le(bytes, 0);
    if (offset < header_size || offset >= bytes.size()) {
      return paths;
    }
    auto rest = bytes.substr(offset);
    while (rest.size() >= 2) {
      const auto path = until_utf16_nul(rest);
      if (path.empty()) {
        break;
      }
      paths.push_back(utf16_to_utf8(path, false));
      rest.remove_prefix(std::min(rest.size(), path.size() + 2));
    }
    return paths;
  }

  std::size_t png_length(const std::string_view bytes) {
    constexpr std::string_view signature = "\x89PNG\r\n\x1A\n";
    if (!bytes.starts_with(signature)) {
      return bytes.size();
    }
    std::size_t pos = signature.size();
    while (pos + 12 <= bytes.size()) {
      std::uint32_t length = 0;
      for (std::size_t i = 0; i < 4; ++i) {
        length = (length << 8) | static_cast<unsigned char>(bytes[pos + i]);
      }
      const auto next = pos + 12 + static_cast<std::size_t>(length);
      if (next > bytes.size()) {
        break;
      }
      if (bytes.substr(pos + 4, 4) == "IEND") {
        return next;
      }
      pos = next;
    }
    return bytes.size();
  }

  std::string windows_to_canonical(const std::string_view canonical, const std::string_view bytes) {
    if (canonical == mime::text) {
      return text_from_utf16(bytes);
    }
    if (canonical == mime::html) {
      return html_from_cf_html(bytes);
    }
    if (canonical == mime::rtf) {
      return std::string {bytes.substr(0, bytes.find('\0'))};
    }
    if (canonical == mime::png) {
      return std::string {bytes.substr(0, png_length(bytes))};
    }
    return std::string {bytes};
  }

  native_offers_t windows_native_offers(const std::map<std::string, std::string, std::less<>> &canonical, const std::vector<std::string> &staged_paths) {
    constexpr std::uint32_t drop_effect_copy = 1;
    native_offers_t offers;
    if (!staged_paths.empty()) {
      offers.emplace_back(std::string {windows::drop_files}, shared(make_drop_files(staged_paths)));
      offers.emplace_back(std::string {windows::drop_effect}, shared(dword(drop_effect_copy)));
    }
    if (const auto it = canonical.find(mime::html); it != canonical.end()) {
      offers.emplace_back(std::string {windows::html}, shared(make_cf_html(it->second) + '\0'));
    }
    if (const auto it = canonical.find(mime::rtf); it != canonical.end()) {
      offers.emplace_back(std::string {windows::rtf}, shared(it->second + '\0'));
    }
    if (const auto it = canonical.find(mime::png); it != canonical.end()) {
      offers.emplace_back(std::string {windows::png}, shared(it->second));
    }
    if (const auto it = canonical.find(mime::text); it != canonical.end()) {
      offers.emplace_back(std::string {windows::text}, shared(utf16_from_text(it->second)));
    } else if (!staged_paths.empty()) {
      offers.emplace_back(std::string {windows::text}, shared(utf16_from_text(make_path_text(staged_paths))));
    }
    offers.emplace_back(std::string {windows::exclude_monitoring}, shared(dword(0)));
    offers.emplace_back(std::string {windows::history_opt_in}, shared(dword(0)));
    offers.emplace_back(std::string {windows::cloud_opt_in}, shared(dword(0)));
    return offers;
  }
}  // namespace clipboard::formats
