#include "runtime/linuxulator_home.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {

using mocktail::runtime::FindLinuxulatorShadowHome;
using mocktail::runtime::LinuxulatorHomeHost;

constexpr char kFreeBsdVersion[] =
    "Linux version 5.15.0 (des@freebsd.org) (gcc version FreeBSD Clang 19.1.7)\n";
constexpr char kStockMounts[] =
    "/dev/gpt/rootfs / ufs rw,noatime 0 0\n"
    "devfs /dev devfs rw 0 0\n"
    "proc /proc proc rw 0 0\n"
    "sysfs /sys sysfs rw 0 0\n";

LinuxulatorHomeHost ShadowedHost() {
  LinuxulatorHomeHost host;
  host.proc_version = kFreeBsdVersion;
  host.home = "/home/vm";
  host.compat_home_realpath = "/compat/linux/home/vm";
  host.proc_mounts = kStockMounts;
  return host;
}

TEST(LinuxulatorHomeTest, FindsAStrayHomeCopy) {
  EXPECT_EQ(FindLinuxulatorShadowHome(ShadowedHost()),
            std::optional<std::string>("/compat/linux/home/vm"));
}

TEST(LinuxulatorHomeTest, IgnoresLinux) {
  LinuxulatorHomeHost host = ShadowedHost();
  host.proc_version = "Linux version 7.3.6-3-cachyos (linux-cachyos@cachyos)\n";
  EXPECT_EQ(FindLinuxulatorShadowHome(host), std::nullopt);
}

TEST(LinuxulatorHomeTest, IgnoresAMissingCompatHome) {
  LinuxulatorHomeHost host = ShadowedHost();
  host.compat_home_realpath.clear();
  EXPECT_EQ(FindLinuxulatorShadowHome(host), std::nullopt);
}

TEST(LinuxulatorHomeTest, AcceptsACompatHomeThatLinksToTheRealOne) {
  LinuxulatorHomeHost host = ShadowedHost();
  host.compat_home_realpath = "/home/vm";
  EXPECT_EQ(FindLinuxulatorShadowHome(host), std::nullopt);
}

TEST(LinuxulatorHomeTest, AcceptsANullfsMountOfTheHomeOrAParent) {
  LinuxulatorHomeHost home = ShadowedHost();
  home.proc_mounts = std::string(kStockMounts) + "/home/vm /home/vm nullfs rw 0 0\n";
  EXPECT_EQ(FindLinuxulatorShadowHome(home), std::nullopt);

  LinuxulatorHomeHost parent = ShadowedHost();
  parent.proc_mounts = std::string(kStockMounts) + "/home /home nullfs rw 0 0\n";
  EXPECT_EQ(FindLinuxulatorShadowHome(parent), std::nullopt);
}

TEST(LinuxulatorHomeTest, IgnoresMountsThatDoNotMirrorTheHome) {
  LinuxulatorHomeHost dataset = ShadowedHost();
  dataset.proc_mounts = std::string(kStockMounts) + "zroot/home /home zfs rw 0 0\n";
  EXPECT_TRUE(FindLinuxulatorShadowHome(dataset).has_value());

  LinuxulatorHomeHost other = ShadowedHost();
  other.proc_mounts = std::string(kStockMounts) + "/data/vm /home/vm nullfs rw 0 0\n";
  EXPECT_TRUE(FindLinuxulatorShadowHome(other).has_value());

  LinuxulatorHomeHost sibling = ShadowedHost();
  sibling.proc_mounts = std::string(kStockMounts) + "/home/vmx /home/vmx nullfs rw 0 0\n";
  EXPECT_TRUE(FindLinuxulatorShadowHome(sibling).has_value());
}

TEST(LinuxulatorHomeTest, KeepsTheCompositorOnFreeBsd) {
  EXPECT_STREQ(mocktail::runtime::X11CompositorBypassDefault(kFreeBsdVersion), "0");
  EXPECT_STREQ(mocktail::runtime::X11CompositorBypassDefault(
                "Linux version 7.3.6-3-cachyos (linux-cachyos@cachyos)\n"),
            "1");
}

TEST(LinuxulatorHomeTest, NamesTheMountInTheWarning) {
  const std::string message =
      mocktail::runtime::LinuxulatorShadowHomeWarning("/home/vm", "/compat/linux/home/vm");
  EXPECT_NE(message.find("mount -t nullfs /home/vm /compat/linux/home/vm"),
            std::string::npos);
}

}  // namespace
