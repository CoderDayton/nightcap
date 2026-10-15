#include "runtime/roblox_haptic_protocol_bridge.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

using Json = nlohmann::json;

std::string Copy(JNIEnv* env, jstring value) {
  if (!value)
    return {};
  const char* chars = env->GetStringUTFChars(value, nullptr);
  const std::string copy = chars ? chars : "";
  if (chars)
    env->ReleaseStringUTFChars(value, chars);
  return copy;
}

struct RecordedHaptic {
  int hand = -1;
  float amplitude = 0.0f;
  uint64_t duration_ns = 0;
  float frequency_hz = 0.0f;
};

struct Probe {
  jnivm::VM* vm = nullptr;
  std::string expected_protocol = "HapticProtocol";
  std::map<std::string, jobject> legacy;
  std::map<std::string, jobject> handlers;
  int callbacks_created = 0;
  int callbacks_cleared = 0;
  int handlers_created = 0;
  int handlers_cleared = 0;
  int subscriptions = 0;
  int disconnected = 0;
  int native_cleared = 0;
  std::mutex mutex;

  struct Published {
    std::string method;
    Json body;
    jint code;
  };

  std::vector<Published> published;
  std::vector<std::pair<std::string, Json>> resolved;

  std::vector<RecordedHaptic> recorded_sinks;
  std::vector<int> recorded_stops;
};

Probe* g_probe = nullptr;

void Prepare(void* context) {
  static_cast<jnivm::VM*>(context)->RestoreFunctions();
}

jobject CreateRaw(void* context, std::shared_ptr<void> target,
                  void (*run)(void*, JNIEnv*, jstring)) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->callbacks_created;
  return probe->vm->CreateMessageBusRawCallback(
      std::move(target), jnivm::MessageBusRawCallbacks{run});
}

void ClearRaw(void* context, jobject callback) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->callbacks_cleared;
  probe->vm->ClearMessageBusRawCallback(callback);
}

jobject CreateAsync(void* context, std::shared_ptr<void> target,
                    void (*run)(void*, JNIEnv*, jstring, jstring)) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->handlers_created;
  return probe->vm->CreateMessageBusAsyncRequestHandler(
      std::move(target), jnivm::MessageBusAsyncRequestHandlerCallbacks{run});
}

void ClearAsync(void* context, jobject handler) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->handlers_cleared;
  probe->vm->ClearMessageBusAsyncRequestHandler(handler);
}

void InvokeLegacy(JNIEnv* env, jobject callback, const char* payload) {
  jclass cls = env->GetObjectClass(callback);
  jmethodID run = env->GetMethodID(cls, "run", "(Ljava/lang/String;)V");
  jstring message = env->NewStringUTF(payload);
  env->CallVoidMethod(callback, run, message);
  env->DeleteLocalRef(message);
  env->DeleteLocalRef(cls);
}

void InvokeAsync(JNIEnv* env, jobject handler, const char* payload,
                 const char* response_id) {
  jclass cls = env->GetObjectClass(handler);
  jmethodID run =
      env->GetMethodID(cls, "run", "(Ljava/lang/String;Ljava/lang/String;)V");
  jstring message = payload ? env->NewStringUTF(payload) : nullptr;
  jstring id = env->NewStringUTF(response_id);
  jvalue args[2]{};
  args[0].l = message;
  args[1].l = id;
  env->CallVoidMethodA(handler, run, args);
  if (message)
    env->DeleteLocalRef(message);
  env->DeleteLocalRef(id);
  env->DeleteLocalRef(cls);
}

jobject Subscribe(JNIEnv* env, jobject, jstring protocol, jstring method,
                  jobject callback, jboolean once) {
  EXPECT_EQ(Copy(env, protocol), g_probe->expected_protocol);
  EXPECT_EQ(once, JNI_FALSE);
  const int handle = ++g_probe->subscriptions;
  g_probe->legacy[Copy(env, method)] = callback;
  jclass cls = env->FindClass("com/roblox/universalapp/messagebus/Connection");
  jmethodID ctor = env->GetMethodID(cls, "<init>", "(J)V");
  jobject connection = env->NewObject(cls, ctor, static_cast<jlong>(handle));
  env->DeleteLocalRef(cls);
  return connection;
}

void Disconnect(JNIEnv*, jobject, jlong handle) {
  EXPECT_GT(handle, 0);
  ++g_probe->disconnected;
}

void SetHandler(JNIEnv* env, jobject, jstring protocol, jstring method,
                jobject handler) {
  EXPECT_EQ(Copy(env, protocol), g_probe->expected_protocol);
  g_probe->handlers[Copy(env, method)] = handler;
}

void ClearHandler(JNIEnv* env, jobject, jstring protocol, jstring) {
  EXPECT_EQ(Copy(env, protocol), g_probe->expected_protocol);
  ++g_probe->native_cleared;
}

void Publish(JNIEnv* env, jobject, jstring protocol, jstring method,
             jstring response, jint code, jstring telemetry) {
  EXPECT_EQ(Copy(env, protocol), g_probe->expected_protocol);
  EXPECT_EQ(Copy(env, telemetry), "{}");
  std::unique_lock<std::mutex> lock(g_probe->mutex);
  g_probe->published.push_back(
      {Copy(env, method), Json::parse(Copy(env, response)), code});
}

void Resolve(JNIEnv* env, jobject, jstring id, jstring body) {
  std::lock_guard<std::mutex> lock(g_probe->mutex);
  g_probe->resolved.emplace_back(Copy(env, id), Json::parse(Copy(env, body)));
}

class RobloxHapticProtocolBridgeTest : public testing::Test {
 protected:
  void SetUp() override {
    probe.vm = &vm;
    g_probe = &probe;
    env = vm.GetJNIEnv();
    jclass cls =
        env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
    bus = env->AllocObject(cls);
    env->DeleteLocalRef(cls);
  }

  void TearDown() override {
    bridge.reset();
    env->DeleteLocalRef(bus);
    g_probe = nullptr;
  }

  Status Initialize() {
    auto sink = [](int hand, float amplitude, uint64_t duration_ns,
                   float frequency_hz) {
      if (g_probe) {
        std::lock_guard<std::mutex> lock(g_probe->mutex);
        g_probe->recorded_sinks.push_back(
            {hand, amplitude, duration_ns, frequency_hz});
      }
    };
    auto stop = [](int hand) {
      if (g_probe) {
        std::lock_guard<std::mutex> lock(g_probe->mutex);
        g_probe->recorded_stops.push_back(hand);
      }
    };

    bridge = std::make_unique<RobloxHapticProtocolBridge>(
        JniEnvironmentProvider{vm.GetJavaVM(), &vm, Prepare},
        RobloxMessageBusSymbols{Subscribe, Disconnect, Publish, SetHandler,
                                ClearHandler, Resolve},
        RobloxMessageBusObjects{bus, &probe, CreateRaw, ClearRaw, CreateAsync,
                                ClearAsync},
        sink, stop);
    return bridge->Initialize();
  }

  Probe::Published Legacy(const char* method, const char* payload = "{}") {
    const std::size_t mark = probe.published.size();
    InvokeLegacy(env, probe.legacy.at(method), payload);
    EXPECT_EQ(probe.published.size(), mark + 1);
    return probe.published.back();
  }

  std::pair<std::string, Json> Async(const char* method,
                                     const char* payload = "{}",
                                     const char* id = "id-123") {
    const std::size_t mark = probe.resolved.size();
    InvokeAsync(env, probe.handlers.at(method), payload, id);
    EXPECT_EQ(probe.resolved.size(), mark + 1);
    return probe.resolved.back();
  }

  jnivm::VM vm;
  JNIEnv* env = nullptr;
  jobject bus = nullptr;
  Probe probe;
  std::unique_ptr<RobloxHapticProtocolBridge> bridge;
};

TEST_F(RobloxHapticProtocolBridgeTest, InitializesAndShutsDownCleanly) {
  ASSERT_TRUE(Initialize().ok());
  EXPECT_EQ(probe.subscriptions, 4);
  EXPECT_EQ(probe.handlers_created, 4);
  EXPECT_TRUE(bridge->Shutdown().ok());
  EXPECT_EQ(probe.disconnected, 4);
  EXPECT_EQ(probe.handlers_cleared, 4);
}

TEST_F(RobloxHapticProtocolBridgeTest, SupportsHapticsRepliesTrue) {
  ASSERT_TRUE(Initialize().ok());

  const auto reply = Legacy("SupportsHaptics");
  EXPECT_EQ(reply.code, 0);
  EXPECT_TRUE(reply.body.value("supportsHaptics", false));

  const auto async_reply = Async("SupportsHaptics", "{}", "async-1");
  EXPECT_EQ(async_reply.first, "async-1");
  EXPECT_TRUE(async_reply.second.value("supportsHaptics", false));
}

TEST_F(RobloxHapticProtocolBridgeTest, GetMotorsReturnsValidPositions) {
  ASSERT_TRUE(Initialize().ok());

  const auto reply = Legacy("GetMotors");
  EXPECT_EQ(reply.code, 0);
  EXPECT_TRUE(reply.body.contains("leftHand"));
  EXPECT_TRUE(reply.body.contains("rightHand"));
  EXPECT_TRUE(reply.body.contains("defaultMotor"));

  EXPECT_LT(reply.body["leftHand"]["x"].get<double>(), 0.0);
  EXPECT_GT(reply.body["rightHand"]["x"].get<double>(), 0.0);
}

// Waits for the waveform player thread; returns whether `done` became true.
template <typename Predicate>
bool WaitFor(Probe& probe, Predicate done) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(probe.mutex);
      if (done())
        return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

// SetMotorState is a look-ahead waveform. Each sample plays for its own slot
// (a short lease, never the whole buffer), in order, then the hand stops.
TEST_F(RobloxHapticProtocolBridgeTest, SetMotorStateStepsThroughTheSamples) {
  ASSERT_TRUE(Initialize().ok());

  const char* payload = R"({
    "LeftHand": {"millisecondsPerSample": 20.0, "intensities": [0.0, 0.0]},
    "RightHand": {"millisecondsPerSample": 20.0,
                  "intensities": [0.2, 0.8, 0.5, 0.0, 0.0, 0.0]},
    "Small": {"millisecondsPerSample": 20.0, "intensities": [0.0, 0.0, 0.0]}
  })";

  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(Legacy("SetMotorState", payload).code, 0);
  ASSERT_TRUE(WaitFor(probe, [&] {
    return std::count(probe.recorded_stops.begin(), probe.recorded_stops.end(),
                      1) > 0;
  }));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  std::lock_guard<std::mutex> lock(probe.mutex);
  // The silent left hand stops at once; the right hand plays its samples in
  // order (a late wake may skip one) and nothing ever holds the peak.
  EXPECT_EQ(probe.recorded_stops.front(), 0);
  const std::vector<float> expected = {0.2f, 0.8f, 0.5f};
  std::size_t next = 0;
  for (const auto& sink : probe.recorded_sinks) {
    EXPECT_EQ(sink.hand, 1);
    EXPECT_EQ(sink.duration_ns, 50000000ULL);
    while (next < expected.size() && expected[next] != sink.amplitude)
      ++next;
    EXPECT_LT(next, expected.size()) << sink.amplitude << " out of order";
  }
  ASSERT_FALSE(probe.recorded_sinks.empty());
  EXPECT_FLOAT_EQ(probe.recorded_sinks.front().amplitude, 0.2f);
  // Three 20 ms samples: the trailing silence is not played.
  EXPECT_GE(elapsed, std::chrono::milliseconds(55));
  EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
}

// Roblox pads a short bump to 100 x 25 ms; it must end after the bump, not
// after 2.5 s.
TEST_F(RobloxHapticProtocolBridgeTest, SetMotorStateTrimsTrailingSilence) {
  ASSERT_TRUE(Initialize().ok());

  Json motor = {{"millisecondsPerSample", 25.0}};
  std::vector<double> intensities(100, 0.0);
  intensities[0] = 0.6;
  motor["intensities"] = intensities;
  const std::string payload = Json{{"RightHand", motor}}.dump();

  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(Legacy("SetMotorState", payload.c_str()).code, 0);
  ASSERT_TRUE(WaitFor(probe, [&] { return !probe.recorded_stops.empty(); }));
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::milliseconds(500));

  std::lock_guard<std::mutex> lock(probe.mutex);
  ASSERT_EQ(probe.recorded_sinks.size(), 1u);
  EXPECT_FLOAT_EQ(probe.recorded_sinks[0].amplitude, 0.6f);
  EXPECT_EQ(probe.recorded_stops, std::vector<int>{1});
}

// A newer buffer replaces the one playing, and shutting down stops the hand.
TEST_F(RobloxHapticProtocolBridgeTest, SetMotorStateFollowsTheLatestBuffer) {
  ASSERT_TRUE(Initialize().ok());

  const std::string longer =
      Json{{"LeftHand",
            {{"millisecondsPerSample", 25.0},
             {"intensities", std::vector<double>(40, 0.3)}}}}
          .dump();
  EXPECT_EQ(Legacy("SetMotorState", longer.c_str()).code, 0);
  ASSERT_TRUE(WaitFor(probe, [&] { return !probe.recorded_sinks.empty(); }));
  EXPECT_EQ(Legacy("SetMotorState",
                   R"({"LeftHand": {"millisecondsPerSample": 25.0,
                                    "intensities": [0.9]}})")
                .code,
            0);
  ASSERT_TRUE(WaitFor(probe, [&] {
    return !probe.recorded_sinks.empty() &&
           probe.recorded_sinks.back().amplitude == 0.9f;
  }));
  ASSERT_TRUE(WaitFor(probe, [&] { return !probe.recorded_stops.empty(); }));
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    EXPECT_EQ(probe.recorded_sinks.back().amplitude, 0.9f);
  }

  EXPECT_EQ(Legacy("SetMotorState", longer.c_str()).code, 0);
  ASSERT_TRUE(WaitFor(probe, [&] {
    return probe.recorded_sinks.back().amplitude == 0.3f;
  }));
  std::size_t stops_before = 0;
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    stops_before = probe.recorded_stops.size();
  }
  ASSERT_TRUE(bridge->Shutdown().ok());
  std::lock_guard<std::mutex> lock(probe.mutex);
  EXPECT_EQ(probe.recorded_stops.size(), stops_before + 1);
}

// Positional effects aimed at a hand sit at x = +-1 with a 0.5 radius, so the
// hand motors must be reported there.
TEST_F(RobloxHapticProtocolBridgeTest, GetMotorsPlacesHandsAtUnitOffsets) {
  ASSERT_TRUE(Initialize().ok());

  const auto reply = Legacy("GetMotors");
  for (const char* left : {"leftHand", "LeftHand"})
    EXPECT_DOUBLE_EQ(reply.body[left]["x"].get<double>(), -1.0) << left;
  for (const char* right : {"rightHand", "RightHand"})
    EXPECT_DOUBLE_EQ(reply.body[right]["x"].get<double>(), 1.0) << right;
}

TEST_F(RobloxHapticProtocolBridgeTest, UpdateSingletonVibrationBroadcasts) {
  ASSERT_TRUE(Initialize().ok());

  const auto reply = Legacy("UpdateSingletonVibration", R"({"intensity": 0.6})");
  EXPECT_EQ(reply.code, 0);
  ASSERT_EQ(probe.recorded_sinks.size(), 2u);
  EXPECT_EQ(probe.recorded_sinks[0].hand, 0);
  EXPECT_FLOAT_EQ(probe.recorded_sinks[0].amplitude, 0.6f);
  EXPECT_EQ(probe.recorded_sinks[1].hand, 1);
  EXPECT_FLOAT_EQ(probe.recorded_sinks[1].amplitude, 0.6f);

  const auto stop_reply =
      Legacy("UpdateSingletonVibration", R"({"intensity": 0.0})");
  EXPECT_EQ(stop_reply.code, 0);
  ASSERT_GE(probe.recorded_stops.size(), 2u);
}

TEST_F(RobloxHapticProtocolBridgeTest, RejectsMalformedJsonDefensively) {
  ASSERT_TRUE(Initialize().ok());

  const auto reply = Legacy("SetMotorState", "{not valid json");
  EXPECT_EQ(reply.code, 13);
  EXPECT_TRUE(probe.recorded_sinks.empty());
}

// The shared MessageBus bridge logs one line per call; the haptic bridge adds
// none of its own.
TEST_F(RobloxHapticProtocolBridgeTest, RumbleMessagesAddNoHapticLogLines) {
  ASSERT_TRUE(Initialize().ok());

  testing::internal::CaptureStderr();
  Legacy("SetMotorState", R"({"leftHand": {"intensities": [0.5]}})");
  Legacy("UpdateSingletonVibration", R"({"intensity": 0.5})");
  ASSERT_TRUE(WaitFor(probe, [&] { return probe.recorded_sinks.size() >= 3; }));
  EXPECT_EQ(testing::internal::GetCapturedStderr().find("[haptic]"),
            std::string::npos);
}

TEST(RobloxHapticProtocolBridgeSession, RunsOnlyDuringAVrSession) {
  unsetenv("MOCKTAIL_VR_ENABLED");
  EXPECT_FALSE(RobloxHapticProtocolBridge::EnabledForThisSession());
  ASSERT_EQ(setenv("MOCKTAIL_VR_ENABLED", "0", 1), 0);
  EXPECT_FALSE(RobloxHapticProtocolBridge::EnabledForThisSession());
  ASSERT_EQ(setenv("MOCKTAIL_VR_ENABLED", "1", 1), 0);
  EXPECT_TRUE(RobloxHapticProtocolBridge::EnabledForThisSession());
  unsetenv("MOCKTAIL_VR_ENABLED");
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
