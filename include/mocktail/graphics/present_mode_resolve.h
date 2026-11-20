#ifndef MOCKTAIL_GRAPHICS_PRESENT_MODE_RESOLVE_H_
#define MOCKTAIL_GRAPHICS_PRESENT_MODE_RESOLVE_H_

#include <string_view>

namespace mocktail {
namespace graphics {

enum class PresentModePolicy {
  kHostDefault,
  kVsync,
  kUnthrottled,
  kFifo,
  kMailbox,
  kImmediate,
};

inline PresentModePolicy ResolvePresentModePolicy(
    std::string_view vsync_value, std::string_view frame_rate_value,
    std::string_view present_mode = "auto") {
  if (present_mode == "fifo") return PresentModePolicy::kFifo;
  if (present_mode == "mailbox") return PresentModePolicy::kMailbox;
  if (present_mode == "immediate") return PresentModePolicy::kImmediate;
  if (vsync_value == "on" || vsync_value == "1") {
    return PresentModePolicy::kVsync;
  }
  if (vsync_value == "off" || vsync_value == "0") {
    return PresentModePolicy::kUnthrottled;
  }
  if (frame_rate_value == "unlimited") {
    return PresentModePolicy::kUnthrottled;
  }
  return PresentModePolicy::kVsync;
}

}
}

#endif
