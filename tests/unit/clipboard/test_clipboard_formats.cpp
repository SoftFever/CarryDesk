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

TEST(ClipboardFormatsTest, WindowsOfferMapsClipboardFormats) {
  const auto offer = map_windows_offer({"CF_DIB", "HTML Format", "CF_UNICODETEXT", "Rich Text Format", "CF_DIBV5", "CF_HDROP", "CF_TEXT", "Preferred DropEffect"}, false);
  EXPECT_FALSE(offer.sensitive);
  EXPECT_EQ(offer.formats, (std::vector<native_format_t> {
                             {std::string {mime::text}, "CF_UNICODETEXT"},
                             {std::string {mime::html}, "HTML Format"},
                             {std::string {mime::rtf}, "Rich Text Format"},
                             {std::string {mime::png}, "CF_DIB"},
                             {std::string {mime::files}, "CF_HDROP"},
                           }));
  EXPECT_TRUE(map_windows_offer({}, false).formats.empty());
  EXPECT_EQ(map_windows_offer({"html format"}, false).formats.at(0).native, "html format");
}

TEST(ClipboardFormatsTest, WindowsOfferPrefersAPngFormatOverBitmaps) {
  const auto png_of = [](const std::vector<std::string> &formats) {
    return map_windows_offer(formats, false).formats.at(0).native;
  };
  EXPECT_EQ(png_of({"CF_DIBV5", "CF_DIB", "image/png", "PNG"}), "PNG");
  EXPECT_EQ(png_of({"CF_DIBV5", "CF_DIB", "image/png"}), "image/png");
  EXPECT_EQ(png_of({"CF_DIBV5", "CF_DIB"}), "CF_DIBV5");
  EXPECT_TRUE(map_windows_offer({"CF_BITMAP"}, false).formats.empty());
}

TEST(ClipboardFormatsTest, WindowsContentMarkedPrivateIsSensitive) {
  for (const auto *marker : {"ExcludeClipboardContentFromMonitorProcessing", "Clipboard Viewer Ignore"}) {
    const auto offer = map_windows_offer({"CF_UNICODETEXT", marker}, false);
    EXPECT_TRUE(offer.sensitive) << marker;
    EXPECT_TRUE(offer.formats.empty()) << marker;
  }
  EXPECT_TRUE(map_windows_offer({"CF_UNICODETEXT"}, true).sensitive);
}

TEST(ClipboardFormatsTest, WindowsTextConvertsBetweenUtf16AndCanonicalText) {
  const auto native = utf16_from_text("a\nb\r\n\xF0\x9F\x98\x80");
  EXPECT_EQ(native, std::string("a\0\r\0\n\0b\0\r\0\n\0\x3D\xD8\x00\xDE\0\0", 18));
  EXPECT_EQ(text_from_utf16(native), "a\nb\n\xF0\x9F\x98\x80");
  EXPECT_EQ(text_from_utf16(std::string("h\0i\0\0\0j\0", 8)), "hi");
  EXPECT_EQ(text_from_utf16(std::string("\x00\xD8x", 3)), std::string {fffd} + std::string {fffd});
  EXPECT_EQ(utf16_from_text(""), std::string("\0\0", 2));
  EXPECT_EQ(utf16_from_text("\xFF"), std::string("\xFD\xFF\0\0", 4));
}

TEST(ClipboardFormatsTest, CfHtmlWrapsFragmentsWithCorrectOffsets) {
  const auto data = make_cf_html("<b>hi</b>");
  ASSERT_TRUE(data.starts_with("Version:0.9\r\nStartHTML:0000000105\r\nEndHTML:"));
  const auto field = [&](const std::string &key) {
    const auto pos = data.find(key);
    return std::stoul(data.substr(pos + key.size(), 10));
  };
  EXPECT_EQ(field("EndHTML:"), data.size());
  EXPECT_EQ(data.substr(field("StartFragment:"), field("EndFragment:") - field("StartFragment:")), "<b>hi</b>");
  EXPECT_EQ(data.substr(105), "<html>\r\n<body>\r\n<!--StartFragment--><b>hi</b><!--EndFragment-->\r\n</body>\r\n</html>");
  EXPECT_EQ(html_from_cf_html(data + '\0'), data.substr(105));
}

TEST(ClipboardFormatsTest, CfHtmlMarksTheBodyOfADocument) {
  const std::string document = "<html><head><style>p{color:red}</style></head><BODY class=\"x\"><p>t</p></body></html>";
  const auto data = make_cf_html(document);
  const auto field = [&](const std::string &key) {
    const auto pos = data.find(key);
    return std::stoul(data.substr(pos + key.size(), 10));
  };
  EXPECT_EQ(data.substr(field("StartFragment:"), field("EndFragment:") - field("StartFragment:")), "<p>t</p>");
  EXPECT_EQ(html_from_cf_html(data), "<html><head><style>p{color:red}</style></head><BODY class=\"x\"><!--StartFragment--><p>t</p><!--EndFragment--></body></html>");

  const std::string marked = "<html><body><!--StartFragment-->x<!--EndFragment--></body></html>";
  EXPECT_EQ(html_from_cf_html(make_cf_html(marked)), marked);
  const auto unclosed = make_cf_html("<body><p>x");
  EXPECT_NE(unclosed.find("<!--StartFragment--><body><p>x<!--EndFragment-->"), std::string::npos);
  EXPECT_NE(make_cf_html("<bodyguard>").find("<!--StartFragment--><bodyguard><!--EndFragment-->"), std::string::npos);
}

TEST(ClipboardFormatsTest, CfHtmlReadingFallsBackWhenTheHeaderIsIncomplete) {
  const std::string fragment_only = "Version:1.0\r\nStartHTML:-1\r\nEndHTML:-1\r\nStartFragment:0000000092\r\nEndFragment:0000000096\r\n<p>abcd</p>";
  ASSERT_EQ(fragment_only.find("<p>"), 89);
  EXPECT_EQ(html_from_cf_html(fragment_only), "abcd");
  EXPECT_EQ(html_from_cf_html("Version:0.9\r\nStartHTML:junk\r\n<i>x</i>"), "<i>x</i>");
  EXPECT_EQ(html_from_cf_html("Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000040\r\n<i>x</i>"), "<i>x</i>");
  EXPECT_EQ(html_from_cf_html("Version:0.9\r\nStartHTML:0000000036\r\nEndHTML:0000009999\r\n<i>x</i>\0junk"), "<i>x</i>");
  EXPECT_EQ(html_from_cf_html("no markup"), "");
}

TEST(ClipboardFormatsTest, DropFilesListWidePaths) {
  const std::vector<std::string> paths {"C:\\a b\\x.txt", "C:\\\xC3\xA9"};
  const auto bytes = make_drop_files(paths);
  EXPECT_EQ(bytes.size(), 20 + 13 * 2 + 5 * 2 + 2);
  EXPECT_EQ(bytes.substr(0, 20), std::string("\x14\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x01\0\0\0", 20));
  EXPECT_EQ(paths_from_drop_files(bytes), paths);
  EXPECT_EQ(paths_from_drop_files(make_drop_files({})), std::vector<std::string> {});

  auto narrow = bytes;
  narrow[16] = '\0';
  EXPECT_FALSE(paths_from_drop_files(narrow));
  EXPECT_FALSE(paths_from_drop_files(bytes.substr(0, 19)));
  auto outside = bytes;
  outside[0] = '\x7F';
  EXPECT_EQ(paths_from_drop_files(outside), std::vector<std::string> {});
  EXPECT_EQ(paths_from_drop_files(bytes.substr(0, 20 + 4)), std::vector<std::string> {"C:"});
}

TEST(ClipboardFormatsTest, PngLengthEndsAtTheEndChunk) {
  const auto chunk = [](const std::string &type, const std::string &data) {
    std::string out;
    for (const auto shift : {24, 16, 8, 0}) {
      out.push_back(static_cast<char>((data.size() >> shift) & 0xFF));
    }
    return out + type + data + "CRC!";
  };
  const auto png = std::string("\x89PNG\r\n\x1A\n", 8) + chunk("IHDR", std::string(13, 'h')) + chunk("IEND", "");
  EXPECT_EQ(png_length(png + std::string(16, '\0')), png.size());
  EXPECT_EQ(png_length(png), png.size());
  EXPECT_EQ(png_length("not a png"), 9);
  EXPECT_EQ(png_length(png.substr(0, 20)), 20);
  EXPECT_EQ(windows_to_canonical(mime::png, png + "pad"), png);
}

TEST(ClipboardFormatsTest, WindowsToCanonicalConvertsEachFormat) {
  EXPECT_EQ(windows_to_canonical(mime::text, std::string("a\0\r\0\n\0\0\0", 8)), "a\n");
  EXPECT_EQ(windows_to_canonical(mime::html, make_cf_html("<i>x</i>")), "<html>\r\n<body>\r\n<!--StartFragment--><i>x</i><!--EndFragment-->\r\n</body>\r\n</html>");
  EXPECT_EQ(windows_to_canonical(mime::rtf, std::string("{\\rtf1}\0\0", 9)), "{\\rtf1}");
}

TEST(ClipboardFormatsTest, WindowsNativeOffersForClientContent) {
  const std::map<std::string, std::string, std::less<>> canonical {
    {std::string {mime::text}, "hi\n"},
    {std::string {mime::html}, "<b>hi</b>"},
    {std::string {mime::rtf}, "{\\rtf1}"},
    {std::string {mime::png}, "PNG"},
  };
  const auto offers = windows_native_offers(canonical, {});
  EXPECT_EQ(offered(offers, "CF_UNICODETEXT"), utf16_from_text("hi\n"));
  EXPECT_EQ(offered(offers, "HTML Format"), make_cf_html("<b>hi</b>") + '\0');
  EXPECT_EQ(offered(offers, "Rich Text Format"), std::string("{\\rtf1}\0", 8));
  EXPECT_EQ(offered(offers, "PNG"), "PNG");
  for (const auto *marker : {"ExcludeClipboardContentFromMonitorProcessing", "CanIncludeInClipboardHistory", "CanUploadToCloudClipboard"}) {
    EXPECT_EQ(offered(offers, marker), std::string(4, '\0')) << marker;
  }
  EXPECT_FALSE(offered(offers, "CF_HDROP"));
  EXPECT_EQ(offers.size(), 7);
}

TEST(ClipboardFormatsTest, WindowsNativeOffersForStagedFiles) {
  const std::vector<std::string> paths {"C:\\s\\a.txt", "C:\\s\\b"};
  const auto files_only = windows_native_offers({}, paths);
  EXPECT_EQ(offered(files_only, "CF_HDROP"), make_drop_files(paths));
  EXPECT_EQ(offered(files_only, "Preferred DropEffect"), std::string("\x01\0\0\0", 4));
  EXPECT_EQ(offered(files_only, "CF_UNICODETEXT"), utf16_from_text("C:\\s\\a.txt\nC:\\s\\b"));
  EXPECT_EQ(files_only.front().first, "CF_HDROP");

  const std::map<std::string, std::string, std::less<>> text {{std::string {mime::text}, "own text"}};
  EXPECT_EQ(offered(windows_native_offers(text, paths), "CF_UNICODETEXT"), utf16_from_text("own text"));
}

TEST(ClipboardFormatsTest, EmptyClientContentOffersOnlyTheWindowsMarkers) {
  const auto offers = windows_native_offers({}, {});
  ASSERT_EQ(offers.size(), 3);
  EXPECT_EQ(offers[0].first, "ExcludeClipboardContentFromMonitorProcessing");
}
