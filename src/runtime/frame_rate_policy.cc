#include "runtime/frame_rate_policy.h"

#define JSON_NOEXCEPTION 1
#include <nlohmann/json.hpp>

#include <charconv>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace mocktail {
namespace runtime {
namespace {

bool SetCompatibleValue(nlohmann::json *object, const char *key,
                        const std::string &value, std::string *error) {
  const auto existing = object->find(key);
  if (existing != object->end()) {
    if (!existing->is_string() || existing->get<std::string>() != value) {
      if (error != nullptr) {
        *error = std::string("frame-rate policy conflicts with ") + key;
      }
      return false;
    }
    return true;
  }
  (*object)[key] = value;
  return true;
}

} // namespace

FrameRatePolicy ParseFrameRatePolicy(std::string_view value) {
  if (value.empty() || value == "-1") {
    return {};
  }
  if (value == "display") {
    return {FrameRateLimitMode::kDisplay, 0};
  }
  if (value == "unlimited") {
    return {FrameRateLimitMode::kUnlimited, 0};
  }
  int fps = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), fps);
  if (parsed.ec == std::errc() &&
      parsed.ptr == value.data() + value.size() && fps > 0) {
    return {FrameRateLimitMode::kFixed, fps};
  }
  return {FrameRateLimitMode::kInvalid, 0};
}

bool MergeFrameRateClientSettingsOverrides(const FrameRatePolicy &policy,
                                           std::string_view base_json,
                                           std::string *merged_json,
                                           std::string *error) {
  if (merged_json == nullptr || !policy.valid()) {
    if (error != nullptr) {
      *error = "invalid frame-rate policy";
    }
    return false;
  }
  nlohmann::json overrides = nlohmann::json::parse(
      base_json.empty() ? "{}" : base_json, nullptr, false, true);
  if (overrides.is_discarded() || !overrides.is_object()) {
    if (error != nullptr) {
      *error = "client-settings overrides must be a JSON object";
    }
    return false;
  }
  if (!SetCompatibleValue(&overrides,
                          "FFlagGameBasicSettingsFramerateCap5", "True",
                          error)) {
    return false;
  }
  if (policy.mode == FrameRateLimitMode::kUnmanaged ||
      policy.mode == FrameRateLimitMode::kDisplay ||
      RobloxSettingsFramerateCap(policy).has_value()) {
    *merged_json = overrides.dump();
    return true;
  }
  if (!SetCompatibleValue(&overrides, "DFIntTaskSchedulerTargetFps",
                          std::to_string(policy.fixed_fps), error)) {
    return false;
  }
  *merged_json = overrides.dump();
  return true;
}

std::optional<int> RobloxSettingsFramerateCap(const FrameRatePolicy &policy) {
  if (policy.mode == FrameRateLimitMode::kUnlimited) {
    return kUnlimitedRobloxFramerateCap;
  }
  if (policy.mode == FrameRateLimitMode::kFixed &&
      policy.fixed_fps > kMaximumSupportedRobloxSchedulerFps) {
    return policy.fixed_fps;
  }
  return std::nullopt;
}

bool WriteRobloxFramerateCap(const std::filesystem::path &android_data_root,
                             int fps, std::string *error) {
  namespace fs = std::filesystem;
  const fs::path file =
      android_data_root / "files/appData/GlobalBasicSettings_13.xml";
  std::error_code exists_error;
  if (!fs::exists(file, exists_error)) {
    return true;
  }
  std::string text;
  {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
      *error = "cannot read " + file.string();
      return false;
    }
    text.assign(std::istreambuf_iterator<char>(input), {});
  }

  const std::string open = "<int name=\"FramerateCap\">";
  const std::string value = std::to_string(fps);
  const std::size_t found = text.find(open);
  if (found != std::string::npos) {
    const std::size_t start = found + open.size();
    const std::size_t end = text.find("</int>", start);
    if (end == std::string::npos) {
      *error = "FramerateCap in " + file.string() + " is not closed";
      return false;
    }
    text.replace(start, end - start, value);
  } else {
    const std::string properties = "<Properties>\n";
    const std::size_t at = text.find(properties);
    if (at == std::string::npos) {
      *error = file.string() + " has no Properties section";
      return false;
    }
    text.insert(at + properties.size(), open + value + "</int>\n");
  }

  const fs::path temporary = file.string() + ".nightcap";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << text;
    if (!output.flush()) {
      *error = "cannot write " + temporary.string();
      return false;
    }
  }
  std::error_code status_error;
  fs::permissions(temporary, fs::status(file, status_error).permissions(),
                  status_error);
  std::error_code rename_error;
  fs::rename(temporary, file, rename_error);
  if (rename_error) {
    fs::remove(temporary, rename_error);
    *error = "cannot replace " + file.string();
    return false;
  }
  return true;
}

} // namespace runtime
} // namespace mocktail
