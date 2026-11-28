#include "runtime/linuxulator_alsa.h"

#include <unistd.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

namespace mocktail::runtime {
namespace {

constexpr std::string_view kOssAlsaConfig =
    "# Written by Nightcap on FreeBSD's Linuxulator. Set ALSA_CONFIG_PATH or\n"
    "# create ~/.asoundrc to use your own ALSA configuration instead.\n"
    "pcm.!default {\n"
    "    type oss\n"
    "    device /dev/dsp\n"
    "}\n"
    "ctl.!default {\n"
    "    type oss\n"
    "    device /dev/dsp\n"
    "}\n";

constexpr std::array<const char*, 3> kOssPluginPaths = {
    "/usr/lib64/alsa-lib/libasound_module_pcm_oss.so",
    "/usr/lib/alsa-lib/libasound_module_pcm_oss.so",
    "/usr/lib/x86_64-linux-gnu/alsa-lib/libasound_module_pcm_oss.so",
};

bool IsFreeBsdKernel(std::string_view version) {
  return version.find("FreeBSD") != std::string_view::npos ||
         version.find("freebsd.org") != std::string_view::npos;
}

bool HasSettings(std::string_view config) {
  std::size_t start = 0;
  while (start < config.size()) {
    std::size_t end = config.find('\n', start);
    if (end == std::string_view::npos) end = config.size();
    const std::string_view line = config.substr(start, end - start);
    const std::size_t first = line.find_first_not_of(" \t\r");
    if (first != std::string_view::npos && line[first] != '#') return true;
    start = end + 1;
  }
  return false;
}

std::string ReadSmallFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

}  // namespace

LinuxulatorAlsaPlan PlanLinuxulatorAlsa(const LinuxulatorAlsaHost& host) {
  if (!IsFreeBsdKernel(host.proc_version) || host.alsa_config_path_set ||
      host.user_asoundrc || HasSettings(host.system_asound_conf)) {
    return LinuxulatorAlsaPlan::kLeaveAlone;
  }
  if (!host.sdl_audio_driver.empty() &&
      host.sdl_audio_driver.find("alsa") == std::string::npos) {
    return LinuxulatorAlsaPlan::kLeaveAlone;
  }
  return host.oss_plugin_installed ? LinuxulatorAlsaPlan::kRouteToOss
                                   : LinuxulatorAlsaPlan::kOssPluginMissing;
}

LinuxulatorAlsaHost ReadLinuxulatorAlsaHost() {
  LinuxulatorAlsaHost host;
  host.proc_version = ReadSmallFile("/proc/version");
  if (!IsFreeBsdKernel(host.proc_version)) return host;
  host.alsa_config_path_set = std::getenv("ALSA_CONFIG_PATH") != nullptr;
  if (const char* driver = std::getenv("SDL_AUDIO_DRIVER")) {
    host.sdl_audio_driver = driver;
  }
  std::error_code error;
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    host.user_asoundrc =
        std::filesystem::exists(std::filesystem::path(home) / ".asoundrc", error);
  }
  host.system_asound_conf = ReadSmallFile("/etc/asound.conf");
  for (const char* plugin : kOssPluginPaths) {
    if (::access(plugin, R_OK) == 0) {
      host.oss_plugin_installed = true;
      break;
    }
  }
  return host;
}

std::optional<std::filesystem::path> WriteLinuxulatorOssAlsaConfig(
    const std::filesystem::path& directory, std::string* error) {
  const std::filesystem::path path = directory / "linuxulator-alsa.conf";
  if (ReadSmallFile(path) == kOssAlsaConfig) return path;

  std::error_code status;
  std::filesystem::create_directories(directory, status);
  if (status) {
    if (error != nullptr) *error = directory.string() + ": " + status.message();
    return std::nullopt;
  }
  const std::filesystem::path temporary =
      directory / ("linuxulator-alsa.conf." + std::to_string(::getpid()));
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << kOssAlsaConfig;
    if (!output.flush()) {
      if (error != nullptr) *error = temporary.string() + ": write failed";
      std::filesystem::remove(temporary, status);
      return std::nullopt;
    }
  }
  std::filesystem::rename(temporary, path, status);
  if (status) {
    if (error != nullptr) *error = path.string() + ": " + status.message();
    std::filesystem::remove(temporary, status);
    return std::nullopt;
  }
  return path;
}

}  // namespace mocktail::runtime
