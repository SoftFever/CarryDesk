/**
 * @file tests/unit/clipboard/test_clipboard_formats.cpp
 * @brief Test src/clipboard/formats.*
 */

// test includes
#include "../../tests_common.h"

// local includes
#include <src/clipboard/formats.h>
#include <src/clipboard/protocol.h>

using namespace clipboard;
using namespace clipboard::formats;

namespace {
  constexpr std::string_view fffd = "\xEF\xBF\xBD";

  /**
   * @brief Find the bytes offered for a native type.
   *
   * @param offers Native offers.
   * @param type Native type.
   * @return Offered bytes, or no value when the type is not offered.
   */
  std::optional<std::string> offered(const native_offers_t &offers, std::string_view type) {
    for (const auto &[name, bytes] : offers) {
      if (name == type) {
        return *bytes;
      }
    }
    return std::nullopt;
  }
}  // namespace

TEST(ClipboardFormatsTest, Utf8ValidationRejectsMalformedSequences) {
  EXPECT_TRUE(is_valid_utf8(""));
  EXPECT_TRUE(is_valid_utf8("plain ascii"));
  EXPECT_TRUE(is_valid_utf8("\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 \xED\x9F\xBF \xF4\x8F\xBF\xBF"));
  for (const std::string_view bad : {
         std::string_view {"\xC0\x80"},
         std::string_view {"\xC1\xBF"},
         std::string_view {"\xE0\x80\x80"},
         std::string_view {"\xED\xA0\x80"},
         std::string_view {"\xF0\x80\x80\x80"},
         std::string_view {"\xF4\x90\x80\x80"},
         std::string_view {"\xF5\x80\x80\x80"},
         std::string_view {"\xFF"},
         std::string_view {"\x80"},
         std::string_view {"\xC3"},
         std::string_view {"\xE2\x82"},
         std::string_view {"\xF0\x9F\x98"},
       }) {
    EXPECT_FALSE(is_valid_utf8(bad)) << testing::PrintToString(std::string {bad});
  }
}

TEST(ClipboardFormatsTest, SanitizeReplacesEachInvalidByte) {
  EXPECT_EQ(sanitize_utf8("a\xFF"
                          "b"),
            std::string {"a"} + std::string {fffd} + "b");
  EXPECT_EQ(sanitize_utf8("\xC3\xA9"), "\xC3\xA9");
  EXPECT_EQ(sanitize_utf8("\xE2\x82"), std::string {fffd} + std::string {fffd});
}

TEST(ClipboardFormatsTest, NormalizeTextConvertsLineEndings) {
  EXPECT_EQ(normalize_text("a\r\nb\rc\nd\r"), "a\nb\nc\nd\n");
  EXPECT_EQ(normalize_text("\r\r\n"), "\n\n");
  EXPECT_EQ(normalize_text("x\x80y"), std::string {"x"} + std::string {fffd} + "y");
}

TEST(ClipboardFormatsTest, HtmlFromUtf16WithByteOrderMark) {
  const std::string little {"\xFF\xFE<\0b\0>\0\xE9\0=\xD8\x00\xDE", 14};
  EXPECT_EQ(html_to_utf8(little), "<b>\xC3\xA9\xF0\x9F\x98\x80");

  const std::string big {"\xFE\xFF\0<\0\xE9\x20\xAC", 8};
  EXPECT_EQ(html_to_utf8(big), "<\xC3\xA9\xE2\x82\xAC");

  const std::string lone_surrogate {
    "\xFF\xFE\x00\xD8"
    "a\0\x00\xDC",
    8
  };
  EXPECT_EQ(html_to_utf8(lone_surrogate), std::string {fffd} + "a" + std::string {fffd});

  const std::string high_at_end {"\xFF\xFE\x00\xD8", 4};
  EXPECT_EQ(html_to_utf8(high_at_end), std::string {fffd});

  const std::string odd {"\xFF\xFEh\0i", 5};
  EXPECT_EQ(html_to_utf8(odd), std::string {"h"} + std::string {fffd});

  EXPECT_EQ(html_to_utf8("<p>\xC3\xA9</p>"), "<p>\xC3\xA9</p>");
  EXPECT_EQ(html_to_utf8("x"), "x");
  EXPECT_EQ(html_to_utf8("<\xFF>"), std::string {"<"} + std::string {fffd} + ">");
}

TEST(ClipboardFormatsTest, PercentEncodingKeepsOnlyUnreservedAndSlash) {
  EXPECT_EQ(percent_encode_path("/a b/#%/é~._-Z9"), "/a%20b/%23%25/%C3%A9~._-Z9");
  EXPECT_EQ(file_uri("/tmp/x y"), "file:///tmp/x%20y");
}

TEST(ClipboardFormatsTest, UriListYieldsLocalFilePathsOnly) {
  const auto paths = file_paths_from_uri_list(
    "# comment\r\n"
    "file:///home/u/a%20b.txt\r\n"
    "https://example.com/x\r\n"
    "FILE://localhost/srv/%c3%a9\r\n"
    "file://otherhost/share/x\r\n"
    "file:/single/slash\n"
    "file://\n"
    "file:relative\n"
    "\n"
    "file:///bad%zz%4\t\n"
    "file:///last"
  );
  const std::vector<std::string> expected {"/home/u/a b.txt", "/srv/\xC3\xA9", "/single/slash", "/bad%zz%4", "/last"};
  EXPECT_EQ(paths, expected);
  EXPECT_TRUE(file_paths_from_uri_list("").empty());
  EXPECT_TRUE(file_paths_from_uri_list("http://a\r\n").empty());
}

TEST(ClipboardFormatsTest, StagedSelectionContentsAreExact) {
  const std::vector<std::string> paths {"/s/a b#%.txt", "/s/d\xC3\xA9"};
  EXPECT_EQ(make_uri_list(paths), "file:///s/a%20b%23%25.txt\r\nfile:///s/d%C3%A9\r\n");
  EXPECT_EQ(make_gnome_copied_files(paths), "copy\nfile:///s/a%20b%23%25.txt\nfile:///s/d%C3%A9");
  EXPECT_EQ(make_path_text(paths), "/s/a b#%.txt\n/s/d\xC3\xA9");
  EXPECT_EQ(make_uri_list({}), "");
  EXPECT_EQ(make_gnome_copied_files({}), "copy");
  EXPECT_EQ(make_path_text({}), "");
}

TEST(ClipboardFormatsTest, LinuxOfferPrefersCanonicalNativeTypes) {
  const auto offer = map_linux_offer({"TEXT", "STRING", "text/plain", "UTF8_STRING", "Text/Plain; charset=UTF-8", "text/html", "application/rtf", "image/png", "image/jpeg", "text/uri-list"}, true);
  EXPECT_FALSE(offer.sensitive);
  const std::vector<native_format_t> expected {
    {std::string {mime::text}, "Text/Plain; charset=UTF-8"},
    {std::string {mime::html}, "text/html"},
    {std::string {mime::rtf}, "application/rtf"},
    {std::string {mime::png}, "image/png"},
    {std::string {mime::files}, "text/uri-list"},
  };
  EXPECT_EQ(offer.formats, expected);
}

TEST(ClipboardFormatsTest, LinuxOfferTextFallbacksAndExclusions) {
  const auto utf8_string = map_linux_offer({"STRING", "UTF8_STRING", "text/plain"}, false);
  ASSERT_EQ(utf8_string.formats.size(), 1);
  EXPECT_EQ(utf8_string.formats[0].native, "UTF8_STRING");

  const auto plain = map_linux_offer({"TEXT", "text/plain", "text/rtf"}, false);
  ASSERT_EQ(plain.formats.size(), 2);
  EXPECT_EQ(plain.formats[0].native, "text/plain");
  EXPECT_EQ(plain.formats[1].native, "text/rtf");

  EXPECT_TRUE(map_linux_offer({"STRING", "TEXT", "image/jpeg"}, false).formats.empty());
  EXPECT_TRUE(map_linux_offer({"text/uri-list"}, false).formats.empty());
}

TEST(ClipboardFormatsTest, LinuxOfferWithPasswordHintIsSensitive) {
  const auto offer = map_linux_offer({"text/plain", "x-kde-passwordManagerHint"}, false);
  EXPECT_TRUE(offer.sensitive);
  EXPECT_TRUE(offer.formats.empty());
}

TEST(ClipboardFormatsTest, LinuxToCanonicalConvertsTextAndHtml) {
  EXPECT_EQ(linux_to_canonical(mime::text, "a\r\nb"), "a\nb");
  EXPECT_EQ(linux_to_canonical(mime::html, std::string {"\xFF\xFEh\0", 4}), "h");
  EXPECT_EQ(linux_to_canonical(mime::rtf, "{\\rtf1\r\n}"), "{\\rtf1\r\n}");
  EXPECT_EQ(linux_to_canonical(mime::png, std::string {"\x89PNG\r\n\0", 7}), std::string("\x89PNG\r\n\0", 7));
}

TEST(ClipboardFormatsTest, LinuxNativeOffersForClientContent) {
  const std::map<std::string, std::string, std::less<>> canonical {
    {std::string {mime::text}, "hi"},
    {std::string {mime::html}, "<b>hi</b>"},
    {std::string {mime::rtf}, "{\\rtf1}"},
    {std::string {mime::png}, "PNG"},
  };
  const auto offers = linux_native_offers(canonical, {});
  EXPECT_EQ(offered(offers, "text/plain;charset=utf-8"), "hi");
  EXPECT_EQ(offered(offers, "text/plain"), "hi");
  EXPECT_EQ(offered(offers, "UTF8_STRING"), "hi");
  EXPECT_FALSE(offered(offers, "STRING"));
  EXPECT_FALSE(offered(offers, "TEXT"));
  EXPECT_EQ(offered(offers, "text/html"), "<b>hi</b>");
  EXPECT_EQ(offered(offers, "text/rtf"), "{\\rtf1}");
  EXPECT_EQ(offered(offers, "application/rtf"), "{\\rtf1}");
  EXPECT_EQ(offered(offers, "image/png"), "PNG");
  EXPECT_EQ(offered(offers, "x-kde-passwordManagerHint"), "secret");
  EXPECT_FALSE(offered(offers, "text/uri-list"));
  EXPECT_EQ(offers.size(), 8);
}

TEST(ClipboardFormatsTest, LinuxNativeOffersForStagedFiles) {
  const std::vector<std::string> paths {"/c/x y"};
  const auto files_only = linux_native_offers({}, paths);
  EXPECT_EQ(offered(files_only, "text/uri-list"), "file:///c/x%20y\r\n");
  EXPECT_EQ(offered(files_only, "x-special/gnome-copied-files"), "copy\nfile:///c/x%20y");
  EXPECT_EQ(offered(files_only, "text/plain;charset=utf-8"), "/c/x y");
  EXPECT_EQ(offered(files_only, "UTF8_STRING"), "/c/x y");
  EXPECT_EQ(offered(files_only, "x-kde-passwordManagerHint"), "secret");

  const std::map<std::string, std::string, std::less<>> text {{std::string {mime::text}, "own text"}};
  const auto with_text = linux_native_offers(text, paths);
  EXPECT_EQ(offered(with_text, "text/plain"), "own text");
  EXPECT_TRUE(offered(with_text, "text/uri-list"));
}

TEST(ClipboardFormatsTest, EmptyClientContentOffersOnlyTheHint) {
  const auto offers = linux_native_offers({}, {});
  ASSERT_EQ(offers.size(), 1);
  EXPECT_EQ(offers[0].first, "x-kde-passwordManagerHint");
}
