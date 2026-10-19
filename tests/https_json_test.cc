#include "runtime/https_json.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace mocktail::runtime {
namespace {

TEST(HttpsJsonTest, CancelledRequestReturnsWithoutWaitingForTimeouts) {
  std::atomic<bool> cancel{true};
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(GetHttpsJson("https://10.255.255.1/json", &cancel).has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::milliseconds(1000));
}

TEST(HttpsJsonTest, CancellingARunningRequestStopsIt) {
  std::atomic<bool> cancel{false};
  std::thread canceller([&cancel] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    cancel.store(true);
  });
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(GetHttpsJson("https://10.255.255.1/json", &cancel).has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::milliseconds(1000));
  canceller.join();
}

}
}
