#include <adwaita.h>

#include <cstdlib>
#include <deque>
#include <string>
#include <string_view>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"
#include "ui/settings_model.h"

namespace {

using mocktail::ui::ChoiceIndex;
using mocktail::ui::FrameRate;
using mocktail::ui::FrameRateMode;
using mocktail::ui::kNoChoice;
using mocktail::ui::Setting;
using mocktail::ui::SettingKind;
using mocktail::ui::SettingsDocument;
using mocktail::ui::SettingsGroup;
using mocktail::ui::SettingsPage;

constexpr char kApplicationId[] = "io.github.CoderDayton.nightcap";

struct SettingsWindow;

struct Row {
  SettingsWindow* window = nullptr;
  const Setting* setting = nullptr;
  GtkWidget* widget = nullptr;
  // The custom frames-per-second row. Only a kFrameRate row has one.
  GtkWidget* custom = nullptr;
  // The button that puts a text back to its default. Only a kText row has
  // one.
  GtkWidget* reset = nullptr;
  // A value the file holds that the row does not list. It is shown as one
  // more choice so opening the window never changes it.
  std::string unlisted;
};

struct SettingsWindow {
  explicit SettingsWindow(std::filesystem::path config_file)
      : document(std::move(config_file)) {}

  SettingsDocument document;
  std::deque<Row> rows;
  AdwNavigationSplitView* split = nullptr;
  AdwNavigationPage* content_page = nullptr;
  GtkStack* stack = nullptr;
  AdwToastOverlay* toasts = nullptr;
  // The card under the settings that says when changes take effect.
  GtkRevealer* notice = nullptr;
  // Set while rows are filled from the file, so that filling one is not
  // taken for a change by the user.
  bool refreshing = false;
};

std::string Text(std::string_view text) { return std::string(text); }

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

void RefreshRow(Row* row) {
  const Setting& setting = *row->setting;
  const std::string value = row->window->document.Value(setting.key);
  switch (setting.kind) {
    case SettingKind::kToggle:
      adw_switch_row_set_active(ADW_SWITCH_ROW(row->widget),
                                mocktail::ui::ToggleState(setting, value));
      break;
    case SettingKind::kChoice: {
      const std::size_t index = ChoiceIndex(setting, value);
      row->unlisted = index == kNoChoice ? value : std::string();
      SetChoices(row);
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
    case SettingKind::kText: {
      const bool is_set = row->window->document.IsSet(setting.key);
      const std::string text = is_set ? value : Text(setting.default_value);
      // Setting the same text again would mark the field as edited.
      if (text != gtk_editable_get_text(GTK_EDITABLE(row->widget))) {
        gtk_editable_set_text(GTK_EDITABLE(row->widget), text.c_str());
      }
      gtk_widget_set_sensitive(row->reset, is_set);
      break;
    }
    case SettingKind::kEditor:
      break;
  }
}

void RefreshRows(SettingsWindow* window) {
  window->refreshing = true;
  for (Row& row : window->rows) {
    RefreshRow(&row);
  }
  window->refreshing = false;
}

// Shows how a save went, then shows every row as the file now has it.
void ShowSaved(SettingsWindow* window, bool saved, const std::string& error) {
  if (saved) {
    gtk_revealer_set_reveal_child(window->notice, TRUE);
  } else {
    AdwToast* toast = adw_toast_new(error.c_str());
    adw_toast_set_use_markup(toast, FALSE);
    adw_toast_set_timeout(toast, 0);
    adw_toast_overlay_add_toast(window->toasts, toast);
  }
  // On failure the rows go back to what the file holds.
  RefreshRows(window);
}

void Apply(Row* row, const std::string& value) {
  SettingsWindow* window = row->window;
  if (window->refreshing) {
    return;
  }
  std::string error;
  const bool saved = window->document.Set(row->setting->key, value, &error);
  ShowSaved(window, saved, error);
}

void OnNumberChanged(GObject* object, GParamSpec*, gpointer user_data) {
  Apply(static_cast<Row*>(user_data),
        mocktail::ui::NumberValue(
            adw_spin_row_get_value(ADW_SPIN_ROW(object))));
}

void OnTextApplied(AdwEntryRow* entry, gpointer user_data) {
  Row* row = static_cast<Row*>(user_data);
  SettingsWindow* window = row->window;
  std::string error;
  const bool saved = window->document.SetText(
      row->setting->key, gtk_editable_get_text(GTK_EDITABLE(entry)), &error);
  ShowSaved(window, saved, error);
}

void OnTextReset(GtkButton*, gpointer user_data) {
  // An empty value unsets the key, which brings the default back.
  Apply(static_cast<Row*>(user_data), std::string());
}

void OnToggleChanged(GObject* object, GParamSpec*, gpointer user_data) {
  Apply(static_cast<Row*>(user_data),
        adw_switch_row_get_active(ADW_SWITCH_ROW(object)) ? "true" : "false");
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

void AddRow(SettingsWindow* window, AdwPreferencesGroup* group,
            const Setting& setting) {
  Row& row = window->rows.emplace_back();
  row.window = window;
  row.setting = &setting;
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
      row.widget = adw_spin_row_new_with_range(0, 1048576, 256);
      g_signal_connect(row.widget, "notify::value",
                       G_CALLBACK(OnNumberChanged), &row);
      break;
    case SettingKind::kText:
      row.widget = adw_entry_row_new();
      adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(row.widget), TRUE);
      row.reset = gtk_button_new_from_icon_name("edit-undo-symbolic");
      gtk_widget_set_tooltip_text(row.reset, "Reset to default");
      gtk_widget_set_valign(row.reset, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(row.reset, "flat");
      adw_entry_row_add_suffix(ADW_ENTRY_ROW(row.widget), row.reset);
      g_signal_connect(row.widget, "apply", G_CALLBACK(OnTextApplied), &row);
      g_signal_connect(row.reset, "clicked", G_CALLBACK(OnTextReset), &row);
      break;
    case SettingKind::kEditor:
      row.widget = adw_action_row_new();
      adw_action_row_add_suffix(
          ADW_ACTION_ROW(row.widget),
          gtk_image_new_from_icon_name("go-next-symbolic"));
      break;
  }
  // A setting with no working control yet shows the file's value, grayed
  // out.
  gtk_widget_set_sensitive(row.widget, mocktail::ui::IsAvailable(setting));
  SetRowText(row.widget, setting.title, setting.subtitle);
  adw_preferences_group_add(group, row.widget);
  if (row.custom != nullptr) {
    adw_preferences_group_add(group, row.custom);
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
  gtk_widget_set_margin_start(label, 12);
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

GtkWidget* BuildSidebar(SettingsWindow* window) {
  GtkWidget* list = gtk_list_box_new();
  gtk_widget_add_css_class(list, "navigation-sidebar");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_BROWSE);
  gtk_list_box_set_header_func(GTK_LIST_BOX(list), SetSidebarHeader, nullptr,
                               nullptr);
  for (const SettingsPage& page : mocktail::ui::SettingsPages()) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(box),
                   gtk_image_new_from_icon_name(Text(page.icon_name).c_str()));
    GtkWidget* label = gtk_label_new(Text(page.title).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_box_append(GTK_LIST_BOX(list), box);
  }
  g_signal_connect(list, "row-selected", G_CALLBACK(OnPageSelected), window);
  g_signal_connect(list, "row-activated", G_CALLBACK(OnPageActivated),
                   window);

  GtkWidget* scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);

  GtkWidget* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                               adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scrolled);
  gtk_list_box_select_row(
      GTK_LIST_BOX(list),
      gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
  return toolbar;
}

GtkWidget* BuildNotice(SettingsWindow* window) {
  GtkWidget* icon =
      gtk_image_new_from_icon_name("dialog-information-symbolic");
  GtkWidget* label = gtk_label_new(
      "Saved. Changes apply the next time Nightcap starts.");
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_margin_start(icon, 14);
  gtk_widget_set_margin_end(label, 14);
  gtk_widget_set_margin_top(label, 12);
  gtk_widget_set_margin_bottom(label, 12);

  // The stock "card" style gives the rounded shape the rows above have.
  GtkWidget* card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(card, "card");
  gtk_box_append(GTK_BOX(card), icon);
  gtk_box_append(GTK_BOX(card), label);
  gtk_widget_set_margin_start(card, 12);
  gtk_widget_set_margin_end(card, 12);
  gtk_widget_set_margin_bottom(card, 12);

  GtkWidget* revealer = gtk_revealer_new();
  window->notice = GTK_REVEALER(revealer);
  gtk_revealer_set_transition_type(window->notice,
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_child(window->notice, card);
  return revealer;
}

GtkWidget* BuildPages(SettingsWindow* window) {
  GtkWidget* stack = gtk_stack_new();
  window->stack = GTK_STACK(stack);
  for (const SettingsPage& page : mocktail::ui::SettingsPages()) {
    GtkWidget* page_widget = adw_preferences_page_new();
    for (const SettingsGroup& group : page.groups) {
      GtkWidget* group_widget = adw_preferences_group_new();
      adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_widget),
                                      Text(group.title).c_str());
      for (const Setting& setting : group.settings) {
        AddRow(window, ADW_PREFERENCES_GROUP(group_widget), setting);
      }
      adw_preferences_page_add(ADW_PREFERENCES_PAGE(page_widget),
                               ADW_PREFERENCES_GROUP(group_widget));
    }
    gtk_stack_add_named(GTK_STACK(stack), page_widget,
                        Text(page.name).c_str());
  }
  return stack;
}

// The sidebar of pages beside the settings of the chosen one.
GtkWidget* BuildSplitView(SettingsWindow* window,
                          AdwApplicationWindow* window_widget) {
  GtkWidget* split = adw_navigation_split_view_new();
  window->split = ADW_NAVIGATION_SPLIT_VIEW(split);
  adw_navigation_split_view_set_min_sidebar_width(window->split, 200);
  adw_navigation_split_view_set_max_sidebar_width(window->split, 240);

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

void OnActivate(GtkApplication* application, gpointer user_data) {
  auto* window = static_cast<SettingsWindow*>(user_data);
  if (GtkWindow* existing = gtk_application_get_active_window(application)) {
    gtk_window_present(existing);
    return;
  }

  GtkWidget* window_widget = adw_application_window_new(application);
  gtk_window_set_title(GTK_WINDOW(window_widget), "Nightcap Settings");
  gtk_window_set_default_size(GTK_WINDOW(window_widget), 860, 620);
  gtk_widget_set_size_request(window_widget, 360, 420);
  gtk_window_set_icon_name(GTK_WINDOW(window_widget), kApplicationId);

  GtkWidget* content = nullptr;
  std::string error;
  if (window->document.Load(&error)) {
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
  SettingsWindow window(paths.config_file());

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
