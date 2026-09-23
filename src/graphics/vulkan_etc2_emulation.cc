#include "mocktail/graphics/vulkan_etc2_emulation.h"

#include "mocktail/graphics/chrome_trace_writer.h"
#include "mocktail/graphics/texture_override.h"

#include <algorithm>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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

}  // namespace

bool Etc2SupportAdvertised(const char* value) {
  return value == nullptr || std::strcmp(value, "0") != 0;
}

bool Etc2SupportAdvertised() {
  static const bool advertised =
      Etc2SupportAdvertised(std::getenv("MOCKTAIL_ADVERTISE_ETC2"));
  return advertised;
}

std::uint32_t SmallTextureUpscale(const char* value) {
  if (value == nullptr || value[0] < '0' || value[0] > '9') {
    return kDefaultSmallTextureUpscale;
  }
  const long parsed = std::strtol(value, nullptr, 10);
  if (parsed < 1) {
    return 1;
  }
  return parsed > 8 ? 8 : static_cast<std::uint32_t>(parsed);
}

namespace {

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
  PFN_vkCmdCopyImage copy_image = nullptr;
  PFN_vkCmdCopyImage2 copy_image2 = nullptr;
  PFN_vkCmdBlitImage blit_image = nullptr;
  PFN_vkCmdExecuteCommands execute_commands = nullptr;
  PFN_vkCreateBuffer create_buffer = nullptr;
  PFN_vkAllocateMemory allocate_memory = nullptr;
  PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
  PFN_vkCmdPipelineBarrier pipeline_barrier = nullptr;
  PFN_vkSignalSemaphore signal_semaphore = nullptr;
  PFN_vkDestroySemaphore destroy_semaphore = nullptr;
  // Signalled by the decode dispatcher so a submit can wait for its uploads
  // on the GPU instead of on the submitting thread. Null when the device was
  // created without the timelineSemaphore feature.
  VkSemaphore timeline = VK_NULL_HANDLE;
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
  // Host image dimensions are the application's times this factor.
  std::uint32_t scale = 1;
  // Application-side level-0 extent; mip extents derive from it.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Replacement pixels chosen from the level-0 upload; smaller mip levels
  // are resampled from it.
  std::shared_ptr<const RgbaImage> override;
  // Decoded level-0 texels of a scaled image; every host mip is resampled
  // from them so the texture keeps its detail at every draw size.
  std::shared_ptr<const RgbaImage> base;
};

bool IsColorFormat(EtcFormat format) {
  return format == EtcFormat::kEtc2Rgb8 || format == EtcFormat::kEtc2Rgb8A1 ||
         format == EtcFormat::kEtc2Rgba8;
}

// A host-visible transfer buffer that stays mapped for its whole life.
// Uploads take ranges of it front to back, and once every range has been
// released it starts over from the front. Creating a buffer and destroying
// it each cost the driver a kernel call of a tenth of a millisecond or more,
// so a join's thousands of uploads share a few blocks instead.
struct StagingBlock {
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize capacity = 0;
  std::uint8_t* mapped = nullptr;
  // First byte not yet handed out, and the ranges command buffers hold.
  VkDeviceSize head = 0;
  std::size_t live = 0;
};

// One recorded copy's range of a staging block.
struct Staging {
  StagingBlock* block = nullptr;
  VkDeviceSize offset = 0;
};

// Ranges start on this boundary. It meets the bufferOffset rule of every
// host format and keeps ranges written by different threads off shared cache
// lines.
constexpr VkDeviceSize kStagingAlignment = 256;
// Empty blocks kept for reuse, counted in block sizes; the rest are
// destroyed.
constexpr VkDeviceSize kIdleStagingBlocks = 4;
// Decode batches kept for their buffers once they complete.
constexpr std::size_t kMaxSpareBatches = 8;

// A growable byte buffer that leaves new bytes uninitialized. The gather
// writes every byte it later reads, and zeroing a multi-megabyte buffer is
// itself frame-visible work.
class RawBuffer {
 public:
  void EnsureSize(std::size_t bytes) {
    if (bytes <= size_) {
      return;
    }
    // new[] on a trivial type default-initializes, so nothing is zeroed.
    data_.reset(new std::uint8_t[bytes]);
    size_ = bytes;
  }
  std::uint8_t* data() { return data_.get(); }
  std::size_t size() const { return size_; }

 private:
  std::unique_ptr<std::uint8_t[]> data_;
  std::size_t size_ = 0;
};

struct PendingUpload {
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer source = VK_NULL_HANDLE;
  VkDeviceSize source_offset = 0;
  VkDeviceSize compressed = 0;
  VkDeviceSize decoded = 0;
  // Source pitch in bytes from bufferRowLength/bufferImageHeight, and the
  // bytes the region spans from `source_offset` including that padding.
  // `compressed` stays the packed size the decoder consumes.
  VkDeviceSize source_row_stride = 0;
  VkDeviceSize source_layer_stride = 0;
  VkDeviceSize source_span = 0;
  VkDeviceSize target_offset = 0;
  EtcFormat format = EtcFormat::kEtc2Rgb8;
  VkImage image = VK_NULL_HANDLE;
  std::uint32_t mip_level = 0;
  // Application-side region; `decoded` covers width x height texels.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t layers = 0;
  std::uint32_t scale = 1;
  // Texels of the region inside the application mip (block-rounded
  // regions overhang it) and the host region they map to, which takes
  // `target_bytes`.
  std::uint32_t source_width = 0;
  std::uint32_t source_height = 0;
  std::uint32_t target_width = 0;
  std::uint32_t target_height = 0;
  // The region covers the whole application mip.
  bool full_mip = false;
  VkDeviceSize target_bytes = 0;
  std::uint8_t* target = nullptr;
};

struct CommandRecord {
  std::vector<PendingUpload> uploads;
  std::vector<Staging> staging;
  std::vector<VkCommandBuffer> secondaries;
  // Highest decode ticket dispatched for this command buffer. Its staging
  // must not be released before that ticket completes.
  std::uint64_t last_ticket = 0;
};

// One submit's worth of uploads, decoded off the submitting thread. The
// buffers and the upload list are owned so nothing in them depends on the
// caller's stack.
struct DecodeBatch {
  VkDevice device = VK_NULL_HANDLE;
  std::uint64_t ticket = 0;
  RawBuffer source;
  std::vector<std::uint8_t> decoded;
  // Bytes each buffer must hold. Both are grown on the dispatcher, since
  // sizing them on the submitting thread puts that cost back on the frame.
  // `decoded` stays a zeroed vector so a failed decode leaves defined bytes
  // rather than whatever the allocator handed back.
  VkDeviceSize source_bytes = 0;
  VkDeviceSize decoded_bytes = 0;
  std::vector<PendingUpload> uploads;
  // The application's mapped pointer for each upload, parallel to `uploads`.
  // Non-empty when the gather was deferred to the dispatcher, which Vulkan
  // allows: the application may not write these bytes between the submit and
  // the copy's completion. Unmapping or freeing that memory drains pending
  // decodes first, so the pointers stay dereferenceable.
  std::vector<const std::uint8_t*> deferred_sources;
};

std::atomic<unsigned> g_image_logs{0};
std::atomic<unsigned> g_decode_logs{0};
std::atomic<unsigned> g_failure_logs{0};

void LogFailure(const char* message) {
  if (ShouldLog(&g_failure_logs, 8)) {
    std::fprintf(stderr, "  [vulkan] ETC2 emulation: %s\n", message);
  }
}

bool CreateStagingBlock(const HostDevice& dev, VkDeviceSize size,
                        StagingBlock* block) {
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
  block->device = dev.device;
  block->buffer = buffer;
  block->memory = memory;
  block->capacity = size;
  block->mapped = static_cast<std::uint8_t*>(data);
  return true;
}

// Packs each upload's compressed bytes into `destination`, dropping the row
// and layer padding the application may have asked for. Sources are parallel
// to `uploads`.
void GatherCompressed(const std::vector<PendingUpload>& uploads,
                      const std::vector<const std::uint8_t*>& sources,
                      std::uint8_t* destination) {
  VkDeviceSize offset = 0;
  for (std::size_t index = 0; index < uploads.size(); ++index) {
    const PendingUpload& upload = uploads[index];
    const std::uint8_t* source = sources[index];
    std::uint8_t* target = destination + offset;
    const VkDeviceSize compressed_layer = upload.compressed / upload.layers;
    if (upload.source_span == upload.compressed) {
      std::memcpy(target, source, upload.compressed);
    } else {
      const VkDeviceSize block_rows =
          (static_cast<VkDeviceSize>(upload.height) + 3) / 4;
      const VkDeviceSize packed_row = compressed_layer / block_rows;
      for (std::uint32_t layer = 0; layer < upload.layers; ++layer) {
        for (VkDeviceSize row = 0; row < block_rows; ++row) {
          std::memcpy(target + layer * compressed_layer + row * packed_row,
                      source + layer * upload.source_layer_stride +
                          row * upload.source_row_stride,
                      packed_row);
        }
      }
    }
    offset += upload.compressed;
  }
}

// Makes the decode's host writes to staging visible to the transfer that
// reads them. A queue submission performs the host-to-device domain operation
// only for writes that happened before it, and an asynchronous decode writes
// after the submit, so the dependency has to be recorded explicitly.
void RecordHostWriteBarrier(const HostDevice& dev,
                            VkCommandBuffer command_buffer) {
  if (dev.pipeline_barrier == nullptr) {
    return;
  }
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  dev.pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
}

// Freeing mapped memory unmaps it implicitly.
void DestroyStagingBlock(const HostDevice& dev, const StagingBlock& block) {
  if (dev.destroy_buffer != nullptr) {
    dev.destroy_buffer(dev.device, block.buffer, nullptr);
  }
  if (dev.free_memory != nullptr) {
    dev.free_memory(dev.device, block.memory, nullptr);
  }
}

// Maps one axis of an application region at `mip` onto the host image of a
// scaled record. A region reaching the application mip's edge reaches the
// host mip's edge; block-rounded overhang is dropped rather than scaled.
// `texels` receives the application texels inside the mip.
bool ScaledAxis(std::uint32_t base, std::uint32_t scale, std::uint32_t mip,
                std::int32_t offset, std::uint32_t extent,
                std::uint32_t* texels, std::uint32_t* host_start,
                std::uint32_t* host_end) {
  const std::uint32_t app_mip = std::max<std::uint32_t>(1, base >> mip);
  const std::uint32_t host_mip =
      std::max<std::uint32_t>(1, (base * scale) >> mip);
  if (offset < 0 || static_cast<std::uint32_t>(offset) >= app_mip) {
    return false;
  }
  const std::uint32_t app_end =
      std::min(app_mip, static_cast<std::uint32_t>(offset) + extent);
  const std::uint32_t start = static_cast<std::uint32_t>(offset) * scale;
  const std::uint32_t end =
      app_end == app_mip ? host_mip : std::min(host_mip, app_end * scale);
  if (end <= start) {
    return false;
  }
  *texels = app_end - static_cast<std::uint32_t>(offset);
  *host_start = start;
  *host_end = end;
  return true;
}

// Host-side bounds of an application region on one image; unscaled images
// keep the region as given.
bool HostRegion(const ImageRecord* record, std::uint32_t mip,
                VkOffset3D offset, VkExtent3D extent, VkOffset3D* start,
                VkOffset3D* end) {
  if (record == nullptr || record->scale <= 1) {
    *start = offset;
    *end = {offset.x + static_cast<std::int32_t>(extent.width),
            offset.y + static_cast<std::int32_t>(extent.height),
            offset.z + static_cast<std::int32_t>(extent.depth)};
    return true;
  }
  std::uint32_t texels = 0;
  std::uint32_t x0 = 0;
  std::uint32_t x1 = 0;
  std::uint32_t y0 = 0;
  std::uint32_t y1 = 0;
  if (!ScaledAxis(record->width, record->scale, mip, offset.x, extent.width,
                  &texels, &x0, &x1) ||
      !ScaledAxis(record->height, record->scale, mip, offset.y,
                  extent.height, &texels, &y0, &y1)) {
    return false;
  }
  *start = {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), 0};
  *end = {static_cast<std::int32_t>(x1), static_cast<std::int32_t>(y1), 1};
  return true;
}

template <typename Region>
bool PlanRegions(const ImageRecord& record, VkDevice device, VkBuffer source,
                 VkImage destination, std::uint32_t count,
                 const Region* regions, std::vector<Region>* rewritten,
                 std::vector<PendingUpload>* uploads, VkDeviceSize* total) {
  *total = 0;
  if (regions == nullptr || count == 0) {
    return false;
  }
  const std::uint32_t scale = record.scale;
  for (std::uint32_t index = 0; index < count; ++index) {
    const Region& region = regions[index];
    PendingUpload upload;
    if (!Etc2UploadByteCounts(record.format.etc_format, region.imageExtent,
                              region.imageSubresource.layerCount,
                              &upload.compressed, &upload.decoded)) {
      return false;
    }
    if (scale > 1 && region.imageSubresource.layerCount != 1) {
      return false;
    }
    upload.device = device;
    upload.source = source;
    upload.source_offset = region.bufferOffset;
    upload.target_offset = *total;
    upload.format = record.format.etc_format;
    upload.image = destination;
    upload.mip_level = region.imageSubresource.mipLevel;
    upload.width = region.imageExtent.width;
    upload.height = region.imageExtent.height;
    upload.layers = region.imageSubresource.layerCount;
    upload.scale = scale;
    upload.source_width = upload.width;
    upload.source_height = upload.height;
    upload.target_width = upload.width;
    upload.target_height = upload.height;
    upload.target_bytes = upload.decoded;
    const VkDeviceSize row_texels =
        region.bufferRowLength != 0 ? region.bufferRowLength : upload.width;
    const VkDeviceSize layer_rows =
        region.bufferImageHeight != 0 ? region.bufferImageHeight
                                      : upload.height;
    if (row_texels < upload.width || layer_rows < upload.height) {
      return false;
    }
    const VkDeviceSize block_bytes = EtcBlockBytes(record.format.etc_format);
    const VkDeviceSize block_rows =
        (static_cast<VkDeviceSize>(upload.height) + 3) / 4;
    const VkDeviceSize packed_row =
        ((static_cast<VkDeviceSize>(upload.width) + 3) / 4) * block_bytes;
    upload.source_row_stride = ((row_texels + 3) / 4) * block_bytes;
    upload.source_layer_stride =
        upload.source_row_stride * ((layer_rows + 3) / 4);
    upload.source_span = (upload.layers - 1) * upload.source_layer_stride +
                         (block_rows - 1) * upload.source_row_stride +
                         packed_row;
    Region copy = region;
    copy.bufferOffset = *total;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    if (scale > 1) {
      const std::uint32_t mip = region.imageSubresource.mipLevel;
      const auto axis = [&](std::uint32_t base, std::int32_t offset,
                            std::uint32_t extent, std::uint32_t* source,
                            std::uint32_t* target, std::int32_t* host_offset,
                            std::uint32_t* host_extent) {
        std::uint32_t start = 0;
        std::uint32_t end = 0;
        if (!ScaledAxis(base, scale, mip, offset, extent, source, &start,
                        &end)) {
          return false;
        }
        *target = end - start;
        *host_offset = static_cast<std::int32_t>(start);
        *host_extent = end - start;
        return true;
      };
      if (!axis(record.width, region.imageOffset.x, region.imageExtent.width,
                &upload.source_width, &upload.target_width,
                &copy.imageOffset.x, &copy.imageExtent.width) ||
          !axis(record.height, region.imageOffset.y,
                region.imageExtent.height, &upload.source_height,
                &upload.target_height, &copy.imageOffset.y,
                &copy.imageExtent.height)) {
        return false;
      }
      upload.target_bytes = static_cast<VkDeviceSize>(upload.target_width) *
                            upload.target_height *
                            EtcDecodedTexelBytes(record.format.etc_format);
      upload.full_mip =
          region.imageOffset.x == 0 && region.imageOffset.y == 0 &&
          upload.source_width == std::max<std::uint32_t>(1, record.width >> mip) &&
          upload.source_height == std::max<std::uint32_t>(1, record.height >> mip);
    }
    rewritten->push_back(copy);
    uploads->push_back(upload);
    *total += upload.target_bytes;
  }
  return true;
}

}  // namespace

struct VulkanEtc2Emulation::State {
  explicit State(VkDeviceSize block_bytes)
      : staging_block_bytes(block_bytes) {}

  std::mutex mutex;
  std::mutex devices_mutex;
  std::unordered_map<VkPhysicalDevice, bool> physical_devices;
  std::vector<std::unique_ptr<HostDevice>> devices;
  std::unordered_map<VkImage, ImageRecord> images;
  std::unordered_map<VkBuffer, BufferBinding> buffers;
  std::unordered_map<VkDeviceMemory, Mapping> mappings;
  std::unordered_map<VkCommandBuffer, CommandRecord> commands;
  std::atomic<bool> has_commands{false};
  // Blocks of every device, in creation order. Guarded by `mutex`; a block's
  // address is stable until it is destroyed.
  const VkDeviceSize staging_block_bytes;
  std::vector<std::unique_ptr<StagingBlock>> staging_blocks;
  // Heap buffers reused across submits; they only grow.
  std::mutex scratch_mutex;
  RawBuffer scratch_source;
  std::vector<std::uint8_t> scratch_decoded;
  TextureOverrides overrides = TextureOverrides::FromEnvironment();
  const std::uint32_t upscale =
      SmallTextureUpscale(std::getenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"));

  // Level-0 colour uploads are hashed, dumped and matched against override
  // files; the match is kept on the image so its mip levels follow.
  std::shared_ptr<const RgbaImage> FindOverride(const PendingUpload& upload,
                                                const std::uint8_t* compressed,
                                                const std::uint8_t* decoded) {
    if (!overrides.enabled() || upload.layers != 1 ||
        !IsColorFormat(upload.format)) {
      return nullptr;
    }
    std::shared_ptr<const RgbaImage> replacement;
    if (upload.mip_level == 0) {
      const std::uint64_t hash = HashBytes(compressed, upload.compressed);
      overrides.Dump(hash, upload.width, upload.height, decoded);
      replacement = overrides.Lookup(hash);
      std::lock_guard<std::mutex> lock(mutex);
      const auto record = images.find(upload.image);
      if (record != images.end()) {
        record->second.override = replacement;
      }
    } else {
      std::lock_guard<std::mutex> lock(mutex);
      const auto record = images.find(upload.image);
      if (record != images.end()) {
        replacement = record->second.override;
      }
    }
    return replacement;
  }

  // Writes the host-side texels of one upload: the override or the decoded
  // texels, resampled to the host region size.
  void EmitUpload(const PendingUpload& upload, const std::uint8_t* compressed,
                  const std::uint8_t* decoded) {
    const std::shared_ptr<const RgbaImage> replacement =
        FindOverride(upload, compressed, decoded);
    if (replacement != nullptr) {
      ResampleRgba(*replacement, upload.target_width, upload.target_height,
                   upload.target);
    } else if (upload.scale > 1) {
      std::shared_ptr<const RgbaImage> base;
      if (upload.full_mip) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto record = images.find(upload.image);
        if (record != images.end()) {
          if (upload.mip_level == 0) {
            auto level0 = std::make_shared<RgbaImage>();
            level0->width = upload.source_width;
            level0->height = upload.source_height;
            level0->pixels.assign(decoded, decoded + upload.decoded);
            if (upload.source_width != upload.width ||
                upload.source_height != upload.height) {
              level0->pixels.clear();
              for (std::uint32_t y = 0; y < upload.source_height; ++y) {
                const std::uint8_t* row =
                    decoded + static_cast<std::size_t>(y) * upload.width * 4;
                level0->pixels.insert(level0->pixels.end(), row,
                                      row + upload.source_width * 4);
              }
            }
            record->second.base = level0;
          }
          base = record->second.base;
        }
      }
      if (base != nullptr) {
        ResampleRgba(*base, upload.target_width, upload.target_height,
                     upload.target);
        return;
      }
      const std::uint8_t* source = decoded;
      std::vector<std::uint8_t> cropped;
      if (upload.source_width != upload.width ||
          upload.source_height != upload.height) {
        const std::size_t row = static_cast<std::size_t>(upload.source_width) * 4;
        cropped.resize(row * upload.source_height);
        for (std::uint32_t y = 0; y < upload.source_height; ++y) {
          std::memcpy(cropped.data() + y * row,
                      decoded + static_cast<std::size_t>(y) * upload.width * 4,
                      row);
        }
        source = cropped.data();
      }
      ResampleRgba(source, upload.source_width, upload.source_height,
                   upload.target_width, upload.target_height, upload.target);
    } else {
      std::memcpy(upload.target, decoded, upload.decoded);
    }
  }

  struct EmitJob {
    const PendingUpload* upload = nullptr;
    const std::uint8_t* compressed = nullptr;
    const std::uint8_t* decoded = nullptr;
  };

  struct EmitQueue {
    State* state = nullptr;
    std::vector<EmitJob> jobs;
    std::atomic<std::size_t> next{0};

    void Run() {
      for (;;) {
        const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= jobs.size()) {
          return;
        }
        const EmitJob& job = jobs[index];
        state->EmitUpload(*job.upload, job.compressed, job.decoded);
      }
    }
  };

  // Resampling a batch costs sixteen times the decoded texels at the default
  // upscale, so uploads are emitted across the decode worker pool. They write
  // disjoint staging ranges, and level 0 is emitted before the rest because it
  // publishes the texels the other mips resample from.
  void EmitBatch(std::vector<EmitJob> jobs, unsigned worker_count) {
    if (jobs.empty()) {
      return;
    }
    std::vector<EmitJob> level_zero;
    std::vector<EmitJob> rest;
    for (const EmitJob& job : jobs) {
      (job.upload->mip_level == 0 ? level_zero : rest).push_back(job);
    }
    EmitPass(std::move(level_zero), worker_count);
    EmitPass(std::move(rest), worker_count);
  }

  void EmitPass(std::vector<EmitJob> jobs, unsigned worker_count) {
    if (jobs.empty()) {
      return;
    }
    EmitQueue queue;
    queue.state = this;
    queue.jobs = std::move(jobs);
    const std::size_t threads =
        std::min<std::size_t>(std::max(worker_count, 1U), queue.jobs.size());
    RunOnDecodeWorkers(
        [](void* context) { static_cast<EmitQueue*>(context)->Run(); }, &queue,
        static_cast<unsigned>(threads));
  }

  // Takes `size` bytes from the first block of the device with room for
  // them, or from a new block of at least `staging_block_bytes`.
  bool AcquireStaging(const HostDevice& dev, VkDeviceSize size,
                      Staging* staging) {
    const VkDeviceSize aligned =
        (size + kStagingAlignment - 1) / kStagingAlignment * kStagingAlignment;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& block : staging_blocks) {
        if (block->device == dev.device &&
            block->capacity - block->head >= aligned) {
          *staging = {block.get(), block->head};
          block->head += aligned;
          ++block->live;
          return true;
        }
      }
    }
    // Created outside the lock, which the decode workers also take.
    auto block = std::make_unique<StagingBlock>();
    {
      const VkDeviceSize capacity = std::max(staging_block_bytes, aligned);
      TraceScope trace_scope(ActiveProfileTrace(), "etc2 staging create",
                             "texture");
      trace_scope.Arg("bytes", static_cast<std::int64_t>(capacity));
      if (!CreateStagingBlock(dev, capacity, block.get())) {
        return false;
      }
    }
    block->head = aligned;
    block->live = 1;
    *staging = {block.get(), 0};
    std::lock_guard<std::mutex> lock(mutex);
    staging_blocks.push_back(std::move(block));
    return true;
  }

  // Returns ranges to their blocks. A block with no ranges left starts over
  // from its front; empty blocks beyond kIdleStagingBlocks block sizes are
  // destroyed.
  void ReleaseStaging(const std::vector<Staging>& released) {
    std::vector<std::unique_ptr<StagingBlock>> doomed;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const Staging& staging : released) {
        if (--staging.block->live == 0) {
          staging.block->head = 0;
        }
      }
      const VkDeviceSize idle_limit = kIdleStagingBlocks * staging_block_bytes;
      VkDeviceSize idle_bytes = 0;
      for (auto it = staging_blocks.begin(); it != staging_blocks.end();) {
        if ((*it)->live == 0 && idle_bytes + (*it)->capacity > idle_limit) {
          doomed.push_back(std::move(*it));
          it = staging_blocks.erase(it);
          continue;
        }
        if ((*it)->live == 0) {
          idle_bytes += (*it)->capacity;
        }
        ++it;
      }
    }
    if (doomed.empty()) {
      return;
    }
    TraceScope trace_scope(ActiveProfileTrace(), "etc2 staging destroy",
                           "texture");
    trace_scope.Arg("buffers", static_cast<std::int64_t>(doomed.size()));
    for (const auto& block : doomed) {
      if (const HostDevice* dev = Find(block->device); dev != nullptr) {
        DestroyStagingBlock(*dev, *block);
      }
    }
  }

  const HostDevice* Find(VkDevice device) {
    std::lock_guard<std::mutex> lock(devices_mutex);
    for (const auto& candidate : devices) {
      if (candidate->device == device) {
        return candidate.get();
      }
    }
    return nullptr;
  }

  bool BlitScaledCopy(const HostDevice& dev, VkCommandBuffer command_buffer,
                      VkImage source, VkImageLayout source_layout,
                      VkImage destination, VkImageLayout destination_layout,
                      std::uint32_t count, const VkImageCopy* regions);

  bool LookupImage(const HostDevice& dev, VkImage image,
                   ImageRecord* record) {
    if (!dev.emulated) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = images.find(image);
    if (found == images.end()) {
      return false;
    }
    *record = found->second;
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

  // Asynchronous decode. One dispatcher runs batches in the order tickets
  // were handed out, so the timeline semaphore is always signalled with an
  // increasing value and a waiter never observes a later batch's signal.
  std::mutex queue_mutex;
  std::condition_variable queue_ready;
  std::condition_variable queue_done;
  std::deque<std::unique_ptr<DecodeBatch>> queue;
  std::vector<std::unique_ptr<DecodeBatch>> spare_batches;
  std::uint64_t issued_ticket = 0;
  std::uint64_t completed_ticket = 0;
  bool stopping = false;
  std::thread dispatcher;

  // Recycles a batch's buffers; they only grow, as the scratch buffers do.
  std::unique_ptr<DecodeBatch> AcquireBatch() {
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (spare_batches.empty()) {
      return std::make_unique<DecodeBatch>();
    }
    std::unique_ptr<DecodeBatch> batch = std::move(spare_batches.back());
    spare_batches.pop_back();
    batch->uploads.clear();
    return batch;
  }

  void StartDispatcher() {
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (dispatcher.joinable() || stopping) {
      return;
    }
    dispatcher = std::thread([this] { RunDispatcher(); });
  }

  // Hands out the next ticket and queues the batch behind it.
  std::uint64_t Enqueue(std::unique_ptr<DecodeBatch> batch) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    batch->ticket = ++issued_ticket;
    const std::uint64_t ticket = batch->ticket;
    queue.push_back(std::move(batch));
    queue_ready.notify_one();
    return ticket;
  }

  void RunDispatcher() {
    for (;;) {
      std::unique_ptr<DecodeBatch> batch;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_ready.wait(lock, [this] { return stopping || !queue.empty(); });
        if (queue.empty()) {
          if (stopping) {
            return;
          }
          continue;
        }
        batch = std::move(queue.front());
        queue.pop_front();
      }
      RunBatch(*batch);
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        completed_ticket = batch->ticket;
        if (spare_batches.size() < kMaxSpareBatches) {
          spare_batches.push_back(std::move(batch));
        }
        queue_done.notify_all();
      }
    }
  }

  void RunBatch(DecodeBatch& batch) {
    TraceScope trace_scope(ActiveProfileTrace(), "etc2 decode", "texture");
    trace_scope.Arg("uploads", static_cast<std::int64_t>(batch.uploads.size()));
    trace_scope.Arg("compressed_bytes",
                    static_cast<std::int64_t>(batch.source_bytes));
    trace_scope.Arg("decoded_bytes",
                    static_cast<std::int64_t>(batch.decoded_bytes));
    batch.source.EnsureSize(batch.source_bytes);
    if (batch.decoded.size() < batch.decoded_bytes) {
      batch.decoded.resize(batch.decoded_bytes);
    }
    if (!batch.deferred_sources.empty()) {
      GatherCompressed(batch.uploads, batch.deferred_sources,
                       batch.source.data());
      batch.deferred_sources.clear();
    }
    DecodeAndEmit(batch.uploads, batch.source.data(), batch.decoded.data());
    SignalDecoded(batch.device, batch.ticket);
  }

  void SignalDecoded(VkDevice device, std::uint64_t value) {
    const HostDevice* dev = Find(device);
    if (dev == nullptr || dev->timeline == VK_NULL_HANDLE ||
        dev->signal_semaphore == nullptr) {
      return;
    }
    VkSemaphoreSignalInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    info.semaphore = dev->timeline;
    info.value = value;
    if (dev->signal_semaphore(dev->device, &info) != VK_SUCCESS) {
      LogFailure("could not signal the decode timeline semaphore");
    }
  }

  // Decodes every layer of every upload and writes the host texels. Shared by
  // the dispatcher and by the synchronous path.
  void DecodeAndEmit(const std::vector<PendingUpload>& uploads,
                     std::uint8_t* source_base, std::uint8_t* decoded_base) {
    std::vector<EtcDecodeJob> jobs;
    VkDeviceSize compressed_offset = 0;
    VkDeviceSize decoded_offset = 0;
    for (const PendingUpload& upload : uploads) {
      std::uint8_t* source = source_base + compressed_offset;
      std::uint8_t* decoded = decoded_base + decoded_offset;
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
      compressed_offset += upload.compressed;
      decoded_offset += upload.decoded;
      if (ShouldLog(&g_decode_logs, 4)) {
        std::fprintf(stderr, "  [vulkan] ETC2 upload decoded %ux%u layers=%u\n",
                     upload.width, upload.height, upload.layers);
      }
    }
    DecodeEtcJobs(jobs.data(), jobs.size(), DecodeWorkerCount());
    for (const EtcDecodeJob& job : jobs) {
      if (!job.ok) {
        LogFailure("could not decode an upload layer");
      }
    }
    compressed_offset = 0;
    decoded_offset = 0;
    std::vector<EmitJob> emit_jobs;
    emit_jobs.reserve(uploads.size());
    for (const PendingUpload& upload : uploads) {
      emit_jobs.push_back({&upload, source_base + compressed_offset,
                           decoded_base + decoded_offset});
      compressed_offset += upload.compressed;
      decoded_offset += upload.decoded;
    }
    EmitBatch(std::move(emit_jobs), DecodeWorkerCount());
  }

  // Blocks until every batch up to `ticket` has been decoded and emitted.
  void WaitForTicket(std::uint64_t ticket) {
    if (ticket == 0) {
      return;
    }
    std::unique_lock<std::mutex> lock(queue_mutex);
    queue_done.wait(lock, [this, ticket] {
      return completed_ticket >= ticket || stopping;
    });
  }

  // Blocks until nothing is queued or in flight.
  void DrainDecodes() {
    std::uint64_t ticket = 0;
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      ticket = issued_ticket;
    }
    WaitForTicket(ticket);
  }

  void StopDispatcher() {
    std::thread worker;
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      stopping = true;
      queue_ready.notify_all();
      queue_done.notify_all();
      worker = std::move(dispatcher);
    }
    if (worker.joinable()) {
      worker.join();
    }
  }
};

VulkanEtc2Emulation::VulkanEtc2Emulation(VkDeviceSize staging_block_bytes)
    : state_(new State(staging_block_bytes)) {}

VulkanEtc2Emulation::~VulkanEtc2Emulation() {
  state_->StopDispatcher();
  delete state_;
}

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
    PFN_vkGetDeviceProcAddr get_device_proc_addr, bool timeline_semaphore) {
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
  dev->copy_image = DeviceProc<PFN_vkCmdCopyImage>(get, device, "vkCmdCopyImage");
  dev->copy_image2 = DeviceProc<PFN_vkCmdCopyImage2>(
      get, device, "vkCmdCopyImage2", "vkCmdCopyImage2KHR");
  dev->blit_image = DeviceProc<PFN_vkCmdBlitImage>(get, device, "vkCmdBlitImage");
  dev->execute_commands =
      DeviceProc<PFN_vkCmdExecuteCommands>(get, device, "vkCmdExecuteCommands");
  dev->create_buffer =
      DeviceProc<PFN_vkCreateBuffer>(get, device, "vkCreateBuffer");
  dev->allocate_memory =
      DeviceProc<PFN_vkAllocateMemory>(get, device, "vkAllocateMemory");
  dev->get_buffer_memory_requirements =
      DeviceProc<PFN_vkGetBufferMemoryRequirements>(
          get, device, "vkGetBufferMemoryRequirements");
  dev->pipeline_barrier =
      DeviceProc<PFN_vkCmdPipelineBarrier>(get, device, "vkCmdPipelineBarrier");
  dev->signal_semaphore = DeviceProc<PFN_vkSignalSemaphore>(
      get, device, "vkSignalSemaphore", "vkSignalSemaphoreKHR");
  dev->destroy_semaphore =
      DeviceProc<PFN_vkDestroySemaphore>(get, device, "vkDestroySemaphore");
  if (emulated && timeline_semaphore && dev->signal_semaphore != nullptr) {
    const auto create_semaphore =
        DeviceProc<PFN_vkCreateSemaphore>(get, device, "vkCreateSemaphore");
    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;
    if (create_semaphore != nullptr &&
        create_semaphore(device, &info, nullptr, &dev->timeline) !=
            VK_SUCCESS) {
      dev->timeline = VK_NULL_HANDLE;
    }
    if (dev->timeline != VK_NULL_HANDLE) {
      state_->StartDispatcher();
    }
  }
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
  // Nothing may free staging or the semaphore while a decode still writes it.
  state_->DrainDecodes();
  std::vector<std::unique_ptr<StagingBlock>> doomed;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (auto it = state_->commands.begin(); it != state_->commands.end();) {
      const bool owned = std::any_of(
          it->second.staging.begin(), it->second.staging.end(),
          [device](const Staging& staging) {
            return staging.block->device == device;
          });
      it = owned ? state_->commands.erase(it) : std::next(it);
    }
    auto& blocks = state_->staging_blocks;
    for (auto it = blocks.begin(); it != blocks.end();) {
      if ((*it)->device == device) {
        doomed.push_back(std::move(*it));
        it = blocks.erase(it);
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
    for (const auto& block : doomed) {
      DestroyStagingBlock(*dev, *block);
    }
    if (dev->timeline != VK_NULL_HANDLE && dev->destroy_semaphore != nullptr) {
      dev->destroy_semaphore(device, dev->timeline, nullptr);
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
  std::uint32_t scale = 1;
  if (state_->upscale > 1 && IsColorFormat(format.etc_format) &&
      create_info->imageType == VK_IMAGE_TYPE_2D &&
      create_info->arrayLayers == 1 && create_info->extent.depth == 1 &&
      create_info->extent.width <= kSmallTextureMaxExtent &&
      create_info->extent.height <= kSmallTextureMaxExtent) {
    scale = state_->upscale;
    host_info.extent.width *= scale;
    host_info.extent.height *= scale;
  }
  const VkResult result = dev->create_image(device, &host_info, allocator, image);
  if (result == VK_SUCCESS && image != nullptr) {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->images[*image] = {device,
                                format,
                                scale,
                                create_info->extent.width,
                                create_info->extent.height,
                                nullptr,
                                nullptr};
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
    // A deferred gather reads this mapping on a worker, so it must finish
    // before the pointer stops being valid.
    state_->DrainDecodes();
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
    state_->DrainDecodes();
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
    state_->DrainDecodes();
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
  ImageRecord record;
  if (!state_->LookupImage(*dev, destination, &record)) {
    dev->copy_buffer_to_image(command_buffer, source, destination, layout,
                              region_count, regions);
    return;
  }
  std::vector<VkBufferImageCopy> rewritten;
  std::vector<PendingUpload> uploads;
  VkDeviceSize total = 0;
  Staging staging;
  if (!PlanRegions(record, device, source, destination, region_count, regions,
                   &rewritten, &uploads, &total)) {
    LogFailure("skipped an upload with an unsupported region");
    return;
  }
  if (!state_->AcquireStaging(*dev, total, &staging)) {
    LogFailure("could not allocate a host-visible staging buffer");
    return;
  }
  for (PendingUpload& upload : uploads) {
    upload.target = staging.block->mapped + staging.offset + upload.target_offset;
  }
  for (VkBufferImageCopy& region : rewritten) {
    region.bufferOffset += staging.offset;
  }
  state_->Record(command_buffer, std::move(uploads), staging);
  RecordHostWriteBarrier(*dev, command_buffer);
  dev->copy_buffer_to_image(command_buffer, staging.block->buffer, destination,
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
  ImageRecord record;
  if (!state_->LookupImage(*dev, info->dstImage, &record)) {
    dev->copy_buffer_to_image2(command_buffer, info);
    return;
  }
  std::vector<VkBufferImageCopy2> rewritten;
  std::vector<PendingUpload> uploads;
  VkDeviceSize total = 0;
  Staging staging;
  if (!PlanRegions(record, device, info->srcBuffer, info->dstImage,
                   info->regionCount, info->pRegions, &rewritten, &uploads,
                   &total)) {
    LogFailure("skipped an upload with an unsupported region");
    return;
  }
  if (!state_->AcquireStaging(*dev, total, &staging)) {
    LogFailure("could not allocate a host-visible staging buffer");
    return;
  }
  for (PendingUpload& upload : uploads) {
    upload.target = staging.block->mapped + staging.offset + upload.target_offset;
  }
  for (VkBufferImageCopy2& region : rewritten) {
    region.bufferOffset += staging.offset;
  }
  state_->Record(command_buffer, std::move(uploads), staging);
  RecordHostWriteBarrier(*dev, command_buffer);
  VkCopyBufferToImageInfo2 host_info = *info;
  host_info.srcBuffer = staging.block->buffer;
  host_info.pRegions = rewritten.data();
  dev->copy_buffer_to_image2(command_buffer, &host_info);
}

// Copies touching a scaled image become blits, so both sides land on their
// own host bounds. Returns false when no image involved is scaled.
bool VulkanEtc2Emulation::State::BlitScaledCopy(
    const HostDevice& dev, VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, std::uint32_t count,
    const VkImageCopy* regions) {
  ImageRecord source_record;
  ImageRecord destination_record;
  const bool source_scaled =
      LookupImage(dev, source, &source_record) && source_record.scale > 1;
  const bool destination_scaled =
      LookupImage(dev, destination, &destination_record) &&
      destination_record.scale > 1;
  if ((!source_scaled && !destination_scaled) || dev.blit_image == nullptr ||
      regions == nullptr) {
    return false;
  }
  std::vector<VkImageBlit> blits;
  blits.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const VkImageCopy& region = regions[index];
    VkImageBlit blit{};
    blit.srcSubresource = region.srcSubresource;
    blit.dstSubresource = region.dstSubresource;
    if (!HostRegion(source_scaled ? &source_record : nullptr,
                    region.srcSubresource.mipLevel, region.srcOffset,
                    region.extent, &blit.srcOffsets[0], &blit.srcOffsets[1]) ||
        !HostRegion(destination_scaled ? &destination_record : nullptr,
                    region.dstSubresource.mipLevel, region.dstOffset,
                    region.extent, &blit.dstOffsets[0], &blit.dstOffsets[1])) {
      LogFailure("skipped an image copy with an unsupported region");
      return true;
    }
    blits.push_back(blit);
  }
  const bool same_scale = source_scaled && destination_scaled &&
                          source_record.scale == destination_record.scale;
  dev.blit_image(command_buffer, source, source_layout, destination,
                 destination_layout, static_cast<std::uint32_t>(blits.size()),
                 blits.data(), same_scale ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
  return true;
}

void VulkanEtc2Emulation::CmdCopyImage(
    VkDevice device, VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, std::uint32_t region_count,
    const VkImageCopy* regions) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_image == nullptr) {
    return;
  }
  if (dev->emulated && state_->upscale > 1 &&
      state_->BlitScaledCopy(*dev, command_buffer, source, source_layout,
                             destination, destination_layout, region_count,
                             regions)) {
    return;
  }
  dev->copy_image(command_buffer, source, source_layout, destination,
                  destination_layout, region_count, regions);
}

void VulkanEtc2Emulation::CmdCopyImage2(VkDevice device,
                                        VkCommandBuffer command_buffer,
                                        const VkCopyImageInfo2* info) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_image2 == nullptr || info == nullptr) {
    return;
  }
  if (dev->emulated && state_->upscale > 1 && info->pRegions != nullptr) {
    std::vector<VkImageCopy> regions;
    regions.reserve(info->regionCount);
    for (std::uint32_t index = 0; index < info->regionCount; ++index) {
      const VkImageCopy2& region = info->pRegions[index];
      regions.push_back({region.srcSubresource, region.srcOffset,
                         region.dstSubresource, region.dstOffset,
                         region.extent});
    }
    if (state_->BlitScaledCopy(*dev, command_buffer, info->srcImage,
                               info->srcImageLayout, info->dstImage,
                               info->dstImageLayout, info->regionCount,
                               regions.data())) {
      return;
    }
  }
  dev->copy_image2(command_buffer, info);
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

Etc2SubmitWait VulkanEtc2Emulation::PrepareSubmit(
    const VkCommandBuffer* command_buffers, std::uint32_t count,
    bool allow_async) {
  if (command_buffers == nullptr || count == 0 ||
      !state_->has_commands.load(std::memory_order_acquire)) {
    return {};
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
                     upload.source_span <= mapping->second.size -
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
  if (work.empty()) {
    return {};
  }
  // A submit carries one device, but a batch that somehow spans two cannot be
  // signalled by a single timeline, so it decodes on this thread instead.
  VkDevice batch_device = work.front().upload.device;
  bool single_device = true;
  for (const Work& item : work) {
    if (item.upload.device != batch_device) {
      single_device = false;
      break;
    }
  }
  const HostDevice* batch_dev = state_->Find(batch_device);
  const bool async = allow_async && single_device && batch_dev != nullptr &&
                     batch_dev->timeline != VK_NULL_HANDLE &&
                     batch_dev->signal_semaphore != nullptr;

  TraceScope trace_scope(ActiveProfileTrace(),
                         async ? "etc2 gather" : "etc2 decode", "texture");
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
  trace_scope.Arg("uploads", static_cast<std::int64_t>(copies.size()));
  trace_scope.Arg("compressed_bytes",
                  static_cast<std::int64_t>(compressed_total));
  trace_scope.Arg("decoded_bytes", static_cast<std::int64_t>(decoded_total));

  std::unique_ptr<DecodeBatch> batch;
  if (async) {
    batch = state_->AcquireBatch();
    batch->device = batch_device;
  }
  // Owned copies, so the decode never reads the caller's stack.
  std::vector<PendingUpload> uploads;
  uploads.reserve(copied_uploads.size());
  for (const PendingUpload* upload : copied_uploads) {
    uploads.push_back(*upload);
  }
  std::vector<const std::uint8_t*> sources;
  sources.reserve(copies.size());
  for (const Copy& copy : copies) {
    sources.push_back(copy.source);
  }

  // Reading the application's staging means reading write-combined memory,
  // which is slow enough to show in a frame. When nothing had to be mapped
  // for this batch, the application's own mapping stays valid until the copy
  // completes, so the gather moves to the dispatcher with the decode and the
  // submitting thread keeps only the bookkeeping.
  const bool defer_gather = async && temporary.empty();

  if (!async) {
    std::lock_guard<std::mutex> scratch_lock(state_->scratch_mutex);
    state_->scratch_source.EnsureSize(compressed_total);
    if (state_->scratch_decoded.size() < decoded_total) {
      state_->scratch_decoded.resize(decoded_total);
    }
    GatherCompressed(uploads, sources, state_->scratch_source.data());
    for (const auto& [memory, mapping] : temporary) {
      if (mapping.dev->unmap_memory != nullptr) {
        mapping.dev->unmap_memory(mapping.dev->device, memory);
      }
    }
    state_->DecodeAndEmit(uploads, state_->scratch_source.data(),
                          state_->scratch_decoded.data());
    return {};
  }

  batch->source_bytes = compressed_total;
  batch->decoded_bytes = decoded_total;
  if (defer_gather) {
    batch->deferred_sources = std::move(sources);
  } else {
    batch->source.EnsureSize(compressed_total);
    GatherCompressed(uploads, sources, batch->source.data());
    for (const auto& [memory, mapping] : temporary) {
      if (mapping.dev->unmap_memory != nullptr) {
        mapping.dev->unmap_memory(mapping.dev->device, memory);
      }
    }
  }

  batch->uploads = std::move(uploads);
  // The staging these uploads write into must outlive the decode, so the
  // command buffers that own it remember the ticket. The batch is queued
  // while the state lock is held, so a concurrent ReleaseCommandBuffer cannot
  // read a stale ticket and recycle staging the dispatcher is still writing.
  // Tickets only ever go up, so a record keeps the highest one it has seen.
  std::uint64_t ticket = 0;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ticket = state_->Enqueue(std::move(batch));
    const auto remember = [this, ticket](VkCommandBuffer command_buffer) {
      const auto record = state_->commands.find(command_buffer);
      if (record != state_->commands.end()) {
        record->second.last_ticket =
            std::max(record->second.last_ticket, ticket);
      }
    };
    for (std::uint32_t index = 0; index < count; ++index) {
      const auto record = state_->commands.find(command_buffers[index]);
      if (record == state_->commands.end()) {
        continue;
      }
      record->second.last_ticket =
          std::max(record->second.last_ticket, ticket);
      for (VkCommandBuffer secondary : record->second.secondaries) {
        remember(secondary);
      }
    }
  }
  trace_scope.Arg("ticket", static_cast<std::int64_t>(ticket));
  return {batch_dev->timeline, ticket};
}

void VulkanEtc2Emulation::ReleaseCommandBuffer(VkCommandBuffer command_buffer) {
  if (!state_->has_commands.load(std::memory_order_acquire)) {
    return;
  }
  std::uint64_t ticket = 0;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto record = state_->commands.find(command_buffer);
    if (record == state_->commands.end()) {
      return;
    }
    ticket = record->second.last_ticket;
  }
  // Waited on without holding the state lock, which the decode itself takes.
  // Only waits long enough to matter are traced; this runs for every
  // command buffer the application resets.
  ChromeTraceWriter* const trace = ActiveProfileTrace();
  const std::uint64_t wait_start_ns = trace != nullptr ? TraceClockNanos() : 0;
  state_->WaitForTicket(ticket);
  if (trace != nullptr) {
    const std::uint64_t wait_end_ns = TraceClockNanos();
    if (wait_end_ns - wait_start_ns >= 100'000) {
      trace->Slice("etc2 release wait", "texture", wait_start_ns, wait_end_ns,
                   {{"ticket", static_cast<std::int64_t>(ticket)}});
    }
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
  state_->ReleaseStaging(std::move(doomed));
}

}  // namespace mocktail::graphics
