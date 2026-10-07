#include "window/host_surface_commit_gate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace mocktail {
namespace window {
namespace {

using std::chrono::milliseconds;

TEST(HostSurfaceCommitGateTest, MainThreadWaitsForHostPresentToFinish) {
  HostSurfaceCommitGate gate;
  std::atomic<bool> present_done{false};
  std::atomic<bool> saw_present_done{false};
  std::atomic<bool> owned{false};

  gate.BeginHostPresent();
  std::thread main_thread([&] {
    HostSurfaceCommitGate::MainThreadScope scope(gate, milliseconds(5000));
    owned.store(scope.owns_lock());
    saw_present_done.store(present_done.load());
  });
  std::this_thread::sleep_for(milliseconds(50));
  present_done.store(true);
  gate.EndHostPresent();
  main_thread.join();

  EXPECT_TRUE(owned.load());
  EXPECT_TRUE(saw_present_done.load());
}

TEST(HostSurfaceCommitGateTest, HostPresentWaitsForMainThreadScope) {
  HostSurfaceCommitGate gate;
  std::atomic<bool> scope_released{false};
  std::atomic<bool> saw_scope_released{false};
  std::thread render_thread;

  {
    HostSurfaceCommitGate::MainThreadScope scope(gate, milliseconds(5000));
    ASSERT_TRUE(scope.owns_lock());
    render_thread = std::thread([&] {
      gate.BeginHostPresent();
      saw_scope_released.store(scope_released.load());
      gate.EndHostPresent();
    });
    std::this_thread::sleep_for(milliseconds(50));
    scope_released.store(true);
  }
  render_thread.join();

  EXPECT_TRUE(saw_scope_released.load());
}

TEST(HostSurfaceCommitGateTest, MainThreadGivesUpWhenHostPresentIsStuck) {
  HostSurfaceCommitGate gate;
  std::atomic<bool> owned{true};

  gate.BeginHostPresent();
  std::thread main_thread([&] {
    HostSurfaceCommitGate::MainThreadScope scope(gate, milliseconds(1));
    owned.store(scope.owns_lock());
  });
  main_thread.join();
  gate.EndHostPresent();

  EXPECT_FALSE(owned.load());
}

TEST(HostSurfaceCommitGateTest, EndWithoutBeginIsIgnored) {
  HostSurfaceCommitGate gate;

  gate.EndHostPresent();
  HostSurfaceCommitGate::MainThreadScope scope(gate, milliseconds(1));

  EXPECT_TRUE(scope.owns_lock());
}

TEST(HostSurfaceCommitGateTest, BeginWithoutEndDoesNotBlockTheNextPresent) {
  HostSurfaceCommitGate gate;

  gate.BeginHostPresent();
  gate.BeginHostPresent();
  gate.EndHostPresent();
  HostSurfaceCommitGate::MainThreadScope scope(gate, milliseconds(1));

  EXPECT_TRUE(scope.owns_lock());
}

TEST(HostSurfaceCommitGateTest, PresentsFromOneThreadCanRepeat) {
  HostSurfaceCommitGate gate;

  for (int frame = 0; frame < 3; ++frame) {
    gate.BeginHostPresent();
    gate.EndHostPresent();
  }
  HostSurfaceCommitGate::MainThreadScope scope(gate, milliseconds(1));

  EXPECT_TRUE(scope.owns_lock());
}

}  // namespace
}  // namespace window
}  // namespace mocktail
