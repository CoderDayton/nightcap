#include "runtime/server_info_overlay.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <arpa/inet.h>
#include <fontconfig/fontconfig.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>

#include "compat/bionic_socket_runtime.h"
#include "mocktail/platform/android_log_observer.h"
#include "runtime/https_json.h"
#include "runtime/server_info.h"
#include "window/window.h"

namespace mocktail::runtime {
namespace {

constexpr std::int64_t kSecondUs = 1000000;
constexpr std::int64_t kInGameSilenceUs = 5 * kSecondUs;
constexpr std::int64_t kNoticeUs = 6 * kSecondUs;
constexpr std::int64_t kNoticeWaitForPlaceUs = 3 * kSecondUs;
constexpr std::int64_t kStatsLogUs = 30 * kSecondUs;
constexpr auto kTick = std::chrono::milliseconds(250);
constexpr float kTopMargin = 40.0F;
constexpr float kEdgeMargin = 24.0F;

struct Color {
  float r;
  float g;
  float b;
  float a;
};

constexpr Color Hex(std::uint32_t rgb, float alpha = 1.0F) {
  return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0F,
          static_cast<float>((rgb >> 8) & 0xFF) / 255.0F,
          static_cast<float>(rgb & 0xFF) / 255.0F, alpha};
}

constexpr Color kPanel = Hex(0x191B1D, 0.85F);
constexpr Color kSeparator = Hex(0xFFFFFF, 0.2F);
constexpr Color kInk = Hex(0xFFFFFF);
constexpr Color kMuted = Hex(0xFFFFFF, 0.72F);
constexpr Color kBarOff = Hex(0xFFFFFF, 0.25F);
constexpr Color kGood = Hex(0x3FD47D);
constexpr Color kFair = Hex(0xF5C542);
constexpr Color kPoor = Hex(0xFF5A5F);

std::int64_t MonotonicMicros() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * kSecondUs +
         now.tv_nsec / 1000;
}

struct Shared {
  std::atomic<std::uint64_t> target{0};
  std::atomic<std::int64_t> last_packet_us{0};
  AckPingEstimator ping;
  std::mutex join_mutex;
  std::string join_address;
  std::uint64_t join_generation = 0;
};

Shared& State() {
  static Shared* shared = new Shared();
  return *shared;
}

std::uint64_t TargetKey(std::uint32_t network_address, std::uint16_t port) {
  return (static_cast<std::uint64_t>(network_address) << 16) | port;
}

void ObserveLog(int, const char*, const char* message) {
  if (message == nullptr) {
    return;
  }
  const std::optional<GameServerEndpoint> endpoint =
      ParseGameServerLog(message);
  if (!endpoint.has_value()) {
    return;
  }
  in_addr address{};
  if (inet_pton(AF_INET, endpoint->address.c_str(), &address) != 1) {
    return;
  }
  Shared& shared = State();
  shared.ping.Reset();
  shared.last_packet_us.store(0, std::memory_order_relaxed);
  shared.target.store(TargetKey(address.s_addr, endpoint->port),
                      std::memory_order_release);
  std::lock_guard<std::mutex> lock(shared.join_mutex);
  shared.join_address = endpoint->address;
  ++shared.join_generation;
}

void ObserveDatagram(bool outgoing, const sockaddr* peer,
                     socklen_t peer_length, ssize_t bytes) {
  if (peer->sa_family != AF_INET || peer_length < sizeof(sockaddr_in)) {
    return;
  }
  Shared& shared = State();
  const std::uint64_t target = shared.target.load(std::memory_order_acquire);
  const auto* address = reinterpret_cast<const sockaddr_in*>(peer);
  if (target == 0 ||
      TargetKey(address->sin_addr.s_addr, ntohs(address->sin_port)) !=
          target) {
    return;
  }
  const std::int64_t now = MonotonicMicros();
  if (outgoing) {
    shared.ping.OnSend(now);
    return;
  }
  shared.ping.OnReceive(now, static_cast<std::size_t>(bytes));
  shared.last_packet_us.store(now, std::memory_order_relaxed);
}

std::mutex g_instance_mutex;
ServerInfoOverlay* g_instance = nullptr;
std::atomic<bool> g_may_present{false};

class Canvas {
 public:
  Canvas(int width, int height)
      : width_(width), height_(height),
        pixels_(static_cast<std::size_t>(width) * height * 4, 0.0F) {}

  int width() const { return width_; }
  int height() const { return height_; }

  void Blend(int x, int y, const Color& color, float coverage) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
      return;
    }
    const float alpha = color.a * std::clamp(coverage, 0.0F, 1.0F);
    if (alpha <= 0.0F) {
      return;
    }
    float* pixel = &pixels_[(static_cast<std::size_t>(y) * width_ + x) * 4];
    pixel[0] = color.r * alpha + pixel[0] * (1.0F - alpha);
    pixel[1] = color.g * alpha + pixel[1] * (1.0F - alpha);
    pixel[2] = color.b * alpha + pixel[2] * (1.0F - alpha);
    pixel[3] = alpha + pixel[3] * (1.0F - alpha);
  }

  template <typename Distance>
  void Fill(float left, float top, float right, float bottom,
            const Color& color, Distance distance) {
    const int x0 = std::max(0, static_cast<int>(std::floor(left)));
    const int y0 = std::max(0, static_cast<int>(std::floor(top)));
    const int x1 = std::min(width_, static_cast<int>(std::ceil(right)));
    const int y1 = std::min(height_, static_cast<int>(std::ceil(bottom)));
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        Blend(x, y, color, 0.5F - distance(x + 0.5F, y + 0.5F));
      }
    }
  }

  std::vector<std::uint8_t> StraightRgba() const {
    std::vector<std::uint8_t> bytes(pixels_.size());
    for (std::size_t index = 0; index < pixels_.size(); index += 4) {
      const float alpha = pixels_[index + 3];
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const float straight =
            alpha > 0.0F ? pixels_[index + channel] / alpha : 0.0F;
        bytes[index + channel] = static_cast<std::uint8_t>(
            std::lround(std::clamp(straight, 0.0F, 1.0F) * 255.0F));
      }
      bytes[index + 3] =
          static_cast<std::uint8_t>(std::lround(alpha * 255.0F));
    }
    return bytes;
  }

 private:
  int width_;
  int height_;
  std::vector<float> pixels_;
};

float RoundedBox(float x, float y, float center_x, float center_y,
                 float half_width, float half_height, float radius) {
  const float qx = std::fabs(x - center_x) - half_width + radius;
  const float qy = std::fabs(y - center_y) - half_height + radius;
  return std::hypot(std::max(qx, 0.0F), std::max(qy, 0.0F)) +
         std::min(std::max(qx, qy), 0.0F) - radius;
}

float Circle(float x, float y, float center_x, float center_y, float radius) {
  return std::hypot(x - center_x, y - center_y) - radius;
}

float Triangle(float x, float y, const float (&points)[3][2]) {
  float distance = std::numeric_limits<float>::max();
  float sign = 1.0F;
  for (int index = 0, previous = 2; index < 3; previous = index++) {
    const float edge_x = points[previous][0] - points[index][0];
    const float edge_y = points[previous][1] - points[index][1];
    const float to_x = x - points[index][0];
    const float to_y = y - points[index][1];
    const float along = std::clamp(
        (to_x * edge_x + to_y * edge_y) / (edge_x * edge_x + edge_y * edge_y),
        0.0F, 1.0F);
    const float off_x = to_x - edge_x * along;
    const float off_y = to_y - edge_y * along;
    distance = std::min(distance, off_x * off_x + off_y * off_y);
    const bool above = y >= points[index][1];
    const bool below = y < points[previous][1];
    const bool left = edge_x * to_y > edge_y * to_x;
    if ((above && below && left) || (!above && !below && !left)) {
      sign = -sign;
    }
  }
  return sign * std::sqrt(distance);
}

struct Fonts {
  TTF_Font* title = nullptr;
  TTF_Font* detail = nullptr;
  float scale = 0.0F;

  void Close() {
    if (title != nullptr) {
      TTF_CloseFont(title);
    }
    if (detail != nullptr) {
      TTF_CloseFont(detail);
    }
    title = nullptr;
    detail = nullptr;
    scale = 0.0F;
  }
};

std::string FindFont(const char* family, int weight) {
  FcPattern* pattern = FcPatternCreate();
  if (pattern == nullptr) {
    return {};
  }
  FcPatternAddString(pattern, FC_FAMILY,
                     reinterpret_cast<const FcChar8*>(family));
  FcPatternAddInteger(pattern, FC_WEIGHT, weight);
  FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);
  FcResult result = FcResultNoMatch;
  FcPattern* matched = FcFontMatch(nullptr, pattern, &result);
  FcPatternDestroy(pattern);
  std::string path;
  FcChar8* file = nullptr;
  if (matched != nullptr &&
      FcPatternGetString(matched, FC_FILE, 0, &file) == FcResultMatch &&
      file != nullptr) {
    path = reinterpret_cast<const char*>(file);
  }
  if (matched != nullptr) {
    FcPatternDestroy(matched);
  }
  return path;
}

std::string RobloxFont(const char* file, int fallback_weight) {
  const char* content = std::getenv("MOCKTAIL_ASSET_PATH");
  if (content != nullptr && content[0] != '\0') {
    const std::string path = std::string(content) + "/fonts/" + file;
    if (access(path.c_str(), R_OK) == 0) {
      return path;
    }
  }
  return FindFont("sans-serif", fallback_weight);
}

bool OpenFonts(Fonts* fonts, float scale) {
  if (fonts->title != nullptr && fonts->scale == scale) {
    return true;
  }
  fonts->Close();
  static const std::string title_file =
      RobloxFont("BuilderSans-Bold.otf", FC_WEIGHT_BOLD);
  static const std::string detail_file =
      RobloxFont("BuilderSans-Medium.otf", FC_WEIGHT_MEDIUM);
  if (title_file.empty() || detail_file.empty()) {
    return false;
  }
  fonts->title = TTF_OpenFont(title_file.c_str(), 14.0F * scale);
  fonts->detail = TTF_OpenFont(detail_file.c_str(), 14.0F * scale);
  if (fonts->title == nullptr || fonts->detail == nullptr) {
    fonts->Close();
    return false;
  }
  fonts->scale = scale;
  return true;
}

int TextWidth(TTF_Font* font, const std::string& text) {
  int width = 0;
  int height = 0;
  if (!TTF_GetStringSize(font, text.c_str(), 0, &width, &height)) {
    return 0;
  }
  return width;
}

void DrawText(Canvas* canvas, TTF_Font* font, const std::string& text,
              float left, float center_y, const Color& color) {
  SDL_Surface* rendered =
      TTF_RenderText_Blended(font, text.c_str(), 0, SDL_Color{255, 255, 255, 255});
  if (rendered == nullptr) {
    return;
  }
  SDL_Surface* surface = SDL_ConvertSurface(rendered, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(rendered);
  if (surface == nullptr) {
    return;
  }
  const int top = static_cast<int>(std::lround(center_y - surface->h / 2.0F));
  const int x0 = static_cast<int>(std::lround(left));
  const auto* pixels = static_cast<const std::uint8_t*>(surface->pixels);
  for (int y = 0; y < surface->h; ++y) {
    const std::uint8_t* row = pixels + static_cast<std::size_t>(y) * surface->pitch;
    for (int x = 0; x < surface->w; ++x) {
      canvas->Blend(x0 + x, top + y, color, row[x * 4 + 3] / 255.0F);
    }
  }
  SDL_DestroySurface(surface);
}

struct Card {
  std::vector<std::uint8_t> rgba;
  int width = 0;
  int height = 0;
};

std::optional<Card> DrawCard(Fonts* fonts, float scale,
                             const std::string& place, int ping_ms, int fps) {
  if (!OpenFonts(fonts, scale)) {
    return std::nullopt;
  }
  const std::string title = place.empty() ? "Unknown location" : place;
  const std::string ping_text =
      (ping_ms >= 0 ? std::to_string(ping_ms) : std::string("--")) + " ms";
  const std::string fps_text =
      (fps >= 0 ? std::to_string(fps) : std::string("--")) + " fps";
  const float unit = scale;
  const float height = std::round(40.0F * unit);
  const float pad_left = 14.0F * unit;
  const float pad_right = 14.0F * unit;
  const float pin_width = 10.0F * unit;
  const float pin_gap = 7.0F * unit;
  const float min_gap = 10.0F * unit;
  const float separator = 1.0F * unit;
  const float bars_width = 12.0F * unit;
  const float bars_gap = 6.0F * unit;

  const float ping_text_width =
      static_cast<float>(TextWidth(fonts->detail, ping_text));
  const float fps_text_width =
      static_cast<float>(TextWidth(fonts->detail, fps_text));
  const float ping_width = std::max(
      ping_text_width, static_cast<float>(TextWidth(fonts->detail, "999 ms")));
  const float fps_width = std::max(
      fps_text_width, static_cast<float>(TextWidth(fonts->detail, "999 fps")));
  const float fixed = pad_left + pin_width + pin_gap + separator * 2.0F +
                      bars_width + bars_gap + ping_width + fps_width +
                      pad_right;
  const float title_width =
      static_cast<float>(TextWidth(fonts->title, title));
  const float width = std::max(std::round(275.0F * unit),
                               std::ceil(fixed + title_width + min_gap * 4.0F));
  const float gap = (width - fixed - title_width) / 4.0F;
  Canvas canvas(static_cast<int>(std::ceil(width)),
                static_cast<int>(std::ceil(height)));
  const float center_y = height / 2.0F;
  canvas.Fill(0, 0, width, height, kPanel, [&](float x, float y) {
    return RoundedBox(x, y, width / 2.0F, center_y, width / 2.0F,
                      height / 2.0F, 8.0F * unit);
  });

  float cursor = pad_left;
  const float pin_x = cursor + pin_width / 2.0F;
  const float head_y = center_y - 2.0F * unit;
  const float tip[3][2] = {{pin_x - 4.1F * unit, head_y + 1.8F * unit},
                           {pin_x + 4.1F * unit, head_y + 1.8F * unit},
                           {pin_x, center_y + 6.5F * unit}};
  canvas.Fill(cursor - 1, center_y - 8.0F * unit, cursor + pin_width + 1,
              center_y + 8.0F * unit, kInk, [&](float x, float y) {
                const float pin = std::min(
                    Circle(x, y, pin_x, head_y, 4.6F * unit),
                    Triangle(x, y, tip));
                return std::max(pin,
                                -Circle(x, y, pin_x, head_y, 1.8F * unit));
              });
  cursor += pin_width + pin_gap;
  DrawText(&canvas, fonts->title, title, cursor, center_y, kInk);
  cursor += title_width + gap;

  const auto draw_separator = [&]() {
    const float line_x = cursor + separator / 2.0F;
    canvas.Fill(cursor - 1, center_y - 9.0F * unit, cursor + separator + 1,
                center_y + 9.0F * unit, kSeparator, [&](float x, float y) {
                  return RoundedBox(x, y, line_x, center_y, separator / 2.0F,
                                    8.0F * unit, 0.0F);
                });
    cursor += separator + gap;
  };
  draw_separator();

  Color ping_color = kMuted;
  int lit_bars = 0;
  if (ping_ms >= 0) {
    switch (RatePing(ping_ms)) {
      case PingRating::kGood:
        ping_color = kGood;
        lit_bars = 3;
        break;
      case PingRating::kFair:
        ping_color = kFair;
        lit_bars = 2;
        break;
      case PingRating::kPoor:
        ping_color = kPoor;
        lit_bars = 1;
        break;
    }
  }
  const float bar_heights[3] = {3.6F, 6.4F, 10.0F};
  const float bar_bottom = center_y + 5.0F * unit;
  const float ping_slack = (ping_width - ping_text_width) / 2.0F;
  cursor += ping_slack;
  for (int bar = 0; bar < 3; ++bar) {
    const float bar_width = 3.0F * unit;
    const float bar_left = cursor + bar * 4.5F * unit;
    const float bar_height = bar_heights[bar] * unit;
    canvas.Fill(bar_left - 1, bar_bottom - bar_height - 1,
                bar_left + bar_width + 1, bar_bottom + 1,
                bar < lit_bars ? ping_color : kBarOff, [&](float x, float y) {
                  return RoundedBox(x, y, bar_left + bar_width / 2.0F,
                                    bar_bottom - bar_height / 2.0F,
                                    bar_width / 2.0F, bar_height / 2.0F,
                                    0.8F * unit);
                });
  }
  cursor += bars_width + bars_gap;
  DrawText(&canvas, fonts->detail, ping_text, cursor, center_y, kInk);
  cursor += ping_text_width + ping_slack + gap;
  draw_separator();
  DrawText(&canvas, fonts->detail, fps_text, width - pad_right - fps_text_width,
           center_y, kMuted);

  Card card;
  card.width = canvas.width();
  card.height = canvas.height();
  card.rgba = canvas.StraightRgba();
  return card;
}

std::string LookupPlace(const std::string& address,
                        const std::atomic<bool>* cancel) {
  const std::optional<nlohmann::json> reply =
      GetHttpsJson("https://ipinfo.io/" + address + "/json", cancel);
  return reply.has_value() ? ParseIpInfoPlace(*reply) : std::string();
}

}

bool ServerInfoOverlay::Content::operator==(const Content& other) const {
  return visible == other.visible && place == other.place &&
         ping_ms == other.ping_ms && fps == other.fps &&
         pixel_width == other.pixel_width &&
         pixel_height == other.pixel_height && scale == other.scale;
}

ServerInfoOverlay::ServerInfoOverlay(OverlayConfig config) : config_(config) {}

ServerInfoOverlay::~ServerInfoOverlay() { Stop(); }

void ServerInfoOverlay::Start() {
  if (started_ || config_.server_info == ServerInfoMode::kOff) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    if (g_instance != nullptr) {
      return;
    }
    g_instance = this;
  }
  started_ = true;
  stopping_ = false;
  cancel_lookup_.store(false, std::memory_order_relaxed);
  (void)mocktail_android_log_add_observer(&ObserveLog);
  mocktail_bionic_set_datagram_observer(&ObserveDatagram);
  worker_ = std::thread(&ServerInfoOverlay::Run, this);
  std::fprintf(stderr, "  [server-info] %s at %s\n",
               std::string(ServerInfoModeName(config_.server_info)).c_str(),
               std::string(OverlayPositionName(config_.position)).c_str());
}

void ServerInfoOverlay::Stop() {
  if (!started_) {
    return;
  }
  mocktail_android_log_remove_observer(&ObserveLog);
  mocktail_bionic_set_datagram_observer(nullptr);
  cancel_lookup_.store(true, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  g_may_present.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(g_instance_mutex);
  if (g_instance == this) {
    g_instance = nullptr;
  }
  started_ = false;
}

void ServerInfoOverlay::Run() {
  const bool ttf_ready = TTF_Init();
  Fonts fonts;
  Shared& shared = State();
  std::map<std::string, std::string> places;
  std::uint64_t seen_generation = 0;
  std::string address;
  std::string place;
  std::int64_t joined_at = 0;
  std::int64_t notice_until = 0;
  bool notice_armed = false;
  bool place_pending = false;
  std::uint64_t frames_before = window::PresentedFrameCount();
  std::int64_t frames_at = MonotonicMicros();
  int fps = -1;
  std::int64_t logged_at = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(wait_mutex_);
      if (wake_.wait_for(lock, kTick, [this] { return stopping_; })) {
        break;
      }
    }
    std::int64_t now = MonotonicMicros();
    {
      std::lock_guard<std::mutex> lock(shared.join_mutex);
      if (shared.join_generation != seen_generation) {
        seen_generation = shared.join_generation;
        address = shared.join_address;
        joined_at = now;
        notice_armed = true;
        place_pending = !address.empty();
        place.clear();
      }
    }
    if (place_pending) {
      place_pending = false;
      const auto cached = places.find(address);
      if (cached != places.end()) {
        place = cached->second;
      } else {
        place = LookupPlace(address, &cancel_lookup_);
        if (cancel_lookup_.load(std::memory_order_relaxed)) {
          break;
        }
        if (!place.empty()) {
          places.emplace(address, place);
        }
        std::fprintf(stderr, "  [server-info] server=%s location=%s\n",
                     address.c_str(),
                     place.empty() ? "unknown" : place.c_str());
      }
      now = MonotonicMicros();
    }
    if (now - frames_at >= kSecondUs) {
      const std::uint64_t frames = window::PresentedFrameCount();
      fps = static_cast<int>(std::lround(
          static_cast<double>(frames - frames_before) * kSecondUs /
          static_cast<double>(now - frames_at)));
      frames_before = frames;
      frames_at = now;
    }
    const std::int64_t last_packet =
        shared.last_packet_us.load(std::memory_order_relaxed);
    const bool in_game = last_packet != 0 && now - last_packet < kInGameSilenceUs;
    const std::optional<int> ping = shared.ping.EstimateMs(now);
    if (notice_armed && in_game &&
        (!place.empty() || now - joined_at >= kNoticeWaitForPlaceUs)) {
      notice_armed = false;
      notice_until = now + kNoticeUs;
    }
    if (in_game && now - logged_at >= kStatsLogUs) {
      logged_at = now;
      std::fprintf(stderr, "  [server-info] ping=%d ms fps=%d\n",
                   ping.value_or(-1), fps);
    }

    Content content;
    content.visible = ttf_ready &&
                      (config_.server_info == ServerInfoMode::kAlways
                           ? in_game
                           : now < notice_until);
    if (content.visible) {
      const window::WindowViewportSnapshot viewport =
          window::GetWindowViewportSnapshot();
      if (!viewport.valid()) {
        content.visible = false;
      } else {
        content.place = place;
        content.ping_ms = ping.value_or(-1);
        content.fps = fps;
        content.pixel_width = viewport.pixel_width;
        content.pixel_height = viewport.pixel_height;
        content.scale = static_cast<float>(viewport.pixel_width) /
                        static_cast<float>(viewport.logical_width);
      }
    }
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      if (content == shown_) {
        continue;
      }
    }
    if (!content.visible) {
      Hide(content);
      continue;
    }
    std::optional<Card> card = DrawCard(&fonts, content.scale, content.place,
                                        content.ping_ms, content.fps);
    if (!card.has_value()) {
      content.visible = false;
      Hide(content);
      continue;
    }
    const OverlayPoint at = PlaceOverlay(
        config_.position, content.pixel_width, content.pixel_height,
        card->width, card->height,
        static_cast<int>(std::lround(kTopMargin * content.scale)),
        static_cast<int>(std::lround(kEdgeMargin * content.scale)));
    std::lock_guard<std::mutex> lock(frame_mutex_);
    shown_ = content;
    rgba_ = std::move(card->rgba);
    ++revision_;
    frame_ = {};
    frame_.visible = 1;
    frame_.revision = kRevisionBit | revision_;
    frame_.coordinate_width = static_cast<std::uint32_t>(content.pixel_width);
    frame_.coordinate_height = static_cast<std::uint32_t>(content.pixel_height);
    frame_.x = at.x;
    frame_.y = at.y;
    frame_.width = static_cast<std::uint32_t>(card->width);
    frame_.height = static_cast<std::uint32_t>(card->height);
    frame_.row_bytes = frame_.width * 4U;
    frame_.rgba_bytes = static_cast<std::uint64_t>(frame_.row_bytes) *
                        frame_.height;
    g_may_present.store(true, std::memory_order_release);
  }
  fonts.Close();
  if (ttf_ready) {
    TTF_Quit();
  }
}

void ServerInfoOverlay::Hide(const Content& content) {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  shown_ = content;
  frame_ = {};
  rgba_.clear();
  g_may_present.store(false, std::memory_order_release);
}

bool ServerInfoOverlay::MayPresent() {
  return g_may_present.load(std::memory_order_acquire);
}

bool ServerInfoOverlay::QueryFrame(MocktailTextOverlayFrameInfo* frame) {
  std::lock_guard<std::mutex> lock(g_instance_mutex);
  if (g_instance == nullptr || frame == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> frame_lock(g_instance->frame_mutex_);
  *frame = g_instance->frame_;
  return frame->visible != 0;
}

bool ServerInfoOverlay::CopyFrame(std::uint64_t revision, void* rgba,
                                  std::size_t rgba_capacity) {
  std::lock_guard<std::mutex> lock(g_instance_mutex);
  if (g_instance == nullptr || rgba == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> frame_lock(g_instance->frame_mutex_);
  const ServerInfoOverlay& self = *g_instance;
  if (self.frame_.visible == 0 || self.frame_.revision != revision ||
      rgba_capacity < self.rgba_.size()) {
    return false;
  }
  std::memcpy(rgba, self.rgba_.data(), self.rgba_.size());
  return true;
}

}
