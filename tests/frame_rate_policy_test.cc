#include "runtime/frame_rate_policy.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

namespace fs = std::filesystem;

constexpr char kSettings[] =
    "<roblox>\n\t<Item class=\"UserGameSettings\">\n\t\t<Properties>\n"
    "\t\t\t<bool name=\"BadgeVisible\">true</bool>\n"
    "\t\t\t<int name=\"FramerateCap\">120</int>\n"
    "\t\t\t<int name=\"GraphicsQualityLevel\">0</int>\n"
    "\t\t</Properties>\n\t</Item>\n</roblox>\n";

class SettingsFile {
 public:
  SettingsFile() {
    char pattern[] = "/tmp/mocktail_frame_rate_XXXXXX";
    if (mkdtemp(pattern) != nullptr) {
      root_ = pattern;
    }
  }
  ~SettingsFile() {
    std::error_code error;
    fs::remove_all(root_, error);
  }

  fs::path path() const {
    return root_ / "files/appData/GlobalBasicSettings_13.xml";
  }
  const fs::path& root() const { return root_; }

  void Write(const std::string& text) const {
    fs::create_directories(path().parent_path());
    std::ofstream(path(), std::ios::binary) << text;
  }

  std::string Read() const {
    std::ifstream input(path(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
  }

 private:
  fs::path root_;
};

std::string WithCap(const std::string& value) {
  std::string text = kSettings;
  const std::string old_line = "<int name=\"FramerateCap\">120</int>";
  text.replace(text.find(old_line), old_line.size(),
               "<int name=\"FramerateCap\">" + value + "</int>");
  return text;
}

TEST(FrameRatePolicyTest, OnlyTargetsAboveTheSchedulerLimitNeedTheRobloxCap) {
  EXPECT_EQ(RobloxSettingsFramerateCap(ParseFrameRatePolicy("unlimited")),
            kUnlimitedRobloxFramerateCap);
  EXPECT_EQ(RobloxSettingsFramerateCap(ParseFrameRatePolicy("300")), 300);
  EXPECT_EQ(RobloxSettingsFramerateCap(ParseFrameRatePolicy("240")),
            std::nullopt);
  EXPECT_EQ(RobloxSettingsFramerateCap(ParseFrameRatePolicy("60")),
            std::nullopt);
  EXPECT_EQ(RobloxSettingsFramerateCap(ParseFrameRatePolicy("-1")),
            std::nullopt);
  EXPECT_EQ(RobloxSettingsFramerateCap(ParseFrameRatePolicy("display")),
            std::nullopt);
}

TEST(FrameRatePolicyTest, TargetsAboveTheSchedulerLimitLeaveTheSchedulerUnset) {
  for (const char* value : {"unlimited", "300", "9999"}) {
    std::string merged;
    std::string error;
    ASSERT_TRUE(MergeFrameRateClientSettingsOverrides(
        ParseFrameRatePolicy(value), "{}", &merged, &error))
        << error;
    EXPECT_EQ(merged.find("DFIntTaskSchedulerTargetFps"), std::string::npos)
        << value;
    EXPECT_NE(merged.find("\"FFlagGameBasicSettingsFramerateCap5\":\"True\""),
              std::string::npos)
        << value;
  }
}

TEST(FrameRatePolicyTest, WritesTheCapIntoRobloxSettings) {
  SettingsFile settings;
  settings.Write(kSettings);
  std::string error;
  ASSERT_TRUE(WriteRobloxFramerateCap(settings.root(), 300, &error)) << error;
  EXPECT_EQ(settings.Read(), WithCap("300"));
}

TEST(FrameRatePolicyTest, AddsTheCapWhenRobloxSettingsLackIt) {
  SettingsFile settings;
  std::string without = kSettings;
  const std::string line = "\t\t\t<int name=\"FramerateCap\">120</int>\n";
  without.erase(without.find(line), line.size());
  settings.Write(without);
  std::string error;
  ASSERT_TRUE(WriteRobloxFramerateCap(settings.root(), 9999, &error)) << error;
  const std::string written = settings.Read();
  EXPECT_NE(written.find("<Properties>\n<int name=\"FramerateCap\">9999</int>\n"),
            std::string::npos)
      << written;
  EXPECT_NE(written.find("GraphicsQualityLevel"), std::string::npos);
}

TEST(FrameRatePolicyTest, LeavesMissingRobloxSettingsForRobloxToCreate) {
  SettingsFile settings;
  std::string error;
  EXPECT_TRUE(WriteRobloxFramerateCap(settings.root(), 300, &error)) << error;
  EXPECT_FALSE(fs::exists(settings.path()));
}

TEST(FrameRatePolicyTest, RejectsRobloxSettingsItCannotPlaceTheCapIn) {
  SettingsFile settings;
  settings.Write("<roblox></roblox>\n");
  std::string error;
  EXPECT_FALSE(WriteRobloxFramerateCap(settings.root(), 300, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(settings.Read(), "<roblox></roblox>\n");
}

TEST(FrameRatePolicyTest, ParsesSupportedModes) {
  EXPECT_EQ(ParseFrameRatePolicy("").mode,
            FrameRateLimitMode::kUnmanaged);
  EXPECT_EQ(ParseFrameRatePolicy("-1").mode,
            FrameRateLimitMode::kUnmanaged);
  EXPECT_EQ(ParseFrameRatePolicy("display").mode, FrameRateLimitMode::kDisplay);
  EXPECT_EQ(ParseFrameRatePolicy("10").fixed_fps, 10);
  EXPECT_EQ(ParseFrameRatePolicy("70").fixed_fps, 70);
  EXPECT_EQ(ParseFrameRatePolicy("144").fixed_fps, 144);
  EXPECT_EQ(ParseFrameRatePolicy("unlimited").mode,
            FrameRateLimitMode::kUnlimited);
  EXPECT_EQ(ParseFrameRatePolicy("999").fixed_fps, 999);
  EXPECT_FALSE(ParseFrameRatePolicy("0").valid());
  EXPECT_FALSE(ParseFrameRatePolicy("-10").valid());
  EXPECT_FALSE(ParseFrameRatePolicy("10fps").valid());
  EXPECT_FALSE(ParseFrameRatePolicy("10 ").valid());
}

TEST(FrameRatePolicyTest, UnmanagedOnlyEnablesRobloxBasicSettingsCap) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("-1"),
      R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})", &merged,
      &error))
      << error;
  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.at("FFlagGameBasicSettingsFramerateCap5"), "True");
  EXPECT_FALSE(parsed.contains("DFIntTaskSchedulerTargetFps"));
  EXPECT_FALSE(parsed.contains("FFlagTaskSchedulerLimitTargetFpsTo2402"));
  EXPECT_EQ(parsed.at("FStringGraphicsVulkanShaderMTDenyPattern"), "4318:.*");
}

TEST(FrameRatePolicyTest, MergesArbitraryFixedValueIntoSchedulerFflag) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("10"), "{}", &merged, &error));
  EXPECT_NE(merged.find("\"DFIntTaskSchedulerTargetFps\":\"10\""),
            std::string::npos);
  EXPECT_NE(merged.find("\"FFlagGameBasicSettingsFramerateCap5\":\"True\""),
            std::string::npos);
}

TEST(FrameRatePolicyTest, MergesUnlimitedWithoutLosingGraphicsPolicy) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("unlimited"),
      R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})", &merged,
      &error));
  EXPECT_EQ(merged.find("DFIntTaskSchedulerTargetFps"), std::string::npos);
  EXPECT_EQ(merged.find("FFlagTaskSchedulerLimitTargetFpsTo2402"),
            std::string::npos);
  EXPECT_NE(merged.find("FStringGraphicsVulkanShaderMTDenyPattern"),
            std::string::npos);
}

TEST(FrameRatePolicyTest, RejectsMalformedAndConflictingOverrides) {
  std::string merged;
  std::string error;
  EXPECT_FALSE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("120"), "not-json", &merged, &error));
  EXPECT_FALSE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("120"), R"({"DFIntTaskSchedulerTargetFps":"60"})",
      &merged, &error));
}

} // namespace
} // namespace runtime
} // namespace mocktail
