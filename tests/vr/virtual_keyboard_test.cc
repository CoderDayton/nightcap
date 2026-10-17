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
    out.scancodes.insert(out.scancodes.end(), up.scancodes.begin(),
                         up.scancodes.end());
    return out;
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
  const auto pressed = d.Frame(aim, 0.9f);
  ASSERT_EQ(pressed.text.size(), 1u);
  EXPECT_EQ(pressed.text[0], "q");
  EXPECT_EQ(d.keyboard.pressed_key(1), FindKey(d.keyboard, "q"));
  // Held, and hovering between the hysteresis thresholds: no repeat for text.
  EXPECT_TRUE(d.Frame(aim, 0.7f, 1, 600 * kMs).text.empty());
  EXPECT_TRUE(d.Frame(aim, 0.f).text.empty());
  EXPECT_EQ(d.keyboard.pressed_key(1), -1);
  EXPECT_EQ(d.Tap("Enter").scancodes, std::vector<std::uint32_t>{kReturn});
  EXPECT_EQ(d.Tap("Space").text, std::vector<std::string>{" "});
  EXPECT_EQ(d.Tap("Tab").scancodes, std::vector<std::uint32_t>{kTab});
}

TEST(VirtualKeyboardTest, ShiftIsOneShotAndSelectsUpperSymbols) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  d.Tap("Shift");
  EXPECT_TRUE(d.keyboard.shifted());
  EXPECT_GE(FindKey(d.keyboard, "!"), 0);  // labels follow Shift
  EXPECT_EQ(d.Tap("A").text, std::vector<std::string>{"A"});
  EXPECT_FALSE(d.keyboard.shifted());
  EXPECT_EQ(d.Tap("b").text, std::vector<std::string>{"b"});
  d.Tap("Shift");
  EXPECT_EQ(d.Tap("!").text, std::vector<std::string>{"!"});
  EXPECT_EQ(d.Tap("1").text, std::vector<std::string>{"1"});
  d.Tap("Shift");
  EXPECT_EQ(d.Tap("|").text, std::vector<std::string>{"|"});
  EXPECT_EQ(d.Tap("\\").text, std::vector<std::string>{"\\"});
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
    const VirtualKey key = d.keyboard.keys()[index];
    const Pose aim =
        AimAt(d.keyboard, (key.u0 + key.u1) / 2, (key.v0 + key.v1) / 2);

    // Unshifted press types the plain character.
    d.Frame(aim, 0.f);
    EXPECT_EQ(d.Frame(aim, 1.f).text, std::vector<std::string>{plain});
    d.Frame(aim, 0.f);

    // Shift relabels this key, the press types the shifted character, and
    // the one-shot Shift then puts the plain label back.
    d.Tap("Shift");
    ASSERT_TRUE(d.keyboard.shifted());
    EXPECT_EQ(d.keyboard.keys()[index].label, shifted);
    d.Frame(aim, 0.f);
    EXPECT_EQ(d.Frame(aim, 1.f).text, std::vector<std::string>{shifted});
    d.Frame(aim, 0.f);
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
    const VirtualKey key = d.keyboard.keys()[index];
    const Pose aim =
        AimAt(d.keyboard, (key.u0 + key.u1) / 2, (key.v0 + key.v1) / 2);
    d.Frame(aim, 0.f);
    EXPECT_EQ(d.Frame(aim, 1.f).text, std::vector<std::string>{caps});
    d.Frame(aim, 0.f);
    // Caps Lock + Shift: letters back to lower, symbols shifted.
    d.Tap("Shift");
    const std::string both = letter ? plain : shifted;
    EXPECT_EQ(d.keyboard.keys()[index].label, both);
    d.Frame(aim, 0.f);
    EXPECT_EQ(d.Frame(aim, 1.f).text, std::vector<std::string>{both});
    d.Frame(aim, 0.f);
    EXPECT_TRUE(d.keyboard.caps_lock());
  }
}

TEST(VirtualKeyboardTest, CapsLockAffectsLettersOnlyAndShiftInvertsIt) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  d.Tap("Caps");
  EXPECT_TRUE(d.keyboard.caps_lock());
  EXPECT_EQ(d.Tap("C").text, std::vector<std::string>{"C"});
  EXPECT_EQ(d.Tap("D").text, std::vector<std::string>{"D"});
  EXPECT_EQ(d.Tap("1").text, std::vector<std::string>{"1"});
  d.Tap("Shift");
  EXPECT_EQ(d.Tap("e").text, std::vector<std::string>{"e"});
  EXPECT_TRUE(d.keyboard.caps_lock());
  d.Tap("Caps");
  EXPECT_EQ(d.Tap("f").text, std::vector<std::string>{"f"});
}

TEST(VirtualKeyboardTest, BackspaceRepeatsWhileHeld) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const Pose aim = AimAtKey(d.keyboard, "Backspace");
  EXPECT_EQ(d.Frame(aim, 1.f).scancodes,
            std::vector<std::uint32_t>{kBackspace});
  EXPECT_TRUE(d.Frame(aim, 1.f, 1, 300 * kMs).scancodes.empty());
  EXPECT_EQ(d.Frame(aim, 1.f, 1, 200 * kMs).scancodes.size(), 1u);
  EXPECT_TRUE(d.Frame(aim, 1.f, 1, 30 * kMs).scancodes.empty());
  EXPECT_EQ(d.Frame(aim, 1.f, 1, 40 * kMs).scancodes.size(), 1u);
  // Sliding off the key stops the repeat.
  const Pose off = AimAtKey(d.keyboard, "p");
  EXPECT_TRUE(d.Frame(off, 1.f, 1, 500 * kMs).scancodes.empty());
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
}

TEST(VirtualKeyboardTest, LayoutRowsFillThePanelAndCloseKeyHides) {
  VirtualKeyboard keyboard;
  // Five rows; every row starts at the left edge and ends at the right edge.
  int rows = 0;
  for (const VirtualKey& key : keyboard.keys()) {
    if (key.u0 == 0.f) ++rows;
  }
  EXPECT_EQ(rows, 5);
  for (const VirtualKey& key : keyboard.keys()) {
    EXPECT_LT(key.u0, key.u1);
    EXPECT_LE(key.u1, 1.f);
  }
  for (const char* label : {"`", "=", "Backspace", "Tab", "[", "]", "\\",
                            "Caps", ";", "'", "Enter", "Shift", "/", "Space",
                            "Close"}) {
    EXPECT_GE(FindKey(keyboard, label), 0) << label;
  }
  for (const char* label : {"Esc", "F1", "Ctrl", "Alt", "Menu"}) {
    EXPECT_EQ(FindKey(keyboard, label), -1) << label;
  }

  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const auto closed = d.Frame(AimAtKey(d.keyboard, "Close"), 1.f);
  EXPECT_EQ(closed.scancodes, std::vector<std::uint32_t>{kEscape});
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
  d.Frame(q, 0.f);
  EXPECT_EQ(d.Frame(q, 1.f).text, std::vector<std::string>{"q"});
}

TEST(VirtualKeyboardTest, CursorMovesRedrawAtMostThirtyTimesASecond) {
  Driver d;
  d.keyboard.Show(HeadAt(0.f, 1.6f, 0.f));
  const VirtualKey space = d.keyboard.keys()[FindKey(d.keyboard, "Space")];
  const float v = (space.v0 + space.v1) / 2;
  float u = 0.10f;
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
  float left_u = 0.10f, right_u = 0.50f;
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
    left_u += 0.005f;
    right_u += 0.005f;
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
  EXPECT_TRUE(out.scancodes.empty());
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
