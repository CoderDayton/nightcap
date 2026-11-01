#include "runtime/roblox_cookie_jar.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iterator>

#include <nlohmann/json.hpp>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kSite = "roblox.com";
constexpr std::string_view kCredentialName = ".ROBLOSECURITY";
// Browser limits: 180 cookies per site, 4096 bytes per `name=value`.
constexpr std::size_t kMaximumCookies = 180;
constexpr std::size_t kMaximumCookieBytes = 4096;

struct ParsedUrl {
  std::string scheme;
  std::string host;
  std::string path;
};

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

ParsedUrl ParseUrl(std::string_view url) {
  ParsedUrl parsed;
  const std::size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) {
    return parsed;
  }
  parsed.scheme = Lower(url.substr(0, scheme_end));
  std::string_view rest = url.substr(scheme_end + 3);
  const std::size_t host_end = rest.find_first_of("/?#");
  std::string_view host = rest.substr(0, host_end);
  const std::size_t at = host.rfind('@');
  if (at != std::string_view::npos) {
    host.remove_prefix(at + 1);
  }
  const std::size_t port = host.find(':');
  parsed.host = Lower(host.substr(0, port));
  if (host_end == std::string_view::npos || rest[host_end] != '/') {
    parsed.path = "/";
    return parsed;
  }
  std::string_view path = rest.substr(host_end);
  path = path.substr(0, path.find_first_of("?#"));
  parsed.path = std::string(path);
  return parsed;
}

bool DomainMatches(std::string_view host, std::string_view domain) {
  if (host.empty() || domain.empty()) {
    return false;
  }
  if (host == domain) {
    return true;
  }
  return host.size() > domain.size() &&
         host.substr(host.size() - domain.size()) == domain &&
         host[host.size() - domain.size() - 1] == '.';
}

std::string DefaultPath(std::string_view request_path) {
  if (request_path.empty() || request_path.front() != '/') {
    return "/";
  }
  const std::size_t last = request_path.rfind('/');
  return last == 0 ? "/" : std::string(request_path.substr(0, last));
}

bool ValidName(std::string_view name) {
  if (name.empty() || name == kCredentialName) {
    return false;
  }
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte <= 0x20 || byte >= 0x7f || c == '=' || c == ';' || c == ',') {
      return false;
    }
  }
  return true;
}

bool ValidValue(std::string_view value) {
  for (const char c : value) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f || c == ';') {
      return false;
    }
  }
  return true;
}

int MonthIndex(std::string_view token) {
  static constexpr std::string_view kMonths[] = {
      "jan", "feb", "mar", "apr", "may", "jun",
      "jul", "aug", "sep", "oct", "nov", "dec"};
  if (token.size() < 3) {
    return -1;
  }
  const std::string prefix = Lower(token.substr(0, 3));
  for (int index = 0; index < 12; ++index) {
    if (prefix == kMonths[index]) {
      return index;
    }
  }
  return -1;
}

bool AllDigits(std::string_view token) {
  return !token.empty() &&
         std::all_of(token.begin(), token.end(), [](char c) {
           return std::isdigit(static_cast<unsigned char>(c)) != 0;
         });
}

// Accepts the RFC 1123 and dashed Netscape forms of a cookie date.
bool ParseCookieDate(std::string_view text, std::int64_t* out) {
  int day = -1, month = -1, year = -1, hour = -1, minute = -1, second = -1;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find_first_of(" ,-", start);
    const std::string_view token =
        text.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                         : end - start);
    if (!token.empty()) {
      if (hour < 0 && token.find(':') != std::string_view::npos) {
        if (token.size() == 8 && token[2] == ':' && token[5] == ':' &&
            AllDigits(token.substr(0, 2)) && AllDigits(token.substr(3, 2)) &&
            AllDigits(token.substr(6, 2))) {
          hour = std::stoi(std::string(token.substr(0, 2)));
          minute = std::stoi(std::string(token.substr(3, 2)));
          second = std::stoi(std::string(token.substr(6, 2)));
        }
      } else if (AllDigits(token) && token.size() <= 2 && day < 0) {
        day = std::stoi(std::string(token));
      } else if (AllDigits(token) && year < 0 &&
                 (token.size() == 2 || token.size() == 4)) {
        year = std::stoi(std::string(token));
        if (token.size() == 2) {
          year += year < 70 ? 2000 : 1900;
        }
      } else if (month < 0 && MonthIndex(token) >= 0) {
        month = MonthIndex(token);
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  if (day < 1 || day > 31 || month < 0 || year < 1970 || hour < 0 ||
      hour > 23 || minute > 59 || second > 60) {
    return false;
  }
  std::tm parts{};
  parts.tm_year = year - 1900;
  parts.tm_mon = month;
  parts.tm_mday = day;
  parts.tm_hour = hour;
  parts.tm_min = minute;
  parts.tm_sec = second;
  *out = static_cast<std::int64_t>(timegm(&parts));
  return true;
}

bool Expired(const RobloxCookieJar::Cookie& cookie, std::int64_t now) {
  return cookie.expires != 0 && cookie.expires <= now;
}

bool SameCookie(const RobloxCookieJar::Cookie& left,
                const RobloxCookieJar::Cookie& right) {
  return left.name == right.name && left.domain == right.domain &&
         left.path == right.path;
}

bool SameContents(const RobloxCookieJar::Cookie& left,
                  const RobloxCookieJar::Cookie& right) {
  return SameCookie(left, right) && left.value == right.value &&
         left.host_only == right.host_only && left.secure == right.secure &&
         left.expires == right.expires;
}

bool StorableCookie(const RobloxCookieJar::Cookie& cookie) {
  return ValidName(cookie.name) && ValidValue(cookie.value) &&
         cookie.name.size() + 1 + cookie.value.size() <= kMaximumCookieBytes &&
         DomainMatches(cookie.domain, kSite) && !cookie.path.empty() &&
         cookie.path.front() == '/';
}

// Reads an optional field; false when it is present with the wrong type.
bool ReadString(const nlohmann::json& entry, const char* key, std::string* out) {
  const auto field = entry.find(key);
  if (field == entry.end()) {
    return true;
  }
  if (!field->is_string()) {
    return false;
  }
  *out = field->get<std::string>();
  return true;
}

bool ReadBool(const nlohmann::json& entry, const char* key, bool* out) {
  const auto field = entry.find(key);
  if (field == entry.end()) {
    return true;
  }
  if (!field->is_boolean()) {
    return false;
  }
  *out = field->get<bool>();
  return true;
}

bool ReadTime(const nlohmann::json& entry, const char* key, std::int64_t* out) {
  const auto field = entry.find(key);
  if (field == entry.end()) {
    return true;
  }
  if (field->is_number_unsigned()) {
    const auto value = field->get<std::uint64_t>();
    if (value > static_cast<std::uint64_t>(INT64_MAX)) {
      return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
  }
  if (!field->is_number_integer()) {
    return false;
  }
  *out = field->get<std::int64_t>();
  return *out >= 0;
}

void Wipe(std::string* value) {
  volatile char* bytes = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    bytes[index] = '\0';
  }
  value->clear();
}

}  // namespace

RobloxCookieJar::~RobloxCookieJar() {
  for (Cookie& cookie : cookies_) {
    Wipe(&cookie.value);
  }
}

RobloxCookieJar RobloxCookieJar::Parse(std::string_view json) {
  RobloxCookieJar jar;
  const nlohmann::json document = nlohmann::json::parse(json, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return jar;
  }
  const auto list = document.find("cookies");
  if (list == document.end() || !list->is_array()) {
    return jar;
  }
  for (const nlohmann::json& entry : *list) {
    if (!entry.is_object() || jar.cookies_.size() >= kMaximumCookies) {
      continue;
    }
    Cookie cookie;
    const bool typed = ReadString(entry, "name", &cookie.name) &&
                       ReadString(entry, "value", &cookie.value) &&
                       ReadString(entry, "domain", &cookie.domain) &&
                       ReadString(entry, "path", &cookie.path) &&
                       ReadBool(entry, "host_only", &cookie.host_only) &&
                       ReadBool(entry, "secure", &cookie.secure) &&
                       ReadTime(entry, "expires", &cookie.expires);
    cookie.domain = Lower(cookie.domain);
    if (!typed || !StorableCookie(cookie)) {
      Wipe(&cookie.value);
      continue;
    }
    jar.cookies_.push_back(std::move(cookie));
  }
  return jar;
}

std::string RobloxCookieJar::Serialize() const {
  nlohmann::json list = nlohmann::json::array();
  for (const Cookie& cookie : cookies_) {
    list.push_back({{"name", cookie.name},
                    {"value", cookie.value},
                    {"domain", cookie.domain},
                    {"path", cookie.path},
                    {"host_only", cookie.host_only},
                    {"secure", cookie.secure},
                    {"expires", cookie.expires}});
  }
  return nlohmann::json{{"version", 1}, {"cookies", std::move(list)}}.dump();
}

bool RobloxCookieJar::Apply(std::string_view url,
                            const std::vector<std::string>& set_cookies,
                            std::int64_t now) {
  const ParsedUrl origin = ParseUrl(url);
  if (origin.scheme != "https" || !DomainMatches(origin.host, kSite)) {
    return false;
  }
  bool changed = false;
  for (const std::string& header : set_cookies) {
    std::string_view rest(header);
    const std::size_t first_end = rest.find(';');
    const std::string_view pair = Trim(rest.substr(0, first_end));
    const std::size_t equals = pair.find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    Cookie cookie;
    cookie.name = std::string(Trim(pair.substr(0, equals)));
    cookie.value = std::string(Trim(pair.substr(equals + 1)));

    std::string domain;
    std::string path;
    bool has_max_age = false;
    std::int64_t max_age = 0;
    bool has_expires = false;
    std::int64_t expires = 0;
    std::size_t start =
        first_end == std::string_view::npos ? rest.size() : first_end + 1;
    while (start < rest.size()) {
      const std::size_t end = rest.find(';', start);
      const std::string_view attribute = Trim(rest.substr(
          start, end == std::string_view::npos ? std::string_view::npos
                                               : end - start));
      const std::size_t attribute_equals = attribute.find('=');
      const std::string key = Lower(Trim(attribute.substr(0, attribute_equals)));
      const std::string_view value =
          attribute_equals == std::string_view::npos
              ? std::string_view()
              : Trim(attribute.substr(attribute_equals + 1));
      if (key == "domain") {
        domain = Lower(value);
        while (!domain.empty() && domain.front() == '.') {
          domain.erase(domain.begin());
        }
      } else if (key == "path") {
        path = std::string(value);
      } else if (key == "secure") {
        cookie.secure = true;
      } else if (key == "max-age") {
        const std::string digits(value.substr(value.empty() || value[0] != '-' ? 0 : 1));
        if (AllDigits(digits) && digits.size() <= 12) {
          has_max_age = true;
          max_age = std::stoll(digits);
          if (!value.empty() && value[0] == '-') {
            max_age = -max_age;
          }
        }
      } else if (key == "expires") {
        has_expires = ParseCookieDate(value, &expires);
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }

    if (domain.empty()) {
      cookie.host_only = true;
      cookie.domain = origin.host;
    } else {
      cookie.domain = domain;
    }
    if (!DomainMatches(origin.host, cookie.domain)) {
      Wipe(&cookie.value);
      continue;
    }
    cookie.path = path.empty() || path.front() != '/'
                      ? DefaultPath(origin.path)
                      : path;
    if (!StorableCookie(cookie)) {
      Wipe(&cookie.value);
      continue;
    }

    bool remove = false;
    if (has_max_age) {
      remove = max_age <= 0;
      cookie.expires = remove ? 0 : now + max_age;
    } else if (has_expires) {
      remove = expires <= now;
      cookie.expires = expires;
    }

    const auto existing =
        std::find_if(cookies_.begin(), cookies_.end(),
                     [&](const Cookie& stored) { return SameCookie(stored, cookie); });
    if (remove) {
      if (existing != cookies_.end()) {
        Wipe(&existing->value);
        cookies_.erase(existing);
        changed = true;
      }
      Wipe(&cookie.value);
      continue;
    }
    if (existing != cookies_.end()) {
      // Keep the vector in set order: the newest cookie is last.
      if (SameContents(*existing, cookie) &&
          std::next(existing) == cookies_.end()) {
        Wipe(&cookie.value);
        continue;
      }
      Wipe(&existing->value);
      cookies_.erase(existing);
    } else if (cookies_.size() >= kMaximumCookies) {
      Wipe(&cookies_.front().value);
      cookies_.erase(cookies_.begin());
    }
    cookies_.push_back(std::move(cookie));
    changed = true;
  }

  const auto stale = std::remove_if(cookies_.begin(), cookies_.end(),
                                    [&](Cookie& cookie) {
                                      if (!Expired(cookie, now)) {
                                        return false;
                                      }
                                      Wipe(&cookie.value);
                                      return true;
                                    });
  if (stale != cookies_.end()) {
    cookies_.erase(stale, cookies_.end());
    changed = true;
  }
  return changed;
}

RobloxCookieJar& RobloxCookieJar::operator=(RobloxCookieJar&& other) noexcept {
  if (this != &other) {
    for (Cookie& cookie : cookies_) {
      Wipe(&cookie.value);
    }
    cookies_ = std::move(other.cookies_);
    other.cookies_.clear();
  }
  return *this;
}

std::string RobloxCookieJar::StartupHeader(std::int64_t now) const {
  std::vector<const Cookie*> picked;
  for (auto cookie = cookies_.rbegin(); cookie != cookies_.rend(); ++cookie) {
    if (Expired(*cookie, now)) {
      continue;
    }
    const bool seen = std::any_of(
        picked.begin(), picked.end(),
        [&](const Cookie* newer) { return newer->name == cookie->name; });
    if (!seen) {
      picked.push_back(&*cookie);
    }
  }
  std::string header;
  for (auto cookie = picked.rbegin(); cookie != picked.rend(); ++cookie) {
    if (!header.empty()) {
      header += "; ";
    }
    header += (*cookie)->name;
    header += '=';
    header += (*cookie)->value;
  }
  return header;
}

std::string CombineRobloxStartupCookies(std::string_view credential,
                                        std::string_view jar_header) {
  std::string combined(credential);
  if (!combined.empty() && !jar_header.empty()) {
    combined += "; ";
  }
  combined += jar_header;
  return combined;
}

}
}
