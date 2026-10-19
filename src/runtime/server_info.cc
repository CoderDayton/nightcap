#include "runtime/server_info.h"

#include <arpa/inet.h>

#include <algorithm>
#include <charconv>

#include <nlohmann/json.hpp>

namespace mocktail::runtime {

std::optional<GameServerEndpoint> ParseGameServerLog(std::string_view message) {
  constexpr std::string_view kAddress = "UDMUX Address = ";
  constexpr std::string_view kPort = "Port = ";
  const std::size_t address_begin = message.find(kAddress);
  if (address_begin == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view rest = message.substr(address_begin + kAddress.size());
  const std::size_t comma = rest.find(',');
  if (comma == std::string_view::npos || comma > INET_ADDRSTRLEN) {
    return std::nullopt;
  }
  GameServerEndpoint endpoint;
  endpoint.address = std::string(rest.substr(0, comma));
  in_addr parsed{};
  if (inet_pton(AF_INET, endpoint.address.c_str(), &parsed) != 1) {
    return std::nullopt;
  }
  const std::size_t port_begin = rest.find(kPort, comma);
  if (port_begin == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view digits = rest.substr(port_begin + kPort.size());
  unsigned int port = 0;
  const auto [end, error] =
      std::from_chars(digits.data(), digits.data() + digits.size(), port);
  if (error != std::errc() || end == digits.data() || port == 0 ||
      port > 65535) {
    return std::nullopt;
  }
  endpoint.port = static_cast<std::uint16_t>(port);
  return endpoint;
}

std::string ParseIpInfoPlace(const nlohmann::json& document) {
  if (!document.is_object()) {
    return {};
  }
  const auto text = [&document](const char* key) {
    const auto found = document.find(key);
    return found != document.end() && found->is_string()
               ? found->get<std::string>()
               : std::string();
  };
  const auto bogon = document.find("bogon");
  if (bogon != document.end() && bogon->is_boolean() && bogon->get<bool>()) {
    return {};
  }
  std::string place = text("city");
  if (place.empty()) {
    place = text("region");
  }
  const std::string country = text("country");
  if (place.empty()) {
    return country;
  }
  return country.empty() ? place : place + ", " + country;
}

void AckPingEstimator::OnSend(std::int64_t now_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  sends_[send_next_] = now_us;
  send_next_ = (send_next_ + 1) % kSendHistory;
  send_count_ = std::min(send_count_ + 1, kSendHistory);
}

void AckPingEstimator::OnReceive(std::int64_t now_us, std::size_t bytes) {
  if (bytes != kAckBytes) {
    return;
  }
  constexpr std::int64_t kBinUs = kBinMs * 1000;
  const std::int64_t index = now_us / 1000000;
  std::lock_guard<std::mutex> lock(mutex_);
  Second& second = seconds_[static_cast<std::size_t>(index % kWindowSeconds)];
  if (second.index != index) {
    second.index = index;
    second.bins.fill(0);
  }
  for (std::size_t slot = 0; slot < send_count_; ++slot) {
    const std::int64_t delay = now_us - sends_[slot];
    if (delay <= 0 || delay >= kBins * kBinUs) {
      continue;
    }
    std::uint16_t& bin = second.bins[static_cast<std::size_t>(delay / kBinUs)];
    if (bin != UINT16_MAX) {
      ++bin;
    }
  }
}

std::optional<int> AckPingEstimator::EstimateMs(std::int64_t now_us) const {
  const std::int64_t current = now_us / 1000000;
  std::array<int, kBins> totals{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Second& second : seconds_) {
      if (second.index < 0 || second.index > current ||
          second.index <= current - kWindowSeconds) {
        continue;
      }
      for (int bin = 0; bin < kBins; ++bin) {
        totals[bin] += second.bins[bin];
      }
    }
  }
  int best = 0;
  int best_total = 0;
  for (int bin = 1; bin + 1 < kBins; ++bin) {
    const int total = totals[bin - 1] + totals[bin] + totals[bin + 1];
    if (total > best_total) {
      best = bin;
      best_total = total;
    }
  }
  if (best_total < kMinimumPeak) {
    return std::nullopt;
  }
  double weighted = 0.0;
  for (int bin = best - 1; bin <= best + 1; ++bin) {
    weighted += static_cast<double>(totals[bin]) * (bin * kBinMs + kBinMs / 2.0);
  }
  return static_cast<int>(weighted / best_total + 0.5);
}

void AckPingEstimator::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  send_count_ = 0;
  send_next_ = 0;
  for (Second& second : seconds_) {
    second.index = -1;
    second.bins.fill(0);
  }
}

PingRating RatePing(int ping_ms) {
  if (ping_ms < 80) {
    return PingRating::kGood;
  }
  return ping_ms < 160 ? PingRating::kFair : PingRating::kPoor;
}

OverlayPoint PlaceOverlay(OverlayPosition position, int view_width,
                          int view_height, int width, int height,
                          int top_margin, int margin) {
  int x = margin;
  switch (position) {
    case OverlayPosition::kTopCenter:
    case OverlayPosition::kBottomCenter:
      x = (view_width - width) / 2;
      break;
    case OverlayPosition::kTopRight:
    case OverlayPosition::kBottomRight:
      x = view_width - width - margin;
      break;
    case OverlayPosition::kBottomLeft:
      break;
  }
  const bool bottom = position == OverlayPosition::kBottomLeft ||
                      position == OverlayPosition::kBottomCenter ||
                      position == OverlayPosition::kBottomRight;
  int y = bottom ? view_height - height - margin : top_margin;
  x = std::clamp(x, 0, std::max(0, view_width - width));
  y = std::clamp(y, 0, std::max(0, view_height - height));
  return {x, y};
}

}
