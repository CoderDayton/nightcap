#include "runtime/device_memory_profile.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::uint64_t kMebibyte = 1024U * 1024U;

void ExpectLowRamProfile(const DeviceMemoryProfile& profile) {
  EXPECT_TRUE(profile.low_ram_device);
  EXPECT_EQ(profile.total_memory_mb, 2048);
  EXPECT_EQ(profile.memory_class_mb, 256);
  EXPECT_EQ(profile.large_memory_class_mb, 512);
  EXPECT_EQ(profile.low_memory_killer_background_threshold, 256);
  EXPECT_EQ(profile.low_memory_killer_foreground_threshold, 512);
}

TEST(DeviceMemoryProfileTest, KeepsTheLowRamProfileOnSmallOrUnknownHosts) {
  ExpectLowRamProfile(BuildDeviceMemoryProfile(0));
  ExpectLowRamProfile(BuildDeviceMemoryProfile(2048U * kMebibyte));
  ExpectLowRamProfile(BuildDeviceMemoryProfile(4095U * kMebibyte));
}

TEST(DeviceMemoryProfileTest, ReportsRealHostMemoryAsANormalRamDevice) {
  const DeviceMemoryProfile profile =
      BuildDeviceMemoryProfile(62951U * kMebibyte);
  EXPECT_FALSE(profile.low_ram_device);
  EXPECT_EQ(profile.total_memory_mb, 62951);
  EXPECT_EQ(profile.memory_class_mb, 512);
  EXPECT_EQ(profile.large_memory_class_mb, 1024);
  EXPECT_EQ(profile.low_memory_killer_background_threshold, 0);
  EXPECT_EQ(profile.low_memory_killer_foreground_threshold, 0);
}

TEST(DeviceMemoryProfileTest, StartsTheNormalRamProfileAtFourGibibytes) {
  const DeviceMemoryProfile profile =
      BuildDeviceMemoryProfile(4096U * kMebibyte);
  EXPECT_FALSE(profile.low_ram_device);
  EXPECT_EQ(profile.total_memory_mb, 4096);
}

TEST(DeviceMemoryProfileTest, ClampsTotalMemoryToTheJavaIntRange) {
  const DeviceMemoryProfile profile =
      BuildDeviceMemoryProfile(std::numeric_limits<std::uint64_t>::max());
  EXPECT_FALSE(profile.low_ram_device);
  EXPECT_EQ(profile.total_memory_mb, std::numeric_limits<std::int32_t>::max());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
