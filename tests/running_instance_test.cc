#include "runtime/running_instance.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/prctl.h>
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

  // Forks a process that holds the lock from inside directory_. With
  // `ignore_term` it stays on SIGTERM; otherwise it exits on it after a short
  // delay. With `hide_proc` its /proc files cannot be read.
  void StartHolder(bool ignore_term, bool hide_proc = false) {
    int ready[2];
    ASSERT_EQ(pipe(ready), 0);
    holder_ = fork();
    ASSERT_GE(holder_, 0);
    if (holder_ == 0) {
      close(ready[0]);
      if (chdir(directory_.c_str()) != 0 ||
          (hide_proc && prctl(PR_SET_DUMPABLE, 0) != 0)) {
        _exit(1);
      }
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
      if (write(ready[1], "x", 1) != 1) {
        _exit(1);
      }
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
    if (chdir(directory_.c_str()) != 0) {
      _exit(126);
    }
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
  EXPECT_EQ(command->working_directory,
            std::filesystem::canonical(directory_));
  EXPECT_FALSE(ReadLaunchCommand(-1).has_value());
}

TEST_F(RunningInstanceTest, StartDetachedPassesArgumentsAndEnvironment) {
  const std::filesystem::path marker = directory_ / "marker";
  const std::filesystem::path program = directory_ / "program.sh";
  std::ofstream(program) << "#!/bin/sh\nprintf '%s|%s|%s|%s' \"$0\" \"$1\" "
                            "\"$X\" \"$(pwd)\" > '"
                         << marker.string() << ".tmp' && mv '"
                         << marker.string() << ".tmp' '" << marker.string()
                         << "'\n";
  std::filesystem::permissions(program, std::filesystem::perms::owner_all);

  LaunchCommand command;
  command.arguments = {"renamed", "first"};
  command.environment = {"X=yes", "PATH=/usr/bin:/bin"};
  command.working_directory = std::filesystem::canonical(directory_);
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
  EXPECT_EQ(text, program.string() + "|first|yes|" +
                      command.working_directory.string());
}

TEST_F(RunningInstanceTest, StartDetachedRunsWhereItCanWhenTheFolderIsGone) {
  const std::filesystem::path marker = directory_ / "marker";
  const std::filesystem::path program = directory_ / "program.sh";
  std::ofstream(program) << "#!/bin/sh\n: > '" << marker.string() << "'\n";
  std::filesystem::permissions(program, std::filesystem::perms::owner_all);
  LaunchCommand command;
  command.working_directory = directory_ / "gone";

  std::string error;
  ASSERT_TRUE(StartDetached(program, &error, command)) << error;
  for (int attempt = 0; attempt < 100 && !std::filesystem::exists(marker);
       ++attempt) {
    std::this_thread::sleep_for(milliseconds(20));
  }
  EXPECT_TRUE(std::filesystem::exists(marker));
}

TEST_F(RunningInstanceTest, RestartLeavesAnInstanceItCannotReadRunning) {
  StartHolder(false, true);
  if (ReadLaunchCommand(holder_).has_value()) {
    GTEST_SKIP() << "this user can read every process, as root can";
  }
  std::string error;
  EXPECT_FALSE(RestartRunningInstance(lock_, directory_ / "missing",
                                      milliseconds(2000), &error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(FindRunningInstance(lock_).running);
}

TEST_F(RunningInstanceTest, RestartStartsTheProgramWhereTheOldOneRan) {
  StartHolder(false);
  const std::filesystem::path marker = directory_ / "marker";
  const std::filesystem::path program = directory_ / "program.sh";
  std::ofstream(program) << "#!/bin/sh\npwd > '" << marker.string()
                         << ".tmp' && mv '" << marker.string() << ".tmp' '"
                         << marker.string() << "'\n";
  std::filesystem::permissions(program, std::filesystem::perms::owner_all);

  std::string error;
  ASSERT_TRUE(RestartRunningInstance(lock_, program, milliseconds(5000),
                                     &error))
      << error;
  EXPECT_EQ(waitpid(holder_, nullptr, 0), holder_);
  holder_ = -1;
  for (int attempt = 0; attempt < 100 && !std::filesystem::exists(marker);
       ++attempt) {
    std::this_thread::sleep_for(milliseconds(20));
  }
  ASSERT_TRUE(std::filesystem::exists(marker));
  std::string text;
  std::getline(std::ifstream(marker), text);
  EXPECT_EQ(text, std::filesystem::canonical(directory_).string());
}

TEST_F(RunningInstanceTest, StartDetachedDoesNotPassOnIgnoredSignals) {
  const std::filesystem::path marker = directory_ / "marker";
  const std::filesystem::path program = directory_ / "program.sh";
  std::ofstream(program) << "#!/bin/sh\ngrep SigIgn /proc/$$/status > '"
                         << marker.string() << ".tmp' && mv '"
                         << marker.string() << ".tmp' '" << marker.string()
                         << "'\n";
  std::filesystem::permissions(program, std::filesystem::perms::owner_all);

  // A shell runs a background job with SIGINT and SIGQUIT ignored.
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  struct sigaction old_int {};
  struct sigaction old_quit {};
  sigaction(SIGINT, &ignore, &old_int);
  sigaction(SIGQUIT, &ignore, &old_quit);
  std::string error;
  const bool started = StartDetached(program, &error);
  sigaction(SIGINT, &old_int, nullptr);
  sigaction(SIGQUIT, &old_quit, nullptr);
  ASSERT_TRUE(started) << error;
  for (int attempt = 0; attempt < 100 && !std::filesystem::exists(marker);
       ++attempt) {
    std::this_thread::sleep_for(milliseconds(20));
  }
  ASSERT_TRUE(std::filesystem::exists(marker));
  std::string line;
  std::getline(std::ifstream(marker), line);
  const unsigned long long ignored =
      std::stoull(line.substr(line.find_first_not_of("SigIgn:\t ")), nullptr,
                  16);
  EXPECT_EQ(ignored & ((1ULL << (SIGINT - 1)) | (1ULL << (SIGQUIT - 1))), 0u)
      << line;
}

TEST_F(RunningInstanceTest, StartDetachedReportsAProgramThatCannotRun) {
  std::string error;
  EXPECT_FALSE(StartDetached(directory_ / "missing", &error));
  EXPECT_NE(error.find("missing"), std::string::npos) << error;
}

TEST_F(RunningInstanceTest, ReplaceProcessRunsTheProgramInThisProcess) {
  const std::filesystem::path marker = directory_ / "marker";
  const std::filesystem::path program = directory_ / "program.sh";
  std::ofstream(program)
      << "#!/bin/sh\n{ echo \"$$ $1 $MARK\"; while read -r line; do "
         "case $line in SigBlk:*|SigIgn:*) echo \"$line\";; esac; "
         "done < /proc/$$/status; } > '"
      << marker.string() << "'\n";
  std::filesystem::permissions(program, std::filesystem::perms::owner_all);

  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ignore, nullptr);
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigprocmask(SIG_BLOCK, &blocked, nullptr);
    std::string error;
    ReplaceProcess(program, {{program.string(), "again"}, {"MARK=yes"}, {}},
                   &error);
    _exit(99);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  std::ifstream input(marker);
  std::string first;
  std::string ignored_line;
  std::string blocked_line;
  std::getline(input, first);
  std::getline(input, blocked_line);
  std::getline(input, ignored_line);
  EXPECT_EQ(first, std::to_string(child) + " again yes");
  const auto mask = [](const std::string& line) {
    return std::stoull(line.substr(line.find_first_not_of("SigBlkIgn:\t ")),
                       nullptr, 16);
  };
  EXPECT_EQ(mask(blocked_line) & (1ULL << (SIGUSR1 - 1)), 0u) << blocked_line;
  EXPECT_EQ(mask(ignored_line) & (1ULL << (SIGINT - 1)), 0u) << ignored_line;
}

TEST_F(RunningInstanceTest, ReplaceProcessClosesInheritedDescriptors) {
  const std::filesystem::path marker = directory_ / "marker";
  const std::filesystem::path program = directory_ / "program.sh";
  std::ofstream(program) << "#!/bin/sh\nif [ -e /proc/$$/fd/50 ]; then echo "
                            "open; else echo closed; fi > '"
                         << marker.string() << "'\n";
  std::filesystem::permissions(program, std::filesystem::perms::owner_all);

  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const int leaked = open("/dev/null", O_RDONLY);
    dup2(leaked, 50);
    std::string error;
    ReplaceProcess(program, {{program.string()}, {"MARK=yes"}, {}}, &error);
    _exit(99);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  std::ifstream input(marker);
  std::string line;
  std::getline(input, line);
  EXPECT_EQ(line, "closed");
}

TEST_F(RunningInstanceTest, ReplaceProcessKeepsSignalsWhenItCannotRun) {
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  struct sigaction old_int {};
  sigaction(SIGINT, &ignore, &old_int);
  std::string error;
  EXPECT_FALSE(ReplaceProcess(directory_ / "missing", {}, &error));
  struct sigaction now {};
  sigaction(SIGINT, nullptr, &now);
  sigaction(SIGINT, &old_int, nullptr);
  EXPECT_EQ(now.sa_handler, SIG_IGN);
  EXPECT_NE(error.find("missing"), std::string::npos) << error;
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
