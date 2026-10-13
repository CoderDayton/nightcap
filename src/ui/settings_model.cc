#include "ui/settings_model.h"

#define JSON_NOEXCEPTION 1
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <system_error>

#include "runtime/device_profile.h"
#include "runtime/frame_rate_policy.h"
#include "runtime/runtime_config_bootstrap.h"
#include "runtime/runtime_config_editor.h"
#include "runtime/runtime_config_store.h"
#include "services/client_settings_service.h"

namespace mocktail {
namespace ui {
namespace {

// The touch switch. The file holds it inside the device block, which the
// loader does not let input.touch_enabled be combined with.
constexpr std::string_view kTouchKey = "device.touch";
// Where a file with no device key holds it.
constexpr std::string_view kTouchKeyWithoutDevice = "input.touch_enabled";

// Whether Nightcap turns touch input on for the config `text`.
bool TouchInput(std::string_view text) {
  for (const std::string_view key : {kTouchKey, kTouchKeyWithoutDevice}) {
    const runtime::RuntimeConfigValue value =
        runtime::GetRuntimeConfigValue(text, key);
    if (value.found) {
      return value.value == "true";
    }
  }
  // Unset, the device decides.
  runtime::RuntimeConfigValue device =
      runtime::GetRuntimeConfigValue(text, "device");
  if (!device.found) {
    device = runtime::GetRuntimeConfigValue(text, "device.type");
  }
  const runtime::DeviceProfile* profile = runtime::FindDeviceProfile(
      device.found ? std::string_view(device.value)
                   : runtime::kDefaultDeviceProfileName);
  return profile != nullptr && profile->touch_enabled;
}

// `text` with touch input set to `value`, which is "true" or "false".
runtime::RuntimeConfigEdit SetTouchInput(std::string_view text,
                                         std::string_view value) {
  const runtime::RuntimeConfigValue preset =
      runtime::GetRuntimeConfigValue(text, "device");
  if (preset.found) {
    // A device given as one name has no place for the switch. It becomes
    // the block form of the same device.
    runtime::RuntimeConfigEdit block =
        runtime::SetRuntimeConfigString(text, "device.type", preset.value);
    if (!block) {
      return block;
    }
    return runtime::SetRuntimeConfigString(block.text, kTouchKey, value);
  }
  if (runtime::GetRuntimeConfigValue(text, "device.type").found) {
    return runtime::SetRuntimeConfigString(text, kTouchKey, value);
  }
  return runtime::SetRuntimeConfigString(text, kTouchKeyWithoutDevice, value);
}

// Accepts every value, and records where the parser gave up and the first
// name the outermost object lists twice.
class JsonSyntaxProbe final : public nlohmann::json_sax<nlohmann::json> {
 public:
  bool null() override { return true; }
  bool boolean(bool) override { return true; }
  bool number_integer(number_integer_t) override { return true; }
  bool number_unsigned(number_unsigned_t) override { return true; }
  bool number_float(number_float_t, const string_t&) override { return true; }
  bool string(string_t&) override { return true; }
  bool binary(binary_t&) override { return true; }
  bool start_object(std::size_t) override { return Enter(); }
  bool key(string_t& name) override {
    if (depth_ == 1 && !names_.insert(name).second && !repeated.has_value()) {
      repeated = name;
    }
    return true;
  }
  bool end_object() override { return Leave(); }
  bool start_array(std::size_t) override { return Enter(); }
  bool end_array() override { return Leave(); }
  bool parse_error(std::size_t position, const std::string&,
                   const nlohmann::detail::exception&) override {
    failed = true;
    fault = position;
    return false;
  }

  bool failed = false;
  // How many bytes the parser had read when it gave up.
  std::size_t fault = 0;
  std::optional<std::string> repeated;

 private:
  bool Enter() {
    ++depth_;
    return true;
  }
  bool Leave() {
    --depth_;
    return true;
  }

  // How many objects and arrays the parser is inside.
  std::size_t depth_ = 0;
  std::set<std::string> names_;
};

JsonSyntaxProbe ProbeJson(std::string_view text) {
  JsonSyntaxProbe probe;
  nlohmann::json::sax_parse(text, &probe, nlohmann::json::input_format_t::json,
                            true, true);
  return probe;
}

TextPosition FaultPosition(const JsonSyntaxProbe& probe,
                           std::string_view text) {
  // The fault is the last byte read.
  const std::size_t end =
      probe.fault == 0 ? 0 : std::min(probe.fault - 1, text.size());
  TextPosition position;
  for (std::size_t index = 0; index < end; ++index) {
    const char character = text[index];
    // A carriage return ends a line by itself, as it does in the editor,
    // unless a line feed follows and ends it.
    const bool lone_return =
        character == '\r' &&
        (index + 1 == text.size() || text[index + 1] != '\n');
    if (character == '\n' || lone_return) {
      ++position.line;
      position.column = 1;
    } else if ((static_cast<unsigned char>(character) & 0xC0) != 0x80) {
      // Only the first byte of a character counts.
      ++position.column;
    }
  }
  return position;
}

std::string FaultMessage(const TextPosition& position) {
  return "Not valid JSON at line " + std::to_string(position.line) +
         ", column " + std::to_string(position.column);
}

std::string Quoted(std::string_view name) {
  return "\"" + std::string(name) + "\"";
}

// Why Nightcap would not use `text` as fflags.json, or empty. The file's
// loader has the last word on a save; this names the flag at fault.
std::string FFlagsContentError(std::string_view text) {
  const JsonSyntaxProbe probe = ProbeJson(text);
  if (probe.failed) {
    return FaultMessage(FaultPosition(probe, text));
  }
  const nlohmann::json values =
      nlohmann::json::parse(text, nullptr, false, true);
  if (!values.is_object()) {
    return "fflags.json must be one JSON object, like "
           "{ \"FFlagName\": \"True\" }";
  }
  if (probe.repeated.has_value()) {
    return Quoted(*probe.repeated) + " is listed twice";
  }
  for (const auto& [name, value] : values.items()) {
    if (name.empty()) {
      return "A flag has an empty name";
    }
    if (!value.is_string() && !value.is_boolean() &&
        !value.is_number_integer()) {
      return Quoted(name) +
             ": the value must be text, true or false, or a whole number";
    }
  }
  return {};
}

// Whether `text` is UTF-8 with no NUL byte, which a text view needs.
bool IsUtf8Text(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    if (lead == 0) {
      return false;
    }
    if (lead < 0x80) {
      ++index;
      continue;
    }
    std::size_t length = 0;
    char32_t point = 0;
    // The smallest value this length may encode.
    char32_t least = 0;
    if ((lead & 0xE0) == 0xC0) {
      length = 2;
      point = lead & 0x1F;
      least = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      point = lead & 0x0F;
      least = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      point = lead & 0x07;
      least = 0x10000;
    } else {
      return false;
    }
    if (text.size() - index < length) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const unsigned char next =
          static_cast<unsigned char>(text[index + offset]);
      if ((next & 0xC0) != 0x80) {
        return false;
      }
      point = (point << 6) | (next & 0x3F);
    }
    if (point < least || point > 0x10FFFF ||
        (point >= 0xD800 && point <= 0xDFFF)) {
      return false;
    }
    index += length;
  }
  return true;
}

}  // namespace

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
           {"Touch",
            {
                {kTouchKey,
                 "Touch input",
                 "Turn on for a touch screen or tablet.",
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
                {"audio.output_device",
                 "Output device",
                 "",
                 SettingKind::kDevice,
                 {{"default", "System default"}},
                 "default",
                 {}},
                {"audio.input_device",
                 "Input device",
                 "Off keeps the microphone closed.",
                 SettingKind::kDevice,
                 {{"default", "System default"}, {"disabled", "Off"}},
                 "default",
                 {}},
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
                 "Edit fflags.json. Text that is not valid JSON is not "
                 "saved.",
                 SettingKind::kEditor,
                 {},
                 "",
                 {}},
            }},
           {"Updates",
            {
                {"updates.automatic",
                 "Update Roblox automatically",
                 "Off keeps the Roblox build you have. Roblox stops old "
                 "builds from joining after a while.",
                 SettingKind::kToggle,
                 {},
                 "true",
                 {}},
            }},
           {"Files",
            {
                {"config_folder",
                 "Open config folder",
                 "Where config.yaml and fflags.json are kept.",
                 SettingKind::kFolder,
                 {},
                 "",
                 {}},
            }},
       }},
  };
  return pages;
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

std::vector<DeviceChoice> DeviceChoices(const Setting& setting,
                                        const std::vector<std::string>& devices,
                                        std::string_view value) {
  std::vector<DeviceChoice> choices;
  for (const SettingChoice& choice : setting.choices) {
    choices.push_back({std::string(choice.value), std::string(choice.label)});
  }
  const auto listed = [&choices](std::string_view name) {
    return std::any_of(choices.begin(), choices.end(),
                       [name](const DeviceChoice& choice) {
                         return choice.value == name;
                       });
  };
  for (const std::string& device : devices) {
    if (device.empty() || listed(device) ||
        std::count(devices.begin(), devices.end(), device) != 1) {
      continue;
    }
    choices.push_back({device, device});
  }
  if (value.empty()) {
    value = setting.default_value;
  }
  if (listed(value)) {
    return choices;
  }
  // The input device can be given as "id:" and SDL's number for it.
  constexpr std::string_view kNumberPrefix = "id:";
  const std::string_view number =
      value.substr(std::min(kNumberPrefix.size(), value.size()));
  if (setting.key == "audio.input_device" &&
      value.substr(0, kNumberPrefix.size()) == kNumberPrefix &&
      !number.empty() &&
      number.find_first_not_of("0123456789") == std::string_view::npos) {
    choices.push_back(
        {std::string(value), "Device number " + std::string(number)});
    return choices;
  }
  const bool shared =
      std::find(devices.begin(), devices.end(), value) != devices.end();
  choices.push_back(
      {std::string(value),
       std::string(value) + (shared ? " (name not unique)" : " (not found)")});
  return choices;
}

bool ToggleState(const Setting& setting, std::string_view value) {
  return (value.empty() ? setting.default_value : value) == "true";
}

FrameRate ParseFrameRate(std::string_view value) {
  // The loader's own reading of the key, so the window and Nightcap cannot
  // disagree about what a value means.
  const runtime::FrameRatePolicy policy = runtime::ParseFrameRatePolicy(value);
  FrameRate rate;
  switch (policy.mode) {
    case runtime::FrameRateLimitMode::kDisplay:
      rate.mode = FrameRateMode::kDisplay;
      break;
    case runtime::FrameRateLimitMode::kUnlimited:
      rate.mode = FrameRateMode::kUnlimited;
      break;
    case runtime::FrameRateLimitMode::kFixed:
      rate.mode = FrameRateMode::kCustom;
      rate.custom = policy.fixed_fps;
      break;
    case runtime::FrameRateLimitMode::kUnmanaged:
    case runtime::FrameRateLimitMode::kInvalid:
      break;
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
  if (key == kTouchKey) {
    return TouchInput(text_) ? "true" : "false";
  }
  return runtime::GetRuntimeConfigValue(text_, key).value;
}

bool SettingsDocument::IsSet(std::string_view key) const {
  return runtime::GetRuntimeConfigValue(text_, key).found;
}

bool SettingsDocument::Set(std::string_view key, std::string_view value,
                           std::string* error) {
  runtime::RuntimeConfigEdit edit =
      key == kTouchKey ? SetTouchInput(text_, value)
      : value.empty()  ? runtime::UnsetRuntimeConfigValue(text_, key)
                       : runtime::SetRuntimeConfigString(text_, key, value);
  return Save(std::move(edit.text), std::move(edit.error), error);
}

bool SettingsDocument::SetText(std::string_view key, std::string_view value,
                               std::string* error) {
  runtime::RuntimeConfigEdit edit =
      runtime::SetRuntimeConfigString(text_, key, value);
  return Save(std::move(edit.text), std::move(edit.error), error);
}

bool SettingsDocument::Undo(std::string* error) {
  if (!undo_.has_value()) {
    return true;
  }
  std::string previous = std::move(*undo_);
  const bool saved = Save(std::move(previous), std::string(), error);
  // An undo is not itself a change to take back.
  undo_.reset();
  return saved;
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
    if (read && read.text != text_) {
      text_ = std::move(read.text);
      undo_.reset();
    }
    return false;
  }
  undo_ = std::move(text_);
  text_ = std::move(edited);
  ++saved_changes_;
  return true;
}

std::optional<TextPosition> JsonSyntaxFault(std::string_view text) {
  const JsonSyntaxProbe probe = ProbeJson(text);
  if (!probe.failed) {
    return std::nullopt;
  }
  return FaultPosition(probe, text);
}

std::string JsonSyntaxError(std::string_view text) {
  const std::optional<TextPosition> fault = JsonSyntaxFault(text);
  return fault.has_value() ? FaultMessage(*fault) : std::string();
}

FFlagsDocument::FFlagsDocument(std::filesystem::path file)
    : file_(std::move(file)) {}

runtime::RuntimeConfigText FFlagsDocument::Read() const {
  // No more of the file is read than Nightcap itself reads.
  runtime::RuntimeConfigText read = runtime::ReadOptionalTextFile(
      file_, static_cast<std::size_t>(services::kMaximumFflagsFileBytes));
  if (read && read.text.size() > services::kMaximumFflagsFileBytes) {
    read.error = "fflags.json is larger than 64 KiB, the most Nightcap "
                 "reads. Shorten or remove " +
                 file_.string();
  }
  return read;
}

bool FFlagsDocument::Load(std::string* error) {
  runtime::RuntimeConfigText read = Read();
  if (!read) {
    *error = read.error;
    return false;
  }
  if (!IsUtf8Text(read.text)) {
    *error = "fflags.json is not UTF-8 text. Fix or remove " + file_.string();
    return false;
  }
  disk_ = std::move(read.text);
  return true;
}

std::string FFlagsDocument::Text() const {
  if (disk_.find_first_not_of(" \t\r\n") == std::string::npos) {
    return "{\n}\n";
  }
  return disk_;
}

bool FFlagsDocument::Save(std::string_view text, std::string* error) {
  std::string content_error = FFlagsContentError(text);
  if (!content_error.empty()) {
    *error = std::move(content_error);
    return false;
  }
  // Nightcap refuses to start on a file its loader rejects, so the loader
  // itself checks the text.
  const runtime::RuntimeConfigSaveResult saved = runtime::SaveCheckedTextFile(
      file_, disk_, text, [](const std::filesystem::path& candidate) {
        return services::LoadAndMergeFflagsFile(candidate, "{}").error;
      });
  if (!saved) {
    *error = saved.error;
    runtime::RuntimeConfigText read = Read();
    if (!read) {
      *error = read.error;
    } else if (read.text != disk_) {
      disk_ = std::move(read.text);
      *error =
          "fflags.json changed on disk since it was opened. Save again to "
          "replace it.";
    }
    return false;
  }
  disk_ = std::string(text);
  return true;
}

}  // namespace ui
}  // namespace mocktail
