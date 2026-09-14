#ifndef MOCKTAIL_GRAPHICS_VULKAN_ETC2_EMULATION_H_
#define MOCKTAIL_GRAPHICS_VULKAN_ETC2_EMULATION_H_

#include <vulkan/vulkan.h>

#include <cstdint>

#include "mocktail/graphics/etc2_decoder.h"

namespace mocktail::graphics {

// Host format that stores a decoded ETC2/EAC image, with its block encoding.
struct Etc2EmulatedFormat {
  VkFormat host_format = VK_FORMAT_UNDEFINED;
  EtcFormat etc_format = EtcFormat::kEtc2Rgb8;
};

// Maps an ETC2/EAC Vulkan format to its decoded host format. Returns false for
// every other format.
bool LookupEmulatedEtc2Format(VkFormat format, Etc2EmulatedFormat* out);

// Format features reported for an emulated ETC2/EAC format: the host format's
// sampling and transfer features only, as for a native compressed format.
VkFormatFeatureFlags EmulatedEtc2FormatFeatures(
    VkFormatFeatureFlags host_features);

// Staging bytes consumed and decoded bytes produced by one 2D upload region.
// Returns false for an empty extent, a depth other than 1, or no layers.
bool Etc2UploadByteCounts(EtcFormat format, const VkExtent3D& extent,
                          std::uint32_t layer_count, VkDeviceSize* compressed,
                          VkDeviceSize* decoded);

// Reports ETC2/EAC support on hosts without native ETC2 (desktop GPUs) and
// stores those textures decoded. Images are created with the host format;
// each staged upload into one is redirected to a host-visible staging buffer
// that is filled by decoding the application's staging bytes when the
// command buffer is submitted. Every submit decodes again, as a native copy
// re-reads its source on each execution.
// Staging buffers live until their command buffer is begun, reset or freed.
// MOCKTAIL_DISABLE_ETC2_EMULATION=1 turns the emulation off.
class VulkanEtc2Emulation final {
 public:
  VulkanEtc2Emulation();
  ~VulkanEtc2Emulation();
  VulkanEtc2Emulation(const VulkanEtc2Emulation&) = delete;
  VulkanEtc2Emulation& operator=(const VulkanEtc2Emulation&) = delete;

  // True when this physical device lacks native ETC2 and emulation is on.
  bool PhysicalDeviceNeedsEmulation(VkPhysicalDevice physical_device,
                                    PFN_vkGetPhysicalDeviceFeatures host);

  void RegisterDevice(VkDevice device, VkPhysicalDevice physical_device,
                      bool emulated,
                      const VkPhysicalDeviceMemoryProperties& memory,
                      PFN_vkGetDeviceProcAddr get_device_proc_addr);
  void DestroyDevice(VkDevice device);

  VkResult CreateImage(VkDevice device, const VkImageCreateInfo* create_info,
                       const VkAllocationCallbacks* allocator, VkImage* image);
  void DestroyImage(VkDevice device, VkImage image,
                    const VkAllocationCallbacks* allocator);
  VkResult CreateImageView(VkDevice device,
                           const VkImageViewCreateInfo* create_info,
                           const VkAllocationCallbacks* allocator,
                           VkImageView* view);
  VkResult BindBufferMemory(VkDevice device, VkBuffer buffer,
                            VkDeviceMemory memory, VkDeviceSize offset);
  VkResult BindBufferMemory2(VkDevice device, std::uint32_t count,
                             const VkBindBufferMemoryInfo* infos);
  VkResult MapMemory(VkDevice device, VkDeviceMemory memory,
                     VkDeviceSize offset, VkDeviceSize size,
                     VkMemoryMapFlags flags, void** data);
  VkResult MapMemory2(VkDevice device, const VkMemoryMapInfo* info,
                      void** data);
  void UnmapMemory(VkDevice device, VkDeviceMemory memory);
  VkResult UnmapMemory2(VkDevice device, const VkMemoryUnmapInfo* info);
  void FreeMemory(VkDevice device, VkDeviceMemory memory,
                  const VkAllocationCallbacks* allocator);
  void DestroyBuffer(VkDevice device, VkBuffer buffer,
                     const VkAllocationCallbacks* allocator);
  void CmdCopyBufferToImage(VkDevice device, VkCommandBuffer command_buffer,
                            VkBuffer source, VkImage destination,
                            VkImageLayout layout, std::uint32_t region_count,
                            const VkBufferImageCopy* regions);
  void CmdCopyBufferToImage2(VkDevice device, VkCommandBuffer command_buffer,
                             const VkCopyBufferToImageInfo2* info);
  void CmdExecuteCommands(VkDevice device, VkCommandBuffer command_buffer,
                          std::uint32_t count,
                          const VkCommandBuffer* secondaries);

  // Decodes every upload recorded into these command buffers and the
  // secondary command buffers they execute. Large batches decode on worker
  // threads; this returns once every upload is decoded.
  void PrepareSubmit(const VkCommandBuffer* command_buffers,
                     std::uint32_t count);
  // Destroys the staging buffers owned by a command buffer that is no longer
  // pending.
  void ReleaseCommandBuffer(VkCommandBuffer command_buffer);

  struct State;

 private:
  State* state_;
};

}  // namespace mocktail::graphics

#endif  // MOCKTAIL_GRAPHICS_VULKAN_ETC2_EMULATION_H_
