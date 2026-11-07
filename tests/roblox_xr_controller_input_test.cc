#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <variant>
#include <vector>

#include "mocktail/platform/platform_runtime.h"
#include "mocktail/vr/xr_controller_abi.h"
#include "runtime/roblox_xr_controller_input.h"

namespace mocktail {
namespace runtime {
namespace {

using platform::GamepadAxisEvent;
using platform::GamepadButtonEvent;
using platform::GamepadConnectionEvent;
using platform::PlatformEvent;

// The XR controller ABI is context-free (resolved through dlsym in
// production), so the test doubles live at file scope exactly like the real
// exported symbols would.
struct FakeBackend {
  std::deque<MocktailVrControllerDelivery> queue;
  int haptic_calls = 0;
  int stop_calls = 0;
  float last_amplitude = 0.f;
  std::uint64_t last_duration_ns = 0;
  int last_hand = -1;
} g_backend;

std::vector<PlatformEvent> g_events;

int32_t FakePop(MocktailVrControllerDelivery *out) {
  if (g_backend.queue.empty()) {
    return 0;
  }
  *out = g_backend.queue.front();
  g_backend.queue.pop_front();
  return 1;
}

void FakeHaptics(int32_t hand, float amplitude, std::uint64_t duration_ns,
                 float) {
  g_backend.haptic_calls++;
  g_backend.last_hand = hand;
  g_backend.last_amplitude = amplitude;
  g_backend.last_duration_ns = duration_ns;
}

void FakeStop(int32_t hand) {
  g_backend.stop_calls++;
  g_backend.last_hand = hand;
}

void FakeSink(void *, const PlatformEvent &event) { g_events.push_back(event); }

template <typename T> std::vector<T> Collect() {
  std::vector<T> out;
  for (const auto &event : g_events) {
    if (const auto *value = std::get_if<T>(&event.payload)) {
      out.push_back(*value);
    }
  }
  return out;
}

MocktailVrControllerDelivery MakeDelivery() {
  MocktailVrControllerDelivery delivery{};
  delivery.connected[0] = 1;
  delivery.connected[1] = 1;
  delivery.axes_valid[0] = 1;
  delivery.axes_valid[1] = 1;
  return delivery;
}

class XrControllerInputTest : public ::testing::Test {
protected:
  void SetUp() override {
    g_backend = FakeBackend{};
    g_events.clear();
    ASSERT_TRUE(input_.InitializeWithAbi(&FakeSink, nullptr, &FakePop,
                                         &FakeHaptics, &FakeStop));
    ASSERT_TRUE(input_.active());
  }
  RobloxXrControllerInput input_;
};

TEST_F(XrControllerInputTest, ConnectsOnceAndMergesBothHandsIntoOneGamepad) {
  auto delivery = MakeDelivery();
  delivery.frame = 1;
  // Left hand presses X (face west), right hand presses A (face south).
  delivery.button_levels[0][MOCKTAIL_VR_BUTTON_FACE_WEST] = 1;
  delivery.button_levels[1][MOCKTAIL_VR_BUTTON_FACE_SOUTH] = 1;
  delivery.edge_count = 2;
  delivery.edges[0] = {0, MOCKTAIL_VR_BUTTON_FACE_WEST, 1};
  delivery.edges[1] = {1, MOCKTAIL_VR_BUTTON_FACE_SOUTH, 1};
  g_backend.queue.push_back(delivery);

  input_.Drain();

  const auto connections = Collect<GamepadConnectionEvent>();
  ASSERT_EQ(connections.size(), 1u);
  EXPECT_TRUE(connections[0].connected);
  EXPECT_EQ(connections[0].instance_id, RobloxXrControllerInput::kInstanceLeft);
  const auto buttons = Collect<GamepadButtonEvent>();
  ASSERT_EQ(buttons.size(), 2u);
  EXPECT_EQ(buttons[0].instance_id, RobloxXrControllerInput::kInstanceLeft);
  EXPECT_TRUE(buttons[0].pressed);
  EXPECT_TRUE(buttons[1].pressed);
  EXPECT_NE(buttons[0].button, buttons[1].button);
}

TEST_F(XrControllerInputTest,
       ResyncDoesNotCancelLeftHandWithUnboundRightButton) {
  auto d = MakeDelivery();
  d.resync = 1;
  d.button_levels[0][MOCKTAIL_VR_BUTTON_FACE_WEST] = 1;
  g_backend.queue.push_back(d);
  input_.Drain();
  auto buttons = Collect<GamepadButtonEvent>();
  bool west = false;
  for (const auto &b : buttons)
    if (b.button == 2)
      west = b.pressed;
  EXPECT_TRUE(west);
}
TEST_F(XrControllerInputTest, SharedMenuReleasesOnlyAfterBothHandsRelease) {
  auto d = MakeDelivery();
  d.button_levels[0][MOCKTAIL_VR_BUTTON_MENU] = 1;
  d.button_levels[1][MOCKTAIL_VR_BUTTON_MENU] = 1;
  g_backend.queue.push_back(d);
  input_.Drain();
  g_events.clear();
  d.button_levels[0][MOCKTAIL_VR_BUTTON_MENU] = 0;
  g_backend.queue.push_back(d);
  input_.Drain();
  EXPECT_TRUE(Collect<GamepadButtonEvent>().empty());
  d.button_levels[1][MOCKTAIL_VR_BUTTON_MENU] = 0;
  g_backend.queue.push_back(d);
  input_.Drain();
  auto buttons = Collect<GamepadButtonEvent>();
  ASSERT_EQ(buttons.size(), 1u);
  EXPECT_FALSE(buttons[0].pressed);
}
TEST_F(XrControllerInputTest, NewGenerationDisconnectsAndRejectsLateOldFrames) {
  auto d = MakeDelivery();
  d.session_generation = 1;
  g_backend.queue.push_back(d);
  input_.Drain();
  g_events.clear();
  d.session_generation = 2;
  g_backend.queue.push_back(d);
  input_.Drain();
  auto connections = Collect<GamepadConnectionEvent>();
  ASSERT_EQ(connections.size(), 2u);
  EXPECT_FALSE(connections[0].connected);
  EXPECT_TRUE(connections[1].connected);
  g_events.clear();
  d.session_generation = 1;
  g_backend.queue.push_back(d);
  input_.Drain();
  EXPECT_TRUE(g_events.empty());
}

TEST_F(XrControllerInputTest, AxesDeliveredAsRawInt16ForSingleDeadzone) {
  auto delivery = MakeDelivery();
  delivery.frame = 1;
  delivery.axes[0] = -1.0f; // LEFTX
  delivery.axes[5] = 0.5f;  // TRIGGERRIGHT
  g_backend.queue.push_back(delivery);

  input_.Drain();

  const auto axes = Collect<GamepadAxisEvent>();
  ASSERT_FALSE(axes.empty());
  bool saw_left_x = false, saw_right_trigger = false;
  for (const auto &axis : axes) {
    EXPECT_EQ(axis.instance_id, RobloxXrControllerInput::kInstanceLeft);
    if (axis.axis == 0) {
      EXPECT_EQ(axis.value, -32767);
      saw_left_x = true;
    }
    if (axis.axis == 5) {
      EXPECT_NEAR(axis.value, 16384, 2);
      saw_right_trigger = true;
    }
  }
  EXPECT_TRUE(saw_left_x);
  EXPECT_TRUE(saw_right_trigger);
}

TEST_F(XrControllerInputTest, DisconnectReleasesAndStopsFurtherDelivery) {
  auto connected = MakeDelivery();
  connected.frame = 1;
  connected.button_levels[1][MOCKTAIL_VR_BUTTON_FACE_SOUTH] = 1;
  connected.edge_count = 1;
  connected.edges[0] = {1, MOCKTAIL_VR_BUTTON_FACE_SOUTH, 1};
  g_backend.queue.push_back(connected);
  input_.Drain();
  ASSERT_TRUE(input_.connected());

  auto disconnected = MakeDelivery();
  disconnected.frame = 2;
  disconnected.connected[0] = 0;
  disconnected.connected[1] = 0;
  g_backend.queue.push_back(disconnected);
  input_.Drain();

  const auto connections = Collect<GamepadConnectionEvent>();
  ASSERT_EQ(connections.size(), 2u);
  EXPECT_TRUE(connections[0].connected);
  EXPECT_FALSE(connections[1].connected);
  EXPECT_FALSE(input_.connected());
}

TEST_F(XrControllerInputTest, ResyncReDeliversEveryHeldLevel) {
  auto delivery = MakeDelivery();
  delivery.frame = 5;
  delivery.resync = 1;
  // Right A held, left stick click held.
  delivery.button_levels[1][MOCKTAIL_VR_BUTTON_FACE_SOUTH] = 1;
  delivery.button_levels[0][MOCKTAIL_VR_BUTTON_THUMBSTICK] = 1;
  g_backend.queue.push_back(delivery);

  input_.Drain();

  EXPECT_EQ(input_.resyncs(), 1u);
  const auto buttons = Collect<GamepadButtonEvent>();
  bool saw_face_south = false, saw_thumbstick = false;
  for (const auto &button : buttons) {
    if (!button.pressed) {
      continue;
    }
    if (button.button == 0)
      saw_face_south = true; // SOUTH ordinal 0
    if (button.button == 7)
      saw_thumbstick = true; // LEFT_STICK ordinal 7
  }
  EXPECT_TRUE(saw_face_south);
  EXPECT_TRUE(saw_thumbstick);
}

TEST_F(XrControllerInputTest, DrainIsBoundedPerPump) {
  for (int index = 0; index < 10; ++index) {
    auto delivery = MakeDelivery();
    delivery.frame = static_cast<std::uint64_t>(index);
    g_backend.queue.push_back(delivery);
  }
  input_.Drain();
  // kMaxDrainPerPump is 4, so at least 6 batches remain queued.
  EXPECT_GE(g_backend.queue.size(), 6u);
}

TEST_F(XrControllerInputTest, HapticsForwardedAndStopOnRequest) {
  input_.RequestHaptics(1, 0.75f, 120000000ULL, 160.f);
  EXPECT_EQ(g_backend.haptic_calls, 1);
  EXPECT_EQ(g_backend.last_hand, 1);
  EXPECT_FLOAT_EQ(g_backend.last_amplitude, 0.75f);
  EXPECT_EQ(g_backend.last_duration_ns, 120000000ULL);
  input_.StopHaptics(0);
  EXPECT_EQ(g_backend.stop_calls, 1);
  EXPECT_EQ(g_backend.last_hand, 0);
  // Invalid hand indices are ignored.
  input_.RequestHaptics(5, 1.f, 0, 0.f);
  EXPECT_EQ(g_backend.haptic_calls, 1);
}

TEST_F(XrControllerInputTest, InertWithoutPopFunction) {
  RobloxXrControllerInput inert;
  EXPECT_FALSE(
      inert.InitializeWithAbi(&FakeSink, nullptr, nullptr, nullptr, nullptr));
  EXPECT_FALSE(inert.active());
  inert.Drain(); // safe no-op
  EXPECT_TRUE(g_events.empty());
}

TEST_F(XrControllerInputTest, ShutdownDisconnectsSyntheticController) {
  auto delivery = MakeDelivery();
  delivery.frame = 1;
  g_backend.queue.push_back(delivery);
  input_.Drain();
  ASSERT_TRUE(input_.connected());
  input_.Shutdown();
  const auto connections = Collect<GamepadConnectionEvent>();
  ASSERT_FALSE(connections.empty());
  EXPECT_FALSE(connections.back().connected);
  EXPECT_FALSE(input_.connected());
}

} // namespace
} // namespace runtime
} // namespace mocktail
