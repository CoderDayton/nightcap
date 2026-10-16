#ifndef MOCKTAIL_RUNTIME_LUAU_CODEGEN_SWITCH_H_
#define MOCKTAIL_RUNTIME_LUAU_CODEGEN_SWITCH_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <vector>

#include "runtime/roblox_experience_presence.h"

namespace mocktail {
namespace runtime {

// Nightcap defaults this on and keeps it on only outside games; a user value
// set in fflags.json or the overrides environment disables the switching.
inline constexpr std::string_view kLuauCodegenAllClientSetting =
    "FFlagDebugLuauCodegenAll";
inline constexpr std::string_view kLuauCodegenAllFastFlagName =
    "DebugLuauCodegenAll";

struct LoadedImageSegment {
  std::uintptr_t address = 0;
  std::size_t size = 0;
  bool writable = false;
};

std::vector<LoadedImageSegment> LoadedImageSegments(std::uintptr_t image_base);

// Record layout: {u8 value; pad; const char* name; size_t length}, in a
// writable segment filled by libroblox's static constructors. Returns null
// unless exactly one record names a NUL-terminated copy of `name`.
std::uint8_t* FindFastFlagValue(
    const std::vector<LoadedImageSegment>& segments, std::string_view name);

bool ClientSettingsSetLuauCodegenAll(std::string_view overrides_json);

bool LuauCodegenAllForPhase(RobloxExperiencePresencePhase phase);

// Locates the flag on the first notification; a byte that is not 0 or 1 is
// left alone.
class LuauCodegenMenuSwitch {
 public:
  using LocateFlagFn = std::function<std::uint8_t*()>;

  LuauCodegenMenuSwitch(LocateFlagFn locate_flag,
                        RobloxExperiencePresenceObserver next);

  RobloxExperiencePresenceObserver observer();
  void OnPhase(RobloxExperiencePresencePhase phase,
               const RobloxExperienceLaunchRequest* request);

 private:
  static void Notify(void* context, RobloxExperiencePresencePhase phase,
                     const RobloxExperienceLaunchRequest* request);

  std::mutex mutex_;
  LocateFlagFn locate_flag_;
  bool located_ = false;
  std::uint8_t* flag_ = nullptr;
  const RobloxExperiencePresenceObserver next_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_LUAU_CODEGEN_SWITCH_H_
