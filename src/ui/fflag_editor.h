#ifndef MOCKTAIL_UI_FFLAG_EDITOR_H_
#define MOCKTAIL_UI_FFLAG_EDITOR_H_

#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <string>

#include "ui/settings_model.h"

namespace mocktail {
namespace ui {

struct FFlagEditorHost {
  GtkWidget* parent = nullptr;
  // Already loaded.
  FFlagsDocument* fflags = nullptr;
  std::filesystem::path presets_file;
  std::function<void(const std::string&)> show_error;
  // Runs after the editor saves fflags.json.
  std::function<void()> saved;
  // Opens the text editor on `text`. `done` runs after it saves fflags.json.
  std::function<void(const std::string& text, GtkWidget* parent,
                     std::function<void()> done)>
      edit_as_text;
};

void PresentFFlagEditor(const FFlagEditorHost& host);

}  // namespace ui
}  // namespace mocktail

#endif  // MOCKTAIL_UI_FFLAG_EDITOR_H_
