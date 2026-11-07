#include <gtest/gtest.h>

#include <cmath>

#include "mocktail/vr/xr_controller.h"

namespace mocktail::vr {
namespace {

using Button = ControllerButton;
using Axis = ControllerAxis;

TEST(XrControllerState,
     PublishEmitsOneEdgePerTransitionDespiteRepeatedGetters) {
  ControllerStateBuilder builder;
  std::vector<ControllerStateBuilder::Edge> edges;

  builder.SetConnected(0, true);
  builder.SetButton(0, Button::kFaceSouth, true, /*active=*/true);
  const auto first = builder.Publish(1, 0, &edges);
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_EQ(edges[0].hand, 0);
  EXPECT_EQ(edges[0].button, Button::kFaceSouth);
  EXPECT_TRUE(edges[0].pressed);

  // Repeated publishes at the same level emit no further edges: native getters
  // reading the snapshot must never see duplicate transitions.
  edges.clear();
  builder.SetButton(0, Button::kFaceSouth, true, true);
  builder.Publish(2, 0, &edges);
  builder.Publish(3, 0, &edges);
  EXPECT_TRUE(edges.empty());

  edges.clear();
  builder.SetButton(0, Button::kFaceSouth, false, true);
  builder.Publish(4, 0, &edges);
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_FALSE(edges[0].pressed);
  EXPECT_EQ(first.hands[0].connected, true);
}

TEST(XrControllerState, InactiveActionsNeverPressAndReleaseHeldControls) {
  ControllerStateBuilder builder;
  std::vector<ControllerStateBuilder::Edge> edges;
  builder.SetButton(0, Button::kMenu, false, true);
  builder.Publish(1, 0, &edges);

  edges.clear();
  // An inactive action reporting "true" must not fabricate a press.
  builder.SetButton(0, Button::kMenu, true, /*active=*/false);
  builder.Publish(2, 0, &edges);
  EXPECT_TRUE(edges.empty());
  EXPECT_FALSE(builder.snapshot()
                   .hands[0]
                   .buttons[static_cast<std::size_t>(Button::kMenu)]);
}

TEST(XrControllerState, InactiveActionReleasesPreviouslyHeldControl) {
  ControllerStateBuilder b;
  b.SetButton(0, Button::kMenu, true, true);
  b.Publish(1, 0, nullptr);
  b.SetButton(0, Button::kMenu, true, false);
  std::vector<ControllerStateBuilder::Edge> edges;
  b.Publish(2, 0, &edges);
  ASSERT_EQ(edges.size(), 1u);
  EXPECT_FALSE(edges[0].pressed);
}

TEST(XrControllerState, ReleaseAllEmitsReleaseForHeldButtonsOnlyAndZeroesAxes) {
  ControllerStateBuilder builder;
  std::vector<ControllerStateBuilder::Edge> edges;
  builder.SetButton(0, Button::kFaceSouth, true, true);
  builder.SetButton(1, Button::kSqueeze, true, true);
  builder.SetAxis(0, Axis::kThumbstickX, 0.8f, true);
  builder.SetAxis(0, Axis::kTrigger, 1.f, true);
  builder.Publish(1, 0, &edges);
  EXPECT_EQ(edges.size(), 2u);

  edges.clear();
  builder.ReleaseAll(2, 0, &edges);
  ASSERT_EQ(edges.size(), 2u);
  for (const auto &edge : edges) {
    EXPECT_FALSE(edge.pressed);
  }
  const auto &snapshot = builder.snapshot();
  EXPECT_FLOAT_EQ(
      snapshot.hands[0].axes[static_cast<std::size_t>(Axis::kThumbstickX)],
      0.f);
  EXPECT_FLOAT_EQ(
      snapshot.hands[0].axes[static_cast<std::size_t>(Axis::kTrigger)], 0.f);
  // A second ReleaseAll must not re-emit anything (no stuck-down, no repeats).
  edges.clear();
  builder.ReleaseAll(3, 0, &edges);
  EXPECT_TRUE(edges.empty());
}

TEST(XrControllerState, AxesAreClampedAndNonFiniteRejected) {
  ControllerStateBuilder builder;
  builder.SetAxis(0, Axis::kThumbstickY, 4.5f, true);
  builder.SetAxis(1, Axis::kTrigger, -2.f, true);
  builder.SetAxis(0, Axis::kTrigger, 0.5f, true);
  builder.SetAxis(0, Axis::kTrigger, std::nanf(""), true);
  builder.Publish(1, 0, nullptr);
  const auto &s = builder.snapshot();
  EXPECT_FLOAT_EQ(s.hands[0].axes[static_cast<std::size_t>(Axis::kThumbstickY)],
                  1.f);
  EXPECT_FLOAT_EQ(s.hands[1].axes[static_cast<std::size_t>(Axis::kTrigger)],
                  0.f);
  // Invalid input must neutralize any previously held trigger.
  EXPECT_FLOAT_EQ(s.hands[0].axes[static_cast<std::size_t>(Axis::kTrigger)],
                  0.f);
}

TEST(XrControllerState, AnalogClickHysteresis) {
  ControllerStateBuilder builder;
  builder.SetAxis(0, Axis::kTrigger, 0.7f, true);
  EXPECT_FALSE(builder.AnalogClick(0, Axis::kTrigger)); // below press 0.75
  builder.SetAxis(0, Axis::kTrigger, 0.8f, true);
  EXPECT_TRUE(builder.AnalogClick(0, Axis::kTrigger)); // latched on
  builder.SetAxis(0, Axis::kTrigger, 0.65f, true);
  EXPECT_TRUE(builder.AnalogClick(0, Axis::kTrigger)); // inside hysteresis band
  builder.SetAxis(0, Axis::kTrigger, 0.55f, true);
  EXPECT_FALSE(builder.AnalogClick(0, Axis::kTrigger)); // below release 0.6
  // Non-click axes and invalid hands are rejected.
  EXPECT_FALSE(builder.AnalogClick(0, Axis::kThumbstickX));
  EXPECT_FALSE(builder.AnalogClick(5, Axis::kTrigger));
}

TEST(XrControllerState, SessionGenerationAndFrameTrackPublish) {
  ControllerStateBuilder builder;
  const auto snapshot = builder.Publish(42, 7, nullptr);
  EXPECT_EQ(snapshot.frame, 42u);
  EXPECT_EQ(snapshot.session_generation, 7u);
}

TEST(XrControllerAndroidMapping, PerHandKeysAndAxesMatchVerifiedGamepadRoute) {
  // Values pinned against MapSdlGamepadButtonToAndroid/kAndroidAxes.
  EXPECT_EQ(ControllerButtonToAndroidKey(0, Button::kFaceSouth), 96);
  EXPECT_EQ(ControllerButtonToAndroidKey(1, Button::kFaceSouth), 96);
  EXPECT_EQ(ControllerButtonToAndroidKey(0, Button::kFaceEast), 97);
  EXPECT_EQ(ControllerButtonToAndroidKey(0, Button::kFaceWest), 99);
  EXPECT_EQ(ControllerButtonToAndroidKey(0, Button::kFaceNorth), 100);
  EXPECT_EQ(ControllerButtonToAndroidKey(0, Button::kSqueeze), 102); // L1
  EXPECT_EQ(ControllerButtonToAndroidKey(1, Button::kSqueeze), 103); // R1
  EXPECT_EQ(ControllerButtonToAndroidKey(0, Button::kThumbstick), 106);
  EXPECT_EQ(ControllerButtonToAndroidKey(1, Button::kThumbstick), 107);
  EXPECT_EQ(ControllerButtonToAndroidKey(0, Button::kMenu), 108);
  EXPECT_EQ(ControllerButtonToAndroidKey(0, Button::kTrigger), -1); // analog
  EXPECT_EQ(ControllerAxisToAndroidAxis(0, Axis::kThumbstickX), 0);
  EXPECT_EQ(ControllerAxisToAndroidAxis(0, Axis::kThumbstickY), 1);
  EXPECT_EQ(ControllerAxisToAndroidAxis(1, Axis::kThumbstickX), 11);
  EXPECT_EQ(ControllerAxisToAndroidAxis(1, Axis::kThumbstickY), 14);
  EXPECT_EQ(ControllerAxisToAndroidAxis(0, Axis::kTrigger), 17);
  EXPECT_EQ(ControllerAxisToAndroidAxis(1, Axis::kTrigger), 18);
  EXPECT_EQ(ControllerAxisToAndroidAxis(0, Axis::kSqueeze), -1);
  EXPECT_EQ(ControllerAxisComponentCount(Axis::kThumbstickX), 2);
  EXPECT_EQ(ControllerAxisComponentCount(Axis::kTrigger), 1);
  EXPECT_EQ(ControllerAxisComponentCount(Axis::kSqueeze), 0);
}

TEST(XrControllerState, InvalidHandAndButtonIndicesAreIgnored) {
  ControllerStateBuilder builder;
  builder.SetButton(-1, Button::kFaceSouth, true, true);
  builder.SetButton(9, Button::kFaceSouth, true, true);
  builder.SetAxis(7, Axis::kTrigger, 1.f, true);
  builder.SetConnected(3, true);
  const auto snapshot = builder.Publish(1, 0, nullptr);
  EXPECT_FALSE(snapshot.hands[0].connected);
  EXPECT_FALSE(snapshot.hands[1].connected);
  EXPECT_FALSE(
      snapshot.hands[0].buttons[static_cast<std::size_t>(Button::kFaceSouth)]);
}

TEST(XrControllerQueue, BoundedCapacityDropsOldestAndArmsResync) {
  ControllerDeliveryQueue queue;
  for (std::uint64_t frame = 1; frame <= ControllerDeliveryQueue::kCapacity;
       ++frame) {
    ControllerDelivery delivery;
    delivery.frame = frame;
    queue.Push(std::move(delivery));
  }
  EXPECT_EQ(queue.size(), ControllerDeliveryQueue::kCapacity);
  EXPECT_FALSE(queue.overflowed());

  // One past capacity: the oldest frame is dropped and the newest arrival
  // carries the resync request.
  ControllerDelivery extra;
  extra.frame = 99;
  queue.Push(std::move(extra));
  EXPECT_EQ(queue.size(), ControllerDeliveryQueue::kCapacity);
  EXPECT_EQ(queue.dropped_frames(), 1u);

  ControllerDelivery out;
  ASSERT_TRUE(queue.Pop(&out));
  EXPECT_EQ(out.frame, 2u); // frame 1 was dropped, not frame 2
  // Drain to the newest: it must demand a resync because edges were lost.
  while (queue.Pop(&out)) {
  }
  EXPECT_TRUE(out.resync);
  EXPECT_EQ(out.frame, 99u);
}

TEST(XrControllerQueue, DeferredOverflowResyncSurvivesDroppedCarrier) {
  ControllerDeliveryQueue queue;
  for (std::uint64_t frame = 0; frame < ControllerDeliveryQueue::kCapacity;
       ++frame) {
    queue.Push(ControllerDelivery{});
  }
  // First overflow: this frame carries resync but is itself dropped next.
  ControllerDelivery carrier;
  carrier.frame = 100;
  queue.Push(std::move(carrier));
  ControllerDelivery newest;
  newest.frame = 101;
  queue.Push(std::move(newest));

  ControllerDelivery out;
  while (queue.Pop(&out)) {
  }
  // The resync must survive on the frame the consumer actually receives.
  EXPECT_TRUE(out.resync);
  EXPECT_EQ(out.frame, 101u);
}

TEST(XrControllerQueue, PopOnEmptyIsFalseAndClearEmpties) {
  ControllerDeliveryQueue queue;
  ControllerDelivery out;
  EXPECT_FALSE(queue.Pop(&out));
  EXPECT_FALSE(queue.Pop(nullptr));
  queue.Push(ControllerDelivery{});
  EXPECT_EQ(queue.size(), 1u);
  queue.Clear();
  EXPECT_EQ(queue.size(), 0u);
}

TEST(XrControllerSdlMapping, OrdinalsMatchRouterExpectations) {
  // SDL3 ordinals the existing router converts through
  // MapSdlGamepadButtonToAndroid/kAndroidAxes; verified pairs only.
  EXPECT_EQ(ControllerButtonToSdlOrdinal(0, Button::kFaceSouth), 0);
  EXPECT_EQ(ControllerButtonToSdlOrdinal(0, Button::kMenu), 6);
  EXPECT_EQ(ControllerButtonToSdlOrdinal(0, Button::kSqueeze), 9);
  EXPECT_EQ(ControllerButtonToSdlOrdinal(1, Button::kSqueeze), 10);
  EXPECT_EQ(ControllerButtonToSdlOrdinal(0, Button::kThumbstick), 7);
  EXPECT_EQ(ControllerButtonToSdlOrdinal(1, Button::kThumbstick), 8);
  EXPECT_EQ(ControllerButtonToSdlOrdinal(0, Button::kTrigger), -1);
  EXPECT_EQ(ControllerAxisToSdlOrdinal(0, Axis::kThumbstickX), 0);
  EXPECT_EQ(ControllerAxisToSdlOrdinal(0, Axis::kThumbstickY), 1);
  EXPECT_EQ(ControllerAxisToSdlOrdinal(1, Axis::kThumbstickX), 2);
  EXPECT_EQ(ControllerAxisToSdlOrdinal(1, Axis::kThumbstickY), 3);
  EXPECT_EQ(ControllerAxisToSdlOrdinal(0, Axis::kTrigger), 4);
  EXPECT_EQ(ControllerAxisToSdlOrdinal(1, Axis::kTrigger), 5);
  EXPECT_EQ(ControllerAxisToSdlOrdinal(0, Axis::kSqueeze), -1);
}

} // namespace
} // namespace mocktail::vr

TEST(XrControllerNative,
     ExactChannelsMergeBothHandsAndNeverTurnTouchIntoClick) {
  mocktail::vr::ControllerSnapshot snapshot;
  snapshot.hands[0].connected = snapshot.hands[1].connected = true;
  snapshot.hands[0]
      .buttons[static_cast<size_t>(mocktail::vr::ControllerButton::kFaceWest)] =
      true;
  snapshot.hands[1]
      .buttons[static_cast<size_t>(mocktail::vr::ControllerButton::kFaceEast)] =
      true;
  snapshot.hands[0]
      .buttons[static_cast<size_t>(mocktail::vr::ControllerButton::kMenu)] =
      true;
  snapshot.hands[1]
      .axes[static_cast<size_t>(mocktail::vr::ControllerAxis::kTrigger)] = .8f;
  snapshot.hands[0]
      .axes[static_cast<size_t>(mocktail::vr::ControllerAxis::kThumbstickY)] =
      1;
  snapshot.hands[0]
      .touches[static_cast<size_t>(mocktail::vr::ControllerTouch::kA)] = true;
  snapshot.hands[0]
      .axes[static_cast<size_t>(mocktail::vr::ControllerAxis::kSqueeze)] = .37f;
  snapshot.hands[0]
      .buttons[static_cast<size_t>(mocktail::vr::ControllerButton::kSqueeze)] =
      true;
  float channels[28]{};
  EXPECT_TRUE(mocktail::vr::EncodeNativeControllerChannels(snapshot, channels));
  EXPECT_FLOAT_EQ(channels[4],
                  .37f);            // preserve analog grip, not its click latch
  EXPECT_FLOAT_EQ(channels[24], 1); // X
  EXPECT_FLOAT_EQ(channels[23], 1); // B
  EXPECT_FLOAT_EQ(channels[27], 1); // Start/Menu
  EXPECT_FLOAT_EQ(channels[22], 0); // capacitive A is not a press
  EXPECT_FLOAT_EQ(channels[12], .8f); // right trigger
  EXPECT_FLOAT_EQ(channels[2], 1); // OpenXR up remains positive in native route
  snapshot.hands[0].connected = false;
  EXPECT_TRUE(mocktail::vr::EncodeNativeControllerChannels(snapshot, channels));
  EXPECT_FLOAT_EQ(channels[24], 0);
  EXPECT_FLOAT_EQ(channels[27], 0);
  EXPECT_FLOAT_EQ(channels[23], 1);
}
