#include <SDL3/SDL.h>
#include <adwaita.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mocktail/platform/sdl_window_icon_data.h"
#include "runtime/environment.h"
#include "runtime/running_instance.h"
#include "runtime/runtime_paths.h"
#include "ui/settings_model.h"

namespace {

using mocktail::ui::ChoiceIndex;
using mocktail::ui::DeviceChoice;
using mocktail::ui::FFlagsDocument;
using mocktail::ui::FrameRate;
using mocktail::ui::FrameRateMode;
using mocktail::ui::kNoChoice;
using mocktail::ui::Setting;
using mocktail::ui::SettingKind;
using mocktail::ui::SettingsDocument;
using mocktail::ui::SettingsGroup;
using mocktail::ui::SettingsPage;

constexpr char kApplicationId[] = "io.github.CoderDayton.nightcap";

constexpr char kWindowStyle[] = R"(
.navigation-sidebar > row { min-height: 44px; margin: 1px 6px; padding: 0 14px; }
.row-reset { opacity: 0; transition: opacity 150ms ease-in-out; }
row:hover .row-reset, row:focus-within .row-reset { opacity: 1; }
.count-badge {
  min-width: 18px; min-height: 18px; padding: 0 6px; border-radius: 9px;
  font-size: 0.75em; font-weight: 700;
  background-color: alpha(@accent_bg_color, 0.25); color: @accent_color;
}
.changed-chip {
  min-height: 26px; padding: 0 10px; border-radius: 13px; font-weight: 700;
  background-color: alpha(@accent_bg_color, 0.2); color: @accent_color;
}
.notice-bar {
  min-height: 54px; padding-right: 6px; background-color: @window_bg_color;
  box-shadow: inset 0 1px alpha(@window_fg_color, 0.1);
}
)";

struct SettingsWindow;

struct Row {
  SettingsWindow* window = nullptr;
  const Setting* setting = nullptr;
  GtkWidget* widget = nullptr;
  // The custom frames-per-second row. Only a kFrameRate row has one.
  GtkWidget* custom = nullptr;
  // The button that puts the row back to its default. Shown only while the
  // row is changed.
  GtkWidget* reset = nullptr;
  // The button that opens a file picker. Only a kFile row has one.
  GtkWidget* pick = nullptr;
  // Whether `widget` is an expander row whose own switch is this toggle.
  bool expander_switch = false;
  // The row's page, in SettingsPages() order.
  std::size_t page = 0;
  // A value the file holds that the row does not list. It is shown as one
  // more choice so opening the window never changes it.
  std::string unlisted;
  // Whether the choices have been put in the drop-down yet.
  bool choices_set = false;
  std::vector<DeviceChoice> devices;
  // The text a kText row was last filled with. A field that holds anything
  // else has an edit that is not saved yet.
  std::string shown;
};

struct SettingsWindow {
  SettingsWindow(std::filesystem::path config_file,
                 std::filesystem::path fflags_file)
      : document(std::move(config_file)), fflags(std::move(fflags_file)) {}

  SettingsDocument document;
  FFlagsDocument fflags;
  GtkWidget* widget = nullptr;
  std::deque<Row> rows;
  AdwNavigationSplitView* split = nullptr;
  AdwNavigationPage* content_page = nullptr;
  GtkStack* stack = nullptr;
  AdwToastOverlay* toasts = nullptr;
  std::vector<std::string> output_devices;
  std::vector<std::string> input_devices;
  // The card under the settings that says when changes take effect.
  GtkRevealer* notice = nullptr;
  GtkLabel* notice_label = nullptr;
  // The button on that card that restarts a running Nightcap.
  GtkWidget* restart = nullptr;
  // Both empty when the window was not started by `mocktail --settings`.
  std::filesystem::path instance_lock;
  std::filesystem::path restart_program;
  // The restart question is asked once per window.
  bool asked_restart = false;
  bool restarting = false;
  // A change saved while the restart ran may not reach the new Nightcap.
  bool saved_while_restarting = false;
  // Set once the window is destroyed, after which its widgets are gone.
  bool closed = false;
  // The button on that card that takes the last saved change back.
  GtkWidget* undo = nullptr;
  // Set while rows are filled from the file, so that filling one is not
  // taken for a change by the user.
  bool refreshing = false;
  // Each page's count of changed rows in the sidebar, in SettingsPages()
  // order.
  std::vector<GtkWidget*> badges;
  GtkWidget* changed_chip = nullptr;
  GtkListBox* sidebar = nullptr;
};

std::string Text(std::string_view text) { return std::string(text); }

bool HideDescendant(GtkWidget* parent, const char* name) {
  for (GtkWidget* child = gtk_widget_get_first_child(parent); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    if (gtk_widget_has_css_class(child, name)) {
      gtk_widget_set_visible(child, FALSE);
      return true;
    }
    if (HideDescendant(child, name)) {
      return true;
    }
  }
  return false;
}

void SetChoices(Row* row) {
  GtkStringList* labels = gtk_string_list_new(nullptr);
  for (const auto& choice : row->setting->choices) {
    gtk_string_list_append(labels, Text(choice.label).c_str());
  }
  if (!row->unlisted.empty()) {
    gtk_string_list_append(labels, row->unlisted.c_str());
  }
  adw_combo_row_set_model(ADW_COMBO_ROW(row->widget), G_LIST_MODEL(labels));
  g_object_unref(labels);
}

// The names SDL gives the audio devices, which are the names Nightcap picks
// a device by. Empty when SDL finds no audio system.
std::vector<std::string> AudioDeviceNames(bool recording) {
  std::vector<std::string> names;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return names;
  }
  int count = 0;
  SDL_AudioDeviceID* ids = recording ? SDL_GetAudioRecordingDevices(&count)
                                     : SDL_GetAudioPlaybackDevices(&count);
  for (int index = 0; ids != nullptr && index < count; ++index) {
    if (const char* name = SDL_GetAudioDeviceName(ids[index])) {
      names.emplace_back(name);
    }
  }
  SDL_free(ids);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  return names;
}

void RefreshDeviceRow(Row* row, const std::string& value) {
  const SettingsWindow& window = *row->window;
  std::vector<DeviceChoice> devices = mocktail::ui::DeviceChoices(
      *row->setting,
      row->setting->key == "audio.input_device" ? window.input_devices
                                                : window.output_devices,
      value);
  const std::string chosen =
      value.empty() ? Text(row->setting->default_value) : value;
  guint selected = 0;
  GtkStringList* labels = gtk_string_list_new(nullptr);
  for (std::size_t index = 0; index < devices.size(); ++index) {
    gtk_string_list_append(labels, devices[index].label.c_str());
    if (devices[index].value == chosen) {
      selected = static_cast<guint>(index);
    }
  }
  row->devices = std::move(devices);
  adw_combo_row_set_model(ADW_COMBO_ROW(row->widget), G_LIST_MODEL(labels));
  g_object_unref(labels);
  adw_combo_row_set_selected(ADW_COMBO_ROW(row->widget), selected);
}

// Shows the file's value in `row`. With `keep_edit`, text typed into the row
// and not saved yet is left alone.
void RefreshRow(Row* row, bool keep_edit) {
  const Setting& setting = *row->setting;
  const std::string value = row->window->document.Value(setting.key);
  switch (setting.kind) {
    case SettingKind::kToggle:
      if (row->expander_switch) {
        adw_expander_row_set_enable_expansion(
            ADW_EXPANDER_ROW(row->widget),
            mocktail::ui::ToggleState(setting, value));
      } else {
        adw_switch_row_set_active(ADW_SWITCH_ROW(row->widget),
                                  mocktail::ui::ToggleState(setting, value));
      }
      break;
    case SettingKind::kChoice: {
      const std::size_t index = ChoiceIndex(setting, value);
      std::string unlisted = index == kNoChoice ? value : std::string();
      // The list changes only when the unlisted value does.
      if (!row->choices_set || unlisted != row->unlisted) {
        row->unlisted = std::move(unlisted);
        row->choices_set = true;
        SetChoices(row);
      }
      adw_combo_row_set_selected(
          ADW_COMBO_ROW(row->widget),
          static_cast<guint>(index == kNoChoice ? setting.choices.size()
                                                : index));
      break;
    }
    case SettingKind::kFrameRate: {
      const FrameRate rate = mocktail::ui::ParseFrameRate(value);
      adw_combo_row_set_selected(ADW_COMBO_ROW(row->widget),
                                 static_cast<guint>(rate.mode));
      if (rate.mode == FrameRateMode::kCustom) {
        adw_spin_row_set_value(ADW_SPIN_ROW(row->custom), rate.custom);
      }
      gtk_widget_set_visible(row->custom, rate.mode == FrameRateMode::kCustom);
      break;
    }
    case SettingKind::kNumber:
      adw_spin_row_set_value(
          ADW_SPIN_ROW(row->widget),
          g_ascii_strtod(
              Text(value.empty() ? setting.default_value : value).c_str(),
              nullptr));
      break;
    case SettingKind::kText:
    case SettingKind::kFile: {
      const bool is_set = row->window->document.IsSet(setting.key);
      const std::string text = is_set ? value : Text(setting.default_value);
      const std::string current =
          gtk_editable_get_text(GTK_EDITABLE(row->widget));
      if (!keep_edit || current == row->shown) {
        // Setting the same text again would mark the field as edited.
        if (text != current) {
          gtk_editable_set_text(GTK_EDITABLE(row->widget), text.c_str());
        }
        row->shown = text;
      }
      break;
    }
    case SettingKind::kDevice:
      RefreshDeviceRow(row, value);
      break;
    case SettingKind::kEditor:
    case SettingKind::kFolder:
      break;
  }
}

// Shows the reset button of each row that differs from its default, and the
// counts of those rows. Rows a switched-off block leaves without effect are
// greyed out.
void ShowChanged(SettingsWindow* window) {
  std::vector<std::size_t> counts(window->badges.size(), 0);
  for (Row& row : window->rows) {
    gtk_widget_set_sensitive(row.widget,
                             window->document.IsActive(row.setting->key));
    const bool changed = window->document.IsChanged(*row.setting);
    if (changed && row.page < counts.size()) {
      ++counts[row.page];
    }
    if (row.reset != nullptr) {
      gtk_widget_set_visible(row.reset, changed);
    }
  }
  std::size_t total = 0;
  for (std::size_t index = 0; index < window->badges.size(); ++index) {
    const std::size_t count = counts[index];
    total += count;
    gtk_label_set_text(GTK_LABEL(window->badges[index]),
                       std::to_string(count).c_str());
    gtk_widget_set_visible(window->badges[index], count > 0);
  }
  if (window->changed_chip != nullptr) {
    gtk_button_set_label(GTK_BUTTON(window->changed_chip),
                         (std::to_string(total) + " changed").c_str());
    gtk_widget_set_visible(window->changed_chip, total > 0);
  }
}

// Shows every row as the file has it. Text typed into a row and not saved
// yet stays, except in `saved_row`, whose text was just saved.
void RefreshRows(SettingsWindow* window, const Row* saved_row = nullptr) {
  window->refreshing = true;
  for (Row& row : window->rows) {
    RefreshRow(&row, &row != saved_row);
  }
  window->refreshing = false;
  ShowChanged(window);
}

void ShowError(SettingsWindow* window, const std::string& error) {
  AdwToast* toast = adw_toast_new(error.c_str());
  adw_toast_set_use_markup(toast, FALSE);
  adw_toast_set_timeout(toast, 0);
  adw_toast_overlay_add_toast(window->toasts, toast);
}

constexpr char kSavedText[] =
    "Saved. Changes apply the next time Nightcap starts.";
constexpr char kRestartToApplyText[] =
    "Saved. Restart Nightcap to apply the changes.";

struct RestartJob {
  std::filesystem::path instance_lock;
  std::filesystem::path restart_program;
  std::string error;
};

void RunRestart(GTask* task, gpointer, gpointer task_data, GCancellable*) {
  auto* job = static_cast<RestartJob*>(task_data);
  // Read before Nightcap closes, so the new one starts with the same
  // arguments and environment as the one it replaces.
  const std::optional<mocktail::runtime::LaunchCommand> command =
      mocktail::runtime::ReadLaunchCommand(
          mocktail::runtime::FindRunningInstance(job->instance_lock).pid);
  const bool restarted =
      mocktail::runtime::StopRunningInstance(
          job->instance_lock, std::chrono::seconds(15), &job->error) &&
      mocktail::runtime::StartDetached(
          job->restart_program, &job->error,
          command.value_or(mocktail::runtime::LaunchCommand{}));
  g_task_return_boolean(task, restarted);
}

void OnRestartDone(GObject*, GAsyncResult* result, gpointer user_data) {
  auto* window = static_cast<SettingsWindow*>(user_data);
  GTask* task = G_TASK(result);
  const auto* job = static_cast<const RestartJob*>(g_task_get_task_data(task));
  window->restarting = false;
  const bool restarted = g_task_propagate_boolean(task, nullptr);
  // Closing the window while the restart ran must not end the process
  // between closing Nightcap and starting it again.
  g_application_release(g_application_get_default());
  if (window->closed) {
    return;
  }
  if (restarted && !window->saved_while_restarting) {
    gtk_label_set_text(window->notice_label, "Saved. Nightcap restarted.");
    gtk_widget_set_visible(window->restart, FALSE);
    return;
  }
  gtk_label_set_text(window->notice_label, kRestartToApplyText);
  gtk_widget_set_sensitive(window->restart, TRUE);
  if (!restarted) {
    ShowError(window, job->error);
  }
}

void StartRestart(SettingsWindow* window) {
  if (window->restarting) {
    return;
  }
  window->restarting = true;
  window->saved_while_restarting = false;
  gtk_widget_set_sensitive(window->restart, FALSE);
  gtk_label_set_text(window->notice_label, "Restarting Nightcap…");
  g_application_hold(g_application_get_default());
  auto* job = new RestartJob{window->instance_lock, window->restart_program,
                             std::string()};
  GTask* task = g_task_new(nullptr, nullptr, OnRestartDone, window);
  g_task_set_task_data(task, job, [](gpointer data) {
    delete static_cast<RestartJob*>(data);
  });
  g_task_run_in_thread(task, RunRestart);
  g_object_unref(task);
}

void OnRestartClicked(GtkButton*, gpointer user_data) {
  StartRestart(static_cast<SettingsWindow*>(user_data));
}

void OnRestartResponse(AdwAlertDialog*, const char* response,
                       gpointer user_data) {
  if (g_strcmp0(response, "restart") == 0) {
    StartRestart(static_cast<SettingsWindow*>(user_data));
  }
}

void AskRestart(SettingsWindow* window) {
  AdwDialog* question = adw_alert_dialog_new(
      "Restart Nightcap?",
      "Nightcap is running, so the changes apply after it restarts. "
      "Restarting leaves the game you are in.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(question), "later",
                                 "Later", "restart", "Restart Now", nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(question),
                                           "restart",
                                           ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(question), "restart");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(question), "later");
  g_signal_connect(question, "response", G_CALLBACK(OnRestartResponse),
                   window);
  adw_dialog_present(question, window->widget);
}

// Shows the saved card, and while Nightcap runs, offers to restart it.
void OnChangeSaved(SettingsWindow* window) {
  gtk_revealer_set_reveal_child(window->notice, TRUE);
  if (window->restarting) {
    window->saved_while_restarting = true;
    return;
  }
  // An empty lock path finds nothing running.
  const mocktail::runtime::RunningInstance instance =
      mocktail::runtime::FindRunningInstance(window->instance_lock);
  const bool can_restart = instance.running && instance.pid > 0 &&
                           !window->restart_program.empty();
  gtk_label_set_text(window->notice_label,
                     instance.running ? kRestartToApplyText : kSavedText);
  gtk_widget_set_visible(window->restart, can_restart);
  gtk_widget_set_sensitive(window->restart, TRUE);
  if (can_restart && !window->asked_restart) {
    window->asked_restart = true;
    AskRestart(window);
  }
}

// Shows how saving `row` went, then shows every row as the file now has it.
// `before` is the document's count of saved changes from before the save.
void ShowSaved(Row* row, std::size_t before, bool saved,
               const std::string& error) {
  SettingsWindow* window = row->window;
  if (!saved) {
    ShowError(window, error);
  } else if (window->document.saved_changes() != before) {
    OnChangeSaved(window);
  }
  gtk_widget_set_visible(window->undo, window->document.CanUndo());
  // On failure the rows go back to what the file holds. A text that was
  // refused stays in its field, so it can be corrected.
  RefreshRows(window, saved ? row : nullptr);
}

void Apply(Row* row, const std::string& value) {
  SettingsWindow* window = row->window;
  if (window->refreshing) {
    return;
  }
  std::string error;
  const std::size_t before = window->document.saved_changes();
  const bool saved = window->document.Set(row->setting->key, value, &error);
  ShowSaved(row, before, saved, error);
}

void OnUndo(GtkButton*, gpointer user_data) {
  auto* window = static_cast<SettingsWindow*>(user_data);
  std::string error;
  if (!window->document.Undo(&error)) {
    ShowError(window, error);
  }
  gtk_widget_set_visible(window->undo, FALSE);
  RefreshRows(window);
}

void OnNumberChanged(GObject* object, GParamSpec*, gpointer user_data) {
  Apply(static_cast<Row*>(user_data),
        mocktail::ui::NumberValue(
            adw_spin_row_get_value(ADW_SPIN_ROW(object))));
}

gboolean OnNumberOutput(AdwSpinRow* spin, gpointer user_data) {
  const Row* row = static_cast<const Row*>(user_data);
  if (adw_spin_row_get_value(spin) != 0) {
    return FALSE;
  }
  gtk_editable_set_text(GTK_EDITABLE(spin),
                        Text(row->setting->zero_label).c_str());
  return TRUE;
}

gint OnNumberInput(AdwSpinRow* spin, double* value, gpointer user_data) {
  const Row* row = static_cast<const Row*>(user_data);
  if (g_ascii_strcasecmp(gtk_editable_get_text(GTK_EDITABLE(spin)),
                         Text(row->setting->zero_label).c_str()) != 0) {
    return FALSE;
  }
  *value = 0;
  return TRUE;
}

void OnTextApplied(AdwEntryRow* entry, gpointer user_data) {
  Row* row = static_cast<Row*>(user_data);
  SettingsWindow* window = row->window;
  std::string error;
  const std::size_t before = window->document.saved_changes();
  const std::string_view text = gtk_editable_get_text(GTK_EDITABLE(entry));
  // A field with no default left empty means unset, not an empty text the
  // loader would refuse.
  const bool saved =
      text.empty() && row->setting->default_value.empty()
          ? window->document.Set(row->setting->key, text, &error)
          : window->document.SetText(row->setting->key, text, &error);
  ShowSaved(row, before, saved, error);
}

void OnReset(GtkButton*, gpointer user_data) {
  Row* row = static_cast<Row*>(user_data);
  SettingsWindow* window = row->window;
  std::string error;
  const std::size_t before = window->document.saved_changes();
  const bool saved = window->document.Reset(*row->setting, &error);
  ShowSaved(row, before, saved, error);
}

void OnToggleChanged(GObject* object, GParamSpec*, gpointer user_data) {
  Apply(static_cast<Row*>(user_data),
        adw_switch_row_get_active(ADW_SWITCH_ROW(object)) ? "true" : "false");
}

void OnExpanderToggled(GObject* object, GParamSpec*, gpointer user_data) {
  Apply(static_cast<Row*>(user_data),
        adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(object))
            ? "true"
            : "false");
}

void OnFilePicked(GObject* source, GAsyncResult* result, gpointer user_data) {
  Row* row = static_cast<Row*>(user_data);
  GError* error = nullptr;
  GFile* file =
      gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
  if (file == nullptr) {
    if (!g_error_matches(error, GTK_DIALOG_ERROR,
                         GTK_DIALOG_ERROR_DISMISSED)) {
      ShowError(row->window, error->message);
    }
    g_error_free(error);
    return;
  }
  if (char* path = g_file_get_path(file)) {
    gtk_editable_set_text(GTK_EDITABLE(row->widget), path);
    g_free(path);
    OnTextApplied(ADW_ENTRY_ROW(row->widget), row);
  }
  g_object_unref(file);
}

void OnPickFile(GtkButton*, gpointer user_data) {
  Row* row = static_cast<Row*>(user_data);
  GtkFileDialog* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, Text(row->setting->title).c_str());
  const std::filesystem::path current =
      gtk_editable_get_text(GTK_EDITABLE(row->widget));
  if (current.is_absolute()) {
    GFile* initial = g_file_new_for_path(current.c_str());
    gtk_file_dialog_set_initial_file(dialog, initial);
    g_object_unref(initial);
  }
  gtk_file_dialog_open(dialog, GTK_WINDOW(row->window->widget), nullptr,
                       OnFilePicked, row);
  g_object_unref(dialog);
}

void OnChoiceChanged(GObject* object, GParamSpec*, gpointer user_data) {
  Row* row = static_cast<Row*>(user_data);
  const guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(object));
  if (selected >= row->setting->choices.size()) {
    // The unlisted value is already what the file holds.
    return;
  }
  Apply(row, Text(row->setting->choices[selected].value));
}

void OnDeviceChanged(GObject* object, GParamSpec*, gpointer user_data) {
  Row* row = static_cast<Row*>(user_data);
  const guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(object));
  if (selected >= row->devices.size()) {
    return;
  }
  // Apply fills the rows again, which replaces row->devices.
  const std::string value = row->devices[selected].value;
  Apply(row, value);
}

void OnFrameRateChanged(GObject*, GParamSpec*, gpointer user_data) {
  Row* row = static_cast<Row*>(user_data);
  const guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row->widget));
  if (selected > static_cast<guint>(FrameRateMode::kCustom)) {
    return;
  }
  FrameRate rate;
  rate.mode = static_cast<FrameRateMode>(selected);
  rate.custom =
      static_cast<int>(adw_spin_row_get_value(ADW_SPIN_ROW(row->custom)));
  Apply(row, mocktail::ui::FrameRateValue(rate));
}

// The dialog that edits fflags.json as text.
struct FFlagsEditor {
  SettingsWindow* window = nullptr;
  AdwDialog* dialog = nullptr;
  GtkTextView* view = nullptr;
  GtkTextBuffer* buffer = nullptr;
  AdwBanner* problem = nullptr;
  GtkWidget* save = nullptr;
};

// Closing asks first while there is unsaved text. See OnFFlagsCloseAttempt.
void OnFFlagsCancel(GtkButton*, gpointer user_data) {
  adw_dialog_close(static_cast<FFlagsEditor*>(user_data)->dialog);
}

void SaveFFlags(FFlagsEditor* editor) {
  if (!gtk_text_buffer_get_modified(editor->buffer)) {
    return;
  }
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(editor->buffer, &start, &end);
  char* characters =
      gtk_text_buffer_get_text(editor->buffer, &start, &end, FALSE);
  const std::string text = characters;
  g_free(characters);
  std::string error;
  if (editor->window->fflags.Save(text, &error)) {
    // Undo cannot take back a save of fflags.json.
    editor->window->document.ForgetUndo();
    gtk_widget_set_visible(editor->window->undo, FALSE);
    SettingsWindow* window = editor->window;
    // The dialog owns `editor` and may free it on close.
    adw_dialog_force_close(editor->dialog);
    OnChangeSaved(window);
    return;
  }
  // The dialog stays open with the text, so nothing typed is lost.
  adw_banner_set_title(editor->problem, error.c_str());
  adw_banner_set_revealed(editor->problem, TRUE);
  if (const auto fault = mocktail::ui::JsonSyntaxFault(text)) {
    GtkTextIter where;
    gtk_text_buffer_get_iter_at_line_offset(
        editor->buffer, &where, static_cast<int>(fault->line - 1),
        static_cast<int>(fault->column - 1));
    gtk_text_buffer_place_cursor(editor->buffer, &where);
    gtk_text_view_scroll_to_mark(editor->view,
                                 gtk_text_buffer_get_insert(editor->buffer),
                                 0.1, FALSE, 0, 0);
  }
  gtk_widget_grab_focus(GTK_WIDGET(editor->view));
}

void OnFFlagsSave(GtkButton*, gpointer user_data) {
  SaveFFlags(static_cast<FFlagsEditor*>(user_data));
}

gboolean OnFFlagsSaveShortcut(GtkWidget*, GVariant*, gpointer user_data) {
  SaveFFlags(static_cast<FFlagsEditor*>(user_data));
  return TRUE;
}

// Save has work to do, and closing needs a question, only while the text
// differs from what was opened.
void OnFFlagsModifiedChanged(GtkTextBuffer* buffer, gpointer user_data) {
  auto* editor = static_cast<FFlagsEditor*>(user_data);
  const gboolean modified = gtk_text_buffer_get_modified(buffer);
  gtk_widget_set_sensitive(editor->save, modified);
  adw_dialog_set_can_close(editor->dialog, !modified);
}

// A problem shown for the text as it was says nothing about the text now.
void OnFFlagsChanged(GtkTextBuffer*, gpointer user_data) {
  adw_banner_set_revealed(static_cast<FFlagsEditor*>(user_data)->problem,
                          FALSE);
}

void OnFFlagsDiscardResponse(AdwAlertDialog*, const char* response,
                             gpointer user_data) {
  if (g_strcmp0(response, "discard") == 0) {
    adw_dialog_force_close(static_cast<FFlagsEditor*>(user_data)->dialog);
  }
}

// Runs when the dialog is asked to close with unsaved text in it.
void OnFFlagsCloseAttempt(AdwDialog* dialog, gpointer user_data) {
  AdwDialog* question = adw_alert_dialog_new(
      "Discard changes?", "Your changes to fflags.json are not saved.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(question), "keep",
                                 "Keep Editing", "discard", "Discard",
                                 nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(question),
                                           "discard",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(question), "keep");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(question), "keep");
  g_signal_connect(question, "response", G_CALLBACK(OnFFlagsDiscardResponse),
                   user_data);
  adw_dialog_present(question, GTK_WIDGET(dialog));
}

void OnFFlagsActivated(AdwActionRow*, gpointer user_data) {
  auto* window = static_cast<SettingsWindow*>(user_data);
  std::string error;
  if (!window->fflags.Load(&error)) {
    ShowError(window, error);
    return;
  }

  auto* editor = new FFlagsEditor;
  editor->window = window;
  editor->dialog = adw_dialog_new();
  adw_dialog_set_title(editor->dialog, "FFlag overrides");
  adw_dialog_set_content_width(editor->dialog, 640);
  adw_dialog_set_content_height(editor->dialog, 520);
  g_object_set_data_full(
      G_OBJECT(editor->dialog), "editor", editor,
      [](gpointer data) { delete static_cast<FFlagsEditor*>(data); });

  GtkWidget* cancel = gtk_button_new_with_label("Cancel");
  GtkWidget* save = gtk_button_new_with_label("Save");
  editor->save = save;
  gtk_widget_add_css_class(save, "suggested-action");
  gtk_widget_set_tooltip_text(save, "Save (Ctrl+S)");
  gtk_widget_set_sensitive(save, FALSE);
  g_signal_connect(cancel, "clicked", G_CALLBACK(OnFFlagsCancel), editor);
  g_signal_connect(save, "clicked", G_CALLBACK(OnFFlagsSave), editor);
  g_signal_connect(editor->dialog, "close-attempt",
                   G_CALLBACK(OnFFlagsCloseAttempt), editor);
  GtkEventController* shortcuts = gtk_shortcut_controller_new();
  gtk_shortcut_controller_add_shortcut(
      GTK_SHORTCUT_CONTROLLER(shortcuts),
      gtk_shortcut_new(
          gtk_keyval_trigger_new(GDK_KEY_s, GDK_CONTROL_MASK),
          gtk_callback_action_new(OnFFlagsSaveShortcut, editor, nullptr)));
  gtk_widget_add_controller(GTK_WIDGET(editor->dialog), shortcuts);
  GtkWidget* header = adw_header_bar_new();
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), save);

  GtkWidget* problem = adw_banner_new("");
  editor->problem = ADW_BANNER(problem);
  adw_banner_set_use_markup(editor->problem, FALSE);

  GtkWidget* view = gtk_text_view_new();
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 12);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 12);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 12);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 12);
  gtk_accessible_update_property(GTK_ACCESSIBLE(view),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "fflags.json",
                                 -1);
  editor->view = GTK_TEXT_VIEW(view);
  editor->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  // Undo must not go back past the file's own text.
  gtk_text_buffer_begin_irreversible_action(editor->buffer);
  gtk_text_buffer_set_text(editor->buffer, window->fflags.Text().c_str(), -1);
  gtk_text_buffer_end_irreversible_action(editor->buffer);
  gtk_text_buffer_set_modified(editor->buffer, FALSE);
  GtkTextIter top;
  gtk_text_buffer_get_start_iter(editor->buffer, &top);
  gtk_text_buffer_place_cursor(editor->buffer, &top);
  g_signal_connect(editor->buffer, "modified-changed",
                   G_CALLBACK(OnFFlagsModifiedChanged), editor);
  g_signal_connect(editor->buffer, "changed", G_CALLBACK(OnFFlagsChanged),
                   editor);
  GtkWidget* scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);

  GtkWidget* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), problem);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scrolled);
  adw_dialog_set_child(editor->dialog, toolbar);
  adw_dialog_present(editor->dialog, window->widget);
  gtk_widget_grab_focus(view);
}

void OnFolderOpened(GObject* source, GAsyncResult* result,
                    gpointer user_data) {
  GError* error = nullptr;
  if (gtk_file_launcher_open_containing_folder_finish(
          GTK_FILE_LAUNCHER(source), result, &error)) {
    return;
  }
  if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
    ShowError(static_cast<SettingsWindow*>(user_data), error->message);
  }
  g_error_free(error);
}

// Shows config.yaml in the file manager.
void OnFolderActivated(AdwActionRow*, gpointer user_data) {
  auto* window = static_cast<SettingsWindow*>(user_data);
  GFile* file = g_file_new_for_path(window->document.config_file().c_str());
  GtkFileLauncher* launcher = gtk_file_launcher_new(file);
  gtk_file_launcher_open_containing_folder(
      launcher, GTK_WINDOW(window->widget), nullptr, OnFolderOpened, window);
  g_object_unref(launcher);
  g_object_unref(file);
}

void SetRowText(GtkWidget* widget, std::string_view title,
                std::string_view subtitle) {
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(widget), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widget),
                                Text(title).c_str());
  // A text row shows its title inside the field and has no subtitle.
  if (ADW_IS_ACTION_ROW(widget)) {
    adw_action_row_set_subtitle(ADW_ACTION_ROW(widget),
                                Text(subtitle).c_str());
  }
}

void AddToContainer(GtkWidget* container, GtkWidget* widget) {
  if (ADW_IS_EXPANDER_ROW(container)) {
    adw_expander_row_add_row(ADW_EXPANDER_ROW(container), widget);
  } else {
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(container), widget);
  }
}

GtkWidget* NewResetButton(Row* row) {
  GtkWidget* button = gtk_button_new_from_icon_name("edit-undo-symbolic");
  gtk_widget_set_tooltip_text(button, "Reset to default");
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "row-reset");
  gtk_widget_set_visible(button, FALSE);
  g_signal_connect(button, "clicked", G_CALLBACK(OnReset), row);
  return button;
}

void AddRow(SettingsWindow* window, GtkWidget* container,
            const Setting& setting, bool expander_switch = false) {
  Row& row = window->rows.emplace_back();
  row.window = window;
  row.setting = &setting;
  if (expander_switch) {
    row.widget = container;
    row.expander_switch = true;
    g_signal_connect(container, "notify::enable-expansion",
                     G_CALLBACK(OnExpanderToggled), &row);
    return;
  }
  switch (setting.kind) {
    case SettingKind::kToggle:
      row.widget = adw_switch_row_new();
      g_signal_connect(row.widget, "notify::active",
                       G_CALLBACK(OnToggleChanged), &row);
      break;
    case SettingKind::kChoice:
      row.widget = adw_combo_row_new();
      g_signal_connect(row.widget, "notify::selected",
                       G_CALLBACK(OnChoiceChanged), &row);
      break;
    case SettingKind::kDevice:
      row.widget = adw_combo_row_new();
      g_signal_connect(row.widget, "notify::selected",
                       G_CALLBACK(OnDeviceChanged), &row);
      break;
    case SettingKind::kFrameRate:
      row.widget = adw_combo_row_new();
      SetChoices(&row);
      row.custom = adw_spin_row_new_with_range(
          1, mocktail::ui::kMaximumCustomFrameRate, 1);
      adw_spin_row_set_value(ADW_SPIN_ROW(row.custom),
                             mocktail::ui::kDefaultCustomFrameRate);
      SetRowText(row.custom, "Custom limit", "Frames per second");
      g_signal_connect(row.widget, "notify::selected",
                       G_CALLBACK(OnFrameRateChanged), &row);
      g_signal_connect(row.custom, "notify::value",
                       G_CALLBACK(OnFrameRateChanged), &row);
      break;
    case SettingKind::kNumber:
      row.widget = adw_spin_row_new_with_range(setting.minimum,
                                               setting.maximum, setting.step);
      g_signal_connect(row.widget, "notify::value",
                       G_CALLBACK(OnNumberChanged), &row);
      if (!setting.zero_label.empty()) {
        // A numeric row drops the label's letters.
        adw_spin_row_set_numeric(ADW_SPIN_ROW(row.widget), FALSE);
        g_signal_connect(row.widget, "output", G_CALLBACK(OnNumberOutput),
                         &row);
        g_signal_connect(row.widget, "input", G_CALLBACK(OnNumberInput), &row);
      }
      break;
    case SettingKind::kText:
    case SettingKind::kFile:
      row.widget = adw_entry_row_new();
      adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(row.widget), TRUE);
      row.reset = NewResetButton(&row);
      adw_entry_row_add_suffix(ADW_ENTRY_ROW(row.widget), row.reset);
      g_signal_connect(row.widget, "apply", G_CALLBACK(OnTextApplied), &row);
      if (setting.kind == SettingKind::kFile) {
        row.pick = gtk_button_new_from_icon_name("document-open-symbolic");
        gtk_widget_set_tooltip_text(row.pick, "Choose a file");
        gtk_widget_set_valign(row.pick, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(row.pick, "flat");
        // The picker stands in for the pencil.
        HideDescendant(row.widget, "edit-icon");
        adw_entry_row_add_suffix(ADW_ENTRY_ROW(row.widget), row.pick);
        g_signal_connect(row.pick, "clicked", G_CALLBACK(OnPickFile), &row);
      }
      break;
    case SettingKind::kEditor:
      row.widget = adw_action_row_new();
      adw_action_row_add_suffix(
          ADW_ACTION_ROW(row.widget),
          gtk_image_new_from_icon_name("go-next-symbolic"));
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row.widget), TRUE);
      g_signal_connect(row.widget, "activated",
                       G_CALLBACK(OnFFlagsActivated), window);
      break;
    case SettingKind::kFolder:
      row.widget = adw_action_row_new();
      adw_action_row_add_suffix(
          ADW_ACTION_ROW(row.widget),
          gtk_image_new_from_icon_name("folder-open-symbolic"));
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row.widget), TRUE);
      g_signal_connect(row.widget, "activated",
                       G_CALLBACK(OnFolderActivated), window);
      break;
  }
  if (row.reset == nullptr && setting.kind != SettingKind::kEditor &&
      setting.kind != SettingKind::kFolder) {
    row.reset = NewResetButton(&row);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row.widget), row.reset);
    // Before the row's own control, so the control stays at the edge.
    if (GtkWidget* suffixes = gtk_widget_get_parent(row.reset);
        GTK_IS_BOX(suffixes)) {
      gtk_box_reorder_child_after(GTK_BOX(suffixes), row.reset, nullptr);
    }
  }
  SetRowText(row.widget, setting.title, setting.subtitle);
  AddToContainer(container, row.widget);
  if (row.custom != nullptr) {
    AddToContainer(container, row.custom);
  }
}

const SettingsPage& PageOf(GtkListBoxRow* row) {
  return mocktail::ui::SettingsPages()[static_cast<std::size_t>(
      gtk_list_box_row_get_index(row))];
}

// Puts a section name above the first sidebar row of each section.
void SetSidebarHeader(GtkListBoxRow* row, GtkListBoxRow* before, gpointer) {
  const SettingsPage& page = PageOf(row);
  if (before != nullptr && PageOf(before).section == page.section) {
    gtk_list_box_row_set_header(row, nullptr);
    return;
  }
  char* upper = g_utf8_strup(page.section.data(),
                             static_cast<gssize>(page.section.size()));
  GtkWidget* label = gtk_label_new(upper);
  g_free(upper);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_add_css_class(label, "caption-heading");
  gtk_widget_add_css_class(label, "dim-label");
  gtk_widget_set_margin_start(label, 14);
  gtk_widget_set_margin_top(label, before == nullptr ? 6 : 18);
  gtk_widget_set_margin_bottom(label, 6);
  gtk_list_box_row_set_header(row, label);
}

void OnPageSelected(GtkListBox*, GtkListBoxRow* row, gpointer user_data) {
  auto* window = static_cast<SettingsWindow*>(user_data);
  if (row == nullptr) {
    return;
  }
  const SettingsPage& page = PageOf(row);
  gtk_stack_set_visible_child_name(window->stack, Text(page.name).c_str());
  adw_navigation_page_set_title(window->content_page,
                                Text(page.title).c_str());
}

// On a narrow window the sidebar and the settings take turns.
void OnPageActivated(GtkListBox*, GtkListBoxRow*, gpointer user_data) {
  adw_navigation_split_view_set_show_content(
      static_cast<SettingsWindow*>(user_data)->split, TRUE);
}

// The Nightcap artwork beside the name. The artwork is built in, so it shows
// whether or not the icon theme has Nightcap's icon installed.
GtkWidget* BuildSidebarTitle() {
  // The artwork has clear space around it, so the gap to the name is small.
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GBytes* bytes = g_bytes_new_static(
      mocktail::platform::internal::kMocktailWindowIconPng,
      mocktail::platform::internal::kMocktailWindowIconPngSize);
  if (GdkTexture* texture = gdk_texture_new_from_bytes(bytes, nullptr)) {
    GtkWidget* image = gtk_image_new_from_paintable(GDK_PAINTABLE(texture));
    gtk_image_set_pixel_size(GTK_IMAGE(image), 24);
    gtk_box_append(GTK_BOX(box), image);
    g_object_unref(texture);
  }
  g_bytes_unref(bytes);
  GtkWidget* label = gtk_label_new("Nightcap");
  gtk_widget_add_css_class(label, "title");
  gtk_box_append(GTK_BOX(box), label);
  return box;
}

void OnChangedChipClicked(GtkButton*, gpointer user_data);

GtkWidget* BuildSidebar(SettingsWindow* window) {
  GtkWidget* list = gtk_list_box_new();
  gtk_widget_add_css_class(list, "navigation-sidebar");
  window->sidebar = GTK_LIST_BOX(list);
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_BROWSE);
  gtk_list_box_set_header_func(GTK_LIST_BOX(list), SetSidebarHeader, nullptr,
                               nullptr);
  for (const SettingsPage& page : mocktail::ui::SettingsPages()) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(box),
                   gtk_image_new_from_icon_name(Text(page.icon_name).c_str()));
    GtkWidget* label = gtk_label_new(Text(page.title).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);
    GtkWidget* badge = gtk_label_new("");
    gtk_widget_add_css_class(badge, "count-badge");
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(badge, "Settings changed from the default");
    gtk_widget_set_visible(badge, FALSE);
    gtk_box_append(GTK_BOX(box), badge);
    window->badges.push_back(badge);
    gtk_list_box_append(GTK_LIST_BOX(list), box);
  }
  g_signal_connect(list, "row-selected", G_CALLBACK(OnPageSelected), window);
  g_signal_connect(list, "row-activated", G_CALLBACK(OnPageActivated),
                   window);

  GtkWidget* scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);

  // The artwork and name sit at the left, in line with the rows below.
  GtkWidget* header = adw_header_bar_new();
  adw_header_bar_set_show_title(ADW_HEADER_BAR(header), FALSE);
  GtkWidget* title = BuildSidebarTitle();
  gtk_widget_set_margin_start(title, 4);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), title);
  window->changed_chip = gtk_button_new();
  gtk_widget_add_css_class(window->changed_chip, "changed-chip");
  gtk_widget_set_valign(window->changed_chip, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(window->changed_chip,
                              "Go to the first changed setting");
  gtk_widget_set_visible(window->changed_chip, FALSE);
  g_signal_connect(window->changed_chip, "clicked",
                   G_CALLBACK(OnChangedChipClicked), window);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), window->changed_chip);
  GtkWidget* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scrolled);
  gtk_list_box_select_row(
      GTK_LIST_BOX(list),
      gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
  return toolbar;
}

GtkWidget* BuildNotice(SettingsWindow* window) {
  GtkWidget* icon =
      gtk_image_new_from_icon_name("dialog-information-symbolic");
  GtkWidget* label = gtk_label_new(kSavedText);
  window->notice_label = GTK_LABEL(label);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_set_margin_start(icon, 14);
  gtk_widget_set_margin_end(label, 14);
  window->undo = gtk_button_new_with_label("Undo");
  gtk_widget_set_tooltip_text(window->undo, "Take the last change back");
  gtk_widget_set_valign(window->undo, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end(window->undo, 8);
  gtk_widget_add_css_class(window->undo, "flat");
  g_signal_connect(window->undo, "clicked", G_CALLBACK(OnUndo), window);
  window->restart = gtk_button_new_with_label("Restart Now");
  gtk_widget_set_valign(window->restart, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end(window->restart, 8);
  gtk_widget_add_css_class(window->restart, "suggested-action");
  gtk_widget_set_visible(window->restart, FALSE);
  g_signal_connect(window->restart, "clicked", G_CALLBACK(OnRestartClicked),
                   window);
  gtk_widget_set_margin_top(label, 12);
  gtk_widget_set_margin_bottom(label, 12);

  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(bar, "notice-bar");
  gtk_box_append(GTK_BOX(bar), icon);
  gtk_box_append(GTK_BOX(bar), label);
  gtk_box_append(GTK_BOX(bar), window->undo);
  gtk_box_append(GTK_BOX(bar), window->restart);

  GtkWidget* revealer = gtk_revealer_new();
  window->notice = GTK_REVEALER(revealer);
  gtk_revealer_set_transition_type(window->notice,
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_child(window->notice, bar);
  return revealer;
}

GtkWidget* BuildPages(SettingsWindow* window) {
  GtkWidget* stack = gtk_stack_new();
  window->stack = GTK_STACK(stack);
  const auto& pages = mocktail::ui::SettingsPages();
  for (std::size_t page_index = 0; page_index < pages.size(); ++page_index) {
    const SettingsPage& page = pages[page_index];
    const std::size_t first_row = window->rows.size();
    // A clamp of our own instead of AdwPreferencesPage, whose fixed 600 px
    // clamp leaves wide empty gutters.
    GtkWidget* groups = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_widget_set_margin_top(groups, 24);
    gtk_widget_set_margin_bottom(groups, 24);
    gtk_widget_set_margin_start(groups, 14);
    gtk_widget_set_margin_end(groups, 14);
    GtkWidget* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 720);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 720);
    adw_clamp_set_child(ADW_CLAMP(clamp), groups);
    GtkWidget* page_widget = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page_widget),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(page_widget), clamp);
    for (const SettingsGroup& group : page.groups) {
      GtkWidget* group_widget = adw_preferences_group_new();
      adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_widget),
                                      Text(group.title).c_str());
      adw_preferences_group_set_description(
          ADW_PREFERENCES_GROUP(group_widget),
          group.description.empty() ? nullptr
                                    : Text(group.description).c_str());
      GtkWidget* container = group_widget;
      std::size_t first = 0;
      if (!group.expander.empty()) {
        container = adw_expander_row_new();
        SetRowText(container, group.expander, {});
        adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(container),
                                      Text(group.expander_subtitle).c_str());
        if (group.expander_switch && !group.settings.empty()) {
          adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(container),
                                                  TRUE);
          AddRow(window, container, group.settings.front(), true);
          first = 1;
        }
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_widget),
                                  container);
      }
      for (std::size_t index = first; index < group.settings.size();
           ++index) {
        AddRow(window, container, group.settings[index]);
      }
      gtk_box_append(GTK_BOX(groups), group_widget);
    }
    for (std::size_t index = first_row; index < window->rows.size(); ++index) {
      window->rows[index].page = page_index;
    }
    gtk_stack_add_named(GTK_STACK(stack), page_widget,
                        Text(page.name).c_str());
  }
  return stack;
}

void OnChangedChipClicked(GtkButton*, gpointer user_data) {
  auto* window = static_cast<SettingsWindow*>(user_data);
  for (Row& row : window->rows) {
    if (!window->document.IsChanged(*row.setting)) {
      continue;
    }
    gtk_list_box_select_row(
        window->sidebar,
        gtk_list_box_get_row_at_index(window->sidebar,
                                      static_cast<int>(row.page)));
    adw_navigation_split_view_set_show_content(window->split, TRUE);
    // A row inside a closed expander cannot take focus. One whose switch is
    // off cannot open, so its own row takes the focus.
    GtkWidget* target = row.widget;
    for (GtkWidget* parent = gtk_widget_get_parent(row.widget);
         parent != nullptr; parent = gtk_widget_get_parent(parent)) {
      if (!ADW_IS_EXPANDER_ROW(parent)) {
        continue;
      }
      AdwExpanderRow* expander = ADW_EXPANDER_ROW(parent);
      if (adw_expander_row_get_enable_expansion(expander)) {
        adw_expander_row_set_expanded(expander, TRUE);
      } else {
        target = parent;
      }
    }
    gtk_widget_grab_focus(target);
    return;
  }
}

// The sidebar of pages beside the settings of the chosen one.
GtkWidget* BuildSplitView(SettingsWindow* window,
                          AdwApplicationWindow* window_widget) {
  GtkWidget* split = adw_navigation_split_view_new();
  window->split = ADW_NAVIGATION_SPLIT_VIEW(split);
  adw_navigation_split_view_set_min_sidebar_width(window->split, 224);
  adw_navigation_split_view_set_max_sidebar_width(window->split, 260);

  GtkWidget* toasts = adw_toast_overlay_new();
  window->toasts = ADW_TOAST_OVERLAY(toasts);
  adw_toast_overlay_set_child(window->toasts, BuildPages(window));
  GtkWidget* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                               adw_header_bar_new());
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar),
                                  BuildNotice(window));
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), toasts);
  window->content_page = adw_navigation_page_new(toolbar, "Settings");
  adw_navigation_split_view_set_content(window->split, window->content_page);

  // The sidebar selects its first row, which needs the pages above.
  adw_navigation_split_view_set_sidebar(
      window->split,
      adw_navigation_page_new(BuildSidebar(window), "Nightcap"));

  AdwBreakpoint* narrow =
      adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 620sp"));
  GValue collapsed = G_VALUE_INIT;
  g_value_init(&collapsed, G_TYPE_BOOLEAN);
  g_value_set_boolean(&collapsed, TRUE);
  adw_breakpoint_add_setter(narrow, G_OBJECT(split), "collapsed", &collapsed);
  g_value_unset(&collapsed);
  adw_application_window_add_breakpoint(window_widget, narrow);
  return split;
}

GtkWidget* BuildProblemPage(const SettingsWindow& window,
                            const std::string& error) {
  GtkWidget* page = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(page),
                                "dialog-warning-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(page),
                            "The settings file has a problem");
  char* description = g_markup_printf_escaped(
      "%s\n\nFix or remove this file, then open Settings again:\n%s",
      error.c_str(), window.document.config_file().c_str());
  adw_status_page_set_description(ADW_STATUS_PAGE(page), description);
  g_free(description);
  return page;
}

void OnDiscardTextResponse(AdwAlertDialog*, const char* response,
                           gpointer user_data) {
  if (g_strcmp0(response, "discard") == 0) {
    gtk_window_destroy(
        GTK_WINDOW(static_cast<SettingsWindow*>(user_data)->widget));
  }
}

// Closing asks first while a text row holds text that is not applied.
gboolean OnCloseRequest(GtkWindow*, gpointer user_data) {
  auto* window = static_cast<SettingsWindow*>(user_data);
  const Row* edited = nullptr;
  for (const Row& row : window->rows) {
    if ((row.setting->kind == SettingKind::kText ||
         row.setting->kind == SettingKind::kFile) &&
        row.shown != gtk_editable_get_text(GTK_EDITABLE(row.widget))) {
      edited = &row;
      break;
    }
  }
  if (edited == nullptr) {
    return FALSE;
  }
  const std::string body = "The text typed into \"" +
                           Text(edited->setting->title) +
                           "\" is not applied.";
  AdwDialog* question = adw_alert_dialog_new("Discard changes?", nullptr);
  adw_alert_dialog_set_body(ADW_ALERT_DIALOG(question), body.c_str());
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(question), "keep",
                                 "Keep Editing", "discard", "Discard",
                                 nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(question),
                                           "discard",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(question), "keep");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(question), "keep");
  g_signal_connect(question, "response", G_CALLBACK(OnDiscardTextResponse),
                   window);
  adw_dialog_present(question, window->widget);
  return TRUE;
}

void OnWindowDestroy(GtkWidget*, gpointer user_data) {
  static_cast<SettingsWindow*>(user_data)->closed = true;
}

void OnActivate(GtkApplication* application, gpointer user_data) {
  auto* window = static_cast<SettingsWindow*>(user_data);
  if (GtkWindow* existing = gtk_application_get_active_window(application)) {
    gtk_window_present(existing);
    return;
  }

  GtkCssProvider* style = gtk_css_provider_new();
  gtk_css_provider_load_from_string(style, kWindowStyle);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(style),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(style);

  GtkWidget* window_widget = adw_application_window_new(application);
  window->widget = window_widget;
  gtk_window_set_title(GTK_WINDOW(window_widget), "Nightcap Settings");
  gtk_window_set_default_size(GTK_WINDOW(window_widget), 860, 620);
  gtk_widget_set_size_request(window_widget, 360, 420);
  gtk_window_set_icon_name(GTK_WINDOW(window_widget), kApplicationId);
  g_signal_connect(window_widget, "close-request",
                   G_CALLBACK(OnCloseRequest), window);
  g_signal_connect(window_widget, "destroy", G_CALLBACK(OnWindowDestroy),
                   window);

  GtkWidget* content = nullptr;
  std::string error;
  if (window->document.Load(&error)) {
    window->output_devices = AudioDeviceNames(false);
    window->input_devices = AudioDeviceNames(true);
    content = BuildSplitView(window, ADW_APPLICATION_WINDOW(window_widget));
    RefreshRows(window);
  } else {
    content = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(content),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content),
                                 BuildProblemPage(*window, error));
  }

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window_widget),
                                     content);
  gtk_window_present(GTK_WINDOW(window_widget));
}

}  // namespace

int main(int argc, char* argv[]) {
  g_set_application_name("Nightcap Settings");
  const mocktail::runtime::RuntimePaths paths =
      mocktail::runtime::RuntimePaths::FromEnvironment(
          mocktail::runtime::ProcessEnvironment());
  SettingsWindow window(paths.config_file(),
                        paths.config_root() / "fflags.json");
  const auto absolute_path = [](const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::filesystem::path(value).is_absolute()
               ? std::filesystem::path(value)
               : std::filesystem::path();
  };
  window.instance_lock =
      absolute_path(mocktail::runtime::kInstanceLockVariable);
  window.restart_program =
      absolute_path(mocktail::runtime::kRestartProgramVariable);

  // The application ID matches the desktop entry so the window gets
  // Nightcap's icon. Nightcap itself does not own the name on the session
  // bus, and neither does this window.
  AdwApplication* application =
      adw_application_new(kApplicationId, G_APPLICATION_NON_UNIQUE);
  g_signal_connect(application, "activate", G_CALLBACK(OnActivate), &window);
  const int status =
      g_application_run(G_APPLICATION(application), argc, argv);
  g_object_unref(application);
  return status;
}
