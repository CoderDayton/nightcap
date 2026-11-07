#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mocktail/vr/virtual_keyboard.h"

namespace mocktail::vr {
namespace {

using pose::Pose;
using pose::Vec3;

Pose Head() {
  Pose head;
  head.position = Vec3{0.f, 1.6f, 0.f};
  return head;
}

int FindKey(const VirtualKeyboard& keyboard, const std::string& label) {
  const auto& keys = keyboard.keys();
  for (std::size_t i = 0; i < keys.size(); ++i)
    if (keys[i].label == label) return static_cast<int>(i);
  return -1;
}

void SetText(VirtualKeyboard* keyboard, const std::string& utf8, bool password,
             std::uint64_t revision) {
  VirtualKeyboardText text;
  text.utf8 = utf8;
  text.cursor_utf16 = static_cast<int>(utf8.size());
  text.password = password;
  text.revision = revision;
  keyboard->SetText(text);
}

TEST(VirtualKeyboardRasterTest, DrawsFieldTextAndCaret) {
  VirtualKeyboard keyboard;
  keyboard.Show(Head());
  VirtualKeyboardRasterizer rasterizer;
  auto field_light_pixels = [&](const std::vector<std::uint8_t>& rgba) {
    const int bottom = static_cast<int>(VirtualKeyboard::kFieldFraction *
                                        VirtualKeyboard::kTextureHeight);
    int count = 0;
    for (int y = 8; y < bottom - 8; ++y)
      for (int x = 30; x < VirtualKeyboard::kTextureWidth - 30; ++x)
        if (rgba[(static_cast<std::size_t>(y) *
                      VirtualKeyboard::kTextureWidth + x) * 4 + 1] > 120)
          ++count;
    return count;
  };
  std::vector<std::uint8_t> empty, typed, masked;
  if (!rasterizer.Draw(keyboard, &empty)) GTEST_SKIP() << "no host font";
  SetText(&keyboard, "Hello world", false, 1);
  ASSERT_TRUE(rasterizer.Draw(keyboard, &typed));
  EXPECT_GT(field_light_pixels(typed), field_light_pixels(empty) + 200);
  SetText(&keyboard, "Hello world", true, 2);
  ASSERT_TRUE(rasterizer.Draw(keyboard, &masked));
  EXPECT_NE(masked, typed);  // dots, not the letters
  EXPECT_GT(field_light_pixels(masked), field_light_pixels(empty));
}

TEST(VirtualKeyboardRasterTest, PasswordPictureDoesNotDependOnItsLetters) {
  VirtualKeyboard keyboard;
  keyboard.Show(Head());
  VirtualKeyboardRasterizer rasterizer;
  std::vector<std::uint8_t> first, second;
  SetText(&keyboard, "Hello world", true, 1);
  if (!rasterizer.Draw(keyboard, &first)) GTEST_SKIP() << "no host font";
  SetText(&keyboard, "Jumpy vixen", true, 2);
  ASSERT_TRUE(rasterizer.Draw(keyboard, &second));
  EXPECT_EQ(first, second);
}

TEST(VirtualKeyboardRasterTest, RoundsThePanelAndKeepsKeysOpaque) {
  VirtualKeyboard keyboard;
  keyboard.Show(Head());
  VirtualKeyboardRasterizer rasterizer;
  std::vector<std::uint8_t> rgba;
  (void)rasterizer.Draw(keyboard, &rgba);  // labels depend on host fonts
  constexpr int width = VirtualKeyboard::kTextureWidth;
  constexpr int height = VirtualKeyboard::kTextureHeight;
  ASSERT_EQ(rgba.size(), static_cast<std::size_t>(width) * height * 4);
  auto at = [&](int x, int y) {
    return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
  };
  // The corners are cut away; the panel lets a little of the game through.
  EXPECT_EQ(at(0, 0)[3], 0);
  EXPECT_EQ(at(width - 1, height - 1)[3], 0);
  const int body_bottom =
      static_cast<int>(VirtualKeyboard::kBodyFraction * height);
  const std::uint8_t* panel = at(width / 2, body_bottom - 2);
  EXPECT_GT(panel[3], 200);
  EXPECT_LT(panel[3], 255);
  // Below the body only the drag bar is drawn.
  const int strip_middle = (body_bottom + height) / 2;
  EXPECT_EQ(at(width / 10, strip_middle)[3], 0);
  EXPECT_GT(at(width / 2, strip_middle)[3], 100);
  // A key is opaque and lighter than the panel behind it.
  const auto& q = keyboard.keys()[FindKey(keyboard, "q")];
  const std::uint8_t* key =
      at(static_cast<int>(q.u0 * width) + 20, static_cast<int>(q.v0 * height) + 20);
  EXPECT_EQ(key[3], 255);
  EXPECT_GT(key[0], panel[0]);
}

TEST(VirtualKeyboardRasterTest, HoverDrawsTheSelectionRing) {
  VirtualKeyboard keyboard;
  keyboard.Show(Head());
  VirtualKeyboardRasterizer rasterizer;
  std::vector<std::uint8_t> idle, hovered;
  (void)rasterizer.Draw(keyboard, &idle);
  const int g = FindKey(keyboard, "g");
  const VirtualKey& key = keyboard.keys()[g];
  // Aim straight at the key's centre from in front of the panel.
  const Pose& panel = keyboard.panel_pose();
  const float u = (key.u0 + key.u1) / 2.f;
  const float v = (key.v0 + key.v1) / 2.f;
  const Vec3 offset = pose::QuatRotate(
      panel.orientation,
      Vec3{(u - 0.5f) * VirtualKeyboard::kPanelWidthMetres,
           (0.5f - v) * VirtualKeyboard::kPanelHeightMetres, 0.3f});
  VirtualKeyboardPointer pointers[2];
  pointers[0].valid = true;
  pointers[0].aim.orientation = panel.orientation;
  pointers[0].aim.position = Vec3{panel.position.x + offset.x,
                                  panel.position.y + offset.y,
                                  panel.position.z + offset.z};
  (void)keyboard.Update(1'000'000'000ULL, pointers);
  ASSERT_EQ(keyboard.hovered_key(0), g);
  (void)rasterizer.Draw(keyboard, &hovered);
  // Just outside the key's left edge, halfway down: white ring.
  constexpr int width = VirtualKeyboard::kTextureWidth;
  const int x = static_cast<int>(key.u0 * width) + 5 - 3;
  const int y = static_cast<int>(v * VirtualKeyboard::kTextureHeight);
  const std::size_t px = (static_cast<std::size_t>(y) * width + x) * 4;
  EXPECT_GT(hovered[px], 240);
  EXPECT_LT(idle[px], 60);
}

}  // namespace
}  // namespace mocktail::vr
