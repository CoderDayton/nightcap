#include "runtime/roblox_account_bridge.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

using Json = nlohmann::json;

// Distinct from the production values so tests prove the bridge uses the
// names the engine reports instead of hard-coded strings.
constexpr char kProtocol[] = "TestAccountProtocol";
constexpr char kAvailableMethod[] = "TestIntegrityAvailable";
constexpr char kTokenMethod[] = "TestIntegrityToken";
constexpr char kSupportKey[] = "testSupport";
constexpr char kTokenKey[] = "testToken";
constexpr char kResultKey[] = "testResult";
constexpr char kTokenRequest[] =
    R"({"requestHash":"opaque-hash","timeoutMillis":60000})";

std::string Copy(JNIEnv* env, jstring value) {
  if (!value) return {};
  const char* chars = env->GetStringUTFChars(value, nullptr);
  const std::string copy = chars ? chars : "";
  if (chars) env->ReleaseStringUTFChars(value, chars);
  return copy;
}

struct Probe {
  jnivm::VM* vm = nullptr;
  std::map<std::string, jobject> handlers;
  std::vector<std::string> native_cleared;
  int handlers_created = 0;
  int handlers_cleared = 0;
  int fail_handler_at = 0;
  bool null_token_key = false;
};

Probe* g_probe = nullptr;

void Prepare(void* context) {
  static_cast<jnivm::VM*>(context)->RestoreFunctions();
}

jstring ProtocolName(JNIEnv* env, jclass) { return env->NewStringUTF(kProtocol); }
jstring AvailableMethod(JNIEnv* env, jclass) {
  return env->NewStringUTF(kAvailableMethod);
}
jstring TokenMethod(JNIEnv* env, jclass) {
  return env->NewStringUTF(kTokenMethod);
}
jstring SupportKey(JNIEnv* env, jclass) {
  return env->NewStringUTF(kSupportKey);
}
jstring TokenKey(JNIEnv* env, jclass) {
  return g_probe->null_token_key ? nullptr : env->NewStringUTF(kTokenKey);
}
jstring ResultKey(JNIEnv* env, jclass) { return env->NewStringUTF(kResultKey); }

void SetHandler(JNIEnv* env, jobject, jstring protocol, jstring method,
                jobject handler) {
  EXPECT_EQ(Copy(env, protocol), kProtocol);
  g_probe->handlers[Copy(env, method)] = handler;
}

void ClearHandler(JNIEnv* env, jobject, jstring protocol, jstring method) {
  EXPECT_EQ(Copy(env, protocol), kProtocol);
  g_probe->native_cleared.push_back(Copy(env, method));
}

jobject CreateHandler(void* context, std::shared_ptr<void> target,
                      std::string (*run)(void*, JNIEnv*, jstring)) {
  auto* probe = static_cast<Probe*>(context);
  if (++probe->handlers_created == probe->fail_handler_at) return nullptr;
  return probe->vm->CreateMessageBusRequestHandler(
      std::move(target), jnivm::MessageBusRequestHandlerCallbacks{run});
}

void ClearHandlerObject(void* context, jobject handler) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->handlers_cleared;
  probe->vm->ClearMessageBusRequestHandler(handler);
}

RobloxAccountProtocolSymbols Symbols() {
  return {ProtocolName, AvailableMethod, TokenMethod, SupportKey,
          TokenKey,     ResultKey,       SetHandler,  ClearHandler};
}

class RobloxAccountBridgeTest : public testing::Test {
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
  Status Initialize(RobloxAccountProtocolSymbols symbols = Symbols()) {
    bridge = std::make_unique<RobloxAccountBridge>(
        JniEnvironmentProvider{vm.GetJavaVM(), &vm, Prepare}, symbols,
        RobloxAccountProtocolObjects{bus, &probe, CreateHandler,
                                     ClearHandlerObject});
    return bridge->Initialize();
  }
  Json Request(const char* method, const char* payload) {
    const auto found = probe.handlers.find(method);
    EXPECT_NE(found, probe.handlers.end()) << method;
    if (found == probe.handlers.end()) return nullptr;
    jstring message = env->NewStringUTF(payload);
    jstring response =
        vm.DispatchMessageBusRequestHandler(found->second, env, message);
    env->DeleteLocalRef(message);
    const std::string body = Copy(env, response);
    if (response) env->DeleteLocalRef(response);
    return Json::parse(body, nullptr, false);
  }

  jnivm::VM vm;
  Probe probe;
  JNIEnv* env = nullptr;
  jobject bus = nullptr;
  std::unique_ptr<RobloxAccountBridge> bridge;
};

TEST_F(RobloxAccountBridgeTest, RegistersBothHandlersUnderEngineNames) {
  ASSERT_TRUE(Initialize().ok());

  EXPECT_EQ(probe.handlers.size(), 2u);
  EXPECT_TRUE(probe.handlers.count(kAvailableMethod));
  EXPECT_TRUE(probe.handlers.count(kTokenMethod));
}

TEST_F(RobloxAccountBridgeTest, ReportsDeviceIntegrityUnsupported) {
  ASSERT_TRUE(Initialize().ok());

  EXPECT_EQ(Request(kAvailableMethod, "{}"), (Json{{kSupportKey, false}}));
}

TEST_F(RobloxAccountBridgeTest, AnswersTokenRequestWithoutToken) {
  ASSERT_TRUE(Initialize().ok());

  EXPECT_EQ(Request(kTokenMethod, kTokenRequest),
            (Json{{kTokenKey, ""}, {kResultKey, "TOKEN_PROVIDER_UNINITIALIZED"}}));
}

TEST_F(RobloxAccountBridgeTest, AnswersMalformedTokenRequestWithoutToken) {
  ASSERT_TRUE(Initialize().ok());

  EXPECT_EQ(Request(kTokenMethod, "not json"),
            (Json{{kTokenKey, ""}, {kResultKey, "TOKEN_PROVIDER_UNINITIALIZED"}}));
}

TEST_F(RobloxAccountBridgeTest, RejectsIncompletePrerequisites) {
  RobloxAccountProtocolSymbols symbols = Symbols();
  symbols.get_token_key = nullptr;

  EXPECT_FALSE(Initialize(symbols).ok());
  EXPECT_TRUE(probe.handlers.empty());
  EXPECT_EQ(probe.handlers_created, 0);
}

TEST_F(RobloxAccountBridgeTest, FailsWithoutRegisteringWhenEngineNameIsNull) {
  probe.null_token_key = true;

  EXPECT_FALSE(Initialize().ok());
  EXPECT_TRUE(probe.handlers.empty());
  EXPECT_EQ(probe.handlers_created, 0);
}

TEST_F(RobloxAccountBridgeTest, HandlerCreationFailureReleasesEarlierHandler) {
  probe.fail_handler_at = 2;

  EXPECT_FALSE(Initialize().ok());
  EXPECT_EQ(probe.handlers_cleared, 1);
  for (const auto& [method, handler] : probe.handlers) {
    EXPECT_NE(std::find(probe.native_cleared.begin(),
                        probe.native_cleared.end(), method),
              probe.native_cleared.end())
        << method;
  }
}

TEST_F(RobloxAccountBridgeTest, InitializeTwiceFails) {
  ASSERT_TRUE(Initialize().ok());

  EXPECT_FALSE(bridge->Initialize().ok());
  EXPECT_EQ(probe.handlers_created, 2);
}

TEST_F(RobloxAccountBridgeTest, ShutdownClearsBothHandlersOnce) {
  ASSERT_TRUE(Initialize().ok());

  EXPECT_TRUE(bridge->Shutdown().ok());
  EXPECT_TRUE(bridge->Shutdown().ok());
  EXPECT_EQ(probe.handlers_cleared, 2);
  EXPECT_EQ(probe.native_cleared,
            (std::vector<std::string>{kAvailableMethod, kTokenMethod}));
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
