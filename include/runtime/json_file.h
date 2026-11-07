#ifndef MOCKTAIL_RUNTIME_JSON_FILE_H_
#define MOCKTAIL_RUNTIME_JSON_FILE_H_

#if defined(INCLUDE_NLOHMANN_JSON_HPP_) && !defined(JSON_NOEXCEPTION)
#error "include runtime/json_file.h before nlohmann/json.hpp"
#endif
#define JSON_NOEXCEPTION 1
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

enum class JsonFileStatus { kMissing, kReady, kInvalid };

struct JsonFileReadResult {
  JsonFileStatus status = JsonFileStatus::kMissing;
  nlohmann::json value;
  std::string error;
};

// Reads a JSON object from a regular file of at most 16 MiB without following
// a symlink. `label` names the file in errors.
JsonFileReadResult ReadJsonObjectFile(const std::filesystem::path& path,
                                      std::string_view label);

struct JsonFileWriteNames {
  std::string_view label;
  std::string_view target;
  std::string_view temporary_prefix;
};

// Atomic, fsynced 0600 replace. Refuses to replace a symlink.
bool WriteJsonFileAtomically(const std::filesystem::path& path,
                             const nlohmann::json& value,
                             const JsonFileWriteNames& names,
                             std::string* error);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_JSON_FILE_H_
