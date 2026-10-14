#include "runtime/roblox_haptic_protocol_bridge.h"

#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>

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

void DispatchHapticForMotor(const std::string& motor_name, float amplitude,
                            uint64_t duration_ns, float frequency_hz,
                            const HapticSinkFn& sink,
                            const HapticStopFn& stop) {
  int hand = -1;
  std::string lower_name = motor_name;
  for (char& c : lower_name) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lower_name.find("left") != std::string::npos ||
      lower_name.find("large") != std::string::npos) {
    hand = 0;
  } else if (lower_name.find("right") != std::string::npos ||
             lower_name.find("small") != std::string::npos) {
    hand = 1;
  }

  if (amplitude <= 0.0f || !std::isfinite(amplitude)) {
    if (stop) {
      if (hand == -1) {
        stop(0);
        stop(1);
      } else {
        stop(hand);
      }
    }
  } else {
    amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    duration_ns = std::clamp(duration_ns, kMinimumDurationNs, kMaximumDurationNs);
    std::fprintf(stderr,
                 "  [haptic] dispatch: hand=%d motor='%s' amp=%.3f "
                 "dur_ms=%.1f\n",
                 hand, motor_name.c_str(), amplitude,
                 duration_ns / 1000000.0);
    if (sink) {
      if (hand == -1) {
        sink(0, amplitude, duration_ns, frequency_hz);
        sink(1, amplitude, duration_ns, frequency_hz);
      } else {
        sink(hand, amplitude, duration_ns, frequency_hz);
      }
    }
  }
}

RobloxMessageBusReply Answer(JNIEnv* env, jstring message, std::size_t method,
                             const HapticSinkFn& sink,
                             const HapticStopFn& stop,
                             const HapticStateSyncFn& state_sync) {
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
    for (auto it = root.begin(); it != root.end(); ++it) {
      const std::string& motor_name = it.key();
      const auto& motor_val = it.value();
      if (!motor_val.is_object())
        continue;

      float max_intensity = 0.0f;
      std::size_t sample_count = 1;
      double ms_per_sample = 20.0;

      if (motor_val.contains("millisecondsPerSample") &&
          motor_val["millisecondsPerSample"].is_number()) {
        ms_per_sample = motor_val["millisecondsPerSample"].get<double>();
      }

      if (motor_val.contains("intensities") &&
          motor_val["intensities"].is_array()) {
        const auto& arr = motor_val["intensities"];
        sample_count = std::max<std::size_t>(1, arr.size());
        for (const auto& item : arr) {
          if (item.is_number()) {
            float val = item.get<float>();
            if (val > max_intensity)
              max_intensity = val;
          }
        }
      } else if (motor_val.contains("intensity") &&
                 motor_val["intensity"].is_number()) {
        max_intensity = motor_val["intensity"].get<float>();
      }

      uint64_t duration_ns = static_cast<uint64_t>(
          std::max(0.0, ms_per_sample) * sample_count * 1000000.0);
      DispatchHapticForMotor(motor_name, max_intensity, duration_ns, 0.0f, sink,
                            stop);
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
    DispatchHapticForMotor("defaultMotor", intensity, kMinimumDurationNs, 0.0f,
                          sink, stop);
    return RobloxMessageBusReply{"{}", 0, "ok"};
  }

  return RobloxMessageBusReply{"{}", 13, "unknown_method"};
}

}  // namespace

std::string RobloxHapticProtocolBridge::GetMotorsJson() {
  const nlohmann::json motors = {
      {"leftHand", {{"x", -0.2}, {"y", 0.0}, {"z", 0.0}}},
      {"rightHand", {{"x", 0.2}, {"y", 0.0}, {"z", 0.0}}},
      {"LeftHand", {{"x", -0.2}, {"y", 0.0}, {"z", 0.0}}},
      {"RightHand", {{"x", 0.2}, {"y", 0.0}, {"z", 0.0}}},
      {"Large", {{"x", -0.2}, {"y", 0.0}, {"z", 0.0}}},
      {"Small", {{"x", 0.2}, {"y", 0.0}, {"z", 0.0}}},
      {"large", {{"x", -0.2}, {"y", 0.0}, {"z", 0.0}}},
      {"small", {{"x", 0.2}, {"y", 0.0}, {"z", 0.0}}},
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
  if (!haptic_state_sync) {
    using NativeUpdateMobileHapticsStateFn =
        void (*)(JNIEnv*, jclass, jboolean, jstring);
    auto fn = reinterpret_cast<NativeUpdateMobileHapticsStateFn>(
        dlsym(RTLD_DEFAULT,
              "Java_com_roblox_engine_jni_NativeGLInterface_"
              "nativeUpdateMobileHapticsState"));
    if (fn != nullptr) {
      haptic_state_sync = [fn](JNIEnv* env) {
        if (!env) return;
        jclass clazz = env->FindClass("com/roblox/engine/jni/NativeGLInterface");
        if (!clazz) return;
        const std::string motors = RobloxHapticProtocolBridge::GetMotorsJson();
        jstring str = env->NewStringUTF(motors.c_str());
        if (str) {
          fn(env, clazz, JNI_TRUE, str);
          env->DeleteLocalRef(str);
        }
        env->DeleteLocalRef(clazz);
      };
    }
  }

  bridge_ = std::make_unique<RobloxMessageBusRequestBridge>(
      environment, symbols, objects, "HapticProtocol",
      std::vector<std::string>{"SupportsHaptics", "GetMotors", "SetMotorState",
                               "UpdateSingletonVibration"},
      [sink = std::move(haptic_sink), stop = std::move(haptic_stop),
       sync = std::move(haptic_state_sync)](
          JNIEnv* env, jstring message, std::size_t method) {
        return Answer(env, message, method, sink, stop, sync);
      });
}

RobloxHapticProtocolBridge::~RobloxHapticProtocolBridge() = default;

Status RobloxHapticProtocolBridge::Initialize() {
  return bridge_->Initialize();
}

Status RobloxHapticProtocolBridge::Shutdown() {
  return bridge_->Shutdown();
}

}  // namespace runtime
}  // namespace mocktail
