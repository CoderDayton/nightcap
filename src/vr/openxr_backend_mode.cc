#include "mocktail/vr/openxr_backend.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#define JSON_NOEXCEPTION 1
#include <nlohmann/json.hpp>
namespace mocktail::vr {
std::string
SelectVrRuntimeManifest(const std::string &explicit_manifest,
                        const std::vector<std::string> &candidates) {
  if (!explicit_manifest.empty())
    return explicit_manifest;
  for (const auto &path : candidates) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error))
      return path;
  }
  return {};
}

std::vector<std::string> VrRuntimeManifestCandidates(
    const std::string &preference, const std::string &home,
    const std::string &config_home, const std::string &config_dirs) {
  if (preference == "steamvr" || preference == "alvr") {
    if (home.empty())
      return {};
    std::vector<std::string> paths;
    const auto config = config_home.empty() ? home + "/.config" : config_home;
    const auto registry = config + "/openvr/openvrpaths.vrpath";
    std::error_code error;
    if (std::filesystem::file_size(registry, error) <= 1024 * 1024 && !error) {
      std::ifstream input(registry);
      const auto json = nlohmann::json::parse(input, nullptr, false);
      if (json.is_object() && json.contains("runtime") &&
          json["runtime"].is_array()) {
        for (const auto &item : json["runtime"]) {
          if (item.is_string()) {
            const auto root = std::filesystem::path(item.get<std::string>());
            if (root.is_absolute())
              paths.push_back((root / "steamxr_linux64.json").string());
          }
        }
      }
    }
    const std::vector<std::string> standard = {
        home +
            "/.local/share/Steam/steamapps/common/SteamVR/steamxr_linux64.json",
        home + "/.steam/steam/steamapps/common/SteamVR/steamxr_linux64.json",
        home + "/.steam/debian-installation/steamapps/common/SteamVR/"
               "steamxr_linux64.json"};
    paths.insert(paths.end(), standard.begin(), standard.end());
    return paths;
  }
  const std::vector<std::string> wivrn = {
      "/usr/local/share/openxr/1/openxr_wivrn.json",
      "/usr/share/openxr/1/openxr_wivrn.json"};
  if (preference == "wivrn")
    return wivrn;
  if (preference == "system")
    return {}; // Use the loader's full discovery rules.
  std::vector<std::string> candidates;
  const auto add_config = [&](const std::string &path) {
    if (path.empty() || path[0] != '/')
      return;
    candidates.push_back(path + "/openxr/1/active_runtime.x86_64.json");
    candidates.push_back(path + "/openxr/1/active_runtime.json");
  };
  add_config(!config_home.empty() ? config_home
                                  : (home.empty() ? "" : home + "/.config"));
  std::istringstream directories(config_dirs.empty() ? "/etc/xdg"
                                                     : config_dirs);
  std::string directory;
  while (std::getline(directories, directory, ':'))
    add_config(directory);
  add_config("/etc");
  candidates.insert(candidates.end(), wivrn.begin(), wivrn.end());
  return candidates;
}

VrRuntimeDiscovery DiscoverVrRuntime(const std::string &preference,
                                     const std::string &home,
                                     const std::string &config_home,
                                     const std::string &config_dirs,
                                     const std::string &runtime_dir,
                                     const std::string &proc_root) {
  VrRuntimeDiscovery result;
  auto steam =
      VrRuntimeManifestCandidates("steamvr", home, config_home, config_dirs);
  auto wivrn =
      VrRuntimeManifestCandidates("wivrn", home, config_home, config_dirs);
  bool steam_running = false;
  bool wivrn_listening = false;
  // Read the kernel's socket table: a leftover socket pathname alone is not
  // evidence of a listening server. Do not connect and create a dummy XR
  // client.
  const auto ipc = (std::filesystem::path(runtime_dir) / "wivrn/comp_ipc")
                       .lexically_normal();
  if (!runtime_dir.empty()) {
    std::ifstream sockets(std::filesystem::path(proc_root) / "net/unix");
    std::string line;
    while (std::getline(sockets, line)) {
      std::istringstream row(line);
      std::string num, refs, protocol, flags, type, state, inode, path;
      if (!(row >> num >> refs >> protocol >> flags >> type >> state >> inode))
        continue;
      std::getline(row >> std::ws, path);
      if (flags == "00010000" && type == "0001" &&
          std::filesystem::path(path).lexically_normal() == ipc) {
        struct stat socket_info{};
        if (stat(path.c_str(), &socket_info) == 0 &&
            S_ISSOCK(socket_info.st_mode) && socket_info.st_uid == getuid())
          wivrn_listening = true;
      }
    }
  }
  // Restrict process discovery to this user. In particular, never select a
  // root server for a desktop client or another user's SteamVR installation.
  std::error_code error;
  std::filesystem::directory_iterator processes(proc_root, error);
  for (; !error && processes != std::filesystem::directory_iterator();
       processes.increment(error)) {
    const auto name = processes->path().filename().string();
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) {
          return c >= '0' && c <= '9';
        }))
      continue;
    struct stat metadata{};
    if (stat(processes->path().c_str(), &metadata) != 0 ||
        metadata.st_uid != getuid())
      continue;
    std::ifstream comm_file(processes->path() / "comm");
    std::string comm;
    std::getline(comm_file, comm);
    if (comm != "vrserver" && comm != "wivrn-server")
      continue;
    std::error_code exe_error;
    const auto executable =
        std::filesystem::read_symlink(processes->path() / "exe", exe_error);
    if (exe_error || !executable.is_absolute() || executable.filename() != comm)
      continue;
    if (comm == "vrserver") {
      steam_running = true;
      steam.insert(steam.begin(),
                   (executable.parent_path().parent_path().parent_path() /
                    "steamxr_linux64.json")
                       .string());
    } else {
      // Native developer builds keep this manifest at their build root.
      auto directory = executable.parent_path();
      for (int i = 0; i < 3 && directory.has_relative_path();
           ++i, directory = directory.parent_path()) {
        wivrn.insert(wivrn.begin(),
                     (directory / "openxr_wivrn-dev.json").string());
      }
    }
  }
  if (preference == "steamvr" || preference == "alvr") {
    result.candidates = steam;
    result.reason = "explicit SteamVR/ALVR preference";
  } else if (preference == "wivrn") {
    result.candidates = wivrn;
    result.reason = "explicit WiVRn preference";
  } else if (preference == "system") {
    result.reason = "system OpenXR loader selection";
  } else {
    const auto registered =
        VrRuntimeManifestCandidates("auto", home, config_home, config_dirs);
    if (steam_running != wivrn_listening) {
      const std::string runtime_tag = steam_running ? "steamvr" : "wivrn";
      for (const auto &path : registered) {
        // Only registered entries, not native fallback paths.
        if (std::filesystem::path(path).filename().string().find(
                "active_runtime") != 0)
          continue;
        std::error_code manifest_error;
        if (std::filesystem::file_size(path, manifest_error) > 1024 * 1024 ||
            manifest_error)
          continue;
        auto identity =
            std::filesystem::weakly_canonical(path, manifest_error).string();
        std::ifstream input(path);
        const auto json = nlohmann::json::parse(input, nullptr, false);
        if (json.is_object() && json.contains("runtime") &&
            json["runtime"].is_object()) {
          for (const auto *key : {"name", "library_path"}) {
            const auto &runtime = json["runtime"];
            if (runtime.contains(key) && runtime[key].is_string())
              identity += " " + runtime[key].get<std::string>();
          }
        }
        std::transform(identity.begin(), identity.end(), identity.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (identity.find(runtime_tag) != std::string::npos)
          result.candidates.push_back(path);
      }
      const auto &active = steam_running ? steam : wivrn;
      result.candidates.insert(result.candidates.end(), active.begin(),
                               active.end());
      result.reason = steam_running
                          ? "running SteamVR detected (including ALVR)"
                          : "listening WiVRn server detected";
    } else {
      result.reason =
          steam_running
              ? "both servers detected; registered runtime has priority"
              : "no single running server detected; registered runtime has "
                "priority";
    }
    result.candidates.insert(result.candidates.end(), registered.begin(),
                             registered.end());
    result.candidates.insert(result.candidates.end(), steam.begin(),
                             steam.end());
    result.candidates.insert(result.candidates.end(), wivrn.begin(),
                             wivrn.end());
  }
  return result;
}

std::string VrRuntimeUnavailableHint(bool user_selected,
                                     const std::string &manifest) {
  if (user_selected) {
    return "User-selected XR_RUNTIME_JSON '" + manifest +
           "' is unavailable. Check the manifest/library and that its runtime "
           "server "
           "is running with a connected headset; this error alone does not "
           "distinguish them.";
  }
  if (!manifest.empty()) {
    return "Selected OpenXR runtime '" + manifest +
           "' is unavailable. Start its server "
           "(WiVRn, or SteamVR with ALVR) and connect the headset. Run the "
           "server and "
           "Mocktail as the same desktop user in the same runtime environment.";
  }
  return "The registered OpenXR runtime is unavailable. Start WiVRn or SteamVR "
         "with ALVR and "
         "connect the headset, or explicitly select an installed runtime with "
         "XR_RUNTIME_JSON.";
}

VrBackendMode ResolveVrBackendMode(bool vr_enabled) {
  if (!vr_enabled) {
    return VrBackendMode::kDisabled;
  }
  const char *value = std::getenv("MOCKTAIL_VR_BACKEND");
  const std::string mode = value ? value : "xr";
  if (mode == "native" || mode == "native-stereo") {
    return VrBackendMode::kNativeOnly;
  }
  return VrBackendMode::kXrOutput;
}

const char *VrBackendModeName(VrBackendMode mode) {
  switch (mode) {
  case VrBackendMode::kDisabled:
    return "disabled";
  case VrBackendMode::kNativeOnly:
    return "native-stereo-diagnostic";
  case VrBackendMode::kXrOutput:
    return "openxr-output";
  }
  return "disabled";
}

} // namespace mocktail::vr
