#include "mocktail/graphics/etc2_gpu_decoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

#include "generated/etc2_decode_spv.h"
#include "generated/etc2_scale_spv.h"
#include "generated/etc2_transcode_spv.h"
#include "mocktail/graphics/bc_decoder.h"

namespace mocktail::graphics {

struct Etc2GpuDecoder::Procs {
  PFN_vkCreateShaderModule create_shader_module = nullptr;
  PFN_vkDestroyShaderModule destroy_shader_module = nullptr;
  PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout = nullptr;
  PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout = nullptr;
  PFN_vkCreatePipelineLayout create_pipeline_layout = nullptr;
  PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
  PFN_vkCreateComputePipelines create_compute_pipelines = nullptr;
  PFN_vkDestroyPipeline destroy_pipeline = nullptr;
  PFN_vkCreateDescriptorPool create_descriptor_pool = nullptr;
  PFN_vkDestroyDescriptorPool destroy_descriptor_pool = nullptr;
  PFN_vkAllocateDescriptorSets allocate_descriptor_sets = nullptr;
  PFN_vkUpdateDescriptorSets update_descriptor_sets = nullptr;
  PFN_vkCmdBindPipeline cmd_bind_pipeline = nullptr;
  PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets = nullptr;
  PFN_vkCmdPushConstants cmd_push_constants = nullptr;
  PFN_vkCmdDispatch cmd_dispatch = nullptr;
  PFN_vkCreateBuffer create_buffer = nullptr;
  PFN_vkDestroyBuffer destroy_buffer = nullptr;
  PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
  PFN_vkAllocateMemory allocate_memory = nullptr;
  PFN_vkFreeMemory free_memory = nullptr;
  PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
  PFN_vkMapMemory map_memory = nullptr;
  PFN_vkCreateCommandPool create_command_pool = nullptr;
  PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
  PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
  PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
  PFN_vkEndCommandBuffer end_command_buffer = nullptr;
  PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = nullptr;
  PFN_vkCmdCopyBuffer cmd_copy_buffer = nullptr;
  PFN_vkQueueSubmit queue_submit = nullptr;
  PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
};

namespace {

struct PushConstants {
  std::uint32_t format;
  std::uint32_t blocks_wide;
  std::uint32_t blocks_high;
  std::uint32_t source_offset;
  std::uint32_t source_row_blocks;
  std::uint32_t source_layer_blocks;
  std::uint32_t destination_offset;
  std::uint32_t destination_layer_words;
};

constexpr std::uint32_t kGroupBlocks = 8;
// Broken BC packing decodes to noise, far below this; real encodes of random
// blocks sit well above it.
constexpr double kMinTranscodePsnr = 20.0;

// Compares a tight, single-layer transcode job with the CPU decode of the
// same blocks.
bool TranscodeCloseToCpu(const Etc2GpuJob& job, const std::uint8_t* source,
                         const std::uint8_t* output) {
  const BcFormat bc = BcFormatFor(job.format);
  const std::size_t texel_bytes = EtcDecodedTexelBytes(job.format);
  const bool color = texel_bytes == 4 && bc == BcFormat::kBc7;
  const bool is_signed =
      bc == BcFormat::kBc4Signed || bc == BcFormat::kBc5Signed;
  std::uint8_t want[64];
  std::uint8_t got[64];
  double squared = 0;
  double samples = 0;
  const std::size_t blocks =
      std::size_t{job.blocks_wide} * job.blocks_high * job.layers;
  for (std::size_t b = 0; b < blocks; ++b) {
    const std::size_t column = b % job.blocks_wide;
    const std::size_t row = b / job.blocks_wide % job.blocks_high;
    const std::size_t layer = b / job.blocks_wide / job.blocks_high;
    const std::size_t read = layer * job.source_layer_blocks +
                             row * job.source_row_blocks + column;
    if (!DecodeEtcImage(job.format, source + read * EtcBlockBytes(job.format),
                        EtcBlockBytes(job.format), 4, 4, want, sizeof(want)) ||
        !DecodeBcBlock(bc, output + b * BcBlockBytes(bc), got)) {
      return false;
    }
    const std::size_t values = color ? 64 : 16 * texel_bytes / 2;
    for (std::size_t v = 0; v < values; ++v) {
      double error = 0;
      if (color) {
        error = (want[v] - got[v]) / 255.0;
      } else {
        const int a = want[v * 2] | (want[v * 2 + 1] << 8);
        const int c = got[v * 2] | (got[v * 2 + 1] << 8);
        error = is_signed ? (static_cast<std::int16_t>(a) -
                             static_cast<std::int16_t>(c)) / 32767.0
                          : (a - c) / 65535.0;
      }
      squared += error * error;
      samples += 1;
    }
  }
  const double mse = squared / samples;
  return mse == 0 || 10 * std::log10(1 / mse) >= kMinTranscodePsnr;
}
constexpr VkDeviceSize kMaxWordOffset = std::numeric_limits<std::uint32_t>::max();

template <typename T>
bool Load(PFN_vkGetDeviceProcAddr get, VkDevice device, const char* name,
          T* out) {
  *out = reinterpret_cast<T>(get(device, name));
  return *out != nullptr;
}

}

std::uint32_t Etc2GpuRowTexels(std::uint32_t blocks_wide) {
  return blocks_wide * 4;
}

VkDeviceSize Etc2GpuLayerBytes(EtcFormat format, std::uint32_t blocks_wide,
                               std::uint32_t blocks_high) {
  return VkDeviceSize{Etc2GpuRowTexels(blocks_wide)} * blocks_high * 4 *
         EtcDecodedTexelBytes(format);
}

namespace {

VkDeviceSize OutputLayerBytes(const Etc2GpuJob& job) {
  if (job.scale) {
    return VkDeviceSize{job.blocks_wide} * job.blocks_high * 16 * 4;
  }
  return job.transcode
             ? VkDeviceSize{job.blocks_wide} * job.blocks_high *
                   BcBlockBytes(BcFormatFor(job.format))
             : Etc2GpuLayerBytes(job.format, job.blocks_wide, job.blocks_high);
}

}

VkDeviceSize Etc2GpuOutputBytes(const Etc2GpuJob& job) {
  return OutputLayerBytes(job) * job.layers;
}

std::unique_ptr<Etc2GpuDecoder> Etc2GpuDecoder::Create(
    VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr,
    std::string* error) {
  std::unique_ptr<Etc2GpuDecoder> decoder(new Etc2GpuDecoder());
  decoder->device_ = device;
  decoder->procs_ = std::make_unique<Procs>();
  Procs& p = *decoder->procs_;
  const PFN_vkGetDeviceProcAddr get = get_device_proc_addr;
  if (get == nullptr ||
      !Load(get, device, "vkCreateShaderModule", &p.create_shader_module) ||
      !Load(get, device, "vkDestroyShaderModule", &p.destroy_shader_module) ||
      !Load(get, device, "vkCreateDescriptorSetLayout",
            &p.create_descriptor_set_layout) ||
      !Load(get, device, "vkDestroyDescriptorSetLayout",
            &p.destroy_descriptor_set_layout) ||
      !Load(get, device, "vkCreatePipelineLayout", &p.create_pipeline_layout) ||
      !Load(get, device, "vkDestroyPipelineLayout",
            &p.destroy_pipeline_layout) ||
      !Load(get, device, "vkCreateComputePipelines",
            &p.create_compute_pipelines) ||
      !Load(get, device, "vkDestroyPipeline", &p.destroy_pipeline) ||
      !Load(get, device, "vkCreateDescriptorPool", &p.create_descriptor_pool) ||
      !Load(get, device, "vkDestroyDescriptorPool",
            &p.destroy_descriptor_pool) ||
      !Load(get, device, "vkAllocateDescriptorSets",
            &p.allocate_descriptor_sets) ||
      !Load(get, device, "vkUpdateDescriptorSets", &p.update_descriptor_sets) ||
      !Load(get, device, "vkCmdBindPipeline", &p.cmd_bind_pipeline) ||
      !Load(get, device, "vkCmdBindDescriptorSets",
            &p.cmd_bind_descriptor_sets) ||
      !Load(get, device, "vkCmdPushConstants", &p.cmd_push_constants) ||
      !Load(get, device, "vkCmdDispatch", &p.cmd_dispatch) ||
      !Load(get, device, "vkCreateBuffer", &p.create_buffer) ||
      !Load(get, device, "vkDestroyBuffer", &p.destroy_buffer) ||
      !Load(get, device, "vkGetBufferMemoryRequirements",
            &p.get_buffer_memory_requirements) ||
      !Load(get, device, "vkAllocateMemory", &p.allocate_memory) ||
      !Load(get, device, "vkFreeMemory", &p.free_memory) ||
      !Load(get, device, "vkBindBufferMemory", &p.bind_buffer_memory) ||
      !Load(get, device, "vkMapMemory", &p.map_memory) ||
      !Load(get, device, "vkCreateCommandPool", &p.create_command_pool) ||
      !Load(get, device, "vkDestroyCommandPool", &p.destroy_command_pool) ||
      !Load(get, device, "vkAllocateCommandBuffers",
            &p.allocate_command_buffers) ||
      !Load(get, device, "vkBeginCommandBuffer", &p.begin_command_buffer) ||
      !Load(get, device, "vkEndCommandBuffer", &p.end_command_buffer) ||
      !Load(get, device, "vkCmdPipelineBarrier", &p.cmd_pipeline_barrier) ||
      !Load(get, device, "vkCmdCopyBuffer", &p.cmd_copy_buffer) ||
      !Load(get, device, "vkQueueSubmit", &p.queue_submit) ||
      !Load(get, device, "vkQueueWaitIdle", &p.queue_wait_idle)) {
    *error = "missing Vulkan compute entry points";
    return nullptr;
  }

  VkShaderModuleCreateInfo shader{};
  shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader.codeSize = sizeof(kEtc2DecodeSpv);
  shader.pCode = kEtc2DecodeSpv;
  if (p.create_shader_module(device, &shader, nullptr, &decoder->shader_) !=
      VK_SUCCESS) {
    *error = "vkCreateShaderModule failed";
    return nullptr;
  }

  VkDescriptorSetLayoutBinding bindings[2] = {};
  for (std::uint32_t i = 0; i < 2; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_layout{};
  set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout.bindingCount = 2;
  set_layout.pBindings = bindings;
  if (p.create_descriptor_set_layout(device, &set_layout, nullptr,
                                     &decoder->set_layout_) != VK_SUCCESS) {
    *error = "vkCreateDescriptorSetLayout failed";
    return nullptr;
  }

  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(PushConstants)};
  VkPipelineLayoutCreateInfo pipeline_layout{};
  pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout.setLayoutCount = 1;
  pipeline_layout.pSetLayouts = &decoder->set_layout_;
  pipeline_layout.pushConstantRangeCount = 1;
  pipeline_layout.pPushConstantRanges = &push;
  if (p.create_pipeline_layout(device, &pipeline_layout, nullptr,
                               &decoder->pipeline_layout_) != VK_SUCCESS) {
    *error = "vkCreatePipelineLayout failed";
    return nullptr;
  }

  VkComputePipelineCreateInfo pipeline{};
  pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline.stage.module = decoder->shader_;
  pipeline.stage.pName = "main";
  pipeline.layout = decoder->pipeline_layout_;
  if (p.create_compute_pipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr,
                                 &decoder->pipeline_) != VK_SUCCESS) {
    *error = "vkCreateComputePipelines failed";
    return nullptr;
  }

  shader.codeSize = sizeof(kEtc2TranscodeSpv);
  shader.pCode = kEtc2TranscodeSpv;
  if (p.create_shader_module(device, &shader, nullptr,
                             &decoder->transcode_shader_) == VK_SUCCESS) {
    pipeline.stage.module = decoder->transcode_shader_;
    if (p.create_compute_pipelines(device, VK_NULL_HANDLE, 1, &pipeline,
                                   nullptr, &decoder->transcode_pipeline_) !=
        VK_SUCCESS) {
      decoder->transcode_pipeline_ = VK_NULL_HANDLE;
    }
  }
  shader.codeSize = sizeof(kEtc2ScaleSpv);
  shader.pCode = kEtc2ScaleSpv;
  if (p.create_shader_module(device, &shader, nullptr,
                             &decoder->scale_shader_) == VK_SUCCESS) {
    pipeline.stage.module = decoder->scale_shader_;
    if (p.create_compute_pipelines(device, VK_NULL_HANDLE, 1, &pipeline,
                                   nullptr, &decoder->scale_pipeline_) !=
        VK_SUCCESS) {
      decoder->scale_pipeline_ = VK_NULL_HANDLE;
    }
  }
  return decoder;
}

Etc2GpuDecoder::~Etc2GpuDecoder() {
  if (procs_ == nullptr) {
    return;
  }
  DisableTranscode();
  if (transcode_shader_ != VK_NULL_HANDLE) {
    procs_->destroy_shader_module(device_, transcode_shader_, nullptr);
  }
  if (scale_pipeline_ != VK_NULL_HANDLE) {
    procs_->destroy_pipeline(device_, scale_pipeline_, nullptr);
  }
  if (scale_shader_ != VK_NULL_HANDLE) {
    procs_->destroy_shader_module(device_, scale_shader_, nullptr);
  }
  if (pipeline_ != VK_NULL_HANDLE) {
    procs_->destroy_pipeline(device_, pipeline_, nullptr);
  }
  if (pipeline_layout_ != VK_NULL_HANDLE) {
    procs_->destroy_pipeline_layout(device_, pipeline_layout_, nullptr);
  }
  if (set_layout_ != VK_NULL_HANDLE) {
    procs_->destroy_descriptor_set_layout(device_, set_layout_, nullptr);
  }
  if (shader_ != VK_NULL_HANDLE) {
    procs_->destroy_shader_module(device_, shader_, nullptr);
  }
}

bool Etc2GpuDecoder::CreateBinding(VkBuffer source, VkBuffer destination,
                                   Etc2GpuBinding* binding) {
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
  VkDescriptorPoolCreateInfo pool{};
  pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool.maxSets = 1;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
  Etc2GpuBinding out;
  if (procs_->create_descriptor_pool(device_, &pool, nullptr, &out.pool) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = out.pool;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &set_layout_;
  if (procs_->allocate_descriptor_sets(device_, &allocate, &out.set) !=
      VK_SUCCESS) {
    procs_->destroy_descriptor_pool(device_, out.pool, nullptr);
    return false;
  }
  VkDescriptorBufferInfo buffers[2] = {{source, 0, VK_WHOLE_SIZE},
                                       {destination, 0, VK_WHOLE_SIZE}};
  VkWriteDescriptorSet writes[2] = {};
  for (std::uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = out.set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buffers[i];
  }
  procs_->update_descriptor_sets(device_, 2, writes, 0, nullptr);
  *binding = out;
  return true;
}

void Etc2GpuDecoder::DestroyBinding(const Etc2GpuBinding& binding) {
  if (binding.pool != VK_NULL_HANDLE) {
    procs_->destroy_descriptor_pool(device_, binding.pool, nullptr);
  }
}

bool Etc2GpuDecoder::Record(VkCommandBuffer command_buffer,
                            const Etc2GpuBinding& binding,
                            const Etc2GpuJob* jobs, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    const Etc2GpuJob& job = jobs[i];
    const VkDeviceSize layer_bytes = OutputLayerBytes(job);
    if (job.scale) {
      if (scale_pipeline_ == VK_NULL_HANDLE || job.transcode ||
          job.rgba_source || job.layers != 1 || job.scale_width == 0 ||
          job.scale_height == 0 || job.blocks_wide != (job.scale_width + 3) / 4 ||
          job.blocks_high != (job.scale_height + 3) / 4 ||
          job.source_offset % 4 != 0 || job.destination_offset % 4 != 0 ||
          job.scale_texels_offset % 4 != 0 ||
          job.source_offset / 4 > kMaxWordOffset ||
          job.scale_texels_offset / 4 > kMaxWordOffset ||
          job.destination_offset / 4 > kMaxWordOffset ||
          layer_bytes / 4 > kMaxWordOffset) {
        return false;
      }
      continue;
    }
    if ((job.transcode && transcode_pipeline_ == VK_NULL_HANDLE) ||
        (job.rgba_source &&
         (!job.transcode || BcFormatFor(job.format) != BcFormat::kBc7)) ||
        job.blocks_wide == 0 || job.blocks_high == 0 || job.layers == 0 ||
        job.source_offset % 4 != 0 || job.destination_offset % 4 != 0 ||
        job.source_row_blocks < job.blocks_wide ||
        job.source_layer_blocks <
            VkDeviceSize{job.source_row_blocks} * (job.blocks_high - 1) +
                job.blocks_wide ||
        job.source_offset / 4 > kMaxWordOffset ||
        job.destination_offset / 4 > kMaxWordOffset ||
        layer_bytes / 4 > kMaxWordOffset) {
      return false;
    }
  }
  VkPipeline bound = VK_NULL_HANDLE;
  for (std::size_t i = 0; i < count; ++i) {
    const Etc2GpuJob& job = jobs[i];
    const VkPipeline wanted = job.scale       ? scale_pipeline_
                              : job.transcode ? transcode_pipeline_
                                              : pipeline_;
    if (wanted != bound) {
      procs_->cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                wanted);
      if (bound == VK_NULL_HANDLE) {
        procs_->cmd_bind_descriptor_sets(command_buffer,
                                         VK_PIPELINE_BIND_POINT_COMPUTE,
                                         pipeline_layout_, 0, 1, &binding.set,
                                         0, nullptr);
      }
      bound = wanted;
    }
    // The transcode shader's format code for RGBA8 texels.
    constexpr std::uint32_t kRgbaTexels = 7;
    // The scale shader reads the target size where the others read the
    // source strides, and runs one invocation per texel.
    const PushConstants push{
        job.rgba_source ? kRgbaTexels : static_cast<std::uint32_t>(job.format),
        job.blocks_wide,
        job.blocks_high,
        static_cast<std::uint32_t>(job.source_offset / 4),
        job.scale ? job.scale_width : job.source_row_blocks,
        job.scale ? job.scale_height : job.source_layer_blocks,
        static_cast<std::uint32_t>(job.destination_offset / 4),
        static_cast<std::uint32_t>(
            !job.scale ? OutputLayerBytes(job) / 4
            : job.scale_texels_offset != 0
                ? job.scale_texels_offset / 4
                : (job.source_offset + kEtc2ScaleHeaderBytes) / 4)};
    procs_->cmd_push_constants(command_buffer, pipeline_layout_,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
    const std::uint32_t group = job.scale ? kGroupBlocks / 4 : kGroupBlocks;
    procs_->cmd_dispatch(command_buffer,
                         (job.blocks_wide + group - 1) / group,
                         (job.blocks_high + group - 1) / group, job.layers);
  }
  return true;
}

bool Etc2GpuDecoder::SelfTest(VkQueue queue, std::uint32_t queue_family,
                              const VkPhysicalDeviceMemoryProperties& memory,
                              std::string* error) {
  constexpr std::uint32_t kBlocks = 32;
  constexpr EtcFormat kFormats[] = {
      EtcFormat::kEtc2Rgb8,    EtcFormat::kEtc2Rgb8A1,     EtcFormat::kEtc2Rgba8,
      EtcFormat::kEacR11,      EtcFormat::kEacR11Signed,   EtcFormat::kEacRg11,
      EtcFormat::kEacRg11Signed};
  std::vector<Etc2GpuJob> jobs;
  VkDeviceSize source_bytes = 0;
  for (EtcFormat format : kFormats) {
    Etc2GpuJob job;
    job.format = format;
    job.blocks_wide = kBlocks;
    job.blocks_high = kBlocks;
    job.source_offset = source_bytes;
    job.source_row_blocks = kBlocks;
    job.source_layer_blocks = kBlocks * kBlocks;
    source_bytes += VkDeviceSize{kBlocks} * kBlocks * EtcBlockBytes(format);
    jobs.push_back(job);
  }
  for (EtcFormat format : {EtcFormat::kEtc2Rgba8, EtcFormat::kEacR11}) {
    Etc2GpuJob job;
    job.format = format;
    job.blocks_wide = 13;
    job.blocks_high = 9;
    job.layers = 3;
    job.source_offset = source_bytes;
    job.source_row_blocks = 16;
    job.source_layer_blocks = 16 * 9 + 7;
    source_bytes += VkDeviceSize{job.source_layer_blocks} * job.layers *
                    EtcBlockBytes(format);
    jobs.push_back(job);
  }
  if (transcode_pipeline_ != VK_NULL_HANDLE) {
    const std::size_t decoded_jobs = jobs.size();
    for (std::size_t i = 0; i < decoded_jobs; ++i) {
      Etc2GpuJob job = jobs[i];
      job.transcode = true;
      jobs.push_back(job);
    }
  }
  VkDeviceSize total = source_bytes;
  for (Etc2GpuJob& job : jobs) {
    job.destination_offset = total;
    total += Etc2GpuOutputBytes(job);
  }
  std::vector<std::uint8_t> source(source_bytes);
  std::uint64_t state = 0x9E3779B97F4A7C15ULL;
  for (std::uint8_t& byte : source) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    byte = static_cast<std::uint8_t>(state >> 24);
  }

  const Procs& p = *procs_;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  Etc2GpuBinding binding;
  const auto finish = [&](bool ok, const char* message) {
    if (pool != VK_NULL_HANDLE) {
      p.destroy_command_pool(device_, pool, nullptr);
    }
    DestroyBinding(binding);
    if (buffer != VK_NULL_HANDLE) {
      p.destroy_buffer(device_, buffer, nullptr);
    }
    if (buffer_memory != VK_NULL_HANDLE) {
      p.free_memory(device_, buffer_memory, nullptr);
    }
    if (!ok) {
      *error = message;
    }
    return ok;
  };

  constexpr VkMemoryPropertyFlags kHost =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  void* mapped = nullptr;
  if (!CreateBuffer(total, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory, kHost,
                    kHost, &buffer, &buffer_memory) ||
      p.map_memory(device_, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
          VK_SUCCESS) {
    return finish(false, "self-test memory setup failed");
  }
  auto* bytes = static_cast<std::uint8_t*>(mapped);
  std::memcpy(bytes, source.data(), source.size());
  std::memset(bytes + source_bytes, 0, total - source_bytes);

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = queue_family;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo command_info{};
  command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 1;
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (p.create_command_pool(device_, &pool_info, nullptr, &pool) !=
      VK_SUCCESS) {
    return finish(false, "self-test command pool creation failed");
  }
  command_info.commandPool = pool;
  if (p.allocate_command_buffers(device_, &command_info, &command_buffer) !=
          VK_SUCCESS ||
      !CreateBinding(buffer, buffer, &binding) ||
      p.begin_command_buffer(command_buffer, &begin) != VK_SUCCESS ||
      !Record(command_buffer, binding, jobs.data(), jobs.size())) {
    return finish(false, "self-test recording failed");
  }
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  p.cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                         0, nullptr);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  if (p.end_command_buffer(command_buffer) != VK_SUCCESS ||
      p.queue_submit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
      p.queue_wait_idle(queue) != VK_SUCCESS) {
    return finish(false, "self-test submit failed");
  }

  std::vector<std::uint8_t> tight;
  std::vector<std::uint8_t> expected;
  bool transcode_ok = true;
  for (const Etc2GpuJob& job : jobs) {
    if (job.transcode) {
      transcode_ok = transcode_ok &&
                     TranscodeCloseToCpu(job, source.data() + job.source_offset,
                                         bytes + job.destination_offset);
      continue;
    }
    const std::size_t block_bytes = EtcBlockBytes(job.format);
    const std::size_t row_bytes = std::size_t{job.blocks_wide} * block_bytes;
    const VkDeviceSize layer_bytes =
        Etc2GpuLayerBytes(job.format, job.blocks_wide, job.blocks_high);
    tight.resize(row_bytes * job.blocks_high);
    expected.resize(layer_bytes);
    for (std::uint32_t layer = 0; layer < job.layers; ++layer) {
      for (std::uint32_t row = 0; row < job.blocks_high; ++row) {
        std::memcpy(tight.data() + row * row_bytes,
                    source.data() + job.source_offset +
                        (std::size_t{layer} * job.source_layer_blocks +
                         std::size_t{row} * job.source_row_blocks) *
                            block_bytes,
                    row_bytes);
      }
      if (!DecodeEtcImage(job.format, tight.data(), tight.size(),
                          job.blocks_wide * 4, job.blocks_high * 4,
                          expected.data(), expected.size()) ||
          std::memcmp(bytes + job.destination_offset + layer * layer_bytes,
                      expected.data(), layer_bytes) != 0) {
        return finish(false, "self-test output differs from the CPU decoder");
      }
    }
  }
  if (!transcode_ok) {
    DisableTranscode();
  }
  return finish(true, nullptr);
}

namespace {

// Speed probe: 256x256 textures transcoded, and 64x64 textures, the largest
// small texture, upscaled 4x. Each workload runs a few times and the fastest
// run counts, so first-use costs drop out.
constexpr std::uint32_t kProbeTranscodes = 4;
constexpr std::uint32_t kProbeTranscodeBlocks = 64;
constexpr std::uint32_t kProbeUpscales = 8;
constexpr std::uint32_t kProbeSmallBlocks = 16;
constexpr std::uint32_t kProbeScale = 4;
constexpr int kProbeRuns = 3;
constexpr int kProbeEmptyRuns = 8;

VkDeviceSize AlignProbe(VkDeviceSize bytes) { return (bytes + 255) / 256 * 256; }

}

bool Etc2GpuDecoder::MeasureSpeed(VkQueue queue, std::uint32_t queue_family,
                                  const VkPhysicalDeviceMemoryProperties& memory,
                                  bool encode, Etc2GpuSpeed* speed,
                                  std::string* error) {
  if ((encode && transcode_pipeline_ == VK_NULL_HANDLE) ||
      scale_pipeline_ == VK_NULL_HANDLE) {
    *error = "transcode or scale pipeline unavailable";
    return false;
  }
  constexpr EtcFormat kFormat = EtcFormat::kEtc2Rgba8;
  const VkDeviceSize block_bytes = EtcBlockBytes(kFormat);
  VkDeviceSize total = 0;
  std::vector<Etc2GpuJob> transcodes;
  for (std::uint32_t i = 0; encode && i < kProbeTranscodes; ++i) {
    Etc2GpuJob job;
    job.format = kFormat;
    job.transcode = true;
    job.blocks_wide = kProbeTranscodeBlocks;
    job.blocks_high = kProbeTranscodeBlocks;
    job.source_row_blocks = kProbeTranscodeBlocks;
    job.source_layer_blocks = kProbeTranscodeBlocks * kProbeTranscodeBlocks;
    job.source_offset = total;
    total += AlignProbe(job.source_layer_blocks * block_bytes);
    job.destination_offset = total;
    total += AlignProbe(Etc2GpuOutputBytes(job));
    transcodes.push_back(job);
  }
  // Laid out as the emulation stages them: a scale header and the blocks,
  // then the decoded, scaled and encoded results.
  constexpr std::uint32_t kTarget = kProbeSmallBlocks * 4 * kProbeScale;
  std::vector<VkDeviceSize> headers;
  std::vector<Etc2GpuJob> decodes;
  std::vector<Etc2GpuJob> scales;
  std::vector<Etc2GpuJob> encodes;
  for (std::uint32_t i = 0; i < kProbeUpscales; ++i) {
    headers.push_back(total);
    Etc2GpuJob decode;
    decode.format = kFormat;
    decode.blocks_wide = kProbeSmallBlocks;
    decode.blocks_high = kProbeSmallBlocks;
    decode.source_row_blocks = kProbeSmallBlocks;
    decode.source_layer_blocks = kProbeSmallBlocks * kProbeSmallBlocks;
    decode.source_offset = total + kEtc2ScaleHeaderBytes;
    total += AlignProbe(kEtc2ScaleHeaderBytes +
                        decode.source_layer_blocks * block_bytes);
    decode.destination_offset = total;
    total += AlignProbe(Etc2GpuOutputBytes(decode));
    Etc2GpuJob scale;
    scale.format = kFormat;
    scale.scale = true;
    scale.scale_width = kTarget;
    scale.scale_height = kTarget;
    scale.blocks_wide = kTarget / 4;
    scale.blocks_high = kTarget / 4;
    scale.source_offset = headers.back();
    scale.scale_texels_offset = decode.destination_offset;
    scale.destination_offset = total;
    total += AlignProbe(Etc2GpuOutputBytes(scale));
    decodes.push_back(decode);
    scales.push_back(scale);
    if (!encode) {
      continue;
    }
    Etc2GpuJob encoding;
    encoding.format = kFormat;
    encoding.rgba_source = true;
    encoding.transcode = true;
    encoding.blocks_wide = kTarget / 4;
    encoding.blocks_high = kTarget / 4;
    encoding.source_row_blocks = kTarget / 4;
    encoding.source_layer_blocks = (kTarget / 4) * (kTarget / 4);
    encoding.source_offset = scale.destination_offset;
    encoding.destination_offset = total;
    total += AlignProbe(Etc2GpuOutputBytes(encoding));
    encodes.push_back(encoding);
  }

  const Procs& p = *procs_;
  VkBuffer upload = VK_NULL_HANDLE;
  VkDeviceMemory upload_memory = VK_NULL_HANDLE;
  VkBuffer work = VK_NULL_HANDLE;
  VkDeviceMemory work_memory = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  Etc2GpuBinding binding;
  const auto finish = [&](bool ok, const char* message) {
    if (pool != VK_NULL_HANDLE) {
      p.destroy_command_pool(device_, pool, nullptr);
    }
    DestroyBinding(binding);
    for (VkBuffer buffer : {upload, work}) {
      if (buffer != VK_NULL_HANDLE) {
        p.destroy_buffer(device_, buffer, nullptr);
      }
    }
    for (VkDeviceMemory allocation : {upload_memory, work_memory}) {
      if (allocation != VK_NULL_HANDLE) {
        p.free_memory(device_, allocation, nullptr);
      }
    }
    if (!ok) {
      *error = message;
    }
    return ok;
  };
  constexpr VkMemoryPropertyFlags kHost =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  void* mapped = nullptr;
  if (!CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, memory, kHost,
                    kHost, &upload, &upload_memory) ||
      !CreateBuffer(total,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    memory, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &work,
                    &work_memory) ||
      p.map_memory(device_, upload_memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
          VK_SUCCESS) {
    return finish(false, "speed probe memory setup failed");
  }
  auto* bytes = static_cast<std::uint8_t*>(mapped);
  std::uint64_t state = 0x2545F4914F6CDD1DULL;
  for (VkDeviceSize i = 0; i < total; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    bytes[i] = static_cast<std::uint8_t>(state >> 24);
  }
  constexpr std::uint32_t kTexels = kProbeSmallBlocks * 4;
  const std::uint32_t header[4] = {kTexels, kTexels, 0, 0};
  for (VkDeviceSize offset : headers) {
    std::memcpy(bytes + offset, header, sizeof(header));
  }

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = queue_family;
  VkCommandBuffer buffers[4] = {};
  VkCommandBufferAllocateInfo command_info{};
  command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 4;
  if (p.create_command_pool(device_, &pool_info, nullptr, &pool) !=
      VK_SUCCESS) {
    pool = VK_NULL_HANDLE;
    return finish(false, "speed probe command pool creation failed");
  }
  command_info.commandPool = pool;
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  const auto barrier = [&](VkCommandBuffer command_buffer,
                           VkPipelineStageFlags source_stage,
                           VkAccessFlags source_access) {
    VkMemoryBarrier memory_barrier{};
    memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memory_barrier.srcAccessMask = source_access;
    memory_barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    p.cmd_pipeline_barrier(command_buffer, source_stage,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                           &memory_barrier, 0, nullptr, 0, nullptr);
  };
  constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  constexpr VkAccessFlags kWrite = VK_ACCESS_SHADER_WRITE_BIT;
  bool ok = p.allocate_command_buffers(device_, &command_info, buffers) ==
                VK_SUCCESS &&
            CreateBinding(work, work, &binding);
  if (ok) {
    ok = p.begin_command_buffer(buffers[0], &begin) == VK_SUCCESS;
    if (ok) {
      const VkBufferCopy copy{0, 0, total};
      p.cmd_copy_buffer(buffers[0], upload, work, 1, &copy);
      barrier(buffers[0], VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT);
      ok = p.end_command_buffer(buffers[0]) == VK_SUCCESS;
    }
    ok = ok && p.begin_command_buffer(buffers[1], &begin) == VK_SUCCESS &&
         Record(buffers[1], binding, transcodes.data(), transcodes.size()) &&
         p.end_command_buffer(buffers[1]) == VK_SUCCESS;
    ok = ok && p.begin_command_buffer(buffers[2], &begin) == VK_SUCCESS &&
         Record(buffers[2], binding, decodes.data(), decodes.size());
    if (ok) {
      barrier(buffers[2], kCompute, kWrite);
      ok = Record(buffers[2], binding, scales.data(), scales.size());
    }
    if (ok) {
      barrier(buffers[2], kCompute, kWrite);
      ok = Record(buffers[2], binding, encodes.data(), encodes.size()) &&
           p.end_command_buffer(buffers[2]) == VK_SUCCESS;
    }
    ok = ok && p.begin_command_buffer(buffers[3], &begin) == VK_SUCCESS &&
         p.end_command_buffer(buffers[3]) == VK_SUCCESS;
  }
  if (!ok) {
    return finish(false, "speed probe recording failed");
  }
  // Wall time of a submit and wait; the fastest of a few runs.
  const auto time = [&](VkCommandBuffer command_buffer, int runs,
                        double* best_ns) {
    for (int run = 0; run < runs; ++run) {
      VkSubmitInfo submit{};
      submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &command_buffer;
      const auto start = std::chrono::steady_clock::now();
      if (p.queue_submit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
          p.queue_wait_idle(queue) != VK_SUCCESS) {
        return false;
      }
      const double ns = std::chrono::duration<double, std::nano>(
                            std::chrono::steady_clock::now() - start)
                            .count();
      *best_ns = run == 0 ? ns : std::min(*best_ns, ns);
    }
    return true;
  };
  double copy_ns = 0;
  double empty_ns = 0;
  double transcode_ns = 0;
  double upscale_ns = 0;
  if (!time(buffers[0], 1, &copy_ns) ||
      !time(buffers[3], kProbeEmptyRuns, &empty_ns) ||
      (encode && !time(buffers[1], kProbeRuns, &transcode_ns)) ||
      !time(buffers[2], kProbeRuns, &upscale_ns)) {
    return finish(false, "speed probe submit failed");
  }
  // An empty submit's round trip is latency, not work. On a busy machine one
  // can take as long as the work, so at most half a measurement goes.
  const auto work_ns = [empty_ns](double measured) {
    return std::max(measured - empty_ns, measured / 2);
  };
  speed->transcode_ns =
      encode ? work_ns(transcode_ns) / (kProbeTranscodes *
                                        kProbeTranscodeBlocks *
                                        kProbeTranscodeBlocks)
             : 0;
  speed->upscale_ns = work_ns(upscale_ns) /
                      (kProbeUpscales * (kTarget / 4) * (kTarget / 4));
  return finish(true, nullptr);
}

bool Etc2GpuDecoder::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  const VkPhysicalDeviceMemoryProperties& memory,
                                  VkMemoryPropertyFlags want,
                                  VkMemoryPropertyFlags fallback,
                                  VkBuffer* buffer, VkDeviceMemory* allocation) {
  const Procs& p = *procs_;
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = size;
  info.usage = usage;
  if (p.create_buffer(device_, &info, nullptr, buffer) != VK_SUCCESS) {
    *buffer = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements requirements{};
  p.get_buffer_memory_requirements(device_, *buffer, &requirements);
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = UINT32_MAX;
  for (VkMemoryPropertyFlags flags : {want, fallback}) {
    for (std::uint32_t i = 0; i < memory.memoryTypeCount &&
                              allocate.memoryTypeIndex == UINT32_MAX;
         ++i) {
      if ((requirements.memoryTypeBits & (1U << i)) != 0 &&
          (memory.memoryTypes[i].propertyFlags & flags) == flags) {
        allocate.memoryTypeIndex = i;
      }
    }
  }
  if (allocate.memoryTypeIndex == UINT32_MAX ||
      p.allocate_memory(device_, &allocate, nullptr, allocation) !=
          VK_SUCCESS) {
    *allocation = VK_NULL_HANDLE;
    return false;
  }
  return p.bind_buffer_memory(device_, *buffer, *allocation, 0) == VK_SUCCESS;
}

void Etc2GpuDecoder::DisableTranscode() {
  if (transcode_pipeline_ != VK_NULL_HANDLE) {
    procs_->destroy_pipeline(device_, transcode_pipeline_, nullptr);
    transcode_pipeline_ = VK_NULL_HANDLE;
  }
}

}
