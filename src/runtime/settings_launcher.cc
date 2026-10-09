#include "runtime/settings_launcher.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#ifndef MOCKTAIL_INSTALL_LIBDIR
#define MOCKTAIL_INSTALL_LIBDIR "lib"
#endif

namespace mocktail {
namespace runtime {
namespace {

constexpr char kProgramName[] = "mocktail_settings";

bool IsExecutableRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) &&
         !std::filesystem::is_symlink(path, error) &&
         access(path.c_str(), X_OK) == 0;
}

}  // namespace

std::filesystem::path FindSettingsWindow(const Environment& environment) {
  const std::optional<std::string> named =
      environment.Get("MOCKTAIL_SETTINGS_HELPER");
  if (named.has_value()) {
    const std::filesystem::path helper(*named);
    return helper.is_absolute() && IsExecutableRegularFile(helper)
               ? helper
               : std::filesystem::path{};
  }

  const std::optional<std::string> project_root =
      environment.Get("MOCKTAIL_PROJECT_ROOT");
  if (project_root.has_value()) {
    const std::filesystem::path root(*project_root);
    if (root.is_absolute()) {
      const std::filesystem::path helper = root / "bin" / kProgramName;
      if (IsExecutableRegularFile(helper)) {
        return helper;
      }
    }
  }

  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  if (error || executable.empty()) {
    return {};
  }
  const std::filesystem::path executable_dir = executable.parent_path();
  const std::vector<std::filesystem::path> candidates = {
      executable_dir / kProgramName,
      executable_dir.parent_path() / MOCKTAIL_INSTALL_LIBDIR / "mocktail" /
          kProgramName,
      executable_dir.parent_path() / "libexec" / "mocktail" / kProgramName,
  };
  for (const std::filesystem::path& candidate : candidates) {
    if (IsExecutableRegularFile(candidate)) {
      return candidate;
    }
  }
  return {};
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
