#include "runtime/linuxulator_alsa.h"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using mocktail::runtime::LinuxulatorAlsaHost;
using mocktail::runtime::LinuxulatorAlsaPlan;
using mocktail::runtime::PlanLinuxulatorAlsa;

constexpr char kFreeBsdVersion[] =
    "Linux version 5.15.0 (des@freebsd.org) (gcc version FreeBSD Clang 19.1.7) "
    "#4 Sun Dec 18 04:30:00 CET 1977\n";

LinuxulatorAlsaHost FreeBsdHost() {
  LinuxulatorAlsaHost host;
  host.proc_version = kFreeBsdVersion;
  host.oss_plugin_installed = true;
  return host;
}

TEST(LinuxulatorAlsaTest, LeavesLinuxAlone) {
  LinuxulatorAlsaHost host = FreeBsdHost();
  host.proc_version = "Linux version 7.3.6-3-cachyos (linux-cachyos@cachyos)\n";
  EXPECT_EQ(PlanLinuxulatorAlsa(host), LinuxulatorAlsaPlan::kLeaveAlone);
}

TEST(LinuxulatorAlsaTest, RoutesFreeBsdToOss) {
  EXPECT_EQ(PlanLinuxulatorAlsa(FreeBsdHost()), LinuxulatorAlsaPlan::kRouteToOss);
}

TEST(LinuxulatorAlsaTest, CommentOnlySystemConfigIsNotAUserConfig) {
  LinuxulatorAlsaHost host = FreeBsdHost();
  host.system_asound_conf =
      "#\n# Place your global alsa-lib configuration here...\n#\n\n";
  EXPECT_EQ(PlanLinuxulatorAlsa(host), LinuxulatorAlsaPlan::kRouteToOss);
}

TEST(LinuxulatorAlsaTest, KeepsTheUsersOwnConfig) {
  LinuxulatorAlsaHost system = FreeBsdHost();
  system.system_asound_conf = "pcm.!default {\n  type oss\n  device /dev/dsp3\n}\n";
  EXPECT_EQ(PlanLinuxulatorAlsa(system), LinuxulatorAlsaPlan::kLeaveAlone);

  LinuxulatorAlsaHost user = FreeBsdHost();
  user.user_asoundrc = true;
  EXPECT_EQ(PlanLinuxulatorAlsa(user), LinuxulatorAlsaPlan::kLeaveAlone);

  LinuxulatorAlsaHost path = FreeBsdHost();
  path.alsa_config_path_set = true;
  EXPECT_EQ(PlanLinuxulatorAlsa(path), LinuxulatorAlsaPlan::kLeaveAlone);
}

TEST(LinuxulatorAlsaTest, FollowsTheChosenSdlAudioDriver) {
  LinuxulatorAlsaHost pulse = FreeBsdHost();
  pulse.sdl_audio_driver = "pulseaudio";
  EXPECT_EQ(PlanLinuxulatorAlsa(pulse), LinuxulatorAlsaPlan::kLeaveAlone);

  LinuxulatorAlsaHost alsa = FreeBsdHost();
  alsa.sdl_audio_driver = "pipewire,alsa";
  EXPECT_EQ(PlanLinuxulatorAlsa(alsa), LinuxulatorAlsaPlan::kRouteToOss);
}

TEST(LinuxulatorAlsaTest, ReportsAMissingOssPlugin) {
  LinuxulatorAlsaHost host = FreeBsdHost();
  host.oss_plugin_installed = false;
  EXPECT_EQ(PlanLinuxulatorAlsa(host), LinuxulatorAlsaPlan::kOssPluginMissing);
}

TEST(LinuxulatorAlsaTest, WritesTheOssConfigOnlyWhenItChanges) {
  char root[] = "/tmp/mocktail_linuxulator_alsa_XXXXXX";
  ASSERT_NE(::mkdtemp(root), nullptr);
  const std::filesystem::path dir = std::filesystem::path(root) / "cache";

  std::string error;
  const auto path = mocktail::runtime::WriteLinuxulatorOssAlsaConfig(dir, &error);
  ASSERT_TRUE(path.has_value()) << error;
  std::stringstream text;
  text << std::ifstream(*path).rdbuf();
  EXPECT_NE(text.str().find("pcm.!default"), std::string::npos);
  EXPECT_NE(text.str().find("ctl.!default"), std::string::npos);
  EXPECT_NE(text.str().find("type oss"), std::string::npos);
  EXPECT_NE(text.str().find("device /dev/dsp\n"), std::string::npos);

  struct stat before {};
  ASSERT_EQ(::stat(path->c_str(), &before), 0);
  ASSERT_EQ(mocktail::runtime::WriteLinuxulatorOssAlsaConfig(dir, &error), path);
  struct stat after {};
  ASSERT_EQ(::stat(path->c_str(), &after), 0);
  EXPECT_EQ(before.st_ino, after.st_ino);

  std::filesystem::remove_all(root);
}

}  // namespace
