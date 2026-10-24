#include "window/window_pointer_capture_owner.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
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

  static constexpr std::uint64_t kRobloxCursorLifetimeFrames =
      WindowPointerCaptureOwner::kRobloxCursorLifetimeFrames;
};

namespace {

class FakeBackend final : public PointerCaptureBackend {
 public:
  bool Apply(bool relative_mode, CursorShape cursor,
             const CursorImagePtr& image, bool center_pointer) override {
    calls.push_back({relative_mode, cursor != CursorShape::kHidden, cursor,
                     image, center_pointer});
    return succeeds;
  }

  bool ShowsCursorWhileCaptured() const override {
    return shows_captured_cursor;
  }

  struct Call {
    bool relative_mode;
    bool cursor_visible;
    CursorShape cursor;
    CursorImagePtr image;
    bool center_pointer;
  };
  std::vector<Call> calls;
  bool succeeds = true;
  bool shows_captured_cursor = false;
};

struct QueryState {
  bool succeeds = true;
  bool locked_center = false;
  bool locked_current_position = false;
  bool reports_current_position = false;
};

bool Query(void* context, MouseLock* lock) {
  auto* state = static_cast<QueryState*>(context);
  if (state == nullptr || lock == nullptr || !state->succeeds) {
    return false;
  }
  lock->center = state->locked_center;
  lock->current_position = state->locked_current_position;
  lock->reports_current_position = state->reports_current_position;
  return true;
}

struct ReentrantQueryState {
  WindowPointerCaptureOwner* owner = nullptr;
};

bool ClearQueryReentrantly(void* context, MouseLock* lock) {
  auto* state = static_cast<ReentrantQueryState*>(context);
  if (state == nullptr || state->owner == nullptr || lock == nullptr) {
    return false;
  }
  lock->center = false;
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

bool GatedQuery(void* context, MouseLock* mouse_lock) {
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
  mouse_lock->center = false;
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

TEST(WindowPointerCaptureOwnerTest, VrNeverTrapsThePointer) {
  FakeBackend backend;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.Pump(false));
  ASSERT_TRUE(owner.captured());

  // Turning VR on releases a held lock and shows the desktop cursor.
  owner.SetVrActive(true);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  // Neither Roblox's mouse lock nor an RMB drag captures it again.
  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.OnShiftKeyPressed(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.OnFocusGained(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  ASSERT_FALSE(backend.calls.empty());
  EXPECT_FALSE(backend.calls.back().relative_mode);
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

TEST(WindowPointerCaptureOwnerTest, RightDragOnUiThatRobloxKeepsDoesNotCapture) {
  FakeBackend backend;
  QueryState query;
  query.reports_current_position = true;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, RightDragCapturesOnceRobloxStartsPanning) {
  FakeBackend backend;
  QueryState query;
  query.reports_current_position = true;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_FALSE(owner.captured());
  query.locked_current_position = true;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());

  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, CameraPanKeepsCaptureWhenRobloxDropsItsLock) {
  FakeBackend backend;
  QueryState query;
  query.reports_current_position = true;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.Pump(false));

  query.locked_current_position = true;
  ASSERT_TRUE(owner.OnRightButton(true, false));
  ASSERT_TRUE(owner.captured());
  query.locked_current_position = false;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  query.locked_current_position = true;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());

  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());

  query.locked_current_position = false;
  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.OnRightButton(false, false));
}

TEST(WindowPointerCaptureOwnerTest, FailedQueryDoesNotHoldRightDragCapture) {
  FakeBackend backend;
  QueryState query;
  query.reports_current_position = true;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.Pump(false));

  query.succeeds = false;
  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  query.succeeds = true;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.OnRightButton(false, false));
}

TEST(WindowPointerCaptureOwnerTest, PanDuringTextInputDoesNotHoldCaptureAfter) {
  FakeBackend backend;
  QueryState query;
  query.reports_current_position = true;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.Pump(false));

  query.locked_current_position = true;
  EXPECT_TRUE(owner.OnRightButton(true, true));
  EXPECT_FALSE(owner.captured());
  query.locked_current_position = false;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.OnRightButton(false, false));
}

TEST(WindowPointerCaptureOwnerTest, LockCurrentPositionAloneDoesNotCapture) {
  FakeBackend backend;
  QueryState query;
  query.reports_current_position = true;
  query.locked_current_position = true;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.SetGameSessionActive(true);

  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
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

std::uint64_t g_fake_frames = 0;

std::uint64_t FakeFrames() { return g_fake_frames; }

TEST(WindowPointerCaptureOwnerTest, ShowsTheSystemCursorRobloxReports) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  // Nothing reported: Roblox draws its own cursor, as without the setting.
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kHidden);

  owner.PublishRobloxCursor(CursorShape::kPointer);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kPointer);
  EXPECT_EQ(backend.calls.back().cursor, CursorShape::kPointer);

  owner.PublishRobloxCursor(CursorShape::kText);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kText);

  // A game's own cursor stays Roblox's to draw.
  owner.PublishRobloxCursor(CursorShape::kHidden);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kHidden);
}

TEST(WindowPointerCaptureOwnerTest, MouseLockHidesTheReportedCursor) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.PublishRobloxCursor(CursorShape::kArrow);
  ASSERT_TRUE(owner.Pump(false));
  ASSERT_EQ(owner.cursor(), CursorShape::kArrow);

  query.locked_center = true;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_EQ(owner.cursor(), CursorShape::kHidden);

  query.locked_center = false;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_EQ(owner.cursor(), CursorShape::kArrow);
}

TEST(WindowPointerCaptureOwnerTest, StaleReportHidesTheSystemCursor) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend, &FakeFrames);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  g_fake_frames = 100;
  owner.PublishRobloxCursor(CursorShape::kArrow);
  g_fake_frames +=
      WindowPointerCaptureOwnerTestPeer::kRobloxCursorLifetimeFrames;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kArrow);

  // Roblox presents frames with no cursor, as on a loading screen.
  g_fake_frames += 1;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kHidden);
}

TEST(WindowPointerCaptureOwnerTest, IdleRobloxKeepsTheReportedCursor) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend, &FakeFrames);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  g_fake_frames = 100;
  owner.PublishRobloxCursor(CursorShape::kPointer);
  // The screen sits still: Roblox presents nothing and reports nothing, for
  // as many pumps as it takes. The cursor must not blink.
  const std::size_t calls_before = backend.calls.size();
  for (int pump = 0; pump < 1000; ++pump) {
    EXPECT_TRUE(owner.Pump(false));
    EXPECT_EQ(owner.cursor(), CursorShape::kPointer);
  }
  EXPECT_EQ(backend.calls.size(), calls_before + 1);
}

TEST(WindowPointerCaptureOwnerTest, FocusLossShowsTheArrowOverReports) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.PublishRobloxCursor(CursorShape::kText);
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_EQ(owner.cursor(), CursorShape::kArrow);
  EXPECT_TRUE(owner.OnFocusGained(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kText);
}

TEST(WindowPointerCaptureOwnerTest, CameraDragKeepsTheCursorAtThePressPoint) {
  FakeBackend backend;
  backend.shows_captured_cursor = true;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.PublishRobloxCursor(CursorShape::kPointer);
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_EQ(owner.cursor(), CursorShape::kPointer);
  // Roblox hides its own cursor for the drag; the shape from the press holds.
  owner.PublishRobloxCursor(CursorShape::kHidden);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kPointer);
  EXPECT_TRUE(backend.calls.back().relative_mode);

  owner.PublishRobloxCursor(CursorShape::kArrow);
  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_EQ(owner.cursor(), CursorShape::kArrow);
}

TEST(WindowPointerCaptureOwnerTest, CameraDragOverRobloxCursorKeepsItHidden) {
  FakeBackend backend;
  backend.shows_captured_cursor = true;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));
  ASSERT_EQ(owner.cursor(), CursorShape::kHidden);

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_EQ(owner.cursor(), CursorShape::kHidden);
}

TEST(WindowPointerCaptureOwnerTest, RefocusingDragOverRobloxCursorKeepsItHidden) {
  FakeBackend backend;
  backend.shows_captured_cursor = true;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));
  ASSERT_TRUE(owner.OnFocusLost());
  ASSERT_EQ(owner.cursor(), CursorShape::kArrow);

  // The right click that brings focus back starts the drag at once.
  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_EQ(owner.cursor(), CursorShape::kHidden);
}

TEST(WindowPointerCaptureOwnerTest, FirstPersonCentersTheReportedCursor) {
  FakeBackend backend;
  backend.shows_captured_cursor = true;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.PublishRobloxCursor(CursorShape::kArrow);
  ASSERT_TRUE(owner.Pump(false));

  // First person: Roblox locks the mouse and draws its cursor at the center.
  query.locked_center = true;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_EQ(owner.cursor(), CursorShape::kArrow);
  EXPECT_TRUE(backend.calls.back().center_pointer);

  // It follows what Roblox reports, such as a game's crosshair.
  const CursorImagePtr crosshair = std::make_shared<CursorImage>();
  owner.PublishRobloxCursor(CursorShape::kCustom, crosshair);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kCustom);
  EXPECT_EQ(owner.cursor_image(), crosshair);

  // The shift lock cursor stays Roblox's.
  owner.PublishRobloxCursor(CursorShape::kHidden);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kHidden);

  query.locked_center = false;
  owner.PublishRobloxCursor(CursorShape::kArrow);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(backend.calls.back().center_pointer);
}

TEST(WindowPointerCaptureOwnerTest, CameraDragDoesNotCenterThePointer) {
  FakeBackend backend;
  backend.shows_captured_cursor = true;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.PublishRobloxCursor(CursorShape::kArrow);
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  ASSERT_FALSE(backend.calls.empty());
  EXPECT_FALSE(backend.calls.back().center_pointer);
}

TEST(WindowPointerCaptureOwnerTest, LockDuringCameraDragCentersThePointer) {
  FakeBackend backend;
  backend.shows_captured_cursor = true;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.PublishRobloxCursor(CursorShape::kArrow);
  ASSERT_TRUE(owner.Pump(false));
  ASSERT_TRUE(owner.OnRightButton(true, false));
  ASSERT_FALSE(backend.calls.back().center_pointer);

  // Zooming into first person or enabling shift lock mid-drag moves the
  // cursor from the press point to the center, where Roblox now draws it.
  query.locked_center = true;
  owner.PublishRobloxCursor(CursorShape::kPointer);
  EXPECT_TRUE(owner.OnShiftKeyPressed(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_TRUE(backend.calls.back().center_pointer);
  EXPECT_EQ(owner.cursor(), CursorShape::kPointer);

  // Releasing the button keeps the lock, and the cursor, at the center.
  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_TRUE(backend.calls.back().center_pointer);
}

TEST(WindowPointerCaptureOwnerTest, CameraDragHidesTheCursorWhereItWouldJump) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  owner.PublishRobloxCursor(CursorShape::kArrow);
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_EQ(owner.cursor(), CursorShape::kHidden);
}

CursorImagePtr MakeCursorImage() {
  auto image = std::make_shared<CursorImage>();
  image->width = 2;
  image->height = 2;
  image->hot_x = 1;
  image->hot_y = 1;
  image->rgba.assign(16, 0xFF);
  return image;
}

TEST(WindowPointerCaptureOwnerTest, ShowsAGameCursorPicture) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  const CursorImagePtr first = MakeCursorImage();
  owner.PublishRobloxCursor(CursorShape::kCustom, first);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kCustom);
  EXPECT_EQ(owner.cursor_image(), first);
  EXPECT_EQ(backend.calls.back().image, first);

  // An animated cursor swaps pictures; each one reaches the desktop.
  const std::size_t calls = backend.calls.size();
  const CursorImagePtr second = MakeCursorImage();
  owner.PublishRobloxCursor(CursorShape::kCustom, second);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(backend.calls.size(), calls + 1);
  EXPECT_EQ(owner.cursor_image(), second);

  owner.PublishRobloxCursor(CursorShape::kArrow, second);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kArrow);
  EXPECT_EQ(owner.cursor_image(), nullptr);
}

TEST(WindowPointerCaptureOwnerTest, GameCursorWithoutAPictureStaysRobloxs) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  owner.PublishRobloxCursor(CursorShape::kCustom, nullptr);
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_EQ(owner.cursor(), CursorShape::kHidden);
}

TEST(WindowPointerCaptureOwnerTest, CameraDragKeepsTheGameCursorPicture) {
  FakeBackend backend;
  backend.shows_captured_cursor = true;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  const CursorImagePtr image = MakeCursorImage();
  owner.PublishRobloxCursor(CursorShape::kCustom, image);
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_EQ(owner.cursor(), CursorShape::kCustom);
  EXPECT_EQ(owner.cursor_image(), image);
}

TEST(WindowPointerCaptureOwnerTest, LockLeavesTheCursorToRobloxOnlyWhereHidden) {
  FakeBackend backend;
  backend.shows_captured_cursor = true;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  EXPECT_FALSE(owner.DesktopShowsRobloxCursor());

  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.DesktopShowsRobloxCursor());

  // Under Roblox's lock the desktop shows the cursor at the center.
  query.locked_center = true;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.DesktopShowsRobloxCursor());

  query.locked_center = false;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.DesktopShowsRobloxCursor());

  // The drag keeps showing the desktop cursor where it can.
  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.DesktopShowsRobloxCursor());
  EXPECT_TRUE(owner.OnRightButton(false, false));

  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_FALSE(owner.DesktopShowsRobloxCursor());
}

TEST(WindowPointerCaptureOwnerTest, DragThatHidesTheCursorLeavesItToRoblox) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.DesktopShowsRobloxCursor());
}

TEST(WindowPointerCaptureOwnerTest, LockWhereTheCursorWouldJumpLeavesItToRoblox) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  // A game's crosshair under shift lock on X11: the desktop hides its
  // cursor, so Roblox must keep drawing it.
  query.locked_center = true;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.DesktopShowsRobloxCursor());
  EXPECT_EQ(owner.cursor(), CursorShape::kHidden);
}

std::uint64_t g_fake_now_ms = 0;

std::uint64_t FakeNowMs() { return g_fake_now_ms; }

TEST(WindowPointerCaptureOwnerTest, BriefLockDropKeepsTheCapture) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend, &FakeFrames, &FakeNowMs);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  g_fake_frames = 10;
  g_fake_now_ms = 1000;
  query.locked_center = true;
  ASSERT_TRUE(owner.Pump(false));
  ASSERT_TRUE(owner.captured());
  const std::size_t calls_before = backend.calls.size();

  for (int frame = 0; frame < 20; ++frame) {
    query.locked_center = frame % 2 == 0;
    g_fake_frames += 1;
    g_fake_now_ms += 16;
    EXPECT_TRUE(owner.Pump(false));
    EXPECT_TRUE(owner.captured());
  }
  EXPECT_EQ(backend.calls.size(), calls_before);
}

TEST(WindowPointerCaptureOwnerTest, LockOffForSeveralFramesReleases) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend, &FakeFrames, &FakeNowMs);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  g_fake_frames = 10;
  g_fake_now_ms = 1000;
  query.locked_center = true;
  ASSERT_TRUE(owner.Pump(false));

  query.locked_center = false;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  g_fake_frames += 2;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  g_fake_frames += 1;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, LockDropReleasesWhenRobloxStopsPresenting) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend, &FakeFrames, &FakeNowMs);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  g_fake_frames = 10;
  g_fake_now_ms = 1000;
  query.locked_center = true;
  ASSERT_TRUE(owner.Pump(false));

  query.locked_center = false;
  EXPECT_TRUE(owner.Pump(false));
  g_fake_now_ms += 249;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  g_fake_now_ms += 1;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, CameraDragReleasesAtOnce) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend, &FakeFrames, &FakeNowMs);
  owner.SetGameSessionActive(true);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  g_fake_frames = 10;
  g_fake_now_ms = 1000;
  ASSERT_TRUE(owner.OnRightButton(true, false));
  ASSERT_TRUE(owner.captured());

  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, LockCentersThePointerWhereTheCursorHides) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  query.locked_center = true;
  EXPECT_TRUE(owner.Pump(false));
  ASSERT_FALSE(backend.calls.empty());
  EXPECT_TRUE(backend.calls.back().center_pointer);
}

TEST(WindowPointerCaptureOwnerTest, FailedLockCaptureGetsNoGrace) {
  FakeBackend backend;
  backend.succeeds = false;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend, &FakeFrames, &FakeNowMs);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  g_fake_frames = 10;
  g_fake_now_ms = 1000;
  query.locked_center = true;
  EXPECT_FALSE(owner.Pump(false));
  backend.calls.clear();

  query.locked_center = false;
  owner.Pump(false);
  for (const FakeBackend::Call& call : backend.calls) {
    EXPECT_FALSE(call.relative_mode);
  }
}

}  // namespace
}  // namespace window
}  // namespace mocktail
