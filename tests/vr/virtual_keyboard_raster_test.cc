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

TEST(VirtualKeyboardRasterTest, FillsAnOpaqueTexture) {
  VirtualKeyboard keyboard;
  keyboard.Show(Head());
  VirtualKeyboardRasterizer rasterizer;
  std::vector<std::uint8_t> rgba;
  (void)rasterizer.Draw(keyboard, &rgba);  // labels depend on host fonts
  ASSERT_EQ(rgba.size(), static_cast<std::size_t>(
                             VirtualKeyboard::kTextureWidth) *
                             VirtualKeyboard::kTextureHeight * 4);
  for (std::size_t i = 3; i < rgba.size(); i += 4) ASSERT_EQ(rgba[i], 255);
  // A key is lighter than the background behind it.
  const auto& q = keyboard.keys()[FindKey(keyboard, "q")];
  const int x = static_cast<int>(q.u0 * VirtualKeyboard::kTextureWidth) + 20;
  const int y = static_cast<int>(q.v0 * VirtualKeyboard::kTextureHeight) + 20;
  const std::size_t key_px =
      (static_cast<std::size_t>(y) * VirtualKeyboard::kTextureWidth + x) * 4;
  EXPECT_GT(rgba[key_px], rgba[0]);
}

}  // namespace
}  // namespace mocktail::vr
