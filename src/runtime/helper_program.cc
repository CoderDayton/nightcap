#include "runtime/helper_program.h"

#include <unistd.h>

#include <optional>
#include <string>
#include <system_error>

#ifndef MOCKTAIL_INSTALL_LIBDIR
#define MOCKTAIL_INSTALL_LIBDIR "lib"
#endif

namespace mocktail {
namespace runtime {
namespace {

bool IsExecutableRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) &&
         !std::filesystem::is_symlink(path, error) &&
         access(path.c_str(), X_OK) == 0;
}

}  // namespace

std::filesystem::path FindHelperProgram(const Environment& environment,
                                        std::string_view override_variable,
                                        std::string_view name) {
  const std::optional<std::string> named = environment.Get(override_variable);
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
      const std::filesystem::path helper = root / "bin" / name;
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
  const std::filesystem::path install_root = executable_dir.parent_path();
  for (const std::filesystem::path& candidate :
       {executable_dir / name,
        install_root / MOCKTAIL_INSTALL_LIBDIR / "mocktail" / name,
        install_root / "libexec" / "mocktail" / name}) {
    if (IsExecutableRegularFile(candidate)) {
      return candidate;
    }
  }
  return {};
}

}  // namespace runtime
}  // namespace mocktail
