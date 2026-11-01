#include "runtime/roblox_app_storage.h"

#include <fstream>
#include <iterator>
#include <thread>

#include <nlohmann/json.hpp>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::uintmax_t kMaximumAppStorageBytes = 16u * 1024u * 1024u;

}

std::string ReadRobloxAccountBlob(const std::filesystem::path& app_storage) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(app_storage, error);
  if (error || size > kMaximumAppStorageBytes) {
    return {};
  }
  std::ifstream input(app_storage, std::ios::binary);
  if (!input) {
    return {};
  }
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  const nlohmann::json document =
      nlohmann::json::parse(text, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return {};
  }
  const auto found = document.find("AccountBlob");
  if (found == document.end() || !found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

SavedAccountBlob WaitForSavedRobloxAccountBlob(
    const std::filesystem::path& app_storage,
    std::filesystem::file_time_type since, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    std::error_code error;
    const auto written = std::filesystem::last_write_time(app_storage, error);
    SavedAccountBlob result{ReadRobloxAccountBlob(app_storage), false};
    result.saved = !error && written >= since && !result.blob.empty();
    if (result.saved || std::chrono::steady_clock::now() >= deadline) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

}
}
