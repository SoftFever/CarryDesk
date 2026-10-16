/**
 * @file tests/unit/clipboard/test_clipboard_staging.cpp
 * @brief Test src/clipboard/staging.*
 */

// test includes
#include "../../tests_common.h"

// standard includes
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifndef _WIN32
  #include <sys/stat.h>
  #include <unistd.h>

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
    ASSERT_EQ(::write(fd, content.data(), content.size()), static_cast<ssize_t>(content.size()));
    ::close(fd);
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
      fs::remove_all(base);
      fs::create_directories(base);
      area_path = base / "area";
    }

    /**
     * @brief Remove the scratch directory.
     */
    void TearDown() override {
      std::error_code ec;
      fs::remove_all(base, ec);
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
  struct stat st {};
  ASSERT_EQ(stat(area_path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0700);
  EXPECT_TRUE(area.free_bytes());

  fs::create_directories(base / "real");
  fs::create_symlink(base / "real", base / "link");
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
  struct stat st {};
  ASSERT_EQ(stat(first->path().c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0700);
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
  EXPECT_EQ(paths[0], (root->path() / "Folder").string());
  EXPECT_EQ(paths[1], (root->path() / "folder (1)").string());

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
  fs::create_directory_symlink(base / "precious", root->path() / "d" / "link");
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
  fs::last_write_time(unmarked->path(), fs::file_time_type::clock::now() - 3h);
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
