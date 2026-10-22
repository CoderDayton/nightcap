#include "runtime/fleasion.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

namespace mocktail::runtime {
namespace {

constexpr char kFlag[] = "FFlagAssetProviderUseAssetDeliveryV3";

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
