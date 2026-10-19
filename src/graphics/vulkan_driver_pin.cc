#include "mocktail/graphics/vulkan_driver_pin.h"

#include <dlfcn.h>

#include <cstdio>

namespace mocktail::graphics {

bool PinLoadedLibrary(const char* name) {
  return dlopen(name, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) != nullptr;
}

void PinLoadedVulkanDrivers() {
  static bool pinned = false;
  if (pinned) {
    return;
  }
  if (PinLoadedLibrary("libGLX_nvidia.so.0")) {
    pinned = true;
    std::fprintf(stderr, "  [vulkan] NVIDIA driver pinned against unload\n");
  }
}

}
