#include "runtime/roblox_xr_controller_input.h"

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "mocktail/vr/xr_controller.h"
#include "mocktail/vr/xr_controller_abi.h"

namespace mocktail {
namespace runtime {
namespace {

// SDL3 gamepad ordinals for the merged synthetic controller. Both hands feed
// one device: left hand -> left stick/left shoulder/X/Y, right hand -> right
// stick/right shoulder/A/B; menu -> START.
constexpr std::int16_t kAxisFull = 32767;

std::int16_t FloatToRaw(float value) {
  const float clamped =
      std::clamp(std::isfinite(value) ? value : 0.f, -1.f, 1.f);
  return static_cast<std::int16_t>(std::lround(clamped * kAxisFull));
}

std::int16_t TriggerToRaw(float value) {
  const float clamped =
      std::clamp(std::isfinite(value) ? value : 0.f, 0.f, 1.f);
  return static_cast<std::int16_t>(std::lround(clamped * kAxisFull));
}

std::uint64_t NowNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

} // namespace

bool RobloxXrControllerInput::Initialize(EventSink sink, void *context) {
  if (sink == nullptr) {
    return false;
  }
  // Resolve the XR controller ABI through dlsym so this module links and runs
  // in non-VR builds (symbols absent) and stays inert while no backend is
  // armed. Never a hard dependency on OpenXR here.
  auto pop = reinterpret_cast<MocktailVrControllerPopFn>(
      dlsym(RTLD_DEFAULT, "mocktail_vr_controller_pop_delivery"));
  auto haptics = reinterpret_cast<MocktailVrControllerHapticsFn>(
      dlsym(RTLD_DEFAULT, "mocktail_vr_controller_request_haptics"));
  auto stop_haptics = reinterpret_cast<MocktailVrControllerStopHapticsFn>(
      dlsym(RTLD_DEFAULT, "mocktail_vr_controller_stop_haptics"));
  return InitializeWithAbi(sink, context, pop, haptics, stop_haptics);
}

bool RobloxXrControllerInput::InitializeWithAbi(
    EventSink sink, void *context, MocktailVrControllerPopFn pop,
    MocktailVrControllerHapticsFn haptics,
    MocktailVrControllerStopHapticsFn stop_haptics) {
  if (sink == nullptr || pop == nullptr) {
    // No XR controller layer (non-VR build or backend without controllers).
    // This is the normal case for --no-vr and must not be treated as failure.
    sink_ = nullptr;
    context_ = nullptr;
    pop_ = nullptr;
    haptics_ = nullptr;
    stop_haptics_ = nullptr;
    initialized_ = false;
    return false;
  }
  sink_ = sink;
  context_ = context;
  pop_ = reinterpret_cast<void *>(pop);
  haptics_ = reinterpret_cast<void *>(haptics);
  stop_haptics_ = reinterpret_cast<void *>(stop_haptics);
  initialized_ = true;
  return true;
}

void RobloxXrControllerInput::Shutdown() {
  if (connected_) {
    // Release the synthetic controller once so no button or stick stays live
    // after teardown.
    EmitConnection(false);
  }
  initialized_ = false;
  generation_ = 0;
  force_resync_ = false;
  sink_ = nullptr;
  context_ = nullptr;
  pop_ = nullptr;
  haptics_ = nullptr;
  stop_haptics_ = nullptr;
}

void RobloxXrControllerInput::EmitConnection(bool connected) {
  if (sink_ == nullptr) {
    return;
  }
  platform::GamepadDescriptor descriptor;
  descriptor.family = platform::GamepadFamily::kXbox;
  descriptor.name = connected ? "Mocktail VR Controllers" : "";
  if (connected) {
    // Advertise exactly the buttons/axes this route can emit so the router
    // accepts them: SOUTH/EAST/WEST/NORTH, START, both stick clicks, both
    // shoulders.
    for (const std::uint8_t button :
         {static_cast<std::uint8_t>(vr::ControllerButtonToSdlOrdinal(
              0, vr::ControllerButton::kFaceWest)),
          static_cast<std::uint8_t>(vr::ControllerButtonToSdlOrdinal(
              0, vr::ControllerButton::kFaceNorth)),
          static_cast<std::uint8_t>(vr::ControllerButtonToSdlOrdinal(
              1, vr::ControllerButton::kFaceSouth)),
          static_cast<std::uint8_t>(vr::ControllerButtonToSdlOrdinal(
              1, vr::ControllerButton::kFaceEast)),
          static_cast<std::uint8_t>(
              vr::ControllerButtonToSdlOrdinal(0, vr::ControllerButton::kMenu)),
          static_cast<std::uint8_t>(vr::ControllerButtonToSdlOrdinal(
              0, vr::ControllerButton::kThumbstick)),
          static_cast<std::uint8_t>(vr::ControllerButtonToSdlOrdinal(
              1, vr::ControllerButton::kThumbstick)),
          static_cast<std::uint8_t>(vr::ControllerButtonToSdlOrdinal(
              0, vr::ControllerButton::kSqueeze)),
          static_cast<std::uint8_t>(vr::ControllerButtonToSdlOrdinal(
              1, vr::ControllerButton::kSqueeze))}) {
      descriptor.buttons |= (std::uint32_t{1} << button);
    }
    // Six axes: both sticks (x/y) and both triggers.
    descriptor.axes = 0b111111;
  }
  platform::GamepadConnectionEvent event;
  event.instance_id = kInstanceLeft;
  event.connected = connected;
  event.descriptor = std::move(descriptor);
  event.remapped = false;
  sink_(context_, platform::PlatformEvent{NowNs(), std::move(event)});
  connected_ = connected;
  std::fill(std::begin(last_buttons_), std::end(last_buttons_), false);
  if (connected) {
    std::fprintf(stderr, "  [vr-input] XR controllers connected to Roblox as a "
                         "single synthetic gamepad\n");
  } else {
    std::fprintf(stderr,
                 "  [vr-input] XR controllers disconnected from Roblox\n");
  }
}

void RobloxXrControllerInput::EmitButton(int sdl_button, bool pressed) {
  if (sink_ == nullptr || sdl_button < 0) {
    return;
  }
  platform::GamepadButtonEvent event;
  event.instance_id = kInstanceLeft;
  event.button = static_cast<std::uint8_t>(sdl_button);
  event.pressed = pressed;
  sink_(context_, platform::PlatformEvent{NowNs(), std::move(event)});
  ++delivered_events_;
}

void RobloxXrControllerInput::EmitAxis(int sdl_axis, std::int16_t value) {
  if (sink_ == nullptr || sdl_axis < 0) {
    return;
  }
  platform::GamepadAxisEvent event;
  event.instance_id = kInstanceLeft;
  event.axis = static_cast<std::uint8_t>(sdl_axis);
  event.value = value;
  sink_(context_, platform::PlatformEvent{NowNs(), std::move(event)});
  ++delivered_events_;
}

void RobloxXrControllerInput::Drain() {
  if (!initialized_ || pop_ == nullptr || sink_ == nullptr) {
    return;
  }
  auto pop = reinterpret_cast<MocktailVrControllerPopFn>(pop_);
  for (int batch = 0; batch < kMaxDrainPerPump; ++batch) {
    MocktailVrControllerDelivery delivery{};
    if (pop(&delivery) == 0) {
      return;
    }
    if (delivery.session_generation < generation_)
      continue;
    if (delivery.session_generation != generation_) {
      if (connected_)
        EmitConnection(false);
      generation_ = delivery.session_generation;
    }
    // Connection transitions first so subsequent button/axis events land on a
    // registered device. Either hand being connected presents the merged
    // controller as connected.
    const bool connected = delivery.connected[0] || delivery.connected[1];
    if (connected != connected_) {
      EmitConnection(connected);
    }
    if (!connected_) {
      // Nothing to deliver while disconnected; the disconnect already released
      // every control through the router.
      continue;
    }

    if (force_resync_) {
      delivery.resync = 1;
      force_resync_ = false;
    }
    if (delivery.resync)
      ++resyncs_;
    // Levels belong to this delivery, not a separately read newer snapshot.
    // Merge first so a release/unbound button on one hand cannot cancel the
    // other.
    bool merged[16] = {};
    for (int hand = 0; hand < 2; ++hand) {
      if (!delivery.connected[hand])
        continue;
      for (int index = 0; index < MOCKTAIL_VR_CONTROLLER_BUTTON_COUNT;
           ++index) {
        const int button = vr::ControllerButtonToSdlOrdinal(
            hand, static_cast<vr::ControllerButton>(index));
        if (button >= 0 && button < 16)
          merged[button] |= delivery.button_levels[hand][index] != 0;
      }
    }
    for (int button = 0; button < 16; ++button) {
      if (delivery.resync || merged[button] != last_buttons_[button])
        EmitButton(button, merged[button]);
      last_buttons_[button] = merged[button];
    }

    // Analog axes. Delivered as raw int16 so the router applies its single
    // deadzone; we never pre-filter (avoids a double deadzone).
    if (delivery.axes_valid[0] || delivery.axes_valid[1]) {
      EmitAxis(
          vr::ControllerAxisToSdlOrdinal(0, vr::ControllerAxis::kThumbstickX),
          FloatToRaw(delivery.axes[0]));
      EmitAxis(
          vr::ControllerAxisToSdlOrdinal(0, vr::ControllerAxis::kThumbstickY),
          FloatToRaw(delivery.axes[1]));
      EmitAxis(
          vr::ControllerAxisToSdlOrdinal(1, vr::ControllerAxis::kThumbstickX),
          FloatToRaw(delivery.axes[2]));
      EmitAxis(
          vr::ControllerAxisToSdlOrdinal(1, vr::ControllerAxis::kThumbstickY),
          FloatToRaw(delivery.axes[3]));
      EmitAxis(vr::ControllerAxisToSdlOrdinal(0, vr::ControllerAxis::kTrigger),
               TriggerToRaw(delivery.axes[4]));
      EmitAxis(vr::ControllerAxisToSdlOrdinal(1, vr::ControllerAxis::kTrigger),
               TriggerToRaw(delivery.axes[5]));
    }
  }
}

void RobloxXrControllerInput::RequestHaptics(int hand, float amplitude,
                                             std::uint64_t duration_ns,
                                             float frequency_hz) {
  if (haptics_ == nullptr || (hand != 0 && hand != 1)) {
    return;
  }
  reinterpret_cast<MocktailVrControllerHapticsFn>(haptics_)(
      hand, amplitude, duration_ns, frequency_hz);
}

void RobloxXrControllerInput::StopHaptics(int hand) {
  if (stop_haptics_ == nullptr || (hand != 0 && hand != 1)) {
    return;
  }
  reinterpret_cast<MocktailVrControllerStopHapticsFn>(stop_haptics_)(hand);
}

} // namespace runtime
} // namespace mocktail
