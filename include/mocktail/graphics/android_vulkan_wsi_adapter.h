#ifndef MOCKTAIL_GRAPHICS_ANDROID_VULKAN_WSI_ADAPTER_H_
#define MOCKTAIL_GRAPHICS_ANDROID_VULKAN_WSI_ADAPTER_H_

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#include "mocktail/graphics/sdl_vulkan_wsi.h"
#include "mocktail/status.h"

namespace mocktail {
namespace graphics {

// Stable signed 32-bit ABI.
using AndroidVulkanResult = VkResult;

inline constexpr AndroidVulkanResult kAndroidVulkanSuccess =
    static_cast<AndroidVulkanResult>(0);
inline constexpr AndroidVulkanResult kAndroidVulkanErrorInitializationFailed =
    static_cast<AndroidVulkanResult>(-3);

inline constexpr char kAndroidSurfaceExtension[] =
    "VK_KHR_android_surface";

// VK_SUBOPTIMAL_KHR still returns a usable image. Roblox handles it as a fatal
// error, so accept it until the host reports a real out-of-date surface.
VkResult NormalizeAndroidSwapchainResult(VkResult result);

// Returned storage owns the translated extension names.
Status TranslateAndroidVulkanInstanceExtensions(
    const std::vector<std::string>& android_extensions,
    const std::vector<std::string>& host_wsi_extensions,
    std::vector<std::string>* host_extensions);

// Ignores the guest native-window field; SDL alone creates the host surface.
class AndroidVulkanWsiAdapter final {
 public:
  Status Initialize(SdlVulkanWsi* host_wsi);
  void Shutdown();
  bool IsInitialized() const { return host_wsi_ != nullptr; }

  Status TranslateInstanceExtensions(
      const std::vector<std::string>& android_extensions,
      std::vector<std::string>* host_extensions) const;

  AndroidVulkanResult CreateAndroidSurface(
      VkInstance instance, const void* android_create_info,
      const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) const;
  void DestroySurface(VkInstance instance, VkSurfaceKHR surface,
                      const VkAllocationCallbacks* allocator) const;

 private:
  SdlVulkanWsi* host_wsi_ = nullptr;
};

}  // namespace graphics
}  // namespace mocktail

#endif  // MOCKTAIL_GRAPHICS_ANDROID_VULKAN_WSI_ADAPTER_H_
