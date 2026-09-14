#include "mocktail/graphics/vulkan_etc2_emulation.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mocktail::graphics {

bool LookupEmulatedEtc2Format(VkFormat format, Etc2EmulatedFormat* out) {
  Etc2EmulatedFormat mapped;
  switch (format) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_UNORM, EtcFormat::kEtc2Rgb8};
      break;
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_SRGB, EtcFormat::kEtc2Rgb8};
      break;
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_UNORM, EtcFormat::kEtc2Rgb8A1};
      break;
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_SRGB, EtcFormat::kEtc2Rgb8A1};
      break;
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_UNORM, EtcFormat::kEtc2Rgba8};
      break;
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_SRGB, EtcFormat::kEtc2Rgba8};
      break;
    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
      mapped = {VK_FORMAT_R16_UNORM, EtcFormat::kEacR11};
      break;
    case VK_FORMAT_EAC_R11_SNORM_BLOCK:
      mapped = {VK_FORMAT_R16_SNORM, EtcFormat::kEacR11Signed};
      break;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
      mapped = {VK_FORMAT_R16G16_UNORM, EtcFormat::kEacRg11};
      break;
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
      mapped = {VK_FORMAT_R16G16_SNORM, EtcFormat::kEacRg11Signed};
      break;
    default:
      return false;
  }
  if (out != nullptr) {
    *out = mapped;
  }
  return true;
}

VkFormatFeatureFlags EmulatedEtc2FormatFeatures(
    VkFormatFeatureFlags host_features) {
  constexpr VkFormatFeatureFlags kCompressedFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  return host_features & kCompressedFeatures;
}

bool Etc2UploadByteCounts(EtcFormat format, const VkExtent3D& extent,
                          std::uint32_t layer_count, VkDeviceSize* compressed,
                          VkDeviceSize* decoded) {
  if (extent.width == 0 || extent.height == 0 || extent.depth != 1 ||
      layer_count == 0 || compressed == nullptr || decoded == nullptr) {
    return false;
  }
  const VkDeviceSize blocks =
      ((static_cast<VkDeviceSize>(extent.width) + 3) / 4) *
      ((static_cast<VkDeviceSize>(extent.height) + 3) / 4);
  *compressed = blocks * EtcBlockBytes(format) * layer_count;
  *decoded = static_cast<VkDeviceSize>(extent.width) * extent.height *
             EtcDecodedTexelBytes(format) * layer_count;
  return true;
}

namespace {

bool EmulationDisabledByEnvironment() {
  static const bool disabled = [] {
    const char* value = std::getenv("MOCKTAIL_DISABLE_ETC2_EMULATION");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
  }();
  return disabled;
}

bool ShouldLog(std::atomic<unsigned>* counter, unsigned limit) {
  return counter->fetch_add(1, std::memory_order_relaxed) < limit;
}

// Decode threads for one submit, the submitting thread included. Half the
// host's hardware threads, at most 8, leave cores for the game's own threads.
unsigned DecodeWorkerCount() {
  static const unsigned count =
      std::min(8U, std::max(1U, std::thread::hardware_concurrency() / 2));
  return count;
}

struct HostDevice {
  VkDevice device = VK_NULL_HANDLE;
  bool emulated = false;
  VkPhysicalDeviceMemoryProperties memory{};
  PFN_vkCreateImage create_image = nullptr;
  PFN_vkDestroyImage destroy_image = nullptr;
  PFN_vkCreateImageView create_image_view = nullptr;
  PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
  PFN_vkBindBufferMemory2 bind_buffer_memory2 = nullptr;
  PFN_vkMapMemory map_memory = nullptr;
  PFN_vkMapMemory2 map_memory2 = nullptr;
  PFN_vkUnmapMemory unmap_memory = nullptr;
  PFN_vkUnmapMemory2 unmap_memory2 = nullptr;
  PFN_vkFreeMemory free_memory = nullptr;
  PFN_vkDestroyBuffer destroy_buffer = nullptr;
  PFN_vkCmdCopyBufferToImage copy_buffer_to_image = nullptr;
  PFN_vkCmdCopyBufferToImage2 copy_buffer_to_image2 = nullptr;
  PFN_vkCmdExecuteCommands execute_commands = nullptr;
  PFN_vkCreateBuffer create_buffer = nullptr;
  PFN_vkAllocateMemory allocate_memory = nullptr;
  PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
};

template <typename Function>
Function DeviceProc(PFN_vkGetDeviceProcAddr get, VkDevice device,
                    const char* name, const char* alias = nullptr) {
  PFN_vkVoidFunction proc = get(device, name);
  if (proc == nullptr && alias != nullptr) {
    proc = get(device, alias);
  }
  return reinterpret_cast<Function>(proc);
}

struct BufferBinding {
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
};

struct Mapping {
  void* data = nullptr;
  VkDeviceSize offset = 0;
  VkDeviceSize size = VK_WHOLE_SIZE;
};

struct ImageRecord {
  VkDevice device = VK_NULL_HANDLE;
  Etc2EmulatedFormat format;
};

struct Staging {
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct PendingUpload {
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer source = VK_NULL_HANDLE;
  VkDeviceSize source_offset = 0;
  VkDeviceSize compressed = 0;
  VkDeviceSize decoded = 0;
  VkDeviceSize target_offset = 0;
  EtcFormat format = EtcFormat::kEtc2Rgb8;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t layers = 0;
  std::uint8_t* target = nullptr;
};

struct CommandRecord {
  std::vector<PendingUpload> uploads;
  std::vector<Staging> staging;
  std::vector<VkCommandBuffer> secondaries;
};

std::atomic<unsigned> g_image_logs{0};
std::atomic<unsigned> g_decode_logs{0};
std::atomic<unsigned> g_failure_logs{0};

void LogFailure(const char* message) {
  if (ShouldLog(&g_failure_logs, 8)) {
    std::fprintf(stderr, "  [vulkan] ETC2 emulation: %s\n", message);
  }
}

bool CreateStaging(const HostDevice& dev, VkDeviceSize size, Staging* staging,
                   std::uint8_t** mapped) {
  if (dev.create_buffer == nullptr || dev.allocate_memory == nullptr ||
      dev.get_buffer_memory_requirements == nullptr ||
      dev.bind_buffer_memory == nullptr || dev.map_memory == nullptr ||
      dev.destroy_buffer == nullptr || dev.free_memory == nullptr ||
      size == 0) {
    return false;
  }
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  if (dev.create_buffer(dev.device, &buffer_info, nullptr, &buffer) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements{};
  dev.get_buffer_memory_requirements(dev.device, buffer, &requirements);
  constexpr VkMemoryPropertyFlags kRequired =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  std::uint32_t type = UINT32_MAX;
  for (std::uint32_t index = 0; index < dev.memory.memoryTypeCount; ++index) {
    if ((requirements.memoryTypeBits & (1U << index)) != 0 &&
        (dev.memory.memoryTypes[index].propertyFlags & kRequired) ==
            kRequired) {
      type = index;
      break;
    }
  }
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkMemoryAllocateInfo allocate_info{};
  allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = type;
  void* data = nullptr;
  if (type == UINT32_MAX ||
      dev.allocate_memory(dev.device, &allocate_info, nullptr, &memory) !=
          VK_SUCCESS) {
    dev.destroy_buffer(dev.device, buffer, nullptr);
    return false;
  }
  if (dev.bind_buffer_memory(dev.device, buffer, memory, 0) != VK_SUCCESS ||
      dev.map_memory(dev.device, memory, 0, VK_WHOLE_SIZE, 0, &data) !=
          VK_SUCCESS) {
    dev.destroy_buffer(dev.device, buffer, nullptr);
    dev.free_memory(dev.device, memory, nullptr);
    return false;
  }
  *staging = {dev.device, buffer, memory};
  *mapped = static_cast<std::uint8_t*>(data);
  return true;
}

// Freeing mapped memory unmaps it implicitly.
void DestroyStaging(const HostDevice& dev, const Staging& staging) {
  if (dev.destroy_buffer != nullptr) {
    dev.destroy_buffer(dev.device, staging.buffer, nullptr);
  }
  if (dev.free_memory != nullptr) {
    dev.free_memory(dev.device, staging.memory, nullptr);
  }
}

template <typename Region>
bool PlanRegions(const Etc2EmulatedFormat& format, VkDevice device,
                 VkBuffer source, std::uint32_t count, const Region* regions,
                 std::vector<Region>* rewritten,
                 std::vector<PendingUpload>* uploads, VkDeviceSize* total) {
  *total = 0;
  if (regions == nullptr || count == 0) {
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    const Region& region = regions[index];
    PendingUpload upload;
    if (!Etc2UploadByteCounts(format.etc_format, region.imageExtent,
                              region.imageSubresource.layerCount,
                              &upload.compressed, &upload.decoded)) {
      return false;
    }
    upload.device = device;
    upload.source = source;
    upload.source_offset = region.bufferOffset;
    upload.target_offset = *total;
    upload.format = format.etc_format;
    upload.width = region.imageExtent.width;
    upload.height = region.imageExtent.height;
    upload.layers = region.imageSubresource.layerCount;
    Region copy = region;
    copy.bufferOffset = *total;
    rewritten->push_back(copy);
    uploads->push_back(upload);
    *total += upload.decoded;
  }
  return true;
}

}  // namespace

struct VulkanEtc2Emulation::State {
  std::mutex mutex;
  std::mutex devices_mutex;
  std::unordered_map<VkPhysicalDevice, bool> physical_devices;
  std::vector<std::unique_ptr<HostDevice>> devices;
  std::unordered_map<VkImage, ImageRecord> images;
  std::unordered_map<VkBuffer, BufferBinding> buffers;
  std::unordered_map<VkDeviceMemory, Mapping> mappings;
  std::unordered_map<VkCommandBuffer, CommandRecord> commands;
  std::atomic<bool> has_commands{false};
  // Heap buffers reused across submits; they only grow.
  std::mutex scratch_mutex;
  std::vector<std::uint8_t> scratch_source;
  std::vector<std::uint8_t> scratch_decoded;

  const HostDevice* Find(VkDevice device) {
    std::lock_guard<std::mutex> lock(devices_mutex);
    for (const auto& candidate : devices) {
      if (candidate->device == device) {
        return candidate.get();
      }
    }
    return nullptr;
  }

  bool LookupImage(const HostDevice& dev, VkImage image,
                   Etc2EmulatedFormat* format) {
    if (!dev.emulated) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = images.find(image);
    if (found == images.end()) {
      return false;
    }
    *format = found->second.format;
    return true;
  }

  void Record(VkCommandBuffer command_buffer,
              std::vector<PendingUpload> uploads, const Staging& staging) {
    std::lock_guard<std::mutex> lock(mutex);
    CommandRecord& record = commands[command_buffer];
    record.uploads.insert(record.uploads.end(), uploads.begin(),
                          uploads.end());
    record.staging.push_back(staging);
    has_commands.store(true, std::memory_order_release);
  }
};

VulkanEtc2Emulation::VulkanEtc2Emulation() : state_(new State) {}

VulkanEtc2Emulation::~VulkanEtc2Emulation() { delete state_; }

bool VulkanEtc2Emulation::PhysicalDeviceNeedsEmulation(
    VkPhysicalDevice physical_device, PFN_vkGetPhysicalDeviceFeatures host) {
  if (EmulationDisabledByEnvironment() || host == nullptr ||
      physical_device == VK_NULL_HANDLE) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->physical_devices.find(physical_device);
    if (found != state_->physical_devices.end()) {
      return found->second;
    }
  }
  VkPhysicalDeviceFeatures features{};
  host(physical_device, &features);
  const bool needed = features.textureCompressionETC2 != VK_TRUE;
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->physical_devices[physical_device] = needed;
  return needed;
}

void VulkanEtc2Emulation::RegisterDevice(
    VkDevice device, VkPhysicalDevice physical_device, bool emulated,
    const VkPhysicalDeviceMemoryProperties& memory,
    PFN_vkGetDeviceProcAddr get_device_proc_addr) {
  static_cast<void>(physical_device);
  if (device == VK_NULL_HANDLE || get_device_proc_addr == nullptr) {
    return;
  }
  auto dev = std::make_unique<HostDevice>();
  const auto get = get_device_proc_addr;
  dev->device = device;
  dev->emulated = emulated;
  dev->memory = memory;
  dev->create_image = DeviceProc<PFN_vkCreateImage>(get, device, "vkCreateImage");
  dev->destroy_image =
      DeviceProc<PFN_vkDestroyImage>(get, device, "vkDestroyImage");
  dev->create_image_view =
      DeviceProc<PFN_vkCreateImageView>(get, device, "vkCreateImageView");
  dev->bind_buffer_memory =
      DeviceProc<PFN_vkBindBufferMemory>(get, device, "vkBindBufferMemory");
  dev->bind_buffer_memory2 = DeviceProc<PFN_vkBindBufferMemory2>(
      get, device, "vkBindBufferMemory2", "vkBindBufferMemory2KHR");
  dev->map_memory = DeviceProc<PFN_vkMapMemory>(get, device, "vkMapMemory");
  dev->map_memory2 = DeviceProc<PFN_vkMapMemory2>(get, device, "vkMapMemory2",
                                                  "vkMapMemory2KHR");
  dev->unmap_memory =
      DeviceProc<PFN_vkUnmapMemory>(get, device, "vkUnmapMemory");
  dev->unmap_memory2 = DeviceProc<PFN_vkUnmapMemory2>(
      get, device, "vkUnmapMemory2", "vkUnmapMemory2KHR");
  dev->free_memory = DeviceProc<PFN_vkFreeMemory>(get, device, "vkFreeMemory");
  dev->destroy_buffer =
      DeviceProc<PFN_vkDestroyBuffer>(get, device, "vkDestroyBuffer");
  dev->copy_buffer_to_image = DeviceProc<PFN_vkCmdCopyBufferToImage>(
      get, device, "vkCmdCopyBufferToImage");
  dev->copy_buffer_to_image2 = DeviceProc<PFN_vkCmdCopyBufferToImage2>(
      get, device, "vkCmdCopyBufferToImage2", "vkCmdCopyBufferToImage2KHR");
  dev->execute_commands =
      DeviceProc<PFN_vkCmdExecuteCommands>(get, device, "vkCmdExecuteCommands");
  dev->create_buffer =
      DeviceProc<PFN_vkCreateBuffer>(get, device, "vkCreateBuffer");
  dev->allocate_memory =
      DeviceProc<PFN_vkAllocateMemory>(get, device, "vkAllocateMemory");
  dev->get_buffer_memory_requirements =
      DeviceProc<PFN_vkGetBufferMemoryRequirements>(
          get, device, "vkGetBufferMemoryRequirements");
  if (emulated) {
    std::fprintf(stderr,
                 "  [vulkan] ETC2/EAC emulation enabled: compressed uploads "
                 "decode to RGBA8/R16 host images\n");
  }
  std::lock_guard<std::mutex> lock(state_->devices_mutex);
  state_->devices.erase(
      std::remove_if(state_->devices.begin(), state_->devices.end(),
                     [device](const std::unique_ptr<HostDevice>& candidate) {
                       return candidate->device == device;
                     }),
      state_->devices.end());
  state_->devices.push_back(std::move(dev));
}

void VulkanEtc2Emulation::DestroyDevice(VkDevice device) {
  std::vector<Staging> doomed;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (auto it = state_->commands.begin(); it != state_->commands.end();) {
      const bool owned = std::any_of(
          it->second.staging.begin(), it->second.staging.end(),
          [device](const Staging& staging) { return staging.device == device; });
      if (owned) {
        doomed.insert(doomed.end(), it->second.staging.begin(),
                      it->second.staging.end());
        it = state_->commands.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = state_->images.begin(); it != state_->images.end();) {
      it = it->second.device == device ? state_->images.erase(it) : std::next(it);
    }
    state_->has_commands.store(!state_->commands.empty(),
                               std::memory_order_release);
  }
  if (const HostDevice* dev = state_->Find(device); dev != nullptr) {
    for (const Staging& staging : doomed) {
      DestroyStaging(*dev, staging);
    }
  }
  std::lock_guard<std::mutex> lock(state_->devices_mutex);
  state_->devices.erase(
      std::remove_if(state_->devices.begin(), state_->devices.end(),
                     [device](const std::unique_ptr<HostDevice>& candidate) {
                       return candidate->device == device;
                     }),
      state_->devices.end());
}

VkResult VulkanEtc2Emulation::CreateImage(VkDevice device,
                                          const VkImageCreateInfo* create_info,
                                          const VkAllocationCallbacks* allocator,
                                          VkImage* image) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->create_image == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  Etc2EmulatedFormat format;
  if (!dev->emulated || create_info == nullptr ||
      !LookupEmulatedEtc2Format(create_info->format, &format)) {
    return dev->create_image(device, create_info, allocator, image);
  }
  VkImageCreateInfo host_info = *create_info;
  host_info.format = format.host_format;
  host_info.flags &= ~static_cast<VkImageCreateFlags>(
      VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT);
  const VkResult result = dev->create_image(device, &host_info, allocator, image);
  if (result == VK_SUCCESS && image != nullptr) {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->images[*image] = {device, format};
    }
    if (ShouldLog(&g_image_logs, 4)) {
      std::fprintf(stderr,
                   "  [vulkan] ETC2 image %ux%u mips=%u format=%d -> host "
                   "format=%d\n",
                   create_info->extent.width, create_info->extent.height,
                   create_info->mipLevels,
                   static_cast<int>(create_info->format),
                   static_cast<int>(format.host_format));
    }
  }
  return result;
}

void VulkanEtc2Emulation::DestroyImage(VkDevice device, VkImage image,
                                       const VkAllocationCallbacks* allocator) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->destroy_image == nullptr) {
    return;
  }
  if (dev->emulated) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->images.erase(image);
  }
  dev->destroy_image(device, image, allocator);
}

VkResult VulkanEtc2Emulation::CreateImageView(
    VkDevice device, const VkImageViewCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImageView* view) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->create_image_view == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  Etc2EmulatedFormat format;
  if (!dev->emulated || create_info == nullptr ||
      !LookupEmulatedEtc2Format(create_info->format, &format)) {
    return dev->create_image_view(device, create_info, allocator, view);
  }
  VkImageViewCreateInfo host_info = *create_info;
  host_info.format = format.host_format;
  return dev->create_image_view(device, &host_info, allocator, view);
}

VkResult VulkanEtc2Emulation::BindBufferMemory(VkDevice device, VkBuffer buffer,
                                               VkDeviceMemory memory,
                                               VkDeviceSize offset) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->bind_buffer_memory == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      dev->bind_buffer_memory(device, buffer, memory, offset);
  if (result == VK_SUCCESS && dev->emulated) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->buffers[buffer] = {memory, offset};
  }
  return result;
}

VkResult VulkanEtc2Emulation::BindBufferMemory2(
    VkDevice device, std::uint32_t count, const VkBindBufferMemoryInfo* infos) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->bind_buffer_memory2 == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = dev->bind_buffer_memory2(device, count, infos);
  if (result == VK_SUCCESS && dev->emulated && infos != nullptr) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (std::uint32_t index = 0; index < count; ++index) {
      state_->buffers[infos[index].buffer] = {infos[index].memory,
                                              infos[index].memoryOffset};
    }
  }
  return result;
}

VkResult VulkanEtc2Emulation::MapMemory(VkDevice device, VkDeviceMemory memory,
                                        VkDeviceSize offset, VkDeviceSize size,
                                        VkMemoryMapFlags flags, void** data) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->map_memory == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      dev->map_memory(device, memory, offset, size, flags, data);
  if (result == VK_SUCCESS && dev->emulated && data != nullptr) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->mappings[memory] = {*data, offset, size};
  }
  return result;
}

VkResult VulkanEtc2Emulation::MapMemory2(VkDevice device,
                                         const VkMemoryMapInfo* info,
                                         void** data) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->map_memory2 == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = dev->map_memory2(device, info, data);
  if (result == VK_SUCCESS && dev->emulated && info != nullptr &&
      data != nullptr) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->mappings[info->memory] = {*data, info->offset, info->size};
  }
  return result;
}

void VulkanEtc2Emulation::UnmapMemory(VkDevice device, VkDeviceMemory memory) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->unmap_memory == nullptr) {
    return;
  }
  if (dev->emulated) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->mappings.erase(memory);
  }
  dev->unmap_memory(device, memory);
}

VkResult VulkanEtc2Emulation::UnmapMemory2(VkDevice device,
                                           const VkMemoryUnmapInfo* info) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->unmap_memory2 == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (dev->emulated && info != nullptr) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->mappings.erase(info->memory);
  }
  return dev->unmap_memory2(device, info);
}

void VulkanEtc2Emulation::FreeMemory(VkDevice device, VkDeviceMemory memory,
                                     const VkAllocationCallbacks* allocator) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->free_memory == nullptr) {
    return;
  }
  if (dev->emulated) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->mappings.erase(memory);
  }
  dev->free_memory(device, memory, allocator);
}

void VulkanEtc2Emulation::DestroyBuffer(VkDevice device, VkBuffer buffer,
                                        const VkAllocationCallbacks* allocator) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->destroy_buffer == nullptr) {
    return;
  }
  if (dev->emulated) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->buffers.erase(buffer);
  }
  dev->destroy_buffer(device, buffer, allocator);
}

void VulkanEtc2Emulation::CmdCopyBufferToImage(
    VkDevice device, VkCommandBuffer command_buffer, VkBuffer source,
    VkImage destination, VkImageLayout layout, std::uint32_t region_count,
    const VkBufferImageCopy* regions) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_buffer_to_image == nullptr) {
    return;
  }
  Etc2EmulatedFormat format;
  if (!state_->LookupImage(*dev, destination, &format)) {
    dev->copy_buffer_to_image(command_buffer, source, destination, layout,
                              region_count, regions);
    return;
  }
  std::vector<VkBufferImageCopy> rewritten;
  std::vector<PendingUpload> uploads;
  VkDeviceSize total = 0;
  Staging staging;
  std::uint8_t* mapped = nullptr;
  if (!PlanRegions(format, device, source, region_count, regions, &rewritten,
                   &uploads, &total)) {
    LogFailure("skipped an upload with an unsupported region");
    return;
  }
  if (!CreateStaging(*dev, total, &staging, &mapped)) {
    LogFailure("could not allocate a host-visible staging buffer");
    return;
  }
  for (PendingUpload& upload : uploads) {
    upload.target = mapped + upload.target_offset;
  }
  state_->Record(command_buffer, std::move(uploads), staging);
  dev->copy_buffer_to_image(command_buffer, staging.buffer, destination,
                            layout, region_count, rewritten.data());
}

void VulkanEtc2Emulation::CmdCopyBufferToImage2(
    VkDevice device, VkCommandBuffer command_buffer,
    const VkCopyBufferToImageInfo2* info) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_buffer_to_image2 == nullptr ||
      info == nullptr) {
    return;
  }
  Etc2EmulatedFormat format;
  if (!state_->LookupImage(*dev, info->dstImage, &format)) {
    dev->copy_buffer_to_image2(command_buffer, info);
    return;
  }
  std::vector<VkBufferImageCopy2> rewritten;
  std::vector<PendingUpload> uploads;
  VkDeviceSize total = 0;
  Staging staging;
  std::uint8_t* mapped = nullptr;
  if (!PlanRegions(format, device, info->srcBuffer, info->regionCount,
                   info->pRegions, &rewritten, &uploads, &total)) {
    LogFailure("skipped an upload with an unsupported region");
    return;
  }
  if (!CreateStaging(*dev, total, &staging, &mapped)) {
    LogFailure("could not allocate a host-visible staging buffer");
    return;
  }
  for (PendingUpload& upload : uploads) {
    upload.target = mapped + upload.target_offset;
  }
  state_->Record(command_buffer, std::move(uploads), staging);
  VkCopyBufferToImageInfo2 host_info = *info;
  host_info.srcBuffer = staging.buffer;
  host_info.pRegions = rewritten.data();
  dev->copy_buffer_to_image2(command_buffer, &host_info);
}

void VulkanEtc2Emulation::CmdExecuteCommands(
    VkDevice device, VkCommandBuffer command_buffer, std::uint32_t count,
    const VkCommandBuffer* secondaries) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->execute_commands == nullptr) {
    return;
  }
  if (dev->emulated && secondaries != nullptr &&
      state_->has_commands.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (std::uint32_t index = 0; index < count; ++index) {
      if (state_->commands.count(secondaries[index]) != 0) {
        state_->commands[command_buffer].secondaries.push_back(
            secondaries[index]);
      }
    }
  }
  dev->execute_commands(command_buffer, count, secondaries);
}

void VulkanEtc2Emulation::PrepareSubmit(const VkCommandBuffer* command_buffers,
                                        std::uint32_t count) {
  if (command_buffers == nullptr || count == 0 ||
      !state_->has_commands.load(std::memory_order_acquire)) {
    return;
  }
  struct Work {
    PendingUpload upload;
    std::uint8_t* source = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memory_offset = 0;
  };
  std::vector<Work> work;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto collect = [this, &work](VkCommandBuffer command_buffer) {
      const auto record = state_->commands.find(command_buffer);
      if (record == state_->commands.end()) {
        return;
      }
      for (const PendingUpload& upload : record->second.uploads) {
        const auto binding = state_->buffers.find(upload.source);
        if (binding == state_->buffers.end()) {
          LogFailure("upload source buffer has no tracked memory binding");
          continue;
        }
        Work item;
        item.upload = upload;
        const VkDeviceSize absolute =
            binding->second.offset + upload.source_offset;
        const auto mapping = state_->mappings.find(binding->second.memory);
        if (mapping == state_->mappings.end()) {
          item.memory = binding->second.memory;
          item.memory_offset = absolute;
        } else if (absolute >= mapping->second.offset &&
                   (mapping->second.size == VK_WHOLE_SIZE ||
                    (absolute - mapping->second.offset <=
                         mapping->second.size &&
                     upload.compressed <= mapping->second.size -
                                              (absolute -
                                               mapping->second.offset)))) {
          item.source = static_cast<std::uint8_t*>(mapping->second.data) +
                        (absolute - mapping->second.offset);
        } else {
          LogFailure("upload source lies outside the mapped range");
          continue;
        }
        work.push_back(item);
      }
    };
    for (std::uint32_t index = 0; index < count; ++index) {
      collect(command_buffers[index]);
      const auto record = state_->commands.find(command_buffers[index]);
      if (record != state_->commands.end()) {
        const std::vector<VkCommandBuffer> secondaries =
            record->second.secondaries;
        for (VkCommandBuffer secondary : secondaries) {
          collect(secondary);
        }
      }
    }
  }
  // Mapped Vulkan memory is uncached or write-combined, which makes the
  // decoder's scattered byte reads and writes tens of times slower than on
  // heap memory. Compressed bytes are streamed into a heap scratch buffer,
  // decoded heap to heap, and the result streamed into the staging buffer.
  // Vulkan forbids mapping one memory object twice, so each unmapped source
  // memory is mapped once for the whole batch and unmapped after copying.
  struct TemporaryMapping {
    const HostDevice* dev = nullptr;
    std::uint8_t* data = nullptr;
  };
  std::unordered_map<VkDeviceMemory, TemporaryMapping> temporary;
  struct Copy {
    const std::uint8_t* source = nullptr;
    VkDeviceSize compressed = 0;
    VkDeviceSize decoded = 0;
  };
  std::vector<Copy> copies;
  std::vector<const PendingUpload*> copied_uploads;
  VkDeviceSize compressed_total = 0;
  VkDeviceSize decoded_total = 0;
  for (const Work& item : work) {
    const PendingUpload& upload = item.upload;
    const std::uint8_t* source = item.source;
    if (source == nullptr) {
      auto found = temporary.find(item.memory);
      if (found == temporary.end()) {
        const HostDevice* dev = state_->Find(upload.device);
        void* data = nullptr;
        if (dev == nullptr || dev->map_memory == nullptr ||
            dev->map_memory(dev->device, item.memory, 0, VK_WHOLE_SIZE, 0,
                            &data) != VK_SUCCESS) {
          LogFailure("could not map an unmapped upload source");
          continue;
        }
        found = temporary
                    .emplace(item.memory,
                             TemporaryMapping{
                                 dev, static_cast<std::uint8_t*>(data)})
                    .first;
      }
      source = found->second.data + item.memory_offset;
    }
    copies.push_back({source, upload.compressed, upload.decoded});
    copied_uploads.push_back(&upload);
    compressed_total += upload.compressed;
    decoded_total += upload.decoded;
  }

  std::lock_guard<std::mutex> scratch_lock(state_->scratch_mutex);
  std::vector<std::uint8_t>& scratch_source = state_->scratch_source;
  std::vector<std::uint8_t>& scratch_decoded = state_->scratch_decoded;
  if (scratch_source.size() < compressed_total) {
    scratch_source.resize(compressed_total);
  }
  if (scratch_decoded.size() < decoded_total) {
    scratch_decoded.resize(decoded_total);
  }
  std::vector<EtcDecodeJob> jobs;
  VkDeviceSize compressed_offset = 0;
  VkDeviceSize decoded_offset = 0;
  for (std::size_t index = 0; index < copies.size(); ++index) {
    const Copy& copy = copies[index];
    const PendingUpload& upload = *copied_uploads[index];
    std::uint8_t* source = scratch_source.data() + compressed_offset;
    std::uint8_t* decoded = scratch_decoded.data() + decoded_offset;
    std::memcpy(source, copy.source, copy.compressed);
    const VkDeviceSize compressed_layer = upload.compressed / upload.layers;
    const VkDeviceSize decoded_layer = upload.decoded / upload.layers;
    for (std::uint32_t layer = 0; layer < upload.layers; ++layer) {
      EtcDecodeJob job;
      job.format = upload.format;
      job.source = source + layer * compressed_layer;
      job.source_bytes = compressed_layer;
      job.width = upload.width;
      job.height = upload.height;
      job.destination = decoded + layer * decoded_layer;
      job.destination_bytes = decoded_layer;
      jobs.push_back(job);
    }
    compressed_offset += copy.compressed;
    decoded_offset += copy.decoded;
    if (ShouldLog(&g_decode_logs, 4)) {
      std::fprintf(stderr,
                   "  [vulkan] ETC2 upload decoded %ux%u layers=%u\n",
                   upload.width, upload.height, upload.layers);
    }
  }
  for (const auto& [memory, mapping] : temporary) {
    if (mapping.dev->unmap_memory != nullptr) {
      mapping.dev->unmap_memory(mapping.dev->device, memory);
    }
  }
  DecodeEtcJobs(jobs.data(), jobs.size(), DecodeWorkerCount());
  for (const EtcDecodeJob& job : jobs) {
    if (!job.ok) {
      LogFailure("could not decode an upload layer");
    }
  }
  decoded_offset = 0;
  for (const PendingUpload* upload : copied_uploads) {
    std::memcpy(upload->target, scratch_decoded.data() + decoded_offset,
                upload->decoded);
    decoded_offset += upload->decoded;
  }
}

void VulkanEtc2Emulation::ReleaseCommandBuffer(VkCommandBuffer command_buffer) {
  if (!state_->has_commands.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<Staging> doomed;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto record = state_->commands.find(command_buffer);
    if (record == state_->commands.end()) {
      return;
    }
    doomed = std::move(record->second.staging);
    state_->commands.erase(record);
    state_->has_commands.store(!state_->commands.empty(),
                               std::memory_order_release);
  }
  for (const Staging& staging : doomed) {
    if (const HostDevice* dev = state_->Find(staging.device); dev != nullptr) {
      DestroyStaging(*dev, staging);
    }
  }
}

}  // namespace mocktail::graphics
