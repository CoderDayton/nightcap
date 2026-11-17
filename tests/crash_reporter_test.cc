#include "compat/crash_reporter.h"

#include <gtest/gtest.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdlib>

#include "compat/bionic_signal_runtime.h"

namespace mocktail::compat {
namespace {

void NoCoreDump() {
  const rlimit none{0, 0};
  setrlimit(RLIMIT_CORE, &none);
}

__attribute__((noinline)) void WriteThrough(volatile int* pointer) {
  *pointer = 1;
}

void GuestExit(int, siginfo_t*, void*) { _exit(42); }

TEST(CrashReporterDeathTest, ReportsAFaultAndStillDiesFromIt) {
  EXPECT_EXIT(
      {
        NoCoreDump();
        ASSERT_TRUE(InstallCrashReporter());
        WriteThrough(nullptr);
      },
      testing::KilledBySignal(SIGSEGV),
      "\\[crash\\] SIGSEGV .*at 0x0[^\n]*\n"
      "\\[crash\\] pc crash_reporter_test\\+0x[0-9a-f]+");
}

TEST(CrashReporterDeathTest, ReportsAnAbort) {
  EXPECT_EXIT(
      {
        NoCoreDump();
        ASSERT_TRUE(InstallCrashReporter());
        std::abort();
      },
      testing::KilledBySignal(SIGABRT),
      "\\[crash\\] SIGABRT .*\n.*\\[crash\\] #1 ");
}

TEST(CrashReporterDeathTest, GuestHandlerRunsAfterTheReport) {
  EXPECT_EXIT(
      {
        NoCoreDump();
        ASSERT_TRUE(InstallCrashReporter());
        BionicSigaction guest;
        guest.flags = SA_SIGINFO;
        guest.callback.action = GuestExit;
        ASSERT_EQ(BionicSigactionCall(SIGSEGV, &guest, nullptr), 0);
        BionicSigaction seen;
        ASSERT_EQ(BionicSigactionCall(SIGSEGV, nullptr, &seen), 0);
        ASSERT_EQ(seen.callback.action, GuestExit);
        WriteThrough(nullptr);
      },
      testing::ExitedWithCode(42), "\\[crash\\] SIGSEGV ");
}

}
}
