#ifndef MOCKTAIL_RUNTIME_APPIMAGE_MENU_ENTRY_H_
#define MOCKTAIL_RUNTIME_APPIMAGE_MENU_ENTRY_H_

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {

inline constexpr std::string_view kAppImageMenuEntryName =
    "io.github.CoderDayton.nightcap.appimage.desktop";

struct AppImageMenuPaths {
  std::filesystem::path applications_dir;
  std::filesystem::path icon_file;
  std::filesystem::path declined_marker;
  std::vector<std::filesystem::path> installed_entry_dirs;
};

AppImageMenuPaths AppImageMenuPathsFor(const Environment& environment,
                                       const RuntimePaths& paths);

std::filesystem::path RunningAppImage(const Environment& environment);

std::filesystem::path AppImageIcon(const Environment& environment);

std::string AppImageDesktopEntry(const std::filesystem::path& app_image,
                                 std::string_view icon);

enum class AppImageMenuAction {
  kNone,
  kAdded,
  kMoved,
  kRemoved,
  kDeclined,
  kFailed,
};

struct AppImageMenuOutcome {
  AppImageMenuAction action = AppImageMenuAction::kNone;
  std::string detail;
};

using AskToAddMenuEntry = std::function<std::optional<bool>()>;

AppImageMenuOutcome UpdateAppImageMenuEntry(
    const AppImageMenuPaths& paths, const std::filesystem::path& app_image,
    const std::filesystem::path& app_image_icon, const AskToAddMenuEntry& ask);

}
}

#endif
