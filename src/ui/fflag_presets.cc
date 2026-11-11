#include "ui/fflag_presets.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <utility>

#include "ui/settings_model.h"

namespace mocktail {
namespace ui {

namespace {

using Json = nlohmann::ordered_json;

const std::vector<FlagLevel> kOn = {{"On", "", {"true"}}};

FlagEffect Gain(std::string_view text) { return {text, false}; }
FlagEffect Cost(std::string_view text) { return {text, true}; }

CatalogFlag Flag(std::vector<std::string_view> names, std::string_view group,
                 std::string_view summary, std::string_view details,
                 std::vector<FlagEffect> effects, std::vector<FlagLevel> levels,
                 std::size_t default_level = 0) {
  CatalogFlag flag;
  flag.names = std::move(names);
  flag.group = group;
  flag.summary = summary;
  flag.details = details;
  flag.effects = std::move(effects);
  flag.levels = std::move(levels);
  flag.default_level = default_level;
  return flag;
}

CatalogFlag NumberFlag(std::string_view name, std::string_view group,
                       std::string_view summary, std::string_view details,
                       std::vector<FlagEffect> effects, int minimum,
                       int maximum, int step, int default_number) {
  CatalogFlag flag =
      Flag({name}, group, summary, details, std::move(effects), {});
  flag.minimum = minimum;
  flag.maximum = maximum;
  flag.step = step;
  flag.default_number = default_number;
  return flag;
}

std::vector<CatalogFlag> MakeCatalog() {
  return {
      Flag({"FIntDebugForceMSAASamples"}, "Graphics",
           "Smoother edges, for a little FPS.",
           "Edges of parts, buildings and characters look less jagged. "
           "Higher is smoother. Above 4, Roblox starts drawing things wrong, "
           "so 4 is the most offered here.",
           {Gain("Smoother edges"), Cost("A little less FPS")},
           {{"Off", "Normal Roblox edges", {"0"}},
            {"Smoother", "Uses a bit more of the graphics card", {"2"}},
            {"Smoothest", "The most that works without problems", {"4"}}},
           1),
      Flag({"DFIntTextureQualityOverride",
            "DFFlagTextureQualityOverrideEnabled"},
           "Graphics", "Less video memory, blurrier textures.",
           "Lower uses less video memory, which helps on graphics cards with "
           "4 GB or less. 3 is Roblox's normal quality.",
           {Gain("Less video memory"), Cost("Blurrier textures")},
           {{"Lowest", "Blurriest, least video memory", {"0", "true"}},
            {"Low", "Blurry", {"1", "true"}},
            {"Medium", "For 4 GB graphics cards", {"2", "true"}},
            {"Normal", "Roblox's own quality", {"3", "true"}}},
           2),
      Flag({"FIntFRMMaxGrassDistance", "FIntFRMMinGrassDistance"}, "Graphics",
           "More FPS, no grass.", "Grass is not drawn at all.",
           {Gain("More FPS"), Cost("No grass")}, {{"On", "", {"0", "0"}}}),
      Flag({"FIntGrassMovementReducedMotionFactor"}, "Graphics",
           "Grass stays still.", "Grass stays still instead of swaying.",
           {Gain("Still grass")}, {{"On", "", {"0"}}}),
      NumberFlag("DFIntDebugFRMQualityLevelOverride", "Graphics",
                 "Locks graphics quality. Lower means more FPS.",
                 "1 is lowest and 21 is highest. The in-game Graphics "
                 "Quality slider stops working, quality no longer drops when "
                 "the frame rate does, and dynamic resolution turns off. Try "
                 "the in-game slider first.",
                 {Gain("Lower: more FPS"), Gain("Higher: looks better")}, 1,
                 21, 1, 10),
      NumberFlag("DFIntCSGLevelOfDetailSwitchingDistance", "Graphics",
                 "Lower means more FPS, blockier far objects.",
                 "Lower is faster but blockier. Roblox uses 250.",
                 {Gain("Lower: more FPS"), Cost("Lower: blockier far objects")},
                 0, 10000, 10, 250),
      NumberFlag("DFIntCSGLevelOfDetailSwitchingDistanceL12", "Graphics",
                 "Lower means more FPS. Second detail step.",
                 "Lower is faster but blockier. Roblox uses 500.",
                 {Gain("Lower: more FPS"), Cost("Lower: blockier far objects")},
                 0, 10000, 10, 500),
      NumberFlag("DFIntCSGLevelOfDetailSwitchingDistanceL23", "Graphics",
                 "Lower means more FPS. Third detail step.",
                 "Lower is faster but blockier. Roblox uses 2000.",
                 {Gain("Lower: more FPS"), Cost("Lower: blockier far objects")},
                 0, 10000, 50, 2000),
      NumberFlag("DFIntCSGLevelOfDetailSwitchingDistanceL34", "Graphics",
                 "Lower means more FPS. Last detail step.",
                 "Lower is faster but blockier. Roblox uses 3000.",
                 {Gain("Lower: more FPS"), Cost("Lower: blockier far objects")},
                 0, 10000, 50, 3000),
      Flag({"DFFlagDebugPauseVoxelizer"}, "Graphics",
           "More FPS, but lighting stops updating.",
           "Light and shadows can be wrong.",
           {Gain("More FPS"), Cost("Wrong light and shadows")}, kOn),
      Flag({"DFFlagDebugForceAnisoOff"}, "Graphics",
           "Slightly more FPS, blurrier far floors.",
           "Floors look blurrier further away.",
           {Gain("Slightly more FPS"), Cost("Blurrier far floors")}, kOn),
      Flag({"FIntDebugTextureManagerSkipMips"}, "Graphics",
           "Less video memory, less texture detail.", "1 and 2 are mild.",
           {Gain("Less video memory"), Cost("Less texture detail")},
           {{"Mild", "A little less detail", {"1"}},
            {"Less detail", "Saves more video memory", {"2"}}}),
      NumberFlag("FIntTerrainArraySliceSize", "Graphics",
                 "Lower means less video memory, blurrier terrain.",
                 "Roblox uses 256. Lower uses less video memory.",
                 {Gain("Lower: less video memory"),
                  Cost("Lower: blurrier terrain")},
                 16, 256, 16, 256),
      NumberFlag("FIntRenderShadowmapBias", "Graphics",
                 "Higher hides striped shadows.",
                 "Roblox uses 75. Change it only if shadows look striped.",
                 {Gain("Higher: fewer shadow stripes"),
                  Cost("Higher: shadows drift from objects")},
                 0, 200, 5, 75),
      Flag({"FFlagFastGPULightCulling3"}, "Graphics",
           "Sorts lights on the GPU. Can help weak CPUs.",
           "Only matters in games with Future lighting and many lights.",
           {Gain("Less CPU in lit games"), Cost("A bit more GPU work")}, kOn),
      Flag({"FIntRenderLocalLightUpdatesMax", "FIntRenderLocalLightUpdatesMin"},
           "Graphics", "More FPS, lights update slower.",
           "How many lights with shadows update each frame. Roblox uses 2 "
           "to 8, more at higher graphics quality.",
           {Gain("More FPS"), Cost("Lights and shadows update slower")},
           {{"Fewer", "Up to 4 lights a frame", {"4", "1"}},
            {"Fewest", "1 light a frame", {"1", "1"}}}),

      Flag({"FFlagDebugDisplayFPS"}, "Performance",
           "Shows an FPS counter in the top-left corner.",
           "Handy for checking what other flags do.", {Gain("FPS counter")},
           kOn),
      Flag({"DFFlagReduceCPUWhenBG"}, "Performance",
           "Uses less CPU while Roblox is in the background.", "",
           {Gain("Less CPU in the background")}, kOn),
      Flag({"DFFlagEnableMeshPreloading", "DFFlagEnableTexturePreloading",
            "DFFlagEnableSoundPreloading"},
           "Performance", "Less pop-in, longer loading.",
           "Loads what a game asks for up front. Best with 16 GB of RAM or "
           "more.",
           {Gain("Less pop-in"), Cost("Uses more RAM"),
            Cost("Longer loading")}, {{"On", "", {"true", "true", "true"}}}),
      Flag({"DFIntAnimatorThrottleMaxFramesToSkip",
            "DFIntAnimationLodThrottleMaxFramesToSkip"},
           "Performance", "Smoother far animations, more CPU.",
           "Far-away characters stop skipping frames. Roblox uses 4 and 8.",
           {Gain("Smoother far animations"), Cost("Uses more CPU")}, {{"On", "", {"0", "0"}}}),

      Flag({"FIntRobloxGuiBlurIntensity"}, "Menus and sound",
           "Turns off the blur behind the Roblox menu.", "",
           {Gain("No menu blur")}, {{"On", "", {"0"}}}),
      Flag({"DFFlagDisableDPIScale"}, "Menus and sound",
           "Makes menus smaller on high-resolution screens.", "",
           {Gain("Smaller menus")}, kOn),
      NumberFlag("DFIntVoiceChatVolumeThousandths", "Menus and sound",
                 "Voice chat volume.", "1000 is normal.",
                 {Gain("Higher: louder voice chat"),
                  Gain("Lower: quieter voice chat")},
                 0, 3000, 100, 1000),
      NumberFlag("FIntFontSizePadding", "Menus and sound",
                 "Space around menu text.", "Roblox uses 1.",
                 {Gain("Higher: more space around text"),
                  Gain("Lower: tighter menus")},
                 0, 20, 1, 1),
      NumberFlag("FIntScrollWheelDeltaAmount", "Menus and sound",
                 "Menu scroll speed.", "Roblox uses 100.",
                 {Gain("Higher: faster scrolling"),
                  Gain("Lower: slower scrolling")},
                 10, 500, 10, 100),

      Flag({"DFFlagDisableFastLogTelemetry",
            "DFIntHttpInfluxHundredthsPercentage",
            "DFIntClientNetworkInfluxHundredthsPercentage",
            "DFIntRaknetBandwidthInfluxHundredthsPercentageV2",
            "DFIntParallelLuaStatsEventHundredthsPercent",
            "DFIntHttpGAFailureReportPercent",
            "DFIntHttpStatsErrorInfluxReportsPerMillion",
            "DFIntHttpStatsDnsErrorInfluxReportsPerMillion",
            "DFIntHttp2StatsHundredthsPercent"},
           "Usage stats", "Stops Roblox sending some usage and network stats.",
           "Nothing changes in game.", {Gain("Sends fewer stats")},
           {{"On", "", {"true", "0", "0", "0", "0", "0", "0", "0", "0"}}}),
  };
}

std::string Lower(std::string_view text) {
  std::string lower(text);
  for (char& character : lower) {
    character =
        static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return lower;
}

// Roblox reads "True" as true and "12" as 12.
Json Canonical(std::string_view value) {
  Json json = Json::parse(value, nullptr, false);
  if (!json.is_string()) {
    return json;
  }
  const std::string text = json.get<std::string>();
  const std::string lower = Lower(text);
  if (lower == "true") {
    return true;
  }
  if (lower == "false") {
    return false;
  }
  const std::size_t digits = !text.empty() && text[0] == '-' ? 1 : 0;
  if (text.size() > digits && text.size() < 19 &&
      std::all_of(text.begin() + static_cast<std::ptrdiff_t>(digits),
                  text.end(), [](char character) {
                    return std::isdigit(static_cast<unsigned char>(character));
                  })) {
    return std::stoll(text);
  }
  return json;
}

bool SameValue(std::string_view a, std::string_view b) {
  return Canonical(a) == Canonical(b);
}

std::map<std::string, Json, std::less<>> OnFlags(const Preset& preset) {
  std::map<std::string, Json, std::less<>> flags;
  for (const PresetFlag& flag : preset.flags) {
    if (flag.on) {
      flags[flag.name] = Canonical(flag.value);
    }
  }
  return flags;
}

std::optional<std::size_t> IndexOf(const Preset& preset,
                                   std::string_view name) {
  for (std::size_t i = 0; i < preset.flags.size(); ++i) {
    if (preset.flags[i].name == name) {
      return i;
    }
  }
  return std::nullopt;
}

void SetValue(Preset* preset, std::string_view name, std::string value) {
  if (const auto index = IndexOf(*preset, name)) {
    preset->flags[*index].value = std::move(value);
    preset->flags[*index].on = true;
    return;
  }
  preset->flags.push_back({std::string(name), std::move(value), true});
}

// Takes hand edits of fflags.json into `preset`, which fflags.json holds.
void TakeIn(Preset* preset, const std::vector<PresetFlag>& fflags) {
  std::vector<PresetFlag> kept;
  for (PresetFlag& flag : preset->flags) {
    const auto in_file = std::find_if(
        fflags.begin(), fflags.end(),
        [&flag](const PresetFlag& file) { return file.name == flag.name; });
    if (in_file != fflags.end()) {
      flag.value = in_file->value;
      flag.on = true;
      kept.push_back(std::move(flag));
    } else if (!flag.on) {
      kept.push_back(std::move(flag));
    }
  }
  preset->flags = std::move(kept);
  for (const PresetFlag& file : fflags) {
    if (!IndexOf(*preset, file.name)) {
      preset->flags.push_back(file);
    }
  }
}

}  // namespace

const std::vector<FlagGroup>& FlagGroups() {
  static const std::vector<FlagGroup> groups = {
      {"Graphics", "applications-graphics-symbolic"},
      {"Performance", "power-profile-performance-symbolic"},
      {"Menus and sound", "audio-speakers-symbolic"},
      {"Usage stats", "preferences-system-privacy-symbolic"},
  };
  return groups;
}

const std::vector<CatalogFlag>& FlagCatalog() {
  static const std::vector<CatalogFlag> catalog = MakeCatalog();
  return catalog;
}

const CatalogFlag* FindCatalogFlag(std::string_view first_name) {
  for (const CatalogFlag& flag : FlagCatalog()) {
    if (flag.names[0] == first_name) {
      return &flag;
    }
  }
  return nullptr;
}

bool IsRemovedFlag(std::string_view name) {
  static constexpr std::string_view kRemoved[] = {
      "FFlagDebugForceFutureIsBrightPhase2",
      "FFlagDebugForceFutureIsBrightPhase3",
      "DFFlagDebugRenderForceTechnologyVoxel",
      "FFlagRenderUnifiedLighting10",
      "FIntRenderShadowIntensity",
      "FFlagDisablePostFx",
      "FFlagRenderNoLowFrmBloom",
      "FFlagNewLightAttenuation",
      "FFlagGlobalWindRendering",
      "FFlagGlobalWindActivated",
      "FFlagTaskSchedulerLimitTargetFpsTo2402",
      "FFlagHandleAltEnterFullscreenManually",
      "FFlagEnableDisplaySize9",
      "FFlagAdServiceEnabled",
      "DFFlagSkipHighResolutiontextureMipsOnLowMemoryDevices",
      "DFFlagSkipHighResolutiontextureMipsOnLowMemoryDevices2",
      "FFlagSoundsUsePhysicalVelocity",
      "FIntRuntimeMaxNumOfThreads",
  };
  if (name.rfind("FFlagDebugDisableTelemetry", 0) == 0) {
    return true;
  }
  return std::find(std::begin(kRemoved), std::end(kRemoved), name) !=
         std::end(kRemoved);
}

Preset* FFlagPresets::Find(std::string_view name) {
  for (Preset& preset : presets) {
    if (preset.name == name) {
      return &preset;
    }
  }
  return nullptr;
}

const Preset* FFlagPresets::Find(std::string_view name) const {
  return const_cast<FFlagPresets*>(this)->Find(name);
}

bool ParseFFlags(std::string_view text, std::vector<PresetFlag>* flags,
                 std::string* error) {
  flags->clear();
  if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
    return true;
  }
  *error = FFlagsContentError(text);
  if (!error->empty()) {
    return false;
  }
  const Json json = Json::parse(text, nullptr, false, true);
  for (const auto& [name, value] : json.items()) {
    flags->push_back({name, value.dump(), true});
  }
  return true;
}

std::string FFlagsText(const Preset& preset) {
  Json json = Json::object();
  for (const PresetFlag& flag : preset.flags) {
    if (flag.on) {
      json[flag.name] = Json::parse(flag.value, nullptr, false);
    }
  }
  return json.empty() ? "{\n}\n" : json.dump(2) + "\n";
}

bool HoldsFlags(const Preset& preset, const std::vector<PresetFlag>& flags) {
  Preset file;
  file.flags = flags;
  return OnFlags(preset) == OnFlags(file);
}

bool LoadPresets(std::string_view text, const std::vector<PresetFlag>& fflags,
                 FFlagPresets* presets, std::string* error) {
  FFlagPresets loaded;
  if (text.find_first_not_of(" \t\r\n") != std::string_view::npos) {
    const Json json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object() ||
        !json.value("presets", Json::array()).is_array()) {
      *error = "The presets file is not one Nightcap wrote";
      return false;
    }
    if (json.contains("active") && json["active"].is_string()) {
      loaded.active = json["active"].get<std::string>();
    }
    for (const Json& entry : json.value("presets", Json::array())) {
      if (!entry.is_object() || !entry.contains("name") ||
          !entry["name"].is_string()) {
        continue;
      }
      Preset preset;
      preset.name = entry["name"].get<std::string>();
      if (preset.name.empty() || loaded.Find(preset.name) != nullptr) {
        continue;
      }
      for (const Json& flag : entry.value("flags", Json::array())) {
        if (!flag.is_object() || !flag.contains("name") ||
            !flag["name"].is_string() || !flag.contains("value")) {
          continue;
        }
        const Json& value = flag["value"];
        if (!value.is_string() && !value.is_boolean() &&
            !value.is_number_integer()) {
          continue;
        }
        const std::string name = flag["name"].get<std::string>();
        if (name.empty() || IndexOf(preset, name)) {
          continue;
        }
        // value() throws when "on" is there but not a boolean.
        const bool on = !flag.contains("on") || !flag["on"].is_boolean() ||
                        flag["on"].get<bool>();
        preset.flags.push_back({name, value.dump(), on});
      }
      loaded.presets.push_back(std::move(preset));
    }
  }

  Preset* active = loaded.Find(loaded.active);
  if (active != nullptr) {
    TakeIn(active, fflags);
  } else {
    for (Preset& preset : loaded.presets) {
      if (HoldsFlags(preset, fflags)) {
        active = &preset;
        TakeIn(active, fflags);
        break;
      }
    }
    if (active == nullptr) {
      Preset mine;
      mine.name = UniquePresetName(loaded, "My setup");
      mine.flags = fflags;
      loaded.presets.push_back(std::move(mine));
      active = &loaded.presets.back();
    }
    loaded.active = active->name;
  }
  *presets = std::move(loaded);
  return true;
}

std::string PresetsText(const FFlagPresets& presets) {
  Json json = Json::object();
  json["active"] = presets.active;
  json["presets"] = Json::array();
  for (const Preset& preset : presets.presets) {
    Json flags = Json::array();
    for (const PresetFlag& flag : preset.flags) {
      flags.push_back({{"name", flag.name},
                       {"value", Json::parse(flag.value, nullptr, false)},
                       {"on", flag.on}});
    }
    json["presets"].push_back({{"name", preset.name}, {"flags", flags}});
  }
  return json.dump(2) + "\n";
}

std::string UniquePresetName(const FFlagPresets& presets,
                             std::string_view base) {
  std::string name(base);
  for (int number = 2; presets.Find(name) != nullptr; ++number) {
    name = std::string(base) + " " + std::to_string(number);
  }
  return name;
}

std::vector<PresetRow> PresetRows(const Preset& preset) {
  std::vector<PresetRow> rows;
  std::vector<bool> used(preset.flags.size(), false);
  for (std::size_t i = 0; i < preset.flags.size(); ++i) {
    if (used[i]) {
      continue;
    }
    const std::string& name = preset.flags[i].name;
    PresetRow row;
    // A flag set with another one belongs to that one's row.
    for (const CatalogFlag& flag : FlagCatalog()) {
      if (std::find(flag.names.begin(), flag.names.end(), name) !=
              flag.names.end() &&
          IndexOf(preset, flag.names[0])) {
        row.flag = &flag;
        break;
      }
    }
    if (row.flag == nullptr) {
      row.entries = {i};
      used[i] = true;
      row.on = preset.flags[i].on;
      row.removed = IsRemovedFlag(name);
      rows.push_back(std::move(row));
      continue;
    }
    for (std::string_view member : row.flag->names) {
      if (const auto index = IndexOf(preset, member)) {
        row.entries.push_back(*index);
        used[*index] = true;
      }
    }
    row.on = preset.flags[row.entries[0]].on;
    for (std::size_t level = 0; level < row.flag->levels.size(); ++level) {
      const FlagLevel& values = row.flag->levels[level];
      bool matches = row.entries.size() == row.flag->names.size();
      for (std::size_t n = 0; matches && n < row.flag->names.size(); ++n) {
        const auto index = IndexOf(preset, row.flag->names[n]);
        matches = index && SameValue(preset.flags[*index].value,
                                     values.values[n]);
      }
      if (matches) {
        row.level = level;
        break;
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

int RowNumber(const Preset& preset, const PresetRow& row, int fallback) {
  const Json value = Canonical(preset.flags[row.entries[0]].value);
  if (!value.is_number_integer()) {
    return fallback;
  }
  const long long number = value.get<long long>();
  if (number < INT32_MIN || number > INT32_MAX) {
    return fallback;
  }
  return static_cast<int>(number);
}

bool HasCatalogFlag(const Preset& preset, const CatalogFlag& flag) {
  return IndexOf(preset, flag.names[0]).has_value();
}

void AddCatalogFlag(Preset* preset, const CatalogFlag& flag) {
  for (std::size_t n = 0; n < flag.names.size(); ++n) {
    SetValue(preset, flag.names[n],
             flag.levels.empty()
                 ? std::to_string(flag.default_number)
                 : std::string(flag.levels[flag.default_level].values[n]));
  }
}

void SetRowOn(Preset* preset, const PresetRow& row, bool on) {
  for (std::size_t index : row.entries) {
    preset->flags[index].on = on;
  }
}

void SetRowLevel(Preset* preset, const PresetRow& row, std::size_t level) {
  if (row.flag == nullptr || level >= row.flag->levels.size()) {
    return;
  }
  for (std::size_t n = 0; n < row.flag->names.size(); ++n) {
    SetValue(preset, row.flag->names[n],
             std::string(row.flag->levels[level].values[n]));
    preset->flags[*IndexOf(*preset, row.flag->names[n])].on = row.on;
  }
}

void SetRowNumber(Preset* preset, const PresetRow& row, int number) {
  for (std::size_t index : row.entries) {
    preset->flags[index].value = std::to_string(number);
  }
}

void RemoveRow(Preset* preset, const PresetRow& row) {
  std::vector<std::size_t> entries = row.entries;
  std::sort(entries.rbegin(), entries.rend());
  for (std::size_t index : entries) {
    preset->flags.erase(preset->flags.begin() +
                        static_cast<std::ptrdiff_t>(index));
  }
}

std::vector<const CatalogFlag*> FindFlags(std::string_view group,
                                          std::string_view query) {
  const std::string needle = Lower(query);
  std::vector<const CatalogFlag*> found;
  for (const CatalogFlag& flag : FlagCatalog()) {
    if (!group.empty() && flag.group != group) {
      continue;
    }
    std::string haystack = Lower(flag.summary) + "\n" + Lower(flag.details);
    for (const FlagEffect& effect : flag.effects) {
      haystack += "\n" + Lower(effect.text);
    }
    for (std::string_view name : flag.names) {
      haystack += "\n" + Lower(name);
    }
    if (haystack.find(needle) != std::string::npos) {
      found.push_back(&flag);
    }
  }
  return found;
}

}  // namespace ui
}  // namespace mocktail
