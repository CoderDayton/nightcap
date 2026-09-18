#ifndef MOCKTAIL_RUNTIME_GAME_MODE_H_
#define MOCKTAIL_RUNTIME_GAME_MODE_H_

#include <sched.h>

#include <string>
#include <string_view>

#include "mocktail/status.h"

namespace mocktail {
namespace runtime {

enum class GameModePolicy {
  kAuto,
  kOn,
  kOff,
};

bool ParseGameModePolicy(std::string_view value, GameModePolicy* policy);
const char* GameModePolicyName(GameModePolicy policy);

enum class GameModeSessionState {
  kDisabled,
  kUnavailable,
  kRequestFailed,
  kDeclinedCorePinning,
  kAlreadyActive,
  kActive,
  kStopped,
  kStopFailed,
};

const char* GameModeSessionStateName(GameModeSessionState state);

// libgamemode's public header exposes header-only wrappers. The shared object
// itself exports the real_* entry points represented by this table.
struct GameModeClientApi {
  using RequestFn = int (*)();
  using QueryFn = int (*)();
  using ErrorFn = const char* (*)();

  RequestFn request_start = nullptr;
  RequestFn request_end = nullptr;
  QueryFn query_status = nullptr;
  ErrorFn error_string = nullptr;
};

// The process affinity syscalls, injectable so tests exercise the restore
// without moving the calling thread.
struct CpuAffinityApi {
  using GetFn = int (*)(cpu_set_t* mask);
  using SetFn = int (*)(const cpu_set_t* mask);

  GetFn get = nullptr;
  SetFn set = nullptr;
};

CpuAffinityApi HostCpuAffinityApi();

// True when `after` permits a strict subset of the CPUs `before` permits.
// An equal, wider or merely relocated set is not a narrowing.
bool CpuAffinityNarrowed(const cpu_set_t& before, const cpu_set_t& after);

// Owns at most one ref-counted GameMode request. Missing host support and
// daemon rejection remain nonfatal because GameMode is an optimization, not a
// correctness dependency.
//
// GameMode pins the calling process to the host's fastest cores when it sees a
// wide core-speed spread. Roblox spreads its own work over a worker pool, so
// that pin concentrates the load onto a couple of cores instead of helping,
// and the daemon re-applies it every few seconds, so correcting the mask in
// place does not hold. Under `auto`, a request that narrows the affinity is
// released again: ending the request is what releases the pin. `on` is an
// explicit choice and keeps the pinned session.
class GameModeSession final {
 public:
  GameModeSession() = default;
  ~GameModeSession();

  GameModeSession(const GameModeSession&) = delete;
  GameModeSession& operator=(const GameModeSession&) = delete;
  GameModeSession(GameModeSession&& other) noexcept;
  GameModeSession& operator=(GameModeSession&& other) = delete;

  static GameModeSession Start(GameModePolicy policy);
  static GameModeSession StartWithClientForTesting(
      GameModePolicy policy, GameModeClientApi api);
  static GameModeSession StartWithClientForTesting(
      GameModePolicy policy, GameModeClientApi api, CpuAffinityApi affinity);

  GameModeSessionState state() const { return state_; }
  const std::string& detail() const { return detail_; }
  bool active() const {
    return state_ == GameModeSessionState::kActive ||
           state_ == GameModeSessionState::kAlreadyActive;
  }
  bool owns_request() const { return owns_request_; }

  Status Stop();

 private:
  static GameModeSession StartBound(GameModePolicy policy,
                                    GameModeClientApi api,
                                    CpuAffinityApi affinity,
                                    void* library_handle);
  void CloseLibrary();

  GameModeClientApi api_;
  void* library_handle_ = nullptr;
  GameModeSessionState state_ = GameModeSessionState::kDisabled;
  bool owns_request_ = false;
  std::string detail_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_GAME_MODE_H_
