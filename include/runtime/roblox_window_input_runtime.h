#ifndef MOCKTAIL_RUNTIME_ROBLOX_WINDOW_INPUT_RUNTIME_H_
#define MOCKTAIL_RUNTIME_ROBLOX_WINDOW_INPUT_RUNTIME_H_

#include <memory>

#include "mocktail/platform/sdl_gamepad_manager.h"
#include "mocktail/platform/text_clipboard.h"
#include "mocktail/status.h"
#include "runtime/roblox_input_native_adapter.h"
#include "runtime/roblox_text_surface_overlay.h"
#include "runtime/roblox_xr_controller_input.h"

namespace mocktail {
namespace window {
struct MouseLock;
}
namespace runtime {

// Composition owner for the single SDL event observer. It queries logical and
// pixel extents from the window, initializes JNI input first, then registers
// the observer. Shutdown clears the observer before releasing pressed input
// and JNI references, so no event can reach a torn-down Roblox runtime.
class RobloxWindowInputRuntime final {
 public:
  RobloxWindowInputRuntime(JniEnvironmentProvider environment,
                           RobloxInputSymbols symbols);
  ~RobloxWindowInputRuntime();

  RobloxWindowInputRuntime(const RobloxWindowInputRuntime&) = delete;
  RobloxWindowInputRuntime& operator=(const RobloxWindowInputRuntime&) = delete;

  Status Initialize();
  Status Shutdown();
  Status BeginTextFocusSession(RobloxTextFocusSession session);
  Status EndTextFocusSession(int64_t textbox_handle, uint64_t generation,
                             bool notify_native);
  Status ReplaceFocusedTextFromEngine(uint64_t generation,
                                      std::string authoritative_utf8);
  Status QueryCurrentTextBoxInfo(RobloxNativeTextBoxInfoQueryResult* result);
  Status UpdateTextFocusProperties(uint64_t generation,
                                   const RobloxTextFocusProperties& properties);
  // Feeds a synthetic platform event through the same authoritative router as
  // real SDL events. Used by the XR controller delivery path so tracked
  // controllers reuse the verified gamepad route (single event source, router
  // deduplication and focus gating) instead of a second JNI input path.
  void InjectPlatformEvent(const platform::PlatformEvent& event);
  RobloxInputSnapshot Snapshot() const;
  void SetRawMouseEnabled(bool enabled);

  // Drains one batch of XR tracked-controller input into the same router.
  // Called once per main-loop iteration on the input thread; inert in non-VR
  // builds or while no OpenXR controller layer is active.
  void DrainXrControllers();
  // Routes an engine/experience vibration request to the active XR hand.
  void RequestXrControllerHaptics(int hand, float amplitude,
                                  std::uint64_t duration_ns, float frequency_hz);
  void StopXrControllerHaptics(int hand);

 private:
  static void PlatformEventCallback(void* context,
                                    const platform::PlatformEvent& event);
  static bool MouseLockQueryCallback(void* context, window::MouseLock* lock);
  static void GamepadEventCallback(void* context,
                                   const platform::PlatformEvent& event);
  static void XrControllerEventCallback(void* context,
                                        const platform::PlatformEvent& event);

  RobloxTextSurfaceOverlay text_surface_overlay_;
  std::unique_ptr<platform::TextClipboard> text_clipboard_;
  RobloxInputRuntime runtime_;
  platform::SdlGamepadManager gamepads_;
  // XR tracked-controller producer. Feeds the same runtime_ router as SDL so
  // there is exactly one authoritative input route.
  RobloxXrControllerInput xr_controllers_;
  bool xr_controllers_initialized_ = false;
  bool observer_registered_ = false;
  bool mouse_lock_query_registered_ = false;
  bool initialized_ = false;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_WINDOW_INPUT_RUNTIME_H_
