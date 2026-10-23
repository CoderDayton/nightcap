#include "runtime/roblox_desktop_app_policy.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_desktop_policy_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

bool WriteJson(const std::filesystem::path& path, const nlohmann::json& value) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }
  std::ofstream output(path);
  output << value.dump();
  return output.good();
}

nlohmann::json ReadJson(const std::filesystem::path& path) {
  std::ifstream input(path);
  return nlohmann::json::parse(input, nullptr, false, true);
}

nlohmann::json DecodeConfigurations(const nlohmann::json& storage) {
  return nlohmann::json::parse(
      storage["AppConfiguration"].get_ref<const std::string&>(), nullptr, false,
      true);
}

nlohmann::json DecodePolicy(const nlohmann::json& configurations,
                            const std::string& key) {
  return nlohmann::json::parse(
      configurations[key].get_ref<const std::string&>(), nullptr, false, true);
}

mode_t FileMode(const std::filesystem::path& path) {
  struct stat status{};
  return stat(path.c_str(), &status) == 0 ? status.st_mode & 0777 : 0;
}

void ExpectDesktopLayout(const nlohmann::json& policy) {
  EXPECT_EQ(policy["PlatformGroup"], "Desktop");
  EXPECT_EQ(policy["UseGridHomePage"], true);
  EXPECT_EQ(policy["UseGridPageLayout"], true);
  EXPECT_EQ(policy["SystemBarPlacement"], "Left");
  EXPECT_EQ(policy["ShouldSystemBarUsuallyBePresent"], true);
  EXPECT_EQ(policy["DevicePreferencesPersistentPresenceVariant"], "windows");
  EXPECT_EQ(policy["ShowUncheckedBadge"], false);
}

TEST(RobloxDesktopAppPolicyTest,
     NormalizesLayoutWithoutChangingServerEntitlements) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path storage_path =
      temporary.path() / "appData/LocalStorage/appStorage.json";
  const std::filesystem::path default_path =
      temporary.path() / "assets/GuacDefaultPolicy-GlobalDist.json";
  const nlohmann::json original_policy = {
      {"PlatformGroup", "Unknown"},       {"UseGridHomePage", nullptr},
      {"SystemBarPlacement", "Bottom"},   {"EligibleForVideoCapture", false},
      {"ShowUncheckedBadge", true},       {"AccountOwnedMarker", "preserve"},
  };
  const nlohmann::json configurations = {
      {"GUAC:42:app-policy", original_policy.dump()},
  };
  ASSERT_TRUE(
      WriteJson(storage_path, {{"AppConfiguration", configurations.dump()},
                               {"UnrelatedPreference", "keep"}}));
  ASSERT_TRUE(WriteJson(default_path, {{"PlatformGroup", "Unknown"}}));

  const DesktopAppPolicyResult result =
      ApplyDesktopAppPolicy(storage_path, default_path, 42, "dark");
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.updated);
  EXPECT_EQ(result.normalized_policy_count, 1U);
  EXPECT_EQ(FileMode(storage_path), 0600);
  ExpectDesktopLayout(nlohmann::json::parse(result.policy_json));

  const nlohmann::json storage = ReadJson(storage_path);
  EXPECT_EQ(storage["UnrelatedPreference"], "keep");
  const nlohmann::json updated = DecodeConfigurations(storage);
  ASSERT_TRUE(updated.contains("GUAC:-1:app-policy"));
  ASSERT_TRUE(updated.contains("GUAC:42:app-policy"));
  const nlohmann::json user_policy =
      DecodePolicy(updated, "GUAC:42:app-policy");
  ExpectDesktopLayout(user_policy);
  EXPECT_EQ(user_policy["EligibleForVideoCapture"], false);
  EXPECT_EQ(user_policy["AccountOwnedMarker"], "preserve");
  EXPECT_EQ(user_policy["ForceTheme"], "dark");
  ExpectDesktopLayout(DecodePolicy(updated, "GUAC:-1:app-policy"));

  const DesktopAppPolicyResult repeated =
      ApplyDesktopAppPolicy(storage_path, default_path, 42, "dark");
  ASSERT_TRUE(repeated) << repeated.error;
  EXPECT_FALSE(repeated.updated);
  EXPECT_EQ(repeated.normalized_policy_count, 2U);
}

TEST(RobloxDesktopAppPolicyTest, SeedsMissingStorageFromPayloadDefault) {
  TemporaryDirectory temporary;
  const std::filesystem::path storage_path =
      temporary.path() / "appData/LocalStorage/appStorage.json";
  const std::filesystem::path default_path =
      temporary.path() / "assets/GuacDefaultPolicy-GlobalDist.json";
  ASSERT_TRUE(WriteJson(default_path, {{"PlatformGroup", "Unknown"},
                                       {"PayloadDefaultMarker", 7}}));

  const DesktopAppPolicyResult result =
      ApplyDesktopAppPolicy(storage_path, default_path, -1, "dark");
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.app_storage_created);
  EXPECT_TRUE(result.updated);

  const nlohmann::json configurations =
      DecodeConfigurations(ReadJson(storage_path));
  ASSERT_EQ(configurations.size(), 1U);
  const nlohmann::json policy =
      DecodePolicy(configurations, "GUAC:-1:app-policy");
  ExpectDesktopLayout(policy);
  EXPECT_EQ(policy["ForceTheme"], "dark");
  EXPECT_EQ(policy["PayloadDefaultMarker"], 7);
  EXPECT_EQ(nlohmann::json::parse(result.policy_json)["PayloadDefaultMarker"],
            7);
}

TEST(RobloxDesktopAppPolicyTest, RobloxThemeDoesNotForceCachedPolicy) {
  TemporaryDirectory temporary;
  const std::filesystem::path storage_path = temporary.path() / "storage.json";
  const std::filesystem::path default_path = temporary.path() / "default.json";
  ASSERT_TRUE(WriteJson(default_path,
                        {{"PlatformGroup", "Unknown"},
                         {"ForceTheme", "dark"}}));

  const DesktopAppPolicyResult result =
      ApplyDesktopAppPolicy(storage_path, default_path, 42, "roblox");
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(nlohmann::json::parse(result.policy_json)["ForceTheme"], "");
}

TEST(RobloxDesktopAppPolicyTest,
     SelectsAuthenticatedEntitlementsAndSynchronizesCacheKeys) {
  TemporaryDirectory temporary;
  const std::filesystem::path storage_path =
      temporary.path() / "appData/LocalStorage/appStorage.json";
  const std::filesystem::path default_path = temporary.path() / "default.json";
  const nlohmann::json guest_policy = {{"PlatformGroup", "Unknown"},
                                       {"Entitlement", "guest"}};
  const nlohmann::json user_policy = {{"PlatformGroup", "Unknown"},
                                      {"Entitlement", "authenticated"}};
  const nlohmann::json configurations = {
      {"GUAC:-1:app-policy", guest_policy.dump()},
      {"GUAC:42:app-policy", user_policy.dump()},
  };
  ASSERT_TRUE(
      WriteJson(storage_path, {{"AppConfiguration", configurations.dump()}}));
  ASSERT_TRUE(WriteJson(default_path, {{"PlatformGroup", "Unknown"}}));

  const DesktopAppPolicyResult result =
      ApplyDesktopAppPolicy(storage_path, default_path, 42, "system");
  ASSERT_TRUE(result) << result.error;
  const nlohmann::json selected = nlohmann::json::parse(result.policy_json);
  ExpectDesktopLayout(selected);
  EXPECT_EQ(selected["Entitlement"], "authenticated");
  EXPECT_EQ(selected["ForceTheme"], "");

  const nlohmann::json updated = DecodeConfigurations(ReadJson(storage_path));
  EXPECT_EQ(DecodePolicy(updated, "GUAC:-1:app-policy"), selected);
  EXPECT_EQ(DecodePolicy(updated, "GUAC:42:app-policy"), selected);
}

TEST(RobloxDesktopAppPolicyTest,
     MergesRuntimeOverrideWithoutDiscardingOtherClientSettings) {
  const std::string policy =
      nlohmann::json({{"PlatformGroup", "Desktop"}}).dump();
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeDesktopAppPolicyClientSettingsOverride(
      policy, R"({"DFIntTaskSchedulerTargetFps":"240"})", &merged, &error))
      << error;
  const nlohmann::json overrides = nlohmann::json::parse(merged);
  EXPECT_EQ(overrides["DFIntTaskSchedulerTargetFps"], "240");
  EXPECT_EQ(overrides["FStringAppConfigurationOverrideAppPolicy"], policy);

  EXPECT_FALSE(MergeDesktopAppPolicyClientSettingsOverride(
      policy, R"({"FStringAppConfigurationOverrideAppPolicy":"different"})",
      &merged, &error));
  EXPECT_NE(error.find("conflicts"), std::string::npos);
  EXPECT_FALSE(MergeDesktopAppPolicyClientSettingsOverride("not-json", "{}",
                                                           &merged, &error));
}

TEST(RobloxDesktopAppPolicyTest, DefaultsDesktopFlyoutSidePanelUnlessOverridden) {
  const std::string policy =
      nlohmann::json({{"PlatformGroup", "Desktop"}}).dump();
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeDesktopAppPolicyClientSettingsOverride(policy, "{}", &merged,
                                                          &error))
      << error;
  EXPECT_EQ(nlohmann::json::parse(merged)["FIntFlyoutTestVariantRollout12"],
            "2");

  ASSERT_TRUE(MergeDesktopAppPolicyClientSettingsOverride(
      policy, R"({"FIntFlyoutTestVariantRollout12":"0"})", &merged, &error))
      << error;
  EXPECT_EQ(nlohmann::json::parse(merged)["FIntFlyoutTestVariantRollout12"],
            "0");
}

TEST(RobloxDesktopAppPolicyTest, DefaultsLuauCodegenAllUnlessOverridden) {
  const std::string policy =
      nlohmann::json({{"PlatformGroup", "Desktop"}}).dump();
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeDesktopAppPolicyClientSettingsOverride(policy, "{}", &merged,
                                                          &error))
      << error;
  EXPECT_EQ(nlohmann::json::parse(merged)["FFlagDebugLuauCodegenAll"], "True");

  ASSERT_TRUE(MergeDesktopAppPolicyClientSettingsOverride(
      policy, R"({"FFlagDebugLuauCodegenAll":false})", &merged, &error))
      << error;
  EXPECT_EQ(nlohmann::json::parse(merged)["FFlagDebugLuauCodegenAll"], false);
}

TEST(RobloxDesktopAppPolicyTest, RejectsMalformedOrSymlinkedStorage) {
  TemporaryDirectory temporary;
  const std::filesystem::path default_path = temporary.path() / "default.json";
  ASSERT_TRUE(WriteJson(default_path, {{"PlatformGroup", "Unknown"}}));
  const std::filesystem::path malformed = temporary.path() / "malformed.json";
  {
    std::ofstream output(malformed);
    output << "not-json";
  }
  EXPECT_FALSE(ApplyDesktopAppPolicy(malformed, default_path, 42, "dark"));

  const std::filesystem::path outside = temporary.path() / "outside.json";
  const std::filesystem::path linked = temporary.path() / "linked.json";
  ASSERT_TRUE(WriteJson(outside, {{"Preserve", true}}));
  ASSERT_EQ(symlink(outside.c_str(), linked.c_str()), 0);
  EXPECT_FALSE(ApplyDesktopAppPolicy(linked, default_path, 42, "dark"));
  EXPECT_EQ(ReadJson(outside)["Preserve"], true);

  EXPECT_FALSE(ApplyDesktopAppPolicy(outside, default_path, 42, "invalid"));
}

struct OtaFixture {
  std::filesystem::path storage;
  std::filesystem::path backups;
  std::vector<OtaPatchSeed> patches;
};

bool Touch(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream output(path);
  output << "blob";
  return output.good();
}

OtaFixture MakeOtaFixture(const std::filesystem::path& root) {
  OtaFixture fixture;
  fixture.storage = root / "files/appData/LocalStorage/appStorage.json";
  fixture.backups = root / "files/appData/OTAPatchBackups";
  const std::filesystem::path universal =
      root / "assets/UniversalAppPatchConfig.json";
  const std::filesystem::path in_experience =
      root / "assets/InExperiencePatchConfig.json";
  WriteJson(universal,
            {{"AppStorageResetId", "0"},
             {"AssetId", "118593852151835"},
             {"AssetVersion", "14827"},
             {"IsForcedUpdate", false},
             {"LocalAssetURI",
              "rbxasset://models/UniversalApp/UniversalApp.rbxm"},
             {"LocalAssetHash", "6d9cfee80b8ff14f68a9d1007c549be2"},
             {"MaxAppVersion", "742"}});
  WriteJson(in_experience,
            {{"AppStorageResetId", "0"},
             {"AssetId", "80471914653504"},
             {"AssetVersion", "14738"},
             {"IsForcedUpdate", false},
             {"LocalAssetURI",
              "rbxasset://models/InExperience/InExperience.rbxm"},
             {"MaxAppVersion", "742"}});
  fixture.patches = {{"UniversalAppPatch", universal},
                     {"InExperiencePatch", in_experience}};
  return fixture;
}

nlohmann::json DecodePatchEntries(const nlohmann::json& storage,
                                  const std::string& key) {
  return nlohmann::json::parse(storage[key].get_ref<const std::string&>(),
                               nullptr, false, true);
}

TEST(RobloxOtaPatchSeedTest, RecordsNewestDownloadedPatchForInstallation) {
  TemporaryDirectory temporary;
  const OtaFixture fixture = MakeOtaFixture(temporary.path());
  ASSERT_TRUE(WriteJson(fixture.storage, {{"AppInstallationId", "1583"},
                                          {"RobloxLocaleId", "en_us"}}));
  ASSERT_TRUE(Touch(fixture.backups / "DataModelPatch_118593852151835_15350"));
  ASSERT_TRUE(Touch(fixture.backups / "DataModelPatch_118593852151835_15200"));
  ASSERT_TRUE(Touch(fixture.backups / "DataModelPatch_80471914653504_15258"));
  ASSERT_TRUE(Touch(fixture.backups / "DataModelPatch_999_20000"));

  const OtaPatchSeedResult result = SeedDownloadedOtaPatchConfigs(
      fixture.storage, fixture.backups, fixture.patches);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.seeded_count, 2U);

  const nlohmann::json storage = ReadJson(fixture.storage);
  EXPECT_EQ(storage["RobloxLocaleId"], "en_us");
  EXPECT_EQ(FileMode(fixture.storage), 0600);
  const nlohmann::json universal =
      DecodePatchEntries(storage, "_Patch__UniversalAppPatch");
  ASSERT_TRUE(universal.contains("app:1583"));
  ASSERT_EQ(universal["app:1583"].size(), 1U);
  const nlohmann::json entry = universal["app:1583"][0];
  EXPECT_EQ(entry["AssetId"], "118593852151835");
  EXPECT_EQ(entry["AssetVersion"], "15350");
  EXPECT_EQ(entry["MaxAppVersion"], "742");
  EXPECT_EQ(entry["LocalAssetURI"],
            "rbxasset://models/UniversalApp/UniversalApp.rbxm");
  EXPECT_EQ(entry["AppStorageResetId"], "0");
  EXPECT_EQ(entry["IsForcedUpdate"], false);
  EXPECT_EQ(entry["DownloadSource"], "in_process");
  EXPECT_EQ(DecodePatchEntries(storage, "_Patch__InExperiencePatch")
                ["app:1583"][0]["AssetVersion"],
            "15258");
}

TEST(RobloxOtaPatchSeedTest, LeavesRobloxOwnedEntriesAlone) {
  TemporaryDirectory temporary;
  const OtaFixture fixture = MakeOtaFixture(temporary.path());
  const std::string owned = R"({"app:1583":[{"AssetVersion":"15000"}]})";
  ASSERT_TRUE(WriteJson(fixture.storage,
                        {{"AppInstallationId", "1583"},
                         {"_Patch__UniversalAppPatch", owned}}));
  ASSERT_TRUE(Touch(fixture.backups / "DataModelPatch_118593852151835_15350"));

  const OtaPatchSeedResult result = SeedDownloadedOtaPatchConfigs(
      fixture.storage, fixture.backups, fixture.patches);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.seeded_count, 0U);
  const nlohmann::json storage = ReadJson(fixture.storage);
  EXPECT_EQ(storage["_Patch__UniversalAppPatch"], owned);
  EXPECT_FALSE(storage.contains("_Patch__InExperiencePatch"));
}

TEST(RobloxOtaPatchSeedTest, IgnoresDownloadsNotNewerThanBuiltInPatch) {
  TemporaryDirectory temporary;
  const OtaFixture fixture = MakeOtaFixture(temporary.path());
  ASSERT_TRUE(WriteJson(fixture.storage, {{"AppInstallationId", "1583"}}));
  ASSERT_TRUE(Touch(fixture.backups / "DataModelPatch_118593852151835_14827"));
  ASSERT_TRUE(Touch(fixture.backups / "DataModelPatch_80471914653504_100"));
  ASSERT_TRUE(
      Touch(fixture.backups / "DataModelPatch_80471914653504_15258.tmp.3"));

  const OtaPatchSeedResult result = SeedDownloadedOtaPatchConfigs(
      fixture.storage, fixture.backups, fixture.patches);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.seeded_count, 0U);
  EXPECT_FALSE(ReadJson(fixture.storage).contains("_Patch__UniversalAppPatch"));
}

TEST(RobloxOtaPatchSeedTest, CountsUnrecordedPatchesWithoutWriting) {
  TemporaryDirectory temporary;
  const OtaFixture fixture = MakeOtaFixture(temporary.path());
  ASSERT_TRUE(WriteJson(fixture.storage, {{"AppInstallationId", "1583"}}));
  EXPECT_EQ(CountUnrecordedOtaPatches(fixture.storage, fixture.backups,
                                      fixture.patches),
            0U);
  ASSERT_TRUE(Touch(fixture.backups / "DataModelPatch_118593852151835_15350"));
  ASSERT_TRUE(Touch(fixture.backups / "DataModelPatch_80471914653504_15258"));

  EXPECT_EQ(CountUnrecordedOtaPatches(fixture.storage, fixture.backups,
                                      fixture.patches),
            2U);
  EXPECT_FALSE(ReadJson(fixture.storage).contains("_Patch__UniversalAppPatch"));
  ASSERT_TRUE(SeedDownloadedOtaPatchConfigs(fixture.storage, fixture.backups,
                                            fixture.patches));
  EXPECT_EQ(CountUnrecordedOtaPatches(fixture.storage, fixture.backups,
                                      fixture.patches),
            0U);
}

TEST(RobloxOtaPatchSeedTest, SkipsProfilesRobloxHasNotStarted) {
  TemporaryDirectory temporary;
  const OtaFixture fixture = MakeOtaFixture(temporary.path());
  ASSERT_TRUE(Touch(fixture.backups / "DataModelPatch_118593852151835_15350"));

  OtaPatchSeedResult result = SeedDownloadedOtaPatchConfigs(
      fixture.storage, fixture.backups, fixture.patches);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.seeded_count, 0U);
  EXPECT_FALSE(std::filesystem::exists(fixture.storage));

  ASSERT_TRUE(WriteJson(fixture.storage, {{"RobloxLocaleId", "en_us"}}));
  result = SeedDownloadedOtaPatchConfigs(fixture.storage, fixture.backups,
                                         fixture.patches);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.seeded_count, 0U);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
