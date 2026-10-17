/**
 * @file tests/unit/platform/windows/test_clipboard_windows.cpp
 * @brief Test src/platform/windows/clipboard.* in a private window station.
 * @details The clipboard belongs to the window station, so the suite creates its own window station
 *          and desktop and never touches the clipboard of the session it runs in. Threads that act
 *          as other applications join that desktop. Each backend test runs twice: reading in the
 *          backend's process, and through an agent, which runs on a thread here instead of in a
 *          user's session.
 */
// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <cstring>
  #include <functional>
  #include <mutex>
  #include <optional>
  #include <thread>
  #include <type_traits>

  // platform includes
  #include <Windows.h>
  #include <shellapi.h>

  // local includes
  #include <src/clipboard/formats.h>
  #include <src/clipboard/protocol.h>
  #include <src/platform/windows/clipboard.h>

using namespace std::chrono_literals;
using namespace clipboard::formats;

namespace {
  const std::string text_mime {clipboard::mime::text};
  const std::string html_mime {clipboard::mime::html};
  const std::string rtf_mime {clipboard::mime::rtf};
  const std::string png_mime {clipboard::mime::png};
  const std::string files_mime {clipboard::mime::files};
  constexpr auto desktop_name = L"SunshineClipboardTest";

  using report_t = std::pair<std::uint64_t, std::vector<std::string>>;  ///< Revision and formats.
  using content_t = std::vector<std::pair<std::string, std::string>>;  ///< Format names and bytes, in order.

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
   * @brief Get the id of a format by name.
   *
   * @param name `CF_` name of a predefined format, or a registered name.
   * @return Format id.
   */
  UINT format(const std::string_view name) {
    if (name == "CF_UNICODETEXT") {
      return CF_UNICODETEXT;
    }
    if (name == "CF_TEXT") {
      return CF_TEXT;
    }
    if (name == "CF_DIB") {
      return CF_DIB;
    }
    if (name == "CF_DIBV5") {
      return CF_DIBV5;
    }
    if (name == "CF_HDROP") {
      return CF_HDROP;
    }
    const std::wstring wide {name.begin(), name.end()};
    return RegisterClipboardFormatW(wide.c_str());
  }

  /**
   * @brief A `DWORD` as clipboard bytes.
   *
   * @param value Value.
   * @return Four little-endian bytes.
   */
  std::string dword(const std::uint32_t value) {
    return std::string {reinterpret_cast<const char *>(&value), sizeof(value)};
  }

  /**
   * @brief Open the clipboard, retrying while another thread holds it.
   *
   * @param owner Window to associate, or null.
   * @return `true` when open.
   */
  bool open_clipboard(HWND owner) {
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (OpenClipboard(owner)) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }

  /**
   * @brief Build a bitmap with a `BITMAPINFOHEADER`.
   *
   * @param width Width.
   * @param height Height; negative for top-down rows.
   * @param bits Bits per pixel.
   * @param pixels Pixel rows, each padded to four bytes.
   * @param palette Color table.
   * @return `CF_DIB` bytes.
   */
  std::string make_dib(const LONG width, const LONG height, const WORD bits, const std::string &pixels, const std::string &palette = {}) {
    BITMAPINFOHEADER info {};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = bits;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(pixels.size());
    info.biClrUsed = static_cast<DWORD>(palette.size() / 4);
    return std::string {reinterpret_cast<const char *>(&info), sizeof(info)} + palette + pixels;
  }

  /**
   * @brief Build a 2×1 bitmap with a `BITMAPV5HEADER` and an alpha channel.
   *
   * @return `CF_DIBV5` bytes: opaque red, then half-transparent green.
   */
  std::string make_dibv5() {
    BITMAPV5HEADER v5 {};
    v5.bV5Size = sizeof(v5);
    v5.bV5Width = 2;
    v5.bV5Height = 1;
    v5.bV5Planes = 1;
    v5.bV5BitCount = 32;
    v5.bV5Compression = BI_BITFIELDS;
    v5.bV5SizeImage = 8;
    v5.bV5RedMask = 0x00FF0000;
    v5.bV5GreenMask = 0x0000FF00;
    v5.bV5BlueMask = 0x000000FF;
    v5.bV5AlphaMask = 0xFF000000;
    v5.bV5CSType = 0x73524742;
    return std::string {reinterpret_cast<const char *>(&v5), sizeof(v5)} + std::string("\x00\x00\xFF\xFF\x00\xFF\x00\x80", 8);
  }

  /**
   * @brief A PNG of opaque red and half-transparent green.
   *
   * @return PNG bytes.
   */
  std::string test_png() {
    return clipboard::win::png_from_bitmap(make_dibv5()).value_or(std::string {});
  }

  /**
   * @brief Start agents on threads of this process instead of in a user's session.
   *
   * @param starts Counts the agents started.
   * @param serve Whether an agent serves requests, or closes its pipes at once.
   * @return Agent factory.
   */
  clipboard::win::agent_factory_t thread_agent(const std::shared_ptr<std::atomic<int>> &starts, const bool serve = true) {
    return [starts, serve]() -> std::optional<clipboard::win::agent_link_t> {
      HANDLE request_read = nullptr;
      HANDLE request_write = nullptr;
      HANDLE reply_read = nullptr;
      HANDLE reply_write = nullptr;
      if (!CreatePipe(&request_read, &request_write, nullptr, 0)) {
        return std::nullopt;
      }
      if (!CreatePipe(&reply_read, &reply_write, nullptr, 0)) {
        CloseHandle(request_read);
        CloseHandle(request_write);
        return std::nullopt;
      }
      ++*starts;
      auto thread = std::make_shared<std::thread>([request_read, reply_write, serve]() {
        if (serve) {
          clipboard::win::run_agent(request_read, reply_write, desktop_name);
        }
        CloseHandle(request_read);
        CloseHandle(reply_write);
      });
      return clipboard::win::agent_link_t {request_write, reply_read, [thread]() {
                                             thread->join();
                                           }};
    };
  }

  /**
   * @brief Run a backend in a private window station and act as other applications on it.
   * @details The parameter selects reading through an agent.
   */
  class WindowsClipboardTest: public testing::TestWithParam<bool> {
  protected:
    /**
     * @brief Create the window station and desktop and start the backend.
     */
    void SetUp() override {
      static int counter = 0;
      original_ = GetProcessWindowStation();
      const auto name = L"SunshineClipboardTest" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++counter);
      station_ = CreateWindowStationW(name.c_str(), 0, WINSTA_ALL_ACCESS, nullptr);
      ASSERT_TRUE(station_) << GetLastError();
      ASSERT_TRUE(SetProcessWindowStation(station_)) << GetLastError();
      desktop_ = CreateDesktopW(desktop_name, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
      ASSERT_TRUE(desktop_) << GetLastError();
      backend = clipboard::win::make_clipboard(desktop_name, GetParam() ? thread_agent(agent_starts) : clipboard::win::agent_factory_t {});
      ASSERT_TRUE(backend->start(observer));
      initial = observer.wait_after(0);
      ASSERT_TRUE(initial);
    }

    /**
     * @brief Stop the backend and return to the original window station.
     */
    void TearDown() override {
      if (backend) {
        backend->stop();
        backend.reset();
      }
      if (original_) {
        SetProcessWindowStation(original_);
      }
      if (desktop_) {
        CloseDesktop(desktop_);
      }
      if (station_) {
        CloseWindowStation(station_);
      }
    }

    /**
     * @brief Run a function on a thread of the private desktop, as another application would.
     *
     * @param function Function to run.
     * @return Its result.
     */
    template<class F>
    std::invoke_result_t<F> as_app(F &&function) {
      std::invoke_result_t<F> result {};
      std::thread thread {[&]() {
        SetThreadDesktop(desktop_);
        result = function();
      }};
      thread.join();
      return result;
    }

    /**
     * @brief Put content on the clipboard as another application.
     *
     * @param content Formats and bytes.
     * @return `true` when every format was set.
     */
    bool copy(const content_t &content) {
      return as_app([&]() {
        const auto window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        bool ok = window && open_clipboard(window);
        if (ok) {
          ok = EmptyClipboard();
          for (const auto &[name, bytes] : content) {
            const auto memory = GlobalAlloc(GMEM_MOVEABLE, std::max<std::size_t>(bytes.size(), 1));
            std::memcpy(GlobalLock(memory), bytes.data(), bytes.size());
            GlobalUnlock(memory);
            ok = ok && SetClipboardData(format(name), memory);
          }
          CloseClipboard();
        }
        if (window) {
          DestroyWindow(window);
        }
        return ok;
      });
    }

    /**
     * @brief Copy as another application and wait for the backend's report.
     *
     * @param content Formats and bytes.
     * @return The report.
     */
    report_t copy_and_wait(const content_t &content) {
      const auto count = observer.count();
      EXPECT_TRUE(copy(content));
      const auto report = observer.wait_after(count);
      EXPECT_TRUE(report);
      return report.value_or(report_t {});
    }

    /**
     * @brief Read a format as another application.
     *
     * @param name Format name.
     * @return Bytes, or no value when the clipboard does not hold it.
     */
    std::optional<std::string> paste(const std::string_view name) {
      return as_app([&]() -> std::optional<std::string> {
        if (!open_clipboard(nullptr)) {
          return std::nullopt;
        }
        std::optional<std::string> bytes;
        if (const auto handle = GetClipboardData(format(name))) {
          bytes = std::string {static_cast<const char *>(GlobalLock(handle)), GlobalSize(handle)};
          GlobalUnlock(handle);
        }
        CloseClipboard();
        return bytes;
      });
    }

    /**
     * @brief Count the formats on the clipboard as another application.
     *
     * @return Format count.
     */
    int format_count() {
      return as_app([]() {
        return CountClipboardFormats();
      });
    }

    /**
     * @brief Read through the backend from a thread of the private desktop.
     *
     * @param revision Revision.
     * @param mime Canonical format.
     * @param max_size Size limit.
     * @return Read result.
     */
    platf::clipboard_read_t read(const std::uint64_t revision, const std::string &mime, const std::uint64_t max_size = 1 << 20) {
      return as_app([&]() {
        return backend->read(revision, mime, max_size);
      });
    }

    /**
     * @brief Check that no report arrives for a while.
     *
     * @param count Reports seen so far.
     */
    void expect_no_report(const std::size_t count) {
      EXPECT_FALSE(observer.wait_after(count, 800ms));
    }

    std::shared_ptr<std::atomic<int>> agent_starts = std::make_shared<std::atomic<int>>(0);  ///< Agents started for the backend.
    observer_t observer;  ///< Receives the backend's reports.
    std::unique_ptr<platf::clipboard_t> backend;  ///< Backend under test.
    std::optional<report_t> initial;  ///< Report of the clipboard at start.

  private:
    HWINSTA original_ = nullptr;  ///< Window station to return to.
    HWINSTA station_ = nullptr;  ///< Private window station.
    HDESK desktop_ = nullptr;  ///< Private desktop.
  };
}  // namespace

TEST_P(WindowsClipboardTest, StartReportsTheCurrentClipboard) {
  EXPECT_TRUE(initial->second.empty());
  EXPECT_EQ(backend->formats(), (std::vector<std::string> {text_mime, html_mime, rtf_mime, png_mime, files_mime}));
}

TEST_P(WindowsClipboardTest, ForeignTextIsReportedAndReadAsCanonicalText) {
  const auto [revision, formats] = copy_and_wait({{"CF_UNICODETEXT", utf16_from_text("a\nb \xC3\xA9")}});
  EXPECT_GT(revision, initial->first);
  EXPECT_EQ(formats, std::vector<std::string> {text_mime});
  const auto result = read(revision, text_mime);
  EXPECT_EQ(result.result, platf::clipboard_result_e::ok);
  EXPECT_EQ(result.bytes, "a\nb \xC3\xA9");

  const auto ansi = copy_and_wait({{"CF_TEXT", std::string("plain\r\ntext\0", 12)}});
  EXPECT_EQ(ansi.second, std::vector<std::string> {text_mime});
  EXPECT_EQ(read(ansi.first, text_mime).bytes, "plain\ntext");
}

TEST_P(WindowsClipboardTest, RichFormatsAreReportedAndRead) {
  const auto png = test_png();
  ASSERT_FALSE(png.empty());
  const auto [revision, formats] = copy_and_wait({
    {"HTML Format", make_cf_html("<b>x</b>") + '\0'},
    {"Rich Text Format", std::string("{\\rtf1 x}\0", 10)},
    {"PNG", png + std::string(8, '\0')},
    {"CF_UNICODETEXT", utf16_from_text("x")},
  });
  EXPECT_EQ(formats, (std::vector<std::string> {text_mime, html_mime, rtf_mime, png_mime}));
  EXPECT_EQ(read(revision, html_mime).bytes, "<html>\r\n<body>\r\n<!--StartFragment--><b>x</b><!--EndFragment-->\r\n</body>\r\n</html>");
  EXPECT_EQ(read(revision, rtf_mime).bytes, "{\\rtf1 x}");
  EXPECT_EQ(read(revision, png_mime).bytes, png);
  EXPECT_EQ(read(revision, text_mime).bytes, "x");
}

TEST_P(WindowsClipboardTest, BitmapsAreReadAsPng) {
  // Opaque red and blue in a 32-bit bitmap whose fourth bytes are zero, as many applications leave them.
  const auto [revision, formats] = copy_and_wait({{"CF_DIB", make_dib(2, 1, 32, std::string("\x00\x00\xFF\x00\xFF\x00\x00\x00", 8))}});
  EXPECT_EQ(formats, std::vector<std::string> {png_mime});
  const auto result = read(revision, png_mime);
  ASSERT_EQ(result.result, platf::clipboard_result_e::ok);
  ASSERT_TRUE(result.bytes.starts_with("\x89PNG"));
  const auto bitmaps = clipboard::win::bitmaps_from_png(result.bytes);
  ASSERT_TRUE(bitmaps);
  EXPECT_EQ(bitmaps->first.substr(sizeof(BITMAPV5HEADER)), std::string("\x00\x00\xFF\xFF\xFF\x00\x00\xFF", 8));

  const auto v5 = copy_and_wait({{"CF_DIBV5", make_dibv5()}});
  EXPECT_EQ(v5.second, std::vector<std::string> {png_mime});
  const auto with_alpha = clipboard::win::bitmaps_from_png(read(v5.first, png_mime).bytes);
  ASSERT_TRUE(with_alpha);
  EXPECT_EQ(with_alpha->first.substr(sizeof(BITMAPV5HEADER)), std::string("\x00\x00\xFF\xFF\x00\xFF\x00\x80", 8));
}

TEST_P(WindowsClipboardTest, PrivateContentIsNotShared) {
  for (const auto &marker : std::vector<std::pair<std::string, std::string>> {
         {"ExcludeClipboardContentFromMonitorProcessing", ""},
         {"Clipboard Viewer Ignore", ""},
         {"CanUploadToCloudClipboard", dword(0)},
         {"CanIncludeInClipboardHistory", dword(0)},
       }) {
    const auto [revision, formats] = copy_and_wait({{"CF_UNICODETEXT", utf16_from_text("secret")}, marker});
    EXPECT_TRUE(formats.empty()) << marker.first;
    EXPECT_EQ(read(revision, text_mime).result, platf::clipboard_result_e::stale) << marker.first;
  }
  const auto allowed = copy_and_wait({{"CF_UNICODETEXT", utf16_from_text("fine")}, {"CanIncludeInClipboardHistory", dword(1)}});
  EXPECT_EQ(allowed.second, std::vector<std::string> {text_mime});
}

TEST_P(WindowsClipboardTest, DroppedFilesAreReadAsPaths) {
  const std::vector<std::string> paths {"C:\\a b\\x.txt", "D:\\\xC3\xA9"};
  const auto [revision, formats] = copy_and_wait({{"CF_HDROP", make_drop_files(paths)}, {"Preferred DropEffect", dword(5)}});
  EXPECT_EQ(formats, std::vector<std::string> {files_mime});
  const auto result = read(revision, files_mime);
  EXPECT_EQ(result.result, platf::clipboard_result_e::ok);
  EXPECT_EQ(result.paths, paths);
}

TEST_P(WindowsClipboardTest, OwnershipServesEveryFormatWithoutEcho) {
  const auto png = test_png();
  const std::map<std::string, std::string, std::less<>> content {
    {text_mime, "hi\n"},
    {html_mime, "<i>x</i>"},
    {rtf_mime, "{\\rtf1}"},
    {png_mime, png},
  };
  const auto count = observer.count();
  ASSERT_EQ(backend->take_ownership(initial->first, content, {}), platf::clipboard_result_e::ok);
  EXPECT_EQ(text_from_utf16(paste("CF_UNICODETEXT").value_or("")), "hi\n");
  EXPECT_EQ(html_from_cf_html(paste("HTML Format").value_or("")), "<html>\r\n<body>\r\n<!--StartFragment--><i>x</i><!--EndFragment-->\r\n</body>\r\n</html>");
  EXPECT_EQ(windows_to_canonical(rtf_mime, paste("Rich Text Format").value_or("")), "{\\rtf1}");
  EXPECT_EQ(windows_to_canonical(png_mime, paste("PNG").value_or("")), png);
  const auto bitmaps = clipboard::win::bitmaps_from_png(png);
  ASSERT_TRUE(bitmaps);
  EXPECT_TRUE(paste("CF_DIBV5").value_or("").starts_with(bitmaps->first));
  EXPECT_TRUE(paste("CF_DIB").value_or("").starts_with(bitmaps->second));
  for (const auto *marker : {"ExcludeClipboardContentFromMonitorProcessing", "CanIncludeInClipboardHistory", "CanUploadToCloudClipboard"}) {
    EXPECT_EQ(paste(marker).value_or("").substr(0, 4), dword(0)) << marker;
  }
  expect_no_report(count);
}

TEST_P(WindowsClipboardTest, BitmapsOfAClientImageCompositeOnWhite) {
  ASSERT_EQ(backend->take_ownership(initial->first, {{png_mime, test_png()}}, {}), platf::clipboard_result_e::ok);
  const auto dib = paste("CF_DIB").value_or("");
  ASSERT_GE(dib.size(), sizeof(BITMAPINFOHEADER) + 8);
  BITMAPINFOHEADER info {};
  std::memcpy(&info, dib.data(), sizeof(info));
  EXPECT_EQ(info.biBitCount, 24);
  EXPECT_EQ(info.biWidth, 2);
  EXPECT_EQ(info.biHeight, 1);
  // Red stays red; half-transparent green over white is light green.
  EXPECT_EQ(dib.substr(sizeof(info), 8), std::string("\x00\x00\xFF\x7F\xFF\x7F\x00\x00", 8));
}

TEST_P(WindowsClipboardTest, StagedFilesAreOfferedAsDropFilesAndPaths) {
  const std::vector<std::string> paths {"C:\\staged\\a.txt", "C:\\staged\\dir"};
  ASSERT_EQ(backend->take_ownership(initial->first, {}, paths), platf::clipboard_result_e::ok);
  EXPECT_EQ(paths_from_drop_files(paste("CF_HDROP").value_or("")), paths);
  EXPECT_EQ(paste("Preferred DropEffect").value_or("").substr(0, 4), dword(1));
  EXPECT_EQ(text_from_utf16(paste("CF_UNICODETEXT").value_or("")), "C:\\staged\\a.txt\nC:\\staged\\dir");
  const auto shell_count = as_app([]() {
    UINT count = 0;
    if (open_clipboard(nullptr)) {
      count = DragQueryFileW(static_cast<HDROP>(GetClipboardData(CF_HDROP)), 0xFFFFFFFF, nullptr, 0);
      CloseClipboard();
    }
    return count;
  });
  EXPECT_EQ(shell_count, 2);
}

TEST_P(WindowsClipboardTest, ForeignCopyAfterOwnershipIsReportedAndWins) {
  ASSERT_EQ(backend->take_ownership(initial->first, {{text_mime, "ours"}}, {}), platf::clipboard_result_e::ok);
  const auto [revision, formats] = copy_and_wait({{"CF_UNICODETEXT", utf16_from_text("theirs")}});
  EXPECT_GT(revision, initial->first);
  EXPECT_EQ(formats, std::vector<std::string> {text_mime});
  EXPECT_EQ(backend->take_ownership(initial->first, {{text_mime, "late"}}, {}), platf::clipboard_result_e::stale);
  EXPECT_EQ(read(revision, text_mime).bytes, "theirs");
  EXPECT_EQ(backend->take_ownership(revision, {{text_mime, "next"}}, {}), platf::clipboard_result_e::ok);
  EXPECT_EQ(text_from_utf16(paste("CF_UNICODETEXT").value_or("")), "next");
}

TEST_P(WindowsClipboardTest, ReleaseEmptiesTheClipboardWithoutAReport) {
  ASSERT_EQ(backend->take_ownership(initial->first, {{text_mime, "ours"}}, {}), platf::clipboard_result_e::ok);
  const auto count = observer.count();
  backend->release();
  EXPECT_EQ(format_count(), 0);
  expect_no_report(count);

  const auto foreign = copy_and_wait({{"CF_UNICODETEXT", utf16_from_text("theirs")}});
  backend->release();
  EXPECT_EQ(text_from_utf16(paste("CF_UNICODETEXT").value_or("")), "theirs");
  EXPECT_EQ(read(foreign.first, text_mime).bytes, "theirs");
}

TEST_P(WindowsClipboardTest, StaleRevisionsAreRefused) {
  const auto first = copy_and_wait({{"CF_UNICODETEXT", utf16_from_text("a")}});
  const auto second = copy_and_wait({{"CF_UNICODETEXT", utf16_from_text("b")}});
  EXPECT_EQ(read(first.first, text_mime).result, platf::clipboard_result_e::stale);
  EXPECT_EQ(read(second.first, text_mime).bytes, "b");
  EXPECT_EQ(read(second.first, html_mime).result, platf::clipboard_result_e::stale);
}

TEST_P(WindowsClipboardTest, ReadsEnforceTheSizeLimit) {
  const auto [revision, formats] = copy_and_wait({{"CF_UNICODETEXT", utf16_from_text(std::string(1000, 'x'))}});
  EXPECT_EQ(read(revision, text_mime, 999).result, platf::clipboard_result_e::too_large);
  EXPECT_EQ(read(revision, text_mime, 1000).bytes, std::string(1000, 'x'));
}

TEST_P(WindowsClipboardTest, StopReleasesOwnership) {
  ASSERT_EQ(backend->take_ownership(initial->first, {{text_mime, "ours"}}, {}), platf::clipboard_result_e::ok);
  backend->stop();
  backend.reset();
  EXPECT_EQ(format_count(), 0);
}

TEST_P(WindowsClipboardTest, AnAgentIsStartedOnceAndKept) {
  if (!GetParam()) {
    GTEST_SKIP() << "reads in the backend's process";
  }
  for (const auto *text : {"one", "two"}) {
    const auto [revision, formats] = copy_and_wait({{"CF_UNICODETEXT", utf16_from_text(text)}});
    EXPECT_EQ(read(revision, text_mime).bytes, text);
  }
  EXPECT_EQ(agent_starts->load(), 1);
}

TEST_P(WindowsClipboardTest, ABrokenAgentLeavesTheBackendsOwnView) {
  backend->stop();
  const auto broken_starts = std::make_shared<std::atomic<int>>(0);
  backend = clipboard::win::make_clipboard(desktop_name, thread_agent(broken_starts, false));
  observer_t fresh;
  ASSERT_TRUE(backend->start(fresh));
  ASSERT_TRUE(fresh.wait_after(0));
  EXPECT_EQ(broken_starts->load(), 1);
  const auto count = fresh.count();
  ASSERT_TRUE(copy({{"CF_UNICODETEXT", utf16_from_text("still here")}}));
  const auto report = fresh.wait_after(count);
  ASSERT_TRUE(report);
  EXPECT_EQ(report->second, std::vector<std::string> {text_mime});
  EXPECT_EQ(read(report->first, text_mime).bytes, "still here");
  backend->stop();
  backend.reset();
}

INSTANTIATE_TEST_SUITE_P(Reading, WindowsClipboardTest, testing::Values(false, true), [](const testing::TestParamInfo<bool> &info) {
  return info.param ? "ThroughAnAgent" : "InProcess";
});

TEST(WindowsClipboardImageTest, PngBecomesBitmapsWithAlphaAndOnWhite) {
  const auto bitmaps = clipboard::win::bitmaps_from_png(test_png());
  ASSERT_TRUE(bitmaps);
  BITMAPV5HEADER v5 {};
  std::memcpy(&v5, bitmaps->first.data(), sizeof(v5));
  EXPECT_EQ(v5.bV5Size, sizeof(v5));
  EXPECT_EQ(v5.bV5Width, 2);
  EXPECT_EQ(v5.bV5Height, 1);
  EXPECT_EQ(v5.bV5BitCount, 32);
  EXPECT_EQ(v5.bV5Compression, BI_BITFIELDS);
  EXPECT_EQ(v5.bV5AlphaMask, 0xFF000000);
  EXPECT_EQ(bitmaps->first.substr(sizeof(v5)), std::string("\x00\x00\xFF\xFF\x00\xFF\x00\x80", 8));
  EXPECT_EQ(bitmaps->second.size(), sizeof(BITMAPINFOHEADER) + 8);
}

TEST(WindowsClipboardImageTest, BitmapsOfEveryLayoutBecomePng) {
  const auto pixels_of = [](const std::string &dib) {
    const auto png = clipboard::win::png_from_bitmap(dib);
    const auto bitmaps = png ? clipboard::win::bitmaps_from_png(*png) : std::nullopt;
    return bitmaps ? bitmaps->first.substr(sizeof(BITMAPV5HEADER)) : std::string {};
  };
  // Red above blue, as the bottom-up rows of a CF_DIBV5.
  const std::string red_over_blue("\xFF\x00\x00\xFF\x00\x00\xFF\xFF", 8);
  // Bottom-up 24-bit rows: blue first, then red above it.
  EXPECT_EQ(pixels_of(make_dib(1, 2, 24, std::string("\xFF\x00\x00\x00\x00\x00\xFF\x00", 8))), red_over_blue);
  // Top-down 32-bit rows.
  EXPECT_EQ(pixels_of(make_dib(1, -2, 32, std::string("\x00\x00\xFF\x00\xFF\x00\x00\x00", 8))), red_over_blue);
  // 8-bit palette.
  EXPECT_EQ(pixels_of(make_dib(1, 2, 8, std::string("\x01\x00\x00\x00\x00\x00\x00\x00", 8), std::string("\x00\x00\xFF\x00\xFF\x00\x00\x00", 8))), red_over_blue);
}

TEST(WindowsClipboardImageTest, OversizedAndMalformedImagesAreRefused) {
  EXPECT_FALSE(clipboard::win::png_from_bitmap(make_dib(20000, 1, 32, std::string(80000, '\0'))));
  EXPECT_FALSE(clipboard::win::png_from_bitmap(make_dib(2, 0, 32, "")));
  EXPECT_FALSE(clipboard::win::png_from_bitmap(make_dib(2, 2, 32, "short")));
  EXPECT_FALSE(clipboard::win::png_from_bitmap("tiny"));
  EXPECT_FALSE(clipboard::win::bitmaps_from_png("not a png"));
  EXPECT_FALSE(clipboard::win::bitmaps_from_png(test_png().substr(0, 30)));
}

TEST(WindowsClipboardFactoryTest, PlatformFactoryMakesTheWindowsBackend) {
  const auto backend = platf::make_clipboard();
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(backend->formats().size(), 5);
}
#endif
