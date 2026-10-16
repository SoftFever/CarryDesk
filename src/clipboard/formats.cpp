/**
 * @file src/clipboard/formats.cpp
 * @brief Definitions for clipboard format conversions and native format mapping.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cctype>
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
}  // namespace clipboard::formats
