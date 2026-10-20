#include "mocktail/graphics/vulkan_driver_pin.h"

#include <dlfcn.h>
#include <gtest/gtest.h>

namespace mocktail::graphics {
namespace {

constexpr const char* kFixture = MOCKTAIL_TEST_PIN_FIXTURE;

bool Loaded(const char* path) {
  void* handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
  if (handle == nullptr) {
    return false;
  }
  dlclose(handle);
  return true;
}

TEST(VulkanDriverPinTest, DoesNotLoadALibraryThatIsNotLoaded) {
  ASSERT_FALSE(Loaded(kFixture));
  EXPECT_FALSE(PinLoadedLibrary(kFixture));
  EXPECT_FALSE(Loaded(kFixture));
}

TEST(VulkanDriverPinTest, PinnedLibraryStaysLoadedAfterItsLastClose) {
  void* handle = dlopen(kFixture, RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(handle, nullptr) << dlerror();
  EXPECT_TRUE(PinLoadedLibrary(kFixture));
  dlclose(handle);
  EXPECT_TRUE(Loaded(kFixture));
}

}
}
