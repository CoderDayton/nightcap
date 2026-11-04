#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "mocktail/graphics/bc_decoder.h"
#include "mocktail/graphics/etc2_decoder.h"
#include "mocktail/graphics/texture_override.h"
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

// A fixed upscale and no encode budget, so expectations hold on any GPU.
constexpr std::uint32_t kTestUpscale = 4;
class FixedUpscaleEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    setenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE",
           std::to_string(kTestUpscale).c_str(), 1);
    setenv("MOCKTAIL_ETC2_ENCODE_BUDGET", "0", 1);
  }
};
[[maybe_unused]] ::testing::Environment* const kFixedUpscale =
    ::testing::AddGlobalTestEnvironment(new FixedUpscaleEnvironment);

// The encode budget on for one test. It goes back off even when an
// assertion returns early.
class ScopedEncodeBudget final {
 public:
  ScopedEncodeBudget() { unsetenv("MOCKTAIL_ETC2_ENCODE_BUDGET"); }
  ~ScopedEncodeBudget() { setenv("MOCKTAIL_ETC2_ENCODE_BUDGET", "0", 1); }
  ScopedEncodeBudget(const ScopedEncodeBudget&) = delete;
  ScopedEncodeBudget& operator=(const ScopedEncodeBudget&) = delete;
};

struct UploadCase {
  VkFormat format = VK_FORMAT_UNDEFINED;
  EtcFormat etc = EtcFormat::kEtc2Rgb8;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t layers = 1;
  std::uint32_t row_length = 0;
  std::uint32_t image_height = 0;
  VkDeviceSize offset = 0;
  // Keep the image compressed as BC7/BC4/BC5.
  bool transcode = false;
  // One copy region per layer instead of one for all of them.
  bool region_per_layer = false;
  VkDeviceSize staging_block_bytes = kEtc2StagingBlockBytes;
};

// Decodes a BC readback into the layout DecodeEtcImage writes.
std::vector<std::uint8_t> DecodeBcReadback(const UploadCase& c,
                                           const std::uint8_t* blocks) {
  const BcFormat format = BcFormatFor(c.etc);
  const std::size_t texel_bytes = EtcDecodedTexelBytes(c.etc);
  const std::uint32_t blocks_wide = (c.width + 3) / 4;
  const std::uint32_t blocks_high = (c.height + 3) / 4;
  const std::size_t layer_texels = std::size_t{c.width} * c.height;
  std::vector<std::uint8_t> out(layer_texels * texel_bytes * c.layers);
  std::uint8_t texels[16 * 8];
  for (std::uint32_t layer = 0; layer < c.layers; ++layer) {
    for (std::uint32_t by = 0; by < blocks_high; ++by) {
      for (std::uint32_t bx = 0; bx < blocks_wide; ++bx) {
        const std::size_t block =
            (std::size_t{layer} * blocks_high + by) * blocks_wide + bx;
        EXPECT_TRUE(DecodeBcBlock(format, blocks + block * BcBlockBytes(format),
                                  texels));
        for (std::uint32_t t = 0; t < 16; ++t) {
          const std::uint32_t x = bx * 4 + t % 4;
          const std::uint32_t y = by * 4 + t / 4;
          if (x < c.width && y < c.height) {
            std::memcpy(out.data() + layer * layer_texels * texel_bytes +
                            (std::size_t{y} * c.width + x) * texel_bytes,
                        texels + t * texel_bytes, texel_bytes);
          }
        }
      }
    }
  }
  return out;
}

// PSNR over every channel, on the 8-bit or 16-bit scale of the format.
double Psnr(const UploadCase& c, const std::vector<std::uint8_t>& a,
            const std::vector<std::uint8_t>& b) {
  const bool wide = c.etc != EtcFormat::kEtc2Rgb8 &&
                    c.etc != EtcFormat::kEtc2Rgb8A1 &&
                    c.etc != EtcFormat::kEtc2Rgba8;
  const bool is_signed = c.etc == EtcFormat::kEacR11Signed ||
                         c.etc == EtcFormat::kEacRg11Signed;
  double sum = 0;
  std::size_t count = 0;
  for (std::size_t i = 0; i < a.size(); i += wide ? 2 : 1) {
    double x = a[i];
    double y = b[i];
    if (wide) {
      std::uint16_t u = 0;
      std::uint16_t v = 0;
      std::memcpy(&u, a.data() + i, 2);
      std::memcpy(&v, b.data() + i, 2);
      x = is_signed ? static_cast<std::int16_t>(u) : u;
      y = is_signed ? static_cast<std::int16_t>(v) : v;
    }
    sum += (x - y) * (x - y);
    ++count;
  }
  const double peak = wide ? (is_signed ? 32767.0 : 65535.0) : 255.0;
  return sum == 0 ? 99.0
                  : 10 * std::log10(peak * peak / (sum / count));
}

VulkanEtc2Emulation* Emu(VulkanEtc2Emulation& emulation) { return &emulation; }
VulkanEtc2Emulation* Emu(VulkanEtc2Emulation* emulation) { return emulation; }

// Goes through the emulation, as the app's barriers do, so a barrier on an
// upscaled image reaches its host levels.
void ImageBarrier(VulkanEtc2Emulation* emulation, const VulkanTestDevice& vk,
                  VkCommandBuffer command_buffer, VkImage image,
                  std::uint32_t layers, VkImageLayout from, VkImageLayout to,
                  VkAccessFlags source_access,
                  VkAccessFlags destination_access, std::uint32_t mip = 0) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = source_access;
  barrier.dstAccessMask = destination_access;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, layers};
  emulation->CmdPipelineBarrier(vk.device, command_buffer,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                0, nullptr, 1, &barrier);
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
    if (c.transcode && !vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation(c.staging_block_bytes);
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, c.transcode);
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
    const std::size_t blocks = std::size_t{(c.width + 3) / 4} *
                               ((c.height + 3) / 4) * c.layers;
    const std::size_t image_bytes =
        c.transcode ? blocks * BcBlockBytes(BcFormatFor(c.etc))
                    : std::size_t{c.width} * c.height *
                          EtcDecodedTexelBytes(c.etc) * c.layers;
    EXPECT_EQ(emulation.TextureBytes().decoded, image_bytes);

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
    const std::size_t readback_bytes = image_bytes;
    HostBuffer readback = CreateHostBuffer(vk, readback_bytes,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    ImageBarrier(Emu(emulation), vk, commands.buffer, image, c.layers,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    std::vector<VkBufferImageCopy> regions;
    for (std::uint32_t layer = 0; layer < c.layers;
         layer += c.region_per_layer ? 1 : c.layers) {
      VkBufferImageCopy region{};
      region.bufferOffset = c.offset + layer * layer_stride;
      region.bufferRowLength = c.row_length;
      region.bufferImageHeight = c.image_height;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer,
                                 c.region_per_layer ? 1 : c.layers};
      region.imageExtent = {c.width, c.height, 1};
      regions.push_back(region);
    }
    emulation.CmdCopyBufferToImage(
        vk.device, commands.buffer, source.buffer, image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<std::uint32_t>(regions.size()), regions.data());
    ImageBarrier(Emu(emulation), vk, commands.buffer, image, c.layers,
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
      if (c.transcode) {
        // Unweighted PSNR of random blocks measures 22.3 dB for RGBA8 and
        // 31.3 dB for signed RG11 on the snorm scale; blocks read from the
        // wrong offset or stride land far below.
        const bool color = c.etc == EtcFormat::kEtc2Rgb8 ||
                           c.etc == EtcFormat::kEtc2Rgb8A1 ||
                           c.etc == EtcFormat::kEtc2Rgba8;
        const double floor = color ? 21.0 : 30.0;
        EXPECT_GE(Psnr(c, DecodeBcReadback(
                              c, static_cast<std::uint8_t*>(readback.mapped)),
                       expected),
                  floor);
      } else {
        ASSERT_EQ(expected.size(), readback_bytes);
        EXPECT_EQ(
            std::memcmp(readback.mapped, expected.data(), readback_bytes), 0);
      }
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

TEST(VulkanEtc2GpuEmulationTest, KeepsAPaddedRgbaUploadCompressed) {
  UploadCase c;
  c.format = VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
  c.etc = EtcFormat::kEtc2Rgba8;
  c.width = 260;
  c.height = 130;
  c.row_length = 272;
  c.offset = 512;
  c.transcode = true;
  ExpectGpuUploadMatchesCpu(c);
}

TEST(VulkanEtc2GpuEmulationTest, KeepsALayeredSignedEacUploadCompressed) {
  UploadCase c;
  c.format = VK_FORMAT_EAC_R11G11_SNORM_BLOCK;
  c.etc = EtcFormat::kEacRg11Signed;
  c.width = 96;
  c.height = 80;
  c.layers = 2;
  c.image_height = 88;
  c.offset = 64;
  c.transcode = true;
  ExpectGpuUploadMatchesCpu(c);
}

TEST(VulkanEtc2GpuEmulationTest, KeepsAnR11UploadCompressed) {
  UploadCase c;
  c.format = VK_FORMAT_EAC_R11_UNORM_BLOCK;
  c.etc = EtcFormat::kEacR11;
  c.width = 100;
  c.height = 68;
  c.transcode = true;
  ExpectGpuUploadMatchesCpu(c);
}

// Staging for one GPU upload is capped at eight staging blocks, 512 KiB
// here. Each 512x512 RGB layer stages 384 KiB, so the four take four ranges.
TEST(VulkanEtc2GpuEmulationTest, SplitsACompressedUploadOverTheStagingCap) {
  UploadCase c;
  c.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  c.etc = EtcFormat::kEtc2Rgb8;
  c.width = 512;
  c.height = 512;
  c.layers = 4;
  c.region_per_layer = true;
  c.staging_block_bytes = 64 * 1024;
  c.transcode = true;
  ExpectGpuUploadMatchesCpu(c);
}

// One region of two 520x260 layers stages about 450 KiB per layer against a
// 128 KiB cap, so each layer is split into bands of block rows.
TEST(VulkanEtc2GpuEmulationTest, SplitsOneCompressedRegionOverTheStagingCap) {
  UploadCase c;
  c.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  c.etc = EtcFormat::kEtc2Rgb8;
  c.width = 520;
  c.height = 260;
  c.layers = 2;
  c.row_length = 528;
  c.offset = 256;
  c.staging_block_bytes = 16 * 1024;
  c.transcode = true;
  ExpectGpuUploadMatchesCpu(c);
}

struct TestImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
};

TestImage CreateTestImage(VulkanEtc2Emulation* emulation,
                          const VulkanTestDevice& vk, VkFormat format,
                          std::uint32_t size, std::uint32_t mips,
                          VkImageUsageFlags extra_usage = 0) {
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {size, size, 1};
  image_info.mipLevels = mips;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT | extra_usage;
  TestImage out;
  EXPECT_EQ(emulation->CreateImage(vk.device, &image_info, nullptr, &out.image),
            VK_SUCCESS);
  VkMemoryRequirements requirements{};
  vk.Device<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
      vk.device, out.image, &requirements);
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = FindMemoryType(vk, requirements.memoryTypeBits, 0);
  EXPECT_EQ(vk.Device<PFN_vkAllocateMemory>("vkAllocateMemory")(
                vk.device, &allocate, nullptr, &out.memory),
            VK_SUCCESS);
  vk.Device<PFN_vkBindImageMemory>("vkBindImageMemory")(vk.device, out.image,
                                                        out.memory, 0);
  return out;
}

void DestroyTestImage(VulkanEtc2Emulation* emulation,
                      const VulkanTestDevice& vk, const TestImage& image) {
  emulation->DestroyImage(vk.device, image.image, nullptr);
  vk.Device<PFN_vkFreeMemory>("vkFreeMemory")(vk.device, image.memory,
                                              nullptr);
}

// Random ETC2 RGB blocks for a size x size level, in a buffer the emulation
// tracks so it can read them back at submit.
HostBuffer CreateRandomEtcSource(VulkanEtc2Emulation* emulation,
                                 const VulkanTestDevice& vk,
                                 std::uint32_t size) {
  const VkDeviceSize bytes = VkDeviceSize{size / 4} * (size / 4) * 8;
  HostBuffer source =
      CreateHostBuffer(vk, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  EXPECT_EQ(emulation->BindBufferMemory(vk.device, source.buffer,
                                        source.memory, 0),
            VK_SUCCESS);
  EXPECT_EQ(emulation->MapMemory(vk.device, source.memory, 0, VK_WHOLE_SIZE, 0,
                                 &source.mapped),
            VK_SUCCESS);
  std::mt19937_64 random(size);
  auto* out = static_cast<std::uint8_t*>(source.mapped);
  for (VkDeviceSize i = 0; i < bytes; ++i) {
    out[i] = static_cast<std::uint8_t>(random());
  }
  return source;
}

void DestroyEtcSource(VulkanEtc2Emulation* emulation,
                      const VulkanTestDevice& vk, const HostBuffer& source) {
  emulation->UnmapMemory(vk.device, source.memory);
  emulation->DestroyBuffer(vk.device, source.buffer, nullptr);
  emulation->FreeMemory(vk.device, source.memory, nullptr);
}

void UploadLevel(VulkanEtc2Emulation* emulation, const VulkanTestDevice& vk,
                 VkCommandBuffer command_buffer, VkBuffer source,
                 VkImage image, std::uint32_t size) {
  ImageBarrier(Emu(emulation), vk, command_buffer, image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {size, size, 1};
  emulation->CmdCopyBufferToImage(vk.device, command_buffer, source, image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                  &region);
  ImageBarrier(Emu(emulation), vk, command_buffer, image, 1,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
}

bool SubmitThroughEmulation(VulkanEtc2Emulation* emulation,
                            const VulkanTestDevice& vk,
                            VkCommandBuffer command_buffer) {
  const Etc2SubmitWait wait =
      emulation->PrepareSubmit(&command_buffer, 1, false, vk.queue);
  std::vector<VkCommandBuffer> submitted = wait.decode_commands;
  submitted.push_back(command_buffer);
  return SubmitAndWait(vk, submitted);
}

// Roblox streams a texture into a small placeholder first and later copies
// it into the tail mips of the full-size image. The placeholder is upscaled
// RGBA8 and the full image is BC7, so the copy has to be transcoded.
TEST(VulkanEtc2GpuEmulationTest, CopiesAnUpscaledImageIntoACompressedOne) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const TestImage placeholder = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const TestImage full = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, 3);
    const HostBuffer source = CreateRandomEtcSource(&emulation, vk, 32);
    HostBuffer readback = CreateHostBuffer(vk, 64 * 16,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands upload = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, upload.buffer, source.buffer,
                placeholder.image, 32);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(upload.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, upload.buffer));

    TestCommands copy = BeginTestCommands(vk);
    ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
    region.extent = {32, 32, 1};
    emulation.CmdCopyImage(vk.device, copy.buffer, placeholder.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, full.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 2);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
    back.imageExtent = {32, 32, 1};
    vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        copy.buffer, full.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.buffer, 1, &back);
    HostReadBarrier(vk, copy.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(copy.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, copy.buffer));

    UploadCase c;
    c.etc = EtcFormat::kEtc2Rgb8;
    c.width = 32;
    c.height = 32;
    EXPECT_GE(Psnr(c,
                   DecodeBcReadback(
                       c, static_cast<std::uint8_t*>(readback.mapped)),
                   DecodeOnCpu(c, static_cast<std::uint8_t*>(source.mapped))),
              21.0);

    emulation.ReleaseCommandBuffer(upload.buffer);
    emulation.ReleaseCommandBuffer(copy.buffer);
    DestroyTestCommands(vk, upload);
    DestroyTestCommands(vk, copy);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, placeholder);
    DestroyTestImage(&emulation, vk, full);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// The copy reads the placeholder's blocks as of its place in the command
// buffer: a re-upload recorded just before it, not the earlier upload.
TEST(VulkanEtc2GpuEmulationTest, CopiesAnUpscaledImageUploadedInTheSameCommands) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const TestImage placeholder = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const TestImage full = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, 3);
    const HostBuffer first = CreateRandomEtcSource(&emulation, vk, 32);
    const HostBuffer second = CreateRandomEtcSource(&emulation, vk, 32);
    auto* bytes = static_cast<std::uint8_t*>(second.mapped);
    for (std::size_t i = 0; i < 8 * 8 * 8; ++i) {
      bytes[i] = static_cast<std::uint8_t>(bytes[i] * 37 + 11);
    }
    HostBuffer readback = CreateHostBuffer(vk, 64 * 16,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands upload = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, upload.buffer, first.buffer,
                placeholder.image, 32);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(upload.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, upload.buffer));

    TestCommands copy = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, copy.buffer, second.buffer, placeholder.image,
                32);
    ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
    region.extent = {32, 32, 1};
    emulation.CmdCopyImage(vk.device, copy.buffer, placeholder.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, full.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 2);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
    back.imageExtent = {32, 32, 1};
    vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        copy.buffer, full.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.buffer, 1, &back);
    HostReadBarrier(vk, copy.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(copy.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, copy.buffer));

    UploadCase c;
    c.etc = EtcFormat::kEtc2Rgb8;
    c.width = 32;
    c.height = 32;
    const std::vector<std::uint8_t> got = DecodeBcReadback(
        c, static_cast<std::uint8_t*>(readback.mapped));
    EXPECT_GE(Psnr(c, got,
                   DecodeOnCpu(c, static_cast<std::uint8_t*>(second.mapped))),
              21.0);

    emulation.ReleaseCommandBuffer(upload.buffer);
    emulation.ReleaseCommandBuffer(copy.buffer);
    DestroyTestCommands(vk, upload);
    DestroyTestCommands(vk, copy);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, first);
    DestroyEtcSource(&emulation, vk, second);
    DestroyTestImage(&emulation, vk, placeholder);
    DestroyTestImage(&emulation, vk, full);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A BC7 image copied into one kept as RGBA8 (here a colour attachment) is
// decompressed by a blit, since the two texel blocks differ in size.
TEST(VulkanEtc2GpuEmulationTest, CopiesACompressedImageIntoADecodedOne) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const TestImage compressed = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, 1);
    const TestImage decoded =
        CreateTestImage(&emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128,
                        1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    const VkDeviceSize compressed_bytes = 32 * 32 * 16;
    ASSERT_EQ(emulation.TextureBytes().decoded,
              compressed_bytes + 128 * 128 * 4);
    const HostBuffer source = CreateRandomEtcSource(&emulation, vk, 128);
    HostBuffer readback = CreateHostBuffer(vk, 128 * 128 * 4,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, commands.buffer, source.buffer,
                compressed.image, 128);
    ImageBarrier(Emu(emulation), vk, commands.buffer, decoded.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {128, 128, 1};
    emulation.CmdCopyImage(vk.device, commands.buffer, compressed.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, decoded.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, commands.buffer, decoded.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.imageExtent = {128, 128, 1};
    vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        commands.buffer, decoded.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.buffer, 1, &back);
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, commands.buffer));

    UploadCase c;
    c.etc = EtcFormat::kEtc2Rgb8;
    c.width = 128;
    c.height = 128;
    const auto* texels = static_cast<std::uint8_t*>(readback.mapped);
    EXPECT_GE(Psnr(c, std::vector<std::uint8_t>(texels, texels + 128 * 128 * 4),
                   DecodeOnCpu(c, static_cast<std::uint8_t*>(source.mapped))),
              21.0);

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, compressed);
    DestroyTestImage(&emulation, vk, decoded);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

PFN_vkGetDeviceProcAddr g_real_get_device_proc_addr = nullptr;
PFN_vkCreateImage g_real_create_image = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL RejectBcCreateImage(
    VkDevice device, const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkImage* image) {
  if (info->format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK &&
      info->format <= VK_FORMAT_BC7_SRGB_BLOCK) {
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  return g_real_create_image(device, info, allocator, image);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
GetDeviceProcAddrRejectingBc(VkDevice device, const char* name) {
  if (std::strcmp(name, "vkCreateImage") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&RejectBcCreateImage);
  }
  return g_real_get_device_proc_addr(device, name);
}

// A driver that cannot create the BC image still gets a usable texture.
TEST(VulkanEtc2GpuEmulationTest, KeepsTheImageDecodedWhenBcCreationFails) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    g_real_get_device_proc_addr = vk.get_device_proc_addr;
    g_real_create_image = vk.Device<PFN_vkCreateImage>("vkCreateImage");
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             &GetDeviceProcAddrRejectingBc, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    image_info.extent = {128, 128, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image = VK_NULL_HANDLE;
    ASSERT_EQ(emulation.CreateImage(vk.device, &image_info, nullptr, &image),
              VK_SUCCESS);
    EXPECT_EQ(emulation.TextureBytes().decoded, VkDeviceSize{128} * 128 * 4);
    emulation.DestroyImage(vk.device, image, nullptr);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A small texture is upscaled on the CPU and then encoded as BC7 on the GPU.
TEST(VulkanEtc2GpuEmulationTest, UpscalesASmallUploadIntoBc7) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const std::uint32_t host = 32 * kTestUpscale;
    const TestImage image = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 2);
    // Two top levels, 128 and 64, above the image's own 32 and 16.
    const VkDeviceSize level0 = VkDeviceSize{host / 4} * (host / 4) * 16;
    EXPECT_EQ(emulation.TextureBytes().decoded,
              level0 + level0 / 4 + level0 / 16 + level0 / 64);
    const HostBuffer source = CreateRandomEtcSource(&emulation, vk, 32);
    HostBuffer readback =
        CreateHostBuffer(vk, level0, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, commands.buffer, source.buffer, image.image,
                32);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.imageExtent = {host, host, 1};
    vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        commands.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.buffer, 1, &back);
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, commands.buffer));

    UploadCase small;
    small.etc = EtcFormat::kEtc2Rgb8;
    small.width = 32;
    small.height = 32;
    const std::vector<std::uint8_t> decoded =
        DecodeOnCpu(small, static_cast<std::uint8_t*>(source.mapped));
    std::vector<std::uint8_t> expected(std::size_t{host} * host * 4);
    UpscaleRgba(decoded.data(), 32, 32, 32 * 4, host, host, expected.data());
    UploadCase large = small;
    large.width = host;
    large.height = host;
    // BC7 of the upscaled random blocks measures 33.6 dB; a nearest-scaled
    // upload measures 21.2 dB.
    EXPECT_GE(Psnr(large,
                   DecodeBcReadback(
                       large, static_cast<std::uint8_t*>(readback.mapped)),
                   expected),
              30.0);

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, image);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Uploads `source` (at least one block) as `size` x `size` texels of `mip`,
// then leaves that mip readable as a transfer source.
void UploadMip(VulkanEtc2Emulation* emulation, const VulkanTestDevice& vk,
               VkCommandBuffer command_buffer, VkBuffer source, VkImage image,
               std::uint32_t mip, std::uint32_t size) {
  ImageBarrier(Emu(emulation), vk, command_buffer, image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT, mip);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
  region.imageExtent = {size, size, 1};
  emulation->CmdCopyBufferToImage(vk.device, command_buffer, source, image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                  &region);
  ImageBarrier(Emu(emulation), vk, command_buffer, image, 1,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, mip);
}

VkDeviceSize Bc7MipBytes(std::uint32_t size) {
  const VkDeviceSize blocks = (std::max<std::uint32_t>(1, size) + 3) / 4;
  return blocks * blocks * 16;
}

// Queues a readback of `size` x `size` host texels of a BC7 `mip`.
void ReadMipBc7(const VulkanTestDevice& vk, VkCommandBuffer command_buffer,
                VkImage image, std::uint32_t mip, std::uint32_t size,
                VkBuffer readback, VkDeviceSize offset) {
  VkBufferImageCopy back{};
  back.bufferOffset = offset;
  back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
  back.imageExtent = {size, size, 1};
  vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
      command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
      &back);
}

// Roblox moves a streamed texture through placeholders before the full
// BC image. With little encode budget left a placeholder stays RGBA8 (here a
// colour attachment), so each hop may change representation; the full image
// must still get the texels rather than black.
// A placeholder above the small-texture limit keeps no blocks, so its texels
// cannot reach the full image (`reaches` false); the log then names it.
void ExpectPlaceholderHopReachesBc(bool first_decoded, bool second_decoded,
                                   std::uint32_t size = 32,
                                   bool reaches = true) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const auto usage = [](bool decoded) {
      return decoded ? VkImageUsageFlags{VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT}
                     : VkImageUsageFlags{0};
    };
    const TestImage first =
        CreateTestImage(&emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
                        size, 1, usage(first_decoded));
    const TestImage second =
        CreateTestImage(&emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
                        size, 1, usage(second_decoded));
    const TestImage full = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, size * 4, 3);
    const HostBuffer source = CreateRandomEtcSource(&emulation, vk, size);
    HostBuffer readback = CreateHostBuffer(vk, Bc7MipBytes(size),
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands upload = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, upload.buffer, source.buffer, first.image,
                size);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(upload.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, upload.buffer));

    TestCommands copy = BeginTestCommands(vk);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {size, size, 1};
    ImageBarrier(Emu(emulation), vk, copy.buffer, second.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    emulation.CmdCopyImage(vk.device, copy.buffer, first.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, second.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, copy.buffer, second.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    region.dstSubresource.mipLevel = 2;
    emulation.CmdCopyImage(vk.device, copy.buffer, second.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, full.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 2);
    ReadMipBc7(vk, copy.buffer, full.image, 2, size, readback.buffer, 0);
    HostReadBarrier(vk, copy.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(copy.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, copy.buffer));

    UploadCase c;
    c.etc = EtcFormat::kEtc2Rgb8;
    c.width = size;
    c.height = size;
    if (reaches) {
      EXPECT_GE(
          Psnr(c,
               DecodeBcReadback(c,
                                static_cast<std::uint8_t*>(readback.mapped)),
               DecodeOnCpu(c, static_cast<std::uint8_t*>(source.mapped))),
          21.0);
    }

    emulation.ReleaseCommandBuffer(upload.buffer);
    emulation.ReleaseCommandBuffer(copy.buffer);
    DestroyTestCommands(vk, upload);
    DestroyTestCommands(vk, copy);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, first);
    DestroyTestImage(&emulation, vk, second);
    DestroyTestImage(&emulation, vk, full);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

TEST(VulkanEtc2GpuEmulationTest, DecodedPlaceholderHopsReachBc) {
  ExpectPlaceholderHopReachesBc(true, true);
}

TEST(VulkanEtc2GpuEmulationTest, DecodedToBcPlaceholderHopReachesBc) {
  ExpectPlaceholderHopReachesBc(true, false);
}

TEST(VulkanEtc2GpuEmulationTest, BcToDecodedPlaceholderHopReachesBc) {
  ExpectPlaceholderHopReachesBc(false, true);
}

TEST(VulkanEtc2GpuEmulationTest, BcPlaceholderHopsReachBc) {
  ExpectPlaceholderHopReachesBc(false, false);
}

TEST(VulkanEtc2GpuEmulationTest, LogsTheSourceOfACopyWithoutBlocks) {
  ::testing::internal::CaptureStderr();
  ExpectPlaceholderHopReachesBc(false, true, 128, false);
  const std::string log = ::testing::internal::GetCapturedStderr();
  // Devices without BC textures never copy into a compressed image.
  if (log.find("stay compressed as BC7") != std::string::npos) {
    EXPECT_NE(log.find("without its source: 128x128 RGBA8 mip 0, no kept blocks"),
              std::string::npos)
        << log;
  }
}

// Every mip of a full chain, down to 1x1, holds its own texels.
TEST(VulkanEtc2GpuEmulationTest, KeepsEveryMipOfAFullChainInBc7) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    constexpr std::uint32_t kSize = 128;
    constexpr std::uint32_t kMips = 8;
    const TestImage image = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSize, kMips);
    std::vector<HostBuffer> sources;
    std::vector<VkDeviceSize> offsets;
    VkDeviceSize total = 0;
    for (std::uint32_t mip = 0; mip < kMips; ++mip) {
      sources.push_back(CreateRandomEtcSource(
          &emulation, vk, std::max<std::uint32_t>(4, kSize >> mip)));
      offsets.push_back(total);
      total += Bc7MipBytes(kSize >> mip);
    }
    HostBuffer readback =
        CreateHostBuffer(vk, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    for (std::uint32_t mip = 0; mip < kMips; ++mip) {
      UploadMip(&emulation, vk, commands.buffer, sources[mip].buffer,
                image.image, mip, kSize >> mip);
    }
    for (std::uint32_t mip = 0; mip < kMips; ++mip) {
      ReadMipBc7(vk, commands.buffer, image.image, mip, kSize >> mip,
                 readback.buffer, offsets[mip]);
    }
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, commands.buffer));

    for (std::uint32_t mip = 0; mip < kMips; ++mip) {
      SCOPED_TRACE(mip);
      UploadCase c;
      c.etc = EtcFormat::kEtc2Rgb8;
      c.width = kSize >> mip;
      c.height = kSize >> mip;
      EXPECT_GE(Psnr(c,
                     DecodeBcReadback(c, static_cast<std::uint8_t*>(
                                             readback.mapped) +
                                             offsets[mip]),
                     DecodeOnCpu(c, static_cast<std::uint8_t*>(
                                        sources[mip].mapped))),
                21.0);
    }

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    for (const HostBuffer& source : sources) {
      DestroyEtcSource(&emulation, vk, source);
    }
    DestroyTestImage(&emulation, vk, image);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Every mip of non-square and layered chains in each ETC2/EAC format keeps
// its texels in BC: the mips a far draw samples must not come back black.
TEST(VulkanEtc2GpuEmulationTest, KeepsEveryMipOfEachFormatAndShapeInBc) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  struct Format {
    VkFormat vk;
    EtcFormat etc;
  };
  struct Shape {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t layers;
  };
  const Format formats[] = {
      {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, EtcFormat::kEtc2Rgba8},
      {VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, EtcFormat::kEtc2Rgb8},
      {VK_FORMAT_EAC_R11_UNORM_BLOCK, EtcFormat::kEacR11},
      {VK_FORMAT_EAC_R11G11_UNORM_BLOCK, EtcFormat::kEacRg11},
  };
  const Shape shapes[] = {{256, 256, 1}, {256, 64, 1}, {64, 256, 1},
                          {128, 128, 2}};
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    for (const Format& format : formats) {
      for (const Shape& shape : shapes) {
        SCOPED_TRACE(::testing::Message()
                     << "format " << format.vk << " " << shape.width << "x"
                     << shape.height << "x" << shape.layers);
        VulkanEtc2Emulation emulation;
        emulation.RegisterDevice(vk.device, vk.physical_device, true,
                                 vk.memory, vk.get_device_proc_addr, false,
                                 true);
        emulation.RegisterQueue(vk.device, vk.queue, vk.family);
        ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
        std::uint32_t mips = 1;
        while ((std::max(shape.width, shape.height) >> mips) != 0) {
          ++mips;
        }
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format.vk;
        info.extent = {shape.width, shape.height, 1};
        info.mipLevels = mips;
        info.arrayLayers = shape.layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT;
        VkImage image = VK_NULL_HANDLE;
        ASSERT_EQ(emulation.CreateImage(vk.device, &info, nullptr, &image),
                  VK_SUCCESS);
        VkMemoryRequirements requirements{};
        vk.Device<PFN_vkGetImageMemoryRequirements>(
            "vkGetImageMemoryRequirements")(vk.device, image, &requirements);
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex =
            FindMemoryType(vk, requirements.memoryTypeBits, 0);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        ASSERT_EQ(vk.Device<PFN_vkAllocateMemory>("vkAllocateMemory")(
                      vk.device, &allocate, nullptr, &memory),
                  VK_SUCCESS);
        vk.Device<PFN_vkBindImageMemory>("vkBindImageMemory")(
            vk.device, image, memory, 0);

        const VkDeviceSize etc_block = EtcBlockBytes(format.etc);
        const VkDeviceSize bc_block = BcBlockBytes(BcFormatFor(format.etc));
        std::vector<UploadCase> cases;
        std::vector<VkDeviceSize> source_offsets;
        std::vector<VkDeviceSize> readback_offsets;
        VkDeviceSize source_total = 0;
        VkDeviceSize readback_total = 0;
        for (std::uint32_t mip = 0; mip < mips; ++mip) {
          UploadCase c;
          c.etc = format.etc;
          c.width = std::max<std::uint32_t>(1, shape.width >> mip);
          c.height = std::max<std::uint32_t>(1, shape.height >> mip);
          c.layers = shape.layers;
          const VkDeviceSize blocks = VkDeviceSize{(c.width + 3) / 4} *
                                      ((c.height + 3) / 4) * shape.layers;
          cases.push_back(c);
          source_offsets.push_back(source_total);
          readback_offsets.push_back(readback_total);
          source_total += (blocks * etc_block + 255) / 256 * 256;
          readback_total += (blocks * bc_block + 255) / 256 * 256;
        }
        HostBuffer source = CreateHostBuffer(vk, source_total,
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        ASSERT_EQ(emulation.BindBufferMemory(vk.device, source.buffer,
                                             source.memory, 0),
                  VK_SUCCESS);
        ASSERT_EQ(emulation.MapMemory(vk.device, source.memory, 0,
                                      VK_WHOLE_SIZE, 0, &source.mapped),
                  VK_SUCCESS);
        std::mt19937_64 random(shape.width * 131 + shape.height);
        auto* bytes = static_cast<std::uint8_t*>(source.mapped);
        for (VkDeviceSize i = 0; i < source_total; ++i) {
          bytes[i] = static_cast<std::uint8_t>(random());
        }
        HostBuffer readback = CreateHostBuffer(
            vk, readback_total, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        BindAndMap(vk, &readback);

        TestCommands commands = BeginTestCommands(vk);
        for (std::uint32_t mip = 0; mip < mips; ++mip) {
          ImageBarrier(Emu(emulation), vk, commands.buffer, image, shape.layers,
                       VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, mip);
          VkBufferImageCopy region{};
          region.bufferOffset = source_offsets[mip];
          region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0,
                                     shape.layers};
          region.imageExtent = {cases[mip].width, cases[mip].height, 1};
          emulation.CmdCopyBufferToImage(vk.device, commands.buffer,
                                         source.buffer, image,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         1, &region);
          ImageBarrier(Emu(emulation), vk, commands.buffer, image, shape.layers,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT, mip);
        }
        for (std::uint32_t mip = 0; mip < mips; ++mip) {
          VkBufferImageCopy back{};
          back.bufferOffset = readback_offsets[mip];
          back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0,
                                   shape.layers};
          back.imageExtent = {cases[mip].width, cases[mip].height, 1};
          vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
              commands.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              readback.buffer, 1, &back);
        }
        HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
        vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(
            commands.buffer);
        ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, commands.buffer));

        for (std::uint32_t mip = 0; mip < mips; ++mip) {
          SCOPED_TRACE(::testing::Message() << "mip " << mip);
          UploadCase c = cases[mip];
          c.offset = source_offsets[mip];
          EXPECT_GE(Psnr(c,
                         DecodeBcReadback(
                             c, static_cast<std::uint8_t*>(readback.mapped) +
                                    readback_offsets[mip]),
                         DecodeOnCpu(c, bytes)),
                    12.0);
        }

        emulation.ReleaseCommandBuffer(commands.buffer);
        DestroyTestCommands(vk, commands);
        DestroyHostBuffer(vk, readback);
        DestroyEtcSource(&emulation, vk, source);
        emulation.DestroyImage(vk.device, image, nullptr);
        vk.Device<PFN_vkFreeMemory>("vkFreeMemory")(vk.device, memory,
                                                    nullptr);
        emulation.DestroyDevice(vk.device);
      }
    }
  }
  CloseTestDevices(&devices);
}

// A mip uploaded before level 0 keeps its own blocks, at its own size below
// the two top levels.
TEST(VulkanEtc2GpuEmulationTest, EncodesAMipUploadedBeforeLevelZeroIntoBc7) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const TestImage image = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 2);
    const HostBuffer level1 = CreateRandomEtcSource(&emulation, vk, 16);
    HostBuffer readback = CreateHostBuffer(vk, Bc7MipBytes(16),
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    UploadMip(&emulation, vk, commands.buffer, level1.buffer, image.image, 1,
              16);
    ReadMipBc7(vk, commands.buffer, image.image, 3, 16, readback.buffer, 0);
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, commands.buffer));

    UploadCase small;
    small.etc = EtcFormat::kEtc2Rgb8;
    small.width = 16;
    small.height = 16;
    const std::vector<std::uint8_t> decoded =
        DecodeOnCpu(small, static_cast<std::uint8_t*>(level1.mapped));
    // As KeepsAChainUploadedInOneCopyInBc7: random blocks in BC7.
    EXPECT_GE(Psnr(small,
                   DecodeBcReadback(
                       small, static_cast<std::uint8_t*>(readback.mapped)),
                   decoded),
              21.0);

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, level1);
    DestroyTestImage(&emulation, vk, image);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Roblox streams a texture into a small placeholder with its whole mip chain,
// then copies those mips into the tail of the full-size image: the mips used
// at a distance. Every one of them has to arrive.
TEST(VulkanEtc2GpuEmulationTest, CopiesAPlaceholderChainIntoTheTailMips) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    constexpr std::uint32_t kPlaceholder = 32;
    constexpr std::uint32_t kPlaceholderMips = 6;
    constexpr std::uint32_t kFull = 128;
    constexpr std::uint32_t kFullMips = 8;
    constexpr std::uint32_t kTail = kFullMips - kPlaceholderMips;
    const TestImage placeholder =
        CreateTestImage(&emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
                        kPlaceholder, kPlaceholderMips);
    const TestImage full = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kFull, kFullMips);
    std::vector<HostBuffer> sources;
    std::vector<VkDeviceSize> offsets;
    VkDeviceSize total = 0;
    for (std::uint32_t mip = 0; mip < kPlaceholderMips; ++mip) {
      sources.push_back(CreateRandomEtcSource(
          &emulation, vk, std::max<std::uint32_t>(4, kPlaceholder >> mip)));
      offsets.push_back(total);
      total += Bc7MipBytes(kPlaceholder >> mip);
    }
    HostBuffer readback =
        CreateHostBuffer(vk, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands upload = BeginTestCommands(vk);
    for (std::uint32_t mip = 0; mip < kPlaceholderMips; ++mip) {
      UploadMip(&emulation, vk, upload.buffer, sources[mip].buffer,
                placeholder.image, mip, kPlaceholder >> mip);
    }
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(upload.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, upload.buffer));

    TestCommands copy = BeginTestCommands(vk);
    for (std::uint32_t mip = 0; mip < kPlaceholderMips; ++mip) {
      const std::uint32_t size = kPlaceholder >> mip;
      ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, mip + kTail);
      VkImageCopy region{};
      region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
      region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip + kTail, 0, 1};
      region.extent = {size, size, 1};
      emulation.CmdCopyImage(vk.device, copy.buffer, placeholder.image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, full.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   mip + kTail);
      ReadMipBc7(vk, copy.buffer, full.image, mip + kTail, size,
                 readback.buffer, offsets[mip]);
    }
    HostReadBarrier(vk, copy.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(copy.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, copy.buffer));

    for (std::uint32_t mip = 0; mip < kPlaceholderMips; ++mip) {
      SCOPED_TRACE(mip + kTail);
      UploadCase c;
      c.etc = EtcFormat::kEtc2Rgb8;
      c.width = kPlaceholder >> mip;
      c.height = kPlaceholder >> mip;
      EXPECT_GE(Psnr(c,
                     DecodeBcReadback(c, static_cast<std::uint8_t*>(
                                             readback.mapped) +
                                             offsets[mip]),
                     DecodeOnCpu(c, static_cast<std::uint8_t*>(
                                        sources[mip].mapped))),
                21.0);
    }

    emulation.ReleaseCommandBuffer(upload.buffer);
    emulation.ReleaseCommandBuffer(copy.buffer);
    DestroyTestCommands(vk, upload);
    DestroyTestCommands(vk, copy);
    DestroyHostBuffer(vk, readback);
    for (const HostBuffer& source : sources) {
      DestroyEtcSource(&emulation, vk, source);
    }
    DestroyTestImage(&emulation, vk, placeholder);
    DestroyTestImage(&emulation, vk, full);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Random ETC2 RGB blocks for a whole mip chain packed into one buffer, with
// one copy region per mip. `offsets` receives each mip's byte offset.
HostBuffer CreateRandomEtcChain(VulkanEtc2Emulation* emulation,
                                const VulkanTestDevice& vk,
                                std::uint32_t size, std::uint32_t mips,
                                std::vector<VkBufferImageCopy>* regions,
                                std::vector<VkDeviceSize>* offsets) {
  VkDeviceSize bytes = 0;
  for (std::uint32_t mip = 0; mip < mips; ++mip) {
    const std::uint32_t extent = std::max<std::uint32_t>(1, size >> mip);
    const VkDeviceSize blocks = (extent + 3) / 4;
    VkBufferImageCopy region{};
    region.bufferOffset = bytes;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    region.imageExtent = {extent, extent, 1};
    regions->push_back(region);
    offsets->push_back(bytes);
    bytes += blocks * blocks * 8;
  }
  HostBuffer source =
      CreateHostBuffer(vk, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  EXPECT_EQ(emulation->BindBufferMemory(vk.device, source.buffer,
                                        source.memory, 0),
            VK_SUCCESS);
  EXPECT_EQ(emulation->MapMemory(vk.device, source.memory, 0, VK_WHOLE_SIZE, 0,
                                 &source.mapped),
            VK_SUCCESS);
  std::mt19937_64 random(size * 7 + mips);
  auto* out = static_cast<std::uint8_t*>(source.mapped);
  for (VkDeviceSize i = 0; i < bytes; ++i) {
    out[i] = static_cast<std::uint8_t>(random());
  }
  return source;
}

// Uploads a whole chain in one copy and reads every host level back as BC7:
// the `top` levels above the chain, then the chain. The host size of a level
// is `host_size >> level`.
void UploadChainInOneCopy(VulkanEtc2Emulation* emulation,
                          const VulkanTestDevice& vk, VkImage image,
                          const HostBuffer& source,
                          const std::vector<VkBufferImageCopy>& regions,
                          std::uint32_t host_size, HostBuffer* readback,
                          std::vector<VkDeviceSize>* readback_offsets,
                          std::uint32_t top = 0) {
  TestCommands commands = BeginTestCommands(vk);
  for (const VkBufferImageCopy& region : regions) {
    ImageBarrier(Emu(emulation), vk, commands.buffer, image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, region.imageSubresource.mipLevel);
  }
  emulation->CmdCopyBufferToImage(
      vk.device, commands.buffer, source.buffer, image,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      static_cast<std::uint32_t>(regions.size()), regions.data());
  for (const VkBufferImageCopy& region : regions) {
    ImageBarrier(Emu(emulation), vk, commands.buffer, image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 region.imageSubresource.mipLevel);
  }
  VkDeviceSize offset = 0;
  const auto levels = static_cast<std::uint32_t>(regions.size()) + top;
  for (std::uint32_t level = 0; level < levels; ++level) {
    ReadMipBc7(vk, commands.buffer, image, level,
               std::max<std::uint32_t>(1, host_size >> level),
               readback->buffer, offset);
    readback_offsets->push_back(offset);
    offset += Bc7MipBytes(host_size >> level);
  }
  HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT);
  vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
  EXPECT_TRUE(SubmitThroughEmulation(emulation, vk, commands.buffer));
  emulation->ReleaseCommandBuffer(commands.buffer);
  DestroyTestCommands(vk, commands);
}

VkDeviceSize ChainReadbackBytes(std::uint32_t host_size, std::uint32_t mips) {
  VkDeviceSize total = 0;
  for (std::uint32_t mip = 0; mip < mips; ++mip) {
    total += Bc7MipBytes(host_size >> mip);
  }
  return total;
}

// Roblox uploads a texture's mips as the regions of one copy.
TEST(VulkanEtc2GpuEmulationTest, KeepsAChainUploadedInOneCopyInBc7) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    constexpr std::uint32_t kSize = 128;
    constexpr std::uint32_t kMips = 8;
    const TestImage image = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSize, kMips);
    std::vector<VkBufferImageCopy> regions;
    std::vector<VkDeviceSize> source_offsets;
    const HostBuffer source = CreateRandomEtcChain(
        &emulation, vk, kSize, kMips, &regions, &source_offsets);
    HostBuffer readback = CreateHostBuffer(vk, ChainReadbackBytes(kSize, kMips),
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);
    std::vector<VkDeviceSize> offsets;
    UploadChainInOneCopy(&emulation, vk, image.image, source, regions, kSize,
                         &readback, &offsets);

    for (std::uint32_t mip = 0; mip < kMips; ++mip) {
      SCOPED_TRACE(mip);
      UploadCase c;
      c.etc = EtcFormat::kEtc2Rgb8;
      c.width = kSize >> mip;
      c.height = kSize >> mip;
      c.offset = source_offsets[mip];
      EXPECT_GE(Psnr(c,
                     DecodeBcReadback(c, static_cast<std::uint8_t*>(
                                             readback.mapped) +
                                             offsets[mip]),
                     DecodeOnCpu(c, static_cast<std::uint8_t*>(source.mapped))),
                21.0);
    }

    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, image);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A chain uploaded in one copy keeps every level's own texels, below two
// top levels scaled from level 0.
TEST(VulkanEtc2GpuEmulationTest, UpscalesAChainUploadedInOneCopyIntoBc7) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    constexpr std::uint32_t kSize = 32;
    constexpr std::uint32_t kMips = 6;
    const std::uint32_t host = kSize * kTestUpscale;
    const TestImage image = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSize, kMips);
    std::vector<VkBufferImageCopy> regions;
    std::vector<VkDeviceSize> source_offsets;
    const HostBuffer source = CreateRandomEtcChain(
        &emulation, vk, kSize, kMips, &regions, &source_offsets);
    constexpr std::uint32_t kTop = 2;
    HostBuffer readback =
        CreateHostBuffer(vk, ChainReadbackBytes(host, kMips + kTop),
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);
    std::vector<VkDeviceSize> offsets;
    UploadChainInOneCopy(&emulation, vk, image.image, source, regions, host,
                         &readback, &offsets, kTop);

    UploadCase level0;
    level0.etc = EtcFormat::kEtc2Rgb8;
    level0.width = kSize;
    level0.height = kSize;
    const std::vector<std::uint8_t> decoded =
        DecodeOnCpu(level0, static_cast<std::uint8_t*>(source.mapped));
    for (std::uint32_t level = 0; level < kTop; ++level) {
      SCOPED_TRACE(level);
      const std::uint32_t size = host >> level;
      std::vector<std::uint8_t> expected(std::size_t{size} * size * 4);
      UpscaleRgba(decoded.data(), kSize, kSize, kSize * 4, size, size,
                  expected.data());
      UploadCase scaled = level0;
      scaled.width = size;
      scaled.height = size;
      EXPECT_GE(Psnr(scaled,
                     DecodeBcReadback(scaled,
                                      static_cast<std::uint8_t*>(
                                          readback.mapped) +
                                          offsets[level]),
                     expected),
                30.0);
    }
    for (std::uint32_t mip = 0; mip < kMips; ++mip) {
      SCOPED_TRACE(mip);
      UploadCase own = level0;
      own.width = std::max<std::uint32_t>(1, kSize >> mip);
      own.height = own.width;
      own.offset = source_offsets[mip];
      // Random blocks in BC7; a 4x4 or smaller level is one block, and
      // measures 20.3 dB on this seed. A level that never arrived is ~3 dB.
      EXPECT_GE(Psnr(own,
                     DecodeBcReadback(own, static_cast<std::uint8_t*>(
                                               readback.mapped) +
                                               offsets[mip + kTop]),
                     DecodeOnCpu(own, static_cast<std::uint8_t*>(source.mapped))),
                own.width > 4 ? 21.0 : 18.0);
    }

    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, image);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A 32x32 placeholder is the size of the bundled crosshair override, so it
// stays upscaled RGBA8 while the full-size image is BC7.
TEST(VulkanEtc2GpuEmulationTest, CopiesAnOverrideSizedPlaceholderIntoATailMip) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("mocktail-overrides-copy-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    RgbaImage replacement;
    replacement.width = 2;
    replacement.height = 2;
    replacement.pixels.assign(16, 255);
    ASSERT_TRUE(WritePngRgba((dir / "75dcd1166a6cbaa9_32x32.png").string(),
                             replacement));
    ASSERT_EQ(setenv("MOCKTAIL_TEXTURE_OVERRIDE_DIR", dir.c_str(), 1), 0);
    VulkanEtc2Emulation emulation;
    std::filesystem::remove_all(dir);
    ASSERT_EQ(unsetenv("MOCKTAIL_TEXTURE_OVERRIDE_DIR"), 0);
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const TestImage placeholder = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const TestImage full = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, 3);
    const HostBuffer source = CreateRandomEtcSource(&emulation, vk, 32);
    HostBuffer readback = CreateHostBuffer(vk, Bc7MipBytes(32),
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands upload = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, upload.buffer, source.buffer,
                placeholder.image, 32);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(upload.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, upload.buffer));

    TestCommands copy = BeginTestCommands(vk);
    ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
    region.extent = {32, 32, 1};
    emulation.CmdCopyImage(vk.device, copy.buffer, placeholder.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, full.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 2);
    ReadMipBc7(vk, copy.buffer, full.image, 2, 32, readback.buffer, 0);
    HostReadBarrier(vk, copy.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(copy.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, copy.buffer));

    UploadCase c;
    c.etc = EtcFormat::kEtc2Rgb8;
    c.width = 32;
    c.height = 32;
    EXPECT_GE(Psnr(c,
                   DecodeBcReadback(
                       c, static_cast<std::uint8_t*>(readback.mapped)),
                   DecodeOnCpu(c, static_cast<std::uint8_t*>(source.mapped))),
              21.0);

    emulation.ReleaseCommandBuffer(upload.buffer);
    emulation.ReleaseCommandBuffer(copy.buffer);
    DestroyTestCommands(vk, upload);
    DestroyTestCommands(vk, copy);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, placeholder);
    DestroyTestImage(&emulation, vk, full);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A placeholder kept as RGBA8 (here by its usage; an image the size of a
// texture override is kept the same way) copied into the tail of a BC7 image.
TEST(VulkanEtc2GpuEmulationTest, CopiesADecodedImageIntoACompressedTailMip) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const TestImage placeholder =
        CreateTestImage(&emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128,
                        1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    const TestImage full = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 256, 2);
    const HostBuffer source = CreateRandomEtcSource(&emulation, vk, 128);
    HostBuffer readback = CreateHostBuffer(vk, Bc7MipBytes(128),
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands upload = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, upload.buffer, source.buffer,
                placeholder.image, 128);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(upload.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, upload.buffer));

    TestCommands copy = BeginTestCommands(vk);
    ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 1);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
    region.extent = {128, 128, 1};
    emulation.CmdCopyImage(vk.device, copy.buffer, placeholder.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, full.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, copy.buffer, full.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 1);
    ReadMipBc7(vk, copy.buffer, full.image, 1, 128, readback.buffer, 0);
    HostReadBarrier(vk, copy.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(copy.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, copy.buffer));

    UploadCase c;
    c.etc = EtcFormat::kEtc2Rgb8;
    c.width = 128;
    c.height = 128;
    EXPECT_GE(Psnr(c,
                   DecodeBcReadback(
                       c, static_cast<std::uint8_t*>(readback.mapped)),
                   DecodeOnCpu(c, static_cast<std::uint8_t*>(source.mapped))),
              21.0);

    emulation.ReleaseCommandBuffer(upload.buffer);
    emulation.ReleaseCommandBuffer(copy.buffer);
    DestroyTestCommands(vk, upload);
    DestroyTestCommands(vk, copy);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, placeholder);
    DestroyTestImage(&emulation, vk, full);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A later mip of an upscaled BC7 image is encoded from the level-0 texels, not
// left empty.
TEST(VulkanEtc2GpuEmulationTest, UpscalesALaterMipIntoBc7) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const std::uint32_t host = 32 * kTestUpscale;
    const TestImage image = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 2);
    const HostBuffer level0 = CreateRandomEtcSource(&emulation, vk, 32);
    const HostBuffer level1 = CreateRandomEtcSource(&emulation, vk, 16);
    const VkDeviceSize bytes0 = VkDeviceSize{host / 4} * (host / 4) * 16;
    const VkDeviceSize bytes1 = bytes0 / 4;
    HostBuffer readback = CreateHostBuffer(vk, bytes0 + bytes1,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands first = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, first.buffer, level0.buffer, image.image, 32);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(first.buffer);
    Etc2SubmitWait wait =
        emulation.PrepareSubmit(&first.buffer, 1, false, vk.queue);
    EXPECT_FALSE(wait.decode_commands.empty());
    std::vector<VkCommandBuffer> submitted = wait.decode_commands;
    submitted.push_back(first.buffer);
    ASSERT_TRUE(SubmitAndWait(vk, submitted));

    TestCommands second = BeginTestCommands(vk);
    ImageBarrier(Emu(emulation), vk, second.buffer, image.image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 1);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
    region.imageExtent = {16, 16, 1};
    emulation.CmdCopyBufferToImage(vk.device, second.buffer, level1.buffer,
                                   image.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
    ImageBarrier(Emu(emulation), vk, second.buffer, image.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 1);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.imageExtent = {host, host, 1};
    const auto copy_back =
        vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    copy_back(second.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              readback.buffer, 1, &back);
    back.bufferOffset = bytes0;
    back.imageSubresource.mipLevel = 1;
    back.imageExtent = {host / 2, host / 2, 1};
    copy_back(second.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              readback.buffer, 1, &back);
    HostReadBarrier(vk, second.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(second.buffer);
    wait = emulation.PrepareSubmit(&second.buffer, 1, false, vk.queue);
    EXPECT_FALSE(wait.decode_commands.empty());
    submitted = wait.decode_commands;
    submitted.push_back(second.buffer);
    ASSERT_TRUE(SubmitAndWait(vk, submitted));

    UploadCase small;
    small.etc = EtcFormat::kEtc2Rgb8;
    small.width = 32;
    small.height = 32;
    const std::vector<std::uint8_t> decoded =
        DecodeOnCpu(small, static_cast<std::uint8_t*>(level0.mapped));
    const auto* got = static_cast<const std::uint8_t*>(readback.mapped);
    for (const std::uint32_t size : {host, host / 2}) {
      SCOPED_TRACE(size);
      std::vector<std::uint8_t> expected(std::size_t{size} * size * 4);
      UpscaleRgba(decoded.data(), 32, 32, 32 * 4, size, size, expected.data());
      UploadCase scaled = small;
      scaled.width = size;
      scaled.height = size;
      EXPECT_GE(Psnr(scaled,
                     DecodeBcReadback(scaled,
                                      got + (size == host ? 0 : bytes0)),
                     expected),
                30.0);
    }

    emulation.ReleaseCommandBuffer(first.buffer);
    emulation.ReleaseCommandBuffer(second.buffer);
    DestroyTestCommands(vk, first);
    DestroyTestCommands(vk, second);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, level0);
    DestroyEtcSource(&emulation, vk, level1);
    DestroyTestImage(&emulation, vk, image);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Without BC support the upscaled texture stays RGBA8, still scaled on the
// GPU. A later upload of a smaller mip is scaled from the level-0 texels.
TEST(VulkanEtc2GpuEmulationTest, UpscalesSmallUploadsOnTheGpu) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, false);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const std::uint32_t host = 32 * kTestUpscale;
    const TestImage image = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 2);
    const HostBuffer level0 = CreateRandomEtcSource(&emulation, vk, 32);
    const HostBuffer level1 = CreateRandomEtcSource(&emulation, vk, 16);
    const VkDeviceSize bytes0 = VkDeviceSize{host} * host * 4;
    const VkDeviceSize bytes1 = bytes0 / 4;
    HostBuffer readback = CreateHostBuffer(vk, bytes0 + bytes1,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands first = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, first.buffer, level0.buffer, image.image, 32);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(first.buffer);
    Etc2SubmitWait wait =
        emulation.PrepareSubmit(&first.buffer, 1, false, vk.queue);
    EXPECT_FALSE(wait.decode_commands.empty());
    std::vector<VkCommandBuffer> submitted = wait.decode_commands;
    submitted.push_back(first.buffer);
    ASSERT_TRUE(SubmitAndWait(vk, submitted));

    TestCommands second = BeginTestCommands(vk);
    ImageBarrier(Emu(emulation), vk, second.buffer, image.image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 1);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
    region.imageExtent = {16, 16, 1};
    emulation.CmdCopyBufferToImage(vk.device, second.buffer, level1.buffer,
                                   image.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
    ImageBarrier(Emu(emulation), vk, second.buffer, image.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 1);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.imageExtent = {host, host, 1};
    const auto copy_back =
        vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    copy_back(second.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              readback.buffer, 1, &back);
    back.bufferOffset = bytes0;
    back.imageSubresource.mipLevel = 1;
    back.imageExtent = {host / 2, host / 2, 1};
    copy_back(second.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              readback.buffer, 1, &back);
    HostReadBarrier(vk, second.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(second.buffer);
    wait = emulation.PrepareSubmit(&second.buffer, 1, false, vk.queue);
    EXPECT_FALSE(wait.decode_commands.empty());
    submitted = wait.decode_commands;
    submitted.push_back(second.buffer);
    ASSERT_TRUE(SubmitAndWait(vk, submitted));

    UploadCase small;
    small.etc = EtcFormat::kEtc2Rgb8;
    small.width = 32;
    small.height = 32;
    const std::vector<std::uint8_t> decoded =
        DecodeOnCpu(small, static_cast<std::uint8_t*>(level0.mapped));
    const auto* got = static_cast<const std::uint8_t*>(readback.mapped);
    for (const std::uint32_t size : {host, host / 2}) {
      SCOPED_TRACE(size);
      std::vector<std::uint8_t> expected(std::size_t{size} * size * 4);
      UpscaleRgba(decoded.data(), 32, 32, 32 * 4, size, size, expected.data());
      const std::vector<std::uint8_t> actual(
          got + (size == host ? 0 : bytes0),
          got + (size == host ? 0 : bytes0) + expected.size());
      UploadCase scaled = small;
      scaled.width = size;
      scaled.height = size;
      EXPECT_GE(Psnr(scaled, actual, expected), 50.0);
    }

    emulation.ReleaseCommandBuffer(first.buffer);
    emulation.ReleaseCommandBuffer(second.buffer);
    DestroyTestCommands(vk, first);
    DestroyTestCommands(vk, second);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, level0);
    DestroyEtcSource(&emulation, vk, level1);
    DestroyTestImage(&emulation, vk, image);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A mip whose size is not a whole number of blocks, uploaded before level 0,
// keeps all of its own texels, edge blocks included, below the top levels.
TEST(VulkanEtc2GpuEmulationTest, KeepsAPartialBlockMipFromItsOwnBlocks) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, false);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const std::uint32_t host = 6;
    const TestImage image = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 12, 2);
    // 2x2 blocks: the 6x6 mip.
    const HostBuffer level1 = CreateRandomEtcSource(&emulation, vk, 8);
    const VkDeviceSize bytes = VkDeviceSize{host} * host * 4;
    HostBuffer readback =
        CreateHostBuffer(vk, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    ImageBarrier(Emu(emulation), vk, commands.buffer, image.image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 1);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
    region.imageExtent = {6, 6, 1};
    emulation.CmdCopyBufferToImage(vk.device, commands.buffer, level1.buffer,
                                   image.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
    ImageBarrier(Emu(emulation), vk, commands.buffer, image.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 1);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 3, 0, 1};
    back.imageExtent = {host, host, 1};
    vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        commands.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.buffer, 1, &back);
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    const Etc2SubmitWait wait =
        emulation.PrepareSubmit(&commands.buffer, 1, false, vk.queue);
    EXPECT_FALSE(wait.decode_commands.empty());
    std::vector<VkCommandBuffer> submitted = wait.decode_commands;
    submitted.push_back(commands.buffer);
    ASSERT_TRUE(SubmitAndWait(vk, submitted));

    UploadCase mip;
    mip.etc = EtcFormat::kEtc2Rgb8;
    mip.width = 6;
    mip.height = 6;
    const std::vector<std::uint8_t> decoded =
        DecodeOnCpu(mip, static_cast<std::uint8_t*>(level1.mapped));
    const auto* got = static_cast<const std::uint8_t*>(readback.mapped);
    const std::vector<std::uint8_t> actual(got, got + bytes);
    EXPECT_GE(Psnr(mip, actual, decoded), 50.0);

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, level1);
    DestroyTestImage(&emulation, vk, image);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Roblox's small UI textures: 60x33 ETC2 RGBA8 with a full mip chain.
TEST(VulkanEtc2GpuEmulationTest, KeepsRobloxSmallTexturesCompressed) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    info.extent = {60, 33, 1};
    info.mipLevels = 6;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image = VK_NULL_HANDLE;
    ASSERT_EQ(emulation.CreateImage(vk.device, &info, nullptr, &image),
              VK_SUCCESS);
    const EmulatedTextureBytes bytes = emulation.TextureBytes();
    // Host 240x132 BC7: one byte per texel over the mip chain, versus four
    // for RGBA8.
    EXPECT_LT(bytes.decoded, VkDeviceSize{240} * 132 * 2);
    emulation.DestroyImage(vk.device, image, nullptr);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

VkImageCreateInfo Etc2ImageInfo(std::uint32_t size, std::uint32_t mips) {
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
  info.extent = {size, size, 1};
  info.mipLevels = mips;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  return info;
}

// Host bytes an image adds to the emulation's texture total.
VkDeviceSize CreatedBytes(VulkanEtc2Emulation& emulation, VkDevice device,
                          const VkImageCreateInfo& info,
                          std::vector<VkImage>* images) {
  const VkDeviceSize before = emulation.TextureBytes().decoded;
  VkImage image = VK_NULL_HANDLE;
  EXPECT_EQ(emulation.CreateImage(device, &info, nullptr, &image), VK_SUCCESS);
  images->push_back(image);
  return emulation.TextureBytes().decoded - before;
}

TEST(VulkanEtc2GpuEmulationTest, KeepsAHeldBackSmallTextureAtTheSetFactor) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  const ScopedEncodeBudget budget;
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    std::vector<VkImage> images;
    // An 8192x8192 chain, 5.6 million blocks, costs several bursts even on
    // an RTX 4090 and leaves the budget in debt for longer than the test.
    CreatedBytes(emulation, vk.device, Etc2ImageInfo(8192, 14), &images);
    const VkImageCreateInfo small = Etc2ImageInfo(64, 7);
    VkDeviceSize compressed = 0;
    VkDeviceSize decoded = 0;
    ASSERT_TRUE(Etc2ImageByteCounts(EtcFormat::kEtc2Rgba8, small.extent, 7, 1,
                                    kTestUpscale, &compressed, &decoded));
    EXPECT_EQ(CreatedBytes(emulation, vk.device, small, &images), decoded);
    for (VkImage image : images) {
      emulation.DestroyImage(vk.device, image, nullptr);
    }
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Only BC encodes are budgeted, so without them there is nothing to time.
TEST(VulkanEtc2GpuEmulationTest, SkipsTheSpeedProbeWithoutCompression) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  const ScopedEncodeBudget budget;
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, false);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const Etc2GpuSpeed speed = emulation.GpuSpeed(vk.device);
    EXPECT_EQ(speed.transcode_ns, 0.0);
    EXPECT_EQ(speed.upscale_ns, 0.0);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

TEST(VulkanEtc2GpuEmulationTest, ChargesRepeatedUploadsToTheEncodeBudget) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  const ScopedEncodeBudget budget;
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    std::vector<VkImage> images;
    const VkImageCreateInfo small = Etc2ImageInfo(64, 7);
    const VkDeviceSize fresh =
        CreatedBytes(emulation, vk.device, small, &images);

    constexpr std::uint32_t kSize = 512;
    const VkImageCreateInfo large = Etc2ImageInfo(kSize, 1);
    CreatedBytes(emulation, vk.device, large, &images);
    const VkImage image = images.back();
    VkMemoryRequirements requirements{};
    vk.Device<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
        vk.device, image, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        FindMemoryType(vk, requirements.memoryTypeBits, 0);
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    ASSERT_EQ(vk.Device<PFN_vkAllocateMemory>("vkAllocateMemory")(
                  vk.device, &allocate, nullptr, &image_memory),
              VK_SUCCESS);
    vk.Device<PFN_vkBindImageMemory>("vkBindImageMemory")(vk.device, image,
                                                          image_memory, 0);
    const VkDeviceSize source_bytes =
        VkDeviceSize{kSize / 4} * (kSize / 4) * 16;
    HostBuffer source =
        CreateHostBuffer(vk, source_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    ASSERT_EQ(emulation.BindBufferMemory(vk.device, source.buffer,
                                         source.memory, 0),
              VK_SUCCESS);
    ASSERT_EQ(emulation.MapMemory(vk.device, source.memory, 0, VK_WHOLE_SIZE,
                                  0, &source.mapped),
              VK_SUCCESS);
    std::memset(source.mapped, 0x5A, source_bytes);

    // Creating the image paid for one upload; the rest add up to twice the
    // burst at the measured speed.
    const double upload_ns = static_cast<double>(kSize / 4) * (kSize / 4) *
                             emulation.GpuSpeed(vk.device).transcode_ns;
    ASSERT_GT(upload_ns, 0.0);
    const int uploads = 1 + static_cast<int>(2 * kEncodeBudgetBurstNs / upload_ns);
    ASSERT_LE(uploads, 1000) << "probe measured " << upload_ns << " ns";
    TestCommands commands = BeginTestCommands(vk);
    ImageBarrier(Emu(emulation), vk, commands.buffer, image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kSize, kSize, 1};
    for (int upload = 0; upload <= uploads; ++upload) {
      emulation.CmdCopyBufferToImage(vk.device, commands.buffer, source.buffer,
                                     image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                     &region);
    }
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    EXPECT_NE(CreatedBytes(emulation, vk.device, small, &images), fresh);

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    emulation.UnmapMemory(vk.device, source.memory);
    emulation.DestroyBuffer(vk.device, source.buffer, nullptr);
    emulation.FreeMemory(vk.device, source.memory, nullptr);
    for (VkImage created : images) {
      emulation.DestroyImage(vk.device, created, nullptr);
    }
    vk.Device<PFN_vkFreeMemory>("vkFreeMemory")(vk.device, image_memory,
                                                nullptr);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Roblox drops a far texture to a smaller copy: a mip of a full-size BC7
// image is copied into a small image, which is upscaled. The copy has to
// arrive rather than leave the small texture black.
TEST(VulkanEtc2GpuEmulationTest, CopiesAFullSizeMipIntoAnUpscaledImage) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    constexpr std::uint32_t kBig = 128;
    constexpr std::uint32_t kSmall = 32;
    const std::uint32_t host = kSmall * kTestUpscale;
    const TestImage big = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kBig, 3);
    const TestImage small = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSmall, 1);
    const TestImage again = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kBig, 3);
    std::vector<HostBuffer> sources;
    for (std::uint32_t mip = 0; mip < 3; ++mip) {
      sources.push_back(CreateRandomEtcSource(&emulation, vk, kBig >> mip));
    }
    HostBuffer readback =
        CreateHostBuffer(vk, Bc7MipBytes(host) + Bc7MipBytes(kSmall),
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    for (std::uint32_t mip = 0; mip < 3; ++mip) {
      UploadMip(&emulation, vk, commands.buffer, sources[mip].buffer,
                big.image, mip, kBig >> mip);
    }
    ImageBarrier(Emu(emulation), vk, commands.buffer, small.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {kSmall, kSmall, 1};
    emulation.CmdCopyImage(vk.device, commands.buffer, big.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, small.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, commands.buffer, small.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    ReadMipBc7(vk, commands.buffer, small.image, 0, host, readback.buffer, 0);
    // Back into the tail of a full-size image, as when the texture is
    // raised again.
    ImageBarrier(Emu(emulation), vk, commands.buffer, again.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    VkImageCopy back = region;
    back.srcSubresource.mipLevel = 0;
    back.dstSubresource.mipLevel = 2;
    emulation.CmdCopyImage(vk.device, commands.buffer, small.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, again.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &back);
    ImageBarrier(Emu(emulation), vk, commands.buffer, again.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 2);
    ReadMipBc7(vk, commands.buffer, again.image, 2, kSmall, readback.buffer,
               Bc7MipBytes(host));
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, commands.buffer));

    UploadCase expected;
    expected.etc = EtcFormat::kEtc2Rgb8;
    expected.width = kSmall;
    expected.height = kSmall;
    UploadCase actual = expected;
    actual.width = host;
    actual.height = host;
    const auto mean = [](const std::vector<std::uint8_t>& texels) {
      double sum = 0;
      for (std::size_t i = 0; i < texels.size(); i += 4) {
        sum += texels[i] + texels[i + 1] + texels[i + 2];
      }
      return sum / (texels.size() / 4 * 3);
    };
    const double want = mean(DecodeOnCpu(
        expected, static_cast<std::uint8_t*>(sources[2].mapped)));
    const double got = mean(DecodeBcReadback(
        actual, static_cast<std::uint8_t*>(readback.mapped)));
    EXPECT_NEAR(got, want, 12.0);
    EXPECT_GE(Psnr(expected,
                   DecodeBcReadback(
                       expected, static_cast<std::uint8_t*>(readback.mapped) +
                                     Bc7MipBytes(host)),
                   DecodeOnCpu(expected, static_cast<std::uint8_t*>(
                                             sources[2].mapped))),
              21.0);

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    for (const HostBuffer& source : sources) {
      DestroyEtcSource(&emulation, vk, source);
    }
    DestroyTestImage(&emulation, vk, again);
    DestroyTestImage(&emulation, vk, small);
    DestroyTestImage(&emulation, vk, big);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Part of a full-size BC7 mip copied into an upscaled image arrives, and
// a copy of the result back into a full-size tail mip carries the same
// texels.
TEST(VulkanEtc2GpuEmulationTest, CopiesAPartOfAMipIntoAnUpscaledImage) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    constexpr std::uint32_t kBig = 128;
    constexpr std::uint32_t kSmall = 32;
    constexpr std::uint32_t kOffset = 16;
    const std::uint32_t host = kSmall * kTestUpscale;
    const TestImage big = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kBig, 2);
    const TestImage small = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSmall, 1);
    const TestImage again = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kBig, 3);
    std::vector<HostBuffer> sources;
    for (std::uint32_t mip = 0; mip < 2; ++mip) {
      sources.push_back(CreateRandomEtcSource(&emulation, vk, kBig >> mip));
    }
    HostBuffer readback =
        CreateHostBuffer(vk, Bc7MipBytes(host) + Bc7MipBytes(kSmall),
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    for (std::uint32_t mip = 0; mip < 2; ++mip) {
      UploadMip(&emulation, vk, commands.buffer, sources[mip].buffer,
                big.image, mip, kBig >> mip);
    }
    ImageBarrier(Emu(emulation), vk, commands.buffer, small.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
    region.srcOffset = {kOffset, kOffset, 0};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {kSmall, kSmall, 1};
    emulation.CmdCopyImage(vk.device, commands.buffer, big.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, small.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, commands.buffer, small.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    ReadMipBc7(vk, commands.buffer, small.image, 0, host, readback.buffer, 0);
    ImageBarrier(Emu(emulation), vk, commands.buffer, again.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    VkImageCopy back{};
    back.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
    back.extent = {kSmall, kSmall, 1};
    emulation.CmdCopyImage(vk.device, commands.buffer, small.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, again.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &back);
    ImageBarrier(Emu(emulation), vk, commands.buffer, again.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 2);
    ReadMipBc7(vk, commands.buffer, again.image, 2, kSmall, readback.buffer,
               Bc7MipBytes(host));
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, commands.buffer));

    UploadCase mip1;
    mip1.etc = EtcFormat::kEtc2Rgb8;
    mip1.width = kBig / 2;
    mip1.height = kBig / 2;
    const std::vector<std::uint8_t> whole =
        DecodeOnCpu(mip1, static_cast<std::uint8_t*>(sources[1].mapped));
    std::vector<std::uint8_t> part;
    for (std::uint32_t y = kOffset; y < kOffset + kSmall; ++y) {
      const auto row = whole.begin() + (std::size_t{y} * mip1.width + kOffset) * 4;
      part.insert(part.end(), row, row + kSmall * 4);
    }
    UploadCase expected;
    expected.etc = EtcFormat::kEtc2Rgb8;
    expected.width = kSmall;
    expected.height = kSmall;
    UploadCase upscaled = expected;
    upscaled.width = host;
    upscaled.height = host;
    const auto mean = [](const std::vector<std::uint8_t>& texels) {
      double sum = 0;
      for (std::size_t i = 0; i < texels.size(); i += 4) {
        sum += texels[i] + texels[i + 1] + texels[i + 2];
      }
      return sum / (texels.size() / 4 * 3);
    };
    EXPECT_NEAR(mean(DecodeBcReadback(
                    upscaled, static_cast<std::uint8_t*>(readback.mapped))),
                mean(part), 12.0);
    EXPECT_GE(Psnr(expected,
                   DecodeBcReadback(
                       expected, static_cast<std::uint8_t*>(readback.mapped) +
                                     Bc7MipBytes(host)),
                   part),
              21.0);

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    for (const HostBuffer& source : sources) {
      DestroyEtcSource(&emulation, vk, source);
    }
    DestroyTestImage(&emulation, vk, again);
    DestroyTestImage(&emulation, vk, small);
    DestroyTestImage(&emulation, vk, big);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

void StartBcEmulation(VulkanEtc2Emulation* emulation,
                      const VulkanTestDevice& vk, bool timeline = false) {
  emulation->RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                            vk.get_device_proc_addr, timeline, true);
  emulation->RegisterQueue(vk.device, vk.queue, vk.family);
  ASSERT_TRUE(emulation->EnableGpuDecode(vk.device, vk.queue, vk.family));
}

void EndAndSubmit(VulkanEtc2Emulation* emulation, const VulkanTestDevice& vk,
                  VkCommandBuffer command_buffer) {
  vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command_buffer);
  ASSERT_TRUE(SubmitThroughEmulation(emulation, vk, command_buffer));
}

void CopyImage(VulkanEtc2Emulation* emulation, const VulkanTestDevice& vk,
               VkCommandBuffer command_buffer, VkImage source,
               std::uint32_t source_mip, VkOffset3D source_offset,
               VkImage destination, std::uint32_t destination_mip,
               VkOffset3D destination_offset, std::uint32_t size,
               bool fresh_destination = true) {
  if (fresh_destination) {
    ImageBarrier(Emu(emulation), vk, command_buffer, destination, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, destination_mip);
  } else {
    ImageBarrier(Emu(emulation), vk, command_buffer, destination, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 destination_mip);
  }
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, source_mip, 0, 1};
  region.srcOffset = source_offset;
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, destination_mip, 0, 1};
  region.dstOffset = destination_offset;
  region.extent = {size, size, 1};
  emulation->CmdCopyImage(vk.device, command_buffer, source,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  ImageBarrier(Emu(emulation), vk, command_buffer, destination, 1,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               destination_mip);
}

UploadCase Rgb8Case(std::uint32_t size) {
  UploadCase c;
  c.etc = EtcFormat::kEtc2Rgb8;
  c.width = size;
  c.height = size;
  return c;
}

double MeanRgb(const std::vector<std::uint8_t>& texels) {
  double sum = 0;
  for (std::size_t i = 0; i < texels.size(); i += 4) {
    sum += texels[i] + texels[i + 1] + texels[i + 2];
  }
  return sum / (texels.size() / 4 * 3);
}

// Keeping the blocks of a BC image's small mips is CPU work the GPU copy
// never reads, so the submit does not wait on it, and a later copy into an
// upscaled image still finds them.
TEST(VulkanEtc2GpuEmulationTest, KeepsSmallMipBlocksWithoutAGpuWait) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures || !vk.timeline) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    StartBcEmulation(&emulation, vk, true);
    constexpr std::uint32_t kBig = 128;
    constexpr std::uint32_t kSmall = 32;
    const std::uint32_t host = kSmall * kTestUpscale;
    const TestImage big = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kBig, 3);
    const TestImage small = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSmall, 1);
    std::vector<HostBuffer> sources;
    for (std::uint32_t mip = 0; mip < 3; ++mip) {
      sources.push_back(CreateRandomEtcSource(&emulation, vk, kBig >> mip));
    }
    HostBuffer readback = CreateHostBuffer(vk, Bc7MipBytes(host),
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands upload = BeginTestCommands(vk);
    for (std::uint32_t mip = 0; mip < 3; ++mip) {
      UploadMip(&emulation, vk, upload.buffer, sources[mip].buffer, big.image,
                mip, kBig >> mip);
    }
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(upload.buffer);
    const Etc2SubmitWait wait =
        emulation.PrepareSubmit(&upload.buffer, 1, true, vk.queue);
    EXPECT_EQ(wait.semaphore, VK_NULL_HANDLE);
    std::vector<VkCommandBuffer> submitted = wait.decode_commands;
    submitted.push_back(upload.buffer);
    ASSERT_TRUE(SubmitAndWait(vk, submitted));

    TestCommands copy = BeginTestCommands(vk);
    CopyImage(&emulation, vk, copy.buffer, big.image, 2, {0, 0, 0},
              small.image, 0, {0, 0, 0}, kSmall);
    ReadMipBc7(vk, copy.buffer, small.image, 0, host, readback.buffer, 0);
    HostReadBarrier(vk, copy.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(copy.buffer);
    const Etc2SubmitWait copy_wait =
        emulation.PrepareSubmit(&copy.buffer, 1, true, vk.queue);
    if (copy_wait.semaphore != VK_NULL_HANDLE) {
      VkSemaphoreWaitInfo wait_info{};
      wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
      wait_info.semaphoreCount = 1;
      wait_info.pSemaphores = &copy_wait.semaphore;
      wait_info.pValues = &copy_wait.value;
      ASSERT_EQ(vk.Device<PFN_vkWaitSemaphores>("vkWaitSemaphores")(
                    vk.device, &wait_info, UINT64_MAX),
                VK_SUCCESS);
    }
    submitted = copy_wait.decode_commands;
    submitted.push_back(copy.buffer);
    ASSERT_TRUE(SubmitAndWait(vk, submitted));

    EXPECT_NEAR(MeanRgb(DecodeBcReadback(
                    Rgb8Case(host),
                    static_cast<std::uint8_t*>(readback.mapped))),
                MeanRgb(DecodeOnCpu(
                    Rgb8Case(kSmall),
                    static_cast<std::uint8_t*>(sources[2].mapped))),
                12.0);

    emulation.ReleaseCommandBuffer(upload.buffer);
    emulation.ReleaseCommandBuffer(copy.buffer);
    DestroyTestCommands(vk, upload);
    DestroyTestCommands(vk, copy);
    DestroyHostBuffer(vk, readback);
    for (const HostBuffer& source : sources) {
      DestroyEtcSource(&emulation, vk, source);
    }
    DestroyTestImage(&emulation, vk, small);
    DestroyTestImage(&emulation, vk, big);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A submit that keeps blocks and copies into other mips runs the copies at
// once, so the GPU does not wait for the blocks to be kept.
TEST(VulkanEtc2GpuEmulationTest, KeepsBlocksBesideACopyWithoutAGpuWait) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures || !vk.timeline) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    StartBcEmulation(&emulation, vk, true);
    constexpr std::uint32_t kBig = 128;
    const TestImage big = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kBig, 3);
    const TestImage placeholder = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const TestImage small32 = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const TestImage small64 = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 64, 1);
    const HostBuffer source128 = CreateRandomEtcSource(&emulation, vk, 128);
    const HostBuffer source64 = CreateRandomEtcSource(&emulation, vk, 64);
    const HostBuffer source32 = CreateRandomEtcSource(&emulation, vk, 32);
    const std::uint32_t host32 = 32 * kTestUpscale;
    const std::uint32_t host64 = 64 * kTestUpscale;
    HostBuffer readback32 = CreateHostBuffer(vk, Bc7MipBytes(host32),
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    HostBuffer readback64 = CreateHostBuffer(vk, Bc7MipBytes(host64),
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback32);
    BindAndMap(vk, &readback64);

    TestCommands prepare = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, prepare.buffer, source32.buffer,
                placeholder.image, 32);
    EndAndSubmit(&emulation, vk, prepare.buffer);

    TestCommands stream = BeginTestCommands(vk);
    UploadMip(&emulation, vk, stream.buffer, source128.buffer, big.image, 0,
              128);
    UploadMip(&emulation, vk, stream.buffer, source64.buffer, big.image, 1, 64);
    ImageBarrier(Emu(emulation), vk, stream.buffer, big.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
    region.extent = {32, 32, 1};
    emulation.CmdCopyImage(vk.device, stream.buffer, placeholder.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, big.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, stream.buffer, big.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 2);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(stream.buffer);
    const Etc2SubmitWait wait =
        emulation.PrepareSubmit(&stream.buffer, 1, true, vk.queue);
    EXPECT_EQ(wait.semaphore, VK_NULL_HANDLE);
    std::vector<VkCommandBuffer> submitted = wait.decode_commands;
    submitted.push_back(stream.buffer);
    ASSERT_TRUE(SubmitAndWait(vk, submitted));

    // Both the copied mip and the kept one reach later copies.
    TestCommands copy = BeginTestCommands(vk);
    CopyImage(&emulation, vk, copy.buffer, big.image, 2, {0, 0, 0},
              small32.image, 0, {0, 0, 0}, 32);
    CopyImage(&emulation, vk, copy.buffer, big.image, 1, {0, 0, 0},
              small64.image, 0, {0, 0, 0}, 64);
    ReadMipBc7(vk, copy.buffer, small32.image, 0, host32, readback32.buffer, 0);
    ReadMipBc7(vk, copy.buffer, small64.image, 0, host64, readback64.buffer, 0);
    HostReadBarrier(vk, copy.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(copy.buffer);
    const Etc2SubmitWait copy_wait =
        emulation.PrepareSubmit(&copy.buffer, 1, true, vk.queue);
    if (copy_wait.semaphore != VK_NULL_HANDLE) {
      VkSemaphoreWaitInfo wait_info{};
      wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
      wait_info.semaphoreCount = 1;
      wait_info.pSemaphores = &copy_wait.semaphore;
      wait_info.pValues = &copy_wait.value;
      ASSERT_EQ(vk.Device<PFN_vkWaitSemaphores>("vkWaitSemaphores")(
                    vk.device, &wait_info, UINT64_MAX),
                VK_SUCCESS);
    }
    submitted = copy_wait.decode_commands;
    submitted.push_back(copy.buffer);
    ASSERT_TRUE(SubmitAndWait(vk, submitted));

    EXPECT_NEAR(MeanRgb(DecodeBcReadback(
                    Rgb8Case(host32),
                    static_cast<std::uint8_t*>(readback32.mapped))),
                MeanRgb(DecodeOnCpu(
                    Rgb8Case(32), static_cast<std::uint8_t*>(source32.mapped))),
                12.0);
    EXPECT_NEAR(MeanRgb(DecodeBcReadback(
                    Rgb8Case(host64),
                    static_cast<std::uint8_t*>(readback64.mapped))),
                MeanRgb(DecodeOnCpu(
                    Rgb8Case(64), static_cast<std::uint8_t*>(source64.mapped))),
                12.0);

    emulation.ReleaseCommandBuffer(prepare.buffer);
    emulation.ReleaseCommandBuffer(stream.buffer);
    emulation.ReleaseCommandBuffer(copy.buffer);
    DestroyTestCommands(vk, prepare);
    DestroyTestCommands(vk, stream);
    DestroyTestCommands(vk, copy);
    DestroyHostBuffer(vk, readback32);
    DestroyHostBuffer(vk, readback64);
    DestroyEtcSource(&emulation, vk, source128);
    DestroyEtcSource(&emulation, vk, source64);
    DestroyEtcSource(&emulation, vk, source32);
    DestroyTestImage(&emulation, vk, small64);
    DestroyTestImage(&emulation, vk, small32);
    DestroyTestImage(&emulation, vk, placeholder);
    DestroyTestImage(&emulation, vk, big);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// An upload from memory the host cannot map still reaches the GPU, and the
// CPU never tries to map that memory to keep its blocks.
TEST(VulkanEtc2GpuEmulationTest, KeepsNoBlocksFromMemoryTheHostCannotRead) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    constexpr std::uint32_t kBig = 128;
    constexpr std::uint32_t kMip = 2;
    constexpr std::uint32_t kSize = kBig >> kMip;
    const VkDeviceSize bytes = VkDeviceSize{kSize / 4} * (kSize / 4) * 8;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer device_buffer = VK_NULL_HANDLE;
    ASSERT_EQ(vk.Device<PFN_vkCreateBuffer>("vkCreateBuffer")(
                  vk.device, &info, nullptr, &device_buffer),
              VK_SUCCESS);
    VkMemoryRequirements requirements{};
    vk.Device<PFN_vkGetBufferMemoryRequirements>(
        "vkGetBufferMemoryRequirements")(vk.device, device_buffer,
                                         &requirements);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < vk.memory.memoryTypeCount; ++i) {
      const VkMemoryPropertyFlags flags = vk.memory.memoryTypes[i].propertyFlags;
      if ((requirements.memoryTypeBits & (1U << i)) != 0 &&
          (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        type = i;
        break;
      }
    }
    if (type == UINT32_MAX) {
      vk.Device<PFN_vkDestroyBuffer>("vkDestroyBuffer")(vk.device,
                                                        device_buffer, nullptr);
      continue;
    }
    VulkanEtc2Emulation emulation;
    StartBcEmulation(&emulation, vk);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    VkDeviceMemory device_memory = VK_NULL_HANDLE;
    ASSERT_EQ(vk.Device<PFN_vkAllocateMemory>("vkAllocateMemory")(
                  vk.device, &allocate, nullptr, &device_memory),
              VK_SUCCESS);
    emulation.TrackAllocation(vk.device, &allocate, device_memory);
    ASSERT_EQ(emulation.BindBufferMemory(vk.device, device_buffer,
                                         device_memory, 0),
              VK_SUCCESS);
    HostBuffer blocks =
        CreateHostBuffer(vk, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    BindAndMap(vk, &blocks);
    std::mt19937_64 random(kSize);
    for (VkDeviceSize i = 0; i < bytes; ++i) {
      static_cast<std::uint8_t*>(blocks.mapped)[i] =
          static_cast<std::uint8_t>(random());
    }
    TestCommands fill = BeginTestCommands(vk);
    const VkBufferCopy whole{0, 0, bytes};
    vk.Device<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(
        fill.buffer, blocks.buffer, device_buffer, 1, &whole);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(fill.buffer);
    ASSERT_TRUE(SubmitAndWait(vk, {fill.buffer}));

    const TestImage big = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kBig, 3);
    HostBuffer readback = CreateHostBuffer(vk, Bc7MipBytes(kSize),
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);
    const std::uint64_t unmappable =
        Etc2FailureCount("could not map an unmapped upload source");
    TestCommands upload = BeginTestCommands(vk);
    UploadMip(&emulation, vk, upload.buffer, device_buffer, big.image, kMip,
              kSize);
    ReadMipBc7(vk, upload.buffer, big.image, kMip, kSize, readback.buffer, 0);
    HostReadBarrier(vk, upload.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    EndAndSubmit(&emulation, vk, upload.buffer);

    EXPECT_EQ(Etc2FailureCount("could not map an unmapped upload source"),
              unmappable);
    EXPECT_GE(Psnr(Rgb8Case(kSize),
                   DecodeBcReadback(Rgb8Case(kSize), static_cast<std::uint8_t*>(
                                                         readback.mapped)),
                   DecodeOnCpu(Rgb8Case(kSize),
                               static_cast<std::uint8_t*>(blocks.mapped))),
              21.0);

    emulation.ReleaseCommandBuffer(upload.buffer);
    DestroyTestCommands(vk, upload);
    DestroyTestCommands(vk, fill);
    DestroyHostBuffer(vk, readback);
    DestroyHostBuffer(vk, blocks);
    DestroyTestImage(&emulation, vk, big);
    emulation.DestroyBuffer(vk.device, device_buffer, nullptr);
    emulation.FreeMemory(vk.device, device_memory, nullptr);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Only mips small enough to keep their blocks can be copied into an upscaled
// image. A copy from a larger mip is skipped when it is recorded, with its
// own reason, instead of filling the image with zeros at submit.
TEST(VulkanEtc2GpuEmulationTest, SkipsACopyFromAMipTooLargeToKeep) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    StartBcEmulation(&emulation, vk);
    const TestImage big = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, 1);
    const TestImage small = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const HostBuffer source = CreateRandomEtcSource(&emulation, vk, 128);
    const std::uint64_t zeroed =
        Etc2FailureCount("copied into a compressed image without its source");
    const std::uint64_t skipped =
        Etc2FailureCount("skipped a copy from a mip too large to keep");

    TestCommands commands = BeginTestCommands(vk);
    UploadMip(&emulation, vk, commands.buffer, source.buffer, big.image, 0,
              128);
    CopyImage(&emulation, vk, commands.buffer, big.image, 0, {32, 32, 0},
              small.image, 0, {0, 0, 0}, 32);
    EndAndSubmit(&emulation, vk, commands.buffer);

    EXPECT_EQ(
        Etc2FailureCount("copied into a compressed image without its source"),
        zeroed);
    EXPECT_EQ(Etc2FailureCount("skipped a copy from a mip too large to keep"),
              skipped + 1);

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, small);
    DestroyTestImage(&emulation, vk, big);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A copy from an image the emulation never saw is skipped once, not again for
// each top level of the destination.
TEST(VulkanEtc2GpuEmulationTest, SkipsACopyFromAnUnknownImageOnce) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    StartBcEmulation(&emulation, vk);
    const TestImage small = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const std::uint64_t skipped =
        Etc2FailureCount("skipped a copy into a compressed image");

    // Both paths log and return before the driver sees the unknown handle.
    TestCommands commands = BeginTestCommands(vk);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {32, 32, 1};
    emulation.CmdCopyImage(vk.device, commands.buffer,
                           reinterpret_cast<VkImage>(std::uintptr_t{0xdead0}),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, small.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    EXPECT_EQ(Etc2FailureCount("skipped a copy into a compressed image"),
              skipped + 1);

    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyTestImage(&emulation, vk, small);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A partial copy between upscaled images patches the destination's kept
// blocks, so a later copy out of it carries both parts.
TEST(VulkanEtc2GpuEmulationTest, PatchesKeptBlocksOnAPartialCopy) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    StartBcEmulation(&emulation, vk);
    constexpr std::uint32_t kSmall = 32;
    constexpr std::uint32_t kPart = 16;
    const TestImage first = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSmall, 1);
    const TestImage second = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSmall, 1);
    const TestImage big = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, 3);
    const HostBuffer first_source =
        CreateRandomEtcSource(&emulation, vk, kSmall);
    // Refilled from another seed, since sources are seeded by size.
    const HostBuffer second_blocks =
        CreateRandomEtcSource(&emulation, vk, kSmall);
    std::mt19937_64 random(99);
    auto* second_bytes = static_cast<std::uint8_t*>(second_blocks.mapped);
    for (std::size_t i = 0; i < std::size_t{kSmall / 4} * (kSmall / 4) * 8;
         ++i) {
      second_bytes[i] = static_cast<std::uint8_t>(random());
    }
    HostBuffer readback = CreateHostBuffer(vk, Bc7MipBytes(kSmall),
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    UploadMip(&emulation, vk, commands.buffer, first_source.buffer,
              first.image, 0, kSmall);
    UploadMip(&emulation, vk, commands.buffer, second_blocks.buffer,
              second.image, 0, kSmall);
    CopyImage(&emulation, vk, commands.buffer, first.image, 0, {0, 0, 0},
              second.image, 0, {kPart, 0, 0}, kPart, false);
    CopyImage(&emulation, vk, commands.buffer, second.image, 0, {0, 0, 0},
              big.image, 2, {0, 0, 0}, kSmall);
    ReadMipBc7(vk, commands.buffer, big.image, 2, kSmall, readback.buffer, 0);
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    EndAndSubmit(&emulation, vk, commands.buffer);

    std::vector<std::uint8_t> expected = DecodeOnCpu(Rgb8Case(kSmall), second_bytes);
    const std::vector<std::uint8_t> part = DecodeOnCpu(
        Rgb8Case(kSmall), static_cast<std::uint8_t*>(first_source.mapped));
    for (std::uint32_t y = 0; y < kPart; ++y) {
      std::memcpy(expected.data() + (std::size_t{y} * kSmall + kPart) * 4,
                  part.data() + std::size_t{y} * kSmall * 4, kPart * 4);
    }
    EXPECT_GE(Psnr(Rgb8Case(kSmall),
                   DecodeBcReadback(Rgb8Case(kSmall),
                                    static_cast<std::uint8_t*>(readback.mapped)),
                   expected),
              21.0);

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, second_blocks);
    DestroyEtcSource(&emulation, vk, first_source);
    DestroyTestImage(&emulation, vk, big);
    DestroyTestImage(&emulation, vk, second);
    DestroyTestImage(&emulation, vk, first);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A tail mip of a full-size image filled by a copy from a small texture keeps
// those blocks, so copying that mip into another small texture carries them.
TEST(VulkanEtc2GpuEmulationTest, KeepsBlocksOfATailMipFilledByACopy) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    StartBcEmulation(&emulation, vk);
    constexpr std::uint32_t kSmall = 32;
    const TestImage placeholder = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSmall, 1);
    const TestImage full = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, 3);
    const TestImage lowered = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSmall, 1);
    const TestImage raised = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, 3);
    const HostBuffer source = CreateRandomEtcSource(&emulation, vk, kSmall);
    HostBuffer readback = CreateHostBuffer(vk, Bc7MipBytes(kSmall),
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    UploadMip(&emulation, vk, commands.buffer, source.buffer,
              placeholder.image, 0, kSmall);
    CopyImage(&emulation, vk, commands.buffer, placeholder.image, 0,
              {0, 0, 0}, full.image, 2, {0, 0, 0}, kSmall);
    CopyImage(&emulation, vk, commands.buffer, full.image, 2, {0, 0, 0},
              lowered.image, 0, {0, 0, 0}, kSmall);
    CopyImage(&emulation, vk, commands.buffer, lowered.image, 0, {0, 0, 0},
              raised.image, 2, {0, 0, 0}, kSmall);
    ReadMipBc7(vk, commands.buffer, raised.image, 2, kSmall, readback.buffer,
               0);
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    EndAndSubmit(&emulation, vk, commands.buffer);

    EXPECT_GE(Psnr(Rgb8Case(kSmall),
                   DecodeBcReadback(Rgb8Case(kSmall),
                                    static_cast<std::uint8_t*>(readback.mapped)),
                   DecodeOnCpu(Rgb8Case(kSmall),
                               static_cast<std::uint8_t*>(source.mapped))),
              21.0);

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, raised);
    DestroyTestImage(&emulation, vk, lowered);
    DestroyTestImage(&emulation, vk, full);
    DestroyTestImage(&emulation, vk, placeholder);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Benchmark, run by hand with --gtest_also_run_disabled_tests: the submit
// cost of 128px chains with only level 0 against full chains, whose 64px and
// smaller mips keep their blocks. The gap bounds the cost of keeping them.
TEST(VulkanEtc2GpuEmulationTest, DISABLED_BenchmarkKeepingSmallMipBlocks) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  for (VulkanTestDevice& vk : devices) {
    if (!vk.bc_textures || !vk.timeline) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    StartBcEmulation(&emulation, vk, true);
    constexpr std::uint32_t kTextures = 256;
    constexpr std::uint32_t kMips = 8;
    std::vector<TestImage> images;
    for (std::uint32_t i = 0; i < kTextures; ++i) {
      images.push_back(CreateTestImage(
          &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, kMips));
    }
    std::vector<HostBuffer> sources;
    for (std::uint32_t mip = 0; mip < kMips; ++mip) {
      sources.push_back(
          CreateRandomEtcSource(&emulation, vk, std::max(4U, 128U >> mip)));
    }
    for (const std::uint32_t mips : {1U, kMips}) {
      std::vector<double> times;
      for (int run = 0; run < 7; ++run) {
        TestCommands commands = BeginTestCommands(vk);
        for (const TestImage& image : images) {
          for (std::uint32_t mip = 0; mip < mips; ++mip) {
            UploadMip(&emulation, vk, commands.buffer, sources[mip].buffer,
                      image.image, mip, std::max(1U, 128U >> mip));
          }
        }
        vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(
            commands.buffer);
        const auto start = std::chrono::steady_clock::now();
        const Etc2SubmitWait wait =
            emulation.PrepareSubmit(&commands.buffer, 1, true, vk.queue);
        times.push_back(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count());
        EXPECT_EQ(wait.semaphore, VK_NULL_HANDLE);
        std::vector<VkCommandBuffer> submitted = wait.decode_commands;
        submitted.push_back(commands.buffer);
        ASSERT_TRUE(SubmitAndWait(vk, submitted));
        emulation.ReleaseCommandBuffer(commands.buffer);
        DestroyTestCommands(vk, commands);
      }
      std::sort(times.begin(), times.end());
      std::printf("%s: %u textures, mips 0-%u: PrepareSubmit median %.3f ms\n",
                  vk.name.c_str(), kTextures, mips - 1, times[times.size() / 2]);
    }
    for (const HostBuffer& source : sources) {
      DestroyEtcSource(&emulation, vk, source);
    }
    for (const TestImage& image : images) {
      DestroyTestImage(&emulation, vk, image);
    }
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Benchmark, run by hand with --gtest_also_run_disabled_tests: one submit
// streaming 256 textures as Roblox does, uploading each full image's top
// mips and copying its 32 px placeholder into the tail, so uploads and image
// copies alternate. Times PrepareSubmit, and the submit until the GPU is done.
TEST(VulkanEtc2GpuEmulationTest, DISABLED_BenchmarkStreamingUploadsAndCopies) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  for (VulkanTestDevice& vk : devices) {
    if (!vk.bc_textures || !vk.timeline) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    StartBcEmulation(&emulation, vk, true);
    constexpr std::uint32_t kTextures = 256;
    std::vector<TestImage> fulls;
    std::vector<TestImage> placeholders;
    for (std::uint32_t i = 0; i < kTextures; ++i) {
      fulls.push_back(CreateTestImage(
          &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, 3));
      placeholders.push_back(CreateTestImage(
          &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1));
    }
    const HostBuffer source128 = CreateRandomEtcSource(&emulation, vk, 128);
    const HostBuffer source64 = CreateRandomEtcSource(&emulation, vk, 64);
    const HostBuffer source32 = CreateRandomEtcSource(&emulation, vk, 32);
    TestCommands prepare = BeginTestCommands(vk);
    for (const TestImage& placeholder : placeholders) {
      UploadLevel(&emulation, vk, prepare.buffer, source32.buffer,
                  placeholder.image, 32);
    }
    EndAndSubmit(&emulation, vk, prepare.buffer);

    std::vector<double> prepare_ms;
    std::vector<double> total_ms;
    for (int run = 0; run < 7; ++run) {
      TestCommands commands = BeginTestCommands(vk);
      for (std::uint32_t i = 0; i < kTextures; ++i) {
        UploadMip(&emulation, vk, commands.buffer, source128.buffer,
                  fulls[i].image, 0, 128);
        UploadMip(&emulation, vk, commands.buffer, source64.buffer,
                  fulls[i].image, 1, 64);
        ImageBarrier(Emu(emulation), vk, commands.buffer, fulls[i].image, 1,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT, 2);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
        region.extent = {32, 32, 1};
        emulation.CmdCopyImage(vk.device, commands.buffer,
                               placeholders[i].image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               fulls[i].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ImageBarrier(Emu(emulation), vk, commands.buffer, fulls[i].image, 1,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     2);
      }
      vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
      const auto start = std::chrono::steady_clock::now();
      const Etc2SubmitWait wait =
          emulation.PrepareSubmit(&commands.buffer, 1, true, vk.queue);
      prepare_ms.push_back(std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start)
                               .count());
      std::vector<VkCommandBuffer> submitted = wait.decode_commands;
      submitted.push_back(commands.buffer);
      if (wait.semaphore != VK_NULL_HANDLE) {
        VkSemaphoreWaitInfo wait_info{};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &wait.semaphore;
        wait_info.pValues = &wait.value;
        ASSERT_EQ(vk.Device<PFN_vkWaitSemaphores>("vkWaitSemaphores")(
                      vk.device, &wait_info, UINT64_MAX),
                  VK_SUCCESS);
      }
      ASSERT_TRUE(SubmitAndWait(vk, submitted));
      total_ms.push_back(std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count());
      emulation.ReleaseCommandBuffer(commands.buffer);
      DestroyTestCommands(vk, commands);
    }
    std::sort(prepare_ms.begin(), prepare_ms.end());
    std::sort(total_ms.begin(), total_ms.end());
    std::printf("%s: %u textures streamed: PrepareSubmit median %.3f ms, "
                "until done %.3f ms\n",
                vk.name.c_str(), kTextures, prepare_ms[3], total_ms[3]);
    emulation.ReleaseCommandBuffer(prepare.buffer);
    DestroyTestCommands(vk, prepare);
    DestroyEtcSource(&emulation, vk, source128);
    DestroyEtcSource(&emulation, vk, source64);
    DestroyEtcSource(&emulation, vk, source32);
    for (const TestImage& image : fulls) {
      DestroyTestImage(&emulation, vk, image);
    }
    for (const TestImage& image : placeholders) {
      DestroyTestImage(&emulation, vk, image);
    }
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A small texture filled by a copy from another small one, then copied into
// the tail mip of a full-size BC7 image, keeps its texels on the way.
TEST(VulkanEtc2GpuEmulationTest, CopiesACopiedPlaceholderIntoATailMip) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    constexpr std::uint32_t kBig = 128;
    constexpr std::uint32_t kSmall = 32;
    const TestImage first = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSmall, 1);
    const TestImage second = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kSmall, 1);
    const TestImage big = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, kBig, 3);
    const HostBuffer source = CreateRandomEtcSource(&emulation, vk, kSmall);
    HostBuffer readback = CreateHostBuffer(vk, Bc7MipBytes(kSmall),
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, commands.buffer, source.buffer, first.image,
                kSmall);
    ImageBarrier(Emu(emulation), vk, commands.buffer, second.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {kSmall, kSmall, 1};
    emulation.CmdCopyImage(vk.device, commands.buffer, first.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, second.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, commands.buffer, second.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    ImageBarrier(Emu(emulation), vk, commands.buffer, big.image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    region.dstSubresource.mipLevel = 2;
    emulation.CmdCopyImage(vk.device, commands.buffer, second.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, big.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, commands.buffer, big.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 2);
    ReadMipBc7(vk, commands.buffer, big.image, 2, kSmall, readback.buffer, 0);
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, commands.buffer));

    UploadCase c;
    c.etc = EtcFormat::kEtc2Rgb8;
    c.width = kSmall;
    c.height = kSmall;
    EXPECT_GE(Psnr(c,
                   DecodeBcReadback(
                       c, static_cast<std::uint8_t*>(readback.mapped)),
                   DecodeOnCpu(c, static_cast<std::uint8_t*>(source.mapped))),
              21.0);

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, big);
    DestroyTestImage(&emulation, vk, second);
    DestroyTestImage(&emulation, vk, first);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// An upload followed in its submit only by a copy between other images is
// still upscaled: its host image matches the same upload submitted alone.
TEST(VulkanEtc2GpuEmulationTest, UpscalesAnUploadFollowedByAnUnrelatedCopy) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const std::uint32_t host = 32 * kTestUpscale;
    const VkDeviceSize bytes = VkDeviceSize{host / 4} * (host / 4) * 16;
    const TestImage control = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const TestImage first = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const TestImage other = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const TestImage sink = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const HostBuffer source = CreateRandomEtcSource(&emulation, vk, 32);
    HostBuffer readback =
        CreateHostBuffer(vk, bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);
    const auto copy_back = vk.Device<PFN_vkCmdCopyImageToBuffer>(
        "vkCmdCopyImageToBuffer");
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.imageExtent = {host, host, 1};

    TestCommands alone = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, alone.buffer, source.buffer, control.image,
                32);
    ImageBarrier(Emu(emulation), vk, alone.buffer, control.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    copy_back(alone.buffer, control.image,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &back);
    HostReadBarrier(vk, alone.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(alone.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, alone.buffer));

    TestCommands commands = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, commands.buffer, source.buffer, first.image,
                32);
    ImageBarrier(Emu(emulation), vk, commands.buffer, other.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_READ_BIT);
    ImageBarrier(Emu(emulation), vk, commands.buffer, sink.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {32, 32, 1};
    emulation.CmdCopyImage(vk.device, commands.buffer, other.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sink.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, commands.buffer, first.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    back.bufferOffset = bytes;
    copy_back(commands.buffer, first.image,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &back);
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, commands.buffer));

    const auto* blocks = static_cast<const std::uint8_t*>(readback.mapped);
    EXPECT_NE(std::count(blocks, blocks + bytes, 0), static_cast<long>(bytes));
    EXPECT_EQ(std::memcmp(blocks, blocks + bytes, bytes), 0);

    emulation.ReleaseCommandBuffer(alone.buffer);
    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, alone);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, sink);
    DestroyTestImage(&emulation, vk, other);
    DestroyTestImage(&emulation, vk, first);
    DestroyTestImage(&emulation, vk, control);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Copies between two upscaled BC7 images stay compressed, on host bounds.
TEST(VulkanEtc2GpuEmulationTest, CopiesBetweenUpscaledCompressedImages) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const std::uint32_t host = 32 * kTestUpscale;
    const VkDeviceSize bytes = VkDeviceSize{host / 4} * (host / 4) * 16;
    const TestImage first = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const TestImage second = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const HostBuffer source = CreateRandomEtcSource(&emulation, vk, 32);
    HostBuffer readback =
        CreateHostBuffer(vk, bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands commands = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, commands.buffer, source.buffer, first.image,
                32);
    ImageBarrier(Emu(emulation), vk, commands.buffer, second.image, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {32, 32, 1};
    emulation.CmdCopyImage(vk.device, commands.buffer, first.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, second.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(Emu(emulation), vk, commands.buffer, second.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.imageExtent = {host, host, 1};
    const auto copy_back = vk.Device<PFN_vkCmdCopyImageToBuffer>(
        "vkCmdCopyImageToBuffer");
    copy_back(commands.buffer, first.image,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &back);
    back.bufferOffset = bytes;
    copy_back(commands.buffer, second.image,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &back);
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, commands.buffer));

    const auto* blocks = static_cast<const std::uint8_t*>(readback.mapped);
    EXPECT_EQ(std::memcmp(blocks, blocks + bytes, bytes), 0);
    EXPECT_NE(std::count(blocks, blocks + bytes, 0), static_cast<long>(bytes));

    emulation.ReleaseCommandBuffer(commands.buffer);
    DestroyTestCommands(vk, commands);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, source);
    DestroyTestImage(&emulation, vk, first);
    DestroyTestImage(&emulation, vk, second);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// A higher mip recorded before its level 0 still takes the override level 0
// picks, even when another image's uploads split the submit into runs.
TEST(VulkanEtc2GpuEmulationTest, MipsTakeTheOverrideOfALaterLevelZero) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("mocktail-overrides-order-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    RgbaImage replacement;
    replacement.width = 2;
    replacement.height = 2;
    replacement.pixels.assign(16, 255);
    // Level 0's texels, and the override named for them.
    const std::vector<std::uint8_t> level0_bytes(512, 0x33);
    ASSERT_TRUE(WritePngRgba(
        (dir / (HashName(HashBytes(level0_bytes.data(), level0_bytes.size())) +
                "_32x32.png"))
            .string(),
        replacement));
    ASSERT_EQ(setenv("MOCKTAIL_TEXTURE_OVERRIDE_DIR", dir.c_str(), 1), 0);
    VulkanEtc2Emulation emulation;
    // The PNG loads on first use, so the folder stays until the end.
    ASSERT_EQ(unsetenv("MOCKTAIL_TEXTURE_OVERRIDE_DIR"), 0);
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, false);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const TestImage image = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 2);
    const TestImage other = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const HostBuffer level0 = CreateRandomEtcSource(&emulation, vk, 32);
    std::memcpy(level0.mapped, level0_bytes.data(), level0_bytes.size());
    const HostBuffer level1 = CreateRandomEtcSource(&emulation, vk, 16);
    const HostBuffer other_source = CreateRandomEtcSource(&emulation, vk, 32);
    // Other texels, so the other image matches no override.
    std::memset(other_source.mapped, 0x5a, 512);
    // Its own levels keep their size below the upscaled top levels.
    const std::uint32_t host1 = 16;
    HostBuffer readback = CreateHostBuffer(
        vk, VkDeviceSize{host1} * host1 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BindAndMap(vk, &readback);

    TestCommands uploads = BeginTestCommands(vk);
    UploadMip(&emulation, vk, uploads.buffer, level1.buffer, image.image, 1,
              16);
    // Two uploads of one mip never share a run, so these split the submit.
    UploadLevel(&emulation, vk, uploads.buffer, other_source.buffer,
                other.image, 32);
    UploadLevel(&emulation, vk, uploads.buffer, other_source.buffer,
                other.image, 32);
    UploadMip(&emulation, vk, uploads.buffer, level0.buffer, image.image, 0,
              32);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
    back.imageExtent = {host1, host1, 1};
    emulation.CmdCopyImageToBuffer(vk.device, uploads.buffer, image.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.buffer, 1, &back);
    HostReadBarrier(vk, uploads.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(uploads.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, uploads.buffer));

    const auto* got = static_cast<const std::uint8_t*>(readback.mapped);
    EXPECT_GT(MeanRgb(std::vector<std::uint8_t>(
                  got, got + std::size_t{host1} * host1 * 4)),
              250.0);

    emulation.ReleaseCommandBuffer(uploads.buffer);
    DestroyTestCommands(vk, uploads);
    DestroyHostBuffer(vk, readback);
    DestroyEtcSource(&emulation, vk, level0);
    DestroyEtcSource(&emulation, vk, level1);
    DestroyEtcSource(&emulation, vk, other_source);
    DestroyTestImage(&emulation, vk, other);
    DestroyTestImage(&emulation, vk, image);
    emulation.DestroyDevice(vk.device);
    std::filesystem::remove_all(dir);
  }
  CloseTestDevices(&devices);
}

// An override named for a 32x32 texture keeps 32x32 images upscaled RGBA8
// for the CPU path; other sizes still become BC7.
TEST(VulkanEtc2GpuEmulationTest, KeepsOnlyOverrideSizedImagesDecoded) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("mocktail-overrides-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    RgbaImage replacement;
    replacement.width = 2;
    replacement.height = 2;
    replacement.pixels.assign(16, 255);
    ASSERT_TRUE(WritePngRgba((dir / "75dcd1166a6cbaa9_32x32.png").string(),
                             replacement));
    ASSERT_EQ(setenv("MOCKTAIL_TEXTURE_OVERRIDE_DIR", dir.c_str(), 1), 0);
    VulkanEtc2Emulation emulation;
    std::filesystem::remove_all(dir);
    ASSERT_EQ(unsetenv("MOCKTAIL_TEXTURE_OVERRIDE_DIR"), 0);
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    image_info.extent = {32, 32, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image = VK_NULL_HANDLE;
    ASSERT_EQ(emulation.CreateImage(vk.device, &image_info, nullptr, &image),
              VK_SUCCESS);
    image_info.extent = {16, 16, 1};
    VkImage other = VK_NULL_HANDLE;
    ASSERT_EQ(emulation.CreateImage(vk.device, &image_info, nullptr, &other),
              VK_SUCCESS);
    // Each image: two top levels and its own level, so 21/16 of the top.
    const VkDeviceSize upscaled = 32 * kTestUpscale;
    const VkDeviceSize other_bc7 = 16 * kTestUpscale;
    EXPECT_EQ(emulation.TextureBytes().decoded,
              (upscaled * upscaled * 4 + other_bc7 * other_bc7) * 21 / 16);
    emulation.DestroyImage(vk.device, other, nullptr);
    emulation.DestroyImage(vk.device, image, nullptr);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Drivers rarely support BC with linear tiling, so linear images stay RGBA8.
TEST(VulkanEtc2GpuEmulationTest, KeepsLinearImagesDecoded) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (!vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, true);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    image_info.extent = {128, 128, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image = VK_NULL_HANDLE;
    ASSERT_EQ(emulation.CreateImage(vk.device, &image_info, nullptr, &image),
              VK_SUCCESS);
    EXPECT_EQ(emulation.TextureBytes().decoded, VkDeviceSize{128} * 128 * 4);
    emulation.DestroyImage(vk.device, image, nullptr);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// New random blocks in a source, so two sources of one size differ.
void Reseed(const HostBuffer& source, std::uint32_t size, std::uint64_t seed) {
  std::mt19937_64 random(seed);
  auto* out = static_cast<std::uint8_t*>(source.mapped);
  for (VkDeviceSize i = 0; i < VkDeviceSize{size / 4} * (size / 4) * 8; ++i) {
    out[i] = static_cast<std::uint8_t>(random());
  }
}

std::vector<std::uint8_t> DecodeRgb8(const HostBuffer& source,
                                     std::uint32_t size) {
  UploadCase c;
  c.etc = EtcFormat::kEtc2Rgb8;
  c.width = size;
  c.height = size;
  return DecodeOnCpu(c, static_cast<const std::uint8_t*>(source.mapped));
}

// What a BC7 round trip of the random test texels keeps, against the CPU
// upscale; a level with the wrong picture scores far below it.
constexpr double kBc7Psnr = 28.0;

// The levels above a 32 px image's own must be the CPU upscale of what its
// level 0 holds now, however that level was last written.
void ExpectTopLevelsOf(VulkanEtc2Emulation* emulation,
                       const VulkanTestDevice& vk, VkImage image,
                       const std::vector<std::uint8_t>& level0,
                       double min_psnr = 50.0, bool compressed = false) {
  const std::uint32_t host = 32 * kTestUpscale;
  const VkDeviceSize bytes0 =
      compressed ? Bc7MipBytes(host) : VkDeviceSize{host} * host * 4;
  const VkDeviceSize bytes1 =
      compressed ? Bc7MipBytes(host / 2) : bytes0 / 4;
  HostBuffer readback = CreateHostBuffer(vk, bytes0 + bytes1,
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  BindAndMap(vk, &readback);
  TestCommands commands = BeginTestCommands(vk);
  if (compressed) {
    ReadMipBc7(vk, commands.buffer, image, 0, host, readback.buffer, 0);
    ReadMipBc7(vk, commands.buffer, image, 1, host / 2, readback.buffer,
               bytes0);
  } else {
    const auto copy_back =
        vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.imageExtent = {host, host, 1};
    copy_back(commands.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              readback.buffer, 1, &back);
    back.bufferOffset = bytes0;
    back.imageSubresource.mipLevel = 1;
    back.imageExtent = {host / 2, host / 2, 1};
    copy_back(commands.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              readback.buffer, 1, &back);
  }
  HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT);
  vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
  ASSERT_TRUE(SubmitThroughEmulation(emulation, vk, commands.buffer));
  const auto* got = static_cast<const std::uint8_t*>(readback.mapped);
  for (const std::uint32_t size : {host, host / 2}) {
    SCOPED_TRACE(size);
    std::vector<std::uint8_t> expected(std::size_t{size} * size * 4);
    UpscaleRgba(level0.data(), 32, 32, 32 * 4, size, size, expected.data());
    const std::uint8_t* at = got + (size == host ? 0 : bytes0);
    UploadCase scaled;
    scaled.etc = EtcFormat::kEtc2Rgb8;
    scaled.width = size;
    scaled.height = size;
    const std::vector<std::uint8_t> actual =
        compressed ? DecodeBcReadback(scaled, at)
                   : std::vector<std::uint8_t>(at, at + expected.size());
    EXPECT_GE(Psnr(scaled, actual, expected), min_psnr);
  }
  emulation->ReleaseCommandBuffer(commands.buffer);
  DestroyTestCommands(vk, commands);
  DestroyHostBuffer(vk, readback);
}

// How a new picture reaches a 32 px texture's level 0 after its first
// upload: Roblox reuses textures and moves streamed levels between images.
// kCopyFromLargeMip copies from a mip too large to keep its blocks;
// kCopyAfterMove copies from a mip last written by an image copy.
// kPartUpload replaces the middle 16 x 16 texels only.
// kCopyThenPartUpload copies a new level in, then replaces its middle, in
// one command buffer. kCopyFromPieces copies from a mip uploaded in two
// halves, so it has no kept blocks.
enum class LevelZeroWrite {
  kUpload,
  kPartUpload,
  kCopy,
  kCopyFromLargeMip,
  kCopyAfterMove,
  kCopyThenPartUpload,
  kCopyFromPieces
};

// The 32 x 32 texels at (x, y) of a decoded `size` px RGBA level.
std::vector<std::uint8_t> Crop32(const std::vector<std::uint8_t>& decoded,
                                 std::uint32_t size, std::uint32_t x,
                                 std::uint32_t y) {
  std::vector<std::uint8_t> out(std::size_t{32} * 32 * 4);
  for (std::uint32_t row = 0; row < 32; ++row) {
    std::memcpy(out.data() + std::size_t{row} * 32 * 4,
                decoded.data() + (std::size_t{y + row} * size + x) * 4,
                std::size_t{32} * 4);
  }
  return out;
}

// With `compressed`, the images are BC7 and the levels are checked after
// the BC7 round trip, so the bar is lower.
void ExpectTopLevelsFollowLevelZero(LevelZeroWrite write,
                                    bool compressed = false) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    if (compressed && !vk.bc_textures) {
      continue;
    }
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, compressed);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const TestImage small = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 2);
    // Past every upscale limit, so its levels stay as uploaded.
    const TestImage big = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, 3);
    const HostBuffer first = CreateRandomEtcSource(&emulation, vk, 32);
    const HostBuffer second = CreateRandomEtcSource(&emulation, vk, 32);
    Reseed(second, 32, 7);
    const HostBuffer large = CreateRandomEtcSource(&emulation, vk, 128);
    const TestImage donor = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 128, 3);
    std::vector<std::uint8_t> expected = DecodeRgb8(second, 32);
    // A blit fallback filters differently from the upload's upscale.
    double min_psnr = compressed ? kBc7Psnr : 50.0;
    std::uint32_t source_mip = 2;
    VkOffset3D source_offset{};

    // Fills the big image's level first, so its kept blocks are in place.
    TestCommands prepare = BeginTestCommands(vk);
    if (write == LevelZeroWrite::kCopyFromLargeMip) {
      UploadMip(&emulation, vk, prepare.buffer, large.buffer, big.image, 0,
                128);
      expected = Crop32(DecodeRgb8(large, 128), 128, 32, 32);
      min_psnr = 20.0;
      source_mip = 0;
      source_offset = {32, 32, 0};
    } else if (write == LevelZeroWrite::kCopyFromPieces) {
      ImageBarrier(Emu(emulation), vk, prepare.buffer, big.image, 1,
                   VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, 2);
      VkBufferImageCopy halves[2]{};
      for (std::uint32_t half = 0; half < 2; ++half) {
        halves[half].bufferOffset = VkDeviceSize{half} * 4 * 8 * 8;
        halves[half].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
        halves[half].imageOffset = {0, static_cast<std::int32_t>(half * 16), 0};
        halves[half].imageExtent = {32, 16, 1};
      }
      emulation.CmdCopyBufferToImage(vk.device, prepare.buffer, second.buffer,
                                     big.image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2,
                                     halves);
      ImageBarrier(Emu(emulation), vk, prepare.buffer, big.image, 1,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 2);
      // Falls back to a blit, which filters differently.
      min_psnr = compressed ? kBc7Psnr : 20.0;
    } else if (write == LevelZeroWrite::kCopyAfterMove) {
      UploadMip(&emulation, vk, prepare.buffer, first.buffer, big.image, 2, 32);
      UploadMip(&emulation, vk, prepare.buffer, second.buffer, donor.image, 2,
                32);
      ImageBarrier(Emu(emulation), vk, prepare.buffer, big.image, 1,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 2);
      VkImageCopy move{};
      move.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
      move.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
      move.extent = {32, 32, 1};
      emulation.CmdCopyImage(vk.device, prepare.buffer, donor.image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, big.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &move);
      ImageBarrier(Emu(emulation), vk, prepare.buffer, big.image, 1,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 2);
    }
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(prepare.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, prepare.buffer));

    TestCommands commands = BeginTestCommands(vk);
    UploadLevel(&emulation, vk, commands.buffer, first.buffer, small.image, 32);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, commands.buffer));
    ExpectTopLevelsOf(&emulation, vk, small.image, DecodeRgb8(first, 32),
                      compressed ? kBc7Psnr : 50.0, compressed);

    TestCommands again = BeginTestCommands(vk);
    const HostBuffer part = CreateRandomEtcSource(&emulation, vk, 16);
    if (write == LevelZeroWrite::kUpload) {
      UploadLevel(&emulation, vk, again.buffer, second.buffer, small.image, 32);
    } else if (write == LevelZeroWrite::kPartUpload) {
      ImageBarrier(Emu(emulation), vk, again.buffer, small.image, 1,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageOffset = {8, 8, 0};
      region.imageExtent = {16, 16, 1};
      emulation.CmdCopyBufferToImage(vk.device, again.buffer, part.buffer,
                                     small.image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                     &region);
      ImageBarrier(Emu(emulation), vk, again.buffer, small.image, 1,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      expected = DecodeRgb8(first, 32);
      const std::vector<std::uint8_t> patch = DecodeRgb8(part, 16);
      for (std::uint32_t y = 0; y < 16; ++y) {
        std::memcpy(&expected[(std::size_t{y + 8} * 32 + 8) * 4],
                    &patch[std::size_t{y} * 16 * 4], 16 * 4);
      }
    } else {
      if (write == LevelZeroWrite::kCopy ||
          write == LevelZeroWrite::kCopyThenPartUpload) {
        UploadMip(&emulation, vk, again.buffer, second.buffer, big.image, 2,
                  32);
        ImageBarrier(Emu(emulation), vk, again.buffer, big.image, 1,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     2);
      }
      ImageBarrier(Emu(emulation), vk, again.buffer, small.image, 1,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      VkImageCopy region{};
      region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, source_mip, 0, 1};
      region.srcOffset = source_offset;
      region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.extent = {32, 32, 1};
      emulation.CmdCopyImage(vk.device, again.buffer, big.image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, small.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      ImageBarrier(Emu(emulation), vk, again.buffer, small.image, 1,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      if (write == LevelZeroWrite::kCopyThenPartUpload) {
        ImageBarrier(Emu(emulation), vk, again.buffer, small.image, 1,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy patch_region{};
        patch_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        patch_region.imageOffset = {8, 8, 0};
        patch_region.imageExtent = {16, 16, 1};
        emulation.CmdCopyBufferToImage(vk.device, again.buffer, part.buffer,
                                       small.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &patch_region);
        ImageBarrier(Emu(emulation), vk, again.buffer, small.image, 1,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const std::vector<std::uint8_t> patch = DecodeRgb8(part, 16);
        for (std::uint32_t y = 0; y < 16; ++y) {
          std::memcpy(&expected[(std::size_t{y + 8} * 32 + 8) * 4],
                      &patch[std::size_t{y} * 16 * 4], 16 * 4);
        }
      }
    }
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(again.buffer);
    ASSERT_TRUE(SubmitThroughEmulation(&emulation, vk, again.buffer));
    ExpectTopLevelsOf(&emulation, vk, small.image, expected, min_psnr,
                      compressed);

    emulation.ReleaseCommandBuffer(commands.buffer);
    emulation.ReleaseCommandBuffer(prepare.buffer);
    emulation.ReleaseCommandBuffer(again.buffer);
    DestroyTestCommands(vk, commands);
    DestroyTestCommands(vk, prepare);
    DestroyTestCommands(vk, again);
    DestroyEtcSource(&emulation, vk, first);
    DestroyEtcSource(&emulation, vk, second);
    DestroyEtcSource(&emulation, vk, large);
    DestroyEtcSource(&emulation, vk, part);
    DestroyTestImage(&emulation, vk, donor);
    DestroyTestImage(&emulation, vk, big);
    DestroyTestImage(&emulation, vk, small);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

// Uploads of one mip in one submit keep the last one's blocks, which a part
// upload in the next submit is scaled with. The uploads alternate between
// two pictures, so a race between them shows up as the wrong picture.
TEST(VulkanEtc2GpuEmulationTest, KeepsTheLastOfSeveralUploadsOfOneMip) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a graphics and compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(vk.device, vk.physical_device, true, vk.memory,
                             vk.get_device_proc_addr, false, false);
    emulation.RegisterQueue(vk.device, vk.queue, vk.family);
    ASSERT_TRUE(emulation.EnableGpuDecode(vk.device, vk.queue, vk.family));
    const TestImage small = CreateTestImage(
        &emulation, vk, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 32, 1);
    const HostBuffer first = CreateRandomEtcSource(&emulation, vk, 32);
    const HostBuffer second = CreateRandomEtcSource(&emulation, vk, 32);
    Reseed(second, 32, 7);
    const HostBuffer part = CreateRandomEtcSource(&emulation, vk, 16);

    TestCommands uploads = BeginTestCommands(vk);
    for (int upload = 0; upload < 8; ++upload) {
      UploadLevel(&emulation, vk, uploads.buffer,
                  (upload % 2 == 0 ? first : second).buffer, small.image, 32);
    }
    EndAndSubmit(&emulation, vk, uploads.buffer);

    TestCommands patch = BeginTestCommands(vk);
    ImageBarrier(Emu(emulation), vk, patch.buffer, small.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {8, 8, 0};
    region.imageExtent = {16, 16, 1};
    emulation.CmdCopyBufferToImage(vk.device, patch.buffer, part.buffer,
                                   small.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
    ImageBarrier(Emu(emulation), vk, patch.buffer, small.image, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    EndAndSubmit(&emulation, vk, patch.buffer);

    std::vector<std::uint8_t> expected = DecodeRgb8(second, 32);
    const std::vector<std::uint8_t> patched = DecodeRgb8(part, 16);
    for (std::uint32_t y = 0; y < 16; ++y) {
      std::memcpy(&expected[(std::size_t{y + 8} * 32 + 8) * 4],
                  &patched[std::size_t{y} * 16 * 4], 16 * 4);
    }
    ExpectTopLevelsOf(&emulation, vk, small.image, expected);

    emulation.ReleaseCommandBuffer(uploads.buffer);
    emulation.ReleaseCommandBuffer(patch.buffer);
    DestroyTestCommands(vk, uploads);
    DestroyTestCommands(vk, patch);
    DestroyEtcSource(&emulation, vk, first);
    DestroyEtcSource(&emulation, vk, second);
    DestroyEtcSource(&emulation, vk, part);
    DestroyTestImage(&emulation, vk, small);
    emulation.DestroyDevice(vk.device);
  }
  CloseTestDevices(&devices);
}

TEST(VulkanEtc2GpuEmulationTest, TopLevelsFollowAReuploadedLevelZero) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kUpload);
}

TEST(VulkanEtc2GpuEmulationTest, TopLevelsFollowAPartlyUploadedLevelZero) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kPartUpload);
}

TEST(VulkanEtc2GpuEmulationTest, CompressedTopLevelsFollowAPartUpload) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kPartUpload, true);
}

TEST(VulkanEtc2GpuEmulationTest, TopLevelsFollowALevelZeroCopiedIn) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kCopy);
}

TEST(VulkanEtc2GpuEmulationTest, TopLevelsFollowACopyFromALargeMip) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kCopyFromLargeMip);
}

TEST(VulkanEtc2GpuEmulationTest, TopLevelsFollowACopyFromAMovedMip) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kCopyAfterMove);
}

TEST(VulkanEtc2GpuEmulationTest, TopLevelsFollowACopyThenAPartUpload) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kCopyThenPartUpload);
}

TEST(VulkanEtc2GpuEmulationTest, TopLevelsFollowACopyFromAMipUploadedInPieces) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kCopyFromPieces);
}

TEST(VulkanEtc2GpuEmulationTest, CompressedTopLevelsFollowACopyThenAPartUpload) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kCopyThenPartUpload, true);
}

TEST(VulkanEtc2GpuEmulationTest, CompressedTopLevelsFollowAReupload) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kUpload, true);
}

TEST(VulkanEtc2GpuEmulationTest, CompressedTopLevelsFollowACopy) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kCopy, true);
}

TEST(VulkanEtc2GpuEmulationTest, CompressedTopLevelsFollowACopyFromAMovedMip) {
  ExpectTopLevelsFollowLevelZero(LevelZeroWrite::kCopyAfterMove, true);
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
