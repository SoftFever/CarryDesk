/**
 * @file tests/unit/clipboard/test_clipboard_staging.cpp
 * @brief Test src/clipboard/staging.*
 */

// test includes
#include "../../tests_common.h"

// standard includes
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
  #include <io.h>
  // platform includes
  #include <aclapi.h>
  #include <Windows.h>
#else
  #include <sys/stat.h>
  #include <unistd.h>
#endif

// local includes
#include <src/clipboard/staging.h>

using namespace clipboard;
using namespace clipboard::staging;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
  /**
   * @brief Read a whole file.
   *
   * @param path File path.
   * @return Content.
   */
  std::string read_file(const fs::path &path) {
    std::ifstream stream {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {stream}, {}};
  }

  /**
   * @brief Write bytes to a descriptor and close it.
   *
   * @param fd Descriptor.
   * @param content Bytes.
   */
  void write_fd(int fd, const std::string &content) {
    ASSERT_GE(fd, 0);
#ifdef _WIN32
    ASSERT_EQ(_write(fd, content.data(), static_cast<unsigned int>(content.size())), static_cast<int>(content.size()));
    _close(fd);
#else
    ASSERT_EQ(::write(fd, content.data(), content.size()), static_cast<ssize_t>(content.size()));
    ::close(fd);
#endif
  }

  /**
   * @brief Delete a scratch tree without following the links in it.
   *
   * @param path Tree to delete.
   */
  void remove_scratch(const fs::path &path) {
#ifdef _WIN32
    // std::filesystem trips over junctions; rmdir deletes them without following them.
    const auto native = fs::path {path}.make_preferred().string();
    std::system(("cmd /c if exist \"" + native + "\" rmdir /s /q \"" + native + "\"").c_str());
#else
    std::error_code ec;
    fs::remove_all(path, ec);
#endif
  }

  /**
   * @brief Create a link to a directory.
   *
   * @param link Path of the link.
   * @param target Directory it points to.
   * @return `true` when it was created.
   */
  bool link_directory(const fs::path &link, const fs::path &target) {
#ifdef _WIN32
    // A junction needs no privilege, unlike a symbolic link.
    const auto command = "cmd /c mklink /J \"" + fs::path {link}.make_preferred().string() + "\" \"" + fs::path {target}.make_preferred().string() + "\" > nul";
    return std::system(command.c_str()) == 0 && fs::exists(link);
#else
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    return !ec;
#endif
  }

  /**
   * @brief Check that only this user may access a directory.
   *
   * @param path Directory.
   */
  void expect_private(const fs::path &path) {
#ifdef _WIN32
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ASSERT_EQ(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor), ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    GetSecurityDescriptorControl(descriptor, &control, &revision);
    EXPECT_TRUE(control & SE_DACL_PROTECTED);
    ASSERT_TRUE(dacl);
    EXPECT_EQ(dacl->AceCount, 2);
    HANDLE token = nullptr;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    std::array<std::uint8_t, 256> user {};
    DWORD size = 0;
    ASSERT_TRUE(GetTokenInformation(token, TokenUser, user.data(), user.size(), &size));
    CloseHandle(token);
    const auto user_sid = reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
      void *ace = nullptr;
      ASSERT_TRUE(GetAce(dacl, i, &ace));
      const auto *allowed = static_cast<ACCESS_ALLOWED_ACE *>(ace);
      EXPECT_EQ(allowed->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
      EXPECT_EQ(allowed->Mask, FILE_ALL_ACCESS);
      const auto sid = const_cast<DWORD *>(&allowed->SidStart);
      EXPECT_TRUE(EqualSid(sid, user_sid) || IsWellKnownSid(sid, WinLocalSystemSid));
    }
    LocalFree(descriptor);
#else
    struct stat st {};
    ASSERT_EQ(stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0700);
#endif
  }

  /**
   * @brief Staging area in a scratch directory.
   */
  class ClipboardStagingTest: public testing::Test {
  protected:
    /**
     * @brief Create the scratch directory.
     */
    void SetUp() override {
      base = fs::path {SUNSHINE_TEST_BIN_DIR} / "clipboard_staging";
      remove_scratch(base);
      fs::create_directories(base);
      area_path = base / "area";
    }

    /**
     * @brief Remove the scratch directory.
     */
    void TearDown() override {
      remove_scratch(base);
    }

    /**
     * @brief Count the area's entries.
     *
     * @return Entry count.
     */
    std::size_t area_entries() const {
      return static_cast<std::size_t>(std::distance(fs::directory_iterator {area_path}, fs::directory_iterator {}));
    }

    fs::path base;  ///< Scratch directory.
    fs::path area_path;  ///< Staging area inside it.
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();  ///< Reference time.
  };
}  // namespace

TEST_F(ClipboardStagingTest, AreaIsAPrivateDirectory) {
  area_t area {area_path};
  ASSERT_TRUE(area.open());
  ASSERT_TRUE(area.open());
  EXPECT_EQ(area.path(), area_path);
  expect_private(area_path);
  EXPECT_TRUE(area.free_bytes());

  fs::create_directories(base / "real");
  ASSERT_TRUE(link_directory(base / "link", base / "real"));
  area_t linked {base / "link"};
  EXPECT_FALSE(linked.open());
  EXPECT_FALSE(linked.free_bytes());
  EXPECT_FALSE(linked.create_root());
  linked.reap(now);
}

TEST_F(ClipboardStagingTest, RootsArePrivateAndRandom) {
  area_t area {area_path};
  ASSERT_TRUE(area.open());
  const auto first = area.create_root();
  const auto second = area.create_root();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first->path(), second->path());
  EXPECT_EQ(first->path().parent_path(), area_path);
  EXPECT_EQ(first->path().filename().string().size(), 32);
  expect_private(first->path());
}

TEST_F(ClipboardStagingTest, PrepareCreatesDirectoriesAndRenamesCollisions) {
  area_t area {area_path};
  ASSERT_TRUE(area.open());
  const auto root = area.create_root();
  const std::vector<file_entry_t> entries {
    {0, "Folder", 0, 0, true},
    {1, "Folder/a.txt", 3, 0, false},
    {2, "folder", 0, 0, true},
    {3, "folder/A.TXT", 2, 0, false},
    {4, "x y#%\xC3\xA9.txt", 1, 0, false},
    {5, ".released", 1, 0, false},
    {6, std::string(32, 'a') + ".released", 1, 0, false},
  };
  ASSERT_FALSE(root->prepare(entries));
  EXPECT_EQ(root->final_path(2), "folder (1)");
  EXPECT_EQ(root->final_path(3), "folder (1)/A.TXT");
  write_fd(root->create_file(1), "one");
  write_fd(root->create_file(3), "two");
  write_fd(root->create_file(4), "3");
  write_fd(root->create_file(5), "4");
  write_fd(root->create_file(6), "5");
  EXPECT_EQ(root->create_file(1), -1);
  EXPECT_EQ(root->create_file(0), -1);
  EXPECT_EQ(root->create_file(99), -1);
  EXPECT_EQ(read_file(root->path() / "Folder" / "a.txt"), "one");
  EXPECT_EQ(read_file(root->path() / "folder (1)" / "A.TXT"), "two");
  EXPECT_EQ(read_file(root->path() / ("x y#%\xC3\xA9.txt")), "3");

  const auto paths = root->top_level_paths();
  ASSERT_EQ(paths.size(), 5);
  EXPECT_EQ(fs::path {paths[0]}, root->path() / "Folder");
  EXPECT_EQ(fs::path {paths[1]}, root->path() / "folder (1)");

  area.reap(now + 2h);
  EXPECT_TRUE(fs::exists(root->path()));
}

TEST_F(ClipboardStagingTest, PrepareRefusesBadManifests) {
  area_t area {area_path};
  ASSERT_TRUE(area.open());
  EXPECT_EQ(area.create_root()->prepare({{0, "missing/child", 1, 0, false}}), error_e::bad_request);
  EXPECT_EQ(area.create_root()->prepare({{0, "../escape", 1, 0, false}}), error_e::bad_request);
  EXPECT_EQ(area.create_root()->prepare({{0, "a/b", 0, 0, true}}), error_e::bad_request);

  const auto root = area.create_root();
  fs::create_directories(root->path() / "taken");
  EXPECT_EQ(root->prepare({{0, "taken", 0, 0, true}}), error_e::io_error);
}

TEST_F(ClipboardStagingTest, RemoveNeverFollowsLinks) {
  area_t area {area_path};
  ASSERT_TRUE(area.open());
  const auto root = area.create_root();
  ASSERT_FALSE(root->prepare({{0, "d", 0, 0, true}, {1, "d/f", 1, 0, false}}));
  write_fd(root->create_file(1), "f");
  fs::create_directories(base / "precious");
  std::ofstream {base / "precious" / "keep"} << "keep";
  ASSERT_TRUE(link_directory(root->path() / "d" / "link", base / "precious"));
  root->remove();
  root->remove();
  EXPECT_FALSE(fs::exists(root->path()));
  EXPECT_EQ(read_file(base / "precious" / "keep"), "keep");
}

TEST_F(ClipboardStagingTest, ReleasedRootsAreDeletedAfterAnHour) {
  area_t area {area_path};
  ASSERT_TRUE(area.open());
  const auto root = area.create_root();
  const auto kept = area.create_root();
  area.release(*root, now);
  const auto marker = area_path / (root->path().filename().string() + ".released");
  ASSERT_TRUE(fs::exists(marker));

  area.reap(now + 59min);
  EXPECT_TRUE(fs::exists(root->path()));
  area.reap(now + 61min);
  EXPECT_FALSE(fs::exists(root->path()));
  EXPECT_FALSE(fs::exists(marker));
  EXPECT_TRUE(fs::exists(kept->path()));
}

TEST_F(ClipboardStagingTest, SweepAdoptsLeftoversOfAPreviousRun) {
  area_t first {area_path};
  ASSERT_TRUE(first.open());
  const auto unmarked = first.create_root();
  std::error_code ec;
  fs::last_write_time(unmarked->path(), fs::file_time_type::clock::now() - 3h, ec);
  const auto old = first.create_root();
  first.release(*old, now - 2h);
  const auto garbled = first.create_root();
  std::ofstream {area_path / (garbled->path().filename().string() + ".released")} << "not a number";
  std::ofstream {area_path / (std::string(32, 'b') + ".released")} << "1";
  std::ofstream {area_path / "unrelated.txt"} << "x";

  area_t second {area_path};
  ASSERT_TRUE(second.open());
  second.sweep(now);
  EXPECT_TRUE(fs::exists(unmarked->path()));
  EXPECT_TRUE(fs::exists(area_path / (unmarked->path().filename().string() + ".released")));
  EXPECT_FALSE(fs::exists(old->path()));
  EXPECT_TRUE(fs::exists(garbled->path()));
  EXPECT_FALSE(fs::exists(area_path / (std::string(32, 'b') + ".released")));
  EXPECT_TRUE(fs::exists(area_path / "unrelated.txt"));

  second.reap(now + 61min);
  EXPECT_FALSE(fs::exists(unmarked->path()));
  EXPECT_FALSE(fs::exists(garbled->path()));
  EXPECT_EQ(area_entries(), 1);
}

TEST_F(ClipboardStagingTest, StrayFilesAreNeverTreatedAsRoots) {
  area_t area {area_path};
  ASSERT_TRUE(area.open());
  std::ofstream {area_path / ".DS_Store"} << "x";
  std::ofstream {area_path / std::string(32, 'd')} << "file named like a root";
  fs::create_directories(area_path / "not-a-root");
  area.sweep(now);
  area.reap(now + 2h);
  EXPECT_TRUE(fs::exists(area_path / ".DS_Store"));
  EXPECT_TRUE(fs::exists(area_path / std::string(32, 'd')));
  EXPECT_TRUE(fs::exists(area_path / "not-a-root"));
  EXPECT_FALSE(fs::exists(area_path / ".DS_Store.released"));
  EXPECT_FALSE(fs::exists(area_path / (std::string(32, 'd') + ".released")));
  EXPECT_FALSE(fs::exists(area_path / "not-a-root.released"));
  EXPECT_EQ(area_entries(), 3);
}

#ifdef _WIN32
TEST_F(ClipboardStagingTest, NamesWindowsCannotStoreAreReplaced) {
  area_t area {area_path};
  ASSERT_TRUE(area.open());
  const auto root = area.create_root();
  ASSERT_FALSE(root->prepare({{0, "what?.txt", 1, 0, false}, {1, "a<b>|\"*", 1, 0, false}, {2, "what*.txt", 1, 0, false}}));
  EXPECT_EQ(root->final_path(0), "what_.txt");
  EXPECT_EQ(root->final_path(1), "a_b____");
  EXPECT_EQ(root->final_path(2), "what_ (1).txt");
  write_fd(root->create_file(0), "q");
  EXPECT_EQ(read_file(root->path() / "what_.txt"), "q");
}

TEST(ClipboardStagingAreaTest, DefaultAreaIsFoundWhenOpened) {
  EXPECT_TRUE(default_area().empty());
}
#else
TEST(ClipboardStagingAreaTest, DefaultAreaFollowsTheCacheDirectory) {
  const auto *cache = std::getenv("XDG_CACHE_HOME");
  const std::optional<std::string> saved = cache ? std::optional<std::string> {cache} : std::nullopt;
  setenv("XDG_CACHE_HOME", "/custom/cache", 1);
  EXPECT_EQ(default_area(), fs::path {"/custom/cache/sunshine/clipboard"});
  setenv("XDG_CACHE_HOME", "relative", 1);
  EXPECT_EQ(default_area(), fs::path {std::getenv("HOME")} / ".cache" / "sunshine" / "clipboard");
  if (saved) {
    setenv("XDG_CACHE_HOME", saved->c_str(), 1);
  } else {
    unsetenv("XDG_CACHE_HOME");
  }
}
#endif
