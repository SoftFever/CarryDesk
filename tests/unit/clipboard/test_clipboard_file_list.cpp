/**
 * @file tests/unit/clipboard/test_clipboard_file_list.cpp
 * @brief Test src/clipboard/file_list.*
 */

// test includes
#include "../../tests_common.h"

// standard includes
#include <filesystem>
#include <fstream>

#ifndef _WIN32
  #include <sys/stat.h>
  #include <unistd.h>
#endif

// local includes
#include <src/clipboard/file_list.h>

using namespace clipboard;
using namespace clipboard::files;
namespace fs = std::filesystem;

namespace {
  /**
   * @brief Write a file with content.
   *
   * @param path File path.
   * @param content File content.
   */
  void write_file(const fs::path &path, const std::string &content) {
    std::ofstream {path, std::ios::binary} << content;
  }

  /**
   * @brief Read everything from a descriptor.
   *
   * @param fd Descriptor; closed afterwards.
   * @return Content.
   */
  std::string read_fd(int fd) {
    std::string content;
#ifndef _WIN32
    char buffer[4096];
    while (true) {
      const auto size = ::read(fd, buffer, sizeof(buffer));
      if (size <= 0) {
        break;
      }
      content.append(buffer, static_cast<std::size_t>(size));
    }
    ::close(fd);
#endif
    return content;
  }

  /**
   * @brief Temporary directory removed after each test.
   */
  class ClipboardFileListTest: public testing::Test {
  protected:
    /**
     * @brief Create the directory.
     */
    void SetUp() override {
      dir = fs::path {SUNSHINE_TEST_BIN_DIR} / "clipboard_file_list";
      fs::remove_all(dir);
      fs::create_directories(dir);
    }

    /**
     * @brief Remove the directory.
     */
    void TearDown() override {
      std::error_code ec;
      fs::remove_all(dir, ec);
    }

    fs::path dir;  ///< Scratch directory.
  };
}  // namespace

TEST(ClipboardPathRulesTest, AcceptsRelativeUtf8Paths) {
  for (const auto *path : {"a", "a/b/c.txt", "\xC3\xA9t\xC3\xA9/x y", ".hidden", "a..b", "x.released", "com10", "conx.txt"}) {
    EXPECT_FALSE(check_received_path(path)) << path;
  }
}

TEST(ClipboardPathRulesTest, RefusesUnsafePaths) {
  std::string deep;
  for (int i = 0; i < 65; ++i) {
    deep += (i ? "/" : "") + std::string {"d"};
  }
  for (const auto &path : std::vector<std::string> {
         "",
         "/abs",
         "a//b",
         "a/",
         "//server/share",
         "./a",
         "a/./b",
         "a/../b",
         "..",
         "a\\b",
         "C:x",
         "con",
         "CON.txt",
         "lpt9.log",
         "Aux",
         "a.",
         "a ",
         std::string {"a\x01"},
         std::string {"x\x7F"},
         std::string(256, 'a'),
         std::string(2049, 'a') + "/" + std::string(2049, 'b'),
         deep,
         "\xFF",
       }) {
    EXPECT_TRUE(check_received_path(path)) << testing::PrintToString(path);
  }
}

TEST(ClipboardCollisionTest, KeysFoldCaseAndNormaliseToNfc) {
  EXPECT_EQ(collision_key("ABC.txt"), collision_key("abc.TXT"));
  EXPECT_EQ(collision_key("e\xCC\x81"), collision_key("\xC3\xA9"));
  EXPECT_EQ(collision_key("\xC3\x89"), collision_key("\xC3\xA9"));
  EXPECT_NE(collision_key("a"), collision_key("b"));
}

TEST(ClipboardCollisionTest, CollidingNamesAreRenamed) {
  unique_names_t names;
  EXPECT_EQ(names.claim("", "a.txt", false), "a.txt");
  EXPECT_EQ(names.claim("", "A.TXT", false), "A (1).TXT");
  EXPECT_EQ(names.claim("", "a.txt", false), "a (2).txt");
  EXPECT_EQ(names.claim("", "dir.d", true), "dir.d");
  EXPECT_EQ(names.claim("", "DIR.D", true), "DIR.D (1)");
  EXPECT_EQ(names.claim("other", "a.txt", false), "a.txt");
  EXPECT_EQ(names.claim("", ".bashrc", false), ".bashrc");
  EXPECT_EQ(names.claim("", ".BASHRC", false), ".BASHRC (1)");
  EXPECT_EQ(names.claim("", "e\xCC\x81", false), "e\xCC\x81");
  EXPECT_EQ(names.claim("", "\xC3\xA9", false), "\xC3\xA9 (1)");

  const std::string long_name = std::string(250, 'x') + "\xC3\xA9.txt";
  EXPECT_EQ(names.claim("", long_name, false), long_name);
  const auto renamed = names.claim("", long_name, false);
  EXPECT_LE(renamed.size(), 255);
  EXPECT_TRUE(renamed.ends_with(" (1).txt"));
  EXPECT_FALSE(check_received_path(renamed));
}

TEST(ClipboardManifestParseTest, ParsesOrderedLines) {
  const auto entries = parse_manifest(serialize(file_entry_t {0, "d", 0, 1, true}) + serialize(file_entry_t {1, "d/f", 3, 2, false}));
  ASSERT_TRUE(entries);
  ASSERT_EQ(entries->size(), 2);
  EXPECT_EQ((*entries)[1].path, "d/f");
  EXPECT_TRUE(parse_manifest("")->empty());
}

TEST(ClipboardManifestParseTest, RefusesMalformedManifests) {
  const auto line = serialize(file_entry_t {0, "f", 3, 2, false});
  EXPECT_FALSE(parse_manifest(line.substr(0, line.size() - 1)));
  EXPECT_FALSE(parse_manifest(serialize(file_entry_t {1, "f", 3, 2, false})));
  EXPECT_FALSE(parse_manifest(serialize(file_entry_t {0, "d", 5, 2, true})));
  EXPECT_FALSE(parse_manifest("not json\n"));
}

#ifndef _WIN32
TEST_F(ClipboardFileListTest, ManifestWalksDirectoriesAndSkipsLinksAndSpecialFiles) {
  fs::create_directories(dir / "tree" / "sub");
  write_file(dir / "tree" / "a.txt", "hello");
  write_file(dir / "tree" / "sub" / "b.bin", std::string(1000, 'b'));
  write_file(dir / "outside.txt", "outside");
  fs::create_symlink(dir / "outside.txt", dir / "tree" / "link");
  ASSERT_EQ(mkfifo((dir / "tree" / "fifo").c_str(), 0600), 0);
  write_file(dir / "tree" / "bad:name", "x");
  write_file(dir / "tree" / "bad\xFFname", "x");
  write_file(dir / "solo.txt", "solo");

  const auto result = build_manifest({(dir / "tree").string() + "/", (dir / "solo.txt").string(), (dir / "missing").string(), "/"}, 1 << 20);
  ASSERT_FALSE(result.error);
  ASSERT_TRUE(result.manifest);
  const auto &entries = result.manifest->entries;
  ASSERT_EQ(entries.size(), 5);
  EXPECT_EQ(entries[0], (file_entry_t {0, "tree", 0, entries[0].mtime, true}));
  EXPECT_EQ(entries[1].path, "tree/a.txt");
  EXPECT_EQ(entries[1].size, 5);
  EXPECT_EQ(entries[2].path, "tree/sub");
  EXPECT_TRUE(entries[2].dir);
  EXPECT_EQ(entries[3].path, "tree/sub/b.bin");
  EXPECT_EQ(entries[4].path, "solo.txt");
  EXPECT_EQ(result.manifest->total_bytes, 1009);
  EXPECT_EQ(result.skipped, 6);
  EXPECT_GT(entries[1].mtime, 0);

  EXPECT_EQ(read_fd(open_entry(*result.manifest, 1)), "hello");
  EXPECT_EQ(read_fd(open_entry(*result.manifest, 4)), "solo");
  EXPECT_EQ(open_entry(*result.manifest, 0), -1);
  EXPECT_EQ(open_entry(*result.manifest, 9), -1);
}

TEST_F(ClipboardFileListTest, ReplacedEntriesAreRefused) {
  fs::create_directories(dir / "tree" / "sub");
  write_file(dir / "tree" / "a.txt", "original");
  write_file(dir / "tree" / "sub" / "b.txt", "nested");
  write_file(dir / "other.txt", "secret");
  const auto result = build_manifest({(dir / "tree").string()}, 1 << 20);
  ASSERT_TRUE(result.manifest);
  ASSERT_EQ(result.manifest->entries.size(), 4);

  fs::rename(dir / "other.txt", dir / "tree" / "a.txt");
  EXPECT_EQ(open_entry(*result.manifest, 1), -1);

  fs::rename(dir / "tree" / "sub", dir / "moved");
  fs::create_symlink(dir / "moved", dir / "tree" / "sub");
  EXPECT_EQ(open_entry(*result.manifest, 3), -1);
}

TEST_F(ClipboardFileListTest, ManifestLimitsAreEnforced) {
  write_file(dir / "big.bin", std::string(100, 'x'));
  auto result = build_manifest({(dir / "big.bin").string()}, 99);
  EXPECT_EQ(result.error, error_e::too_large);
  EXPECT_FALSE(result.manifest);
  result = build_manifest({(dir / "big.bin").string()}, 100);
  EXPECT_FALSE(result.error);

  auto deep = dir / "deep";
  for (int i = 0; i < 65; ++i) {
    deep /= "d";
  }
  fs::create_directories(deep);
  result = build_manifest({(dir / "deep").string()}, 1 << 20);
  EXPECT_EQ(result.error, error_e::too_large);
}

TEST_F(ClipboardFileListTest, NothingReadableIsAnError) {
  const auto result = build_manifest({(dir / "missing").string()}, 1 << 20);
  EXPECT_EQ(result.error, error_e::io_error);
  EXPECT_FALSE(build_manifest({}, 1 << 20).error);
}
#endif
