#include "ui/fflag_presets.h"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "services/client_settings_service.h"

namespace mocktail {
namespace ui {
namespace {

std::vector<PresetFlag> Flags(std::string_view text) {
  std::vector<PresetFlag> flags;
  std::string error;
  EXPECT_TRUE(ParseFFlags(text, &flags, &error)) << error;
  return flags;
}

FFlagPresets Load(std::string_view presets_text, std::string_view fflags) {
  FFlagPresets presets;
  std::string error;
  EXPECT_TRUE(LoadPresets(presets_text, Flags(fflags), &presets, &error))
      << error;
  return presets;
}

const PresetFlag* Flag(const Preset& preset, std::string_view name) {
  for (const PresetFlag& flag : preset.flags) {
    if (flag.name == name) {
      return &flag;
    }
  }
  return nullptr;
}

std::string LoaderError(std::string_view text) {
  char pattern[] = "/tmp/fflag_presets_test_XXXXXX";
  const int fd = mkstemp(pattern);
  EXPECT_GE(fd, 0);
  close(fd);
  {
    std::ofstream out(pattern, std::ios::binary | std::ios::trunc);
    out << text;
  }
  const std::string error =
      services::LoadAndMergeFflagsFile(pattern, "{}").error;
  std::filesystem::remove(pattern);
  return error;
}

TEST(FFlagPresetsTest, CatalogEntriesAreWellFormed) {
  std::set<std::string_view> names;
  std::set<std::string_view> groups;
  for (const FlagGroup& group : FlagGroups()) {
    groups.insert(group.name);
  }
  for (const CatalogFlag& flag : FlagCatalog()) {
    ASSERT_FALSE(flag.names.empty());
    SCOPED_TRACE(std::string(flag.names[0]));
    EXPECT_TRUE(groups.count(flag.group)) << flag.group;
    EXPECT_FALSE(flag.summary.empty());
    EXPECT_FALSE(flag.effects.empty());
    const bool either_way = flag.levels.empty() &&
                            flag.minimum < flag.default_number &&
                            flag.default_number < flag.maximum;
    for (const FlagEffect& effect : flag.effects) {
      EXPECT_FALSE(effect.text.empty());
      if (either_way) {
        EXPECT_TRUE(effect.text.rfind("Lower: ", 0) == 0 ||
                    effect.text.rfind("Higher: ", 0) == 0)
            << effect.text;
      }
    }
    for (std::string_view name : flag.names) {
      EXPECT_TRUE(names.insert(name).second) << name << " is listed twice";
      EXPECT_FALSE(IsRemovedFlag(name));
    }
    for (const FlagLevel& level : flag.levels) {
      EXPECT_EQ(level.values.size(), flag.names.size()) << level.label;
    }
    if (flag.levels.empty()) {
      EXPECT_LE(flag.minimum, flag.default_number);
      EXPECT_LE(flag.default_number, flag.maximum);
      EXPECT_GT(flag.step, 0);
    } else {
      EXPECT_LT(flag.default_level, flag.levels.size());
    }
    EXPECT_EQ(FindCatalogFlag(flag.names[0]), &flag);
  }
}

TEST(FFlagPresetsTest, EveryLevelIsWhatNightcapLoads) {
  for (const CatalogFlag& flag : FlagCatalog()) {
    for (std::size_t level = 0; level < flag.levels.size(); ++level) {
      Preset preset;
      AddCatalogFlag(&preset, flag);
      const std::vector<PresetRow> rows = PresetRows(preset);
      ASSERT_EQ(rows.size(), 1u) << flag.names[0];
      SetRowLevel(&preset, rows[0], level);
      EXPECT_EQ(LoaderError(FFlagsText(preset)), "") << flag.names[0];
    }
  }
}

TEST(FFlagPresetsTest, FirstOpenMakesMySetupFromFFlags) {
  const FFlagPresets presets =
      Load("", R"({"FFlagDebugDisplayFPS": true, "FIntFoo": 3})");
  ASSERT_EQ(presets.active, "My setup");
  const Preset* mine = presets.Find("My setup");
  ASSERT_NE(mine, nullptr);
  EXPECT_EQ(presets.presets.size(), 1u);
  ASSERT_EQ(mine->flags.size(), 2u);
  EXPECT_EQ(mine->flags[0].name, "FFlagDebugDisplayFPS");
  EXPECT_EQ(mine->flags[1].value, "3");
  EXPECT_TRUE(mine->flags[1].on);
}

TEST(FFlagPresetsTest, AnOffFlagStaysInThePresetButNotInFFlags) {
  FFlagPresets presets = Load("", R"({"FIntA": 1, "FIntB": 2})");
  Preset* mine = presets.Find(presets.active);
  ASSERT_NE(mine, nullptr);
  mine->flags[1].on = false;
  const std::string fflags = FFlagsText(*mine);
  EXPECT_EQ(Flags(fflags).size(), 1u);

  const FFlagPresets again = Load(PresetsText(presets), fflags);
  const Preset* reread = again.Find(again.active);
  ASSERT_NE(reread, nullptr);
  ASSERT_EQ(reread->flags.size(), 2u);
  EXPECT_TRUE(reread->flags[0].on);
  EXPECT_FALSE(reread->flags[1].on);
  EXPECT_EQ(reread->flags[1].value, "2");
}

TEST(FFlagPresetsTest, HandEditsToFFlagsAreTakenIn) {
  FFlagPresets presets =
      Load("", R"({"FIntA": 1, "FIntB": 2, "FIntC": 3})");
  presets.Find(presets.active)->flags[1].on = false;
  const std::string saved = PresetsText(presets);

  // A changed, B off and untouched, C deleted, D added.
  const FFlagPresets again =
      Load(saved, R"({"FIntA": 5, "FFlagD": true})");
  const Preset* mine = again.Find(again.active);
  ASSERT_NE(mine, nullptr);
  ASSERT_EQ(mine->flags.size(), 3u);
  EXPECT_EQ(Flag(*mine, "FIntA")->value, "5");
  EXPECT_FALSE(Flag(*mine, "FIntB")->on);
  EXPECT_EQ(Flag(*mine, "FIntC"), nullptr);
  EXPECT_TRUE(Flag(*mine, "FFlagD")->on);
}

// Earlier builds had built-in presets, which the file names but never held.
TEST(FFlagPresetsTest, AnActivePresetNoLongerThereBecomesMySetup) {
  const FFlagPresets presets =
      Load(R"({"active": "Starter", "presets": []})",
           R"({"FFlagDebugDisplayFPS": true})");
  EXPECT_EQ(presets.active, "My setup");
  ASSERT_EQ(presets.presets.size(), 1u);
  EXPECT_EQ(presets.presets[0].flags.size(), 1u);
}

TEST(FFlagPresetsTest, NotAPresetsFileIsAnError) {
  FFlagPresets presets;
  std::string error;
  EXPECT_FALSE(LoadPresets("[1, 2]", {}, &presets, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(LoadPresets("{", {}, &presets, &error));
}

TEST(FFlagPresetsTest, AHandEditedOnThatIsNotABooleanCountsAsOn) {
  const FFlagPresets presets = Load(
      R"({"active": "Mine", "presets": [{"name": "Mine", "flags": [
          {"name": "FIntA", "value": 1, "on": 0}]}]})",
      R"({"FIntA": 1})");
  const Preset* mine = presets.Find("Mine");
  ASSERT_NE(mine, nullptr);
  ASSERT_EQ(mine->flags.size(), 1u);
  EXPECT_TRUE(mine->flags[0].on);
}

TEST(FFlagPresetsTest, FFlagsThatAreNotFlagsAreAnError) {
  std::vector<PresetFlag> flags;
  std::string error;
  EXPECT_FALSE(ParseFFlags(R"({"FIntA": 1.5})", &flags, &error));
  EXPECT_FALSE(ParseFFlags("[]", &flags, &error));
  EXPECT_FALSE(ParseFFlags(R"({"": 1})", &flags, &error));
  EXPECT_FALSE(ParseFFlags(R"({"FIntA": 1, "FIntA": 2})", &flags, &error));
  EXPECT_EQ(error, "\"FIntA\" is listed twice");
  EXPECT_TRUE(ParseFFlags("// note\n{\"FIntA\": 1}", &flags, &error)) << error;
  EXPECT_TRUE(ParseFFlags("", &flags, &error)) << error;
  EXPECT_TRUE(flags.empty());
}

TEST(FFlagPresetsTest, FlagsSetTogetherAreOneRow) {
  Preset preset;
  preset.flags = {{"FIntFRMMinGrassDistance", "0"},
                  {"FFlagDebugDisplayFPS", "\"True\""},
                  {"FIntFRMMaxGrassDistance", "0"}};
  const std::vector<PresetRow> rows = PresetRows(preset);
  ASSERT_EQ(rows.size(), 2u);
  ASSERT_NE(rows[0].flag, nullptr);
  EXPECT_EQ(rows[0].flag->names[0], "FIntFRMMaxGrassDistance");
  EXPECT_EQ(rows[0].entries.size(), 2u);
  EXPECT_EQ(rows[0].level, 0u);
  // "True" is how many guides write true.
  EXPECT_EQ(rows[1].level, 0u);

  SetRowOn(&preset, rows[0], false);
  EXPECT_FALSE(Flag(preset, "FIntFRMMinGrassDistance")->on);
  EXPECT_FALSE(Flag(preset, "FIntFRMMaxGrassDistance")->on);
  EXPECT_TRUE(Flag(preset, "FFlagDebugDisplayFPS")->on);
}

TEST(FFlagPresetsTest, ALevelSetsEveryFlagOfTheRow) {
  const CatalogFlag* texture = FindCatalogFlag("DFIntTextureQualityOverride");
  ASSERT_NE(texture, nullptr);
  ASSERT_GE(texture->levels.size(), 2u);
  Preset preset;
  AddCatalogFlag(&preset, *texture);
  EXPECT_EQ(preset.flags.size(), texture->names.size());
  std::vector<PresetRow> rows = PresetRows(preset);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].level, texture->default_level);

  SetRowLevel(&preset, rows[0], 0);
  rows = PresetRows(preset);
  EXPECT_EQ(rows[0].level, 0u);
  for (std::size_t i = 0; i < texture->names.size(); ++i) {
    EXPECT_EQ(Flag(preset, texture->names[i])->value,
              texture->levels[0].values[i]);
  }
}

TEST(FFlagPresetsTest, AddingAFlagChangesItRatherThanListingItTwice) {
  const CatalogFlag* grass = FindCatalogFlag("FIntFRMMaxGrassDistance");
  ASSERT_NE(grass, nullptr);
  Preset preset;
  preset.flags = {{"FIntFRMMinGrassDistance", "40", false}};
  EXPECT_FALSE(HasCatalogFlag(preset, *grass));
  AddCatalogFlag(&preset, *grass);
  EXPECT_TRUE(HasCatalogFlag(preset, *grass));
  ASSERT_EQ(preset.flags.size(), 2u);
  EXPECT_EQ(Flag(preset, "FIntFRMMinGrassDistance")->value, "0");
  EXPECT_TRUE(Flag(preset, "FIntFRMMinGrassDistance")->on);
}

TEST(FFlagPresetsTest, ANumberFlagTakesTheNumberGiven) {
  const CatalogFlag* number = nullptr;
  for (const CatalogFlag& flag : FlagCatalog()) {
    if (flag.levels.empty()) {
      number = &flag;
      break;
    }
  }
  ASSERT_NE(number, nullptr);
  Preset preset;
  AddCatalogFlag(&preset, *number);
  std::vector<PresetRow> rows = PresetRows(preset);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(RowNumber(preset, rows[0], -1), number->default_number);
  SetRowNumber(&preset, rows[0], number->minimum);
  EXPECT_EQ(RowNumber(preset, PresetRows(preset)[0], -1), number->minimum);
}

TEST(FFlagPresetsTest, RemovedAndUnknownFlagsAreRowsOfTheirOwn) {
  Preset preset;
  preset.flags = {{"FFlagNewLightAttenuation", "true"},
                  {"FFlagSomethingNew", "true"}};
  const std::vector<PresetRow> rows = PresetRows(preset);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_TRUE(rows[0].removed);
  EXPECT_EQ(rows[0].flag, nullptr);
  EXPECT_FALSE(rows[1].removed);
  EXPECT_EQ(rows[1].flag, nullptr);
  RemoveRow(&preset, rows[0]);
  ASSERT_EQ(preset.flags.size(), 1u);
  EXPECT_EQ(preset.flags[0].name, "FFlagSomethingNew");
  EXPECT_TRUE(IsRemovedFlag("FFlagDebugDisableTelemetryEventIngest"));
}

TEST(FFlagPresetsTest, NewPresetNamesAreUnique) {
  const FFlagPresets presets = Load("", "{}");
  EXPECT_EQ(UniquePresetName(presets, "My setup"), "My setup 2");
  EXPECT_EQ(UniquePresetName(presets, "Racing"), "Racing");
}

TEST(FFlagPresetsTest, SearchLooksAtNamesAndText) {
  const std::vector<const CatalogFlag*> grass = FindFlags("", "GRASS");
  EXPECT_GE(grass.size(), 2u);
  const std::vector<const CatalogFlag*> fps = FindFlags("Performance", "fps");
  ASSERT_FALSE(fps.empty());
  EXPECT_EQ(fps[0]->names[0], "FFlagDebugDisplayFPS");
  EXPECT_TRUE(FindFlags("Graphics", "fps counter").empty());
  // Effects are searched too, so "fps" finds what raises it.
  const std::vector<const CatalogFlag*> raises = FindFlags("", "fps");
  for (std::string_view name : {"DFFlagDebugPauseVoxelizer",
                                "DFIntCSGLevelOfDetailSwitchingDistance"}) {
    EXPECT_TRUE(std::any_of(raises.begin(), raises.end(),
                            [name](const CatalogFlag* flag) {
                              return flag->names[0] == name;
                            }))
        << name;
  }
  EXPECT_EQ(FindFlags("Graphics", "").size(),
            static_cast<std::size_t>(std::count_if(
                FlagCatalog().begin(), FlagCatalog().end(),
                [](const CatalogFlag& flag) {
                  return flag.group == "Graphics";
                })));
}

}  // namespace
}  // namespace ui
}  // namespace mocktail
