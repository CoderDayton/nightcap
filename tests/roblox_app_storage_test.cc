#include "runtime/roblox_app_storage.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <thread>

namespace mocktail {
namespace runtime {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

class RobloxAppStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("nightcap-app-storage-" +
             std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "-" + ::testing::UnitTest::GetInstance()
                       ->current_test_info()
                       ->name());
    std::filesystem::remove_all(root_);
    file_ = root_ / "appStorage.json";
    std::filesystem::create_directories(root_);
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  void Write(const std::string& text) {
    const std::filesystem::path temp = file_.string() + ".tmp";
    std::ofstream(temp) << text;
    std::filesystem::rename(temp, file_);
  }

  void Age() {
    std::filesystem::last_write_time(
        file_, std::filesystem::file_time_type::clock::now() - seconds(60));
  }

  static std::filesystem::file_time_type SwitchTime() {
    return std::filesystem::file_time_type::clock::now() - seconds(2);
  }

  std::filesystem::path root_;
  std::filesystem::path file_;
};

TEST_F(RobloxAppStorageTest, ReadsAccountBlob) {
  Write(R"({"PreviousAccountsList":"[]","AccountBlob":"blob-1"})");
  EXPECT_EQ(ReadRobloxAccountBlob(file_), "blob-1");
}

TEST_F(RobloxAppStorageTest, MissingOrBrokenFileReadsEmpty) {
  EXPECT_EQ(ReadRobloxAccountBlob(file_), "");
  Write("{not json");
  EXPECT_EQ(ReadRobloxAccountBlob(file_), "");
  Write(R"({"AccountBlob":42})");
  EXPECT_EQ(ReadRobloxAccountBlob(file_), "");
}

TEST_F(RobloxAppStorageTest, BlobSavedJustBeforeTheSwitchCountsAtOnce) {
  Write(R"({"AccountBlob":"new"})");
  const auto start = std::chrono::steady_clock::now();
  const SavedAccountBlob result =
      WaitForSavedRobloxAccountBlob(file_, SwitchTime(), milliseconds(2000));
  EXPECT_TRUE(result.saved);
  EXPECT_EQ(result.blob, "new");
  EXPECT_LT(std::chrono::steady_clock::now() - start, milliseconds(500));
}

TEST_F(RobloxAppStorageTest, OlderBlobWaitsForTheNextSave) {
  Write(R"({"AccountBlob":"old"})");
  Age();
  std::thread writer([this] {
    std::this_thread::sleep_for(milliseconds(200));
    Write(R"({"AccountBlob":"new"})");
  });
  const SavedAccountBlob result =
      WaitForSavedRobloxAccountBlob(file_, SwitchTime(), milliseconds(3000));
  writer.join();
  EXPECT_TRUE(result.saved);
  EXPECT_EQ(result.blob, "new");
}

TEST_F(RobloxAppStorageTest, OlderBlobGivesUpAfterTimeout) {
  Write(R"({"AccountBlob":"old"})");
  Age();
  const auto start = std::chrono::steady_clock::now();
  const SavedAccountBlob result =
      WaitForSavedRobloxAccountBlob(file_, SwitchTime(), milliseconds(300));
  const auto waited = std::chrono::steady_clock::now() - start;
  EXPECT_FALSE(result.saved);
  EXPECT_EQ(result.blob, "old");
  EXPECT_GE(waited, milliseconds(300));
  EXPECT_LT(waited, milliseconds(1500));
}

TEST_F(RobloxAppStorageTest, EmptyBlobIsNotSaved) {
  Write(R"({"AccountBlob":""})");
  const SavedAccountBlob result =
      WaitForSavedRobloxAccountBlob(file_, SwitchTime(), milliseconds(300));
  EXPECT_FALSE(result.saved);
  EXPECT_EQ(result.blob, "");
}

}
}
}
