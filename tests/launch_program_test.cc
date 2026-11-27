#include "runtime/launch_program.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace mocktail {
namespace runtime {
namespace {

using std::chrono::milliseconds;

bool GroupAlive(pid_t group) { return kill(-group, 0) == 0 || errno != ESRCH; }

bool GroupGoneWithin(pid_t group, milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (GroupAlive(group) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(milliseconds(20));
  }
  return !GroupAlive(group);
}

// Stands in for Nightcap: starts the program, then exits normally when
// `control` closes. The program writes its pid, which is its group, to a file.
struct Owner {
  pid_t pid = -1;
  int control = -1;
  pid_t program = -1;
};

Owner StartOwner(const std::string& command, const std::string& lock = "") {
  const std::filesystem::path pid_file =
      std::filesystem::temp_directory_path() /
      ("launch-program-" + std::to_string(getpid()));
  std::filesystem::remove(pid_file);
  int control[2];
  EXPECT_EQ(pipe(control), 0);
  Owner owner;
  owner.pid = fork();
  if (owner.pid == 0) {
    close(control[1]);
    if (!lock.empty()) {
      // No O_CLOEXEC, like a descriptor some library forgot to mark.
      const int fd = open(lock.c_str(), O_RDWR | O_CREAT, 0600);
      if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) _exit(2);
    }
    const std::string script = "echo $$ > " + pid_file.string() + "; " + command;
    setenv(kLaunchProgramVariable, script.c_str(), 1);
    if (!StartLaunchProgram(ProcessEnvironment())) _exit(3);
    char byte;
    while (read(control[0], &byte, 1) < 0 && errno == EINTR) {
    }
    std::exit(0);
  }
  close(control[0]);
  owner.control = control[1];
  for (int tries = 0; tries < 250 && owner.program <= 0; ++tries) {
    std::this_thread::sleep_for(milliseconds(20));
    std::ifstream(pid_file) >> owner.program;
  }
  std::filesystem::remove(pid_file);
  return owner;
}

TEST(LaunchProgramTest, StopsTheProgramAndItsChildrenWhenNightcapExits) {
  Owner owner = StartOwner("sleep 30 & exec sleep 30");
  ASSERT_GT(owner.program, 0);
  EXPECT_TRUE(GroupAlive(owner.program));

  close(owner.control);
  ASSERT_EQ(waitpid(owner.pid, nullptr, 0), owner.pid);
  EXPECT_TRUE(GroupGoneWithin(owner.program, milliseconds(2000)));
}

TEST(LaunchProgramTest, StopsTheProgramWhenNightcapIsKilled) {
  Owner owner = StartOwner("exec sleep 30");
  ASSERT_GT(owner.program, 0);

  kill(owner.pid, SIGKILL);
  ASSERT_EQ(waitpid(owner.pid, nullptr, 0), owner.pid);
  close(owner.control);
  EXPECT_TRUE(GroupGoneWithin(owner.program, milliseconds(2000)));
}

TEST(LaunchProgramTest, KillsAProgramIgnoringSigtermWithoutHoldingNightcapsLocks) {
  const std::string lock = (std::filesystem::temp_directory_path() /
                            ("launch-program-lock-" + std::to_string(getpid())))
                               .string();
  Owner owner = StartOwner("trap '' TERM; exec sleep 30", lock);
  ASSERT_GT(owner.program, 0);

  kill(owner.pid, SIGKILL);
  ASSERT_EQ(waitpid(owner.pid, nullptr, 0), owner.pid);
  close(owner.control);
  std::this_thread::sleep_for(milliseconds(500));
  EXPECT_TRUE(GroupAlive(owner.program));
  const int probe = open(lock.c_str(), O_RDWR | O_CLOEXEC);
  ASSERT_GE(probe, 0);
  EXPECT_EQ(flock(probe, LOCK_EX | LOCK_NB), 0);
  close(probe);
  std::filesystem::remove(lock);

  EXPECT_TRUE(GroupGoneWithin(owner.program, milliseconds(5000)));
}

TEST(LaunchProgramTest, DoesNotPassTheVariableToTheProgram) {
  const std::filesystem::path seen =
      std::filesystem::temp_directory_path() /
      ("launch-program-env-" + std::to_string(getpid()));
  Owner owner = StartOwner("printf %s \"${" + std::string(kLaunchProgramVariable) +
                           "-unset}\" > " + seen.string() + "; exec sleep 30");
  ASSERT_GT(owner.program, 0);
  std::string value;
  for (int tries = 0; tries < 100 && value.empty(); ++tries) {
    std::this_thread::sleep_for(milliseconds(20));
    std::ifstream(seen) >> value;
  }
  std::filesystem::remove(seen);
  EXPECT_EQ(value, "unset");

  close(owner.control);
  ASSERT_EQ(waitpid(owner.pid, nullptr, 0), owner.pid);
  EXPECT_TRUE(GroupGoneWithin(owner.program, milliseconds(2000)));
}

TEST(LaunchProgramTest, DoesNotHoldNightcapsOutputOpen) {
  int output[2];
  ASSERT_EQ(pipe(output), 0);
  const int saved_stdout = dup(STDOUT_FILENO);
  dup2(output[1], STDOUT_FILENO);
  Owner owner = StartOwner("trap '' TERM; exec sleep 30");
  dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
  close(output[1]);
  ASSERT_GT(owner.program, 0);

  close(owner.control);
  ASSERT_EQ(waitpid(owner.pid, nullptr, 0), owner.pid);
  const int flags = fcntl(output[0], F_GETFL);
  fcntl(output[0], F_SETFL, flags | O_NONBLOCK);
  char buffer[256];
  ssize_t got = -1;
  for (int tries = 0; tries < 25 && got != 0; ++tries) {
    while ((got = read(output[0], buffer, sizeof(buffer))) > 0) {
    }
    if (got != 0) std::this_thread::sleep_for(milliseconds(20));
  }
  EXPECT_EQ(got, 0) << "the session log's pipe is still held";
  EXPECT_TRUE(GroupAlive(owner.program));
  close(output[0]);
  EXPECT_TRUE(GroupGoneWithin(owner.program, milliseconds(5000)));
}

TEST(LaunchProgramTest, DoesNothingWithoutTheVariable) {
  unsetenv(kLaunchProgramVariable);
  EXPECT_FALSE(StartLaunchProgram(ProcessEnvironment()));
}

}
}  // namespace runtime
}  // namespace mocktail
