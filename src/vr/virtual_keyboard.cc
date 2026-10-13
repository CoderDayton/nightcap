#include "mocktail/vr/virtual_keyboard.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>

namespace mocktail::vr {
namespace {

using pose::kPi;
using pose::Pose;
using pose::Quat;
using pose::QuatConjugate;
using pose::QuatFromAxisAngle;
using pose::QuatMultiply;
using pose::QuatRotate;
using pose::Vec3;

// SDL scancodes, kept numeric so this pure module needs no SDL headers.
constexpr std::uint32_t kScancodeReturn = 40;     // SDL_SCANCODE_RETURN
constexpr std::uint32_t kScancodeEscape = 41;     // SDL_SCANCODE_ESCAPE
constexpr std::uint32_t kScancodeBackspace = 42;  // SDL_SCANCODE_BACKSPACE
constexpr std::uint32_t kScancodeTab = 43;        // SDL_SCANCODE_TAB

// Matches ControllerStateBuilder's analog click hysteresis.
constexpr float kTriggerPress = 0.75f;
constexpr float kTriggerRelease = 0.6f;
constexpr std::uint64_t kRepeatDelayNs = 450'000'000ULL;
constexpr std::uint64_t kRepeatIntervalNs = 60'000'000ULL;
// Cursor moves smaller than this (in UV) do not force a redraw.
constexpr float kCursorRedrawStep = 0.004f;
// A moving cursor redraws the panel at most this often; each redraw is a
// full texture upload on the frame thread.
constexpr std::uint64_t kCursorRedrawIntervalNs = 33'000'000ULL;

constexpr float kPanelDistanceMetres = 0.45f;
constexpr float kPanelDropMetres = 0.28f;
constexpr float kPanelTiltRadians = 35.f * kPi / 180.f;

// US QWERTY without the function row or Ctrl/Alt/Menu, in normal-key units.
// Widths follow the desktop shell OSK (Tab 1.6, Shift 2.5, wide keys fill).
constexpr float kColumns = 15.f;
constexpr int kRows = 5;

std::mutex g_host_mutex;
VirtualKeyboardHost g_host;
std::mutex g_text_mutex;
VirtualKeyboardText g_text;

void ScrubText(std::string* text) {
  std::fill(text->begin(), text->end(), '\0');
  text->clear();
}

struct KeySpec {
  const char* label;
  const char* text;
  const char* shift_text;
  VirtualKeyAction action;
  float width;
};

KeySpec Char(const char* text, const char* shift_text) {
  return {text, text, shift_text, VirtualKeyAction::kText, 1.f};
}
KeySpec Special(const char* label, VirtualKeyAction action, float width) {
  return {label, "", "", action, width};
}

std::vector<std::vector<KeySpec>> Rows() {
  using A = VirtualKeyAction;
  return {
      {Char("`", "~"), Char("1", "!"), Char("2", "@"), Char("3", "#"),
       Char("4", "$"), Char("5", "%"), Char("6", "^"), Char("7", "&"),
       Char("8", "*"), Char("9", "("), Char("0", ")"), Char("-", "_"),
       Char("=", "+"), Special("Backspace", A::kBackspace, 2.f)},
      {Special("Tab", A::kTab, 1.6f), Char("q", "Q"), Char("w", "W"),
       Char("e", "E"), Char("r", "R"), Char("t", "T"), Char("y", "Y"),
       Char("u", "U"), Char("i", "I"), Char("o", "O"), Char("p", "P"),
       Char("[", "{"), Char("]", "}"), {"\\", "\\", "|", A::kText, 1.4f}},
      {Special("Caps", A::kCapsLock, 2.f), Char("a", "A"), Char("s", "S"),
       Char("d", "D"), Char("f", "F"), Char("g", "G"), Char("h", "H"),
       Char("j", "J"), Char("k", "K"), Char("l", "L"), Char(";", ":"),
       Char("'", "\""), Special("Enter", A::kEnter, 2.f)},
      {Special("Shift", A::kShift, 2.5f), Char("z", "Z"), Char("x", "X"),
       Char("c", "C"), Char("v", "V"), Char("b", "B"), Char("n", "N"),
       Char("m", "M"), Char(",", "<"), Char(".", ">"), Char("/", "?"),
       Special("Shift", A::kShift, 2.5f)},
      {Special("Space", A::kSpace, 12.5f), Special("Close", A::kClose, 2.5f)},
  };
}

bool IsLetter(const std::string& text) {
  return text.size() == 1 &&
         std::isalpha(static_cast<unsigned char>(text[0])) != 0;
}

float Dot(const Vec3& a, const Vec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 Sub(const Vec3& a, const Vec3& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

}  // namespace

void SetVirtualKeyboardHost(const VirtualKeyboardHost* host) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  g_host = host != nullptr ? *host : VirtualKeyboardHost{};
}

VirtualKeyboardHost CurrentVirtualKeyboardHost() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return g_host;
}

void SetVirtualKeyboardText(const char* utf8, std::size_t size,
                            int cursor_utf16, bool password) {
  std::lock_guard<std::mutex> lock(g_text_mutex);
  ScrubText(&g_text.utf8);
  if (utf8 != nullptr) g_text.utf8.assign(utf8, size);
  g_text.cursor_utf16 = std::max(0, cursor_utf16);
  g_text.password = password;
  ++g_text.revision;
}

void ClearVirtualKeyboardText() {
  std::lock_guard<std::mutex> lock(g_text_mutex);
  ScrubText(&g_text.utf8);
  g_text.cursor_utf16 = 0;
  g_text.password = false;
  ++g_text.revision;
}

VirtualKeyboardText CurrentVirtualKeyboardText() {
  std::lock_guard<std::mutex> lock(g_text_mutex);
  return g_text;
}

bool IntersectVirtualKeyboardPanel(const Pose& quad, float width, float height,
                                   const Pose& aim, float* u, float* v,
                                   float* distance) {
  const Vec3 normal = QuatRotate(quad.orientation, Vec3{0.f, 0.f, 1.f});
  const Vec3 direction = QuatRotate(aim.orientation, Vec3{0.f, 0.f, -1.f});
  const float denominator = Dot(direction, normal);
  // Only the front face takes input; a ray from behind passes through.
  if (denominator > -1e-4f) return false;
  const float t = Dot(Sub(quad.position, aim.position), normal) / denominator;
  if (!(t > 0.f)) return false;
  const Vec3 hit{aim.position.x + direction.x * t,
                 aim.position.y + direction.y * t,
                 aim.position.z + direction.z * t};
  const Vec3 local =
      QuatRotate(QuatConjugate(quad.orientation), Sub(hit, quad.position));
  const float hit_u = local.x / width + 0.5f;
  const float hit_v = 0.5f - local.y / height;
  if (hit_u < 0.f || hit_u > 1.f || hit_v < 0.f || hit_v > 1.f) return false;
  if (u != nullptr) *u = hit_u;
  if (v != nullptr) *v = hit_v;
  if (distance != nullptr) *distance = t;
  return true;
}

VirtualKeyboard::VirtualKeyboard() { RebuildLayout(); }

void VirtualKeyboard::RebuildLayout() {
  const auto rows = Rows();
  keys_.clear();
  for (int row = 0; row < kRows; ++row) {
    float column = 0.f;
    for (const KeySpec& spec : rows[row]) {
      VirtualKey key;
      key.action = spec.action;
      key.text = spec.text;
      key.shift_text = spec.shift_text;
      key.label = spec.action == VirtualKeyAction::kText ? Output(key)
                                                         : spec.label;
      key.u0 = column / kColumns;
      key.u1 = std::min(1.f, (column + spec.width) / kColumns);
      constexpr float kField = VirtualKeyboard::kFieldFraction;
      key.v0 = kField + (1.f - kField) * static_cast<float>(row) / kRows;
      key.v1 = kField + (1.f - kField) * static_cast<float>(row + 1) / kRows;
      keys_.push_back(std::move(key));
      column += spec.width;
    }
  }
  for (Hand& hand : hands_) {
    hand.hovered = -1;
    hand.pressed = -1;
  }
  Touch();
}

std::string VirtualKeyboard::Output(const VirtualKey& key) const {
  // Caps Lock only affects letters; Shift inverts it for letters and selects
  // the upper symbol on every other key.
  if (IsLetter(key.text)) {
    return (shift_ != caps_lock_) ? key.shift_text : key.text;
  }
  return shift_ ? key.shift_text : key.text;
}

const std::vector<VirtualKey>& VirtualKeyboard::keys() const { return keys_; }

void VirtualKeyboard::Show(const Pose& head) {
  Vec3 forward = QuatRotate(head.orientation, Vec3{0.f, 0.f, -1.f});
  forward.y = 0.f;
  const float length = std::sqrt(forward.x * forward.x + forward.z * forward.z);
  if (length < 1e-3f) {
    forward = Vec3{0.f, 0.f, -1.f};
  } else {
    forward.x /= length;
    forward.z /= length;
  }
  const float yaw = std::atan2(-forward.x, -forward.z);
  panel_.orientation =
      QuatMultiply(QuatFromAxisAngle(Vec3{0.f, 1.f, 0.f}, yaw),
                   QuatFromAxisAngle(Vec3{1.f, 0.f, 0.f}, -kPanelTiltRadians));
  panel_.position =
      Vec3{head.position.x + forward.x * kPanelDistanceMetres,
           head.position.y - kPanelDropMetres,
           head.position.z + forward.z * kPanelDistanceMetres};
  visible_ = true;
  shift_ = false;
  for (Hand& hand : hands_) hand = Hand{};
  RebuildLayout();
}

void VirtualKeyboard::SetText(const VirtualKeyboardText& text) {
  if (text.revision == text_.revision) return;
  const bool changed = text.utf8 != text_.utf8 ||
                       text.cursor_utf16 != text_.cursor_utf16 ||
                       text.password != text_.password;
  ScrubText(&text_.utf8);
  text_ = text;
  if (changed) Touch();
}

void VirtualKeyboard::Hide() {
  ScrubText(&text_.utf8);
  text_.revision = UINT64_MAX;  // the next SetText always reloads
  if (!visible_) return;
  visible_ = false;
  for (int index = 0; index < 2; ++index) {
    // Keeps a press already pending from an earlier hide of this same press.
    release_pending_[index] =
        release_pending_[index] ||
        (hands_[index].trigger_down && hands_[index].captured);
    hands_[index] = Hand{};
  }
  Touch();
}

int VirtualKeyboard::KeyAt(float u, float v) const {
  for (std::size_t index = 0; index < keys_.size(); ++index) {
    const VirtualKey& key = keys_[index];
    if (u >= key.u0 && u < key.u1 && v >= key.v0 && v < key.v1) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

bool VirtualKeyboard::CapturesTrigger(int hand) const {
  const Hand& state = hands_[hand & 1];
  return release_pending_[hand & 1] ||
         (visible_ && (state.over_panel || state.captured));
}

bool VirtualKeyboard::cursor(int hand, float* u, float* v) const {
  const Hand& state = hands_[hand & 1];
  if (!visible_ || !state.over_panel) return false;
  *u = state.u;
  *v = state.v;
  return true;
}

void VirtualKeyboard::Activate(const VirtualKey& key,
                               VirtualKeyboardOutput* output) {
  switch (key.action) {
    case VirtualKeyAction::kText:
      output->text.push_back(Output(key));
      if (shift_) {
        shift_ = false;  // one-shot, like a phone keyboard
        RebuildLayout();
      }
      return;
    case VirtualKeyAction::kSpace:
      output->text.emplace_back(" ");
      return;
    case VirtualKeyAction::kBackspace:
      output->scancodes.push_back(kScancodeBackspace);
      return;
    case VirtualKeyAction::kEnter:
      output->scancodes.push_back(kScancodeReturn);
      return;
    case VirtualKeyAction::kTab:
      output->scancodes.push_back(kScancodeTab);
      return;
    case VirtualKeyAction::kShift:
      shift_ = !shift_;
      RebuildLayout();
      return;
    case VirtualKeyAction::kCapsLock:
      caps_lock_ = !caps_lock_;
      RebuildLayout();
      return;
    case VirtualKeyAction::kClose:
      output->scancodes.push_back(kScancodeEscape);
      Hide();
      return;
  }
}

VirtualKeyboardOutput VirtualKeyboard::Update(
    std::uint64_t now_ns, const VirtualKeyboardPointer (&pointers)[2]) {
  VirtualKeyboardOutput output;
  bool held_over[2] = {false, false};
  for (int index = 0; index < 2; ++index) {
    if (!release_pending_[index]) continue;
    // Losing the aim pose is not a release: the trigger is read regardless.
    held_over[index] = pointers[index].trigger > kTriggerRelease;
    release_pending_[index] = held_over[index];
  }
  // Decided once per frame, or one moving hand starves the other's cursor.
  const bool cursor_redraw_due =
      now_ns - cursor_drawn_ns_ >= kCursorRedrawIntervalNs;
  for (int index = 0; index < 2 && visible_; ++index) {
    if (held_over[index]) continue;
    Hand& hand = hands_[index];
    const VirtualKeyboardPointer& pointer = pointers[index];
    float u = 0.f, v = 0.f;
    const bool over =
        pointer.valid &&
        IntersectVirtualKeyboardPanel(panel_, kPanelWidthMetres,
                                      kPanelHeightMetres, pointer.aim, &u, &v,
                                      nullptr);
    const int hovered = over ? KeyAt(u, v) : -1;
    const bool cursor_moved =
        over && (std::abs(u - hand.u) > kCursorRedrawStep ||
                 std::abs(v - hand.v) > kCursorRedrawStep);
    if (over != hand.over_panel || hovered != hand.hovered ||
        (cursor_moved && cursor_redraw_due)) {
      Touch();
      hand.u = u;
      hand.v = v;
      cursor_drawn_ns_ = now_ns;
    }
    hand.over_panel = over;
    hand.hovered = hovered;

    const bool was_down = hand.trigger_down;
    if (!pointer.valid) {
      hand.trigger_down = false;
    } else if (was_down) {
      hand.trigger_down = pointer.trigger > kTriggerRelease;
    } else {
      hand.trigger_down = pointer.trigger >= kTriggerPress;
    }

    if (hand.trigger_down && !was_down) {
      hand.captured = over;
      if (hovered >= 0) {
        hand.pressed = hovered;
        hand.repeat_at_ns = now_ns + kRepeatDelayNs;
        Touch();
        // Activate may rebuild the layout and clear pressed/hovered.
        const VirtualKey key = keys_[hovered];
        Activate(key, &output);
      }
    } else if (hand.trigger_down) {
      if (hand.pressed >= 0 && hand.pressed != hovered) {
        hand.pressed = -1;
        Touch();
      } else if (hand.pressed >= 0 &&
                 keys_[hand.pressed].action == VirtualKeyAction::kBackspace &&
                 now_ns >= hand.repeat_at_ns) {
        output.scancodes.push_back(kScancodeBackspace);
        hand.repeat_at_ns = now_ns + kRepeatIntervalNs;
      }
    } else if (was_down) {
      hand.captured = false;
      if (hand.pressed >= 0) {
        hand.pressed = -1;
        Touch();
      }
    }
  }
  return output;
}

}  // namespace mocktail::vr
