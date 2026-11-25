#include "mocktail/graphics/vulkan_object_reuse.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mocktail::graphics {
namespace {

void AppendBytes(std::string* key, const void* data, std::size_t size) {
  key->append(static_cast<const char*>(data), size);
}

template <typename Value>
void AppendValue(std::string* key, const Value& value) {
  AppendBytes(key, &value, sizeof(value));
}

void AppendNames(std::string* key, const char* const* names,
                 std::uint32_t count) {
  std::vector<std::string> sorted;
  for (std::uint32_t index = 0; index < count; ++index) {
    sorted.emplace_back(names[index] != nullptr ? names[index] : "");
  }
  std::sort(sorted.begin(), sorted.end());
  AppendValue(key, count);
  for (const std::string& name : sorted) {
    key->append(name);
    key->push_back('\0');
  }
}

// End of the last field of a known feature struct, so tail padding, which the
// app may leave uninitialized, never reaches the key.
#define MOCKTAIL_FEATURE_END(Type, Struct, last) \
  case Type:                                     \
    return offsetof(Struct, last) + sizeof(Struct::last);

std::size_t FeatureStructEnd(VkStructureType type) {
  switch (type) {
    MOCKTAIL_FEATURE_END(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                         VkPhysicalDeviceFeatures2, features)
    MOCKTAIL_FEATURE_END(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                         VkPhysicalDeviceVulkan11Features, shaderDrawParameters)
    MOCKTAIL_FEATURE_END(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                         VkPhysicalDeviceVulkan12Features,
                         subgroupBroadcastDynamicId)
    MOCKTAIL_FEATURE_END(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                         VkPhysicalDeviceVulkan13Features, maintenance4)
    MOCKTAIL_FEATURE_END(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        VkPhysicalDeviceTimelineSemaphoreFeatures, timelineSemaphore)
    MOCKTAIL_FEATURE_END(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,
        VkPhysicalDeviceHostQueryResetFeatures, hostQueryReset)
    MOCKTAIL_FEATURE_END(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
        VkPhysicalDeviceSynchronization2Features, synchronization2)
    MOCKTAIL_FEATURE_END(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        VkPhysicalDeviceDynamicRenderingFeatures, dynamicRendering)
    MOCKTAIL_FEATURE_END(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
        VkPhysicalDeviceDescriptorIndexingFeatures, runtimeDescriptorArray)
    MOCKTAIL_FEATURE_END(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
        VkPhysicalDeviceBufferDeviceAddressFeatures,
        bufferDeviceAddressMultiDevice)
    MOCKTAIL_FEATURE_END(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
                         VkPhysicalDevice16BitStorageFeatures,
                         storageInputOutput16)
    MOCKTAIL_FEATURE_END(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,
                         VkPhysicalDevice8BitStorageFeatures,
                         storagePushConstant8)
    MOCKTAIL_FEATURE_END(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
        VkPhysicalDeviceShaderFloat16Int8Features, shaderInt8)
    MOCKTAIL_FEATURE_END(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
        VkPhysicalDeviceSamplerYcbcrConversionFeatures, samplerYcbcrConversion)
    MOCKTAIL_FEATURE_END(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
                         VkPhysicalDeviceMultiviewFeatures,
                         multiviewTessellationShader)
    MOCKTAIL_FEATURE_END(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,
        VkPhysicalDeviceShaderDrawParametersFeatures, shaderDrawParameters)
    default:
      return 0;
  }
}

#undef MOCKTAIL_FEATURE_END

}  // namespace

std::string VulkanInstanceKey(const VkInstanceCreateInfo& info) {
  std::string key;
  AppendValue(&key, info.flags);
  AppendValue(&key, info.pApplicationInfo != nullptr
                        ? info.pApplicationInfo->apiVersion
                        : 0U);
  AppendNames(&key, info.ppEnabledLayerNames, info.enabledLayerCount);
  AppendNames(&key, info.ppEnabledExtensionNames, info.enabledExtensionCount);
  return key;
}

std::string VulkanDeviceKey(VkPhysicalDevice physical_device,
                            const VkDeviceCreateInfo& info) {
  std::string key;
  AppendValue(&key, physical_device);
  AppendValue(&key, info.flags);
  AppendNames(&key, info.ppEnabledExtensionNames, info.enabledExtensionCount);
  AppendValue(&key, info.queueCreateInfoCount);
  for (std::uint32_t index = 0; index < info.queueCreateInfoCount; ++index) {
    const VkDeviceQueueCreateInfo& queue = info.pQueueCreateInfos[index];
    AppendValue(&key, queue.flags);
    AppendValue(&key, queue.queueFamilyIndex);
    AppendValue(&key, queue.queueCount);
    if (queue.pQueuePriorities != nullptr) {
      AppendBytes(&key, queue.pQueuePriorities,
                  sizeof(float) * queue.queueCount);
    }
  }
  if (info.pEnabledFeatures != nullptr) {
    AppendValue(&key, *info.pEnabledFeatures);
  }
  // A struct this does not know is keyed by type only. The same game build
  // asks for the same chain each time.
  for (auto* link = static_cast<const VkBaseInStructure*>(info.pNext);
       link != nullptr; link = link->pNext) {
    AppendValue(&key, link->sType);
    const std::size_t end = FeatureStructEnd(link->sType);
    if (end > sizeof(VkBaseInStructure)) {
      AppendBytes(&key,
                  reinterpret_cast<const char*>(link) +
                      sizeof(VkBaseInStructure),
                  end - sizeof(VkBaseInStructure));
    }
  }
  return key;
}

}  // namespace mocktail::graphics
