#include "mocktail/graphics/vulkan_text_overlay_compositor.h"

#include <gtest/gtest.h>
#include <libplacebo/vulkan.h>

#include <atomic>
#include <cstdint>

namespace mocktail {
namespace graphics {
namespace {

std::atomic<int> g_submit_calls{0};
std::atomic<int> g_submit2_calls{0};
std::atomic<int> g_bind_sparse_calls{0};
std::atomic<int> g_wait_idle_calls{0};
std::atomic<VkQueue> g_last_queue{VK_NULL_HANDLE};

VkQueue FakeQueue() {
  return reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0x51b));
}

VkResult RecordingQueueSubmit(VkQueue queue, uint32_t, const VkSubmitInfo*,
                              VkFence) {
  g_submit_calls.fetch_add(1, std::memory_order_relaxed);
  g_last_queue.store(queue, std::memory_order_relaxed);
  return VK_SUCCESS;
}

VkResult RecordingQueueSubmit2(VkQueue queue, uint32_t, const VkSubmitInfo2*,
                               VkFence) {
  g_submit2_calls.fetch_add(1, std::memory_order_relaxed);
  g_last_queue.store(queue, std::memory_order_relaxed);
  return VK_SUCCESS;
}

VkResult RecordingQueueBindSparse(VkQueue queue, uint32_t,
                                  const VkBindSparseInfo*, VkFence) {
  g_bind_sparse_calls.fetch_add(1, std::memory_order_relaxed);
  g_last_queue.store(queue, std::memory_order_relaxed);
  return VK_SUCCESS;
}

VkResult RecordingQueueWaitIdle(VkQueue queue) {
  g_wait_idle_calls.fetch_add(1, std::memory_order_relaxed);
  g_last_queue.store(queue, std::memory_order_relaxed);
  return VK_SUCCESS;
}

class VulkanTextOverlayCompositorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_submit_calls.store(0, std::memory_order_relaxed);
    g_submit2_calls.store(0, std::memory_order_relaxed);
    g_bind_sparse_calls.store(0, std::memory_order_relaxed);
    g_wait_idle_calls.store(0, std::memory_order_relaxed);
    g_last_queue.store(VK_NULL_HANDLE, std::memory_order_relaxed);
  }
};

class VulkanTextOverlayCompositorDeviceTest
    : public VulkanTextOverlayCompositorTest {
 protected:
  void SetUp() override {
    VulkanTextOverlayCompositorTest::SetUp();
    pl_vulkan_params params = pl_vulkan_default_params;
    params.allow_software = true;
    params.async_compute = false;
    params.async_transfer = false;
    host_ = pl_vulkan_create(nullptr, &params);
    if (host_ == nullptr) {
      GTEST_SKIP() << "A compatible Vulkan device is unavailable";
    }
    get_device_proc_addr_ = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        host_->get_proc_addr(host_->instance, "vkGetDeviceProcAddr"));
    ASSERT_NE(get_device_proc_addr_, nullptr);
  }

  void TearDown() override { pl_vulkan_destroy(&host_); }

  bool RegisterHostDevice(VulkanTextOverlayCompositor* compositor) {
    return compositor->RegisterDevice(
        host_->instance, host_->phys_device, host_->device, host_->api_version,
        host_->extensions, host_->num_extensions, host_->queue_graphics.index,
        host_->queue_graphics.count, host_->features, host_->get_proc_addr,
        get_device_proc_addr_);
  }

  pl_vulkan host_ = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr_ = nullptr;
};

TEST_F(VulkanTextOverlayCompositorDeviceTest,
       RegisteredDeviceTeardownRemovesStateAndPreservesHost) {
  VulkanTextOverlayCompositor compositor;
  ASSERT_TRUE(RegisterHostDevice(&compositor));
  const auto get_device_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(
      get_device_proc_addr_(host_->device, "vkGetDeviceQueue"));
  ASSERT_NE(get_device_queue, nullptr);
  VkQueue queue = VK_NULL_HANDLE;
  get_device_queue(host_->device, host_->queue_graphics.index, 0, &queue);
  ASSERT_NE(queue, VK_NULL_HANDLE);
  ASSERT_TRUE(compositor.RegisterQueue(host_->device, queue,
                                       host_->queue_graphics.index, 0));

  compositor.DestroyDevice(VK_NULL_HANDLE);
  compositor.DestroyDevice(
      reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0x51d)));
  ASSERT_TRUE(compositor.RegisterQueue(host_->device, queue,
                                       host_->queue_graphics.index, 0));

  // Run with ThreadSanitizer to detect an unlock after DeviceState is freed.
  compositor.DestroyDevice(host_->device);
  EXPECT_FALSE(compositor.RegisterQueue(host_->device, queue,
                                        host_->queue_graphics.index, 0));
  compositor.DestroyDevice(host_->device);
  EXPECT_EQ(compositor.QueueSubmit(queue, 0, nullptr, VK_NULL_HANDLE,
                                   RecordingQueueSubmit),
            VK_SUCCESS);
  EXPECT_EQ(g_submit_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_last_queue.load(std::memory_order_relaxed), queue);

  const auto device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
      get_device_proc_addr_(host_->device, "vkDeviceWaitIdle"));
  ASSERT_NE(device_wait_idle, nullptr);
  EXPECT_EQ(device_wait_idle(host_->device), VK_SUCCESS);
  ASSERT_TRUE(RegisterHostDevice(&compositor));
  compositor.DestroyDevice(host_->device);
}

TEST_F(VulkanTextOverlayCompositorTest,
       InactiveOverlayQueueSubmitInvokesFallback) {
  VulkanTextOverlayCompositor compositor;
  const VkQueue queue = FakeQueue();
  EXPECT_EQ(compositor.QueueSubmit(queue, 0, nullptr, VK_NULL_HANDLE,
                                   RecordingQueueSubmit),
            VK_SUCCESS);
  EXPECT_EQ(g_submit_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_last_queue.load(std::memory_order_relaxed), queue);
}

TEST_F(VulkanTextOverlayCompositorTest,
       InactiveOverlayQueueSubmit2WaitIdleAndBindSparseInvokeFallback) {
  VulkanTextOverlayCompositor compositor;
  const VkQueue queue = FakeQueue();
  EXPECT_EQ(compositor.QueueSubmit2(queue, 0, nullptr, VK_NULL_HANDLE,
                                    RecordingQueueSubmit2),
            VK_SUCCESS);
  EXPECT_EQ(compositor.QueueBindSparse(queue, 0, nullptr, VK_NULL_HANDLE,
                                       RecordingQueueBindSparse),
            VK_SUCCESS);
  EXPECT_EQ(compositor.QueueWaitIdle(queue, RecordingQueueWaitIdle),
            VK_SUCCESS);
  EXPECT_EQ(g_submit2_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_bind_sparse_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_wait_idle_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_last_queue.load(std::memory_order_relaxed), queue);
}

TEST_F(VulkanTextOverlayCompositorTest, NullFallbackFailsClosed) {
  VulkanTextOverlayCompositor compositor;
  const VkQueue queue = FakeQueue();
  EXPECT_EQ(
      compositor.QueueSubmit(queue, 0, nullptr, VK_NULL_HANDLE, nullptr),
      VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(
      compositor.QueueSubmit2(queue, 0, nullptr, VK_NULL_HANDLE, nullptr),
      VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(
      compositor.QueueBindSparse(queue, 0, nullptr, VK_NULL_HANDLE, nullptr),
      VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(compositor.QueueWaitIdle(queue, nullptr),
            VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(g_submit_calls.load(std::memory_order_relaxed), 0);
}

}  // namespace
}  // namespace graphics
}  // namespace mocktail
