#include "runtime/server_info.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <random>

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

TEST(ServerInfoTest, KeepsTheAlwaysShownBubbleSteady) {
  using Mode = ServerInfoMode;
  EXPECT_EQ(FindSteadyBubble(Mode::kAlways, Mode::kNotice),
            SteadyBubble::kServerInfo);
  EXPECT_EQ(FindSteadyBubble(Mode::kNotice, Mode::kAlways), SteadyBubble::kFps);
  EXPECT_EQ(FindSteadyBubble(Mode::kAlways, Mode::kAlways), SteadyBubble::kBoth);
  EXPECT_EQ(FindSteadyBubble(Mode::kNotice, Mode::kNotice), SteadyBubble::kBoth);
  EXPECT_EQ(FindSteadyBubble(Mode::kAlways, Mode::kOff), SteadyBubble::kBoth);
}

TEST(ServerInfoTest, FadesOverTheFadeTime) {
  EXPECT_FLOAT_EQ(StepFade(0.0F, true, 50000, 200000), 0.25F);
  EXPECT_FLOAT_EQ(StepFade(0.25F, true, 200000, 200000), 1.0F);
  EXPECT_FLOAT_EQ(StepFade(1.0F, false, 100000, 200000), 0.5F);
  EXPECT_FLOAT_EQ(StepFade(0.1F, false, 50000, 200000), 0.0F);
  EXPECT_FLOAT_EQ(StepFade(0.5F, true, -10, 200000), 0.5F);
}

TEST(ServerInfoTest, PutsTheSteadyBubbleOnTheAnchoredSide) {
  // Left-anchored: the steady bubble is on the left.
  EXPECT_FALSE(FpsBubbleFirst(OverlayPosition::kBottomLeft,
                              SteadyBubble::kServerInfo));
  EXPECT_TRUE(FpsBubbleFirst(OverlayPosition::kBottomLeft, SteadyBubble::kFps));
  // Right-anchored: the steady bubble is on the right.
  EXPECT_TRUE(FpsBubbleFirst(OverlayPosition::kTopRight,
                             SteadyBubble::kServerInfo));
  EXPECT_FALSE(FpsBubbleFirst(OverlayPosition::kBottomRight,
                              SteadyBubble::kFps));
  // Centered, or nothing steady: the frame rate goes first.
  EXPECT_TRUE(FpsBubbleFirst(OverlayPosition::kTopCenter, SteadyBubble::kFps));
  EXPECT_TRUE(FpsBubbleFirst(OverlayPosition::kBottomLeft, SteadyBubble::kBoth));
  EXPECT_TRUE(FpsBubbleFirst(OverlayPosition::kTopRight, SteadyBubble::kBoth));
}

}
}
