#ifndef MOCKTAIL_RUNTIME_RUNNING_INSTANCE_H_
#define MOCKTAIL_RUNTIME_RUNNING_INSTANCE_H_

#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/environment.h"

namespace mocktail {
namespace runtime {

inline constexpr char kInstanceLockVariable[] = "MOCKTAIL_INSTANCE_LOCK";
inline constexpr char kRestartProgramVariable[] = "MOCKTAIL_RESTART_PROGRAM";

// A Flatpak settings window runs in a sandbox of its own and cannot reach
// the game.
bool CanRestartFromSettings(const Environment& environment);

std::optional<pid_t> FindFlockHolder(std::string_view proc_locks,
                                     dev_t device, ino_t inode);

struct RunningInstance {
  bool running = false;
  // 0 when Nightcap runs but this process cannot see it.
  pid_t pid = 0;
};

RunningInstance FindRunningInstance(const std::filesystem::path& lock_file);

// SIGTERM reaches Nightcap as SDL_EVENT_QUIT, the same as closing its window.
bool StopRunningInstance(const std::filesystem::path& lock_file,
                         std::chrono::milliseconds timeout,
                         std::string* error);

struct LaunchCommand {
  std::vector<std::string> arguments;
  // NAME=value entries.
  std::vector<std::string> environment;
};

// The arguments in /proc/<pid>/cmdline. Nightcap zeroes a join link after
// reading it, since the link holds a one-time login ticket, so the zeroed
// argument is dropped along with a --launch-uri or --launch-request-json
// left without it.
std::vector<std::string> ParseProcArguments(std::string_view cmdline);
std::vector<std::string> ParseProcEnvironment(std::string_view variables);

// Nothing when /proc cannot be read, as for another user's process.
std::optional<LaunchCommand> ReadLaunchCommand(pid_t pid);

// Starts `program` in a session of its own, so it keeps running after this
// process exits. Empty arguments run it with just its path; an empty
// environment passes this process's own.
bool StartDetached(const std::filesystem::path& program, std::string* error,
                   const LaunchCommand& command = {});

}  // namespace runtime
}  // namespace mocktail

#endif
