#include <gtest/gtest.h>

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "mocktail/vr/openxr_backend.h"

namespace {

using SubmitSynchronized = VkResult(VKAPI_PTR *)(PFN_vkQueueSubmit, VkDevice,
                                                 VkQueue, uint32_t,
                                                 const VkSubmitInfo *, VkFence);

std::atomic<int> inside_driver{0}, max_inside{0};

VKAPI_ATTR VkResult VKAPI_CALL SlowSubmit(VkQueue, uint32_t,
                                          const VkSubmitInfo *, VkFence) {
  const int active = ++inside_driver;
  int previous = max_inside.load();
  while (active > previous &&
         !max_inside.compare_exchange_weak(previous, active)) {
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  --inside_driver;
  return VK_SUCCESS;
}

// The runtime arms the backend first and loads the Vulkan adapter with
// dlopen() afterwards; this binary does the same and does not link it.
TEST(VrQueueSharingLoad, AdapterLoadedAfterArmingSerializesQueueCalls) {
  mocktail::vr::internal::RequestVulkanQueueSharing();
  void *adapter = dlopen(MOCKTAIL_TEST_VULKAN_ADAPTER, RTLD_LAZY | RTLD_GLOBAL);
  ASSERT_NE(adapter, nullptr) << dlerror();
  const auto submit = reinterpret_cast<SubmitSynchronized>(
      dlsym(adapter, "mocktail_vulkan_submit_synchronized"));
  ASSERT_NE(submit, nullptr);
  const auto queue = reinterpret_cast<VkQueue>(30);
  auto call = [&] {
    EXPECT_EQ(submit(SlowSubmit, reinterpret_cast<VkDevice>(2), queue, 0,
                     nullptr, VK_NULL_HANDLE),
              VK_SUCCESS);
  };
  std::thread first(call), second(call);
  first.join();
  second.join();
  EXPECT_EQ(max_inside, 1);
}

}  // namespace
