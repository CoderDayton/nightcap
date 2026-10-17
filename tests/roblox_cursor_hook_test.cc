#include "runtime/roblox_cursor_hook.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using mocktail::runtime::ClassifyRobloxCursorContent;
using mocktail::runtime::CursorHookSegment;
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
class CursorImage {
 public:
  static constexpr std::uintptr_t kDataAddress = 0x200000;
  static constexpr std::uintptr_t kCodeAddress = 0x400000;
  static constexpr std::size_t kFunction = 0x40;
  static constexpr std::size_t kAnchorLoad = kFunction + 0x997;
  static constexpr std::size_t kDrawSite = kFunction + 0x141b;
  static constexpr std::size_t kCallee = 0x3000;

  CursorImage() : data_(0x100, 0), code_(0x4000, 0xCC) {
    const std::string message = "[FLog::CursorCache] Invalid asset ID: %s";
    std::memcpy(data_.data() + kMessage, message.data(), message.size());
    const std::uint8_t prologue[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57,
                                     0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                     0x53, 0x48, 0x81, 0xEC, 0xC8, 0x01,
                                     0x00, 0x00};
    std::memcpy(code_.data() + kFunction, prologue, sizeof(prologue));
    WriteAnchorLoad(kAnchorLoad);
    WriteDrawSite(kDrawSite);
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
  CursorImage image;
  std::string error;
  const std::optional<CursorRenderSite> site =
      LocateCursorRenderSite(image.Segments(), &error);
  ASSERT_TRUE(site.has_value()) << error;
  EXPECT_EQ(site->render_function,
            CursorImage::kCodeAddress + CursorImage::kFunction);
  EXPECT_EQ(site->content_type_call,
            CursorImage::kCodeAddress + CursorImage::kDrawSite + 7);
  EXPECT_EQ(site->content_type_function,
            CursorImage::kCodeAddress + CursorImage::kCallee);
}

TEST(RobloxCursorHookTest, RefusesASecondDrawSite) {
  CursorImage image;
  image.WriteDrawSite(CursorImage::kDrawSite + 0x100);
  std::string error;
  EXPECT_FALSE(LocateCursorRenderSite(image.Segments(), &error).has_value());
  EXPECT_EQ(error, "cursor draw site is ambiguous");
}

TEST(RobloxCursorHookTest, RefusesASecondAnchorLoad) {
  CursorImage image;
  image.WriteAnchorLoad(CursorImage::kAnchorLoad + 0x20);
  std::string error;
  EXPECT_FALSE(LocateCursorRenderSite(image.Segments(), &error).has_value());
  EXPECT_EQ(error, "cursor cache message not loaded exactly once");
}

TEST(RobloxCursorHookTest, RefusesAChangedPrologue) {
  CursorImage image;
  // push r15 became push r14: the relocated bytes would no longer match.
  image.code()[CursorImage::kFunction + 5] = 0x56;
  std::string error;
  EXPECT_FALSE(LocateCursorRenderSite(image.Segments(), &error).has_value());
  EXPECT_EQ(error, "cursor draw function prologue not found");
}

TEST(RobloxCursorHookTest, RefusesAMissingDrawSite) {
  CursorImage image;
  image.code()[CursorImage::kDrawSite + 15] = 0x03;
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

}  // namespace
