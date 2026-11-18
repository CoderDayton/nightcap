#include "mocktail/graphics/deferred_teardown.h"

#include <utility>

namespace mocktail::graphics {

DeferredTeardown::~DeferredTeardown() {
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    worker = std::move(worker_);
  }
  ready_.notify_all();
  if (worker.joinable()) {
    worker.join();
  }
}

void DeferredTeardown::Post(std::function<void()> work) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back(std::move(work));
  if (!worker_.joinable()) {
    worker_ = std::thread([this] { Run(); });
  }
  ready_.notify_one();
}

bool DeferredTeardown::Busy() {
  std::lock_guard<std::mutex> lock(mutex_);
  return running_ || !queue_.empty();
}

void DeferredTeardown::Drain() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return !running_ && queue_.empty(); });
}

void DeferredTeardown::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
    if (queue_.empty()) {
      return;
    }
    std::function<void()> work = std::move(queue_.front());
    queue_.pop_front();
    running_ = true;
    lock.unlock();
    work();
    lock.lock();
    running_ = false;
    if (queue_.empty()) {
      idle_.notify_all();
    }
  }
}

}
