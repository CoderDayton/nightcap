#ifndef MOCKTAIL_UI_SETTINGS_MODEL_H_
#define MOCKTAIL_UI_SETTINGS_MODEL_H_

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/runtime_config_store.h"

namespace mocktail {
namespace ui {

// What the settings window shows and how each row maps to config.yaml. This
// file has no GTK in it, so the mapping is tested without a display.

enum class SettingKind {
  kChoice,
  kToggle,
  kFrameRate,
  kNumber,
  kText,
  // A drop-down of the setting's choices and the audio devices found.
  kDevice,
  // A row that opens the editor for fflags.json.
  kEditor,
  // A row that opens the folder config.yaml is in.
  kFolder,
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

inline constexpr std::size_t kNoChoice = static_cast<std::size_t>(-1);

// Index of the choice that `value`, the file's value or empty when unset,
// stands for. kNoChoice when the file holds a value the window does not list.
std::size_t ChoiceIndex(const Setting& setting, std::string_view value);

struct DeviceChoice {
  std::string value;
  std::string label;
};

// What a kDevice row offers: the setting's choices, then each name in
// `devices`, then `value`, the file's value, when nothing listed stands for
// it. A name Nightcap could not pick the device by is left out: one that two
// devices share, and one that is also a choice's value.
std::vector<DeviceChoice> DeviceChoices(const Setting& setting,
                                        const std::vector<std::string>& devices,
                                        std::string_view value);

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

// The file's value for what a number row holds.
std::string NumberValue(double number);

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

  // The file's value for `key`, or empty when it is not set. The touch
  // switch, "device.touch", is never empty: unset, it is what the file's
  // device uses.
  std::string Value(std::string_view key) const;

  // Whether the file sets `key`. A key can be set to an empty text.
  bool IsSet(std::string_view key) const;

  // Sets `key` and saves. An empty `value` unsets the key. The touch switch
  // is written into the device block; a device given as one name becomes the
  // block form of the same device. On failure nothing
  // is saved, and the document holds whatever is on disk now, so the window
  // must show its rows again.
  bool Set(std::string_view key, std::string_view value, std::string* error);

  // Like Set, but an empty `value` is saved as an empty text.
  bool SetText(std::string_view key, std::string_view value,
               std::string* error);

  // Whether a change saved from this document can be taken back.
  bool CanUndo() const { return undo_.has_value(); }

  // Puts the file back as it was before the last change that was saved. A
  // file edited by hand since then is left alone, and that is an error.
  bool Undo(std::string* error);

  // Drops the change Undo would take back. The file is left as it is.
  void ForgetUndo() { undo_.reset(); }

  // How many times this document has changed the file. Setting a value the
  // file already holds does not count.
  std::size_t saved_changes() const { return saved_changes_; }

  const std::filesystem::path& config_file() const { return config_file_; }

 private:
  bool Save(std::string edited, std::string edit_error, std::string* error);

  std::filesystem::path config_file_;
  std::string text_;
  std::optional<std::string> undo_;
  std::size_t saved_changes_ = 0;
};

// A place in a text. Both numbers start at 1, and the column counts
// characters, not bytes.
struct TextPosition {
  std::size_t line = 1;
  std::size_t column = 1;
};

// Where `text` stops being JSON, or nothing when it is JSON. Comments are
// allowed, as they are in fflags.json.
std::optional<TextPosition> JsonSyntaxFault(std::string_view text);

// Why `text` is not JSON, with the line and column of the fault. Empty when
// it is JSON.
std::string JsonSyntaxError(std::string_view text);

// fflags.json as the window edits it: the whole file as one text.
class FFlagsDocument {
 public:
  explicit FFlagsDocument(std::filesystem::path file);

  // Reads the file. Having no file is not an error. A file that is not
  // UTF-8 text, or is larger than Nightcap reads, is an error: the editor
  // could not show it whole.
  bool Load(std::string* error);

  // What the editor starts with: the file's text, or an empty JSON object
  // when there is no file or it is empty.
  std::string Text() const;

  // Saves `text` as the file. Text that is not JSON, or that Nightcap would
  // refuse to start with, is not saved, and the error names the flag at
  // fault when there is one. A flag listed twice is refused too, since only
  // one of the two values would be used. When the file changed on disk since
  // it was read, nothing is saved, the error says so, and saving again
  // replaces the file.
  bool Save(std::string_view text, std::string* error);

  const std::filesystem::path& file() const { return file_; }

 private:
  // The file's text, or an error. A file larger than Nightcap reads is an
  // error, and is not read whole.
  runtime::RuntimeConfigText Read() const;

  std::filesystem::path file_;
  // The file's text as last read or saved. Empty when there is no file.
  std::string disk_;
};

}  // namespace ui
}  // namespace mocktail

#endif  // MOCKTAIL_UI_SETTINGS_MODEL_H_
