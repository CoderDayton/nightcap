#ifndef MOCKTAIL_VR_XR_ACTIONS_H_
#define MOCKTAIL_VR_XR_ACTIONS_H_

#include <cstdint>
#include <string>

#include "mocktail/status.h"
#include "mocktail/vr/vr_pose_math.h"
#include "mocktail/vr/xr_controller.h"

namespace mocktail::vr {

// Session-owned OpenXR action layer: one action set, left/right subactions,
// grip+aim pose spaces, and interaction-profile bindings for Touch (primary),
// Quest 2, Index, Vive wands and the simple controller. All XR handles stay
// opaque here so non-OpenXR consumers (bridge, tests) need no XR headers.
//
// Frame contract: Sync() runs exactly once per frame from the backend frame
// cycle, immediately after xrWaitFrame returned the predicted display time and
// before the guest renders, so located hand poses share the head/eye frame.
// There is no second frame wait and no polling from guest getters.
class XrActions final {
public:
  XrActions() = default;
  ~XrActions();
  XrActions(const XrActions &) = delete;
  XrActions &operator=(const XrActions &) = delete;

  // Creates the action set/actions and suggests bindings. Instance-level:
  // valid before any session exists. Failure is non-fatal for XR output; the
  // backend logs and continues without controller input.
  Status Create(void *xr_instance);

  // Attaches to a session and creates the per-hand pose spaces. Must be
  // called after xrCreateSession and before the first Sync; recreated on every
  // session replacement (no stale handles survive).
  Status Attach(void *xr_instance, void *xr_session, void *xr_local_space);

  // Destroys spaces and detaches; the action set survives for reattach.
  void Detach();
  void Destroy();

  bool attached() const { return attached_; }

  // One per-frame sync at predicted_display_time. Locates grip and aim poses
  // in the session's LOCAL space, reads analog/boolean states, refreshes
  // connection state from the runtime's current interaction profile, and
  // builds the coherent snapshot + delivery batch. Returns false when actions
  // are unavailable (runtime refused sync); input is reset and a disconnect
  // remains drainable even while the session is detached.
  bool Sync(void *xr_session, std::uint64_t predicted_display_time,
            std::uint64_t frame, std::uint64_t session_generation);

  // Latest coherent snapshot (frame/session generation tagged).
  const ControllerSnapshot &snapshot() const { return snapshot_; }
  // Hand poses in metres/LOCAL space for the state-copy records.
  const VrHandPose &hand(int index) const { return hands_[index & 1]; }

  // Drains the next pending delivery batch built by Sync. Returns false when
  // nothing is queued. Thread-safe: Sync runs on the render thread, the drain
  // runs on the window thread. Backed by a bounded queue, so a slow consumer
  // drops oldest frames and converges via resync rather than growing.
  bool TakeDelivery(ControllerDelivery *out);

  // Queues a vibration request for one hand. Safe from any thread; applied on
  // the next Sync's XR call window. amplitude [0,1], duration in nanoseconds
  // (0 = runtime default). A newer request for the same hand replaces the
  // pending one. StopHaptics cancels.
  void RequestHaptics(int hand, float amplitude, std::uint64_t duration_ns,
                      float frequency_hz);

  void StopHaptics(int hand);

  // Interaction-profile diagnostics (refreshed on profile-change events and
  // every Sync that observes a change).
  std::string ActiveProfile(int hand) const;

  // Test seam: publish an externally sourced profile name (event dispatch in
  // the backend) without XR calls.
  void NoteProfileChanged();

  // Called by the backend on focus/session loss, under controller ownership.
  void ResetInput();

private:
  struct Impl;
  Impl *impl_ = nullptr;
  bool created_ = false;
  bool attached_ = false;
  ControllerStateBuilder builder_;
  ControllerSnapshot snapshot_;
  VrHandPose hands_[2];
  ControllerDeliveryQueue delivery_queue_;
  std::uint64_t session_generation_ = 0;
  // Last connection level handed to the consumer, to mark connection changes.
  bool last_delivered_connected_[2] = {false, false};
};

} // namespace mocktail::vr

#endif // MOCKTAIL_VR_XR_ACTIONS_H_
