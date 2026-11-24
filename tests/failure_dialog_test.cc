#include "runtime/failure_dialog.h"
#include "runtime/failure_reason.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(std::unordered_map<std::string, std::string> values)
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

class FakeDialogHelper final {
 public:
  FakeDialogHelper() {
    char pattern[] = "/tmp/mocktail_failure_dialog_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created == nullptr) {
      return;
    }
    root_ = created;
    helper_ = root_ / "helper.py";
    capture_ = root_ / "capture";
    std::ofstream output(helper_);
    output << "#!/usr/bin/python3\n"
              "import socket\n"
              "import sys\n"
              "capture = '"
           << capture_.string()
           << "'\n"
              "if len(sys.argv) == 3 and sys.argv[1] in "
              "('--message', '--warning'):\n"
              "    with open(capture, 'a', encoding='utf-8') as stream:\n"
              "        stream.write(sys.argv[1] + ':' + sys.argv[2] + "
              "'\\n')\n"
              "    raise SystemExit(0)\n"
              "if len(sys.argv) == 6 and sys.argv[1] == '--question':\n"
              "    raise SystemExit(int(sys.argv[2]))\n"
              "channel = socket.socket(fileno=0)\n"
              "while True:\n"
              "    packet = channel.recv(4096)\n"
              "    if not packet:\n"
              "        break\n"
              "    with open(capture, 'a', encoding='utf-8') as stream:\n"
              "        stream.write(packet.decode('utf-8') + '\\n')\n"
              "    if packet[:1] in (b'S', b'F'):\n"
              "        break\n";
    output.close();
    std::filesystem::permissions(helper_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
  }

  ~FakeDialogHelper() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  const std::filesystem::path& helper() const { return helper_; }
  const std::filesystem::path& capture() const { return capture_; }

 private:
  std::filesystem::path root_;
  std::filesystem::path helper_;
  std::filesystem::path capture_;
};

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(input, line);) {
    lines.push_back(line);
  }
  return lines;
}

MapEnvironment DialogEnvironment(const FakeDialogHelper& helper) {
  return MapEnvironment({
      {"DISPLAY", ":test"},
      {"MOCKTAIL_FAILURE_DIALOG_HELPER", helper.helper().string()},
  });
}

TEST(FailureDialogTest, DisablesDialogsForCanaryAndHeadlessLaunches) {
  FakeDialogHelper helper;
  const MapEnvironment canary({
      {"DISPLAY", ":test"},
      {"MOCKTAIL_FAILURE_DIALOG_HELPER", helper.helper().string()},
      {"MOCKTAIL_ISOLATED_CANARY", "1"},
  });
  const MapEnvironment headless({
      {"DISPLAY", ":test"},
      {"MOCKTAIL_FAILURE_DIALOG_HELPER", helper.helper().string()},
      {"MOCKTAIL_HEADLESS", "1"},
  });
  const MapEnvironment explicitly_disabled({
      {"DISPLAY", ":test"},
      {"MOCKTAIL_FAILURE_DIALOG_HELPER", helper.helper().string()},
      {"MOCKTAIL_DISABLE_FAILURE_DIALOG", "1"},
  });
  const MapEnvironment no_display(
      {{"MOCKTAIL_FAILURE_DIALOG_HELPER", helper.helper().string()}});

  EXPECT_FALSE(FailureDialogsEnabled(canary));
  EXPECT_FALSE(FailureDialogMonitor::Start(canary, "ignored").active());
  EXPECT_FALSE(ShowFailureDialog(canary, "ignored"));
  EXPECT_FALSE(FailureDialogsEnabled(headless));
  EXPECT_FALSE(FailureDialogsEnabled(explicitly_disabled));
  EXPECT_FALSE(FailureDialogsEnabled(no_display));
}

TEST(FailureDialogTest, QuestionTellsANoFromAClosedDialog) {
  FakeDialogHelper helper;
  const MapEnvironment environment = DialogEnvironment(helper);
  const auto ask = [&](std::string_view exit_code) {
    return AskYesNo(environment, exit_code, "body", "No", "Yes");
  };
  EXPECT_EQ(ask("0"), std::optional<bool>(true));
  EXPECT_EQ(ask("2"), std::optional<bool>(false));
  EXPECT_EQ(ask("3"), std::nullopt);
  EXPECT_EQ(ask("1"), std::nullopt);
}

TEST(FailureDialogTest, OneShotPassesAUserFacingMessage) {
  FakeDialogHelper helper;
  const MapEnvironment environment = DialogEnvironment(helper);

  ASSERT_TRUE(ShowFailureDialog(environment, "Already running."));
  ASSERT_TRUE(ShowWarningDialog(environment, "Sign in again."));
  EXPECT_EQ(ReadLines(helper.capture()),
            std::vector<std::string>({"--message:Already running.",
                                      "--warning:Sign in again."}));
}

TEST(FailureDialogTest, MonitorDismissesAfterSuccessfulShutdown) {
  FakeDialogHelper helper;
  const MapEnvironment environment = DialogEnvironment(helper);
  FailureDialogMonitor monitor =
      FailureDialogMonitor::Start(environment, "Startup failed.");
  ASSERT_TRUE(monitor.active());

  monitor.SetMessage("Runtime failed.");
  monitor.MarkSuccessful();

  EXPECT_EQ(
      ReadLines(helper.capture()),
      std::vector<std::string>({"MStartup failed.", "MRuntime failed.", "S"}));
}

TEST(FailureDialogTest, MonitorPassesTheLogFolderForItsButton) {
  FakeDialogHelper helper;
  const MapEnvironment environment = DialogEnvironment(helper);
  {
    FailureDialogMonitor monitor =
        FailureDialogMonitor::Start(environment, "Startup failed.");
    ASSERT_TRUE(monitor.active());
    monitor.SetLogFolder("/state/mocktail/logs/sessions");
  }

  EXPECT_EQ(ReadLines(helper.capture()),
            std::vector<std::string>({"MStartup failed.",
                                      "L/state/mocktail/logs/sessions", "F"}));
}

TEST(FailureDialogTest, MonitorReportsNormalFailureOnScopeExit) {
  FakeDialogHelper helper;
  const MapEnvironment environment = DialogEnvironment(helper);
  {
    FailureDialogMonitor monitor =
        FailureDialogMonitor::Start(environment, "Startup failed.");
    ASSERT_TRUE(monitor.active());
    monitor.SetMessage("Runtime failed.");
  }

  EXPECT_EQ(
      ReadLines(helper.capture()),
      std::vector<std::string>({"MStartup failed.", "MRuntime failed.", "F"}));
}

TEST(FailureDialogTest, MonitorPassesTheSessionLogForTheDetails) {
  FakeDialogHelper helper;
  const MapEnvironment environment = DialogEnvironment(helper);
  {
    FailureDialogMonitor monitor =
        FailureDialogMonitor::Start(environment, "Startup failed.");
    ASSERT_TRUE(monitor.active());
    monitor.SetSessionLog("/state/mocktail/logs/sessions/now.log");
  }

  EXPECT_EQ(ReadLines(helper.capture()),
            std::vector<std::string>(
                {"MStartup failed.", "G/state/mocktail/logs/sessions/now.log",
                 "F"}));
}

TEST(FailureReasonTest, NamesTheCrashAndTheFatalErrorInLogOrder) {
  const std::string log =
      "[stage 6] Invoking Roblox startup entry points\n"
      "[crash] guest installed a SIGSEGV handler; it runs after the report\n"
      "[I/Roblox] Warning noise\n"
      "[crash] SIGSEGV (code 1) at 0x10 in thread \"RenderJob\" tid 42\n"
      "[crash] pc libroblox.so+0x2c18f35\n"
      "[crash] #0 libroblox.so+0x2c18f35\n"
      "[FATAL] Windowed runtime exited without a real presented frame.\r\n"
      "[diagnostic] stage=shutdown\n";

  EXPECT_EQ(FailureReasonFromLog(log),
            "SIGSEGV (code 1) at 0x10 in thread \"RenderJob\" tid 42\n"
            "pc libroblox.so+0x2c18f35\n"
            "Windowed runtime exited without a real presented frame.");
}

TEST(FailureReasonTest, NamesMemoryLimitsCpuLimitsAndAssertions) {
  EXPECT_EQ(FailureReasonFromLog("[memory] limit exceeded: RSS+swap=9000 MiB, "
                                 "limit=8192 MiB; terminating Mocktail with "
                                 "exit 137\n"),
            "Memory limit exceeded: RSS+swap=9000 MiB, limit=8192 MiB; "
            "terminating Mocktail with exit 137");
  EXPECT_EQ(FailureReasonFromLog("\n[crash] SIGXCPU signal=24 code=0\n"),
            "SIGXCPU signal=24 code=0");
  EXPECT_EQ(FailureReasonFromLog("Bionic assertion failed: a.cc:3: x != 0\n"),
            "Bionic assertion failed: a.cc:3: x != 0");
}

TEST(FailureReasonTest, SkipsNoiseAndKeepsTheFirstFewReasons) {
  EXPECT_EQ(FailureReasonFromLog("[crash] reporter unavailable\n"
                                 "[crash] pc libroblox.so+0x1\n"
                                 "  [window] FATAL in prose\n"),
            "");
  EXPECT_EQ(FailureReasonFromLog("[FATAL] same\n[FATAL] same\n[FATAL] b\n"
                                 "[FATAL] c\n[FATAL] d\n[FATAL] e\n"),
            "same\nb\nc\nd");
  const std::string long_reason =
      FailureReasonFromLog("[FATAL] " + std::string(400, 'x') + "\n");
  EXPECT_EQ(long_reason, std::string(300, 'x') + "...");
}

TEST(FailureReasonTest, ReadsOnlyWholeLinesFromTheEndOfTheLog) {
  char pattern[] = "/tmp/mocktail_failure_reason_XXXXXX";
  ASSERT_NE(mkdtemp(pattern), nullptr);
  const std::filesystem::path log = std::filesystem::path(pattern) / "s.log";
  std::ofstream(log) << "[FATAL] early\n" << std::string(64, 'n') << '\n'
                     << "[FATAL] late\n";
  const std::string tail =
      ReadSettledLogTail(log, 40, std::chrono::milliseconds(1),
                         std::chrono::milliseconds(5));
  EXPECT_EQ(tail, "[FATAL] late\n");
  EXPECT_EQ(ReadSettledLogTail(std::filesystem::path(pattern) / "missing"),
            "");
  std::error_code error;
  std::filesystem::remove_all(pattern, error);
}

TEST(FailureDialogTest, SignalHandlerCanSetTheMessageBeforeExiting) {
  FakeDialogHelper helper;
  const MapEnvironment environment = DialogEnvironment(helper);
  struct sigaction action {};
  struct sigaction previous {};
  action.sa_handler = [](int) { SendPreparedFailureMessage(); };
  sigemptyset(&action.sa_mask);
  ASSERT_EQ(sigaction(SIGUSR1, &action, &previous), 0);
  {
    FailureDialogMonitor monitor =
        FailureDialogMonitor::Start(environment, "Startup failed.");
    ASSERT_TRUE(monitor.active());
    PrepareSignalSafeFailureMessage("Roblox took too long to load.");
    ASSERT_EQ(raise(SIGUSR1), 0);
  }
  ASSERT_EQ(raise(SIGUSR1), 0);
  ASSERT_EQ(sigaction(SIGUSR1, &previous, nullptr), 0);
  PrepareSignalSafeFailureMessage({});

  EXPECT_EQ(ReadLines(helper.capture()),
            std::vector<std::string>({"MStartup failed.",
                                      "MRoblox took too long to load.", "F"}));
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
