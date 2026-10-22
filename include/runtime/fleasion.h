#ifndef MOCKTAIL_RUNTIME_FLEASION_H_
#define MOCKTAIL_RUNTIME_FLEASION_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "runtime/runtime_config.h"
#include "runtime/runtime_paths.h"

namespace mocktail::runtime {

struct FleasionPreparation {
  std::filesystem::path certificate;
  std::filesystem::path base_bundle;
  std::filesystem::path bundle;
  std::string error;
  explicit operator bool() const { return error.empty(); }
};

// Generates a private, replaceable trust bundle outside the managed payload.
// Reads certificates only; never reads Fleasion's private key or changes it.
FleasionPreparation PrepareFleasion(const RuntimeConfig& config,
                                   const Environment& environment,
                                   const RuntimePaths& paths);

bool WebsiteJoinNeedsFreshLaunch(const RuntimeConfig& config);

bool MergeFleasionClientSettingsOverrides(std::string_view base_json,
                                          std::string* merged_json,
                                          std::string* error);

}  // namespace mocktail::runtime

#endif
