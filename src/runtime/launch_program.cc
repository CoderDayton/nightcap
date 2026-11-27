#include "runtime/launch_program.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <string>
#include <vector>

namespace mocktail {
namespace runtime {

// Forked from a multithreaded process, so it makes only async-signal-safe
// calls. It waits for the pipe to close, which happens when Nightcap exits.
[[noreturn]] static void Watch(int fd, char* const* argv, char* const* envp) {
  const struct sigaction action {};  // SIG_DFL.
  for (int signal_number = 1; signal_number < NSIG; ++signal_number)
    sigaction(signal_number, &action, nullptr);
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, nullptr);
  setsid();  // So Ctrl+C in the terminal does not kill the watcher first.
  dup2(fd, 3);
  // Off the session log's pipe, which a restart waits on, 2 s if held.
  const int null_fd = open("/dev/null", O_RDWR);
  for (int i = 0; i < 3; ++i) dup2(null_fd, i);
  // Inherited descriptors would keep Nightcap's locks held after it exits.
  if (syscall(SYS_close_range, 4U, ~0U, 0U) != 0)  // Before Linux 5.9.
    for (int i = 4; i < 1024; ++i) close(i);
  const pid_t program = fork();
  if (program == 0) {
    close(3);
    setpgid(0, 0);
    execve("/bin/sh", argv, envp);
    _exit(127);
  }
  if (program < 0) _exit(1);
  setpgid(program, program);
  for (char byte; read(3, &byte, 1) < 0 && errno == EINTR;) {
  }
  kill(-program, SIGTERM);
  const timespec tick{0, 100 * 1000 * 1000};
  for (int ticks = 0; ticks < 30 && kill(-program, 0) == 0; ++ticks) {
    waitpid(program, nullptr, WNOHANG);
    nanosleep(&tick, nullptr);
  }
  kill(-program, SIGKILL);
  _exit(0);
}

bool StartLaunchProgram(const Environment& environment) {
  std::string command = environment.GetOr(kLaunchProgramVariable, "");
  if (command.empty()) return false;
  char shell[] = "sh", flag[] = "-c";
  char* const argv[] = {shell, flag, command.data(), nullptr};
  // Without the variable, a program that starts Nightcap cannot loop.
  const std::string prefix = std::string(kLaunchProgramVariable) + '=';
  std::vector<char*> envp;
  for (char** entry = environ; *entry != nullptr; ++entry)
    if (std::string(*entry).compare(0, prefix.size(), prefix) != 0)
      envp.push_back(*entry);
  envp.push_back(nullptr);
  int keepalive[2] = {-1, -1};
  const pid_t watcher = pipe2(keepalive, O_CLOEXEC) == 0 ? fork() : -1;
  if (watcher == 0) {
    close(keepalive[1]);
    Watch(keepalive[0], argv, envp.data());
  }
  close(keepalive[0]);
  if (watcher < 0) close(keepalive[1]);
  (watcher < 0 ? std::cerr : std::cout)
      << "  [launch] " << (watcher < 0 ? "cannot start " : "started ")
      << kLaunchProgramVariable << '\n';
  return watcher > 0;
}

}  // namespace runtime
}  // namespace mocktail
