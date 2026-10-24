#ifndef MOCKTAIL_RUNTIME_ROBLOX_STORE_PURCHASE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_STORE_PURCHASE_H_

#include <jni.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mocktail {
namespace runtime {

inline constexpr char kWebRobuxPurchaseUrl[] =
    "https://www.roblox.com/upgrades/robux";

struct RobloxStorePurchase {
  jlong player_id = 0;
  std::string product_id;
};

class RobloxStorePurchaseQueue {
 public:
  void Push(jlong player_id, std::string product_id);
  std::vector<RobloxStorePurchase> Take();

 private:
  std::mutex mutex_;
  std::vector<RobloxStorePurchase> purchases_;
};

class RepeatGuard {
 public:
  explicit RepeatGuard(std::chrono::milliseconds window) : window_(window) {}
  bool Allow(std::chrono::steady_clock::time_point now);

 private:
  std::mutex mutex_;
  std::chrono::milliseconds window_;
  std::optional<std::chrono::steady_clock::time_point> last_;
};

using RobloxInGamePurchaseFinishedFn = void (*)(JNIEnv* env, jclass clazz,
                                                jboolean success,
                                                jlong player_id,
                                                jstring product_id,
                                                jstring checkout_session_id);
using RobloxPurchaseHandOff = void (*)();

void FinishStorePurchasesInBrowser(
    const std::vector<RobloxStorePurchase>& purchases, JNIEnv* env,
    jclass native_gl_interface, RobloxInGamePurchaseFinishedFn finished,
    RobloxPurchaseHandOff hand_off);

void HandOffRobuxPurchaseToBrowser();

struct EnvironmentChange {
  std::string name;
  std::optional<std::string> value;
};

std::vector<EnvironmentChange> BrowserEnvironmentChanges(
    const char* (*get)(const char* name));

bool OpenUrlInDefaultBrowser(const char* url);

}
}

#endif
