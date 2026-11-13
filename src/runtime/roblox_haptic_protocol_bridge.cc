#include "runtime/roblox_haptic_protocol_bridge.h"

#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

extern "C" {
void mocktail_vr_controller_request_haptics(int32_t hand, float amplitude,
                                            uint64_t duration_ns,
                                            float frequency_hz) __attribute__((weak));
void mocktail_vr_controller_stop_haptics(int32_t hand) __attribute__((weak));
}

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumJsonBytes = 64 * 1024;
constexpr uint64_t kMinimumDurationNs = 50000000ULL;      // 50ms
constexpr uint64_t kMaximumDurationNs = 5000000000ULL;    // 5 seconds

bool HasBoundedDepth(const std::string& json) {
  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (const char ch : json) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (ch == '\\')
        escaped = true;
      else if (ch == '"')
        quoted = false;
    } else if (ch == '"') {
      quoted = true;
    } else if (ch == '{' || ch == '[') {
      if (++depth > 8)
        return false;
    } else if (ch == '}' || ch == ']') {
      --depth;
    }
  }
  return true;
}

std::optional<nlohmann::json> ParseJsonMessage(JNIEnv* env, jstring message) {
  if (!message)
    return std::nullopt;
  const jsize size = env->GetStringUTFLength(message);
  if (size <= 0 || static_cast<std::size_t>(size) > kMaximumJsonBytes)
    return std::nullopt;
  const char* bytes = env->GetStringUTFChars(message, nullptr);
  if (!bytes)
    return std::nullopt;
  const std::string owned(bytes, static_cast<std::size_t>(size));
  env->ReleaseStringUTFChars(message, bytes);
  if (!HasBoundedDepth(owned))
    return std::nullopt;
  auto parsed = nlohmann::json::parse(owned, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object())
    return std::nullopt;
  return parsed;
}

// 0 left, 1 right, -1 both (a motor that names neither hand).
int HandForMotor(const std::string& motor_name) {
  std::string lower_name = motor_name;
  for (char& c : lower_name) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lower_name.find("left") != std::string::npos ||
      lower_name.find("large") != std::string::npos) {
    return 0;
  }
  if (lower_name.find("right") != std::string::npos ||
      lower_name.find("small") != std::string::npos) {
    return 1;
  }
  return -1;
}

// One message's demand on each controller: a hand vibrates at the strongest
// motor that maps to it instead of at whichever one came last.
struct HandDemand {
  bool present = false;
  float amplitude = 0.0f;
  uint64_t duration_ns = 0;
};

void AddMotorDemand(HandDemand (&hands)[2], const std::string& motor_name,
                    float amplitude, uint64_t duration_ns) {
  if (!std::isfinite(amplitude))
    amplitude = 0.0f;
  const int hand = HandForMotor(motor_name);
  for (int index = 0; index < 2; ++index) {
    if (hand != -1 && hand != index)
      continue;
    auto& demand = hands[index];
    demand.present = true;
    if (amplitude > demand.amplitude) {
      demand.amplitude = amplitude;
      demand.duration_ns = duration_ns;
    }
  }
}

void DispatchHandDemands(const HandDemand (&hands)[2], const HapticSinkFn& sink,
                         const HapticStopFn& stop) {
  for (int hand = 0; hand < 2; ++hand) {
    const auto& demand = hands[hand];
    if (!demand.present)
      continue;
    if (demand.amplitude <= 0.0f) {
      if (stop)
        stop(hand);
    } else if (sink) {
      sink(hand, std::clamp(demand.amplitude, 0.0f, 1.0f),
           std::clamp(demand.duration_ns, kMinimumDurationNs,
                      kMaximumDurationNs),
           0.0f);
    }
  }
}

}  // namespace

// Plays SetMotorState buffers sample by sample. Roblox sends HapticEffect
// output as look-ahead waveforms (e.g. 100 samples x 25 ms) and replaces them
// as it mixes, so each hand follows its latest buffer in real time.
class HapticWaveformPlayer {
 public:
  using Clock = std::chrono::steady_clock;

  HapticWaveformPlayer(HapticSinkFn sink, HapticStopFn stop)
      : sink_(std::move(sink)), stop_(std::move(stop)) {}
  ~HapticWaveformPlayer() { Halt(); }

  HapticWaveformPlayer(const HapticWaveformPlayer&) = delete;
  HapticWaveformPlayer& operator=(const HapticWaveformPlayer&) = delete;

  // Starts `samples` (already trimmed, at least one) on `hand` now.
  void Play(int hand, std::vector<float> samples, uint64_t ns_per_sample) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (halted_)
        return;
      tracks_[hand] = Track{std::move(samples), ns_per_sample, Clock::now(), 0,
                            true};
      if (!thread_.joinable())
        thread_ = std::thread([this] { Run(); });
    }
    wake_.notify_all();
  }

  void Stop(int hand) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tracks_[hand].active = false;
    }
    if (stop_)
      stop_(hand);
  }

  // Accepts buffers again after Halt.
  void Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    halted_ = false;
  }

  // Ends playback, stopping any hand still vibrating.
  void Halt() {
    bool was_active[2] = {};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      halted_ = true;
      for (int hand = 0; hand < 2; ++hand) {
        was_active[hand] = tracks_[hand].active;
        tracks_[hand].active = false;
      }
    }
    wake_.notify_all();
    if (thread_.joinable())
      thread_.join();
    for (int hand = 0; hand < 2; ++hand) {
      if (was_active[hand] && stop_)
        stop_(hand);
    }
  }

 private:
  struct Track {
    std::vector<float> samples;
    uint64_t ns_per_sample = 0;
    Clock::time_point start;
    std::size_t next = 0;
    bool active = false;
  };
  struct Step {
    int hand;
    float amplitude;
    uint64_t duration_ns;
  };

  void Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!halted_) {
      const auto now = Clock::now();
      std::optional<Clock::time_point> wake_at;
      std::vector<Step> steps;
      for (int hand = 0; hand < 2; ++hand) {
        Track& track = tracks_[hand];
        if (!track.active)
          continue;
        const auto period = std::chrono::nanoseconds(track.ns_per_sample);
        const auto index = static_cast<std::size_t>((now - track.start) / period);
        if (index >= track.samples.size()) {
          track.active = false;
          steps.push_back({hand, 0.0f, 0});
          continue;
        }
        // A late wake skips to the current sample rather than replaying.
        if (index >= track.next) {
          steps.push_back({hand, track.samples[index], track.ns_per_sample});
          track.next = index + 1;
        }
        const auto due = track.start + period * track.next;
        if (!wake_at || due < *wake_at)
          wake_at = due;
      }
      lock.unlock();
      for (const Step& step : steps) {
        if (step.amplitude <= 0.0f) {
          if (stop_)
            stop_(step.hand);
        } else if (sink_) {
          // The lease outlives one sample a little so steps join seamlessly;
          // the next sample, or the final stop, replaces it.
          sink_(step.hand, step.amplitude,
                std::clamp(step.duration_ns, kMinimumDurationNs,
                           kMaximumDurationNs),
                0.0f);
        }
      }
      lock.lock();
      if (halted_)
        break;
      if (wake_at)
        wake_.wait_until(lock, *wake_at);
      else
        wake_.wait(lock);
    }
  }

  const HapticSinkFn sink_;
  const HapticStopFn stop_;
  std::mutex mutex_;
  std::condition_variable wake_;
  Track tracks_[2];
  bool halted_ = false;
  std::thread thread_;
};

namespace {

// One hand's share of a SetMotorState: the per-sample maximum of its motors,
// on the period of the first motor that reached it.
struct HandWaveform {
  bool present = false;
  uint64_t ns_per_sample = 0;
  std::vector<float> samples;
};

void AddMotorWaveform(HandWaveform (&hands)[2], const std::string& motor_name,
                      const std::vector<float>& samples,
                      uint64_t ns_per_sample) {
  const int hand = HandForMotor(motor_name);
  for (int index = 0; index < 2; ++index) {
    if (hand != -1 && hand != index)
      continue;
    HandWaveform& wave = hands[index];
    if (!wave.present) {
      wave.present = true;
      wave.ns_per_sample = ns_per_sample;
    }
    // Resample by time onto the hand's period (Roblox uses one period for
    // every motor, so this is normally index for index).
    const double ratio = static_cast<double>(wave.ns_per_sample) /
                         static_cast<double>(ns_per_sample);
    const auto length = static_cast<std::size_t>(
        std::ceil(static_cast<double>(samples.size()) / ratio));
    if (wave.samples.size() < length)
      wave.samples.resize(length, 0.0f);
    for (std::size_t at = 0; at < length; ++at) {
      const auto source = static_cast<std::size_t>(at * ratio);
      if (source < samples.size())
        wave.samples[at] = std::max(wave.samples[at], samples[source]);
    }
  }
}

RobloxMessageBusReply Answer(JNIEnv* env, jstring message, std::size_t method,
                             const HapticSinkFn& sink,
                             const HapticStopFn& stop,
                             const HapticStateSyncFn& state_sync,
                             HapticWaveformPlayer& player) {
  // Method 0: SupportsHaptics
  if (method == 0) {
    if (state_sync) {
      state_sync(env);
    }
    return RobloxMessageBusReply{R"({"supportsHaptics":true})", 0, "supported"};
  }

  // Method 1: GetMotors
  if (method == 1) {
    if (state_sync) {
      state_sync(env);
    }
    return RobloxMessageBusReply{RobloxHapticProtocolBridge::GetMotorsJson(), 0,
                                 "ok"};
  }

  // Method 2: SetMotorState
  if (method == 2) {
    const auto parsed = ParseJsonMessage(env, message);
    if (!parsed.has_value()) {
      return RobloxMessageBusReply{"{}", 13, "invalid_request"};
    }
    const auto& root = *parsed;
    HandWaveform hands[2];
    for (auto it = root.begin(); it != root.end(); ++it) {
      const auto& motor = it.value();
      if (!motor.is_object())
        continue;
      double ms_per_sample = 20.0;
      if (motor.contains("millisecondsPerSample") &&
          motor["millisecondsPerSample"].is_number()) {
        ms_per_sample = motor["millisecondsPerSample"].get<double>();
      }
      if (!std::isfinite(ms_per_sample) || ms_per_sample <= 0.0)
        continue;
      ms_per_sample = std::clamp(ms_per_sample, 1.0, 1000.0);
      const auto ns_per_sample =
          static_cast<uint64_t>(ms_per_sample * 1000000.0);
      std::vector<float> samples;
      if (motor.contains("intensities") && motor["intensities"].is_array()) {
        for (const auto& item : motor["intensities"]) {
          const float value = item.is_number() ? item.get<float>() : 0.0f;
          samples.push_back(std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f)
                                                 : 0.0f);
        }
      } else if (motor.contains("intensity") && motor["intensity"].is_number()) {
        const float value = motor["intensity"].get<float>();
        samples.push_back(
            std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f);
      }
      AddMotorWaveform(hands, it.key(), samples, ns_per_sample);
    }
    for (int hand = 0; hand < 2; ++hand) {
      HandWaveform& wave = hands[hand];
      if (!wave.present)
        continue;
      // The buffer is padded with silence; play only up to its last bump.
      while (!wave.samples.empty() && wave.samples.back() <= 0.0f)
        wave.samples.pop_back();
      const std::size_t longest =
          static_cast<std::size_t>(kMaximumDurationNs / wave.ns_per_sample);
      if (wave.samples.size() > longest)
        wave.samples.resize(longest);
      if (wave.samples.empty())
        player.Stop(hand);
      else
        player.Play(hand, std::move(wave.samples), wave.ns_per_sample);
    }
    return RobloxMessageBusReply{"{}", 0, "ok"};
  }

  // Method 3: UpdateSingletonVibration
  if (method == 3) {
    const auto parsed = ParseJsonMessage(env, message);
    if (!parsed.has_value()) {
      return RobloxMessageBusReply{"{}", 13, "invalid_request"};
    }
    float intensity = 0.0f;
    if (parsed->contains("intensity") && (*parsed)["intensity"].is_number()) {
      intensity = (*parsed)["intensity"].get<float>();
    }
    HandDemand hands[2];
    AddMotorDemand(hands, "defaultMotor", intensity, kMinimumDurationNs);
    DispatchHandDemands(hands, sink, stop);
    return RobloxMessageBusReply{"{}", 0, "ok"};
  }

  return RobloxMessageBusReply{"{}", 13, "unknown_method"};
}

}  // namespace

bool RobloxHapticProtocolBridge::EnabledForThisSession() {
  const char* value = std::getenv("MOCKTAIL_VR_ENABLED");
  const std::string_view vr = value != nullptr ? value : "";
  return vr == "1" || vr == "true" || vr == "on";
}

std::string RobloxHapticProtocolBridge::GetMotorsJson() {
  // Positional HapticEffects place hands at x = -1 (left) and +1 (right):
  // VOXEL DESTRUCT 2 aims its per-hand effects there with a 0.5 radius.
  const nlohmann::json motors = {
      {"leftHand", {{"x", -1.0}, {"y", 0.0}, {"z", 0.0}}},
      {"rightHand", {{"x", 1.0}, {"y", 0.0}, {"z", 0.0}}},
      {"LeftHand", {{"x", -1.0}, {"y", 0.0}, {"z", 0.0}}},
      {"RightHand", {{"x", 1.0}, {"y", 0.0}, {"z", 0.0}}},
      {"Large", {{"x", -1.0}, {"y", 0.0}, {"z", 0.0}}},
      {"Small", {{"x", 1.0}, {"y", 0.0}, {"z", 0.0}}},
      {"large", {{"x", -1.0}, {"y", 0.0}, {"z", 0.0}}},
      {"small", {{"x", 1.0}, {"y", 0.0}, {"z", 0.0}}},
      {"defaultMotor", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}},
  };
  return motors.dump();
}

RobloxHapticProtocolBridge::RobloxHapticProtocolBridge(
    JniEnvironmentProvider environment, RobloxMessageBusSymbols symbols,
    RobloxMessageBusObjects objects, HapticSinkFn haptic_sink,
    HapticStopFn haptic_stop, HapticStateSyncFn haptic_state_sync) {
  if (!haptic_sink) {
    haptic_sink = [](int hand, float amplitude, uint64_t duration_ns,
                     float frequency_hz) {
      if (&mocktail_vr_controller_request_haptics != nullptr) {
        mocktail_vr_controller_request_haptics(hand, amplitude, duration_ns,
                                               frequency_hz);
      } else {
        using RequestFn = void (*)(int32_t, float, uint64_t, float);
        auto fn = reinterpret_cast<RequestFn>(
            dlsym(RTLD_DEFAULT, "mocktail_vr_controller_request_haptics"));
        if (fn != nullptr) {
          fn(hand, amplitude, duration_ns, frequency_hz);
        }
      }
    };
  }
  if (!haptic_stop) {
    haptic_stop = [](int hand) {
      if (&mocktail_vr_controller_stop_haptics != nullptr) {
        mocktail_vr_controller_stop_haptics(hand);
      } else {
        using StopFn = void (*)(int32_t);
        auto fn = reinterpret_cast<StopFn>(
            dlsym(RTLD_DEFAULT, "mocktail_vr_controller_stop_haptics"));
        if (fn != nullptr) {
          fn(hand);
        }
      }
    };
  }
  player_ = std::make_shared<HapticWaveformPlayer>(haptic_sink, haptic_stop);
  bridge_ = std::make_unique<RobloxMessageBusRequestBridge>(
      environment, symbols, objects, "HapticProtocol",
      std::vector<std::string>{"SupportsHaptics", "GetMotors", "SetMotorState",
                               "UpdateSingletonVibration"},
      [sink = std::move(haptic_sink), stop = std::move(haptic_stop),
       sync = std::move(haptic_state_sync), player = player_](
          JNIEnv* env, jstring message, std::size_t method) {
        return Answer(env, message, method, sink, stop, sync, *player);
      });
}

RobloxHapticProtocolBridge::~RobloxHapticProtocolBridge() {
  // A handler may outlive the bridge inside the message bus; never let its
  // player keep a controller vibrating.
  player_->Halt();
}

Status RobloxHapticProtocolBridge::Initialize() {
  player_->Resume();
  return bridge_->Initialize();
}

Status RobloxHapticProtocolBridge::Shutdown() {
  const Status status = bridge_->Shutdown();
  player_->Halt();
  return status;
}

}  // namespace runtime
}  // namespace mocktail
