#include "runtime/roblox_haptic_protocol_bridge.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

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

TEST_F(RobloxHapticProtocolBridgeTest, SetMotorStateRoutesHands) {
  ASSERT_TRUE(Initialize().ok());

  const char* payload_left = R"({
    "leftHand": {
      "millisecondsPerSample": 20.0,
      "intensities": [0.2, 0.8, 0.5]
    }
  })";

  const auto reply = Legacy("SetMotorState", payload_left);
  EXPECT_EQ(reply.code, 0);
  ASSERT_EQ(probe.recorded_sinks.size(), 1u);
  EXPECT_EQ(probe.recorded_sinks[0].hand, 0);
  EXPECT_FLOAT_EQ(probe.recorded_sinks[0].amplitude, 0.8f);
  EXPECT_GE(probe.recorded_sinks[0].duration_ns, 50000000ULL);

  const char* payload_right = R"({
    "rightHand": {
      "millisecondsPerSample": 10.0,
      "intensities": [0.4]
    }
  })";

  const auto reply_right = Legacy("SetMotorState", payload_right);
  EXPECT_EQ(reply_right.code, 0);
  ASSERT_EQ(probe.recorded_sinks.size(), 2u);
  EXPECT_EQ(probe.recorded_sinks[1].hand, 1);
  EXPECT_FLOAT_EQ(probe.recorded_sinks[1].amplitude, 0.4f);
}

TEST_F(RobloxHapticProtocolBridgeTest, SetMotorStateZeroStopsHaptics) {
  ASSERT_TRUE(Initialize().ok());

  const char* payload = R"({
    "leftHand": {
      "millisecondsPerSample": 20.0,
      "intensities": [0.0]
    }
  })";

  const auto reply = Legacy("SetMotorState", payload);
  EXPECT_EQ(reply.code, 0);
  ASSERT_EQ(probe.recorded_stops.size(), 1u);
  EXPECT_EQ(probe.recorded_stops[0], 0);
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

}  // namespace
}  // namespace runtime
}  // namespace mocktail
