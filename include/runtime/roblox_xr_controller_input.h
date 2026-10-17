#ifndef MOCKTAIL_RUNTIME_ROBLOX_XR_CONTROLLER_INPUT_H_
#define MOCKTAIL_RUNTIME_ROBLOX_XR_CONTROLLER_INPUT_H_

#include <cstdint>

#include "mocktail/platform/platform_runtime.h"
#include "mocktail/status.h"
#include "mocktail/vr/xr_controller_abi.h"

namespace mocktail {
namespace runtime {

// Delivers OpenXR tracked-controller input through the one authoritative
// Roblox input route (RobloxInputRouter::HandleEvent), the same verified path
// physical SDL gamepads use. Because both sources converge on that single
// route, the router's own level deduplication, focus gating and
// release-on-disconnect logic apply unchanged and an XR controller can never
// double-fire alongside a physical gamepad.
//
// Both hands are merged into ONE synthetic gamepad: the left hand supplies
// LEFTX/LEFTY, the left shoulder (squeeze) and left stick click plus X/Y, the
// right hand supplies RIGHTX/RIGHTY, the right shoulder, right stick click and
// A/B. That matches how Roblox reads VR controller buttons through
// GamepadService, and it means the guest sees a single normal controller
// rather than two competing devices.
//
// The XR backend is resolved through dlsym so this module links and runs in
// non-VR builds and while no OpenXR backend is armed; then every drain is a
// no-op. No OpenXR headers are required here.
class RobloxXrControllerInput final {
public:
  using EventSink = void (*)(void *context,
                             const platform::PlatformEvent &event);

  // Synthetic device identity. Deliberately far outside the SDL joystick id
  // range so it can never collide with a physical controller.
  static constexpr std::int64_t kInstanceLeft =
      static_cast<std::int64_t>(0x7FFFFFFFFFFFFFF0LL);

  RobloxXrControllerInput() = default;
  ~RobloxXrControllerInput() = default;
  RobloxXrControllerInput(const RobloxXrControllerInput &) = delete;
  RobloxXrControllerInput &operator=(const RobloxXrControllerInput &) = delete;

  // Resolves the XR controller ABI. Returns false (and stays inert) when the
  // symbols are absent, which is the normal non-VR or no-backend case; that is
  // not an error condition.
  bool Initialize(EventSink sink, void *context);
  // Test/injection seam: binds explicit ABI function pointers instead of
  // resolving them through dlsym. A null pop makes the module inert, matching
  // the non-VR case. Production callers use Initialize().
  bool InitializeWithAbi(EventSink sink, void *context,
                         MocktailVrControllerPopFn pop,
                         MocktailVrControllerHapticsFn haptics,
                         MocktailVrControllerStopHapticsFn stop_haptics);
  void Shutdown();
  bool active() const { return pop_ != nullptr; }

  // Drains every queued delivery and forwards it as platform events. Call once
  // per main-loop iteration, after window::PumpEvents(), on the same thread
  // that delivers SDL input. Bounded: drains at most kMaxDrainPerPump batches.
  void Drain();
  void ResendState() { force_resync_ = true; }

  // Forwards a vibration request to the XR hand output. hand: 0 left, 1 right.
  // No-op when the XR layer is inactive. amplitude is clamped by the backend.
  void RequestHaptics(int hand, float amplitude, std::uint64_t duration_ns,
                      float frequency_hz);
  void StopHaptics(int hand);

  // Last observed state, for diagnostics/tests.
  bool connected() const { return connected_; }
  std::uint64_t delivered_events() const { return delivered_events_; }
  std::uint64_t resyncs() const { return resyncs_; }

private:
  static constexpr int kMaxDrainPerPump = 4;
  void EmitConnection(bool connected);
  void EmitButton(int sdl_button, bool pressed);
  void EmitAxis(int sdl_axis, std::int16_t value);

  void *context_ = nullptr;
  EventSink sink_ = nullptr;
  void *pop_ = nullptr;
  void *haptics_ = nullptr;
  void *stop_haptics_ = nullptr;
  bool force_resync_ = false;
  std::uint64_t generation_ = 0;
  bool connected_ = false;
  bool initialized_ = false;
  std::uint64_t delivered_events_ = 0;
  std::uint64_t resyncs_ = 0;
  // Last delivered button levels, for resync and to avoid redundant events.
  bool last_buttons_[16] = {};
};

} // namespace runtime
} // namespace mocktail

#endif // MOCKTAIL_RUNTIME_ROBLOX_XR_CONTROLLER_INPUT_H_
