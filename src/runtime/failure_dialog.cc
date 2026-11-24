#include "runtime/failure_dialog.h"

#include "runtime/helper_environment.h"

#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/helper_program.h"

extern char** environ;

namespace mocktail {
namespace runtime {
namespace {

constexpr char kMessagePacket = 'M';
constexpr char kFailurePacket = 'F';
constexpr char kSuccessPacket = 'S';
constexpr char kLogFolderPacket = 'L';
constexpr char kSessionLogPacket = 'G';
constexpr std::size_t kMaximumMessageBytes = 2048;

std::atomic<int> g_monitor_socket{-1};
char g_prepared_packet[kMaximumMessageBytes + 1];
std::atomic<std::size_t> g_prepared_size{0};

void RetireMonitorSocket(int socket) {
  int expected = socket;
  g_monitor_socket.compare_exchange_strong(expected, -1);
}

bool Enabled(const Environment& environment, std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  return value.has_value() && !value->empty() && *value != "0";
}

bool HasNonEmpty(const Environment& environment, std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  return value.has_value() && !value->empty();
}

std::filesystem::path DialogHelper(const Environment& environment) {
  return FindHelperProgram(environment, "MOCKTAIL_FAILURE_DIALOG_HELPER",
                           "mocktail_failure_dialog");
}

void WaitForHelper(int helper_pid) {
  if (helper_pid <= 0) {
    return;
  }
  while (waitpid(helper_pid, nullptr, 0) < 0 && errno == EINTR) {
  }
}

std::optional<int> SpawnAndWait(const std::filesystem::path& helper,
                                std::vector<std::string> arguments) {
  std::string helper_string = helper.string();
  std::vector<char*> argv = {helper_string.data()};
  for (std::string& argument : arguments) {
    if (argument.size() > kMaximumMessageBytes) {
      argument.resize(kMaximumMessageBytes);
    }
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);
  pid_t child = -1;
  const auto child_environment = BuildHelperEnvironment(environ);
  if (posix_spawn(&child, helper.c_str(), nullptr, nullptr, argv.data(),
                  child_environment.data()) != 0) {
    return std::nullopt;
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return std::nullopt;
  }
  if (!WIFEXITED(status)) return std::nullopt;
  return WEXITSTATUS(status);
}

bool SpawnOneShot(const std::filesystem::path& helper,
                  std::string_view option,
                  std::string_view message) {
  return SpawnAndWait(helper, {std::string(option), std::string(message)})
      .has_value();
}

std::pair<int, int> SpawnMonitor(const std::filesystem::path& helper) {
  int sockets[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
    return {-1, -1};
  }

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(sockets[0]);
    close(sockets[1]);
    return {-1, -1};
  }
  int action_status = 0;
  if (sockets[0] == STDIN_FILENO) {
    action_status = posix_spawn_file_actions_addclose(&actions, sockets[0]);
  }
  if (action_status == 0 && sockets[1] != STDIN_FILENO) {
    action_status =
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
  }
  if (action_status == 0 && sockets[0] != STDIN_FILENO) {
    action_status = posix_spawn_file_actions_addclose(&actions, sockets[0]);
  }
  if (action_status == 0 && sockets[1] != STDIN_FILENO) {
    action_status = posix_spawn_file_actions_addclose(&actions, sockets[1]);
  }

  std::string helper_string = helper.string();
  char monitor_option[] = "--monitor";
  char* arguments[] = {helper_string.data(), monitor_option, nullptr};
  pid_t child = -1;
  int spawn_status = action_status;
  if (spawn_status == 0) {
    const auto child_environment = BuildHelperEnvironment(environ);
    spawn_status = posix_spawn(&child, helper.c_str(), &actions, nullptr,
                               arguments, child_environment.data());
  }
  posix_spawn_file_actions_destroy(&actions);
  close(sockets[1]);
  if (spawn_status != 0) {
    close(sockets[0]);
    return {-1, -1};
  }
  return {sockets[0], static_cast<int>(child)};
}

bool SendPacket(int socket, char kind, std::string_view payload = {}) {
  std::string packet(1, kind);
  packet.append(payload.substr(0, kMaximumMessageBytes));
  const ssize_t sent = send(socket, packet.data(), packet.size(), MSG_NOSIGNAL);
  return sent == static_cast<ssize_t>(packet.size());
}

}  // namespace

bool FailureDialogsEnabled(const Environment& environment) {
  if (Enabled(environment, "MOCKTAIL_DISABLE_FAILURE_DIALOG") ||
      Enabled(environment, "MOCKTAIL_ISOLATED_CANARY") ||
      Enabled(environment, "MOCKTAIL_HEADLESS")) {
    return false;
  }
  return HasNonEmpty(environment, "DISPLAY") ||
         HasNonEmpty(environment, "WAYLAND_DISPLAY");
}

bool ShowFailureDialog(const Environment& environment,
                       std::string_view message) {
  if (!FailureDialogsEnabled(environment)) {
    return false;
  }
  const std::filesystem::path helper = DialogHelper(environment);
  return !helper.empty() && SpawnOneShot(helper, "--message", message);
}

bool ShowWarningDialog(const Environment& environment,
                       std::string_view message) {
  if (!FailureDialogsEnabled(environment)) {
    return false;
  }
  const std::filesystem::path helper = DialogHelper(environment);
  return !helper.empty() && SpawnOneShot(helper, "--warning", message);
}

void PrepareSignalSafeFailureMessage(std::string_view message) {
  g_prepared_size.store(0, std::memory_order_release);
  if (message.empty()) {
    return;
  }
  const std::size_t size = std::min(message.size(), kMaximumMessageBytes);
  g_prepared_packet[0] = kMessagePacket;
  std::memcpy(g_prepared_packet + 1, message.data(), size);
  g_prepared_size.store(size + 1, std::memory_order_release);
}

void SendPreparedFailureMessage() {
  const std::size_t size = g_prepared_size.load(std::memory_order_acquire);
  const int socket = g_monitor_socket.load(std::memory_order_acquire);
  if (size == 0 || socket < 0) {
    return;
  }
  (void)send(socket, g_prepared_packet, size, MSG_NOSIGNAL);
}

std::optional<bool> AskYesNo(const Environment& environment,
                             std::string_view heading, std::string_view body,
                             std::string_view decline,
                             std::string_view accept) {
  if (!FailureDialogsEnabled(environment)) {
    return std::nullopt;
  }
  const std::filesystem::path helper = DialogHelper(environment);
  if (helper.empty()) {
    return std::nullopt;
  }
  const std::optional<int> status = SpawnAndWait(
      helper, {"--question", std::string(heading), std::string(body),
               std::string(decline), std::string(accept)});
  if (status == 0) return true;
  if (status == 2) return false;
  return std::nullopt;
}

FailureDialogMonitor::FailureDialogMonitor(int socket, int helper_pid)
    : socket_(socket), helper_pid_(helper_pid) {}

FailureDialogMonitor::~FailureDialogMonitor() { Finish(kFailurePacket); }

FailureDialogMonitor::FailureDialogMonitor(
    FailureDialogMonitor&& other) noexcept
    : socket_(std::exchange(other.socket_, -1)),
      helper_pid_(std::exchange(other.helper_pid_, -1)) {}

FailureDialogMonitor& FailureDialogMonitor::operator=(
    FailureDialogMonitor&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Finish(kFailurePacket);
  socket_ = std::exchange(other.socket_, -1);
  helper_pid_ = std::exchange(other.helper_pid_, -1);
  return *this;
}

FailureDialogMonitor FailureDialogMonitor::Start(
    const Environment& environment, std::string_view initial_message) {
  if (!FailureDialogsEnabled(environment)) {
    return {};
  }
  const std::filesystem::path helper = DialogHelper(environment);
  if (helper.empty()) {
    return {};
  }
  const auto [socket, helper_pid] = SpawnMonitor(helper);
  FailureDialogMonitor monitor(socket, helper_pid);
  if (monitor.active()) {
    g_monitor_socket.store(socket, std::memory_order_release);
    monitor.SetMessage(initial_message);
  }
  return monitor;
}

void FailureDialogMonitor::SetMessage(std::string_view message) {
  if (!message.empty()) Send(kMessagePacket, message);
}

void FailureDialogMonitor::SetLogFolder(const std::filesystem::path& folder) {
  if (!folder.empty()) Send(kLogFolderPacket, folder.string());
}

void FailureDialogMonitor::SetSessionLog(const std::filesystem::path& log) {
  if (!log.empty()) Send(kSessionLogPacket, log.string());
}

void FailureDialogMonitor::Send(char kind, std::string_view payload) {
  if (!active() || SendPacket(socket_, kind, payload)) return;
  RetireMonitorSocket(socket_);
  close(socket_);
  socket_ = -1;
  WaitForHelper(helper_pid_);
  helper_pid_ = -1;
}

void FailureDialogMonitor::MarkSuccessful() { Finish(kSuccessPacket); }

void FailureDialogMonitor::Finish(char disposition) {
  if (!active()) {
    return;
  }
  RetireMonitorSocket(socket_);
  (void)SendPacket(socket_, disposition);
  (void)shutdown(socket_, SHUT_RDWR);
  close(socket_);
  socket_ = -1;
  WaitForHelper(helper_pid_);
  helper_pid_ = -1;
}

}  // namespace runtime
}  // namespace mocktail
