/**
 * @file tests/unit/platform/macos/test_clipboard_macos.mm
 * @brief Test src/platform/macos/clipboard.* on a private pasteboard.
 * @details Each test gives the backend a pasteboard of its own and acts as another application by
 *          writing to and reading from that pasteboard, so the suite never touches the clipboard of
 *          the session it runs in. This file is built without ARC.
 */
// test includes
#include "../../../tests_common.h"

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// platform includes
#include <AppKit/AppKit.h>

// local includes
#include <src/clipboard/formats.h>
#include <src/clipboard/protocol.h>
#include <src/platform/macos/clipboard.h>

using namespace std::chrono_literals;

namespace {
  const std::string text_mime {clipboard::mime::text};
  const std::string html_mime {clipboard::mime::html};
  const std::string rtf_mime {clipboard::mime::rtf};
  const std::string png_mime {clipboard::mime::png};
  const std::string files_mime {clipboard::mime::files};

  using report_t = std::pair<std::uint64_t, std::vector<std::string>>;  ///< Revision and formats.
  using item_t = std::map<std::string, std::string>;  ///< Pasteboard types and bytes of one item.

  /**
   * @brief Records the backend's reports.
   */
  class observer_t: public platf::clipboard_observer_t {
  public:
    void on_clipboard_changed(const std::uint64_t revision, std::vector<std::string> formats) override {
      {
        std::lock_guard lock {mutex_};
        reports_.emplace_back(revision, std::move(formats));
      }
      changed_.notify_all();
    }

    /**
     * @brief Count the reports so far.
     *
     * @return Report count.
     */
    std::size_t count() {
      std::lock_guard lock {mutex_};
      return reports_.size();
    }

    /**
     * @brief Wait for a report after the first `count` ones.
     *
     * @param count Reports already seen.
     * @param timeout Longest wait.
     * @return The latest report, or no value on timeout.
     */
    std::optional<report_t> wait_after(const std::size_t count, const std::chrono::milliseconds timeout = 5s) {
      std::unique_lock lock {mutex_};
      if (!changed_.wait_for(lock, timeout, [&]() {
            return reports_.size() > count;
          })) {
        return std::nullopt;
      }
      return reports_.back();
    }

  private:
    std::mutex mutex_;  ///< Guards `reports_`.
    std::condition_variable changed_;  ///< Signalled on each report.
    std::vector<report_t> reports_;  ///< Reports in order.
  };

  /**
   * @brief Make a string from UTF-8 bytes.
   *
   * @param bytes UTF-8 bytes.
   * @return Autoreleased string.
   */
  NSString *ns_string(const std::string &bytes) {
    return [[[NSString alloc] initWithBytes:bytes.data() length:bytes.size() encoding:NSUTF8StringEncoding] autorelease];
  }

  /**
   * @brief Spell paths as macOS file URLs give them back, with decomposed accents.
   *
   * @param paths UTF-8 paths.
   * @return File system representations.
   */
  std::vector<std::string> file_system_paths(const std::vector<std::string> &paths) {
    std::vector<std::string> out;
    for (const auto &path : paths) {
      @autoreleasepool {
        out.emplace_back(ns_string(path).fileSystemRepresentation);
      }
    }
    return out;
  }

  /**
   * @brief Make a two-pixel image: opaque red, then half-transparent green.
   *
   * @return Autoreleased bitmap.
   */
  NSBitmapImageRep *test_image() {
    NSBitmapImageRep *image = [[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:2 pixelsHigh:1 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:8 bitsPerPixel:32] autorelease];
    const unsigned char pixels[] {0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x80};
    std::memcpy(image.bitmapData, pixels, sizeof(pixels));
    return image;
  }

  /**
   * @brief Encode the test image.
   *
   * @return PNG bytes.
   */
  std::string test_png() {
    @autoreleasepool {
      NSData *png = [test_image() representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
      return {static_cast<const char *>(png.bytes), png.length};
    }
  }

  /**
   * @brief Encode the test image as TIFF.
   *
   * @return TIFF bytes.
   */
  std::string test_tiff() {
    @autoreleasepool {
      NSData *tiff = test_image().TIFFRepresentation;
      return {static_cast<const char *>(tiff.bytes), tiff.length};
    }
  }

  class MacosClipboardTest: public testing::Test {
  protected:
    /**
     * @brief Create the private pasteboard and start the backend on it.
     */
    void SetUp() override {
      @autoreleasepool {
        pasteboard_ = [[NSPasteboard pasteboardWithUniqueName] retain];
        name_ = pasteboard_.name.UTF8String;
      }
      start();
    }

    /**
     * @brief Stop the backend and drop the private pasteboard.
     */
    void TearDown() override {
      if (backend) {
        backend->stop();
        backend.reset();
      }
      [pasteboard_ releaseGlobally];
      [pasteboard_ release];
    }

    /**
     * @brief Start a backend on the private pasteboard and wait for its first report.
     */
    void start() {
      const auto count = observer.count();
      backend = clipboard::mac::make_clipboard(name_);
      ASSERT_TRUE(backend->start(observer));
      initial = observer.wait_after(count);
      ASSERT_TRUE(initial);
    }

    /**
     * @brief Put content on the pasteboard as another application.
     *
     * @param items Types and bytes of each item.
     * @return `true` when the pasteboard took every item.
     */
    bool copy(const std::vector<item_t> &items) {
      @autoreleasepool {
        NSMutableArray<NSPasteboardItem *> *objects = [NSMutableArray array];
        for (const auto &item : items) {
          NSPasteboardItem *object = [[[NSPasteboardItem alloc] init] autorelease];
          for (const auto &[type, bytes] : item) {
            [object setData:[NSData dataWithBytes:bytes.data() length:bytes.size()] forType:ns_string(type)];
          }
          [objects addObject:object];
        }
        [pasteboard_ clearContents];
        return [pasteboard_ writeObjects:objects];
      }
    }

    /**
     * @brief Copy as another application and wait for the backend's report.
     *
     * @param items Types and bytes of each item.
     * @return The report.
     */
    report_t copy_and_wait(const std::vector<item_t> &items) {
      const auto count = observer.count();
      EXPECT_TRUE(copy(items));
      const auto report = observer.wait_after(count);
      EXPECT_TRUE(report);
      return report.value_or(report_t {});
    }

    /**
     * @brief Read a type as another application.
     *
     * @param type Pasteboard type.
     * @return Bytes, or no value when the pasteboard does not hold the type.
     */
    std::optional<std::string> paste(const std::string &type) {
      @autoreleasepool {
        NSData *data = [pasteboard_ dataForType:ns_string(type)];
        if (!data) {
          return std::nullopt;
        }
        return std::string {static_cast<const char *>(data.bytes), data.length};
      }
    }

    /**
     * @brief Read the file URLs on the pasteboard as Finder would.
     *
     * @return Paths of the file URLs in item order.
     */
    std::vector<std::string> pasted_paths() {
      @autoreleasepool {
        std::vector<std::string> paths;
        NSArray<NSURL *> *urls = [pasteboard_ readObjectsForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
        for (NSURL *url in urls) {
          paths.emplace_back(url.path.fileSystemRepresentation);
        }
        return paths;
      }
    }

    /**
     * @brief Count the items on the pasteboard.
     *
     * @return Item count.
     */
    std::size_t item_count() {
      @autoreleasepool {
        return pasteboard_.pasteboardItems.count;
      }
    }

    /**
     * @brief Read through the backend.
     *
     * @param revision Revision.
     * @param mime Canonical format.
     * @param max_size Size limit.
     * @return Read result.
     */
    platf::clipboard_read_t read(const std::uint64_t revision, const std::string &mime, const std::uint64_t max_size = 1 << 20) {
      return backend->read(revision, mime, max_size);
    }

    /**
     * @brief Check that no report arrives for a while.
     *
     * @param count Reports seen so far.
     */
    void expect_no_report(const std::size_t count) {
      EXPECT_FALSE(observer.wait_after(count, 800ms));
    }

    observer_t observer;  ///< Receives the backend's reports.
    std::unique_ptr<platf::clipboard_t> backend;  ///< Backend under test.
    std::optional<report_t> initial;  ///< Report of the pasteboard at start.

  private:
    NSPasteboard *pasteboard_ = nil;  ///< Private pasteboard.
    std::string name_;  ///< Its name.
  };
}  // namespace

TEST_F(MacosClipboardTest, StartReportsTheCurrentPasteboard) {
  EXPECT_TRUE(initial->second.empty());
  EXPECT_EQ(backend->formats(), (std::vector<std::string> {text_mime, html_mime, rtf_mime, png_mime, files_mime}));

  backend->stop();
  ASSERT_TRUE(copy({{{"public.utf8-plain-text", "there before"}}}));
  start();
  EXPECT_EQ(initial->second, std::vector<std::string> {text_mime});
  EXPECT_EQ(read(initial->first, text_mime).bytes, "there before");
}

TEST_F(MacosClipboardTest, ForeignTextIsReportedAndReadAsCanonicalText) {
  const auto [revision, formats] = copy_and_wait({{{"public.utf8-plain-text", "a\r\nb \xC3\xA9\rc"}}});
  EXPECT_GT(revision, initial->first);
  EXPECT_EQ(formats, std::vector<std::string> {text_mime});
  const auto result = read(revision, text_mime);
  EXPECT_EQ(result.result, platf::clipboard_result_e::ok);
  EXPECT_EQ(result.bytes, "a\nb \xC3\xA9\nc");
}

TEST_F(MacosClipboardTest, RichFormatsAreReportedAndRead) {
  const auto png = test_png();
  ASSERT_FALSE(png.empty());
  const auto [revision, formats] = copy_and_wait({{
    {"public.html", "<b>\xC3\xA9</b>"},
    {"public.rtf", "{\\rtf1 x}"},
    {"public.png", png},
    {"public.utf8-plain-text", "x"},
  }});
  EXPECT_EQ(formats, (std::vector<std::string> {text_mime, html_mime, rtf_mime, png_mime}));
  EXPECT_EQ(read(revision, html_mime).bytes, "<b>\xC3\xA9</b>");
  EXPECT_EQ(read(revision, rtf_mime).bytes, "{\\rtf1 x}");
  EXPECT_EQ(read(revision, png_mime).bytes, png);
  EXPECT_EQ(read(revision, text_mime).bytes, "x");
}

TEST_F(MacosClipboardTest, TiffIsReadAsPng) {
  const auto [revision, formats] = copy_and_wait({{{"public.tiff", test_tiff()}}});
  EXPECT_EQ(formats, std::vector<std::string> {png_mime});
  const auto result = read(revision, png_mime);
  ASSERT_EQ(result.result, platf::clipboard_result_e::ok);
  ASSERT_TRUE(result.bytes.starts_with("\x89PNG"));
  @autoreleasepool {
    NSBitmapImageRep *image = [NSBitmapImageRep imageRepWithData:[NSData dataWithBytes:result.bytes.data() length:result.bytes.size()]];
    ASSERT_NE(image, nil);
    EXPECT_EQ(image.pixelsWide, 2);
    EXPECT_EQ(image.pixelsHigh, 1);
  }
}

TEST_F(MacosClipboardTest, PrivateContentIsNotShared) {
  for (const auto *marker : {"org.nspasteboard.ConcealedType", "org.nspasteboard.TransientType"}) {
    const auto [revision, formats] = copy_and_wait({{{"public.utf8-plain-text", "secret"}, {marker, ""}}});
    EXPECT_TRUE(formats.empty()) << marker;
    EXPECT_EQ(read(revision, text_mime).result, platf::clipboard_result_e::stale) << marker;
  }
  const auto allowed = copy_and_wait({{{"public.utf8-plain-text", "fine"}}});
  EXPECT_EQ(allowed.second, std::vector<std::string> {text_mime});
}

TEST_F(MacosClipboardTest, FileUrlsAreReadAsPathsAlone) {
  const std::vector<std::string> paths {"/tmp/a b/x.txt", "/tmp/\xC3\xA9"};
  std::vector<item_t> items;
  for (const auto &path : paths) {
    @autoreleasepool {
      NSURL *url = [NSURL fileURLWithPath:ns_string(path) isDirectory:NO];
      items.push_back({{"public.file-url", url.absoluteString.UTF8String}, {"public.utf8-plain-text", path.substr(path.rfind('/') + 1)}});
    }
  }
  const auto [revision, formats] = copy_and_wait(items);
  EXPECT_EQ(formats, std::vector<std::string> {files_mime});
  const auto result = read(revision, files_mime);
  EXPECT_EQ(result.result, platf::clipboard_result_e::ok);
  EXPECT_EQ(result.paths, file_system_paths(paths));
  EXPECT_EQ(read(revision, text_mime).result, platf::clipboard_result_e::stale);
}

TEST_F(MacosClipboardTest, OwnershipServesEveryFormatWithoutEcho) {
  const auto png = test_png();
  const std::map<std::string, std::string, std::less<>> content {
    {text_mime, "hi\n\xC3\xA9"},
    {html_mime, "<i>x</i>"},
    {rtf_mime, "{\\rtf1}"},
    {png_mime, png},
  };
  const auto count = observer.count();
  ASSERT_EQ(backend->take_ownership(initial->first, content, {}), platf::clipboard_result_e::ok);
  EXPECT_EQ(paste("public.utf8-plain-text"), "hi\n\xC3\xA9");
  EXPECT_EQ(paste("public.html"), "<i>x</i>");
  EXPECT_EQ(paste("public.rtf"), "{\\rtf1}");
  EXPECT_EQ(paste("public.png"), png);
  EXPECT_TRUE(paste("public.tiff").has_value());
  EXPECT_TRUE(paste("org.nspasteboard.TransientType").has_value());
  EXPECT_EQ(item_count(), 1);
  expect_no_report(count);
}

TEST_F(MacosClipboardTest, StagedFilesAreOfferedAsFileUrlsAndPaths) {
  const std::vector<std::string> paths {"/tmp/staged/a b.txt", "/tmp/staged/\xC3\xA9"};
  ASSERT_EQ(backend->take_ownership(initial->first, {}, paths), platf::clipboard_result_e::ok);
  EXPECT_EQ(pasted_paths(), file_system_paths(paths));
  EXPECT_EQ(item_count(), 2);
  EXPECT_EQ(paste("public.utf8-plain-text"), "/tmp/staged/a b.txt\n/tmp/staged/\xC3\xA9");

  ASSERT_EQ(backend->take_ownership(initial->first, {{text_mime, "own text"}}, paths), platf::clipboard_result_e::ok);
  EXPECT_EQ(paste("public.utf8-plain-text"), "own text");
  EXPECT_EQ(pasted_paths(), file_system_paths(paths));
}

TEST_F(MacosClipboardTest, ForeignCopyAfterOwnershipIsReportedAndWins) {
  ASSERT_EQ(backend->take_ownership(initial->first, {{text_mime, "ours"}}, {}), platf::clipboard_result_e::ok);
  const auto [revision, formats] = copy_and_wait({{{"public.utf8-plain-text", "theirs"}}});
  EXPECT_GT(revision, initial->first);
  EXPECT_EQ(formats, std::vector<std::string> {text_mime});
  EXPECT_EQ(backend->take_ownership(initial->first, {{text_mime, "late"}}, {}), platf::clipboard_result_e::stale);
  EXPECT_EQ(read(revision, text_mime).bytes, "theirs");
  EXPECT_EQ(backend->take_ownership(revision, {{text_mime, "next"}}, {}), platf::clipboard_result_e::ok);
  EXPECT_EQ(paste("public.utf8-plain-text"), "next");
}

TEST_F(MacosClipboardTest, ACopyTheBackendHasNotSeenMakesOwnershipStale) {
  ASSERT_TRUE(copy({{{"public.utf8-plain-text", "just now"}}}));
  EXPECT_EQ(backend->take_ownership(initial->first, {{text_mime, "ours"}}, {}), platf::clipboard_result_e::stale);
  EXPECT_EQ(paste("public.utf8-plain-text"), "just now");
}

TEST_F(MacosClipboardTest, ReleaseEmptiesThePasteboardWithoutAReport) {
  ASSERT_EQ(backend->take_ownership(initial->first, {{text_mime, "ours"}}, {}), platf::clipboard_result_e::ok);
  const auto count = observer.count();
  backend->release();
  EXPECT_EQ(item_count(), 0);
  expect_no_report(count);

  const auto foreign = copy_and_wait({{{"public.utf8-plain-text", "theirs"}}});
  backend->release();
  EXPECT_EQ(paste("public.utf8-plain-text"), "theirs");
  EXPECT_EQ(read(foreign.first, text_mime).bytes, "theirs");
}

TEST_F(MacosClipboardTest, StaleRevisionsAreRefused) {
  const auto first = copy_and_wait({{{"public.utf8-plain-text", "a"}}});
  const auto second = copy_and_wait({{{"public.utf8-plain-text", "b"}}});
  EXPECT_EQ(read(first.first, text_mime).result, platf::clipboard_result_e::stale);
  EXPECT_EQ(read(second.first, text_mime).bytes, "b");
  EXPECT_EQ(read(second.first, html_mime).result, platf::clipboard_result_e::stale);
}

TEST_F(MacosClipboardTest, ReadsEnforceTheSizeLimit) {
  const auto [revision, formats] = copy_and_wait({{{"public.utf8-plain-text", std::string(1000, 'x')}, {"public.rtf", std::string(1000, 'r')}}});
  EXPECT_EQ(read(revision, text_mime, 999).result, platf::clipboard_result_e::too_large);
  EXPECT_EQ(read(revision, text_mime, 1000).bytes, std::string(1000, 'x'));
  EXPECT_EQ(read(revision, rtf_mime, 999).result, platf::clipboard_result_e::too_large);
}

TEST_F(MacosClipboardTest, StopReleasesOwnership) {
  ASSERT_EQ(backend->take_ownership(initial->first, {{text_mime, "ours"}}, {}), platf::clipboard_result_e::ok);
  backend->stop();
  backend.reset();
  EXPECT_EQ(item_count(), 0);
}

TEST(MacosClipboardFactoryTest, PlatformFactoryMakesTheMacBackend) {
  const auto backend = platf::make_clipboard();
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(backend->formats().size(), 5);
}
