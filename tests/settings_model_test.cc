#include "ui/settings_model.h"

#include <gtest/gtest.h>
#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

#include "runtime/environment.h"
#include "runtime/runtime_config_file.h"

namespace mocktail {
namespace ui {
namespace {

class EmptyEnvironment final : public runtime::Environment {
 public:
  std::optional<std::string> Get(std::string_view) const override {
    return std::nullopt;
  }
};

const Setting& Find(std::string_view key) {
  for (const SettingsPage& page : SettingsPages()) {
    for (const SettingsGroup& group : page.groups) {
      for (const Setting& setting : group.settings) {
        if (setting.key == key) {
          return setting;
        }
      }
    }
  }
  ADD_FAILURE() << "no setting " << key;
  static const Setting missing;
  return missing;
}

class SettingsModelTest : public testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mocktail_settings_model_XXXXXX";
    const char* created = mkdtemp(pattern);
    ASSERT_NE(created, nullptr);
    directory_ = created;
    file_ = directory_ / "mocktail" / "config.yaml";
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  std::string Read() {
    std::ostringstream contents;
    contents << std::ifstream(file_, std::ios::binary).rdbuf();
    return contents.str();
  }

  void Write(std::string_view contents) {
    std::filesystem::create_directories(file_.parent_path());
    std::ofstream(file_, std::ios::binary) << contents;
  }

  runtime::RuntimeConfigLoadResult LoadFile() {
    return runtime::LoadRuntimeConfig(EmptyEnvironment(), file_);
  }

  std::filesystem::path directory_;
  std::filesystem::path file_;
};

TEST_F(SettingsModelTest, PagesListEverySettingOnce) {
  std::set<std::string_view> keys;
  std::vector<std::string_view> titles;
  for (const SettingsPage& page : SettingsPages()) {
    titles.push_back(page.title);
    EXPECT_FALSE(page.section.empty()) << page.title;
    EXPECT_FALSE(page.icon_name.empty()) << page.title;
    for (const SettingsGroup& group : page.groups) {
      for (const Setting& setting : group.settings) {
        EXPECT_FALSE(setting.title.empty()) << setting.key;
        EXPECT_TRUE(keys.insert(setting.key).second) << setting.key;
      }
    }
  }
  EXPECT_EQ(titles, (std::vector<std::string_view>{
                        "Graphics", "Input", "Performance", "Audio",
                        "Discord", "Advanced"}));
}

TEST_F(SettingsModelTest, PagesOfOneSectionAreNextToEachOther) {
  std::set<std::string_view> closed;
  std::string_view current;
  for (const SettingsPage& page : SettingsPages()) {
    if (page.section != current) {
      EXPECT_TRUE(closed.insert(page.section).second) << page.section;
      current = page.section;
    }
  }
}

TEST_F(SettingsModelTest, OnlySettingsWithAControlAreAvailable) {
  EXPECT_TRUE(IsAvailable(Find("graphics.backend")));
  EXPECT_TRUE(IsAvailable(Find("graphics.frame_rate_limit")));
  EXPECT_TRUE(IsAvailable(Find("integrations.discord_rpc.enabled")));
  EXPECT_FALSE(IsAvailable(Find("performance.memory_limit_mb")));
  EXPECT_FALSE(IsAvailable(Find("audio.output_device")));
  EXPECT_FALSE(IsAvailable(Find("fflags")));
}

TEST_F(SettingsModelTest, EveryValueTheWindowOffersSavesAndLoads) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;

  for (const SettingsPage& page : SettingsPages()) {
    for (const SettingsGroup& group : page.groups) {
      for (const Setting& setting : group.settings) {
        if (!IsAvailable(setting)) {
          continue;
        }
        std::vector<std::string> values;
        if (setting.kind == SettingKind::kToggle) {
          values = {"true", "false"};
        } else if (setting.kind == SettingKind::kFrameRate) {
          for (const FrameRateMode mode :
               {FrameRateMode::kDisplay, FrameRateMode::kUnlimited,
                FrameRateMode::kCustom, FrameRateMode::kRoblox}) {
            values.push_back(FrameRateValue({mode, 144}));
          }
        } else {
          for (const SettingChoice& choice : setting.choices) {
            values.emplace_back(choice.value);
          }
        }
        for (const std::string& value : values) {
          ASSERT_TRUE(document.Set(setting.key, value, &error))
              << setting.key << " = " << value << ": " << error;
          EXPECT_EQ(document.Value(setting.key), value) << setting.key;
          const runtime::RuntimeConfigLoadResult loaded = LoadFile();
          ASSERT_TRUE(loaded) << setting.key << " = " << value << ": "
                              << loaded.error;
        }
      }
    }
  }
}

TEST_F(SettingsModelTest, SavedValuesReachTheLoadedConfig) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  ASSERT_TRUE(document.Set("graphics.backend", "opengl", &error)) << error;
  ASSERT_TRUE(document.Set("graphics.vsync", "on", &error)) << error;
  ASSERT_TRUE(document.Set("window.high_dpi", "true", &error)) << error;
  ASSERT_TRUE(document.Set("input.raw_mouse", "true", &error)) << error;

  const runtime::RuntimeConfigLoadResult loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.graphics_backend_name(), "opengl");
  EXPECT_EQ(loaded.config.vsync_mode(), "on");
  EXPECT_TRUE(loaded.config.window().high_dpi);
  EXPECT_TRUE(loaded.config.input_capabilities().raw_mouse);
}

TEST_F(SettingsModelTest, LoadCreatesTheFirstRunFile) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_TRUE(std::filesystem::exists(file_));
  EXPECT_EQ(document.Value("graphics.backend"), "direct-vulkan");
  EXPECT_EQ(document.Value("graphics.vsync"), "");
}

TEST_F(SettingsModelTest, LoadReportsAFileThatIsNotYaml) {
  Write("graphics: [1, 2\n");
  SettingsDocument document(file_);
  std::string error;
  EXPECT_FALSE(document.Load(&error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(Read(), "graphics: [1, 2\n");
}

TEST_F(SettingsModelTest, AnEmptyValueUnsetsTheKey) {
  Write("graphics:\n  vsync: on\n  backend: opengl\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  ASSERT_TRUE(document.Set("graphics.vsync", "", &error)) << error;
  EXPECT_EQ(document.Value("graphics.vsync"), "");
  EXPECT_EQ(Read(), "graphics:\n  # vsync: on\n  backend: opengl\n");
}

TEST_F(SettingsModelTest, AValueTheLoaderRejectsIsNotSaved) {
  Write("graphics:\n  backend: opengl\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_FALSE(document.Set("graphics.backend", "no-such-backend", &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(document.Value("graphics.backend"), "opengl");
  EXPECT_EQ(Read(), "graphics:\n  backend: opengl\n");
}

TEST_F(SettingsModelTest, AHandEditMadeMeanwhileIsKeptAndShown) {
  Write("graphics:\n  backend: opengl\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;

  Write("graphics:\n  backend: angle-vulkan\n  vsync: off\n");
  EXPECT_FALSE(document.Set("graphics.vsync", "on", &error));
  EXPECT_EQ(Read(), "graphics:\n  backend: angle-vulkan\n  vsync: off\n");
  EXPECT_EQ(document.Value("graphics.backend"), "angle-vulkan");

  // The document now holds the hand edit, so the next change goes through.
  ASSERT_TRUE(document.Set("graphics.vsync", "on", &error)) << error;
  EXPECT_EQ(Read(), "graphics:\n  backend: angle-vulkan\n  vsync: on\n");
}

TEST_F(SettingsModelTest, ChoiceIndexMapsDefaultsAndOtherSpellings) {
  const Setting& backend = Find("graphics.backend");
  EXPECT_EQ(ChoiceIndex(backend, ""), 0u);
  EXPECT_EQ(ChoiceIndex(backend, "direct-vulkan"), 0u);
  EXPECT_EQ(ChoiceIndex(backend, "vulkan"), 0u);
  EXPECT_EQ(ChoiceIndex(backend, "opengl"), 1u);
  EXPECT_EQ(ChoiceIndex(backend, "system"), 1u);
  EXPECT_EQ(ChoiceIndex(backend, "gles"), 1u);
  EXPECT_EQ(ChoiceIndex(backend, "angle-vulkan"), 2u);
  EXPECT_EQ(ChoiceIndex(backend, "angle-swiftshader"), kNoChoice);

  const Setting& vsync = Find("graphics.vsync");
  EXPECT_EQ(ChoiceIndex(vsync, ""), 0u);
  EXPECT_EQ(ChoiceIndex(vsync, "off"), 3u);
}

TEST_F(SettingsModelTest, ToggleStateUsesTheDefaultWhenUnset) {
  const Setting& raw_mouse = Find("input.raw_mouse");
  EXPECT_FALSE(ToggleState(raw_mouse, ""));
  EXPECT_FALSE(ToggleState(raw_mouse, "false"));
  EXPECT_TRUE(ToggleState(raw_mouse, "true"));
}

TEST_F(SettingsModelTest, FrameRateReadsEveryForm) {
  EXPECT_EQ(ParseFrameRate("").mode, FrameRateMode::kRoblox);
  EXPECT_EQ(ParseFrameRate("-1").mode, FrameRateMode::kRoblox);
  EXPECT_EQ(ParseFrameRate("0").mode, FrameRateMode::kRoblox);
  EXPECT_EQ(ParseFrameRate("fast").mode, FrameRateMode::kRoblox);
  EXPECT_EQ(ParseFrameRate("display").mode, FrameRateMode::kDisplay);
  EXPECT_EQ(ParseFrameRate("unlimited").mode, FrameRateMode::kUnlimited);
  EXPECT_EQ(ParseFrameRate("144").mode, FrameRateMode::kCustom);
  EXPECT_EQ(ParseFrameRate("144").custom, 144);

  EXPECT_EQ(FrameRateValue({FrameRateMode::kRoblox, 144}), "");
  EXPECT_EQ(FrameRateValue({FrameRateMode::kDisplay, 0}), "display");
  EXPECT_EQ(FrameRateValue({FrameRateMode::kUnlimited, 0}), "unlimited");
  EXPECT_EQ(FrameRateValue({FrameRateMode::kCustom, 144}), "144");
  EXPECT_EQ(FrameRateValue({FrameRateMode::kCustom, 0}), "120");
}

}  // namespace
}  // namespace ui
}  // namespace mocktail
