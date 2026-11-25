#ifndef MOCKTAIL_GRAPHICS_VULKAN_OBJECT_REUSE_H_
#define MOCKTAIL_GRAPHICS_VULKAN_OBJECT_REUSE_H_

#include <vulkan/vulkan.h>

#include <string>
#include <utility>

namespace mocktail::graphics {

// Keys that are equal only when two create calls ask for the same object.
std::string VulkanInstanceKey(const VkInstanceCreateInfo& info);
std::string VulkanDeviceKey(VkPhysicalDevice physical_device,
                            const VkDeviceCreateInfo& info);

// Holds one host object that the app destroyed, for its next identical create.
template <typename Handle>
class KeptVulkanObject final {
 public:
  // Returns the object this one displaces, which the caller must destroy.
  Handle Keep(Handle handle, std::string key) {
    Handle displaced = handle_;
    handle_ = handle;
    key_ = std::move(key);
    return displaced;
  }

  Handle Take(const std::string& key) {
    if (handle_ == VK_NULL_HANDLE || key != key_) {
      return VK_NULL_HANDLE;
    }
    Handle taken = handle_;
    handle_ = VK_NULL_HANDLE;
    key_.clear();
    return taken;
  }

  Handle Held() const { return handle_; }

 private:
  Handle handle_ = VK_NULL_HANDLE;
  std::string key_;
};

}  // namespace mocktail::graphics

#endif
