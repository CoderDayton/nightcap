#include "mocktail/graphics/deferred_teardown.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace mocktail::graphics {
namespace {

TEST(DeferredTeardownTest, PostReturnsBeforeSlowWorkFinishes) {
  DeferredTeardown teardown;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<bool> finished{false};

  const auto started = std::chrono::steady_clock::now();
  teardown.Post([released, &finished] {
    released.wait();
    finished = true;
  });
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::milliseconds(100));
  EXPECT_TRUE(teardown.Busy());
  EXPECT_FALSE(finished);

  release.set_value();
  teardown.Drain();
  EXPECT_TRUE(finished);
  EXPECT_FALSE(teardown.Busy());
}

TEST(DeferredTeardownTest, RunsWorkInPostOrderOffTheCallingThread) {
  DeferredTeardown teardown;
  std::mutex mutex;
  std::vector<int> order;
  std::vector<std::thread::id> threads;
  for (int index = 0; index < 4; ++index) {
    teardown.Post([index, &mutex, &order, &threads] {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      std::lock_guard<std::mutex> lock(mutex);
      order.push_back(index);
      threads.push_back(std::this_thread::get_id());
    });
  }
  teardown.Drain();
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3}));
  for (const std::thread::id id : threads) {
    EXPECT_NE(id, std::this_thread::get_id());
  }
}

TEST(DeferredTeardownTest, DestructorFinishesQueuedWork) {
  std::atomic<int> ran{0};
  {
    DeferredTeardown teardown;
    for (int index = 0; index < 3; ++index) {
      teardown.Post([&ran] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ++ran;
      });
    }
  }
  EXPECT_EQ(ran, 3);
}

TEST(DeferredTeardownTest, IdleWithoutWork) {
  DeferredTeardown teardown;
  EXPECT_FALSE(teardown.Busy());
  teardown.Drain();
}

}
}
