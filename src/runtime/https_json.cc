#include "runtime/https_json.h"

#include <curl/curl.h>

#include <cstdlib>
#include <limits>
#include <mutex>

#include "runtime/environment.h"
#include "runtime/runtime_config.h"

namespace mocktail::runtime {
namespace {

constexpr std::size_t kMaximumResponseBytes = 512U * 1024U;

struct BoundedResponse {
  std::string body;
  bool exceeded = false;
};

std::once_flag g_curl_init_once;
CURLcode g_curl_init_status = CURLE_FAILED_INIT;

void InitializeCurl() {
  g_curl_init_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}

size_t WriteHttpResponse(char* data, size_t size, size_t count,
                         void* user_data) {
  auto* response = static_cast<BoundedResponse*>(user_data);
  if (response == nullptr || data == nullptr ||
      (count != 0 && size > std::numeric_limits<size_t>::max() / count)) {
    return 0;
  }
  const std::size_t bytes = size * count;
  if (bytes > kMaximumResponseBytes ||
      response->body.size() > kMaximumResponseBytes - bytes) {
    response->exceeded = true;
    return 0;
  }
  response->body.append(data, bytes);
  return bytes;
}

bool Cancelled(const std::atomic<bool>* cancel) {
  return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

CURLcode Perform(CURL* curl, const std::atomic<bool>* cancel) {
  if (cancel == nullptr) {
    return curl_easy_perform(curl);
  }
  CURLM* multi = curl_multi_init();
  if (multi == nullptr) {
    return CURLE_OUT_OF_MEMORY;
  }
  CURLcode status = CURLE_ABORTED_BY_CALLBACK;
  if (curl_multi_add_handle(multi, curl) == CURLM_OK) {
    int running = 1;
    while (!Cancelled(cancel) &&
           curl_multi_perform(multi, &running) == CURLM_OK && running != 0) {
      curl_multi_poll(multi, nullptr, 0, 50, nullptr);
    }
    int queued = 0;
    while (CURLMsg* message = curl_multi_info_read(multi, &queued)) {
      if (message->msg == CURLMSG_DONE) {
        status = message->data.result;
      }
    }
    curl_multi_remove_handle(multi, curl);
  }
  curl_multi_cleanup(multi);
  return status;
}

}

void EnsureCurlInitialized() {
  std::call_once(g_curl_init_once, InitializeCurl);
}

std::optional<nlohmann::json> GetHttpsJson(const std::string& url,
                                           const std::atomic<bool>* cancel) {
  if (Cancelled(cancel)) {
    return std::nullopt;
  }
  EnsureCurlInitialized();
  if (g_curl_init_status != CURLE_OK) {
    return std::nullopt;
  }
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return std::nullopt;
  }
  BoundedResponse response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3500L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mocktail/0.1");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteHttpResponse);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  std::string proxy_url;
  const auto proxy = ReadNetworkProxy(ProcessEnvironment());
  if (proxy.has_value()) {
    proxy_url = BuildNetworkProxyUrl(*proxy);
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url.c_str());
    if (!proxy->username.empty()) {
      curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, proxy->username.c_str());
      curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, proxy->password.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "");
  }
  if (const char* ca_bundle = std::getenv("MOCKTAIL_CA_BUNDLE")) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
  }
  const CURLcode status = Perform(curl, cancel);
  long http_status = 0;
  if (status == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  }
  curl_easy_cleanup(curl);
  if (status != CURLE_OK || http_status != 200 || response.exceeded) {
    return std::nullopt;
  }
  nlohmann::json parsed = nlohmann::json::parse(response.body, nullptr, false);
  if (parsed.is_discarded()) {
    return std::nullopt;
  }
  return parsed;
}

}
