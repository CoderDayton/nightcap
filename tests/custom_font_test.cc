#include "runtime/custom_font.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

class CustomFontTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mocktail_custom_font_XXXXXX";
    ASSERT_NE(mkdtemp(pattern), nullptr);
    root_ = pattern;
  }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  static void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
  }
  static std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }

  std::filesystem::path root_;
};

TEST_F(CustomFontTest, FindsTheFontSettingsKept) {
  const std::filesystem::path dir = root_ / "custom_font";
  EXPECT_TRUE(InstalledCustomFont(dir).empty());
  Write(dir / "notes.txt", "x");
  EXPECT_TRUE(InstalledCustomFont(dir).empty());
  Write(dir / "Comic.TTF", "font");
  EXPECT_EQ(InstalledCustomFont(dir), dir / "Comic.TTF");
}

TEST_F(CustomFontTest, LeavesEmojiAndOtherScriptsToRoblox) {
  EXPECT_TRUE(IsReplaceableRobloxFont("BuilderSans-Regular.otf"));
  EXPECT_TRUE(IsReplaceableRobloxFont("Arimo-Bold.ttf"));
  EXPECT_FALSE(IsReplaceableRobloxFont("RobloxEmoji.ttf"));
  EXPECT_FALSE(IsReplaceableRobloxFont("TwemojiMozilla.ttf"));
  EXPECT_FALSE(IsReplaceableRobloxFont("NotoSansThaiUI-Regular.ttf"));
  EXPECT_FALSE(IsReplaceableRobloxFont("font-mappings.json"));
}

TEST_F(CustomFontTest, PointsEveryRobloxFontAtThePick) {
  const std::filesystem::path font = root_ / "custom_font" / "Comic.ttf";
  Write(font, "comic");
  const std::filesystem::path stock = root_ / "assets" / "content" / "fonts";
  Write(stock / "BuilderSans-Regular.otf", "builder");
  Write(stock / "RobloxEmoji.ttf", "emoji");
  Write(stock / "families" / "BuilderSans.json", R"({
    "name": "Builder Sans",
    "faces": [
      {"name": "Light", "weight": 300, "style": "normal",
       "assetId": "rbxassetid://16480736281"},
      {"name": "Regular", "weight": 400, "style": "normal",
       "assetId": "rbxasset://fonts/BuilderSans-Regular.otf"}
    ]})");
  Write(stock / "families" / "NotoSansCJKFallback.json",
        R"({"name": "NotoSansCJK Fallback", "faces": []})");

  const std::filesystem::path out = root_ / "overlay";
  std::size_t replaced = 0;
  std::string error;
  ASSERT_TRUE(BuildCustomFontOverlay(font, stock, out, &replaced, &error))
      << error;

  const std::filesystem::path fonts = out / "content" / "fonts";
  EXPECT_EQ(Read(fonts / "BuilderSans-Regular.otf"), "comic");
  EXPECT_EQ(Read(fonts / "NightcapCustomFont.ttf"), "comic");
  EXPECT_FALSE(std::filesystem::exists(fonts / "RobloxEmoji.ttf"));
  EXPECT_FALSE(
      std::filesystem::exists(fonts / "families" / "NotoSansCJKFallback.json"));
  const nlohmann::json family =
      nlohmann::json::parse(Read(fonts / "families" / "BuilderSans.json"));
  EXPECT_EQ(family["name"], "Builder Sans");
  ASSERT_EQ(family["faces"].size(), 2U);
  EXPECT_EQ(family["faces"][0]["weight"], 300);
  for (const nlohmann::json& face : family["faces"]) {
    EXPECT_EQ(face["assetId"], "rbxasset://fonts/NightcapCustomFont.ttf");
  }
  EXPECT_EQ(replaced, 2U);
}

TEST_F(CustomFontTest, ARebuildDropsFontsRobloxNoLongerShips) {
  const std::filesystem::path font = root_ / "Comic.ttf";
  Write(font, "comic");
  const std::filesystem::path stock = root_ / "fonts";
  Write(stock / "Old-Regular.ttf", "old");
  const std::filesystem::path out = root_ / "overlay";
  std::size_t replaced = 0;
  std::string error;
  ASSERT_TRUE(BuildCustomFontOverlay(font, stock, out, &replaced, &error));
  std::filesystem::remove(stock / "Old-Regular.ttf");
  Write(stock / "New-Regular.ttf", "new");
  ASSERT_TRUE(BuildCustomFontOverlay(font, stock, out, &replaced, &error));
  EXPECT_FALSE(std::filesystem::exists(out / "content/fonts/Old-Regular.ttf"));
  EXPECT_TRUE(std::filesystem::exists(out / "content/fonts/New-Regular.ttf"));
}

// A new pick in Settings must not break the fonts of a session already running.
TEST_F(CustomFontTest, TheOverlayOutlivesThePick) {
  const std::filesystem::path font = root_ / "custom_font" / "Comic.ttf";
  Write(font, "comic");
  const std::filesystem::path stock = root_ / "fonts";
  Write(stock / "Arimo-Regular.ttf", "arimo");
  const std::filesystem::path out = root_ / "overlay";
  std::size_t replaced = 0;
  std::string error;
  ASSERT_TRUE(BuildCustomFontOverlay(font, stock, out, &replaced, &error))
      << error;
  std::filesystem::remove(font);
  const std::filesystem::path fonts = out / "content" / "fonts";
  EXPECT_EQ(Read(fonts / "NightcapCustomFont.ttf"), "comic");
  EXPECT_EQ(Read(fonts / "Arimo-Regular.ttf"), "comic");
}

TEST_F(CustomFontTest, APickReplacesTheLastOneAndCanBeUndone) {
  const std::filesystem::path dir = root_ / "custom_font";
  Write(root_ / "First.ttf", "first");
  Write(root_ / "Second.otf", "second");
  std::string error;
  ASSERT_TRUE(InstallCustomFont(root_ / "First.ttf", dir, &error)) << error;
  ASSERT_TRUE(InstallCustomFont(root_ / "Second.otf", dir, &error)) << error;
  EXPECT_EQ(InstalledCustomFont(dir), dir / "Second.otf");
  EXPECT_FALSE(std::filesystem::exists(dir / "First.ttf"));
  EXPECT_EQ(Read(dir / "Second.otf"), "second");
  EXPECT_TRUE(std::filesystem::exists(root_ / "Second.otf"));

  EXPECT_FALSE(InstallCustomFont(root_ / "notes.txt", dir, &error));
  EXPECT_EQ(InstalledCustomFont(dir), dir / "Second.otf");

  ASSERT_TRUE(RemoveCustomFont(dir, &error)) << error;
  EXPECT_TRUE(InstalledCustomFont(dir).empty());
}

TEST_F(CustomFontTest, RefusesAMissingFont) {
  std::size_t replaced = 0;
  std::string error;
  EXPECT_FALSE(BuildCustomFontOverlay(root_ / "none.ttf", root_, root_ / "out",
                                      &replaced, &error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
