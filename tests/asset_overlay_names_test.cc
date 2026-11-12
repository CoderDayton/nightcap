#include "runtime/asset_overlay_names.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mocktail::runtime::DisableAssetNameMappings;
using mocktail::runtime::OverlayRbxAssetNames;
using mocktail::runtime::OverlayHasCustomSky;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_asset_overlay_names_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << "x";
}

// Joins strings the way a string table stores them: each followed by a NUL.
std::vector<std::uint8_t> StringTable(
    const std::vector<std::string_view>& strings) {
  std::vector<std::uint8_t> table;
  for (std::string_view text : strings) {
    table.insert(table.end(), text.begin(), text.end());
    table.push_back(0);
  }
  return table;
}

std::string TextAt(const std::vector<std::uint8_t>& table, std::size_t offset) {
  return std::string(reinterpret_cast<const char*>(table.data()) + offset);
}

TEST(AssetOverlayNamesTest, ListsFilesUnderContentAsRbxAssetNames) {
  TemporaryDirectory overlay;
  ASSERT_FALSE(overlay.path().empty());
  WriteFile(overlay.path() / "content/textures/MouseLockedCursor.png");
  WriteFile(overlay.path() /
            "content/textures/Cursors/KeyboardMouse/IBeamCursor.png");
  WriteFile(overlay.path() / "notes.txt");
  std::filesystem::create_directories(overlay.path() / "content/empty");

  EXPECT_EQ(OverlayRbxAssetNames(overlay.path()),
            (std::vector<std::string>{
                "rbxasset://textures/Cursors/KeyboardMouse/IBeamCursor.png",
                "rbxasset://textures/MouseLockedCursor.png",
            }));
}

TEST(AssetOverlayNamesTest, MissingOverlayFolderHasNoNames) {
  TemporaryDirectory overlay;
  ASSERT_FALSE(overlay.path().empty());

  EXPECT_TRUE(OverlayRbxAssetNames(overlay.path() / "absent").empty());
  EXPECT_TRUE(OverlayRbxAssetNames(overlay.path()).empty());
}

// The runtime passes no folder while Custom assets is off.
TEST(AssetOverlayNamesTest, NoFolderListsNothingFromTheWorkingDirectory) {
  TemporaryDirectory working;
  ASSERT_FALSE(working.path().empty());
  WriteFile(working.path() / "content/textures/MouseLockedCursor.png");
  const std::filesystem::path previous = std::filesystem::current_path();
  std::filesystem::current_path(working.path());
  const std::vector<std::string> names = OverlayRbxAssetNames({});
  std::filesystem::current_path(previous);
  EXPECT_TRUE(names.empty());
}

TEST(AssetOverlayNamesTest, ChangesOnlyTheNamedWholeStrings) {
  const std::string name = "rbxasset://textures/MouseLockedCursor.png";
  const std::string longer = name + ".bak";
  const std::string prefixed = "x" + name;
  std::vector<std::uint8_t> table =
      StringTable({"first", name, longer, prefixed, name});
  const std::size_t first_copy = 6;
  const std::size_t longer_copy = first_copy + name.size() + 1;
  const std::size_t prefixed_copy = longer_copy + longer.size() + 1;
  const std::size_t second_copy = prefixed_copy + prefixed.size() + 1;

  EXPECT_EQ(DisableAssetNameMappings(table.data(), table.size(), {name}), 2u);

  EXPECT_EQ(TextAt(table, 0), "first");
  EXPECT_EQ(TextAt(table, first_copy), "!" + name.substr(1));
  EXPECT_EQ(TextAt(table, longer_copy), longer);
  EXPECT_EQ(TextAt(table, prefixed_copy), prefixed);
  EXPECT_EQ(TextAt(table, second_copy), "!" + name.substr(1));
}

TEST(AssetOverlayNamesTest, MatchesANameAtTheStartOfTheData) {
  const std::string name = "rbxasset://textures/IBeamCursor.png";
  std::vector<std::uint8_t> table = StringTable({name, "other"});

  EXPECT_EQ(DisableAssetNameMappings(table.data(), table.size(), {name}), 1u);
  EXPECT_EQ(TextAt(table, 0), "!" + name.substr(1));
}

TEST(AssetOverlayNamesTest, LeavesDataAloneWithoutAMatch) {
  std::vector<std::uint8_t> table = StringTable({"alpha", "beta"});
  const std::vector<std::uint8_t> before = table;

  EXPECT_EQ(DisableAssetNameMappings(table.data(), table.size(),
                                     {"rbxasset://missing.png", ""}),
            0u);
  EXPECT_EQ(table, before);
  EXPECT_EQ(DisableAssetNameMappings(nullptr, 0, {"alpha"}), 0u);
}

void WriteFaces(const std::filesystem::path& overlay, const char* prefix) {
  for (const char* face : {"bk", "dn", "ft", "lf", "rt", "up"}) {
    WriteFile(overlay / "content/android/textures/sky" /
              (std::string(prefix) + face + ".tex"));
  }
}

TEST(CustomSkyTest, NeedsAllSixSkyAndIndoorFaces) {
  TemporaryDirectory temporary;
  EXPECT_FALSE(OverlayHasCustomSky(temporary.path()));
  WriteFaces(temporary.path(), "sky512_");
  WriteFaces(temporary.path(), "indoor512_");
  EXPECT_TRUE(OverlayHasCustomSky(temporary.path()));
  std::filesystem::remove(temporary.path() /
                          "content/android/textures/sky/indoor512_up.tex");
  EXPECT_FALSE(OverlayHasCustomSky(temporary.path()));
  EXPECT_FALSE(OverlayHasCustomSky({}));
}

// An indoor sky alone only feeds reflections; games keep their own sky.
TEST(CustomSkyTest, AnIndoorSkyAloneIsNotACustomSky) {
  TemporaryDirectory temporary;
  WriteFaces(temporary.path(), "indoor512_");
  EXPECT_FALSE(OverlayHasCustomSky(temporary.path()));
}

}  // namespace
