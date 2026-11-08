#ifndef MOCKTAIL_RUNTIME_FLEASION_H_
#define MOCKTAIL_RUNTIME_FLEASION_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "runtime/runtime_config.h"
#include "runtime/runtime_paths.h"

namespace mocktail::runtime {

struct FleasionPreparation {
  std::filesystem::path certificate;
  std::filesystem::path base_bundle;
  std::filesystem::path bundle;
  // Certificates read from `base_bundle`, Fleasion's own included.
  int base_certificates = 0;
  std::string error;
  explicit operator bool() const { return error.empty(); }
};

// Generates a private, replaceable trust bundle outside the managed payload.
// Reads certificates only; never reads Fleasion's private key or changes it.
FleasionPreparation PrepareFleasion(const RuntimeConfig& config,
                                   const Environment& environment,
                                   const RuntimePaths& paths);

// True when something accepts TCP connections on 127.0.0.1:`port` within
// `timeout_ms`. Env mode needs Fleasion's proxy up before Roblox starts.
bool FleasionProxyReachable(int port, int timeout_ms);

// Whether Sober is deployed in the system or user Flatpak installation.
// On Linux, Fleasion's Env Proxy mode refuses to start without it. Unknown
// inside Nightcap's own Flatpak, which cannot see the host's installations.
std::optional<bool> SoberInstalled(
    const Environment& environment, const std::filesystem::path& home,
    const std::filesystem::path& system_flatpak = "/var/lib/flatpak");

bool WebsiteJoinNeedsFreshLaunch(const RuntimeConfig& config);

bool MergeFleasionClientSettingsOverrides(std::string_view base_json,
                                          std::string* merged_json,
                                          std::string* error);

}  // namespace mocktail::runtime

#endif
