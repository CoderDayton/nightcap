// Inert XrActions implementation for non-VR builds. The public header is
// OpenXR-free (opaque Impl pointer), so these definitions let OpenXrBackend
// hold an XrActions member and link without the OpenXR SDK. Every entry point
// is a no-op: a non-VR build has no session, no runtime and no controllers.
#include "mocktail/vr/xr_actions.h"

namespace mocktail::vr {

struct XrActions::Impl {};

XrActions::~XrActions() = default;

Status XrActions::Create(void *) {
  return Status::Error(StatusCode::kUnavailable,
                       "OpenXR support is disabled in this build");
}

Status XrActions::Attach(void *, void *, void *) {
  return Status::Error(StatusCode::kUnavailable,
                       "OpenXR support is disabled in this build");
}

void XrActions::ResetInput() {}

void XrActions::Detach() { attached_ = false; }

void XrActions::Destroy() {
  attached_ = false;
  created_ = false;
  snapshot_ = ControllerSnapshot{};
  hands_[0] = VrHandPose{};
  hands_[1] = VrHandPose{};
  delivery_queue_.Clear();
  last_delivered_connected_[0] = false;
  last_delivered_connected_[1] = false;
  session_generation_ = 0;
}

bool XrActions::Sync(void *, std::uint64_t, std::uint64_t, std::uint64_t) {
  return false;
}

bool XrActions::TakeDelivery(ControllerDelivery *) { return false; }

void XrActions::RequestHaptics(int, float, std::uint64_t, float) {}

void XrActions::StopHaptics(int) {}

void XrActions::NoteProfileChanged() {}

std::string XrActions::ActiveProfile(int) const { return {}; }

} // namespace mocktail::vr
