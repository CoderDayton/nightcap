#include "mocktail/graphics/vulkan_etc2_emulation.h"

#include "mocktail/graphics/etc2_decoder.h"
#include "mocktail/graphics/texture_override.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

TEST(VulkanEtc2EmulationTest, TranscodesToBcUnlessSwitchedOff) {
  EXPECT_TRUE(Etc2BcTranscodeEnabled(nullptr));
  EXPECT_TRUE(Etc2BcTranscodeEnabled(""));
  EXPECT_TRUE(Etc2BcTranscodeEnabled("1"));
  EXPECT_FALSE(Etc2BcTranscodeEnabled("0"));
}

TEST(VulkanEtc2EmulationTest, ParsesSmallTextureUpscale) {
  EXPECT_EQ(SmallTextureUpscale(nullptr), kAdaptiveSmallTextureUpscale);
  EXPECT_EQ(SmallTextureUpscale(""), kAdaptiveSmallTextureUpscale);
  EXPECT_EQ(SmallTextureUpscale("abc"), kAdaptiveSmallTextureUpscale);
  EXPECT_EQ(SmallTextureUpscale("adaptive"), kAdaptiveSmallTextureUpscale);
  EXPECT_EQ(SmallTextureUpscale("0"), 1u);
  EXPECT_EQ(SmallTextureUpscale("1"), 1u);
  EXPECT_EQ(SmallTextureUpscale("2"), 2u);
  EXPECT_EQ(SmallTextureUpscale("99"), 8u);
}

// Each halving of the factor is one level added above the texture's own.
TEST(VulkanEtc2EmulationTest, RoundsTheUpscaleDownToAPowerOfTwo) {
  EXPECT_EQ(SmallTextureUpscale("3"), 2u);
  EXPECT_EQ(SmallTextureUpscale("4"), 4u);
  EXPECT_EQ(SmallTextureUpscale("7"), 4u);
  EXPECT_EQ(SmallTextureUpscale("8"), 8u);
}

TEST(VulkanEtc2EmulationTest, CountsHostBlocksOfAScaledChain) {
  // 256 and 128 texels square above the texture's own 64 down to 1.
  EXPECT_EQ(Etc2HostBlockCount({64, 64, 1}, 7, 1, 4), 4096u + 1024 + 343);
  // 64 down to 1 texel; the last three levels take a block each.
  EXPECT_EQ(Etc2HostBlockCount({64, 64, 1}, 7, 1, 1), 343u);
  EXPECT_EQ(Etc2HostBlockCount({64, 32, 1}, 1, 3, 2),
            3u * (32 * 16 + 16 * 8));
}

TEST(VulkanEtc2EmulationTest, PicksTheLargestUpscaleTheGpuEncodesInTime) {
  // A 64x64 chain costs at most kAdaptiveUpscaleTextureNs: its own 343
  // blocks transcoded, plus top levels of 5120 blocks at 4x or 1024 at 2x
  // upscaled. Only the factors the setting offers are picked.
  EXPECT_EQ(AdaptiveSmallTextureUpscale(9.2, 9.2), 4u);
  EXPECT_EQ(AdaptiveSmallTextureUpscale(91.0, 91.0), 4u);
  EXPECT_EQ(AdaptiveSmallTextureUpscale(120.0, 120.0), 2u);
  EXPECT_EQ(AdaptiveSmallTextureUpscale(300.0, 300.0), 2u);
  EXPECT_EQ(AdaptiveSmallTextureUpscale(400.0, 400.0), 1u);
  EXPECT_EQ(AdaptiveSmallTextureUpscale(726.0, 726.0), 1u);
  // The own levels are charged at the transcode speed: 5120 * 95 + 343 * 10
  // fits where 5463 * 95 would not, and so does 1024 * 450 + 343 * 10.
  EXPECT_EQ(AdaptiveSmallTextureUpscale(95.0, 10.0), 4u);
  EXPECT_EQ(AdaptiveSmallTextureUpscale(450.0, 10.0), 2u);
  // Without a measurement the card may be too slow for any upscale.
  EXPECT_EQ(AdaptiveSmallTextureUpscale(0.0, 0.0), 1u);
}

TEST(VulkanEtc2EmulationTest, EncodeBudgetRefillsWithWallTime) {
  constexpr std::uint64_t kStart = 1'000'000'000;
  Etc2EncodeBudget budget(0.1, 16e6);
  EXPECT_TRUE(budget.Take(10e6, kStart));
  EXPECT_TRUE(budget.Take(6e6, kStart));
  EXPECT_FALSE(budget.Take(1e3, kStart));
  // 10 ms of wall time refills 1 ms.
  EXPECT_TRUE(budget.Take(1e6, kStart + 10'000'000));
  EXPECT_FALSE(budget.Take(1e3, kStart + 10'000'000));
}

TEST(VulkanEtc2EmulationTest, EncodeBudgetHoldsAtMostItsBurst) {
  constexpr std::uint64_t kStart = 1'000'000'000;
  Etc2EncodeBudget budget(0.1, 16e6);
  EXPECT_TRUE(budget.Take(16e6, kStart));
  // An hour idle still refills only the burst.
  const std::uint64_t later = kStart + 3'600'000'000'000;
  EXPECT_TRUE(budget.Take(16e6, later));
  EXPECT_FALSE(budget.Take(1e3, later));
}

TEST(VulkanEtc2EmulationTest, EncodeBudgetLetsALargeEncodeRunWhenFull) {
  constexpr std::uint64_t kStart = 1'000'000'000;
  Etc2EncodeBudget budget(0.1, 16e6);
  // More than the burst runs from a full budget and is paid back over time.
  EXPECT_TRUE(budget.Take(48e6, kStart));
  EXPECT_FALSE(budget.Take(1e3, kStart + 300'000'000));
  EXPECT_TRUE(budget.Take(1e3, kStart + 330'000'000));
}

TEST(VulkanEtc2EmulationTest, EncodeBudgetChargesWorkAlreadyRecorded) {
  constexpr std::uint64_t kStart = 1'000'000'000;
  Etc2EncodeBudget budget(0.1, 16e6);
  // A charge is never refused and can leave the budget in debt.
  budget.Charge(20e6, kStart);
  EXPECT_FALSE(budget.Take(1e3, kStart + 30'000'000));
  EXPECT_TRUE(budget.Take(1e3, kStart + 50'000'000));
}

TEST(VulkanEtc2EmulationTest, EncodeBudgetTakesBackARefund) {
  constexpr std::uint64_t kStart = 1'000'000'000;
  Etc2EncodeBudget budget(0.1, 16e6);
  EXPECT_TRUE(budget.Take(16e6, kStart));
  budget.Refund(10e6);
  EXPECT_TRUE(budget.Take(10e6, kStart));
  EXPECT_FALSE(budget.Take(1e3, kStart));
  // A refund fills the budget no further than its burst.
  budget.Refund(100e6);
  EXPECT_TRUE(budget.Take(16e6, kStart));
  EXPECT_FALSE(budget.Take(1e3, kStart));
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

TEST(VulkanEtc2EmulationTest, CountsWholeImageBytesAcrossMipsAndLayers) {
  VkDeviceSize compressed = 0;
  VkDeviceSize decoded = 0;
  // 64x64 with a full chain: 64, 32, 16, 8, 4, 2, 1.
  ASSERT_TRUE(Etc2ImageByteCounts(EtcFormat::kEtc2Rgb8, {64, 64, 1}, 7, 1, 1,
                                  &compressed, &decoded));
  EXPECT_EQ(compressed, 343U * 8U);
  EXPECT_EQ(decoded, 5461U * 4U);

  // Two layers of EAC R11, 2 bytes per decoded texel.
  ASSERT_TRUE(Etc2ImageByteCounts(EtcFormat::kEacR11, {8, 4, 1}, 1, 2, 1,
                                  &compressed, &decoded));
  EXPECT_EQ(compressed, 2U * 2U * 8U);
  EXPECT_EQ(decoded, 2U * 32U * 2U);
}

TEST(VulkanEtc2EmulationTest, CountsDecodedBytesAtTheUpscaledExtent) {
  VkDeviceSize compressed = 0;
  VkDeviceSize decoded = 0;
  // The host image is 256x256: 256 and 128 above the 7 levels of 64 down
  // to 1.
  ASSERT_TRUE(Etc2ImageByteCounts(EtcFormat::kEtc2Rgba8, {64, 64, 1}, 7, 1, 4,
                                  &compressed, &decoded));
  EXPECT_EQ(compressed, 343U * 16U);
  EXPECT_EQ(decoded, (65536U + 16384U + 5461U) * 4U);

  EXPECT_FALSE(Etc2ImageByteCounts(EtcFormat::kEtc2Rgb8, {0, 4, 1}, 1, 1, 1,
                                   &compressed, &decoded));
  EXPECT_FALSE(Etc2ImageByteCounts(EtcFormat::kEtc2Rgb8, {4, 4, 1}, 0, 1, 1,
                                   &compressed, &decoded));
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
// Every staging allocation maps here. Staging blocks are larger, but these
// tests write only the first few kilobytes of any block.
std::array<std::uint8_t, 64 * 1024> g_staging{};
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
std::uint32_t g_blit_calls = 0;
std::uint32_t g_created_mip_levels = 0;
// Appended by every call until a test clears them.
std::vector<VkBufferImageCopy> g_uploaded_regions;
std::vector<VkImageCopy> g_image_copies;
std::vector<VkImageBlit> g_blits;
VkImageSubresourceRange g_view_range{};
std::vector<VkImageMemoryBarrier> g_image_barriers;
std::vector<VkImageMemoryBarrier2> g_image_barriers2;
const void* g_image_barrier_array = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL FakeCreateImage(VkDevice,
                                               const VkImageCreateInfo* info,
                                               const VkAllocationCallbacks*,
                                               VkImage* image) {
  g_created_extent = info->extent;
  g_created_mip_levels = info->mipLevels;
  *image = FakeHandle<VkImage>(g_next_image++);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL FakeDestroyImage(VkDevice, VkImage,
                                            const VkAllocationCallbacks*) {}

VKAPI_ATTR void VKAPI_CALL FakeCopyImage(VkCommandBuffer, VkImage,
                                         VkImageLayout, VkImage, VkImageLayout,
                                         std::uint32_t count,
                                         const VkImageCopy* copies) {
  ++g_copy_image_calls;
  g_image_copies.insert(g_image_copies.end(), copies, copies + count);
}

VKAPI_ATTR void VKAPI_CALL FakeBlitImage(VkCommandBuffer, VkImage,
                                         VkImageLayout, VkImage, VkImageLayout,
                                         std::uint32_t count,
                                         const VkImageBlit* blits,
                                         VkFilter filter) {
  g_blit = count > 0 ? blits[0] : VkImageBlit{};
  g_blit_filter = filter;
  ++g_blit_calls;
  g_blits.insert(g_blits.end(), blits, blits + count);
}

VKAPI_ATTR void VKAPI_CALL FakeBlitImage2(VkCommandBuffer,
                                          const VkBlitImageInfo2* info) {
  for (std::uint32_t index = 0; index < info->regionCount; ++index) {
    VkImageBlit blit{};
    blit.srcSubresource = info->pRegions[index].srcSubresource;
    blit.dstSubresource = info->pRegions[index].dstSubresource;
    g_blits.push_back(blit);
  }
}

std::vector<std::uint32_t> g_readback_mips;

VKAPI_ATTR void VKAPI_CALL FakeCopyImageToBuffer(
    VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, std::uint32_t count,
    const VkBufferImageCopy* regions) {
  for (std::uint32_t index = 0; index < count; ++index) {
    g_readback_mips.push_back(regions[index].imageSubresource.mipLevel);
  }
}

VKAPI_ATTR void VKAPI_CALL FakeCopyImageToBuffer2(
    VkCommandBuffer, const VkCopyImageToBufferInfo2* info) {
  for (std::uint32_t index = 0; index < info->regionCount; ++index) {
    g_readback_mips.push_back(info->pRegions[index].imageSubresource.mipLevel);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL FakeCreateBuffer(VkDevice,
                                                const VkBufferCreateInfo* info,
                                                const VkAllocationCallbacks*,
                                                VkBuffer* buffer) {
  g_created_buffer_size = info->size;
  *buffer = FakeHandle<VkBuffer>(kStagingBuffer + g_create_buffer_calls);
  ++g_create_buffer_calls;
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL FakeBufferRequirements(
    VkDevice, VkBuffer, VkMemoryRequirements* requirements) {
  requirements->size = g_created_buffer_size;
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
// Records the host-write barrier that must precede each staged copy, so the
// decode's writes are visible to the device even though they land after the
// submit that reads them.
std::uint32_t g_barriers_before_copy = 0;
VkPipelineStageFlags g_barrier_src_stage = 0;
VkPipelineStageFlags g_barrier_dst_stage = 0;
VkAccessFlags g_barrier_src_access = 0;
VkAccessFlags g_barrier_dst_access = 0;
std::uint32_t g_pending_barriers = 0;
// Set by the barrier benchmark, whose threads must not touch the globals.
std::atomic<bool> g_quiet_barriers{false};

VKAPI_ATTR void VKAPI_CALL FakePipelineBarrier(
    VkCommandBuffer, VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage, VkDependencyFlags, std::uint32_t count,
    const VkMemoryBarrier* barriers, std::uint32_t, const VkBufferMemoryBarrier*,
    std::uint32_t image_count, const VkImageMemoryBarrier* images) {
  if (g_quiet_barriers.load(std::memory_order_relaxed)) {
    return;
  }
  g_image_barrier_array = images;
  g_image_barriers.insert(g_image_barriers.end(), images, images + image_count);
  g_barrier_src_stage = src_stage;
  g_barrier_dst_stage = dst_stage;
  if (count > 0) {
    g_barrier_src_access = barriers[0].srcAccessMask;
    g_barrier_dst_access = barriers[0].dstAccessMask;
  }
  ++g_pending_barriers;
}

void RecordDependency(const VkDependencyInfo* info) {
  g_image_barrier_array = info->pImageMemoryBarriers;
  g_image_barriers2.insert(
      g_image_barriers2.end(), info->pImageMemoryBarriers,
      info->pImageMemoryBarriers + info->imageMemoryBarrierCount);
}

VKAPI_ATTR void VKAPI_CALL FakePipelineBarrier2(VkCommandBuffer,
                                                const VkDependencyInfo* info) {
  RecordDependency(info);
}

VKAPI_ATTR void VKAPI_CALL FakeWaitEvents(
    VkCommandBuffer, std::uint32_t, const VkEvent*, VkPipelineStageFlags,
    VkPipelineStageFlags, std::uint32_t, const VkMemoryBarrier*,
    std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t image_count,
    const VkImageMemoryBarrier* images) {
  g_image_barrier_array = images;
  g_image_barriers.insert(g_image_barriers.end(), images, images + image_count);
}

VKAPI_ATTR void VKAPI_CALL FakeSetEvent2(VkCommandBuffer, VkEvent,
                                         const VkDependencyInfo* info) {
  RecordDependency(info);
}

VKAPI_ATTR void VKAPI_CALL FakeWaitEvents2(VkCommandBuffer,
                                           std::uint32_t count,
                                           const VkEvent*,
                                           const VkDependencyInfo* infos) {
  for (std::uint32_t index = 0; index < count; ++index) {
    RecordDependency(&infos[index]);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL FakeCreateImageView(
    VkDevice, const VkImageViewCreateInfo* info, const VkAllocationCallbacks*,
    VkImageView* view) {
  static std::uintptr_t next = 0x800;
  g_view_range = info->subresourceRange;
  *view = FakeHandle<VkImageView>(next++);
  return VK_SUCCESS;
}

std::uint32_t g_create_sampler_calls = 0;
std::vector<std::pair<VkSampler, VkSamplerCreateInfo>> g_created_samplers;
std::vector<VkSampler> g_destroyed_samplers;

VKAPI_ATTR VkResult VKAPI_CALL FakeCreateSampler(
    VkDevice, const VkSamplerCreateInfo* info, const VkAllocationCallbacks*,
    VkSampler* sampler) {
  *sampler = FakeHandle<VkSampler>(0x810 + g_create_sampler_calls);
  ++g_create_sampler_calls;
  g_created_samplers.emplace_back(*sampler, *info);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL FakeDestroySampler(VkDevice, VkSampler sampler,
                                              const VkAllocationCallbacks*) {
  g_destroyed_samplers.push_back(sampler);
}

VKAPI_ATTR void VKAPI_CALL FakeDestroyImageView(VkDevice, VkImageView,
                                                const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL FakeCreateDescriptorSetLayout(
    VkDevice, const VkDescriptorSetLayoutCreateInfo*,
    const VkAllocationCallbacks*, VkDescriptorSetLayout* layout) {
  *layout = FakeHandle<VkDescriptorSetLayout>(0x840);
  return VK_SUCCESS;
}

// Samplers of the image descriptors each descriptor command recorded.
std::vector<VkSampler> g_written_samplers;

void RecordWrites(std::uint32_t count, const VkWriteDescriptorSet* writes) {
  if (g_quiet_barriers.load(std::memory_order_relaxed)) {
    return;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    for (std::uint32_t element = 0; element < writes[index].descriptorCount;
         ++element) {
      g_written_samplers.push_back(writes[index].pImageInfo[element].sampler);
    }
  }
}

VKAPI_ATTR void VKAPI_CALL FakeUpdateDescriptorSets(
    VkDevice, std::uint32_t count, const VkWriteDescriptorSet* writes,
    std::uint32_t, const VkCopyDescriptorSet*) {
  RecordWrites(count, writes);
}

VKAPI_ATTR void VKAPI_CALL FakePushDescriptorSet(
    VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, std::uint32_t,
    std::uint32_t count, const VkWriteDescriptorSet* writes) {
  RecordWrites(count, writes);
}

VKAPI_ATTR VkResult VKAPI_CALL FakeCreateDescriptorUpdateTemplate(
    VkDevice, const VkDescriptorUpdateTemplateCreateInfo*,
    const VkAllocationCallbacks*, VkDescriptorUpdateTemplate* update) {
  *update = FakeHandle<VkDescriptorUpdateTemplate>(0x830);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL FakeDestroyDescriptorUpdateTemplate(
    VkDevice, VkDescriptorUpdateTemplate, const VkAllocationCallbacks*) {}

// The test's templates hold one image descriptor at offset 0.
VKAPI_ATTR void VKAPI_CALL FakeUpdateDescriptorSetWithTemplate(
    VkDevice, VkDescriptorSet, VkDescriptorUpdateTemplate, const void* data) {
  g_written_samplers.push_back(
      static_cast<const VkDescriptorImageInfo*>(data)->sampler);
}

VKAPI_ATTR void VKAPI_CALL FakePushDescriptorSetWithTemplate(
    VkCommandBuffer, VkDescriptorUpdateTemplate, VkPipelineLayout,
    std::uint32_t, const void* data) {
  g_written_samplers.push_back(
      static_cast<const VkDescriptorImageInfo*>(data)->sampler);
}

// Staging buffer and offset of each recorded copy's first region.
struct StagedCopy {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
};
std::vector<StagedCopy> g_staged_copies;

VKAPI_ATTR void VKAPI_CALL FakeCopyBufferToImage(
    VkCommandBuffer, VkBuffer buffer, VkImage, VkImageLayout,
    std::uint32_t count, const VkBufferImageCopy* regions) {
  g_copied_extent = count > 0 ? regions[0].imageExtent : VkExtent3D{};
  g_uploaded_regions.insert(g_uploaded_regions.end(), regions,
                            regions + count);
  g_barriers_before_copy = g_pending_barriers;
  if (count > 0) {
    g_staged_copies.push_back({buffer, regions[0].bufferOffset});
  }
}

constexpr std::uintptr_t kTimelineSemaphore = 0x900;

std::mutex g_signal_mutex;
std::vector<std::uint64_t> g_signalled;
std::condition_variable g_signal_changed;
bool g_hold_signals = false;
bool g_signal_held = false;

void HoldDecodeSignals(bool hold) {
  std::lock_guard<std::mutex> lock(g_signal_mutex);
  g_hold_signals = hold;
  g_signal_held = false;
  g_signal_changed.notify_all();
}

bool WaitForHeldSignal() {
  std::unique_lock<std::mutex> lock(g_signal_mutex);
  return g_signal_changed.wait_for(lock, std::chrono::seconds(5),
                                   [] { return g_signal_held; });
}

VKAPI_ATTR VkResult VKAPI_CALL FakeCreateSemaphore(
    VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*,
    VkSemaphore* semaphore) {
  *semaphore = FakeHandle<VkSemaphore>(kTimelineSemaphore);
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
FakeSignalSemaphore(VkDevice, const VkSemaphoreSignalInfo* info) {
  std::unique_lock<std::mutex> lock(g_signal_mutex);
  if (g_hold_signals) {
    g_signal_held = true;
    g_signal_changed.notify_all();
    g_signal_changed.wait(lock, [] { return !g_hold_signals; });
  }
  g_signalled.push_back(info->value);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL FakeDestroySemaphore(VkDevice, VkSemaphore,
                                                const VkAllocationCallbacks*) {}

std::vector<std::uint64_t> SignalledValues() {
  std::lock_guard<std::mutex> lock(g_signal_mutex);
  return g_signalled;
}

void ResetSignalledValues() {
  std::lock_guard<std::mutex> lock(g_signal_mutex);
  g_signalled.clear();
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeGetDeviceProcAddr(
    VkDevice, const char* name) {
  const auto is = [name](const char* candidate) {
    return std::strcmp(name, candidate) == 0;
  };
  if (is("vkCreateSemaphore"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeCreateSemaphore);
  if (is("vkSignalSemaphore"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeSignalSemaphore);
  if (is("vkDestroySemaphore"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeDestroySemaphore);
  if (is("vkCreateImage")) return reinterpret_cast<PFN_vkVoidFunction>(FakeCreateImage);
  if (is("vkDestroyImage")) return reinterpret_cast<PFN_vkVoidFunction>(FakeDestroyImage);
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
  if (is("vkCmdPipelineBarrier"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakePipelineBarrier);
  if (is("vkCmdPipelineBarrier2"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakePipelineBarrier2);
  if (is("vkCmdWaitEvents"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeWaitEvents);
  if (is("vkCmdWaitEvents2"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeWaitEvents2);
  if (is("vkCmdSetEvent2"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeSetEvent2);
  if (is("vkCreateImageView"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeCreateImageView);
  if (is("vkCreateSampler"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeCreateSampler);
  if (is("vkDestroySampler"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeDestroySampler);
  if (is("vkDestroyImageView"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeDestroyImageView);
  if (is("vkCreateDescriptorSetLayout"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeCreateDescriptorSetLayout);
  if (is("vkUpdateDescriptorSets"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeUpdateDescriptorSets);
  if (is("vkCmdPushDescriptorSetKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakePushDescriptorSet);
  if (is("vkCreateDescriptorUpdateTemplate"))
    return reinterpret_cast<PFN_vkVoidFunction>(
        FakeCreateDescriptorUpdateTemplate);
  if (is("vkDestroyDescriptorUpdateTemplate"))
    return reinterpret_cast<PFN_vkVoidFunction>(
        FakeDestroyDescriptorUpdateTemplate);
  if (is("vkUpdateDescriptorSetWithTemplate"))
    return reinterpret_cast<PFN_vkVoidFunction>(
        FakeUpdateDescriptorSetWithTemplate);
  if (is("vkCmdPushDescriptorSetWithTemplateKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(
        FakePushDescriptorSetWithTemplate);
  if (is("vkCmdCopyImage")) return reinterpret_cast<PFN_vkVoidFunction>(FakeCopyImage);
  if (is("vkCmdBlitImage")) return reinterpret_cast<PFN_vkVoidFunction>(FakeBlitImage);
  if (is("vkCmdBlitImage2"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeBlitImage2);
  if (is("vkCmdCopyImageToBuffer"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeCopyImageToBuffer);
  if (is("vkCmdCopyImageToBuffer2"))
    return reinterpret_cast<PFN_vkVoidFunction>(FakeCopyImageToBuffer2);
  return nullptr;
}

TEST(VulkanEtc2EmulationTest, TotalsTheBytesOfLiveEmulatedImages) {
  const VkDevice device = FakeHandle<VkDevice>(0x12);
  VkPhysicalDeviceMemoryProperties memory{};
  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x20), true,
                           memory, FakeGetDeviceProcAddr);

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_3D;
  image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  image_info.extent = {8, 8, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  VkImage first = VK_NULL_HANDLE;
  VkImage second = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &first),
            VK_SUCCESS);
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &second),
            VK_SUCCESS);
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  VkImage plain = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &plain),
            VK_SUCCESS);

  EmulatedTextureBytes totals = emulation.TextureBytes();
  EXPECT_EQ(totals.compressed, 2U * 4U * 8U);
  EXPECT_EQ(totals.decoded, 2U * 64U * 4U);

  emulation.DestroyImage(device, first, nullptr);
  emulation.DestroyImage(device, plain, nullptr);
  totals = emulation.TextureBytes();
  EXPECT_EQ(totals.compressed, 4U * 8U);
  EXPECT_EQ(totals.decoded, 64U * 4U);

  emulation.DestroyDevice(device);
  totals = emulation.TextureBytes();
  EXPECT_EQ(totals.compressed, 0U);
  EXPECT_EQ(totals.decoded, 0U);
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

VkPhysicalDeviceMemoryProperties CoherentMemory() {
  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  return memory;
}

// The factor is read when the emulation is built; null builds the default.
std::unique_ptr<VulkanEtc2Emulation> UpscalingEmulation(
    const char* upscale, VkDevice device, VkPhysicalDevice physical_device) {
  if (upscale != nullptr) {
    EXPECT_EQ(setenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE", upscale, 1), 0);
  } else {
    EXPECT_EQ(unsetenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"), 0);
  }
  auto emulation = std::make_unique<VulkanEtc2Emulation>();
  EXPECT_EQ(unsetenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"), 0);
  emulation->RegisterDevice(device, physical_device, true, CoherentMemory(),
                            FakeGetDeviceProcAddr);
  return emulation;
}

VkImage CreateEtc2Image(VulkanEtc2Emulation& emulation, VkDevice device,
                        std::uint32_t size, std::uint32_t mips) {
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.extent = {size, size, 1};
  info.mipLevels = mips;
  info.arrayLayers = 1;
  VkImage image = VK_NULL_HANDLE;
  EXPECT_EQ(emulation.CreateImage(device, &info, nullptr, &image), VK_SUCCESS);
  return image;
}

void MapSource(VulkanEtc2Emulation& emulation, VkDevice device,
               VkBuffer source) {
  const VkDeviceMemory memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  ASSERT_EQ(emulation.BindBufferMemory(device, source, memory, 0), VK_SUCCESS);
  void* mapped = nullptr;
  ASSERT_EQ(emulation.MapMemory(device, memory, 0, g_source.size(), 0, &mapped),
            VK_SUCCESS);
}

// A differential block whose every texel decodes to one grey.
std::array<std::uint8_t, 8> GreyBlock(std::uint8_t base) {
  return {base, base, base, 0x02, 0, 0, 0, 0};
}

std::vector<std::uint8_t> DecodeSquare(const std::uint8_t* blocks,
                                       std::size_t bytes, std::uint32_t size) {
  std::vector<std::uint8_t> texels(std::size_t{size} * size * 4);
  EXPECT_TRUE(DecodeEtcImage(EtcFormat::kEtc2Rgb8, blocks, bytes, size, size,
                             texels.data(), texels.size()));
  return texels;
}

std::vector<std::uint8_t> UpscaleSquare(const std::vector<std::uint8_t>& texels,
                                        std::uint32_t size,
                                        std::uint32_t target) {
  std::vector<std::uint8_t> scaled(std::size_t{target} * target * 4);
  UpscaleRgba(texels.data(), size, size, std::size_t{size} * 4, target, target,
              scaled.data());
  return scaled;
}

// The upload recorded into host level `mip`, or null.
const VkBufferImageCopy* UploadedLevel(std::uint32_t mip) {
  for (const VkBufferImageCopy& region : g_uploaded_regions) {
    if (region.imageSubresource.mipLevel == mip) {
      return &region;
    }
  }
  return nullptr;
}

void ExpectStaged(const VkBufferImageCopy& region,
                  const std::vector<std::uint8_t>& texels) {
  ASSERT_LE(region.bufferOffset + texels.size(), g_staging.size());
  EXPECT_TRUE(std::equal(texels.begin(), texels.end(),
                         g_staging.begin() + region.bufferOffset));
}

// 2x adds one level above the texture: level 0 upscaled, then the texture's
// own level 0 at its size.
TEST(VulkanEtc2EmulationTest, UpscalesLevelZeroIntoTheTopLevel) {
  const VkDevice device = FakeHandle<VkDevice>(0x12);
  const VkBuffer source = FakeHandle<VkBuffer>(0x202);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x702);
  g_source.fill(0);
  g_staging.fill(0xAA);
  g_uploaded_regions.clear();
  auto emulation =
      UpscalingEmulation("2", device, FakeHandle<VkPhysicalDevice>(0x22));
  const VkImage image = CreateEtc2Image(*emulation, device, 4, 1);
  EXPECT_EQ(g_created_extent.width, 8u);
  EXPECT_EQ(g_created_extent.height, 8u);
  EXPECT_EQ(g_created_mip_levels, 2u);
  MapSource(*emulation, device, source);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {4, 4, 1};
  emulation->CmdCopyBufferToImage(device, command_buffer, source, image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                  &region);
  const auto block = GreyBlock(0x81);
  std::memcpy(g_source.data(), block.data(), block.size());
  emulation->PrepareSubmit(&command_buffer, 1);

  const std::vector<std::uint8_t> level0 =
      DecodeSquare(block.data(), block.size(), 4);
  ASSERT_EQ(g_uploaded_regions.size(), 2u);
  const VkBufferImageCopy* top = UploadedLevel(0);
  const VkBufferImageCopy* own = UploadedLevel(1);
  ASSERT_NE(top, nullptr);
  ASSERT_NE(own, nullptr);
  EXPECT_EQ(top->imageExtent.width, 8u);
  EXPECT_EQ(top->imageExtent.height, 8u);
  ExpectStaged(*top, UpscaleSquare(level0, 4, 8));
  EXPECT_EQ(own->imageExtent.width, 4u);
  EXPECT_EQ(own->imageExtent.height, 4u);
  ExpectStaged(*own, level0);
  emulation->ReleaseCommandBuffer(command_buffer);
}

// Block-rounded uploads (a 1x1 mip sent as 4x4) stop at the mip's edge.
TEST(VulkanEtc2EmulationTest, ClampsBlockRoundedUploadsToTheMip) {
  const VkDevice device = FakeHandle<VkDevice>(0x14);
  const VkBuffer source = FakeHandle<VkBuffer>(0x204);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x704);
  g_source.fill(0);
  g_staging.fill(0xAA);
  g_uploaded_regions.clear();
  auto emulation =
      UpscalingEmulation("2", device, FakeHandle<VkPhysicalDevice>(0x24));
  const VkImage image = CreateEtc2Image(*emulation, device, 4, 3);
  MapSource(*emulation, device, source);

  // Mip 2 is 1x1, and lands in host level 3 below the one top level.
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1};
  region.imageExtent = {4, 4, 1};
  emulation->CmdCopyBufferToImage(device, command_buffer, source, image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                  &region);
  const auto block = GreyBlock(0x81);
  std::memcpy(g_source.data(), block.data(), block.size());
  emulation->PrepareSubmit(&command_buffer, 1);

  ASSERT_EQ(g_uploaded_regions.size(), 1u);
  const VkBufferImageCopy& uploaded = g_uploaded_regions[0];
  EXPECT_EQ(uploaded.imageSubresource.mipLevel, 3u);
  EXPECT_EQ(uploaded.imageExtent.width, 1u);
  EXPECT_EQ(uploaded.imageExtent.height, 1u);
  ExpectStaged(uploaded, {134, 134, 134, 255});
  EXPECT_EQ(g_staging[uploaded.bufferOffset + 4], 0xAA);
  emulation->ReleaseCommandBuffer(command_buffer);
}

// Levels 0 and 1 hold different texels. Each keeps its own below the top
// level, which is built from level 0 whichever order they are recorded in.
void ExpectEachLevelKept(bool level_zero_last) {
  const VkDevice device = FakeHandle<VkDevice>(0x15);
  const VkBuffer source = FakeHandle<VkBuffer>(0x205);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x705);
  g_source.fill(0);
  g_staging.fill(0xAA);
  g_uploaded_regions.clear();
  auto emulation =
      UpscalingEmulation("2", device, FakeHandle<VkPhysicalDevice>(0x25));
  const VkImage image = CreateEtc2Image(*emulation, device, 4, 3);
  MapSource(*emulation, device, source);

  const auto level0 = GreyBlock(0x81);
  const auto level1 = GreyBlock(0x88);
  std::memcpy(g_source.data(), level0.data(), level0.size());
  std::memcpy(g_source.data() + 8, level1.data(), level1.size());
  std::array<VkBufferImageCopy, 2> regions{};
  regions[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  regions[0].imageExtent = {4, 4, 1};
  regions[1].bufferOffset = 8;
  regions[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
  regions[1].imageExtent = {2, 2, 1};
  if (level_zero_last) {
    std::swap(regions[0], regions[1]);
  }
  emulation->CmdCopyBufferToImage(device, command_buffer, source, image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2,
                                  regions.data());
  emulation->PrepareSubmit(&command_buffer, 1);

  const std::vector<std::uint8_t> texels0 =
      DecodeSquare(level0.data(), level0.size(), 4);
  const std::vector<std::uint8_t> texels1 =
      DecodeSquare(level1.data(), level1.size(), 2);
  ASSERT_NE(texels0[0], texels1[0]);
  ASSERT_EQ(g_uploaded_regions.size(), 3u);
  for (std::uint32_t mip = 0; mip < 3; ++mip) {
    ASSERT_NE(UploadedLevel(mip), nullptr) << mip;
  }
  ExpectStaged(*UploadedLevel(0), UpscaleSquare(texels0, 4, 8));
  ExpectStaged(*UploadedLevel(1), texels0);
  ExpectStaged(*UploadedLevel(2), texels1);
  emulation->ReleaseCommandBuffer(command_buffer);
}

TEST(VulkanEtc2EmulationTest, KeepsEachLevelsOwnTexels) {
  ExpectEachLevelKept(false);
}

TEST(VulkanEtc2EmulationTest, KeepsEachLevelsOwnTexelsWithLevelZeroLast) {
  ExpectEachLevelKept(true);
}

// Ported from upstream's DefaultPreservesStreamedMipChain. Roblox streams a
// texture's levels, each with its own content, and moves the levels it has
// into a larger image as a sharper one arrives. `upscale` is the
// MOCKTAIL_SMALL_TEXTURE_UPSCALE value, or null for the default; `top` is
// the levels it adds above the texture's own.
void ExpectStreamedMipChainKept(const char* upscale, std::uint32_t top) {
  const VkDevice device = FakeHandle<VkDevice>(0x2a);
  const VkBuffer source = FakeHandle<VkBuffer>(0x22a);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x72a);
  g_source.fill(0);
  g_staging.fill(0xAA);
  g_uploaded_regions.clear();
  auto emulation =
      UpscalingEmulation(upscale, device, FakeHandle<VkPhysicalDevice>(0x3a));

  // 8x8 with 4 levels, so every level fits the 64-byte source.
  constexpr std::uint32_t kMips = 4;
  const VkImage image = CreateEtc2Image(*emulation, device, 8, kMips);
  EXPECT_EQ(g_created_extent.width, 8u << top);
  EXPECT_EQ(g_created_extent.height, 8u << top);
  EXPECT_EQ(g_created_mip_levels, kMips + top);
  MapSource(*emulation, device, source);

  // Each level gets its own grey: rebuilding a level from level 0 loses it.
  std::array<VkBufferImageCopy, kMips> regions{};
  std::vector<std::vector<std::uint8_t>> expected(kMips);
  VkDeviceSize offset = 0;
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    const std::uint32_t size = 8u >> mip;
    const std::uint32_t blocks = ((size + 3) / 4) * ((size + 3) / 4);
    const auto block = GreyBlock(static_cast<std::uint8_t>(0x80 + mip * 0x10));
    for (std::uint32_t index = 0; index < blocks; ++index) {
      std::memcpy(g_source.data() + offset + index * 8, block.data(), 8);
    }
    expected[mip] = DecodeSquare(g_source.data() + offset, blocks * 8, size);
    regions[mip].bufferOffset = offset;
    regions[mip].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    regions[mip].imageExtent = {size, size, 1};
    offset += blocks * 8;
  }
  ASSERT_NE(expected[0], expected[1]);
  emulation->CmdCopyBufferToImage(device, command_buffer, source, image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kMips,
                                  regions.data());
  emulation->PrepareSubmit(&command_buffer, 1);
  ASSERT_EQ(g_uploaded_regions.size(), kMips + top);
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    SCOPED_TRACE(mip);
    const VkBufferImageCopy* uploaded = UploadedLevel(mip + top);
    ASSERT_NE(uploaded, nullptr);
    EXPECT_EQ(uploaded->imageExtent.width, 8u >> mip);
    EXPECT_EQ(uploaded->imageExtent.height, 8u >> mip);
    ExpectStaged(*uploaded, expected[mip]);
  }
  for (std::uint32_t level = 0; level < top; ++level) {
    SCOPED_TRACE(level);
    const std::uint32_t size = (8u << top) >> level;
    const VkBufferImageCopy* uploaded = UploadedLevel(level);
    ASSERT_NE(uploaded, nullptr);
    EXPECT_EQ(uploaded->imageExtent.width, size);
    ExpectStaged(*uploaded, UpscaleSquare(expected[0], 8, size));
  }
  emulation->ReleaseCommandBuffer(command_buffer);

  // A sharper level arrives: the existing levels move down one in a 16x16
  // image, copied as they are rather than scaled.
  const VkImage larger = CreateEtc2Image(*emulation, device, 16, kMips + 1);
  std::array<VkImageCopy, kMips> copies{};
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    copies[mip].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    copies[mip].dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip + 1, 0, 1};
    copies[mip].extent = {8u >> mip, 8u >> mip, 1};
  }
  g_blit_calls = 0;
  g_image_copies.clear();
  emulation->CmdCopyImage(device, command_buffer, image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, larger,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kMips,
                          copies.data());
  EXPECT_EQ(g_blit_calls, 0u);
  ASSERT_EQ(g_image_copies.size(), kMips);
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    SCOPED_TRACE(mip);
    EXPECT_EQ(g_image_copies[mip].srcSubresource.mipLevel, mip + top);
    EXPECT_EQ(g_image_copies[mip].dstSubresource.mipLevel, mip + 1 + top);
    EXPECT_EQ(g_image_copies[mip].extent.width, 8u >> mip);
    EXPECT_EQ(g_image_copies[mip].extent.height, 8u >> mip);
  }
  emulation->DestroyImage(device, larger, nullptr);
  emulation->DestroyImage(device, image, nullptr);
}

// Adaptive cannot time this fake GPU, so it falls back to 1x.
TEST(VulkanEtc2EmulationTest, UnmeasuredAdaptiveUpscaleKeepsTheStreamedMipChain) {
  ExpectStreamedMipChainKept(nullptr, 0);
}

TEST(VulkanEtc2EmulationTest, OneTimesUpscaleKeepsTheStreamedMipChain) {
  ExpectStreamedMipChainKept("1", 0);
}

TEST(VulkanEtc2EmulationTest, TwoTimesUpscaleKeepsTheStreamedMipChain) {
  ExpectStreamedMipChainKept("2", 1);
}

TEST(VulkanEtc2EmulationTest, FourTimesUpscaleKeepsTheStreamedMipChain) {
  ExpectStreamedMipChainKept("4", 2);
}

// A view starting at level 0 takes the top levels too; one starting lower
// moves down past them.
TEST(VulkanEtc2EmulationTest, ViewsOfAnUpscaledImageIncludeTheTopLevels) {
  const VkDevice device = FakeHandle<VkDevice>(0x2b);
  auto emulation =
      UpscalingEmulation("4", device, FakeHandle<VkPhysicalDevice>(0x3b));
  const VkImage image = CreateEtc2Image(*emulation, device, 8, 4);
  const VkImage large = CreateEtc2Image(*emulation, device, 128, 8);

  const auto view_of = [&](VkImage target, std::uint32_t base,
                           std::uint32_t count) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = target;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    EXPECT_EQ(emulation->CreateImageView(device, &info, nullptr, &view),
              VK_SUCCESS);
    return g_view_range;
  };
  VkImageSubresourceRange range = view_of(image, 0, 4);
  EXPECT_EQ(range.baseMipLevel, 0u);
  EXPECT_EQ(range.levelCount, 6u);
  range = view_of(image, 1, 3);
  EXPECT_EQ(range.baseMipLevel, 3u);
  EXPECT_EQ(range.levelCount, 3u);
  range = view_of(image, 0, VK_REMAINING_MIP_LEVELS);
  EXPECT_EQ(range.baseMipLevel, 0u);
  EXPECT_EQ(range.levelCount, VK_REMAINING_MIP_LEVELS);
  range = view_of(large, 1, 2);
  EXPECT_EQ(range.baseMipLevel, 1u);
  EXPECT_EQ(range.levelCount, 2u);
}

TEST(VulkanEtc2EmulationTest, CreatesSamplersThroughTheHost) {
  const VkDevice device = FakeHandle<VkDevice>(0x2d);
  auto emulation =
      UpscalingEmulation("4", device, FakeHandle<VkPhysicalDevice>(0x3d));
  VkSamplerCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.maxLod = VK_LOD_CLAMP_NONE;
  VkSampler sampler = VK_NULL_HANDLE;
  g_create_sampler_calls = 0;
  EXPECT_EQ(emulation->CreateSampler(device, &info, nullptr, &sampler),
            VK_SUCCESS);
  EXPECT_EQ(sampler, FakeHandle<VkSampler>(0x810));
  EXPECT_EQ(g_create_sampler_calls, 1u);
}

// Layout transitions must reach the top levels and the shifted ones, through
// every command that carries image barriers.
TEST(VulkanEtc2EmulationTest, BarriersOnAnUpscaledImageCoverTheTopLevels) {
  const VkDevice device = FakeHandle<VkDevice>(0x2c);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x72c);
  auto emulation =
      UpscalingEmulation("4", device, FakeHandle<VkPhysicalDevice>(0x3c));
  const VkImage image = CreateEtc2Image(*emulation, device, 8, 4);
  const VkImage large = CreateEtc2Image(*emulation, device, 128, 8);

  std::array<VkImageMemoryBarrier, 3> barriers{};
  for (VkImageMemoryBarrier& barrier : barriers) {
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
  }
  barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, 0, 1};
  barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 1, 0, 1};
  barriers[2].image = large;
  barriers[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 1, 0, 1};
  const auto expect_remapped = [](std::uint32_t base0, std::uint32_t count0,
                                  std::uint32_t base1, std::uint32_t base2) {
    EXPECT_EQ(base0, 0u);
    EXPECT_EQ(count0, 6u);
    EXPECT_EQ(base1, 4u);
    EXPECT_EQ(base2, 2u);
  };

  g_image_barriers.clear();
  emulation->CmdPipelineBarrier(
      device, command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3,
      barriers.data());
  ASSERT_EQ(g_image_barriers.size(), 3u);
  expect_remapped(g_image_barriers[0].subresourceRange.baseMipLevel,
                  g_image_barriers[0].subresourceRange.levelCount,
                  g_image_barriers[1].subresourceRange.baseMipLevel,
                  g_image_barriers[2].subresourceRange.baseMipLevel);

  g_image_barriers.clear();
  const VkEvent event = FakeHandle<VkEvent>(0x90);
  emulation->CmdWaitEvents(device, command_buffer, 1, &event,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, nullptr,
                           0, nullptr, 3, barriers.data());
  ASSERT_EQ(g_image_barriers.size(), 3u);
  expect_remapped(g_image_barriers[0].subresourceRange.baseMipLevel,
                  g_image_barriers[0].subresourceRange.levelCount,
                  g_image_barriers[1].subresourceRange.baseMipLevel,
                  g_image_barriers[2].subresourceRange.baseMipLevel);

  std::array<VkImageMemoryBarrier2, 3> barriers2{};
  for (std::size_t index = 0; index < barriers2.size(); ++index) {
    barriers2[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers2[index].image = barriers[index].image;
    barriers2[index].subresourceRange = barriers[index].subresourceRange;
  }
  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.imageMemoryBarrierCount = 3;
  dependency.pImageMemoryBarriers = barriers2.data();
  g_image_barriers2.clear();
  emulation->CmdPipelineBarrier2(device, command_buffer, &dependency);
  ASSERT_EQ(g_image_barriers2.size(), 3u);
  expect_remapped(g_image_barriers2[0].subresourceRange.baseMipLevel,
                  g_image_barriers2[0].subresourceRange.levelCount,
                  g_image_barriers2[1].subresourceRange.baseMipLevel,
                  g_image_barriers2[2].subresourceRange.baseMipLevel);

  g_image_barriers2.clear();
  emulation->CmdWaitEvents2(device, command_buffer, 1, &event, &dependency);
  ASSERT_EQ(g_image_barriers2.size(), 3u);
  expect_remapped(g_image_barriers2[0].subresourceRange.baseMipLevel,
                  g_image_barriers2[0].subresourceRange.levelCount,
                  g_image_barriers2[1].subresourceRange.baseMipLevel,
                  g_image_barriers2[2].subresourceRange.baseMipLevel);

  // vkCmdWaitEvents2 must see the dependency vkCmdSetEvent2 was given.
  g_image_barriers2.clear();
  emulation->CmdSetEvent2(device, command_buffer, event, &dependency);
  ASSERT_EQ(g_image_barriers2.size(), 3u);
  expect_remapped(g_image_barriers2[0].subresourceRange.baseMipLevel,
                  g_image_barriers2[0].subresourceRange.levelCount,
                  g_image_barriers2[1].subresourceRange.baseMipLevel,
                  g_image_barriers2[2].subresourceRange.baseMipLevel);

  // Barriers on images without top levels reach the driver untouched.
  emulation->CmdPipelineBarrier(
      device, command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
      &barriers[2]);
  EXPECT_EQ(g_image_barrier_array, &barriers[2]);
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barriers2[2];
  emulation->CmdPipelineBarrier2(device, command_buffer, &dependency);
  EXPECT_EQ(g_image_barrier_array, &barriers2[2]);
}

// A sampler whose minLod skips the app's level 0 must skip the top levels
// too, so it is swapped for one shifted by them wherever it meets a view that
// includes them.
// Reporters' logs show how a clamped sampler reaches the shader, once per
// way, so a sampler the swap can't cover is visible.
TEST(VulkanEtc2EmulationTest, LogsHowClampedSamplersAreBound) {
  const VkDevice device = FakeHandle<VkDevice>(0x2a);
  auto emulation =
      UpscalingEmulation("4", device, FakeHandle<VkPhysicalDevice>(0x3a));
  const VkImage image = CreateEtc2Image(*emulation, device, 8, 4);
  const auto view_of = [&](std::uint32_t base) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base,
                             VK_REMAINING_MIP_LEVELS, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    EXPECT_EQ(emulation->CreateImageView(device, &info, nullptr, &view),
              VK_SUCCESS);
    return view;
  };
  const VkImageView with_top = view_of(0);
  const VkImageView own = view_of(1);
  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.minLod = 1;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  VkSampler clamped = VK_NULL_HANDLE;
  ASSERT_EQ(
      emulation->CreateSampler(device, &sampler_info, nullptr, &clamped),
      VK_SUCCESS);
  const auto write = [&](VkDescriptorType type, VkImageView view) {
    const VkDescriptorImageInfo info{clamped, view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.descriptorType = type;
    write.descriptorCount = 1;
    write.pImageInfo = &info;
    emulation->UpdateDescriptorSets(device, 1, &write, 0, nullptr);
  };
  testing::internal::CaptureStderr();
  write(VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE);
  write(VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE);
  write(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, own);
  write(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, with_top);
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 3;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  binding.pImmutableSamplers = &clamped;
  VkDescriptorSetLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &binding;
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  EXPECT_EQ(emulation->CreateDescriptorSetLayout(device, &layout_info,
                                                 nullptr, &layout),
            VK_SUCCESS);
  const std::string log = testing::internal::GetCapturedStderr();
  const auto count = [&log](const std::string& text) {
    std::size_t found = 0;
    for (std::size_t at = log.find(text); at != std::string::npos;
         at = log.find(text, at + 1)) {
      ++found;
    }
    return found;
  };
  EXPECT_EQ(count("sampler lod min=1 max=1000 bound as a separate sampler"),
            1u)
      << log;
  EXPECT_EQ(count("sampler lod min=1 max=1000 bound with a view without top "
                  "levels"),
            1u)
      << log;
  EXPECT_EQ(count("sampler lod min=1 max=1000 bound with an upscaled view "
                  "(top=2)"),
            1u)
      << log;
  EXPECT_EQ(count("sampler lod min=1 max=1000 is immutable in a descriptor "
                  "set layout (binding 3), so it keeps its clamps"),
            1u)
      << log;
}

TEST(VulkanEtc2EmulationTest, ShiftsClampedSamplersOnViewsWithTopLevels) {
  const VkDevice device = FakeHandle<VkDevice>(0x28);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x728);
  auto emulation =
      UpscalingEmulation("4", device, FakeHandle<VkPhysicalDevice>(0x38));
  const VkImage image = CreateEtc2Image(*emulation, device, 8, 4);
  const auto view_of = [&](std::uint32_t base) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base,
                             VK_REMAINING_MIP_LEVELS, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    EXPECT_EQ(emulation->CreateImageView(device, &info, nullptr, &view),
              VK_SUCCESS);
    return view;
  };
  const VkImageView with_top = view_of(0);
  const VkImageView own = view_of(1);
  const auto sampler_of = [&](float min_lod) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.minLod = min_lod;
    info.maxLod = min_lod > 0 ? 8 : VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    EXPECT_EQ(emulation->CreateSampler(device, &info, nullptr, &sampler),
              VK_SUCCESS);
    return sampler;
  };
  g_create_sampler_calls = 0;
  g_created_samplers.clear();
  const VkSampler clamped = sampler_of(1);
  const VkSampler plain = sampler_of(0);

  std::array<VkDescriptorImageInfo, 4> images{{
      {clamped, with_top, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {plain, with_top, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {clamped, own, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {clamped, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
  }};
  std::array<VkWriteDescriptorSet, 2> writes{};
  for (VkWriteDescriptorSet& write : writes) {
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  }
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[0].descriptorCount = 3;
  writes[0].pImageInfo = images.data();
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &images[3];
  g_written_samplers.clear();
  emulation->UpdateDescriptorSets(device, 2, writes.data(), 0, nullptr);
  ASSERT_EQ(g_written_samplers.size(), 4u);
  const VkSampler shifted = g_written_samplers[0];
  EXPECT_NE(shifted, clamped);
  EXPECT_EQ(g_written_samplers[1], plain);
  EXPECT_EQ(g_written_samplers[2], clamped);
  EXPECT_EQ(g_written_samplers[3], clamped);
  ASSERT_EQ(g_created_samplers.size(), 3u);
  EXPECT_EQ(g_created_samplers[2].first, shifted);
  EXPECT_EQ(g_created_samplers[2].second.minLod, 3.0f);
  EXPECT_EQ(g_created_samplers[2].second.maxLod, 10.0f);
  // The application's descriptors are left as they were.
  EXPECT_EQ(images[0].sampler, clamped);

  // The shifted sampler is made once, and every other path uses it.
  g_written_samplers.clear();
  emulation->UpdateDescriptorSets(device, 1, writes.data(), 0, nullptr);
  emulation->CmdPushDescriptorSet(device, command_buffer,
                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  VK_NULL_HANDLE, 0, 1, writes.data());
  VkDescriptorUpdateTemplateEntry entry{};
  entry.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  entry.descriptorCount = 1;
  entry.stride = sizeof(VkDescriptorImageInfo);
  VkDescriptorUpdateTemplateCreateInfo template_info{};
  template_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO;
  template_info.descriptorUpdateEntryCount = 1;
  template_info.pDescriptorUpdateEntries = &entry;
  VkDescriptorUpdateTemplate update = VK_NULL_HANDLE;
  ASSERT_EQ(emulation->CreateDescriptorUpdateTemplate(device, &template_info,
                                                      nullptr, &update),
            VK_SUCCESS);
  emulation->UpdateDescriptorSetWithTemplate(device, VK_NULL_HANDLE, update,
                                             images.data());
  emulation->CmdPushDescriptorSetWithTemplate(
      device, command_buffer, update, VK_NULL_HANDLE, 0, images.data());
  EXPECT_EQ(g_written_samplers,
            (std::vector<VkSampler>{shifted, plain, clamped, shifted, plain,
                                    clamped, shifted, shifted}));
  EXPECT_EQ(g_created_samplers.size(), 3u);
  EXPECT_EQ(images[0].sampler, clamped);

  // A destroyed view is forgotten, and a destroyed sampler takes its shifted
  // copy with it.
  emulation->DestroyImageView(device, with_top, nullptr);
  g_written_samplers.clear();
  emulation->UpdateDescriptorSets(device, 1, writes.data(), 0, nullptr);
  EXPECT_EQ(g_written_samplers[0], clamped);
  g_destroyed_samplers.clear();
  emulation->DestroySampler(device, clamped, nullptr);
  EXPECT_EQ(g_destroyed_samplers,
            (std::vector<VkSampler>{shifted, clamped}));
  emulation->DestroyDescriptorUpdateTemplate(device, update, nullptr);
}

// A finite maxLod counts the app's levels, so it is shifted past the top
// levels too, while a minLod of 0 stays 0 to keep them reachable.
TEST(VulkanEtc2EmulationTest, ShiftsTheMaxLodOfSamplersOnViewsWithTopLevels) {
  const VkDevice device = FakeHandle<VkDevice>(0x26);
  auto emulation =
      UpscalingEmulation("4", device, FakeHandle<VkPhysicalDevice>(0x36));
  const VkImage image = CreateEtc2Image(*emulation, device, 8, 4);
  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  ASSERT_EQ(emulation->CreateImageView(device, &view_info, nullptr, &view),
            VK_SUCCESS);
  const auto sampler_of = [&](float max_lod) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.maxLod = max_lod;
    VkSampler sampler = VK_NULL_HANDLE;
    EXPECT_EQ(emulation->CreateSampler(device, &info, nullptr, &sampler),
              VK_SUCCESS);
    return sampler;
  };
  g_create_sampler_calls = 0;
  g_created_samplers.clear();
  const VkSampler finite = sampler_of(3);
  const VkSampler unclamped = sampler_of(VK_LOD_CLAMP_NONE);
  const std::array<VkDescriptorImageInfo, 2> images{{
      {finite, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {unclamped, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  }};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.descriptorCount = 2;
  write.pImageInfo = images.data();
  g_written_samplers.clear();
  emulation->UpdateDescriptorSets(device, 1, &write, 0, nullptr);
  ASSERT_EQ(g_written_samplers.size(), 2u);
  EXPECT_NE(g_written_samplers[0], finite);
  EXPECT_EQ(g_written_samplers[1], unclamped);
  ASSERT_EQ(g_created_samplers.size(), 3u);
  EXPECT_EQ(g_created_samplers[2].second.minLod, 0.0f);
  EXPECT_EQ(g_created_samplers[2].second.maxLod, 5.0f);
}

// Reading back or blitting from an upscaled image reaches its own levels, not
// the top levels above them.
TEST(VulkanEtc2EmulationTest, ReadsAndBlitsTheOwnLevelsOfAnUpscaledImage) {
  const VkDevice device = FakeHandle<VkDevice>(0x2a);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x72a);
  auto emulation =
      UpscalingEmulation("4", device, FakeHandle<VkPhysicalDevice>(0x3a));
  const VkImage image = CreateEtc2Image(*emulation, device, 8, 4);
  const VkImage other = FakeHandle<VkImage>(0x5a);
  const VkBuffer buffer = FakeHandle<VkBuffer>(0x6a);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
  region.imageExtent = {4, 4, 1};
  g_readback_mips.clear();
  emulation->CmdCopyImageToBuffer(device, command_buffer, image,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                                  1, &region);
  VkBufferImageCopy2 region2{};
  region2.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
  region2.imageSubresource = region.imageSubresource;
  region2.imageExtent = region.imageExtent;
  VkCopyImageToBufferInfo2 info{};
  info.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
  info.srcImage = image;
  info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  info.dstBuffer = buffer;
  info.regionCount = 1;
  info.pRegions = &region2;
  emulation->CmdCopyImageToBuffer2(device, command_buffer, &info);
  info.srcImage = other;
  emulation->CmdCopyImageToBuffer2(device, command_buffer, &info);
  EXPECT_EQ(g_readback_mips, (std::vector<std::uint32_t>{3, 3, 1}));

  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  g_blits.clear();
  emulation->CmdBlitImage(device, command_buffer, image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, other,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                          VK_FILTER_LINEAR);
  VkImageBlit2 blit2{};
  blit2.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
  blit2.srcSubresource = blit.srcSubresource;
  blit2.dstSubresource = blit.dstSubresource;
  VkBlitImageInfo2 blit_info{};
  blit_info.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
  blit_info.srcImage = image;
  blit_info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  blit_info.dstImage = other;
  blit_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  blit_info.regionCount = 1;
  blit_info.pRegions = &blit2;
  blit_info.filter = VK_FILTER_LINEAR;
  emulation->CmdBlitImage2(device, command_buffer, &blit_info);
  ASSERT_EQ(g_blits.size(), 2u);
  for (const VkImageBlit& recorded : g_blits) {
    EXPECT_EQ(recorded.srcSubresource.mipLevel, 2u);
    EXPECT_EQ(recorded.dstSubresource.mipLevel, 0u);
  }
}

// Barriers find their device again after it is destroyed and registered anew.
TEST(VulkanEtc2EmulationTest, BarriersFollowADeviceRegisteredAgain) {
  const VkDevice device = FakeHandle<VkDevice>(0x2f);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x72f);
  auto emulation =
      UpscalingEmulation("4", device, FakeHandle<VkPhysicalDevice>(0x3f));
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.image = FakeHandle<VkImage>(0x5f);
  const auto record = [&] {
    g_image_barriers.clear();
    emulation->CmdPipelineBarrier(
        device, command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &barrier);
    return g_image_barriers.size();
  };
  EXPECT_EQ(record(), 1u);
  emulation->DestroyDevice(device);
  EXPECT_EQ(record(), 0u);
  emulation->RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x3f), true,
                            CoherentMemory(), FakeGetDeviceProcAddr);
  EXPECT_EQ(record(), 1u);
  // A second emulation with the same device handle has its own registration.
  VulkanEtc2Emulation other;
  g_image_barriers.clear();
  other.CmdPipelineBarrier(device, command_buffer,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &barrier);
  EXPECT_TRUE(g_image_barriers.empty());
  EXPECT_EQ(record(), 1u);
}

// Run with --gtest_also_run_disabled_tests. CPU cost of level-0 uploads into
// a 4x upscaled 8x8 image, which also fill its two top levels.
TEST(VulkanEtc2EmulationTest, DISABLED_BenchmarkUpscaledUploads) {
  const VkDevice device = FakeHandle<VkDevice>(0x29);
  const VkBuffer source = FakeHandle<VkBuffer>(0x229);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x729);
  for (std::size_t index = 0; index < 32; ++index) {
    g_source[index] = static_cast<std::uint8_t>(index * 37 + 11);
  }
  for (const char* upscale : {"1", "4"}) {
    auto emulation =
        UpscalingEmulation(upscale, device, FakeHandle<VkPhysicalDevice>(0x39));
    const VkImage image = CreateEtc2Image(*emulation, device, 8, 1);
    MapSource(*emulation, device, source);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {8, 8, 1};
    constexpr int kUploads = 20'000;
    const auto start = std::chrono::steady_clock::now();
    for (int upload = 0; upload < kUploads; ++upload) {
      g_uploaded_regions.clear();
      emulation->CmdCopyBufferToImage(device, command_buffer, source, image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                      &region);
      emulation->PrepareSubmit(&command_buffer, 1);
      emulation->ReleaseCommandBuffer(command_buffer);
    }
    const double us = std::chrono::duration<double, std::micro>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    std::printf("8x8 upload at %sx: %.2f us each\n", upscale, us / kUploads);
  }
}

// Run with --gtest_also_run_disabled_tests. Two threads write one combined
// image sampler per call while a clamped sampler and a view with top levels
// exist, so every write is looked up.
TEST(VulkanEtc2EmulationTest, DISABLED_BenchmarkDescriptorWrites) {
  const VkDevice device = FakeHandle<VkDevice>(0x27);
  auto emulation =
      UpscalingEmulation("4", device, FakeHandle<VkPhysicalDevice>(0x37));
  const VkImage image = CreateEtc2Image(*emulation, device, 8, 4);
  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  ASSERT_EQ(emulation->CreateImageView(device, &view_info, nullptr, &view),
            VK_SUCCESS);
  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  VkSampler plain = VK_NULL_HANDLE;
  ASSERT_EQ(emulation->CreateSampler(device, &sampler_info, nullptr, &plain),
            VK_SUCCESS);
  sampler_info.minLod = 1;
  VkSampler clamped = VK_NULL_HANDLE;
  ASSERT_EQ(
      emulation->CreateSampler(device, &sampler_info, nullptr, &clamped),
      VK_SUCCESS);
  constexpr int kCalls = 1'000'000;
  for (const VkSampler sampler : {plain, clamped}) {
    const VkDescriptorImageInfo info{sampler, view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &info;
    g_quiet_barriers.store(true);
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 2; ++thread) {
      threads.emplace_back([&] {
        for (int call = 0; call < kCalls; ++call) {
          emulation->UpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
      });
    }
    for (std::thread& thread : threads) {
      thread.join();
    }
    const double ns = std::chrono::duration<double, std::nano>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    g_quiet_barriers.store(false);
    std::printf("descriptor write, %s sampler: %.1f ns/call (2 threads)\n",
                sampler == plain ? "plain" : "clamped", ns / kCalls);
  }
}

// Run with --gtest_also_run_disabled_tests. Two threads record barriers on
// one device while an upscaled image exists, as Roblox's render threads do.
TEST(VulkanEtc2EmulationTest, DISABLED_BenchmarkBarriers) {
  const VkDevice device = FakeHandle<VkDevice>(0x2b);
  auto emulation =
      UpscalingEmulation("4", device, FakeHandle<VkPhysicalDevice>(0x3b));
  const VkImage image = CreateEtc2Image(*emulation, device, 8, 4);
  constexpr int kCalls = 2'000'000;
  for (const bool with_top : {false, true}) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = with_top ? image : FakeHandle<VkImage>(0x5b);
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_quiet_barriers.store(true);
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 2; ++thread) {
      threads.emplace_back([&, thread] {
        const VkCommandBuffer command_buffer =
            FakeHandle<VkCommandBuffer>(0x900 + thread);
        for (int call = 0; call < kCalls; ++call) {
          emulation->CmdPipelineBarrier(
              device, command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
              nullptr, 1, &barrier);
        }
      });
    }
    for (std::thread& thread : threads) {
      thread.join();
    }
    const double ns =
        std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - start)
            .count();
    g_quiet_barriers.store(false);
    std::printf("barrier on %s image: %.1f ns/call (2 threads)\n",
                with_top ? "an upscaled" : "another", ns / kCalls);
  }
}

// A destroyed device takes its images' top levels with it, so a later image
// that reuses a handle is not remapped.
TEST(VulkanEtc2EmulationTest, ForgetsTopLevelsWithTheirDevice) {
  const VkDevice device = FakeHandle<VkDevice>(0x2e);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x72e);
  auto emulation =
      UpscalingEmulation("4", device, FakeHandle<VkPhysicalDevice>(0x3e));
  const VkImage image = CreateEtc2Image(*emulation, device, 8, 4);
  emulation->DestroyDevice(device);
  emulation->RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x3e), true,
                            CoherentMemory(), FakeGetDeviceProcAddr);

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, 0, 1};
  g_image_barriers.clear();
  emulation->CmdPipelineBarrier(
      device, command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
      &barrier);
  EXPECT_EQ(g_image_barrier_array, &barrier);
  ASSERT_EQ(g_image_barriers.size(), 1u);
  EXPECT_EQ(g_image_barriers[0].subresourceRange.levelCount, 4u);
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

// Registers `device` and creates an EAC R11 image of `extent` whose uploads
// read from `source`. EAC is never upscaled, so staging bytes are the
// decoded size: two bytes per texel.
VkImage SetUpStagingTest(VulkanEtc2Emulation& emulation, VkDevice device,
                         VkBuffer source, VkExtent3D extent) {
  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x28), true,
                           memory, FakeGetDeviceProcAddr);
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.format = VK_FORMAT_EAC_R11_UNORM_BLOCK;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = extent;
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  VkImage image = VK_NULL_HANDLE;
  EXPECT_EQ(emulation.CreateImage(device, &image_info, nullptr, &image),
            VK_SUCCESS);
  EXPECT_EQ(emulation.BindBufferMemory(
                device, source, FakeHandle<VkDeviceMemory>(kSourceMemory), 0),
            VK_SUCCESS);
  g_create_buffer_calls = 0;
  g_destroy_buffer_calls = 0;
  g_staged_copies.clear();
  return image;
}

void RecordUpload(VulkanEtc2Emulation& emulation, VkDevice device,
                  VkCommandBuffer command_buffer, VkBuffer source,
                  VkImage image, VkExtent3D extent) {
  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = extent;
  emulation.CmdCopyBufferToImage(device, command_buffer, source, image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &region);
}

// One buffer and allocation per upload costs the render thread a driver call
// on each side of the upload's life, so uploads share large blocks.
TEST(VulkanEtc2EmulationTest, SubAllocatesUploadsFromOneStagingBlock) {
  const VkDevice device = FakeHandle<VkDevice>(0x18);
  const VkBuffer source = FakeHandle<VkBuffer>(0x208);
  VulkanEtc2Emulation emulation;
  const VkImage image = SetUpStagingTest(emulation, device, source, {4, 4, 1});

  std::vector<VkCommandBuffer> command_buffers;
  for (std::uintptr_t index = 0; index < 8; ++index) {
    command_buffers.push_back(FakeHandle<VkCommandBuffer>(0x790 + index));
    RecordUpload(emulation, device, command_buffers.back(), source, image,
                 {4, 4, 1});
  }
  EXPECT_EQ(g_create_buffer_calls, 1u);
  ASSERT_EQ(g_staged_copies.size(), 8u);
  for (std::size_t index = 0; index < g_staged_copies.size(); ++index) {
    EXPECT_EQ(g_staged_copies[index].buffer, g_staged_copies[0].buffer);
    EXPECT_EQ(g_staged_copies[index].offset % 4, 0u) << index;
    if (index > 0) {
      // Each upload takes 4 x 4 texels of 2 bytes, clear of the one before.
      EXPECT_GE(g_staged_copies[index].offset,
                g_staged_copies[index - 1].offset + 4 * 4 * 2)
          << index;
    }
  }
  for (VkCommandBuffer command_buffer : command_buffers) {
    emulation.ReleaseCommandBuffer(command_buffer);
  }
  EXPECT_EQ(g_destroy_buffer_calls, 0u);
  emulation.DestroyDevice(device);
  EXPECT_EQ(g_destroy_buffer_calls, 1u);
}

TEST(VulkanEtc2EmulationTest, StartsANewStagingBlockWhenTheCurrentIsFull) {
  const VkDevice device = FakeHandle<VkDevice>(0x19);
  const VkBuffer source = FakeHandle<VkBuffer>(0x209);
  VulkanEtc2Emulation emulation(1024);
  // 16 x 16 texels of 2 bytes: two uploads fill a block.
  const VkImage image =
      SetUpStagingTest(emulation, device, source, {16, 16, 1});
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x7a0);
  for (int upload = 0; upload < 3; ++upload) {
    RecordUpload(emulation, device, command_buffer, source, image,
                 {16, 16, 1});
  }
  EXPECT_EQ(g_create_buffer_calls, 2u);
  ASSERT_EQ(g_staged_copies.size(), 3u);
  EXPECT_EQ(g_staged_copies[1].buffer, g_staged_copies[0].buffer);
  EXPECT_NE(g_staged_copies[2].buffer, g_staged_copies[0].buffer);
  EXPECT_EQ(g_staged_copies[2].offset, 0u);
  emulation.ReleaseCommandBuffer(command_buffer);
}

TEST(VulkanEtc2EmulationTest, GivesAnUploadLargerThanABlockItsOwnBlock) {
  const VkDevice device = FakeHandle<VkDevice>(0x1a);
  const VkBuffer source = FakeHandle<VkBuffer>(0x20a);
  VulkanEtc2Emulation emulation(1024);
  const VkImage image =
      SetUpStagingTest(emulation, device, source, {32, 32, 1});
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x7a1);
  RecordUpload(emulation, device, command_buffer, source, image, {32, 32, 1});
  EXPECT_EQ(g_create_buffer_calls, 1u);
  EXPECT_GE(g_created_buffer_size, 32u * 32u * 2u);
  emulation.ReleaseCommandBuffer(command_buffer);
}

TEST(VulkanEtc2EmulationTest, RefillsAStagingBlockOnceItsCopiesAreReleased) {
  const VkDevice device = FakeHandle<VkDevice>(0x1b);
  const VkBuffer source = FakeHandle<VkBuffer>(0x20b);
  VulkanEtc2Emulation emulation(1024);
  const VkImage image =
      SetUpStagingTest(emulation, device, source, {16, 16, 1});
  const VkCommandBuffer first = FakeHandle<VkCommandBuffer>(0x7a2);
  const VkCommandBuffer second = FakeHandle<VkCommandBuffer>(0x7a3);
  RecordUpload(emulation, device, first, source, image, {16, 16, 1});
  RecordUpload(emulation, device, first, source, image, {16, 16, 1});
  emulation.ReleaseCommandBuffer(first);
  RecordUpload(emulation, device, second, source, image, {16, 16, 1});
  EXPECT_EQ(g_create_buffer_calls, 1u);
  ASSERT_EQ(g_staged_copies.size(), 3u);
  EXPECT_EQ(g_staged_copies[2].buffer, g_staged_copies[0].buffer);
  EXPECT_EQ(g_staged_copies[2].offset, 0u);
  emulation.ReleaseCommandBuffer(second);
}

TEST(VulkanEtc2EmulationTest, KeepsFourIdleStagingBlocksAndDestroysTheRest) {
  const VkDevice device = FakeHandle<VkDevice>(0x1c);
  const VkBuffer source = FakeHandle<VkBuffer>(0x20c);
  VulkanEtc2Emulation emulation(1024);
  const VkImage image =
      SetUpStagingTest(emulation, device, source, {16, 16, 1});
  // Six command buffers each fill a block of their own.
  std::vector<VkCommandBuffer> command_buffers;
  for (std::uintptr_t index = 0; index < 6; ++index) {
    command_buffers.push_back(FakeHandle<VkCommandBuffer>(0x7b0 + index));
    RecordUpload(emulation, device, command_buffers.back(), source, image,
                 {16, 16, 1});
    RecordUpload(emulation, device, command_buffers.back(), source, image,
                 {16, 16, 1});
  }
  ASSERT_EQ(g_create_buffer_calls, 6u);
  for (VkCommandBuffer command_buffer : command_buffers) {
    emulation.ReleaseCommandBuffer(command_buffer);
  }
  EXPECT_EQ(g_destroy_buffer_calls, 2u);

  const VkCommandBuffer again = FakeHandle<VkCommandBuffer>(0x7c0);
  RecordUpload(emulation, device, again, source, image, {16, 16, 1});
  EXPECT_EQ(g_create_buffer_calls, 6u);
  emulation.ReleaseCommandBuffer(again);
}

// Copies out of a scaled image cover its host bounds and shrink back onto
// an unscaled destination, which a plain copy cannot do.
// The texture's own levels sit below the top ones at their own size, so a
// copy between them needs no blit; it only stops at the mip's edge.
TEST(VulkanEtc2EmulationTest, CopiesTheOwnLevelsOfAnUpscaledImageAsTheyAre) {
  const VkDevice device = FakeHandle<VkDevice>(0x16);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x706);
  auto emulation =
      UpscalingEmulation("2", device, FakeHandle<VkPhysicalDevice>(0x26));
  const VkImage small = CreateEtc2Image(*emulation, device, 4, 3);
  const VkImage large = CreateEtc2Image(*emulation, device, 128, 8);

  // Mip 1 of the small image (2x2, block-rounded to 4x4) lands in mip 6 of
  // the large one.
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 6, 0, 1};
  region.extent = {4, 4, 1};
  g_copy_image_calls = 0;
  g_blit_calls = 0;
  g_image_copies.clear();
  emulation->CmdCopyImage(device, command_buffer, small,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, large,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  EXPECT_EQ(g_blit_calls, 0u);
  ASSERT_EQ(g_image_copies.size(), 1u);
  EXPECT_EQ(g_image_copies[0].srcSubresource.mipLevel, 2u);
  EXPECT_EQ(g_image_copies[0].dstSubresource.mipLevel, 6u);
  EXPECT_EQ(g_image_copies[0].extent.width, 2u);
  EXPECT_EQ(g_image_copies[0].extent.height, 2u);

  g_image_copies.clear();
  emulation->CmdCopyImage(device, command_buffer, large,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, large,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  ASSERT_EQ(g_image_copies.size(), 1u);
  EXPECT_EQ(g_image_copies[0].extent.width, 4u);
  EXPECT_EQ(g_copy_image_calls, 2u);
}

// Level 0 between images of one factor brings its top levels along.
TEST(VulkanEtc2EmulationTest, CopiesTheTopLevelsWithLevelZero) {
  const VkDevice device = FakeHandle<VkDevice>(0x17);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x707);
  auto emulation =
      UpscalingEmulation("2", device, FakeHandle<VkPhysicalDevice>(0x27));
  const VkImage source = CreateEtc2Image(*emulation, device, 8, 1);
  const VkImage destination = CreateEtc2Image(*emulation, device, 8, 1);

  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = region.srcSubresource;
  region.extent = {8, 8, 1};
  g_blit_calls = 0;
  g_image_copies.clear();
  emulation->CmdCopyImage(device, command_buffer, source,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  EXPECT_EQ(g_blit_calls, 0u);
  ASSERT_EQ(g_image_copies.size(), 2u);
  const auto level = [](std::uint32_t mip) -> const VkImageCopy* {
    for (const VkImageCopy& copy : g_image_copies) {
      if (copy.dstSubresource.mipLevel == mip) {
        return &copy;
      }
    }
    return nullptr;
  };
  ASSERT_NE(level(0), nullptr);
  ASSERT_NE(level(1), nullptr);
  EXPECT_EQ(level(0)->srcSubresource.mipLevel, 0u);
  EXPECT_EQ(level(0)->extent.width, 16u);
  EXPECT_EQ(level(0)->extent.height, 16u);
  EXPECT_EQ(level(1)->srcSubresource.mipLevel, 1u);
  EXPECT_EQ(level(1)->extent.width, 8u);
}

// Level 0 copied from an image without top levels has none to bring, so the
// destination's are scaled up from the copied texels.
TEST(VulkanEtc2EmulationTest, RebuildsTheTopLevelsOfACopiedLevelZero) {
  const VkDevice device = FakeHandle<VkDevice>(0x19);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x709);
  auto emulation =
      UpscalingEmulation("2", device, FakeHandle<VkPhysicalDevice>(0x29));
  const VkImage large = CreateEtc2Image(*emulation, device, 128, 8);
  const VkImage small = CreateEtc2Image(*emulation, device, 8, 1);

  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 4, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {8, 8, 1};
  g_blits.clear();
  g_image_copies.clear();
  emulation->CmdCopyImage(device, command_buffer, large,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, small,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  ASSERT_EQ(g_image_copies.size(), 1u);
  EXPECT_EQ(g_image_copies[0].srcSubresource.mipLevel, 4u);
  EXPECT_EQ(g_image_copies[0].dstSubresource.mipLevel, 1u);
  EXPECT_EQ(g_image_copies[0].extent.width, 8u);
  ASSERT_EQ(g_blits.size(), 1u);
  EXPECT_EQ(g_blits[0].srcSubresource.mipLevel, 4u);
  EXPECT_EQ(g_blits[0].srcOffsets[1].x, 8);
  EXPECT_EQ(g_blits[0].srcOffsets[1].y, 8);
  EXPECT_EQ(g_blits[0].dstSubresource.mipLevel, 0u);
  EXPECT_EQ(g_blits[0].dstOffsets[1].x, 16);
  EXPECT_EQ(g_blits[0].dstOffsets[1].y, 16);
  EXPECT_EQ(g_blit_filter, VK_FILTER_LINEAR);
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

// Records one 4x4 ETC2 upload whose every texel decodes to 134, and returns
// the image it targets.
VkImage RecordDecodeOf134(VulkanEtc2Emulation* emulation, VkDevice device,
                          VkBuffer source, VkDeviceMemory source_memory,
                          VkCommandBuffer command_buffer) {
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  image_info.extent = {4, 4, 1};
  VkImage image = VK_NULL_HANDLE;
  EXPECT_EQ(emulation->CreateImage(device, &image_info, nullptr, &image),
            VK_SUCCESS);
  EXPECT_EQ(emulation->BindBufferMemory(device, source, source_memory, 0),
            VK_SUCCESS);
  void* mapped = nullptr;
  EXPECT_EQ(emulation->MapMemory(device, source_memory, 0, g_source.size(), 0,
                                 &mapped),
            VK_SUCCESS);

  const std::array<std::uint8_t, 8> block = {0x81, 0x81, 0x81, 0x02,
                                             0,    0,    0,    0};
  std::memcpy(g_source.data(), block.data(), block.size());

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {4, 4, 1};
  emulation->CmdCopyBufferToImage(device, command_buffer, source, image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                  &region);
  return image;
}

VkPhysicalDeviceMemoryProperties HostVisibleMemory() {
  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  return memory;
}

TEST(VulkanEtc2EmulationTest, DecodesOffTheCallingThreadWithATimeline) {
  const VkDevice device = FakeHandle<VkDevice>(0x18);
  const VkBuffer source = FakeHandle<VkBuffer>(0x208);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x708);
  g_source.fill(0);
  g_staging.fill(0xAA);
  ResetSignalledValues();

  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x28), true,
                           HostVisibleMemory(), FakeGetDeviceProcAddr, true);
  RecordDecodeOf134(&emulation, device, source, source_memory, command_buffer);

  const Etc2SubmitWait wait =
      emulation.PrepareSubmit(&command_buffer, 1, true);
  EXPECT_EQ(wait.semaphore, FakeHandle<VkSemaphore>(kTimelineSemaphore));
  EXPECT_GT(wait.value, 0U);

  // Releasing the command buffer waits for the decode that writes its staging.
  emulation.ReleaseCommandBuffer(command_buffer);
  EXPECT_EQ(g_staging[0], 134);
  EXPECT_EQ(SignalledValues(), std::vector<std::uint64_t>{wait.value});
}

TEST(VulkanEtc2EmulationTest, DecodesOnTheCallerWithoutATimeline) {
  const VkDevice device = FakeHandle<VkDevice>(0x19);
  const VkBuffer source = FakeHandle<VkBuffer>(0x209);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x709);
  g_source.fill(0);
  g_staging.fill(0xAA);
  ResetSignalledValues();

  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x29), true,
                           HostVisibleMemory(), FakeGetDeviceProcAddr, false);
  RecordDecodeOf134(&emulation, device, source, source_memory, command_buffer);

  const Etc2SubmitWait wait =
      emulation.PrepareSubmit(&command_buffer, 1, true);
  EXPECT_EQ(wait.semaphore, VK_NULL_HANDLE);
  EXPECT_EQ(wait.value, 0U);
  // Decoded before the call returned, so no wait is owed.
  EXPECT_EQ(g_staging[0], 134);
  EXPECT_TRUE(SignalledValues().empty());
  emulation.ReleaseCommandBuffer(command_buffer);
}

TEST(VulkanEtc2EmulationTest, UnmapWaitsForDecodesReadingThatMemory) {
  const VkDevice device = FakeHandle<VkDevice>(0x1C);
  const VkBuffer source = FakeHandle<VkBuffer>(0x20C);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x70D);
  g_source.fill(0);
  g_staging.fill(0xAA);
  ResetSignalledValues();

  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x2C), true,
                           HostVisibleMemory(), FakeGetDeviceProcAddr, true);
  RecordDecodeOf134(&emulation, device, source, source_memory, command_buffer);

  const Etc2SubmitWait wait =
      emulation.PrepareSubmit(&command_buffer, 1, true);
  ASSERT_NE(wait.semaphore, VK_NULL_HANDLE);

  // A deferred gather reads the application's mapping on a worker, so the
  // unmap must not return while a decode can still dereference it.
  emulation.UnmapMemory(device, source_memory);
  const std::vector<std::uint64_t> signalled = SignalledValues();
  ASSERT_FALSE(signalled.empty()) << "unmap returned with a decode in flight";
  EXPECT_GE(signalled.back(), wait.value);
  EXPECT_EQ(g_staging[0], 134);
  emulation.ReleaseCommandBuffer(command_buffer);
}

TEST(VulkanEtc2EmulationTest, FreeMemoryWaitsForDecodesReadingThatMemory) {
  const VkDevice device = FakeHandle<VkDevice>(0x1D);
  const VkBuffer source = FakeHandle<VkBuffer>(0x20D);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x70E);
  g_source.fill(0);
  g_staging.fill(0xAA);
  ResetSignalledValues();

  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x2D), true,
                           HostVisibleMemory(), FakeGetDeviceProcAddr, true);
  RecordDecodeOf134(&emulation, device, source, source_memory, command_buffer);

  const Etc2SubmitWait wait =
      emulation.PrepareSubmit(&command_buffer, 1, true);
  ASSERT_NE(wait.semaphore, VK_NULL_HANDLE);

  emulation.FreeMemory(device, source_memory, nullptr);
  const std::vector<std::uint64_t> signalled = SignalledValues();
  ASSERT_FALSE(signalled.empty()) << "free returned with a decode in flight";
  EXPECT_GE(signalled.back(), wait.value);
  EXPECT_EQ(g_staging[0], 134);
  emulation.ReleaseCommandBuffer(command_buffer);
}

TEST(VulkanEtc2EmulationTest, ReleasingOtherMemoryDoesNotWaitForDecodes) {
  const VkDevice device = FakeHandle<VkDevice>(0x1E);
  const VkBuffer source = FakeHandle<VkBuffer>(0x20E);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkDeviceMemory other_memory = FakeHandle<VkDeviceMemory>(0x310);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x70F);
  g_source.fill(0);
  g_staging.fill(0xAA);
  ResetSignalledValues();

  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x2E), true,
                           HostVisibleMemory(), FakeGetDeviceProcAddr, true);
  RecordDecodeOf134(&emulation, device, source, source_memory, command_buffer);

  HoldDecodeSignals(true);
  const Etc2SubmitWait wait =
      emulation.PrepareSubmit(&command_buffer, 1, true);
  const bool held = wait.semaphore != VK_NULL_HANDLE && WaitForHeldSignal();
  if (!held) {
    HoldDecodeSignals(false);
  }
  ASSERT_TRUE(held) << "no deferred decode reached its signal";

  std::future<void> released = std::async(std::launch::async, [&] {
    emulation.UnmapMemory(device, other_memory);
    emulation.FreeMemory(device, other_memory, nullptr);
  });
  const bool returned = released.wait_for(std::chrono::seconds(2)) ==
                        std::future_status::ready;
  HoldDecodeSignals(false);
  released.wait();
  EXPECT_TRUE(returned) << "releasing other memory waited for a decode";
  emulation.ReleaseCommandBuffer(command_buffer);
}

TEST(VulkanEtc2EmulationTest, GuardsStagedCopiesWithAHostWriteBarrier) {
  const VkDevice device = FakeHandle<VkDevice>(0x1B);
  const VkBuffer source = FakeHandle<VkBuffer>(0x20B);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x70C);
  g_source.fill(0);
  g_staging.fill(0xAA);
  g_pending_barriers = 0;
  g_barriers_before_copy = 0;

  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x2B), true,
                           HostVisibleMemory(), FakeGetDeviceProcAddr, true);
  RecordDecodeOf134(&emulation, device, source, source_memory, command_buffer);

  // An asynchronous decode writes the staging after the submit that reads it,
  // so the queue submission's host-to-device domain operation does not cover
  // those writes; the copy must be preceded by an explicit barrier.
  EXPECT_EQ(g_barriers_before_copy, 1U);
  EXPECT_EQ(g_barrier_src_stage,
            static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_HOST_BIT));
  EXPECT_NE(g_barrier_dst_stage &
                static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_TRANSFER_BIT),
            0U);
  EXPECT_EQ(g_barrier_src_access,
            static_cast<VkAccessFlags>(VK_ACCESS_HOST_WRITE_BIT));
  EXPECT_NE(g_barrier_dst_access &
                static_cast<VkAccessFlags>(VK_ACCESS_TRANSFER_READ_BIT),
            0U);
  emulation.PrepareSubmit(&command_buffer, 1, true);
  emulation.ReleaseCommandBuffer(command_buffer);
}

TEST(VulkanEtc2EmulationTest, SignalsDecodeValuesInHandOutOrder) {
  const VkDevice device = FakeHandle<VkDevice>(0x1A);
  const VkBuffer source = FakeHandle<VkBuffer>(0x20A);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer first = FakeHandle<VkCommandBuffer>(0x70A);
  const VkCommandBuffer second = FakeHandle<VkCommandBuffer>(0x70B);
  g_source.fill(0);
  g_staging.fill(0xAA);
  ResetSignalledValues();

  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x2A), true,
                           HostVisibleMemory(), FakeGetDeviceProcAddr, true);
  RecordDecodeOf134(&emulation, device, source, source_memory, first);
  RecordDecodeOf134(&emulation, device, source, source_memory, second);

  const Etc2SubmitWait one = emulation.PrepareSubmit(&first, 1, true);
  const Etc2SubmitWait two = emulation.PrepareSubmit(&second, 1, true);
  EXPECT_GT(two.value, one.value);

  emulation.ReleaseCommandBuffer(first);
  emulation.ReleaseCommandBuffer(second);

  const std::vector<std::uint64_t> signalled = SignalledValues();
  ASSERT_FALSE(signalled.empty());
  EXPECT_EQ(signalled.back(), two.value);
  for (std::size_t index = 1; index < signalled.size(); ++index) {
    EXPECT_GT(signalled[index], signalled[index - 1]) << "at " << index;
  }
}

}  // namespace
}  // namespace mocktail::graphics
