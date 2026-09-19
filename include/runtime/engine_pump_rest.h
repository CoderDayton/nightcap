#ifndef MOCKTAIL_RUNTIME_ENGINE_PUMP_REST_H_
#define MOCKTAIL_RUNTIME_ENGINE_PUMP_REST_H_

#include <cstdint>

namespace mocktail::runtime {

// The engine posts its per-frame main-thread step at a time of its own
// choosing, with no signal the host can block on, so the host main thread
// polls nativeCallMessagesFromMainThread. An empty poll costs a few
// microseconds; polled back to back it burns a core.
//
// Between polls the thread rests for kEnginePumpRestNs. A post is then picked
// up at most kEnginePumpRestNs + timer slack + wake latency late, far under
// the engine's sub-millisecond slack.
//
// How it rests decides the clock. Under a power-saving governor the CPU clocks
// a bursty core down unless some core stays awake, and the engine's render
// thread then runs slower and drops frames. So:
//   kSleep   nanosleep. The core idles. Correct only when the governor holds
//            the clock up on its own (performance, which GameMode sets).
//   kTpause  TPAUSE in C0.2. The core stays awake at reduced power, the clock
//            holds, and the OS books the time as busy. Needs WAITPKG.
//   kSpin    No rest. The core stays awake at full power.
inline constexpr uint64_t kEnginePumpRestNs = 100'000;
// Timer slack the calling thread is set to; the kernel default of 50 us would
// otherwise stretch every sleep.
inline constexpr uint64_t kEnginePumpTimerSlackNs = 10'000;
// Shallow C-state exit plus scheduler latency on the host, for the bound.
inline constexpr uint64_t kShallowWakeNs = 50'000;

enum class EnginePumpRestMode { kSleep, kTpause, kSpin };

const char* EnginePumpRestModeName(EnginePumpRestMode mode);

struct EnginePumpRestInputs {
  bool governor_performance = false;
  bool cpu_has_waitpkg = false;
  // MOCKTAIL_ENGINE_PUMP_REST: "sleep", "tpause" or "spin". Null, empty or
  // unknown defers to the policy; "tpause" without WAITPKG does too.
  const char* override = nullptr;
};

EnginePumpRestMode ChooseEnginePumpRestMode(const EnginePumpRestInputs& inputs);

// True when the file holds "performance". `path` is a cpufreq
// scaling_governor file; cpu0's stands for the machine.
bool CpuGovernorIsPerformance(const char* path);

bool CpuHasWaitPkg();

// The mode chosen from the host on first use.
EnginePumpRestMode ActiveEnginePumpRestMode();

// Rests the calling thread in the given mode and returns the rest length, 0
// for kSpin. The first call lowers the thread's timer slack to
// kEnginePumpTimerSlackNs.
uint64_t RestAfterEnginePump(EnginePumpRestMode mode);
inline uint64_t RestAfterEnginePump() {
  return RestAfterEnginePump(ActiveEnginePumpRestMode());
}

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_ENGINE_PUMP_REST_H_
