#include "mocktail/graphics/etc2_gpu_decoder.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "generated/etc2_decode_spv.h"

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

VkDeviceSize Etc2GpuDecodedBytes(const Etc2GpuJob& job) {
  return Etc2GpuLayerBytes(job.format, job.blocks_wide, job.blocks_high) *
         job.layers;
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
  return decoder;
}

Etc2GpuDecoder::~Etc2GpuDecoder() {
  if (procs_ == nullptr) {
    return;
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
    const VkDeviceSize layer_bytes =
        Etc2GpuLayerBytes(job.format, job.blocks_wide, job.blocks_high);
    if (job.blocks_wide == 0 || job.blocks_high == 0 || job.layers == 0 ||
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
  procs_->cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_);
  procs_->cmd_bind_descriptor_sets(command_buffer,
                                   VK_PIPELINE_BIND_POINT_COMPUTE,
                                   pipeline_layout_, 0, 1, &binding.set, 0,
                                   nullptr);
  for (std::size_t i = 0; i < count; ++i) {
    const Etc2GpuJob& job = jobs[i];
    const PushConstants push{
        static_cast<std::uint32_t>(job.format),
        job.blocks_wide,
        job.blocks_high,
        static_cast<std::uint32_t>(job.source_offset / 4),
        job.source_row_blocks,
        job.source_layer_blocks,
        static_cast<std::uint32_t>(job.destination_offset / 4),
        static_cast<std::uint32_t>(
            Etc2GpuLayerBytes(job.format, job.blocks_wide, job.blocks_high) /
            4)};
    procs_->cmd_push_constants(command_buffer, pipeline_layout_,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);
    procs_->cmd_dispatch(command_buffer,
                         (job.blocks_wide + kGroupBlocks - 1) / kGroupBlocks,
                         (job.blocks_high + kGroupBlocks - 1) / kGroupBlocks,
                         job.layers);
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
  VkDeviceSize total = source_bytes;
  for (Etc2GpuJob& job : jobs) {
    job.destination_offset = total;
    total += Etc2GpuDecodedBytes(job);
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

  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = total;
  buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (p.create_buffer(device_, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
    return finish(false, "self-test buffer creation failed");
  }
  VkMemoryRequirements requirements{};
  p.get_buffer_memory_requirements(device_, buffer, &requirements);
  constexpr VkMemoryPropertyFlags kHost =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = UINT32_MAX;
  for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if ((requirements.memoryTypeBits & (1U << i)) != 0 &&
        (memory.memoryTypes[i].propertyFlags & kHost) == kHost) {
      allocate.memoryTypeIndex = i;
      break;
    }
  }
  void* mapped = nullptr;
  if (allocate.memoryTypeIndex == UINT32_MAX ||
      p.allocate_memory(device_, &allocate, nullptr, &buffer_memory) !=
          VK_SUCCESS ||
      p.bind_buffer_memory(device_, buffer, buffer_memory, 0) != VK_SUCCESS ||
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
  for (const Etc2GpuJob& job : jobs) {
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
  return finish(true, nullptr);
}

}
