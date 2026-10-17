#ifndef MOCKTAIL_VR_VIRTUAL_KEYBOARD_H_
#define MOCKTAIL_VR_VIRTUAL_KEYBOARD_H_

#include <cstdint>
#include <string>
#include <vector>

#include "mocktail/vr/vr_pose_math.h"

namespace mocktail::vr {

// Host side of the in-headset keyboard. The window module owns Roblox's
// TextBox focus and the SDL event path; main wires these callbacks so the VR
// library never links the window library. Every callback may be called from
// the render thread and must be thread-safe.
struct VirtualKeyboardHost {
  void* context = nullptr;
  // True while Roblox has asked for an on-screen keyboard (showKeyboard).
  bool (*input_requested)(void* context) = nullptr;
  // Commits UTF-8 text exactly like an SDL_EVENT_TEXT_INPUT.
  void (*commit_text)(void* context, const char* utf8) = nullptr;
  // Presses and releases one SDL scancode exactly like a physical key.
  void (*press_key)(void* context, std::uint32_t scancode) = nullptr;
};

// Installs the process-wide host. Passing nullptr clears it.
void SetVirtualKeyboardHost(const VirtualKeyboardHost* host);
VirtualKeyboardHost CurrentVirtualKeyboardHost();

// The focused TextBox's current contents, shown in the panel's text field.
// The desktop draws TextBox text as a window overlay that never reaches the
// eye images, so the headset would otherwise show an empty box. Safe from any
// thread; the text is sensitive and is never logged.
struct VirtualKeyboardText {
  std::string utf8;
  int cursor_utf16 = 0;
  bool password = false;
  std::uint64_t revision = 0;
};
void SetVirtualKeyboardText(const char* utf8, std::size_t size,
                            int cursor_utf16, bool password);
void ClearVirtualKeyboardText();
VirtualKeyboardText CurrentVirtualKeyboardText();

enum class VirtualKeyAction {
  kText,       // commits text, or shift_text under Shift/Caps Lock
  kBackspace,
  kEnter,
  kTab,
  kShift,      // one-shot
  kCapsLock,
  kSpace,
  kClose,      // Escape: leaves the TextBox and hides the panel
};

struct VirtualKey {
  std::string label;
  std::string text;        // committed text for kText
  std::string shift_text;  // committed under Shift (Caps Lock for letters)
  VirtualKeyAction action = VirtualKeyAction::kText;
  // Panel UV rectangle, origin top-left, both axes 0..1.
  float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
};

// One controller's pointing state for a frame. Positions are metres in the
// OpenXR LOCAL space; the ray points along the aim pose's -Z axis.
struct VirtualKeyboardPointer {
  bool valid = false;
  pose::Pose aim{};
  float trigger = 0.f;  // 0..1
};

// What the keyboard asks the host to do after an Update.
struct VirtualKeyboardOutput {
  std::vector<std::string> text;          // committed in order
  std::vector<std::uint32_t> scancodes;   // pressed in order, after text
};

// Ray/quad intersection. The quad is centred on `quad` with its face along
// +Z, `width` x `height` metres. Returns false when the ray misses or points
// away. u/v are panel UVs (origin top-left).
bool IntersectVirtualKeyboardPanel(const pose::Pose& quad, float width,
                                   float height, const pose::Pose& aim,
                                   float* u, float* v, float* distance);

// Pure keyboard state machine: layout, paging, shift, hover, trigger edges and
// backspace auto-repeat. No OpenXR, Vulkan or SDL types, so it is unit-tested
// directly.
class VirtualKeyboard {
 public:
  // A text field over 15 x 5 normal-key units, 4 cm per unit.
  static constexpr float kPanelWidthMetres = 0.60f;
  static constexpr float kPanelHeightMetres = 0.248f;
  // Texture aspect matches the panel so keys are not stretched.
  static constexpr int kTextureWidth = 1500;
  static constexpr int kTextureHeight = 620;
  // Top share of the panel holding the text field; keys fill the rest.
  static constexpr float kFieldFraction = 120.f / 620.f;

  VirtualKeyboard();

  // Opens the panel in front of `head` (LOCAL space), below eye level and
  // tilted up toward the viewer. Resets Shift and hover; keeps Caps Lock.
  void Show(const pose::Pose& head);
  void Hide();
  bool visible() const { return visible_; }
  const pose::Pose& panel_pose() const { return panel_; }

  // Text shown in the field. Bumps the revision only when it changed.
  void SetText(const VirtualKeyboardText& text);
  const std::string& text() const { return text_.utf8; }
  int text_cursor_utf16() const { return text_.cursor_utf16; }
  bool text_password() const { return text_.password; }

  // Processes one frame of both pointers. now_ns is monotonic.
  VirtualKeyboardOutput Update(std::uint64_t now_ns,
                               const VirtualKeyboardPointer (&pointers)[2]);

  // True when this hand's trigger belongs to the keyboard this frame (aimed
  // at the panel, or a press that began on it is still held, including after
  // the panel hid mid-press). The backend withholds that trigger from Roblox
  // so a key press is not also a click.
  bool CapturesTrigger(int hand) const;

  // Current layout, with Shift/Caps Lock applied to the labels.
  const std::vector<VirtualKey>& keys() const;
  // -1 when the hand is not over a key.
  int hovered_key(int hand) const { return hands_[hand & 1].hovered; }
  int pressed_key(int hand) const { return hands_[hand & 1].pressed; }
  // Pointer position on the panel, when the hand is over it.
  bool cursor(int hand, float* u, float* v) const;
  bool shifted() const { return shift_; }
  bool caps_lock() const { return caps_lock_; }

  // Advances whenever anything visible changes; the backend re-rasterizes
  // and re-uploads only when this moves.
  std::uint64_t revision() const { return revision_; }

 private:
  struct Hand {
    int hovered = -1;
    int pressed = -1;   // key under an active press, -1 otherwise
    bool trigger_down = false;
    bool captured = false;
    bool over_panel = false;
    float u = 0.f, v = 0.f;
    std::uint64_t repeat_at_ns = 0;
  };

  void RebuildLayout();
  int KeyAt(float u, float v) const;
  std::string Output(const VirtualKey& key) const;
  void Activate(const VirtualKey& key, VirtualKeyboardOutput* output);
  void Touch() { ++revision_; }

  bool visible_ = false;
  bool shift_ = false;
  bool caps_lock_ = false;
  pose::Pose panel_{};
  VirtualKeyboardText text_;
  Hand hands_[2];
  // A press the keyboard owned when it hid; held until that trigger releases.
  bool release_pending_[2] = {false, false};
  std::uint64_t cursor_drawn_ns_ = 0;
  std::vector<VirtualKey> keys_;
  std::uint64_t revision_ = 1;
};

// Draws the keyboard into a tightly packed RGBA8 buffer
// (VirtualKeyboard::kTextureWidth x kTextureHeight, sRGB-encoded, opaque).
// Glyphs come from SDL3_ttf with fontconfig's sans fallback chain and are
// cached per label, so redraws after the first are plain fills and blits.
class VirtualKeyboardRasterizer {
 public:
  VirtualKeyboardRasterizer();
  ~VirtualKeyboardRasterizer();
  VirtualKeyboardRasterizer(const VirtualKeyboardRasterizer&) = delete;
  VirtualKeyboardRasterizer& operator=(const VirtualKeyboardRasterizer&) =
      delete;

  // Returns false when no font could be opened; keys still draw, unlabelled.
  bool Draw(const VirtualKeyboard& keyboard, std::vector<std::uint8_t>* rgba);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace mocktail::vr

#endif  // MOCKTAIL_VR_VIRTUAL_KEYBOARD_H_
