#ifndef MOCKTAIL_RUNTIME_ROBLOX_CACHE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_CACHE_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "runtime/runtime_paths.h"
#include "runtime/single_instance_lock.h"

namespace mocktail::runtime {

struct RobloxCacheClear {
  std::uintmax_t bytes = 0;
  std::string error;
  explicit operator bool() const { return error.empty(); }
};

std::uintmax_t RobloxCacheBytes(const std::filesystem::path& android_data_root);
RobloxCacheClear ClearRobloxCache(const std::filesystem::path& android_data_root);

std::filesystem::path RobloxDataLockPath(const RuntimePaths& paths);
SingleInstanceLock AcquireRobloxDataLock(const std::filesystem::path& lock,
                                         std::chrono::milliseconds wait);

}

#endif
