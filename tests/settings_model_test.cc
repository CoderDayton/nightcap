#include "ui/settings_model.h"

#include <gtest/gtest.h>
#include <stdlib.h>

#include <algorithm>
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
#include "update/update_config.h"

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

// A text each text row accepts.
std::string TextSample(std::string_view key) {
  if (key == "runtime.roblox_library") return "/opt/roblox/libroblox.so";
  if (key == "network.ca_bundle") return "/etc/ssl/custom.pem";
  if (key == "integrations.fleasion.ca_certificate") return "/etc/ssl/ca.crt";
  if (key == "integrations.discord_rpc.application_id") {
    return "123456789012345678";
  }
  if (key == "network.proxy") return "127.0.0.1:8080";
  return "Custom: {place_name} #1";
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
                        "Graphics", "Input", "VR", "Performance", "Audio",
                        "Network", "Discord", "Fleasion", "Advanced"}));
}

TEST_F(SettingsModelTest, EveryConfigKeyHasARow) {
  // Every key the loader and the updater accept, and the row that sets it.
  std::vector<std::pair<std::string_view, std::string_view>> keys = {
      {"device", "device"},
      {"device.type", "device"},
      {"compatibility.desktop_playability", "device"},
      {"input.touch_enabled", "device.touch"},
      {"network.proxy_host", "network.proxy"},
      {"network.proxy_port", "network.proxy"},
      {"vr.enabled", "vr.mode"},
  };
  for (const std::string_view key : {
           "device.platform", "device.name", "device.manufacturer",
           "device.model", "device.brand", "device.code", "device.sku",
           "device.soc_model", "device.touch", "device.mouse",
           "device.keyboard", "runtime.headless", "runtime.roblox_library",
           "appearance.theme", "graphics.backend", "graphics.frame_rate_limit",
           "graphics.vsync", "performance.multithreaded_rendering",
           "performance.physics_worker_mode", "performance.memory_limit_mb",
           "performance.gamemode", "performance.mangohud",
           "audio.output_device", "audio.input_device",
           "window.width", "window.height", "window.title", "window.high_dpi",
           "input.raw_mouse", "input.system_cursor", "input.hardware_cursor",
           "overlay.location", "overlay.location_offset", "overlay.fps",
           "overlay.ping", "overlay.position",
           "network.use_system_proxy", "network.proxy_scheme",
           "network.proxy_username", "network.proxy_password",
           "network.ca_bundle",
           "integrations.fleasion.enabled", "integrations.fleasion.proxy_mode",
           "integrations.fleasion.proxy_port",
           "integrations.fleasion.ca_certificate",
           "integrations.fleasion.clear_cache_on_launch",
           "integrations.discord_rpc.enabled",
           "integrations.discord_rpc.show_place_name",
           "integrations.discord_rpc.show_elapsed_time",
           "integrations.discord_rpc.application_id",
           "integrations.discord_rpc.join.enabled",
           "integrations.discord_rpc.join.public_servers_only",
           "integrations.discord_rpc.join.button_label",
           "integrations.discord_rpc.text.browsing",
           "integrations.discord_rpc.text.joining",
           "integrations.discord_rpc.text.playing",
           "integrations.discord_rpc.text.state",
           "integrations.discord_rpc.text.unknown_place",
           "integrations.discord_rpc.text.title",
           "integrations.discord_rpc.images.large",
           "integrations.discord_rpc.images.large_text",
           "integrations.discord_rpc.images.small",
           "integrations.discord_rpc.images.small_text", "updates.automatic",
           "updates.source", "updates.launch_after_update", "vr.mode",
           "vr.anti_aliasing", "vr.frame_rate_limit", "vr.spatial_toolbar"}) {
    keys.emplace_back(key, key);
  }
  for (const auto& [key, row] : keys) {
    EXPECT_EQ(Find(row).key, row) << key;
  }
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

TEST_F(SettingsModelTest, TheAdvancedPageOpensTheConfigFolder) {
  const SettingsPage& advanced = SettingsPages().back();
  ASSERT_EQ(advanced.name, "advanced");
  const Setting& folder = Find("config_folder");
  EXPECT_EQ(folder.kind, SettingKind::kFolder);
  EXPECT_EQ(&advanced.groups.back().settings.back(), &folder);
}

TEST_F(SettingsModelTest, TheAdvancedPageClearsTheRobloxCache) {
  const SettingsPage& advanced = SettingsPages().back();
  const Setting& cache = Find("roblox_cache");
  EXPECT_EQ(cache.kind, SettingKind::kCache);
  bool on_advanced = false;
  for (const SettingsGroup& group : advanced.groups) {
    for (const Setting& setting : group.settings) {
      on_advanced = on_advanced || &setting == &cache;
    }
  }
  EXPECT_TRUE(on_advanced);
}

TEST_F(SettingsModelTest, AutomaticUpdatesIsASwitchThatIsOnUntilTurnedOff) {
  const Setting& updates = Find("updates.automatic");
  EXPECT_EQ(updates.kind, SettingKind::kToggle);

  // The first-run file, then a file from before the updates section.
  for (const std::string_view file : {"", "graphics:\n  backend: opengl\n"}) {
    SCOPED_TRACE(std::string(file));
    std::filesystem::remove(file_);
    if (!file.empty()) {
      Write(file);
    }
    SettingsDocument document(file_);
    std::string error;
    ASSERT_TRUE(document.Load(&error)) << error;
    EXPECT_TRUE(ToggleState(updates, document.Value(updates.key)));

    ASSERT_TRUE(document.Set(updates.key, "false", &error)) << error;
    EXPECT_FALSE(ToggleState(updates, document.Value(updates.key)));
    EXPECT_NE(Read().find("updates:\n"), std::string::npos) << Read();
    EXPECT_NE(Read().find("\n  automatic: false\n"), std::string::npos)
        << Read();
    const runtime::RuntimeConfigLoadResult loaded = LoadFile();
    EXPECT_TRUE(loaded) << loaded.error;
  }
}

bool NeedsFixedProxy(std::string_view key) {
  return key == "network.proxy_scheme" || key == "network.proxy_username" ||
         key == "network.proxy_password";
}

TEST_F(SettingsModelTest, EveryValueTheWindowOffersSavesAndLoads) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;

  for (const SettingsPage& page : SettingsPages()) {
    for (const SettingsGroup& group : page.groups) {
      for (const Setting& setting : group.settings) {
        if (setting.kind == SettingKind::kEditor ||
            NeedsFixedProxy(setting.key)) {
          continue;
        }
        if (setting.kind == SettingKind::kText ||
            setting.kind == SettingKind::kFile) {
          const std::string value = TextSample(setting.key);
          ASSERT_TRUE(document.SetText(setting.key, value, &error))
              << setting.key << " = " << value << ": " << error;
          EXPECT_EQ(document.Value(setting.key), value) << setting.key;
          runtime::RuntimeConfigLoadResult loaded = LoadFile();
          ASSERT_TRUE(loaded) << setting.key << " = " << value << ": "
                              << loaded.error;
          // A text with no default goes back to unset.
          ASSERT_TRUE(setting.default_value.empty()
                          ? document.Set(setting.key, "", &error)
                          : document.SetText(setting.key,
                                             setting.default_value, &error))
              << setting.key << ": " << error;
          loaded = LoadFile();
          ASSERT_TRUE(loaded) << setting.key << ": " << loaded.error;
          continue;
        }
        // Each row ends on its default, so the next rows start from a file
        // Nightcap loads.
        std::vector<std::string> values;
        if (setting.kind == SettingKind::kNumber) {
          values = {NumberValue(std::min(4096.0, setting.maximum)),
                    std::string(setting.default_value)};
        } else if (setting.kind == SettingKind::kToggle) {
          const bool on = setting.default_value == "true";
          values = {on ? "false" : "true", on ? "true" : "false"};
        } else if (setting.kind == SettingKind::kFrameRate) {
          for (const FrameRateMode mode :
               {FrameRateMode::kDisplay, FrameRateMode::kUnlimited,
                FrameRateMode::kCustom, FrameRateMode::kRoblox}) {
            values.push_back(FrameRateSettingValue(setting, {mode, 144}));
          }
        } else {
          for (const SettingChoice& choice : setting.choices) {
            values.emplace_back(choice.value);
          }
          values.emplace_back(setting.default_value);
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

TEST_F(SettingsModelTest, VrValuesReachTheLoadedConfig) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  ASSERT_TRUE(document.Set("vr.mode", "auto", &error)) << error;
  ASSERT_TRUE(document.Set("vr.anti_aliasing", "smaa", &error)) << error;
  ASSERT_TRUE(document.Set("vr.spatial_toolbar", "false", &error)) << error;
  ASSERT_TRUE(document.Set(
      "vr.frame_rate_limit",
      FrameRateSettingValue(Find("vr.frame_rate_limit"),
                            {FrameRateMode::kRoblox, 0}),
      &error))
      << error;

  const runtime::RuntimeConfigLoadResult loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.vr_mode(), runtime::VrMode::kAuto);
  EXPECT_EQ(loaded.config.vr_anti_aliasing(), "smaa");
  EXPECT_FALSE(loaded.config.vr_spatial_toolbar());
  EXPECT_EQ(loaded.config.vr_frame_rate().mode,
            runtime::FrameRateLimitMode::kUnmanaged);
}

TEST_F(SettingsModelTest, TheOldVrSwitchShowsAsAModeAndMakesWayForOne) {
  Write("vr:\n  enabled: true\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_EQ(document.Value("vr.mode"), "on");
  EXPECT_TRUE(document.IsChanged(Find("vr.mode")));

  ASSERT_TRUE(document.Set("vr.mode", "auto", &error)) << error;
  EXPECT_EQ(Read().find("\n  enabled:"), std::string::npos) << Read();
  const runtime::RuntimeConfigLoadResult loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.vr_mode(), runtime::VrMode::kAuto);

  Write("vr:\n  enabled: false\n");
  ASSERT_TRUE(document.Load(&error)) << error;
  ASSERT_TRUE(document.Reset(Find("vr.mode"), &error)) << error;
  ASSERT_TRUE(LoadFile()) << Read();
  EXPECT_EQ(document.Value("vr.mode"), "");
  EXPECT_FALSE(document.IsChanged(Find("vr.mode")));
}

TEST_F(SettingsModelTest, AFrameRateRowStartsFromItsOwnDefault) {
  const Setting& vr = Find("vr.frame_rate_limit");
  const Setting& graphics = Find("graphics.frame_rate_limit");
  EXPECT_EQ(FrameRateSettingState(vr, "").mode, FrameRateMode::kUnlimited);
  EXPECT_EQ(FrameRateSettingState(graphics, "").mode, FrameRateMode::kRoblox);
  EXPECT_EQ(FrameRateSettingValue(vr, {FrameRateMode::kRoblox, 0}), "-1");
  EXPECT_EQ(FrameRateSettingValue(graphics, {FrameRateMode::kRoblox, 0}), "");
  EXPECT_EQ(FrameRateSettingValue(vr, {FrameRateMode::kDisplay, 0}),
            "display");
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
  const std::vector<DeviceChoice> missing =
      DeviceChoices(input, {"USB Mic"}, "Old Mic");
  ASSERT_EQ(Values(missing),
            (std::vector<std::string>{"default", "disabled", "USB Mic",
                                      "Old Mic"}));
  EXPECT_EQ(missing.back().label, "Old Mic (not found)");
  // The input device can be given by its number, which names no device.
  const std::vector<DeviceChoice> numbered =
      DeviceChoices(input, {"USB Mic"}, "id:7");
  ASSERT_EQ(Values(numbered),
            (std::vector<std::string>{"default", "disabled", "USB Mic",
                                      "id:7"}));
  EXPECT_EQ(numbered.back().label, "Device number 7");
  for (const std::string value : {"id:", "id:7a", "id: 7"}) {
    EXPECT_EQ(DeviceChoices(input, {}, value).back().label,
              value + " (not found)");
  }
  EXPECT_EQ(DeviceChoices(Find("audio.output_device"), {}, "id:7")
                .back()
                .label,
            "id:7 (not found)");
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

TEST_F(SettingsModelTest, OnlyAChangeThatReachesTheFileCountsAsSaved) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_EQ(document.saved_changes(), 0u);

  // The value the file already holds.
  ASSERT_TRUE(document.Set("input.raw_mouse", "false", &error)) << error;
  EXPECT_EQ(document.saved_changes(), 0u);

  ASSERT_TRUE(document.Set("input.raw_mouse", "true", &error)) << error;
  EXPECT_EQ(document.saved_changes(), 1u);

  EXPECT_FALSE(document.Set("graphics.backend", "directx", &error));
  EXPECT_EQ(document.saved_changes(), 1u);
}

TEST_F(SettingsModelTest, AForgottenChangeCannotBeUndone) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  ASSERT_TRUE(document.Set("input.raw_mouse", "true", &error)) << error;
  const std::string saved = Read();

  document.ForgetUndo();
  EXPECT_FALSE(document.CanUndo());
  EXPECT_TRUE(document.Undo(&error)) << error;
  EXPECT_EQ(Read(), saved);
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

TEST_F(SettingsModelTest, TheDevicePresetKeepsTheFileLoadable) {
  const Setting& device = Find("device");
  ASSERT_EQ(device.kind, SettingKind::kChoice);
  struct Case {
    std::string_view file;
    std::string_view shown;
    // What else the file must still say once the preset is console.
    std::string_view kept;
  };
  for (const Case& test : {
           Case{"device: pc\n", "pc", ""},
           Case{"device:\n  type: mobile\n  model: Tablet\n", "mobile",
                "model: Tablet"},
           Case{"compatibility:\n  desktop_playability: false\n",
                "mobile-pixel-7", ""},
           Case{"input:\n  touch_enabled: true\n", "", ""},
           Case{"device:\n  type: pc\n  touch: false\n  mouse: false\n", "pc",
                "type: console-ps5"},
       }) {
    SCOPED_TRACE(std::string(test.file));
    Write(test.file);
    SettingsDocument document(file_);
    std::string error;
    ASSERT_TRUE(document.Load(&error)) << error;
    EXPECT_EQ(document.Value(device.key), test.shown);
    EXPECT_NE(ChoiceIndex(device, document.Value(device.key)), kNoChoice);

    ASSERT_TRUE(document.Set(device.key, "console-ps5", &error))
        << error << "\n" << Read();
    EXPECT_EQ(document.Value(device.key), "console-ps5");
    const runtime::RuntimeConfigLoadResult loaded = LoadFile();
    ASSERT_TRUE(loaded) << loaded.error << "\n" << Read();
    EXPECT_EQ(loaded.config.device_profile().name, "console-ps5") << Read();
    EXPECT_NE(Read().find(test.kept), std::string::npos) << Read();
    // The switches of the old preset are left only as comments.
    for (const std::string_view key :
         {"  desktop_playability:", "  touch_enabled:", "  touch:",
          "  mouse:", "  keyboard:"}) {
      EXPECT_EQ(Read().find(key), std::string::npos) << key << "\n" << Read();
    }
    EXPECT_EQ(document.Value("device.touch"), "false");
    EXPECT_EQ(document.Value("device.keyboard"), "true");
  }
}

TEST_F(SettingsModelTest, DeviceDetailsTurnAPresetIntoADeviceBlock) {
  Write("device: mobile\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_EQ(document.Value("device.model"), "");
  ASSERT_TRUE(document.SetText("device.model", "Tablet", &error)) << error;
  EXPECT_EQ(document.Value("device.model"), "Tablet");
  EXPECT_EQ(document.Value("device"), "mobile");
  runtime::RuntimeConfigLoadResult loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error << "\n" << Read();
  EXPECT_EQ(loaded.config.device_profile().model, "Tablet");
  EXPECT_EQ(loaded.config.device_profile().device_class,
            runtime::DeviceClass::kMobile);

  ASSERT_TRUE(document.Set("device.model", "", &error)) << error;
  loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error << "\n" << Read();
  EXPECT_EQ(loaded.config.device_profile().name, "mobile-pixel-7");
}

TEST_F(SettingsModelTest, MouseAndKeyboardSwitchesShowAndSetTheDevice) {
  for (const std::string_view key : {"device.mouse", "device.keyboard"}) {
    SCOPED_TRACE(std::string(key));
    std::filesystem::remove(file_);
    SettingsDocument document(file_);
    std::string error;
    ASSERT_TRUE(document.Load(&error)) << error;
    const Setting& setting = Find(key);
    ASSERT_EQ(setting.kind, SettingKind::kToggle);
    EXPECT_TRUE(ToggleState(setting, document.Value(key)));

    ASSERT_TRUE(document.Set(key, "false", &error)) << error;
    EXPECT_FALSE(ToggleState(setting, document.Value(key)));
    const runtime::RuntimeConfigLoadResult loaded = LoadFile();
    ASSERT_TRUE(loaded) << loaded.error << "\n" << Read();
    const runtime::InputCapabilityConfig& input =
        loaded.config.input_capabilities();
    EXPECT_EQ(key == "device.mouse" ? input.mouse_enabled
                                    : input.keyboard_enabled,
              false);
    EXPECT_EQ(loaded.config.device_profile().name, "pc-windows-11");
  }
}

TEST_F(SettingsModelTest, TheFixedProxyIsOneHostAndPortRow) {
  const Setting& proxy = Find("network.proxy");
  ASSERT_EQ(proxy.kind, SettingKind::kText);
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_FALSE(document.IsSet(proxy.key));

  ASSERT_TRUE(document.SetText(proxy.key, "proxy.lan:3128", &error)) << error;
  EXPECT_TRUE(document.IsSet(proxy.key));
  EXPECT_EQ(document.Value(proxy.key), "proxy.lan:3128");
  runtime::RuntimeConfigLoadResult loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  ASSERT_TRUE(loaded.config.network_proxy().has_value());
  EXPECT_EQ(loaded.config.network_proxy()->host, "proxy.lan");
  EXPECT_EQ(loaded.config.network_proxy()->port, 3128);

  const std::string before = Read();
  for (const std::string_view refused : {"proxy.lan", "proxy.lan:", ":80"}) {
    EXPECT_FALSE(document.SetText(proxy.key, refused, &error)) << refused;
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(Read(), before);
  }

  ASSERT_TRUE(document.Set(proxy.key, "", &error)) << error;
  EXPECT_FALSE(document.IsSet(proxy.key));
  loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_FALSE(loaded.config.network_proxy().has_value());
}

TEST_F(SettingsModelTest, LocationAndPerformanceOverlayAreSeparateGroups) {
  const SettingsGroup* location = nullptr;
  const SettingsGroup* performance = nullptr;
  for (const SettingsPage& page : SettingsPages()) {
    for (const SettingsGroup& group : page.groups) {
      for (const Setting& setting : group.settings) {
        if (setting.key == "overlay.location") {
          location = &group;
        }
        if (setting.key == "overlay.position") {
          performance = &group;
        }
      }
    }
  }
  ASSERT_NE(location, nullptr);
  ASSERT_NE(performance, nullptr);
  EXPECT_EQ(location->title, "Server location");
  ASSERT_EQ(location->settings.size(), 2U);
  EXPECT_EQ(location->settings[0].key, "overlay.location");
  EXPECT_EQ(location->settings[1].key, "overlay.location_offset");
  EXPECT_EQ(performance->title, "Performance overlay");
  ASSERT_EQ(performance->settings.size(), 3U);
  EXPECT_EQ(performance->settings[0].key, "overlay.fps");
  EXPECT_EQ(performance->settings[1].key, "overlay.ping");
  EXPECT_EQ(performance->settings[2].key, "overlay.position");
}

TEST_F(SettingsModelTest, OnlyOverlaySettingsApplyWithoutARestart) {
  std::set<std::string_view> live;
  for (const SettingsPage& page : SettingsPages()) {
    for (const SettingsGroup& group : page.groups) {
      for (const Setting& setting : group.settings) {
        if (setting.live) {
          live.insert(setting.key);
        }
      }
    }
  }
  EXPECT_EQ(live, (std::set<std::string_view>{
                      "overlay.location", "overlay.location_offset",
                      "overlay.fps", "overlay.ping", "overlay.position"}));
}

TEST_F(SettingsModelTest, LocationSwitchReadsTheOldServerInfoKey) {
  std::string error;
  Write("version: 1\noverlay:\n  server_info: notice\n");
  SettingsDocument on(file_);
  ASSERT_TRUE(on.Load(&error)) << error;
  EXPECT_EQ(on.Value("overlay.location"), "true");

  Write("version: 1\noverlay:\n  server_info: off\n");
  SettingsDocument off(file_);
  ASSERT_TRUE(off.Load(&error)) << error;
  EXPECT_EQ(off.Value("overlay.location"), "false");

  Write("version: 1\noverlay:\n  server_info: always\n  location: false\n");
  SettingsDocument both(file_);
  ASSERT_TRUE(both.Load(&error)) << error;
  EXPECT_EQ(both.Value("overlay.location"), "false");

  ASSERT_TRUE(both.Set("overlay.location", "true", &error)) << error;
  EXPECT_EQ(both.Value("overlay.location"), "true");
}

TEST_F(SettingsModelTest, OverlayPositionNeedsAStatShown) {
  EXPECT_EQ(Find("overlay.location").kind, SettingKind::kToggle);
  EXPECT_EQ(Find("overlay.fps").kind, SettingKind::kToggle);
  EXPECT_EQ(Find("overlay.ping").kind, SettingKind::kToggle);
  EXPECT_EQ(Find("overlay.position").kind, SettingKind::kChoice);
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_EQ(document.Value("overlay.location"), "false");
  EXPECT_EQ(document.Value("overlay.position"), "bottom_left");
  EXPECT_FALSE(document.Set("overlay.position", "top_left", &error));
  EXPECT_FALSE(document.IsActive("overlay.position"));
  EXPECT_EQ(Find("overlay.location_offset").kind, SettingKind::kNumber);
  EXPECT_EQ(document.Value("overlay.location_offset"), "12");
  EXPECT_FALSE(document.IsActive("overlay.location_offset"));

  ASSERT_TRUE(document.Set("overlay.location", "true", &error)) << error;
  EXPECT_TRUE(document.IsActive("overlay.location_offset"));
  EXPECT_FALSE(document.IsActive("overlay.position"));
  ASSERT_TRUE(document.Set("overlay.ping", "true", &error)) << error;
  EXPECT_TRUE(document.IsActive("overlay.position"));
  ASSERT_TRUE(document.Set("overlay.ping", "false", &error)) << error;
  ASSERT_TRUE(document.Set("overlay.fps", "true", &error)) << error;
  EXPECT_TRUE(document.IsActive("overlay.position"));

  ASSERT_TRUE(document.Set("overlay.position", "bottom_right", &error))
      << error;
  runtime::RuntimeConfigLoadResult loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_TRUE(loaded.config.overlay().location);
  EXPECT_TRUE(loaded.config.overlay().fps);
  EXPECT_FALSE(loaded.config.overlay().ping);
  EXPECT_EQ(loaded.config.overlay().position,
            runtime::OverlayPosition::kBottomRight);
}

TEST_F(SettingsModelTest, ProxyTypeAndLoginNeedAFixedProxy) {
  EXPECT_EQ(Find("network.proxy_scheme").kind, SettingKind::kChoice);
  EXPECT_EQ(Find("network.proxy_username").kind, SettingKind::kText);
  EXPECT_EQ(Find("network.proxy_password").kind, SettingKind::kPassword);
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  for (const std::string_view key :
       {"network.proxy_scheme", "network.proxy_username",
        "network.proxy_password"}) {
    EXPECT_FALSE(document.IsActive(key)) << key;
  }

  ASSERT_TRUE(document.SetText("network.proxy", "proxy.lan:1080", &error))
      << error;
  EXPECT_TRUE(document.IsActive("network.proxy_scheme"));
  EXPECT_TRUE(document.IsActive("network.proxy_username"));
  EXPECT_FALSE(document.IsActive("network.proxy_password"));

  ASSERT_TRUE(document.Set("network.proxy_scheme", "socks5", &error)) << error;
  ASSERT_TRUE(document.SetText("network.proxy_username", "me", &error))
      << error;
  EXPECT_TRUE(document.IsActive("network.proxy_password"));
  runtime::RuntimeConfigLoadResult loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  ASSERT_TRUE(loaded.config.network_proxy().has_value());
  EXPECT_EQ(loaded.config.network_proxy()->scheme, "socks5h");
  EXPECT_EQ(loaded.config.network_proxy()->username, "me");
  EXPECT_EQ(loaded.config.network_proxy()->password, "");

  ASSERT_TRUE(document.SetText("network.proxy_password", "p@ss word", &error))
      << error;
  ASSERT_TRUE(document.SetText("network.proxy_password", "", &error)) << error;
  ASSERT_TRUE(document.SetText("network.proxy_password", "p@ss word", &error))
      << error;
  loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.network_proxy()->password, "p@ss word");

  ASSERT_TRUE(document.Set("network.proxy_username", "", &error)) << error;
  EXPECT_FALSE(document.IsSet("network.proxy_password"));
  ASSERT_TRUE(document.Set("network.proxy_username", "me", &error)) << error;
  ASSERT_TRUE(document.Set("network.proxy", "", &error)) << error;
  for (const std::string_view key :
       {"network.proxy_scheme", "network.proxy_username",
        "network.proxy_password"}) {
    EXPECT_FALSE(document.IsSet(key)) << key;
  }
  loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_FALSE(loaded.config.network_proxy().has_value());
}

TEST_F(SettingsModelTest, NumberRowsKeepToTheRangeTheLoaderAccepts) {
  EXPECT_EQ(Find("window.width").minimum, 1);
  EXPECT_EQ(Find("window.height").minimum, 1);
  EXPECT_EQ(Find("integrations.fleasion.proxy_port").minimum, 1);
  EXPECT_EQ(Find("integrations.fleasion.proxy_port").maximum, 65535);
  EXPECT_EQ(Find("performance.memory_limit_mb").minimum, 0);
}

TEST_F(SettingsModelTest, TheNewRowsReachTheLoadedConfig) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  ASSERT_TRUE(document.Set("window.width", "1600", &error)) << error;
  ASSERT_TRUE(document.SetText("window.title", "Nightcap", &error)) << error;
  ASSERT_TRUE(document.Set("runtime.headless", "true", &error)) << error;
  ASSERT_TRUE(document.Set("network.use_system_proxy", "true", &error))
      << error;
  ASSERT_TRUE(document.Set("integrations.fleasion.proxy_mode", "hosts", &error))
      << error;
  ASSERT_TRUE(document.SetText("integrations.discord_rpc.application_id",
                               "123456789012345678", &error))
      << error;
  ASSERT_TRUE(document.Set("updates.source", "auto", &error)) << error;
  ASSERT_TRUE(document.Set("updates.launch_after_update", "true", &error))
      << error;

  const runtime::RuntimeConfigLoadResult loaded = LoadFile();
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.window().width, 1600);
  EXPECT_EQ(loaded.config.window().title, "Nightcap");
  EXPECT_TRUE(loaded.config.headless());
  EXPECT_TRUE(loaded.config.use_system_proxy());
  EXPECT_EQ(loaded.config.fleasion_proxy_mode(), "hosts");
  EXPECT_EQ(loaded.config.discord_rpc().application_id, "123456789012345678");
  const update::UpdateConfigResult updates = update::LoadUpdateConfig(file_);
  ASSERT_TRUE(updates) << updates.error;
  EXPECT_EQ(updates.config.source, "auto");
  EXPECT_TRUE(updates.config.launch_after_update);
}

// A value for `setting` other than its default, or nothing when the row
// cannot take one here.
std::optional<std::string> ChangedSample(const Setting& setting) {
  switch (setting.kind) {
    case SettingKind::kToggle:
      return ToggleState(setting, setting.default_value) ? "false" : "true";
    case SettingKind::kChoice:
    case SettingKind::kDevice:
      for (const SettingChoice& choice : setting.choices) {
        if (!choice.value.empty() &&
            ChoiceIndex(setting, choice.value) !=
                ChoiceIndex(setting, setting.default_value)) {
          return std::string(choice.value);
        }
      }
      return std::nullopt;
    case SettingKind::kFrameRate:
      return "144";
    case SettingKind::kNumber: {
      const double value = std::stod(std::string(setting.default_value));
      return NumberValue(value + setting.step <= setting.maximum
                             ? value + setting.step
                             : value - setting.step);
    }
    case SettingKind::kText:
    case SettingKind::kFile:
      return TextSample(setting.key);
    case SettingKind::kEditor:
    case SettingKind::kFolder:
    case SettingKind::kCache:
      return std::nullopt;
  }
  return std::nullopt;
}

TEST_F(SettingsModelTest, ANewFileHasNoChangedRows) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  for (const SettingsPage& page : SettingsPages()) {
    for (const SettingsGroup& group : page.groups) {
      for (const Setting& setting : group.settings) {
        EXPECT_FALSE(document.IsChanged(setting))
            << setting.key << " = " << document.Value(setting.key);
      }
    }
  }
}

std::size_t ChangedCount(const SettingsDocument& document) {
  std::size_t count = 0;
  for (const SettingsPage& page : SettingsPages()) {
    for (const SettingsGroup& group : page.groups) {
      for (const Setting& setting : group.settings) {
        count += document.IsChanged(setting) ? 1 : 0;
      }
    }
  }
  return count;
}

TEST_F(SettingsModelTest, AChangedRowIsTheOnlyOneAndResetPutsItBack) {
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  for (const SettingsPage& page : SettingsPages()) {
    for (const SettingsGroup& group : page.groups) {
      for (const Setting& setting : group.settings) {
        const std::optional<std::string> value = ChangedSample(setting);
        if (!value.has_value() || NeedsFixedProxy(setting.key)) {
          continue;
        }
        SCOPED_TRACE(std::string(setting.key) + " = " + *value);
        const bool text = setting.kind == SettingKind::kText ||
                          setting.kind == SettingKind::kFile;
        ASSERT_TRUE(text ? document.SetText(setting.key, *value, &error)
                         : document.Set(setting.key, *value, &error))
            << error;
        EXPECT_TRUE(document.IsChanged(setting));
        EXPECT_EQ(ChangedCount(document), 1u);

        ASSERT_TRUE(document.Reset(setting, &error)) << error;
        EXPECT_FALSE(document.IsChanged(setting)) << document.Value(setting.key);
        EXPECT_EQ(ChangedCount(document), 0u);
        const runtime::RuntimeConfigLoadResult loaded = LoadFile();
        ASSERT_TRUE(loaded) << loaded.error << "\n" << Read();
      }
    }
  }
}

TEST_F(SettingsModelTest, DeviceInputsCompareWithTheChosenDevice) {
  Write("device: mobile\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_TRUE(document.IsChanged(Find("device")));
  // A phone has touch on and no mouse, so neither switch counts as changed.
  EXPECT_FALSE(document.IsChanged(Find("device.touch")));
  EXPECT_FALSE(document.IsChanged(Find("device.mouse")));

  ASSERT_TRUE(document.Set("device.mouse", "true", &error)) << error;
  EXPECT_TRUE(document.IsChanged(Find("device.mouse")));
  ASSERT_TRUE(document.Reset(Find("device.mouse"), &error)) << error;
  EXPECT_FALSE(document.IsChanged(Find("device.mouse")));
  EXPECT_EQ(document.Value("device"), "mobile");
}

TEST_F(SettingsModelTest, AnAliasOfTheDefaultIsNotAChange) {
  Write("device: pc\ngraphics:\n  vsync: false\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_FALSE(document.IsChanged(Find("device")));
}

TEST_F(SettingsModelTest, ResetTakesTheKeyOutOfTheFile) {
  Write("graphics:\n  vsync: off\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  for (const std::string_view key : {"window.title", "runtime.headless"}) {
    SCOPED_TRACE(key);
    const Setting& setting = Find(key);
    ASSERT_TRUE(setting.kind == SettingKind::kText
                    ? document.SetText(key, "Custom", &error)
                    : document.Set(key, "true", &error))
        << error;
    ASSERT_TRUE(document.Reset(setting, &error)) << error;
    EXPECT_FALSE(document.IsSet(key)) << Read();
  }
}

TEST_F(SettingsModelTest, ResetCanTakeOutTheLastKey) {
  Write("graphics:\n  vsync: off\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  ASSERT_TRUE(document.Reset(Find("graphics.vsync"), &error)) << error;
  EXPECT_FALSE(document.IsChanged(Find("graphics.vsync"))) << Read();
  const runtime::RuntimeConfigLoadResult loaded = LoadFile();
  EXPECT_TRUE(loaded) << loaded.error << "\n" << Read();

  ASSERT_TRUE(document.Set("graphics.vsync", "off", &error)) << error;
  EXPECT_TRUE(document.IsChanged(Find("graphics.vsync"))) << Read();
  const runtime::RuntimeConfigLoadResult reloaded = LoadFile();
  EXPECT_TRUE(reloaded) << reloaded.error << "\n" << Read();
}

TEST_F(SettingsModelTest, ResetTakesADeviceInputSwitchOut) {
  Write("device:\n  type: pc\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  for (const std::string_view key :
       {"device.touch", "device.mouse", "device.keyboard"}) {
    SCOPED_TRACE(key);
    const std::string flipped =
        document.Value(key) == "true" ? "false" : "true";
    ASSERT_TRUE(document.Set(key, flipped, &error)) << error;
    ASSERT_TRUE(document.Reset(Find(key), &error)) << error;
    EXPECT_FALSE(document.IsSet(key)) << Read();
    EXPECT_FALSE(document.IsSet("input.touch_enabled")) << Read();
  }
}

TEST_F(SettingsModelTest, ASwitchedOffBlockLeavesItsOtherRowsInactive) {
  Write("integrations:\n  discord_rpc:\n    join:\n      enabled: false\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_TRUE(document.IsActive("integrations.discord_rpc.join.enabled"));
  EXPECT_FALSE(document.IsActive("integrations.discord_rpc.join.button_label"));
  EXPECT_TRUE(document.IsActive("integrations.discord_rpc.text.title"));
  EXPECT_TRUE(document.IsActive("graphics.vsync"));

  ASSERT_TRUE(document.Set("integrations.discord_rpc.enabled", "false", &error))
      << error;
  EXPECT_TRUE(document.IsActive("integrations.discord_rpc.enabled"));
  EXPECT_FALSE(document.IsActive("integrations.discord_rpc.join.enabled"));
  EXPECT_FALSE(document.IsActive("integrations.discord_rpc.text.title"));
}

TEST_F(SettingsModelTest, TheSystemProxyLeavesAnEmptyFixedProxyInactive) {
  Write("network:\n  use_system_proxy: true\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_FALSE(document.IsActive("network.proxy"));

  // A fixed proxy stays editable, so it can be cleared.
  Write("network:\n  proxy_host: 127.0.0.1\n  proxy_port: 8080\n");
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_TRUE(document.IsActive("network.proxy"));
}

TEST_F(SettingsModelTest, HostsRoutingLeavesTheFleasionPortInactive) {
  Write("integrations:\n  fleasion:\n    enabled: true\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_TRUE(document.IsActive("integrations.fleasion.proxy_port"));

  ASSERT_TRUE(
      document.Set("integrations.fleasion.proxy_mode", "hosts", &error))
      << error;
  EXPECT_FALSE(document.IsActive("integrations.fleasion.proxy_port"));
  EXPECT_TRUE(document.IsActive("integrations.fleasion.ca_certificate"));
}

TEST_F(SettingsModelTest, ClearingOnLaunchNeedsFleasion) {
  Write("integrations:\n  fleasion:\n    enabled: false\n");
  SettingsDocument document(file_);
  std::string error;
  ASSERT_TRUE(document.Load(&error)) << error;
  EXPECT_FALSE(
      document.IsActive("integrations.fleasion.clear_cache_on_launch"));
  ASSERT_TRUE(document.Set("integrations.fleasion.enabled", "true", &error))
      << error;
  EXPECT_TRUE(
      document.IsActive("integrations.fleasion.clear_cache_on_launch"));
}

TEST(SettingsPagesTest, TextGroupsExplainTheirPlaceholders) {
  for (const SettingsPage& page : SettingsPages()) {
    for (const SettingsGroup& group : page.groups) {
      if (group.title != "Text" && group.title != "Images") {
        continue;
      }
      SCOPED_TRACE(group.title);
      for (const std::string_view placeholder :
           {"{place_name}", "{place_icon}"}) {
        EXPECT_NE(group.description.find(placeholder), std::string_view::npos);
      }
    }
  }
}

}  // namespace
}  // namespace ui
}  // namespace mocktail
