#ifndef MOCKTAIL_TESTS_VULKAN_TEST_DEVICE_H_
#define MOCKTAIL_TESTS_VULKAN_TEST_DEVICE_H_

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mocktail::testing {

struct VulkanTestDevice {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory{};
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  std::uint32_t family = 0;
  std::string name;
  // The device was created with textureCompressionBC.
  bool bc_textures = false;

  template <typename T>
  T Instance(const char* proc) const {
    return reinterpret_cast<T>(get_instance_proc_addr(instance, proc));
  }
  template <typename T>
  T Device(const char* proc) const {
    return reinterpret_cast<T>(get_device_proc_addr(device, proc));
  }
};

struct HostBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
};

inline VkInstance CreateTestInstance(PFN_vkGetInstanceProcAddr* get) {
  void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    return VK_NULL_HANDLE;
  }
  *get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(library, "vkGetInstanceProcAddr"));
  if (*get == nullptr) {
    return VK_NULL_HANDLE;
  }
  auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
      (*get)(VK_NULL_HANDLE, "vkCreateInstance"));
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  if (create_instance == nullptr ||
      create_instance(&info, nullptr, &instance) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return instance;
}

inline std::vector<VulkanTestDevice> OpenTestDevices() {
  std::vector<VulkanTestDevice> devices;
  VulkanTestDevice base;
  base.instance = CreateTestInstance(&base.get_instance_proc_addr);
  if (base.instance == VK_NULL_HANDLE) {
    return devices;
  }
  auto enumerate = base.Instance<PFN_vkEnumeratePhysicalDevices>(
      "vkEnumeratePhysicalDevices");
  std::uint32_t count = 0;
  enumerate(base.instance, &count, nullptr);
  std::vector<VkPhysicalDevice> physical(count);
  enumerate(base.instance, &count, physical.data());
  for (VkPhysicalDevice candidate : physical) {
    VulkanTestDevice vk = base;
    vk.physical_device = candidate;
    VkPhysicalDeviceProperties properties{};
    vk.Instance<PFN_vkGetPhysicalDeviceProperties>(
        "vkGetPhysicalDeviceProperties")(candidate, &properties);
    vk.name = properties.deviceName;
    vk.Instance<PFN_vkGetPhysicalDeviceMemoryProperties>(
        "vkGetPhysicalDeviceMemoryProperties")(candidate, &vk.memory);
    auto queue_properties =
        vk.Instance<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            "vkGetPhysicalDeviceQueueFamilyProperties");
    std::uint32_t families = 0;
    queue_properties(candidate, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_list(families);
    queue_properties(candidate, &families, family_list.data());
    bool found = false;
    for (std::uint32_t i = 0; i < families; ++i) {
      if ((family_list[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
          (family_list[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
        vk.family = i;
        found = true;
        break;
      }
    }
    if (!found) {
      continue;
    }
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = vk.family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures supported{};
    vk.Instance<PFN_vkGetPhysicalDeviceFeatures>(
        "vkGetPhysicalDeviceFeatures")(candidate, &supported);
    VkPhysicalDeviceFeatures features{};
    features.textureCompressionBC = supported.textureCompressionBC;
    vk.bc_textures = supported.textureCompressionBC == VK_TRUE;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &features;
    if (vk.Instance<PFN_vkCreateDevice>("vkCreateDevice")(
            candidate, &device_info, nullptr, &vk.device) != VK_SUCCESS) {
      continue;
    }
    vk.get_device_proc_addr =
        vk.Instance<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    vk.Device<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(vk.device, vk.family,
                                                       0, &vk.queue);
    devices.push_back(vk);
  }
  if (devices.empty()) {
    base.Instance<PFN_vkDestroyInstance>("vkDestroyInstance")(base.instance,
                                                              nullptr);
  }
  return devices;
}

inline void CloseTestDevices(std::vector<VulkanTestDevice>* devices) {
  if (devices->empty()) {
    return;
  }
  for (VulkanTestDevice& vk : *devices) {
    vk.Device<PFN_vkDestroyDevice>("vkDestroyDevice")(vk.device, nullptr);
  }
  const VulkanTestDevice& first = devices->front();
  first.Instance<PFN_vkDestroyInstance>("vkDestroyInstance")(first.instance,
                                                             nullptr);
  devices->clear();
}

inline std::uint32_t FindMemoryType(const VulkanTestDevice& vk,
                                    std::uint32_t allowed,
                                    VkMemoryPropertyFlags wanted) {
  for (std::uint32_t i = 0; i < vk.memory.memoryTypeCount; ++i) {
    if ((allowed & (1U << i)) != 0 &&
        (vk.memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
      return i;
    }
  }
  return UINT32_MAX;
}

inline HostBuffer CreateHostBuffer(const VulkanTestDevice& vk,
                                   VkDeviceSize size,
                                   VkBufferUsageFlags usage) {
  HostBuffer out;
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = size;
  info.usage = usage;
  EXPECT_EQ(vk.Device<PFN_vkCreateBuffer>("vkCreateBuffer")(
                vk.device, &info, nullptr, &out.buffer),
            VK_SUCCESS);
  VkMemoryRequirements requirements{};
  vk.Device<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(
      vk.device, out.buffer, &requirements);
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex =
      FindMemoryType(vk, requirements.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  EXPECT_NE(allocate.memoryTypeIndex, UINT32_MAX);
  EXPECT_EQ(vk.Device<PFN_vkAllocateMemory>("vkAllocateMemory")(
                vk.device, &allocate, nullptr, &out.memory),
            VK_SUCCESS);
  return out;
}

inline void BindAndMap(const VulkanTestDevice& vk, HostBuffer* buffer) {
  vk.Device<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
      vk.device, buffer->buffer, buffer->memory, 0);
  vk.Device<PFN_vkMapMemory>("vkMapMemory")(vk.device, buffer->memory, 0,
                                            VK_WHOLE_SIZE, 0, &buffer->mapped);
}

inline void DestroyHostBuffer(const VulkanTestDevice& vk,
                              const HostBuffer& buffer) {
  vk.Device<PFN_vkDestroyBuffer>("vkDestroyBuffer")(vk.device, buffer.buffer,
                                                    nullptr);
  vk.Device<PFN_vkFreeMemory>("vkFreeMemory")(vk.device, buffer.memory,
                                              nullptr);
}

struct TestCommands {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer buffer = VK_NULL_HANDLE;
};

inline TestCommands BeginTestCommands(const VulkanTestDevice& vk) {
  TestCommands out;
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = vk.family;
  vk.Device<PFN_vkCreateCommandPool>("vkCreateCommandPool")(
      vk.device, &pool_info, nullptr, &out.pool);
  VkCommandBufferAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate.commandPool = out.pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  vk.Device<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
      vk.device, &allocate, &out.buffer);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vk.Device<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(out.buffer,
                                                              &begin);
  return out;
}

inline void HostReadBarrier(const VulkanTestDevice& vk,
                            VkCommandBuffer command_buffer,
                            VkPipelineStageFlags source_stage,
                            VkAccessFlags source_access) {
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = source_access;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vk.Device<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
      command_buffer, source_stage, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier,
      0, nullptr, 0, nullptr);
}

inline bool SubmitAndWait(const VulkanTestDevice& vk,
                          const std::vector<VkCommandBuffer>& buffers) {
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = static_cast<std::uint32_t>(buffers.size());
  submit.pCommandBuffers = buffers.data();
  return vk.Device<PFN_vkQueueSubmit>("vkQueueSubmit")(vk.queue, 1, &submit,
                                                       VK_NULL_HANDLE) ==
             VK_SUCCESS &&
         vk.Device<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(vk.queue) ==
             VK_SUCCESS;
}

inline void DestroyTestCommands(const VulkanTestDevice& vk,
                                const TestCommands& commands) {
  vk.Device<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(
      vk.device, commands.pool, nullptr);
}

}

#endif
