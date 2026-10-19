#ifndef MOCKTAIL_RUNTIME_SERVER_INFO_OVERLAY_H_
#define MOCKTAIL_RUNTIME_SERVER_INFO_OVERLAY_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mocktail/graphics/text_overlay_frame.h"
#include "runtime/runtime_config.h"

namespace mocktail::runtime {

class ServerInfoOverlay final {
 public:
  ServerInfoOverlay(OverlayConfig config, std::filesystem::path config_file);
  ~ServerInfoOverlay();

  ServerInfoOverlay(const ServerInfoOverlay&) = delete;
  ServerInfoOverlay& operator=(const ServerInfoOverlay&) = delete;

  void Start();
  void Stop();

  static bool MayPresent();
  static bool QueryFrame(MocktailTextOverlayFrameInfo* frame);
  static bool CopyFrame(std::uint64_t revision, void* rgba,
                        std::size_t rgba_capacity);

  static constexpr std::uint64_t kRevisionBit = 1ULL << 63;

 private:
  struct Content {
    bool visible = false;
    bool show_popup = false;
    bool show_hud = false;
    int popup_alpha = 0;
    int hud_alpha = 0;
    std::string place;
    int ping_ms = -1;
    int fps = -1;
    bool show_fps = false;
    bool show_ping = false;
    OverlayPosition position = OverlayPosition::kBottomLeft;
    int location_offset = 12;
    int pixel_width = 0;
    int pixel_height = 0;
    float scale = 1.0F;
    bool operator==(const Content& other) const;
  };

  void Run();
  void Hide(const Content& content);

  OverlayConfig config_;
  std::filesystem::path config_file_;
  std::thread worker_;
  std::mutex wait_mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  bool started_ = false;
  std::atomic<bool> cancel_lookup_{false};

  std::mutex frame_mutex_;
  Content shown_;
  std::vector<std::uint8_t> rgba_;
  MocktailTextOverlayFrameInfo frame_{};
  std::uint64_t revision_ = 0;
};

}

#endif
