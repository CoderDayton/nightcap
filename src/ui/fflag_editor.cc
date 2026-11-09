#include "fflag_editor.h"

#include <adwaita.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "accessible_rows.h"
#include "icon_names.h"
#include "runtime/runtime_config_store.h"
#include "ui/fflag_presets.h"

namespace mocktail {
namespace ui {
namespace {

enum class View {
  kPreset,
  kGroup,
  kSearch,
};

struct Editor;

// What a row's signal handlers need. Rows are found again by name, since
// indexes into a preset move when flags are added or removed.
struct RowData {
  Editor* editor = nullptr;
  std::string name;
  const CatalogFlag* flag = nullptr;
  std::size_t level = kNoLevel;
  GtkWidget* value = nullptr;
};

struct Editor {
  FFlagEditorHost host;
  FFlagPresets presets;
  // The presets file as read or last saved.
  std::string presets_disk;
  bool dirty = false;
  // Set while widgets are built, so their first values are not changes.
  bool filling = false;
  // A screen reader can press Cancel again behind an open question.
  bool asking_discard = false;

  AdwDialog* dialog = nullptr;
  AdwNavigationSplitView* split = nullptr;
  GtkListBox* preset_list = nullptr;
  GtkListBox* group_list = nullptr;
  GtkSearchEntry* search = nullptr;
  AdwToolbarView* content = nullptr;
  AdwWindowTitle* title = nullptr;
  AdwBanner* problem = nullptr;
  GtkWidget* save = nullptr;
  GtkWidget* use = nullptr;
  GtkWidget* menu = nullptr;
  GSimpleActionGroup* actions = nullptr;

  View view = View::kPreset;
  std::string shown;
  std::string group;
  std::vector<std::unique_ptr<RowData>> rows;
  // Expires with the editor, for callbacks that may outlive it.
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

std::string Escaped(std::string_view text) {
  char* escaped = g_markup_escape_text(text.data(),
                                       static_cast<gssize>(text.size()));
  std::string result = escaped;
  g_free(escaped);
  return result;
}

std::string FlagMarkup(std::string_view name) {
  return "<span font_family=\"monospace\">" + Escaped(name) + "</span>";
}

Preset* Shown(Editor* editor) { return editor->presets.Find(editor->shown); }

// The preset the Add buttons add to: the one last shown, else the active one.
Preset* Target(Editor* editor) {
  Preset* shown = Shown(editor);
  return shown != nullptr ? shown
                          : editor->presets.Find(editor->presets.active);
}

void ShowProblem(Editor* editor, const std::string& error) {
  adw_banner_set_title(editor->problem, error.c_str());
  adw_banner_set_revealed(editor->problem, TRUE);
}

void SetDirty(Editor* editor, bool dirty) {
  editor->dirty = dirty;
  gtk_widget_set_sensitive(editor->save, dirty);
  adw_dialog_set_can_close(editor->dialog, !dirty);
}

void Changed(Editor* editor) {
  if (!editor->filling) {
    SetDirty(editor, true);
  }
}

bool SavePresetsFile(Editor* editor, std::string* error) {
  const std::string text = PresetsText(editor->presets);
  if (text == editor->presets_disk) {
    return true;
  }
  const runtime::RuntimeConfigSaveResult saved = runtime::SaveCheckedTextFile(
      editor->host.presets_file, editor->presets_disk, text,
      [](const std::filesystem::path& candidate) {
        const runtime::RuntimeConfigText read =
            runtime::ReadOptionalTextFile(candidate);
        if (!read) {
          return read.error;
        }
        FFlagPresets check;
        std::string check_error;
        LoadPresets(read.text, {}, &check, &check_error);
        return check_error;
      });
  if (!saved) {
    *error = saved.error;
    return false;
  }
  editor->presets_disk = text;
  return true;
}

// fflags.json is written only when its flags change, so a hand-written file
// keeps its comments and layout otherwise.
bool SaveAll(Editor* editor) {
  const Preset* active = editor->presets.Find(editor->presets.active);
  std::vector<PresetFlag> disk;
  std::string error;
  bool fflags_saved = false;
  if (active != nullptr &&
      (!ParseFFlags(editor->host.fflags->Text(), &disk, &error) ||
       !HoldsFlags(*active, disk))) {
    if (!editor->host.fflags->Save(FFlagsText(*active), &error)) {
      ShowProblem(editor, error);
      return false;
    }
    fflags_saved = true;
  }
  if (!SavePresetsFile(editor, &error)) {
    ShowProblem(editor, "Your flags are saved, but your presets are not: " +
                            error);
    if (fflags_saved && editor->host.saved) {
      editor->host.saved();
    }
    return false;
  }
  SetDirty(editor, false);
  if (fflags_saved && editor->host.saved) {
    editor->host.saved();
  }
  return true;
}

void OnSave(GtkButton*, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  if (SaveAll(editor)) {
    adw_dialog_force_close(editor->dialog);
  }
}

gboolean OnSaveShortcut(GtkWidget*, GVariant*, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  if (editor->dirty) {
    OnSave(nullptr, data);
  }
  return TRUE;
}

void OnCancel(GtkButton*, gpointer data) {
  adw_dialog_close(static_cast<Editor*>(data)->dialog);
}

void OnDiscardResponse(AdwAlertDialog*, const char* response, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  editor->asking_discard = false;
  if (g_strcmp0(response, "discard") == 0) {
    adw_dialog_force_close(editor->dialog);
  }
}

void OnCloseAttempt(AdwDialog* dialog, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  if (editor->asking_discard) return;
  editor->asking_discard = true;
  AdwDialog* question = adw_alert_dialog_new(
      "Discard changes?", "Your changes to your flags are not saved.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(question), "keep",
                                 "Keep Editing", "discard", "Discard",
                                 nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(question),
                                           "discard",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(question), "keep");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(question), "keep");
  g_signal_connect(question, "response", G_CALLBACK(OnDiscardResponse), data);
  adw_dialog_present(question, GTK_WIDGET(dialog));
}

void Refresh(Editor* editor);
void FillContent(Editor* editor);

struct RefreshRequest {
  Editor* editor = nullptr;
  std::weak_ptr<bool> alive;
};

gboolean RefreshLater(gpointer data) {
  auto* request = static_cast<RefreshRequest*>(data);
  if (!request->alive.expired()) {
    Refresh(request->editor);
  }
  return G_SOURCE_REMOVE;
}

// The dialog may close before the idle runs.
void RefreshSoon(Editor* editor) {
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, RefreshLater,
                  new RefreshRequest{editor, editor->alive},
                  [](gpointer data) {
                    delete static_cast<RefreshRequest*>(data);
                  });
}

RowData* NewRowData(Editor* editor, std::string_view name,
                    const CatalogFlag* flag, std::size_t level = kNoLevel) {
  auto data = std::make_unique<RowData>();
  data->editor = editor;
  data->name = std::string(name);
  data->flag = flag;
  data->level = level;
  editor->rows.push_back(std::move(data));
  return editor->rows.back().get();
}

bool FindRow(Editor* editor, std::string_view name, PresetRow* found) {
  const Preset* preset = Shown(editor);
  if (preset == nullptr) {
    return false;
  }
  for (const PresetRow& row : PresetRows(*preset)) {
    if (preset->flags[row.entries[0]].name == name) {
      *found = row;
      return true;
    }
  }
  return false;
}

std::string ValueText(const Preset& preset, const PresetRow& row) {
  if (row.flag == nullptr) {
    return "";
  }
  if (row.flag->levels.empty()) {
    return std::to_string(
        RowNumber(preset, row, row.flag->default_number));
  }
  if (row.flag->levels.size() == 1) {
    return row.level == kNoLevel ? "Custom" : "";
  }
  return row.level == kNoLevel ? "Custom"
                               : std::string(row.flag->levels[row.level].label);
}

// Shown only when the row is opened, to keep the list plain.
GtkWidget* EffectsRow(const CatalogFlag& flag) {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 10);
  gtk_widget_set_margin_bottom(box, 10);
  for (const FlagEffect& effect : flag.effects) {
    GtkWidget* badge = gtk_label_new(std::string(effect.text).c_str());
    gtk_widget_add_css_class(badge, "status-badge");
    gtk_widget_add_css_class(badge, effect.cost ? "warn" : "ok");
    gtk_box_append(GTK_BOX(box), badge);
  }
  GtkWidget* row = gtk_list_box_row_new();
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  return row;
}

GtkWidget* DetailRow(const std::string& text) {
  GtkWidget* label = gtk_label_new(text.c_str());
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_margin_start(label, 12);
  gtk_widget_set_margin_end(label, 12);
  gtk_widget_set_margin_top(label, 10);
  gtk_widget_set_margin_bottom(label, 10);
  GtkWidget* row = gtk_list_box_row_new();
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
  return row;
}

std::string DetailText(const CatalogFlag& flag) {
  std::string text(flag.details.empty() ? flag.summary : flag.details);
  if (flag.names.size() > 1) {
    text += "\n\nAlso sets ";
    for (std::size_t i = 1; i < flag.names.size(); ++i) {
      if (i > 1) {
        text += i + 1 == flag.names.size() ? " and " : ", ";
      }
      text += flag.names[i];
    }
    text += ".";
  }
  return text;
}

void OnRowSwitch(GObject* toggle, GParamSpec*, gpointer data) {
  auto* row_data = static_cast<RowData*>(data);
  Editor* editor = row_data->editor;
  PresetRow row;
  if (editor->filling || !FindRow(editor, row_data->name, &row)) {
    return;
  }
  SetRowOn(Shown(editor), row, gtk_switch_get_active(GTK_SWITCH(toggle)));
  Changed(editor);
}

void OnLevelToggled(GtkCheckButton* check, gpointer data) {
  auto* row_data = static_cast<RowData*>(data);
  Editor* editor = row_data->editor;
  PresetRow row;
  if (editor->filling || !gtk_check_button_get_active(check) ||
      !FindRow(editor, row_data->name, &row)) {
    return;
  }
  SetRowLevel(Shown(editor), row, row_data->level);
  if (row_data->value != nullptr) {
    gtk_label_set_text(
        GTK_LABEL(row_data->value),
        std::string(row_data->flag->levels[row_data->level].label).c_str());
  }
  Changed(editor);
}

void OnNumberChanged(GObject* spin, GParamSpec*, gpointer data) {
  auto* row_data = static_cast<RowData*>(data);
  Editor* editor = row_data->editor;
  PresetRow row;
  if (editor->filling || !FindRow(editor, row_data->name, &row)) {
    return;
  }
  const int number =
      static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin)));
  SetRowNumber(Shown(editor), row, number);
  if (row_data->value != nullptr) {
    gtk_label_set_text(GTK_LABEL(row_data->value),
                       std::to_string(number).c_str());
  }
  Changed(editor);
}

void OnUseRecommended(GtkWidget*, gpointer data) {
  auto* row_data = static_cast<RowData*>(data);
  Editor* editor = row_data->editor;
  PresetRow row;
  if (!FindRow(editor, row_data->name, &row)) {
    return;
  }
  SetRowLevel(Shown(editor), row, 0);
  Changed(editor);
  RefreshSoon(editor);
}

// What the row's flags are set to, for values no level stands for.
std::string CurrentValues(const Preset& preset, const PresetRow& row) {
  std::string text = "Set to ";
  for (std::size_t i = 0; i < row.entries.size(); ++i) {
    const PresetFlag& flag = preset.flags[row.entries[i]];
    if (i > 0) {
      text += ", ";
    }
    text += row.entries.size() > 1 ? flag.name + " = " + flag.value
                                   : flag.value;
  }
  return text + ".";
}

void OnRemoveRow(GtkWidget*, gpointer data) {
  auto* row_data = static_cast<RowData*>(data);
  Editor* editor = row_data->editor;
  PresetRow row;
  if (!FindRow(editor, row_data->name, &row)) {
    return;
  }
  RemoveRow(Shown(editor), row);
  Changed(editor);
  // The row being clicked is rebuilt, so not from inside its own signal.
  RefreshSoon(editor);
}

GtkWidget* RemovedFlagRow(Editor* editor, const Preset& preset,
                          const PresetRow& row) {
  const std::string& name = preset.flags[row.entries[0]].name;
  GtkWidget* widget = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widget),
                                FlagMarkup(name).c_str());
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(widget),
      "Roblox no longer has this flag, so it does nothing.");
  GtkWidget* remove = gtk_button_new_with_label("Remove");
  SetAccessibleLabel(remove, "Remove " + name);
  gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
  g_signal_connect(remove, "clicked", G_CALLBACK(OnRemoveRow),
                   NewRowData(editor, name, nullptr));
  adw_action_row_add_suffix(ADW_ACTION_ROW(widget), remove);
  return widget;
}

void AddLevelRows(Editor* editor, GtkWidget* expander, const PresetRow& row,
                  RowData* row_data) {
  const CatalogFlag& flag = *row.flag;
  GtkCheckButton* first = nullptr;
  for (std::size_t level = 0; level < flag.levels.size(); ++level) {
    const FlagLevel& choice = flag.levels[level];
    GtkWidget* option = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(option),
                                  Escaped(choice.label).c_str());
    adw_action_row_set_subtitle(ADW_ACTION_ROW(option),
                                Escaped(choice.description).c_str());
    GtkWidget* check = gtk_check_button_new();
    if (first == nullptr) {
      first = GTK_CHECK_BUTTON(check);
    } else {
      gtk_check_button_set_group(GTK_CHECK_BUTTON(check), first);
    }
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), row.level == level);
    SetAccessibleLabel(check, std::string(choice.label));
    adw_action_row_add_prefix(ADW_ACTION_ROW(option), check);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(option), check);
    GtkWidget* value = gtk_label_new(std::string(choice.values[0]).c_str());
    gtk_widget_add_css_class(value, "dim-label");
    gtk_widget_add_css_class(value, "monospace");
    adw_action_row_add_suffix(ADW_ACTION_ROW(option), value);
    RowData* level_data =
        NewRowData(editor, row_data->name, row.flag, level);
    level_data->value = row_data->value;
    g_signal_connect(check, "toggled", G_CALLBACK(OnLevelToggled), level_data);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), option);
  }
}

GtkWidget* PresetFlagRow(Editor* editor, const Preset& preset,
                         const PresetRow& row) {
  if (row.removed) {
    return RemovedFlagRow(editor, preset, row);
  }
  const PresetFlag& main = preset.flags[row.entries[0]];
  GtkWidget* expander = adw_expander_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander),
                                FlagMarkup(main.name).c_str());
  adw_expander_row_set_subtitle(
      ADW_EXPANDER_ROW(expander),
      row.flag != nullptr ? Escaped(row.flag->summary).c_str()
                          : "Not in Nightcap's list yet.");
  RowData* row_data = NewRowData(editor, main.name, row.flag);

  // Expander suffixes are shown in the reverse of the order added.
  GtkWidget* toggle = gtk_switch_new();
  gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
  gtk_switch_set_active(GTK_SWITCH(toggle), row.on);
  gtk_widget_set_tooltip_text(
      toggle, "Off keeps the flag here without sending it to Roblox");
  SetAccessibleLabel(toggle, "Send " + main.name + " to Roblox");
  g_signal_connect(toggle, "notify::active", G_CALLBACK(OnRowSwitch),
                   row_data);
  adw_expander_row_add_suffix(ADW_EXPANDER_ROW(expander), toggle);
  const std::string value = ValueText(preset, row);
  if (!value.empty()) {
    row_data->value = gtk_label_new(value.c_str());
    gtk_widget_add_css_class(row_data->value, "dim-label");
    adw_expander_row_add_suffix(ADW_EXPANDER_ROW(expander), row_data->value);
  }

  if (row.flag != nullptr) {
    adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), EffectsRow(*row.flag));
  }
  adw_expander_row_add_row(
      ADW_EXPANDER_ROW(expander),
      DetailRow(row.flag != nullptr
                    ? DetailText(*row.flag)
                    : "Value: " + main.value +
                          ". Change it with Edit as Text."));
  if (row.flag != nullptr && !row.flag->levels.empty() &&
      row.level == kNoLevel) {
    adw_expander_row_add_row(ADW_EXPANDER_ROW(expander),
                             DetailRow(CurrentValues(preset, row)));
  }
  if (row.flag != nullptr && row.flag->levels.size() == 1 &&
      row.level == kNoLevel) {
    GtkWidget* recommended = adw_button_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(recommended),
                                  "Use the Recommended Value");
    g_signal_connect(recommended, "activated", G_CALLBACK(OnUseRecommended),
                     row_data);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), recommended);
  }
  if (row.flag != nullptr && row.flag->levels.size() > 1) {
    AddLevelRows(editor, expander, row, row_data);
  } else if (row.flag != nullptr && row.flag->levels.empty()) {
    const int current = RowNumber(preset, row, row.flag->default_number);
    // A hand-set number outside the range is shown as it is, not clamped.
    GtkWidget* number = NewNumberRow(std::min(row.flag->minimum, current),
                                     std::max(row.flag->maximum, current),
                                     row.flag->step);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(number), "Value");
    GtkSpinButton* spin = SpinOf(number);
    gtk_spin_button_set_value(spin, current);
    g_signal_connect(spin, "notify::value", G_CALLBACK(OnNumberChanged),
                     row_data);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), number);
  }
  GtkWidget* remove = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(remove),
                                "Remove from Preset");
  gtk_widget_add_css_class(remove, "destructive-action");
  g_signal_connect(remove, "activated", G_CALLBACK(OnRemoveRow), row_data);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), remove);
  return expander;
}

void OnAdd(GtkButton* button, gpointer data) {
  auto* row_data = static_cast<RowData*>(data);
  Preset* target = Target(row_data->editor);
  if (target == nullptr) {
    return;
  }
  AddCatalogFlag(target, *row_data->flag);
  gtk_button_set_icon_name(button, "object-select-symbolic");
  gtk_widget_set_tooltip_text(GTK_WIDGET(button), "Added");
  SetAccessibleLabel(GTK_WIDGET(button), row_data->name + " added");
  gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
  Changed(row_data->editor);
}

GtkWidget* CatalogFlagRow(Editor* editor, const CatalogFlag& flag,
                          const Preset* target) {
  GtkWidget* expander = adw_expander_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander),
                                FlagMarkup(flag.names[0]).c_str());
  adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(expander),
                                Escaped(flag.summary).c_str());
  const bool added = target != nullptr && HasCatalogFlag(*target, flag);
  GtkWidget* add = gtk_button_new_from_icon_name(
      added ? "object-select-symbolic" : "list-add-symbolic");
  gtk_widget_add_css_class(add, "flat");
  gtk_widget_add_css_class(add, "circular");
  gtk_widget_set_tooltip_text(add, added ? "Added" : "Add to the preset");
  const std::string name(flag.names[0]);
  SetAccessibleLabel(add, added ? name + " added" : "Add " + name);
  gtk_widget_set_valign(add, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive(add, target != nullptr && !added);
  g_signal_connect(add, "clicked", G_CALLBACK(OnAdd),
                   NewRowData(editor, flag.names[0], &flag));
  adw_expander_row_add_suffix(ADW_EXPANDER_ROW(expander), add);

  adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), EffectsRow(flag));
  adw_expander_row_add_row(ADW_EXPANDER_ROW(expander),
                           DetailRow(DetailText(flag)));
  if (flag.levels.size() > 1) {
    for (const FlagLevel& level : flag.levels) {
      GtkWidget* option = adw_action_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(option),
                                    Escaped(level.label).c_str());
      adw_action_row_set_subtitle(ADW_ACTION_ROW(option),
                                  Escaped(level.description).c_str());
      GtkWidget* value = gtk_label_new(std::string(level.values[0]).c_str());
      gtk_widget_add_css_class(value, "dim-label");
      gtk_widget_add_css_class(value, "monospace");
      adw_action_row_add_suffix(ADW_ACTION_ROW(option), value);
      adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), option);
    }
  } else if (flag.levels.empty()) {
    adw_expander_row_add_row(
        ADW_EXPANDER_ROW(expander),
        DetailRow("Takes a number from " + std::to_string(flag.minimum) +
                  " to " + std::to_string(flag.maximum) + "."));
  }
  return expander;
}

void SelectGroupRow(Editor* editor, std::size_t index) {
  gtk_list_box_select_row(
      editor->group_list,
      gtk_list_box_get_row_at_index(editor->group_list,
                                    static_cast<int>(index)));
}

void OnAddFlags(GtkWidget*, gpointer data) {
  SelectGroupRow(static_cast<Editor*>(data), 0);
}

// A centered page for when there is nothing to list.
GtkWidget* EmptyPage(const char* icon, const char* title,
                     const char* description) {
  GtkWidget* status = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(status),
                                ThemedIconName(icon).c_str());
  adw_status_page_set_title(ADW_STATUS_PAGE(status), title);
  adw_status_page_set_description(ADW_STATUS_PAGE(status), description);
  return status;
}

GtkWidget* PresetPage(Editor* editor) {
  const Preset* preset = Shown(editor);
  const std::vector<PresetRow> rows = PresetRows(*preset);
  if (rows.empty()) {
    GtkWidget* status = EmptyPage("flag-symbolic", "No Flags Yet",
                                  "Roblox uses its own settings.");
    GtkWidget* add = gtk_button_new_with_label("Add Flags");
    gtk_widget_add_css_class(add, "pill");
    gtk_widget_add_css_class(add, "suggested-action");
    gtk_widget_set_halign(add, GTK_ALIGN_CENTER);
    g_signal_connect(add, "clicked", G_CALLBACK(OnAddFlags), editor);
    adw_status_page_set_child(ADW_STATUS_PAGE(status), add);
    return status;
  }
  GtkWidget* page = adw_preferences_page_new();
  GtkWidget* group = adw_preferences_group_new();
  const bool active = preset->name == editor->presets.active;
  adw_preferences_group_set_description(
      ADW_PREFERENCES_GROUP(group),
      active ? "These flags go to Roblox the next time it starts. Turn one "
               "off to play without it. It stays in the list."
             : "Not in use. Use This Preset sends these flags to Roblox "
               "instead.");
  for (const PresetRow& row : rows) {
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              PresetFlagRow(editor, *preset, row));
  }
  GtkWidget* add = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add), "Add Flags");
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(add), "list-add-symbolic");
  g_signal_connect(add, "activated", G_CALLBACK(OnAddFlags), editor);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                           ADW_PREFERENCES_GROUP(group));
  return page;
}

GtkWidget* FlagsPage(Editor* editor,
                     const std::vector<const CatalogFlag*>& flags) {
  if (flags.empty()) {
    return EmptyPage("edit-find-symbolic", "No Results",
                     "Try a different word, like fps or memory.");
  }
  const Preset* target = Target(editor);
  GtkWidget* page = adw_preferences_page_new();
  GtkWidget* group = adw_preferences_group_new();
  for (const CatalogFlag* flag : flags) {
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              CatalogFlagRow(editor, *flag, target));
  }
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                           ADW_PREFERENCES_GROUP(group));
  return page;
}

void SetAction(Editor* editor, const char* name, bool enabled) {
  g_simple_action_set_enabled(
      G_SIMPLE_ACTION(
          g_action_map_lookup_action(G_ACTION_MAP(editor->actions), name)),
      enabled);
}

void FillContent(Editor* editor) {
  editor->filling = true;
  // Handlers of the widgets being replaced may still point here.
  std::vector<std::unique_ptr<RowData>> old = std::move(editor->rows);
  editor->rows.clear();
  GtkWidget* page = nullptr;
  const Preset* target = Target(editor);
  const std::string adding =
      target != nullptr ? "Adding to " + target->name : "";
  const Preset* preset = Shown(editor);
  const bool on_preset = editor->view == View::kPreset && preset != nullptr;
  if (on_preset) {
    const bool active = preset->name == editor->presets.active;
    const std::size_t count = PresetRows(*preset).size();
    const std::string state = active ? "In use" : "Not in use";
    adw_window_title_set_title(editor->title, preset->name.c_str());
    adw_window_title_set_subtitle(
        editor->title,
        (state + " · " + std::to_string(count) +
         (count == 1 ? " flag" : " flags"))
            .c_str());
    page = PresetPage(editor);
  } else if (editor->view == View::kGroup) {
    adw_window_title_set_title(editor->title, editor->group.c_str());
    adw_window_title_set_subtitle(editor->title, adding.c_str());
    page = FlagsPage(editor, FindFlags(editor->group, ""));
  } else {
    adw_window_title_set_title(editor->title, "Search");
    adw_window_title_set_subtitle(editor->title, adding.c_str());
    page = FlagsPage(editor, FindFlags("", gtk_editable_get_text(
                                               GTK_EDITABLE(editor->search))));
  }
  gtk_widget_set_visible(editor->use,
                         on_preset && preset->name != editor->presets.active);
  gtk_widget_set_visible(editor->menu, on_preset);
  SetAction(editor, "rename", on_preset);
  SetAction(editor, "duplicate", on_preset);
  SetAction(editor, "delete",
            on_preset && preset->name != editor->presets.active);
  adw_toolbar_view_set_content(editor->content, page);
  editor->filling = false;
}

GtkWidget* SidebarRow(const char* icon_name, const std::string& text,
                      const std::string& note, bool dim_icon) {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget* icon = gtk_image_new_from_icon_name(icon_name);
  if (icon_name == nullptr) {
    gtk_widget_set_size_request(icon, 16, 16);
  } else if (dim_icon) {
    gtk_widget_add_css_class(icon, "dim-label");
  }
  gtk_box_append(GTK_BOX(box), icon);
  GtkWidget* label = gtk_label_new(text.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(box), label);
  if (!note.empty()) {
    GtkWidget* count = gtk_label_new(note.c_str());
    gtk_widget_add_css_class(count, "dim-label");
    gtk_box_append(GTK_BOX(box), count);
  }
  GtkWidget* row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  std::string spoken = text;
  if (note == "In use") {
    spoken += ", in use";
  } else if (!note.empty()) {
    spoken += ", " + note + (note == "1" ? " flag" : " flags");
  }
  SetAccessibleLabel(row, spoken);
  return row;
}

void ShowContentPane(Editor* editor) {
  adw_navigation_split_view_set_show_content(editor->split, TRUE);
}

void Refresh(Editor* editor) {
  editor->filling = true;
  gtk_list_box_remove_all(editor->preset_list);
  GtkListBoxRow* selected = nullptr;
  for (const Preset& preset : editor->presets.presets) {
    const bool active = preset.name == editor->presets.active;
    GtkWidget* row = SidebarRow(
        active ? "object-select-symbolic" : nullptr, preset.name,
        active ? "In use" : std::to_string(PresetRows(preset).size()), false);
    g_object_set_data_full(G_OBJECT(row), "preset",
                           g_strdup(preset.name.c_str()), g_free);
    gtk_list_box_append(editor->preset_list, row);
    if (editor->view == View::kPreset && preset.name == editor->shown) {
      selected = GTK_LIST_BOX_ROW(row);
    }
  }
  GtkWidget* create =
      SidebarRow("list-add-symbolic", "New Preset", "", true);
  g_object_set_data(G_OBJECT(create), "new", GINT_TO_POINTER(1));
  // An action, not a page: moving focus onto it must not open the prompt.
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(create), FALSE);
  gtk_list_box_append(editor->preset_list, create);
  gtk_list_box_select_row(editor->preset_list, selected);
  if (editor->view != View::kGroup) {
    gtk_list_box_unselect_all(editor->group_list);
  }
  editor->filling = false;
  FillContent(editor);
}

// Asks for a preset's name, for a new preset or a rename.
struct NamePrompt {
  Editor* editor = nullptr;
  GtkEditable* entry = nullptr;
  // Empty for a new preset.
  std::string old_name;
};

std::string PromptName(const NamePrompt& prompt) {
  std::string name = gtk_editable_get_text(prompt.entry);
  name.erase(0, name.find_first_not_of(" \t"));
  name.erase(name.find_last_not_of(" \t") + 1);
  return name;
}

// Why the typed name can't be used, or empty.
std::string NameProblem(const NamePrompt& prompt) {
  const std::string name = PromptName(prompt);
  if (name.empty()) {
    return "A preset needs a name.";
  }
  if (name != prompt.old_name && prompt.editor->presets.Find(name) != nullptr) {
    return "There is already a preset called " + name + ".";
  }
  return {};
}

// The prompt stays open on a bad name, so the typing is kept.
void OnNameTyped(GtkEditable*, gpointer data) {
  auto* prompt = static_cast<NamePrompt*>(data);
  const std::string problem = NameProblem(*prompt);
  AdwAlertDialog* question = ADW_ALERT_DIALOG(gtk_widget_get_ancestor(
      GTK_WIDGET(prompt->entry), ADW_TYPE_ALERT_DIALOG));
  adw_alert_dialog_set_response_enabled(question, "ok", problem.empty());
  adw_alert_dialog_set_body(question, problem.c_str());
  if (problem.empty()) {
    gtk_widget_remove_css_class(GTK_WIDGET(prompt->entry), "error");
  } else {
    gtk_widget_add_css_class(GTK_WIDGET(prompt->entry), "error");
  }
}

void OnNameResponse(AdwAlertDialog*, const char* response, gpointer data) {
  auto* prompt = static_cast<NamePrompt*>(data);
  Editor* editor = prompt->editor;
  if (g_strcmp0(response, "ok") != 0 || !NameProblem(*prompt).empty()) {
    return;
  }
  const std::string name = PromptName(*prompt);
  if (prompt->old_name.empty()) {
    Preset preset;
    preset.name = name;
    editor->presets.presets.push_back(std::move(preset));
  } else if (Preset* preset = editor->presets.Find(prompt->old_name)) {
    preset->name = name;
    if (editor->presets.active == prompt->old_name) {
      editor->presets.active = name;
    }
  }
  editor->shown = name;
  editor->view = View::kPreset;
  Changed(editor);
  Refresh(editor);
}

void AskName(Editor* editor, const char* heading, const std::string& initial,
             const char* confirm, std::string old_name) {
  AdwDialog* question = adw_alert_dialog_new(heading, nullptr);
  GtkWidget* entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(entry), initial.c_str());
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(question), entry);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(question), "cancel",
                                 "Cancel", "ok", confirm, nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(question), "ok",
                                           ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(question), "ok");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(question), "cancel");
  auto* prompt = new NamePrompt{editor, GTK_EDITABLE(entry),
                                std::move(old_name)};
  g_object_set_data_full(G_OBJECT(question), "prompt", prompt,
                         [](gpointer data) {
                           delete static_cast<NamePrompt*>(data);
                         });
  g_signal_connect(question, "response", G_CALLBACK(OnNameResponse), prompt);
  g_signal_connect(entry, "changed", G_CALLBACK(OnNameTyped), prompt);
  adw_dialog_present(question, GTK_WIDGET(editor->dialog));
  gtk_widget_grab_focus(entry);
}

void OnPresetSelected(GtkListBox*, GtkListBoxRow* row, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  if (editor->filling || row == nullptr ||
      g_object_get_data(G_OBJECT(row), "new") != nullptr) {
    return;
  }
  editor->view = View::kPreset;
  editor->shown =
      static_cast<const char*>(g_object_get_data(G_OBJECT(row), "preset"));
  editor->filling = true;
  gtk_list_box_unselect_all(editor->group_list);
  gtk_editable_set_text(GTK_EDITABLE(editor->search), "");
  editor->filling = false;
  FillContent(editor);
  ShowContentPane(editor);
}

// A selected row is not selected again, so on a narrow dialog activation is
// what goes back to the content.
void OnPresetActivated(GtkListBox*, GtkListBoxRow* row, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  if (g_object_get_data(G_OBJECT(row), "new") != nullptr) {
    AskName(editor, "New Preset",
            UniquePresetName(editor->presets, "New preset"), "Create", "");
    return;
  }
  ShowContentPane(editor);
}

void OnGroupActivated(GtkListBox*, GtkListBoxRow*, gpointer data) {
  ShowContentPane(static_cast<Editor*>(data));
}

void OnGroupSelected(GtkListBox*, GtkListBoxRow* row, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  if (editor->filling || row == nullptr) {
    return;
  }
  editor->view = View::kGroup;
  editor->group =
      static_cast<const char*>(g_object_get_data(G_OBJECT(row), "group"));
  editor->filling = true;
  gtk_list_box_unselect_all(editor->preset_list);
  gtk_editable_set_text(GTK_EDITABLE(editor->search), "");
  editor->filling = false;
  FillContent(editor);
  ShowContentPane(editor);
}

void OnSearchChanged(GtkSearchEntry* entry, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  if (editor->filling) {
    return;
  }
  if (gtk_editable_get_text(GTK_EDITABLE(entry))[0] == '\0') {
    if (editor->view == View::kSearch) {
      editor->view = View::kPreset;
      Refresh(editor);
    }
    return;
  }
  editor->view = View::kSearch;
  editor->filling = true;
  gtk_list_box_unselect_all(editor->preset_list);
  gtk_list_box_unselect_all(editor->group_list);
  editor->filling = false;
  FillContent(editor);
  ShowContentPane(editor);
}

void OnUse(GtkButton*, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  editor->presets.active = editor->shown;
  Changed(editor);
  Refresh(editor);
}

void OnRename(GSimpleAction*, GVariant*, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  AskName(editor, "Rename Preset", editor->shown, "Rename", editor->shown);
}

void OnDuplicate(GSimpleAction*, GVariant*, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  const Preset* shown = Shown(editor);
  if (shown == nullptr) {
    return;
  }
  Preset copy = *shown;
  copy.name = UniquePresetName(editor->presets, shown->name + " copy");
  editor->shown = copy.name;
  editor->presets.presets.push_back(std::move(copy));
  Changed(editor);
  Refresh(editor);
}

void OnDeleteResponse(AdwAlertDialog*, const char* response, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  if (g_strcmp0(response, "delete") != 0) {
    return;
  }
  std::vector<Preset>& presets = editor->presets.presets;
  for (auto it = presets.begin(); it != presets.end(); ++it) {
    if (it->name == editor->shown) {
      presets.erase(it);
      break;
    }
  }
  editor->shown = editor->presets.active;
  Changed(editor);
  Refresh(editor);
}

void OnDelete(GSimpleAction*, GVariant*, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  const std::string heading = "Delete " + editor->shown + "?";
  AdwDialog* question = adw_alert_dialog_new(
      heading.c_str(), "Its flags are deleted with it.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(question), "cancel",
                                 "Cancel", "delete", "Delete", nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(question),
                                           "delete",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(question), "cancel");
  g_signal_connect(question, "response", G_CALLBACK(OnDeleteResponse),
                   editor);
  adw_dialog_present(question, GTK_WIDGET(editor->dialog));
}

// fflags.json was saved as text: take it in, and keep the presets with it.
void OnTextSaved(Editor* editor) {
  std::vector<PresetFlag> flags;
  std::string error;
  FFlagPresets presets;
  if (!ParseFFlags(editor->host.fflags->Text(), &flags, &error) ||
      !LoadPresets(PresetsText(editor->presets), flags, &presets, &error)) {
    ShowProblem(editor, error);
    return;
  }
  editor->presets = std::move(presets);
  if (Shown(editor) == nullptr) {
    editor->shown = editor->presets.active;
  }
  if (SavePresetsFile(editor, &error)) {
    SetDirty(editor, false);
  } else {
    ShowProblem(editor, "Your presets are not saved: " + error);
  }
  Refresh(editor);
}

void OnEditAsText(GtkButton*, gpointer data) {
  auto* editor = static_cast<Editor*>(data);
  const Preset* active = editor->presets.Find(editor->presets.active);
  // The file itself while it holds these flags, so its comments and layout
  // stay and Save is not ready for nothing.
  std::string text = editor->host.fflags->Text();
  std::vector<PresetFlag> disk;
  std::string error;
  if (active != nullptr && (!ParseFFlags(text, &disk, &error) ||
                            !HoldsFlags(*active, disk))) {
    text = FFlagsText(*active);
  }
  std::weak_ptr<bool> alive = editor->alive;
  editor->host.edit_as_text(
      text,
      GTK_WIDGET(editor->dialog), [editor, alive] {
        if (!alive.expired()) {
          OnTextSaved(editor);
        }
      });
}

}  // namespace

namespace {

GtkWidget* SidebarHeading(const char* text) {
  GtkWidget* label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_add_css_class(label, "heading");
  gtk_widget_add_css_class(label, "dim-label");
  gtk_widget_set_margin_start(label, 18);
  gtk_widget_set_margin_top(label, 12);
  gtk_widget_set_margin_bottom(label, 4);
  return label;
}

GtkListBox* SidebarList(Editor* editor, const char* label,
                        GCallback selected, GCallback activated) {
  GtkWidget* list = gtk_list_box_new();
  SetAccessibleLabel(list, label);
  gtk_widget_add_css_class(list, "navigation-sidebar");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
  g_signal_connect(list, "row-selected", selected, editor);
  g_signal_connect(list, "row-activated", activated, editor);
  return GTK_LIST_BOX(list);
}

GtkWidget* BuildSidebar(Editor* editor) {
  GtkWidget* cancel = gtk_button_new_with_label("Cancel");
  g_signal_connect(cancel, "clicked", G_CALLBACK(OnCancel), editor);
  GtkWidget* header = adw_header_bar_new();
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
  adw_header_bar_set_show_title(ADW_HEADER_BAR(header), FALSE);

  GtkWidget* search = gtk_search_entry_new();
  editor->search = GTK_SEARCH_ENTRY(search);
  gtk_search_entry_set_placeholder_text(editor->search,
                                        "Search flags");
  SetAccessibleLabel(search, "Search flags");
  gtk_widget_set_margin_start(search, 12);
  gtk_widget_set_margin_end(search, 12);
  gtk_widget_set_margin_bottom(search, 6);
  g_signal_connect(search, "search-changed", G_CALLBACK(OnSearchChanged),
                   editor);

  editor->preset_list =
      SidebarList(editor, "Presets", G_CALLBACK(OnPresetSelected),
                  G_CALLBACK(OnPresetActivated));
  editor->group_list =
      SidebarList(editor, "Add Flags", G_CALLBACK(OnGroupSelected),
                  G_CALLBACK(OnGroupActivated));
  for (const FlagGroup& group : FlagGroups()) {
    const std::string count =
        std::to_string(FindFlags(group.name, "").size());
    GtkWidget* row = SidebarRow(
        ThemedIconName(std::string(group.icon_name)).c_str(),
                                std::string(group.name), count, false);
    g_object_set_data(G_OBJECT(row), "group",
                      const_cast<char*>(group.name.data()));
    gtk_list_box_append(editor->group_list, row);
  }

  GtkWidget* lists = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(lists), SidebarHeading("Presets"));
  gtk_box_append(GTK_BOX(lists), GTK_WIDGET(editor->preset_list));
  gtk_box_append(GTK_BOX(lists), SidebarHeading("Add Flags"));
  gtk_box_append(GTK_BOX(lists), GTK_WIDGET(editor->group_list));
  GtkWidget* scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), lists);
  gtk_widget_set_vexpand(scrolled, TRUE);

  GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(body), search);
  gtk_box_append(GTK_BOX(body), scrolled);

  GtkWidget* text = gtk_button_new_with_label("Edit as Text");
  gtk_widget_add_css_class(text, "flat");
  gtk_widget_set_tooltip_text(text, "Edit fflags.json by hand");
  gtk_widget_set_margin_start(text, 12);
  gtk_widget_set_margin_end(text, 12);
  gtk_widget_set_margin_top(text, 6);
  gtk_widget_set_margin_bottom(text, 6);
  g_signal_connect(text, "clicked", G_CALLBACK(OnEditAsText), editor);

  GtkWidget* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), body);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), text);
  return toolbar;
}

GtkWidget* BuildContent(Editor* editor) {
  editor->title = ADW_WINDOW_TITLE(adw_window_title_new("", ""));
  editor->save = gtk_button_new_with_label("Save");
  gtk_widget_add_css_class(editor->save, "suggested-action");
  gtk_widget_set_tooltip_text(editor->save, "Save (Ctrl+S)");
  gtk_widget_set_sensitive(editor->save, FALSE);
  g_signal_connect(editor->save, "clicked", G_CALLBACK(OnSave), editor);

  editor->use = gtk_button_new_with_label("Use This Preset");
  g_signal_connect(editor->use, "clicked", G_CALLBACK(OnUse), editor);

  GMenu* menu = g_menu_new();
  g_menu_append(menu, "Rename…", "editor.rename");
  g_menu_append(menu, "Duplicate", "editor.duplicate");
  g_menu_append(menu, "Delete…", "editor.delete");
  editor->menu = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(editor->menu),
                                "view-more-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(editor->menu),
                                 G_MENU_MODEL(menu));
  gtk_widget_set_tooltip_text(editor->menu, "Preset");
  SetAccessibleLabel(editor->menu, "Preset options");
  g_object_unref(menu);

  GtkWidget* header = adw_header_bar_new();
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                  GTK_WIDGET(editor->title));
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), editor->save);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), editor->menu);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), editor->use);

  GtkWidget* problem = adw_banner_new("");
  editor->problem = ADW_BANNER(problem);
  adw_banner_set_use_markup(editor->problem, FALSE);

  GtkWidget* toolbar = adw_toolbar_view_new();
  editor->content = ADW_TOOLBAR_VIEW(toolbar);
  adw_toolbar_view_add_top_bar(editor->content, header);
  adw_toolbar_view_add_top_bar(editor->content, problem);
  return toolbar;
}

}  // namespace

void PresentFFlagEditor(const FFlagEditorHost& host) {
  const runtime::RuntimeConfigText presets_text =
      runtime::ReadOptionalTextFile(host.presets_file);
  if (!presets_text) {
    host.show_error(presets_text.error);
    return;
  }
  std::vector<PresetFlag> flags;
  std::string error;
  if (!ParseFFlags(host.fflags->Text(), &flags, &error)) {
    // Only the text editor can fix a file that is not flags.
    host.show_error(error + ". Fix it as text.");
    host.edit_as_text(host.fflags->Text(), host.parent, nullptr);
    return;
  }
  auto* editor = new Editor;
  if (!LoadPresets(presets_text.text, flags, &editor->presets, &error)) {
    delete editor;
    host.show_error(error + ". Fix or remove " + host.presets_file.string());
    return;
  }
  editor->host = host;
  editor->presets_disk = presets_text.text;
  editor->shown = editor->presets.active;

  editor->dialog = adw_dialog_new();
  adw_dialog_set_title(editor->dialog, "FFlags");
  adw_dialog_set_content_width(editor->dialog, 960);
  adw_dialog_set_content_height(editor->dialog, 680);
  gtk_widget_set_size_request(GTK_WIDGET(editor->dialog), 360, 360);
  g_object_set_data_full(G_OBJECT(editor->dialog), "editor", editor,
                         [](gpointer data) {
                           delete static_cast<Editor*>(data);
                         });
  g_signal_connect(editor->dialog, "close-attempt",
                   G_CALLBACK(OnCloseAttempt), editor);

  const GActionEntry entries[] = {
      {"rename", OnRename, nullptr, nullptr, nullptr, {}},
      {"duplicate", OnDuplicate, nullptr, nullptr, nullptr, {}},
      {"delete", OnDelete, nullptr, nullptr, nullptr, {}},
  };
  editor->actions = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(editor->actions), entries,
                                  G_N_ELEMENTS(entries), editor);
  gtk_widget_insert_action_group(GTK_WIDGET(editor->dialog), "editor",
                                 G_ACTION_GROUP(editor->actions));
  g_object_unref(editor->actions);

  GtkEventController* shortcuts = gtk_shortcut_controller_new();
  gtk_shortcut_controller_add_shortcut(
      GTK_SHORTCUT_CONTROLLER(shortcuts),
      gtk_shortcut_new(
          gtk_keyval_trigger_new(GDK_KEY_s, GDK_CONTROL_MASK),
          gtk_callback_action_new(OnSaveShortcut, editor, nullptr)));
  gtk_widget_add_controller(GTK_WIDGET(editor->dialog), shortcuts);

  AdwNavigationPage* sidebar =
      adw_navigation_page_new(BuildSidebar(editor), "FFlags");
  AdwNavigationPage* content =
      adw_navigation_page_new(BuildContent(editor), "Flags");
  GtkWidget* split = adw_navigation_split_view_new();
  editor->split = ADW_NAVIGATION_SPLIT_VIEW(split);
  adw_navigation_split_view_set_sidebar(editor->split, sidebar);
  adw_navigation_split_view_set_content(editor->split, content);
  adw_navigation_split_view_set_min_sidebar_width(editor->split, 220);
  adw_navigation_split_view_set_max_sidebar_width(editor->split, 260);
  adw_dialog_set_child(editor->dialog, split);

  AdwBreakpoint* narrow =
      adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 640sp"));
  GValue collapsed = G_VALUE_INIT;
  g_value_init(&collapsed, G_TYPE_BOOLEAN);
  g_value_set_boolean(&collapsed, TRUE);
  adw_breakpoint_add_setter(narrow, G_OBJECT(split), "collapsed", &collapsed);
  g_value_unset(&collapsed);
  adw_dialog_add_breakpoint(editor->dialog, narrow);

  Refresh(editor);
  adw_dialog_present(editor->dialog, host.parent);
}

}  // namespace ui
}  // namespace mocktail
