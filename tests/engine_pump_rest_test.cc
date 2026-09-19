#include "runtime/engine_pump_rest.h"

#include <gtest/gtest.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace mocktail::runtime {
namespace {

uint64_t NowNs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(now.tv_nsec);
}

class GovernorFile final {
 public:
  explicit GovernorFile(const char* content) {
    char pattern[] = "/tmp/mocktail_governor_XXXXXX";
    const int fd = mkstemp(pattern);
    if (fd >= 0) {
      close(fd);
      path_ = pattern;
      std::ofstream(path_) << content;
    }
  }
  ~GovernorFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  const char* path() const { return path_.c_str(); }

 private:
  std::string path_;
};

// A performance governor holds the clock up on its own, so the thread can
// sleep. Without it, only a core that stays awake keeps the clock up: tpause
// where the CPU has it, a spin otherwise.
TEST(EnginePumpRestTest, PolicyFollowsGovernorThenCpuFeature) {
  EXPECT_EQ(ChooseEnginePumpRestMode({true, true, nullptr}),
            EnginePumpRestMode::kSleep);
  EXPECT_EQ(ChooseEnginePumpRestMode({true, false, nullptr}),
            EnginePumpRestMode::kSleep);
  EXPECT_EQ(ChooseEnginePumpRestMode({false, true, nullptr}),
            EnginePumpRestMode::kTpause);
  EXPECT_EQ(ChooseEnginePumpRestMode({false, false, nullptr}),
            EnginePumpRestMode::kSpin);
}

TEST(EnginePumpRestTest, OverrideWinsWhenTheCpuCanHonorIt) {
  EXPECT_EQ(ChooseEnginePumpRestMode({false, true, "sleep"}),
            EnginePumpRestMode::kSleep);
  EXPECT_EQ(ChooseEnginePumpRestMode({true, true, "spin"}),
            EnginePumpRestMode::kSpin);
  EXPECT_EQ(ChooseEnginePumpRestMode({true, true, "tpause"}),
            EnginePumpRestMode::kTpause);
  // tpause needs the instruction; without it the policy decides.
  EXPECT_EQ(ChooseEnginePumpRestMode({false, false, "tpause"}),
            EnginePumpRestMode::kSpin);
  EXPECT_EQ(ChooseEnginePumpRestMode({false, true, "bogus"}),
            EnginePumpRestMode::kTpause);
  EXPECT_EQ(ChooseEnginePumpRestMode({false, true, ""}),
            EnginePumpRestMode::kTpause);
}

TEST(EnginePumpRestTest, ReadsThePerformanceGovernorFromSysfs) {
  GovernorFile performance("performance\n");
  GovernorFile powersave("powersave\n");
  EXPECT_TRUE(CpuGovernorIsPerformance(performance.path()));
  EXPECT_FALSE(CpuGovernorIsPerformance(powersave.path()));
  EXPECT_FALSE(CpuGovernorIsPerformance("/nonexistent/scaling_governor"));
}

TEST(EnginePumpRestTest, ModeNamesAreStable) {
  EXPECT_STREQ(EnginePumpRestModeName(EnginePumpRestMode::kSleep), "sleep");
  EXPECT_STREQ(EnginePumpRestModeName(EnginePumpRestMode::kTpause), "tpause");
  EXPECT_STREQ(EnginePumpRestModeName(EnginePumpRestMode::kSpin), "spin");
}

// The rest bounds how late an engine post is picked up. With the thread's
// timer slack lowered and a shallow C-state exit, the bound sits far under
// the engine's sub-millisecond slack.
TEST(EnginePumpRestTest, BoundStaysUnderTheEngineSlack) {
  EXPECT_LE(kEnginePumpRestNs + kEnginePumpTimerSlackNs + kShallowWakeNs,
            300'000ULL);
}

TEST(EnginePumpRestTest, LowersTheCallingThreadTimerSlackOnce) {
  (void)RestAfterEnginePump(EnginePumpRestMode::kSleep);
  EXPECT_EQ(prctl(PR_GET_TIMERSLACK), static_cast<int>(kEnginePumpTimerSlackNs));
}

// A real sleep, not a spin: the call takes at least the rest, and on an idle
// core not much more. The upper bound is loose so a loaded box does not flake.
TEST(EnginePumpRestTest, SleepModeSleepsForTheRestInterval) {
  const uint64_t start = NowNs();
  const uint64_t reported = RestAfterEnginePump(EnginePumpRestMode::kSleep);
  const uint64_t elapsed = NowNs() - start;
  EXPECT_EQ(reported, kEnginePumpRestNs);
  EXPECT_GE(elapsed, kEnginePumpRestNs);
  EXPECT_LT(elapsed, 2'000'000ULL) << "rest overshot by " << elapsed << " ns";
}

TEST(EnginePumpRestTest, TpauseModeWaitsForTheRestInterval) {
  if (!CpuHasWaitPkg()) {
    GTEST_SKIP() << "CPU has no WAITPKG";
  }
  const uint64_t start = NowNs();
  const uint64_t reported = RestAfterEnginePump(EnginePumpRestMode::kTpause);
  const uint64_t elapsed = NowNs() - start;
  EXPECT_EQ(reported, kEnginePumpRestNs);
  EXPECT_GE(elapsed, kEnginePumpRestNs);
  EXPECT_LT(elapsed, 2'000'000ULL) << "rest overshot by " << elapsed << " ns";
}

TEST(EnginePumpRestTest, SpinModeDoesNotRest) {
  const uint64_t start = NowNs();
  EXPECT_EQ(RestAfterEnginePump(EnginePumpRestMode::kSpin), 0U);
  EXPECT_LT(NowNs() - start, kEnginePumpRestNs);
}

}  // namespace
}  // namespace mocktail::runtime
