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

TEST(RobloxDesktopAppPolicyTest,
     VrDisablesIdleThrottleAfterDesktopCompositionWithoutPersistingIt) {
  TemporaryDirectory temporary;
  const auto storage_path = temporary.path() / "storage.json";
  const auto default_path = temporary.path() / "default.json";
  const nlohmann::json cached_policy = {
      {"ThrottleFramerate", true}, {"EligibleForVideoCapture", false},
      {"AccountOwnedMarker", "preserve"},
  };
  const nlohmann::json configurations = {
      {"GUAC:42:app-policy", cached_policy.dump()},
  };
  ASSERT_TRUE(WriteJson(storage_path,
                        {{"AppConfiguration", configurations.dump()}}));
  const auto desktop =
      ApplyDesktopAppPolicy(storage_path, default_path, 42, "dark");
  ASSERT_TRUE(desktop) << desktop.error;
  const auto stored_before_vr = ReadJson(storage_path);

  std::string client, fast, error;
  ASSERT_TRUE(MergeDesktopAppPolicyClientSettingsOverride(
      desktop.policy_json, R"({"DFIntTaskSchedulerTargetFps":"90"})", &client,
      &error)) << error;
  ASSERT_TRUE(MergeVrAppPolicyClientSettingsOverrides(
      true, client, R"({"FFlagUnrelated":"True"})", &client, &fast, &error))
      << error;
  const auto client_json = nlohmann::json::parse(client);
  const auto fast_json = nlohmann::json::parse(fast);
  auto expected_policy = nlohmann::json::parse(desktop.policy_json);
  expected_policy["ThrottleFramerate"] = false;
  EXPECT_EQ(client_json["FStringAppConfigurationOverrideAppPolicy"],
            expected_policy.dump());
  EXPECT_EQ(fast_json["FStringAppConfigurationOverrideAppPolicy"],
            expected_policy.dump());
  EXPECT_EQ(client_json["DFIntTaskSchedulerTargetFps"], "90");
  EXPECT_EQ(fast_json["FFlagUnrelated"], "True");
  EXPECT_EQ(ReadJson(storage_path), stored_before_vr);

  const auto next_launch =
      ApplyDesktopAppPolicy(storage_path, default_path, 42, "dark");
  ASSERT_TRUE(next_launch) << next_launch.error;
  EXPECT_TRUE(nlohmann::json::parse(next_launch.policy_json)
                  .at("ThrottleFramerate").get<bool>());
  EXPECT_FALSE(next_launch.updated);

  const std::string first_client = client;
  const std::string first_fast = fast;
  ASSERT_TRUE(MergeVrAppPolicyClientSettingsOverrides(
      true, client, fast, &client, &fast, &error)) << error;
  EXPECT_EQ(client, first_client);
  EXPECT_EQ(fast, first_fast);
}

TEST(RobloxDesktopAppPolicyTest, VrPolicyWorksWithoutDesktopProfile) {
  std::string client, fast, error;
  ASSERT_TRUE(MergeVrAppPolicyClientSettingsOverrides(
      true, "{}", "{}", &client, &fast, &error)) << error;
  const nlohmann::json policy = {{"ThrottleFramerate", false}};
  const nlohmann::json expected = {
      {"FStringAppConfigurationOverrideAppPolicy", policy.dump()},
  };
  EXPECT_EQ(nlohmann::json::parse(client), expected);
  EXPECT_EQ(nlohmann::json::parse(fast), expected);
}

TEST(RobloxDesktopAppPolicyTest, VrKeepsLateFastFlagsPolicyPrecedence) {
  const nlohmann::json earlier = {{"EarlierPolicy", 1}};
  nlohmann::json later = {
      {"ThrottleFramerate", true}, {"LaterPolicy", 2}, {"ForceTheme", "light"},
  };
  const nlohmann::json client_input = {
      {"FStringAppConfigurationOverrideAppPolicy", earlier.dump()},
      {"FIntClientMarker", "42"},
  };
  const nlohmann::json fast_input = {
      {"FStringAppConfigurationOverrideAppPolicy", later.dump()},
      {"FIntFastMarker", "7"},
  };
  std::string client, fast, error;
  ASSERT_TRUE(MergeVrAppPolicyClientSettingsOverrides(
      true, client_input.dump(), fast_input.dump(), &client, &fast, &error))
      << error;
  later["ThrottleFramerate"] = false;
  EXPECT_EQ(nlohmann::json::parse(client)
                .at("FStringAppConfigurationOverrideAppPolicy"), later.dump());
  EXPECT_EQ(nlohmann::json::parse(fast)
                .at("FStringAppConfigurationOverrideAppPolicy"), later.dump());
  EXPECT_EQ(nlohmann::json::parse(client).at("FIntClientMarker"), "42");
  EXPECT_EQ(nlohmann::json::parse(fast).at("FIntFastMarker"), "7");
}

TEST(RobloxDesktopAppPolicyTest, NonVrPreservesBothPolicyChannels) {
  const std::string client_input =
      R"({ "FStringAppConfigurationOverrideAppPolicy": "{\"ThrottleFramerate\":true}" })";
  const std::string fast_input = R"({ "FIntUnrelated": "42" })";
  std::string client, fast, error;
  ASSERT_TRUE(MergeVrAppPolicyClientSettingsOverrides(
      false, client_input, fast_input, &client, &fast, &error)) << error;
  EXPECT_EQ(client, client_input);
  EXPECT_EQ(fast, fast_input);
}

TEST(RobloxDesktopAppPolicyTest, VrAcceptsEmptyAppPolicyString) {
  std::string client, fast, error;
  ASSERT_TRUE(MergeVrAppPolicyClientSettingsOverrides(
      true, "{}", R"({"FStringAppConfigurationOverrideAppPolicy":""})",
      &client, &fast, &error)) << error;
  EXPECT_EQ(nlohmann::json::parse(client)
                .at("FStringAppConfigurationOverrideAppPolicy"),
            R"({"ThrottleFramerate":false})");
  EXPECT_EQ(client, fast);
}

TEST(RobloxDesktopAppPolicyTest, VrRejectsMalformedOverridesWithoutErasingThem) {
  const std::string bad_inputs[] = {
      "[]", "bad", "null",
      R"({"FStringAppConfigurationOverrideAppPolicy":true})",
      R"({"FStringAppConfigurationOverrideAppPolicy":"bad"})",
      R"({"FStringAppConfigurationOverrideAppPolicy":"[]"})",
      R"({"FStringAppConfigurationOverrideAppPolicy":"null"})",
  };
  for (const auto& input : bad_inputs) {
    SCOPED_TRACE(input);
    std::string client = "unchanged", fast = "unchanged", error;
    EXPECT_FALSE(MergeVrAppPolicyClientSettingsOverrides(
        true, input, "{}", &client, &fast, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(MergeVrAppPolicyClientSettingsOverrides(
        true, "{}", input, &client, &fast, &error));
    EXPECT_EQ(client, "unchanged");
    EXPECT_EQ(fast, "unchanged");
  }
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
