#include "runtime/server_info.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <stdlib.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>

namespace mocktail::runtime {
namespace {

constexpr std::int64_t kMs = 1000;

TEST(ServerInfoTest, ReadsTheGameServerFromRobloxsJoinLine) {
  const std::optional<GameServerEndpoint> endpoint = ParseGameServerLog(
      "2026-10-18T16:56:32.279Z,19.279047,8aad96c0,7 [FLog::Network] UDMUX "
      "Address = 128.116.2.33, Port = 53358 | RCC Server Address = "
      "10.37.6.90, Port = 53358");
  ASSERT_TRUE(endpoint.has_value());
  EXPECT_EQ(endpoint->address, "128.116.2.33");
  EXPECT_EQ(endpoint->port, 53358);
}

TEST(ServerInfoTest, IgnoresOtherLinesAndMalformedAddresses) {
  EXPECT_FALSE(ParseGameServerLog("[FLog::Network] serverId: 1.2.3.4|5"));
  EXPECT_FALSE(ParseGameServerLog("UDMUX Address = 999.1.1.1, Port = 5"));
  EXPECT_FALSE(ParseGameServerLog("UDMUX Address = 1.2.3.4, Port = 0"));
  EXPECT_FALSE(ParseGameServerLog("UDMUX Address = 1.2.3.4, Port = 70000"));
  EXPECT_FALSE(ParseGameServerLog("UDMUX Address = 1.2.3.4"));
}

TEST(ServerInfoTest, NamesThePlaceByCityAndCountry) {
  const auto place = [](const char* text) {
    return ParseIpInfoPlace(nlohmann::json::parse(text, nullptr, false));
  };
  EXPECT_EQ(place(R"({"ip":"128.116.2.33","city":"Ashburn",)"
                  R"("region":"Virginia","country":"US"})"),
            "Ashburn, US");
  EXPECT_EQ(place(R"({"region":"Virginia","country":"US"})"),
            "Virginia, US");
  EXPECT_EQ(place(R"({"country":"DE"})"), "DE");
  EXPECT_EQ(place(R"({"ip":"10.0.0.1","bogon":true})"), "");
  EXPECT_EQ(place(R"({"city":7,"country":"US"})"), "US");
  EXPECT_EQ(place("not json"), "");
  EXPECT_EQ(place("[1,2]"), "");
}

TEST(ServerInfoTest, PingFollowsTheDelayToTheServersAcknowledgments) {
  AckPingEstimator estimator;
  std::mt19937 random(7);
  std::uniform_int_distribution<int> jitter(0, 6);
  std::int64_t now = 0;
  std::deque<std::int64_t> acks;
  for (int index = 0; index < 400; ++index) {
    const std::int64_t next = now + (8 + jitter(random)) * kMs;
    while (!acks.empty() && acks.front() < next) {
      estimator.OnReceive(acks.front(), AckPingEstimator::kAckBytes);
      estimator.OnReceive(acks.front() + 1 * kMs, 1134);
      acks.pop_front();
    }
    now = next;
    estimator.OnSend(now);
    if (index % 3 == 0) {
      acks.push_back(now + 38 * kMs);
    }
  }
  const std::optional<int> ping = estimator.EstimateMs(now);
  ASSERT_TRUE(ping.has_value());
  EXPECT_NEAR(*ping, 38, 2);
}

TEST(ServerInfoTest, PingIsUnknownWithoutRecentAcknowledgments) {
  AckPingEstimator estimator;
  EXPECT_FALSE(estimator.EstimateMs(0).has_value());
  for (int index = 0; index < 200; ++index) {
    estimator.OnSend(index * 10 * kMs);
    estimator.OnReceive(index * 10 * kMs + 20 * kMs, 1134);
  }
  EXPECT_FALSE(estimator.EstimateMs(2000 * kMs).has_value());

  for (int index = 0; index < 200; ++index) {
    estimator.OnSend(index * 10 * kMs);
    estimator.OnReceive(index * 10 * kMs + 20 * kMs,
                        AckPingEstimator::kAckBytes);
  }
  EXPECT_TRUE(estimator.EstimateMs(2000 * kMs).has_value());
  EXPECT_FALSE(estimator.EstimateMs(9000 * kMs).has_value());
}

TEST(ServerInfoTest, ResetForgetsThePreviousServer) {
  AckPingEstimator estimator;
  for (int index = 0; index < 200; ++index) {
    estimator.OnSend(index * 10 * kMs);
    estimator.OnReceive(index * 10 * kMs + 20 * kMs,
                        AckPingEstimator::kAckBytes);
  }
  estimator.Reset();
  EXPECT_FALSE(estimator.EstimateMs(2000 * kMs).has_value());
}

TEST(ServerInfoTest, RatesPingForTheSignalBars) {
  EXPECT_EQ(RatePing(38), PingRating::kGood);
  EXPECT_EQ(RatePing(79), PingRating::kGood);
  EXPECT_EQ(RatePing(80), PingRating::kFair);
  EXPECT_EQ(RatePing(159), PingRating::kFair);
  EXPECT_EQ(RatePing(160), PingRating::kPoor);
}

TEST(ServerInfoTest, PlacesTheHudAtEachPosition) {
  const auto at = [](OverlayPosition position) {
    return PlaceOverlay(position, 1280, 720, 300, 32, 44, 24);
  };
  EXPECT_EQ(at(OverlayPosition::kTopCenter), (OverlayPoint{490, 44}));
  EXPECT_EQ(at(OverlayPosition::kTopRight), (OverlayPoint{956, 44}));
  EXPECT_EQ(at(OverlayPosition::kBottomLeft), (OverlayPoint{24, 664}));
  EXPECT_EQ(at(OverlayPosition::kBottomCenter), (OverlayPoint{490, 664}));
  EXPECT_EQ(at(OverlayPosition::kBottomRight), (OverlayPoint{956, 664}));
  EXPECT_FALSE(ParseOverlayPosition("top_left").has_value());
}

TEST(ServerInfoTest, KeepsTheHudInsideASmallWindow) {
  EXPECT_EQ(PlaceOverlay(OverlayPosition::kTopRight, 200, 20, 300, 30, 44, 24),
            (OverlayPoint{0, 0}));
}

TEST(ServerInfoTest, CentersThePopupAndATopCenterHudAsOneRow) {
  const OverlayLayout layout = LayOutOverlay(
      OverlayPosition::kTopCenter, 1000, 600, OverlaySize{200, 44},
      OverlaySize{120, 44}, 1.0F);
  EXPECT_EQ(layout.popup, (OverlayRect{336, 12, 200, 44}));
  EXPECT_EQ(layout.hud, (OverlayRect{544, 12, 120, 44}));
  EXPECT_EQ(layout.bounds, (OverlayRect{336, 12, 328, 44}));

  const OverlayLayout hud_only = LayOutOverlay(
      OverlayPosition::kTopCenter, 1000, 600, OverlaySize{},
      OverlaySize{120, 44}, 1.0F);
  EXPECT_EQ(hud_only.hud, (OverlayRect{440, 12, 120, 44}));
}

TEST(ServerInfoTest, SlidesTheHudBackToTheCenterAsThePopupLeaves) {
  const OverlayLayout half = LayOutOverlay(
      OverlayPosition::kTopCenter, 1000, 600, OverlaySize{200, 44},
      OverlaySize{120, 44}, 1.0F, 0.5F);
  EXPECT_EQ(half.hud, (OverlayRect{492, 12, 120, 44}));
  EXPECT_EQ(half.popup, (OverlayRect{284, 12, 200, 44}));

  const OverlayLayout gone = LayOutOverlay(
      OverlayPosition::kTopCenter, 1000, 600, OverlaySize{200, 44},
      OverlaySize{120, 44}, 1.0F, 0.0F);
  EXPECT_EQ(gone.hud, (OverlayRect{440, 12, 120, 44}));
}

class ConfigFile {
 public:
  ConfigFile() {
    char pattern[] = "/tmp/mocktail_server_info_XXXXXX";
    directory_ = mkdtemp(pattern);
    path_ = directory_ / "config.yaml";
  }
  ~ConfigFile() { std::filesystem::remove_all(directory_); }

  void Write(const std::string& overlay) {
    stamp_ += std::chrono::seconds(1);
    WriteInSameSecond(overlay);
  }
  void WriteInSameSecond(const std::string& overlay) {
    std::ofstream(path_, std::ios::binary) << "version: 1\noverlay:\n"
                                           << overlay;
    std::filesystem::last_write_time(path_, stamp_);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path directory_;
  std::filesystem::path path_;
  std::filesystem::file_time_type stamp_ =
      std::filesystem::file_time_type::clock::now();
};

TEST(ServerInfoTest, ReloadAppliesOnlyTheOverlaySettingsTheFileChanged) {
  ConfigFile file;
  file.Write("  fps: false\n  position: bottom_left\n");
  OverlayConfig launched;
  launched.ping = true;
  OverlayConfigReloader reloader(file.path(), launched);
  std::string error;
  EXPECT_FALSE(reloader.Reload(launched, &error).has_value());

  file.Write("  fps: false\n  position: top_right\n");
  const std::optional<OverlayConfig> moved = reloader.Reload(launched, &error);
  ASSERT_TRUE(moved.has_value());
  EXPECT_EQ(moved->position, OverlayPosition::kTopRight);
  EXPECT_TRUE(moved->ping);
  EXPECT_FALSE(moved->fps);
  EXPECT_FALSE(reloader.Reload(*moved, &error).has_value());
  EXPECT_TRUE(error.empty());

  file.Write("  fps: banana\n  position: top_right\n");
  EXPECT_FALSE(reloader.Reload(*moved, &error).has_value());
  EXPECT_NE(error.find("overlay.fps"), std::string::npos);

  file.Write("  fps: true\n  position: top_right\n");
  const std::optional<OverlayConfig> shown = reloader.Reload(*moved, &error);
  ASSERT_TRUE(shown.has_value());
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(shown->fps);
  EXPECT_TRUE(shown->ping);
  EXPECT_EQ(shown->position, OverlayPosition::kTopRight);

  file.WriteInSameSecond("  fps: true\n  position: top_center\n");
  const std::optional<OverlayConfig> same_second =
      reloader.Reload(*shown, &error);
  ASSERT_TRUE(same_second.has_value());
  EXPECT_EQ(same_second->position, OverlayPosition::kTopCenter);
}

TEST(ServerInfoTest, DropsThePopupWhenTheCombinedImageIsTooLarge) {
  const OverlayLayout layout = LayOutOverlay(
      OverlayPosition::kBottomRight, 7680, 4320, OverlaySize{400, 88},
      OverlaySize{240, 88}, 2.0F);
  EXPECT_EQ(layout.popup, OverlayRect{});
  EXPECT_EQ(layout.hud, (OverlayRect{7392, 4184, 240, 88}));
  EXPECT_EQ(layout.bounds, layout.hud);

  const OverlayLayout uhd = LayOutOverlay(
      OverlayPosition::kBottomRight, 3840, 2160, OverlaySize{400, 88},
      OverlaySize{240, 88}, 2.0F);
  EXPECT_FALSE(uhd.popup == OverlayRect{});
}

TEST(ServerInfoTest, PutsATopRightHudBelowTheTopBarButtons) {
  const OverlayLayout layout = LayOutOverlay(
      OverlayPosition::kTopRight, 1000, 600, OverlaySize{200, 44},
      OverlaySize{120, 44}, 1.0F);
  EXPECT_EQ(layout.popup, (OverlayRect{400, 12, 200, 44}));
  EXPECT_EQ(layout.hud, (OverlayRect{856, 64, 120, 44}));
}

TEST(ServerInfoTest, BoundsCoverThePopupAndAHudInACorner) {
  const OverlayLayout layout = LayOutOverlay(
      OverlayPosition::kBottomRight, 1000, 600, OverlaySize{200, 44},
      OverlaySize{120, 44}, 2.0F);
  EXPECT_EQ(layout.popup, (OverlayRect{400, 24, 200, 44}));
  EXPECT_EQ(layout.hud, (OverlayRect{832, 508, 120, 44}));
  EXPECT_EQ(layout.bounds, (OverlayRect{400, 24, 552, 528}));
}

TEST(ServerInfoTest, LaysOutOnlyWhatIsShown) {
  const OverlayLayout hud_only = LayOutOverlay(
      OverlayPosition::kBottomLeft, 1000, 600, OverlaySize{},
      OverlaySize{120, 44}, 1.0F);
  EXPECT_EQ(hud_only.popup, (OverlayRect{}));
  EXPECT_EQ(hud_only.hud, (OverlayRect{24, 532, 120, 44}));
  EXPECT_EQ(hud_only.bounds, hud_only.hud);

  const OverlayLayout popup_only = LayOutOverlay(
      OverlayPosition::kBottomLeft, 1000, 600, OverlaySize{200, 44},
      OverlaySize{}, 1.0F);
  EXPECT_EQ(popup_only.hud, (OverlayRect{}));
  EXPECT_EQ(popup_only.bounds, popup_only.popup);
}

TEST(ServerInfoTest, FadesOverTheFadeTime) {
  EXPECT_FLOAT_EQ(StepFade(0.0F, true, 50000, 200000), 0.25F);
  EXPECT_FLOAT_EQ(StepFade(0.25F, true, 200000, 200000), 1.0F);
  EXPECT_FLOAT_EQ(StepFade(1.0F, false, 100000, 200000), 0.5F);
  EXPECT_FLOAT_EQ(StepFade(0.1F, false, 50000, 200000), 0.0F);
  EXPECT_FLOAT_EQ(StepFade(0.5F, true, -10, 200000), 0.5F);
}

}
}
