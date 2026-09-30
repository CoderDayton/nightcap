#ifndef MOCKTAIL_WEBVIEW_ROBLOX_CHALLENGE_FEATURES_H_
#define MOCKTAIL_WEBVIEW_ROBLOX_CHALLENGE_FEATURES_H_

#include <string_view>

namespace mocktail::webview {

// Navigation.navigateToFeature features a Roblox verification page sends when
// the user has solved it. The captcha page sends CaptchaSuccess; the generic
// challenge page (challenge/cdn/hybrid) sends challengeCompleted. The WebView
// helper and the runtime both classify page events with this list.
inline bool IsRobloxChallengeSolvedFeature(std::string_view feature) {
  return feature == "CaptchaSuccess" || feature == "challengeCompleted" ||
         feature == "ChallengeCompleted";
}

}  // namespace mocktail::webview

#endif  // MOCKTAIL_WEBVIEW_ROBLOX_CHALLENGE_FEATURES_H_
