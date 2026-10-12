#ifndef MOCKTAIL_VR_MIRROR_H_
#define MOCKTAIL_VR_MIRROR_H_

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

#include "mocktail/vr/vr_mirror_shm.h"

namespace mocktail::vr {

// The subset of device/instance procs the mirror needs, supplied by the XR
// backend from its already-resolved guest-device table. Keeping this explicit
// avoids a second proc table and keeps the mirror independently testable.
struct MirrorVkProcs {
  PFN_vkCreateBuffer CreateBuffer = nullptr;
  PFN_vkDestroyBuffer DestroyBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkBindBufferMemory BindBufferMemory = nullptr;
  PFN_vkMapMemory MapMemory = nullptr;
  PFN_vkUnmapMemory UnmapMemory = nullptr;
  PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties =
      nullptr;
};

// Producer side of the desktop two-eye mirror. Maps a POSIX shared-memory
// segment (created with the name from MOCKTAIL_VR_MIRROR_SHM) and publishes
// the latest complete eye pair under a seqlock. It owns two persistent,
// bounded host-visible staging buffers reused across frames; the GPU->staging
// copies are recorded into the caller's single batched command buffer so the
// mirror adds no extra queue submission or fence wait.
//
// The mirror never touches the filesystem on the frame path and keeps only the
// newest pair (one latest slot), so a slow consumer drops rather than queues
// old frames.
class VrMirror {
 public:
  VrMirror() = default;
  ~VrMirror();
  VrMirror(const VrMirror&) = delete;
  VrMirror& operator=(const VrMirror&) = delete;

  bool enabled() const { return mapping_ != nullptr; }

  // Opens the shared segment. Returns false (and stays disabled) when the name
  // is empty or mapping fails; the XR path then runs without a mirror.
  bool Open(const std::string& shm_name, std::uint32_t width,
            std::uint32_t height, std::string* error);
  void Close();

  // Ensures the two staging buffers match width*height*4 bytes on this device.
  // Allocates once; a size/device change recreates them. Existing contents are
  // discarded on resize.
  bool EnsureStaging(const MirrorVkProcs& vk, VkDevice device,
                     VkPhysicalDevice physical, std::uint32_t width,
                     std::uint32_t height, std::string* error);
  void DestroyStaging(const MirrorVkProcs& vk);

  // Records a source-eye -> staging-buffer copy for one eye into cmd, adding
  // the layout barriers around it. The source is returned to final_layout so
  // the next guest render pass sees it unchanged.
  void RecordEyeCopy(const MirrorVkProcs& vk, VkCommandBuffer cmd, int eye,
                     VkImage source, VkImageLayout final_layout,
                     std::uint32_t width, std::uint32_t height);

  // After the batched fence wait completed, copies both staging buffers into
  // the shared segment and bumps the seqlock with the frame id and a monotonic
  // timestamp. No-op when staging does not cover width*height.
  void Publish(std::uint64_t frame, VkFormat format, std::uint32_t width,
               std::uint32_t height, std::uint64_t now_ns);

  std::uint64_t published_pairs() const { return published_pairs_; }
  std::uint32_t staging_width() const { return staging_width_; }
  std::uint32_t staging_height() const { return staging_height_; }
  const void* mapping() const { return mapping_; }

  // Test seam: publish a pair from caller-supplied staging bytes without any
  // Vulkan resources, exercising the same seqlock/segment write as the GPU
  // path. Both buffers must hold width*height*4 bytes.
  void PublishFromBytesForTest(std::uint64_t frame, VkFormat format,
                               std::uint32_t width, std::uint32_t height,
                               std::uint64_t now_ns, const void* eye0,
                               const void* eye1);

 private:
  void* mapping_ = nullptr;
  std::uint64_t mapping_bytes_ = 0;
  std::string shm_name_;
  VkBuffer staging_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDeviceMemory staging_memory_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  void* staging_map_[2] = {nullptr, nullptr};
  std::uint32_t staging_width_ = 0;
  std::uint32_t staging_height_ = 0;
  VkDevice staging_device_ = VK_NULL_HANDLE;
  std::uint64_t published_pairs_ = 0;
};

}  // namespace mocktail::vr

#endif  // MOCKTAIL_VR_MIRROR_H_
