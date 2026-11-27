#ifndef MOCKTAIL_RUNTIME_LAUNCH_PROGRAM_H_
#define MOCKTAIL_RUNTIME_LAUNCH_PROGRAM_H_

#include "runtime/environment.h"

namespace mocktail {
namespace runtime {

inline constexpr char kLaunchProgramVariable[] = "MOCKTAIL_LAUNCH_PROGRAM";
// Runs the command with /bin/sh. When this process exits by any path, the
// program's group gets SIGTERM, then SIGKILL after 3 s. Call early: it forks.
bool StartLaunchProgram(const Environment& environment);

}
}

#endif
