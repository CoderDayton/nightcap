#ifndef MOCKTAIL_VR_XR_CONTROLLER_H_
#define MOCKTAIL_VR_XR_CONTROLLER_H_

#include "mocktail/vr/xr_controller_abi.h"
#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace mocktail::vr {

// One tracked controller hand, located at the same predicted display time as
// the head. Grip is the held-object pose; aim is the pointing pose. They are
// not interchangeable: the guest consumes grip for hands/tools and aim for UI
// rays. Position is metres, orientation a quaternion (x, y, z, w) in the
// OpenXR LOCAL reference space. The guest applies its own metres-to-studs
// conversion and hand pitch offset, so neither is pre-applied here.
struct VrHandPose {
  float grip_position[3] = {0.f, 0.f, 0.f};
  float grip_orientation[4] = {0.f, 0.f, 0.f, 1.f};
  float aim_position[3] = {0.f, 0.f, 0.f};
  float aim_orientation[4] = {0.f, 0.f, 0.f, 1.f};
  // Pose validity, reported separately from button activity and connection so
  // a controller can be connected with an untracked pose.
  bool grip_valid = false;
  bool aim_valid = false;
};

// Engine-facing controller capability, independent of which OpenXR
// interaction profile supplied it. The guest receives buttons and analog axes
// through its verified Android gamepad route, so this layer normalizes every
// supported profile onto that one stable contract.
enum class ControllerButton {
  kTrigger,    // analog click semantics; value reported separately
  kSqueeze,    // grip/squeeze
  kThumbstick, // stick click
  kFaceNorth,  // right B / left Y
  kFaceSouth,  // right A / left X
  kFaceEast,   // right B equivalent on layouts that have it
  kFaceWest,   // left X equivalent
  kMenu,       // application-accessible menu only; never a system button
  kCount,
};

// Capacitive contacts are distinct from presses. Some runtimes/profiles do
// not expose them; active=false means unavailable, not an inferred contact.
enum class ControllerTouch {
  kTrigger,
  kThumbstick,
  kThumbrest,
  kA,
  kB,
  kX,
  kY,
  kCount
};

enum class ControllerAxis {
  kTrigger,     // 0..1
  kSqueeze,     // 0..1
  kThumbstickX, // -1..1
  kThumbstickY, // -1..1
  kCount,
};

// Analog trigger/squeeze thresholds used only where the runtime exposes a
// boolean click for an analog control. Hysteresis prevents edge chattering at
// the boundary; native analog values are always preserved alongside it.
struct AnalogClickThresholds {
  float press = 0.75f;
  float release = 0.6f;
};

// One coherent per-frame controller snapshot. Built once per frame by the
// backend after a single xrSyncActions, then read by native getters without
// re-polling. Button booleans are level state; edge detection happens once, at
// publish time, so repeated native getters never emit duplicate edges.
struct ControllerHandState {
  bool connected = false;
  bool pose_valid = false;
  std::array<bool, static_cast<std::size_t>(ControllerTouch::kCount)> touches{};
  std::array<bool, static_cast<std::size_t>(ControllerTouch::kCount)>
      touch_active{};
  std::array<bool, static_cast<std::size_t>(ControllerButton::kCount)>
      buttons{};
  std::array<float, static_cast<std::size_t>(ControllerAxis::kCount)> axes{};
  // Interaction profile actually reported by the runtime for this hand, for
  // diagnostics only (never used to gate input).
  std::string profile;
};

struct ControllerSnapshot {
  ControllerHandState hands[2]; // 0 = left, 1 = right
  std::uint64_t frame = 0;
  std::uint64_t session_generation = 0;
};

// Pure state machine: maps raw OpenXR action values onto the normalized
// contract and derives edges exactly once per published frame. No OpenXR or
// JNI types, so it is unit-testable with injected values.
class ControllerStateBuilder {
public:
  explicit ControllerStateBuilder(AnalogClickThresholds thresholds = {});

  // Records one raw action value for the given hand. Values not set by the
  // runtime are neutral when explicitly marked inactive.
  void SetTouch(int hand, ControllerTouch touch, bool value, bool active);
  void SetButton(int hand, ControllerButton button, bool value, bool active);
  void SetAxis(int hand, ControllerAxis axis, float value, bool active);
  void SetConnected(int hand, bool connected);
  void SetProfile(int hand, std::string profile);
  void SetPoseValid(int hand, bool valid);

  // Produces the snapshot for this frame and records which buttons changed so
  // the caller can emit exactly one native edge per transition. frame must
  // advance monotonically; publishing the same frame twice yields no edges.
  struct Edge {
    int hand = 0;
    ControllerButton button = ControllerButton::kTrigger;
    bool pressed = false;
  };
  ControllerSnapshot Publish(std::uint64_t frame, std::uint64_t generation,
                             std::vector<Edge> *edges);

  // Neutralizes all axes and releases held buttons once (focus loss,
  // disconnect, session stop). Produces release edges for anything held so no
  // button can stay stuck down.
  void ReleaseAll(std::uint64_t frame, std::uint64_t generation,
                  std::vector<Edge> *edges);

  // Derives the click state for an analog control with hysteresis: latches on
  // at `press`, off at `release`. Call after SetAxis for the same hand. Used
  // for profiles that expose a trigger value without a click action; native
  // analog values are always preserved regardless.
  bool AnalogClick(int hand, ControllerAxis axis);

  const ControllerSnapshot &snapshot() const { return snapshot_; }

private:
  AnalogClickThresholds thresholds_;
  ControllerSnapshot snapshot_;
  // Last published level per hand/button, used for edge detection.
  std::array<
      std::array<bool, static_cast<std::size_t>(ControllerButton::kCount)>, 2>
      published_buttons_{};
  // Hysteresis latch per (hand, analog control): [hand*2 + axis].
  std::array<bool, 4> analog_latch_{};
  bool have_published_ = false;
};

// Maps the normalized contract onto the guest's verified Android gamepad
// identifiers, so XR controllers reuse the single authoritative input route
// and cannot double-fire alongside a physical SDL gamepad. hand selects the
// per-hand squeeze/thumbstick/trigger identifiers (0 = left, 1 = right).
// Returns -1 when the control has no keycode (analog click).
int32_t ControllerButtonToAndroidKey(int hand, ControllerButton button);
// Returns the Android MotionEvent axis id for one axis of the given hand
// (left/right trigger differ), or -1 when the control has no analog axis.
int32_t ControllerAxisToAndroidAxis(int hand, ControllerAxis axis);
// How many float components the guest expects for this axis (sticks 2, triggers
// 1).
int32_t ControllerAxisComponentCount(ControllerAxis axis);

// SDL3 gamepad ordinals. XR controller delivery reuses the existing router
// (RobloxInputRouter::HandleEvent) as the single authoritative event route:
// the router performs the verified SDL->Android key/axis conversion, level
// deduplication, focus gating and release-on-disconnect itself. Synthesized
// platform events must therefore carry SDL ordinals, not Android codes.
// hand selects the per-hand squeeze/thumbstick/trigger ordinals.
int32_t ControllerButtonToSdlOrdinal(int hand, ControllerButton button);
// SDL axis ordinal per hand for stick/trigger delivery; -1 when not an axis.
// Left hand: LEFTX/LEFTY/TRIGGERLEFT; right: RIGHTX/RIGHTY/TRIGGERRIGHT.
int32_t ControllerAxisToSdlOrdinal(int hand, ControllerAxis axis);

// One drained delivery batch from the XR backend to the input route. Built
// once per frame after xrSyncActions; edges appear exactly once. `resync`
// requests full level re-delivery (recovery after queue overflow or an
// unfocused drop) — the router deduplicates, so re-delivery is idempotent and
// guarantees no button sticks down.
struct ControllerDelivery {
  std::uint64_t session_generation = 0;
  bool button_levels[2][static_cast<std::size_t>(ControllerButton::kCount)] =
      {};
  bool connected[2] = {false, false};
  bool connection_changed[2] = {false, false};
  struct ButtonEdge {
    int hand = 0;
    ControllerButton button = ControllerButton::kTrigger;
    bool pressed = false;
  };
  std::vector<ButtonEdge> edges;
  // SDL ordinal order: LEFTX, LEFTY, RIGHTX, RIGHTY, TRIGGERLEFT, TRIGGERRIGHT.
  float axes[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
  bool axes_valid[2] = {false, false};
  bool resync = false;
  std::uint64_t frame = 0;

  // Bounded growth guard: the backend caps the edge queue and sets resync
  // instead of growing without limit.
  static constexpr std::size_t kMaxEdges = 64;
};

bool EncodeNativeControllerChannels(const ControllerSnapshot &snapshot,
                                    float (&channels)[28]);

void EncodeControllerDelivery(const ControllerDelivery &delivery,
                              MocktailVrControllerDelivery *out);

// Thread-safe bounded queue between the XR render-thread producer (one push
// per frame after xrSyncActions) and the window-thread consumer that drains
// into the verified input route. Overflow drops the oldest frames and arms a
// resync on the next delivered frame, so the consumer always converges to the
// current levels and a full queue can never leave a button stuck down.
class ControllerDeliveryQueue {
public:
  static constexpr std::size_t kCapacity = 8;

  void Push(ControllerDelivery delivery);
  bool Pop(ControllerDelivery *out);
  std::size_t size() const;
  bool overflowed() const { return overflowed_; }
  std::uint64_t dropped_frames() const { return dropped_frames_; }
  void Clear();

private:
  mutable std::mutex mutex_;
  std::deque<ControllerDelivery> queue_;
  bool overflowed_ = false;
  std::uint64_t dropped_frames_ = 0;
};

} // namespace mocktail::vr

#endif // MOCKTAIL_VR_XR_CONTROLLER_H_
