#ifndef MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
#define MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/runtime_config.h"

namespace mocktail {
namespace runtime {

// Readable ICD manifest for `vendor` that this build can actually load, or an
// empty path. Directories are searched in order; a manifest built for another
// architecture is never selected.
std::string SelectVulkanIcdManifest(
    const std::vector<std::filesystem::path>& directories,
    std::string_view vendor);

// One GPU as the kernel reports it.
struct HostGpu {
  unsigned int vendor = 0;    // PCI vendor ID.
  std::string kernel_driver;  // Bound kernel driver; empty when unknown.
};

struct VulkanIcdSelection {
  std::string manifest;              // Empty when no GPU has a usable driver.
  std::vector<std::string> skipped;  // Why each passed-over GPU was skipped.
};

// ICD manifest for the first GPU whose bound kernel driver has an installed
// Vulkan driver. NVIDIA and AMD cards are tried before Intel when
// `prefer_discrete` is set, and after it otherwise. A GPU with an unknown
// kernel driver is matched by vendor alone.
VulkanIcdSelection SelectHostVulkanIcd(
    const std::vector<HostGpu>& gpus,
    const std::vector<std::filesystem::path>& directories,
    bool prefer_discrete);

// Publishes the resolved graphics backend before the managed payload updater
// starts. OpenGL is a strict system EGL/GLES path; it never silently retries
// through ANGLE/Vulkan or accepts a window without a real graphics context.
bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error = nullptr);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
