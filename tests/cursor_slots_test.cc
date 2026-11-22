#include "ui/cursor_slots.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace mocktail {
namespace ui {
namespace {

std::string Png(std::uint32_t width, std::uint32_t height) {
  std::string png("\x89PNG\r\n\x1a\n", 8);
  png += std::string("\0\0\0\x0dIHDR", 8);
  for (const std::uint32_t side : {width, height}) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      png += static_cast<char>((side >> shift) & 0xff);
    }
  }
  png += std::string("\x08\x06\0\0\0", 5);
  return png;
}

class CursorSlotsTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("cursor_slots_test_" + std::to_string(getpid()) + "_" +
             testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
    overlay_ = root_ / "overlay";
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  std::filesystem::path Write(const std::string& name,
                              const std::string& bytes) {
    const std::filesystem::path path = root_ / name;
    std::ofstream(path, std::ios::binary) << bytes;
    return path;
  }

  static std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }

  std::filesystem::path root_;
  std::filesystem::path overlay_;
};

TEST_F(CursorSlotsTest, EachSlotHasRobloxsFileName) {
  const std::filesystem::path folder =
      overlay_ / "content/textures/Cursors/KeyboardMouse";
  EXPECT_EQ(CursorFile(overlay_, CursorSlot::kArrow),
            folder / "ArrowFarCursor.png");
  EXPECT_EQ(CursorFile(overlay_, CursorSlot::kPointer),
            folder / "ArrowCursor.png");
  EXPECT_EQ(CursorFile(overlay_, CursorSlot::kText), folder / "IBeamCursor.png");
}

TEST_F(CursorSlotsTest, InstallRenamesThePickedFile) {
  const std::string png = Png(64, 64);
  std::string error;
  ASSERT_TRUE(InstallCursor(Write("my cool cursor (1).png", png), overlay_,
                            CursorSlot::kArrow, &error))
      << error;
  EXPECT_EQ(Read(CursorFile(overlay_, CursorSlot::kArrow)), png);
  EXPECT_TRUE(HasCustomCursor(overlay_, CursorSlot::kArrow));
  EXPECT_FALSE(HasCustomCursor(overlay_, CursorSlot::kPointer));
  EXPECT_FALSE(HasCustomCursor(overlay_, CursorSlot::kText));
}

TEST_F(CursorSlotsTest, InstallReplacesTheSlot) {
  std::string error;
  ASSERT_TRUE(InstallCursor(Write("a.png", Png(32, 32)), overlay_,
                            CursorSlot::kText, &error));
  const std::string second = Png(48, 24);
  ASSERT_TRUE(InstallCursor(Write("b.png", second), overlay_,
                            CursorSlot::kText, &error));
  EXPECT_EQ(Read(CursorFile(overlay_, CursorSlot::kText)), second);
}

TEST_F(CursorSlotsTest, InstallRefusesWhatIsNotAPng) {
  std::string error;
  EXPECT_FALSE(InstallCursor(Write("photo.png", "GIF89a not a png at all"),
                             overlay_, CursorSlot::kArrow, &error));
  EXPECT_NE(error.find("photo.png"), std::string::npos);
  EXPECT_FALSE(HasCustomCursor(overlay_, CursorSlot::kArrow));
}

TEST_F(CursorSlotsTest, InstallRefusesAPictureTooBigForACursor) {
  std::string error;
  EXPECT_TRUE(InstallCursor(Write("edge.png", Png(256, 256)), overlay_,
                            CursorSlot::kArrow, &error));
  EXPECT_FALSE(InstallCursor(Write("wide.png", Png(257, 32)), overlay_,
                             CursorSlot::kPointer, &error));
  EXPECT_NE(error.find("257"), std::string::npos);
  EXPECT_FALSE(InstallCursor(Write("empty.png", Png(0, 32)), overlay_,
                             CursorSlot::kPointer, &error));
  EXPECT_FALSE(HasCustomCursor(overlay_, CursorSlot::kPointer));
}

TEST_F(CursorSlotsTest, AFailedInstallKeepsTheCursorInUse) {
  const std::string png = Png(32, 32);
  std::string error;
  ASSERT_TRUE(InstallCursor(Write("good.png", png), overlay_,
                            CursorSlot::kArrow, &error));
  EXPECT_FALSE(InstallCursor(Write("bad.png", "nope"), overlay_,
                             CursorSlot::kArrow, &error));
  EXPECT_FALSE(InstallCursor(root_ / "missing.png", overlay_,
                             CursorSlot::kArrow, &error));
  EXPECT_EQ(Read(CursorFile(overlay_, CursorSlot::kArrow)), png);
}

TEST_F(CursorSlotsTest, RemoveGoesBackToRobloxsAndLeavesTheOthers) {
  std::string error;
  ASSERT_TRUE(InstallCursor(Write("a.png", Png(32, 32)), overlay_,
                            CursorSlot::kArrow, &error));
  ASSERT_TRUE(InstallCursor(Write("p.png", Png(32, 32)), overlay_,
                            CursorSlot::kPointer, &error));
  EXPECT_TRUE(RemoveCursor(overlay_, CursorSlot::kArrow, &error)) << error;
  EXPECT_FALSE(HasCustomCursor(overlay_, CursorSlot::kArrow));
  EXPECT_TRUE(HasCustomCursor(overlay_, CursorSlot::kPointer));
  EXPECT_TRUE(RemoveCursor(overlay_, CursorSlot::kArrow, &error)) << error;
}

}
}
}
