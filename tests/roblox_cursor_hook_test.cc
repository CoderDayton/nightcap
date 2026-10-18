#include "runtime/roblox_cursor_hook.h"

#include <gtest/gtest.h>
#include <png.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using mocktail::runtime::ClassifyRobloxCursorContent;
using mocktail::runtime::CursorHookSegment;
using mocktail::runtime::DecodeCustomCursor;
using mocktail::runtime::IsHardwareCursorContent;
using mocktail::runtime::CursorRenderSite;
using mocktail::runtime::IsStockSystemCursorAsset;
using mocktail::runtime::LocateCursorRenderSite;
using mocktail::runtime::RobloxCursorKind;
using mocktail::runtime::StockSystemCursorAssetNames;
using mocktail::runtime::TransparentCursorPng;

TEST(RobloxCursorHookTest, ClassifiesStockCursors) {
  EXPECT_EQ(ClassifyRobloxCursorContent(
                "rbxasset://textures/Cursors/KeyboardMouse/ArrowFarCursor.png"),
            RobloxCursorKind::kArrow);
  EXPECT_EQ(ClassifyRobloxCursorContent(
                "rbxasset://textures/Cursors/KeyboardMouse/ArrowCursor.png"),
            RobloxCursorKind::kPointer);
  EXPECT_EQ(ClassifyRobloxCursorContent(
                "rbxasset://textures/Cursors/KeyboardMouse/IBeamCursor.png"),
            RobloxCursorKind::kText);
  EXPECT_EQ(ClassifyRobloxCursorContent("rbxasset://textures/ArrowCursor.png"),
            RobloxCursorKind::kPointer);
  // A download name the asset overlay unmapped.
  EXPECT_EQ(ClassifyRobloxCursorContent("!bxasset://textures/IBeamCursor.png"),
            RobloxCursorKind::kText);
  EXPECT_EQ(ClassifyRobloxCursorContent("rbxasset://SystemCursors/PointingHand"),
            RobloxCursorKind::kPointer);
  EXPECT_EQ(ClassifyRobloxCursorContent("SystemCursors/IBeam"),
            RobloxCursorKind::kText);
}

TEST(RobloxCursorHookTest, LeavesOtherCursorsToRoblox) {
  for (const char* content : {
           "",
           "rbxassetid://1234567",
           "http://www.roblox.com/asset/?id=1234567",
           "rbxasset://textures/MouseLockedCursor.png",
           "rbxasset://textures/Cursors/Gamepad/Pointer.png",
           "rbxasset://textures/Cursors/KeyboardMouse/IBeamCursor.png.bak",
           "rbxasset://other/textures/ArrowCursor.png",
           "rbxassetid://textures/ArrowCursor.png",
       }) {
    EXPECT_EQ(ClassifyRobloxCursorContent(content), RobloxCursorKind::kOther)
        << content;
  }
}

TEST(RobloxCursorHookTest, MatchesStockCursorAssetPaths) {
  EXPECT_TRUE(IsStockSystemCursorAsset(
      "android/textures/Cursors/KeyboardMouse/ArrowFarCursor.png"));
  EXPECT_TRUE(IsStockSystemCursorAsset("android/textures/ArrowCursor.png"));
  EXPECT_TRUE(IsStockSystemCursorAsset(
      "rbxasset://textures/Cursors/KeyboardMouse/IBeamCursor.png"));
  EXPECT_TRUE(IsStockSystemCursorAsset("textures/IBeamCursor.png"));
  EXPECT_FALSE(IsStockSystemCursorAsset("android/textures/MouseLockedCursor.png"));
  EXPECT_FALSE(IsStockSystemCursorAsset(
      "android/textures/Cursors/Gamepad/IBeamCursor.png"));
  EXPECT_FALSE(IsStockSystemCursorAsset("android/mytextures/ArrowCursor.png"));
}

TEST(RobloxCursorHookTest, NamesEveryStockCursor) {
  const std::vector<std::string> names = StockSystemCursorAssetNames();
  EXPECT_EQ(names.size(), 6U);
  for (const std::string& name : names) {
    EXPECT_EQ(name.rfind("rbxasset://textures/", 0), 0U) << name;
    EXPECT_TRUE(IsStockSystemCursorAsset(name)) << name;
    EXPECT_NE(ClassifyRobloxCursorContent(name), RobloxCursorKind::kOther)
        << name;
  }
}

TEST(RobloxCursorHookTest, TransparentCursorIsAPng) {
  std::size_t size = 0;
  const std::uint8_t* png = TransparentCursorPng(&size);
  ASSERT_GT(size, 8U);
  EXPECT_EQ(std::memcmp(png, "\x89PNG\r\n\x1a\n", 8), 0);
}

// A data segment with the anchor message and a code segment laid out like the
// draw function in Roblox 2.3222.
class DrawImage {
 public:
  static constexpr std::uintptr_t kDataAddress = 0x200000;
  static constexpr std::uintptr_t kCodeAddress = 0x400000;
  static constexpr std::size_t kFunction = 0x40;
  static constexpr std::size_t kAnchorLoad = kFunction + 0x997;
  static constexpr std::size_t kDrawSite = kFunction + 0x141b;
  static constexpr std::size_t kCallee = 0x3000;
  static constexpr std::size_t kLimitRead = kDrawSite + 0x264;
  static constexpr std::uintptr_t kLimitFlag = 0x80;

  DrawImage() : data_(0x100, 0), code_(0x4000, 0xCC) {
    const std::string message = "[FLog::CursorCache] Invalid asset ID: %s";
    std::memcpy(data_.data() + kMessage, message.data(), message.size());
    const std::uint8_t prologue[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57,
                                     0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                     0x53, 0x48, 0x81, 0xEC, 0xC8, 0x01,
                                     0x00, 0x00};
    std::memcpy(code_.data() + kFunction, prologue, sizeof(prologue));
    WriteAnchorLoad(kAnchorLoad);
    WriteDrawSite(kDrawSite);
    WriteLimitRead(kLimitRead);
  }

  // lea rsi,[rip+message]
  void WriteAnchorLoad(std::size_t at) {
    code_[at] = 0x48;
    code_[at + 1] = 0x8D;
    code_[at + 2] = 0x35;
    WriteRel32(at + 3, kCodeAddress + at + 7, kDataAddress + kMessage);
  }

  // mov rdi,[rbp-0x1e0]; call callee; cmp ax,2; je +0x551
  void WriteDrawSite(std::size_t at) {
    const std::uint8_t load[] = {0x48, 0x8B, 0xBD, 0x20, 0xFE, 0xFF, 0xFF};
    std::memcpy(code_.data() + at, load, sizeof(load));
    code_[at + 7] = 0xE8;
    WriteRel32(at + 8, kCodeAddress + at + 12, kCodeAddress + kCallee);
    const std::uint8_t check[] = {0x66, 0x83, 0xF8, 0x02, 0x0F, 0x84,
                                  0x51, 0x05, 0x00, 0x00};
    std::memcpy(code_.data() + at + 12, check, sizeof(check));
  }

  // cvtsi2ss xmm2,[rip+limit]
  void WriteLimitRead(std::size_t at) {
    const std::uint8_t load[] = {0xF3, 0x0F, 0x2A, 0x15};
    std::memcpy(code_.data() + at, load, sizeof(load));
    WriteRel32(at + 4, kCodeAddress + at + 8, kDataAddress + kLimitFlag);
  }

  std::vector<CursorHookSegment> Segments() const {
    return {{kDataAddress, data_.data(), data_.size(), false},
            {kCodeAddress, code_.data(), code_.size(), true}};
  }

  std::vector<std::uint8_t>& code() { return code_; }

 private:
  static constexpr std::size_t kMessage = 0x11;

  void WriteRel32(std::size_t at, std::uintptr_t next, std::uintptr_t target) {
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(next));
    std::memcpy(code_.data() + at, &displacement, sizeof(displacement));
  }

  std::vector<std::uint8_t> data_;
  std::vector<std::uint8_t> code_;
};

TEST(RobloxCursorHookTest, LocatesTheDrawFunctionAndSite) {
  DrawImage image;
  std::string error;
  const std::optional<CursorRenderSite> site =
      LocateCursorRenderSite(image.Segments(), &error);
  ASSERT_TRUE(site.has_value()) << error;
  EXPECT_EQ(site->render_function,
            DrawImage::kCodeAddress + DrawImage::kFunction);
  EXPECT_EQ(site->content_type_call,
            DrawImage::kCodeAddress + DrawImage::kDrawSite + 7);
  EXPECT_EQ(site->content_type_function,
            DrawImage::kCodeAddress + DrawImage::kCallee);
  EXPECT_EQ(site->max_dimension_flag,
            DrawImage::kDataAddress + DrawImage::kLimitFlag);
}

TEST(RobloxCursorHookTest, DrawSiteWithoutTheLimitStillHooks) {
  DrawImage image;
  image.code()[DrawImage::kLimitRead] = 0xCC;
  std::string error;
  const std::optional<CursorRenderSite> site =
      LocateCursorRenderSite(image.Segments(), &error);
  ASSERT_TRUE(site.has_value()) << error;
  EXPECT_EQ(site->max_dimension_flag, 0U);
}

TEST(RobloxCursorHookTest, RefusesASecondDrawSite) {
  DrawImage image;
  image.WriteDrawSite(DrawImage::kDrawSite + 0x100);
  std::string error;
  EXPECT_FALSE(LocateCursorRenderSite(image.Segments(), &error).has_value());
  EXPECT_EQ(error, "cursor draw site is ambiguous");
}

TEST(RobloxCursorHookTest, RefusesASecondAnchorLoad) {
  DrawImage image;
  image.WriteAnchorLoad(DrawImage::kAnchorLoad + 0x20);
  std::string error;
  EXPECT_FALSE(LocateCursorRenderSite(image.Segments(), &error).has_value());
  EXPECT_EQ(error, "cursor cache message not loaded exactly once");
}

TEST(RobloxCursorHookTest, RefusesAChangedPrologue) {
  DrawImage image;
  // push r15 became push r14: the relocated bytes would no longer match.
  image.code()[DrawImage::kFunction + 5] = 0x56;
  std::string error;
  EXPECT_FALSE(LocateCursorRenderSite(image.Segments(), &error).has_value());
  EXPECT_EQ(error, "cursor draw function prologue not found");
}

TEST(RobloxCursorHookTest, RefusesAMissingDrawSite) {
  DrawImage image;
  image.code()[DrawImage::kDrawSite + 15] = 0x03;
  std::string error;
  EXPECT_FALSE(LocateCursorRenderSite(image.Segments(), &error).has_value());
  EXPECT_EQ(error, "cursor draw site not found");
}

TEST(RobloxCursorHookTest, RefusesAnImageWithoutTheMessage) {
  std::vector<std::uint8_t> code(0x100, 0xCC);
  std::string error;
  EXPECT_FALSE(
      LocateCursorRenderSite({{0x1000, code.data(), code.size(), true}}, &error)
          .has_value());
  EXPECT_EQ(error, "cursor cache message not found exactly once");
}

TEST(RobloxCursorHookTest, HardwareCursorsLeaveOnlyShiftLockToRoblox) {
  EXPECT_TRUE(IsHardwareCursorContent("rbxassetid://1234567"));
  EXPECT_TRUE(
      IsHardwareCursorContent("http://www.roblox.com/asset/?id=1234567"));
  EXPECT_TRUE(IsHardwareCursorContent(
      "rbxasset://textures/Cursors/KeyboardMouse/ArrowCursor.png"));
  EXPECT_FALSE(IsHardwareCursorContent(""));
  EXPECT_FALSE(
      IsHardwareCursorContent("rbxasset://textures/MouseLockedCursor.png"));
  EXPECT_FALSE(IsHardwareCursorContent(
      "!bxasset://textures/Cursors/KeyboardMouse/MouseLockedCursor.png"));
}

// Each pixel's red and green hold its column and row.
std::vector<std::uint8_t> EncodePng(std::uint32_t width, std::uint32_t height) {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t* pixel = pixels.data() + (y * width + x) * 4;
      pixel[0] = static_cast<std::uint8_t>(x);
      pixel[1] = static_cast<std::uint8_t>(y);
      pixel[2] = 7;
      pixel[3] = 200;
    }
  }
  png_image png{};
  png.version = PNG_IMAGE_VERSION;
  png.width = width;
  png.height = height;
  png.format = PNG_FORMAT_RGBA;
  png_alloc_size_t size = 0;
  if (png_image_write_to_memory(&png, nullptr, &size, 0, pixels.data(), 0,
                                nullptr) == 0) {
    return {};
  }
  std::vector<std::uint8_t> encoded(size);
  if (png_image_write_to_memory(&png, encoded.data(), &size, 0, pixels.data(),
                                0, nullptr) == 0) {
    return {};
  }
  encoded.resize(size);
  return encoded;
}

TEST(RobloxCursorHookTest, DecodesAGameCursorCenteredOnThePointer) {
  const std::vector<std::uint8_t> png = EncodePng(6, 4);
  ASSERT_FALSE(png.empty());
  const std::shared_ptr<mocktail::CursorImage> image =
      DecodeCustomCursor(png.data(), png.size(), 64);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->width, 6U);
  EXPECT_EQ(image->height, 4U);
  EXPECT_EQ(image->hot_x, 3U);
  EXPECT_EQ(image->hot_y, 2U);
  ASSERT_EQ(image->rgba.size(), 6U * 4U * 4U);
  const std::uint8_t* last = image->rgba.data() + (3 * 6 + 5) * 4;
  EXPECT_EQ(last[0], 5);
  EXPECT_EQ(last[1], 3);
  EXPECT_EQ(last[2], 7);
  EXPECT_EQ(last[3], 200);
}

TEST(RobloxCursorHookTest, ShrinksAnOversizedCursorLikeRoblox) {
  const std::vector<std::uint8_t> png = EncodePng(8, 6);
  const std::shared_ptr<mocktail::CursorImage> image =
      DecodeCustomCursor(png.data(), png.size(), 4);
  ASSERT_NE(image, nullptr);
  // Each side is shrunk on its own, as Roblox draws it.
  EXPECT_EQ(image->width, 4U);
  EXPECT_EQ(image->height, 4U);
  EXPECT_EQ(image->hot_x, 2U);
  EXPECT_EQ(image->hot_y, 2U);
  // The first pixel averages columns 0 and 1 of row 0.
  EXPECT_EQ(image->rgba[0], 1);
  EXPECT_EQ(image->rgba[1], 0);
  EXPECT_EQ(image->rgba[2], 7);
  EXPECT_EQ(image->rgba[3], 200);
  // The last averages columns 6 and 7 of rows 4 and 5.
  const std::uint8_t* last = image->rgba.data() + (3 * 4 + 3) * 4;
  EXPECT_EQ(last[0], 7);
  EXPECT_EQ(last[1], 5);
}

TEST(RobloxCursorHookTest, LeavesUnusableCursorsToRoblox) {
  const std::uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};
  EXPECT_EQ(DecodeCustomCursor(jpeg, sizeof(jpeg), 64), nullptr);
  EXPECT_EQ(DecodeCustomCursor(nullptr, 0, 64), nullptr);
  // Too large for a desktop cursor when Roblox sets no limit.
  const std::vector<std::uint8_t> png = EncodePng(300, 8);
  EXPECT_EQ(DecodeCustomCursor(png.data(), png.size(), 0), nullptr);
  EXPECT_NE(DecodeCustomCursor(png.data(), png.size(), 128), nullptr);
}

}  // namespace
