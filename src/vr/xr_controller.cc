#include "mocktail/vr/xr_controller.h"

#include <algorithm>
#include <cmath>

namespace mocktail::vr {
namespace {

constexpr std::size_t kButtons =
    static_cast<std::size_t>(ControllerButton::kCount);
constexpr std::size_t kAxes = static_cast<std::size_t>(ControllerAxis::kCount);

bool IsValidHand(int hand) { return hand == 0 || hand == 1; }

} // namespace

ControllerStateBuilder::ControllerStateBuilder(AnalogClickThresholds thresholds)
    : thresholds_(thresholds) {}

void ControllerStateBuilder::SetTouch(int hand, ControllerTouch touch,
                                      bool value, bool active) {
  const auto i = static_cast<std::size_t>(touch);
  if (!IsValidHand(hand) ||
      i >= static_cast<std::size_t>(ControllerTouch::kCount))
    return;
  snapshot_.hands[hand].touches[i] = active && value;
  snapshot_.hands[hand].touch_active[i] = active;
}

void ControllerStateBuilder::SetButton(int hand, ControllerButton button,
                                       bool value, bool active) {
  if (!IsValidHand(hand)) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(button);
  if (index >= kButtons) {
    return;
  }
  snapshot_.hands[hand].buttons[index] = active && value;
}

void ControllerStateBuilder::SetAxis(int hand, ControllerAxis axis, float value,
                                     bool active) {
  if (!IsValidHand(hand)) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(axis);
  if (index >= kAxes) {
    return;
  }
  if (!active || !std::isfinite(value)) {
    snapshot_.hands[hand].axes[index] = 0.f;
    return;
  }
  // Clamp to the contract range; runtimes may overshoot slightly on analog
  // controls and the guest expects normalized values.
  if (axis == ControllerAxis::kTrigger || axis == ControllerAxis::kSqueeze) {
    snapshot_.hands[hand].axes[index] = std::clamp(value, 0.f, 1.f);
  } else {
    snapshot_.hands[hand].axes[index] = std::clamp(value, -1.f, 1.f);
  }
}

void ControllerStateBuilder::SetConnected(int hand, bool connected) {
  if (!IsValidHand(hand)) {
    return;
  }
  snapshot_.hands[hand].connected = connected;
  if (!connected) {
    snapshot_.hands[hand].touches = {};
    snapshot_.hands[hand].touch_active = {};
    analog_latch_[hand * 2] = false;
    analog_latch_[hand * 2 + 1] = false;
  }
}

void ControllerStateBuilder::SetProfile(int hand, std::string profile) {
  if (!IsValidHand(hand)) {
    return;
  }
  snapshot_.hands[hand].profile = std::move(profile);
}

void ControllerStateBuilder::SetPoseValid(int hand, bool valid) {
  if (!IsValidHand(hand)) {
    return;
  }
  snapshot_.hands[hand].pose_valid = valid;
}

bool ControllerStateBuilder::AnalogClick(int hand, ControllerAxis axis) {
  if (!IsValidHand(hand) ||
      (axis != ControllerAxis::kTrigger && axis != ControllerAxis::kSqueeze)) {
    return false;
  }
  const float value =
      snapshot_.hands[hand].axes[static_cast<std::size_t>(axis)];
  const std::size_t axis_index = static_cast<std::size_t>(axis);
  // Only two analog controls are click-derivable; index them 0/1.
  const std::size_t latch_index = static_cast<std::size_t>(hand) * 2 +
                                  (axis == ControllerAxis::kTrigger ? 0u : 1u);
  const bool latched = analog_latch_[latch_index];
  const bool next =
      latched ? value > thresholds_.release : value >= thresholds_.press;
  analog_latch_[latch_index] = next;
  static_cast<void>(axis_index);
  return next;
}

ControllerSnapshot ControllerStateBuilder::Publish(std::uint64_t frame,
                                                   std::uint64_t generation,
                                                   std::vector<Edge> *edges) {
  snapshot_.frame = frame;
  snapshot_.session_generation = generation;
  // Derive click levels for analog controls whose profile has no click action,
  // then emit exactly one edge per transition. published_buttons_ starts
  // all-false, so the first publish legitimately reports already-held buttons
  // as press edges; later publishes report only real transitions, so repeated
  // native getters reading the snapshot never see duplicate edges.
  for (int hand = 0; hand < 2; ++hand) {
    for (std::size_t index = 0; index < kButtons; ++index) {
      const bool level = snapshot_.hands[hand].buttons[index];
      const bool previous = published_buttons_[hand][index];
      if (level != previous && edges != nullptr) {
        edges->push_back(
            Edge{hand, static_cast<ControllerButton>(index), level});
      }
      published_buttons_[hand][index] = level;
    }
  }
  have_published_ = true;
  return snapshot_;
}

void ControllerStateBuilder::ReleaseAll(std::uint64_t frame,
                                        std::uint64_t generation,
                                        std::vector<Edge> *edges) {
  snapshot_.frame = frame;
  snapshot_.session_generation = generation;
  for (int hand = 0; hand < 2; ++hand) {
    snapshot_.hands[hand].touches = {};
    snapshot_.hands[hand].touch_active = {};
    for (std::size_t index = 0; index < kAxes; ++index) {
      snapshot_.hands[hand].axes[index] = 0.f;
    }
    for (std::size_t index = 0; index < kButtons; ++index) {
      if (snapshot_.hands[hand].buttons[index] && edges != nullptr) {
        edges->push_back(
            Edge{hand, static_cast<ControllerButton>(index), false});
      }
      snapshot_.hands[hand].buttons[index] = false;
      published_buttons_[hand][index] = false;
    }
    // Connection and pose validity are properties of the runtime state, not of
    // the press levels; callers set them explicitly around this call.
  }
  analog_latch_.fill(false);
  have_published_ = true;
}

int32_t ControllerButtonToAndroidKey(int hand, ControllerButton button) {
  // Android KEYCODE_BUTTON_* values already verified end-to-end by the
  // existing physical-gamepad route (MapSdlGamepadButtonToAndroid). The
  // per-hand squeeze/thumbstick map onto the shoulder/thumb identifiers the
  // guest already accepts; the right hand uses the R-side variants.
  const bool right = hand == 1;
  switch (button) {
  case ControllerButton::kFaceSouth:
    return 96; // BUTTON_A
  case ControllerButton::kFaceEast:
    return 97; // BUTTON_B
  case ControllerButton::kFaceWest:
    return 99; // BUTTON_X
  case ControllerButton::kFaceNorth:
    return 100; // BUTTON_Y
  case ControllerButton::kSqueeze:
    return right ? 103 : 102; // R1 / L1
  case ControllerButton::kThumbstick:
    return right ? 107 : 106; // THUMB_R / L
  case ControllerButton::kMenu:
    return 108; // BUTTON_START
  case ControllerButton::kTrigger:
    return -1; // analog click, no keycode
  case ControllerButton::kCount:
    break;
  }
  return -1;
}

int32_t ControllerAxisToAndroidAxis(int hand, ControllerAxis axis) {
  // Android MotionEvent AXIS_* values from kAndroidAxes in the verified
  // gamepad route: {AXIS_X=0, AXIS_Y=1, AXIS_Z=11, AXIS_RZ=14,
  // AXIS_LTRIGGER=17, AXIS_RTRIGGER=18}.
  switch (axis) {
  case ControllerAxis::kThumbstickX:
    return hand == 1 ? 11 : 0; // Z / X
  case ControllerAxis::kThumbstickY:
    return hand == 1 ? 14 : 1; // RZ / Y
  case ControllerAxis::kTrigger:
    return hand == 1 ? 18 : 17; // R / L trigger
  case ControllerAxis::kSqueeze:
    return -1; // squeeze has no analog axis id
  case ControllerAxis::kCount:
    break;
  }
  return -1;
}

int32_t ControllerAxisComponentCount(ControllerAxis axis) {
  switch (axis) {
  case ControllerAxis::kThumbstickX:
  case ControllerAxis::kThumbstickY:
    return 2; // sticks are delivered as an (x, y) motion pair
  case ControllerAxis::kTrigger:
    return 1;
  default:
    return 0;
  }
}

int32_t ControllerButtonToSdlOrdinal(int hand, ControllerButton button) {
  // SDL_GamepadButton ordinals (SDL3): SOUTH=0, EAST=1, WEST=2, NORTH=3,
  // BACK=4, START=6, LEFT_STICK=7, RIGHT_STICK=8, LEFT_SHOULDER=9,
  // RIGHT_SHOULDER=10. The router converts these to the verified Android
  // keycodes; XR delivery must not bypass that conversion.
  switch (button) {
  case ControllerButton::kFaceSouth:
    return 0;
  case ControllerButton::kFaceEast:
    return 1;
  case ControllerButton::kFaceWest:
    return 2;
  case ControllerButton::kFaceNorth:
    return 3;
  case ControllerButton::kThumbstick:
    return hand == 1 ? 8 : 7;
  case ControllerButton::kSqueeze:
    return hand == 1 ? 10 : 9;
  case ControllerButton::kMenu:
    return 6;
  case ControllerButton::kTrigger:
    return -1; // analog click, no button
  case ControllerButton::kCount:
    break;
  }
  return -1;
}

int32_t ControllerAxisToSdlOrdinal(int hand, ControllerAxis axis) {
  // SDL_GamepadAxis ordinals (SDL3): LEFTX=0, LEFTY=1, RIGHTX=2, RIGHTY=3,
  // TRIGGERLEFT=4, TRIGGERRIGHT=5.
  switch (axis) {
  case ControllerAxis::kThumbstickX:
    return hand == 1 ? 2 : 0;
  case ControllerAxis::kThumbstickY:
    return hand == 1 ? 3 : 1;
  case ControllerAxis::kTrigger:
    return hand == 1 ? 5 : 4;
  case ControllerAxis::kSqueeze:
    return -1; // delivered as a button only
  case ControllerAxis::kCount:
    break;
  }
  return -1;
}

bool EncodeNativeControllerChannels(const ControllerSnapshot &snapshot,
                                    float (&channels)[28]) {
  std::fill(std::begin(channels), std::end(channels), 0.f);
  for (int hand = 0; hand < 2; ++hand) {
    const auto &input = snapshot.hands[hand];
    if (!input.connected)
      continue;
    auto button = [&](ControllerButton b) {
      return input.buttons[static_cast<std::size_t>(b)] ? 1.f : 0.f;
    };
    const int base = hand * 9;
    channels[base] = button(ControllerButton::kThumbstick);
    channels[base + 1] =
        input.axes[static_cast<std::size_t>(ControllerAxis::kThumbstickX)];
    channels[base + 2] =
        input.axes[static_cast<std::size_t>(ControllerAxis::kThumbstickY)];
    channels[base + 3] =
        input.axes[static_cast<std::size_t>(ControllerAxis::kTrigger)];
    channels[base + 4] =
        input.axes[static_cast<std::size_t>(ControllerAxis::kSqueeze)];
    channels[hand == 0 ? 24 : 22] = button(
        hand == 0 ? ControllerButton::kFaceWest : ControllerButton::kFaceSouth);
    channels[hand == 0 ? 25 : 23] = button(
        hand == 0 ? ControllerButton::kFaceNorth : ControllerButton::kFaceEast);
    channels[27] = std::max(channels[27], button(ControllerButton::kMenu));
  }

  return snapshot.hands[0].connected || snapshot.hands[1].connected;
}

void EncodeControllerDelivery(const ControllerDelivery &delivery,
                              MocktailVrControllerDelivery *out) {
  if (!out)
    return;
  *out = MocktailVrControllerDelivery{};
  out->frame = delivery.frame;
  out->resync = delivery.resync ? 1 : 0;
  out->session_generation = delivery.session_generation;
  const std::size_t button_count =
      static_cast<std::size_t>(mocktail::vr::ControllerButton::kCount);
  for (int hand = 0; hand < 2; ++hand) {
    out->connected[hand] = delivery.connected[hand] ? 1 : 0;
    out->connection_changed[hand] = delivery.connection_changed[hand] ? 1 : 0;
    out->axes_valid[hand] = delivery.axes_valid[hand] ? 1 : 0;
    for (std::size_t index = 0;
         index < button_count &&
         index < static_cast<std::size_t>(MOCKTAIL_VR_CONTROLLER_BUTTON_COUNT);
         ++index) {
      out->button_levels[hand][index] =
          delivery.button_levels[hand][index] ? 1 : 0;
    }
  }
  for (int axis = 0; axis < MOCKTAIL_VR_CONTROLLER_AXIS_COUNT; ++axis) {
    out->axes[axis] = delivery.axes[axis];
  }
  const std::size_t edges = std::min<std::size_t>(
      delivery.edges.size(), MOCKTAIL_VR_CONTROLLER_MAX_EDGES);
  out->edge_count = static_cast<int32_t>(edges);
  for (std::size_t index = 0; index < edges; ++index) {
    out->edges[index].hand = delivery.edges[index].hand;
    out->edges[index].button =
        static_cast<int32_t>(delivery.edges[index].button);
    out->edges[index].pressed = delivery.edges[index].pressed ? 1 : 0;
  }
}

void ControllerDeliveryQueue::Push(ControllerDelivery delivery) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (overflowed_) {
    // The next delivered frame must re-sync full levels because intermediate
    // edges were dropped with the discarded frames.
    delivery.resync = true;
    overflowed_ = false;
  }
  if (queue_.size() >= kCapacity) {
    queue_.pop_front();
    ++dropped_frames_;
    overflowed_ = true;
    // Edges inside the dropped frame are lost; converge through resync on the
    // newest frame so nothing can stick down.
    delivery.resync = true;
  }
  queue_.push_back(std::move(delivery));
}

bool ControllerDeliveryQueue::Pop(ControllerDelivery *out) {
  if (out == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) {
    return false;
  }
  *out = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

std::size_t ControllerDeliveryQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

void ControllerDeliveryQueue::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.clear();
  overflowed_ = false;
}

} // namespace mocktail::vr
