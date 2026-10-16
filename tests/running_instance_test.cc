#include "runtime/running_instance.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

using std::chrono::milliseconds;
using namespace std::string_literals;
using Strings = std::vector<std::string>;

constexpr char kLocks[] =
    "1: POSIX  ADVISORY  WRITE 900 fd:01:4242 0 EOF\n"
    "2: FLOCK  ADVISORY  WRITE 1234 00:1f:4242 0 EOF\n"
    "2: -> FLOCK  ADVISORY  WRITE 1300 00:1f:4242 0 EOF\n"
    "3: FLOCK  ADVISORY  WRITE 777 00:1f:99 0 EOF\n";

class FlatpakEnvironment final : public Environment {
 public:
  explicit FlatpakEnvironment(std::optional<std::string> id)
      : id_(std::move(id)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    return name == "FLATPAK_ID" ? id_ : std::nullopt;
  }

 private:
  std::optional<std::string> id_;
};

TEST(CanRestartFromSettingsTest, IsOffInAFlatpak) {
  EXPECT_FALSE(CanRestartFromSettings(
      FlatpakEnvironment("io.github.CoderDayton.nightcap")));
  EXPECT_TRUE(CanRestartFromSettings(FlatpakEnvironment(std::nullopt)));
  EXPECT_TRUE(CanRestartFromSettings(FlatpakEnvironment("")));
}

TEST(ParseProcArgumentsTest, DropsAWipedJoinLink) {
  EXPECT_EQ(ParseProcArguments("nightcap\0--windowed\0\0\0\0\0"s),
            (Strings{"nightcap", "--windowed"}));
}

TEST(ParseProcArgumentsTest, DropsTheFlagOfAWipedJoinLink) {
  EXPECT_EQ(
      ParseProcArguments("nightcap\0--launch-uri\0\0\0\0\0--headless\0"s),
      (Strings{"nightcap", "--headless"}));
  EXPECT_EQ(ParseProcArguments("nightcap\0--launch-request-json\0\0\0\0"s),
            (Strings{"nightcap"}));
}

TEST(ParseProcArgumentsTest, KeepsEveryOtherArgument) {
  EXPECT_EQ(ParseProcArguments("nightcap\0--profile\0dev\0"s),
            (Strings{"nightcap", "--profile", "dev"}));
  EXPECT_EQ(ParseProcArguments(""), Strings{});
}

TEST(ParseProcEnvironmentTest, ListsEachVariable) {
  EXPECT_EQ(ParseProcEnvironment("A=1\0B=two words\0"s),
            (Strings{"A=1", "B=two words"}));
  EXPECT_EQ(ParseProcEnvironment(""), Strings{});
}

TEST(FindFlockHolderTest, NamesTheHolderOfTheFile) {
  EXPECT_EQ(FindFlockHolder(kLocks, makedev(0, 0x1f), 4242), 1234);
}

TEST(FindFlockHolderTest, SkipsWaitersOtherFilesAndPosixLocks) {
  EXPECT_EQ(FindFlockHolder(kLocks, makedev(0, 0x1f), 5), std::nullopt);
  EXPECT_EQ(FindFlockHolder(kLocks, makedev(0xfd, 1), 4242), std::nullopt);
  EXPECT_EQ(FindFlockHolder("", makedev(0, 0x1f), 4242), std::nullopt);
}

TEST(FindFlockHolderTest, ReportsAHolderInAnotherPidNamespaceAsZero) {
  EXPECT_EQ(FindFlockHolder("1: FLOCK  ADVISORY  WRITE 0 00:1f:4242 0 EOF\n",
                            makedev(0, 0x1f), 4242),
            0);
}

class RunningInstanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("running_instance_test." + std::to_string(getpid()));
    std::filesystem::create_directories(directory_);
    lock_ = directory_ / "instance.lock";
    std::ofstream(lock_).close();
  }

  void TearDown() override {
    if (holder_ > 0) {
      kill(holder_, SIGKILL);
      waitpid(holder_, nullptr, 0);
    }
    std::filesystem::remove_all(directory_);
  }

  // Forks a process that holds the lock. With `ignore_term` it stays on
  // SIGTERM; otherwise it exits on it after a short delay.
  void StartHolder(bool ignore_term) {
    int ready[2];
    ASSERT_EQ(pipe(ready), 0);
    holder_ = fork();
    ASSERT_GE(holder_, 0);
    if (holder_ == 0) {
      close(ready[0]);
      if (ignore_term) {
        signal(SIGTERM, SIG_IGN);
      } else {
        signal(SIGTERM, [](int) {
          usleep(200 * 1000);
          _exit(0);
        });
      }
      const int descriptor = open(lock_.c_str(), O_RDWR);
      if (descriptor < 0 || flock(descriptor, LOCK_EX) != 0) {
        _exit(1);
      }
      (void)write(ready[1], "x", 1);
      for (;;) {
        pause();
      }
    }
    close(ready[1]);
    char byte = 0;
    ASSERT_EQ(read(ready[0], &byte, 1), 1);
    close(ready[0]);
  }

  std::filesystem::path directory_;
  std::filesystem::path lock_;
  pid_t holder_ = -1;
};

TEST_F(RunningInstanceTest, NothingRunsWhileTheLockIsFree) {
  EXPECT_FALSE(FindRunningInstance(lock_).running);
  EXPECT_FALSE(FindRunningInstance(directory_ / "missing.lock").running);
  EXPECT_FALSE(FindRunningInstance({}).running);
}

TEST_F(RunningInstanceTest, FindsTheProcessHoldingTheLock) {
  StartHolder(false);
  const RunningInstance instance = FindRunningInstance(lock_);
  EXPECT_TRUE(instance.running);
  EXPECT_EQ(instance.pid, holder_);
}

TEST_F(RunningInstanceTest, StopWaitsUntilTheHolderExits) {
  StartHolder(false);
  std::string error;
  EXPECT_TRUE(StopRunningInstance(lock_, milliseconds(5000), &error)) << error;
  EXPECT_FALSE(FindRunningInstance(lock_).running);
  int status = 0;
  EXPECT_EQ(waitpid(holder_, &status, 0), holder_);
  holder_ = -1;
}

TEST_F(RunningInstanceTest, StopGivesUpOnAHolderThatStays) {
  StartHolder(true);
  std::string error;
  EXPECT_FALSE(StopRunningInstance(lock_, milliseconds(300), &error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(FindRunningInstance(lock_).running);
}

TEST_F(RunningInstanceTest, StopSucceedsWhenNothingRuns) {
  std::string error;
  EXPECT_TRUE(StopRunningInstance(lock_, milliseconds(300), &error)) << error;
}

TEST_F(RunningInstanceTest, StartDetachedRunsTheProgramInItsOwnSession) {
  const std::filesystem::path marker = directory_ / "marker";
  const std::filesystem::path program = directory_ / "program.sh";
  std::ofstream(program) << "#!/bin/sh\nps -o sid= -p $$ > '" << marker.string()
                         << ".tmp' && mv '" << marker.string() << ".tmp' '"
                         << marker.string() << "'\n";
  std::filesystem::permissions(program, std::filesystem::perms::owner_all);

  std::string error;
  ASSERT_TRUE(StartDetached(program, &error)) << error;
  for (int attempt = 0; attempt < 100 && !std::filesystem::exists(marker);
       ++attempt) {
    std::this_thread::sleep_for(milliseconds(20));
  }
  ASSERT_TRUE(std::filesystem::exists(marker));
  std::ifstream input(marker);
  pid_t session = 0;
  input >> session;
  EXPECT_NE(session, getsid(0));
}

TEST_F(RunningInstanceTest, ReadsHowAProcessWasStarted) {
  holder_ = fork();
  ASSERT_GE(holder_, 0);
  if (holder_ == 0) {
    char* const arguments[] = {const_cast<char*>("sleep"),
                               const_cast<char*>("30"), nullptr};
    char* const environment[] = {const_cast<char*>("NIGHTCAP_TEST=1"),
                                 nullptr};
    execve("/bin/sleep", arguments, environment);
    _exit(127);
  }
  std::optional<LaunchCommand> command;
  for (int attempt = 0; attempt < 100; ++attempt) {
    command = ReadLaunchCommand(holder_);
    if (command.has_value() && !command->arguments.empty() &&
        command->arguments.front() == "sleep") {
      break;
    }
    std::this_thread::sleep_for(milliseconds(20));
  }
  ASSERT_TRUE(command.has_value());
  EXPECT_EQ(command->arguments, (Strings{"sleep", "30"}));
  EXPECT_EQ(command->environment, (Strings{"NIGHTCAP_TEST=1"}));
  EXPECT_FALSE(ReadLaunchCommand(-1).has_value());
}

TEST_F(RunningInstanceTest, StartDetachedPassesArgumentsAndEnvironment) {
  const std::filesystem::path marker = directory_ / "marker";
  const std::filesystem::path program = directory_ / "program.sh";
  std::ofstream(program) << "#!/bin/sh\nprintf '%s|%s|%s' \"$0\" \"$1\" \"$X\" > '"
                         << marker.string() << ".tmp' && mv '"
                         << marker.string() << ".tmp' '" << marker.string()
                         << "'\n";
  std::filesystem::permissions(program, std::filesystem::perms::owner_all);

  LaunchCommand command;
  command.arguments = {"renamed", "first"};
  command.environment = {"X=yes", "PATH=/usr/bin:/bin"};
  std::string error;
  ASSERT_TRUE(StartDetached(program, &error, command)) << error;
  for (int attempt = 0; attempt < 100 && !std::filesystem::exists(marker);
       ++attempt) {
    std::this_thread::sleep_for(milliseconds(20));
  }
  ASSERT_TRUE(std::filesystem::exists(marker));
  std::string text;
  std::getline(std::ifstream(marker), text);
  // A script sees its own path as $0, whatever the first argument says.
  EXPECT_EQ(text, program.string() + "|first|yes");
}

TEST_F(RunningInstanceTest, StartDetachedReportsAProgramThatCannotRun) {
  std::string error;
  EXPECT_FALSE(StartDetached(directory_ / "missing", &error));
  EXPECT_NE(error.find("missing"), std::string::npos) << error;
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
