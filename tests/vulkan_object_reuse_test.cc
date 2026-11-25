#include "mocktail/graphics/vulkan_object_reuse.h"

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include <vector>

namespace mocktail::graphics {
namespace {

VkPhysicalDevice Gpu(std::uintptr_t id) {
  return reinterpret_cast<VkPhysicalDevice>(id);
}

struct DeviceRequest {
  DeviceRequest() {
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = 0;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12.timelineSemaphore = VK_TRUE;
    features.samplerAnisotropy = VK_TRUE;
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &vulkan12;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.pEnabledFeatures = &features;
  }
  DeviceRequest(const DeviceRequest&) = delete;

  float priority = 1.0f;
  std::vector<const char*> extensions = {"VK_KHR_swapchain",
                                         "VK_KHR_maintenance1"};
  VkDeviceQueueCreateInfo queue{};
  VkPhysicalDeviceVulkan12Features vulkan12{};
  VkPhysicalDeviceFeatures features{};
  VkDeviceCreateInfo info{};
};

TEST(VulkanObjectReuseTest, TheSameDeviceRequestGivesTheSameKey) {
  const DeviceRequest first;
  const DeviceRequest second;
  EXPECT_EQ(VulkanDeviceKey(Gpu(1), first.info),
            VulkanDeviceKey(Gpu(1), second.info));
}

TEST(VulkanObjectReuseTest, ADifferentDeviceRequestGivesADifferentKey) {
  const DeviceRequest base;
  const std::string key = VulkanDeviceKey(Gpu(1), base.info);
  EXPECT_NE(key, VulkanDeviceKey(Gpu(2), base.info));

  DeviceRequest extension;
  extension.extensions.pop_back();
  extension.info.enabledExtensionCount = 1;
  EXPECT_NE(key, VulkanDeviceKey(Gpu(1), extension.info));

  DeviceRequest queue;
  queue.priority = 0.5f;
  EXPECT_NE(key, VulkanDeviceKey(Gpu(1), queue.info));

  DeviceRequest chained_feature;
  chained_feature.vulkan12.hostQueryReset = VK_TRUE;
  EXPECT_NE(key, VulkanDeviceKey(Gpu(1), chained_feature.info));

  DeviceRequest root_feature;
  root_feature.features.samplerAnisotropy = VK_FALSE;
  EXPECT_NE(key, VulkanDeviceKey(Gpu(1), root_feature.info));
}

TEST(VulkanObjectReuseTest, ExtensionOrderDoesNotChangeTheKey) {
  const DeviceRequest base;
  DeviceRequest reordered;
  std::swap(reordered.extensions[0], reordered.extensions[1]);
  EXPECT_EQ(VulkanDeviceKey(Gpu(1), base.info),
            VulkanDeviceKey(Gpu(1), reordered.info));
}

TEST(VulkanObjectReuseTest, InstanceKeyFollowsExtensionsLayersAndVersion) {
  const char* extensions[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
  VkApplicationInfo application{};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &application;
  info.enabledExtensionCount = 2;
  info.ppEnabledExtensionNames = extensions;
  const std::string key = VulkanInstanceKey(info);
  EXPECT_EQ(key, VulkanInstanceKey(info));

  application.apiVersion = VK_API_VERSION_1_1;
  EXPECT_NE(key, VulkanInstanceKey(info));
  application.apiVersion = VK_API_VERSION_1_2;
  info.enabledExtensionCount = 1;
  EXPECT_NE(key, VulkanInstanceKey(info));
}

TEST(VulkanObjectReuseTest, AKeptObjectIsHandedBackOnceForAMatchingKey) {
  KeptVulkanObject<VkDevice> kept;
  const auto device = reinterpret_cast<VkDevice>(0x10);
  EXPECT_EQ(kept.Keep(device, "a"), VK_NULL_HANDLE);

  EXPECT_EQ(kept.Take("b"), VK_NULL_HANDLE);
  EXPECT_EQ(kept.Take("a"), device);
  EXPECT_EQ(kept.Take("a"), VK_NULL_HANDLE);
}

TEST(VulkanObjectReuseTest, KeepingAnotherObjectReturnsTheOneItDisplaces) {
  KeptVulkanObject<VkDevice> kept;
  const auto first = reinterpret_cast<VkDevice>(0x10);
  const auto second = reinterpret_cast<VkDevice>(0x20);
  kept.Keep(first, "a");
  EXPECT_EQ(kept.Keep(second, "b"), first);
  EXPECT_EQ(kept.Take("b"), second);
}

}  // namespace
}  // namespace mocktail::graphics
