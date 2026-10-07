#ifndef MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_
#define MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

namespace mocktail {
namespace graphics {

enum class PresentModePolicy {
  kHostDefault,
  kVsync,
  kUnthrottled,
};

PresentModePolicy ResolvePresentModePolicy(std::string_view vsync_value,
                                           std::string_view frame_rate_value);

// Reads MOCKTAIL_VSYNC / MOCKTAIL_FRAME_RATE_LIMIT once. Window pacing and
// the Vulkan adapter must use this instead of getenv'ing the same keys.
PresentModePolicy CachedPresentModePolicy();

std::vector<VkPresentModeKHR>
FilterPresentModes(PresentModePolicy policy,
                   const std::vector<VkPresentModeKHR> &host_modes);
const char *PresentModePolicyName(PresentModePolicy policy);
const char *PresentModeKhrName(VkPresentModeKHR mode);

// Creates the swapchain with the mode in `info`. A driver can reject a mode
// that the device did not enable, so a failed create is retried once with the
// mode the game asked for. `create` takes the info and returns a VkResult.
template <typename Create>
VkResult CreateSwapchainWithPresentModeFallback(VkSwapchainCreateInfoKHR &info,
                                                VkPresentModeKHR requested,
                                                Create create) {
  const VkResult result = create(info);
  if (result == VK_SUCCESS || info.presentMode == requested) {
    return result;
  }
  switch (result) {
  case VK_ERROR_OUT_OF_HOST_MEMORY:
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
  case VK_ERROR_DEVICE_LOST:
  case VK_ERROR_SURFACE_LOST_KHR:
  case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
    return result; // Another present mode cannot fix these.
  default:
    break;
  }
  std::fprintf(stderr,
               "  [vulkan] swapchain presentMode %s failed (%d); retrying "
               "with %s\n",
               PresentModeKhrName(info.presentMode), static_cast<int>(result),
               PresentModeKhrName(requested));
  info.presentMode = requested;
  return create(info);
}

// Extra images so acquire does not wait on the compositor-held buffer.
std::uint32_t PreferSwapchainMinImageCount(PresentModePolicy policy,
                                           std::uint32_t requested,
                                           std::uint32_t min_images,
                                           std::uint32_t max_images);

} // namespace graphics
} // namespace mocktail

#endif // MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_
