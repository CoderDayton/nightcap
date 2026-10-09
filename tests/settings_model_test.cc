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
#include "services/client_settings_service.h"

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

  // fflags.json beside config.yaml, in a folder that exists.
  std::filesystem::path FFlagsFile() {
    std::filesystem::create_directories(file_.parent_path());
    return file_.parent_path() / "fflags.json";
  }

  std::string ReadFFlags() {
    std::ostringstream contents;
    contents << std::ifstream(FFlagsFile(), std::ios::binary).rdbuf();
    return contents.str();
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

TEST_F(SettingsModelTest, EveryValueTheWindowOffersSavesAndLoads) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;

  for (const SettingsPage& page : SettingsPages()) {
    for (const SettingsGroup& group : page.groups) {
      for (const Setting& setting : group.settings) {
        if (setting.kind == SettingKind::kEditor) {
          continue;
        }
        if (setting.kind == SettingKind::kText) {
          for (const std::string value :
               {std::string("Custom: {place_name} #1"),
                std::string(setting.default_value)}) {
            ASSERT_TRUE(document.SetText(setting.key, value, &error))
                << setting.key << " = " << value << ": " << error;
            EXPECT_EQ(document.Value(setting.key), value) << setting.key;
            const runtime::RuntimeConfigLoadResult loaded = LoadFile();
            ASSERT_TRUE(loaded) << setting.key << " = " << value << ": "
                                << loaded.error;
          }
          continue;
        }
        std::vector<std::string> values;
        if (setting.kind == SettingKind::kNumber) {
          values = {NumberValue(4096), NumberValue(0)};
        } else if (setting.kind == SettingKind::kToggle) {
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
  ASSERT_TRUE(document.SetText("audio.output_device", "USB Headset", &error))
      << error;
  ASSERT_TRUE(document.SetText("integrations.discord_rpc.join.button_label",
                               "Hop in", &error))
      << error;

  const runtime::RuntimeConfigLoadResult loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.audio_output_device(), "USB Headset");
  EXPECT_EQ(loaded.config.discord_rpc().join_button_label, "Hop in");
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

TEST_F(SettingsModelTest, ATextCanBeClearedAndPutBackToItsDefault) {
  const std::string_view key = "integrations.discord_rpc.text.state";
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_FALSE(document.IsSet(key));

  ASSERT_TRUE(document.SetText(key, "", &error)) << error;
  EXPECT_TRUE(document.IsSet(key));
  EXPECT_EQ(document.Value(key), "");
  runtime::RuntimeConfigLoadResult loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.discord_rpc().text.state, "");

  ASSERT_TRUE(document.Set(key, "", &error)) << error;
  EXPECT_FALSE(document.IsSet(key));
  loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.discord_rpc().text.state, "by {creator_name}");
}

TEST_F(SettingsModelTest, ATextThatMustNotBeEmptyIsNotSavedEmpty) {
  const std::string_view key = "integrations.discord_rpc.text.playing";
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  const std::string before = Read();
  EXPECT_FALSE(document.SetText(key, "", &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(Read(), before);
  EXPECT_FALSE(document.IsSet(key));
}

TEST_F(SettingsModelTest, NumberValueIsAWholeNumberThatIsNotNegative) {
  EXPECT_EQ(NumberValue(0), "0");
  EXPECT_EQ(NumberValue(4096), "4096");
  EXPECT_EQ(NumberValue(4095.6), "4096");
  EXPECT_EQ(NumberValue(-3), "0");
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

TEST_F(SettingsModelTest, FFlagsStartAsAnEmptyObjectWhenThereIsNoFile) {
  FFlagsDocument document(FFlagsFile());
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_EQ(document.Text(), "{\n}\n");
  EXPECT_FALSE(std::filesystem::exists(FFlagsFile()));
}

TEST_F(SettingsModelTest, SavedFFlagsAreWhatNightcapLoads) {
  FFlagsDocument document(FFlagsFile());
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  const std::string text =
      "{\n  // keep the frame rate open\n  \"DFIntTaskSchedulerTargetFps\": 240,\n"
      "  \"FFlagDebugSkyGray\": true\n}\n";
  ASSERT_TRUE(document.Save(text, &error)) << error;
  EXPECT_EQ(ReadFFlags(), text);
  EXPECT_EQ(document.Text(), text);

  const services::FflagsMergeResult loaded =
      services::LoadAndMergeFflagsFile(FFlagsFile(), "{}");
  ASSERT_TRUE(loaded.error.empty()) << loaded.error;
  EXPECT_EQ(loaded.count, 2u);

  // A second save starts from what the first one wrote.
  ASSERT_TRUE(document.Save("{}\n", &error)) << error;
  EXPECT_EQ(ReadFFlags(), "{}\n");
}

TEST_F(SettingsModelTest, BrokenFFlagsAreRefusedWithTheLineAndColumn) {
  FFlagsDocument document(FFlagsFile());
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  ASSERT_TRUE(document.Save("{}\n", &error)) << error;

  EXPECT_FALSE(document.Save("{\n  \"a\": 1,\n  \"b\" 2\n}\n", &error));
  EXPECT_NE(error.find("line 3"), std::string::npos) << error;
  EXPECT_NE(error.find("column 7"), std::string::npos) << error;
  EXPECT_EQ(ReadFFlags(), "{}\n");

  EXPECT_FALSE(document.Save("", &error));
  EXPECT_EQ(ReadFFlags(), "{}\n");
}

TEST_F(SettingsModelTest, FFlagsNightcapWouldRejectAreRefused) {
  FFlagsDocument document(FFlagsFile());
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  for (const std::string_view text :
       {"[1, 2]\n", "{\"a\": {\"nested\": 1}}\n", "{\"a\": 1.5}\n",
        "{\"\": 1}\n", "{\"a\": null}\n"}) {
    error.clear();
    EXPECT_FALSE(document.Save(text, &error)) << text;
    EXPECT_FALSE(error.empty()) << text;
    EXPECT_FALSE(std::filesystem::exists(FFlagsFile())) << text;
  }
}

TEST_F(SettingsModelTest, FFlagsEditedByHandMeanwhileNeedASecondSave) {
  FFlagsDocument document(FFlagsFile());
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  ASSERT_TRUE(document.Save("{}\n", &error)) << error;

  std::ofstream(FFlagsFile(), std::ios::binary) << "{\"by_hand\": 1}\n";
  EXPECT_FALSE(document.Save("{\"mine\": 2}\n", &error));
  EXPECT_NE(error.find("Save again"), std::string::npos) << error;
  EXPECT_EQ(ReadFFlags(), "{\"by_hand\": 1}\n");

  ASSERT_TRUE(document.Save("{\"mine\": 2}\n", &error)) << error;
  EXPECT_EQ(ReadFFlags(), "{\"mine\": 2}\n");
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
