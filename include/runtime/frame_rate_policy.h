#ifndef MOCKTAIL_RUNTIME_FRAME_RATE_POLICY_H_
#define MOCKTAIL_RUNTIME_FRAME_RATE_POLICY_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

inline constexpr int kMaximumSupportedRobloxSchedulerFps = 240;
inline constexpr int kUnlimitedRobloxFramerateCap = 9999;

enum class FrameRateLimitMode {
  kUnmanaged,
  kDisplay,
  kFixed,
  kUnlimited,
  kInvalid,
};

struct FrameRatePolicy {
  FrameRateLimitMode mode = FrameRateLimitMode::kUnmanaged;
  int fixed_fps = 0;

  bool valid() const { return mode != FrameRateLimitMode::kInvalid; }
};

FrameRatePolicy ParseFrameRatePolicy(std::string_view value);

// Enables Roblox's Basic Settings frame-rate control and, when explicitly
// requested, merges a Mocktail scheduler policy into client settings.
// Unmanaged (-1) does not add a scheduler target, so Roblox owns the cap.
// With VSync auto/off, graphics policy separately requests an unthrottled
// Vulkan present mode when the host exposes one.
// Existing unrelated overrides are retained. A conflicting explicit override
// is rejected instead of silently choosing one source of truth.
bool MergeFrameRateClientSettingsOverrides(const FrameRatePolicy &policy,
                                           std::string_view base_json,
                                           std::string *merged_json,
                                           std::string *error);

std::optional<int> RobloxSettingsFramerateCap(const FrameRatePolicy &policy);

bool WriteRobloxFramerateCap(const std::filesystem::path &android_data_root,
                             int fps, std::string *error);

} // namespace runtime
} // namespace mocktail

#endif // MOCKTAIL_RUNTIME_FRAME_RATE_POLICY_H_
