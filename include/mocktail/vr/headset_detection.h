#ifndef MOCKTAIL_VR_HEADSET_DETECTION_H_
#define MOCKTAIL_VR_HEADSET_DETECTION_H_

#include <chrono>
#include <string>

namespace mocktail::vr {

// Set on the child process that answers DetectSteamVrHeadset. main() checks
// it first and returns RunHeadsetProbeChild() without starting Roblox.
inline constexpr const char* kHeadsetProbeEnvironment =
    "MOCKTAIL_VR_HEADSET_PROBE";

struct HeadsetDetection {
  bool headset = false;
  std::string reason;
};

// vr.mode: auto. True only when this user's SteamVR is running and its OpenXR
// runtime reports a head-mounted display. SteamVR is never started. The
// runtime is asked from a child process (`self_executable` with
// kHeadsetProbeEnvironment set) so its libraries never load into this one
// before the VR backend arms.
HeadsetDetection DetectSteamVrHeadset(
    const std::string& self_executable,
    std::chrono::milliseconds timeout = std::chrono::seconds(5),
    const std::string& proc_root = "/proc");

// The child side: probes the OpenXR runtime in XR_RUNTIME_JSON and returns
// the probe's exit code (0 when a headset is available).
int RunHeadsetProbeChild();

}  // namespace mocktail::vr

#endif  // MOCKTAIL_VR_HEADSET_DETECTION_H_
