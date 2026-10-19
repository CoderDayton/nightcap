#include <gtest/gtest.h>

#include <cctype>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "mocktail/vr/virtual_keyboard.h"

namespace mocktail::vr {
namespace {

using pose::Pose;
using pose::Quat;
using pose::QuatFromAxisAngle;
using pose::QuatRotate;
using pose::Vec3;

constexpr std::uint64_t kMs = 1'000'000ULL;
constexpr std::uint32_t kBackspace = 42;
constexpr std::uint32_t kReturn = 40;
constexpr std::uint32_t kEscape = 41;
constexpr std::uint32_t kTab = 43;
constexpr std::uint32_t kRight = 79;
constexpr std::uint32_t kLeft = 80;

std::vector<std::uint32_t> Scancodes(const VirtualKeyboardOutput& out) {
  std::vector<std::uint32_t> codes;
  for (const VirtualKeyPress& key : out.keys) codes.push_back(key.scancode);
  return codes;
}

Pose HeadAt(float x, float y, float z, float yaw = 0.f) {
  Pose head;
  head.position = Vec3{x, y, z};
  head.orientation = QuatFromAxisAngle(Vec3{0.f, 1.f, 0.f}, yaw);
  return head;
}

// An aim pose 30 cm in front of the panel, pointing straight at (u, v).
Pose AimAt(const VirtualKeyboard& keyboard, float u, float v) {
  const Pose& panel = keyboard.panel_pose();
  const Vec3 local{(u - 0.5f) * VirtualKeyboard::kPanelWidthMetres,
                   (0.5f - v) * VirtualKeyboard::kPanelHeightMetres, 0.3f};
  const Vec3 offset = QuatRotate(panel.orientation, local);
  Pose aim;
  aim.position = Vec3{panel.position.x + offset.x, panel.position.y + offset.y,
                      panel.position.z + offset.z};
  aim.orientation = panel.orientation;  // -Z of the aim is -normal
  return aim;
}

int FindKey(const VirtualKeyboard& keyboard, const std::string& label) {
  const auto& keys = keyboard.keys();
  for (std::size_t i = 0; i < keys.size(); ++i)
    if (keys[i].label == label) return static_cast<int>(i);
  return -1;
}

Pose AimAtKey(const VirtualKeyboard& keyboard, const std::string& label) {
  const int index = FindKey(keyboard, label);
  EXPECT_GE(index, 0) << label;
  const VirtualKey& key = keyboard.keys()[index];
  return AimAt(keyboard, (key.u0 + key.u1) / 2, (key.v0 + key.v1) / 2);
}

struct Driver {
  VirtualKeyboard keyboard;
  std::uint64_t now = 1'000 * kMs;
  VirtualKeyboardOutput Frame(const Pose& aim, float trigger, int hand = 1,
                              std::uint64_t step = 11 * kMs) {
    now += step;
    VirtualKeyboardPointer pointers[2];
    pointers[hand].valid = true;
    pointers[hand].aim = aim;
    pointers[hand].trigger = trigger;
    return keyboard.Update(now, pointers);
  }
  // Press and release a key, returning everything it produced.
  VirtualKeyboardOutput Tap(const std::string& label) {
    const Pose aim = AimAtKey(keyboard, label);
    VirtualKeyboardOutput out = Frame(aim, 1.f);
    const VirtualKeyboardOutput up = Frame(aim, 0.f);
    out.text.insert(out.text.end(), up.text.begin(), up.text.end());
    out.keys.insert(out.keys.end(), up.keys.begin(), up.keys.end());
    return out;
  }
  // Text typed by tapping a key (text keys commit on release).
  std::vector<std::string> Type(const std::string& label) {
    return Tap(label).text;
  }
};

TEST(VirtualKeyboardTest, RayHitsQuadCentreAndRejectsBackAndAway) {
  Pose quad;
  quad.position = Vec3{0.f, 0.f, -1.f};
  Pose aim;  // origin, looking down -Z
  float u = 0.f, v = 0.f, distance = 0.f;
  ASSERT_TRUE(IntersectVirtualKeyboardPanel(quad, 0.5f, 0.2f, aim, &u, &v,
                                            &distance));
  EXPECT_NEAR(u, 0.5f, 1e-5f);
  EXPECT_NEAR(v, 0.5f, 1e-5f);
  EXPECT_NEAR(distance, 1.f, 1e-5f);

  // Up and to the right maps to the top-right of the panel (v grows down).
  aim.position = Vec3{0.2f, 0.08f, 0.f};
  ASSERT_TRUE(IntersectVirtualKeyboardPanel(quad, 0.5f, 0.2f, aim, &u, &v,
                                            nullptr));
  EXPECT_NEAR(u, 0.9f, 1e-5f);
  EXPECT_NEAR(v, 0.1f, 1e-5f);

  aim.position = Vec3{0.3f, 0.f, 0.f};  // misses the 0.5 m width
  EXPECT_FALSE(IntersectVirtualKeyboardPanel(quad, 0.5f, 0.2f, aim, &u, &v,
                                             nullptr));
  aim.position = Vec3{};
  aim.orientation = QuatFromAxisAngle(Vec3{0.f, 1.f, 0.f}, pose::kPi);
  EXPECT_FALSE(IntersectVirtualKeyboardPanel(quad, 0.5f, 0.2f, aim, &u, &v,
                                             nullptr));
  // Behind the panel looking back at it: the back face takes no input.
  aim.position = Vec3{0.f, 0.f, -2.f};
  EXPECT_FALSE(IntersectVirtualKeyboardPanel(quad, 0.5f, 0.2f, aim, &u, &v,
                                             nullptr));
}

TEST(VirtualKeyboardTest, ShowPlacesPanelBelowAndInFrontFacingTheHead) {
  VirtualKeyboard keyboard;
  EXPECT_FALSE(keyboard.visible());
  // Facing +X (yaw -90 degrees) at standing height.
  keyboard.Show(HeadAt(1.f, 1.6f, 2.f, -pose::kPi / 2));
  ASSERT_TRUE(keyboard.visible());
  const Pose& panel = keyboard.panel_pose();
  EXPECT_NEAR(panel.position.x, 1.45f, 1e-4f);
  EXPECT_NEAR(panel.position.y, 1.32f, 1e-4f);
  EXPECT_NEAR(panel.position.z, 2.f, 1e-4f);
  const Vec3 normal = QuatRotate(panel.orientation, Vec3{0.f, 0.f, 1.f});
  EXPECT_LT(normal.x, -0.5f);  // faces back toward the head
  EXPECT_GT(normal.y, 0.3f);   // tilted up toward the eyes

  // Looking from the eyes at the panel centre lands mid-panel.
  Pose eyes = HeadAt(1.f, 1.6f, 2.f);
  const Vec3 to_panel{panel.position.x - 1.f, panel.position.y - 1.6f, 0.f};
  const float pitch = std::atan2(to_panel.y, to_panel.x);
  eyes.orientation = pose::QuatMultiply(
      QuatFromAxisAngle(Vec3{0.f, 1.f, 0.f}, -pose::kPi / 2),
      QuatFromAxisAngle(Vec3{1.f, 0.f, 0.f}, pitch));
  float u = 0.f, v = 0.f;
  ASSERT_TRUE(IntersectVirtualKeyboardPanel(
      panel, VirtualKeyboard::kPanelWidthMetres,
      VirtualKeyboard::kPanelHeightMetres, eyes, &u, &v, nullptr));
  EXPECT_NEAR(u, 0.5f, 1e-3f);
  EXPECT_NEAR(v, 0.5f, 1e-3f);
}

TEST(VirtualKeyboardTest, TriggerPressTypesHoveredKeyOnce) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const Pose aim = AimAtKey(d.keyboard, "q");
  EXPECT_TRUE(d.Frame(aim, 0.f).text.empty());
  EXPECT_EQ(d.keyboard.hovered_key(1), FindKey(d.keyboard, "q"));
  // Text keys type on release; the press only selects the key and ticks.
  const auto pressed = d.Frame(aim, 0.9f);
  EXPECT_TRUE(pressed.text.empty());
  EXPECT_TRUE(pressed.haptic[1]);
  EXPECT_FALSE(pressed.haptic[0]);
  EXPECT_EQ(d.keyboard.pressed_key(1), FindKey(d.keyboard, "q"));
  // Held, and hovering between the hysteresis thresholds: no repeat for text,
  // and "q" has no accents to open.
  EXPECT_TRUE(d.Frame(aim, 0.7f, 1, 600 * kMs).text.empty());
  EXPECT_EQ(d.keyboard.accents().hand, -1);
  EXPECT_EQ(d.Frame(aim, 0.f).text, std::vector<std::string>{"q"});
  EXPECT_EQ(d.keyboard.pressed_key(1), -1);
  EXPECT_EQ(Scancodes(d.Tap("Enter")), std::vector<std::uint32_t>{kReturn});
  EXPECT_EQ(d.Type("Space"), std::vector<std::string>{" "});
  EXPECT_EQ(Scancodes(d.Tap("Tab")), std::vector<std::uint32_t>{kTab});
}

TEST(VirtualKeyboardTest, SlidingOffAKeyBeforeReleaseTypesNothing) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  d.Frame(AimAtKey(d.keyboard, "q"), 1.f);
  d.Frame(AimAtKey(d.keyboard, "w"), 1.f);
  EXPECT_TRUE(d.Frame(AimAtKey(d.keyboard, "w"), 0.f).text.empty());
}

TEST(VirtualKeyboardTest, ShiftIsOneShotAndSelectsUpperSymbols) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  d.Tap("Shift");
  EXPECT_TRUE(d.keyboard.shifted());
  EXPECT_GE(FindKey(d.keyboard, "!"), 0);  // labels follow Shift
  EXPECT_EQ(d.Type("A"), std::vector<std::string>{"A"});
  EXPECT_FALSE(d.keyboard.shifted());
  EXPECT_EQ(d.Type("b"), std::vector<std::string>{"b"});
  d.Tap("Shift");
  EXPECT_EQ(d.Type("!"), std::vector<std::string>{"!"});
  EXPECT_EQ(d.Type("1"), std::vector<std::string>{"1"});
  d.Tap("Shift");
  EXPECT_EQ(d.Type("|"), std::vector<std::string>{"|"});
  EXPECT_EQ(d.Type("\\"), std::vector<std::string>{"\\"});
  // Tapping Shift again cancels it.
  d.Tap("Shift");
  d.Tap("Shift");
  EXPECT_FALSE(d.keyboard.shifted());
}

// US QWERTY pairs from the desktop shell OSK (KiiteShell layouts.json,
// "English (US)"), in layout order.
const std::vector<std::pair<std::string, std::string>> kUsShiftPairs = {
    {"`", "~"},  {"1", "!"},  {"2", "@"},  {"3", "#"},  {"4", "$"},
    {"5", "%"},  {"6", "^"},  {"7", "&"},  {"8", "*"},  {"9", "("},
    {"0", ")"},  {"-", "_"},  {"=", "+"},  {"q", "Q"},  {"w", "W"},
    {"e", "E"},  {"r", "R"},  {"t", "T"},  {"y", "Y"},  {"u", "U"},
    {"i", "I"},  {"o", "O"},  {"p", "P"},  {"[", "{"},  {"]", "}"},
    {"\\", "|"}, {"a", "A"},  {"s", "S"},  {"d", "D"},  {"f", "F"},
    {"g", "G"},  {"h", "H"},  {"j", "J"},  {"k", "K"},  {"l", "L"},
    {";", ":"},  {"'", "\""}, {"z", "Z"},  {"x", "X"},  {"c", "C"},
    {"v", "V"},  {"b", "B"},  {"n", "N"},  {"m", "M"},  {",", "<"},
    {".", ">"},  {"/", "?"}};

std::vector<int> TextKeyIndices(const VirtualKeyboard& keyboard) {
  std::vector<int> indices;
  for (std::size_t i = 0; i < keyboard.keys().size(); ++i)
    if (keyboard.keys()[i].action == VirtualKeyAction::kText)
      indices.push_back(static_cast<int>(i));
  return indices;
}

TEST(VirtualKeyboardTest, EveryKeyShiftsToItsUsQwertyCharacter) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const std::vector<int> text_keys = TextKeyIndices(d.keyboard);
  ASSERT_EQ(text_keys.size(), kUsShiftPairs.size());
  for (std::size_t n = 0; n < kUsShiftPairs.size(); ++n) {
    const auto& [plain, shifted] = kUsShiftPairs[n];
    const int index = text_keys[n];
    SCOPED_TRACE("key " + plain);
    ASSERT_EQ(d.keyboard.keys()[index].label, plain);
    EXPECT_EQ(d.Type(plain), std::vector<std::string>{plain});
    // Shift relabels this key, the tap types the shifted character, and the
    // one-shot Shift then puts the plain label back.
    d.Tap("Shift");
    ASSERT_TRUE(d.keyboard.shifted());
    EXPECT_EQ(d.keyboard.keys()[index].label, shifted);
    EXPECT_EQ(d.Type(shifted), std::vector<std::string>{shifted});
    EXPECT_FALSE(d.keyboard.shifted());
    EXPECT_EQ(d.keyboard.keys()[index].label, plain);
  }
}

TEST(VirtualKeyboardTest, CapsLockAndShiftCombineLikeAPhysicalKeyboard) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const std::vector<int> text_keys = TextKeyIndices(d.keyboard);
  ASSERT_EQ(text_keys.size(), kUsShiftPairs.size());
  d.Tap("Caps");
  ASSERT_TRUE(d.keyboard.caps_lock());
  for (std::size_t n = 0; n < kUsShiftPairs.size(); ++n) {
    const auto& [plain, shifted] = kUsShiftPairs[n];
    const bool letter = plain.size() == 1 && std::isalpha(
        static_cast<unsigned char>(plain[0]));
    const int index = text_keys[n];
    SCOPED_TRACE("key " + plain);
    // Caps Lock: letters upper, everything else unchanged.
    const std::string caps = letter ? shifted : plain;
    EXPECT_EQ(d.keyboard.keys()[index].label, caps);
    EXPECT_EQ(d.Type(caps), std::vector<std::string>{caps});
    // Caps Lock + Shift: letters back to lower, symbols shifted.
    d.Tap("Shift");
    const std::string both = letter ? plain : shifted;
    EXPECT_EQ(d.keyboard.keys()[index].label, both);
    EXPECT_EQ(d.Type(both), std::vector<std::string>{both});
    EXPECT_TRUE(d.keyboard.caps_lock());
  }
}

TEST(VirtualKeyboardTest, CapsLockAffectsLettersOnlyAndShiftInvertsIt) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  d.Tap("Caps");
  EXPECT_TRUE(d.keyboard.caps_lock());
  EXPECT_EQ(d.Type("C"), std::vector<std::string>{"C"});
  EXPECT_EQ(d.Type("D"), std::vector<std::string>{"D"});
  EXPECT_EQ(d.Type("1"), std::vector<std::string>{"1"});
  d.Tap("Shift");
  EXPECT_EQ(d.Type("e"), std::vector<std::string>{"e"});
  EXPECT_TRUE(d.keyboard.caps_lock());
  d.Tap("Caps");
  EXPECT_EQ(d.Type("f"), std::vector<std::string>{"f"});
}

TEST(VirtualKeyboardTest, BackspaceRepeatsWhileHeld) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const Pose aim = AimAtKey(d.keyboard, "Backspace");
  EXPECT_EQ(Scancodes(d.Frame(aim, 1.f)),
            std::vector<std::uint32_t>{kBackspace});
  EXPECT_TRUE(d.Frame(aim, 1.f, 1, 300 * kMs).keys.empty());
  EXPECT_EQ(d.Frame(aim, 1.f, 1, 200 * kMs).keys.size(), 1u);
  EXPECT_TRUE(d.Frame(aim, 1.f, 1, 30 * kMs).keys.empty());
  EXPECT_EQ(d.Frame(aim, 1.f, 1, 40 * kMs).keys.size(), 1u);
  // Sliding off the key stops the repeat.
  const Pose off = AimAtKey(d.keyboard, "p");
  EXPECT_TRUE(d.Frame(off, 1.f, 1, 500 * kMs).keys.empty());
}

TEST(VirtualKeyboardTest, ArrowKeysRepeatAndShortcutsHoldCtrl) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const Pose left = AimAtKey(d.keyboard, "Left");
  EXPECT_EQ(Scancodes(d.Frame(left, 1.f)), std::vector<std::uint32_t>{kLeft});
  EXPECT_EQ(Scancodes(d.Frame(left, 1.f, 1, 500 * kMs)),
            std::vector<std::uint32_t>{kLeft});
  d.Frame(left, 0.f);
  EXPECT_EQ(Scancodes(d.Tap("Right")), std::vector<std::uint32_t>{kRight});
  EXPECT_EQ(d.Tap("Select all").keys,
            (std::vector<VirtualKeyPress>{{4, true}}));
  EXPECT_EQ(d.Tap("Paste").keys, (std::vector<VirtualKeyPress>{{25, true}}));
  EXPECT_FALSE(d.Tap("Left").keys.front().ctrl);
}

TEST(VirtualKeyboardTest, ThumbstickMovesTheCaretWhileAimedAtThePanel) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  auto stick = [&](const Pose& aim, float x, std::uint64_t step = 11 * kMs) {
    d.now += step;
    VirtualKeyboardPointer pointers[2];
    pointers[1].valid = true;
    pointers[1].aim = aim;
    pointers[1].thumbstick_x = x;
    return Scancodes(d.keyboard.Update(d.now, pointers));
  };
  const Pose on = AimAtKey(d.keyboard, "g");
  EXPECT_TRUE(stick(on, 0.f).empty());
  EXPECT_TRUE(d.keyboard.CapturesThumbstick(1));
  EXPECT_FALSE(d.keyboard.CapturesThumbstick(0));
  EXPECT_EQ(stick(on, 0.9f), std::vector<std::uint32_t>{kRight});
  EXPECT_TRUE(stick(on, 0.9f, 200 * kMs).empty());
  EXPECT_EQ(stick(on, 0.9f, 250 * kMs), std::vector<std::uint32_t>{kRight});
  EXPECT_EQ(stick(on, 0.9f, 90 * kMs), std::vector<std::uint32_t>{kRight});
  // Easing into the dead zone's edge keeps the push; letting go re-arms.
  EXPECT_TRUE(stick(on, 0.4f).empty());
  EXPECT_TRUE(stick(on, 0.f).empty());
  EXPECT_EQ(stick(on, -0.8f), std::vector<std::uint32_t>{kLeft});
  // Aimed away, the stick belongs to Roblox.
  Pose away = on;
  away.orientation = QuatFromAxisAngle(Vec3{0.f, 1.f, 0.f}, pose::kPi);
  EXPECT_TRUE(stick(away, 0.9f).empty());
  EXPECT_FALSE(d.keyboard.CapturesThumbstick(1));
}

TEST(VirtualKeyboardTest, LongPressOpensAccentsAndReleaseTypesTheChoice) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const Pose e = AimAtKey(d.keyboard, "e");
  d.Frame(e, 1.f);
  EXPECT_EQ(d.keyboard.accents().hand, -1);
  const auto opened = d.Frame(e, 1.f, 1, 500 * kMs);
  ASSERT_EQ(d.keyboard.accents().hand, 1);
  EXPECT_TRUE(opened.haptic[1]);
  ASSERT_GE(d.keyboard.accents().cells.size(), 3u);
  EXPECT_EQ(d.keyboard.accents().cells[1].text, "é");
  // Resting on the key keeps the first accent; release types it.
  EXPECT_EQ(d.keyboard.accents().selected, 0);
  EXPECT_EQ(d.Frame(e, 0.f).text, std::vector<std::string>{"è"});
  EXPECT_EQ(d.keyboard.accents().hand, -1);

  // Slide onto "é" and release there; under Shift the row is upper case.
  d.Tap("Shift");
  const Pose upper_e = AimAtKey(d.keyboard, "E");
  d.Frame(upper_e, 1.f);
  d.Frame(upper_e, 1.f, 1, 500 * kMs);
  const VirtualKey cell = d.keyboard.accents().cells[1];
  EXPECT_EQ(cell.text, "É");
  EXPECT_LT(cell.v1, d.keyboard.keys()[FindKey(d.keyboard, "E")].v0 + 1e-6f);
  const Pose on_cell =
      AimAt(d.keyboard, (cell.u0 + cell.u1) / 2, (cell.v0 + cell.v1) / 2);
  d.Frame(on_cell, 1.f);
  EXPECT_EQ(d.keyboard.accents().selected, 1);
  EXPECT_EQ(d.Frame(on_cell, 0.f).text, std::vector<std::string>{"É"});
  EXPECT_FALSE(d.keyboard.shifted());

  // Symbol keys follow what they type: Shift+4 offers currency signs.
  d.Tap("Shift");
  const Pose dollar = AimAtKey(d.keyboard, "$");
  d.Frame(dollar, 1.f);
  d.Frame(dollar, 1.f, 1, 500 * kMs);
  ASSERT_EQ(d.keyboard.accents().hand, 1);
  EXPECT_EQ(d.Frame(dollar, 0.f).text, std::vector<std::string>{"€"});

  // Released away from the row and the key: nothing.
  d.Frame(e, 1.f);
  d.Frame(e, 1.f, 1, 500 * kMs);
  ASSERT_EQ(d.keyboard.accents().hand, 1);
  const Pose elsewhere = AimAtKey(d.keyboard, "m");
  d.Frame(elsewhere, 1.f);
  EXPECT_EQ(d.keyboard.accents().selected, -1);
  EXPECT_TRUE(d.Frame(elsewhere, 0.f).text.empty());
}

TEST(VirtualKeyboardTest, DragBarMovesThePanelAndTurnsItToTheHead) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const Pose start = d.keyboard.panel_pose();
  const Pose handle = AimAt(d.keyboard, 0.5f, 0.95f);
  d.Frame(handle, 0.f);
  EXPECT_TRUE(d.keyboard.handle_hovered());
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));
  EXPECT_EQ(d.keyboard.hovered_key(1), -1);
  EXPECT_TRUE(d.Frame(handle, 1.f).haptic[1]);
  EXPECT_TRUE(d.keyboard.dragging());
  // Without a head pose the panel only follows the hand.
  Pose moved = handle;
  moved.position.x += 0.1f;
  moved.position.y += 0.05f;
  const auto out = d.Frame(moved, 1.f);
  EXPECT_TRUE(out.text.empty() && out.keys.empty());
  EXPECT_NEAR(d.keyboard.panel_pose().position.x, start.position.x + 0.1f, 1e-4f);
  EXPECT_NEAR(d.keyboard.panel_pose().position.y, start.position.y + 0.05f, 1e-4f);
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));
  d.Frame(moved, 0.f);
  EXPECT_FALSE(d.keyboard.dragging());

  // With one, the dragged panel faces it.
  const Pose head = HeadAt(0.f, 1.6f, 0.f);
  d.keyboard.SetHead(head);
  const Pose grab = AimAt(d.keyboard, 0.5f, 0.95f);
  d.Frame(grab, 1.f);
  Pose aside = grab;
  aside.position.x += 0.3f;
  d.Frame(aside, 1.f);
  const Pose& panel = d.keyboard.panel_pose();
  const Vec3 normal = QuatRotate(panel.orientation, Vec3{0.f, 0.f, 1.f});
  Vec3 to_head{head.position.x - panel.position.x,
               head.position.y - panel.position.y,
               head.position.z - panel.position.z};
  const float length = std::sqrt(to_head.x * to_head.x + to_head.y * to_head.y +
                                 to_head.z * to_head.z);
  EXPECT_GT((normal.x * to_head.x + normal.y * to_head.y +
             normal.z * to_head.z) / length, 0.99f);
  d.Frame(aside, 0.f);
}

TEST(VirtualKeyboardTest, CapturesTriggerOnPanelAndUntilARelease) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  Pose away = AimAt(d.keyboard, 0.5f, 0.5f);
  away.orientation = QuatFromAxisAngle(Vec3{0.f, 1.f, 0.f}, pose::kPi);
  d.Frame(away, 0.f);
  EXPECT_FALSE(d.keyboard.CapturesTrigger(1));
  const Pose on = AimAtKey(d.keyboard, "g");
  d.Frame(on, 0.f);
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));
  EXPECT_FALSE(d.keyboard.CapturesTrigger(0));
  d.Frame(on, 1.f);
  d.Frame(away, 1.f);  // dragged off while held: still the keyboard's
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));
  d.Frame(away, 0.f);
  EXPECT_FALSE(d.keyboard.CapturesTrigger(1));
  // A press that starts off the panel is never captured.
  d.Frame(away, 1.f);
  d.Frame(on, 1.f);
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));  // aimed at it now
  d.Frame(away, 1.f);
  EXPECT_FALSE(d.keyboard.CapturesTrigger(1));
  // The see-through strip beside the drag bar is not the panel.
  d.Frame(AimAt(d.keyboard, 0.1f, 0.95f), 0.f);
  EXPECT_FALSE(d.keyboard.CapturesTrigger(1));
}

TEST(VirtualKeyboardTest, LayoutRowsFillThePanelAndCloseKeyHides) {
  VirtualKeyboard keyboard;
  // Five rows; every row starts at the left edge and ends at the right edge.
  int rows = 0;
  int ends = 0;
  for (const VirtualKey& key : keyboard.keys()) {
    if (key.u0 == 0.f) ++rows;
    if (std::abs(key.u1 - 1.f) < 1e-5f) ++ends;
    EXPECT_LT(key.u0, key.u1);
    EXPECT_LE(key.u1, 1.f);
    EXPECT_GE(key.v0, VirtualKeyboard::kFieldFraction - 1e-6f) << key.label;
    EXPECT_LE(key.v1, VirtualKeyboard::kBodyFraction + 1e-6f) << key.label;
  }
  EXPECT_EQ(rows, 5);
  EXPECT_EQ(ends, 5);
  for (const char* label : {"`", "=", "Backspace", "Tab", "[", "]", "\\",
                            "Caps", ";", "'", "Enter", "Shift", "/", "Space",
                            "Close", "Left", "Right", "Select all", "Paste"}) {
    EXPECT_GE(FindKey(keyboard, label), 0) << label;
  }
  for (const char* label : {"Esc", "F1", "Ctrl", "Alt", "Menu"}) {
    EXPECT_EQ(FindKey(keyboard, label), -1) << label;
  }

  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const auto closed = d.Frame(AimAtKey(d.keyboard, "Close"), 1.f);
  EXPECT_EQ(Scancodes(closed), std::vector<std::uint32_t>{kEscape});
  EXPECT_FALSE(d.keyboard.visible());
  EXPECT_TRUE(d.Frame(AimAtKey(d.keyboard, "q"), 0.f).text.empty());
}

TEST(VirtualKeyboardTest, TriggerStaysWithheldAfterCloseUntilReleased) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const Pose close = AimAtKey(d.keyboard, "Close");
  d.Frame(close, 1.f);
  ASSERT_FALSE(d.keyboard.visible());
  // The trigger that pressed Close is still down; Roblox must not see it as
  // a click on whatever is behind the panel.
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));
  EXPECT_FALSE(d.keyboard.CapturesTrigger(0));
  d.Frame(close, 1.f);
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));
  d.Frame(close, 0.f);
  EXPECT_FALSE(d.keyboard.CapturesTrigger(1));
  d.Frame(close, 1.f);  // a new press belongs to Roblox
  EXPECT_FALSE(d.keyboard.CapturesTrigger(1));
}

TEST(VirtualKeyboardTest, TriggerStaysWithheldWhenThePanelHidesMidPress) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const Pose enter = AimAtKey(d.keyboard, "Enter");
  d.Frame(enter, 1.f);
  d.keyboard.Hide();  // Enter made the TextBox release focus
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));
  d.Frame(enter, 0.f);
  EXPECT_FALSE(d.keyboard.CapturesTrigger(1));
  // A hand that was only hovering is free at once.
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  d.Frame(enter, 0.f);
  d.keyboard.Hide();
  EXPECT_FALSE(d.keyboard.CapturesTrigger(1));
}

TEST(VirtualKeyboardTest, HeldTriggerTypesNothingWhenThePanelReopens) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  d.Frame(AimAtKey(d.keyboard, "Close"), 1.f);
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const Pose q = AimAtKey(d.keyboard, "q");
  EXPECT_TRUE(d.Frame(q, 1.f).text.empty());
  EXPECT_TRUE(d.Frame(q, 0.f).text.empty());
  d.Frame(q, 1.f);
  EXPECT_EQ(d.Frame(q, 0.f).text, std::vector<std::string>{"q"});
}

TEST(VirtualKeyboardTest, CursorMovesRedrawAtMostThirtyTimesASecond) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const VirtualKey space = d.keyboard.keys()[FindKey(d.keyboard, "Space")];
  const float v = (space.v0 + space.v1) / 2;
  float u = space.u0 + 0.005f;
  d.Frame(AimAt(d.keyboard, u, v), 0.f);
  const std::uint64_t before = d.keyboard.revision();
  // 30 frames at 90 Hz sliding along one key: 330 ms of pointer motion.
  for (int frame = 0; frame < 30; ++frame) {
    u += 0.01f;
    d.Frame(AimAt(d.keyboard, u, v), 0.f);
  }
  const std::uint64_t redraws = d.keyboard.revision() - before;
  EXPECT_LE(redraws, 11u);
  EXPECT_GE(redraws, 5u);
  // Once the pointer rests, the drawn cursor catches up with it.
  d.Frame(AimAt(d.keyboard, u, v), 0.f, 1, 40 * kMs);
  float drawn_u = 0.f, drawn_v = 0.f;
  ASSERT_TRUE(d.keyboard.cursor(1, &drawn_u, &drawn_v));
  EXPECT_NEAR(drawn_u, u, 1e-3f);
}

TEST(VirtualKeyboardTest, BothCursorsKeepUpWhileBothHandsMove) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const VirtualKey space = d.keyboard.keys()[FindKey(d.keyboard, "Space")];
  const float v = (space.v0 + space.v1) / 2;
  float left_u = space.u0 + 0.005f, right_u = space.u0 + 0.15f;
  auto frame = [&] {
    VirtualKeyboardPointer pointers[2];
    pointers[0].valid = pointers[1].valid = true;
    pointers[0].aim = AimAt(d.keyboard, left_u, v);
    pointers[1].aim = AimAt(d.keyboard, right_u, v);
    d.now += 11 * kMs;
    d.keyboard.Update(d.now, pointers);
  };
  frame();
  // 30 frames at 90 Hz, both hands sliding along one key.
  for (int step = 0; step < 30; ++step) {
    left_u += 0.004f;
    right_u += 0.004f;
    frame();
  }
  // Neither drawn cursor trails its hand by more than one redraw interval.
  float drawn_u = 0.f, drawn_v = 0.f;
  ASSERT_TRUE(d.keyboard.cursor(0, &drawn_u, &drawn_v));
  EXPECT_NEAR(drawn_u, left_u, 0.02f);
  ASSERT_TRUE(d.keyboard.cursor(1, &drawn_u, &drawn_v));
  EXPECT_NEAR(drawn_u, right_u, 0.02f);
}

TEST(VirtualKeyboardTest, HeldTriggerStaysWithheldWhenAReopenedPanelHides) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const Pose close = AimAtKey(d.keyboard, "Close");
  d.Frame(close, 1.f);
  // The panel reopens and hides again during the same trigger press.
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  d.Frame(close, 1.f);
  d.keyboard.Hide();
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));
  d.Frame(close, 1.f);
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));
  d.Frame(close, 0.f);
  EXPECT_FALSE(d.keyboard.CapturesTrigger(1));
}

TEST(VirtualKeyboardTest, HeldTriggerStaysWithheldWhileAimTrackingDrops) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  d.Frame(AimAtKey(d.keyboard, "Close"), 1.f);
  ASSERT_TRUE(d.keyboard.CapturesTrigger(1));
  // The aim pose is lost for a frame; the trigger is still down.
  VirtualKeyboardPointer lost[2];
  lost[1].trigger = 1.f;
  d.now += 11 * kMs;
  d.keyboard.Update(d.now, lost);
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));
  lost[1].trigger = 0.f;
  d.now += 11 * kMs;
  d.keyboard.Update(d.now, lost);
  EXPECT_FALSE(d.keyboard.CapturesTrigger(1));
}

TEST(VirtualKeyboardTest, RevisionMovesOnlyWhenTheImageChanges) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const Pose aim = AimAtKey(d.keyboard, "h");
  d.Frame(aim, 0.f);
  const std::uint64_t hovered = d.keyboard.revision();
  d.Frame(aim, 0.f);
  EXPECT_EQ(d.keyboard.revision(), hovered);
  d.Frame(AimAtKey(d.keyboard, "j"), 0.f);
  EXPECT_GT(d.keyboard.revision(), hovered);
}

TEST(VirtualKeyboardTest, TextFieldSitsAboveTheKeysAndTypesNothing) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  for (const VirtualKey& key : d.keyboard.keys())
    EXPECT_GE(key.v0, VirtualKeyboard::kFieldFraction - 1e-6f) << key.label;
  const Pose field = AimAt(d.keyboard, 0.5f, VirtualKeyboard::kFieldFraction / 2);
  d.Frame(field, 0.f);
  EXPECT_EQ(d.keyboard.hovered_key(1), -1);
  EXPECT_TRUE(d.keyboard.CapturesTrigger(1));  // still the keyboard's
  const auto out = d.Frame(field, 1.f);
  EXPECT_TRUE(out.text.empty());
  EXPECT_TRUE(out.keys.empty());
}

TEST(VirtualKeyboardTest, TextMirrorFollowsTheFocusedTextBox) {
  ClearVirtualKeyboardText();
  SetVirtualKeyboardText("hi", 2, 2, false);
  VirtualKeyboardText text = CurrentVirtualKeyboardText();
  EXPECT_EQ(text.utf8, "hi");
  EXPECT_EQ(text.cursor_utf16, 2);

  VirtualKeyboard keyboard;
  keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const std::uint64_t before = keyboard.revision();
  keyboard.SetText(text);
  EXPECT_EQ(keyboard.text(), "hi");
  EXPECT_GT(keyboard.revision(), before);
  // Same mirror revision: nothing to redraw.
  const std::uint64_t same = keyboard.revision();
  keyboard.SetText(text);
  EXPECT_EQ(keyboard.revision(), same);
  // Cursor moves alone still redraw.
  SetVirtualKeyboardText("hi", 2, 1, false);
  keyboard.SetText(CurrentVirtualKeyboardText());
  EXPECT_EQ(keyboard.text_cursor_utf16(), 1);
  EXPECT_GT(keyboard.revision(), same);

  // Hiding forgets the text; reopening reloads it even without a new update.
  keyboard.Hide();
  EXPECT_TRUE(keyboard.text().empty());
  keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  keyboard.SetText(CurrentVirtualKeyboardText());
  EXPECT_EQ(keyboard.text(), "hi");

  ClearVirtualKeyboardText();
  EXPECT_TRUE(CurrentVirtualKeyboardText().utf8.empty());
}

}  // namespace
}  // namespace mocktail::vr
