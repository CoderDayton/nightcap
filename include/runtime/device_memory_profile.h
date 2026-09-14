#ifndef MOCKTAIL_RUNTIME_DEVICE_MEMORY_PROFILE_H_
#define MOCKTAIL_RUNTIME_DEVICE_MEMORY_PROFILE_H_

#include <cstdint>

namespace mocktail {
namespace runtime {

// Android ActivityManager memory values published through DeviceParams.
// Roblox derives its device RAM tier, and tiered render budgets, from them.
struct DeviceMemoryProfile {
  std::int32_t total_memory_mb = 0;
  std::int32_t memory_class_mb = 0;
  std::int32_t large_memory_class_mb = 0;
  std::int64_t low_memory_killer_background_threshold = 0;
  std::int64_t low_memory_killer_foreground_threshold = 0;
  bool low_ram_device = false;
};

// Hosts with at least 4 GiB report their real total memory as a normal-RAM
// device. Smaller hosts, or an unreadable total (0), keep the 2 GiB low-RAM
// profile.
DeviceMemoryProfile BuildDeviceMemoryProfile(std::uint64_t host_memory_bytes);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_DEVICE_MEMORY_PROFILE_H_
