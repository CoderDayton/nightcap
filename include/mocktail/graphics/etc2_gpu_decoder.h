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
};

std::uint32_t Etc2GpuRowTexels(std::uint32_t blocks_wide);
VkDeviceSize Etc2GpuLayerBytes(EtcFormat format, std::uint32_t blocks_wide,
                               std::uint32_t blocks_high);
VkDeviceSize Etc2GpuDecodedBytes(const Etc2GpuJob& job);

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

  bool SelfTest(VkQueue queue, std::uint32_t queue_family,
                const VkPhysicalDeviceMemoryProperties& memory,
                std::string* error);

  struct Procs;

 private:
  Etc2GpuDecoder() = default;

  VkDevice device_ = VK_NULL_HANDLE;
  std::unique_ptr<Procs> procs_;
  VkShaderModule shader_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}

#endif
