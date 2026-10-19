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
constexpr auto kFadeTick = std::chrono::milliseconds(16);
constexpr std::int64_t kFadeUs = 200000;
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

// Matches the Roblox top bar capsule.
constexpr Color kPanel = Hex(0x121215, 0.92F);
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
  fonts->title = TTF_OpenFont(title_file.c_str(), 15.0F * scale);
  fonts->detail = TTF_OpenFont(detail_file.c_str(), 15.0F * scale);
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
  // Where each bubble sits in the card; zero width when it is not drawn.
  int info_left = 0;
  int info_width = 0;
  int fps_left = 0;
  int fps_width = 0;
};

// Draws the server info capsule when show_info is set and the frame rate
// capsule when fps is, side by side in the order fps_first gives.
std::optional<Card> DrawCard(Fonts* fonts, float scale, bool show_info,
                             const std::string& place, int ping_ms,
                             std::optional<int> fps, bool fps_first) {
  if (!OpenFonts(fonts, scale)) {
    return std::nullopt;
  }
  const std::string title = place.empty() ? "Unknown location" : place;
  const std::string ping_text =
      (ping_ms >= 0 ? std::to_string(ping_ms) : std::string("--")) + " ms";
  const std::string fps_text =
      (fps.value_or(-1) >= 0 ? std::to_string(*fps) : std::string("--")) +
      " fps";
  const float unit = scale;
  const float height = std::round(44.0F * unit);
  const float pad_left = 14.0F * unit;
  const float pad_right = 18.0F * unit;
  const float pin_width = 20.0F * unit;
  const float pin_gap = 6.0F * unit;
  const float group_gap = 18.0F * unit;
  const float bars_width = 14.0F * unit;
  const float bars_gap = 6.0F * unit;
  const float fps_pad = 16.0F * unit;
  const float bubble_gap = 8.0F * unit;

  const float ping_text_width =
      static_cast<float>(TextWidth(fonts->detail, ping_text));
  const float fps_text_width =
      static_cast<float>(TextWidth(fonts->detail, fps_text));
  // Reserve room for three digits so the capsule does not resize as the
  // numbers change.
  const float ping_width = std::max(
      ping_text_width, static_cast<float>(TextWidth(fonts->detail, "999 ms")));
  const float fps_width = std::max(
      fps_text_width, static_cast<float>(TextWidth(fonts->detail, "999 fps")));
  const float title_width =
      static_cast<float>(TextWidth(fonts->title, title));
  const float info_width =
      std::ceil(pad_left + pin_width + pin_gap + title_width + group_gap +
                bars_width + bars_gap + ping_width + pad_right);
  const float fps_bubble_width = std::ceil(fps_pad + fps_width + fps_pad);
  const bool both = show_info && fps.has_value();
  const float info_left =
      both && fps_first ? fps_bubble_width + bubble_gap : 0.0F;
  const float fps_left = both && !fps_first ? info_width + bubble_gap : 0.0F;
  const float width = std::max(show_info ? info_left + info_width : 0.0F,
                               fps.has_value() ? fps_left + fps_bubble_width
                                               : 0.0F);
  Canvas canvas(static_cast<int>(std::ceil(width)),
                static_cast<int>(std::ceil(height)));
  const float center_y = height / 2.0F;
  const auto draw_capsule = [&](float left, float capsule_width) {
    canvas.Fill(left, 0, left + capsule_width, height, kPanel,
                [&](float x, float y) {
                  return RoundedBox(x, y, left + capsule_width / 2.0F,
                                    center_y, capsule_width / 2.0F,
                                    height / 2.0F, height / 2.0F);
                });
  };
  Card card;
  if (fps.has_value()) {
    draw_capsule(fps_left, fps_bubble_width);
    DrawText(&canvas, fonts->detail, fps_text,
             fps_left + (fps_bubble_width - fps_text_width) / 2.0F, center_y,
             kInk);
    card.fps_left = static_cast<int>(fps_left);
    card.fps_width = static_cast<int>(fps_bubble_width);
  }
  if (!show_info) {
    card.width = canvas.width();
    card.height = canvas.height();
    card.rgba = canvas.StraightRgba();
    return card;
  }
  draw_capsule(info_left, info_width);
  card.info_left = static_cast<int>(info_left);
  card.info_width = static_cast<int>(info_width);

  float cursor = info_left + pad_left;
  const float pin_x = cursor + pin_width / 2.0F;
  const float head_y = center_y - 2.5F * unit;
  const float tip[3][2] = {{pin_x - 5.0F * unit, head_y + 2.2F * unit},
                           {pin_x + 5.0F * unit, head_y + 2.2F * unit},
                           {pin_x, center_y + 8.0F * unit}};
  canvas.Fill(cursor - 1, center_y - 10.0F * unit, cursor + pin_width + 1,
              center_y + 10.0F * unit, kInk, [&](float x, float y) {
                const float pin = std::min(
                    Circle(x, y, pin_x, head_y, 5.6F * unit),
                    Triangle(x, y, tip));
                return std::max(pin,
                                -Circle(x, y, pin_x, head_y, 2.2F * unit));
              });
  cursor += pin_width + pin_gap;
  DrawText(&canvas, fonts->title, title, cursor, center_y, kInk);
  cursor += title_width + group_gap;

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
  const float bar_heights[3] = {4.5F, 8.0F, 12.0F};
  const float bar_bottom = center_y + 6.0F * unit;
  const float ping_slack = (ping_width - ping_text_width) / 2.0F;
  cursor += ping_slack;
  for (int bar = 0; bar < 3; ++bar) {
    const float bar_width = 3.5F * unit;
    const float bar_left = cursor + bar * 5.25F * unit;
    const float bar_height = bar_heights[bar] * unit;
    canvas.Fill(bar_left - 1, bar_bottom - bar_height - 1,
                bar_left + bar_width + 1, bar_bottom + 1,
                bar < lit_bars ? ping_color : kBarOff, [&](float x, float y) {
                  return RoundedBox(x, y, bar_left + bar_width / 2.0F,
                                    bar_bottom - bar_height / 2.0F,
                                    bar_width / 2.0F, bar_height / 2.0F,
                                    bar_width / 2.0F);
                });
  }
  cursor += bars_width + bars_gap;
  DrawText(&canvas, fonts->detail, ping_text, cursor, center_y, kInk);

  card.width = canvas.width();
  card.height = canvas.height();
  card.rgba = canvas.StraightRgba();
  return card;
}

// Scales the alpha of the columns [left, left + span) by opacity, eased so
// the fade starts and ends gently.
void FadeColumns(Card* card, int left, int span, float opacity) {
  if (span <= 0 || opacity >= 1.0F) {
    return;
  }
  const float eased = opacity * opacity * (3.0F - 2.0F * opacity);
  const int end = std::min(card->width, left + span);
  for (int y = 0; y < card->height; ++y) {
    std::uint8_t* row =
        card->rgba.data() + static_cast<std::size_t>(y) * card->width * 4;
    for (int x = std::max(0, left); x < end; ++x) {
      row[x * 4 + 3] =
          static_cast<std::uint8_t>(std::lround(row[x * 4 + 3] * eased));
    }
  }
}

std::string LookupPlace(const std::string& address,
                        const std::atomic<bool>* cancel) {
  const std::optional<nlohmann::json> reply =
      GetHttpsJson("https://ipinfo.io/" + address + "/json", cancel);
  return reply.has_value() ? ParseIpInfoPlace(*reply) : std::string();
}

}

bool ServerInfoOverlay::Content::operator==(const Content& other) const {
  return visible == other.visible && show_info == other.show_info &&
         show_fps == other.show_fps && info_alpha == other.info_alpha &&
         fps_alpha == other.fps_alpha && place == other.place &&
         ping_ms == other.ping_ms && fps == other.fps &&
         pixel_width == other.pixel_width &&
         pixel_height == other.pixel_height && scale == other.scale;
}

ServerInfoOverlay::ServerInfoOverlay(OverlayConfig config) : config_(config) {}

ServerInfoOverlay::~ServerInfoOverlay() { Stop(); }

void ServerInfoOverlay::Start() {
  if (started_ || (config_.server_info == ServerInfoMode::kOff &&
                   config_.fps == ServerInfoMode::kOff)) {
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
  std::fprintf(stderr, "  [server-info] server info %s, fps %s at %s\n",
               std::string(ServerInfoModeName(config_.server_info)).c_str(),
               std::string(ServerInfoModeName(config_.fps)).c_str(),
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
  const SteadyBubble steady = FindSteadyBubble(config_.server_info, config_.fps);
  const bool fps_first = FpsBubbleFirst(config_.position, steady);
  float info_opacity = 0.0F;
  float fps_opacity = 0.0F;
  bool fading = false;
  std::int64_t faded_at = MonotonicMicros();
  // The last card drawn at full opacity, reused while only the fade changes.
  Content drawn;
  std::optional<Card> drawn_card;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(wait_mutex_);
      if (wake_.wait_for(lock, fading ? kFadeTick : kTick,
                         [this] { return stopping_; })) {
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

    const auto shown = [&](ServerInfoMode mode) {
      switch (mode) {
        case ServerInfoMode::kOff:
          return false;
        case ServerInfoMode::kNotice:
          return now < notice_until;
        case ServerInfoMode::kAlways:
          return in_game;
      }
      return false;
    };
    const window::WindowViewportSnapshot viewport =
        window::GetWindowViewportSnapshot();
    const bool can_show = ttf_ready && viewport.valid();
    const bool want_info = can_show && shown(config_.server_info);
    const bool want_fps = can_show && shown(config_.fps);
    if (!can_show) {
      info_opacity = 0.0F;
      fps_opacity = 0.0F;
    }
    // A fade that starts after an idle tick advances by one fade frame, not
    // by the whole idle wait.
    const std::int64_t elapsed =
        fading ? now - faded_at
               : std::chrono::duration_cast<std::chrono::microseconds>(
                     kFadeTick)
                     .count();
    info_opacity = StepFade(info_opacity, want_info, elapsed, kFadeUs);
    fps_opacity = StepFade(fps_opacity, want_fps, elapsed, kFadeUs);
    faded_at = now;
    fading = info_opacity != (want_info ? 1.0F : 0.0F) ||
             fps_opacity != (want_fps ? 1.0F : 0.0F);

    Content content;
    content.show_info = info_opacity > 0.0F;
    content.show_fps = fps_opacity > 0.0F;
    content.info_alpha = static_cast<int>(std::lround(info_opacity * 255.0F));
    content.fps_alpha = static_cast<int>(std::lround(fps_opacity * 255.0F));
    content.visible = content.show_info || content.show_fps;
    if (content.visible) {
      if (content.show_info) {
        content.place = place;
        content.ping_ms = ping.value_or(-1);
      }
      content.fps = content.show_fps ? fps : -1;
      content.pixel_width = viewport.pixel_width;
      content.pixel_height = viewport.pixel_height;
      content.scale = static_cast<float>(viewport.pixel_width) /
                      static_cast<float>(viewport.logical_width);
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
    Content opaque = content;
    opaque.info_alpha = 0;
    opaque.fps_alpha = 0;
    if (!drawn_card.has_value() || !(opaque == drawn)) {
      drawn_card = DrawCard(
          &fonts, content.scale, content.show_info, content.place,
          content.ping_ms,
          content.show_fps ? std::optional<int>(content.fps) : std::nullopt,
          fps_first);
      drawn = opaque;
    }
    if (!drawn_card.has_value()) {
      content.visible = false;
      info_opacity = 0.0F;
      fps_opacity = 0.0F;
      fading = false;
      Hide(content);
      continue;
    }
    std::optional<Card> card = drawn_card;
    FadeColumns(&*card, card->info_left, card->info_width, info_opacity);
    FadeColumns(&*card, card->fps_left, card->fps_width, fps_opacity);
    // Place the steady bubble where it would sit alone, so it does not move
    // when the other bubble appears or goes away.
    int anchor_left = 0;
    int anchor_width = card->width;
    if (steady == SteadyBubble::kServerInfo && card->info_width > 0) {
      anchor_left = card->info_left;
      anchor_width = card->info_width;
    } else if (steady == SteadyBubble::kFps && card->fps_width > 0) {
      anchor_left = card->fps_left;
      anchor_width = card->fps_width;
    }
    OverlayPoint at = PlaceOverlay(
        config_.position, content.pixel_width, content.pixel_height,
        anchor_width, card->height,
        static_cast<int>(std::lround(kTopMargin * content.scale)),
        static_cast<int>(std::lround(kEdgeMargin * content.scale)));
    at.x = std::clamp(at.x - anchor_left, 0,
                      std::max(0, content.pixel_width - card->width));
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
