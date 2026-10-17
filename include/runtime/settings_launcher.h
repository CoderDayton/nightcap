#ifndef MOCKTAIL_RUNTIME_SETTINGS_LAUNCHER_H_
#define MOCKTAIL_RUNTIME_SETTINGS_LAUNCHER_H_

#include <filesystem>
#include <string>

#include "runtime/environment.h"

namespace mocktail {
namespace runtime {

// The settings window is its own program, so that GTK stays out of the
// process that loads Roblox.

// Path of the settings window program, or empty when there is none to run.
// MOCKTAIL_SETTINGS_HELPER names it outright: it must be an absolute path to
// a regular executable file, and nothing else is tried when it is set.
// Otherwise the program is looked for under MOCKTAIL_PROJECT_ROOT, then
// beside this executable and in its install directories.
std::filesystem::path FindSettingsWindow(const Environment& environment);

// Replaces this process with the settings window. Returns only on failure,
// with `error` set.
bool ExecSettingsWindow(const Environment& environment, std::string* error);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_SETTINGS_LAUNCHER_H_
