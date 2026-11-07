#ifndef MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_
#define MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {

struct PayloadUpdatePreflightResult {
  bool attempted = false;
  std::string error;
  // Last failure the updater reported on its own stderr. Empty when the
  // updater never ran or never explained itself.
  std::string details;

  explicit operator bool() const { return error.empty(); }
};

PayloadUpdatePreflightResult RunPayloadUpdatePreflight(
    const Environment& environment, const RuntimePaths& paths,
    bool force_run_latest = false);

// Turns the updater's failure into dialog text: its FAQ hint as a sentence
// first, then the underlying error without the updater's internal prefixes.
std::string DescribeUpdateFailure(std::string_view details);

// Roblox's answer to whether an app version is current: "Required",
// "Recommended" or "None". Empty when the reply has no answer.
std::string ReadRobloxUpgradeAction(const nlohmann::json& reply);

// Asks clientsettings.roblox.com, which needs no sign-in. Empty on failure.
std::string QueryRobloxUpgradeAction(const std::string& version_name,
                                     const std::atomic<bool>* cancel);

using UpgradeActionQuery = std::function<std::string(
    const std::string& version_name, const std::atomic<bool>* cancel)>;

class BackgroundPayloadUpdate final {
 public:
  ~BackgroundPayloadUpdate();
  // When `ask_roblox` says the active version needs an update, the first
  // check runs right away instead of after the startup delay.
  void Start(const Environment& environment, const RuntimePaths& paths,
             std::chrono::milliseconds interval = std::chrono::minutes(30),
             UpgradeActionQuery ask_roblox = {});
  void Stop();

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  bool stopped_ = false;
  std::atomic<bool> cancel_{false};
  int child_ = -1;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_
