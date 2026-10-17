#include "services/cursor_picture_source.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace mocktail {
namespace services {
namespace {

constexpr std::string_view kAssetDeliveryUrl =
    "https://assetdelivery.roblox.com/v1/asset/?id=";
constexpr std::string_view kCookiePrefix = ".ROBLOSECURITY=";
// Cursor pictures are small; anything larger is not one.
constexpr std::size_t kMaxPictureBytes = 4 * 1024 * 1024;
constexpr long kTimeoutMs = 15000;

bool ConsumePrefix(std::string_view* text, std::string_view prefix) {
  if (text->substr(0, prefix.size()) != prefix) {
    return false;
  }
  text->remove_prefix(prefix.size());
  return true;
}

bool EqualsIgnoringCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size()) == suffix;
}

std::optional<std::uint64_t> ParseId(std::string_view digits) {
  std::uint64_t id = 0;
  const auto [end, error] =
      std::from_chars(digits.data(), digits.data() + digits.size(), id);
  if (error != std::errc() || end == digits.data() || id == 0) {
    return std::nullopt;
  }
  return id;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) ||
      std::filesystem::file_size(path, error) > kMaxPictureBytes || error) {
    return {};
  }
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

// The Cookie header for the asset service, or empty without a usable cookie.
std::string CookieHeader(const std::filesystem::path& cookie_file) {
  std::string cookie = ReadFile(cookie_file);
  while (!cookie.empty() &&
         std::isspace(static_cast<unsigned char>(cookie.back()))) {
    cookie.pop_back();
  }
  const bool usable =
      cookie.size() > kCookiePrefix.size() &&
      cookie.compare(0, kCookiePrefix.size(), kCookiePrefix) == 0 &&
      std::none_of(cookie.begin(), cookie.end(), [](char ch) {
        return ch == '\r' || ch == '\n' || ch == ';';
      });
  return usable ? "Cookie: " + cookie : std::string();
}

std::string RedirectLocation(const HttpResponse& response) {
  for (const std::string& header : response.headers) {
    const std::size_t colon = header.find(':');
    if (colon == std::string::npos ||
        !EqualsIgnoringCase(std::string_view(header).substr(0, colon),
                            "location")) {
      continue;
    }
    std::string_view value = std::string_view(header).substr(colon + 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value[0]))) {
      value.remove_prefix(1);
    }
    return std::string(value);
  }
  return {};
}

// Only Roblox's CDN may serve the picture the asset service points at.
bool IsRobloxCdn(std::string_view url) {
  if (!ConsumePrefix(&url, "https://")) {
    return false;
  }
  const std::string_view host = url.substr(0, url.find_first_of("/?#:"));
  return EndsWith(host, ".rbxcdn.com");
}

}  // namespace

std::string LocalCursorAssetPath(std::string_view content) {
  if (!ConsumePrefix(&content, "rbxasset://") &&
      !ConsumePrefix(&content, "!bxasset://")) {
    return {};
  }
  // Roblox reads textures//DragCursor.png as textures/DragCursor.png.
  std::string path;
  for (const char ch : content) {
    if (ch == '?' || ch == '#') {
      break;
    }
    if (ch == '/' && (path.empty() || path.back() == '/')) {
      continue;
    }
    path.push_back(ch);
  }
  const std::string_view view = path;
  if (path.empty() || view == ".." || view.substr(0, 3) == "../" ||
      view.find("/../") != std::string_view::npos || EndsWith(view, "/..") ||
      view.find('\\') != std::string_view::npos) {
    return {};
  }
  return path;
}

std::optional<std::uint64_t> CursorAssetId(std::string_view content) {
  std::string_view rest = content;
  if (ConsumePrefix(&rest, "rbxassetid://")) {
    return ParseId(rest.substr(0, rest.find_first_not_of("0123456789")));
  }
  if (!ConsumePrefix(&rest, "https://") && !ConsumePrefix(&rest, "http://")) {
    return std::nullopt;
  }
  const std::size_t host_end = rest.find_first_of("/?");
  const std::string_view host = rest.substr(0, host_end);
  if (host != "www.roblox.com" && host != "roblox.com" &&
      host != "assetdelivery.roblox.com") {
    return std::nullopt;
  }
  const std::size_t query = rest.find('?');
  if (query == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view parameters = rest.substr(query + 1);
  while (!parameters.empty()) {
    const std::string_view parameter =
        parameters.substr(0, parameters.find('&'));
    std::string_view value = parameter;
    if (ConsumePrefix(&value, "id=") || ConsumePrefix(&value, "ID=")) {
      return ParseId(value);
    }
    if (parameter.size() == parameters.size()) {
      break;
    }
    parameters.remove_prefix(parameter.size() + 1);
  }
  return std::nullopt;
}

std::vector<std::filesystem::path> LocalCursorPictureCandidates(
    std::string_view relative_path, const std::filesystem::path& overlay_dir,
    const std::filesystem::path& asset_root) {
  std::vector<std::filesystem::path> candidates;
  if (relative_path.empty()) {
    return candidates;
  }
  for (const std::filesystem::path* root : {&overlay_dir, &asset_root}) {
    if (root->empty()) {
      continue;
    }
    candidates.push_back(*root / "content" / relative_path);
    candidates.push_back(*root / relative_path);
  }
  return candidates;
}

CursorPictureSource::CursorPictureSource(
    HttpClient* http, std::filesystem::path cookie_file,
    std::filesystem::path overlay_dir,
    std::function<std::filesystem::path()> asset_root)
    : http_(http),
      cookie_file_(std::move(cookie_file)),
      overlay_dir_(std::move(overlay_dir)),
      asset_root_(std::move(asset_root)) {}

std::string CursorPictureSource::Fetch(const std::string& content) {
  const std::string local = LocalCursorAssetPath(content);
  if (!local.empty()) {
    const std::filesystem::path root =
        asset_root_ ? asset_root_() : std::filesystem::path();
    for (const std::filesystem::path& candidate :
         LocalCursorPictureCandidates(local, overlay_dir_, root)) {
      std::string bytes = ReadFile(candidate);
      if (!bytes.empty()) {
        return bytes;
      }
    }
    return {};
  }
  const std::optional<std::uint64_t> asset_id = CursorAssetId(content);
  return asset_id.has_value() ? Download(*asset_id) : std::string();
}

std::string CursorPictureSource::Download(std::uint64_t asset_id) {
  if (http_ == nullptr) {
    return {};
  }
  HttpRequest request;
  request.url = std::string(kAssetDeliveryUrl) + std::to_string(asset_id);
  request.timeout_ms = kTimeoutMs;
  request.maximum_body_bytes = kMaxPictureBytes;
  request.follow_redirects = false;
  std::string cookie = CookieHeader(cookie_file_);
  if (!cookie.empty()) {
    request.headers.push_back(std::move(cookie));
  }
  HttpResponse response = http_->Get(request);
  if (!response.transport_ok) {
    return {};
  }
  if (response.status_code == 200) {
    return std::move(response.body);
  }
  if (response.status_code < 300 || response.status_code >= 400) {
    return {};
  }
  const std::string location = RedirectLocation(response);
  if (!IsRobloxCdn(location)) {
    return {};
  }
  HttpRequest cdn;
  cdn.url = location;
  cdn.timeout_ms = kTimeoutMs;
  cdn.maximum_body_bytes = kMaxPictureBytes;
  cdn.follow_redirects = false;
  const HttpResponse picture = http_->Get(cdn);
  return picture.transport_ok && picture.status_code == 200 ? picture.body
                                                            : std::string();
}

}  // namespace services
}  // namespace mocktail
