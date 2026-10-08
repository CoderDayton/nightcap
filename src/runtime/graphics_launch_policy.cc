#include "runtime/graphics_launch_policy.h"

#include "runtime/frame_rate_policy.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

// Keep the Vulkan shader workaround narrowly scoped. TextureManager2 is left
// enabled so the runtime can use its normal texture path and mip selection.
constexpr char kVulkanClientSettingsOverrides[] =
    R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})";

constexpr const char* kIcdDirectories[] = {
    "/usr/share/vulkan/icd.d",
    "/etc/vulkan/icd.d",
    "/usr/local/share/vulkan/icd.d",
};

#if defined(__aarch64__)
constexpr char kNativeArchitecture[] = "aarch64";

constexpr const char* kForeignArchitectures[] = {
    "i686",  "i586", "i486",  "i386",    "x86_64",  "x86.",
    "amd64", "armhf", "armv7", "ppc64", "riscv64", "s390x",
};
#else
constexpr char kNativeArchitecture[] = "x86_64";

constexpr const char* kForeignArchitectures[] = {
    "i686", "i586", "i486", "i386",   "x86.",   "aarch64",
    "arm64", "armhf", "armv7", "ppc64", "riscv64", "s390x",
};
#endif

constexpr unsigned int kPciVendorAmd = 0x1002;
constexpr unsigned int kPciVendorNvidia = 0x10de;
constexpr unsigned int kPciVendorIntel = 0x8086;

bool ForeignArchitecture(const std::string& name) {
  for (const char* architecture : kForeignArchitectures) {
    if (name.find(architecture) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool SetValue(const char* name, const std::string& value, std::string* error) {
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot publish resolved graphics setting: ") + name;
  }
  return false;
}

bool SetDefault(const char* name, const std::string& value,
                std::string* error) {
  const char* current = std::getenv(name);
  if (current != nullptr && current[0] != '\0') {
    return true;
  }
  return SetValue(name, value, error);
}

bool IsStrictOpenGlName(const std::string& name) {
  return name == "opengl" || name == "gles";
}

bool EnvIsOff(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr &&
         (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0 ||
          std::strcmp(value, "igpu") == 0);
}

bool UnthrottledPresentation(const RuntimeConfig& config) {
  if (config.vsync_mode() == "off" || config.vsync_mode() == "0") {
    return true;
  }
  return config.frame_rate().mode == FrameRateLimitMode::kUnlimited;
}

bool ParsePciVendor(const std::string& raw, unsigned int* vendor) {
  if (vendor == nullptr || raw.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(raw.c_str(), &end, 16);
  if (end == raw.c_str() || parsed > 0xffffUL) {
    return false;
  }
  *vendor = static_cast<unsigned int>(parsed);
  return true;
}

std::vector<HostGpu> DetectHostGpus() {
  std::vector<HostGpu> gpus;
  for (int index = 0; index < 16; ++index) {
    const std::string device =
        "/sys/class/drm/card" + std::to_string(index) + "/device";
    std::ifstream input(device + "/vendor");
    std::string raw;
    HostGpu gpu;
    if (!(input >> raw) || !ParsePciVendor(raw, &gpu.vendor)) {
      continue;
    }
    std::error_code error;
    const std::filesystem::path driver =
        std::filesystem::read_symlink(device + "/driver", error);
    if (!error) {
      gpu.kernel_driver = driver.filename().string();
    }
    gpus.push_back(std::move(gpu));
  }
  return gpus;
}

bool HasVendor(const std::vector<HostGpu>& gpus, unsigned int vendor) {
  return std::any_of(gpus.begin(), gpus.end(), [vendor](const HostGpu& gpu) {
    return gpu.vendor == vendor;
  });
}

const char* VendorName(unsigned int vendor) {
  switch (vendor) {
    case kPciVendorAmd:
      return "AMD";
    case kPciVendorNvidia:
      return "NVIDIA";
    case kPciVendorIntel:
      return "Intel";
    default:
      return "unknown";
  }
}

// ICD manifest names that can drive `gpu`, best first. Empty when the bound
// kernel driver has no Vulkan driver at all.
std::vector<const char*> VulkanIcdsFor(const HostGpu& gpu) {
  switch (gpu.vendor) {
    case kPciVendorNvidia:
      if (gpu.kernel_driver == "nvidia") {
        return {"nvidia_icd"};
      }
      if (gpu.kernel_driver == "nouveau") {
        return {"nouveau_icd"};
      }
      return {"nvidia_icd", "nouveau_icd"};
    case kPciVendorAmd:
      // RADV needs the amdgpu kernel driver.
      if (gpu.kernel_driver == "radeon") {
        return {};
      }
      return {"radeon_icd"};
    case kPciVendorIntel:
      return {"intel_icd", "intel_hasvk_icd"};
    default:
      return {};
  }
}

std::string SkipReason(const HostGpu& gpu,
                       const std::vector<const char*>& icds) {
  std::string reason = std::string(VendorName(gpu.vendor)) + " card";
  if (!gpu.kernel_driver.empty()) {
    reason += " on the \"" + gpu.kernel_driver + "\" kernel driver";
  }
  if (icds.empty()) {
    return reason + " has no Vulkan support";
  }
  reason += " has no installed Vulkan driver (looked for";
  for (const char* icd : icds) {
    reason += std::string(" ") + icd;
  }
  return reason + ")";
}

// Match Mesa ANV: 75% of RAM when the machine has more than 4GiB, else 50%.
// Forcing 50 on an 8GiB UHD 620 iGPU advertises a smaller Vk heap than ANV
// would, which increases BO eviction inside GEM_EXECBUFFER2.
const char* AnvSysMemLimitPercent() {
  std::ifstream input("/proc/meminfo");
  std::string key;
  unsigned long kb = 0;
  std::string unit;
  while (input >> key >> kb >> unit) {
    if (key == "MemTotal:") {
      return kb > 4UL * 1024UL * 1024UL ? "75" : "50";
    }
  }
  return "50";
}

bool ApplyVulkanIcdPolicy(const std::vector<HostGpu>& gpus,
                          std::string* error) {
  // Drop software/emulation ICDs even when the user already pinned a driver
  // list. Old loaders ignore this variable.
  if (!SetDefault("VK_LOADER_DRIVERS_DISABLE",
                  "lvp_icd:dzn_icd:virtio_icd", error)) {
    return false;
  }
  const char* existing_files = std::getenv("VK_DRIVER_FILES");
  const char* existing_icds = std::getenv("VK_ICD_FILENAMES");
  if ((existing_files != nullptr && existing_files[0] != '\0') ||
      (existing_icds != nullptr && existing_icds[0] != '\0')) {
    return true;
  }
  std::vector<std::filesystem::path> directories;
  for (const char* directory : kIcdDirectories) {
    directories.emplace_back(directory);
  }
  const bool prefer_discrete = !EnvIsOff("DRI_PRIME") &&
                               !EnvIsOff("__NV_PRIME_RENDER_OFFLOAD");
  const VulkanIcdSelection selection =
      SelectHostVulkanIcd(gpus, directories, prefer_discrete);
  for (const std::string& reason : selection.skipped) {
    std::fprintf(stderr, "  [runtime] vulkan: %s\n", reason.c_str());
  }
  if (selection.manifest.empty()) {
    if (!selection.skipped.empty()) {
      std::fprintf(stderr,
                   "  [runtime] vulkan: no GPU here can run Vulkan; set "
                   "graphics.backend: opengl in the config\n");
    }
    return true;
  }
  std::fprintf(stderr, "  [runtime] vulkan ICD=%s\n",
               selection.manifest.c_str());
  return SetDefault("VK_DRIVER_FILES", selection.manifest, error) &&
         SetDefault("VK_ICD_FILENAMES", selection.manifest, error);
}

}  // namespace

VulkanIcdSelection SelectHostVulkanIcd(
    const std::vector<HostGpu>& gpus,
    const std::vector<std::filesystem::path>& directories,
    bool prefer_discrete) {
  constexpr unsigned int kDiscreteFirst[] = {kPciVendorNvidia, kPciVendorAmd,
                                             kPciVendorIntel};
  constexpr unsigned int kIntegratedFirst[] = {
      kPciVendorIntel, kPciVendorNvidia, kPciVendorAmd};
  VulkanIcdSelection selection;
  for (const unsigned int vendor :
       prefer_discrete ? kDiscreteFirst : kIntegratedFirst) {
    for (const HostGpu& gpu : gpus) {
      if (gpu.vendor != vendor) {
        continue;
      }
      const std::vector<const char*> icds = VulkanIcdsFor(gpu);
      for (const char* icd : icds) {
        selection.manifest = SelectVulkanIcdManifest(directories, icd);
        if (!selection.manifest.empty()) {
          return selection;
        }
      }
      selection.skipped.push_back(SkipReason(gpu, icds));
    }
  }
  return selection;
}

std::string SelectVulkanIcdManifest(
    const std::vector<std::filesystem::path>& directories,
    std::string_view vendor) {
  if (vendor.empty()) {
    return {};
  }
  for (const std::filesystem::path& directory : directories) {
    std::error_code error;
    std::vector<std::string> names;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (!iterator->is_regular_file(error)) {
        continue;
      }
      const std::string name = iterator->path().filename().string();
      if (name.find(".json") == std::string::npos ||
          name.find(vendor) == std::string::npos || ForeignArchitecture(name)) {
        continue;
      }
      names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    std::string generic;
    for (const std::string& name : names) {
      const std::string path = (directory / name).string();
      if (access(path.c_str(), R_OK) != 0) {
        continue;
      }
      if (name.find(kNativeArchitecture) != std::string::npos) {
        return path;
      }
      if (generic.empty()) {
        generic = path;
      }
    }
    if (!generic.empty()) {
      return generic;
    }
  }
  return {};
}

bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error) {
  if (config.graphics_backend() == GraphicsBackend::kUnknown) {
    if (error != nullptr) {
      *error = "cannot apply an unknown graphics backend";
    }
    return false;
  }

  const bool direct_vulkan =
      config.graphics_backend() == GraphicsBackend::kVulkan;
  if (!SetValue("MOCKTAIL_GRAPHICS_BACKEND",
                config.graphics_backend_name(), error) ||
      !SetValue("MOCKTAIL_PRELOAD_VULKAN_SHIM",
                direct_vulkan ? "1" : "0", error) ||
      !SetDefault("MOCKTAIL_REQUIRE_REAL_GRAPHICS", "1", error)) {
    return false;
  }

  if (IsStrictOpenGlName(config.graphics_backend_name()) &&
      (!SetValue("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK", "1", error) ||
       !SetValue("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK", "0", error))) {
    return false;
  }

  if (direct_vulkan) {
    const char* wsi_mode =
        UnthrottledPresentation(config) ? "immediate" : "mailbox";
    const std::vector<HostGpu> gpus = DetectHostGpus();
    if (!SetDefault("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON",
                    kVulkanClientSettingsOverrides, error) ||
        !SetDefault("ANV_SYS_MEM_LIMIT", AnvSysMemLimitPercent(), error) ||
        !SetDefault("MESA_VK_WSI_PRESENT_MODE", wsi_mode, error) ||
        // Move GEM_EXECBUFFER2 off the application thread onto Mesa's submit
        // worker so the render thread is not stuck in i915 ioctl.
        !SetDefault("MESA_VK_ENABLE_SUBMIT_THREAD", "1", error) ||
        !ApplyVulkanIcdPolicy(gpus, error)) {
      return false;
    }
    // Low FRM only on Intel-only machines. Hybrid NVIDIA/AMD laptops should
    // keep the desktop quality default on the discrete GPU.
    if (HasVendor(gpus, kPciVendorIntel) &&
        !HasVendor(gpus, kPciVendorNvidia) &&
        !HasVendor(gpus, kPciVendorAmd) &&
        !SetDefault("MOCKTAIL_GRAPHICS_QUALITY", "1", error)) {
      return false;
    }
  }
  return true;
}

}  // namespace runtime
}  // namespace mocktail
