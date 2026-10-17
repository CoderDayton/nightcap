#include "window/window_pointer_capture_owner.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "window/host_surface_commit_gate.h"

namespace mocktail {
namespace window {

namespace {

// ClearQuery may be called by teardown reached from the native query itself.
// Such a call owns the in-flight marker and must not wait for its own stack to
// unwind.
thread_local WindowPointerCaptureOwner* t_pointer_query_owner = nullptr;

std::int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

const char* CursorShapeName(CursorShape shape) {
  switch (shape) {
    case CursorShape::kHidden:
      return "hidden";
    case CursorShape::kArrow:
      return "arrow";
    case CursorShape::kPointer:
      return "pointer";
    case CursorShape::kText:
      return "text";
  }
  return "unknown";
}

WindowPointerCaptureOwner::WindowPointerCaptureOwner(
    PointerCaptureBackend* backend)
    : backend_(backend), clock_(&SteadyNowNs) {}

WindowPointerCaptureOwner::~WindowPointerCaptureOwner() { Shutdown(); }

bool WindowPointerCaptureOwner::RegisterQuery(MouseLockQueryCallback callback,
                                              void* context) {
  if (callback == nullptr || context == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (clearing_ || callback_ != nullptr) {
    return false;
  }
  callback_ = callback;
  context_ = context;
  return true;
}

void WindowPointerCaptureOwner::ClearQuery() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (clearing_ && t_pointer_query_owner == this) {
    return;
  }
  condition_.wait(lock, [this] { return !clearing_; });
  clearing_ = true;
  callback_ = nullptr;
  context_ = nullptr;
  condition_.notify_all();
  condition_.wait(lock, [this] {
    return in_flight_ == 0 || t_pointer_query_owner == this;
  });
  clearing_ = false;
  lock.unlock();
  condition_.notify_all();
}

bool WindowPointerCaptureOwner::Pump(bool text_input_active) {
  if (!focused_ || vr_active_) {
    return Apply(false, CursorShape::kArrow);
  }
  MouseLockQueryCallback callback = nullptr;
  void* context = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clearing_ && callback_ != nullptr) {
      callback = callback_;
      context = context_;
      ++in_flight_;
    }
  }

  bool locked_center = false;
  const bool query_ok =
      callback != nullptr &&
      [&] {
        WindowPointerCaptureOwner* previous_query_owner =
            t_pointer_query_owner;
        t_pointer_query_owner = this;
        struct ResetCallbackMarker {
          WindowPointerCaptureOwner* previous_query_owner;
          ~ResetCallbackMarker() {
            t_pointer_query_owner = previous_query_owner;
          }
        } reset_callback_marker{previous_query_owner};
        return callback(context, &locked_center);
      }();
  if (callback != nullptr) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --in_flight_;
    }
    condition_.notify_all();
  }
  const bool native_lock_active = query_ok && locked_center;
  native_lock_observed_ = native_lock_active;
  if (wait_for_native_unlock_after_right_drag_ && !native_lock_active) {
    wait_for_native_unlock_after_right_drag_ = false;
  }
  const bool client_active = query_ok;
  const bool native_capture_active = client_active && native_lock_active;
  const bool right_drag_active = right_button_held_ && game_session_active_;
  // Roblox can keep Shift Lock active while chat has text focus. Honor that
  // native lock so camera motion stays relative and cannot leave the window.
  // Text editing only suppresses the host's fallback RMB camera capture.
  const bool capture_requested =
      native_capture_active || (right_drag_active && !text_input_active);
  // When capture is released, Roblox still draws its own cursor. Show the
  // system pointer only when the native client cannot provide one, or when
  // Roblox reports a stock cursor the desktop has its own shape for.
  const CursorShape uncaptured_cursor =
      client_active ? CurrentRobloxCursor() : CursorShape::kArrow;
  CursorShape cursor = CursorShape::kHidden;
  if (!capture_requested) {
    cursor = uncaptured_cursor;
  } else {
    // Not cursor_, which is the unfocused arrow on a refocusing click.
    if (!captured_) {
      capture_cursor_ = uncaptured_cursor;
    }
    // An RMB drag keeps a desktop cursor that was already showing.
    if (!native_capture_active && backend_ != nullptr &&
        backend_->ShowsCursorWhileCaptured()) {
      cursor = capture_cursor_;
    }
  }
  return Apply(capture_requested, cursor);
}

void WindowPointerCaptureOwner::PublishRobloxCursor(CursorShape shape) {
  roblox_cursor_.store(shape, std::memory_order_relaxed);
  roblox_cursor_time_ns_.store(clock_(), std::memory_order_release);
}

CursorShape WindowPointerCaptureOwner::CurrentRobloxCursor() const {
  const std::int64_t published =
      roblox_cursor_time_ns_.load(std::memory_order_acquire);
  if (published == 0 || clock_() - published > kRobloxCursorLifetimeNs) {
    return CursorShape::kHidden;
  }
  return roblox_cursor_.load(std::memory_order_relaxed);
}

bool WindowPointerCaptureOwner::OnRightButton(bool pressed,
                                              bool text_input_active) {
  if (pressed) {
    focused_ = true;
  }
  right_button_held_ = pressed;
  return Pump(text_input_active);
}

void WindowPointerCaptureOwner::SetGameSessionActive(bool active) {
  game_session_active_ = active;
}

void WindowPointerCaptureOwner::SetVrActive(bool active) {
  vr_active_ = active;
}

bool WindowPointerCaptureOwner::OnShiftKeyPressed(bool text_input_active) {
  focused_ = true;
  return Pump(text_input_active);
}

bool WindowPointerCaptureOwner::NeedsRightButtonReleaseRecovery(
    bool /*observed_pressed*/) const {
  return false;
}

bool WindowPointerCaptureOwner::ShouldDispatchMouseMotion() {
  return true;
}

bool WindowPointerCaptureOwner::OnFocusGained(bool text_input_active) {
  focused_ = true;
  return Pump(text_input_active);
}

bool WindowPointerCaptureOwner::OnFocusLost() {
  focused_ = false;
  right_button_held_ = false;
  native_lock_observed_ = false;
  native_lock_was_active_before_right_drag_ = false;
  shift_key_pressed_during_right_drag_ = false;
  wait_for_native_unlock_after_right_drag_ = false;
  discard_next_motion_after_right_drag_ = false;
  return Apply(false, CursorShape::kArrow);
}

bool WindowPointerCaptureOwner::Shutdown() {
  ClearQuery();
  focused_ = false;
  right_button_held_ = false;
  native_lock_observed_ = false;
  native_lock_was_active_before_right_drag_ = false;
  shift_key_pressed_during_right_drag_ = false;
  wait_for_native_unlock_after_right_drag_ = false;
  discard_next_motion_after_right_drag_ = false;
  return Apply(false, CursorShape::kArrow);
}

bool WindowPointerCaptureOwner::Apply(bool capture, CursorShape cursor) {
  if (capture == captured_ && cursor == cursor_) {
    return true;
  }
  if (backend_ == nullptr) {
    captured_ = false;
    cursor_ = CursorShape::kArrow;
    return !capture && cursor == CursorShape::kArrow;
  }
  if (backend_->Apply(capture, cursor)) {
    const bool capture_changed = capture != captured_;
    const bool visibility_changed =
        (cursor == CursorShape::kHidden) != (cursor_ == CursorShape::kHidden);
    captured_ = capture;
    cursor_ = cursor;
    if (capture_changed && capture) {
      std::fprintf(stderr, "  [input] SDL relative pointer capture enabled\n");
    } else if (capture_changed) {
      std::fprintf(stderr, "  [input] SDL relative pointer capture released\n");
    }
    // Shape changes follow every hover, so only visibility is logged.
    if (visibility_changed) {
      std::fprintf(stderr, "  [input] SDL system cursor %s\n",
                   cursor != CursorShape::kHidden ? "shown" : "hidden");
    }
    return true;
  }
  backend_->Apply(false, CursorShape::kArrow);
  captured_ = false;
  cursor_ = CursorShape::kArrow;
  return false;
}

bool SdlPointerCaptureBackend::ShowsCursorWhileCaptured() const {
  // Wayland locks the pointer in place for relative mode. Other drivers
  // confine it to the window center, where a shown cursor would jump.
  const char* driver = SDL_GetCurrentVideoDriver();
  return driver != nullptr && std::strcmp(driver, "wayland") == 0;
}

bool SdlPointerCaptureBackend::Apply(bool relative_mode, CursorShape cursor) {
  auto* window = static_cast<SDL_Window*>(window_);
  if (window == nullptr) {
    return false;
  }

  // Leaving relative mode makes SDL warp the pointer, which commits the
  // window surface on Wayland compositors without wp_pointer_warp_v1.
  HostSurfaceCommitGate::MainThreadScope commit_scope(
      ProcessHostSurfaceCommitGate());
  // Read whenever relative mode or the cursor changes, so it is set first.
  SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_CURSOR_VISIBLE,
              relative_mode && cursor != CursorShape::kHidden ? "1" : "0");
  if (relative_mode && !relative_mode_) {
    SDL_GetMouseState(&capture_anchor_x_, &capture_anchor_y_);
    capture_anchor_valid_ = true;
    SDL_SetWindowMouseGrab(window, true);
  }
  if (!relative_mode && relative_mode_ && capture_anchor_valid_) {
    // SDL requires the warp before relative mode is disabled. This preserves
    // the desktop pointer position across Roblox's transient RMB camera drag.
    SDL_WarpMouseInWindow(window, capture_anchor_x_, capture_anchor_y_);
    SDL_SetWindowMouseGrab(window, false);
  }
  if (relative_mode != relative_mode_ &&
      !SDL_SetWindowRelativeMouseMode(window, relative_mode)) {
    if (relative_mode) {
      capture_anchor_valid_ = false;
      SDL_SetWindowMouseGrab(window, false);
    }
    return false;
  }
  relative_mode_ = relative_mode;
  if (!relative_mode_) {
    capture_anchor_valid_ = false;
  }
  if (cursor == CursorShape::kHidden) {
    return SDL_HideCursor();
  }
  if (cursor != shape_) {
    const auto index = static_cast<std::size_t>(cursor);
    if (system_cursors_[index] == nullptr) {
      const SDL_SystemCursor id =
          cursor == CursorShape::kPointer ? SDL_SYSTEM_CURSOR_POINTER
          : cursor == CursorShape::kText  ? SDL_SYSTEM_CURSOR_TEXT
                                          : SDL_SYSTEM_CURSOR_DEFAULT;
      system_cursors_[index] = SDL_CreateSystemCursor(id);
    }
    // A desktop without the shape keeps the cursor SDL already shows.
    if (system_cursors_[index] != nullptr &&
        SDL_SetCursor(static_cast<SDL_Cursor*>(system_cursors_[index]))) {
      shape_ = cursor;
    }
  }
  return SDL_ShowCursor();
}

}  // namespace window
}  // namespace mocktail
