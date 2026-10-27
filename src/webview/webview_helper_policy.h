#ifndef MOCKTAIL_WEBVIEW_WEBVIEW_HELPER_POLICY_H_
#define MOCKTAIL_WEBVIEW_WEBVIEW_HELPER_POLICY_H_

#include <jsc/jsc.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace mocktail {
namespace webview {

inline constexpr std::size_t kMaximumHybridCommandBytes = 64 * 1024;
inline constexpr char kExecuteRobloxHandler[] = "executeRoblox";
inline constexpr char kRobloxWkHybridHandler[] = "RobloxWKHybrid";
inline constexpr char kCompatibilityHandler[] = "mocktailRobloxBridge";

enum class CaptchaEventType {
  kShown,
  kSuccess,
};

struct CaptchaEvent {
  CaptchaEventType type = CaptchaEventType::kShown;
  std::string callback_id;
};

struct UriPolicyResult {
  bool allowed = false;
  bool privileged_bridge_allowed = false;
  std::string scheme = "invalid";
  std::string host = "none";
};

// An explicit WebKit sandbox setting takes precedence over host defaults.
bool ShouldDisableWebKitSandbox(std::string_view kernel_version,
                                const char* sandbox_override);
// Auth and challenge pages can render black with WebKit's hardware
// compositing, on Wayland and on X11 (seen on Cinnamon), so the helper uses
// software compositing unless WEBKIT_DISABLE_COMPOSITING_MODE is "0".
bool ShouldDisableWebViewHardwareAcceleration(const char* compositing_override);
// GTK's GL renderer aborts in libepoxy when it snapshots a page switch on
// hosts where its GL context cannot become current. With software compositing
// the helper does not need GL for its UI, so use the Cairo renderer unless the
// user chose one through GSK_RENDERER.
bool ShouldForceSoftwareGtkRenderer(bool software_compositing,
                                    const char* renderer_override);
// GDK's EGL setup aborts in libepoxy on some NVIDIA hosts even with the Cairo
// renderer. Returns the GDK_DISABLE value that also turns off GTK's GL, or
// nullopt when it is already off or hardware compositing needs it.
std::optional<std::string> GdkDisableWithoutGl(bool software_compositing,
                                               const char* current);

const char* AndroidBridgeSource();
std::string BuildRobloxAndroidUserAgent();
bool IsBrowserLoginUrl(std::string_view url);
bool IsAndroidStorePurchaseNavigation(const char* uri);
enum class StorePurchaseNavigation {
  kNone,
  kHandOff,
  kIgnore,
};
StorePurchaseNavigation ClassifyStorePurchaseNavigation(const char* current_uri,
                                                        const char* target_uri);
bool IsEssentialWebResource(const char* uri);
std::string BoundedLogToken(const char* value, std::string_view fallback);
UriPolicyResult EvaluateNavigationUri(const char* uri);
const char* CaptchaEventName(CaptchaEventType type);
bool ExtractExecuteRobloxCommand(JSCValue* value, std::string* command);
bool ExtractRobloxWkHybridCommand(JSCValue* value, std::string* command);
bool ParseCaptchaEvent(std::string_view command, CaptchaEvent* event);
std::string BuildCallbackScript(std::string_view callback_id);

}  // namespace webview
}  // namespace mocktail

#endif  // MOCKTAIL_WEBVIEW_WEBVIEW_HELPER_POLICY_H_
