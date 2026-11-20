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
    uint64_t engine_surface_generation)
    : restart_context_(restart_context),
      restart_(restart),
      engine_generation_(engine_surface_generation) {}

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
  // A new generation is a recreated surface even without a Created event.
  if (surface.generation != engine_generation_) {
    pending_ = true;
  }
  if (!pending_) {
    return Duplicate("guest LuaApp surface resize follows the extent query");
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
