#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <vector>

#include "mocktail/graphics/etc2_decoder.h"
#include "mocktail/graphics/vulkan_etc2_emulation.h"
#include "vulkan_test_device.h"

namespace mocktail::graphics {
namespace {

using mocktail::testing::BeginTestCommands;
using mocktail::testing::BindAndMap;
using mocktail::testing::CloseTestDevices;
using mocktail::testing::CreateHostBuffer;
using mocktail::testing::DestroyHostBuffer;
using mocktail::testing::DestroyTestCommands;
using mocktail::testing::FindMemoryType;
using mocktail::testing::HostBuffer;
using mocktail::testing::HostReadBarrier;
using mocktail::testing::OpenTestDevices;
using mocktail::testing::SubmitAndWait;
using mocktail::testing::TestCommands;
using mocktail::testing::VulkanTestDevice;

struct UploadCase {
  VkFormat format = VK_FORMAT_UNDEFINED;
  EtcFormat etc = EtcFormat::kEtc2Rgb8;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t layers = 1;
  std::uint32_t row_length = 0;
  std::uint32_t image_height = 0;
  VkDeviceSize offset = 0;
};

void ImageBarrier(const VulkanTestDevice& vk, VkCommandBuffer command_buffer,
                  VkImage image, std::uint32_t layers, VkImageLayout from,
                  VkImageLayout to, VkAccessFlags source_access,
                  VkAccessFlags destination_access) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = source_access;
  barrier.dstAccessMask = destination_access;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
  vk.Device<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
      command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

std::vector<std::uint8_t> DecodeOnCpu(const UploadCase& c,
                                      const std::uint8_t* source) {
  const std::size_t block_bytes = EtcBlockBytes(c.etc);
  const std::size_t texel_bytes = EtcDecodedTexelBytes(c.etc);
  const std::uint32_t blocks_wide = (c.width + 3) / 4;
  const std::uint32_t blocks_high = (c.height + 3) / 4;
  const std::size_t row_stride =
      std::size_t{(c.row_length != 0 ? c.row_length : c.width) + 3} / 4 *
      block_bytes;
  const std::size_t layer_stride =
      row_stride *
      ((std::size_t{c.image_height != 0 ? c.image_height : c.height} + 3) / 4);
  const std::size_t layer_texels = std::size_t{c.width} * c.height;
  std::vector<std::uint8_t> tight(std::size_t{blocks_wide} * blocks_high *
                                  block_bytes);
  std::vector<std::uint8_t> out(layer_texels * texel_bytes * c.layers);
  for (std::uint32_t layer = 0; layer < c.layers; ++layer) {
    for (std::uint32_t row = 0; row < blocks_high; ++row) {
      std::memcpy(tight.data() + std::size_t{row} * blocks_wide * block_bytes,
                  source + c.offset + layer * layer_stride + row * row_stride,
                  std::size_t{blocks_wide} * block_bytes);
    }
    EXPECT_TRUE(DecodeEtcImage(c.etc, tight.data(), tight.size(), c.width,
                               c.height,
                               out.data() + layer * layer_texels * texel_bytes,
                               layer_texels * texel_bytes));
  }
  return out;
}

void ExpectGpuUploadMatchesCpu(const UploadCase& c) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = c.format;
    image_info.extent = {c.width, c.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = c.layers;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image = VK_NULL_HANDLE;
    ASSERT_EQ(emulation.CreateImage(vk.device, &image_info, nullptr, &image),
              VK_SUCCESS);
    VkMemoryRequirements image_requirements{};
    vk.Device<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
        vk.device, image, &image_requirements);
    VkMemoryAllocateInfo image_allocate{};
    image_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_allocate.allocationSize = image_requirements.size;
    image_allocate.memoryTypeIndex = FindMemoryType(
        vk, image_requirements.memoryTypeBits, 0);
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    ASSERT_EQ(vk.Device<PFN_vkAllocateMemory>("vkAllocateMemory")(
                  vk.device, &image_allocate, nullptr, &image_memory),
              VK_SUCCESS);
    vk.Device<PFN_vkBindImageMemory>("vkBindImageMemory")(vk.device, image,
                                                          image_memory, 0);

    const std::size_t block_bytes = EtcBlockBytes(c.etc);
    const std::size_t row_stride =
        std::size_t{(c.row_length != 0 ? c.row_length : c.width) + 3} / 4 *
        block_bytes;
    const std::size_t layer_stride =
        row_stride * ((std::size_t{c.image_height != 0 ? c.image_height
                                                        : c.height} +
                       3) /
                      4);
    const std::size_t source_bytes = c.offset + layer_stride * c.layers;
    HostBuffer source =
        CreateHostBuffer(vk, source_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    ASSERT_EQ(emulation.BindBufferMemory(vk.device, source.buffer,
                                         source.memory, 0),
              VK_SUCCESS);
    ASSERT_EQ(emulation.MapMemory(vk.device, source.memory, 0, VK_WHOLE_SIZE,
                                  0, &source.mapped),
              VK_SUCCESS);
    const std::size_t texel_bytes = EtcDecodedTexelBytes(c.etc);
    const std::size_t readback_bytes =
        std::size_t{c.width} * c.height * texel_bytes * c.layers;
    HostBuffer readback = CreateHostBuffer(vk, readback_bytes,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    ImageBarrier(vk, commands.buffer, image, c.layers,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = c.offset;
    region.bufferRowLength = c.row_length;
    region.bufferImageHeight = c.image_height;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, c.layers};
    region.imageExtent = {c.width, c.height, 1};
    emulation.CmdCopyBufferToImage(vk.device, commands.buffer, source.buffer,
                                   image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &region);
    ImageBarrier(vk, commands.buffer, image, c.layers,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, c.layers};
    back.imageExtent = {c.width, c.height, 1};
    vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        commands.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.buffer, 1, &back);
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);

    std::mt19937_64 random(c.width * 131 + c.height);
    for (int submit = 0; submit < 2; ++submit) {
      SCOPED_TRACE(submit);
      auto* bytes = static_cast<std::uint8_t*>(source.mapped);
      for (std::size_t i = 0; i < source_bytes; ++i) {
        bytes[i] = static_cast<std::uint8_t>(random());
      }
      const Etc2SubmitWait wait =
          emulation.PrepareSubmit(&commands.buffer, 1, true, vk.queue);
      EXPECT_EQ(wait.semaphore, VK_NULL_HANDLE);
      ASSERT_FALSE(wait.decode_commands.empty());
      std::vector<VkCommandBuffer> submitted = wait.decode_commands;
      submitted.push_back(commands.buffer);
      ASSERT_TRUE(SubmitAndWait(vk, submitted));
      const std::vector<std::uint8_t> expected = DecodeOnCpu(c, bytes);
      ASSERT_EQ(expected.size(), readback_bytes);
      EXPECT_EQ(std::memcmp(readback.mapped, expected.data(), readback_bytes),
                0);
    }

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    emulation.UnmapMemory(vk.device, source.memory);
    emulation.DestroyBuffer(vk.device, source.buffer, nullptr);
    emulation.FreeMemory(vk.device, source.memory, nullptr);
    DestroyHostBuffer(vk, readback);
    emulation.DestroyImage(vk.device, image, nullptr);
    vk.Device<PFN_vkFreeMemory>("vkFreeMemory")(vk.device, image_memory,
                                                nullptr);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

TEST(VulkanEtc2GpuEmulationTest, DecodesAPaddedRgbaUploadOnTheGpu) {
  UploadCase c;
  c.format = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
  c.etc = EtcFormat::kEtc2Rgba8;
  c.width = 260;
  c.height = 130;
  c.row_length = 272;
  c.offset = 512;
  ExpectGpuUploadMatchesCpu(c);
}

TEST(VulkanEtc2GpuEmulationTest, DecodesALayeredSignedEacUploadOnTheGpu) {
  UploadCase c;
  c.format = VK_FORMAT_EAC_R11G11_SNORM_BLOCK;
  c.etc = EtcFormat::kEacRg11Signed;
  c.width = 96;
  c.height = 80;
  c.layers = 2;
  c.image_height = 88;
  c.offset = 64;
  ExpectGpuUploadMatchesCpu(c);
}

TEST(VulkanEtc2GpuEmulationTest, DecodesAPunchThroughUploadOnTheGpu) {
  UploadCase c;
  c.format = VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK;
  c.etc = EtcFormat::kEtc2Rgb8A1;
  c.width = 128;
  c.height = 128;
  ExpectGpuUploadMatchesCpu(c);
}

}
}
