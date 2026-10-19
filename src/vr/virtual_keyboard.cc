#include "mocktail/vr/virtual_keyboard.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <mutex>
#include <system_error>

#include <nlohmann/json.hpp>

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
// The emoji grid scrolls a row per push, repeating a little slower.
constexpr std::uint64_t kScrollRepeatIntervalNs = 150'000'000ULL;
// Dragging the grid this far (in rows) turns a press into a scroll.
constexpr float kScrollStartRows = 0.4f;
// A remembered spot farther than this from the head is ignored.
constexpr float kMaxPlacementMetres = 3.f;
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
std::mutex g_state_file_mutex;
std::filesystem::path g_state_file;

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
// The bottom row adds caret editing and the emoji key around a shorter
// Space.
std::vector<std::vector<KeySpec>> Rows(bool with_emoji) {
  std::vector<KeySpec> bottom = {
      Special("Select all", A::kSelectAll, 2.f), Special("Left", A::kLeft, 1.f),
      Special("Space", A::kSpace, with_emoji ? 6.5f : 7.5f),
      Special("Right", A::kRight, 1.f), Special("Paste", A::kPaste, 2.f)};
  if (with_emoji) bottom.push_back(Special("Emoji", A::kEmoji, 1.f));
  bottom.push_back(Special("Close", A::kClose, 1.5f));
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
      bottom,
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
// The key's own alternates (emoji skin tones) come first.
std::vector<std::string> AccentsFor(const VirtualKey& key,
                                    const std::string& output, bool upper) {
  if (key.action != A::kText) return {};
  if (!key.alternates.empty()) return key.alternates;
  const AccentSpec* spec = FindAccents(IsLetter(key.text) ? key.text : output);
  if (spec == nullptr) return {};
  const std::vector<const char*>& texts = upper ? spec->upper : spec->lower;
  return std::vector<std::string>(texts.begin(), texts.end());
}

Quat YawOf(const Pose& head) {
  Vec3 forward = QuatRotate(head.orientation, Vec3{0.f, 0.f, -1.f});
  if (std::abs(forward.x) + std::abs(forward.z) < 1e-3f) return Quat{};
  return QuatFromAxisAngle(Vec3{0.f, 1.f, 0.f},
                           std::atan2(-forward.x, -forward.z));
}

bool Finite(const Pose& pose) {
  const float values[] = {pose.position.x,    pose.position.y,
                          pose.position.z,    pose.orientation.x,
                          pose.orientation.y, pose.orientation.z,
                          pose.orientation.w};
  for (const float value : values)
    if (!std::isfinite(value)) return false;
  return true;
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

void SetVirtualKeyboardStateFile(const std::filesystem::path& file) {
  std::lock_guard<std::mutex> lock(g_state_file_mutex);
  g_state_file = file;
}

std::filesystem::path VirtualKeyboardStateFile() {
  std::lock_guard<std::mutex> lock(g_state_file_mutex);
  return g_state_file;
}

bool LoadVirtualKeyboardPlacement(const std::filesystem::path& file,
                                  Pose* relative) {
  std::ifstream input(file, std::ios::binary);
  if (!input) return false;
  const nlohmann::json state = nlohmann::json::parse(input, nullptr, false);
  if (!state.is_object() || state.value("version", 0) != 1) return false;
  const auto position = state.find("position");
  const auto orientation = state.find("orientation");
  if (position == state.end() || orientation == state.end() ||
      !position->is_array() || position->size() != 3 ||
      !orientation->is_array() || orientation->size() != 4)
    return false;
  for (const auto& value : *position)
    if (!value.is_number()) return false;
  for (const auto& value : *orientation)
    if (!value.is_number()) return false;
  Pose pose;
  pose.position = Vec3{(*position)[0].get<float>(), (*position)[1].get<float>(),
                       (*position)[2].get<float>()};
  pose.orientation =
      Quat{(*orientation)[0].get<float>(), (*orientation)[1].get<float>(),
           (*orientation)[2].get<float>(), (*orientation)[3].get<float>()};
  const float length =
      std::sqrt(Dot(pose.position, pose.position));
  const Quat& q = pose.orientation;
  const float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!Finite(pose) || length > kMaxPlacementMetres || norm < 0.5f)
    return false;
  pose.orientation = Quat{q.x / norm, q.y / norm, q.z / norm, q.w / norm};
  *relative = pose;
  return true;
}

bool SaveVirtualKeyboardPlacement(const std::filesystem::path& file,
                                  const Pose& relative) {
  if (file.empty() || !Finite(relative)) return false;
  const nlohmann::json state = {
      {"version", 1},
      {"position",
       {relative.position.x, relative.position.y, relative.position.z}},
      {"orientation",
       {relative.orientation.x, relative.orientation.y, relative.orientation.z,
        relative.orientation.w}},
  };
  std::error_code error;
  std::filesystem::create_directories(file.parent_path(), error);
  // Written beside the file and renamed over it, so a crash never leaves a
  // half-written spot behind.
  std::filesystem::path temporary = file;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << state.dump() << '\n';
    if (!output.flush()) return false;
  }
  std::filesystem::rename(temporary, file, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

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
  keys_.clear();
  const float row_height = (kBodyFraction - kFieldFraction) / kRows;
  auto place = [&](VirtualKey* key, float column, float width, int row) {
    key->u0 = column / kColumns;
    key->u1 = std::min(1.f, (column + width) / kColumns);
    key->v0 = kFieldFraction + row_height * static_cast<float>(row);
    key->v1 = kFieldFraction + row_height * static_cast<float>(row + 1);
  };
  if (emoji_mode_ && !emoji_.empty()) {
    // The group's emoji fill the top rows, scrolled; the bottom row has the
    // way back, a tab per group, Space and Backspace.
    emoji_group_ = std::clamp(emoji_group_, 0,
                              static_cast<int>(emoji_.groups.size()) - 1);
    emoji_scroll_ = std::clamp(emoji_scroll_, 0,
                               std::max(0, emoji_rows() - (kRows - 1)));
    const std::vector<Emoji>& group = emoji_.groups[emoji_group_].emoji;
    for (int row = 0; row < kRows - 1; ++row) {
      for (int column = 0; column < kColumns; ++column) {
        const std::size_t index =
            static_cast<std::size_t>(emoji_scroll_ + row) * kColumns + column;
        if (index >= group.size()) break;
        VirtualKey key;
        key.label = key.text = key.shift_text = group[index].utf8;
        key.alternates = group[index].variants;
        key.emoji = true;
        place(&key, static_cast<float>(column), 1.f, row);
        keys_.push_back(std::move(key));
      }
    }
    float column = 0.f;
    auto add = [&](VirtualKey key, float width) {
      place(&key, column, width, kRows - 1);
      keys_.push_back(std::move(key));
      column += width;
    };
    VirtualKey letters;
    letters.label = "ABC";
    letters.action = A::kLetters;
    add(letters, 1.5f);
    const int groups = static_cast<int>(emoji_.groups.size());
    for (int index = 0; index < groups; ++index) {
      VirtualKey tab;
      tab.label = emoji_.groups[index].emoji.front().utf8;
      tab.action = A::kEmojiGroup;
      tab.group = index;
      tab.emoji = true;
      add(tab, 1.f);
    }
    VirtualKey space;
    space.label = "Space";
    space.action = A::kSpace;
    add(space, std::max(1.f, kColumns - 3.f - static_cast<float>(groups)));
    VirtualKey backspace;
    backspace.label = "Backspace";
    backspace.action = A::kBackspace;
    add(backspace, 1.5f);
  } else {
  const auto rows = Rows(!emoji_.empty());
  for (int row = 0; row < kRows && row < static_cast<int>(rows.size()); ++row) {
    float column = 0.f;
    for (const KeySpec& spec : rows[row]) {
      VirtualKey key;
      key.action = spec.action;
      key.text = spec.text;
      key.shift_text = spec.shift_text;
      key.label = spec.action == A::kText ? Output(key) : spec.label;
      place(&key, column, spec.width, row);
      keys_.push_back(std::move(key));
      column += spec.width;
    }
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
  const Quat facing = QuatFromAxisAngle(Vec3{0.f, 1.f, 0.f}, yaw);
  if (placement_valid_) {
    // The spot it was last dragged to, turned with the head.
    const Vec3 offset = QuatRotate(facing, placement_.position);
    panel_.position = Vec3{head.position.x + offset.x,
                           head.position.y + offset.y,
                           head.position.z + offset.z};
    panel_.orientation = QuatMultiply(facing, placement_.orientation);
  } else {
    panel_.orientation = QuatMultiply(
        facing, QuatFromAxisAngle(Vec3{1.f, 0.f, 0.f}, -kPanelTiltRadians));
    panel_.position =
        Vec3{head.position.x + forward.x * kPanelDistanceMetres,
             head.position.y - kPanelDropMetres,
             head.position.z + forward.z * kPanelDistanceMetres};
  }
  visible_ = true;
  shift_ = false;
  emoji_mode_ = false;
  for (Hand& hand : hands_) hand = Hand{};
  RebuildLayout();
}

void VirtualKeyboard::SetEmojiCatalog(EmojiCatalog catalog) {
  emoji_ = std::move(catalog);
  if (emoji_.empty()) emoji_mode_ = false;
  emoji_group_ = 0;
  emoji_scroll_ = 0;
  RebuildLayout();
}

int VirtualKeyboard::emoji_rows() const {
  if (emoji_.empty()) return 0;
  const std::size_t count = emoji_.groups[emoji_group_].emoji.size();
  return static_cast<int>((count + kColumns - 1) / kColumns);
}

void VirtualKeyboard::ScrollEmoji(int row) {
  const int clamped = std::clamp(row, 0, std::max(0, emoji_rows() - (kRows - 1)));
  if (clamped == emoji_scroll_) return;
  emoji_scroll_ = clamped;
  RebuildLayout();
}

void VirtualKeyboard::SetPlacement(const Pose& relative) {
  placement_ = relative;
  placement_valid_ = true;
}

bool VirtualKeyboard::TakePlacementChanged(Pose* relative) {
  if (!placement_changed_) return false;
  placement_changed_ = false;
  *relative = placement_;
  return true;
}

void VirtualKeyboard::RememberPlacement() {
  if (!head_valid_) return;
  const Quat facing = YawOf(head_);
  const Quat inverse = QuatConjugate(facing);
  placement_.position =
      QuatRotate(inverse, Sub(panel_.position, head_.position));
  placement_.orientation = QuatMultiply(inverse, panel_.orientation);
  placement_valid_ = true;
  placement_changed_ = true;
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
    case A::kEmoji:
      emoji_mode_ = !emoji_.empty();
      shift_ = false;
      RebuildLayout();
      return;
    case A::kLetters:
      emoji_mode_ = false;
      RebuildLayout();
      return;
    case A::kEmojiGroup:
      emoji_group_ = key.group;
      emoji_scroll_ = 0;
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
  const bool upper = IsLetter(held.text) ? shift_ != caps_lock_ : shift_;
  const std::vector<std::string> texts = AccentsFor(held, Output(held), upper);
  if (texts.empty()) return;
  // Up to a full row wide; more choices (two-person skin tones) wrap onto
  // further rows. Above the key when they fit, else below it.
  const int count = static_cast<int>(texts.size());
  const int columns = std::min(count, kColumns);
  const int rows = (count + columns - 1) / columns;
  const float width = 1.f / kColumns;
  const float height = (kBodyFraction - kFieldFraction) / kRows;
  const float left = std::clamp(held.u0, 0.f, 1.f - columns * width);
  float top = held.v0 - rows * height;
  if (top < 0.f) top = held.v1;
  if (top + rows * height > 1.f) top = std::max(0.f, 1.f - rows * height);
  accents_ = VirtualKeyboardAccents{};
  accents_.hand = hand;
  accents_.key = key;
  accents_.selected = 0;
  for (int i = 0; i < count; ++i) {
    VirtualKey cell;
    cell.label = cell.text = cell.shift_text = texts[i];
    cell.emoji = held.emoji;
    cell.u0 = left + width * static_cast<float>(i % columns);
    cell.u1 = cell.u0 + width;
    cell.v0 = top + height * static_cast<float>(i / columns);
    cell.v1 = cell.v0 + height;
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
      RememberPlacement();
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
        // A press on the emoji grid may turn into dragging it.
        hand.scroll_anchor_v = v;
        hand.scroll_anchor = emoji_scroll_;
        if (key.action != A::kText) Activate(key, &output);
      }
    } else if (hand.trigger_down) {
      const float row_height = (kBodyFraction - kFieldFraction) / kRows;
      const bool grid_press = emoji_mode_ && hand.pressed >= 0 &&
                              keys_[hand.pressed].emoji &&
                              keys_[hand.pressed].action == A::kText;
      if (accents_.hand == index) {
        // The accent row owns this press until it is released.
      } else if (hit && (hand.scrolling ||
                         (grid_press && std::abs(v - hand.scroll_anchor_v) >
                                            kScrollStartRows * row_height))) {
        // Dragging the grid scrolls it, like a phone; nothing is typed.
        if (!hand.scrolling) {
          hand.scrolling = true;
          hand.pressed = -1;
          Touch();
        }
        ScrollEmoji(hand.scroll_anchor + static_cast<int>(std::lround(
                                             (hand.scroll_anchor_v - v) /
                                             row_height)));
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
                   !AccentsFor(key, Output(key), false).empty()) {
          OpenAccents(index, hand.pressed);
          output.haptic[index] = true;
        }
      }
    } else if (was_down) {
      hand.captured = false;
      hand.scrolling = false;
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

    // Up and down on the stick scroll the emoji grid a row at a time.
    int scroll = 0;
    if (emoji_mode_ && hand.over_panel && !hand.dragging) {
      const float y = pointer.thumbstick_y;
      scroll = y >= kStickPress    ? -1
               : y <= -kStickPress ? 1
               : std::abs(y) < kStickRelease ? 0
                                             : hand.scroll_direction;
    }
    if (scroll != 0 && (scroll != hand.scroll_direction ||
                        now_ns >= hand.scroll_repeat_at_ns)) {
      hand.scroll_repeat_at_ns =
          now_ns + (scroll != hand.scroll_direction ? kStickRepeatDelayNs
                                                    : kScrollRepeatIntervalNs);
      ScrollEmoji(emoji_scroll_ + scroll);
    }
    hand.scroll_direction = scroll;
  }
  return output;
}

}  // namespace mocktail::vr
