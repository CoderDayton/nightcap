#ifndef MOCKTAIL_RUNTIME_ROBLOX_ACCOUNT_BRIDGE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_ACCOUNT_BRIDGE_H_

#include <jni.h>

#include <memory>
#include <mutex>
#include <string>

#include "mocktail/status.h"
#include "runtime/roblox_game_session_native_adapter.h"

namespace mocktail {
namespace runtime {

// Exact JNI entrypoints behind the APK AccountProtocol device-integrity
// handlers: JNIAccountProtocol name getters and MessageBus raw handlers.
struct RobloxAccountProtocolSymbols {
  using GetStringFn = jstring (*)(JNIEnv*, jclass);

  GetStringFn get_protocol_name = nullptr;
  GetStringFn get_device_integrity_available_method_name = nullptr;
  GetStringFn get_integrity_token_method_name = nullptr;
  GetStringFn get_support_key = nullptr;
  GetStringFn get_token_key = nullptr;
  GetStringFn get_result_key = nullptr;
  void (*set_request_handler_raw)(JNIEnv*, jobject, jstring, jstring,
                                  jobject) = nullptr;
  void (*clear_request_handler)(JNIEnv*, jobject, jstring, jstring) = nullptr;

  bool complete() const {
    return get_protocol_name && get_device_integrity_available_method_name &&
           get_integrity_token_method_name && get_support_key &&
           get_token_key && get_result_key && set_request_handler_raw &&
           clear_request_handler;
  }
};

struct RobloxAccountProtocolObjects {
  jobject message_bus = nullptr;
  void* context = nullptr;
  jobject (*create_request_handler)(void*, std::shared_ptr<void>,
                                    std::string (*)(void*, JNIEnv*,
                                                    jstring)) = nullptr;
  void (*clear_request_handler)(void*, jobject) = nullptr;

  bool complete() const {
    return message_bus && context && create_request_handler &&
           clear_request_handler;
  }
};

// Answers AccountProtocol device-integrity requests for a host that cannot
// produce Play Integrity tokens. No integrity token is ever produced.
//
// DeviceIntegrityAvailable reports no support. The APK handler always reports
// support, so this answer differs from Android: it tells Roblox's challenge
// flow up front that no token can come, instead of letting it request one.
//
// GetIntegrityToken returns at once with an empty token and
// TOKEN_PROVIDER_UNINITIALIZED, the APK's answer when its Play token provider
// never initialized. The APK answers INVALID_NONCE for a request without a
// requestHash; this bridge never inspects the request, so it cannot.
// LuaApp bounds each token request with its
// DeviceIntegrityNativeTimeoutMilliseconds flag; an immediate answer keeps a
// login challenge from sitting out that bound.
class RobloxAccountBridge final {
 public:
  RobloxAccountBridge(JniEnvironmentProvider environment,
                      RobloxAccountProtocolSymbols symbols,
                      RobloxAccountProtocolObjects objects);
  ~RobloxAccountBridge();
  RobloxAccountBridge(const RobloxAccountBridge&) = delete;
  RobloxAccountBridge& operator=(const RobloxAccountBridge&) = delete;

  Status Initialize();
  Status Shutdown();

 private:
  struct State;
  Status ShutdownLocked();

  const JniEnvironmentProvider environment_;
  const RobloxAccountProtocolSymbols symbols_;
  const RobloxAccountProtocolObjects objects_;
  // Serializes lifecycle only. Native dispatch never holds this mutex.
  std::mutex lifecycle_mutex_;
  std::shared_ptr<State> state_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_ACCOUNT_BRIDGE_H_
