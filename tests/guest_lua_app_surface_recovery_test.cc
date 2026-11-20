#include "window/guest_lua_app_surface_recovery.h"

#include <gtest/gtest.h>

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

TEST(GuestLuaAppSurfaceRecoveryTest, ResizeWithinTheEngineGenerationIsIgnored) {
  RestartProbe probe;
  GuestLuaAppSurfaceRecovery recovery(&probe, &Restart, 1);

  const auto result = recovery.Changed(Surface(1, 1904, 1034));

  EXPECT_TRUE(result.ok());
  EXPECT_FALSE(result.changed());
  EXPECT_TRUE(probe.restarts.empty());
}

TEST(GuestLuaAppSurfaceRecoveryTest, RecreatedSurfaceRestartsOnChangedOnly) {
  RestartProbe probe;
  GuestLuaAppSurfaceRecovery recovery(&probe, &Restart, 1);

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
  RestartProbe probe;
  GuestLuaAppSurfaceRecovery recovery(&probe, &Restart, 1);

  EXPECT_TRUE(recovery.Changed(Surface(3)).changed());
  EXPECT_EQ(probe.restarts.size(), 1u);
}

TEST(GuestLuaAppSurfaceRecoveryTest, StaleDestroyedDoesNotArmARestart) {
  RestartProbe probe;
  GuestLuaAppSurfaceRecovery recovery(&probe, &Restart, 2);

  EXPECT_FALSE(recovery.Destroyed(1).changed());
  EXPECT_FALSE(recovery.restart_pending());
  EXPECT_FALSE(recovery.Changed(Surface(2)).changed());
  EXPECT_TRUE(probe.restarts.empty());
}

TEST(GuestLuaAppSurfaceRecoveryTest, FailedRestartIsNotFatalAndIsRetried) {
  RestartProbe probe;
  probe.result = Status::Error(StatusCode::kUnavailable, "engine busy");
  GuestLuaAppSurfaceRecovery recovery(&probe, &Restart, 1);

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
  GuestLuaAppSurfaceRecovery recovery(nullptr, nullptr, 1);

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
  RestartProbe probe;
  GuestLuaAppSurfaceRecovery recovery(&probe, &Restart, 1);
  WindowGameSurfaceBridge bridge({&lifecycle, &PollLifecycle},
                                 recovery.Consumer());

  EXPECT_TRUE(bridge.Drain().ok());
  EXPECT_TRUE(probe.restarts.empty());

  for (uint64_t generation = 2; generation <= 4; ++generation) {
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
  RestartProbe probe;
  GuestLuaAppSurfaceRecovery recovery(&probe, &Restart, 1);
  WindowGameSurfaceBridge bridge({&lifecycle, &PollLifecycle},
                                 recovery.Consumer());

  ASSERT_TRUE(lifecycle.Observe(kNativeWindow, 1904, 1034, 1.0f).ok());

  EXPECT_TRUE(bridge.Drain().ok());
  EXPECT_TRUE(probe.restarts.empty());
}

}  // namespace
}  // namespace window
}  // namespace mocktail
