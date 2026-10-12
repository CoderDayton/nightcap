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

std::vector<std::string> Values(const std::vector<DeviceChoice>& choices) {
  std::vector<std::string> values;
  for (const DeviceChoice& choice : choices) {
    values.push_back(choice.value);
  }
  return values;
}

TEST_F(SettingsModelTest, DeviceRowsOfferTheirChoicesThenTheDevices) {
  const Setting& output = Find("audio.output_device");
  const Setting& input = Find("audio.input_device");
  ASSERT_EQ(output.kind, SettingKind::kDevice);
  ASSERT_EQ(input.kind, SettingKind::kDevice);

  const std::vector<std::string> devices = {"USB Headset", "Built-in Audio"};
  EXPECT_EQ(Values(DeviceChoices(output, devices, "")),
            (std::vector<std::string>{"default", "USB Headset",
                                      "Built-in Audio"}));
  const std::vector<DeviceChoice> inputs = DeviceChoices(input, devices, "");
  EXPECT_EQ(Values(inputs),
            (std::vector<std::string>{"default", "disabled", "USB Headset",
                                      "Built-in Audio"}));
  EXPECT_EQ(inputs[0].label, "System default");
  EXPECT_EQ(inputs[1].label, "Off");
  EXPECT_EQ(inputs[2].label, "USB Headset");
}

TEST_F(SettingsModelTest, ADeviceNightcapCouldNotPickByNameIsLeftOut) {
  const Setting& input = Find("audio.input_device");
  EXPECT_EQ(Values(DeviceChoices(
                input,
                {"Webcam", "USB Mic", "Webcam", "disabled", "default", ""},
                "")),
            (std::vector<std::string>{"default", "disabled", "USB Mic"}));
}

TEST_F(SettingsModelTest, ADeviceTheFileNamesStaysListedWhenItIsNotFound) {
  const Setting& input = Find("audio.input_device");
  for (const std::string value : {"Old Mic", "id:7"}) {
    const std::vector<DeviceChoice> choices =
        DeviceChoices(input, {"USB Mic"}, value);
    ASSERT_EQ(Values(choices),
              (std::vector<std::string>{"default", "disabled", "USB Mic",
                                        value}));
    EXPECT_EQ(choices.back().label, value + " (not found)");
  }
  // Two devices share the name the file holds, so neither can be picked.
  const std::vector<DeviceChoice> shared =
      DeviceChoices(input, {"Webcam", "Webcam"}, "Webcam");
  ASSERT_EQ(Values(shared),
            (std::vector<std::string>{"default", "disabled", "Webcam"}));
  EXPECT_EQ(shared.back().label, "Webcam (name not unique)");
  // A device that is found is listed once.
  EXPECT_EQ(Values(DeviceChoices(input, {"USB Mic"}, "USB Mic")),
            (std::vector<std::string>{"default", "disabled", "USB Mic"}));
}

TEST_F(SettingsModelTest, APickedDeviceReachesTheLoadedConfig) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  const std::string name = "Alder Lake: PCH-P #2 [HiFi]";
  ASSERT_TRUE(document.Set("audio.output_device", name, &error)) << error;
  ASSERT_TRUE(document.Set("audio.input_device", "disabled", &error)) << error;
  EXPECT_EQ(document.Value("audio.output_device"), name);

  const runtime::RuntimeConfigLoadResult loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.audio_output_device(), name);
  EXPECT_FALSE(loaded.config.microphone_enabled());
}

TEST_F(SettingsModelTest, UndoPutsTheFileBackAsItWasBeforeTheLastChange) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_FALSE(document.CanUndo());

  for (const std::string_view key : {"input.raw_mouse", "device.touch"}) {
    const std::string before = Read();
    ASSERT_TRUE(document.Set(key, "true", &error)) << key << ": " << error;
    ASSERT_NE(Read(), before) << key;
    ASSERT_TRUE(document.CanUndo()) << key;

    ASSERT_TRUE(document.Undo(&error)) << key << ": " << error;
    EXPECT_EQ(Read(), before) << key;
    EXPECT_EQ(document.Value(key), "false") << key;
    EXPECT_FALSE(document.CanUndo()) << key;
  }
}

TEST_F(SettingsModelTest, AChangeThatIsNotSavedLeavesNothingToUndo) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_FALSE(document.Set("graphics.backend", "directx", &error));
  EXPECT_FALSE(document.CanUndo());
  // Saving the value the file already holds changes nothing.
  ASSERT_TRUE(document.Set("graphics.backend",
                           document.Value("graphics.backend"), &error))
      << error;
  EXPECT_FALSE(document.CanUndo());
}

TEST_F(SettingsModelTest, UndoLeavesAHandEditMadeMeanwhileAlone) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  ASSERT_TRUE(document.Set("input.raw_mouse", "true", &error)) << error;
  const std::string edited = Read() + "# a note\n";
  Write(edited);

  EXPECT_FALSE(document.Undo(&error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(Read(), edited);
  EXPECT_FALSE(document.CanUndo());
  EXPECT_EQ(document.Value("input.raw_mouse"), "true");
}

TEST_F(SettingsModelTest, TouchInputIsASwitchThatReachesTheLoadedConfig) {
  const Setting& touch = Find("device.touch");
  EXPECT_EQ(touch.kind, SettingKind::kToggle);

  struct Case {
    // Empty stands for the first-run file.
    std::string_view file;
    std::string_view device;
    bool starts_on;
  };
  for (const Case& test : {
           Case{"", "pc-windows-11", false},
           Case{"device:\n  type: mobile\n  model: Tablet\n  mouse: true\n",
                "mobile-pixel-7", true},
           Case{"device:\n  type: mobile\n  model: Tablet\n  mouse: true\n"
                "  touch: false\n",
                "mobile-pixel-7", false},
           Case{"device:\n  type: pc\n  model: Tablet\n", "pc-windows-11",
                false},
           // A file from before the device key.
           Case{"input:\n  raw_mouse: true\n", "pc-windows-11", false},
           Case{"input:\n  touch_enabled: true\n", "pc-windows-11", true},
       }) {
    SCOPED_TRACE(std::string(test.file));
    std::filesystem::remove(file_);
    if (!test.file.empty()) {
      Write(test.file);
    }
    SettingsDocument document(file_);
    std::string error;
    ASSERT_TRUE(document.Load(&error)) << error;

    // The switch shows what Nightcap uses.
    runtime::RuntimeConfigLoadResult loaded = LoadFile();
    ASSERT_TRUE(loaded) << loaded.error;
    EXPECT_EQ(loaded.config.input_capabilities().touch_enabled,
              test.starts_on);
    EXPECT_EQ(ToggleState(touch, document.Value(touch.key)), test.starts_on);

    for (const bool on : {!test.starts_on, test.starts_on}) {
      ASSERT_TRUE(document.Set(touch.key, on ? "true" : "false", &error))
          << error << "\n" << Read();
      EXPECT_EQ(ToggleState(touch, document.Value(touch.key)), on) << Read();
      loaded = LoadFile();
      ASSERT_TRUE(loaded) << loaded.error << "\n" << Read();
      EXPECT_EQ(loaded.config.input_capabilities().touch_enabled, on)
          << Read();
      // Nothing else about the device changes.
      EXPECT_EQ(loaded.config.device_profile().name, test.device) << Read();
      EXPECT_TRUE(loaded.config.input_capabilities().mouse_enabled ||
                  test.device != "pc-windows-11")
          << Read();
    }
    if (test.file.find("Tablet") != std::string_view::npos) {
      EXPECT_EQ(loaded.config.device_profile().model, "Tablet") << Read();
    }
  }
}

TEST_F(SettingsModelTest, TouchInputStaysOnForADeviceWithNoOtherInput) {
  Write("device: mobile\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_EQ(document.Value("device.touch"), "true");
  EXPECT_FALSE(document.Set("device.touch", "false", &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(Read(), "device: mobile\n");
  EXPECT_EQ(document.Value("device.touch"), "true");
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

TEST_F(SettingsModelTest, AJsonFaultIsCountedInCharacters) {
  EXPECT_FALSE(JsonSyntaxFault("{\"a\": 1}").has_value());
  EXPECT_FALSE(JsonSyntaxFault("// note\n{}\n").has_value());

  // The two-byte letter is one column.
  const std::optional<TextPosition> fault =
      JsonSyntaxFault("{\n  \"\xc3\xa9\": }\n");
  ASSERT_TRUE(fault.has_value());
  EXPECT_EQ(fault->line, 2u);
  EXPECT_EQ(fault->column, 8u);
  EXPECT_EQ(JsonSyntaxError("{\n  \"\xc3\xa9\": }\n"),
            "Not valid JSON at line 2, column 8");

  // A line ends the way the editor ends it: at a lone carriage return too.
  for (const std::string_view text :
       {"{\r  \"a\": }\r", "{\r\n  \"a\": }\r\n"}) {
    const std::optional<TextPosition> returned = JsonSyntaxFault(text);
    ASSERT_TRUE(returned.has_value());
    EXPECT_EQ(returned->line, 2u);
    EXPECT_EQ(returned->column, 8u);
  }

  const std::optional<TextPosition> empty = JsonSyntaxFault("");
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty->line, 1u);
  EXPECT_EQ(empty->column, 1u);
}

TEST_F(SettingsModelTest, AnFFlagsRefusalSaysWhatIsWrong) {
  FFlagsDocument document(FFlagsFile());
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  const std::pair<std::string_view, std::string_view> cases[] = {
      {"[1, 2]\n", "one JSON object"},
      {"\"text\"\n", "one JSON object"},
      {"{\"DFIntA\": 1.5}\n", "\"DFIntA\""},
      {"{\"ok\": 1, \"FFlagB\": null}\n", "\"FFlagB\""},
      {"{\"FFlagC\": [1]}\n", "\"FFlagC\""},
      {"{\"\": 1}\n", "empty name"},
      {"{\"FFlagD\": 1, \"FFlagD\": 2}\n", "\"FFlagD\" is listed twice"},
  };
  for (const auto& [text, expected] : cases) {
    error.clear();
    EXPECT_FALSE(document.Save(text, &error)) << text;
    EXPECT_NE(error.find(expected), std::string::npos)
        << text << " gave: " << error;
    EXPECT_FALSE(std::filesystem::exists(FFlagsFile())) << text;
  }

  // A name used again inside a value is not a repeat.
  ASSERT_TRUE(document.Save("{\"a\": \"a\", \"b\": \"a\"}\n", &error))
      << error;
}

TEST_F(SettingsModelTest, AnFFlagsFileThatIsNotTextIsNotOpened) {
  for (const std::string& bytes :
       {std::string("{\"a\": \"\xff\xfe\"}\n"),
        std::string("{}\n\0{\"a\": 1}\n", 13)}) {
    std::ofstream(FFlagsFile(), std::ios::binary) << bytes;
    FFlagsDocument document(FFlagsFile());
    std::string error;
    EXPECT_FALSE(document.Load(&error));
    EXPECT_NE(error.find("UTF-8"), std::string::npos) << error;
    EXPECT_EQ(ReadFFlags(), bytes);
  }
}

TEST_F(SettingsModelTest, AnFFlagsFileOverTheReadLimitIsNotOpened) {
  const std::string bytes = "{}" + std::string(64 * 1024, ' ');
  std::ofstream(FFlagsFile(), std::ios::binary) << bytes;
  FFlagsDocument document(FFlagsFile());
  std::string error;
  EXPECT_FALSE(document.Load(&error));
  EXPECT_NE(error.find("64 KiB"), std::string::npos) << error;
  EXPECT_EQ(ReadFFlags(), bytes);
}

TEST_F(SettingsModelTest, AnFFlagsSymlinkIsRefusedAsASymlinkWhateverItsSize) {
  const std::filesystem::path target = directory_ / "large.json";
  std::ofstream(target, std::ios::binary)
      << "{}" + std::string(64 * 1024, ' ');
  std::filesystem::create_symlink(target, FFlagsFile());
  FFlagsDocument document(FFlagsFile());
  std::string error;
  EXPECT_FALSE(document.Load(&error));
  EXPECT_NE(error.find("symlink"), std::string::npos) << error;
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
