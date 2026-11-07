#ifndef MOCKTAIL_RUNTIME_PROCESS_LAUNCH_POLICY_H_
#define MOCKTAIL_RUNTIME_PROCESS_LAUNCH_POLICY_H_

#include <string>
#include <vector>

namespace mocktail::runtime {

// Unsets inherited MIMALLOC_* variables (any case) so libroblox's embedded
// mimalloc uses its defaults; MOCKTAIL_KEEP_MIMALLOC_ENV=1 keeps them.
// Returns the names cleared. On failure, sets *error and stops.
std::vector<std::string> ClearHostMimallocEnvironment(std::string* error);

// Call only for interactive launches, before creating any threads. Removes an
// inherited soft CPU limit when the hard limit is unlimited, demotes the
// calling thread from FIFO/RR scheduling, and strips inherited host MIMALLOC_*
// environment variables before libroblox's embedded mimalloc initializes.
// Returns diagnostics for the session log, including failures; never changes
// hard limits or signal dispositions.
std::string ApplyInteractiveProcessLaunchPolicy();

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_PROCESS_LAUNCH_POLICY_H_
