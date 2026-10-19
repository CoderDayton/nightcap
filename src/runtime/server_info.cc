#include "runtime/server_info.h"

#include <arpa/inet.h>
#include <sys/stat.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

#include <nlohmann/json.hpp>

#include "runtime/environment.h"
#include "runtime/runtime_config_file.h"

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

OverlayLayout LayOutOverlay(OverlayPosition hud_position, int view_width,
                            int view_height, OverlaySize popup,
                            OverlaySize hud, float scale,
                            float popup_presence) {
  constexpr float kTopBarMargin = 12.0F;
  constexpr float kBelowTopBarMargin = 64.0F;
  constexpr float kEdgeMargin = 24.0F;
  const auto scaled = [scale](float value) {
    return static_cast<int>(std::lround(value * scale));
  };
  const auto place = [&](OverlayPosition position, OverlaySize size,
                         float top_margin) {
    if (size.width <= 0 || size.height <= 0) {
      return OverlayRect{};
    }
    const OverlayPoint at =
        PlaceOverlay(position, view_width, view_height, size.width,
                     size.height, scaled(top_margin), scaled(kEdgeMargin));
    return OverlayRect{at.x, at.y, size.width, size.height};
  };
  constexpr float kBesideGap = 8.0F;
  const bool shares_row = hud_position == OverlayPosition::kTopCenter;
  OverlayLayout layout;
  layout.popup = place(OverlayPosition::kTopCenter, popup, kTopBarMargin);
  layout.hud = place(hud_position, hud,
                     shares_row ? kTopBarMargin : kBelowTopBarMargin);
  const OverlayRect empty{};
  if (shares_row && !(layout.popup == empty) && !(layout.hud == empty)) {
    const int gap = scaled(kBesideGap);
    const int claimed = static_cast<int>(std::lround(
        (layout.popup.width + gap) * std::clamp(popup_presence, 0.0F, 1.0F)));
    const int left = std::max(0, (view_width - claimed - hud.width) / 2);
    layout.hud.x = left + claimed;
    layout.popup.x = layout.hud.x - gap - layout.popup.width;
  }
  if (layout.popup == empty) {
    layout.bounds = layout.hud;
  } else if (layout.hud == empty) {
    layout.bounds = layout.popup;
  } else {
    const int left = std::min(layout.popup.x, layout.hud.x);
    const int top = std::min(layout.popup.y, layout.hud.y);
    const int right = std::max(layout.popup.x + layout.popup.width,
                               layout.hud.x + layout.hud.width);
    const int bottom = std::max(layout.popup.y + layout.popup.height,
                                layout.hud.y + layout.hud.height);
    layout.bounds = {left, top, right - left, bottom - top};
    constexpr std::int64_t kMaximumPixels = 16LL * 1024 * 1024;
    if (static_cast<std::int64_t>(layout.bounds.width) * layout.bounds.height >
        kMaximumPixels) {
      layout.popup = empty;
      layout.bounds = layout.hud;
    }
  }
  return layout;
}

float StepFade(float opacity, bool shown, std::int64_t elapsed_us,
               std::int64_t fade_us) {
  const float step =
      fade_us > 0 ? static_cast<float>(std::max<std::int64_t>(elapsed_us, 0)) /
                        static_cast<float>(fade_us)
                  : 1.0F;
  return std::clamp(opacity + (shown ? step : -step), 0.0F, 1.0F);
}

namespace {

class NoEnvironment final : public Environment {
 public:
  std::optional<std::string> Get(std::string_view) const override {
    return std::nullopt;
  }
};

OverlayConfigReloader::Stamp StampOf(const std::filesystem::path& file) {
  struct stat status {};
  if (stat(file.c_str(), &status) != 0) {
    return {};
  }
  return {static_cast<std::int64_t>(status.st_mtim.tv_sec),
          static_cast<std::int64_t>(status.st_mtim.tv_nsec),
          static_cast<std::int64_t>(status.st_size),
          static_cast<std::uint64_t>(status.st_ino)};
}

bool SameOverlay(const OverlayConfig& a, const OverlayConfig& b) {
  return a.location == b.location && a.fps == b.fps && a.ping == b.ping &&
         a.position == b.position;
}

}

OverlayConfigReloader::OverlayConfigReloader(std::filesystem::path file,
                                             const OverlayConfig& launched)
    : file_(std::move(file)),
      written_(StampOf(file_)),
      seen_(Read(nullptr).value_or(launched)) {}

std::optional<OverlayConfig> OverlayConfigReloader::Read(std::string* error) {
  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(NoEnvironment(), file_);
  if (!loaded) {
    if (error != nullptr) {
      *error = std::move(loaded.error);
    }
    return std::nullopt;
  }
  return loaded.config.overlay();
}

std::optional<OverlayConfig> OverlayConfigReloader::Reload(
    const OverlayConfig& current, std::string* error) {
  error->clear();
  const Stamp written = StampOf(file_);
  if (written == written_) {
    return std::nullopt;
  }
  written_ = written;
  const std::optional<OverlayConfig> read = Read(error);
  if (!read.has_value()) {
    return std::nullopt;
  }
  OverlayConfig next = current;
  const auto take_changed = [&](auto OverlayConfig::*field) {
    if (*read.*field != seen_.*field) {
      next.*field = *read.*field;
    }
  };
  take_changed(&OverlayConfig::location);
  take_changed(&OverlayConfig::fps);
  take_changed(&OverlayConfig::ping);
  take_changed(&OverlayConfig::position);
  seen_ = *read;
  if (SameOverlay(next, current)) {
    return std::nullopt;
  }
  return next;
}

}
