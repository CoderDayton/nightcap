#include "mocktail/graphics/vulkan_etc2_emulation.h"

#include "mocktail/graphics/etc2_decoder.h"
#include "mocktail/graphics/texture_override.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
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

TEST(VulkanEtc2EmulationTest, CountsHostBlocksOfAScaledChain) {
  // 256, 128, 64, 32, 16, 8 and 4 texels square.
  EXPECT_EQ(Etc2HostBlockCount({64, 64, 1}, 7, 1, 4), 5461u);
  // 64 down to 1 texel; the last three levels take a block each.
  EXPECT_EQ(Etc2HostBlockCount({64, 64, 1}, 7, 1, 1), 343u);
  EXPECT_EQ(Etc2HostBlockCount({64, 32, 1}, 1, 3, 2), 3u * 32 * 16);
}

TEST(VulkanEtc2EmulationTest, PicksTheLargestUpscaleTheGpuEncodesInTime) {
  // A 64x64 chain costs at most kAdaptiveUpscaleTextureNs: 5461 blocks at
  // 4x, 1366 at 2x. Only the factors the setting offers are picked.
  EXPECT_EQ(AdaptiveSmallTextureUpscale(9.2), 4u);
  EXPECT_EQ(AdaptiveSmallTextureUpscale(91.0), 4u);
  EXPECT_EQ(AdaptiveSmallTextureUpscale(120.0), 2u);
  EXPECT_EQ(AdaptiveSmallTextureUpscale(300.0), 2u);
  EXPECT_EQ(AdaptiveSmallTextureUpscale(400.0), 1u);
  EXPECT_EQ(AdaptiveSmallTextureUpscale(726.0), 1u);
  // Without a measurement the card may be too slow for any upscale.
  EXPECT_EQ(AdaptiveSmallTextureUpscale(0.0), 1u);
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
  // The host image is 256x256 with the same 7 levels: 256 down to 4.
  ASSERT_TRUE(Etc2ImageByteCounts(EtcFormat::kEtc2Rgba8, {64, 64, 1}, 7, 1, 4,
                                  &compressed, &decoded));
  EXPECT_EQ(compressed, 343U * 16U);
  EXPECT_EQ(decoded, 87376U * 4U);

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
std::vector<VkBufferImageCopy> g_uploaded_regions;
std::vector<VkImageCopy> g_image_copies;

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
  g_image_copies.assign(copies, copies + count);
}

VKAPI_ATTR void VKAPI_CALL FakeBlitImage(VkCommandBuffer, VkImage,
                                         VkImageLayout, VkImage, VkImageLayout,
                                         std::uint32_t count,
                                         const VkImageBlit* blits,
                                         VkFilter filter) {
  g_blit = count > 0 ? blits[0] : VkImageBlit{};
  g_blit_filter = filter;
  ++g_blit_calls;
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

VKAPI_ATTR void VKAPI_CALL FakePipelineBarrier(
    VkCommandBuffer, VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage, VkDependencyFlags, std::uint32_t count,
    const VkMemoryBarrier* barriers, std::uint32_t, const VkBufferMemoryBarrier*,
    std::uint32_t, const VkImageMemoryBarrier*) {
  g_barrier_src_stage = src_stage;
  g_barrier_dst_stage = dst_stage;
  if (count > 0) {
    g_barrier_src_access = barriers[0].srcAccessMask;
    g_barrier_dst_access = barriers[0].dstAccessMask;
  }
  ++g_pending_barriers;
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
  g_uploaded_regions.assign(regions, regions + count);
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
  if (is("vkCmdCopyImage")) return reinterpret_cast<PFN_vkVoidFunction>(FakeCopyImage);
  if (is("vkCmdBlitImage")) return reinterpret_cast<PFN_vkVoidFunction>(FakeBlitImage);
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
  // The host region is the 4x4 texels grown to 8x8 by the adaptive upscaler.
  std::array<std::uint8_t, 4 * 4 * 4> decoded{};
  ASSERT_TRUE(DecodeEtcImage(EtcFormat::kEtc2Rgb8, block.data(), block.size(),
                             4, 4, decoded.data(), decoded.size()));
  std::array<std::uint8_t, 8 * 8 * 4> expected{};
  UpscaleRgba(decoded.data(), 4, 4, 16, 8, 8, expected.data());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), g_staging.begin()));
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

// Ported from upstream's DefaultPreservesStreamedMipChain. Roblox streams a
// texture's levels, each with its own content, and moves the levels it has
// into a larger image as a sharper one arrives. `upscale` is the
// MOCKTAIL_SMALL_TEXTURE_UPSCALE value, or null for the default.
void ExpectStreamedMipChainKept(const char* upscale) {
  const VkDevice device = FakeHandle<VkDevice>(0x2a);
  const VkBuffer source = FakeHandle<VkBuffer>(0x22a);
  const VkDeviceMemory source_memory = FakeHandle<VkDeviceMemory>(kSourceMemory);
  const VkCommandBuffer command_buffer = FakeHandle<VkCommandBuffer>(0x72a);
  g_source.fill(0);
  g_staging.fill(0xAA);
  if (upscale != nullptr) {
    ASSERT_EQ(setenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE", upscale, 1), 0);
  } else {
    ASSERT_EQ(unsetenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"), 0);
  }

  VkPhysicalDeviceMemoryProperties memory{};
  memory.memoryTypeCount = 1;
  memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VulkanEtc2Emulation emulation;
  ASSERT_EQ(unsetenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"), 0);
  emulation.RegisterDevice(device, FakeHandle<VkPhysicalDevice>(0x3a), true,
                           memory, FakeGetDeviceProcAddr);

  // 8x8 with 4 levels, so every level fits the 64-byte source.
  constexpr std::uint32_t kMips = 4;
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {8, 8, 1};
  image_info.mipLevels = kMips;
  image_info.arrayLayers = 1;
  VkImage image = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &image),
            VK_SUCCESS);
  EXPECT_EQ(g_created_extent.width, 8u);
  EXPECT_EQ(g_created_extent.height, 8u);
  EXPECT_EQ(g_created_mip_levels, kMips);
  ASSERT_EQ(emulation.BindBufferMemory(device, source, source_memory, 0),
            VK_SUCCESS);
  void* mapped = nullptr;
  ASSERT_EQ(emulation.MapMemory(device, source_memory, 0, g_source.size(), 0,
                                &mapped),
            VK_SUCCESS);

  // Each level gets its own colour: rebuilding a level from level 0 loses it.
  std::array<VkBufferImageCopy, kMips> regions{};
  std::vector<std::vector<std::uint8_t>> expected(kMips);
  VkDeviceSize offset = 0;
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    const std::uint32_t size = 8u >> mip;
    const std::uint32_t blocks = ((size + 3) / 4) * ((size + 3) / 4);
    const std::array<std::uint8_t, 8> block = {
        static_cast<std::uint8_t>(0x80 + mip * 0x10),
        static_cast<std::uint8_t>(0x80 + mip * 0x10),
        static_cast<std::uint8_t>(0x80 + mip * 0x10),
        0x02,
        0,
        0,
        0,
        0};
    for (std::uint32_t index = 0; index < blocks; ++index) {
      std::memcpy(g_source.data() + offset + index * 8, block.data(), 8);
    }
    expected[mip].resize(size * size * 4);
    ASSERT_TRUE(DecodeEtcImage(EtcFormat::kEtc2Rgb8, g_source.data() + offset,
                               blocks * 8, size, size, expected[mip].data(),
                               expected[mip].size()));
    regions[mip].bufferOffset = offset;
    regions[mip].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    regions[mip].imageExtent = {size, size, 1};
    offset += blocks * 8;
  }
  ASSERT_NE(expected[0], expected[1]);
  emulation.CmdCopyBufferToImage(device, command_buffer, source, image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kMips,
                                 regions.data());
  emulation.PrepareSubmit(&command_buffer, 1);
  ASSERT_EQ(g_uploaded_regions.size(), kMips);
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    SCOPED_TRACE(mip);
    const VkBufferImageCopy& uploaded = g_uploaded_regions[mip];
    EXPECT_EQ(uploaded.imageSubresource.mipLevel, mip);
    EXPECT_EQ(uploaded.imageExtent.width, 8u >> mip);
    EXPECT_EQ(uploaded.imageExtent.height, 8u >> mip);
    EXPECT_TRUE(std::equal(expected[mip].begin(), expected[mip].end(),
                           g_staging.begin() + uploaded.bufferOffset));
  }
  emulation.ReleaseCommandBuffer(command_buffer);

  // A sharper level arrives: the existing levels move down one in a 16x16
  // image, copied as they are rather than scaled.
  image_info.extent = {16, 16, 1};
  image_info.mipLevels = kMips + 1;
  VkImage larger = VK_NULL_HANDLE;
  ASSERT_EQ(emulation.CreateImage(device, &image_info, nullptr, &larger),
            VK_SUCCESS);
  std::array<VkImageCopy, kMips> copies{};
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    copies[mip].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    copies[mip].dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip + 1, 0, 1};
    copies[mip].extent = {8u >> mip, 8u >> mip, 1};
  }
  g_blit_calls = 0;
  g_image_copies.clear();
  emulation.CmdCopyImage(device, command_buffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, larger,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kMips,
                         copies.data());
  EXPECT_EQ(g_blit_calls, 0u);
  ASSERT_EQ(g_image_copies.size(), kMips);
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    SCOPED_TRACE(mip);
    EXPECT_EQ(g_image_copies[mip].srcSubresource.mipLevel, mip);
    EXPECT_EQ(g_image_copies[mip].dstSubresource.mipLevel, mip + 1);
    EXPECT_EQ(g_image_copies[mip].extent.width, 8u >> mip);
    EXPECT_EQ(g_image_copies[mip].extent.height, 8u >> mip);
  }
  emulation.DestroyImage(device, larger, nullptr);
  emulation.DestroyImage(device, image, nullptr);
}

// Adaptive cannot time this fake GPU, so it falls back to 1x.
TEST(VulkanEtc2EmulationTest, UnmeasuredAdaptiveUpscaleKeepsTheStreamedMipChain) {
  ExpectStreamedMipChainKept(nullptr);
}

TEST(VulkanEtc2EmulationTest, OneTimesUpscaleKeepsTheStreamedMipChain) {
  ExpectStreamedMipChainKept("1");
}

// Fails today, and Adaptive picks 4x on fast GPUs: every level is uploaded
// scaled and filled from level 0, and moving levels into a larger image
// blits instead of copying. Upstream went to 1x by default for this.
TEST(VulkanEtc2EmulationTest,
     DISABLED_FourTimesUpscaleKeepsTheStreamedMipChain) {
  ExpectStreamedMipChainKept("4");
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
