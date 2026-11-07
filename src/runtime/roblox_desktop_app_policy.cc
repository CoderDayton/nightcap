#include "runtime/roblox_desktop_app_policy.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "runtime/json_file.h"
#include "runtime/luau_codegen_switch.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kAppPolicySuffix = ":app-policy";
constexpr std::string_view kDesktopAppPolicyOverride =
    "FStringAppConfigurationOverrideAppPolicy";

bool AtomicWriteJson(const std::filesystem::path& path,
                     const nlohmann::json& value, std::string* error) {
  return WriteJsonFileAtomically(
      path, value,
      {"Roblox appStorage", "Roblox appStorage", ".mocktail-desktop-policy.tmp."},
      error);
}

bool HasSuffix(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

void NormalizeDesktopLayout(nlohmann::json* policy) {
  (*policy)["PlatformGroup"] = "Desktop";
  (*policy)["UseGridHomePage"] = true;
  (*policy)["UseGridPageLayout"] = true;
  (*policy)["SystemBarPlacement"] = "Left";
  (*policy)["ShouldSystemBarUsuallyBePresent"] = true;
  (*policy)["DevicePreferencesPersistentPresenceVariant"] = "windows";
  (*policy)["ShowUncheckedBadge"] = false;
  (*policy)["AccountSwitcherEnabled"] = true;
  (*policy)["ExcludeAccountSwitcher"] = false;
}

bool DecodePolicy(const nlohmann::json& encoded, nlohmann::json* policy) {
  if (!encoded.is_string()) {
    return false;
  }
  *policy = nlohmann::json::parse(encoded.get_ref<const std::string&>(),
                                  nullptr, false, true);
  return !policy->is_discarded() && policy->is_object();
}

bool ReadStringField(const nlohmann::json& object, const char* key,
                     std::string* value) {
  const auto field = object.find(key);
  if (field == object.end() || !field->is_string() ||
      field->get_ref<const std::string&>().empty()) {
    return false;
  }
  *value = field->get<std::string>();
  return true;
}

bool ParseVersion(std::string_view text, std::uint64_t* version) {
  if (text.empty()) {
    return false;
  }
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), *version);
  return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}

std::size_t AddUnrecordedOtaPatchEntries(
    nlohmann::json* storage,
    const std::filesystem::path& patch_backup_directory,
    const std::vector<OtaPatchSeed>& patches) {
  const auto installation = storage->find("AppInstallationId");
  std::string installation_id;
  if (installation != storage->end() && installation->is_string()) {
    installation_id = installation->get<std::string>();
  } else if (installation != storage->end() &&
             installation->is_number_unsigned()) {
    installation_id = std::to_string(installation->get<std::uint64_t>());
  }
  if (installation_id.empty()) {
    return 0;
  }

  std::size_t added = 0;
  for (const OtaPatchSeed& patch : patches) {
    const std::string storage_key = "_Patch__" + patch.storage_name;
    if (storage->contains(storage_key)) {
      continue;
    }
    const JsonFileReadResult builtin =
        ReadJsonObjectFile(patch.builtin_config_file, "built-in OTA patch config");
    if (builtin.status != JsonFileStatus::kReady) {
      continue;
    }
    std::string asset_id;
    std::string builtin_version;
    std::string max_app_version;
    std::string local_asset_uri;
    std::string reset_id = "0";
    if (!ReadStringField(builtin.value, "AssetId", &asset_id) ||
        !ReadStringField(builtin.value, "AssetVersion", &builtin_version) ||
        !ReadStringField(builtin.value, "MaxAppVersion", &max_app_version) ||
        !ReadStringField(builtin.value, "LocalAssetURI", &local_asset_uri)) {
      continue;
    }
    (void)ReadStringField(builtin.value, "AppStorageResetId", &reset_id);
    std::uint64_t newest = 0;
    if (!ParseVersion(builtin_version, &newest)) {
      continue;
    }
    const std::uint64_t builtin_number = newest;
    const std::string prefix = "DataModelPatch_" + asset_id + "_";
    std::error_code error;
    for (std::filesystem::directory_iterator it(patch_backup_directory, error),
         end;
         !error && it != end; it.increment(error)) {
      const std::string name = it->path().filename().string();
      std::uint64_t version = 0;
      if (name.rfind(prefix, 0) != 0 ||
          !ParseVersion(std::string_view(name).substr(prefix.size()),
                        &version) ||
          !std::filesystem::is_regular_file(it->symlink_status())) {
        continue;
      }
      if (version > newest) {
        newest = version;
      }
    }
    if (newest == builtin_number) {
      continue;
    }
    const nlohmann::json entry = {
        {"AssetId", asset_id},
        {"AssetVersion", std::to_string(newest)},
        {"MaxAppVersion", max_app_version},
        {"LocalAssetURI", local_asset_uri},
        {"IsForcedUpdate", false},
        {"AppStorageResetId", reset_id},
        {"DownloadSource", "in_process"}};
    nlohmann::json entries = nlohmann::json::object();
    entries["app:" + installation_id] = nlohmann::json::array({entry});
    (*storage)[storage_key] = entries.dump();
    ++added;
  }
  return added;
}

}  // namespace

DesktopAppPolicyResult ApplyDesktopAppPolicy(
    const std::filesystem::path& app_storage_file,
    const std::filesystem::path& default_policy_file,
    std::int64_t authenticated_user_id, std::string_view theme_mode) {
  DesktopAppPolicyResult result;
  if (!app_storage_file.is_absolute() || !default_policy_file.is_absolute() ||
      authenticated_user_id < -1 ||
      (theme_mode != "roblox" && theme_mode != "system" &&
       theme_mode != "light" && theme_mode != "dark")) {
    result.error = "desktop app-policy paths, user ID, or theme are invalid";
    return result;
  }
  const std::string forced_theme =
      theme_mode == "light" || theme_mode == "dark" ? std::string(theme_mode)
                                                     : std::string();

  JsonFileReadResult storage =
      ReadJsonObjectFile(app_storage_file, "Roblox appStorage");
  if (storage.status == JsonFileStatus::kInvalid) {
    result.error = storage.error;
    return result;
  }
  if (storage.status == JsonFileStatus::kMissing) {
    storage.status = JsonFileStatus::kReady;
    storage.value = nlohmann::json::object();
    result.app_storage_created = true;
  }

  nlohmann::json configurations = nlohmann::json::object();
  const auto encoded_configurations = storage.value.find("AppConfiguration");
  if (encoded_configurations != storage.value.end()) {
    if (!encoded_configurations->is_string()) {
      result.error = "Roblox AppConfiguration is not an encoded object";
      return result;
    }
    configurations = nlohmann::json::parse(
        encoded_configurations->get_ref<const std::string&>(), nullptr, false,
        true);
    if (configurations.is_discarded() || !configurations.is_object()) {
      result.error = "Roblox AppConfiguration encoding is invalid";
      return result;
    }
  }

  nlohmann::json template_policy;
  nlohmann::json guest_policy;
  nlohmann::json authenticated_policy;
  const std::string authenticated_key =
      authenticated_user_id >= 0
          ? "GUAC:" + std::to_string(authenticated_user_id) + ":app-policy"
          : std::string();
  for (auto& entry : configurations.items()) {
    if (!HasSuffix(entry.key(), kAppPolicySuffix)) {
      continue;
    }
    nlohmann::json policy;
    if (!DecodePolicy(entry.value(), &policy)) {
      result.error = "cached Roblox app-policy encoding is invalid";
      return result;
    }
    NormalizeDesktopLayout(&policy);
    policy["ForceTheme"] = forced_theme;
    entry.value() = policy.dump();
    if (!template_policy.is_object()) {
      template_policy = policy;
    }
    if (entry.key() == "GUAC:-1:app-policy") {
      guest_policy = policy;
    }
    if (!authenticated_key.empty() && entry.key() == authenticated_key) {
      authenticated_policy = policy;
    }
    ++result.normalized_policy_count;
  }

  if (authenticated_policy.is_object()) {
    template_policy = std::move(authenticated_policy);
  } else if (guest_policy.is_object()) {
    template_policy = std::move(guest_policy);
  }

  if (!template_policy.is_object()) {
    const JsonFileReadResult fallback =
        ReadJsonObjectFile(default_policy_file, "Roblox default app-policy");
    if (fallback.status != JsonFileStatus::kReady) {
      result.error = fallback.status == JsonFileStatus::kMissing
                         ? "Roblox default app-policy is missing"
                         : fallback.error;
      return result;
    }
    template_policy = fallback.value;
    NormalizeDesktopLayout(&template_policy);
    template_policy["ForceTheme"] = forced_theme;
  }

  const std::string encoded_policy = template_policy.dump();
  result.policy_json = encoded_policy;
  configurations["GUAC:-1:app-policy"] = encoded_policy;
  if (authenticated_user_id >= 0) {
    configurations[authenticated_key] = encoded_policy;
  }

  const std::string encoded = configurations.dump();
  const bool changed =
      encoded_configurations == storage.value.end() ||
      encoded_configurations->get_ref<const std::string&>() != encoded;
  if (!changed) {
    return result;
  }
  storage.value["AppConfiguration"] = encoded;
  if (!AtomicWriteJson(app_storage_file, storage.value, &result.error)) {
    return result;
  }
  result.updated = true;
  return result;
}

bool MergeDesktopAppPolicyClientSettingsOverride(std::string_view policy_json,
                                                 std::string_view base_json,
                                                 std::string* merged_json,
                                                 std::string* error) {
  if (merged_json == nullptr) {
    if (error != nullptr) {
      *error = "desktop app-policy client-settings output is required";
    }
    return false;
  }
  const nlohmann::json policy =
      nlohmann::json::parse(policy_json, nullptr, false, true);
  if (policy.is_discarded() || !policy.is_object()) {
    if (error != nullptr) {
      *error = "desktop app-policy override must be a JSON object";
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
  const auto existing = overrides.find(kDesktopAppPolicyOverride);
  if (existing != overrides.end() &&
      (!existing->is_string() ||
       existing->get_ref<const std::string&>() != policy_json)) {
    if (error != nullptr) {
      *error = "desktop app-policy conflicts with " +
               std::string(kDesktopAppPolicyOverride);
    }
    return false;
  }
  overrides[std::string(kDesktopAppPolicyOverride)] = policy_json;
  // Native Luau code makes the app's menus faster. LuauCodegenMenuSwitch turns
  // it off in games, which keep Roblox's own per-script choice.
  overrides.emplace(std::string(kLuauCodegenAllClientSetting), "True");
  // The SDUI Charts page in 2.736/2.738 can pass a table to TextColor3 and
  // leave the whole LuaApp blank. Default to the working legacy page while
  // preserving explicit user overrides for testing newer UI patches.
  overrides.emplace("FFlagLuaAppChartsAppPage", "False");
  // Roblox enrolls desktop clients in the flyout side panel through a
  // server-side experiment that does not reach this client. Variant 2 is the
  // desktop side panel; an explicit user override is preserved.
  overrides.emplace("FIntFlyoutTestVariantRollout12", "2");
  *merged_json = overrides.dump();
  return true;
}

OtaPatchSeedResult SeedDownloadedOtaPatchConfigs(
    const std::filesystem::path& app_storage_file,
    const std::filesystem::path& patch_backup_directory,
    const std::vector<OtaPatchSeed>& patches) {
  OtaPatchSeedResult result;
  JsonFileReadResult storage =
      ReadJsonObjectFile(app_storage_file, "Roblox appStorage");
  if (storage.status == JsonFileStatus::kInvalid) {
    result.error = storage.error;
    return result;
  }
  if (storage.status == JsonFileStatus::kMissing) {
    return result;
  }
  result.seeded_count = AddUnrecordedOtaPatchEntries(
      &storage.value, patch_backup_directory, patches);
  if (result.seeded_count > 0 &&
      !AtomicWriteJson(app_storage_file, storage.value, &result.error)) {
    result.seeded_count = 0;
  }
  return result;
}

std::size_t CountUnrecordedOtaPatches(
    const std::filesystem::path& app_storage_file,
    const std::filesystem::path& patch_backup_directory,
    const std::vector<OtaPatchSeed>& patches) {
  JsonFileReadResult storage =
      ReadJsonObjectFile(app_storage_file, "Roblox appStorage");
  if (storage.status != JsonFileStatus::kReady) {
    return 0;
  }
  return AddUnrecordedOtaPatchEntries(&storage.value, patch_backup_directory,
                                      patches);
}

}  // namespace runtime
}  // namespace mocktail
