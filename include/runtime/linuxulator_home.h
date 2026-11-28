#ifndef MOCKTAIL_RUNTIME_LINUXULATOR_HOME_H_
#define MOCKTAIL_RUNTIME_LINUXULATOR_HOME_H_

#include <optional>
#include <string>
#include <string_view>

namespace mocktail::runtime {

bool IsFreeBsdKernelVersion(std::string_view version);

// Value for SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR. KDE on FreeBSD suspends
// compositing for the whole desktop while a window requests bypass.
const char* X11CompositorBypassDefault(std::string_view proc_version);

std::string ReadProcVersion();

struct LinuxulatorHomeHost {
  std::string proc_version;
  std::string home;
  // realpath() of "/compat/linux" + home, or "" when it does not exist.
  std::string compat_home_realpath;
  std::string proc_mounts;
};

// FreeBSD's Linuxulator creates files under /compat/linux whenever the parent
// directory exists there, even if the file already exists in the real home.
// A separate /compat/linux copy of the home therefore splits Nightcap's data
// between two trees. Returns that copy, unless it is a symlink to the real home
// or the real home is nullfs-mounted over it (or over a parent).
std::optional<std::string> FindLinuxulatorShadowHome(
    const LinuxulatorHomeHost& host);

LinuxulatorHomeHost ReadLinuxulatorHomeHost();

std::string LinuxulatorShadowHomeWarning(const std::string& home,
                                         const std::string& shadow);

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_LINUXULATOR_HOME_H_
