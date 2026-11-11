#ifndef MOCKTAIL_GRAPHICS_GRAPHICS_BACKEND_H_
#define MOCKTAIL_GRAPHICS_GRAPHICS_BACKEND_H_

#include <string>

namespace mocktail {
namespace graphics {

enum class GraphicsBackendKind {
  kAngleVulkan = 0,
  kDirectVulkan,
  kSystemEgl,
};

enum class CapabilityState {
  kUnavailable = 0,
  // Loader and WSI entry points exist, but no device/surface was validated.
  kLoadable,
  // A real API display/device initialization completed successfully.
  kReady,
};

enum class HardwareAcceleration {
  kUnknown = 0,
  kSoftware,
  kHardware,
};

struct BackendCapability {
  GraphicsBackendKind backend = GraphicsBackendKind::kAngleVulkan;
  CapabilityState state = CapabilityState::kUnavailable;
  HardwareAcceleration acceleration = HardwareAcceleration::kUnknown;
  std::string detail;
};

}  // namespace graphics
}  // namespace mocktail

#endif  // MOCKTAIL_GRAPHICS_GRAPHICS_BACKEND_H_
