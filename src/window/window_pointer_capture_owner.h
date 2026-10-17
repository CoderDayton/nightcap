#ifndef MOCKTAIL_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_
#define MOCKTAIL_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace mocktail {
namespace window {

using MouseLockQueryCallback = bool (*)(void* context, bool* locked_center);

// The desktop cursor shown over the window. kHidden leaves the pointer to
// Roblox, which draws its own.
enum class CursorShape : std::uint8_t {
  kHidden,
  kArrow,
  kPointer,
  kText,
};

const char* CursorShapeName(CursorShape shape);

class PointerCaptureBackend {
 public:
  virtual ~PointerCaptureBackend() = default;
  virtual bool Apply(bool relative_mode, CursorShape cursor) = 0;
  // True when a cursor shown during relative mode stays where the capture
  // began, instead of following the hidden pointer.
  virtual bool ShowsCursorWhileCaptured() const { return false; }
};

class WindowPointerCaptureOwner final {
 public:
  explicit WindowPointerCaptureOwner(PointerCaptureBackend* backend);
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
  void PublishRobloxCursor(CursorShape shape);

  bool captured() const { return captured_; }
  bool cursor_visible() const { return cursor_ != CursorShape::kHidden; }
  CursorShape cursor() const { return cursor_; }

 private:
  friend class WindowPointerCaptureOwnerTestPeer;

  using Clock = std::int64_t (*)();
  // Roblox stops reporting when it stops drawing a cursor, such as on a
  // loading screen. An older report no longer describes the screen.
  static constexpr std::int64_t kRobloxCursorLifetimeNs = 250'000'000;

  bool Apply(bool capture, CursorShape cursor);
  CursorShape CurrentRobloxCursor() const;

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
  bool native_lock_observed_ = false;
  bool native_lock_was_active_before_right_drag_ = false;
  bool shift_key_pressed_during_right_drag_ = false;
  bool wait_for_native_unlock_after_right_drag_ = false;
  bool discard_next_motion_after_right_drag_ = false;
  bool captured_ = false;
  CursorShape cursor_ = CursorShape::kArrow;
  // What showed when the current capture began. An RMB camera drag keeps it
  // on screen at the press point.
  CursorShape capture_cursor_ = CursorShape::kArrow;
  Clock clock_;
  std::atomic<CursorShape> roblox_cursor_{CursorShape::kHidden};
  // 0 until Roblox reports a cursor.
  std::atomic<std::int64_t> roblox_cursor_time_ns_{0};
};

class SdlPointerCaptureBackend final : public PointerCaptureBackend {
 public:
  explicit SdlPointerCaptureBackend(void* window) : window_(window) {}

  bool Apply(bool relative_mode, CursorShape cursor) override;
  bool ShowsCursorWhileCaptured() const override;

 private:
  void* window_ = nullptr;
  // Created on first use and freed by SDL when the video subsystem quits.
  void* system_cursors_[4] = {};
  CursorShape shape_ = CursorShape::kHidden;
  float capture_anchor_x_ = 0.0f;
  float capture_anchor_y_ = 0.0f;
  bool relative_mode_ = false;
  bool capture_anchor_valid_ = false;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_
