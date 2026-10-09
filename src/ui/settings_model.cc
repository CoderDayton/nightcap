#include "ui/settings_model.h"

#include <charconv>
#include <cmath>

#include "runtime/runtime_config_bootstrap.h"
#include "runtime/runtime_config_editor.h"
#include "runtime/runtime_config_store.h"

namespace mocktail {
namespace ui {

const std::vector<SettingsPage>& SettingsPages() {
  static const std::vector<SettingsPage> pages = {
      {"graphics",
       "Graphics",
       "video-display-symbolic",
       "Display & input",
       {
           {"Rendering",
            {
                {"graphics.backend",
                 "Graphics backend",
                 "Use OpenGL if your GPU has no working Vulkan driver.",
                 SettingKind::kChoice,
                 {
                     {"direct-vulkan", "Vulkan"},
                     {"opengl", "OpenGL"},
                     {"angle-vulkan", "ANGLE on Vulkan"},
                 },
                 "direct-vulkan",
                 {
                     {"vulkan", "direct-vulkan"},
                     {"native-vulkan", "direct-vulkan"},
                     {"system", "opengl"},
                     {"gles", "opengl"},
                 }},
                {"graphics.frame_rate_limit",
                 "Frame rate limit",
                 "Roblox setting leaves the cap to the in-game menu.",
                 SettingKind::kFrameRate,
                 {
                     {"", "Roblox setting"},
                     {"display", "Match display"},
                     {"unlimited", "Unlimited"},
                     {"custom", "Custom"},
                 },
                 "",
                 {}},
                {"graphics.vsync",
                 "VSync",
                 "Lines frames up with the display to stop tearing.",
                 SettingKind::kChoice,
                 {
                     {"", "Default"},
                     {"auto", "Automatic"},
                     {"on", "On"},
                     {"off", "Off"},
                 },
                 "",
                 {}},
            }},
           {"Display",
            {
                {"window.high_dpi",
                 "High-DPI rendering",
                 "Render at the display's full pixel density. Sharper, but "
                 "slower.",
                 SettingKind::kToggle,
                 {},
                 "false",
                 {}},
                {"appearance.theme",
                 "Theme",
                 "Roblox account uses the theme saved on your account.",
                 SettingKind::kChoice,
                 {
                     {"roblox", "Roblox account"},
                     {"system", "Follow system"},
                     {"light", "Light"},
                     {"dark", "Dark"},
                 },
                 "roblox",
                 {}},
            }},
       }},
      {"input",
       "Input",
       "input-mouse-symbolic",
       "Display & input",
       {
           {"Mouse",
            {
                {"input.raw_mouse",
                 "Raw mouse input",
                 "Turn on if first-person aiming feels too slow or too fast "
                 "with fractional display scaling.",
                 SettingKind::kToggle,
                 {},
                 "false",
                 {}},
            }},
       }},
      {"performance",
       "Performance",
       "power-profile-performance-symbolic",
       "System",
       {
           {"Processor",
            {
                {"performance.multithreaded_rendering",
                 "Multithreaded rendering",
                 "Size Roblox's render queues from every CPU core.",
                 SettingKind::kToggle,
                 {},
                 "false",
                 {}},
                {"performance.physics_worker_mode",
                 "Physics workers",
                 "Throughput gives Roblox every core. Latency keeps Roblox's "
                 "own pool sizes.",
                 SettingKind::kChoice,
                 {
                     {"throughput", "Throughput"},
                     {"latency", "Latency"},
                     {"auto", "Automatic"},
                 },
                 "throughput",
                 {}},
                {"performance.gamemode",
                 "GameMode",
                 "Ask Feral GameMode to tune the system while you play.",
                 SettingKind::kChoice,
                 {
                     {"auto", "Automatic"},
                     {"on", "On"},
                     {"off", "Off"},
                 },
                 "auto",
                 {}},
            }},
           {"Memory",
            {
                {"performance.memory_limit_mb",
                 "Memory limit",
                 "Hard cap in MiB for Nightcap and Roblox. 0 turns it off.",
                 SettingKind::kNumber,
                 {},
                 "0",
                 {}},
            }},
       }},
      {"audio",
       "Audio",
       "audio-speakers-symbolic",
       "System",
       {
           {"Devices",
            {
                {"audio.output_device", "Output device", "",
                 SettingKind::kText, {}, "default", {}},
                {"audio.input_device", "Input device", "",
                 SettingKind::kText, {}, "default", {}},
            }},
       }},
      {"discord",
       "Discord",
       "user-available-symbolic",
       "Integrations",
       {
           {"Rich Presence",
            {
                {"integrations.discord_rpc.enabled",
                 "Show activity in Discord",
                 "Never signs in to Discord or reads an account token.",
                 SettingKind::kToggle,
                 {},
                 "true",
                 {}},
                {"integrations.discord_rpc.show_place_name",
                 "Show the experience name", "", SettingKind::kToggle, {},
                 "true", {}},
                {"integrations.discord_rpc.show_elapsed_time",
                 "Show elapsed time", "", SettingKind::kToggle, {}, "true",
                 {}},
            }},
           {"Join button",
            {
                {"integrations.discord_rpc.join.enabled",
                 "Let friends join",
                 "Adds a button that opens the experience you are in.",
                 SettingKind::kToggle,
                 {},
                 "true",
                 {}},
                {"integrations.discord_rpc.join.public_servers_only",
                 "Public servers only",
                 "Never share a private or reserved server.",
                 SettingKind::kToggle,
                 {},
                 "true",
                 {}},
                {"integrations.discord_rpc.join.button_label",
                 "Button label", "", SettingKind::kText, {}, "Join Server",
                 {}},
            }},
           {"Text",
            {
                {"integrations.discord_rpc.text.title", "Title", "",
                 SettingKind::kText, {}, "Roblox", {}},
                {"integrations.discord_rpc.text.playing", "Playing", "",
                 SettingKind::kText, {}, "Playing {place_name}", {}},
                {"integrations.discord_rpc.text.state", "Second line", "",
                 SettingKind::kText, {}, "by {creator_name}", {}},
                {"integrations.discord_rpc.text.browsing", "Browsing", "",
                 SettingKind::kText, {}, "Browsing experiences", {}},
                {"integrations.discord_rpc.text.joining", "Joining", "",
                 SettingKind::kText, {}, "Joining an experience", {}},
                {"integrations.discord_rpc.text.unknown_place",
                 "Unknown experience", "", SettingKind::kText, {},
                 "Unknown experience", {}},
            }},
           {"Images",
            {
                {"integrations.discord_rpc.images.large", "Large image", "",
                 SettingKind::kText, {}, "{place_icon}", {}},
                {"integrations.discord_rpc.images.large_text",
                 "Large image text", "", SettingKind::kText, {},
                 "{place_name}", {}},
                {"integrations.discord_rpc.images.small", "Small image", "",
                 SettingKind::kText, {}, "roblox_small", {}},
                {"integrations.discord_rpc.images.small_text",
                 "Small image text", "", SettingKind::kText, {}, "Roblox",
                 {}},
            }},
       }},
      {"advanced",
       "Advanced",
       "applications-engineering-symbolic",
       "Other",
       {
           {"Fast flags",
            {
                {"fflags",
                 "FFlag overrides",
                 "Edit fflags.json. Invalid JSON is refused.",
                 SettingKind::kEditor,
                 {},
                 "",
                 {}},
            }},
       }},
  };
  return pages;
}

bool IsAvailable(const Setting& setting) {
  return setting.kind != SettingKind::kEditor;
}

std::string NumberValue(double number) {
  return std::to_string(number > 0 ? std::llround(number) : 0LL);
}

std::size_t ChoiceIndex(const Setting& setting, std::string_view value) {
  if (value.empty()) {
    value = setting.default_value;
  }
  for (const auto& [alias, choice] : setting.aliases) {
    if (value == alias) {
      value = choice;
    }
  }
  for (std::size_t index = 0; index < setting.choices.size(); ++index) {
    if (setting.choices[index].value == value) {
      return index;
    }
  }
  return kNoChoice;
}

bool ToggleState(const Setting& setting, std::string_view value) {
  return (value.empty() ? setting.default_value : value) == "true";
}

FrameRate ParseFrameRate(std::string_view value) {
  FrameRate rate;
  if (value == "display") {
    rate.mode = FrameRateMode::kDisplay;
  } else if (value == "unlimited") {
    rate.mode = FrameRateMode::kUnlimited;
  } else {
    int custom = 0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), custom);
    if (parsed.ec == std::errc() &&
        parsed.ptr == value.data() + value.size() && custom > 0) {
      rate.mode = FrameRateMode::kCustom;
      rate.custom = custom;
    }
  }
  return rate;
}

std::string FrameRateValue(const FrameRate& rate) {
  switch (rate.mode) {
    case FrameRateMode::kDisplay:
      return "display";
    case FrameRateMode::kUnlimited:
      return "unlimited";
    case FrameRateMode::kCustom:
      return std::to_string(rate.custom > 0 ? rate.custom
                                            : kDefaultCustomFrameRate);
    case FrameRateMode::kRoblox:
      break;
  }
  return {};
}

SettingsDocument::SettingsDocument(std::filesystem::path config_file)
    : config_file_(std::move(config_file)) {}

bool SettingsDocument::Load(std::string* error) {
  const runtime::RuntimeConfigBootstrapResult ensured =
      runtime::EnsureRuntimeConfigFile(config_file_);
  if (!ensured) {
    *error = ensured.error;
    return false;
  }
  runtime::RuntimeConfigText read =
      runtime::ReadRuntimeConfigText(config_file_);
  if (!read) {
    *error = read.error;
    return false;
  }
  // A file the editor cannot read is reported now, not on the first change.
  const runtime::RuntimeConfigValue probe =
      runtime::GetRuntimeConfigValue(read.text, "graphics.backend");
  if (!probe) {
    *error = probe.error;
    return false;
  }
  text_ = std::move(read.text);
  return true;
}

std::string SettingsDocument::Value(std::string_view key) const {
  return runtime::GetRuntimeConfigValue(text_, key).value;
}

bool SettingsDocument::IsSet(std::string_view key) const {
  return runtime::GetRuntimeConfigValue(text_, key).found;
}

bool SettingsDocument::Set(std::string_view key, std::string_view value,
                           std::string* error) {
  runtime::RuntimeConfigEdit edit =
      value.empty() ? runtime::UnsetRuntimeConfigValue(text_, key)
                    : runtime::SetRuntimeConfigString(text_, key, value);
  return Save(std::move(edit.text), std::move(edit.error), error);
}

bool SettingsDocument::SetText(std::string_view key, std::string_view value,
                               std::string* error) {
  runtime::RuntimeConfigEdit edit =
      runtime::SetRuntimeConfigString(text_, key, value);
  return Save(std::move(edit.text), std::move(edit.error), error);
}

bool SettingsDocument::Save(std::string edited, std::string edit_error,
                            std::string* error) {
  if (!edit_error.empty()) {
    *error = std::move(edit_error);
    return false;
  }
  if (edited == text_) {
    return true;
  }
  const runtime::RuntimeConfigSaveResult saved =
      runtime::SaveRuntimeConfigText(config_file_, text_, edited);
  if (!saved) {
    *error = saved.error;
    // The file may have changed under the window. Show what it holds now.
    runtime::RuntimeConfigText read =
        runtime::ReadRuntimeConfigText(config_file_);
    if (read) {
      text_ = std::move(read.text);
    }
    return false;
  }
  text_ = std::move(edited);
  return true;
}

}  // namespace ui
}  // namespace mocktail
