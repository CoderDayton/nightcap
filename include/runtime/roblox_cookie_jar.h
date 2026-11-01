#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace runtime {

// Cookies the engine sets for roblox.com, kept across restarts the way the
// Android app's CookieManager keeps them. `.ROBLOSECURITY` is never stored
// here; the managed credential file owns it.
class RobloxCookieJar {
 public:
  struct Cookie {
    std::string name;
    std::string value;
    std::string domain;
    std::string path;
    bool host_only = false;
    bool secure = false;
    // Unix seconds; 0 keeps the cookie until it is replaced or deleted.
    std::int64_t expires = 0;
  };

  RobloxCookieJar() = default;
  RobloxCookieJar(const RobloxCookieJar&) = delete;
  RobloxCookieJar& operator=(const RobloxCookieJar&) = delete;
  RobloxCookieJar(RobloxCookieJar&&) noexcept = default;
  RobloxCookieJar& operator=(RobloxCookieJar&& other) noexcept;
  ~RobloxCookieJar();

  static RobloxCookieJar Parse(std::string_view json);
  std::string Serialize() const;

  // Applies `Set-Cookie` values received from `url`. Returns true when the
  // stored set changed.
  bool Apply(std::string_view url, const std::vector<std::string>& set_cookies,
             std::int64_t now);

  // Live cookies as one `Cookie` header for nativeSetMultipleCookies, which
  // takes a flat list. Host and path scope are dropped; for a repeated name
  // the most recently set cookie wins.
  std::string StartupHeader(std::int64_t now) const;

  const std::vector<Cookie>& cookies() const { return cookies_; }

 private:
  std::vector<Cookie> cookies_;
};

std::string CombineRobloxStartupCookies(std::string_view credential,
                                        std::string_view jar_header);

}
}
