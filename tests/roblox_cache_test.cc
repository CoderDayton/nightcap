#include "runtime/roblox_cache.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

namespace mocktail::runtime {
namespace {

namespace fs = std::filesystem;

class RobloxCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base_ = fs::temp_directory_path() /
            ("roblox-cache-test-" + std::to_string(getpid()) + "-" +
             ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(base_);
    root_ = base_ / "data";
    fs::create_directories(root_);
  }
  void TearDown() override { fs::remove_all(base_); }

  void Write(const fs::path& path, std::size_t size) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << std::string(size, 'x');
  }

  void WriteTypicalData() {
    Write(root_ / "rbx-storage/00/a", 100);
    Write(root_ / "rbx-storage/ff/b", 50);
    Write(root_ / "files/appData/rbx-storage.db", 30);
    Write(root_ / "files/appData/rbx-storage.db-wal", 20);
    Write(root_ / "files/appData/rbx-storage.db-shm", 10);
    Write(root_ / "files/appData/LocalStorage/appStorage.json", 1000);
    Write(root_ / "files/appData/GlobalBasicSettings_13.xml", 7);
    Write(root_ / "files/appData/rbx-storage.id", 5);
  }

  fs::path base_;
  fs::path root_;
};

TEST_F(RobloxCacheTest, MeasuresOnlyTheAssetCache) {
  WriteTypicalData();
  EXPECT_EQ(RobloxCacheBytes(root_), 210u);
}

TEST_F(RobloxCacheTest, ClearRemovesAssetsAndKeepsSettings) {
  WriteTypicalData();
  const RobloxCacheClear cleared = ClearRobloxCache(root_);
  ASSERT_TRUE(cleared) << cleared.error;
  EXPECT_EQ(cleared.bytes, 210u);
  EXPECT_TRUE(fs::is_directory(root_ / "rbx-storage"));
  EXPECT_TRUE(fs::is_empty(root_ / "rbx-storage"));
  EXPECT_FALSE(fs::exists(root_ / "files/appData/rbx-storage.db"));
  EXPECT_FALSE(fs::exists(root_ / "files/appData/rbx-storage.db-wal"));
  EXPECT_FALSE(fs::exists(root_ / "files/appData/rbx-storage.db-shm"));
  EXPECT_TRUE(fs::exists(root_ / "files/appData/LocalStorage/appStorage.json"));
  EXPECT_TRUE(fs::exists(root_ / "files/appData/GlobalBasicSettings_13.xml"));
  EXPECT_TRUE(fs::exists(root_ / "files/appData/rbx-storage.id"));
  EXPECT_EQ(RobloxCacheBytes(root_), 0u);
}

TEST_F(RobloxCacheTest, AMissingCacheIsEmpty) {
  EXPECT_EQ(RobloxCacheBytes(root_ / "missing"), 0u);
  const RobloxCacheClear cleared = ClearRobloxCache(root_ / "missing");
  ASSERT_TRUE(cleared) << cleared.error;
  EXPECT_EQ(cleared.bytes, 0u);
}

TEST_F(RobloxCacheTest, NeverFollowsLinks) {
  Write(base_ / "outside/keep", 40);
  Write(root_ / "rbx-storage/00/a", 100);
  fs::create_directory_symlink(base_ / "outside", root_ / "rbx-storage/link");
  EXPECT_EQ(RobloxCacheBytes(root_), 100u);
  const RobloxCacheClear cleared = ClearRobloxCache(root_);
  ASSERT_TRUE(cleared) << cleared.error;
  EXPECT_TRUE(fs::exists(base_ / "outside/keep"));
  EXPECT_FALSE(fs::exists(fs::symlink_status(root_ / "rbx-storage/link")));
}

TEST_F(RobloxCacheTest, AnUnreadableFolderDoesNotHideTheRest) {
  for (int index = 0; index < 32; ++index) {
    Write(root_ / "rbx-storage" / std::to_string(index) / "a", 10);
  }
  const fs::path locked = root_ / "rbx-storage/locked";
  Write(locked / "a", 1000);
  fs::permissions(locked, fs::perms::none);
  const std::uintmax_t bytes = RobloxCacheBytes(root_);
  fs::permissions(locked, fs::perms::owner_all);
  EXPECT_EQ(bytes, 320u);
}

TEST_F(RobloxCacheTest, RefusesALinkedStorageFolder) {
  Write(base_ / "outside/keep", 40);
  fs::create_directory_symlink(base_ / "outside", root_ / "rbx-storage");
  EXPECT_EQ(RobloxCacheBytes(root_), 0u);
  EXPECT_FALSE(ClearRobloxCache(root_));
  EXPECT_TRUE(fs::exists(base_ / "outside/keep"));
}

TEST_F(RobloxCacheTest, TheDataLockWaitsForAClearToFinish) {
  const fs::path lock = base_ / "in-use.lock";
  std::optional<SingleInstanceLock> clearing(SingleInstanceLock::Acquire(lock));
  ASSERT_TRUE(clearing->acquired());
  std::thread release([&clearing] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    clearing.reset();
  });
  const SingleInstanceLock launch =
      AcquireRobloxDataLock(lock, std::chrono::seconds(5));
  release.join();
  EXPECT_TRUE(launch.acquired()) << launch.error();
}

TEST_F(RobloxCacheTest, TheDataLockGivesUpAfterItsWait) {
  const fs::path lock = base_ / "in-use.lock";
  const SingleInstanceLock running = SingleInstanceLock::Acquire(lock);
  ASSERT_TRUE(running.acquired());
  const auto started = std::chrono::steady_clock::now();
  const SingleInstanceLock launch =
      AcquireRobloxDataLock(lock, std::chrono::milliseconds(200));
  EXPECT_TRUE(launch.already_running());
  EXPECT_GE(std::chrono::steady_clock::now() - started,
            std::chrono::milliseconds(200));
}

}
}
