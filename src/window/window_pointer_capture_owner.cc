#include "window/window_pointer_capture_owner.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <tuple>

#include "window/host_surface_commit_gate.h"

namespace mocktail {
namespace window {

namespace {

// ClearQuery may be called by teardown reached from the native query itself.
// Such a call owns the in-flight marker and must not wait for its own stack to
// unwind.
thread_local WindowPointerCaptureOwner* t_pointer_query_owner = nullptr;


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
    case CursorShape::kCustom:
      return "custom";
  }
  return "unknown";
}

WindowPointerCaptureOwner::WindowPointerCaptureOwner(
    PointerCaptureBackend* backend, FrameCounter presented_frames)
    : backend_(backend), presented_frames_(presented_frames) {}

std::uint64_t WindowPointerCaptureOwner::PresentedFrames() const {
  return presented_frames_ != nullptr ? presented_frames_() : 0;
}

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
  if (!focused_) {
    desktop_shows_roblox_cursor_.store(false, std::memory_order_relaxed);
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

  MouseLock mouse_lock;
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
        return callback(context, &mouse_lock);
      }();
  if (callback != nullptr) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --in_flight_;
    }
    condition_.notify_all();
  }
  const bool native_lock_active = query_ok && mouse_lock.center;
  native_lock_observed_ = native_lock_active;
  if (wait_for_native_unlock_after_right_drag_ && !native_lock_active) {
    wait_for_native_unlock_after_right_drag_ = false;
  }
  const bool client_active = query_ok;
  const bool native_capture_active = client_active && native_lock_active;
  const bool roblox_pans = !query_ok || !mouse_lock.reports_current_position ||
                           mouse_lock.current_position;
  const bool right_drag_active =
      right_button_held_ && game_session_active_ && roblox_pans;
  // Roblox can keep Shift Lock active while chat has text focus. Honor that
  // native lock so camera motion stays relative and cannot leave the window.
  // Text editing only suppresses the host's fallback RMB camera capture.
  const bool capture_requested =
      native_capture_active || (right_drag_active && !text_input_active);
  // When capture is released, Roblox still draws its own cursor. Show the
  // system pointer only when the native client cannot provide one, or when
  // Roblox reports a stock cursor the desktop has its own shape for.
  const std::pair<CursorShape, CursorImagePtr> uncaptured_cursor =
      client_active ? CurrentRobloxCursor()
                    : std::pair<CursorShape, CursorImagePtr>{
                          CursorShape::kArrow, nullptr};
  CursorShape cursor = CursorShape::kHidden;
  CursorImagePtr image;
  bool shows_roblox_cursor = false;
  bool center_pointer = false;
  if (!capture_requested) {
    std::tie(cursor, image) = uncaptured_cursor;
    shows_roblox_cursor = client_active;
  } else {
    // Not cursor_, which is the unfocused arrow on a refocusing click.
    if (!captured_) {
      std::tie(capture_cursor_, capture_cursor_image_) = uncaptured_cursor;
    }
    if (backend_ != nullptr && backend_->ShowsCursorWhileCaptured()) {
      if (native_capture_active) {
        // First person and shift lock draw the cursor at the center, so the
        // desktop one goes there and follows what Roblox reports.
        std::tie(cursor, image) = uncaptured_cursor;
        center_pointer = true;
      } else {
        // An RMB drag keeps a desktop cursor that was already showing.
        cursor = capture_cursor_;
        image = capture_cursor_image_;
      }
      shows_roblox_cursor = client_active;
    }
  }
  desktop_shows_roblox_cursor_.store(shows_roblox_cursor,
                                     std::memory_order_relaxed);
  return Apply(capture_requested, cursor, std::move(image), center_pointer);
}

void WindowPointerCaptureOwner::PublishRobloxCursor(CursorShape shape,
                                                    CursorImagePtr image) {
  if (shape == CursorShape::kCustom && image == nullptr) {
    shape = CursorShape::kHidden;
  }
  if (shape != CursorShape::kCustom) {
    image.reset();
  }
  const std::uint64_t frame = PresentedFrames();
  std::lock_guard<std::mutex> lock(roblox_cursor_mutex_);
  roblox_cursor_ = shape;
  roblox_cursor_image_ = std::move(image);
  roblox_cursor_reported_ = true;
  roblox_cursor_frame_ = frame;
}

std::pair<CursorShape, CursorImagePtr>
WindowPointerCaptureOwner::CurrentRobloxCursor() {
  const std::uint64_t frame = PresentedFrames();
  std::lock_guard<std::mutex> lock(roblox_cursor_mutex_);
  if (!roblox_cursor_reported_ ||
      frame - roblox_cursor_frame_ > kRobloxCursorLifetimeFrames) {
    return {CursorShape::kHidden, nullptr};
  }
  return {roblox_cursor_, roblox_cursor_image_};
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
  desktop_shows_roblox_cursor_.store(false, std::memory_order_relaxed);
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
  desktop_shows_roblox_cursor_.store(false, std::memory_order_relaxed);
  right_button_held_ = false;
  native_lock_observed_ = false;
  native_lock_was_active_before_right_drag_ = false;
  shift_key_pressed_during_right_drag_ = false;
  wait_for_native_unlock_after_right_drag_ = false;
  discard_next_motion_after_right_drag_ = false;
  return Apply(false, CursorShape::kArrow);
}

bool WindowPointerCaptureOwner::Apply(bool capture, CursorShape cursor,
                                      CursorImagePtr image,
                                      bool center_pointer) {
  center_pointer = center_pointer && capture;
  if (capture == captured_ && cursor == cursor_ &&
      image.get() == cursor_image_.get() &&
      center_pointer == pointer_centered_) {
    return true;
  }
  if (backend_ == nullptr) {
    captured_ = false;
    cursor_ = CursorShape::kArrow;
    cursor_image_.reset();
    return !capture && cursor == CursorShape::kArrow;
  }
  if (backend_->Apply(capture, cursor, image, center_pointer)) {
    const bool capture_changed = capture != captured_;
    const bool visibility_changed =
        (cursor == CursorShape::kHidden) != (cursor_ == CursorShape::kHidden);
    captured_ = capture;
    cursor_ = cursor;
    cursor_image_ = std::move(image);
    pointer_centered_ = center_pointer;
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
  backend_->Apply(false, CursorShape::kArrow, nullptr, false);
  captured_ = false;
  cursor_ = CursorShape::kArrow;
  cursor_image_.reset();
  pointer_centered_ = false;
  return false;
}

bool SdlPointerCaptureBackend::ShowsCursorWhileCaptured() const {
  // Wayland locks the pointer in place for relative mode. Other drivers
  // confine it to the window center, where a shown cursor would jump.
  const char* driver = SDL_GetCurrentVideoDriver();
  return driver != nullptr && std::strcmp(driver, "wayland") == 0;
}

void* SdlPointerCaptureBackend::CursorFor(CursorShape cursor,
                                         const CursorImagePtr& image) {
  if (cursor == CursorShape::kCustom) {
    if (image == nullptr) {
      return nullptr;
    }
    for (const auto& [cached, sdl_cursor] : custom_cursors_) {
      if (cached == image) {
        return sdl_cursor;
      }
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        static_cast<int>(image->width), static_cast<int>(image->height),
        SDL_PIXELFORMAT_RGBA32, const_cast<std::uint8_t*>(image->rgba.data()),
        static_cast<int>(image->width * 4));
    if (surface == nullptr) {
      return nullptr;
    }
    SDL_Cursor* created =
        SDL_CreateColorCursor(surface, static_cast<int>(image->hot_x),
                              static_cast<int>(image->hot_y));
    SDL_DestroySurface(surface);
    if (created == nullptr) {
      return nullptr;
    }
    // Games that animate their cursor swap pictures every few frames.
    constexpr std::size_t kMaxCustomCursors = 16;
    if (custom_cursors_.size() >= kMaxCustomCursors) {
      const auto oldest = std::find_if(
          custom_cursors_.begin(), custom_cursors_.end(),
          [this](const auto& entry) { return entry.second != current_cursor_; });
      SDL_DestroyCursor(static_cast<SDL_Cursor*>(oldest->second));
      custom_cursors_.erase(oldest);
    }
    custom_cursors_.emplace_back(image, created);
    return created;
  }
  const auto index = static_cast<std::size_t>(cursor);
  if (system_cursors_[index] == nullptr) {
    const SDL_SystemCursor id =
        cursor == CursorShape::kPointer ? SDL_SYSTEM_CURSOR_POINTER
        : cursor == CursorShape::kText  ? SDL_SYSTEM_CURSOR_TEXT
                                        : SDL_SYSTEM_CURSOR_DEFAULT;
    system_cursors_[index] = SDL_CreateSystemCursor(id);
  }
  return system_cursors_[index];
}

bool SdlPointerCaptureBackend::Apply(bool relative_mode, CursorShape cursor,
                                     const CursorImagePtr& image,
                                     bool center_pointer) {
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
  // Before relative mode starts: Wayland then locks the pointer where it is.
  if (relative_mode && center_pointer && !pointer_centered_) {
    int width = 0;
    int height = 0;
    if (SDL_GetWindowSize(window, &width, &height)) {
      SDL_WarpMouseInWindow(window, width / 2.0f, height / 2.0f);
    }
  }
  pointer_centered_ = relative_mode && center_pointer;
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
  void* wanted = CursorFor(cursor, image);
  if (wanted == nullptr && cursor == CursorShape::kCustom) {
    wanted = CursorFor(CursorShape::kArrow, nullptr);
  }
  // A desktop without the shape keeps the cursor SDL already shows.
  if (wanted != nullptr && wanted != current_cursor_ &&
      SDL_SetCursor(static_cast<SDL_Cursor*>(wanted))) {
    current_cursor_ = wanted;
  }
  return SDL_ShowCursor();
}

}  // namespace window
}  // namespace mocktail
