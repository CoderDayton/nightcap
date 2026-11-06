#ifndef MOCKTAIL_RUNTIME_RUNTIME_CONFIG_FILE_H_
#define MOCKTAIL_RUNTIME_RUNTIME_CONFIG_FILE_H_

#include <filesystem>
#include <string>
#include <vector>

#include "runtime/environment.h"
#include "runtime/runtime_config.h"

namespace mocktail {
namespace runtime {

struct IgnoredConfigKey {
  std::string key;
  std::string reason;
};

struct RuntimeConfigLoadResult {
  RuntimeConfig config;
  bool file_loaded = false;
  std::string error;
  // File keys left out because this build does not know the key or value.
  std::vector<IgnoredConfigKey> ignored;

  explicit operator bool() const { return error.empty(); }
};

// Loads the optional YAML file as a defaults layer. Values already present in
// the process environment win over YAML; RuntimeConfig defaults are used last.
// A key or value this build does not know, such as one a nightly build wrote,
// is left out and listed in `ignored` instead of failing the load. A file
// version other than 1 still fails.
RuntimeConfigLoadResult LoadRuntimeConfig(
    const Environment& environment, const std::filesystem::path& config_file);

// Transitional composition helper. The legacy runtime still consumes the
// supported settings through environment variables, so the composition
// root exports one already-resolved RuntimeConfig before entering that
// boundary.
bool ExportRuntimeConfigEnvironment(const RuntimeConfig& config,
                                    std::string* error = nullptr);
bool ExportNetworkProxyEnvironment(const RuntimeConfig& config,
                                   std::string* error = nullptr);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_RUNTIME_CONFIG_FILE_H_
