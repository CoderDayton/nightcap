#include "runtime/linuxulator_home.h"

#include <limits.h>
#include <stdlib.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>

namespace mocktail::runtime {
namespace {

constexpr std::string_view kCompatRoot = "/compat/linux";

std::string ReadText(const char* path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

// /proc/mounts escapes spaces and other bytes as \ooo.
std::string UnescapeMountField(std::string_view field) {
  std::string out;
  for (std::size_t i = 0; i < field.size(); ++i) {
    if (field[i] == '\\' && i + 3 < field.size() && field[i + 1] >= '0' && field[i + 1] <= '7' && field[i + 2] >= '0' &&
        field[i + 2] <= '7' && field[i + 3] >= '0' && field[i + 3] <= '7') {
      out.push_back(static_cast<char>((field[i + 1] - '0') * 64 +
                                      (field[i + 2] - '0') * 8 + (field[i + 3] - '0')));
      i += 3;
    } else {
      out.push_back(field[i]);
    }
  }
  return out;
}

bool IsSameOrParent(std::string_view parent, std::string_view path) {
  if (parent == "/" || parent.empty()) return false;
  return path == parent ||
         (path.size() > parent.size() && path.substr(0, parent.size()) == parent &&
          path[parent.size()] == '/');
}

bool HomeIsMirrored(std::string_view mounts, std::string_view home) {
  std::istringstream lines{std::string(mounts)};
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string source, target, type;
    if (!(fields >> source >> target >> type) || type != "nullfs") continue;
    source = UnescapeMountField(source);
    target = UnescapeMountField(target);
    if (source == target && IsSameOrParent(target, home)) return true;
  }
  return false;
}

}

bool IsFreeBsdKernelVersion(std::string_view version) {
  return version.find("FreeBSD") != std::string_view::npos ||
         version.find("freebsd.org") != std::string_view::npos;
}

const char* X11CompositorBypassDefault(std::string_view proc_version) {
  return IsFreeBsdKernelVersion(proc_version) ? "0" : "1";
}

std::string ReadProcVersion() { return ReadText("/proc/version"); }

std::optional<std::string> FindLinuxulatorShadowHome(
    const LinuxulatorHomeHost& host) {
  if (!IsFreeBsdKernelVersion(host.proc_version) || host.home.empty() ||
      host.home.front() != '/' || host.compat_home_realpath.empty()) {
    return std::nullopt;
  }
  if (!IsSameOrParent(kCompatRoot, host.compat_home_realpath) ||
      HomeIsMirrored(host.proc_mounts, host.home)) {
    return std::nullopt;
  }
  return host.compat_home_realpath;
}

LinuxulatorHomeHost ReadLinuxulatorHomeHost() {
  LinuxulatorHomeHost host;
  host.proc_version = ReadText("/proc/version");
  if (!IsFreeBsdKernelVersion(host.proc_version)) return host;
  if (const char* home = std::getenv("HOME")) host.home = home;
  if (host.home.empty() || host.home.front() != '/') return host;
  // The Linuxulator finds no /compat/linux/compat/linux, so this path falls
  // through to the real /compat/linux tree.
  const std::string compat_home = std::string(kCompatRoot) + host.home;
  char resolved[PATH_MAX];
  if (::realpath(compat_home.c_str(), resolved) != nullptr) {
    host.compat_home_realpath = resolved;
  }
  host.proc_mounts = ReadText("/proc/mounts");
  return host;
}

std::string LinuxulatorShadowHomeWarning(const std::string& home,
                                         const std::string& shadow) {
  return "FreeBSD's Linux layer has a separate copy of your home folder at " +
         shadow + ". New files Nightcap and Roblox create go there instead of " +
         home + ", so Nightcap's data splits between the two and Roblox can " +
         "fail to load.\n\nAs root, mount your real home over it:\n\n"
         "mount -t nullfs " + home + " " + shadow +
         "\n\nAdd a matching nullfs line with rw,late to /etc/fstab to keep it "
         "after a reboot. The mount hides what is in " + shadow +
         " until it is unmounted; move anything you need out of it first.";
}

}  // namespace mocktail::runtime
