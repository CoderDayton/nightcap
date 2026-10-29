#ifndef MOCKTAIL_GRAPHICS_ETC2_GPU_DECODER_H_
#define MOCKTAIL_GRAPHICS_ETC2_GPU_DECODER_H_

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "mocktail/graphics/etc2_decoder.h"

namespace mocktail::graphics {

struct Etc2GpuJob {
  EtcFormat format = EtcFormat::kEtc2Rgb8;
  std::uint32_t blocks_wide = 0;
  std::uint32_t blocks_high = 0;
  std::uint32_t layers = 1;
  VkDeviceSize source_offset = 0;
  std::uint32_t source_row_blocks = 0;
  std::uint32_t source_layer_blocks = 0;
  VkDeviceSize destination_offset = 0;
  // Writes tightly packed BC blocks (BcFormatFor) instead of texels.
  bool transcode = false;
  // The source holds RGBA8 texels, a block row being source_row_blocks * 4
  // texels wide, instead of ETC2 blocks. Needs transcode and a colour format.
  bool rgba_source = false;
  // Scales RGBA8 texels to scale_width x scale_height as ScaleRgba does and
  // writes them padded to blocks_wide x blocks_high blocks, repeating the
  // edge texels. The source is a header of kEtc2ScaleHeaderBytes: texel
  // width, height, row pitch in texels (0 for the width) and a zero word. The
  // texels follow it, or start at scale_texels_offset when that is nonzero.
  // The header can be written after recording, up to the size the job allows.
  bool scale = false;
  std::uint32_t scale_width = 0;
  std::uint32_t scale_height = 0;
  VkDeviceSize scale_texels_offset = 0;
};

constexpr VkDeviceSize kEtc2ScaleHeaderBytes = 16;

std::uint32_t Etc2GpuRowTexels(std::uint32_t blocks_wide);
VkDeviceSize Etc2GpuLayerBytes(EtcFormat format, std::uint32_t blocks_wide,
                               std::uint32_t blocks_high);
VkDeviceSize Etc2GpuOutputBytes(const Etc2GpuJob& job);

// GPU time per 4x4 block written, from timed dispatches of the real jobs.
struct Etc2GpuSpeed {
  // ETC2 RGBA8 blocks transcoded to BC7.
  double transcode_ns = 0;
  // A small texture decoded, upscaled 4x and encoded to BC7, per output
  // block.
  double upscale_ns = 0;
};

struct Etc2GpuBinding {
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
};

class Etc2GpuDecoder final {
 public:
  static std::unique_ptr<Etc2GpuDecoder> Create(
      VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr,
      std::string* error);
  ~Etc2GpuDecoder();
  Etc2GpuDecoder(const Etc2GpuDecoder&) = delete;
  Etc2GpuDecoder& operator=(const Etc2GpuDecoder&) = delete;

  bool CreateBinding(VkBuffer source, VkBuffer destination,
                     Etc2GpuBinding* binding);
  void DestroyBinding(const Etc2GpuBinding& binding);

  bool Record(VkCommandBuffer command_buffer, const Etc2GpuBinding& binding,
              const Etc2GpuJob* jobs, std::size_t count);

  // Checks decoding against the CPU decoder and returns false on any
  // difference. A transcode that decodes too far from the CPU result only
  // turns transcoding off.
  bool SelfTest(VkQueue queue, std::uint32_t queue_family,
                const VkPhysicalDeviceMemoryProperties& memory,
                std::string* error);

  // Times transcode and upscale jobs on `queue`. Without `encode` only the
  // decode and scale of an upscale are timed, and transcode_ns stays 0.
  bool MeasureSpeed(VkQueue queue, std::uint32_t queue_family,
                    const VkPhysicalDeviceMemoryProperties& memory,
                    bool encode, Etc2GpuSpeed* speed, std::string* error);

  bool CanTranscode() const { return transcode_pipeline_ != VK_NULL_HANDLE; }
  bool CanScale() const { return scale_pipeline_ != VK_NULL_HANDLE; }

  struct Procs;

 private:
  Etc2GpuDecoder() = default;
  void DisableTranscode();
  // Bound to the first memory type with `want`, else with `fallback`.
  bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    const VkPhysicalDeviceMemoryProperties& memory,
                    VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback,
                    VkBuffer* buffer, VkDeviceMemory* allocation);

  VkDevice device_ = VK_NULL_HANDLE;
  std::unique_ptr<Procs> procs_;
  VkShaderModule shader_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkShaderModule transcode_shader_ = VK_NULL_HANDLE;
  VkPipeline transcode_pipeline_ = VK_NULL_HANDLE;
  VkShaderModule scale_shader_ = VK_NULL_HANDLE;
  VkPipeline scale_pipeline_ = VK_NULL_HANDLE;
};

}

#endif
