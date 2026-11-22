#ifndef MOCKTAIL_WINDOW_GUEST_LUA_APP_SURFACE_RECOVERY_H_
#define MOCKTAIL_WINDOW_GUEST_LUA_APP_SURFACE_RECOVERY_H_

#include <chrono>
#include <cstdint>
#include <functional>

#include "mocktail/status.h"
#include "runtime/game_session_coordinator.h"
#include "window/window_game_surface_bridge.h"

namespace mocktail {
namespace window {

// Restarts the LuaApp on a recreated surface. It must hand the engine a new
// Surface object: Roblox only rebuilds its framebuffer on a new identity.
using GuestLuaAppRestartFn = Status (*)(void* context,
                                        const runtime::GameSurface& surface);

using GuestSurfaceClock = std::chrono::steady_clock;
using GuestSurfaceClockFn = std::function<GuestSurfaceClock::time_point()>;

inline constexpr std::chrono::milliseconds kGuestSurfaceStableExtent{500};

// Surface recovery for the LuaApp before it has an authenticated session.
// RobloxExperienceComposition does this for authenticated sessions; without
// it the login screen keeps presenting into an obsolete swapchain until the
// window is resized.
class GuestLuaAppSurfaceRecovery final {
 public:
  GuestLuaAppSurfaceRecovery(void* restart_context,
                             GuestLuaAppRestartFn restart,
                             const runtime::GameSurface& engine_surface,
                             GuestSurfaceClock::time_point engine_surface_time,
                             GuestSurfaceClockFn clock = {});

  GuestLuaAppSurfaceRecovery(const GuestLuaAppSurfaceRecovery&) = delete;
  GuestLuaAppSurfaceRecovery& operator=(const GuestLuaAppSurfaceRecovery&) =
      delete;

  runtime::GameSessionUpdateResult Created(uint64_t generation);
  runtime::GameSessionUpdateResult Changed(runtime::GameSurface surface);
  runtime::GameSessionUpdateResult Destroyed(uint64_t generation);

  // The recovery must outlive the returned consumer.
  WindowGameSurfaceConsumer Consumer();

  uint64_t engine_surface_generation() const { return engine_generation_; }
  bool restart_pending() const { return pending_; }
  uint64_t restarts() const { return restarts_; }
  uint64_t failed_restarts() const { return failed_restarts_; }

 private:
  void* restart_context_;
  GuestLuaAppRestartFn restart_;
  uint64_t engine_generation_;
  uint32_t width_;
  uint32_t height_;
  GuestSurfaceClock::time_point extent_changed_at_;
  GuestSurfaceClockFn clock_;
  bool pending_ = false;
  uint64_t restarts_ = 0;
  uint64_t failed_restarts_ = 0;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_GUEST_LUA_APP_SURFACE_RECOVERY_H_
