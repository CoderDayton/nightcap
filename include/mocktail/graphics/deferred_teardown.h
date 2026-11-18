#ifndef MOCKTAIL_GRAPHICS_DEFERRED_TEARDOWN_H_
#define MOCKTAIL_GRAPHICS_DEFERRED_TEARDOWN_H_

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace mocktail::graphics {

class DeferredTeardown final {
 public:
  DeferredTeardown() = default;
  ~DeferredTeardown();

  DeferredTeardown(const DeferredTeardown&) = delete;
  DeferredTeardown& operator=(const DeferredTeardown&) = delete;

  void Post(std::function<void()> work);
  bool Busy();
  void Drain();

 private:
  void Run();

  std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable idle_;
  std::deque<std::function<void()>> queue_;
  bool running_ = false;
  bool stopping_ = false;
  std::thread worker_;
};

}

#endif
