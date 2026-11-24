#include "legacy/jni_onload_watchdog.h"

#include <errno.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include "runtime/failure_dialog.h"

namespace mocktail::legacy {
namespace {

constexpr std::size_t kLineCapacity = 512;

volatile sig_atomic_t g_stop_requested = 0;
char g_timeout_line[kLineCapacity];
std::size_t g_timeout_line_size = 0;

std::string Duration(int ms) {
  if (ms >= 1000 && ms % 1000 == 0) {
    return std::to_string(ms / 1000) + " s";
  }
  return std::to_string(ms) + " ms";
}

void WriteAll(const char* data, std::size_t size) {
  while (size > 0) {
    const ssize_t written = write(STDERR_FILENO, data, size);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return;
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
}

char* AppendText(char* out, const char* text) {
  while (*text != '\0') {
    *out++ = *text++;
  }
  return out;
}

char* AppendHex(char* out, std::uintptr_t value) {
  constexpr char kDigits[] = "0123456789abcdef";
  out = AppendText(out, "0x");
  for (int shift = static_cast<int>(sizeof(value) * 8) - 4; shift >= 0;
       shift -= 4) {
    *out++ = kDigits[(value >> shift) & 0xf];
  }
  return out;
}

void WriteRegisters(const void* context) {
#if defined(__x86_64__) || defined(__aarch64__)
  const auto* uc = static_cast<const ucontext_t*>(context);
  if (uc == nullptr) {
    return;
  }
#if defined(__x86_64__)
  const char* const names[] = {"RIP", "RSP", "RBP", "RAX"};
  const std::uintptr_t values[] = {
      static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]),
      static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]),
      static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]),
      static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RAX])};
#else
  const char* const names[] = {"PC", "SP", "FP", "X0"};
  const std::uintptr_t values[] = {
      static_cast<std::uintptr_t>(uc->uc_mcontext.pc),
      static_cast<std::uintptr_t>(uc->uc_mcontext.sp),
      static_cast<std::uintptr_t>(uc->uc_mcontext.regs[29]),
      static_cast<std::uintptr_t>(uc->uc_mcontext.regs[0])};
#endif
  char line[160];
  char* out = AppendText(line, "  [timeout]");
  for (std::size_t index = 0; index < 4; ++index) {
    *out++ = ' ';
    out = AppendText(out, names[index]);
    *out++ = '=';
    out = AppendHex(out, values[index]);
  }
  *out++ = '\n';
  WriteAll(line, static_cast<std::size_t>(out - line));
#else
  static_cast<void>(context);
#endif
}

[[noreturn]] void Stop() {
  WriteAll(g_timeout_line, g_timeout_line_size);
  runtime::SendPreparedFailureMessage();
  _exit(kJniOnLoadTimeoutExitCode);
}

void OnStopSignal(int, siginfo_t*, void* context) {
  if (g_stop_requested == 0) {
    return;
  }
  WriteRegisters(context);
  Stop();
}

}

std::string JniOnLoadSlowWarning(int warn_ms, int timeout_ms) {
  return "  [jvm] JNI_OnLoad still running after " + Duration(warn_ms) +
         " (slow disk?); Nightcap stops it at " + Duration(timeout_ms) + "\n";
}

std::string JniOnLoadTimeoutLogLine(int timeout_ms) {
  return "[FATAL] JNI_OnLoad did not finish within " + Duration(timeout_ms) +
         ". On a slow disk, raise MOCKTAIL_JNI_ONLOAD_TIMEOUT_MS (in ms; 0 "
         "turns the limit off).\n";
}

std::string JniOnLoadTimeoutDialogMessage(int timeout_ms) {
  return "Roblox took longer than " + Duration(timeout_ms) +
         " to load, so Nightcap stopped it. A very slow disk can cause "
         "this.\n\nTo allow more time, start Nightcap with "
         "MOCKTAIL_JNI_ONLOAD_TIMEOUT_MS set higher, in milliseconds, or set "
         "it to 0 to turn the limit off.";
}

JniOnLoadWatchdog::JniOnLoadWatchdog(int timeout_ms, int warn_ms,
                                     int signal_grace_ms)
    : timeout_ms_(timeout_ms),
      warn_ms_(warn_ms),
      signal_grace_ms_(std::max(signal_grace_ms, 0)),
      started_at_(std::chrono::steady_clock::now()),
      watched_(pthread_self()) {
  if (timeout_ms_ <= 0) {
    return;
  }
  const std::string timeout_line = JniOnLoadTimeoutLogLine(timeout_ms_);
  g_timeout_line_size = std::min(timeout_line.size(), kLineCapacity);
  std::memcpy(g_timeout_line, timeout_line.data(), g_timeout_line_size);
  runtime::PrepareSignalSafeFailureMessage(
      JniOnLoadTimeoutDialogMessage(timeout_ms_));
  g_stop_requested = 0;

  struct sigaction action {};
  action.sa_sigaction = OnStopSignal;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  handler_installed_ = sigaction(SIGALRM, &action, &previous_action_) == 0;

  const int create_error = pthread_create(&thread_, nullptr, &RunWatch, this);
  thread_started_ = create_error == 0;
  if (!thread_started_) {
    const std::string message = "  [jvm] JNI_OnLoad watchdog did not start: " +
                                std::string(std::strerror(create_error)) +
                                "\n";
    WriteAll(message.data(), message.size());
  }
}

JniOnLoadWatchdog::~JniOnLoadWatchdog() {
  if (thread_started_) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      finished_ = true;
    }
    finished_cv_.notify_all();
    pthread_join(thread_, nullptr);
  }
  if (handler_installed_) {
    sigaction(SIGALRM, &previous_action_, nullptr);
  }
}

void* JniOnLoadWatchdog::RunWatch(void* watchdog) {
  static_cast<JniOnLoadWatchdog*>(watchdog)->Watch();
  return nullptr;
}

void JniOnLoadWatchdog::Watch() {
  const auto finished = [this] { return finished_; };
  std::unique_lock<std::mutex> lock(mutex_);
  if (warn_ms_ > 0 && warn_ms_ < timeout_ms_) {
    if (finished_cv_.wait_until(
            lock, started_at_ + std::chrono::milliseconds(warn_ms_),
            finished)) {
      return;
    }
    const std::string warning = JniOnLoadSlowWarning(warn_ms_, timeout_ms_);
    WriteAll(warning.data(), warning.size());
  }
  if (finished_cv_.wait_until(
          lock, started_at_ + std::chrono::milliseconds(timeout_ms_),
          finished)) {
    return;
  }
  g_stop_requested = 1;
  if (handler_installed_ && pthread_kill(watched_, SIGALRM) == 0) {
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(signal_grace_ms_));
    constexpr char kNoSignal[] =
        "  [timeout] the stuck thread did not take the stop signal\n";
    WriteAll(kNoSignal, sizeof(kNoSignal) - 1);
  }
  Stop();
}

}
