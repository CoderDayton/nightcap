#include "mocktail/graphics/vulkan_etc2_emulation.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace mocktail::graphics {
namespace {

void ExpectMapping(VkFormat format, VkFormat host, EtcFormat etc) {
  Etc2EmulatedFormat mapped;
  ASSERT_TRUE(LookupEmulatedEtc2Format(format, &mapped)) << format;
  EXPECT_EQ(mapped.host_format, host) << format;
  EXPECT_EQ(mapped.etc_format, etc) << format;
}

TEST(VulkanEtc2EmulationTest, MapsEveryEtc2AndEacFormat) {
  ExpectMapping(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM,
                EtcFormat::kEtc2Rgb8);
  ExpectMapping(VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB,
                EtcFormat::kEtc2Rgb8);
  ExpectMapping(VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM,
                EtcFormat::kEtc2Rgb8A1);
  ExpectMapping(VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB,
                EtcFormat::kEtc2Rgb8A1);
  ExpectMapping(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM,
                EtcFormat::kEtc2Rgba8);
  ExpectMapping(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB,
                EtcFormat::kEtc2Rgba8);
  ExpectMapping(VK_FORMAT_EAC_R11_UNORM_BLOCK, VK_FORMAT_R16_UNORM,
                EtcFormat::kEacR11);
  ExpectMapping(VK_FORMAT_EAC_R11_SNORM_BLOCK, VK_FORMAT_R16_SNORM,
                EtcFormat::kEacR11Signed);
  ExpectMapping(VK_FORMAT_EAC_R11G11_UNORM_BLOCK, VK_FORMAT_R16G16_UNORM,
                EtcFormat::kEacRg11);
  ExpectMapping(VK_FORMAT_EAC_R11G11_SNORM_BLOCK, VK_FORMAT_R16G16_SNORM,
                EtcFormat::kEacRg11Signed);
}

TEST(VulkanEtc2EmulationTest, IgnoresOtherFormats) {
  Etc2EmulatedFormat mapped;
  EXPECT_FALSE(LookupEmulatedEtc2Format(VK_FORMAT_R8G8B8A8_UNORM, &mapped));
  EXPECT_FALSE(LookupEmulatedEtc2Format(VK_FORMAT_BC7_UNORM_BLOCK, &mapped));
  EXPECT_FALSE(
      LookupEmulatedEtc2Format(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &mapped));
}

TEST(VulkanEtc2EmulationTest, KeepsOnlySamplingAndTransferFeatures) {
  const VkFormatFeatureFlags host =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
  EXPECT_EQ(EmulatedEtc2FormatFeatures(host),
            static_cast<VkFormatFeatureFlags>(
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT));
  EXPECT_EQ(EmulatedEtc2FormatFeatures(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT),
            0U);
}

TEST(VulkanEtc2EmulationTest, CountsUploadBytesPerRegion) {
  VkDeviceSize compressed = 0;
  VkDeviceSize decoded = 0;
  ASSERT_TRUE(Etc2UploadByteCounts(EtcFormat::kEtc2Rgba8, {5, 3, 1}, 2,
                                   &compressed, &decoded));
  EXPECT_EQ(compressed, 64U);
  EXPECT_EQ(decoded, 120U);

  ASSERT_TRUE(Etc2UploadByteCounts(EtcFormat::kEacR11, {1, 1, 1}, 1,
                                   &compressed, &decoded));
  EXPECT_EQ(compressed, 8U);
  EXPECT_EQ(decoded, 2U);
}

TEST(VulkanEtc2EmulationTest, RejectsUnsupportedRegions) {
  VkDeviceSize compressed = 0;
  VkDeviceSize decoded = 0;
  EXPECT_FALSE(Etc2UploadByteCounts(EtcFormat::kEtc2Rgb8, {4, 4, 2}, 1,
                                    &compressed, &decoded));
  EXPECT_FALSE(Etc2UploadByteCounts(EtcFormat::kEtc2Rgb8, {0, 4, 1}, 1,
                                    &compressed, &decoded));
  EXPECT_FALSE(Etc2UploadByteCounts(EtcFormat::kEtc2Rgb8, {4, 4, 1}, 0,
                                    &compressed, &decoded));
}

template <typename Handle>
Handle FakeHandle(std::uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

constexpr std::uintptr_t kSourceMemory = 0x300;
constexpr std::uintptr_t kStagingBuffer = 0x500;
constexpr std::uintptr_t kStagingMemory = 0x600;

std::array<std::uint8_t, 64> g_source{};
std::array<std::uint8_t, 64> g_staging{};

VKAPI_ATTR VkResult VKAPI_CALL FakeCreateImage(VkDevice,
                                               const VkImageCreateInfo*,
                                               const VkAllocationCallbacks*,
                                               VkImage* image) {
  *image = FakeHandle<VkImage>(0x100);
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL FakeCreateBuffer(VkDevice,
                                                const VkBufferCreateInfo*,
                                                const VkAllocationCallbacks*,
                                                VkBuffer* buffer) {
  *buffer = FakeHandle<VkBuffer>(kStagingBuffer);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL FakeBufferRequirements(
    VkDevice, VkBuffer, VkMemoryRequirements* requirements) {
  requirements->size = g_staging.size();
  requirements->memoryTypeBits = 1;
}

VKAPI_ATTR VkResult VKAPI_CALL FakeAllocateMemory(
    VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*,
    VkDeviceMemory* memory) {
  *memory = FakeHandle<VkDeviceMemory>(kStagingMemory);
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL FakeBindBufferMemory(VkDevice, VkBuffer,
                                                    VkDeviceMemory,
                                                    VkDeviceSize) {
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL FakeMapMemory(VkDevice, VkDeviceMemory memory,
                                             VkDeviceSize offset, VkDeviceSize,
                                             VkMemoryMapFlags, void** data) {
  *data = memory == FakeHandle<VkDeviceMemory>(kSourceMemory)
              ? g_source.data() + offset
              : g_staging.data() + offset;
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL FakeUnmapMemory(VkDevice, VkDeviceMemory) {}
VKAPI_ATTR void VKAPI_CALL FakeDestroyBuffer(VkDevice, VkBuffer,
                                             const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL FakeFreeMemory(VkDevice, VkDeviceMemory,
                                          const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL FakeCopyBufferToImage(VkCommandBuffer, VkBuffer,
                                                 VkImage, VkImageLayout,
                                                 std::uint32_t,
                                                 const VkBufferImageCopy*) {}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeGetDeviceProcAddr(
    VkDevice, const char* name) {
  const auto is = [name](const char* candidate) {
    return std::strcmp(name, candidate) == 0;
  };
  if (is("vkCreateImage")) return reinterpret_cast<PFN_vkVoidFunction>(FakeCreateImage);
  if (is("vkCreateBuffer")) return reinterpret_cast<PFN_vkVoidFunction>(FakeCreateBuffer);
  if (is("vkGetBufferMemoryRequirements"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeBufferRequirements);
  if (is("vkAllocateMemory")) return reinterpret_cast<PFN_vkVoidFunction>(FakeAllocateMemory);
  if (is("vkBindBufferMemory")) return reinterpret_cast<PFN_vkVoidFunction>(FakeBindBufferMemory);
  if (is("vkMapMemory")) return reinterpret_cast<PFN_vkVoidFunction>(FakeMapMemory);
  if (is("vkUnmapMemory")) return reinterpret_cast<PFN_vkVoidFunction>(FakeUnmapMemory);
  if (is("vkDestroyBuffer")) return reinterpret_cast<PFN_vkVoidFunction>(FakeDestroyBuffer);
  if (is("vkFreeMemory")) return reinterpret_cast<PFN_vkVoidFunction>(FakeFreeMemory);
  if (is("vkCmdCopyBufferToImage"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeCopyBufferToImage);
  return nullptr;
}

TEST(VulkanEtc2EmulationTest, SkipsUploadOutsideTheMappedWindow) {
  const VkDevice device = FakeHandle<VkDevice>(0x10);
  const VkBuffer source = FakeHandle<VkBuffer>(0x200);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x700);
  g_source.fill(0);
  g_staging.fill(0xAA);

  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x20), true,
                           memory, FakeGetDeviceProcAddr);

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  image_info.extent = {4, 4, 1};
  VkImage image = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &image),
            VK_SUCCESS);

  // The 8-byte block sits at memory offset 16, but only [0, 8) is mapped.
  ASSERT_EQ(emulation.BindBufferMemory(device, source, source_memory, 16),
            VK_SUCCESS);
  void* mapped = nullptr;
  ASSERT_EQ(emulation.MapMemory(device, source_memory, 0, 8, 0, &mapped),
            VK_SUCCESS);

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {4, 4, 1};
  emulation.CmdCopyBufferToImage(device, command_buffer, source, image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &region);
  emulation.PrepareSubmit(&command_buffer, 1);

  EXPECT_EQ(g_staging[0], 0xAA);
  emulation.ReleaseCommandBuffer(command_buffer);
}

TEST(VulkanEtc2EmulationTest, DecodesAgainOnEveryResubmit) {
  const VkDevice device = FakeHandle<VkDevice>(0x11);
  const VkBuffer source = FakeHandle<VkBuffer>(0x201);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x701);
  g_source.fill(0);
  g_staging.fill(0xAA);

  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x21), true,
                           memory, FakeGetDeviceProcAddr);

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  image_info.extent = {4, 4, 1};
  VkImage image = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &image),
            VK_SUCCESS);
  ASSERT_EQ(emulation.BindBufferMemory(device, source, source_memory, 0),
            VK_SUCCESS);
  void* mapped = nullptr;
  ASSERT_EQ(emulation.MapMemory(device, source_memory, 0, g_source.size(), 0,
                                &mapped),
            VK_SUCCESS);

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {4, 4, 1};
  emulation.CmdCopyBufferToImage(device, command_buffer, source, image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &region);

  // Differential block: every texel decodes to 134.
  const std::array<std::uint8_t, 8> first = {0x81, 0x81, 0x81, 0x02,
                                             0,    0,    0,    0};
  std::memcpy(g_source.data(), first.data(), first.size());
  emulation.PrepareSubmit(&command_buffer, 1);
  EXPECT_EQ(g_staging[0], 134);

  // Individual block: every texel decodes to 138.
  const std::array<std::uint8_t, 8> second = {0x88, 0x88, 0x88, 0x00,
                                              0,    0,    0,    0};
  std::memcpy(g_source.data(), second.data(), second.size());
  emulation.PrepareSubmit(&command_buffer, 1);
  EXPECT_EQ(g_staging[0], 138);
  emulation.ReleaseCommandBuffer(command_buffer);
}

}  // namespace
}  // namespace mocktail::graphics
