#include "mocktail/vr/vr_mirror.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace mocktail::vr {
namespace {

// Seqlock writers/reader fences: the header is plain shared memory, so
// ordering between the sequence counter and the payload is enforced here.
void StoreFence() { std::atomic_thread_fence(std::memory_order_release); }

std::uint32_t FindHostVisibleMemoryType(const MirrorVkProcs& vk,
                                          VkPhysicalDevice physical,
                                          std::uint32_t type_bits) {
  VkPhysicalDeviceMemoryProperties properties{};
  vk.GetPhysicalDeviceMemoryProperties(physical, &properties);
  const auto required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  // Prefer HOST_CACHED system memory: CPU reads of device-local BAR1 memory
  // cost ~10ms per 2 MB pair on this GPU, while cached RAM readback is ~1ms.
  for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
    if ((type_bits & (1u << index)) != 0 &&
        (properties.memoryTypes[index].propertyFlags &
         (required | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
            (required | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
      return index;
    }
  }
  for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
    if ((type_bits & (1u << index)) != 0 &&
        (properties.memoryTypes[index].propertyFlags & required) == required) {
      return index;
    }
  }
  return UINT32_MAX;
}

}  // namespace

VrMirror::~VrMirror() { Close(); }

bool VrMirror::Open(const std::string& shm_name, std::uint32_t width,
                    std::uint32_t height, std::string* error) {
  if (mapping_ != nullptr) {
    const auto* header = static_cast<const VrMirrorHeader*>(mapping_);
    if (shm_name_ == shm_name && header->width == width && header->height == height) {
      return true;
    }
    Close();
  }
  if (shm_name.empty() || shm_name[0] != '/' || width == 0 || height == 0) {
    if (error != nullptr) {
      *error = "invalid mirror segment name or extent";
    }
    return false;
  }
  const std::uint64_t bytes = VrMirrorBytes(width, height);
  const int descriptor =
      shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    if (error != nullptr) {
      *error = "shm_open failed: " + std::string(std::strerror(errno));
    }
    return false;
  }
  if (ftruncate(descriptor, static_cast<off_t>(bytes)) != 0) {
    if (error != nullptr) {
      *error = "ftruncate failed: " + std::string(std::strerror(errno));
    }
    close(descriptor);
    (void)shm_unlink(shm_name.c_str());
    return false;
  }
  void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                       descriptor, 0);
  close(descriptor);
  if (mapping == MAP_FAILED) {
    if (error != nullptr) {
      *error = "mmap failed: " + std::string(std::strerror(errno));
    }
    (void)shm_unlink(shm_name.c_str());
    return false;
  }
  mapping_ = mapping;
  mapping_bytes_ = bytes;
  shm_name_ = shm_name;
  auto* header = static_cast<VrMirrorHeader*>(mapping_);
  std::memset(header, 0, sizeof(*header));
  header->magic = kVrMirrorMagic;
  header->header_bytes = kVrMirrorHeaderBytes;
  header->width = width;
  header->height = height;
  StoreFence();
  return true;
}

void VrMirror::Close() {
  if (mapping_ != nullptr) {
    munmap(mapping_, mapping_bytes_);
    mapping_ = nullptr;
    mapping_bytes_ = 0;
  }
  if (!shm_name_.empty()) {
    (void)shm_unlink(shm_name_.c_str());
    shm_name_.clear();
  }
  published_pairs_ = 0;
}

bool VrMirror::EnsureStaging(const MirrorVkProcs& vk, VkDevice device,
                             VkPhysicalDevice physical, std::uint32_t width,
                             std::uint32_t height, std::string* error) {
  if (staging_device_ == device && staging_[0] != VK_NULL_HANDLE &&
      staging_[1] != VK_NULL_HANDLE && staging_width_ == width &&
      staging_height_ == height) {
    return true;
  }
  DestroyStaging(vk);
  staging_device_ = device;  // Also owns partially constructed allocations.
  const VkDeviceSize size =
      static_cast<VkDeviceSize>(width) * height * 4u;
  VkPhysicalDeviceMemoryProperties properties{};
  vk.GetPhysicalDeviceMemoryProperties(physical, &properties);
  for (int eye = 0; eye < 2; ++eye) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vk.CreateBuffer(device, &buffer_info, nullptr, &staging_[eye]) !=
        VK_SUCCESS) {
      if (error != nullptr) {
        *error = "mirror staging buffer creation failed";
      }
      DestroyStaging(vk);
      return false;
    }
    VkMemoryRequirements requirements{};
    vk.GetBufferMemoryRequirements(device, staging_[eye], &requirements);
    const std::uint32_t type =
        FindHostVisibleMemoryType(vk, physical, requirements.memoryTypeBits);
    if (type == UINT32_MAX) {
      if (error != nullptr) {
        *error = "no host-visible coherent memory for the mirror";
      }
      DestroyStaging(vk);
      return false;
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (vk.AllocateMemory(device, &allocation, nullptr,
                          &staging_memory_[eye]) != VK_SUCCESS) {
      if (error != nullptr) {
        *error = "mirror staging allocation failed";
      }
      DestroyStaging(vk);
      return false;
    }
    if (vk.BindBufferMemory(device, staging_[eye], staging_memory_[eye], 0) !=
            VK_SUCCESS ||
        vk.MapMemory(device, staging_memory_[eye], 0, size, 0,
                     &staging_map_[eye]) != VK_SUCCESS) {
      if (error != nullptr) {
        *error = "mirror staging bind/map failed";
      }
      DestroyStaging(vk);
      return false;
    }
  }
  staging_device_ = device;
  staging_width_ = width;
  staging_height_ = height;
  return true;
}

void VrMirror::DestroyStaging(const MirrorVkProcs& vk) {
  for (int eye = 0; eye < 2; ++eye) {
    if (staging_map_[eye] != nullptr && staging_memory_[eye] != VK_NULL_HANDLE) {
      vk.UnmapMemory(staging_device_, staging_memory_[eye]);
      staging_map_[eye] = nullptr;
    }
    if (staging_memory_[eye] != VK_NULL_HANDLE) {
      vk.FreeMemory(staging_device_, staging_memory_[eye], nullptr);
      staging_memory_[eye] = VK_NULL_HANDLE;
    }
    if (staging_[eye] != VK_NULL_HANDLE) {
      vk.DestroyBuffer(staging_device_, staging_[eye], nullptr);
      staging_[eye] = VK_NULL_HANDLE;
    }
  }
  staging_width_ = 0;
  staging_height_ = 0;
  staging_device_ = VK_NULL_HANDLE;
}

void VrMirror::RecordEyeCopy(const MirrorVkProcs& vk, VkCommandBuffer cmd,
                             int eye, VkImage source,
                             VkImageLayout /*final_layout*/,
                             std::uint32_t width, std::uint32_t height) {
  // The caller's batched command buffer already holds the source in
  // TRANSFER_SRC_OPTIMAL for the XR swapchain copy and restores its final
  // layout afterwards; recording only the copy here keeps one transition pair
  // per eye per frame.
  if (eye < 0 || eye > 1 || staging_[eye] == VK_NULL_HANDLE ||
      staging_width_ != width || staging_height_ != height) {
    return;
  }
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  vk.CmdCopyImageToBuffer(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          staging_[eye], 1, &region);
}

void VrMirror::Publish(std::uint64_t frame, VkFormat format,
                       std::uint32_t width, std::uint32_t height,
                       std::uint64_t now_ns) {
  if (mapping_ == nullptr || staging_map_[0] == nullptr ||
      staging_map_[1] == nullptr || staging_width_ != width ||
      staging_height_ != height) {
    return;
  }
  PublishFromBytesForTest(frame, format, width, height, now_ns, staging_map_[0],
                          staging_map_[1]);
}

void VrMirror::PublishFromBytesForTest(std::uint64_t frame, VkFormat format,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       std::uint64_t now_ns, const void* eye0,
                                       const void* eye1) {
  if (mapping_ == nullptr || eye0 == nullptr || eye1 == nullptr) {
    return;
  }
  auto* header = static_cast<VrMirrorHeader*>(mapping_);
  auto* pixels = static_cast<std::uint8_t*>(mapping_) + kVrMirrorHeaderBytes;
  const std::uint64_t eye_bytes =
      static_cast<std::uint64_t>(width) * height * 4u;
  if (kVrMirrorHeaderBytes + 2 * eye_bytes > mapping_bytes_) {
    return;
  }
  // Seqlock: odd sequence marks the payload unstable for readers.
  const std::uint64_t sequence = header->seq + 1;
  header->seq = sequence | 1ull;
  StoreFence();
  header->width = width;
  header->height = height;
  header->format = static_cast<std::uint32_t>(format);
  header->frame = frame;
  header->producer_ns = now_ns;
  std::memcpy(pixels, eye0, eye_bytes);
  std::memcpy(pixels + eye_bytes, eye1, eye_bytes);
  header->flags = kVrMirrorFlagValid;
  header->published_pairs = ++published_pairs_;
  StoreFence();
  header->seq = sequence + 1;
}

}  // namespace mocktail::vr
