#include "runtime/settings_launcher.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "runtime/helper_program.h"

namespace mocktail {
namespace runtime {

std::filesystem::path FindSettingsWindow(const Environment& environment) {
  return FindHelperProgram(environment, "MOCKTAIL_SETTINGS_HELPER",
                           "mocktail_settings");
}

bool ExecSettingsWindow(const Environment& environment, std::string* error) {
  const std::filesystem::path helper = FindSettingsWindow(environment);
  if (helper.empty()) {
    *error = "the settings window program (mocktail_settings) is not "
             "installed";
    return false;
  }
  std::string helper_string = helper.string();
  char* arguments[] = {helper_string.data(), nullptr};
  execv(helper.c_str(), arguments);
  *error = "cannot start the settings window: " +
           std::string(std::strerror(errno));
  return false;
}

}  // namespace runtime
}  // namespace mocktail
