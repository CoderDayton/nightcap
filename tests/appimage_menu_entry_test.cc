#include "runtime/appimage_menu_entry.h"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

namespace fs = std::filesystem;

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(std::unordered_map<std::string, std::string> values)
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

std::string Read(const fs::path& path) {
  std::ifstream input(path);
  std::stringstream text;
  text << input.rdbuf();
  return text.str();
}

void Write(const fs::path& path, std::string_view text, bool executable) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << text;
  fs::permissions(path,
                  executable ? fs::perms::owner_all : fs::perms::owner_read |
                                                          fs::perms::owner_write);
}

class AppImageMenuEntryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (fs::temp_directory_path() / "appimage-menu-XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    root_ = pattern;
    paths_.applications_dir = root_ / "data/applications";
    paths_.icon_file = root_ / "data/mocktail/appimage-icon.svg";
    paths_.declined_marker = root_ / "state/mocktail/appimage-menu-declined";
    paths_.installed_entry_dirs = {root_ / "data/applications",
                                   root_ / "usr/share/applications"};
    app_image_ = root_ / "Apps/Nightcap-x86_64.AppImage";
    Write(app_image_, "elf", true);
    icon_ = root_ / "mount/io.github.CoderDayton.nightcap.svg";
    Write(icon_, "<svg/>", false);
  }

  void TearDown() override { fs::remove_all(root_); }

  fs::path Entry() const {
    return paths_.applications_dir / kAppImageMenuEntryName;
  }

  AppImageMenuOutcome Run(const fs::path& app_image,
                          std::optional<bool> answer) {
    return UpdateAppImageMenuEntry(paths_, app_image, icon_, [&, answer] {
      ++asked_;
      return answer;
    });
  }

  fs::path root_;
  AppImageMenuPaths paths_;
  fs::path app_image_;
  fs::path icon_;
  int asked_ = 0;
};

TEST_F(AppImageMenuEntryTest, AddsTheEntryWhenTheUserAgrees) {
  EXPECT_EQ(Run(app_image_, true).action, AppImageMenuAction::kAdded);
  EXPECT_EQ(asked_, 1);
  const std::string entry = Read(Entry());
  EXPECT_NE(entry.find("TryExec=" + app_image_.string() + "\n"),
            std::string::npos);
  EXPECT_NE(entry.find("Icon=" + paths_.icon_file.string() + "\n"),
            std::string::npos);
  EXPECT_EQ(Read(paths_.icon_file), "<svg/>");
  EXPECT_EQ(fs::status(Entry()).permissions() & fs::perms::all,
            fs::perms::owner_read | fs::perms::owner_write |
                fs::perms::group_read | fs::perms::others_read);
}

TEST_F(AppImageMenuEntryTest, AsksOnlyOnceAfterTheEntryIsAdded) {
  Run(app_image_, true);
  EXPECT_EQ(Run(app_image_, true).action, AppImageMenuAction::kNone);
  EXPECT_EQ(asked_, 1);
}

TEST_F(AppImageMenuEntryTest, RemembersANo) {
  EXPECT_EQ(Run(app_image_, false).action, AppImageMenuAction::kDeclined);
  EXPECT_TRUE(fs::exists(paths_.declined_marker));
  EXPECT_FALSE(fs::exists(Entry()));
  EXPECT_EQ(Run(app_image_, true).action, AppImageMenuAction::kNone);
  EXPECT_EQ(asked_, 1);
}

TEST_F(AppImageMenuEntryTest, AsksAgainWhenNoDialogCouldShow) {
  EXPECT_EQ(Run(app_image_, std::nullopt).action, AppImageMenuAction::kNone);
  EXPECT_FALSE(fs::exists(paths_.declined_marker));
  Run(app_image_, std::nullopt);
  EXPECT_EQ(asked_, 2);
}

TEST_F(AppImageMenuEntryTest, RemovesTheEntryOnceTheAppImageIsGone) {
  Run(app_image_, true);
  fs::remove(app_image_);
  EXPECT_EQ(Run({}, true).action, AppImageMenuAction::kRemoved);
  EXPECT_FALSE(fs::exists(Entry()));
  EXPECT_FALSE(fs::exists(paths_.icon_file));
}

TEST_F(AppImageMenuEntryTest, MovesTheEntryToTheAppImageThatReplacedIt) {
  Run(app_image_, true);
  fs::remove(app_image_);
  const fs::path moved = root_ / "Downloads/Nightcap-x86_64.AppImage";
  Write(moved, "elf", true);
  EXPECT_EQ(Run(moved, false).action, AppImageMenuAction::kMoved);
  EXPECT_EQ(asked_, 1);
  EXPECT_NE(Read(Entry()).find("TryExec=" + moved.string() + "\n"),
            std::string::npos);
}

TEST_F(AppImageMenuEntryTest, LeavesAnEntryForAnotherAppImageThatStillExists) {
  Run(app_image_, true);
  const fs::path nightly = root_ / "Apps/Nightcap-nightly-x86_64.AppImage";
  Write(nightly, "elf", true);
  EXPECT_EQ(Run(nightly, true).action, AppImageMenuAction::kNone);
  EXPECT_EQ(asked_, 1);
  EXPECT_NE(Read(Entry()).find("TryExec=" + app_image_.string() + "\n"),
            std::string::npos);
}

TEST_F(AppImageMenuEntryTest, DoesNotOfferWhenNightcapIsAlreadyInTheMenu) {
  Write(root_ / "usr/share/applications/io.github.CoderDayton.nightcap.desktop",
        "[Desktop Entry]\n", false);
  EXPECT_EQ(Run(app_image_, true).action, AppImageMenuAction::kNone);
  EXPECT_EQ(asked_, 0);
}

TEST_F(AppImageMenuEntryTest, NeverRemovesAnEntryItDidNotWrite) {
  const std::string foreign =
      "[Desktop Entry]\nTryExec=" + (root_ / "gone").string() + "\n";
  Write(Entry(), foreign, false);
  EXPECT_EQ(Run({}, true).action, AppImageMenuAction::kNone);
  EXPECT_EQ(Read(Entry()), foreign);
}

TEST_F(AppImageMenuEntryTest, FallsBackToTheNamedIconForAnUnquotableIconPath) {
  paths_.icon_file = root_ / "data/$dir/appimage-icon.svg";
  EXPECT_EQ(Run(app_image_, true).action, AppImageMenuAction::kAdded);
  EXPECT_NE(Read(Entry()).find("Icon=io.github.CoderDayton.nightcap\n"),
            std::string::npos);
}

TEST(AppImageDesktopEntryTest, QuotesTheAppImagePath) {
  const std::string entry = AppImageDesktopEntry(
      "/home/u/My Apps/100%.AppImage", "/home/u/icon.svg");
  EXPECT_NE(entry.find("TryExec=/home/u/My Apps/100%.AppImage\n"),
            std::string::npos);
  EXPECT_NE(entry.find("Exec=env SDL_VIDEODRIVER=wayland,x11 "
                       "\"/home/u/My Apps/100%%.AppImage\" %u\n"),
            std::string::npos);
  EXPECT_NE(entry.find("Exec=\"/home/u/My Apps/100%%.AppImage\" --settings\n"),
            std::string::npos);
  EXPECT_NE(entry.find("MimeType=x-scheme-handler/roblox;"
                       "x-scheme-handler/roblox-player;\n"),
            std::string::npos);
}

TEST(AppImageDesktopEntryTest, RefusesPathsItCannotQuote) {
  for (const char* path :
       {"relative/Nightcap.AppImage", "/a/\"b\".AppImage", "/a/$b.AppImage",
        "/a/`b`.AppImage", "/a/b\\c.AppImage", "/a/b\nc.AppImage"}) {
    EXPECT_EQ(AppImageDesktopEntry(path, "icon"), "") << path;
  }
}

TEST_F(AppImageMenuEntryTest, FindsTheRunningAppImage) {
  const std::string mount = icon_.parent_path().string();
  Write(icon_.parent_path() / "io.github.CoderDayton.nightcap.desktop",
        "[Desktop Entry]\n", false);
  EXPECT_EQ(RunningAppImage(MapEnvironment(
                {{"APPIMAGE", app_image_.string()}, {"APPDIR", mount}})),
            fs::canonical(app_image_));
  EXPECT_EQ(RunningAppImage(MapEnvironment({})), fs::path());
  EXPECT_EQ(RunningAppImage(MapEnvironment(
                {{"APPIMAGE", (root_ / "missing").string()}, {"APPDIR", mount}})),
            fs::path());
  EXPECT_EQ(RunningAppImage(MapEnvironment(
                {{"APPIMAGE", "Nightcap.AppImage"}, {"APPDIR", mount}})),
            fs::path());
}

TEST_F(AppImageMenuEntryTest, IgnoresAnAppImageVariableLeakedFromAnotherApp) {
  const fs::path other_mount = root_ / "other-mount";
  fs::create_directories(other_mount);
  EXPECT_EQ(RunningAppImage(MapEnvironment({{"APPIMAGE", app_image_.string()},
                                            {"APPDIR", other_mount}})),
            fs::path());
  EXPECT_EQ(RunningAppImage(MapEnvironment({{"APPIMAGE", app_image_.string()}})),
            fs::path());
}

TEST_F(AppImageMenuEntryTest, SkipsTheMountedAppImageDataDirWithATrailingSlash) {
  const MapEnvironment environment(
      {{"HOME", root_.string()},
       {"APPDIR", "/tmp/.mount_Nightcap/"},
       {"XDG_DATA_DIRS", "/tmp/.mount_Nightcap/usr/share:/usr/share"}});
  const AppImageMenuPaths paths = AppImageMenuPathsFor(
      environment, RuntimePaths::FromEnvironment(environment, root_));
  EXPECT_EQ(std::find(paths.installed_entry_dirs.begin(),
                      paths.installed_entry_dirs.end(),
                      fs::path("/tmp/.mount_Nightcap/usr/share/applications")),
            paths.installed_entry_dirs.end());
}

TEST_F(AppImageMenuEntryTest, FindsTheIconInTheMountedAppImage) {
  EXPECT_EQ(AppImageIcon(MapEnvironment({{"APPDIR", icon_.parent_path()}})),
            icon_);
  const fs::path dir_icon_only = root_ / "mount2";
  Write(dir_icon_only / ".DirIcon", "<svg/>", false);
  EXPECT_EQ(AppImageIcon(MapEnvironment({{"APPDIR", dir_icon_only}})),
            dir_icon_only / ".DirIcon");
  EXPECT_EQ(AppImageIcon(MapEnvironment({})), fs::path());
}

TEST_F(AppImageMenuEntryTest, PathsFollowXdgDirectories) {
  const MapEnvironment environment({{"HOME", root_.string()},
                                    {"XDG_DATA_HOME", (root_ / "d").string()},
                                    {"XDG_STATE_HOME", (root_ / "s").string()},
                                    {"XDG_DATA_DIRS", "/opt/share:relative"}});
  const AppImageMenuPaths paths = AppImageMenuPathsFor(
      environment, RuntimePaths::FromEnvironment(environment, root_));
  EXPECT_EQ(paths.applications_dir, root_ / "d/applications");
  EXPECT_EQ(paths.declined_marker, root_ / "s/mocktail/appimage-menu-declined");
  EXPECT_EQ(paths.icon_file, root_ / "d/mocktail/appimage-icon.svg");
  EXPECT_EQ(paths.installed_entry_dirs.front(), root_ / "d/applications");
  EXPECT_NE(std::find(paths.installed_entry_dirs.begin(),
                      paths.installed_entry_dirs.end(),
                      fs::path("/opt/share/applications")),
            paths.installed_entry_dirs.end());
  EXPECT_EQ(std::find(paths.installed_entry_dirs.begin(),
                      paths.installed_entry_dirs.end(),
                      fs::path("relative/applications")),
            paths.installed_entry_dirs.end());
}

}
}
}
