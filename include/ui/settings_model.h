#ifndef MOCKTAIL_UI_SETTINGS_MODEL_H_
#define MOCKTAIL_UI_SETTINGS_MODEL_H_

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mocktail {
namespace ui {

// What the settings window shows and how each row maps to config.yaml. This
// file has no GTK in it, so the mapping is tested without a display.

enum class SettingKind {
  kChoice,
  kToggle,
  kFrameRate,
  // The window lists these so every setting has its place, and shows them
  // grayed out: it has no control that edits them yet.
  kNumber,
  kText,
  kEditor,
};

struct SettingChoice {
  // Written to the file. Empty means the key is left unset.
  std::string_view value;
  std::string_view label;
};

struct Setting {
  std::string_view key;
  std::string_view title;
  std::string_view subtitle;
  SettingKind kind = SettingKind::kChoice;
  std::vector<SettingChoice> choices;
  // What Nightcap uses when the file does not set the key.
  std::string_view default_value;
  // Other spellings the loader accepts, each with the choice it means.
  std::vector<std::pair<std::string_view, std::string_view>> aliases;
};

struct SettingsGroup {
  std::string_view title;
  std::vector<Setting> settings;
};

struct SettingsPage {
  std::string_view name;
  std::string_view title;
  std::string_view icon_name;
  // The sidebar heading the page is listed under. Pages that share one are
  // next to each other.
  std::string_view section;
  std::vector<SettingsGroup> groups;
};

const std::vector<SettingsPage>& SettingsPages();

// Whether the window can change `setting`. One it cannot change is shown
// grayed out with the file's value.
bool IsAvailable(const Setting& setting);

inline constexpr std::size_t kNoChoice = static_cast<std::size_t>(-1);

// Index of the choice that `value`, the file's value or empty when unset,
// stands for. kNoChoice when the file holds a value the window does not list.
std::size_t ChoiceIndex(const Setting& setting, std::string_view value);

// Whether a toggle is on for `value`, the file's value or empty when unset.
bool ToggleState(const Setting& setting, std::string_view value);

enum class FrameRateMode {
  kRoblox,
  kDisplay,
  kUnlimited,
  kCustom,
};

struct FrameRate {
  FrameRateMode mode = FrameRateMode::kRoblox;
  // Frames per second. Used only by kCustom.
  int custom = 0;
};

inline constexpr int kDefaultCustomFrameRate = 120;
inline constexpr int kMaximumCustomFrameRate = 9999;

// Reads graphics.frame_rate_limit. Empty, "-1", and anything that is not a
// limit mean Roblox's own setting decides.
FrameRate ParseFrameRate(std::string_view value);
// The file's value for `rate`. Empty means the key is left unset.
std::string FrameRateValue(const FrameRate& rate);

// The config file as the window edits it. Every change is saved at once.
class SettingsDocument {
 public:
  explicit SettingsDocument(std::filesystem::path config_file);

  // Reads the file, creating the first-run file when there is none.
  bool Load(std::string* error);

  // The file's value for `key`, or empty when it is not set.
  std::string Value(std::string_view key) const;

  // Sets `key` and saves. An empty `value` unsets the key. On failure nothing
  // is saved, and the document holds whatever is on disk now, so the window
  // must show its rows again.
  bool Set(std::string_view key, std::string_view value, std::string* error);

  const std::filesystem::path& config_file() const { return config_file_; }

 private:
  std::filesystem::path config_file_;
  std::string text_;
};

}  // namespace ui
}  // namespace mocktail

#endif  // MOCKTAIL_UI_SETTINGS_MODEL_H_
