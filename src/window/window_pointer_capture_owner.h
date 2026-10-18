#ifndef MOCKTAIL_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_
#define MOCKTAIL_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "mocktail/cursor_image.h"

namespace mocktail {
namespace window {

using MouseLockQueryCallback = bool (*)(void* context, bool* locked_center);

// The desktop cursor shown over the window. kHidden leaves the pointer to
// Roblox, which draws its own. kCustom shows a game's own cursor picture.
enum class CursorShape : std::uint8_t {
  kHidden,
  kArrow,
  kPointer,
  kText,
  kCustom,
};

const char* CursorShapeName(CursorShape shape);

using CursorImagePtr = std::shared_ptr<const CursorImage>;

class PointerCaptureBackend {
 public:
  virtual ~PointerCaptureBackend() = default;
  // `image` is set for kCustom only.
  // `center_pointer` moves the pointer to the middle of the window as
  // relative mode starts, where Roblox draws its cursor under its own lock.
  virtual bool Apply(bool relative_mode, CursorShape cursor,
                     const CursorImagePtr& image, bool center_pointer) = 0;
  // True when a cursor shown during relative mode stays where the capture
  // began, instead of following the hidden pointer.
  virtual bool ShowsCursorWhileCaptured() const { return false; }
};

class WindowPointerCaptureOwner final {
 public:
  // Counts the frames presented to the window. Without one, Roblox's
  // reports never go stale.
  using FrameCounter = std::uint64_t (*)();

  explicit WindowPointerCaptureOwner(PointerCaptureBackend* backend,
                                     FrameCounter presented_frames = nullptr);
  ~WindowPointerCaptureOwner();

  WindowPointerCaptureOwner(const WindowPointerCaptureOwner&) = delete;
  WindowPointerCaptureOwner& operator=(const WindowPointerCaptureOwner&) =
      delete;

  bool RegisterQuery(MouseLockQueryCallback callback, void* context);
  void ClearQuery();
  bool Pump(bool text_input_active);
  // Android's lock-center query does not represent desktop RMB camera drag,
  // so the SDL owner tracks that transient capture source independently.
  bool OnRightButton(bool pressed, bool text_input_active);
  // The RMB fallback only stands in for a camera. Outside an experience there
  // is none, and capturing there freezes the cursor over Roblox's own UI.
  // Starts false: the app surface is what the client shows first.
  void SetGameSessionActive(bool active);
  // In VR the controllers aim and the desktop window only mirrors the left
  // eye, so neither Roblox's mouse lock nor RMB drag may trap the pointer.
  void SetVrActive(bool active);
  // Shift can enable Roblox's persistent mouse lock while a transient RMB
  // camera drag is active. Refresh the native state after dispatching that
  // key so the subsequent RMB release preserves the intentional lock.
  bool OnShiftKeyPressed(bool text_input_active);
  bool NeedsRightButtonReleaseRecovery(bool observed_pressed) const;
  // Mirrors Android's captured-pointer listener: the motion that releases a
  // transient capture is consumed instead of leaking one final camera delta.
  bool ShouldDispatchMouseMotion();
  // Focus loss reveals the system cursor. Regaining focus must re-apply the
  // pointer state immediately: Pump() runs at the top of the frame, before
  // SDL_PollEvent delivers the focus event, so deferring the re-apply leaves
  // the desktop arrow on screen for at least one whole frame.
  bool OnFocusGained(bool text_input_active);
  bool OnFocusLost();
  bool Shutdown();

  // The stock cursor Roblox would draw this frame, as its system-cursor
  // equivalent, or kHidden when it draws a custom one or none. Called from
  // Roblox's render thread each time it draws the cursor. While these keep
  // arriving, Pump shows that desktop cursor instead of hiding the pointer.
  void PublishRobloxCursor(CursorShape shape, CursorImagePtr image = nullptr);
  // True while a cursor Roblox reports would be shown by the desktop. When
  // false, as under shift lock or first person, Roblox must keep drawing its
  // cursor, such as a game's crosshair. Read from Roblox's render thread.
  bool DesktopShowsRobloxCursor() const {
    return desktop_shows_roblox_cursor_.load(std::memory_order_relaxed);
  }

  bool captured() const { return captured_; }
  bool cursor_visible() const { return cursor_ != CursorShape::kHidden; }
  CursorShape cursor() const { return cursor_; }
  const CursorImagePtr& cursor_image() const { return cursor_image_; }

 private:
  friend class WindowPointerCaptureOwnerTestPeer;

  // Roblox stops reporting when it stops drawing a cursor, such as on a
  // loading screen: frames still reach the window, without a report. A
  // Roblox that presents nothing, as when the screen sits still, leaves the
  // last frame and its cursor on screen, so the report holds.
  static constexpr std::uint64_t kRobloxCursorLifetimeFrames = 8;

  std::uint64_t PresentedFrames() const;

  bool Apply(bool capture, CursorShape cursor, CursorImagePtr image = nullptr,
             bool center_pointer = false);
  std::pair<CursorShape, CursorImagePtr> CurrentRobloxCursor();

  PointerCaptureBackend* backend_ = nullptr;
  std::mutex mutex_;
  std::condition_variable condition_;
  MouseLockQueryCallback callback_ = nullptr;
  void* context_ = nullptr;
  std::size_t in_flight_ = 0;
  bool clearing_ = false;
  bool focused_ = true;
  bool right_button_held_ = false;
  // Written by the experience composition, read on the SDL thread.
  std::atomic<bool> game_session_active_{false};
  std::atomic<bool> vr_active_{false};
  bool native_lock_observed_ = false;
  bool native_lock_was_active_before_right_drag_ = false;
  bool shift_key_pressed_during_right_drag_ = false;
  bool wait_for_native_unlock_after_right_drag_ = false;
  bool discard_next_motion_after_right_drag_ = false;
  bool captured_ = false;
  CursorShape cursor_ = CursorShape::kArrow;
  CursorImagePtr cursor_image_;
  bool pointer_centered_ = false;
  // What showed when the current capture began. An RMB camera drag keeps it
  // on screen at the press point; kHidden leaves the drag to Roblox.
  CursorShape capture_cursor_ = CursorShape::kHidden;
  CursorImagePtr capture_cursor_image_;
  FrameCounter presented_frames_ = nullptr;
  // Written on Roblox's render thread, read on the SDL thread.
  std::mutex roblox_cursor_mutex_;
  CursorShape roblox_cursor_ = CursorShape::kHidden;
  CursorImagePtr roblox_cursor_image_;
  bool roblox_cursor_reported_ = false;
  std::atomic<bool> desktop_shows_roblox_cursor_{false};
  // PresentedFrames() at the last report.
  std::uint64_t roblox_cursor_frame_ = 0;
};

class SdlPointerCaptureBackend final : public PointerCaptureBackend {
 public:
  explicit SdlPointerCaptureBackend(void* window) : window_(window) {}

  bool Apply(bool relative_mode, CursorShape cursor,
             const CursorImagePtr& image, bool center_pointer) override;
  bool ShowsCursorWhileCaptured() const override;

 private:
  // The SDL cursor for a shape, or for a game's picture; null when the
  // desktop cannot make it.
  void* CursorFor(CursorShape cursor, const CursorImagePtr& image);

  void* window_ = nullptr;
  // Created on first use and freed by SDL when the video subsystem quits.
  void* system_cursors_[4] = {};
  // Recent game cursors, oldest first.
  std::vector<std::pair<CursorImagePtr, void*>> custom_cursors_;
  void* current_cursor_ = nullptr;
  float capture_anchor_x_ = 0.0f;
  float capture_anchor_y_ = 0.0f;
  bool relative_mode_ = false;
  bool pointer_centered_ = false;
  bool capture_anchor_valid_ = false;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_
