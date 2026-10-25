#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace mocktail {
namespace runtime {

// Empty when the file or key is missing or unreadable.
std::string ReadRobloxAccountBlob(const std::filesystem::path& app_storage);

struct SavedAccountBlob {
  std::string blob;
  bool saved = false;
};

// Polls until the file holds a non-empty AccountBlob and was written at or
// after `since`, or the timeout passes.
SavedAccountBlob WaitForSavedRobloxAccountBlob(
    const std::filesystem::path& app_storage,
    std::filesystem::file_time_type since, std::chrono::milliseconds timeout);

}
}
