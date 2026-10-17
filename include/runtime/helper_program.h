#ifndef MOCKTAIL_RUNTIME_HELPER_PROGRAM_H_
#define MOCKTAIL_RUNTIME_HELPER_PROGRAM_H_

#include <filesystem>
#include <string_view>

#include "runtime/environment.h"

namespace mocktail {
namespace runtime {

// Path of the helper program `name`, or empty when there is none to run.
// `override_variable` names it outright: it must be an absolute path to a
// regular executable file, and nothing else is tried when it is set.
// Otherwise the program is looked for under MOCKTAIL_PROJECT_ROOT/bin, then
// beside this executable and in its install directories.
std::filesystem::path FindHelperProgram(const Environment& environment,
                                        std::string_view override_variable,
                                        std::string_view name);

}  // namespace runtime
}  // namespace mocktail

#endif
