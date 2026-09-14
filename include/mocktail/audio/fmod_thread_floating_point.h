#ifndef MOCKTAIL_AUDIO_FMOD_THREAD_FLOATING_POINT_H_
#define MOCKTAIL_AUDIO_FMOD_THREAD_FLOATING_POINT_H_

#include <string_view>

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace mocktail::audio {

// FMOD's Android threads identify themselves through JavaVMAttachArgs before
// entering their mix loop. Voice DSP filters can accumulate subnormal values
// and spend an entire x86 core on floating-point assists (issue #128).
// Limit FTZ/DAZ to these audio threads; retain the caller's other FP controls.
class FmodThreadFloatingPointMode final {
 public:
  FmodThreadFloatingPointMode() = default;
  ~FmodThreadFloatingPointMode() { Restore(); }
  FmodThreadFloatingPointMode(const FmodThreadFloatingPointMode&) = delete;
  FmodThreadFloatingPointMode& operator=(const FmodThreadFloatingPointMode&) =
      delete;

  bool Enable(const char* thread_name) noexcept {
#if defined(__x86_64__)
    if (active_ || thread_name == nullptr) return false;
    const std::string_view name(thread_name);
    if (name != "FMOD mixer thread" && name != "FMOD feeder thread" &&
        name != "FMOD Convolution thread" && name != "FMOD Worker Thread") {
      return false;
    }
    const unsigned int control = _mm_getcsr();
    original_mode_ = control & kModeMask;
    _mm_setcsr(control | kModeMask);
    active_ = true;
    return true;
#else
    (void)thread_name;
    return false;
#endif
  }

  void Restore() noexcept {
#if defined(__x86_64__)
    if (active_) {
      // Restore only our two bits, preserving rounding, exception masks and
      // status flags set by the guest while the thread was attached.
      _mm_setcsr((_mm_getcsr() & ~kModeMask) | original_mode_);
      active_ = false;
    }
#endif
  }

 private:
#if defined(__x86_64__)
  static constexpr unsigned int kModeMask = (1U << 15) | (1U << 6);
  unsigned int original_mode_ = 0;
  bool active_ = false;
#endif
};

}  // namespace mocktail::audio

#endif  // MOCKTAIL_AUDIO_FMOD_THREAD_FLOATING_POINT_H_
