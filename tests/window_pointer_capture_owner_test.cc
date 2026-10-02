#include "window/window_pointer_capture_owner.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace mocktail {
namespace window {

class WindowPointerCaptureOwnerTestPeer {
 public:
  static void WaitForClearingQuery(WindowPointerCaptureOwner* owner) {
    std::unique_lock<std::mutex> lock(owner->mutex_);
    owner->condition_.wait(lock, [owner] {
      return owner->clearing_ && owner->in_flight_ == 1 &&
             owner->callback_ == nullptr && owner->context_ == nullptr;
    });
  }

  static bool ClearingQueryIsInFlight(WindowPointerCaptureOwner* owner) {
    std::lock_guard<std::mutex> lock(owner->mutex_);
    return owner->clearing_ && owner->in_flight_ == 1;
  }

  static bool HasInFlightQuery(WindowPointerCaptureOwner* owner) {
    std::lock_guard<std::mutex> lock(owner->mutex_);
    return owner->in_flight_ != 0;
  }
};

namespace {

class FakeBackend final : public PointerCaptureBackend {
 public:
  bool Apply(bool relative_mode, bool cursor_visible) override {
    calls.push_back({relative_mode, cursor_visible});
    return succeeds;
  }

  struct Call {
    bool relative_mode;
    bool cursor_visible;
  };
  std::vector<Call> calls;
  bool succeeds = true;
};

struct QueryState {
  bool succeeds = true;
  bool locked_center = false;
};

bool Query(void* context, bool* locked_center) {
  auto* state = static_cast<QueryState*>(context);
  if (state == nullptr || locked_center == nullptr || !state->succeeds) {
    return false;
  }
  *locked_center = state->locked_center;
  return true;
}

struct ReentrantQueryState {
  WindowPointerCaptureOwner* owner = nullptr;
};

bool ClearQueryReentrantly(void* context, bool* locked_center) {
  auto* state = static_cast<ReentrantQueryState*>(context);
  if (state == nullptr || state->owner == nullptr || locked_center == nullptr) {
    return false;
  }
  *locked_center = false;
  state->owner->ClearQuery();
  return true;
}

struct GatedQueryState {
  WindowPointerCaptureOwner* owner = nullptr;
  std::mutex mutex;
  std::condition_variable condition;
  bool reentrant = false;
  bool entered = false;
  bool allow_inner_clear = false;
  bool inner_clear_returned = false;
  bool allow_return = false;
  bool external_clear_returned = false;
  bool second_clear_started = false;
  bool second_clear_returned = false;
};

bool GatedQuery(void* context, bool* locked_center) {
  auto* state = static_cast<GatedQueryState*>(context);
  std::unique_lock<std::mutex> lock(state->mutex);
  state->entered = true;
  state->condition.notify_all();
  if (state->reentrant) {
    state->condition.wait(lock, [state] { return state->allow_inner_clear; });
    lock.unlock();
    state->owner->ClearQuery();
    lock.lock();
    state->inner_clear_returned = true;
    state->condition.notify_all();
  }
  state->condition.wait(lock, [state] { return state->allow_return; });
  *locked_center = false;
  return true;
}

void RunOverlappingClearScenario(bool reentrant) {
  // Every failure exits the child directly. A deadlock cannot strand a join or
  // a test destructor in the parent; the alarm bounds all waits in this child.
  alarm(8);
  FakeBackend backend;
  WindowPointerCaptureOwner owner(&backend);
  GatedQueryState state;
  state.owner = &owner;
  state.reentrant = reentrant;
  if (!owner.RegisterQuery(GatedQuery, &state)) {
    std::_Exit(1);
  }
  std::thread pump([&] {
    if (!owner.Pump(false)) {
      std::_Exit(2);
    }
  });
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    state.condition.wait(lock, [&state] { return state.entered; });
  }
  std::thread external_clear([&] {
    owner.ClearQuery();
    if (WindowPointerCaptureOwnerTestPeer::HasInFlightQuery(&owner)) {
      std::_Exit(7);
    }
    std::lock_guard<std::mutex> lock(state.mutex);
    state.external_clear_returned = true;
    state.condition.notify_all();
  });
  WindowPointerCaptureOwnerTestPeer::WaitForClearingQuery(&owner);

  QueryState replacement;
  if (owner.RegisterQuery(Query, &replacement)) {
    std::_Exit(3);
  }
  std::thread second_clear;
  if (reentrant) {
    std::unique_lock<std::mutex> lock(state.mutex);
    state.allow_inner_clear = true;
    state.condition.notify_all();
    state.condition.wait(lock,
                         [&state] { return state.inner_clear_returned; });
  } else {
    second_clear = std::thread([&] {
      {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.second_clear_started = true;
        state.condition.notify_all();
      }
      owner.ClearQuery();
      if (WindowPointerCaptureOwnerTestPeer::HasInFlightQuery(&owner)) {
        std::_Exit(7);
      }
      std::lock_guard<std::mutex> lock(state.mutex);
      state.second_clear_returned = true;
      state.condition.notify_all();
    });
    std::unique_lock<std::mutex> lock(state.mutex);
    state.condition.wait(lock, [&state] { return state.second_clear_started; });
  }

  if (!WindowPointerCaptureOwnerTestPeer::ClearingQueryIsInFlight(&owner) ||
      owner.RegisterQuery(Query, &replacement)) {
    std::_Exit(4);
  }
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.external_clear_returned || state.second_clear_returned) {
      std::_Exit(5);
    }
    state.allow_return = true;
    state.condition.notify_all();
  }
  pump.join();
  external_clear.join();
  if (second_clear.joinable()) {
    second_clear.join();
  }
  if (!state.external_clear_returned ||
      (!reentrant && !state.second_clear_returned) ||
      !owner.RegisterQuery(Query, &replacement)) {
    std::_Exit(6);
  }
  owner.ClearQuery();
  std::_Exit(0);
}

TEST(WindowPointerCaptureOwnerTest, FollowsNativeMouseLockState) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  query.locked_center = true;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  query.locked_center = false;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
}

TEST(WindowPointerCaptureOwnerTest, TextInputPreservesNativeShiftLock) {
  FakeBackend backend;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  // Opening chat must not turn relative motion into absolute motion while
  // Roblox still requests center lock: the pointer would reach the window edge
  // and camera rotation would stop during typing.
  EXPECT_TRUE(owner.Pump(true));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());

  // Roblox can release its lock to let the user interact with a text field.
  query.locked_center = false;
  EXPECT_TRUE(owner.Pump(true));
  EXPECT_FALSE(owner.captured());
  query.locked_center = true;
  EXPECT_TRUE(owner.Pump(true));
  EXPECT_TRUE(owner.captured());

  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_FALSE(owner.captured());
  // Regular pumps continue while the window is unfocused. A stale native
  // lock must not recapture the pointer before focus actually returns.
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_TRUE(owner.OnFocusGained(true));
  EXPECT_TRUE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, QueryOrCaptureFailureStaysReleased) {
  FakeBackend backend;
  backend.succeeds = false;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_FALSE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_EQ(backend.calls.size(), 2U);
  query.succeeds = false;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, QueryCanClearItselfWithoutDeadlock) {
  FakeBackend backend;
  WindowPointerCaptureOwner owner(&backend);
  ReentrantQueryState query{&owner};
  ASSERT_TRUE(owner.RegisterQuery(ClearQueryReentrantly, &query));

  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());

  QueryState replacement;
  EXPECT_TRUE(owner.RegisterQuery(Query, &replacement));
}

TEST(WindowPointerCaptureOwnerTest, ClearRemovesQueryAndAllowsReplacement) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));
  ASSERT_FALSE(owner.cursor_visible());

  owner.ClearQuery();
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_TRUE(owner.RegisterQuery(Query, &query));
}

TEST(WindowPointerCaptureOwnerTest,
     QueryCanClearItselfWhileExternalClearWaitsForItsReturn) {
  ASSERT_EXIT(RunOverlappingClearScenario(true), ::testing::ExitedWithCode(0),
              "");
}

TEST(WindowPointerCaptureOwnerTest,
     ConcurrentExternalClearsWaitForQueryReturn) {
  ASSERT_EXIT(RunOverlappingClearScenario(false), ::testing::ExitedWithCode(0),
              "");
}

TEST(WindowPointerCaptureOwnerTest, RightDragDoesNotCaptureOutsideAnExperience) {
  // The app surface has no camera. Capturing there would freeze the cursor
  // over Roblox's own menu UI.
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, RightDragCapturesOnceTheGameSurfaceIsLive) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  owner.SetGameSessionActive(true);
  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());

  // Leaving the experience releases a capture that is still held.
  owner.SetGameSessionActive(false);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, RightDragCapturesUntilButtonRelease) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());

  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
}

TEST(WindowPointerCaptureOwnerTest,
     RightReleaseRestoresUnlockedStateWhenNoNativeLock) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  query.locked_center = true;
  ASSERT_TRUE(owner.OnRightButton(true, false));
  constexpr int kTenSecondsAt240Hz = 2400;
  for (int pump = 0; pump < kTenSecondsAt240Hz; ++pump) {
    ASSERT_TRUE(owner.Pump(false));
  }
  ASSERT_TRUE(owner.captured());

  query.locked_center = false;
  ASSERT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest,
     RightReleasePreservesNativeLockThatPredatedDrag) {
  FakeBackend backend;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  ASSERT_TRUE(owner.OnRightButton(true, false));
  ASSERT_TRUE(owner.OnRightButton(false, false));

  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest,
     ShiftLockActivatedDuringRightDragSurvivesButtonRelease) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  ASSERT_TRUE(owner.OnRightButton(true, false));
  query.locked_center = true;
  ASSERT_TRUE(owner.OnShiftKeyPressed(false));
  ASSERT_TRUE(owner.OnRightButton(false, false));

  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest,
     ShiftDuringRightDragDoesNotInventNativeMouseLock) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  ASSERT_TRUE(owner.OnRightButton(true, false));
  ASSERT_TRUE(owner.OnShiftKeyPressed(false));
  ASSERT_TRUE(owner.OnRightButton(false, false));

  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest, RightDragDoesNotDependOnNativeLockQuery) {
  FakeBackend backend;
  QueryState query{false, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.SetGameSessionActive(true);

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
}

TEST(WindowPointerCaptureOwnerTest, TextAndFocusCancelRightDragCapture) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.SetGameSessionActive(true);

  EXPECT_TRUE(owner.OnRightButton(true, true));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_TRUE(owner.OnFocusGained(false));
  EXPECT_FALSE(owner.captured());
}

// Roblox draws its own cursor while it owns the pointer. Focus loss reveals the
// system cursor; regaining focus has to hide it again in the same call, because
// Pump() runs before SDL_PollEvent delivers the focus event and would otherwise
// leave the desktop arrow on screen for a whole frame.
TEST(WindowPointerCaptureOwnerTest, FocusGainHidesTheSystemCursorImmediately) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  ASSERT_TRUE(owner.Pump(false));
  ASSERT_FALSE(owner.cursor_visible());

  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_TRUE(owner.cursor_visible());

  const std::size_t calls_before = backend.calls.size();
  EXPECT_TRUE(owner.OnFocusGained(false));
  EXPECT_FALSE(owner.cursor_visible());
  ASSERT_GT(backend.calls.size(), calls_before);
  EXPECT_FALSE(backend.calls.back().cursor_visible);

  // Text input releases capture but Roblox still owns the visible cursor.
  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_TRUE(owner.OnFocusGained(true));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
}

TEST(WindowPointerCaptureOwnerTest, TextInputWithoutNativeCursorShowsSystemCursor) {
  FakeBackend backend;
  QueryState query{false, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_TRUE(owner.Pump(true));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());

  EXPECT_TRUE(owner.OnRightButton(true, true));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());

  query.succeeds = true;
  EXPECT_TRUE(owner.Pump(true));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
}

}  // namespace
}  // namespace window
}  // namespace mocktail
