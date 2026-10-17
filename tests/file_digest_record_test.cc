#include "compat/file_digest_record.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace mocktail::compat {
namespace {

class FileDigestRecordTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("mocktail-file-digest-" + std::to_string(getpid()) + "-" +
             testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
    WriteFile("first");
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  void WriteFile(const std::string& contents) {
    std::ofstream(File(), std::ios::binary | std::ios::trunc) << contents;
  }
  std::filesystem::path File() const { return root_ / "libroblox.so"; }
  std::filesystem::path Records() const { return root_ / "records"; }

  std::string Digest(const std::string& value) {
    return RememberedFileDigest(Records(), File(), [&] {
      ++hash_calls_;
      return value;
    });
  }

  std::filesystem::path root_;
  int hash_calls_ = 0;
};

TEST_F(FileDigestRecordTest, ReusesDigestOfUnchangedFile) {
  EXPECT_EQ(Digest("aaaa"), "aaaa");
  EXPECT_EQ(Digest("bbbb"), "aaaa");
  EXPECT_EQ(hash_calls_, 1);
}

TEST_F(FileDigestRecordTest, RehashesAfterContentChanges) {
  EXPECT_EQ(Digest("aaaa"), "aaaa");
  WriteFile("second, longer");
  EXPECT_EQ(Digest("bbbb"), "bbbb");
  EXPECT_EQ(hash_calls_, 2);
}

TEST_F(FileDigestRecordTest, RehashesSameSizeRewriteWithRestoredMtime) {
  struct stat before = {};
  ASSERT_EQ(stat(File().c_str(), &before), 0);
  EXPECT_EQ(Digest("aaaa"), "aaaa");
  usleep(20'000);
  WriteFile("FIRST");
  const struct timespec times[2] = {before.st_atim, before.st_mtim};
  ASSERT_EQ(utimensat(AT_FDCWD, File().c_str(), times, 0), 0);

  EXPECT_EQ(Digest("bbbb"), "bbbb");
  EXPECT_EQ(hash_calls_, 2);
}

TEST_F(FileDigestRecordTest, RehashesReplacedFile) {
  EXPECT_EQ(Digest("aaaa"), "aaaa");
  const std::filesystem::path replacement = root_ / "replacement";
  std::ofstream(replacement) << "first";
  std::filesystem::rename(replacement, File());
  EXPECT_EQ(Digest("bbbb"), "bbbb");
  EXPECT_EQ(hash_calls_, 2);
}

TEST_F(FileDigestRecordTest, DoesNotRecordAFailedHash) {
  EXPECT_EQ(Digest(""), "");
  EXPECT_EQ(Digest("aaaa"), "aaaa");
  EXPECT_EQ(hash_calls_, 2);
}

TEST_F(FileDigestRecordTest, EmptyRecordDirectoryAlwaysHashes) {
  const auto digest = [&] {
    return RememberedFileDigest({}, File(), [&] {
      ++hash_calls_;
      return std::string("aaaa");
    });
  };
  EXPECT_EQ(digest(), "aaaa");
  EXPECT_EQ(digest(), "aaaa");
  EXPECT_EQ(hash_calls_, 2);
}

TEST_F(FileDigestRecordTest, RehashesWhenRecordIsCorrupt) {
  EXPECT_EQ(Digest("aaaa"), "aaaa");
  for (const auto& entry : std::filesystem::directory_iterator(Records())) {
    std::ofstream(entry.path(), std::ios::trunc) << "garbage";
  }
  EXPECT_EQ(Digest("bbbb"), "bbbb");
  EXPECT_EQ(hash_calls_, 2);
}

TEST_F(FileDigestRecordTest, AlwaysHashesThroughASymlink) {
  const std::filesystem::path link = root_ / "link.so";
  std::filesystem::create_symlink(File(), link);
  EXPECT_EQ(Digest("aaaa"), "aaaa");
  const auto digest = [&] {
    return RememberedFileDigest(Records(), link, [&] {
      ++hash_calls_;
      return std::string("bbbb");
    });
  };
  EXPECT_EQ(digest(), "bbbb");
  EXPECT_EQ(digest(), "bbbb");
  EXPECT_EQ(hash_calls_, 3);
}

TEST_F(FileDigestRecordTest, IgnoresRecordsInASharedDirectory) {
  EXPECT_EQ(Digest("aaaa"), "aaaa");
  ASSERT_EQ(chmod(Records().c_str(), 0777), 0);
  EXPECT_EQ(Digest("bbbb"), "bbbb");
  EXPECT_EQ(hash_calls_, 2);
}

TEST_F(FileDigestRecordTest, KeepsRecordDirectoryPrivate) {
  EXPECT_EQ(Digest("aaaa"), "aaaa");
  struct stat metadata = {};
  ASSERT_EQ(stat(Records().c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0700U);
}

}
}
