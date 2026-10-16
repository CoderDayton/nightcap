#include "mocktail/vr/headset_detection.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "mocktail/vr/openxr_backend.h"
#include "mocktail/vr/openxr_probe.h"

extern char** environ;

namespace mocktail::vr {

HeadsetDetection DetectSteamVrHeadset(const std::string& self_executable,
                                      std::chrono::milliseconds timeout,
                                      const std::string& proc_root) {
  if (!IsOpenXrSupportCompiled()) {
    return {false, "this build has no OpenXR support"};
  }
  const std::string manifest = RunningSteamVrManifest(proc_root);
  if (manifest.empty()) {
    return {false, "SteamVR is not running"};
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(manifest, error)) {
    return {false, "SteamVR's OpenXR runtime is missing at " + manifest};
  }

  // The child inherits this environment with the runtime and probe mode set.
  const std::string runtime = "XR_RUNTIME_JSON=" + manifest;
  const std::string probe = std::string(kHeadsetProbeEnvironment) + "=1";
  std::vector<char*> environment;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    if (std::strncmp(*entry, "XR_RUNTIME_JSON=", 16) != 0 &&
        std::strncmp(*entry, probe.c_str(), probe.size() - 1) != 0) {
      environment.push_back(*entry);
    }
  }
  environment.push_back(const_cast<char*>(runtime.c_str()));
  environment.push_back(const_cast<char*>(probe.c_str()));
  environment.push_back(nullptr);
  std::vector<char*> arguments = {const_cast<char*>(self_executable.c_str()),
                                  nullptr};

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                   O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                   O_WRONLY, 0);
  pid_t child = -1;
  const int spawned =
      posix_spawn(&child, self_executable.c_str(), &actions, nullptr,
                  arguments.data(), environment.data());
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) {
    return {false, std::string("cannot start the headset check: ") +
                       std::strerror(spawned)};
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (true) {
    const pid_t done = waitpid(child, &status, WNOHANG);
    if (done == child) break;
    if (done < 0) {
      return {false, "the headset check was lost"};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(child, SIGKILL);
      (void)waitpid(child, &status, 0);
      return {false, "SteamVR did not answer the headset check in time"};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!WIFEXITED(status)) {
    return {false, "the headset check crashed"};
  }
  const int code = WEXITSTATUS(status);
  if (code == ProbeExitCode(ProbeStatus::kReady)) {
    return {true, "SteamVR reports a connected headset"};
  }
  if (code == ProbeExitCode(ProbeStatus::kHeadsetUnavailable)) {
    return {false, "SteamVR is running without a connected headset"};
  }
  if (code == ProbeExitCode(ProbeStatus::kRuntimeUnavailable)) {
    return {false, "SteamVR's OpenXR runtime is unavailable"};
  }
  return {false, "SteamVR's OpenXR runtime failed the headset check"};
}

int RunHeadsetProbeChild() { return ProbeExitCode(ProbeOpenXr().status); }

}  // namespace mocktail::vr
