#include "runtime/engine_pump_rest.h"

#include <cpuid.h>
#include <immintrin.h>
#include <sys/prctl.h>
#include <time.h>
#include <x86intrin.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mocktail::runtime {
namespace {

uint64_t NowNs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(now.tv_nsec);
}

// The kernel caps one TPAUSE at umwait_control/max_time TSC cycles (100000 by
// default), so the wait is a loop of short pauses up to the deadline. Control
// bit 0 clear selects C0.2, the deeper of the two light states.
__attribute__((target("waitpkg"))) void TpauseUntil(uint64_t deadline_ns) {
  while (NowNs() < deadline_ns) {
    _tpause(0, __rdtsc() + 100000ULL);
  }
}

void LowerTimerSlackOnce() {
  // Only the main thread rests, so the slack is set once per process.
  static const bool slack_set = [] {
    return prctl(PR_SET_TIMERSLACK, kEnginePumpTimerSlackNs) == 0;
  }();
  (void)slack_set;
}

}  // namespace

const char* EnginePumpRestModeName(EnginePumpRestMode mode) {
  switch (mode) {
    case EnginePumpRestMode::kSleep:
      return "sleep";
    case EnginePumpRestMode::kTpause:
      return "tpause";
    case EnginePumpRestMode::kSpin:
      return "spin";
  }
  return "spin";
}

EnginePumpRestMode ChooseEnginePumpRestMode(
    const EnginePumpRestInputs& inputs) {
  if (inputs.override != nullptr) {
    if (std::strcmp(inputs.override, "sleep") == 0) {
      return EnginePumpRestMode::kSleep;
    }
    if (std::strcmp(inputs.override, "spin") == 0) {
      return EnginePumpRestMode::kSpin;
    }
    if (std::strcmp(inputs.override, "tpause") == 0 &&
        inputs.cpu_has_waitpkg) {
      return EnginePumpRestMode::kTpause;
    }
  }
  if (inputs.governor_performance) {
    return EnginePumpRestMode::kSleep;
  }
  return inputs.cpu_has_waitpkg ? EnginePumpRestMode::kTpause
                                : EnginePumpRestMode::kSpin;
}

bool CpuGovernorIsPerformance(const char* path) {
  std::FILE* file = std::fopen(path, "r");
  if (file == nullptr) {
    return false;
  }
  char governor[32] = {};
  const bool read = std::fgets(governor, sizeof(governor), file) != nullptr;
  std::fclose(file);
  if (!read) {
    return false;
  }
  governor[std::strcspn(governor, "\r\n")] = '\0';
  return std::strcmp(governor, "performance") == 0;
}

bool CpuHasWaitPkg() {
  unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0) {
    return false;
  }
  return (ecx & (1u << 5)) != 0;
}

EnginePumpRestMode ActiveEnginePumpRestMode() {
  static const EnginePumpRestMode mode = [] {
    EnginePumpRestInputs inputs;
    inputs.governor_performance = CpuGovernorIsPerformance(
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    inputs.cpu_has_waitpkg = CpuHasWaitPkg();
    inputs.override = std::getenv("MOCKTAIL_ENGINE_PUMP_REST");
    return ChooseEnginePumpRestMode(inputs);
  }();
  return mode;
}

uint64_t RestAfterEnginePump(EnginePumpRestMode mode) {
  LowerTimerSlackOnce();
  switch (mode) {
    case EnginePumpRestMode::kSpin:
      return 0;
    case EnginePumpRestMode::kTpause:
      TpauseUntil(NowNs() + kEnginePumpRestNs);
      return kEnginePumpRestNs;
    case EnginePumpRestMode::kSleep:
      break;
  }
  timespec rest{};
  rest.tv_sec = 0;
  rest.tv_nsec = static_cast<long>(kEnginePumpRestNs);
  // A signal may cut the rest short. The remainder is not slept again: an
  // early poll only costs a few microseconds.
  (void)clock_nanosleep(CLOCK_MONOTONIC, 0, &rest, nullptr);
  return kEnginePumpRestNs;
}

}  // namespace mocktail::runtime
