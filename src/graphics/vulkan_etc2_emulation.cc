#include "mocktail/graphics/vulkan_etc2_emulation.h"

#include "mocktail/graphics/bc_decoder.h"
#include "mocktail/graphics/chrome_trace_writer.h"
#include "mocktail/graphics/etc2_gpu_decoder.h"
#include "mocktail/graphics/texture_override.h"

#include <algorithm>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

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

bool Etc2ImageByteCounts(EtcFormat format, const VkExtent3D& extent,
                         std::uint32_t mip_levels, std::uint32_t layers,
                         std::uint32_t scale, VkDeviceSize* compressed,
                         VkDeviceSize* decoded) {
  if (extent.width == 0 || extent.height == 0 || extent.depth == 0 ||
      mip_levels == 0 || layers == 0 || scale == 0 || compressed == nullptr ||
      decoded == nullptr) {
    return false;
  }
  const auto level = [](std::uint32_t size, std::uint32_t mip) {
    return std::max<VkDeviceSize>(1, static_cast<VkDeviceSize>(size) >> mip);
  };
  VkDeviceSize compressed_total = 0;
  VkDeviceSize decoded_total = 0;
  for (std::uint32_t mip = 0; mip < mip_levels && mip < 32; ++mip) {
    const VkDeviceSize depth = level(extent.depth, mip);
    compressed_total += ((level(extent.width, mip) + 3) / 4) *
                        ((level(extent.height, mip) + 3) / 4) * depth *
                        EtcBlockBytes(format);
    decoded_total += level(extent.width * scale, mip) *
                     level(extent.height * scale, mip) * depth *
                     EtcDecodedTexelBytes(format);
  }
  *compressed = compressed_total * layers;
  *decoded = decoded_total * layers;
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

bool GpuDecodeDisabledByEnvironment() {
  const char* value = std::getenv("MOCKTAIL_ETC2_GPU_DECODE");
  return value != nullptr && std::strcmp(value, "0") == 0;
}

// Unset or anything but "0" means on.
bool OnUnlessZero(const char* value) {
  return value == nullptr || std::strcmp(value, "0") != 0;
}

}  // namespace

bool Etc2SupportAdvertised(const char* value) { return OnUnlessZero(value); }

bool Etc2BcTranscodeEnabled(const char* value) { return OnUnlessZero(value); }

bool Etc2SupportAdvertised() {
  static const bool advertised =
      Etc2SupportAdvertised(std::getenv("MOCKTAIL_ADVERTISE_ETC2"));
  return advertised;
}

std::uint32_t SmallTextureUpscale(const char* value) {
  if (value == nullptr || value[0] < '0' || value[0] > '9') {
    return kAdaptiveSmallTextureUpscale;
  }
  const long parsed = std::strtol(value, nullptr, 10);
  if (parsed < 1) {
    return 1;
  }
  return parsed > 8 ? 8 : static_cast<std::uint32_t>(parsed);
}

VkDeviceSize Etc2HostBlockCount(const VkExtent3D& extent,
                                std::uint32_t mip_levels, std::uint32_t layers,
                                std::uint32_t scale) {
  const auto blocks = [&](std::uint32_t size, std::uint32_t mip) {
    return (std::max<VkDeviceSize>(1, VkDeviceSize{size} * scale >> mip) + 3) /
           4;
  };
  VkDeviceSize total = 0;
  for (std::uint32_t mip = 0; mip < mip_levels && mip < 32; ++mip) {
    total += blocks(extent.width, mip) * blocks(extent.height, mip);
  }
  return total * layers;
}

std::uint32_t AdaptiveSmallTextureUpscale(double upscale_ns) {
  if (upscale_ns <= 0) {
    return 1;
  }
  // A full chain: 64x64 has 7 levels.
  constexpr VkExtent3D kLargestSmall{kSmallTextureMaxExtent,
                                     kSmallTextureMaxExtent, 1};
  constexpr std::uint32_t kLevels = 7;
  for (std::uint32_t scale = kDefaultSmallTextureUpscale; scale > 1;
       scale /= 2) {
    if (static_cast<double>(
            Etc2HostBlockCount(kLargestSmall, kLevels, 1, scale)) *
            upscale_ns <=
        kAdaptiveUpscaleTextureNs) {
      return scale;
    }
  }
  return 1;
}

void Etc2EncodeBudget::Refill(std::uint64_t now_ns) {
  if (last_ns_ != 0 && now_ns > last_ns_) {
    available_ns_ = std::min(
        burst_ns_,
        available_ns_ + static_cast<double>(now_ns - last_ns_) * share_);
  }
  if (now_ns > last_ns_) {
    last_ns_ = now_ns;
  }
}

void Etc2EncodeBudget::Charge(double cost_ns, std::uint64_t now_ns) {
  Refill(now_ns);
  available_ns_ -= cost_ns;
}

void Etc2EncodeBudget::Refund(double cost_ns) {
  available_ns_ = std::min(burst_ns_, available_ns_ + cost_ns);
}

bool Etc2EncodeBudget::Take(double cost_ns, std::uint64_t now_ns) {
  Refill(now_ns);
  if (available_ns_ < std::min(cost_ns, burst_ns_)) {
    return false;
  }
  available_ns_ -= cost_ns;
  return true;
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

// Images whose encode the budget decided since `since_ns`, by host scale.
struct EncodeStats {
  std::uint64_t by_scale[9] = {};
  std::uint64_t held_back = 0;
  double estimated_ns = 0;
  std::uint64_t since_ns = 0;
};

constexpr std::uint64_t kEncodeLogIntervalNs = 5'000'000'000;

// A line every kEncodeLogIntervalNs while images are created, so a session
// log shows what the budget held back on the reporter's GPU.
std::string TakeEncodeLog(EncodeStats* stats, std::uint64_t now) {
  if (stats->since_ns == 0) {
    stats->since_ns = now;
  }
  if (now - stats->since_ns < kEncodeLogIntervalNs) {
    return {};
  }
  std::string line = "  [vulkan] BC encodes over ";
  char part[64];
  std::snprintf(part, sizeof(part), "%.1f s:",
                static_cast<double>(now - stats->since_ns) / 1e9);
  line += part;
  for (std::uint32_t scale = 8; scale >= 1; --scale) {
    if (stats->by_scale[scale] != 0) {
      std::snprintf(part, sizeof(part), " %ux %llu,", scale,
                    static_cast<unsigned long long>(stats->by_scale[scale]));
      line += part;
    }
  }
  std::snprintf(part, sizeof(part), " kept decoded %llu; est %.1f ms GPU\n",
                static_cast<unsigned long long>(stats->held_back),
                stats->estimated_ns / 1e6);
  line += part;
  *stats = {};
  stats->since_ns = now;
  return line;
}

struct HostDevice {
  VkDevice device = VK_NULL_HANDLE;
  bool emulated = false;
  bool bc_textures = false;
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
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  PFN_vkCmdCopyBuffer copy_buffer = nullptr;
  PFN_vkCreateCommandPool create_command_pool = nullptr;
  PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
  PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
  PFN_vkFreeCommandBuffers free_command_buffers = nullptr;
  PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
  PFN_vkEndCommandBuffer end_command_buffer = nullptr;
  std::unique_ptr<Etc2GpuDecoder> gpu;
  // Signalled by the decode dispatcher so a submit can wait for its uploads
  // on the GPU instead of on the submitting thread. Null when the device was
  // created without the timelineSemaphore feature.
  VkSemaphore timeline = VK_NULL_HANDLE;
  // From the startup probe; zero leaves encodes unbudgeted.
  Etc2GpuSpeed speed;
  std::uint32_t adaptive_upscale = 1;
  // Guarded by State::mutex.
  mutable Etc2EncodeBudget budget{kEncodeBudgetShare, kEncodeBudgetBurstNs};
  mutable EncodeStats stats;
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
  EmulatedTextureBytes bytes;
  // The host image is BC7/BC4/BC5 and uploads are transcoded on the GPU.
  bool transcoded = false;
  // Encode budget taken at creation and not yet spent by an upload.
  double prepaid_ns = 0;
  // An override file names this image's size, so its uploads decode on the
  // CPU where they can be hashed and replaced.
  bool may_override = false;
  // Compressed blocks of each full-mip upload to a scaled or decoded image,
  // by mip. Copies into a BC image are transcoded from them.
  std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> blocks;
};

// The BC format an emulated ETC2/EAC format is kept in.
bool BcHostFormat(VkFormat format, VkFormat* out) {
  switch (format) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
      *out = VK_FORMAT_BC7_UNORM_BLOCK;
      return true;
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
      *out = VK_FORMAT_BC7_SRGB_BLOCK;
      return true;
    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
      *out = VK_FORMAT_BC4_UNORM_BLOCK;
      return true;
    case VK_FORMAT_EAC_R11_SNORM_BLOCK:
      *out = VK_FORMAT_BC4_SNORM_BLOCK;
      return true;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
      *out = VK_FORMAT_BC5_UNORM_BLOCK;
      return true;
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
      *out = VK_FORMAT_BC5_SNORM_BLOCK;
      return true;
    default:
      return false;
  }
}

// `now` is read under the lock that produced `totals`, so concurrent changes
// reach the trace in the order they happened.
void TraceTextureBytes(const EmulatedTextureBytes& totals, std::uint64_t now) {
  ChromeTraceWriter* const trace = ActiveProfileTrace();
  if (trace == nullptr) {
    return;
  }
  constexpr double kMegabyte = 1024.0 * 1024.0;
  trace->Counter("emulated texture (MB)", now, totals.decoded / kMegabyte);
  trace->Counter("compressed texture (MB)", now, totals.compressed / kMegabyte);
}

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
  bool device_local = false;
  Etc2GpuBinding binding;
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
// Device-local staging for one GPU upload is capped at this many blocks,
// 128 MiB at the default block size.
constexpr VkDeviceSize kMaxGpuStagingBlocks = 8;

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
  // Zero means the CPU decodes and scales this upload.
  std::uint32_t scale_row_blocks = 0;
  // Only the blocks are kept, for later copies into an upscaled BC image;
  // the GPU writes the texels.
  bool keep_only = false;
};

template <typename Region>
struct GpuCopy {
  VkBuffer staging = VK_NULL_HANDLE;
  std::vector<Region> regions;
};

struct GpuUpload {
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer source = VK_NULL_HANDLE;
  VkDeviceSize source_offset = 0;
  VkDeviceSize source_span = 0;
  VkBuffer staging = VK_NULL_HANDLE;
  Etc2GpuBinding binding;
  Etc2GpuJob job;
  // A decode run before every job, whose output the scale job reads, and a
  // BC7 encode of the scale job's output, run after every job.
  bool decode = false;
  Etc2GpuJob decoding;
  bool encode = false;
  Etc2GpuJob encoding;
};

// Decode, scale and optional BC encode of a scale source. The caller sets the
// source buffer, offset and span.
GpuUpload ScaleJob(VkDevice device, EtcFormat format,
                   std::uint32_t blocks_wide, std::uint32_t blocks_high,
                   std::uint32_t target_width, std::uint32_t target_height,
                   bool encode) {
  GpuUpload gpu;
  gpu.device = device;
  gpu.decode = true;
  gpu.decoding.format = format;
  gpu.decoding.blocks_wide = blocks_wide;
  gpu.decoding.blocks_high = blocks_high;
  gpu.decoding.source_row_blocks = blocks_wide;
  gpu.decoding.source_layer_blocks = blocks_wide * blocks_high;
  gpu.job.format = format;
  gpu.job.scale = true;
  gpu.job.scale_width = target_width;
  gpu.job.scale_height = target_height;
  gpu.job.blocks_wide = (target_width + 3) / 4;
  gpu.job.blocks_high = (target_height + 3) / 4;
  if (encode) {
    gpu.encode = true;
    gpu.encoding.format = format;
    gpu.encoding.rgba_source = true;
    gpu.encoding.transcode = true;
    gpu.encoding.blocks_wide = gpu.job.blocks_wide;
    gpu.encoding.blocks_high = gpu.job.blocks_high;
    gpu.encoding.source_row_blocks = gpu.job.blocks_wide;
    gpu.encoding.source_layer_blocks =
        gpu.job.blocks_wide * gpu.job.blocks_high;
  }
  return gpu;
}

struct DecodeCommands {
  VkDevice device = VK_NULL_HANDLE;
  std::uint32_t family = 0;
  VkCommandBuffer buffer = VK_NULL_HANDLE;
};

// A copy between images, replayed on the kept ETC2 blocks at submit, after
// the uploads before it have stored theirs. It writes the source region into
// staging for a GPU encode, updates the destination's kept blocks, or both.
struct BlockCopy {
  VkImage image = VK_NULL_HANDLE;
  std::uint32_t mip = 0;
  std::uint32_t mip_blocks_wide = 0;
  std::uint32_t mip_blocks_high = 0;
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t wide = 0;
  std::uint32_t high = 0;
  std::size_t block_bytes = 0;
  // Staging for the region's blocks, or null when only the destination's
  // kept blocks change.
  std::uint8_t* target = nullptr;
  // The target is a scale source: a header naming the mip's texels comes
  // before the blocks.
  bool scale_header = false;
  std::uint32_t texels_wide = 0;
  std::uint32_t texels_high = 0;
  // The destination whose kept blocks of `keep_mip`, `keep_blocks_wide` x
  // `keep_blocks_high` blocks, take the region at block `keep_x`, `keep_y`.
  VkImage keep = VK_NULL_HANDLE;
  std::uint32_t keep_mip = 0;
  std::uint32_t keep_x = 0;
  std::uint32_t keep_y = 0;
  std::uint32_t keep_blocks_wide = 0;
  std::uint32_t keep_blocks_high = 0;
};

void KeepIn(BlockCopy* copy, VkImage destination, const ImageRecord& record,
            std::uint32_t mip, VkOffset3D offset) {
  copy->keep = destination;
  copy->keep_mip = mip;
  copy->keep_x = static_cast<std::uint32_t>(std::max(offset.x, 0)) / 4;
  copy->keep_y = static_cast<std::uint32_t>(std::max(offset.y, 0)) / 4;
  copy->keep_blocks_wide =
      (std::max<std::uint32_t>(1, record.width >> mip) + 3) / 4;
  copy->keep_blocks_high =
      (std::max<std::uint32_t>(1, record.height >> mip) + 3) / 4;
}

// Mips whose ETC2 blocks stay on the CPU, so a copy out of them can be
// encoded again: every mip of an upscaled or decoded colour image, and the
// mips of a BC image no larger than a texture that is upscaled.
bool KeepsMipBlocks(const ImageRecord& record, std::uint32_t mip) {
  if (record.scale > 1) {
    return true;
  }
  if (!record.transcoded) {
    return IsColorFormat(record.format.etc_format);
  }
  return std::max<std::uint32_t>(record.width >> mip, record.height >> mip) <=
         kSmallTextureMaxExtent;
}

struct CommandRecord {
  std::vector<PendingUpload> uploads;
  std::vector<BlockCopy> block_copies;
  std::vector<GpuUpload> gpu_uploads;
  std::vector<DecodeCommands> decode_commands;
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
  std::vector<BlockCopy> block_copies;
};

std::atomic<unsigned> g_image_logs{0};
std::atomic<unsigned> g_transcoded_image_logs{0};
std::atomic<unsigned> g_decode_logs{0};
// Messages are literals, keyed by address.
struct FailureCounts {
  std::mutex mutex;
  std::unordered_map<const char*, std::uint64_t> counts;
};

FailureCounts& Failures() {
  static FailureCounts* failures = new FailureCounts;
  return *failures;
}

// Each failure message is logged up to eight times on its own count, so a
// frequent one cannot hide a rarer one.
void LogFailure(const char* message) {
  FailureCounts& failures = Failures();
  {
    std::lock_guard<std::mutex> lock(failures.mutex);
    if (++failures.counts[message] > 8) {
      return;
    }
  }
  std::fprintf(stderr, "  [vulkan] ETC2 emulation: %s\n", message);
}

bool CreateStagingBlock(const HostDevice& dev, VkDeviceSize size,
                        StagingBlock* block, bool device_local = false) {
  if (dev.create_buffer == nullptr || dev.allocate_memory == nullptr ||
      dev.get_buffer_memory_requirements == nullptr ||
      dev.bind_buffer_memory == nullptr || dev.map_memory == nullptr ||
      dev.destroy_buffer == nullptr || dev.free_memory == nullptr ||
      size == 0 || (device_local && dev.gpu == nullptr)) {
    return false;
  }
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage =
      device_local ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                   : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  if (dev.create_buffer(dev.device, &buffer_info, nullptr, &buffer) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements{};
  dev.get_buffer_memory_requirements(dev.device, buffer, &requirements);
  const VkMemoryPropertyFlags required =
      device_local ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                   : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  // Device-local staging falls back to any type the buffer allows, such as
  // host memory the GPU reads over PCIe, once video memory runs out.
  std::vector<std::uint32_t> types;
  for (const VkMemoryPropertyFlags wanted :
       {required, device_local ? VkMemoryPropertyFlags{0} : required}) {
    for (std::uint32_t index = 0; index < dev.memory.memoryTypeCount;
         ++index) {
      if ((requirements.memoryTypeBits & (1U << index)) != 0 &&
          (dev.memory.memoryTypes[index].propertyFlags & wanted) == wanted &&
          std::find(types.begin(), types.end(), index) == types.end()) {
        types.push_back(index);
      }
    }
  }
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkMemoryAllocateInfo allocate_info{};
  allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate_info.allocationSize = requirements.size;
  void* data = nullptr;
  for (const std::uint32_t type : types) {
    allocate_info.memoryTypeIndex = type;
    if (dev.allocate_memory(dev.device, &allocate_info, nullptr, &memory) ==
        VK_SUCCESS) {
      break;
    }
    memory = VK_NULL_HANDLE;
  }
  if (memory == VK_NULL_HANDLE) {
    dev.destroy_buffer(dev.device, buffer, nullptr);
    return false;
  }
  if (dev.bind_buffer_memory(dev.device, buffer, memory, 0) != VK_SUCCESS ||
      (device_local
           ? !dev.gpu->CreateBinding(buffer, buffer, &block->binding)
           : dev.map_memory(dev.device, memory, 0, VK_WHOLE_SIZE, 0, &data) !=
                 VK_SUCCESS)) {
    dev.destroy_buffer(dev.device, buffer, nullptr);
    dev.free_memory(dev.device, memory, nullptr);
    return false;
  }
  block->device_local = device_local;
  block->device = dev.device;
  block->buffer = buffer;
  block->memory = memory;
  block->capacity = size;
  block->mapped = static_cast<std::uint8_t*>(data);
  return true;
}

#if defined(__x86_64__)
__attribute__((target("sse4.1")))
inline void CopyHostVisibleMemory(std::uint8_t* dst, const std::uint8_t* src,
                                  std::size_t bytes) {
  if ((reinterpret_cast<std::uintptr_t>(src) & 15) != 0 || bytes < 16) {
    std::memcpy(dst, src, bytes);
    return;
  }
  std::size_t i = 0;
  for (; i + 64 <= bytes; i += 64) {
    __m128i c0 = _mm_stream_load_si128(
        reinterpret_cast<__m128i*>(const_cast<std::uint8_t*>(src + i)));
    __m128i c1 = _mm_stream_load_si128(
        reinterpret_cast<__m128i*>(const_cast<std::uint8_t*>(src + i + 16)));
    __m128i c2 = _mm_stream_load_si128(
        reinterpret_cast<__m128i*>(const_cast<std::uint8_t*>(src + i + 32)));
    __m128i c3 = _mm_stream_load_si128(
        reinterpret_cast<__m128i*>(const_cast<std::uint8_t*>(src + i + 48)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), c0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 16), c1);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 32), c2);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 48), c3);
  }
  for (; i + 16 <= bytes; i += 16) {
    __m128i c = _mm_stream_load_si128(
        reinterpret_cast<__m128i*>(const_cast<std::uint8_t*>(src + i)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), c);
  }
  if (i < bytes) {
    std::memcpy(dst + i, src + i, bytes - i);
  }
}
#else
inline void CopyHostVisibleMemory(std::uint8_t* dst, const std::uint8_t* src,
                                  std::size_t bytes) {
  std::memcpy(dst, src, bytes);
}
#endif

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
      CopyHostVisibleMemory(target, source, upload.compressed);
    } else {
      const VkDeviceSize block_rows =
          (static_cast<VkDeviceSize>(upload.height) + 3) / 4;
      const VkDeviceSize packed_row = compressed_layer / block_rows;
      for (std::uint32_t layer = 0; layer < upload.layers; ++layer) {
        for (VkDeviceSize row = 0; row < block_rows; ++row) {
          CopyHostVisibleMemory(
              target + layer * compressed_layer + row * packed_row,
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
  if (block.binding.pool != VK_NULL_HANDLE && dev.gpu != nullptr) {
    dev.gpu->DestroyBinding(block.binding);
  }
  if (dev.destroy_buffer != nullptr) {
    dev.destroy_buffer(dev.device, block.buffer, nullptr);
  }
  if (dev.free_memory != nullptr) {
    dev.free_memory(dev.device, block.memory, nullptr);
  }
}

// Grows with the adaptive upscaler and shrinks with a box filter.
void ScaleRgba(const std::uint8_t* source, std::uint32_t source_width,
               std::uint32_t source_height, std::size_t stride,
               std::uint32_t width, std::uint32_t height,
               std::uint8_t* destination) {
  if (width >= source_width && height >= source_height &&
      (width > source_width || height > source_height)) {
    UpscaleRgba(source, source_width, source_height, stride, width, height,
                destination);
  } else {
    ResampleRgba(source, source_width, source_height, stride, width, height,
                 destination);
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

// Splits regions of a transcoded upload so each one stages within `cap`
// bytes: first into single layers, then into bands of block rows.
template <typename Region>
bool SplitForStaging(const ImageRecord& record, std::uint32_t count,
                     const Region* regions, VkDeviceSize cap,
                     std::vector<Region>* out) {
  const auto align = [](VkDeviceSize bytes) {
    return (bytes + kStagingAlignment - 1) / kStagingAlignment *
           kStagingAlignment;
  };
  const EtcFormat format = record.format.etc_format;
  const VkDeviceSize block_bytes = EtcBlockBytes(format);
  for (std::uint32_t index = 0; index < count; ++index) {
    const Region& region = regions[index];
    const std::uint32_t width = region.imageExtent.width;
    const std::uint32_t height = region.imageExtent.height;
    const VkDeviceSize row_texels =
        region.bufferRowLength != 0 ? region.bufferRowLength : width;
    const VkDeviceSize layer_rows =
        region.bufferImageHeight != 0 ? region.bufferImageHeight : height;
    const VkDeviceSize row_stride = (row_texels + 3) / 4 * block_bytes;
    const VkDeviceSize layer_stride = row_stride * ((layer_rows + 3) / 4);
    const std::uint32_t blocks_high = (height + 3) / 4;
    const auto need = [&](std::uint32_t rows, std::uint32_t layers) {
      Etc2GpuJob job;
      job.format = format;
      job.blocks_wide = (width + 3) / 4;
      job.blocks_high = rows;
      job.layers = layers;
      job.transcode = true;
      return align((layers - 1) * layer_stride + (rows - 1) * row_stride +
                   job.blocks_wide * block_bytes) +
             align(Etc2GpuOutputBytes(job));
    };
    const std::uint32_t layers = region.imageSubresource.layerCount;
    if (width == 0 || height == 0 || layers == 0 ||
        need(blocks_high, layers) <= cap) {
      out->push_back(region);
      continue;
    }
    if (need(1, 1) > cap) {
      return false;
    }
    std::uint32_t band = 1;
    for (std::uint32_t high = blocks_high; band < high;) {
      const std::uint32_t middle = band + (high - band + 1) / 2;
      if (need(middle, 1) <= cap) {
        band = middle;
      } else {
        high = middle - 1;
      }
    }
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
      for (std::uint32_t row = 0; row < blocks_high; row += band) {
        Region piece = region;
        piece.bufferOffset += layer * layer_stride + row * row_stride;
        piece.bufferImageHeight = 0;
        piece.imageSubresource.baseArrayLayer += layer;
        piece.imageSubresource.layerCount = 1;
        piece.imageOffset.y += static_cast<std::int32_t>(row * 4);
        piece.imageExtent.height = std::min(band * 4, height - row * 4);
        out->push_back(piece);
      }
    }
  }
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
    } else {
      const std::uint32_t mip = region.imageSubresource.mipLevel;
      upload.full_mip =
          region.imageSubresource.layerCount == 1 &&
          region.imageOffset.x == 0 && region.imageOffset.y == 0 &&
          upload.width == std::max<std::uint32_t>(1, record.width >> mip) &&
          upload.height == std::max<std::uint32_t>(1, record.height >> mip);
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
  // Sum over `images`; guarded by `mutex`.
  EmulatedTextureBytes texture_bytes;
  std::unordered_map<VkBuffer, BufferBinding> buffers;
  std::unordered_map<VkDeviceMemory, Mapping> mappings;
  std::unordered_map<VkDeviceMemory, bool> host_readable;
  std::unordered_map<VkDeviceMemory, std::uint64_t> deferred_reads;
  std::unordered_map<VkCommandBuffer, CommandRecord> commands;
  struct QueueFamily {
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t family = 0;
  };
  std::unordered_map<VkQueue, QueueFamily> queue_families;
  std::atomic<bool> has_commands{false};
  std::mutex pool_mutex;
  std::map<std::pair<VkDevice, std::uint32_t>, VkCommandPool> pools;
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
  const bool encode_budget =
      OnUnlessZero(std::getenv("MOCKTAIL_ETC2_ENCODE_BUDGET"));

  // Prepays an image's first full upload, probe-measured ns per block times
  // the host chain's blocks, at the largest scale the budget affords:
  // `*scale` only, or halving to 1 when `adaptive`. Returns the prepaid ns.
  // Zero leaves the image decoded, at 1x when `adaptive` so it adds no GPU
  // scale work either.
  double BudgetEncode(const HostDevice& dev, const VkImageCreateInfo& info,
                      bool adaptive, std::uint32_t* scale) {
    std::string line;
    double prepaid = 0;
    {
      std::lock_guard<std::mutex> lock(mutex);
      // Read under the lock so the budget and log never see time go back.
      const std::uint64_t now = TraceClockNanos();
      for (std::uint32_t candidate = *scale;;
           candidate = std::max<std::uint32_t>(1, candidate / 2)) {
        const double cost =
            static_cast<double>(Etc2HostBlockCount(
                info.extent, info.mipLevels, info.arrayLayers, candidate)) *
            (candidate > 1 ? dev.speed.upscale_ns : dev.speed.transcode_ns);
        if (dev.budget.Take(cost, now)) {
          *scale = candidate;
          ++dev.stats.by_scale[std::min<std::uint32_t>(candidate, 8)];
          prepaid = cost;
          break;
        }
        if (!adaptive || candidate == 1) {
          break;
        }
      }
      if (prepaid == 0) {
        ++dev.stats.held_back;
        if (adaptive) {
          *scale = 1;
        }
      }
      line = TakeEncodeLog(&dev.stats, now);
    }
    if (!line.empty()) {
      std::fputs(line.c_str(), stderr);
    }
    return prepaid;
  }

  void RefundEncode(const HostDevice& dev, double prepaid,
                    std::uint32_t scale) {
    if (prepaid > 0) {
      std::lock_guard<std::mutex> lock(mutex);
      dev.budget.Refund(prepaid);
      std::uint64_t& counted = dev.stats.by_scale[std::min<std::uint32_t>(scale, 8)];
      counted -= counted > 0 ? 1 : 0;
    }
  }

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

  void EmitUpload(const PendingUpload& upload, const std::uint8_t* compressed,
                  const std::uint8_t* decoded) {
    if (upload.keep_only) {
      StoreScaledUpload(upload, compressed, nullptr);
      return;
    }
    if (upload.scale_row_blocks != 0) {
      EmitScaleSource(upload, compressed);
      return;
    }
    const std::shared_ptr<const RgbaImage> replacement =
        FindOverride(upload, compressed, decoded);
    if (replacement != nullptr) {
      ResampleRgba(*replacement, upload.target_width, upload.target_height,
                   upload.target);
    } else if (upload.scale > 1) {
      const std::shared_ptr<const RgbaImage> base =
          StoreScaledUpload(upload, compressed, decoded);
      if (base != nullptr) {
        ScaleRgba(base->pixels.data(), base->width, base->height,
                  std::size_t{base->width} * 4, upload.target_width,
                  upload.target_height, upload.target);
        return;
      }
      ScaleRgba(decoded, upload.source_width, upload.source_height,
                std::size_t{upload.width} * 4, upload.target_width,
                upload.target_height, upload.target);
    } else {
      // The blocks are kept so a later copy into a BC image can re-encode
      // them.
      StoreScaledUpload(upload, compressed, nullptr);
      std::memcpy(upload.target, decoded, upload.decoded);
    }
  }

  // A full mip is scaled from the level-0 blocks once they are known,
  // anything else from its own blocks.
  void EmitScaleSource(const PendingUpload& upload,
                       const std::uint8_t* compressed) {
    StoreScaledUpload(upload, compressed, nullptr);
    const std::size_t block_bytes = EtcBlockBytes(upload.format);
    const std::size_t pitch = std::size_t{upload.scale_row_blocks} * block_bytes;
    std::uint8_t* blocks = upload.target + kEtc2ScaleHeaderBytes;
    std::uint32_t header[4] = {upload.source_width, upload.source_height,
                               upload.scale_row_blocks * 4, 0};
    std::shared_ptr<const std::vector<std::uint8_t>> level0;
    if (upload.full_mip) {
      std::lock_guard<std::mutex> lock(mutex);
      const auto record = images.find(upload.image);
      if (record != images.end() && !record->second.blocks.empty() &&
          record->second.blocks[0] != nullptr &&
          record->second.blocks[0]->size() ==
              pitch * ((record->second.height + 3) / 4)) {
        level0 = record->second.blocks[0];
        header[0] = record->second.width;
        header[1] = record->second.height;
      }
    }
    if (level0 != nullptr) {
      std::memcpy(blocks, level0->data(), level0->size());
    } else {
      const std::size_t own = (std::size_t{upload.width} + 3) / 4 * block_bytes;
      for (std::uint32_t row = 0; row < (upload.height + 3) / 4; ++row) {
        std::memcpy(blocks + row * pitch, compressed + row * own, own);
      }
    }
    std::memcpy(upload.target, header, sizeof(header));
  }

  std::shared_ptr<const RgbaImage> StoreScaledUpload(
      const PendingUpload& upload, const std::uint8_t* compressed,
      const std::uint8_t* decoded) {
    std::shared_ptr<const RgbaImage> base;
    if (upload.full_mip) {
        auto blocks = std::make_shared<const std::vector<std::uint8_t>>(
            compressed, compressed + upload.compressed);
        std::lock_guard<std::mutex> lock(mutex);
        const auto record = images.find(upload.image);
        if (record != images.end()) {
          std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>& mips =
              record->second.blocks;
          if (mips.size() <= upload.mip_level) {
            mips.resize(upload.mip_level + 1);
          }
          mips[upload.mip_level] = std::move(blocks);
          if (upload.mip_level == 0 && decoded != nullptr) {
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
    } else {
      // Part of a mip changed, so its kept blocks are stale.
      std::lock_guard<std::mutex> lock(mutex);
      const auto record = images.find(upload.image);
      if (record != images.end() &&
          upload.mip_level < record->second.blocks.size()) {
        record->second.blocks[upload.mip_level] = nullptr;
      }
    }
    return base;
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

  VkDeviceSize MaxGpuStagingBytes() const {
    return kMaxGpuStagingBlocks * staging_block_bytes;
  }

  // Takes `size` bytes from the first block of the device with room for
  // them, or from a new block of at least `staging_block_bytes`.
  bool AcquireStaging(const HostDevice& dev, VkDeviceSize size,
                      Staging* staging, bool device_local = false) {
    const VkDeviceSize aligned =
        (size + kStagingAlignment - 1) / kStagingAlignment * kStagingAlignment;
    if (device_local && aligned > MaxGpuStagingBytes()) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& block : staging_blocks) {
        if (block->device == dev.device &&
            block->device_local == device_local &&
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
      if (!CreateStagingBlock(dev, capacity, block.get(), device_local)) {
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

  const HostDevice* Find(VkDevice device) { return FindMutable(device); }

  HostDevice* FindMutable(VkDevice device) {
    std::lock_guard<std::mutex> lock(devices_mutex);
    for (const auto& candidate : devices) {
      if (candidate->device == device) {
        return candidate.get();
      }
    }
    return nullptr;
  }

  // Stages an upload for GPU decode or transcode. Each returned copy reads
  // one device-local staging range. Decoded uploads take one range or none;
  // transcoded images have no CPU path, so their regions are split across
  // ranges under the cap.
  template <typename Region>
  bool PlanGpuUpload(const HostDevice& dev, VkCommandBuffer command_buffer,
                     const ImageRecord& record, VkBuffer source,
                     VkImage destination, std::uint32_t count,
                     const Region* regions,
                     std::vector<GpuCopy<Region>>* copies,
                     bool keep_small_mips = true) {
    copies->clear();
    // A decoded image beside BC ones decodes on the CPU, which keeps its
    // blocks for the copies into a BC image that the GPU path cannot see.
    const bool decoded_beside_bc =
        !record.transcoded && IsColorFormat(record.format.etc_format) &&
        dev.bc_textures && dev.gpu != nullptr && dev.gpu->CanTranscode();
    if (dev.gpu == nullptr || dev.copy_buffer == nullptr ||
        record.scale != 1 || record.may_override || decoded_beside_bc) {
      return false;
    }
    std::vector<Region> split;
    if (record.transcoded) {
      if (regions == nullptr ||
          !SplitForStaging(record, count, regions, MaxGpuStagingBytes(),
                           &split)) {
        return false;
      }
      regions = split.data();
      count = static_cast<std::uint32_t>(split.size());
    }
    std::vector<PendingUpload> uploads;
    std::vector<Region> rewritten;
    VkDeviceSize ignored = 0;
    if (!PlanRegions(record, dev.device, source, destination, count, regions,
                     &rewritten, &uploads, &ignored)) {
      return false;
    }
    std::vector<GpuUpload> gpus;
    for (const PendingUpload& upload : uploads) {
      const VkDeviceSize block_bytes = EtcBlockBytes(upload.format);
      GpuUpload gpu;
      gpu.device = dev.device;
      gpu.source = source;
      gpu.source_offset = upload.source_offset;
      gpu.source_span = upload.source_span;
      gpu.job.format = upload.format;
      gpu.job.blocks_wide = (upload.width + 3) / 4;
      gpu.job.blocks_high = (upload.height + 3) / 4;
      gpu.job.layers = upload.layers;
      gpu.job.source_row_blocks =
          static_cast<std::uint32_t>(upload.source_row_stride / block_bytes);
      gpu.job.source_layer_blocks =
          static_cast<std::uint32_t>(upload.source_layer_stride / block_bytes);
      gpu.job.transcode = record.transcoded;
      gpus.push_back(gpu);
    }
    if (!StageGpuJobs(dev, command_buffer, destination, std::move(gpus),
                      rewritten, copies)) {
      return false;
    }
    // Small mips of a BC image keep their blocks on the CPU, so a copy into
    // an upscaled BC image can be scaled and encoded from them. A partial
    // upload drops them instead.
    std::vector<PendingUpload> kept;
    for (const PendingUpload& upload : uploads) {
      if (keep_small_mips && record.transcoded &&
          KeepsMipBlocks(record, upload.mip_level)) {
        PendingUpload keep = upload;
        keep.keep_only = true;
        keep.decoded = 0;
        keep.target_offset = 0;
        keep.target_bytes = 0;
        keep.target = nullptr;
        kept.push_back(keep);
      }
    }
    if (!kept.empty()) {
      std::lock_guard<std::mutex> lock(mutex);
      CommandRecord& command = commands[command_buffer];
      command.uploads.insert(command.uploads.end(), kept.begin(), kept.end());
      has_commands.store(true, std::memory_order_release);
    }
    return true;
  }

  // Upscaled images: the CPU only copies ETC2 blocks to staging at submit;
  // the GPU decodes, scales, then encodes BC7 for a compressed image.
  template <typename Region>
  bool PlanScaledGpuUpload(const HostDevice& dev,
                           VkCommandBuffer command_buffer,
                           const ImageRecord& record, VkBuffer source,
                           VkImage destination, std::uint32_t count,
                           const Region* regions,
                           std::vector<GpuCopy<Region>>* copies) {
    copies->clear();
    std::vector<PendingUpload> uploads;
    std::vector<Region> rewritten;
    VkDeviceSize total = 0;
    if (dev.gpu == nullptr || !dev.gpu->CanScale() ||
        dev.copy_buffer == nullptr || record.may_override ||
        !PlanRegions(record, dev.device, source, destination, count, regions,
                     &rewritten, &uploads, &total)) {
      return false;
    }
    // Every source fits the level-0 block grid, the largest it scales from.
    const std::uint32_t row_blocks = (record.width + 3) / 4;
    const std::uint32_t rows = (record.height + 3) / 4;
    total = 0;
    for (PendingUpload& upload : uploads) {
      upload.scale_row_blocks = row_blocks;
      upload.decoded = 0;
      upload.target_offset = total;
      upload.target_bytes = kEtc2ScaleHeaderBytes +
                            VkDeviceSize{row_blocks} * rows *
                                EtcBlockBytes(upload.format);
      total += (upload.target_bytes + 3) / 4 * 4;
    }
    Staging texels;
    if (!AcquireStaging(dev, total, &texels)) {
      return false;
    }
    std::vector<GpuUpload> gpus;
    for (PendingUpload& upload : uploads) {
      upload.target =
          texels.block->mapped + texels.offset + upload.target_offset;
      GpuUpload gpu = ScaleJob(dev.device, upload.format, row_blocks, rows,
                               upload.target_width, upload.target_height,
                               record.transcoded);
      gpu.source = texels.block->buffer;
      gpu.source_offset = texels.offset + upload.target_offset;
      gpu.source_span = upload.target_bytes;
      gpus.push_back(gpu);
    }
    if (!StageGpuJobs(dev, command_buffer, destination, std::move(gpus),
                      rewritten, copies)) {
      ReleaseStaging({texels});
      return false;
    }
    Record(command_buffer, std::move(uploads), texels);
    return true;
  }

  // Transcoded jobs are grouped into device-local ranges under the cap.
  template <typename Region>
  bool StageGpuJobs(const HostDevice& dev, VkCommandBuffer command_buffer,
                    VkImage image, std::vector<GpuUpload> gpus,
                    const std::vector<Region>& rewritten,
                    std::vector<GpuCopy<Region>>* copies) {
    double encode_ns = 0;
    const auto blocks = [](const Etc2GpuJob& job) {
      return static_cast<double>(job.blocks_wide) * job.blocks_high *
             job.layers;
    };
    for (const GpuUpload& gpu : gpus) {
      if (encode_budget && gpu.encode) {
        encode_ns += blocks(gpu.encoding) * dev.speed.upscale_ns;
      } else if (encode_budget && gpu.job.transcode) {
        encode_ns += blocks(gpu.job) * dev.speed.transcode_ns;
      }
    }
    const auto align = [](VkDeviceSize bytes) {
      return (bytes + kStagingAlignment - 1) / kStagingAlignment *
             kStagingAlignment;
    };
    // Offsets are relative to the range of their group until it is staged.
    std::vector<std::vector<GpuUpload>> groups(1);
    std::vector<VkDeviceSize> totals(1, 0);
    copies->assign(1, {});
    for (std::size_t index = 0; index < gpus.size(); ++index) {
      GpuUpload& gpu = gpus[index];
      const VkDeviceSize need =
          align(gpu.source_span) +
          (gpu.decode ? align(Etc2GpuOutputBytes(gpu.decoding)) : 0) +
          align(Etc2GpuOutputBytes(gpu.job)) +
          (gpu.encode ? align(Etc2GpuOutputBytes(gpu.encoding)) : 0);
      if ((gpu.job.transcode || gpu.job.scale) && !groups.back().empty() &&
          totals.back() + need > MaxGpuStagingBytes()) {
        groups.emplace_back();
        totals.push_back(0);
        copies->emplace_back();
      }
      VkDeviceSize cursor = totals.back() + align(gpu.source_span);
      if (gpu.decode) {
        gpu.decoding.source_offset = totals.back() + kEtc2ScaleHeaderBytes;
        gpu.decoding.destination_offset = cursor;
        gpu.job.scale_texels_offset = cursor;
        cursor += align(Etc2GpuOutputBytes(gpu.decoding));
      }
      gpu.job.source_offset = totals.back();
      gpu.job.destination_offset = cursor;
      gpu.encoding.source_offset = gpu.job.destination_offset;
      gpu.encoding.destination_offset =
          gpu.job.destination_offset + align(Etc2GpuOutputBytes(gpu.job));
      totals.back() += need;
      const Etc2GpuJob& last = gpu.encode ? gpu.encoding : gpu.job;
      Region region = rewritten[index];
      region.bufferOffset = last.destination_offset;
      region.bufferRowLength = last.transcode || last.scale
                                   ? last.blocks_wide * 4
                                   : Etc2GpuRowTexels(last.blocks_wide);
      region.bufferImageHeight = last.blocks_high * 4;
      copies->back().regions.push_back(region);
      groups.back().push_back(gpu);
    }
    std::vector<Staging> staged;
    for (std::size_t group = 0; group < groups.size(); ++group) {
      Staging staging;
      if (!AcquireStaging(dev, totals[group], &staging, true)) {
        ReleaseStaging(staged);
        copies->clear();
        return false;
      }
      staged.push_back(staging);
      for (GpuUpload& gpu : groups[group]) {
        gpu.staging = staging.block->buffer;
        gpu.binding = staging.block->binding;
        gpu.job.source_offset += staging.offset;
        gpu.job.destination_offset += staging.offset;
        if (gpu.decode) {
          gpu.decoding.source_offset += staging.offset;
          gpu.decoding.destination_offset += staging.offset;
          gpu.job.scale_texels_offset += staging.offset;
        }
        gpu.encoding.source_offset += staging.offset;
        gpu.encoding.destination_offset += staging.offset;
      }
      (*copies)[group].staging = staging.block->buffer;
      for (Region& region : (*copies)[group].regions) {
        region.bufferOffset += staging.offset;
      }
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (encode_ns > 0 && encode_budget) {
      // Re-uploads too: only the first is prepaid at creation.
      double prepaid = 0;
      if (const auto record = images.find(image); record != images.end()) {
        prepaid = std::min(record->second.prepaid_ns, encode_ns);
        record->second.prepaid_ns -= prepaid;
      }
      dev.budget.Charge(encode_ns - prepaid, TraceClockNanos());
      dev.stats.estimated_ns += encode_ns;
    }
    CommandRecord& command = commands[command_buffer];
    for (const std::vector<GpuUpload>& group : groups) {
      command.gpu_uploads.insert(command.gpu_uploads.end(), group.begin(),
                                 group.end());
    }
    command.staging.insert(command.staging.end(), staged.begin(),
                           staged.end());
    has_commands.store(true, std::memory_order_release);
    return true;
  }

  VkCommandBuffer RecordDecodeCommands(const HostDevice& dev,
                                       std::uint32_t family,
                                       const std::vector<GpuUpload>& uploads) {
    std::lock_guard<std::mutex> lock(pool_mutex);
    VkCommandPool& pool = pools[{dev.device, family}];
    if (pool == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo pool_info{};
      pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pool_info.queueFamilyIndex = family;
      if (dev.create_command_pool(dev.device, &pool_info, nullptr, &pool) !=
          VK_SUCCESS) {
        pool = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
      }
    }
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    if (dev.allocate_command_buffers(dev.device, &allocate, &command_buffer) !=
        VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    bool ok = dev.begin_command_buffer(command_buffer, &begin) == VK_SUCCESS;
    const auto barrier = [&](VkPipelineStageFlags source_stage,
                             VkAccessFlags source_access,
                             VkPipelineStageFlags destination_stage,
                             VkAccessFlags destination_access) {
      VkMemoryBarrier memory{};
      memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      memory.srcAccessMask = source_access;
      memory.dstAccessMask = destination_access;
      dev.pipeline_barrier(command_buffer, source_stage, destination_stage, 0, 1,
                           &memory, 0, nullptr, 0, nullptr);
    };
    constexpr VkPipelineStageFlags kStages =
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (ok) {
      // Host writes cover texels the CPU upscaled for an encode.
      barrier(kStages | VK_PIPELINE_STAGE_HOST_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                  VK_ACCESS_HOST_WRITE_BIT,
              kStages,
              VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
      for (const GpuUpload& upload : uploads) {
        const VkBufferCopy copy{upload.source_offset, upload.job.source_offset,
                                upload.source_span};
        dev.copy_buffer(command_buffer, upload.source, upload.staging, 1, &copy);
      }
      barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
      bool decode = false;
      for (const GpuUpload& upload : uploads) {
        ok = ok && (!upload.decode ||
                    dev.gpu->Record(command_buffer, upload.binding,
                                    &upload.decoding, 1));
        decode = decode || upload.decode;
      }
      if (decode) {
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
      }
      bool encode = false;
      for (const GpuUpload& upload : uploads) {
        ok = ok && dev.gpu->Record(command_buffer, upload.binding, &upload.job, 1);
        encode = encode || upload.encode;
      }
      if (encode) {
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        for (const GpuUpload& upload : uploads) {
          ok = ok && (!upload.encode ||
                      dev.gpu->Record(command_buffer, upload.binding,
                                      &upload.encoding, 1));
        }
      }
      barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      ok = dev.end_command_buffer(command_buffer) == VK_SUCCESS && ok;
    }
    if (!ok) {
      dev.free_command_buffers(dev.device, pool, 1, &command_buffer);
      return VK_NULL_HANDLE;
    }
    return command_buffer;
  }

  std::vector<VkCommandBuffer> GpuDecodeCommands(
      const VkCommandBuffer* command_buffers, std::uint32_t count,
      VkQueue queue) {
    std::vector<VkCommandBuffer> out;
    ChromeTraceWriter* const trace = ActiveProfileTrace();
    const std::uint64_t start_ns = trace != nullptr ? TraceClockNanos() : 0;
    std::int64_t uploads = 0;
    std::int64_t compressed_bytes = 0;
    std::int64_t decoded_bytes = 0;
    std::int64_t recorded_buffers = 0;
    std::lock_guard<std::mutex> lock(mutex);
    const auto queue_record = queue_families.find(queue);
    const auto add = [&](VkCommandBuffer command_buffer) {
      const auto record = commands.find(command_buffer);
      if (record == commands.end() || record->second.gpu_uploads.empty()) {
        return;
      }
      if (queue_record == queue_families.end()) {
        LogFailure("GPU-decoded upload submitted to an unknown queue");
        return;
      }
      for (const GpuUpload& upload : record->second.gpu_uploads) {
        ++uploads;
        compressed_bytes += static_cast<std::int64_t>(upload.source_span);
        decoded_bytes += static_cast<std::int64_t>(Etc2GpuOutputBytes(upload.job));
      }
      const std::uint32_t family = queue_record->second.family;
      for (const DecodeCommands& recorded : record->second.decode_commands) {
        if (recorded.family == family) {
          out.push_back(recorded.buffer);
          return;
        }
      }
      ++recorded_buffers;
      const VkDevice device = record->second.gpu_uploads.front().device;
      const HostDevice* dev = Find(device);
      const VkCommandBuffer recorded =
          dev == nullptr ? VK_NULL_HANDLE
                         : RecordDecodeCommands(*dev, family,
                                                record->second.gpu_uploads);
      if (recorded == VK_NULL_HANDLE) {
        LogFailure("could not record the GPU decode commands");
        return;
      }
      record->second.decode_commands.push_back({device, family, recorded});
      out.push_back(recorded);
    };
    for (std::uint32_t index = 0; index < count; ++index) {
      add(command_buffers[index]);
      const auto record = commands.find(command_buffers[index]);
      if (record != commands.end()) {
        for (VkCommandBuffer secondary : record->second.secondaries) {
          add(secondary);
        }
      }
    }
    if (trace != nullptr && uploads > 0) {
      trace->Slice("etc2 gpu decode", "texture", start_ns, TraceClockNanos(),
                   {{"uploads", uploads},
                    {"compressed_bytes", compressed_bytes},
                    {"decoded_bytes", decoded_bytes},
                    {"recorded", recorded_buffers}});
    }
    return out;
  }

  void FreeDecodeCommands(const std::vector<DecodeCommands>& doomed) {
    if (doomed.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(pool_mutex);
    for (const DecodeCommands& decode : doomed) {
      const HostDevice* dev = Find(decode.device);
      const auto pool = pools.find({decode.device, decode.family});
      if (dev != nullptr && pool != pools.end() &&
          dev->free_command_buffers != nullptr) {
        dev->free_command_buffers(dev->device, pool->second, 1,
                                  &decode.buffer);
      }
    }
  }

  bool RedirectImageCopy(const HostDevice& dev, VkCommandBuffer command_buffer,
                         VkImage source, VkImageLayout source_layout,
                         VkImage destination,
                         VkImageLayout destination_layout, std::uint32_t count,
                         const VkImageCopy* regions);

  void TranscodeCopy(const HostDevice& dev, VkCommandBuffer command_buffer,
                     VkImage source_image, const ImageRecord& source,
                     VkImage destination,
                     const ImageRecord& destination_record,
                     VkImageLayout destination_layout, std::uint32_t count,
                     const VkImageCopy* regions);

  void ScaledTranscodeCopy(const HostDevice& dev,
                           VkCommandBuffer command_buffer,
                           VkImage source_image, const ImageRecord& source,
                           VkImage destination,
                           const ImageRecord& destination_record,
                           VkImageLayout destination_layout,
                           std::uint32_t count, const VkImageCopy* regions);

  bool CopyScaledBlocks(const HostDevice& dev, VkCommandBuffer command_buffer,
                        VkImage source, VkImageLayout source_layout,
                        const ImageRecord& source_record, VkImage destination,
                        VkImageLayout destination_layout,
                        const ImageRecord& destination_record,
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
    DecodeAndEmit(batch.uploads, batch.source.data(), batch.decoded.data(),
                  batch.block_copies);
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
                     std::uint8_t* source_base, std::uint8_t* decoded_base,
                     const std::vector<BlockCopy>& block_copies) {
    std::vector<EtcDecodeJob> jobs;
    VkDeviceSize compressed_offset = 0;
    VkDeviceSize decoded_offset = 0;
    for (const PendingUpload& upload : uploads) {
      std::uint8_t* source = source_base + compressed_offset;
      std::uint8_t* decoded = decoded_base + decoded_offset;
      const VkDeviceSize compressed_layer = upload.compressed / upload.layers;
      const VkDeviceSize decoded_layer = upload.decoded / upload.layers;
      for (std::uint32_t layer = 0; layer < upload.layers &&
                                    upload.scale_row_blocks == 0 &&
                                    !upload.keep_only;
           ++layer) {
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
      if (!upload.keep_only && ShouldLog(&g_decode_logs, 4)) {
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
    FillBlockCopies(block_copies);
  }

  // Writes `region` (copy.wide x copy.high blocks, or null when unknown) into
  // the destination's kept blocks. A region that covers only part of the mip
  // patches the kept blocks, and drops them when there are none to patch, so
  // no later copy reads stale texels.
  void KeepBlocks(const BlockCopy& copy, const std::vector<std::uint8_t>* region) {
    if (copy.keep == VK_NULL_HANDLE) {
      return;
    }
    const std::size_t row_bytes = std::size_t{copy.wide} * copy.block_bytes;
    const std::size_t mip_bytes = std::size_t{copy.keep_blocks_wide} *
                                  copy.keep_blocks_high * copy.block_bytes;
    const bool fits = copy.keep_x + copy.wide <= copy.keep_blocks_wide &&
                      copy.keep_y + copy.high <= copy.keep_blocks_high;
    const bool whole = fits && copy.keep_x == 0 && copy.keep_y == 0 &&
                       copy.wide == copy.keep_blocks_wide &&
                       copy.high == copy.keep_blocks_high;
    std::lock_guard<std::mutex> lock(mutex);
    const auto record = images.find(copy.keep);
    if (record == images.end()) {
      return;
    }
    auto& mips = record->second.blocks;
    if (mips.size() <= copy.keep_mip) {
      mips.resize(copy.keep_mip + 1);
    }
    std::shared_ptr<const std::vector<std::uint8_t>>& kept = mips[copy.keep_mip];
    if (region == nullptr || !fits) {
      kept = nullptr;
    } else if (whole) {
      kept = std::make_shared<const std::vector<std::uint8_t>>(*region);
    } else if (kept != nullptr && kept->size() == mip_bytes) {
      auto patched = std::make_shared<std::vector<std::uint8_t>>(*kept);
      for (std::uint32_t row = 0; row < copy.high; ++row) {
        std::memcpy(patched->data() +
                        (std::size_t{copy.keep_y + row} * copy.keep_blocks_wide +
                         copy.keep_x) *
                            copy.block_bytes,
                    region->data() + row * row_bytes, row_bytes);
      }
      kept = std::move(patched);
    } else {
      kept = nullptr;
    }
  }

  void FillBlockCopies(const std::vector<BlockCopy>& block_copies) {
    for (const BlockCopy& copy : block_copies) {
      const std::size_t row_bytes = std::size_t{copy.wide} * copy.block_bytes;
      std::uint8_t* const target =
          copy.target == nullptr
              ? nullptr
              : copy.target + (copy.scale_header ? kEtc2ScaleHeaderBytes : 0);
      if (copy.scale_header && copy.target != nullptr) {
        const std::uint32_t header[4] = {copy.texels_wide, copy.texels_high,
                                         copy.wide * 4, 0};
        std::memcpy(copy.target, header, sizeof(header));
      }
      std::shared_ptr<const std::vector<std::uint8_t>> blocks;
      {
        std::lock_guard<std::mutex> lock(mutex);
        const auto record = images.find(copy.image);
        if (record != images.end() &&
            copy.mip < record->second.blocks.size()) {
          blocks = record->second.blocks[copy.mip];
        }
      }
      if (blocks == nullptr ||
          copy.x + copy.wide > copy.mip_blocks_wide ||
          copy.y + copy.high > copy.mip_blocks_high ||
          blocks->size() < std::size_t{copy.mip_blocks_wide} *
                               copy.mip_blocks_high * copy.block_bytes) {
        if (target != nullptr) {
          LogFailure("copied into a compressed image without its source");
          std::memset(target, 0, row_bytes * copy.high);
        }
        KeepBlocks(copy, nullptr);
        continue;
      }
      std::vector<std::uint8_t> region(row_bytes * copy.high);
      for (std::uint32_t row = 0; row < copy.high; ++row) {
        std::memcpy(region.data() + row * row_bytes,
                    blocks->data() + (std::size_t{copy.y + row} *
                                          copy.mip_blocks_wide +
                                      copy.x) *
                                         copy.block_bytes,
                    row_bytes);
      }
      if (target != nullptr) {
        std::memcpy(target, region.data(), region.size());
      }
      KeepBlocks(copy, &region);
    }
  }

  std::uint64_t IssuedTicket() {
    std::lock_guard<std::mutex> lock(queue_mutex);
    return issued_ticket;
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

  void WaitForReadsOf(VkDeviceMemory memory) {
    std::uint64_t ticket = 0;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto found = deferred_reads.find(memory);
      if (found == deferred_reads.end()) {
        return;
      }
      ticket = found->second;
    }
    WaitForTicket(ticket);
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = deferred_reads.find(memory);
    if (found != deferred_reads.end() && found->second <= ticket) {
      deferred_reads.erase(found);
    }
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
    PFN_vkGetDeviceProcAddr get_device_proc_addr, bool timeline_semaphore,
    bool bc_textures) {
  static_cast<void>(physical_device);
  if (device == VK_NULL_HANDLE || get_device_proc_addr == nullptr) {
    return;
  }
  auto dev = std::make_unique<HostDevice>();
  const auto get = get_device_proc_addr;
  dev->device = device;
  dev->emulated = emulated;
  dev->bc_textures = emulated && bc_textures;
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
  dev->get_device_proc_addr = get;
  dev->copy_buffer = DeviceProc<PFN_vkCmdCopyBuffer>(get, device, "vkCmdCopyBuffer");
  dev->create_command_pool =
      DeviceProc<PFN_vkCreateCommandPool>(get, device, "vkCreateCommandPool");
  dev->destroy_command_pool =
      DeviceProc<PFN_vkDestroyCommandPool>(get, device, "vkDestroyCommandPool");
  dev->allocate_command_buffers = DeviceProc<PFN_vkAllocateCommandBuffers>(
      get, device, "vkAllocateCommandBuffers");
  dev->free_command_buffers =
      DeviceProc<PFN_vkFreeCommandBuffers>(get, device, "vkFreeCommandBuffers");
  dev->begin_command_buffer =
      DeviceProc<PFN_vkBeginCommandBuffer>(get, device, "vkBeginCommandBuffer");
  dev->end_command_buffer =
      DeviceProc<PFN_vkEndCommandBuffer>(get, device, "vkEndCommandBuffer");
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
  EmulatedTextureBytes totals;
  std::uint64_t now = 0;
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
      if (it->second.device == device) {
        state_->texture_bytes.compressed -= it->second.bytes.compressed;
        state_->texture_bytes.decoded -= it->second.bytes.decoded;
        it = state_->images.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = state_->queue_families.begin();
         it != state_->queue_families.end();) {
      it = it->second.device == device ? state_->queue_families.erase(it)
                                       : std::next(it);
    }
    totals = state_->texture_bytes;
    now = TraceClockNanos();
    state_->has_commands.store(!state_->commands.empty(),
                               std::memory_order_release);
  }
  TraceTextureBytes(totals, now);
  if (const HostDevice* dev = state_->Find(device); dev != nullptr) {
    {
      std::lock_guard<std::mutex> lock(state_->pool_mutex);
      for (auto it = state_->pools.begin(); it != state_->pools.end();) {
        if (it->first.first == device) {
          if (it->second != VK_NULL_HANDLE &&
              dev->destroy_command_pool != nullptr) {
            dev->destroy_command_pool(device, it->second, nullptr);
          }
          it = state_->pools.erase(it);
        } else {
          ++it;
        }
      }
    }
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
  host_info.flags &= ~static_cast<VkImageCreateFlags>(
      VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT);
  const bool adaptive = state_->upscale == kAdaptiveSmallTextureUpscale;
  std::uint32_t scale = 1;
  if (IsColorFormat(format.etc_format) &&
      create_info->imageType == VK_IMAGE_TYPE_2D &&
      create_info->arrayLayers == 1 && create_info->extent.depth == 1 &&
      create_info->extent.width <= kSmallTextureMaxExtent &&
      create_info->extent.height <= kSmallTextureMaxExtent) {
    scale = adaptive ? dev->adaptive_upscale : state_->upscale;
  }
  const bool may_override =
      IsColorFormat(format.etc_format) && create_info->arrayLayers == 1 &&
      state_->overrides.MayReplace(create_info->extent.width,
                                   create_info->extent.height);
  constexpr VkImageUsageFlags kBcUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT;
  // A mutable image may carry a view format list naming ETC2 formats, which
  // only the decoded path has always passed through. Drivers rarely support
  // BC with linear tiling.
  VkFormat bc_format = VK_FORMAT_UNDEFINED;
  bool transcoded =
      dev->bc_textures && dev->gpu != nullptr &&
      dev->gpu->CanTranscode() && !may_override &&
      (scale == 1 || dev->gpu->CanScale()) &&
      create_info->imageType == VK_IMAGE_TYPE_2D &&
      create_info->tiling == VK_IMAGE_TILING_OPTIMAL &&
      (create_info->usage & ~kBcUsage) == 0 &&
      (create_info->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == 0 &&
      BcHostFormat(create_info->format, &bc_format);
  double prepaid = 0;
  if (transcoded && state_->encode_budget && dev->speed.transcode_ns > 0) {
    prepaid = state_->BudgetEncode(*dev, *create_info, adaptive, &scale);
    transcoded = prepaid > 0;
  }
  if (transcoded) {
    format.host_format = bc_format;
  }
  host_info.extent.width *= scale;
  host_info.extent.height *= scale;
  host_info.format = format.host_format;
  VkResult result = dev->create_image(device, &host_info, allocator, image);
  if (transcoded && result != VK_SUCCESS) {
    state_->RefundEncode(*dev, prepaid, scale);
    prepaid = 0;
    transcoded = false;
    LookupEmulatedEtc2Format(create_info->format, &format);
    host_info.format = format.host_format;
    result = dev->create_image(device, &host_info, allocator, image);
  }
  if (result == VK_SUCCESS && image != nullptr) {
    EmulatedTextureBytes bytes;
    if (!Etc2ImageByteCounts(format.etc_format, create_info->extent,
                             create_info->mipLevels, create_info->arrayLayers,
                             scale, &bytes.compressed, &bytes.decoded)) {
      bytes = {};
    }
    if (transcoded) {
      bytes.decoded =
          Etc2HostBlockCount(create_info->extent, create_info->mipLevels,
                             create_info->arrayLayers, scale) *
          BcBlockBytes(BcFormatFor(format.etc_format));
    }
    EmulatedTextureBytes totals;
    std::uint64_t now = 0;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      ImageRecord& record = state_->images[*image];
      record = {};
      record.device = device;
      record.format = format;
      record.scale = scale;
      record.width = create_info->extent.width;
      record.height = create_info->extent.height;
      record.bytes = bytes;
      record.transcoded = transcoded;
      record.prepaid_ns = prepaid;
      record.may_override = may_override;
      state_->texture_bytes.compressed += bytes.compressed;
      state_->texture_bytes.decoded += bytes.decoded;
      totals = state_->texture_bytes;
      now = TraceClockNanos();
    }
    TraceTextureBytes(totals, now);
    if (ShouldLog(transcoded ? &g_transcoded_image_logs : &g_image_logs, 4)) {
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
    EmulatedTextureBytes totals;
    std::uint64_t now = 0;
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      const auto it = state_->images.find(image);
      if (it != state_->images.end()) {
        dev->budget.Refund(it->second.prepaid_ns);
        state_->texture_bytes.compressed -= it->second.bytes.compressed;
        state_->texture_bytes.decoded -= it->second.bytes.decoded;
        state_->images.erase(it);
        totals = state_->texture_bytes;
        now = TraceClockNanos();
        changed = true;
      }
    }
    if (changed) {
      TraceTextureBytes(totals, now);
    }
  }
  dev->destroy_image(device, image, allocator);
}

EmulatedTextureBytes VulkanEtc2Emulation::TextureBytes() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->texture_bytes;
}

Etc2GpuSpeed VulkanEtc2Emulation::GpuSpeed(VkDevice device) const {
  const HostDevice* dev = state_->Find(device);
  return dev == nullptr ? Etc2GpuSpeed{} : dev->speed;
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
  ImageRecord record;
  if (state_->LookupImage(*dev, create_info->image, &record) &&
      record.transcoded) {
    BcHostFormat(create_info->format, &host_info.format);
  }
  return dev->create_image_view(device, &host_info, allocator, view);
}

std::uint64_t Etc2FailureCount(const char* message) {
  FailureCounts& failures = Failures();
  std::lock_guard<std::mutex> lock(failures.mutex);
  std::uint64_t total = 0;
  for (const auto& [key, count] : failures.counts) {
    if (std::strcmp(key, message) == 0) {
      total += count;
    }
  }
  return total;
}

void VulkanEtc2Emulation::TrackAllocation(VkDevice device,
                                          const VkMemoryAllocateInfo* info,
                                          VkDeviceMemory memory) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || !dev->emulated || info == nullptr ||
      memory == VK_NULL_HANDLE ||
      info->memoryTypeIndex >= dev->memory.memoryTypeCount) {
    return;
  }
  const bool readable =
      (dev->memory.memoryTypes[info->memoryTypeIndex].propertyFlags &
       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->host_readable[memory] = readable;
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
    state_->WaitForReadsOf(memory);
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
    state_->WaitForReadsOf(info->memory);
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
    state_->WaitForReadsOf(memory);
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->mappings.erase(memory);
    state_->host_readable.erase(memory);
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
  std::vector<GpuCopy<VkBufferImageCopy>> copies;
  if (state_->PlanGpuUpload(*dev, command_buffer, record, source, destination,
                            region_count, regions, &copies)) {
    for (const GpuCopy<VkBufferImageCopy>& copy : copies) {
      dev->copy_buffer_to_image(
          command_buffer, copy.staging, destination, layout,
          static_cast<std::uint32_t>(copy.regions.size()), copy.regions.data());
    }
    return;
  }
  if (record.scale > 1 &&
      state_->PlanScaledGpuUpload(*dev, command_buffer, record, source,
                                  destination, region_count, regions,
                                  &copies)) {
    for (const GpuCopy<VkBufferImageCopy>& copy : copies) {
      dev->copy_buffer_to_image(
          command_buffer, copy.staging, destination, layout,
          static_cast<std::uint32_t>(copy.regions.size()), copy.regions.data());
    }
    return;
  }
  if (record.transcoded) {
    LogFailure("skipped an upload into a compressed image");
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
  std::vector<GpuCopy<VkBufferImageCopy2>> copies;
  if (state_->PlanGpuUpload(*dev, command_buffer, record, info->srcBuffer,
                            info->dstImage, info->regionCount, info->pRegions,
                            &copies)) {
    for (const GpuCopy<VkBufferImageCopy2>& copy : copies) {
      VkCopyBufferToImageInfo2 host_info = *info;
      host_info.srcBuffer = copy.staging;
      host_info.regionCount = static_cast<std::uint32_t>(copy.regions.size());
      host_info.pRegions = copy.regions.data();
      dev->copy_buffer_to_image2(command_buffer, &host_info);
    }
    return;
  }
  if (record.scale > 1 &&
      state_->PlanScaledGpuUpload(*dev, command_buffer, record,
                                  info->srcBuffer, info->dstImage,
                                  info->regionCount, info->pRegions,
                                  &copies)) {
    for (const GpuCopy<VkBufferImageCopy2>& copy : copies) {
      VkCopyBufferToImageInfo2 host_info = *info;
      host_info.srcBuffer = copy.staging;
      host_info.regionCount = static_cast<std::uint32_t>(copy.regions.size());
      host_info.pRegions = copy.regions.data();
      dev->copy_buffer_to_image2(command_buffer, &host_info);
    }
    return;
  }
  if (record.transcoded) {
    LogFailure("skipped an upload into a compressed image");
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

// Re-encodes the compressed blocks kept for a scaled source into a BC
// destination, through the same GPU transcode as an upload.
void VulkanEtc2Emulation::State::TranscodeCopy(
    const HostDevice& dev, VkCommandBuffer command_buffer,
    VkImage source_image, const ImageRecord& source, VkImage destination,
    const ImageRecord& destination_record, VkImageLayout destination_layout,
    std::uint32_t count, const VkImageCopy* regions) {
  const VkDeviceSize block_bytes = EtcBlockBytes(source.format.etc_format);
  if (dev.copy_buffer_to_image == nullptr ||
      source.format.etc_format != destination_record.format.etc_format) {
    LogFailure("skipped a copy between different ETC2 formats");
    return;
  }
  std::vector<BlockCopy> pieces;
  std::vector<VkBufferImageCopy> uploads;
  VkDeviceSize total = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    const VkImageCopy& region = regions[index];
    const std::uint32_t mip = region.srcSubresource.mipLevel;
    const std::uint32_t mip_wide =
        (std::max<std::uint32_t>(1, source.width >> mip) + 3) / 4;
    const std::uint32_t mip_high =
        (std::max<std::uint32_t>(1, source.height >> mip) + 3) / 4;
    BlockCopy piece;
    piece.image = source_image;
    piece.mip = mip;
    piece.block_bytes = block_bytes;
    piece.mip_blocks_wide = mip_wide;
    piece.mip_blocks_high = mip_high;
    piece.wide = (region.extent.width + 3) / 4;
    piece.high = (region.extent.height + 3) / 4;
    if (region.srcSubresource.layerCount != 1 ||
        region.dstSubresource.layerCount != 1 || region.srcOffset.x < 0 ||
        region.srcOffset.y < 0 || region.srcOffset.x % 4 != 0 ||
        region.srcOffset.y % 4 != 0 || piece.wide == 0 || piece.high == 0) {
      LogFailure("skipped a copy into a compressed image with a bad region");
      continue;
    }
    piece.x = static_cast<std::uint32_t>(region.srcOffset.x) / 4;
    piece.y = static_cast<std::uint32_t>(region.srcOffset.y) / 4;
    if (piece.x + piece.wide > mip_wide || piece.y + piece.high > mip_high) {
      LogFailure("skipped a copy into a compressed image with a bad region");
      continue;
    }
    // Kept blocks hold layer 0 only.
    if (region.dstSubresource.baseArrayLayer == 0 &&
        KeepsMipBlocks(destination_record, region.dstSubresource.mipLevel)) {
      KeepIn(&piece, destination, destination_record,
             region.dstSubresource.mipLevel, region.dstOffset);
    }
    VkBufferImageCopy upload{};
    upload.bufferOffset = total;
    upload.imageSubresource = region.dstSubresource;
    upload.imageOffset = region.dstOffset;
    upload.imageExtent = region.extent;
    uploads.push_back(upload);
    pieces.push_back(piece);
    total += VkDeviceSize{piece.wide} * piece.high * block_bytes;
  }
  Staging staging;
  if (uploads.empty() || !AcquireStaging(dev, total, &staging)) {
    return;
  }
  for (std::size_t index = 0; index < pieces.size(); ++index) {
    pieces[index].target =
        staging.block->mapped + staging.offset + uploads[index].bufferOffset;
    uploads[index].bufferOffset += staging.offset;
  }
  std::vector<GpuCopy<VkBufferImageCopy>> copies;
  if (!PlanGpuUpload(dev, command_buffer, destination_record,
                     staging.block->buffer, destination,
                     static_cast<std::uint32_t>(uploads.size()),
                     uploads.data(), &copies, false)) {
    ReleaseStaging({staging});
    LogFailure("skipped a copy into a compressed image");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    CommandRecord& command = commands[command_buffer];
    command.staging.push_back(staging);
    command.block_copies.insert(command.block_copies.end(), pieces.begin(),
                                pieces.end());
    has_commands.store(true, std::memory_order_release);
  }
  for (const GpuCopy<VkBufferImageCopy>& copy : copies) {
    dev.copy_buffer_to_image(command_buffer, copy.staging, destination,
                             destination_layout,
                             static_cast<std::uint32_t>(copy.regions.size()),
                             copy.regions.data());
  }
}

// A copy from an unscaled image that fills a whole mip of an upscaled BC
// image is decoded, scaled and encoded on the GPU from the source mip's kept
// blocks, which the destination mip then keeps as its own.
void VulkanEtc2Emulation::State::ScaledTranscodeCopy(
    const HostDevice& dev, VkCommandBuffer command_buffer,
    VkImage source_image, const ImageRecord& source, VkImage destination,
    const ImageRecord& destination_record, VkImageLayout destination_layout,
    std::uint32_t count, const VkImageCopy* regions) {
  const EtcFormat format = source.format.etc_format;
  if (dev.copy_buffer_to_image == nullptr || dev.gpu == nullptr ||
      !dev.gpu->CanScale() || dev.copy_buffer == nullptr ||
      format != destination_record.format.etc_format) {
    LogFailure("skipped a copy into an upscaled compressed image");
    return;
  }
  const VkDeviceSize block_bytes = EtcBlockBytes(format);
  std::vector<BlockCopy> pieces;
  // Offset of each piece's scale source in the texel staging.
  std::vector<VkDeviceSize> offsets;
  std::vector<VkBufferImageCopy> rewritten;
  std::vector<GpuUpload> gpus;
  VkDeviceSize total = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    const VkImageCopy& region = regions[index];
    const std::uint32_t source_mip = region.srcSubresource.mipLevel;
    const std::uint32_t destination_mip = region.dstSubresource.mipLevel;
    // The region fills the whole destination mip from a block-aligned part
    // of the source mip.
    const std::uint32_t wide = std::max<std::uint32_t>(
        1, destination_record.width >> destination_mip);
    const std::uint32_t high = std::max<std::uint32_t>(
        1, destination_record.height >> destination_mip);
    const std::uint32_t source_wide =
        std::max<std::uint32_t>(1, source.width >> source_mip);
    const std::uint32_t source_high =
        std::max<std::uint32_t>(1, source.height >> source_mip);
    // Kept blocks are read as layer 0.
    if (region.srcSubresource.layerCount != 1 ||
        region.srcSubresource.baseArrayLayer != 0 ||
        region.dstSubresource.layerCount != 1 || region.srcOffset.x < 0 ||
        region.srcOffset.y < 0 || region.srcOffset.x % 4 != 0 ||
        region.srcOffset.y % 4 != 0 || region.dstOffset.x != 0 ||
        region.dstOffset.y != 0 || region.extent.width < wide ||
        region.extent.height < high ||
        static_cast<std::uint32_t>(region.srcOffset.x) + wide > source_wide ||
        static_cast<std::uint32_t>(region.srcOffset.y) + high >
            source_high) {
      LogFailure("skipped a copy into an upscaled compressed image");
      return;
    }
    if (!KeepsMipBlocks(source, source_mip)) {
      LogFailure("skipped a copy from a mip too large to keep");
      return;
    }
    VkBufferImageCopy app{};
    app.imageSubresource = region.dstSubresource;
    app.imageExtent = {wide, high, 1};
    std::vector<VkBufferImageCopy> host;
    std::vector<PendingUpload> planned;
    VkDeviceSize ignored = 0;
    if (!PlanRegions(destination_record, dev.device, VK_NULL_HANDLE,
                     destination, 1, &app, &host, &planned, &ignored)) {
      LogFailure("skipped a copy into an upscaled compressed image");
      return;
    }
    BlockCopy piece;
    piece.image = source_image;
    piece.mip = source_mip;
    piece.block_bytes = block_bytes;
    piece.mip_blocks_wide = (source_wide + 3) / 4;
    piece.mip_blocks_high = (source_high + 3) / 4;
    piece.x = static_cast<std::uint32_t>(region.srcOffset.x) / 4;
    piece.y = static_cast<std::uint32_t>(region.srcOffset.y) / 4;
    piece.wide = (wide + 3) / 4;
    piece.high = (high + 3) / 4;
    KeepIn(&piece, destination, destination_record, destination_mip,
           {0, 0, 0});
    piece.scale_header = true;
    piece.texels_wide = wide;
    piece.texels_high = high;
    const VkDeviceSize bytes =
        kEtc2ScaleHeaderBytes +
        VkDeviceSize{piece.wide} * piece.high * block_bytes;
    GpuUpload gpu = ScaleJob(dev.device, format, piece.wide, piece.high,
                             planned.front().target_width,
                             planned.front().target_height, true);
    gpu.source_offset = total;
    gpu.source_span = bytes;
    gpus.push_back(gpu);
    rewritten.push_back(host.front());
    pieces.push_back(piece);
    offsets.push_back(total);
    total += (bytes + 3) / 4 * 4;
  }
  Staging texels;
  if (gpus.empty() || !AcquireStaging(dev, total, &texels)) {
    LogFailure("skipped a copy into an upscaled compressed image");
    return;
  }
  for (std::size_t index = 0; index < pieces.size(); ++index) {
    pieces[index].target = texels.block->mapped + texels.offset + offsets[index];
  }
  for (GpuUpload& gpu : gpus) {
    gpu.source = texels.block->buffer;
    gpu.source_offset += texels.offset;
  }
  std::vector<GpuCopy<VkBufferImageCopy>> copies;
  if (!StageGpuJobs(dev, command_buffer, destination, std::move(gpus),
                    rewritten, &copies)) {
    ReleaseStaging({texels});
    LogFailure("skipped a copy into an upscaled compressed image");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    CommandRecord& command = commands[command_buffer];
    command.staging.push_back(texels);
    command.block_copies.insert(command.block_copies.end(), pieces.begin(),
                                pieces.end());
    has_commands.store(true, std::memory_order_release);
  }
  for (const GpuCopy<VkBufferImageCopy>& copy : copies) {
    dev.copy_buffer_to_image(command_buffer, copy.staging, destination,
                             destination_layout,
                             static_cast<std::uint32_t>(copy.regions.size()),
                             copy.regions.data());
  }
}

// Copies between upscaled BC7 images of one scale move whole blocks on host
// bounds, and update the destination's kept blocks to match.
bool VulkanEtc2Emulation::State::CopyScaledBlocks(
    const HostDevice& dev, VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, const ImageRecord& source_record,
    VkImage destination, VkImageLayout destination_layout,
    const ImageRecord& destination_record, std::uint32_t count,
    const VkImageCopy* regions) {
  if (dev.copy_image == nullptr) {
    LogFailure("skipped a copy between upscaled compressed images");
    return true;
  }
  std::vector<VkImageCopy> copies;
  copies.reserve(count);
  std::vector<BlockCopy> kept;
  for (std::uint32_t index = 0; index < count; ++index) {
    const VkImageCopy& region = regions[index];
    const std::uint32_t source_mip = region.srcSubresource.mipLevel;
    // Kept blocks hold layer 0 only. A copy from another layer into layer 0
    // leaves them unknown, which a null source image records.
    if (region.dstSubresource.baseArrayLayer == 0) {
      BlockCopy keep;
      if (region.srcSubresource.baseArrayLayer == 0 &&
          region.srcSubresource.layerCount == 1) {
        keep.image = source;
      }
      keep.mip = source_mip;
      keep.block_bytes = EtcBlockBytes(source_record.format.etc_format);
      keep.mip_blocks_wide =
          (std::max<std::uint32_t>(1, source_record.width >> source_mip) + 3) /
          4;
      keep.mip_blocks_high =
          (std::max<std::uint32_t>(1, source_record.height >> source_mip) +
           3) /
          4;
      keep.x = static_cast<std::uint32_t>(std::max(region.srcOffset.x, 0)) / 4;
      keep.y = static_cast<std::uint32_t>(std::max(region.srcOffset.y, 0)) / 4;
      keep.wide = (region.extent.width + 3) / 4;
      keep.high = (region.extent.height + 3) / 4;
      KeepIn(&keep, destination, destination_record,
             region.dstSubresource.mipLevel, region.dstOffset);
      kept.push_back(keep);
    }
    VkImageCopy copy = regions[index];
    VkOffset3D source_end{};
    VkOffset3D destination_end{};
    if (!HostRegion(&source_record, copy.srcSubresource.mipLevel,
                    regions[index].srcOffset, regions[index].extent,
                    &copy.srcOffset, &source_end) ||
        !HostRegion(&destination_record, copy.dstSubresource.mipLevel,
                    regions[index].dstOffset, regions[index].extent,
                    &copy.dstOffset, &destination_end)) {
      LogFailure("skipped an image copy with an unsupported region");
      return true;
    }
    copy.extent = {
        static_cast<std::uint32_t>(std::min(source_end.x - copy.srcOffset.x,
                                            destination_end.x -
                                                copy.dstOffset.x)),
        static_cast<std::uint32_t>(std::min(source_end.y - copy.srcOffset.y,
                                            destination_end.y -
                                                copy.dstOffset.y)),
        1};
    copies.push_back(copy);
  }
  if (!kept.empty()) {
    std::lock_guard<std::mutex> lock(mutex);
    CommandRecord& command = commands[command_buffer];
    command.block_copies.insert(command.block_copies.end(), kept.begin(),
                                kept.end());
    has_commands.store(true, std::memory_order_release);
  }
  dev.copy_image(command_buffer, source, source_layout, destination,
                 destination_layout, static_cast<std::uint32_t>(copies.size()),
                 copies.data());
  return true;
}

// Copies between host images that differ in scale or block size: a scaled
// side becomes a blit onto its own host bounds, a BC source is decompressed
// by a blit, and a BC destination is transcoded again. Returns false when the
// copy can pass through unchanged.
bool VulkanEtc2Emulation::State::RedirectImageCopy(
    const HostDevice& dev, VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, std::uint32_t count,
    const VkImageCopy* regions) {
  ImageRecord source_record;
  ImageRecord destination_record;
  const bool source_found = LookupImage(dev, source, &source_record);
  const bool destination_found =
      LookupImage(dev, destination, &destination_record);
  const bool source_scaled = source_record.scale > 1;
  const bool destination_scaled = destination_record.scale > 1;
  const bool decompress = source_record.transcoded && destination_found &&
                          !destination_record.transcoded;
  if (regions == nullptr || (!source_found && !destination_found)) {
    return false;
  }
  if (destination_record.transcoded) {
    if (source_record.transcoded &&
        source_record.scale == destination_record.scale) {
      return source_scaled &&
             CopyScaledBlocks(dev, command_buffer, source, source_layout,
                              source_record, destination, destination_layout,
                              destination_record, count, regions);
    }
    if (!destination_scaled && source_found &&
        (source_scaled || !source_record.transcoded)) {
      TranscodeCopy(dev, command_buffer, source, source_record, destination,
                    destination_record, destination_layout, count, regions);
    } else if (destination_scaled && source_found && !source_scaled) {
      ScaledTranscodeCopy(dev, command_buffer, source, source_record,
                          destination, destination_record, destination_layout,
                          count, regions);
    } else {
      LogFailure("skipped a copy into a compressed image");
    }
    return true;
  }
  if (!source_scaled && !destination_scaled && !decompress) {
    return false;
  }
  if (dev.blit_image == nullptr) {
    LogFailure("skipped an image copy that needs a blit");
    return true;
  }
  // A blit must stay inside the mip, where a compressed copy may run to the
  // end of the edge block.
  const auto clamp = [decompress](const ImageRecord& record, std::uint32_t mip,
                                  VkOffset3D* end) {
    if (decompress && record.width != 0 && record.scale == 1) {
      end->x = std::min<std::int32_t>(
          end->x, std::max<std::uint32_t>(1, record.width >> mip));
      end->y = std::min<std::int32_t>(
          end->y, std::max<std::uint32_t>(1, record.height >> mip));
    }
  };
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
    clamp(source_record, region.srcSubresource.mipLevel, &blit.srcOffsets[1]);
    clamp(destination_record, region.dstSubresource.mipLevel,
          &blit.dstOffsets[1]);
    blits.push_back(blit);
  }
  const bool same_scale = source_record.scale == destination_record.scale;
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
  if (dev->emulated &&
      state_->RedirectImageCopy(*dev, command_buffer, source, source_layout,
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
  if (dev->emulated && info->pRegions != nullptr) {
    std::vector<VkImageCopy> regions;
    regions.reserve(info->regionCount);
    for (std::uint32_t index = 0; index < info->regionCount; ++index) {
      const VkImageCopy2& region = info->pRegions[index];
      regions.push_back({region.srcSubresource, region.srcOffset,
                         region.dstSubresource, region.dstOffset,
                         region.extent});
    }
    if (state_->RedirectImageCopy(*dev, command_buffer, info->srcImage,
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

void VulkanEtc2Emulation::RegisterQueue(VkDevice device, VkQueue queue,
                                        std::uint32_t queue_family) {
  if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->queue_families[queue] = {device, queue_family};
}

bool VulkanEtc2Emulation::EnableGpuDecode(VkDevice device, VkQueue queue,
                                          std::uint32_t queue_family) {
  HostDevice* dev = state_->FindMutable(device);
  if (dev == nullptr || !dev->emulated || queue == VK_NULL_HANDLE) {
    return false;
  }
  if (GpuDecodeDisabledByEnvironment()) {
    std::fprintf(stderr,
                 "  [vulkan] ETC2 GPU decode off (MOCKTAIL_ETC2_GPU_DECODE=0)\n");
    return false;
  }
  if (dev->copy_buffer == nullptr || dev->create_command_pool == nullptr ||
      dev->destroy_command_pool == nullptr ||
      dev->allocate_command_buffers == nullptr ||
      dev->free_command_buffers == nullptr ||
      dev->begin_command_buffer == nullptr ||
      dev->end_command_buffer == nullptr || dev->pipeline_barrier == nullptr) {
    return false;
  }
  std::string error;
  std::unique_ptr<Etc2GpuDecoder> decoder =
      Etc2GpuDecoder::Create(device, dev->get_device_proc_addr, &error);
  if (decoder == nullptr ||
      !decoder->SelfTest(queue, queue_family, dev->memory, &error)) {
    std::fprintf(stderr,
                 "  [vulkan] ETC2 GPU decode unavailable (%s); decoding on "
                 "the CPU\n",
                 error.c_str());
    return false;
  }
  std::fprintf(stderr, "  [vulkan] ETC2 GPU decode enabled\n");
  if (dev->bc_textures) {
    std::fprintf(stderr,
                 decoder->CanTranscode()
                     ? "  [vulkan] ETC2/EAC textures stay compressed as "
                       "BC7/BC4/BC5\n"
                     : "  [vulkan] BC transcode unavailable or failed its "
                       "self-test; "
                       "textures stay decoded\n");
  }
  const bool adaptive = state_->upscale == kAdaptiveSmallTextureUpscale;
  // Measured before the decoder is published, so CreateImage never sees it
  // without the speed and factor that go with it.
  // Without BC, small textures are only decoded and scaled, so that is what
  // the probe times.
  const bool encode = dev->bc_textures && decoder->CanTranscode();
  if (decoder->CanScale() && (adaptive || state_->encode_budget)) {
    Etc2GpuSpeed speed;
    if (decoder->MeasureSpeed(queue, queue_family, dev->memory, encode,
                              &speed, &error)) {
      dev->speed = speed;
      dev->adaptive_upscale = AdaptiveSmallTextureUpscale(speed.upscale_ns);
      std::fprintf(stderr,
                   "  [vulkan] BC encode speed: transcode %.1f ns/block, "
                   "upscale %.1f ns/block%s; small texture upscale %s%ux; "
                   "encode budget %s\n",
                   speed.transcode_ns, speed.upscale_ns,
                   encode ? "" : " (decode and scale only)",
                   adaptive ? "adaptive up to " : "",
                   adaptive ? dev->adaptive_upscale : state_->upscale,
                   state_->encode_budget && encode ? "on" : "off");
    } else {
      std::fprintf(stderr,
                   "  [vulkan] BC encode speed probe failed (%s); encodes "
                   "are not budgeted and adaptive upscale stays at 1x\n",
                   error.c_str());
    }
  }
  dev->gpu = std::move(decoder);
  return true;
}

Etc2SubmitWait VulkanEtc2Emulation::PrepareSubmit(
    const VkCommandBuffer* command_buffers, std::uint32_t count,
    bool allow_async, VkQueue queue) {
  Etc2SubmitWait result;
  if (command_buffers == nullptr || count == 0 ||
      !state_->has_commands.load(std::memory_order_acquire)) {
    return result;
  }
  result.decode_commands =
      state_->GpuDecodeCommands(command_buffers, count, queue);
  struct Work {
    PendingUpload upload;
    std::uint8_t* source = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memory_offset = 0;
    VkDeviceMemory source_memory = VK_NULL_HANDLE;
  };
  std::vector<Work> work;
  std::vector<BlockCopy> block_copies;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto collect = [this, &work, &block_copies](VkCommandBuffer command_buffer) {
      const auto record = state_->commands.find(command_buffer);
      if (record == state_->commands.end()) {
        return;
      }
      block_copies.insert(block_copies.end(),
                          record->second.block_copies.begin(),
                          record->second.block_copies.end());
      for (const PendingUpload& upload : record->second.uploads) {
        const auto binding = state_->buffers.find(upload.source);
        if (binding == state_->buffers.end()) {
          LogFailure("upload source buffer has no tracked memory binding");
          continue;
        }
        Work item;
        item.upload = upload;
        item.source_memory = binding->second.memory;
        const VkDeviceSize absolute =
            binding->second.offset + upload.source_offset;
        const auto mapping = state_->mappings.find(binding->second.memory);
        const auto readable =
            state_->host_readable.find(binding->second.memory);
        if (mapping == state_->mappings.end() &&
            readable != state_->host_readable.end() && !readable->second) {
          LogFailure(upload.keep_only
                         ? "kept no blocks of an upload the host cannot read"
                         : "upload source is in memory the host cannot read");
          continue;
        }
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
        for (VkCommandBuffer secondary : record->second.secondaries) {
          collect(secondary);
        }
      }
    }
  }
  if (work.empty()) {
    if (!block_copies.empty()) {
      state_->WaitForTicket(state_->IssuedTicket());
      state_->FillBlockCopies(block_copies);
    }
    return result;
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
  // Kept blocks feed only later copies, which run after them in ticket
  // order, so a batch that only keeps blocks writes nothing the GPU reads.
  // Its bytes are gathered now, so the GPU need not wait for it.
  const bool gpu_waits =
      !block_copies.empty() ||
      std::any_of(work.begin(), work.end(),
                  [](const Work& item) { return !item.upload.keep_only; });

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
  const bool defer_gather = async && temporary.empty() && gpu_waits;

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
    state_->WaitForTicket(state_->IssuedTicket());
    state_->DecodeAndEmit(uploads, state_->scratch_source.data(),
                          state_->scratch_decoded.data(), block_copies);
    return result;
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
  batch->block_copies = std::move(block_copies);
  // The staging these uploads write into must outlive the decode, so the
  // command buffers that own it remember the ticket. The batch is queued
  // while the state lock is held, so a concurrent ReleaseCommandBuffer cannot
  // read a stale ticket and recycle staging the dispatcher is still writing.
  // Tickets only ever go up, so a record keeps the highest one it has seen.
  std::uint64_t ticket = 0;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ticket = state_->Enqueue(std::move(batch));
    if (!gpu_waits) {
      return result;
    }
    if (defer_gather) {
      for (const Work& item : work) {
        state_->deferred_reads[item.source_memory] = ticket;
      }
    }
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
  result.semaphore = batch_dev->timeline;
  result.value = ticket;
  return result;
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
  std::vector<DecodeCommands> decode_commands;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto record = state_->commands.find(command_buffer);
    if (record == state_->commands.end()) {
      return;
    }
    doomed = std::move(record->second.staging);
    decode_commands = std::move(record->second.decode_commands);
    state_->commands.erase(record);
    state_->has_commands.store(!state_->commands.empty(),
                               std::memory_order_release);
  }
  state_->FreeDecodeCommands(decode_commands);
  state_->ReleaseStaging(std::move(doomed));
}

}  // namespace mocktail::graphics
