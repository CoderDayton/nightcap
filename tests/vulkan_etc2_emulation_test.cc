#include "mocktail/graphics/vulkan_etc2_emulation.h"

#include "mocktail/graphics/etc2_decoder.h"
#include "mocktail/graphics/texture_override.h"

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

TEST(VulkanEtc2EmulationTest, AdvertisesEtc2UnlessSwitchedOff) {
  EXPECT_TRUE(Etc2SupportAdvertised(nullptr));
  EXPECT_TRUE(Etc2SupportAdvertised(""));
  EXPECT_TRUE(Etc2SupportAdvertised("1"));
  EXPECT_FALSE(Etc2SupportAdvertised("0"));
}

TEST(VulkanEtc2EmulationTest, ParsesSmallTextureUpscale) {
  EXPECT_EQ(SmallTextureUpscale(nullptr), 4u);
  EXPECT_EQ(SmallTextureUpscale(""), 4u);
  EXPECT_EQ(SmallTextureUpscale("abc"), 4u);
  EXPECT_EQ(SmallTextureUpscale("0"), 1u);
  EXPECT_EQ(SmallTextureUpscale("1"), 1u);
  EXPECT_EQ(SmallTextureUpscale("2"), 2u);
  EXPECT_EQ(SmallTextureUpscale("99"), 8u);
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
std::array<std::uint8_t, 512> g_staging{};
VkExtent3D g_created_extent{};
VkExtent3D g_copied_extent{};
VkDeviceSize g_created_buffer_size = 0;
std::uintptr_t g_next_image = 0x100;
std::uint32_t g_copy_image_calls = 0;
std::uint32_t g_create_buffer_calls = 0;
std::uint32_t g_allocate_memory_calls = 0;
std::uint32_t g_destroy_buffer_calls = 0;
std::uint32_t g_free_memory_calls = 0;
VkImageBlit g_blit{};
VkFilter g_blit_filter = VK_FILTER_NEAREST;

VKAPI_ATTR VkResult VKAPI_CALL FakeCreateImage(VkDevice,
                                               const VkImageCreateInfo* info,
                                               const VkAllocationCallbacks*,
                                               VkImage* image) {
  g_created_extent = info->extent;
  *image = FakeHandle<VkImage>(g_next_image++);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL FakeCopyImage(VkCommandBuffer, VkImage,
                                         VkImageLayout, VkImage, VkImageLayout,
                                         std::uint32_t, const VkImageCopy*) {
  ++g_copy_image_calls;
}

VKAPI_ATTR void VKAPI_CALL FakeBlitImage(VkCommandBuffer, VkImage,
                                         VkImageLayout, VkImage, VkImageLayout,
                                         std::uint32_t count,
                                         const VkImageBlit* blits,
                                         VkFilter filter) {
  g_blit = count > 0 ? blits[0] : VkImageBlit{};
  g_blit_filter = filter;
}

VKAPI_ATTR VkResult VKAPI_CALL FakeCreateBuffer(VkDevice,
                                                const VkBufferCreateInfo* info,
                                                const VkAllocationCallbacks*,
                                                VkBuffer* buffer) {
  g_created_buffer_size = info->size;
  ++g_create_buffer_calls;
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
  ++g_allocate_memory_calls;
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
                                             const VkAllocationCallbacks*) {
  ++g_destroy_buffer_calls;
}
VKAPI_ATTR void VKAPI_CALL FakeFreeMemory(VkDevice, VkDeviceMemory,
                                          const VkAllocationCallbacks*) {
  ++g_free_memory_calls;
}
VKAPI_ATTR void VKAPI_CALL FakeCopyBufferToImage(
    VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, std::uint32_t count,
    const VkBufferImageCopy* regions) {
  g_copied_extent = count > 0 ? regions[0].imageExtent : VkExtent3D{};
}

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
  if (is("vkCmdCopyImage")) return reinterpret_cast<PFN_vkVoidFunction>(FakeCopyImage);
  if (is("vkCmdBlitImage")) return reinterpret_cast<PFN_vkVoidFunction>(FakeBlitImage);
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

TEST(VulkanEtc2EmulationTest, UpscalesSmallTexturesAndTheirUploads) {
  const VkDevice device = FakeHandle<VkDevice>(0x12);
  const VkBuffer source = FakeHandle<VkBuffer>(0x202);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x702);
  g_source.fill(0);
  g_staging.fill(0xAA);
  ASSERT_EQ(setenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE", "2", 1), 0);

  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VulkanEtc2Emulation emulation;
  ASSERT_EQ(unsetenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"), 0);
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x22), true,
                           memory, FakeGetDeviceProcAddr);

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {4, 4, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  VkImage image = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &image),
            VK_SUCCESS);
  EXPECT_EQ(g_created_extent.width, 8u);
  EXPECT_EQ(g_created_extent.height, 8u);
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
  EXPECT_EQ(g_copied_extent.width, 8u);
  EXPECT_EQ(g_copied_extent.height, 8u);

  const std::array<std::uint8_t, 8> block = {0x81, 0x81, 0x81, 0x02,
                                             0,    0,    0,    0};
  std::memcpy(g_source.data(), block.data(), block.size());
  emulation.PrepareSubmit(&command_buffer, 1);
  // The host region is 8x8: every 4x4 texel repeated 2x2.
  std::array<std::uint8_t, 4 * 4 * 4> expected{};
  ASSERT_TRUE(DecodeEtcImage(EtcFormat::kEtc2Rgb8, block.data(), block.size(),
                             4, 4, expected.data(), expected.size()));
  for (std::size_t y = 0; y < 8; ++y) {
    for (std::size_t x = 0; x < 8; ++x) {
      for (std::size_t channel = 0; channel < 4; ++channel) {
        ASSERT_EQ(g_staging[(y * 8 + x) * 4 + channel],
                  expected[((y / 2) * 4 + x / 2) * 4 + channel])
            << x << "," << y;
      }
    }
  }
  EXPECT_EQ(g_staging[8 * 8 * 4], 0xAA);
  emulation.ReleaseCommandBuffer(command_buffer);
}

// Block-rounded uploads (a 1x1 mip sent as 4x4) must not scale past the
// host mip; the host region snaps to the host mip's edge instead.
TEST(VulkanEtc2EmulationTest, ClampsScaledUploadsToTheHostMip) {
  const VkDevice device = FakeHandle<VkDevice>(0x14);
  const VkBuffer source = FakeHandle<VkBuffer>(0x204);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x704);
  g_source.fill(0);
  g_staging.fill(0xAA);
  ASSERT_EQ(setenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE", "2", 1), 0);

  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VulkanEtc2Emulation emulation;
  ASSERT_EQ(unsetenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"), 0);
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x24), true,
                           memory, FakeGetDeviceProcAddr);

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {4, 4, 1};
  image_info.mipLevels = 3;
  image_info.arrayLayers = 1;
  VkImage image = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &image),
            VK_SUCCESS);
  ASSERT_EQ(emulation.BindBufferMemory(device, source, source_memory, 0),
            VK_SUCCESS);
  void* mapped = nullptr;
  ASSERT_EQ(emulation.MapMemory(device, source_memory, 0, g_source.size(), 0,
                                &mapped),
            VK_SUCCESS);

  // Mip 2 is 1x1 for the application and 2x2 on the host.
  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 2;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {4, 4, 1};
  g_copied_extent = {};
  emulation.CmdCopyBufferToImage(device, command_buffer, source, image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &region);
  EXPECT_EQ(g_copied_extent.width, 2u);
  EXPECT_EQ(g_copied_extent.height, 2u);
  EXPECT_EQ(g_created_buffer_size, 2u * 2u * 4u);

  // Differential block: every texel decodes to 134.
  const std::array<std::uint8_t, 8> block = {0x81, 0x81, 0x81, 0x02,
                                             0,    0,    0,    0};
  std::memcpy(g_source.data(), block.data(), block.size());
  emulation.PrepareSubmit(&command_buffer, 1);
  for (std::size_t index = 0; index < 2 * 2 * 4; ++index) {
    ASSERT_EQ(g_staging[index], index % 4 == 3 ? 255 : 134) << index;
  }
  EXPECT_EQ(g_staging[2 * 2 * 4], 0xAA);
  emulation.ReleaseCommandBuffer(command_buffer);
}

// Drawn at its own size, a scaled texture samples a smaller host mip, so
// every host mip carries level-0 detail rather than the application's mip.
TEST(VulkanEtc2EmulationTest, FillsScaledMipsFromLevelZero) {
  const VkDevice device = FakeHandle<VkDevice>(0x15);
  const VkBuffer source = FakeHandle<VkBuffer>(0x205);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x705);
  g_source.fill(0);
  g_staging.fill(0xAA);
  ASSERT_EQ(setenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE", "2", 1), 0);

  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VulkanEtc2Emulation emulation;
  ASSERT_EQ(unsetenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"), 0);
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x25), true,
                           memory, FakeGetDeviceProcAddr);

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {4, 4, 1};
  image_info.mipLevels = 3;
  image_info.arrayLayers = 1;
  VkImage image = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &image),
            VK_SUCCESS);
  ASSERT_EQ(emulation.BindBufferMemory(device, source, source_memory, 0),
            VK_SUCCESS);
  void* mapped = nullptr;
  ASSERT_EQ(emulation.MapMemory(device, source_memory, 0, g_source.size(), 0,
                                &mapped),
            VK_SUCCESS);

  // Level 0 decodes to 134 everywhere, level 1 (at byte 8) to 138.
  const std::array<std::uint8_t, 8> level0 = {0x81, 0x81, 0x81, 0x02,
                                              0,    0,    0,    0};
  const std::array<std::uint8_t, 8> level1 = {0x88, 0x88, 0x88, 0x00,
                                              0,    0,    0,    0};
  std::memcpy(g_source.data(), level0.data(), level0.size());
  std::memcpy(g_source.data() + 8, level1.data(), level1.size());

  std::array<VkBufferImageCopy, 2> regions{};
  regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  regions[0].imageSubresource.layerCount = 1;
  regions[0].imageExtent = {4, 4, 1};
  regions[1] = regions[0];
  regions[1].bufferOffset = 8;
  regions[1].imageSubresource.mipLevel = 1;
  regions[1].imageExtent = {2, 2, 1};
  emulation.CmdCopyBufferToImage(device, command_buffer, source, image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2,
                                 regions.data());
  emulation.PrepareSubmit(&command_buffer, 1);

  // Host level 0 is 8x8 (256 bytes); host level 1 is 4x4 and follows it,
  // holding level 0 itself rather than the 138 texels of level 1.
  RgbaImage level0_texels;
  level0_texels.width = 4;
  level0_texels.height = 4;
  level0_texels.pixels.resize(4 * 4 * 4);
  ASSERT_TRUE(DecodeEtcImage(EtcFormat::kEtc2Rgb8, level0.data(),
                             level0.size(), 4, 4, level0_texels.pixels.data(),
                             level0_texels.pixels.size()));
  std::array<std::uint8_t, 4 * 4 * 4> expected{};
  ResampleRgba(level0_texels, 4, 4, expected.data());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    ASSERT_EQ(g_staging[256 + index], expected[index]) << index;
  }
  emulation.ReleaseCommandBuffer(command_buffer);
}

// Level 0 carries the texels every scaled mip is built from, so a mip
// recorded ahead of it must still resolve against it.
TEST(VulkanEtc2EmulationTest, FillsScaledMipsFromLevelZeroRecordedLast) {
  const VkDevice device = FakeHandle<VkDevice>(0x16);
  const VkBuffer source = FakeHandle<VkBuffer>(0x206);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x706);
  g_source.fill(0);
  g_staging.fill(0xAA);
  ASSERT_EQ(setenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE", "2", 1), 0);

  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VulkanEtc2Emulation emulation;
  ASSERT_EQ(unsetenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"), 0);
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x26), true,
                           memory, FakeGetDeviceProcAddr);

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {4, 4, 1};
  image_info.mipLevels = 3;
  image_info.arrayLayers = 1;
  VkImage image = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &image),
            VK_SUCCESS);
  ASSERT_EQ(emulation.BindBufferMemory(device, source, source_memory, 0),
            VK_SUCCESS);
  void* mapped = nullptr;
  ASSERT_EQ(emulation.MapMemory(device, source_memory, 0, g_source.size(), 0,
                                &mapped),
            VK_SUCCESS);

  const std::array<std::uint8_t, 8> level0 = {0x81, 0x81, 0x81, 0x02,
                                              0,    0,    0,    0};
  const std::array<std::uint8_t, 8> level1 = {0x88, 0x88, 0x88, 0x00,
                                              0,    0,    0,    0};
  std::memcpy(g_source.data(), level0.data(), level0.size());
  std::memcpy(g_source.data() + 8, level1.data(), level1.size());

  // Level 1 is recorded first, so its staging bytes lead the batch.
  std::array<VkBufferImageCopy, 2> regions{};
  regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  regions[0].imageSubresource.layerCount = 1;
  regions[0].bufferOffset = 8;
  regions[0].imageSubresource.mipLevel = 1;
  regions[0].imageExtent = {2, 2, 1};
  regions[1] = regions[0];
  regions[1].bufferOffset = 0;
  regions[1].imageSubresource.mipLevel = 0;
  regions[1].imageExtent = {4, 4, 1};
  emulation.CmdCopyBufferToImage(device, command_buffer, source, image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2,
                                 regions.data());
  emulation.PrepareSubmit(&command_buffer, 1);

  RgbaImage level0_texels;
  level0_texels.width = 4;
  level0_texels.height = 4;
  level0_texels.pixels.resize(4 * 4 * 4);
  ASSERT_TRUE(DecodeEtcImage(EtcFormat::kEtc2Rgb8, level0.data(),
                             level0.size(), 4, 4, level0_texels.pixels.data(),
                             level0_texels.pixels.size()));
  std::array<std::uint8_t, 4 * 4 * 4> expected{};
  ResampleRgba(level0_texels, 4, 4, expected.data());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    ASSERT_EQ(g_staging[index], expected[index]) << index;
  }
  emulation.ReleaseCommandBuffer(command_buffer);
}

TEST(VulkanEtc2EmulationTest, ReusesStagingBuffersAcrossCommandBuffers) {
  const VkDevice device = FakeHandle<VkDevice>(0x17);
  const VkBuffer source = FakeHandle<VkBuffer>(0x207);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  g_source.fill(0);

  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x27), true,
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

  g_create_buffer_calls = 0;
  g_allocate_memory_calls = 0;
  g_destroy_buffer_calls = 0;
  g_free_memory_calls = 0;
  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {4, 4, 1};
  for (std::uintptr_t frame = 0; frame < 8; ++frame) {
    const VkCommandBuffer command_buffer =
        FakeHandle<VkCommandBuffer>(0x780 + frame);
    emulation.CmdCopyBufferToImage(device, command_buffer, source, image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
    emulation.PrepareSubmit(&command_buffer, 1);
    emulation.ReleaseCommandBuffer(command_buffer);
  }
  EXPECT_EQ(g_create_buffer_calls, 1u);
  EXPECT_EQ(g_allocate_memory_calls, 1u);
  EXPECT_EQ(g_destroy_buffer_calls, 0u);

  emulation.DestroyDevice(device);
  EXPECT_EQ(g_destroy_buffer_calls, 1u);
  EXPECT_EQ(g_free_memory_calls, 1u);
}

// Copies out of a scaled image cover its host bounds and shrink back onto
// an unscaled destination, which a plain copy cannot do.
TEST(VulkanEtc2EmulationTest, BlitsCopiesThatTouchScaledImages) {
  const VkDevice device = FakeHandle<VkDevice>(0x16);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x706);
  ASSERT_EQ(setenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE", "2", 1), 0);

  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VulkanEtc2Emulation emulation;
  ASSERT_EQ(unsetenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"), 0);
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x26), true,
                           memory, FakeGetDeviceProcAddr);

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {4, 4, 1};
  image_info.mipLevels = 3;
  image_info.arrayLayers = 1;
  VkImage small = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &small),
            VK_SUCCESS);
  image_info.extent = {128, 128, 1};
  image_info.mipLevels = 8;
  VkImage large = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &large),
            VK_SUCCESS);

  // Mip 1 of the small image (2x2, block-rounded to 4x4) lands in mip 6 of
  // the large one, 2x2 at offset (1, 1).
  VkImageCopy region{};
  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.mipLevel = 1;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource = region.srcSubresource;
  region.dstSubresource.mipLevel = 6;
  region.dstOffset = {1, 1, 0};
  region.extent = {4, 4, 1};
  g_copy_image_calls = 0;
  g_blit = {};
  emulation.CmdCopyImage(device, command_buffer, small,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, large,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  EXPECT_EQ(g_copy_image_calls, 0u);
  EXPECT_EQ(g_blit.srcSubresource.mipLevel, 1u);
  EXPECT_EQ(g_blit.srcOffsets[1].x, 4);
  EXPECT_EQ(g_blit.srcOffsets[1].y, 4);
  EXPECT_EQ(g_blit.dstSubresource.mipLevel, 6u);
  EXPECT_EQ(g_blit.dstOffsets[0].x, 1);
  EXPECT_EQ(g_blit.dstOffsets[1].x, 5);
  EXPECT_EQ(g_blit_filter, VK_FILTER_LINEAR);

  // Copies between unscaled images pass through untouched.
  emulation.CmdCopyImage(device, command_buffer, large,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, large,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  EXPECT_EQ(g_copy_image_calls, 1u);
}

// bufferRowLength counts texels of source pitch, not of the copied region, so
// a padded row holds blocks the region does not cover.
TEST(VulkanEtc2EmulationTest, ReadsCompressedRowsAtTheRequestedStride) {
  const VkDevice device = FakeHandle<VkDevice>(0x18);
  const VkBuffer source = FakeHandle<VkBuffer>(0x208);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x708);
  g_source.fill(0);
  g_staging.fill(0xAA);
  ASSERT_EQ(setenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE", "1", 1), 0);

  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VulkanEtc2Emulation emulation;
  ASSERT_EQ(unsetenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"), 0);
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x28), true,
                           memory, FakeGetDeviceProcAddr);

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {8, 8, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  VkImage image = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &image),
            VK_SUCCESS);
  ASSERT_EQ(g_created_extent.width, 8u);
  ASSERT_EQ(emulation.BindBufferMemory(device, source, source_memory, 0),
            VK_SUCCESS);
  void* mapped = nullptr;
  ASSERT_EQ(emulation.MapMemory(device, source_memory, 0, g_source.size(), 0,
                                &mapped),
            VK_SUCCESS);

  // Differential block: every texel decodes to 134.
  const std::array<std::uint8_t, 8> covered = {0x81, 0x81, 0x81, 0x02,
                                               0,    0,    0,    0};
  // Individual block: every texel decodes to 138.
  const std::array<std::uint8_t, 8> padding = {0x88, 0x88, 0x88, 0x00,
                                               0,    0,    0,    0};
  // 16 texels of pitch is 4 blocks per row; the 8x8 region covers the first 2.
  const std::array<const std::uint8_t*, 6> layout = {
      covered.data(), covered.data(), padding.data(),
      padding.data(), covered.data(), covered.data()};
  for (std::size_t block = 0; block < layout.size(); ++block) {
    std::memcpy(g_source.data() + block * 8, layout[block], 8);
  }

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {8, 8, 1};
  region.bufferRowLength = 16;
  emulation.CmdCopyBufferToImage(device, command_buffer, source, image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &region);
  emulation.PrepareSubmit(&command_buffer, 1);

  std::array<std::uint8_t, 32> packed{};
  for (std::size_t block = 0; block < 4; ++block) {
    std::memcpy(packed.data() + block * 8, covered.data(), covered.size());
  }
  std::array<std::uint8_t, 8 * 8 * 4> expected{};
  ASSERT_TRUE(DecodeEtcImage(EtcFormat::kEtc2Rgb8, packed.data(), packed.size(),
                             8, 8, expected.data(), expected.size()));
  for (std::size_t index = 0; index < expected.size(); ++index) {
    ASSERT_EQ(g_staging[index], expected[index]) << "byte " << index;
  }
  emulation.ReleaseCommandBuffer(command_buffer);
}

}  // namespace
}  // namespace mocktail::graphics
