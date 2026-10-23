#include "runtime/running_instance.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace mocktail {
namespace runtime {
namespace {

std::string ReadProcFile(const std::string& path, bool* read) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream text;
  text << input.rdbuf();
  if (read != nullptr) {
    *read = input.is_open() && !input.bad();
  }
  return text.str();
}

std::string ReadProcLocks() { return ReadProcFile("/proc/locks", nullptr); }

// Runs between fork and exec, so it makes only the system call. A report that
// cannot be written leaves the parent reading a successful start.
void ReportStartError(int pipe, int error_number) {
  while (write(pipe, &error_number, sizeof(error_number)) < 0 &&
         errno == EINTR) {
  }
}

// Every entry of a NUL-separated list, empty ones included.
std::vector<std::string> SplitNulList(std::string_view list) {
  std::vector<std::string> entries;
  std::size_t start = 0;
  while (start < list.size()) {
    const std::size_t end = list.find('\0', start);
    const std::size_t stop = end == std::string_view::npos ? list.size() : end;
    entries.emplace_back(list.substr(start, stop - start));
    start = stop + 1;
  }
  return entries;
}

}  // namespace

std::vector<std::string> ParseProcArguments(std::string_view cmdline) {
  const std::vector<std::string> entries = SplitNulList(cmdline);
  std::vector<std::string> arguments;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const std::string& entry = entries[index];
    if (entry.empty()) {
      continue;
    }
    const bool takes_link =
        entry == "--launch-uri" || entry == "--launch-request-json";
    if (takes_link &&
        (index + 1 >= entries.size() || entries[index + 1].empty())) {
      continue;
    }
    arguments.push_back(entry);
  }
  return arguments;
}

std::vector<std::string> ParseProcEnvironment(std::string_view variables) {
  std::vector<std::string> entries;
  for (std::string& entry : SplitNulList(variables)) {
    if (!entry.empty()) {
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

std::optional<LaunchCommand> ReadLaunchCommand(pid_t pid) {
  if (pid <= 0) {
    return std::nullopt;
  }
  const std::string directory = "/proc/" + std::to_string(pid);
  bool read_arguments = false;
  bool read_environment = false;
  const std::string cmdline =
      ReadProcFile(directory + "/cmdline", &read_arguments);
  const std::string variables =
      ReadProcFile(directory + "/environ", &read_environment);
  std::error_code cwd_error;
  std::filesystem::path working_directory =
      std::filesystem::read_symlink(directory + "/cwd", cwd_error);
  if (!read_arguments || !read_environment || cmdline.empty() || cwd_error) {
    return std::nullopt;
  }
  return LaunchCommand{ParseProcArguments(cmdline),
                       ParseProcEnvironment(variables),
                       std::move(working_directory)};
}

bool CanRestartFromSettings(const Environment& environment) {
  return !environment.HasNonEmpty("FLATPAK_ID");
}

std::optional<pid_t> FindFlockHolder(std::string_view proc_locks,
                                     dev_t device, ino_t inode) {
  std::istringstream lines{std::string(proc_locks)};
  std::string line;
  while (std::getline(lines, line)) {
    // "<n>: FLOCK  ADVISORY  WRITE <pid> <major>:<minor>:<inode> <start> <end>"
    // with the numbers of the device in hex. Waiters have "->" after "<n>:".
    std::istringstream fields(line);
    std::string index, type, advisory, mode, pid_text, file;
    fields >> index >> type >> advisory >> mode >> pid_text >> file;
    if (type != "FLOCK" || mode != "WRITE") {
      continue;
    }
    unsigned int major_number = 0;
    unsigned int minor_number = 0;
    unsigned long long inode_number = 0;
    long pid = -1;
    if (std::sscanf(file.c_str(), "%x:%x:%llu", &major_number, &minor_number,
                    &inode_number) != 3 ||
        std::sscanf(pid_text.c_str(), "%ld", &pid) != 1 || pid < 0) {
      continue;
    }
    if (makedev(major_number, minor_number) == device &&
        inode_number == inode) {
      return static_cast<pid_t>(pid);
    }
  }
  return std::nullopt;
}

RunningInstance FindRunningInstance(const std::filesystem::path& lock_file) {
  struct stat status{};
  if (stat(lock_file.c_str(), &status) != 0) {
    return {};
  }
  const std::optional<pid_t> holder =
      FindFlockHolder(ReadProcLocks(), status.st_dev, status.st_ino);
  if (!holder.has_value()) {
    return {};
  }
  return {true, *holder};
}

namespace {

constexpr char kUnreachableError[] =
    "Nightcap is running where the settings window cannot reach it";

bool StopInstance(const std::filesystem::path& lock_file,
                  const RunningInstance& instance,
                  std::chrono::milliseconds timeout, std::string* error) {
  if (!instance.running) {
    return true;
  }
  if (instance.pid <= 0) {
    *error = kUnreachableError;
    return false;
  }
  if (kill(instance.pid, SIGTERM) != 0 && errno != ESRCH) {
    *error = "cannot close Nightcap: " + std::string(std::strerror(errno));
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (FindRunningInstance(lock_file).running) {
    if (std::chrono::steady_clock::now() >= deadline) {
      *error = "Nightcap did not close within " +
               std::to_string(timeout.count() / 1000) +
               " seconds. If it closes later, start it again";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return true;
}

void EnterFolderIfItExists(const std::string& folder) {
  if (!folder.empty() && chdir(folder.c_str()) != 0) {
    return;
  }
}

struct ExecArguments {
  ExecArguments(const std::filesystem::path& program,
                const LaunchCommand& command)
      : path(program.string()),
        argument_text(command.arguments),
        environment_text(command.environment) {
    if (argument_text.empty()) {
      argument_text.push_back(path);
    }
    for (std::string& argument : argument_text) {
      arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);
    for (std::string& variable : environment_text) {
      environment.push_back(variable.data());
    }
    environment.push_back(nullptr);
    envp = environment_text.empty() ? environ : environment.data();
  }
  ExecArguments(const ExecArguments&) = delete;
  ExecArguments& operator=(const ExecArguments&) = delete;
  std::string path;
  std::vector<std::string> argument_text;
  std::vector<std::string> environment_text;
  std::vector<char*> arguments;
  std::vector<char*> environment;
  char* const* envp = nullptr;
};

}

bool StopRunningInstance(const std::filesystem::path& lock_file,
                         std::chrono::milliseconds timeout,
                         std::string* error) {
  return StopInstance(lock_file, FindRunningInstance(lock_file), timeout,
                      error);
}

bool StartDetached(const std::filesystem::path& program, std::string* error,
                   const LaunchCommand& command) {
  const auto fail = [&](int error_number) {
    *error = "cannot start " + program.string() + ": " +
             std::strerror(error_number);
    return false;
  };
  // Built before fork: the caller may have other threads, so the child must
  // not allocate.
  ExecArguments exec(program, command);
  const std::string working_directory = command.working_directory.string();
  // The grandchild reports a failed exec through this pipe. A successful exec
  // closes it, so the parent reads nothing.
  int report[2];
  if (pipe2(report, O_CLOEXEC) != 0) {
    return fail(errno);
  }
  const pid_t child = fork();
  if (child < 0) {
    const int fork_error = errno;
    close(report[0]);
    close(report[1]);
    return fail(fork_error);
  }
  if (child == 0) {
    close(report[0]);
    setsid();
    const pid_t grandchild = fork();
    if (grandchild == 0) {
      // Ignored signals and the signal mask survive exec. A window started
      // as a shell's background job ignores SIGINT and SIGQUIT.
      struct sigaction default_action {};
      default_action.sa_handler = SIG_DFL;
      for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
        sigaction(signal_number, &default_action, nullptr);
      }
      sigset_t none;
      sigemptyset(&none);
      sigprocmask(SIG_SETMASK, &none, nullptr);
      EnterFolderIfItExists(working_directory);
      execve(exec.path.c_str(), exec.arguments.data(), exec.envp);
      ReportStartError(report[1], errno);
      _exit(127);
    }
    if (grandchild < 0) {
      ReportStartError(report[1], errno);
    }
    _exit(0);
  }
  close(report[1]);
  while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
  }
  int start_error = 0;
  ssize_t length = -1;
  do {
    length = read(report[0], &start_error, sizeof(start_error));
  } while (length < 0 && errno == EINTR);
  close(report[0]);
  return length == static_cast<ssize_t>(sizeof(start_error))
             ? fail(start_error)
             : true;
}

bool RestartRunningInstance(const std::filesystem::path& lock_file,
                            const std::filesystem::path& program,
                            std::chrono::milliseconds timeout,
                            std::string* error) {
  const RunningInstance instance = FindRunningInstance(lock_file);
  if (instance.running && instance.pid <= 0) {
    *error = kUnreachableError;
    return false;
  }
  const std::optional<LaunchCommand> command =
      instance.running ? ReadLaunchCommand(instance.pid) : std::nullopt;
  if (instance.running && !command.has_value()) {
    *error = "cannot read how Nightcap was started, so it was left running";
    return false;
  }
  return StopInstance(lock_file, instance, timeout, error) &&
         StartDetached(program, error, command.value_or(LaunchCommand{}));
}

bool ReplaceProcess(const std::filesystem::path& program,
                    const LaunchCommand& command, std::string* error) {
  ExecArguments exec(program, command);
  std::vector<int> unignored;
  struct sigaction default_action {};
  default_action.sa_handler = SIG_DFL;
  for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
    struct sigaction current {};
    if (sigaction(signal_number, nullptr, &current) == 0 &&
        current.sa_handler == SIG_IGN &&
        sigaction(signal_number, &default_action, nullptr) == 0) {
      unignored.push_back(signal_number);
    }
  }
  sigset_t none;
  sigset_t saved_mask;
  sigemptyset(&none);
  pthread_sigmask(SIG_SETMASK, &none, &saved_mask);
  (void)close_range(3, ~0U, CLOSE_RANGE_CLOEXEC);
  EnterFolderIfItExists(command.working_directory.string());
  execve(exec.path.c_str(), exec.arguments.data(), exec.envp);
  const int exec_error = errno;
  pthread_sigmask(SIG_SETMASK, &saved_mask, nullptr);
  struct sigaction ignore_action {};
  ignore_action.sa_handler = SIG_IGN;
  for (const int signal_number : unignored) {
    sigaction(signal_number, &ignore_action, nullptr);
  }
  *error = "cannot start " + exec.path + ": " + std::strerror(exec_error);
  return false;
}

}  // namespace runtime
}  // namespace mocktail
