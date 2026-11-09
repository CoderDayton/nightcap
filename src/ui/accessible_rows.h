#ifndef MOCKTAIL_UI_ACCESSIBLE_ROWS_H_
#define MOCKTAIL_UI_ACCESSIBLE_ROWS_H_

#include <adwaita.h>

#include <string>

namespace mocktail {
namespace ui {

inline void SetAccessibleLabel(GtkWidget* widget, const std::string& label) {
  gtk_accessible_update_property(GTK_ACCESSIBLE(widget),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(),
                                 -1);
}

// AdwSpinRow is missing from the accessibility tree (libadwaita 1.10), so
// number rows are action rows with a spin button.
inline GtkWidget* NewNumberRow(double minimum, double maximum, double step) {
  GtkWidget* row = adw_action_row_new();
  gtk_widget_add_css_class(row, "spin");
  GtkWidget* spin = gtk_spin_button_new_with_range(minimum, maximum, step);
  gtk_widget_set_valign(spin, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), spin);
  adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), spin);
  g_object_set_data(G_OBJECT(row), "spin", spin);
  return row;
}

// The spin button of a NewNumberRow row, or null for any other widget.
inline GtkSpinButton* SpinOf(GtkWidget* row) {
  return GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(row), "spin"));
}

}  // namespace ui
}  // namespace mocktail

#endif  // MOCKTAIL_UI_ACCESSIBLE_ROWS_H_
