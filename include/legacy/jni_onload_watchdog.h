#ifndef MOCKTAIL_LEGACY_JNI_ONLOAD_WATCHDOG_H_
#define MOCKTAIL_LEGACY_JNI_ONLOAD_WATCHDOG_H_

#include <pthread.h>
#include <signal.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace mocktail::legacy {

inline constexpr int kJniOnLoadTimeoutExitCode = 124;
inline constexpr int kJniOnLoadSlowWarningMs = 3000;

std::string JniOnLoadSlowWarning(int warn_ms, int timeout_ms);
std::string JniOnLoadTimeoutLogLine(int timeout_ms);
std::string JniOnLoadTimeoutDialogMessage(int timeout_ms);

class JniOnLoadWatchdog final {
 public:
  JniOnLoadWatchdog(int timeout_ms, int warn_ms, int signal_grace_ms = 2000);
  ~JniOnLoadWatchdog();

  JniOnLoadWatchdog(const JniOnLoadWatchdog&) = delete;
  JniOnLoadWatchdog& operator=(const JniOnLoadWatchdog&) = delete;

 private:
  static void* RunWatch(void* watchdog);
  void Watch();

  const int timeout_ms_;
  const int warn_ms_;
  const int signal_grace_ms_;
  const std::chrono::steady_clock::time_point started_at_;
  const pthread_t watched_;
  struct sigaction previous_action_{};
  bool handler_installed_ = false;
  std::mutex mutex_;
  std::condition_variable finished_cv_;
  bool finished_ = false;
  pthread_t thread_{};
  bool thread_started_ = false;
};

}

#endif
