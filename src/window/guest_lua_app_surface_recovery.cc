#include "window/guest_lua_app_surface_recovery.h"

#include <utility>

namespace mocktail {
namespace window {
namespace {

runtime::GameSessionUpdateResult Applied(const char* message) {
  return {runtime::GameSessionUpdateStatus::kApplied,
          runtime::GameSessionState::kCreated, Status::Ok(), message};
}

runtime::GameSessionUpdateResult Duplicate(const char* message) {
  return {runtime::GameSessionUpdateStatus::kDuplicate,
          runtime::GameSessionState::kCreated, Status::Ok(), message};
}

runtime::GameSessionUpdateResult DispatchCreated(void* context,
                                                 uint64_t generation) {
  return static_cast<GuestLuaAppSurfaceRecovery*>(context)->Created(generation);
}

runtime::GameSessionUpdateResult DispatchChanged(void* context,
                                                 runtime::GameSurface surface) {
  return static_cast<GuestLuaAppSurfaceRecovery*>(context)->Changed(surface);
}

runtime::GameSessionUpdateResult DispatchDestroyed(void* context,
                                                   uint64_t generation) {
  return static_cast<GuestLuaAppSurfaceRecovery*>(context)->Destroyed(
      generation);
}

}  // namespace

GuestLuaAppSurfaceRecovery::GuestLuaAppSurfaceRecovery(
    void* restart_context, GuestLuaAppRestartFn restart,
    const runtime::GameSurface& engine_surface,
    GuestSurfaceClock::time_point engine_surface_time,
    GuestSurfaceClockFn clock)
    : restart_context_(restart_context),
      restart_(restart),
      engine_generation_(engine_surface.generation),
      width_(engine_surface.width),
      height_(engine_surface.height),
      extent_changed_at_(engine_surface_time),
      clock_(clock ? std::move(clock) : &GuestSurfaceClock::now) {}

runtime::GameSessionUpdateResult GuestLuaAppSurfaceRecovery::Created(
    uint64_t generation) {
  if (generation == engine_generation_) {
    return Duplicate("guest LuaApp surface generation already current");
  }
  pending_ = true;
  return Applied("guest LuaApp surface recreation pending");
}

runtime::GameSessionUpdateResult GuestLuaAppSurfaceRecovery::Destroyed(
    uint64_t generation) {
  if (generation != engine_generation_) {
    return Duplicate("guest LuaApp ignored a stale surface destruction");
  }
  pending_ = true;
  return Applied("guest LuaApp surface destruction retained");
}

runtime::GameSessionUpdateResult GuestLuaAppSurfaceRecovery::Changed(
    runtime::GameSurface surface) {
  const GuestSurfaceClock::time_point now = clock_();
  const bool resized = surface.width != width_ || surface.height != height_;
  if (resized) {
    width_ = surface.width;
    height_ = surface.height;
    extent_changed_at_ = now;
  }
  // A new generation is a recreated surface even without a Created event.
  if (surface.generation != engine_generation_) {
    pending_ = true;
  }
  if (!pending_) {
    return Duplicate("guest LuaApp surface resize follows the extent query");
  }
  if (resized || now - extent_changed_at_ < kGuestSurfaceStableExtent) {
    engine_generation_ = surface.generation;
    pending_ = false;
    return Duplicate("guest LuaApp surface recreated during a resize");
  }
  if (restart_ == nullptr) {
    ++failed_restarts_;
    return Duplicate("guest LuaApp surface restart is unavailable");
  }
  // Not fatal: stay pending and retry on the next surface event.
  const Status status = restart_(restart_context_, surface);
  if (!status.ok()) {
    ++failed_restarts_;
    return Duplicate("guest LuaApp surface restart failed");
  }
  engine_generation_ = surface.generation;
  pending_ = false;
  ++restarts_;
  return Applied("guest LuaApp restarted on recreated surface");
}

WindowGameSurfaceConsumer GuestLuaAppSurfaceRecovery::Consumer() {
  return {this, &DispatchCreated, &DispatchChanged, &DispatchDestroyed};
}

}  // namespace window
}  // namespace mocktail
