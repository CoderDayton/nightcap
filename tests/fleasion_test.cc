#include "runtime/fleasion.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mocktail::runtime {
namespace {

constexpr char kFlag[] = "FFlagAssetProviderUseAssetDeliveryV3";

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(
      std::unordered_map<std::string, std::string> values = {})
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

bool NeedsFreshLaunch(const char* enabled, const char* clear) {
  return WebsiteJoinNeedsFreshLaunch(RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_FLEASION_ENABLED", enabled},
                      {"MOCKTAIL_FLEASION_CLEAR_CACHE_ON_LAUNCH", clear}})));
}

TEST(FleasionTest, AWebsiteJoinStartsFreshWhenEveryLaunchClearsTheCache) {
  EXPECT_TRUE(NeedsFreshLaunch("1", "1"));
}

TEST(FleasionTest, AWebsiteJoinGoesToTheOpenNightcapOtherwise) {
  EXPECT_FALSE(NeedsFreshLaunch("1", "0"));
  EXPECT_FALSE(NeedsFreshLaunch("0", "1"));
  EXPECT_FALSE(NeedsFreshLaunch("0", "0"));
}

nlohmann::json Merge(const std::string& base) {
  std::string merged;
  std::string error;
  EXPECT_TRUE(MergeFleasionClientSettingsOverrides(base, &merged, &error))
      << error;
  return nlohmann::json::parse(merged);
}

std::string Value(const nlohmann::json& json, const char* key) {
  return json.value(key, std::string());
}

TEST(FleasionTest, RequestsAssetsFromTheBatchApiFleasionRewrites) {
  EXPECT_EQ(Value(Merge("{}"), kFlag), "False");
}

TEST(FleasionTest, KeepsOtherOverrides) {
  const nlohmann::json merged = Merge(R"({"FIntTargetRefreshRate":"144"})");
  EXPECT_EQ(Value(merged, "FIntTargetRefreshRate"), "144");
  EXPECT_EQ(Value(merged, kFlag), "False");
}

TEST(FleasionTest, KeepsAnExplicitValue) {
  EXPECT_EQ(
      Value(Merge(R"({"FFlagAssetProviderUseAssetDeliveryV3":"True"})"), kFlag),
      "True");
}

TEST(FleasionTest, RejectsOverridesThatAreNotAnObject) {
  std::string merged;
  std::string error;
  EXPECT_FALSE(MergeFleasionClientSettingsOverrides("[]", &merged, &error));
  EXPECT_FALSE(error.empty());
}

}
}
