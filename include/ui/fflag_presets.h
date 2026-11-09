#ifndef MOCKTAIL_UI_FFLAG_PRESETS_H_
#define MOCKTAIL_UI_FFLAG_PRESETS_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace ui {

// The flags the FFlag editor knows, from docs/FFLAGS.md, and the presets it
// keeps. No GTK here, so it is tested without a display.

// Like "More FPS", or "Uses more RAM" as a cost.
struct FlagEffect {
  std::string_view text;
  bool cost = false;
};

struct FlagLevel {
  std::string_view label;
  std::string_view description;
  // One JSON value per name in CatalogFlag::names, in the same order.
  std::vector<std::string_view> values;
};

struct CatalogFlag {
  // The first name is the one shown. The others are always set with it.
  std::vector<std::string_view> names;
  std::string_view group;
  std::string_view summary;
  std::string_view details;
  // A number that can go either way from Roblox's default starts each effect
  // with "Lower: " or "Higher: ".
  std::vector<FlagEffect> effects;
  // One level means on or off only. With none, the flag takes a number from
  // `minimum` to `maximum`, and every name gets that number.
  std::vector<FlagLevel> levels;
  std::size_t default_level = 0;
  int minimum = 0;
  int maximum = 0;
  int step = 1;
  int default_number = 0;
};

struct FlagGroup {
  std::string_view name;
  std::string_view icon_name;
};

const std::vector<FlagGroup>& FlagGroups();
const std::vector<CatalogFlag>& FlagCatalog();
const CatalogFlag* FindCatalogFlag(std::string_view first_name);

// Setting one of these does nothing in the Roblox version Nightcap runs.
bool IsRemovedFlag(std::string_view name);

struct PresetFlag {
  std::string name;
  // JSON text, like `true`, `0` or `"True"`.
  std::string value;
  // An off flag stays in the preset but is not sent to Roblox.
  bool on = true;
};

struct Preset {
  std::string name;
  std::vector<PresetFlag> flags;
};

struct FFlagPresets {
  // The preset fflags.json holds.
  std::string active;
  std::vector<Preset> presets;

  Preset* Find(std::string_view name);
  const Preset* Find(std::string_view name) const;
};

// The flags in fflags.json text, in file order. False when the text is not
// one JSON object of strings, booleans and whole numbers.
bool ParseFFlags(std::string_view text, std::vector<PresetFlag>* flags,
                 std::string* error);

std::string FFlagsText(const Preset& preset);

// Whether the on flags of `preset` are exactly `flags`. "True" equals true,
// and "0" equals 0, as Roblox reads them.
bool HoldsFlags(const Preset& preset, const std::vector<PresetFlag>& flags);

// Reads the presets file (empty text for none) and brings the active preset in
// line with `fflags`, so hand edits to fflags.json are kept. fflags.json that
// matches no preset becomes a new preset of the user's.
bool LoadPresets(std::string_view text, const std::vector<PresetFlag>& fflags,
                 FFlagPresets* presets, std::string* error);

std::string PresetsText(const FFlagPresets& presets);

std::string UniquePresetName(const FFlagPresets& presets,
                             std::string_view base);

inline constexpr std::size_t kNoLevel = static_cast<std::size_t>(-1);

struct PresetRow {
  const CatalogFlag* flag = nullptr;
  // Indexes into Preset::flags. The first is the flag the row is named for.
  std::vector<std::size_t> entries;
  bool on = true;
  std::size_t level = kNoLevel;
  bool removed = false;
};

std::vector<PresetRow> PresetRows(const Preset& preset);

int RowNumber(const Preset& preset, const PresetRow& row, int fallback);

bool HasCatalogFlag(const Preset& preset, const CatalogFlag& flag);

// A name already in the preset is changed, not listed twice.
void AddCatalogFlag(Preset* preset, const CatalogFlag& flag);

void SetRowOn(Preset* preset, const PresetRow& row, bool on);
void SetRowLevel(Preset* preset, const PresetRow& row, std::size_t level);
void SetRowNumber(Preset* preset, const PresetRow& row, int number);
void RemoveRow(Preset* preset, const PresetRow& row);

// An empty `group` matches every group.
std::vector<const CatalogFlag*> FindFlags(std::string_view group,
                                          std::string_view query);

}  // namespace ui
}  // namespace mocktail

#endif  // MOCKTAIL_UI_FFLAG_PRESETS_H_
