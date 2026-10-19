#ifndef MOCKTAIL_RUNTIME_SERVER_INFO_H_
#define MOCKTAIL_RUNTIME_SERVER_INFO_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "runtime/runtime_config.h"

namespace mocktail::runtime {

struct GameServerEndpoint {
  std::string address;
  std::uint16_t port = 0;
};

std::optional<GameServerEndpoint> ParseGameServerLog(std::string_view message);

std::string ParseIpInfoPlace(const nlohmann::json& document);

class AckPingEstimator {
 public:
  static constexpr std::size_t kAckBytes = 54;

  void OnSend(std::int64_t now_us);
  void OnReceive(std::int64_t now_us, std::size_t bytes);
  std::optional<int> EstimateMs(std::int64_t now_us) const;
  void Reset();

 private:
  static constexpr int kBinMs = 2;
  static constexpr int kBins = 200;
  static constexpr int kWindowSeconds = 3;
  static constexpr std::size_t kSendHistory = 128;
  static constexpr int kMinimumPeak = 12;

  struct Second {
    std::int64_t index = -1;
    std::array<std::uint16_t, kBins> bins{};
  };

  mutable std::mutex mutex_;
  std::array<std::int64_t, kSendHistory> sends_{};
  std::size_t send_count_ = 0;
  std::size_t send_next_ = 0;
  std::array<Second, kWindowSeconds> seconds_{};
};

enum class PingRating { kGood, kFair, kPoor };
PingRating RatePing(int ping_ms);

struct OverlayPoint {
  int x = 0;
  int y = 0;
  bool operator==(const OverlayPoint& other) const {
    return x == other.x && y == other.y;
  }
};

OverlayPoint PlaceOverlay(OverlayPosition position, int view_width,
                          int view_height, int width, int height,
                          int top_margin, int margin);

}

#endif
