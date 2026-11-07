#include "mocktail/graphics/etc2_gpu_decoder.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "mocktail/graphics/bc_decoder.h"
#include "vulkan_test_device.h"

namespace mocktail::graphics {
namespace {

using mocktail::testing::BindAndMap;
using mocktail::testing::BeginTestCommands;
using mocktail::testing::CloseTestDevices;
using mocktail::testing::CreateHostBuffer;
using mocktail::testing::DestroyHostBuffer;
using mocktail::testing::DestroyTestCommands;
using mocktail::testing::HostBuffer;
using mocktail::testing::HostReadBarrier;
using mocktail::testing::OpenTestDevices;
using mocktail::testing::SubmitAndWait;
using mocktail::testing::TestCommands;
using mocktail::testing::VulkanTestDevice;

bool RunOnGpu(const VulkanTestDevice& vk, Etc2GpuDecoder& decoder,
              const HostBuffer& source, const HostBuffer& destination,
              const Etc2GpuJob& job) {
  Etc2GpuBinding binding;
  if (!decoder.CreateBinding(source.buffer, destination.buffer, &binding)) {
    return false;
  }
  TestCommands commands = BeginTestCommands(vk);
  const bool recorded = decoder.Record(commands.buffer, binding, &job, 1);
  HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT);
  vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
  const bool ran = recorded && SubmitAndWait(vk, {commands.buffer});
  DestroyTestCommands(vk, commands);
  decoder.DestroyBinding(binding);
  return ran;
}

void ExpectMatchesCpu(EtcFormat format, std::uint32_t width,
                      std::uint32_t height, std::uint32_t layers,
                      std::uint64_t seed) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a compute queue";
  }
  const std::uint32_t blocks_wide = (width + 3) / 4;
  const std::uint32_t blocks_high = (height + 3) / 4;
  const std::size_t block_bytes = EtcBlockBytes(format);
  const std::size_t texel_bytes = EtcDecodedTexelBytes(format);

  Etc2GpuJob job;
  job.format = format;
  job.blocks_wide = blocks_wide;
  job.blocks_high = blocks_high;
  job.layers = layers;
  job.source_offset = 64;
  job.source_row_blocks = blocks_wide + 3;
  job.source_layer_blocks = job.source_row_blocks * blocks_high + 5;
  job.destination_offset = 256;

  const std::size_t source_bytes =
      job.source_offset + job.source_layer_blocks * layers * block_bytes;
  std::vector<std::uint8_t> source(source_bytes);
  std::mt19937_64 random(seed);
  for (std::uint8_t& byte : source) {
    byte = static_cast<std::uint8_t>(random());
  }

  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    std::string error;
    std::unique_ptr<Etc2GpuDecoder> decoder =
        Etc2GpuDecoder::Create(vk.device, vk.get_device_proc_addr, &error);
    ASSERT_NE(decoder, nullptr) << error;
    const VkDeviceSize destination_bytes =
        job.destination_offset + Etc2GpuOutputBytes(job);
    HostBuffer src = CreateHostBuffer(vk, source_bytes,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    HostBuffer dst = CreateHostBuffer(vk, destination_bytes,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    BindAndMap(vk, &src);
    BindAndMap(vk, &dst);
    std::memcpy(src.mapped, source.data(), source_bytes);
    ASSERT_TRUE(RunOnGpu(vk, *decoder, src, dst, job));

    const auto* gpu = static_cast<const std::uint8_t*>(dst.mapped);
    const std::size_t row_bytes = Etc2GpuRowTexels(blocks_wide) * texel_bytes;
    std::vector<std::uint8_t> tight(std::size_t{blocks_wide} * blocks_high *
                                    block_bytes);
    std::vector<std::uint8_t> expected(std::size_t{width} * height *
                                       texel_bytes);
    std::size_t mismatched_rows = 0;
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
      for (std::uint32_t row = 0; row < blocks_high; ++row) {
        std::memcpy(tight.data() + std::size_t{row} * blocks_wide * block_bytes,
                    source.data() + job.source_offset +
                        (std::size_t{layer} * job.source_layer_blocks +
                         std::size_t{row} * job.source_row_blocks) *
                            block_bytes,
                    std::size_t{blocks_wide} * block_bytes);
      }
      ASSERT_TRUE(DecodeEtcImage(format, tight.data(), tight.size(), width,
                                 height, expected.data(), expected.size()));
      const std::uint8_t* layer_out =
          gpu + job.destination_offset +
          layer * Etc2GpuLayerBytes(format, blocks_wide, blocks_high);
      for (std::uint32_t y = 0; y < height; ++y) {
        if (std::memcmp(layer_out + y * row_bytes,
                        expected.data() + std::size_t{y} * width * texel_bytes,
                        std::size_t{width} * texel_bytes) != 0) {
          ++mismatched_rows;
        }
      }
    }
    EXPECT_EQ(mismatched_rows, 0U);
    DestroyHostBuffer(vk, src);
    DestroyHostBuffer(vk, dst);
  }
  CloseTestDevices(&devices);
}

TEST(Etc2GpuDecoderTest, PadsRowsAndLayersToWholeBlocks) {
  EXPECT_EQ(Etc2GpuRowTexels(3), 12U);
  EXPECT_EQ(Etc2GpuLayerBytes(EtcFormat::kEtc2Rgb8, 3, 2), 12U * 8 * 4);
  EXPECT_EQ(Etc2GpuLayerBytes(EtcFormat::kEacR11, 3, 2), 12U * 8 * 2);
  EXPECT_EQ(Etc2GpuLayerBytes(EtcFormat::kEacRg11Signed, 3, 2), 12U * 8 * 4);
  Etc2GpuJob job;
  job.format = EtcFormat::kEtc2Rgba8;
  job.blocks_wide = 3;
  job.blocks_high = 2;
  job.layers = 5;
  EXPECT_EQ(Etc2GpuOutputBytes(job), 5U * 12 * 8 * 4);
}

TEST(Etc2GpuDecoderTest, Rgb8MatchesCpuOnAMillionRandomBlocks) {
  ExpectMatchesCpu(EtcFormat::kEtc2Rgb8, 4095, 2046, 2, 1);
}

TEST(Etc2GpuDecoderTest, Rgb8A1MatchesCpu) {
  ExpectMatchesCpu(EtcFormat::kEtc2Rgb8A1, 1023, 1022, 2, 2);
}

TEST(Etc2GpuDecoderTest, Rgba8MatchesCpu) {
  ExpectMatchesCpu(EtcFormat::kEtc2Rgba8, 1023, 1022, 2, 3);
}

TEST(Etc2GpuDecoderTest, EacR11MatchesCpu) {
  ExpectMatchesCpu(EtcFormat::kEacR11, 1023, 1022, 2, 4);
}

TEST(Etc2GpuDecoderTest, EacR11SignedMatchesCpu) {
  ExpectMatchesCpu(EtcFormat::kEacR11Signed, 1023, 1022, 2, 5);
}

TEST(Etc2GpuDecoderTest, EacRg11MatchesCpu) {
  ExpectMatchesCpu(EtcFormat::kEacRg11, 1023, 1022, 2, 6);
}

TEST(Etc2GpuDecoderTest, EacRg11SignedMatchesCpu) {
  ExpectMatchesCpu(EtcFormat::kEacRg11Signed, 1023, 1022, 2, 7);
}

TEST(Etc2GpuDecoderTest, DecodesTinyImagesAndMipTails) {
  ExpectMatchesCpu(EtcFormat::kEtc2Rgb8, 1, 1, 1, 8);
  ExpectMatchesCpu(EtcFormat::kEtc2Rgba8, 2, 2, 3, 9);
  ExpectMatchesCpu(EtcFormat::kEacR11, 5, 3, 1, 10);
}

// PSNR of a tight single-layer BC7 transcode against the CPU decode of its
// source blocks, or -1 when a block fails to decode.
double TranscodePsnr(const Etc2GpuJob& job, const std::uint8_t* source,
                     const std::uint8_t* output) {
  std::uint8_t want[64];
  std::uint8_t got[64];
  double squared = 0;
  const std::size_t blocks = std::size_t{job.blocks_wide} * job.blocks_high;
  for (std::size_t b = 0; b < blocks; ++b) {
    if (!DecodeEtcImage(job.format, source + b * EtcBlockBytes(job.format),
                        EtcBlockBytes(job.format), 4, 4, want, sizeof(want)) ||
        !DecodeBcBlock(BcFormat::kBc7, output + b * 16, got)) {
      return -1;
    }
    for (int v = 0; v < 64; ++v) {
      const double error = want[v] - got[v];
      squared += error * error;
    }
  }
  return 10 * std::log10(255.0 * 255.0 * blocks * 64 / squared);
}

TEST(Etc2GpuDecoderTest, FastTranscodeStaysCloseToTheFullOne) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a compute queue";
  }
  constexpr std::uint32_t kBlocks = 64;
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    std::string error;
    std::unique_ptr<Etc2GpuDecoder> decoder =
        Etc2GpuDecoder::Create(vk.device, vk.get_device_proc_addr, &error);
    ASSERT_NE(decoder, nullptr) << error;
    if (!decoder->CanTranscode()) {
      continue;
    }
    for (EtcFormat format : {EtcFormat::kEtc2Rgb8, EtcFormat::kEtc2Rgb8A1,
                             EtcFormat::kEtc2Rgba8}) {
      SCOPED_TRACE(static_cast<int>(format));
      Etc2GpuJob job;
      job.format = format;
      job.transcode = true;
      job.blocks_wide = kBlocks;
      job.blocks_high = kBlocks;
      job.source_row_blocks = kBlocks;
      job.source_layer_blocks = kBlocks * kBlocks;
      const std::size_t source_bytes =
          std::size_t{kBlocks} * kBlocks * EtcBlockBytes(format);
      job.destination_offset = source_bytes;
      HostBuffer src = CreateHostBuffer(vk, source_bytes,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      HostBuffer dst = CreateHostBuffer(
          vk, source_bytes + Etc2GpuOutputBytes(job),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      BindAndMap(vk, &src);
      BindAndMap(vk, &dst);
      std::mt19937_64 random(static_cast<int>(format) + 40);
      auto* bytes = static_cast<std::uint8_t*>(src.mapped);
      for (std::size_t i = 0; i < source_bytes; ++i) {
        bytes[i] = static_cast<std::uint8_t>(random());
      }
      const auto* out =
          static_cast<const std::uint8_t*>(dst.mapped) + job.destination_offset;
      const bool full_ran = RunOnGpu(vk, *decoder, src, dst, job);
      const double full = full_ran ? TranscodePsnr(job, bytes, out) : -1;
      job.fast = true;
      const bool fast_ran = RunOnGpu(vk, *decoder, src, dst, job);
      const double fast = fast_ran ? TranscodePsnr(job, bytes, out) : -1;
      DestroyHostBuffer(vk, src);
      DestroyHostBuffer(vk, dst);
      EXPECT_TRUE(full_ran && fast_ran);
      EXPECT_GT(full, 0.0);
      std::printf("format %d: full %.2f dB, fast %.2f dB\n",
                  static_cast<int>(format), full, fast);
      // PERFORMANCE.md promises under 1 dB lost.
      EXPECT_GE(fast, full - 1.0);
    }
  }
  CloseTestDevices(&devices);
}

TEST(Etc2GpuDecoderTest, SelfTestPassesOnEveryDevice) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    std::string error;
    std::unique_ptr<Etc2GpuDecoder> decoder =
        Etc2GpuDecoder::Create(vk.device, vk.get_device_proc_addr, &error);
    ASSERT_NE(decoder, nullptr) << error;
    EXPECT_TRUE(decoder->SelfTest(vk.queue, vk.family, vk.memory, &error))
        << error;
  }
  CloseTestDevices(&devices);
}

TEST(Etc2GpuDecoderTest, MeasuresEncodeSpeedOnEveryDevice) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    std::string error;
    std::unique_ptr<Etc2GpuDecoder> decoder =
        Etc2GpuDecoder::Create(vk.device, vk.get_device_proc_addr, &error);
    ASSERT_NE(decoder, nullptr) << error;
    if (!decoder->CanTranscode() || !decoder->CanScale()) {
      continue;
    }
    Etc2GpuSpeed speed;
    ASSERT_TRUE(decoder->MeasureSpeed(vk.queue, vk.family, vk.memory, true,
                                      &speed, &error))
        << error;
    EXPECT_GT(speed.transcode_ns, 0.0);
    EXPECT_GT(speed.upscale_ns, 0.0);
    EXPECT_GT(speed.fast_transcode_ns, 0.0);
    std::printf(
        "%s: transcode %.2f ns/block, fast %.2f, upscale %.2f ns/block\n",
        vk.name.c_str(), speed.transcode_ns, speed.fast_transcode_ns,
        speed.upscale_ns);
  }
  CloseTestDevices(&devices);
}

TEST(Etc2GpuDecoderTest, MeasuresDecodeAndScaleWithoutEncoding) {
  std::vector<VulkanTestDevice> devices = OpenTestDevices();
  if (devices.empty()) {
    GTEST_SKIP() << "no Vulkan device with a compute queue";
  }
  for (VulkanTestDevice& vk : devices) {
    SCOPED_TRACE(vk.name);
    std::string error;
    std::unique_ptr<Etc2GpuDecoder> decoder =
        Etc2GpuDecoder::Create(vk.device, vk.get_device_proc_addr, &error);
    ASSERT_NE(decoder, nullptr) << error;
    if (!decoder->CanScale()) {
      continue;
    }
    Etc2GpuSpeed speed;
    ASSERT_TRUE(decoder->MeasureSpeed(vk.queue, vk.family, vk.memory, false,
                                      &speed, &error))
        << error;
    EXPECT_EQ(speed.transcode_ns, 0.0);
    EXPECT_GT(speed.upscale_ns, 0.0);
    std::printf("%s: decode and scale %.2f ns/block\n", vk.name.c_str(),
                speed.upscale_ns);
  }
  CloseTestDevices(&devices);
}

TEST(Etc2GpuDecoderTest, CreateFailsWithoutComputeEntryPoints) {
  std::string error;
  const auto none = [](VkDevice, const char*) -> PFN_vkVoidFunction {
    return nullptr;
  };
  EXPECT_EQ(Etc2GpuDecoder::Create(reinterpret_cast<VkDevice>(0x1), none,
                                   &error),
            nullptr);
  EXPECT_FALSE(error.empty());
}

}
}
