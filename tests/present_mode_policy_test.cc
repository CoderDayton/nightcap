// Modified by vii from komaruworld/mocktail. See README "About this fork".
#include "mocktail/graphics/present_mode_policy.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace graphics {
namespace {

TEST(PresentModePolicyTest, DerivesDisplayAndUnlimitedDefaults) {
  // The unmanaged default keeps Roblox's frame cap but still selects the
  // lowest-latency synchronized present mode instead of the engine's FIFO.
  EXPECT_EQ(ResolvePresentModePolicy("auto", "-1"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", ""), PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "display"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "60"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "unlimited"),
            PresentModePolicy::kUnthrottled);
  EXPECT_EQ(ResolvePresentModePolicy("on", "unlimited"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("off", "display"),
            PresentModePolicy::kUnthrottled);
}

TEST(PresentModePolicyTest, FiltersWithoutFabricatingHostModes) {
  const std::vector<VkPresentModeKHR> host = {VK_PRESENT_MODE_IMMEDIATE_KHR,
                                              VK_PRESENT_MODE_FIFO_KHR};
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kVsync, host),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_KHR}));
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kUnthrottled, host),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_IMMEDIATE_KHR}));
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kUnthrottled,
                               {VK_PRESENT_MODE_FIFO_KHR}),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_KHR}));
}

TEST(PresentModePolicyTest, PrefersMailboxForVsyncWhenHostExposesIt) {
  const std::vector<VkPresentModeKHR> host = {VK_PRESENT_MODE_IMMEDIATE_KHR,
                                              VK_PRESENT_MODE_MAILBOX_KHR,
                                              VK_PRESENT_MODE_FIFO_KHR};
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kVsync, host),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_MAILBOX_KHR}));
}

TEST(PresentModePolicyTest, PrefersLatestReadyThenRelaxedOverFifo) {
  EXPECT_EQ(FilterPresentModes(
                PresentModePolicy::kVsync,
                {VK_PRESENT_MODE_FIFO_KHR,
                 VK_PRESENT_MODE_FIFO_LATEST_READY_KHR,
                 VK_PRESENT_MODE_MAILBOX_KHR}),
            (std::vector<VkPresentModeKHR>{
                VK_PRESENT_MODE_FIFO_LATEST_READY_KHR}));
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kVsync,
                               {VK_PRESENT_MODE_FIFO_KHR,
                                VK_PRESENT_MODE_FIFO_RELAXED_KHR}),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_RELAXED_KHR}));
}

TEST(PresentModePolicyTest, RequestsExtraSwapchainImagesWhenUnthrottled) {
  EXPECT_EQ(PreferSwapchainMinImageCount(PresentModePolicy::kVsync, 2, 2, 8),
            4U);
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kUnthrottled, 2, 2, 8),
      5U);
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kUnthrottled, 1, 2, 2),
      2U);
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kUnthrottled, 2, 2, 0),
      5U);
}

TEST(PresentModePolicyTest, RetriesWithTheGamesModeWhenTheDriverRejectsOurs) {
  VkSwapchainCreateInfoKHR info{};
  info.presentMode = VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
  std::vector<VkPresentModeKHR> attempts;
  const VkResult result = CreateSwapchainWithPresentModeFallback(
      info, VK_PRESENT_MODE_FIFO_KHR,
      [&](const VkSwapchainCreateInfoKHR &attempt) {
        attempts.push_back(attempt.presentMode);
        return attempt.presentMode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR
                   ? VK_ERROR_INITIALIZATION_FAILED
                   : VK_SUCCESS;
      });
  EXPECT_EQ(result, VK_SUCCESS);
  EXPECT_EQ(attempts, (std::vector<VkPresentModeKHR>{
                          VK_PRESENT_MODE_FIFO_LATEST_READY_KHR,
                          VK_PRESENT_MODE_FIFO_KHR}));
  EXPECT_EQ(info.presentMode, VK_PRESENT_MODE_FIFO_KHR);
}

TEST(PresentModePolicyTest, DoesNotRetryWhenThereIsNothingToFallBackTo) {
  VkSwapchainCreateInfoKHR info{};
  info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
  int calls = 0;
  EXPECT_EQ(CreateSwapchainWithPresentModeFallback(
                info, VK_PRESENT_MODE_FIFO_KHR,
                [&](const VkSwapchainCreateInfoKHR &) {
                  ++calls;
                  return VK_SUCCESS;
                }),
            VK_SUCCESS);
  EXPECT_EQ(calls, 1);

  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  calls = 0;
  EXPECT_EQ(CreateSwapchainWithPresentModeFallback(
                info, VK_PRESENT_MODE_FIFO_KHR,
                [&](const VkSwapchainCreateInfoKHR &) {
                  ++calls;
                  return VK_ERROR_SURFACE_LOST_KHR;
                }),
            VK_ERROR_SURFACE_LOST_KHR);
  EXPECT_EQ(calls, 1);
}

TEST(PresentModePolicyTest, DoesNotRetryAfterErrorsAnotherModeCannotFix) {
  for (const VkResult error :
       {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY,
        VK_ERROR_DEVICE_LOST, VK_ERROR_SURFACE_LOST_KHR,
        VK_ERROR_NATIVE_WINDOW_IN_USE_KHR}) {
    VkSwapchainCreateInfoKHR info{};
    info.presentMode = VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
    int calls = 0;
    EXPECT_EQ(CreateSwapchainWithPresentModeFallback(
                  info, VK_PRESENT_MODE_FIFO_KHR,
                  [&](const VkSwapchainCreateInfoKHR &) {
                    ++calls;
                    return error;
                  }),
              error);
    EXPECT_EQ(calls, 1) << "error " << static_cast<int>(error);
  }
}

TEST(PresentModePolicyTest, ReturnsTheRetrysResultWhenItAlsoFails) {
  VkSwapchainCreateInfoKHR info{};
  info.presentMode = VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
  int calls = 0;
  EXPECT_EQ(CreateSwapchainWithPresentModeFallback(
                info, VK_PRESENT_MODE_FIFO_KHR,
                [&](const VkSwapchainCreateInfoKHR &) {
                  return ++calls == 1 ? VK_ERROR_INITIALIZATION_FAILED
                                      : VK_ERROR_OUT_OF_HOST_MEMORY;
                }),
            VK_ERROR_OUT_OF_HOST_MEMORY);
  EXPECT_EQ(calls, 2);
}

} // namespace
} // namespace graphics
} // namespace mocktail
