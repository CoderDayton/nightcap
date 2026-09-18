#include "runtime/game_mode.h"

#include <dirent.h>
#include <dlfcn.h>
#if defined(__GLIBC__)
#include <link.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

template <typename Function>
bool ResolveFunction(void* library, const char* name, Function* function,
                     std::string* error) {
  static_assert(sizeof(Function) == sizeof(void*),
                "GameMode function pointers must match dlsym pointers");
  dlerror();
  void* address = dlsym(library, name);
  const char* lookup_error = dlerror();
  if (lookup_error != nullptr || address == nullptr) {
    if (error != nullptr) {
      *error = lookup_error != nullptr ? lookup_error
                                       : std::string("missing symbol ") + name;
    }
    return false;
  }
  std::memcpy(function, &address, sizeof(address));
  return true;
}

std::string ClientError(const GameModeClientApi& api,
                        std::string_view fallback) {
  if (api.error_string != nullptr) {
    const char* error = api.error_string();
    if (error != nullptr && *error != '\0') {
      return error;
    }
  }
  return std::string(fallback);
}

void* OpenClientLibrary(const char* name) {
#if defined(__GLIBC__)
  // Mocktail intentionally exports Bionic compatibility symbols for the
  // Android payload. A normal dlopen() lets host libraries such as libdbus
  // bind to those ABI-compatible-looking exports instead of glibc, which can
  // corrupt the GameMode portal exchange. A new link-map namespace keeps the
  // host client and all of its dependencies on the host ABI.
  return dlmopen(LM_ID_NEWLM, name, RTLD_NOW | RTLD_LOCAL);
#else
  return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

int GetThreadAffinity(cpu_set_t* mask) {
  return sched_getaffinity(0, sizeof(*mask), mask);
}

// sched_setaffinity binds one thread, and GameMode pins every thread it finds,
// so restoring the mask means walking the task list. Threads that exit while
// the directory is being read are skipped, not treated as failures.
int SetAffinityOfEveryThread(const cpu_set_t* mask) {
  DIR* tasks = opendir("/proc/self/task");
  if (tasks == nullptr) {
    return sched_setaffinity(0, sizeof(*mask), mask);
  }
  int result = 0;
  for (;;) {
    // readdir() reports both end-of-directory and failure with a null return,
    // so a read that dies partway must not pass for a completed walk.
    errno = 0;
    const dirent* entry = readdir(tasks);
    if (entry == nullptr) {
      if (errno != 0) {
        result = -1;
      }
      break;
    }
    const long tid = std::strtol(entry->d_name, nullptr, 10);
    if (tid <= 0) {
      continue;
    }
    if (sched_setaffinity(static_cast<pid_t>(tid), sizeof(*mask), mask) != 0 &&
        errno != ESRCH) {
      result = -1;
    }
  }
  closedir(tasks);
  return result;
}

}  // namespace

CpuAffinityApi HostCpuAffinityApi() {
  return CpuAffinityApi{&GetThreadAffinity, &SetAffinityOfEveryThread};
}

bool CpuAffinityNarrowed(const cpu_set_t& before, const cpu_set_t& after) {
  if (CPU_COUNT(&after) >= CPU_COUNT(&before)) {
    return false;
  }
  // A smaller set that escapes `before` is a move, not a narrowing, and the
  // daemon's choice of CPUs is then the only one known to be permitted.
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &after) && !CPU_ISSET(cpu, &before)) {
      return false;
    }
  }
  return true;
}

bool ParseGameModePolicy(std::string_view value, GameModePolicy* policy) {
  if (policy == nullptr) {
    return false;
  }
  if (value == "auto") {
    *policy = GameModePolicy::kAuto;
    return true;
  }
  if (value == "on" || value == "1" || value == "true") {
    *policy = GameModePolicy::kOn;
    return true;
  }
  if (value == "off" || value == "0" || value == "false") {
    *policy = GameModePolicy::kOff;
    return true;
  }
  return false;
}

const char* GameModePolicyName(GameModePolicy policy) {
  switch (policy) {
    case GameModePolicy::kAuto:
      return "auto";
    case GameModePolicy::kOn:
      return "on";
    case GameModePolicy::kOff:
      return "off";
  }
  return "invalid";
}

const char* GameModeSessionStateName(GameModeSessionState state) {
  switch (state) {
    case GameModeSessionState::kDisabled:
      return "disabled";
    case GameModeSessionState::kUnavailable:
      return "unavailable";
    case GameModeSessionState::kRequestFailed:
      return "request-failed";
    case GameModeSessionState::kDeclinedCorePinning:
      return "declined-core-pinning";
    case GameModeSessionState::kAlreadyActive:
      return "already-active";
    case GameModeSessionState::kActive:
      return "active";
    case GameModeSessionState::kStopped:
      return "stopped";
    case GameModeSessionState::kStopFailed:
      return "stop-failed";
  }
  return "invalid";
}

GameModeSession::~GameModeSession() { (void)Stop(); }

GameModeSession::GameModeSession(GameModeSession&& other) noexcept
    : api_(other.api_),
      library_handle_(std::exchange(other.library_handle_, nullptr)),
      state_(other.state_),
      owns_request_(std::exchange(other.owns_request_, false)),
      detail_(std::move(other.detail_)) {
  other.api_ = {};
  other.state_ = GameModeSessionState::kStopped;
}

GameModeSession GameModeSession::Start(GameModePolicy policy) {
  if (policy == GameModePolicy::kOff) {
    return StartBound(policy, {}, {}, nullptr);
  }

  void* library = OpenClientLibrary("libgamemode.so.0");
  if (library == nullptr) {
    library = OpenClientLibrary("libgamemode.so");
  }
  if (library == nullptr) {
    GameModeSession session;
    session.state_ = GameModeSessionState::kUnavailable;
    const char* error = dlerror();
    session.detail_ = error != nullptr ? error : "libgamemode is unavailable";
    return session;
  }

  GameModeClientApi api;
  std::string error;
  if (!ResolveFunction(library, "real_gamemode_request_start",
                       &api.request_start, &error) ||
      !ResolveFunction(library, "real_gamemode_request_end", &api.request_end,
                       &error) ||
      !ResolveFunction(library, "real_gamemode_error_string",
                       &api.error_string, &error)) {
    dlclose(library);
    GameModeSession session;
    session.state_ = GameModeSessionState::kUnavailable;
    session.detail_ = std::move(error);
    return session;
  }
  (void)ResolveFunction(library, "real_gamemode_query_status",
                        &api.query_status, nullptr);

  GameModeSession session =
      StartBound(policy, api, HostCpuAffinityApi(), library);
  if (!session.owns_request_) {
    session.CloseLibrary();
  }
  return session;
}

GameModeSession GameModeSession::StartWithClientForTesting(
    GameModePolicy policy, GameModeClientApi api) {
  return StartBound(policy, api, {}, nullptr);
}

GameModeSession GameModeSession::StartWithClientForTesting(
    GameModePolicy policy, GameModeClientApi api, CpuAffinityApi affinity) {
  return StartBound(policy, api, affinity, nullptr);
}

GameModeSession GameModeSession::StartBound(GameModePolicy policy,
                                            GameModeClientApi api,
                                            CpuAffinityApi affinity,
                                            void* library_handle) {
  GameModeSession session;
  if (policy == GameModePolicy::kOff) {
    return session;
  }
  session.api_ = api;
  session.library_handle_ = library_handle;
  if (api.request_start == nullptr || api.request_end == nullptr) {
    session.state_ = GameModeSessionState::kUnavailable;
    session.detail_ = "libgamemode client entry points are unavailable";
    return session;
  }
  if (api.query_status != nullptr && api.query_status() == 2) {
    session.state_ = GameModeSessionState::kAlreadyActive;
    return session;
  }
  cpu_set_t before;
  CPU_ZERO(&before);
  const bool affinity_readable =
      policy != GameModePolicy::kOn && affinity.get != nullptr &&
      affinity.set != nullptr && affinity.get(&before) == 0;
  if (api.request_start() != 0) {
    session.state_ = GameModeSessionState::kRequestFailed;
    session.detail_ = ClientError(api, "GameMode request was rejected");
    return session;
  }
  session.state_ = GameModeSessionState::kActive;
  session.owns_request_ = true;
  // The daemon pins before the request call returns, and re-pins every few
  // seconds afterwards. Ending the request is what releases the pin.
  cpu_set_t after;
  CPU_ZERO(&after);
  if (affinity_readable && affinity.get(&after) == 0 &&
      CpuAffinityNarrowed(before, after)) {
    session.state_ = GameModeSessionState::kDeclinedCorePinning;
    session.detail_ = "GameMode pinned the process to a subset of its CPUs";
    // A release that fails leaves the daemon holding the request, so keep
    // owning it and let Stop() retry rather than reporting it released.
    if (api.request_end() == 0) {
      session.owns_request_ = false;
    }
    (void)affinity.set(&before);
  }
  return session;
}

Status GameModeSession::Stop() {
  if (!owns_request_) {
    CloseLibrary();
    return Status::Ok();
  }

  owns_request_ = false;
  const int result = api_.request_end != nullptr ? api_.request_end() : -1;
  if (result == 0) {
    state_ = GameModeSessionState::kStopped;
    detail_.clear();
    CloseLibrary();
    return Status::Ok();
  }

  state_ = GameModeSessionState::kStopFailed;
  detail_ = ClientError(api_, "GameMode release request failed");
  const std::string detail = detail_;
  CloseLibrary();
  return Status::Error(StatusCode::kPlatformError, detail);
}

void GameModeSession::CloseLibrary() {
  if (library_handle_ != nullptr) {
    dlclose(library_handle_);
    library_handle_ = nullptr;
  }
}

}  // namespace runtime
}  // namespace mocktail
