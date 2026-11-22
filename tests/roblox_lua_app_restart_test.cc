#include "runtime/roblox_lua_app_restart.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

struct Probe {
  std::vector<std::string> calls;
  int fail_after_call = -1;
  bool pending_exception = false;
  int exceptions_cleared = 0;
  bool fail_string_allocation = false;
};

Probe* g_probe = nullptr;

void Record(const std::string& call) {
  g_probe->calls.push_back(call);
  if (static_cast<int>(g_probe->calls.size()) == g_probe->fail_after_call) {
    g_probe->pending_exception = true;
  }
}

void Pause(JNIEnv*, jclass) { Record("pause"); }

void Background(JNIEnv*, jclass, jboolean enabled, jstring reason) {
  Record(std::string(enabled == JNI_TRUE ? "background:" : "foreground:") +
         reinterpret_cast<const char*>(reason));
}

void Start(JNIEnv*, jclass, jobject params) {
  Record(params == reinterpret_cast<jobject>(0x10) ? "start-app"
                                                   : "start-app:wrong-params");
}

jstring NewStringUtf(JNIEnv*, const char* text) {
  return g_probe->fail_string_allocation
             ? nullptr
             : reinterpret_cast<jstring>(const_cast<char*>(text));
}

void DeleteLocalRef(JNIEnv*, jobject) {}

jboolean ExceptionCheck(JNIEnv*) {
  return g_probe->pending_exception ? JNI_TRUE : JNI_FALSE;
}

void ExceptionClear(JNIEnv*) {
  g_probe->pending_exception = false;
  ++g_probe->exceptions_cleared;
}

struct FakeJni {
  FakeJni() {
    functions.NewStringUTF = NewStringUtf;
    functions.DeleteLocalRef = DeleteLocalRef;
    functions.ExceptionCheck = ExceptionCheck;
    functions.ExceptionClear = ExceptionClear;
    env.functions = &functions;
  }

  JNINativeInterface_ functions{};
  JNIEnv_ env{};
};

class RestartLuaAppTest : public testing::Test {
 protected:
  void SetUp() override { g_probe = &probe_; }
  void TearDown() override { g_probe = nullptr; }

  Status Restart() {
    const LuaAppRestartSymbols symbols{Pause, Background, Start};
    return RestartLuaApp(&jni_.env, reinterpret_cast<jclass>(0x1), symbols,
                         reinterpret_cast<jobject>(0x10));
  }

  Probe probe_;
  FakeJni jni_;
};

TEST_F(RestartLuaAppTest, PausesCyclesTheSchedulerThenStartsWithTheParams) {
  EXPECT_TRUE(Restart().ok());

  EXPECT_EQ(probe_.calls,
            (std::vector<std::string>{"pause", "background:ASMA.stop",
                                      "foreground:ASMA.start", "start-app"}));
}

TEST_F(RestartLuaAppTest, StopsAtTheFirstStepThatRaisesAnException) {
  probe_.fail_after_call = 3;  // The foreground step.

  const Status status = Restart();

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kPlatformError);
  EXPECT_EQ(status.message(),
            "foreground LuaApp for surface recreation failed");
  EXPECT_EQ(probe_.calls.size(), 3u);
  EXPECT_EQ(probe_.exceptions_cleared, 1);
}

TEST_F(RestartLuaAppTest, ReportsAnExceptionFromTheFinalStart) {
  probe_.fail_after_call = 4;

  const Status status = Restart();

  EXPECT_EQ(status.code(), StatusCode::kPlatformError);
  EXPECT_EQ(status.message(), "restart LuaApp with recreated surface failed");
}

TEST_F(RestartLuaAppTest, CallsNothingWhenTheReasonStringsCannotBeAllocated) {
  probe_.fail_string_allocation = true;

  const Status status = Restart();

  EXPECT_EQ(status.code(), StatusCode::kUnavailable);
  EXPECT_TRUE(probe_.calls.empty());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
