#ifndef MOCKTAIL_UPDATE_BACKGROUND_UPDATE_PROCESS_H_
#define MOCKTAIL_UPDATE_BACKGROUND_UPDATE_PROCESS_H_

#include <sys/types.h>

namespace mocktail::update {

// For the download-only updater that runs while Roblox plays: drops to the
// lowest CPU priority (and so the lowest disk priority), and when `parent` is
// set, ends with SIGTERM once that process exits. False when `parent` is
// already gone.
bool EnterBackgroundUpdateMode(pid_t parent);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_BACKGROUND_UPDATE_PROCESS_H_
