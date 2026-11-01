#ifndef MOCKTAIL_GRAPHICS_VULKAN_ETC2_EMULATION_H_
#define MOCKTAIL_GRAPHICS_VULKAN_ETC2_EMULATION_H_

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "mocktail/graphics/etc2_decoder.h"
#include "mocktail/graphics/etc2_gpu_decoder.h"

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

// Sums every mip level and layer; a `scale` above 1 adds log2(scale) host
// levels above level 0, up to `scale` times its width and height.
bool Etc2ImageByteCounts(EtcFormat format, const VkExtent3D& extent,
                         std::uint32_t mip_levels, std::uint32_t layers,
                         std::uint32_t scale, VkDeviceSize* compressed,
                         VkDeviceSize* decoded);

struct EmulatedTextureBytes {
  VkDeviceSize compressed = 0;
  VkDeviceSize decoded = 0;
};

// Whether emulated ETC2 is reported to the application as a supported
// feature, from the MOCKTAIL_ADVERTISE_ETC2 value: "0" hides it, anything
// else (or unset) reports it. Hidden ETC2 still decodes ETC2 images the
// application creates; it only steers the application toward other formats.
bool Etc2SupportAdvertised(const char* value);
bool Etc2SupportAdvertised();

// Whether ETC2/EAC images are kept compressed as BC7/BC4/BC5 where the GPU
// supports it, from the MOCKTAIL_ETC2_BC_TRANSCODE value: "0" keeps them
// decoded, anything else (or unset) transcodes.
bool Etc2BcTranscodeEnabled(const char* value);

// Factor applied to emulated ETC2 colour images no larger than
// kSmallTextureMaxExtent, from the MOCKTAIL_SMALL_TEXTURE_UPSCALE value:
// 1 leaves them alone; 2, 4 or 8 add log2(factor) levels above the image's
// own, upscaled from level 0, so a texture override can carry more detail
// than the original. Unset or non-numeric values ("adaptive") return
// kAdaptiveSmallTextureUpscale; numeric values are clamped to 1..8 and
// rounded down to a power of two.
inline constexpr std::uint32_t kSmallTextureMaxExtent = 64;
inline constexpr std::uint32_t kDefaultSmallTextureUpscale = 4;
inline constexpr std::uint32_t kAdaptiveSmallTextureUpscale = 0;
std::uint32_t SmallTextureUpscale(const char* value);

// Every mip level and layer, plus the levels a `scale` above 1 adds.
VkDeviceSize Etc2HostBlockCount(const VkExtent3D& extent,
                                std::uint32_t mip_levels, std::uint32_t layers,
                                std::uint32_t scale);

// The largest of 4, 2 and 1, the factors the setting offers, whose 64x64 BC7
// chain encodes within kAdaptiveUpscaleTextureNs: the top levels at
// `upscale_ns` per block, the texture's own levels at `transcode_ns`. 0 ns
// (unmeasured) gives 1, as an unknown card may be too slow for any upscale.
inline constexpr double kAdaptiveUpscaleTextureNs = 0.5e6;
std::uint32_t AdaptiveSmallTextureUpscale(double upscale_ns,
                                          double transcode_ns);

// GPU-time token bucket. A cost above the burst runs from a full bucket and
// leaves it negative until wall time pays it back. Take asks before work is
// planned; Charge books work already recorded; Refund returns a Take whose
// work never ran.
inline constexpr double kEncodeBudgetShare = 0.1;
inline constexpr double kEncodeBudgetBurstNs = 16e6;
class Etc2EncodeBudget {
 public:
  Etc2EncodeBudget(double share, double burst_ns)
      : share_(share), burst_ns_(burst_ns), available_ns_(burst_ns) {}
  bool Take(double cost_ns, std::uint64_t now_ns);
  void Charge(double cost_ns, std::uint64_t now_ns);
  void Refund(double cost_ns);

 private:
  void Refill(std::uint64_t now_ns);

  double share_;
  double burst_ns_;
  double available_ns_;
  std::uint64_t last_ns_ = 0;
};

// Staging for decoded uploads is carved out of host-visible blocks this
// large; an upload that needs more gets a block of its own.
inline constexpr VkDeviceSize kEtc2StagingBlockBytes = 16 * 1024 * 1024;

// A timeline semaphore and the value an asynchronous decode will signal.
// `semaphore` is VK_NULL_HANDLE when the decode already finished on the
// calling thread and the submit needs no extra wait.
struct Etc2SubmitWait {
  VkSemaphore semaphore = VK_NULL_HANDLE;
  std::uint64_t value = 0;
  std::vector<VkCommandBuffer> decode_commands;
};

// How many times this process has hit the ETC2 failure `message`, including
// the times past its log cap.
std::uint64_t Etc2FailureCount(const char* message);

// Reports ETC2/EAC support on hosts without native ETC2 (desktop GPUs) and
// stores those textures decoded. Images are created with the host format;
// each staged upload into one is redirected to a host-visible staging buffer
// that is filled by decoding the application's staging bytes when the
// command buffer is submitted. Every submit decodes again, as a native copy
// re-reads its source on each execution.
// Staging ranges live until their command buffer is begun, reset or freed.
// MOCKTAIL_DISABLE_ETC2_EMULATION=1 turns the emulation off.
class VulkanEtc2Emulation final {
 public:
  explicit VulkanEtc2Emulation(
      VkDeviceSize staging_block_bytes = kEtc2StagingBlockBytes);
  ~VulkanEtc2Emulation();
  VulkanEtc2Emulation(const VulkanEtc2Emulation&) = delete;
  VulkanEtc2Emulation& operator=(const VulkanEtc2Emulation&) = delete;

  // True when this physical device lacks native ETC2 and emulation is on.
  bool PhysicalDeviceNeedsEmulation(VkPhysicalDevice physical_device,
                                    PFN_vkGetPhysicalDeviceFeatures host);

  // `timeline_semaphore` states whether the device was created with the
  // timelineSemaphore feature enabled. Without it every decode runs on the
  // calling thread.
  // `bc_textures`: the device has textureCompressionBC enabled, so once the
  // transcode self-test passes, images become BC7/BC4/BC5. Upscaled images,
  // texture overrides, and usages or flags BC lacks stay decoded.
  void RegisterDevice(VkDevice device, VkPhysicalDevice physical_device,
                      bool emulated,
                      const VkPhysicalDeviceMemoryProperties& memory,
                      PFN_vkGetDeviceProcAddr get_device_proc_addr,
                      bool timeline_semaphore = false,
                      bool bc_textures = false);
  void DestroyDevice(VkDevice device);

  void RegisterQueue(VkDevice device, VkQueue queue,
                     std::uint32_t queue_family);
  bool EnableGpuDecode(VkDevice device, VkQueue queue,
                       std::uint32_t queue_family);

  VkResult CreateImage(VkDevice device, const VkImageCreateInfo* create_info,
                       const VkAllocationCallbacks* allocator, VkImage* image);
  void DestroyImage(VkDevice device, VkImage image,
                    const VkAllocationCallbacks* allocator);
  VkResult CreateImageView(VkDevice device,
                           const VkImageViewCreateInfo* create_info,
                           const VkAllocationCallbacks* allocator,
                           VkImageView* view);
  // A sampler's minLod and maxLod count the application's levels. On a view
  // that includes an upscaled image's top levels, a sampler with minLod above
  // 0 or a finite maxLod is swapped for a copy with those clamps shifted by
  // them; a minLod of 0 stays 0. Immutable samplers and separate sampler
  // descriptors can't be paired with a view and keep theirs.
  VkResult CreateSampler(VkDevice device, const VkSamplerCreateInfo* create_info,
                         const VkAllocationCallbacks* allocator,
                         VkSampler* sampler);
  // Logs clamped samplers baked into the layout, which can't be swapped.
  VkResult CreateDescriptorSetLayout(
      VkDevice device, const VkDescriptorSetLayoutCreateInfo* create_info,
      const VkAllocationCallbacks* allocator, VkDescriptorSetLayout* layout);
  // Called after a successful vkAllocateMemory, so the CPU never maps memory
  // the host cannot read.
  void TrackAllocation(VkDevice device, const VkMemoryAllocateInfo* info,
                       VkDeviceMemory memory);
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
  void CmdCopyImage(VkDevice device, VkCommandBuffer command_buffer,
                    VkImage source, VkImageLayout source_layout,
                    VkImage destination, VkImageLayout destination_layout,
                    std::uint32_t region_count, const VkImageCopy* regions);
  void CmdCopyImage2(VkDevice device, VkCommandBuffer command_buffer,
                     const VkCopyImageInfo2* info);
  // Readbacks and blits from an upscaled image read its own levels.
  void CmdCopyImageToBuffer(VkDevice device, VkCommandBuffer command_buffer,
                            VkImage source, VkImageLayout layout,
                            VkBuffer destination, std::uint32_t region_count,
                            const VkBufferImageCopy* regions);
  void CmdCopyImageToBuffer2(VkDevice device, VkCommandBuffer command_buffer,
                             const VkCopyImageToBufferInfo2* info);
  void CmdBlitImage(VkDevice device, VkCommandBuffer command_buffer,
                    VkImage source, VkImageLayout source_layout,
                    VkImage destination, VkImageLayout destination_layout,
                    std::uint32_t region_count, const VkImageBlit* regions,
                    VkFilter filter);
  void CmdBlitImage2(VkDevice device, VkCommandBuffer command_buffer,
                     const VkBlitImageInfo2* info);
  // Image barriers on an upscaled image move onto its host levels.
  void CmdPipelineBarrier(VkDevice device, VkCommandBuffer command_buffer,
                          VkPipelineStageFlags source_stage,
                          VkPipelineStageFlags destination_stage,
                          VkDependencyFlags flags, std::uint32_t memory_count,
                          const VkMemoryBarrier* memory,
                          std::uint32_t buffer_count,
                          const VkBufferMemoryBarrier* buffers,
                          std::uint32_t image_count,
                          const VkImageMemoryBarrier* images);
  void CmdPipelineBarrier2(VkDevice device, VkCommandBuffer command_buffer,
                           const VkDependencyInfo* dependency);
  void CmdWaitEvents(VkDevice device, VkCommandBuffer command_buffer,
                     std::uint32_t event_count, const VkEvent* events,
                     VkPipelineStageFlags source_stage,
                     VkPipelineStageFlags destination_stage,
                     std::uint32_t memory_count, const VkMemoryBarrier* memory,
                     std::uint32_t buffer_count,
                     const VkBufferMemoryBarrier* buffers,
                     std::uint32_t image_count,
                     const VkImageMemoryBarrier* images);
  void CmdWaitEvents2(VkDevice device, VkCommandBuffer command_buffer,
                      std::uint32_t event_count, const VkEvent* events,
                      const VkDependencyInfo* dependencies);
  // Remapped like vkCmdWaitEvents2, whose dependency must match this one.
  void CmdSetEvent2(VkDevice device, VkCommandBuffer command_buffer,
                    VkEvent event, const VkDependencyInfo* dependency);
  void DestroySampler(VkDevice device, VkSampler sampler,
                      const VkAllocationCallbacks* allocator);
  void DestroyImageView(VkDevice device, VkImageView view,
                        const VkAllocationCallbacks* allocator);
  void UpdateDescriptorSets(VkDevice device, std::uint32_t write_count,
                            const VkWriteDescriptorSet* writes,
                            std::uint32_t copy_count,
                            const VkCopyDescriptorSet* copies);
  void CmdPushDescriptorSet(VkDevice device, VkCommandBuffer command_buffer,
                            VkPipelineBindPoint bind_point,
                            VkPipelineLayout layout, std::uint32_t set,
                            std::uint32_t write_count,
                            const VkWriteDescriptorSet* writes);
  VkResult CreateDescriptorUpdateTemplate(
      VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* create_info,
      const VkAllocationCallbacks* allocator,
      VkDescriptorUpdateTemplate* update_template);
  void DestroyDescriptorUpdateTemplate(VkDevice device,
                                       VkDescriptorUpdateTemplate update_template,
                                       const VkAllocationCallbacks* allocator);
  void UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set,
                                       VkDescriptorUpdateTemplate update_template,
                                       const void* data);
  void CmdPushDescriptorSetWithTemplate(
      VkDevice device, VkCommandBuffer command_buffer,
      VkDescriptorUpdateTemplate update_template, VkPipelineLayout layout,
      std::uint32_t set, const void* data);
  void CmdExecuteCommands(VkDevice device, VkCommandBuffer command_buffer,
                          std::uint32_t count,
                          const VkCommandBuffer* secondaries);

  // Decodes every upload recorded into these command buffers and the
  // secondary command buffers they execute.
  //
  // With `allow_async` on a device that has a timeline semaphore, the decode
  // runs on worker threads, and so does the gather of the compressed bytes
  // whenever the application already has them mapped. The returned semaphore
  // and value must then be added to the submit's waits, so the GPU copy
  // cannot execute before the decode finishes. Values are signalled in the
  // order they are handed out.
  //
  // Those threads read the application's source bytes after this returns,
  // which Vulkan allows: the application may not write them between the
  // submit and the copy's completion. Unmapping or freeing that memory waits
  // for the decodes still reading it.
  //
  // Returns `{VK_NULL_HANDLE, 0}` when there was nothing to decode, when
  // `allow_async` is false, or when the device has no timeline semaphore. In
  // those cases every upload is already decoded when this returns. It also
  // returns no semaphore when the uploads only keep ETC2 blocks for later
  // copies, which the GPU never reads.
  Etc2SubmitWait PrepareSubmit(const VkCommandBuffer* command_buffers,
                               std::uint32_t count, bool allow_async = false,
                               VkQueue queue = VK_NULL_HANDLE);
  // Destroys the staging buffers owned by a command buffer that is no longer
  // pending, after waiting for any decode still writing into them.
  void ReleaseCommandBuffer(VkCommandBuffer command_buffer);

  // Traced as the "emulated texture (MB)" and "compressed texture (MB)"
  // counters.
  EmulatedTextureBytes TextureBytes() const;
  // The startup speed probe's result; zero when it did not run.
  Etc2GpuSpeed GpuSpeed(VkDevice device) const;

  struct State;

 private:
  State* state_;
};

}  // namespace mocktail::graphics

#endif  // MOCKTAIL_GRAPHICS_VULKAN_ETC2_EMULATION_H_
