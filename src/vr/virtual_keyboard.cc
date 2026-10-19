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
constexpr std::uint32_t kScancodeA = 4;           // SDL_SCANCODE_A
constexpr std::uint32_t kScancodeV = 25;          // SDL_SCANCODE_V
constexpr std::uint32_t kScancodeReturn = 40;     // SDL_SCANCODE_RETURN
constexpr std::uint32_t kScancodeEscape = 41;     // SDL_SCANCODE_ESCAPE
constexpr std::uint32_t kScancodeBackspace = 42;  // SDL_SCANCODE_BACKSPACE
constexpr std::uint32_t kScancodeTab = 43;        // SDL_SCANCODE_TAB
constexpr std::uint32_t kScancodeRight = 79;      // SDL_SCANCODE_RIGHT
constexpr std::uint32_t kScancodeLeft = 80;       // SDL_SCANCODE_LEFT

// Matches ControllerStateBuilder's analog click hysteresis.
constexpr float kTriggerPress = 0.75f;
constexpr float kTriggerRelease = 0.6f;
constexpr std::uint64_t kRepeatDelayNs = 450'000'000ULL;
constexpr std::uint64_t kRepeatIntervalNs = 60'000'000ULL;
// Holding a key this long opens its accents.
constexpr std::uint64_t kLongPressNs = 450'000'000ULL;
// Thumbstick caret movement: a push past kStickPress moves once, holding it
// repeats; it re-arms below kStickRelease.
constexpr float kStickPress = 0.6f;
constexpr float kStickRelease = 0.3f;
constexpr std::uint64_t kStickRepeatDelayNs = 400'000'000ULL;
constexpr std::uint64_t kStickRepeatIntervalNs = 80'000'000ULL;
// Cursor moves smaller than this (in UV) do not force a redraw.
constexpr float kCursorRedrawStep = 0.004f;
// A moving cursor redraws the panel at most this often; each redraw is a
// full texture upload on the frame thread.
constexpr std::uint64_t kCursorRedrawIntervalNs = 33'000'000ULL;

constexpr float kPanelDistanceMetres = 0.45f;
constexpr float kPanelDropMetres = 0.28f;
constexpr float kPanelTiltRadians = 35.f * kPi / 180.f;

std::mutex g_host_mutex;
VirtualKeyboardHost g_host;
std::mutex g_text_mutex;
VirtualKeyboardText g_text;

void ScrubText(std::string* text) {
  std::fill(text->begin(), text->end(), '\0');
  text->clear();
}

using A = VirtualKeyAction;

struct KeySpec {
  const char* label;
  const char* text;
  const char* shift_text;
  VirtualKeyAction action;
  float width;
};

KeySpec Char(const char* text, const char* shift_text) {
  return {text, text, shift_text, A::kText, 1.f};
}
KeySpec Special(const char* label, VirtualKeyAction action, float width) {
  return {label, "", "", action, width};
}

// US QWERTY without the function row or Ctrl/Alt/Menu, in normal-key units.
// Widths follow the desktop shell OSK (Tab 1.6, Shift 2.5, wide keys fill).
// The bottom row adds caret editing around a shorter Space.
std::vector<std::vector<KeySpec>> Rows() {
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
      {Special("Select all", A::kSelectAll, 2.f), Special("Left", A::kLeft, 1.f),
       Special("Space", A::kSpace, 7.5f), Special("Right", A::kRight, 1.f),
       Special("Paste", A::kPaste, 2.f), Special("Close", A::kClose, 1.5f)},
  };
}

// Long-press alternates, lower and upper case in the same order.
struct AccentSpec {
  const char* key;
  std::vector<const char*> lower;
  std::vector<const char*> upper;
};

const std::vector<AccentSpec>& Accents() {
  static const std::vector<AccentSpec> accents = {
      {"a",
       {"à", "á", "â", "ä", "æ", "ã", "å",
        "ā"},
       {"À", "Á", "Â", "Ä", "Æ", "Ã", "Å",
        "Ā"}},
      {"c", {"ç", "ć", "č"}, {"Ç", "Ć", "Č"}},
      {"e",
       {"è", "é", "ê", "ë", "ē", "ę", "ė"},
       {"È", "É", "Ê", "Ë", "Ē", "Ę", "Ė"}},
      {"i",
       {"ì", "í", "î", "ï", "ī", "į"},
       {"Ì", "Í", "Î", "Ï", "Ī", "Į"}},
      {"l", {"ł"}, {"Ł"}},
      {"n", {"ñ", "ń"}, {"Ñ", "Ń"}},
      {"o",
       {"ò", "ó", "ô", "ö", "õ", "ø", "œ",
        "ō"},
       {"Ò", "Ó", "Ô", "Ö", "Õ", "Ø", "Œ",
        "Ō"}},
      {"s", {"ß", "ś", "š"}, {"ẞ", "Ś", "Š"}},
      {"u",
       {"ù", "ú", "û", "ü", "ū"},
       {"Ù", "Ú", "Û", "Ü", "Ū"}},
      {"y", {"ý", "ÿ"}, {"Ý", "Ÿ"}},
      {"z", {"ž", "ź", "ż"}, {"Ž", "Ź", "Ż"}},
      {".", {"…"}, {"…"}},
      {"-", {"–", "—"}, {"–", "—"}},
      {"$", {"€", "£", "¥", "¢"},
       {"€", "£", "¥", "¢"}},
      {"!", {"¡"}, {"¡"}},
      {"?", {"¿"}, {"¿"}},
      {"'", {"‘", "’"}, {"‘", "’"}},
      {"\"", {"“", "”", "„"}, {"“", "”", "„"}},
      {"%", {"‰"}, {"‰"}},
  };
  return accents;
}

const AccentSpec* FindAccents(const std::string& key_text) {
  for (const AccentSpec& spec : Accents())
    if (key_text == spec.key) return &spec;
  return nullptr;
}

bool IsLetter(const std::string& text) {
  return text.size() == 1 &&
         std::isalpha(static_cast<unsigned char>(text[0])) != 0;
}

// Long-press alternates for a key that types `output` now. Letters keep
// their key's accents in both cases; other keys follow what they type, so
// Shift+4 offers the currency signs.
const AccentSpec* AccentsFor(const VirtualKey& key, const std::string& output) {
  if (key.action != A::kText) return nullptr;
  return FindAccents(IsLetter(key.text) ? key.text : output);
}

bool Repeats(VirtualKeyAction action) {
  return action == A::kBackspace || action == A::kLeft || action == A::kRight;
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
  const float row_height = (kBodyFraction - kFieldFraction) / kRows;
  for (int row = 0; row < kRows && row < static_cast<int>(rows.size()); ++row) {
    float column = 0.f;
    for (const KeySpec& spec : rows[row]) {
      VirtualKey key;
      key.action = spec.action;
      key.text = spec.text;
      key.shift_text = spec.shift_text;
      key.label = spec.action == A::kText ? Output(key) : spec.label;
      key.u0 = column / kColumns;
      key.u1 = std::min(1.f, (column + spec.width) / kColumns);
      key.v0 = kFieldFraction + row_height * static_cast<float>(row);
      key.v1 = kFieldFraction + row_height * static_cast<float>(row + 1);
      keys_.push_back(std::move(key));
      column += spec.width;
    }
  }
  for (Hand& hand : hands_) {
    hand.hovered = -1;
    hand.pressed = -1;
  }
  CloseAccents();
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
  CloseAccents();
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

bool VirtualKeyboard::OverPanelBody(float u, float v) const {
  return u >= 0.f && u <= 1.f && v >= 0.f && v <= kBodyFraction;
}

bool VirtualKeyboard::OverHandle(float u, float v) {
  return u >= kHandleU0 && u <= kHandleU1 && v > kBodyFraction && v <= 1.f;
}

bool VirtualKeyboard::CapturesTrigger(int hand) const {
  const Hand& state = hands_[hand & 1];
  return release_pending_[hand & 1] ||
         (visible_ && (state.over_panel || state.captured || state.dragging));
}

bool VirtualKeyboard::CapturesThumbstick(int hand) const {
  return visible_ && hands_[hand & 1].over_panel;
}

bool VirtualKeyboard::handle_hovered() const {
  return hands_[0].over_handle || hands_[1].over_handle;
}

bool VirtualKeyboard::cursor(int hand, float* u, float* v) const {
  const Hand& state = hands_[hand & 1];
  if (!visible_ || !state.over_panel) return false;
  *u = state.u;
  *v = state.v;
  return true;
}

void VirtualKeyboard::Commit(const std::string& text,
                             VirtualKeyboardOutput* output) {
  output->text.push_back(text);
  if (shift_) {
    shift_ = false;  // one-shot, like a phone keyboard
    RebuildLayout();
  }
}

void VirtualKeyboard::Activate(const VirtualKey& key,
                               VirtualKeyboardOutput* output) {
  switch (key.action) {
    case A::kText:
      Commit(Output(key), output);
      return;
    case A::kSpace:
      output->text.emplace_back(" ");
      return;
    case A::kBackspace:
      output->keys.push_back({kScancodeBackspace, false});
      return;
    case A::kEnter:
      output->keys.push_back({kScancodeReturn, false});
      return;
    case A::kTab:
      output->keys.push_back({kScancodeTab, false});
      return;
    case A::kLeft:
      output->keys.push_back({kScancodeLeft, false});
      return;
    case A::kRight:
      output->keys.push_back({kScancodeRight, false});
      return;
    case A::kSelectAll:
      output->keys.push_back({kScancodeA, true});
      return;
    case A::kPaste:
      output->keys.push_back({kScancodeV, true});
      return;
    case A::kShift:
      shift_ = !shift_;
      RebuildLayout();
      return;
    case A::kCapsLock:
      caps_lock_ = !caps_lock_;
      RebuildLayout();
      return;
    case A::kClose:
      output->keys.push_back({kScancodeEscape, false});
      Hide();
      return;
  }
}

void VirtualKeyboard::OpenAccents(int hand, int key) {
  const VirtualKey& held = keys_[key];
  const AccentSpec* spec = AccentsFor(held, Output(held));
  if (spec == nullptr) return;
  const bool upper = IsLetter(held.text) ? shift_ != caps_lock_ : shift_;
  const std::vector<const char*>& texts = upper ? spec->upper : spec->lower;
  const float width = 1.f / kColumns;
  const float height = (kBodyFraction - kFieldFraction) / kRows;
  const float count = static_cast<float>(texts.size());
  const float left = std::max(0.f, std::min(held.u0, 1.f - count * width));
  const float bottom = held.v0;
  const float top = std::max(0.f, bottom - height);
  accents_ = VirtualKeyboardAccents{};
  accents_.hand = hand;
  accents_.key = key;
  accents_.selected = 0;
  for (std::size_t i = 0; i < texts.size(); ++i) {
    VirtualKey cell;
    cell.label = cell.text = cell.shift_text = texts[i];
    cell.u0 = left + width * static_cast<float>(i);
    cell.u1 = cell.u0 + width;
    cell.v0 = top;
    cell.v1 = bottom;
    accents_.cells.push_back(std::move(cell));
  }
  Touch();
}

void VirtualKeyboard::CloseAccents() {
  if (accents_.hand < 0) return;
  accents_ = VirtualKeyboardAccents{};
  Touch();
}

void VirtualKeyboard::Drag(Hand* hand, const VirtualKeyboardPointer& pointer) {
  const Vec3 direction =
      QuatRotate(pointer.aim.orientation, Vec3{0.f, 0.f, -1.f});
  const Vec3 grab{pointer.aim.position.x + direction.x * hand->drag_distance,
                  pointer.aim.position.y + direction.y * hand->drag_distance,
                  pointer.aim.position.z + direction.z * hand->drag_distance};
  if (head_valid_) {
    // Turn the panel's face (+Z) toward the head, upright.
    const Vec3 offset = QuatRotate(panel_.orientation, hand->drag_offset);
    const Vec3 centre = Sub(grab, offset);
    Vec3 normal = Sub(head_.position, centre);
    const float length = std::sqrt(Dot(normal, normal));
    if (length > 1e-3f) {
      normal = Vec3{normal.x / length, normal.y / length, normal.z / length};
      const float pitch = -std::asin(std::clamp(normal.y, -1.f, 1.f));
      const float yaw = std::atan2(normal.x, normal.z);
      panel_.orientation =
          QuatMultiply(QuatFromAxisAngle(Vec3{0.f, 1.f, 0.f}, yaw),
                       QuatFromAxisAngle(Vec3{1.f, 0.f, 0.f}, pitch));
    }
  }
  panel_.position = Sub(grab, QuatRotate(panel_.orientation, hand->drag_offset));
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

    const bool was_down = hand.trigger_down;
    if (!pointer.valid) {
      // A drag survives a dropped aim pose; the trigger decides.
      hand.trigger_down =
          hand.dragging && pointer.trigger > kTriggerRelease;
    } else if (was_down) {
      hand.trigger_down = pointer.trigger > kTriggerRelease;
    } else {
      hand.trigger_down = pointer.trigger >= kTriggerPress;
    }

    if (hand.dragging) {
      if (hand.trigger_down) {
        if (pointer.valid) Drag(&hand, pointer);
        continue;
      }
      hand.dragging = false;
      hand.captured = false;
      Touch();
    }

    float u = 0.f, v = 0.f, distance = 0.f;
    const bool hit =
        pointer.valid &&
        IntersectVirtualKeyboardPanel(panel_, kPanelWidthMetres,
                                      kPanelHeightMetres, pointer.aim, &u, &v,
                                      &distance);
    const bool over_body = hit && OverPanelBody(u, v);
    const bool over_handle = hit && OverHandle(u, v);
    const bool over = over_body || over_handle;
    const int hovered = over_body ? KeyAt(u, v) : -1;
    const bool cursor_moved =
        over && (std::abs(u - hand.u) > kCursorRedrawStep ||
                 std::abs(v - hand.v) > kCursorRedrawStep);
    if (over != hand.over_panel || over_handle != hand.over_handle ||
        hovered != hand.hovered || (cursor_moved && cursor_redraw_due)) {
      Touch();
      hand.u = u;
      hand.v = v;
      cursor_drawn_ns_ = now_ns;
    }
    hand.over_panel = over;
    hand.over_handle = over_handle;
    hand.hovered = hovered;

    if (accents_.hand == index) {
      int selected = -1;
      for (std::size_t cell = 0; cell < accents_.cells.size() && hit; ++cell) {
        const VirtualKey& key = accents_.cells[cell];
        if (u >= key.u0 && u < key.u1 && v >= key.v0 && v < key.v1)
          selected = static_cast<int>(cell);
      }
      // Still resting on the held key keeps the first accent.
      if (selected < 0 && hovered == accents_.key) selected = 0;
      if (selected != accents_.selected) {
        accents_.selected = selected;
        if (selected >= 0) output.haptic[index] = true;
        Touch();
      }
    }

    if (hand.trigger_down && !was_down) {
      hand.captured = over;
      if (over_handle) {
        hand.dragging = true;
        hand.drag_distance = distance;
        hand.drag_offset = Vec3{(u - 0.5f) * kPanelWidthMetres,
                                (0.5f - v) * kPanelHeightMetres, 0.f};
        output.haptic[index] = true;
        Touch();
        continue;
      }
      if (hovered >= 0) {
        hand.pressed = hovered;
        hand.pressed_at_ns = now_ns;
        hand.repeat_at_ns = now_ns + kRepeatDelayNs;
        output.haptic[index] = true;
        Touch();
        // Text keys type on release, so a long press can pick an accent and
        // sliding off cancels. Activate may rebuild the layout and clear
        // pressed/hovered.
        const VirtualKey key = keys_[hovered];
        if (key.action != A::kText) Activate(key, &output);
      }
    } else if (hand.trigger_down) {
      if (accents_.hand == index) {
        // The accent row owns this press until it is released.
      } else if (hand.pressed >= 0 && hand.pressed != hovered) {
        hand.pressed = -1;
        Touch();
      } else if (hand.pressed >= 0) {
        const VirtualKey& key = keys_[hand.pressed];
        if (Repeats(key.action) && now_ns >= hand.repeat_at_ns) {
          const VirtualKey copy = key;
          Activate(copy, &output);
          hand.repeat_at_ns = now_ns + kRepeatIntervalNs;
        } else if (key.action == A::kText &&
                   now_ns - hand.pressed_at_ns >= kLongPressNs &&
                   accents_.hand < 0 &&
                   AccentsFor(key, Output(key)) != nullptr) {
          OpenAccents(index, hand.pressed);
          output.haptic[index] = true;
        }
      }
    } else if (was_down) {
      hand.captured = false;
      if (accents_.hand == index) {
        const int selected = accents_.selected;
        const std::string text =
            selected >= 0 ? accents_.cells[selected].text : std::string();
        hand.pressed = -1;
        CloseAccents();
        if (!text.empty()) Commit(text, &output);
      } else if (hand.pressed >= 0) {
        const VirtualKey key = keys_[hand.pressed];
        hand.pressed = -1;
        Touch();
        if (key.action == A::kText) Commit(Output(key), &output);
      }
    }

    // The thumbstick moves the caret while this hand points at the panel.
    if (!visible_) break;  // Close hid the panel above
    int direction = 0;
    if (hand.over_panel && !hand.dragging) {
      const float x = pointer.thumbstick_x;
      direction = x >= kStickPress    ? 1
                  : x <= -kStickPress ? -1
                  : std::abs(x) < kStickRelease ? 0
                                                : hand.stick_direction;
    }
    if (direction != 0 &&
        (direction != hand.stick_direction ||
         now_ns >= hand.stick_repeat_at_ns)) {
      output.keys.push_back(
          {direction < 0 ? kScancodeLeft : kScancodeRight, false});
      hand.stick_repeat_at_ns =
          now_ns + (direction != hand.stick_direction ? kStickRepeatDelayNs
                                                      : kStickRepeatIntervalNs);
    }
    hand.stick_direction = direction;
  }
  return output;
}

}  // namespace mocktail::vr
