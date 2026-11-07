#ifndef MOCKTAIL_RUNTIME_ROBLOX_HAPTIC_PROTOCOL_BRIDGE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_HAPTIC_PROTOCOL_BRIDGE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "runtime/roblox_message_bus_request_bridge.h"

namespace mocktail {
namespace runtime {

using HapticSinkFn = std::function<void(int hand, float amplitude,
                                        uint64_t duration_ns, float frequency_hz)>;
using HapticStopFn = std::function<void(int hand)>;
using HapticStateSyncFn = std::function<void(JNIEnv* env)>;

class HapticWaveformPlayer;

class RobloxHapticProtocolBridge final {
 public:
  RobloxHapticProtocolBridge(
      JniEnvironmentProvider environment,
      RobloxMessageBusSymbols symbols,
      RobloxMessageBusObjects objects,
      HapticSinkFn haptic_sink = nullptr,
      HapticStopFn haptic_stop = nullptr,
      HapticStateSyncFn haptic_state_sync = nullptr);
  ~RobloxHapticProtocolBridge();

  Status Initialize();
  Status Shutdown();

  static std::string GetMotorsJson();
  // True during a VR session; main() sets MOCKTAIL_VR_ENABLED to 0 when VR
  // cannot start, so the bridge stays off for desktop play.
  static bool EnabledForThisSession();

 private:
  std::shared_ptr<HapticWaveformPlayer> player_;
  std::unique_ptr<RobloxMessageBusRequestBridge> bridge_;
};

}  // namespace runtime
}  // namespace mocktail
#endif
