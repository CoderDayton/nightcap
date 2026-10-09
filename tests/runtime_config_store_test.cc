#include "runtime/runtime_config_store.h"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "runtime/runtime_config_bootstrap.h"
#include "runtime/runtime_config_editor.h"

namespace mocktail {
namespace runtime {
namespace {

class RuntimeConfigStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mocktail_config_store_XXXXXX";
    const char* created = mkdtemp(pattern);
    ASSERT_NE(created, nullptr);
    directory_ = created;
    file_ = directory_ / "config.yaml";
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  void Write(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream(path, std::ios::binary) << contents;
  }

  std::string Read(const std::filesystem::path& path) {
    std::ostringstream contents;
    contents << std::ifstream(path, std::ios::binary).rdbuf();
    return contents.str();
  }

  mode_t Mode(const std::filesystem::path& path) {
    struct stat metadata = {};
    return stat(path.c_str(), &metadata) == 0 ? metadata.st_mode & 0777 : 0;
  }

  std::size_t EntryCount() {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
      (void)entry;
      ++count;
    }
    return count;
  }

  std::filesystem::path directory_;
  std::filesystem::path file_;
};

constexpr std::string_view kBefore = "graphics:\n  backend: direct-vulkan\n";
constexpr std::string_view kAfter = "graphics:\n  backend: opengl\n";

TEST_F(RuntimeConfigStoreTest, ReadsTheFile) {
  Write(file_, kBefore);
  const RuntimeConfigText read = ReadRuntimeConfigText(file_);
  ASSERT_TRUE(read) << read.error;
  EXPECT_EQ(read.text, kBefore);
}

TEST_F(RuntimeConfigStoreTest, ReadRefusesAMissingFileAndASymlink) {
  EXPECT_FALSE(ReadRuntimeConfigText(file_));

  const std::filesystem::path target = directory_ / "target.yaml";
  Write(target, kBefore);
  std::filesystem::create_symlink(target, file_);
  EXPECT_FALSE(ReadRuntimeConfigText(file_));
}

TEST_F(RuntimeConfigStoreTest, SavesTextThatLoads) {
  Write(file_, kBefore);
  const RuntimeConfigSaveResult saved =
      SaveRuntimeConfigText(file_, kBefore, kAfter);
  ASSERT_TRUE(saved) << saved.error;
  EXPECT_EQ(Read(file_), kAfter);
  EXPECT_EQ(EntryCount(), 1u);
}

TEST_F(RuntimeConfigStoreTest, SavedFileIsPrivateToTheUser) {
  Write(file_, kBefore);
  ASSERT_EQ(chmod(file_.c_str(), 0644), 0);
  ASSERT_TRUE(SaveRuntimeConfigText(file_, kBefore, kAfter));
  EXPECT_EQ(Mode(file_), 0600u);
}

TEST_F(RuntimeConfigStoreTest, AValueTheLoaderRejectsLeavesTheFileUntouched) {
  Write(file_, kBefore);
  const RuntimeConfigSaveResult saved = SaveRuntimeConfigText(
      file_, kBefore, "graphics:\n  backend: no-such-backend\n");
  EXPECT_FALSE(saved);
  EXPECT_NE(saved.error.find("graphics.backend"), std::string::npos)
      << saved.error;
  EXPECT_EQ(Read(file_), kBefore);
  EXPECT_EQ(EntryCount(), 1u);
}

TEST_F(RuntimeConfigStoreTest, BrokenYamlLeavesTheFileUntouched) {
  Write(file_, kBefore);
  EXPECT_FALSE(SaveRuntimeConfigText(file_, kBefore, "graphics: [1, 2\n"));
  EXPECT_EQ(Read(file_), kBefore);
  EXPECT_EQ(EntryCount(), 1u);
}

TEST_F(RuntimeConfigStoreTest, AnEnvironmentOverrideDoesNotHideABadValue) {
  Write(file_, kBefore);
  ASSERT_EQ(setenv("MOCKTAIL_GRAPHICS_BACKEND", "opengl", 1), 0);
  const RuntimeConfigSaveResult saved = SaveRuntimeConfigText(
      file_, kBefore, "graphics:\n  backend: no-such-backend\n");
  ASSERT_EQ(unsetenv("MOCKTAIL_GRAPHICS_BACKEND"), 0);
  EXPECT_FALSE(saved);
  EXPECT_EQ(Read(file_), kBefore);
}

TEST_F(RuntimeConfigStoreTest, RefusesToOverwriteAFileThatChangedOnDisk) {
  const std::string by_hand = "graphics:\n  backend: opengl\n  vsync: on\n";
  Write(file_, by_hand);
  const RuntimeConfigSaveResult saved =
      SaveRuntimeConfigText(file_, kBefore, kAfter);
  EXPECT_FALSE(saved);
  EXPECT_NE(saved.error.find("changed"), std::string::npos) << saved.error;
  EXPECT_EQ(Read(file_), by_hand);
  EXPECT_EQ(EntryCount(), 1u);
}

TEST_F(RuntimeConfigStoreTest, RefusesAMissingFile) {
  EXPECT_FALSE(SaveRuntimeConfigText(file_, "", kAfter));
  EXPECT_FALSE(std::filesystem::exists(file_));
  EXPECT_EQ(EntryCount(), 0u);
}

TEST_F(RuntimeConfigStoreTest, RefusesASymlinkAndLeavesItsTargetAlone) {
  const std::filesystem::path target = directory_ / "target.yaml";
  Write(target, kBefore);
  std::filesystem::create_symlink(target, file_);

  EXPECT_FALSE(SaveRuntimeConfigText(file_, kBefore, kAfter));
  EXPECT_TRUE(std::filesystem::is_symlink(file_));
  EXPECT_EQ(Read(target), kBefore);
  EXPECT_EQ(EntryCount(), 2u);
}

TEST_F(RuntimeConfigStoreTest, ReportsADirectoryItCannotWriteTo) {
  if (geteuid() == 0) {
    GTEST_SKIP() << "root can write to any directory";
  }
  Write(file_, kBefore);
  ASSERT_EQ(chmod(directory_.c_str(), 0500), 0);
  const RuntimeConfigSaveResult saved =
      SaveRuntimeConfigText(file_, kBefore, kAfter);
  ASSERT_EQ(chmod(directory_.c_str(), 0700), 0);
  EXPECT_FALSE(saved);
  EXPECT_EQ(Read(file_), kBefore);
}

TEST_F(RuntimeConfigStoreTest, AnEditedFirstRunFileSavesAndReadsBack) {
  ASSERT_TRUE(EnsureRuntimeConfigFile(file_));
  const RuntimeConfigText read = ReadRuntimeConfigText(file_);
  ASSERT_TRUE(read) << read.error;

  const RuntimeConfigEdit edit =
      SetRuntimeConfigString(read.text, "graphics.vsync", "on");
  ASSERT_TRUE(edit) << edit.error;
  const RuntimeConfigSaveResult saved =
      SaveRuntimeConfigText(file_, read.text, edit.text);
  ASSERT_TRUE(saved) << saved.error;
  EXPECT_EQ(ReadRuntimeConfigText(file_).text, edit.text);
  EXPECT_EQ(Mode(file_), 0600u);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
