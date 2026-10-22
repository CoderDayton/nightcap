#include "runtime/android_device_id.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <regex>
#include <string>

namespace mocktail::runtime {
namespace {

namespace fs = std::filesystem;

class AndroidDeviceIdTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base_ = fs::temp_directory_path() /
            ("android-device-id-test-" + std::to_string(getpid()) + "-" +
             ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(base_);
    fs::create_directories(base_);
    file_ = base_ / "android_id";
  }
  void TearDown() override { fs::remove_all(base_); }

  static bool LooksLikeAndroidId(const std::string& value) {
    return std::regex_match(value, std::regex("[0-9a-f]{16}")) &&
           value != "0000000000000000";
  }

  std::string ReadFile() const {
    std::ifstream input(file_);
    std::string value;
    std::getline(input, value);
    return value;
  }

  fs::path base_;
  fs::path file_;
};

TEST_F(AndroidDeviceIdTest, CreatesRandomHexIdAndSavesItPrivately) {
  const AndroidDeviceId id = LoadOrCreateAndroidDeviceId(file_);
  ASSERT_TRUE(id) << id.error;
  EXPECT_TRUE(LooksLikeAndroidId(id.value)) << id.value;
  EXPECT_EQ(ReadFile(), id.value);
  struct stat info {};
  ASSERT_EQ(stat(file_.c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0777, 0600);
}

TEST_F(AndroidDeviceIdTest, ReusesTheSavedId) {
  const AndroidDeviceId first = LoadOrCreateAndroidDeviceId(file_);
  const AndroidDeviceId second = LoadOrCreateAndroidDeviceId(file_);
  ASSERT_TRUE(first) << first.error;
  ASSERT_TRUE(second) << second.error;
  EXPECT_EQ(first.value, second.value);
}

TEST_F(AndroidDeviceIdTest, SeparateInstallsGetDifferentIds) {
  const AndroidDeviceId first = LoadOrCreateAndroidDeviceId(file_);
  const AndroidDeviceId second =
      LoadOrCreateAndroidDeviceId(base_ / "other" / "android_id");
  ASSERT_TRUE(first) << first.error;
  ASSERT_TRUE(second) << second.error;
  EXPECT_NE(first.value, second.value);
}

TEST_F(AndroidDeviceIdTest, ReplacesSharedOrMalformedIds) {
  for (const char* bad : {"0000000000000000", "not-an-id", ""}) {
    std::ofstream(file_, std::ios::trunc) << bad;
    const AndroidDeviceId id = LoadOrCreateAndroidDeviceId(file_);
    ASSERT_TRUE(id) << id.error;
    EXPECT_TRUE(LooksLikeAndroidId(id.value)) << bad << " -> " << id.value;
    EXPECT_EQ(ReadFile(), id.value);
  }
}

}
}
