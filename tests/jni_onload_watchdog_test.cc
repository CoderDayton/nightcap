#include "legacy/jni_onload_watchdog.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <utility>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "runtime/environment.h"
#include "runtime/failure_dialog.h"

namespace mocktail::legacy {
namespace {

using std::chrono::milliseconds;

void Sentinel(int) {}

class DialogEnvironment final : public runtime::Environment {
 public:
  explicit DialogEnvironment(std::string helper) : helper_(std::move(helper)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    if (name == "DISPLAY") return std::string(":test");
    if (name == "MOCKTAIL_FAILURE_DIALOG_HELPER") return helper_;
    return std::nullopt;
  }

 private:
  std::string helper_;
};

class SentinelAlarmHandler final {
 public:
  SentinelAlarmHandler() {
    struct sigaction action {};
    action.sa_handler = Sentinel;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, &previous_);
  }
  ~SentinelAlarmHandler() { sigaction(SIGALRM, &previous_, nullptr); }

  static bool Installed() {
    struct sigaction current {};
    sigaction(SIGALRM, nullptr, &current);
    return (current.sa_flags & SA_SIGINFO) == 0 &&
           current.sa_handler == Sentinel;
  }

 private:
  struct sigaction previous_ {};
};

[[noreturn]] void HangLikeJniOnLoad() {
  for (;;) {
    pause();
  }
}

TEST(JniOnLoadWatchdogTest, WarnsThenStopsAHungLoad) {
  EXPECT_EXIT(
      {
        const JniOnLoadWatchdog watchdog(400, 100);
        HangLikeJniOnLoad();
      },
      testing::ExitedWithCode(kJniOnLoadTimeoutExitCode),
      "still running after 100 ms.*\\[timeout\\] (RIP|PC)=0x[0-9a-f]+.*"
      "did not finish within 400 ms");
}

TEST(JniOnLoadWatchdogTest, StopsEvenWhenTheLoadBlocksTheSignal) {
  EXPECT_EXIT(
      {
        const JniOnLoadWatchdog watchdog(200, 0, 100);
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGALRM);
        pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
        HangLikeJniOnLoad();
      },
      testing::ExitedWithCode(kJniOnLoadTimeoutExitCode),
      "did not take the stop signal.*did not finish within 200 ms");
}

TEST(JniOnLoadWatchdogTest, TellsTheFailureDialogWhyItStopped) {
  char pattern[] = "/tmp/mocktail_jni_watchdog_XXXXXX";
  ASSERT_NE(mkdtemp(pattern), nullptr);
  const std::filesystem::path root = pattern;
  const std::filesystem::path helper = root / "helper";
  const std::filesystem::path capture = root / "capture";
  {
    std::ofstream script(helper);
    script << "#!/bin/sh\nexec cat > '" << capture.string() << "'\n";
  }
  std::filesystem::permissions(helper, std::filesystem::perms::owner_all);

  EXPECT_EXIT(
      {
        runtime::FailureDialogMonitor monitor =
            runtime::FailureDialogMonitor::Start(
                DialogEnvironment(helper.string()), "Startup failed.");
        if (!monitor.active()) {
          _exit(1);
        }
        const JniOnLoadWatchdog watchdog(200, 0);
        HangLikeJniOnLoad();
      },
      testing::ExitedWithCode(kJniOnLoadTimeoutExitCode), "");

  const std::string expected = "M" + JniOnLoadTimeoutDialogMessage(200);
  std::string captured;
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::ifstream input(capture);
    captured.assign(std::istreambuf_iterator<char>(input), {});
    if (captured.find(expected) != std::string::npos) {
      break;
    }
    std::this_thread::sleep_for(milliseconds(20));
  }
  EXPECT_NE(captured.find(expected), std::string::npos) << captured;
  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(JniOnLoadWatchdogTest, StaysQuietWhenTheLoadFinishesInTime) {
  const SentinelAlarmHandler sentinel;
  testing::internal::CaptureStderr();
  {
    const JniOnLoadWatchdog watchdog(300, 200);
    std::this_thread::sleep_for(milliseconds(20));
  }
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
  EXPECT_TRUE(SentinelAlarmHandler::Installed());
}

TEST(JniOnLoadWatchdogTest, WarnsOnceAndKeepsWaiting) {
  testing::internal::CaptureStderr();
  {
    const JniOnLoadWatchdog watchdog(2000, 50);
    std::this_thread::sleep_for(milliseconds(200));
  }
  EXPECT_EQ(testing::internal::GetCapturedStderr(),
            JniOnLoadSlowWarning(50, 2000));
}

TEST(JniOnLoadWatchdogTest, ZeroTimeoutTurnsItOff) {
  const SentinelAlarmHandler sentinel;
  testing::internal::CaptureStderr();
  {
    const JniOnLoadWatchdog watchdog(0, 50);
    std::this_thread::sleep_for(milliseconds(100));
  }
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
  EXPECT_TRUE(SentinelAlarmHandler::Installed());
}

TEST(JniOnLoadWatchdogTest, MessagesNameTheLimitAndHowToRaiseIt) {
  EXPECT_EQ(JniOnLoadSlowWarning(kJniOnLoadSlowWarningMs, 30000),
            "  [jvm] JNI_OnLoad still running after 3 s (slow disk?); "
            "Nightcap stops it at 30 s\n");
  EXPECT_EQ(JniOnLoadTimeoutLogLine(30000),
            "[FATAL] JNI_OnLoad did not finish within 30 s. On a slow disk, "
            "raise MOCKTAIL_JNI_ONLOAD_TIMEOUT_MS (in ms; 0 turns the limit "
            "off).\n");
  EXPECT_NE(JniOnLoadTimeoutDialogMessage(30000).find(
                "MOCKTAIL_JNI_ONLOAD_TIMEOUT_MS"),
            std::string::npos);
}

}
}
