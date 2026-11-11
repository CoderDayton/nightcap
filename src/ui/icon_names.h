#ifndef MOCKTAIL_UI_ICON_NAMES_H_
#define MOCKTAIL_UI_ICON_NAMES_H_

#include <gtk/gtk.h>

#include <array>
#include <string>
#include <utility>

namespace mocktail {
namespace ui {

// Breeze lacks some of Adwaita's icon names, so fall back to ones it has.
inline std::string ThemedIconName(const std::string& name) {
  static const std::pair<const char*, std::array<const char*, 2>> kFallbacks[] = {
      {"power-profile-performance-symbolic",
       {"speedometer-symbolic", "utilities-system-monitor-symbolic"}},
      {"application-x-addon-symbolic",
       {"extension-symbolic", "applications-system-symbolic"}},
      {"preferences-system-privacy-symbolic",
       {"security-high-symbolic", "view-private-symbolic"}},
      {"flag-symbolic",
       {"applications-engineering-symbolic", "preferences-other-symbolic"}},
  };
  GtkIconTheme* theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
  if (gtk_icon_theme_has_icon(theme, name.c_str())) return name;
  for (const auto& [primary, alternates] : kFallbacks) {
    if (name != primary) continue;
    for (const char* alternate : alternates) {
      if (gtk_icon_theme_has_icon(theme, alternate)) return alternate;
    }
  }
  return name;
}

}  // namespace ui
}  // namespace mocktail

#endif
