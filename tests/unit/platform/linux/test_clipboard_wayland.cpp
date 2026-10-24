/**
 * @file tests/unit/platform/linux/test_clipboard_wayland.cpp
 * @brief Test src/platform/linux/clipboard_wayland.* against a private headless KWin.
 * @details The suite starts its own `kwin_wayland --virtual` in a private D-Bus session, so it never
 *          touches the desktop's clipboard. It is skipped when KWin, `dbus-run-session` or
 *          `wl-clipboard` are not installed.
 */
#ifdef SUNSHINE_BUILD_WAYLAND

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <condition_variable>
  #include <csignal>
  #include <cstdio>
  #include <cstdlib>
  #include <fcntl.h>
  #include <filesystem>
  #include <future>
  #include <mutex>
  #include <optional>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>

  // lib includes
  #include <wayland-client.h>

  // local includes
  #include <src/clipboard/protocol.h>
  #include <src/platform/linux/clipboard_wayland.h>

using namespace std::chrono_literals;

namespace {
  const std::string text_mime {clipboard::mime::text};
  const std::string html_mime {clipboard::mime::html};
  const std::string rtf_mime {clipboard::mime::rtf};
  const std::string png_mime {clipboard::mime::png};
  const std::string files_mime {clipboard::mime::files};

  /**
   * @brief Check whether a program is on the PATH.
   *
   * @param name Program name.
   * @return `true` when found.
   */
  bool have_program(const std::string &name) {
    return std::system(("command -v " + name + " > /dev/null 2>&1").c_str()) == 0;
  }

  /**
   * @brief Run a shell command, feeding it bytes on stdin.
   *
   * @param command Shell command.
   * @param input Bytes for stdin.
   * @return Exit status.
   */
  int run_with_input(const std::string &command, const std::string &input) {
    FILE *pipe = popen(command.c_str(), "w");
    if (!pipe) {
      return -1;
    }
    fwrite(input.data(), 1, input.size(), pipe);
    return pclose(pipe);
  }

  /**
   * @brief Run a shell command and capture its stdout.
   *
   * @param command Shell command.
   * @return Output, or no value when the command failed.
   */
  std::optional<std::string> run_output(const std::string &command) {
    FILE *pipe = popen(command.c_str(), "r");
    if (!pipe) {
      return std::nullopt;
    }
    std::string output;
    char buffer[65536];
    while (const auto size = fread(buffer, 1, sizeof(buffer), pipe)) {
      output.append(buffer, size);
    }
    if (pclose(pipe) != 0) {
      return std::nullopt;
    }
    return output;
  }

  /**
   * @brief Paste one type with wl-paste.
   *
   * @param type Native type.
   * @return Pasted bytes, or no value when wl-paste failed.
   */
  std::optional<std::string> paste(const std::string &type) {
    return run_output("wl-paste -n -t '" + type + "' 2>/dev/null");
  }

  /**
   * @brief Observer that records reported changes.
   */
  class recorder_t: public platf::clipboard_observer_t {
  public:
    void on_clipboard_changed(std::uint64_t revision, std::vector<std::string> formats) override {
      std::lock_guard lock {mutex};
      changes.emplace_back(revision, std::move(formats));
      changed.notify_all();
    }

    /**
     * @brief Wait for a number of reported changes.
     *
     * @param count Changes to wait for in total.
     * @param timeout Longest wait.
     * @return The latest change, or no value on timeout.
     */
    std::optional<std::pair<std::uint64_t, std::vector<std::string>>> wait_for(std::size_t count, std::chrono::milliseconds timeout = 5s) {
      std::unique_lock lock {mutex};
      if (!changed.wait_for(lock, timeout, [&]() {
            return changes.size() >= count;
          })) {
        return std::nullopt;
      }
      return changes.back();
    }

    /**
     * @brief Get the number of reported changes.
     *
     * @return Change count.
     */
    std::size_t count() {
      std::lock_guard lock {mutex};
      return changes.size();
    }

  private:
    std::mutex mutex;  ///< Guards `changes`.
    std::condition_variable changed;  ///< Signalled on each change.
    std::vector<std::pair<std::uint64_t, std::vector<std::string>>> changes;  ///< Reported changes.
  };

  /**
   * @brief Check whether a list contains a value.
   *
   * @param list List to search.
   * @param value Value to find.
   * @return `true` when found.
   */
  bool contains(const std::vector<std::string> &list, const std::string &value) {
    return std::ranges::find(list, value) != list.end();
  }

  /**
   * @brief Run the backend against a private headless KWin.
   */
  class WaylandClipboardTest: public testing::Test {
  protected:
    /**
     * @brief Start the private compositor once for the suite.
     */
    static void SetUpTestSuite() {
      const auto *runtime = std::getenv("XDG_RUNTIME_DIR");
      if (!runtime || !have_program("kwin_wayland") || !have_program("dbus-run-session") || !have_program("wl-copy") || !have_program("wl-paste")) {
        return;
      }
      if (const auto *display = std::getenv("WAYLAND_DISPLAY")) {
        original_display = display;
      }
      socket_name = "sunshine-clipboard-test-" + std::to_string(getpid());
      compositor = fork();
      if (compositor == 0) {
        setsid();
        unsetenv("WAYLAND_DISPLAY");
        unsetenv("DISPLAY");
        const auto null_fd = open("/dev/null", O_RDWR);
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        execlp("dbus-run-session", "dbus-run-session", "--", "kwin_wayland", "--virtual", "--no-lockscreen", "--no-global-shortcuts", "--no-kactivities", "--socket", socket_name.c_str(), nullptr);
        _exit(127);
      }
      const auto deadline = std::chrono::steady_clock::now() + 20s;
      while (std::chrono::steady_clock::now() < deadline) {
        if (auto *display = wl_display_connect(socket_name.c_str())) {
          wl_display_disconnect(display);
          ready = true;
          break;
        }
        std::this_thread::sleep_for(100ms);
      }
      if (ready) {
        setenv("WAYLAND_DISPLAY", socket_name.c_str(), 1);
      }
    }

    /**
     * @brief Stop the private compositor and restore the environment.
     */
    static void TearDownTestSuite() {
      if (compositor > 0) {
        kill(-compositor, SIGTERM);
        int status = 0;
        for (int i = 0; i < 50 && waitpid(compositor, &status, WNOHANG) == 0; ++i) {
          std::this_thread::sleep_for(100ms);
        }
        kill(-compositor, SIGKILL);
        waitpid(compositor, &status, WNOHANG);
        compositor = -1;
      }
      if (original_display) {
        setenv("WAYLAND_DISPLAY", original_display->c_str(), 1);
      } else {
        unsetenv("WAYLAND_DISPLAY");
      }
      ready = false;
    }

    /**
     * @brief Start a backend and wait for the initial selection report.
     */
    void SetUp() override {
      if (!ready) {
        GTEST_SKIP() << "A private kwin_wayland with wl-clipboard is not available";
      }
      backend = wl::make_clipboard();
      ASSERT_TRUE(backend->start(recorder));
      ASSERT_TRUE(recorder.wait_for(1));
    }

    /**
     * @brief Stop the backend.
     */
    void TearDown() override {
      if (backend) {
        backend->stop();
      }
    }

    /**
     * @brief Copy bytes with wl-copy and wait for the backend to report it.
     *
     * @param bytes Content.
     * @param type Native type, or empty for wl-copy's default text types.
     * @return The reported change.
     */
    std::pair<std::uint64_t, std::vector<std::string>> copy(const std::string &bytes, const std::string &type = {}) {
      const auto before = recorder.count();
      const auto command = type.empty() ? std::string {"wl-copy"} : "wl-copy -t '" + type + "'";
      EXPECT_EQ(run_with_input(command, bytes), 0);
      const auto change = recorder.wait_for(before + 1);
      EXPECT_TRUE(change);
      return change.value_or(std::pair<std::uint64_t, std::vector<std::string>> {});
    }

    static inline pid_t compositor = -1;  ///< Process group of the private compositor.
    static inline bool ready = false;  ///< The compositor accepts connections.
    static inline std::string socket_name;  ///< Wayland socket of the compositor.
    static inline std::optional<std::string> original_display;  ///< `WAYLAND_DISPLAY` before the suite.

    recorder_t recorder;  ///< Reported changes.
    std::unique_ptr<platf::clipboard_t> backend;  ///< Backend under test.
  };
}  // namespace

TEST_F(WaylandClipboardTest, ForeignTextIsReportedAndReadAsCanonicalText) {
  const auto [revision, formats] = copy("h\xC3\xA9llo\r\nworld\rend");
  EXPECT_TRUE(contains(formats, text_mime));
  const auto read = backend->read(revision, text_mime, 1024);
  EXPECT_EQ(read.result, platf::clipboard_result_e::ok);
  EXPECT_EQ(read.bytes, "h\xC3\xA9llo\nworld\nend");
}

TEST_F(WaylandClipboardTest, RichFormatsAreReportedAndRead) {
  auto change = copy("<b>bold</b>", "text/html");
  EXPECT_TRUE(contains(change.second, html_mime));
  EXPECT_EQ(backend->read(change.first, html_mime, 1024).bytes, "<b>bold</b>");

  change = copy("{\\rtf1 x}", "application/rtf");
  EXPECT_TRUE(contains(change.second, rtf_mime));
  EXPECT_EQ(backend->read(change.first, rtf_mime, 1024).bytes, "{\\rtf1 x}");

  constexpr char png_bytes[] = "\x89PNG\r\n\x1A\n\0\0binary";
  const std::string png {png_bytes, sizeof(png_bytes) - 1};
  change = copy(png, "image/png");
  EXPECT_TRUE(contains(change.second, png_mime));
  EXPECT_EQ(backend->read(change.first, png_mime, 1024).bytes, png);
  EXPECT_EQ(backend->read(change.first, png_mime, 4).result, platf::clipboard_result_e::too_large);
  EXPECT_EQ(backend->read(change.first, text_mime, 1024).result, platf::clipboard_result_e::stale);
}

TEST_F(WaylandClipboardTest, PasswordManagerContentIsNotShared) {
  const auto [revision, formats] = copy("secret", "x-kde-passwordManagerHint");
  EXPECT_TRUE(formats.empty());
}

TEST_F(WaylandClipboardTest, UriListIsFilesOnlyWithLocalFiles) {
  auto change = copy("file:///tmp/a%20b\r\nhttps://example.com/x\r\n", "text/uri-list");
  EXPECT_TRUE(contains(change.second, files_mime));
  const auto read = backend->read(change.first, files_mime, 1 << 20);
  EXPECT_EQ(read.result, platf::clipboard_result_e::ok);
  EXPECT_EQ(read.paths, std::vector<std::string> {"/tmp/a b"});

  change = copy("https://example.com/x\r\n", "text/uri-list");
  EXPECT_FALSE(contains(change.second, files_mime));
}

TEST_F(WaylandClipboardTest, OwnershipServesEveryNativeTypeWithoutEcho) {
  const auto [revision, formats] = copy("before");
  const std::map<std::string, std::string, std::less<>> content {
    {text_mime, "from the client"},
    {html_mime, "<i>client</i>"},
    {rtf_mime, "{\\rtf1 client}"},
    {png_mime, "PNGBYTES"},
  };
  const auto before = recorder.count();
  ASSERT_EQ(backend->take_ownership(revision, content, {}), platf::clipboard_result_e::ok);
  EXPECT_EQ(paste("text/plain"), "from the client");
  EXPECT_EQ(paste("text/plain;charset=utf-8"), "from the client");
  EXPECT_EQ(paste("UTF8_STRING"), "from the client");
  EXPECT_EQ(paste("text/html"), "<i>client</i>");
  EXPECT_EQ(paste("text/rtf"), "{\\rtf1 client}");
  EXPECT_EQ(paste("application/rtf"), "{\\rtf1 client}");
  EXPECT_EQ(paste("image/png"), "PNGBYTES");
  EXPECT_EQ(paste("x-kde-passwordManagerHint"), "secret");
  const auto types = run_output("wl-paste -l");
  ASSERT_TRUE(types);
  EXPECT_EQ(("\n" + *types).find("\nSTRING\n"), std::string::npos);
  EXPECT_EQ(("\n" + *types).find("\nTEXT\n"), std::string::npos);
  EXPECT_NE(("\n" + *types).find("\nx-kde-passwordManagerHint\n"), std::string::npos);
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(recorder.count(), before);
  EXPECT_EQ(backend->read(revision, text_mime, 1024).result, platf::clipboard_result_e::stale);
}

TEST_F(WaylandClipboardTest, StagedFilesAreOfferedAsUriListsAndPaths) {
  const auto [revision, formats] = copy("before");
  ASSERT_EQ(backend->take_ownership(revision, {}, {"/s/a b#%.txt", "/s/d\xC3\xA9"}), platf::clipboard_result_e::ok);
  EXPECT_EQ(paste("text/uri-list"), "file:///s/a%20b%23%25.txt\r\nfile:///s/d%C3%A9\r\n");
  EXPECT_EQ(paste("x-special/gnome-copied-files"), "copy\nfile:///s/a%20b%23%25.txt\nfile:///s/d%C3%A9");
  EXPECT_EQ(paste("text/plain"), "/s/a b#%.txt\n/s/d\xC3\xA9");
}

TEST_F(WaylandClipboardTest, EmptyContentOffersOnlyTheHint) {
  const auto [revision, formats] = copy("before");
  ASSERT_EQ(backend->take_ownership(revision, {}, {}), platf::clipboard_result_e::ok);
  EXPECT_EQ(run_output("wl-paste -l"), "x-kde-passwordManagerHint\n");
}

TEST_F(WaylandClipboardTest, ForeignCopyAfterOwnershipIsReportedAndWins) {
  const auto [revision, formats] = copy("before");
  ASSERT_EQ(backend->take_ownership(revision, {{text_mime, "ours"}}, {}), platf::clipboard_result_e::ok);
  const auto change = copy("theirs");
  EXPECT_GT(change.first, revision);
  EXPECT_EQ(backend->take_ownership(revision, {{text_mime, "late"}}, {}), platf::clipboard_result_e::stale);
  EXPECT_EQ(paste("text/plain"), "theirs");
  ASSERT_EQ(backend->take_ownership(change.first, {{text_mime, "again"}}, {}), platf::clipboard_result_e::ok);
  ASSERT_EQ(backend->take_ownership(change.first, {{text_mime, "replaced"}}, {}), platf::clipboard_result_e::ok);
  EXPECT_EQ(paste("text/plain"), "replaced");
}

TEST_F(WaylandClipboardTest, OwnershipIsReportedOnlyOnceTheCompositorProcessedIt) {
  const auto [revision, formats] = copy("before");
  ASSERT_EQ(kill(-compositor, SIGSTOP), 0);
  auto ownership = std::async(std::launch::async, [&]() {
    return backend->take_ownership(revision, {{text_mime, "confirmed"}}, {});
  });
  EXPECT_EQ(ownership.wait_for(500ms), std::future_status::timeout);
  ASSERT_EQ(kill(-compositor, SIGCONT), 0);
  EXPECT_EQ(ownership.get(), platf::clipboard_result_e::ok);
  EXPECT_EQ(paste("text/plain"), "confirmed");
}

TEST_F(WaylandClipboardTest, UnconfirmedOwnershipFailsAndIsWithdrawn) {
  const auto [revision, formats] = copy("before");
  ASSERT_EQ(kill(-compositor, SIGSTOP), 0);
  const auto result = backend->take_ownership(revision, {{text_mime, "never confirmed"}}, {});
  ASSERT_EQ(kill(-compositor, SIGCONT), 0);
  EXPECT_EQ(result, platf::clipboard_result_e::io_error);
  std::this_thread::sleep_for(300ms);
  EXPECT_NE(paste("text/plain"), "never confirmed");
}

TEST_F(WaylandClipboardTest, ReleaseEmptiesTheClipboardWithoutAReport) {
  const auto [revision, formats] = copy("before");
  ASSERT_EQ(backend->take_ownership(revision, {{text_mime, "ours"}}, {}), platf::clipboard_result_e::ok);
  EXPECT_EQ(paste("text/plain"), "ours");
  const auto before = recorder.count();
  backend->release();
  std::this_thread::sleep_for(300ms);
  EXPECT_FALSE(paste("text/plain"));
  EXPECT_EQ(recorder.count(), before);
  backend->release();
  const auto after = copy("after");
  EXPECT_GT(after.first, revision);
}

TEST_F(WaylandClipboardTest, LargeContentIsWrittenCompletely) {
  const auto [revision, formats] = copy("before");
  const std::string large(5 * 1048576, 'L');
  ASSERT_EQ(backend->take_ownership(revision, {{text_mime, large}}, {}), platf::clipboard_result_e::ok);
  const auto pasted = paste("text/plain");
  ASSERT_TRUE(pasted);
  EXPECT_EQ(pasted->size(), large.size());
}

TEST_F(WaylandClipboardTest, DataControlIsDetected) {
  EXPECT_TRUE(wl::offers_data_control());
}

TEST_F(WaylandClipboardTest, StartFailsWithoutACompositor) {
  setenv("WAYLAND_DISPLAY", "sunshine-clipboard-missing-socket", 1);
  EXPECT_FALSE(wl::offers_data_control());
  auto missing = wl::make_clipboard();
  recorder_t other;
  EXPECT_FALSE(missing->start(other));
  missing->stop();
  setenv("WAYLAND_DISPLAY", socket_name.c_str(), 1);
}

TEST(WaylandClipboardFactoryTest, NoBackendWithoutAWaylandDisplay) {
  const auto *display = std::getenv("WAYLAND_DISPLAY");
  const std::optional<std::string> saved = display ? std::optional<std::string> {display} : std::nullopt;
  unsetenv("WAYLAND_DISPLAY");
  EXPECT_EQ(platf::make_clipboard(), nullptr);
  setenv("WAYLAND_DISPLAY", "x", 1);
  EXPECT_NE(platf::make_clipboard(), nullptr);
  if (saved) {
    setenv("WAYLAND_DISPLAY", saved->c_str(), 1);
  } else {
    unsetenv("WAYLAND_DISPLAY");
  }
}

TEST(WaylandDrainPipeTest, ReadsUntilEofWithinTheLimit) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  ASSERT_EQ(write(fds[1], "abc", 3), 3);
  close(fds[1]);
  std::string bytes;
  EXPECT_EQ(wl::drain_pipe(fds[0], 3, 1s, bytes), platf::clipboard_result_e::ok);
  EXPECT_EQ(bytes, "abc");
  close(fds[0]);
}

TEST(WaylandDrainPipeTest, StopsBeyondTheLimit) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  ASSERT_EQ(write(fds[1], "abcd", 4), 4);
  close(fds[1]);
  std::string bytes;
  EXPECT_EQ(wl::drain_pipe(fds[0], 3, 1s, bytes), platf::clipboard_result_e::too_large);
  close(fds[0]);
}

TEST(WaylandDrainPipeTest, GivesUpOnAStalledWriter) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  ASSERT_EQ(write(fds[1], "a", 1), 1);
  std::string bytes;
  EXPECT_EQ(wl::drain_pipe(fds[0], 10, 50ms, bytes), platf::clipboard_result_e::io_error);
  EXPECT_EQ(bytes, "a");
  close(fds[0]);
  close(fds[1]);
}

TEST(WaylandDrainPipeTest, ReportsReadErrors) {
  std::string bytes;
  EXPECT_EQ(wl::drain_pipe(-1, 10, 50ms, bytes), platf::clipboard_result_e::io_error);
}
#endif
