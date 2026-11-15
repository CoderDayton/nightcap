#include "runtime/appimage_menu_entry.h"

#include <unistd.h>

#include <fstream>
#include <sstream>
#include <system_error>

namespace mocktail {
namespace runtime {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kOwnMarker = "X-Nightcap-AppImage=true";
constexpr std::string_view kInstalledEntryName =
    "io.github.CoderDayton.nightcap.desktop";
constexpr std::string_view kNamedIcon = "io.github.CoderDayton.nightcap";

fs::path AbsoluteOr(const Environment& environment, std::string_view name,
                    const fs::path& fallback) {
  const fs::path value = environment.GetOr(name, "");
  return value.is_absolute() ? value : fallback;
}

fs::path Normal(const fs::path& path) {
  fs::path normal = path.lexically_normal();
  if (!normal.has_filename() && normal != normal.root_path()) {
    normal = normal.parent_path();
  }
  return normal;
}

bool Within(const fs::path& path, const fs::path& root) {
  if (root.empty()) return false;
  const fs::path normal_path = Normal(path);
  auto part = normal_path.begin();
  for (const fs::path& root_part : Normal(root)) {
    if (part == normal_path.end() || *part != root_part) return false;
    ++part;
  }
  return true;
}

bool Quotable(std::string_view path) {
  for (const unsigned char character : path) {
    if (character < 0x20 || character == 0x7f || character == '"' ||
        character == '`' || character == '$' || character == '\\') {
      return false;
    }
  }
  return true;
}

std::string ExecArgument(std::string_view path) {
  std::string quoted = "\"";
  for (const char character : path) {
    quoted += character;
    if (character == '%') quoted += '%';
  }
  return quoted + "\"";
}

bool Live(const fs::path& path) {
  std::error_code error;
  return !path.empty() && fs::is_regular_file(path, error) &&
         access(path.c_str(), X_OK) == 0;
}

struct ExistingEntry {
  bool present = false;
  bool ours = false;
  fs::path target;
};

ExistingEntry ReadEntry(const fs::path& entry) {
  ExistingEntry existing;
  std::error_code error;
  const fs::file_status status = fs::symlink_status(entry, error);
  existing.present = fs::exists(status);
  if (!fs::is_regular_file(status)) return existing;
  std::ifstream input(entry);
  std::string line;
  while (std::getline(input, line)) {
    if (line == kOwnMarker) existing.ours = true;
    if (line.rfind("TryExec=", 0) == 0) existing.target = line.substr(8);
  }
  return existing;
}

bool WriteFile(const fs::path& path, std::string_view text) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  const fs::path temporary =
      path.parent_path() /
      ("." + path.filename().string() + "." + std::to_string(getpid()));
  {
    std::ofstream output(temporary, std::ios::trunc);
    output << text;
    if (!output.flush()) {
      fs::remove(temporary, error);
      return false;
    }
  }
  fs::permissions(temporary,
                  fs::perms::owner_read | fs::perms::owner_write |
                      fs::perms::group_read | fs::perms::others_read,
                  error);
  if (!error) fs::rename(temporary, path, error);
  if (error) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
  }
  return true;
}

AppImageMenuOutcome Failed(std::string detail) {
  return {AppImageMenuAction::kFailed, std::move(detail)};
}

AppImageMenuOutcome WriteEntry(const AppImageMenuPaths& paths,
                               const fs::path& app_image,
                               const fs::path& app_image_icon,
                               AppImageMenuAction action) {
  std::string icon(kNamedIcon);
  std::error_code error;
  if (!app_image_icon.empty()) {
    fs::create_directories(paths.icon_file.parent_path(), error);
    fs::copy_file(app_image_icon, paths.icon_file,
                  fs::copy_options::overwrite_existing, error);
    if (!error && Quotable(paths.icon_file.string())) {
      icon = paths.icon_file.string();
    }
  }
  const std::string entry = AppImageDesktopEntry(app_image, icon);
  if (entry.empty()) {
    return Failed("its path cannot go in a desktop entry: " +
                  app_image.string());
  }
  const fs::path target = paths.applications_dir / kAppImageMenuEntryName;
  if (!WriteFile(target, entry)) return Failed("cannot write " + target.string());
  return {action, target.string()};
}

}

AppImageMenuPaths AppImageMenuPathsFor(const Environment& environment,
                                       const RuntimePaths& paths) {
  AppImageMenuPaths menu;
  const fs::path data_home =
      AbsoluteOr(environment, "XDG_DATA_HOME", paths.home() / ".local/share");
  menu.applications_dir = data_home / "applications";
  menu.icon_file = paths.data_root() / "appimage-icon.svg";
  menu.declined_marker = paths.state_root() / "appimage-menu-declined";

  const fs::path app_dir = AbsoluteOr(environment, "APPDIR", {});
  menu.installed_entry_dirs.push_back(menu.applications_dir);
  std::stringstream data_dirs(
      environment.GetOr("XDG_DATA_DIRS", "/usr/local/share:/usr/share"));
  std::string data_dir;
  while (std::getline(data_dirs, data_dir, ':')) {
    const fs::path dir = data_dir;
    if (dir.is_absolute() && !Within(dir, app_dir)) {
      menu.installed_entry_dirs.push_back(dir / "applications");
    }
  }
  menu.installed_entry_dirs.push_back(
      paths.home() / ".local/share/flatpak/exports/share/applications");
  menu.installed_entry_dirs.push_back(
      "/var/lib/flatpak/exports/share/applications");
  return menu;
}

fs::path RunningAppImage(const Environment& environment) {
  const fs::path app_image = environment.GetOr("APPIMAGE", "");
  if (!app_image.is_absolute() || !Live(app_image)) return {};
  const fs::path app_dir = environment.GetOr("APPDIR", "");
  std::error_code error;
  if (!app_dir.is_absolute() ||
      !fs::is_regular_file(app_dir / kInstalledEntryName, error)) {
    return {};
  }
  const fs::path canonical = fs::canonical(app_image, error);
  return error ? fs::path() : canonical;
}

fs::path AppImageIcon(const Environment& environment) {
  const fs::path app_dir = environment.GetOr("APPDIR", "");
  if (!app_dir.is_absolute()) return {};
  for (const fs::path& icon :
       {app_dir / (std::string(kNamedIcon) + ".svg"), app_dir / ".DirIcon"}) {
    std::error_code error;
    if (fs::is_regular_file(icon, error)) return icon;
  }
  return {};
}

std::string AppImageDesktopEntry(const fs::path& app_image,
                                 std::string_view icon) {
  const std::string path = app_image.string();
  if (!app_image.is_absolute() || !Quotable(path) || !Quotable(icon)) {
    return {};
  }
  const std::string exec = ExecArgument(path);
  return "[Desktop Entry]\n"
         "Type=Application\n"
         "Name=Nightcap\n"
         "GenericName=Roblox Player\n"
         "Comment=Play Roblox on Linux\n"
         "TryExec=" + path + "\n"
         "Exec=env SDL_VIDEODRIVER=wayland,x11 " + exec + " %u\n"
         "Icon=" + std::string(icon) + "\n"
         "StartupWMClass=io.github.CoderDayton.nightcap\n"
         "Categories=Game;\n"
         "Terminal=false\n"
         "MimeType=x-scheme-handler/roblox;x-scheme-handler/roblox-player;\n" +
         std::string(kOwnMarker) + "\n"
         "Actions=settings;\n"
         "\n"
         "[Desktop Action settings]\n"
         "Name=Settings\n"
         "Exec=" + exec + " --settings\n";
}

AppImageMenuOutcome UpdateAppImageMenuEntry(const AppImageMenuPaths& paths,
                                            const fs::path& app_image,
                                            const fs::path& app_image_icon,
                                            const AskToAddMenuEntry& ask) {
  const fs::path entry = paths.applications_dir / kAppImageMenuEntryName;
  const ExistingEntry existing = ReadEntry(entry);
  bool removed = false;
  if (existing.ours && !Live(existing.target)) {
    std::error_code error;
    fs::remove(entry, error);
    if (error) return Failed("cannot remove " + entry.string());
    fs::remove(paths.icon_file, error);
    removed = true;
  }
  if (app_image.empty()) {
    if (!removed) return {};
    return {AppImageMenuAction::kRemoved, existing.target.string()};
  }
  if (removed) {
    return WriteEntry(paths, app_image, app_image_icon,
                      AppImageMenuAction::kMoved);
  }
  std::error_code error;
  if (existing.present || fs::exists(paths.declined_marker, error)) return {};
  for (const fs::path& dir : paths.installed_entry_dirs) {
    if (fs::exists(dir / kInstalledEntryName, error)) return {};
  }
  const std::optional<bool> answer = ask();
  if (!answer.has_value()) return {};
  if (!*answer) {
    if (!WriteFile(paths.declined_marker, "declined\n")) {
      return Failed("cannot write " + paths.declined_marker.string());
    }
    return {AppImageMenuAction::kDeclined, paths.declined_marker.string()};
  }
  return WriteEntry(paths, app_image, app_image_icon,
                    AppImageMenuAction::kAdded);
}

}
}
