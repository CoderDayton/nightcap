#ifndef MOCKTAIL_RUNTIME_LINUXULATOR_ALSA_H_
#define MOCKTAIL_RUNTIME_LINUXULATOR_ALSA_H_

#include <filesystem>
#include <optional>
#include <string>

namespace mocktail::runtime {

// What the ALSA setup decision needs to know about the host.
struct LinuxulatorAlsaHost {
  std::string proc_version;
  bool alsa_config_path_set = false;
  std::string sdl_audio_driver;
  bool user_asoundrc = false;
  std::string system_asound_conf;
  bool oss_plugin_installed = false;
};

enum class LinuxulatorAlsaPlan {
  kLeaveAlone,
  kRouteToOss,
  kOssPluginMissing,
};

// On FreeBSD's Linuxulator, a Linux userspace's default ALSA device does not
// reach FreeBSD's sound driver, and absolute conf.d symlinks can make ALSA drop
// its whole configuration. Routing ALSA to the OSS plugin fixes both, unless
// the user configured ALSA or chose another SDL audio driver.
LinuxulatorAlsaPlan PlanLinuxulatorAlsa(const LinuxulatorAlsaHost& host);

LinuxulatorAlsaHost ReadLinuxulatorAlsaHost();

// Writes the OSS-only ALSA config into `directory`, leaving an identical file
// untouched, and returns its path.
std::optional<std::filesystem::path> WriteLinuxulatorOssAlsaConfig(
    const std::filesystem::path& directory, std::string* error);

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_LINUXULATOR_ALSA_H_
