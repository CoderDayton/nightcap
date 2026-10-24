#include "runtime/roblox_store_purchase.h"

#include <gio/gio.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <utility>

#include "runtime/desktop_notification.h"

namespace mocktail {
namespace runtime {

void RobloxStorePurchaseQueue::Push(jlong player_id, std::string product_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  purchases_.push_back({player_id, std::move(product_id)});
}

std::vector<RobloxStorePurchase> RobloxStorePurchaseQueue::Take() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RobloxStorePurchase> taken;
  taken.swap(purchases_);
  return taken;
}

bool RepeatGuard::Allow(std::chrono::steady_clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (last_.has_value() && now - *last_ < window_) {
    return false;
  }
  last_ = now;
  return true;
}

void FinishStorePurchasesInBrowser(
    const std::vector<RobloxStorePurchase>& purchases, JNIEnv* env,
    jclass native_gl_interface, RobloxInGamePurchaseFinishedFn finished,
    RobloxPurchaseHandOff hand_off) {
  if (purchases.empty()) {
    return;
  }
  if (hand_off != nullptr) {
    hand_off();
  }
  if (finished == nullptr || env == nullptr) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      std::cerr << "  [store] Roblox has no purchase result call; the game's "
                   "prompt is not told the purchase ended\n";
    }
    return;
  }
  for (const RobloxStorePurchase& purchase : purchases) {
    jstring product_id = env->NewStringUTF(purchase.product_id.c_str());
    jstring checkout_session_id = env->NewStringUTF("");
    finished(env, native_gl_interface, JNI_FALSE, purchase.player_id,
             product_id, checkout_session_id);
    env->DeleteLocalRef(checkout_session_id);
    env->DeleteLocalRef(product_id);
  }
}

void HandOffRobuxPurchaseToBrowser() {
  static RepeatGuard guard(std::chrono::milliseconds(2000));
  if (!guard.Allow(std::chrono::steady_clock::now())) {
    std::cerr << "  [store] repeated Robux purchase ignored\n";
    return;
  }
  std::thread([] {
    const bool opened = OpenUrlInDefaultBrowser(kWebRobuxPurchaseUrl);
    std::cerr << (opened ? "  [store] opened the Robux page in the browser\n"
                         : "  [store] could not open the Robux page in the "
                           "browser\n");
    NotifyWebRobuxPurchase(opened);
  }).detach();
}

std::vector<EnvironmentChange> BrowserEnvironmentChanges(
    const char* (*get)(const char* name)) {
  std::vector<EnvironmentChange> changes;
  const char* saved = get("MOCKTAIL_HOST_ENV");
  if (saved == nullptr || std::strcmp(saved, "1") != 0) {
    return changes;
  }
  const char* path = get("MOCKTAIL_HOST_PATH");
  changes.push_back({"PATH", path != nullptr
                                 ? std::optional<std::string>(path)
                                 : std::nullopt});
  const char* library_path = get("MOCKTAIL_HOST_LD_LIBRARY_PATH");
  changes.push_back(
      {"LD_LIBRARY_PATH", library_path != nullptr && *library_path != '\0'
                              ? std::optional<std::string>(library_path)
                              : std::nullopt});
  return changes;
}

bool OpenUrlInDefaultBrowser(const char* url) {
  GAppLaunchContext* context = g_app_launch_context_new();
  for (const EnvironmentChange& change : BrowserEnvironmentChanges(
           [](const char* name) -> const char* { return std::getenv(name); })) {
    if (change.value.has_value()) {
      g_app_launch_context_setenv(context, change.name.c_str(),
                                  change.value->c_str());
    } else {
      g_app_launch_context_unsetenv(context, change.name.c_str());
    }
  }
  GError* error = nullptr;
  const bool opened =
      g_app_info_launch_default_for_uri(url, context, &error) != FALSE;
  if (!opened) {
    std::cerr << "  [store] browser launch failed: "
              << (error != nullptr ? error->message : "unknown error") << '\n';
    g_clear_error(&error);
  }
  g_object_unref(context);
  return opened;
}

}
}
