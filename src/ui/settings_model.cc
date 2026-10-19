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
constexpr std::string_view kMouseKey = "device.mouse";
constexpr std::string_view kKeyboardKey = "device.keyboard";
constexpr std::string_view kDeviceKey = "device";
constexpr std::string_view kDeviceTypeKey = "device.type";
// The older way to pick a PC or a phone. The loader refuses it beside a
// device.
constexpr std::string_view kPlayabilityKey =
    "compatibility.desktop_playability";
constexpr std::string_view kVrModeKey = "vr.mode";
constexpr std::string_view kVrEnabledKey = "vr.enabled";
constexpr std::string_view kProxyKey = "network.proxy";
constexpr std::string_view kProxyHostKey = "network.proxy_host";
constexpr std::string_view kProxyPortKey = "network.proxy_port";
constexpr std::string_view kProxySchemeKey = "network.proxy_scheme";
constexpr std::string_view kProxyUsernameKey = "network.proxy_username";
constexpr std::string_view kProxyPasswordKey = "network.proxy_password";

runtime::RuntimeConfigValue Get(std::string_view text, std::string_view key) {
  return runtime::GetRuntimeConfigValue(text, key);
}

using ValueCache =
    std::map<std::string, runtime::RuntimeConfigValue, std::less<>>;

class Reader {
 public:
  explicit Reader(std::string_view text, ValueCache* cache = nullptr)
      : text_(text), cache_(cache != nullptr ? cache : &own_) {}
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  const runtime::RuntimeConfigValue& Get(std::string_view key) const {
    auto found = cache_->find(key);
    if (found == cache_->end()) {
      found = cache_
                  ->emplace(std::string(key),
                            runtime::GetRuntimeConfigValue(text_, key))
                  .first;
    }
    return found->second;
  }

 private:
  std::string_view text_;
  mutable ValueCache own_;
  ValueCache* cache_;
};

// The preset the file picks, as the file spells it. Empty when it picks none.
std::string DevicePreset(const Reader& file) {
  for (const std::string_view key : {kDeviceKey, kDeviceTypeKey}) {
    if (const runtime::RuntimeConfigValue& device = file.Get(key);
        device.found) {
      return device.value;
    }
  }
  if (const runtime::RuntimeConfigValue& playability =
          file.Get(kPlayabilityKey);
      playability.found) {
    const bool desktop = playability.value != "0" &&
                         playability.value != "off" &&
                         playability.value != "false";
    return desktop ? "pc-windows-11" : "mobile-pixel-7";
  }
  return {};
}

bool HasYamlContent(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = std::min(text.find('\n', start), text.size());
    const std::size_t first = text.find_first_not_of(" \t\r", start);
    if (first < end && text[first] != '#') {
      return true;
    }
    start = end + 1;
  }
  return false;
}

// Keys cannot be added next to the `{}` that Save writes into an empty file.
std::string WithoutEmptyMapping(std::string_view text) {
  std::string kept;
  bool dropped = false;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = std::min(text.find('\n', start), text.size());
    std::string_view line = text.substr(start, end - start);
    const std::size_t first = line.find_first_not_of(" \t\r");
    const std::size_t last = line.find_last_not_of(" \t\r");
    if (first == std::string_view::npos || line[first] == '#') {
      kept.append(text.substr(start, std::min(end + 1, text.size()) - start));
    } else if (!dropped && line.substr(first, last - first + 1) == "{}") {
      dropped = true;
    } else {
      return std::string(text);
    }
    start = end + 1;
  }
  return kept;
}

// What the file's device turns on for the input `key`.
bool PresetInput(const Reader& file, std::string_view key) {
  const std::string preset = DevicePreset(file);
  const runtime::DeviceProfile* profile = runtime::FindDeviceProfile(
      preset.empty() ? runtime::kDefaultDeviceProfileName
                     : std::string_view(preset));
  if (profile == nullptr) {
    return false;
  }
  return key == kTouchKey   ? profile->touch_enabled
         : key == kMouseKey ? profile->mouse_enabled
                            : profile->keyboard_enabled;
}

bool DeviceInput(const Reader& file, std::string_view key) {
  if (const runtime::RuntimeConfigValue& value = file.Get(key); value.found) {
    return value.value == "true";
  }
  if (key == kTouchKey) {
    if (const runtime::RuntimeConfigValue& value =
            file.Get(kTouchKeyWithoutDevice);
        value.found) {
      return value.value == "true";
    }
  }
  return PresetInput(file, key);
}

// Runs `step` on the text of `edit`, unless `edit` already failed.
template <typename Step>
runtime::RuntimeConfigEdit Then(runtime::RuntimeConfigEdit edit, Step step) {
  return edit ? step(edit.text) : edit;
}

runtime::RuntimeConfigEdit Unchanged(std::string_view text) {
  return {std::string(text), {}};
}

runtime::RuntimeConfigEdit UnsetIfSet(std::string_view text,
                                      std::string_view key) {
  return Get(text, key).found ? runtime::UnsetRuntimeConfigValue(text, key)
                              : Unchanged(text);
}

// `text` with the device in block form: a type, the touch switch moved in
// from input.touch_enabled, and no desktop_playability.
runtime::RuntimeConfigEdit ToDeviceBlock(std::string_view text) {
  if (Get(text, kDeviceTypeKey).found) {
    return Unchanged(text);
  }
  const std::string preset = DevicePreset(Reader(text));
  const runtime::RuntimeConfigValue touch = Get(text, kTouchKeyWithoutDevice);
  runtime::RuntimeConfigEdit edit = runtime::SetRuntimeConfigString(
      text, kDeviceTypeKey,
      preset.empty() ? runtime::kDefaultDeviceProfileName
                     : std::string_view(preset));
  if (touch.found) {
    edit = Then(std::move(edit), [&touch](std::string_view moved) {
      return Then(runtime::SetRuntimeConfigString(moved, kTouchKey, touch.value),
                  [](std::string_view current) {
                    return runtime::UnsetRuntimeConfigValue(
                        current, kTouchKeyWithoutDevice);
                  });
    });
  }
  return Then(std::move(edit), [](std::string_view current) {
    return UnsetIfSet(current, kPlayabilityKey);
  });
}

// A new preset brings its own touch, mouse and keyboard, so the switches set
// for the old one are dropped.
runtime::RuntimeConfigEdit SetDevicePreset(std::string_view text,
                                           std::string_view value) {
  const std::string_view key =
      Get(text, kDeviceTypeKey).found ? kDeviceTypeKey : kDeviceKey;
  runtime::RuntimeConfigEdit edit = Unchanged(text);
  for (const std::string_view dropped :
       {kPlayabilityKey, kTouchKeyWithoutDevice, kTouchKey, kMouseKey,
        kKeyboardKey}) {
    edit = Then(std::move(edit), [dropped](std::string_view current) {
      return UnsetIfSet(current, dropped);
    });
  }
  return Then(std::move(edit), [key, value](std::string_view current) {
    return runtime::SetRuntimeConfigString(current, key, value);
  });
}

std::string VrMode(const Reader& file) {
  if (const runtime::RuntimeConfigValue& mode = file.Get(kVrModeKey);
      mode.found) {
    return mode.value;
  }
  if (const runtime::RuntimeConfigValue& enabled = file.Get(kVrEnabledKey);
      enabled.found) {
    return enabled.value == "true" ? "on" : "off";
  }
  return {};
}

runtime::RuntimeConfigEdit SetVrMode(std::string_view text,
                                     std::string_view value) {
  return Then(UnsetIfSet(text, kVrEnabledKey),
              [value](std::string_view current) {
                return value.empty()
                           ? UnsetIfSet(current, kVrModeKey)
                           : runtime::SetRuntimeConfigString(
                                 current, kVrModeKey, value);
              });
}

std::string ProxyValue(const Reader& file) {
  const runtime::RuntimeConfigValue& host = file.Get(kProxyHostKey);
  const runtime::RuntimeConfigValue& port = file.Get(kProxyPortKey);
  return host.found && port.found ? host.value + ":" + port.value
                                  : std::string();
}

// `text` with the fixed proxy set from "host:port", or unset when empty.
runtime::RuntimeConfigEdit SetProxy(std::string_view text,
                                    std::string_view value) {
  if (value.empty()) {
    runtime::RuntimeConfigEdit edit = Unchanged(text);
    for (const std::string_view dropped :
         {kProxyHostKey, kProxyPortKey, kProxySchemeKey, kProxyUsernameKey,
          kProxyPasswordKey}) {
      edit = Then(std::move(edit), [dropped](std::string_view current) {
        return UnsetIfSet(current, dropped);
      });
    }
    return edit;
  }
  const std::size_t colon = value.rfind(':');
  if (colon == std::string_view::npos || colon == 0 ||
      colon + 1 == value.size()) {
    return {{}, "Write the proxy as host:port, like 127.0.0.1:8080"};
  }
  return Then(runtime::SetRuntimeConfigString(text, kProxyHostKey,
                                              value.substr(0, colon)),
              [value, colon](std::string_view current) {
                return runtime::SetRuntimeConfigString(
                    current, kProxyPortKey, value.substr(colon + 1));
              });
}

runtime::RuntimeConfigEdit SetProxyLogin(std::string_view text,
                                         std::string_view username) {
  if (username.empty()) {
    return Then(UnsetIfSet(text, kProxyUsernameKey),
                [](std::string_view current) {
                  return UnsetIfSet(current, kProxyPasswordKey);
                });
  }
  return Then(runtime::SetRuntimeConfigString(text, kProxyUsernameKey,
                                              username),
              [](std::string_view current) {
                return Get(current, kProxyPasswordKey).found
                           ? Unchanged(current)
                           : runtime::SetRuntimeConfigString(
                                 current, kProxyPasswordKey, "");
              });
}

bool IsDeviceDetail(std::string_view key) {
  return key.substr(0, 7) == "device." && key != kDeviceTypeKey &&
         key != kTouchKey;
}

// `text` with touch input set to `value`, which is "true" or "false".
runtime::RuntimeConfigEdit SetTouchInput(std::string_view text,
                                         std::string_view value) {
  // A device given as one name has no place for the switch. It becomes the
  // block form of the same device.
  if (Get(text, kDeviceKey).found || Get(text, kDeviceTypeKey).found) {
    return Then(ToDeviceBlock(text), [value](std::string_view block) {
      return runtime::SetRuntimeConfigString(block, kTouchKey, value);
    });
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
           {"Server info",
            {
                {"overlay.server_info",
                 "Show server info",
                 "Location, ping and frame rate.",
                 SettingKind::kChoice,
                 {
                     {"off", "Off"},
                     {"notice", "After joining"},
                     {"always", "Always"},
                 },
                 "off",
                 {}},
                {"overlay.position",
                 "Position",
                 "",
                 SettingKind::kChoice,
                 {
                     {"top_center", "Top center"},
                     {"top_right", "Top right"},
                     {"bottom_left", "Bottom left"},
                     {"bottom_center", "Bottom center"},
                     {"bottom_right", "Bottom right"},
                 },
                 "top_center",
                 {}},
            }},
           {"Window",
            {
                {"window.width", "Width",
                 "Starting width, in desktop units.", SettingKind::kNumber,
                 {}, "1280", {}, 1, 16384, 1},
                {"window.height", "Height",
                 "Starting height, in desktop units.", SettingKind::kNumber,
                 {}, "720", {}, 1, 16384, 1},
                {"window.title", "Title", "", SettingKind::kText, {},
                 "Roblox", {}},
            }},
       }},
      {"input",
       "Input",
       "input-mouse-symbolic",
       "Display & input",
       {
           {"Mouse and touch",
            {
                {"input.raw_mouse",
                 "Raw mouse input",
                 "Turn on if first-person aiming feels too slow or too fast "
                 "with fractional display scaling.",
                 SettingKind::kToggle,
                 {},
                 "false",
                 {}},
                {"input.system_cursor",
                 "System cursor theme",
                 "Use your desktop's arrow, hand and text cursors instead of "
                 "Roblox's.",
                 SettingKind::kToggle,
                 {},
                 "false",
                 {}},
                {"input.hardware_cursor",
                 "Hardware cursor",
                 "Let the desktop draw the cursor, a game's own included, so "
                 "cursor effects work with it.",
                 SettingKind::kToggle,
                 {},
                 "false",
                 {}},
                {kTouchKey,
                 "Touch input",
                 "Turn on for a touch screen or tablet.",
                 SettingKind::kToggle,
                 {},
                 "false",
                 {}},
            }},
           {"Device",
            {
                {kDeviceKey,
                 "Device",
                 "What Roblox is told it runs on. Phone and console get "
                 "their own menus and controls.",
                 SettingKind::kChoice,
                 {
                     {"pc-windows-11", "PC"},
                     {"mobile-pixel-7", "Phone"},
                     {"console-ps5", "Console"},
                 },
                 "pc-windows-11",
                 {
                     {"pc", "pc-windows-11"},
                     {"mobile", "mobile-pixel-7"},
                     {"console", "console-ps5"},
                 }},
                {kMouseKey, "Mouse", "Off hides the mouse from Roblox.",
                 SettingKind::kToggle, {}, "true", {}},
                {kKeyboardKey, "Keyboard",
                 "Off hides the keyboard from Roblox.", SettingKind::kToggle,
                 {}, "true", {}},
            }},
       }},
      {"vr",
       "VR",
       "view-dual-symbolic",
       "Display & input",
       {
           {"Headset",
            {
                {kVrModeKey,
                 "Start in VR",
                 "Automatic starts in VR when SteamVR is running with a "
                 "headset. --vr and --no-vr override this.",
                 SettingKind::kChoice,
                 {
                     {"off", "Off"},
                     {"auto", "Automatic"},
                     {"on", "Always"},
                 },
                 "off",
                 {}},
                {"vr.anti_aliasing",
                 "Anti-aliasing",
                 "SMAA keeps text sharper than FXAA, for a little more GPU "
                 "time.",
                 SettingKind::kChoice,
                 {
                     {"fxaa", "FXAA"},
                     {"smaa", "SMAA"},
                     {"off", "Off"},
                 },
                 "fxaa",
                 {}},
                {"vr.frame_rate_limit",
                 "Frame rate limit",
                 "Unlimited lets the headset set the pace.",
                 SettingKind::kFrameRate,
                 {
                     {"-1", "Roblox setting"},
                     {"display", "Match display"},
                     {"unlimited", "Unlimited"},
                     {"custom", "Custom"},
                 },
                 "unlimited",
                 {}},
            }},
           {"In game",
            {
                {"vr.spatial_toolbar",
                 "Floating toolbar",
                 "Use the Quest app's toolbar, which you can drag. Off keeps "
                 "the older bottom bar.",
                 SettingKind::kToggle,
                 {},
                 "true",
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
                 "Hard cap in MiB for Nightcap and Roblox.",
                 SettingKind::kNumber,
                 {},
                 "0",
                 {},
                 0,
                 1048576,
                 256,
                 "Off"},
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
      {"network",
       "Network",
       "network-wired-symbolic",
       "System",
       {
           {"Proxy",
            {
                {"network.use_system_proxy",
                 "Use the system proxy",
                 "Follow the desktop's HTTP or SOCKS5 proxy in place of a "
                 "fixed one.",
                 SettingKind::kToggle,
                 {},
                 "false",
                 {}},
                {kProxyKey, "Fixed proxy (host:port)", "", SettingKind::kText,
                 {}, "", {}},
                {kProxySchemeKey,
                 "Proxy type",
                 "",
                 SettingKind::kChoice,
                 {{"http", "HTTP"}, {"socks5", "SOCKS5"}},
                 "http",
                 {{"https", "http"}, {"socks", "socks5"}, {"socks5h", "socks5"}}},
                {kProxyUsernameKey, "Proxy username",
                 "Leave empty if the proxy has no login.", SettingKind::kText,
                 {}, "", {}},
                {kProxyPasswordKey, "Proxy password",
                 "Saved as plain text in config.yaml.",
                 SettingKind::kPassword, {}, "", {}},
            }},
           {"Certificates",
            {
                {"network.ca_bundle", "CA bundle (PEM)", "",
                 SettingKind::kFile, {}, "", {}},
            },
            {},
            {},
            false,
            "The CA certificates Roblox trusts, as one PEM file. Empty "
            "uses the system's certificates."},
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
                {"integrations.discord_rpc.application_id",
                 "Application ID", "", SettingKind::kText, {}, "", {}},
            },
            {},
            {},
            false,
            "Empty Application ID uses Nightcap's own Discord app."},
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
            },
            {},
            {},
            false,
            "{place_name}, {creator_name} and {place_icon} are filled in "
            "from the experience. A line whose value is missing is left "
            "out."},
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
            },
            {},
            {},
            false,
            "An image key from the Discord app, or {place_icon} for the "
            "experience's icon. The text shows on hover and can use "
            "{place_name}."},
       }},
      {"fleasion",
       "Fleasion",
       "application-x-addon-symbolic",
       "Integrations",
       {
           {"Connection",
            {
                {"integrations.fleasion.enabled", "Use Fleasion", "",
                 SettingKind::kToggle, {}, "false", {}},
                {"integrations.fleasion.proxy_mode",
                 "Routing",
                 "Match the mode set in Fleasion.",
                 SettingKind::kChoice,
                 {
                     {"env", "Proxy"},
                     {"hosts", "Hosts file"},
                 },
                 "env",
                 {}},
                {"integrations.fleasion.proxy_port", "Proxy port",
                 "Fleasion's local proxy port, used by Proxy routing.",
                 SettingKind::kNumber, {}, "58443", {}, 1, 65535, 1},
                {"integrations.fleasion.ca_certificate", "CA certificate",
                 "", SettingKind::kFile, {}, "", {}},
            },
            "Use Fleasion",
            "Trust Fleasion's certificate without editing Roblox files.",
            true,
            "Empty CA certificate uses Fleasion's own, in "
            "~/.config/Fleasion/proxy_ca."},
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
                {"updates.source",
                 "Download source",
                 "Both pick the same sources for now.",
                 SettingKind::kChoice,
                 {
                     {"apk-pure", "APKPure"},
                     {"auto", "Automatic"},
                 },
                 "apk-pure",
                 {}},
                {"updates.launch_after_update",
                 "Start Nightcap after a scripted update",
                 "Used by scripts/auto_update_roblox.sh.",
                 SettingKind::kToggle,
                 {},
                 "false",
                 {}},
            }},
           {"Device identity",
            {
                {"device.platform", "Platform", "", SettingKind::kText, {},
                 "", {}},
                {"device.name", "Device name", "", SettingKind::kText, {},
                 "", {}},
                {"device.manufacturer", "Manufacturer", "",
                 SettingKind::kText, {}, "", {}},
                {"device.model", "Model", "", SettingKind::kText, {}, "",
                 {}},
                {"device.brand", "Brand", "", SettingKind::kText, {}, "",
                 {}},
                {"device.code", "Device code", "", SettingKind::kText, {},
                 "", {}},
                {"device.sku", "SKU", "", SettingKind::kText, {}, "", {}},
                {"device.soc_model", "Chip", "", SettingKind::kText, {}, "",
                 {}},
            },
            "Custom device identity",
            "What the device reports about itself. Empty fields use the "
            "device preset's values."},
           {"Runtime",
            {
                {"runtime.headless",
                 "Start without a window",
                 "For testing. The game cannot be seen or played while this "
                 "is on.",
                 SettingKind::kToggle,
                 {},
                 "false",
                 {}},
                {"runtime.roblox_library", "Roblox library (libroblox.so)",
                 "", SettingKind::kFile, {}, "", {}},
            },
            {},
            {},
            false,
            "Leave the Roblox library empty to run the verified build "
            "Nightcap downloads."},
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

FrameRate FrameRateSettingState(const Setting& setting,
                                std::string_view value) {
  return ParseFrameRate(value.empty() ? setting.default_value : value);
}

std::string FrameRateSettingValue(const Setting& setting,
                                  const FrameRate& rate) {
  std::string value = FrameRateValue(rate);
  if (value.empty() && !setting.default_value.empty()) {
    return "-1";
  }
  return value;
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
  Replace(std::move(read.text));
  return true;
}

void SettingsDocument::Replace(std::string text) {
  text_ = std::move(text);
  values_.clear();
}

std::string SettingsDocument::Value(std::string_view key) const {
  const Reader file(text_, &values_);
  if (key == kTouchKey || key == kMouseKey || key == kKeyboardKey) {
    return DeviceInput(file, key) ? "true" : "false";
  }
  if (key == kDeviceKey) {
    return DevicePreset(file);
  }
  if (key == kVrModeKey) {
    return VrMode(file);
  }
  if (key == kProxyKey) {
    return ProxyValue(file);
  }
  return file.Get(key).value;
}

bool SettingsDocument::IsSet(std::string_view key) const {
  return Reader(text_, &values_)
      .Get(key == kProxyKey ? kProxyHostKey : key)
      .found;
}

bool SettingsDocument::Set(std::string_view key, std::string_view value,
                           std::string* error) {
  return Save(Edit(key, value, true), error);
}

bool SettingsDocument::SetText(std::string_view key, std::string_view value,
                               std::string* error) {
  return Save(Edit(key, value, false), error);
}

std::string SettingsDocument::DefaultValue(const Setting& setting) const {
  if (setting.key == kTouchKey || setting.key == kMouseKey ||
      setting.key == kKeyboardKey) {
    return PresetInput(Reader(text_, &values_), setting.key) ? "true"
                                                             : "false";
  }
  return std::string(setting.default_value);
}

bool SettingsDocument::IsSwitchedOn(std::string_view key) const {
  for (const SettingsPage& page : SettingsPages()) {
    for (const SettingsGroup& group : page.groups) {
      for (const Setting& setting : group.settings) {
        if (setting.key == key && setting.kind == SettingKind::kToggle) {
          return ToggleState(setting, Value(key));
        }
      }
    }
  }
  return true;
}

bool SettingsDocument::IsActive(std::string_view key) const {
  // The loader refuses a fixed proxy next to the system one.
  if (key == kProxyKey && Value(kProxyKey).empty() &&
      IsSwitchedOn("network.use_system_proxy")) {
    return false;
  }
  if ((key == kProxySchemeKey || key == kProxyUsernameKey) &&
      !IsSet(kProxyKey)) {
    return false;
  }
  if (key == kProxyPasswordKey && Value(kProxyUsernameKey).empty()) {
    return false;
  }
  if (key == "overlay.position" && Value("overlay.server_info") == "off") {
    return false;
  }
  if (key == "integrations.fleasion.proxy_port" &&
      Value("integrations.fleasion.proxy_mode") == "hosts") {
    return false;
  }
  for (std::size_t dot = key.find('.'); dot != std::string_view::npos;
       dot = key.find('.', dot + 1)) {
    const std::string switch_key = std::string(key.substr(0, dot)) + ".enabled";
    if (switch_key == key) {
      break;
    }
    if (!IsSwitchedOn(switch_key)) {
      return false;
    }
  }
  return true;
}

bool SettingsDocument::IsChanged(const Setting& setting) const {
  const std::string fallback = DefaultValue(setting);
  std::string value = Value(setting.key);
  if (value.empty()) {
    value = fallback;
  }
  switch (setting.kind) {
    case SettingKind::kToggle:
      return (value == "true") != (fallback == "true");
    case SettingKind::kChoice:
    case SettingKind::kDevice:
      return ChoiceIndex(setting, value) != ChoiceIndex(setting, fallback);
    case SettingKind::kFrameRate: {
      const FrameRate now = ParseFrameRate(value);
      const FrameRate then = ParseFrameRate(fallback);
      return now.mode != then.mode ||
             (now.mode == FrameRateMode::kCustom && now.custom != then.custom);
    }
    case SettingKind::kNumber:
      return std::strtod(value.c_str(), nullptr) !=
             std::strtod(fallback.c_str(), nullptr);
    case SettingKind::kText:
    case SettingKind::kPassword:
    case SettingKind::kFile:
      return value != fallback;
    case SettingKind::kEditor:
    case SettingKind::kFolder:
      return false;
  }
  return false;
}

bool SettingsDocument::Reset(const Setting& setting, std::string* error) {
  // The device's own inputs come back once the switch is out of the file.
  if (setting.key == kTouchKey || setting.key == kMouseKey ||
      setting.key == kKeyboardKey) {
    runtime::RuntimeConfigEdit edit =
        UnsetIfSet(WithoutEmptyMapping(text_), setting.key);
    if (setting.key == kTouchKey) {
      edit = Then(std::move(edit), [](std::string_view current) {
        return UnsetIfSet(current, kTouchKeyWithoutDevice);
      });
    }
    return Save(std::move(edit), error);
  }
  // An empty device would not load, so it is set to the default preset.
  if (setting.key == kDeviceKey) {
    return Set(setting.key, setting.default_value, error);
  }
  // An empty value unsets the key, which brings the default back.
  return Set(setting.key, "", error);
}

runtime::RuntimeConfigEdit SettingsDocument::Edit(std::string_view key,
                                                  std::string_view value,
                                                  bool empty_unsets) const {
  const std::string text = WithoutEmptyMapping(text_);
  if (key == kTouchKey) {
    return SetTouchInput(text, value);
  }
  if (key == kDeviceKey) {
    return SetDevicePreset(text, value);
  }
  if (key == kProxyKey) {
    return SetProxy(text, value);
  }
  if (key == kVrModeKey) {
    return SetVrMode(text, value);
  }
  if (key == kProxyUsernameKey) {
    return SetProxyLogin(text, value);
  }
  if (key == kProxyPasswordKey) {
    return runtime::SetRuntimeConfigString(text, key, value);
  }
  if (value.empty() && empty_unsets) {
    return runtime::UnsetRuntimeConfigValue(text, key);
  }
  if (IsDeviceDetail(key)) {
    return Then(ToDeviceBlock(text), [key, value](std::string_view block) {
      return runtime::SetRuntimeConfigString(block, key, value);
    });
  }
  return runtime::SetRuntimeConfigString(text, key, value);
}

bool SettingsDocument::Undo(std::string* error) {
  if (!undo_.has_value()) {
    return true;
  }
  const bool saved = Save({std::move(*undo_), {}}, error);
  // An undo is not itself a change to take back.
  undo_.reset();
  return saved;
}

bool SettingsDocument::Save(runtime::RuntimeConfigEdit edit,
                            std::string* error) {
  if (!edit) {
    *error = std::move(edit.error);
    return false;
  }
  std::string edited = std::move(edit.text);
  // Nightcap refuses a file without keys, which taking out the last one
  // leaves. An empty mapping stands for every default.
  if (!HasYamlContent(edited)) {
    if (!edited.empty() && edited.back() != '\n') {
      edited += '\n';
    }
    edited += "{}\n";
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
      Replace(std::move(read.text));
      undo_.reset();
    }
    return false;
  }
  undo_ = std::move(text_);
  Replace(std::move(edited));
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
