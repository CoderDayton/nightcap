#include "window/guest_lua_app_surface_recovery.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "window/window_game_surface_bridge.h"
#include "window/window_surface_lifecycle.h"

namespace mocktail {
namespace window {
namespace {

constexpr uintptr_t kNativeWindow = 0x1000;

struct RestartProbe {
  std::vector<runtime::GameSurface> restarts;
  Status result = Status::Ok();
};

Status Restart(void* context, const runtime::GameSurface& surface) {
  auto* probe = static_cast<RestartProbe*>(context);
  probe->restarts.push_back(surface);
  return probe->result;
}

runtime::GameSurface Surface(uint64_t generation, uint32_t width = 948,
                             uint32_t height = 1034) {
  return {generation, kNativeWindow, width, height, 1.0f};
}

struct FakeClock {
  GuestSurfaceClock::time_point now{std::chrono::seconds(100)};

  void Advance(std::chrono::milliseconds amount) { now += amount; }
  GuestSurfaceClockFn Fn() {
    return [this] { return now; };
  }
};

struct Fixture {
  explicit Fixture(uint64_t generation = 1, GuestLuaAppRestartFn restart = &Restart)
      : recovery(restart != nullptr ? &probe : nullptr, restart,
                 Surface(generation), clock.now, clock.Fn()) {
    clock.Advance(std::chrono::seconds(1));
  }

  RestartProbe probe;
  FakeClock clock;
  GuestLuaAppSurfaceRecovery recovery;
};

TEST(GuestLuaAppSurfaceRecoveryTest, ResizeWithinTheEngineGenerationIsIgnored) {
  Fixture fixture;
  RestartProbe& probe = fixture.probe;
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;

  const auto result = recovery.Changed(Surface(1, 1904, 1034));

  EXPECT_TRUE(result.ok());
  EXPECT_FALSE(result.changed());
  EXPECT_TRUE(probe.restarts.empty());
}

TEST(GuestLuaAppSurfaceRecoveryTest, RecreatedSurfaceRestartsOnChangedOnly) {
  Fixture fixture;
  RestartProbe& probe = fixture.probe;
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;

  EXPECT_TRUE(recovery.Destroyed(1).ok());
  EXPECT_TRUE(recovery.Created(2).ok());
  EXPECT_TRUE(probe.restarts.empty());
  EXPECT_TRUE(recovery.restart_pending());

  const auto result = recovery.Changed(Surface(2));

  EXPECT_TRUE(result.changed());
  ASSERT_EQ(probe.restarts.size(), 1u);
  EXPECT_EQ(probe.restarts[0].generation, 2u);
  EXPECT_EQ(probe.restarts[0].width, 948u);
  EXPECT_EQ(recovery.engine_surface_generation(), 2u);
  EXPECT_FALSE(recovery.restart_pending());
}

TEST(GuestLuaAppSurfaceRecoveryTest, NewGenerationWithoutCreatedStillRestarts) {
  Fixture fixture;
  RestartProbe& probe = fixture.probe;
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;

  EXPECT_TRUE(recovery.Changed(Surface(3)).changed());
  EXPECT_EQ(probe.restarts.size(), 1u);
}

TEST(GuestLuaAppSurfaceRecoveryTest,
     NewGenerationAtADifferentExtentIsAdoptedWithoutARestart) {
  Fixture fixture;
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;

  recovery.Destroyed(1);
  recovery.Created(2);
  const auto result = recovery.Changed(Surface(2, 1471, 828));

  EXPECT_TRUE(result.ok());
  EXPECT_FALSE(result.changed());
  EXPECT_TRUE(fixture.probe.restarts.empty());
  EXPECT_EQ(recovery.engine_surface_generation(), 2u);
  EXPECT_FALSE(recovery.restart_pending());

  fixture.clock.Advance(std::chrono::seconds(5));
  EXPECT_FALSE(recovery.Changed(Surface(2, 1335, 752)).changed());
  EXPECT_TRUE(fixture.probe.restarts.empty());
}

TEST(GuestLuaAppSurfaceRecoveryTest,
     NewGenerationSoonAfterASizeChangeIsAdoptedWithoutARestart) {
  Fixture fixture;
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;

  EXPECT_FALSE(recovery.Changed(Surface(1, 1471, 828)).changed());
  fixture.clock.Advance(std::chrono::milliseconds(499));
  const auto result = recovery.Changed(Surface(2, 1471, 828));

  EXPECT_TRUE(result.ok());
  EXPECT_FALSE(result.changed());
  EXPECT_TRUE(fixture.probe.restarts.empty());
  EXPECT_EQ(recovery.engine_surface_generation(), 2u);
  EXPECT_FALSE(recovery.restart_pending());
}

TEST(GuestLuaAppSurfaceRecoveryTest,
     NewGenerationAtAStableExtentRestartsOnce) {
  Fixture fixture;
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;

  EXPECT_FALSE(recovery.Changed(Surface(1, 1471, 828)).changed());
  fixture.clock.Advance(std::chrono::milliseconds(500));

  EXPECT_TRUE(recovery.Changed(Surface(2, 1471, 828)).changed());
  EXPECT_FALSE(recovery.Changed(Surface(2, 1471, 828)).changed());
  ASSERT_EQ(fixture.probe.restarts.size(), 1u);
  EXPECT_EQ(fixture.probe.restarts[0].width, 1471u);
  EXPECT_EQ(recovery.restarts(), 1u);
}

TEST(GuestLuaAppSurfaceRecoveryTest, StaleDestroyedDoesNotArmARestart) {
  Fixture fixture(2);
  RestartProbe& probe = fixture.probe;
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;

  EXPECT_FALSE(recovery.Destroyed(1).changed());
  EXPECT_FALSE(recovery.restart_pending());
  EXPECT_FALSE(recovery.Changed(Surface(2)).changed());
  EXPECT_TRUE(probe.restarts.empty());
}

TEST(GuestLuaAppSurfaceRecoveryTest, FailedRestartIsNotFatalAndIsRetried) {
  Fixture fixture;
  RestartProbe& probe = fixture.probe;
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;
  probe.result = Status::Error(StatusCode::kUnavailable, "engine busy");

  recovery.Created(2);
  const auto failed = recovery.Changed(Surface(2));

  // A rejected update would make the bridge terminal and exit the process.
  EXPECT_TRUE(failed.ok());
  EXPECT_EQ(recovery.failed_restarts(), 1u);
  EXPECT_TRUE(recovery.restart_pending());
  EXPECT_EQ(recovery.engine_surface_generation(), 1u);

  probe.result = Status::Ok();
  EXPECT_TRUE(recovery.Changed(Surface(2)).changed());
  EXPECT_EQ(recovery.restarts(), 1u);
  EXPECT_FALSE(recovery.restart_pending());
}

TEST(GuestLuaAppSurfaceRecoveryTest, MissingRestartHookIsNotFatal) {
  Fixture fixture(1, nullptr);
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;

  recovery.Created(2);

  EXPECT_TRUE(recovery.Changed(Surface(2)).ok());
  EXPECT_EQ(recovery.failed_restarts(), 1u);
}

bool PollLifecycle(void* context, WindowSurfaceEvent* event) {
  return static_cast<WindowSurfaceLifecycle*>(context)->Poll(event);
}

// The host reports the swapchain out of date although the extent never
// changes, and every recovery recreates the surface at the same size. Each
// recreation must reach the engine exactly once.
TEST(GuestLuaAppSurfaceRecoveryTest,
     RepeatedSameSizeRecoveriesEachRestartTheEngineOnce) {
  WindowSurfaceLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.Activate(kNativeWindow, 948, 1034, 1.0f).ok());
  Fixture fixture;
  RestartProbe& probe = fixture.probe;
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;
  WindowGameSurfaceBridge bridge({&lifecycle, &PollLifecycle},
                                 recovery.Consumer());

  EXPECT_TRUE(bridge.Drain().ok());
  EXPECT_TRUE(probe.restarts.empty());

  for (uint64_t generation = 2; generation <= 4; ++generation) {
    fixture.clock.Advance(std::chrono::milliseconds(16));
    ASSERT_TRUE(lifecycle.Recreate().ok());
    const WindowGameSurfaceDrainResult drained = bridge.Drain();
    ASSERT_TRUE(drained.ok());
    EXPECT_EQ(drained.drained_events, 3u);  // Destroyed, Created, Changed.
    ASSERT_EQ(probe.restarts.size(), generation - 1);
    EXPECT_EQ(probe.restarts.back().generation, generation);
    EXPECT_EQ(probe.restarts.back().width, 948u);
    EXPECT_EQ(probe.restarts.back().height, 1034u);
  }
}

TEST(GuestLuaAppSurfaceRecoveryTest,
     PlainResizeThroughTheBridgeDoesNotRestart) {
  WindowSurfaceLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.Activate(kNativeWindow, 948, 1034, 1.0f).ok());
  Fixture fixture;
  RestartProbe& probe = fixture.probe;
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;
  WindowGameSurfaceBridge bridge({&lifecycle, &PollLifecycle},
                                 recovery.Consumer());

  ASSERT_TRUE(lifecycle.Observe(kNativeWindow, 1904, 1034, 1.0f).ok());

  EXPECT_TRUE(bridge.Drain().ok());
  EXPECT_TRUE(probe.restarts.empty());
}

TEST(GuestLuaAppSurfaceRecoveryTest,
     RecreationInTheMiddleOfADragResizeDoesNotRestart) {
  WindowSurfaceLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.Activate(kNativeWindow, 948, 1034, 1.0f).ok());
  Fixture fixture;
  GuestLuaAppSurfaceRecovery& recovery = fixture.recovery;
  WindowGameSurfaceBridge bridge({&lifecycle, &PollLifecycle},
                                 recovery.Consumer());
  ASSERT_TRUE(bridge.Drain().ok());

  const auto resize = [&](uint32_t width, uint32_t height) {
    fixture.clock.Advance(std::chrono::milliseconds(16));
    ASSERT_TRUE(lifecycle.Observe(kNativeWindow, width, height, 1.0f).ok());
    ASSERT_TRUE(bridge.Drain().ok());
  };
  resize(1200, 900);
  resize(1471, 828);
  fixture.clock.Advance(std::chrono::milliseconds(16));
  ASSERT_TRUE(lifecycle.Recreate().ok());
  ASSERT_TRUE(bridge.Drain().ok());
  resize(1400, 790);
  fixture.clock.Advance(std::chrono::milliseconds(16));
  ASSERT_TRUE(lifecycle.Recreate().ok());
  ASSERT_TRUE(bridge.Drain().ok());
  resize(1335, 752);

  EXPECT_TRUE(fixture.probe.restarts.empty());
  EXPECT_EQ(recovery.restarts(), 0u);
  EXPECT_FALSE(recovery.restart_pending());
  EXPECT_EQ(recovery.engine_surface_generation(),
            lifecycle.Snapshot().generation);
}

}  // namespace
}  // namespace window
}  // namespace mocktail
