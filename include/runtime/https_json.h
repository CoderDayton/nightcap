#ifndef MOCKTAIL_RUNTIME_HTTPS_JSON_H_
#define MOCKTAIL_RUNTIME_HTTPS_JSON_H_

#include <atomic>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace mocktail::runtime {

void EnsureCurlInitialized();
std::optional<nlohmann::json> GetHttpsJson(
    const std::string& url, const std::atomic<bool>* cancel = nullptr);

}

#endif
