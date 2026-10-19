// Redirects Android logging calls to stderr on the host.

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include "mocktail/platform/android_log_observer.h"

// Android log priorities
enum AndroidLogPriority {
  ANDROID_LOG_UNKNOWN = 0,
  ANDROID_LOG_DEFAULT,
  ANDROID_LOG_VERBOSE,
  ANDROID_LOG_DEBUG,
  ANDROID_LOG_INFO,
  ANDROID_LOG_WARN,
  ANDROID_LOG_ERROR,
  ANDROID_LOG_FATAL,
  ANDROID_LOG_SILENT,
};

static const char* PriorityChar(int prio) {
  switch (prio) {
    case ANDROID_LOG_VERBOSE: return "V";
    case ANDROID_LOG_DEBUG:   return "D";
    case ANDROID_LOG_INFO:    return "I";
    case ANDROID_LOG_WARN:    return "W";
    case ANDROID_LOG_ERROR:   return "E";
    case ANDROID_LOG_FATAL:   return "F";
    default:                  return "?";
  }
}

namespace {

std::atomic<MocktailAndroidLogObserver> g_log_observers[4] = {};
std::mutex g_log_observers_mutex;

void NotifyObserver(int priority, const char* tag, const char* message) {
  for (auto& slot : g_log_observers) {
    MocktailAndroidLogObserver observer = slot.load(std::memory_order_acquire);
    if (observer != nullptr) {
      observer(priority, tag, message);
    }
  }
}

}  // namespace

extern "C" {

bool mocktail_android_log_add_observer(MocktailAndroidLogObserver observer) {
  if (observer == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_log_observers_mutex);
  for (auto& slot : g_log_observers) {
    if (slot.load(std::memory_order_acquire) == observer) {
      return true;
    }
  }
  for (auto& slot : g_log_observers) {
    MocktailAndroidLogObserver empty = nullptr;
    if (slot.compare_exchange_strong(empty, observer,
                                     std::memory_order_acq_rel)) {
      return true;
    }
  }
  return false;
}

void mocktail_android_log_remove_observer(MocktailAndroidLogObserver observer) {
  std::lock_guard<std::mutex> lock(g_log_observers_mutex);
  for (auto& slot : g_log_observers) {
    MocktailAndroidLogObserver expected = observer;
    slot.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
  }
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list observer_args;
  va_copy(observer_args, args);
  char message[4096] = {};
  if (fmt != nullptr) {
    (void)vsnprintf(message, sizeof(message), fmt, observer_args);
  }
  va_end(observer_args);
  fprintf(stderr, "[%s/%s] ", PriorityChar(prio), tag ? tag : "?");
  int ret = fmt != nullptr ? vfprintf(stderr, fmt, args) : 0;
  fprintf(stderr, "\n");
  va_end(args);
  NotifyObserver(prio, tag, message);
  return ret;
}

int __android_log_write(int prio, const char* tag, const char* text) {
  const int result = fprintf(stderr, "[%s/%s] %s\n", PriorityChar(prio),
                             tag ? tag : "?", text ? text : "");
  NotifyObserver(prio, tag, text ? text : "");
  return result;
}

void __android_log_assert(const char* cond, const char* tag,
                           const char* fmt, ...) {
  fprintf(stderr, "[F/%s] Assert failed: %s\n", tag ? tag : "?",
          cond ? cond : "");
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
  }
  __builtin_trap();
}

int __android_log_buf_write(int, int prio, const char* tag,
                             const char* text) {
  return __android_log_write(prio, tag, text);
}

}  // extern "C"
