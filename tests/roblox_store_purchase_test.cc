#include "runtime/roblox_store_purchase.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

struct FinishedPurchase {
  bool success = true;
  jlong player_id = 0;
  std::string product_id;
  std::string checkout_session_id;
};

std::vector<FinishedPurchase>* g_finished = nullptr;
std::vector<std::string>* g_opened = nullptr;
bool g_open_result = true;

std::string Utf(JNIEnv* env, jstring value) {
  const char* chars = env->GetStringUTFChars(value, nullptr);
  std::string copy = chars == nullptr ? "" : chars;
  env->ReleaseStringUTFChars(value, chars);
  return copy;
}

void RecordFinished(JNIEnv* env, jclass, jboolean success, jlong player_id,
                    jstring product_id, jstring checkout_session_id) {
  g_finished->push_back({success == JNI_TRUE, player_id, Utf(env, product_id),
                         Utf(env, checkout_session_id)});
}

int* g_hand_offs = nullptr;

void RecordHandOff() { ++*g_hand_offs; }

std::map<std::string, std::string>* g_env = nullptr;

const char* FakeGetenv(const char* name) {
  auto it = g_env->find(name);
  return it == g_env->end() ? nullptr : it->second.c_str();
}

class RobloxStorePurchaseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_finished = &finished_;
    g_hand_offs = &hand_offs_;
    env_ = vm_.GetJNIEnv();
    native_gl_ = env_->FindClass("com/roblox/engine/jni/NativeGLInterface");
  }
  void TearDown() override {
    g_finished = nullptr;
    g_hand_offs = nullptr;
  }

  jnivm::VM vm_;
  JNIEnv* env_ = nullptr;
  jclass native_gl_ = nullptr;
  std::vector<FinishedPurchase> finished_;
  int hand_offs_ = 0;
};

TEST_F(RobloxStorePurchaseTest, QueueHandsOutEachPurchaseOnce) {
  RobloxStorePurchaseQueue queue;
  queue.Push(7, "com.roblox.robux80");
  queue.Push(8, "com.roblox.robux400");
  const std::vector<RobloxStorePurchase> taken = queue.Take();
  ASSERT_EQ(taken.size(), 2u);
  EXPECT_EQ(taken[0].player_id, 7);
  EXPECT_EQ(taken[0].product_id, "com.roblox.robux80");
  EXPECT_EQ(taken[1].player_id, 8);
  EXPECT_TRUE(queue.Take().empty());
}

TEST_F(RobloxStorePurchaseTest, HandsOffOnceAndReportsNoPurchase) {
  FinishStorePurchasesInBrowser(
      {{7, "com.roblox.robux80"}, {7, "com.roblox.robux400"}}, env_,
      native_gl_, &RecordFinished, &RecordHandOff);

  EXPECT_EQ(hand_offs_, 1);
  EXPECT_STREQ(kWebRobuxPurchaseUrl, "https://www.roblox.com/upgrades/robux");
  ASSERT_EQ(finished_.size(), 2u);
  for (const FinishedPurchase& purchase : finished_) {
    EXPECT_FALSE(purchase.success);
    EXPECT_EQ(purchase.player_id, 7);
    EXPECT_EQ(purchase.checkout_session_id, "");
  }
  EXPECT_EQ(finished_[0].product_id, "com.roblox.robux80");
  EXPECT_EQ(finished_[1].product_id, "com.roblox.robux400");
}

TEST_F(RobloxStorePurchaseTest, DoesNothingWithoutPurchases) {
  FinishStorePurchasesInBrowser({}, env_, native_gl_, &RecordFinished,
                                &RecordHandOff);
  EXPECT_EQ(hand_offs_, 0);
  EXPECT_TRUE(finished_.empty());
}

TEST_F(RobloxStorePurchaseTest, StillOpensTheBrowserWithoutTheEngineCall) {
  FinishStorePurchasesInBrowser({{7, "com.roblox.robux80"}}, env_, native_gl_,
                                nullptr, &RecordHandOff);
  EXPECT_EQ(hand_offs_, 1);
  EXPECT_TRUE(finished_.empty());
}

TEST(RobloxPurchaseRepeatGuardTest, AllowsOneHandOffPerWindow) {
  using std::chrono::milliseconds;
  const std::chrono::steady_clock::time_point start{};
  RepeatGuard guard(milliseconds(2000));
  EXPECT_TRUE(guard.Allow(start + milliseconds(10000)));
  EXPECT_FALSE(guard.Allow(start + milliseconds(10500)));
  EXPECT_FALSE(guard.Allow(start + milliseconds(11999)));
  EXPECT_TRUE(guard.Allow(start + milliseconds(12000)));
}

TEST(RobloxBrowserEnvironmentTest, RestoresTheHostPathsSavedByTheLauncher) {
  std::map<std::string, std::string> env = {
      {"MOCKTAIL_HOST_ENV", "1"},
      {"MOCKTAIL_HOST_PATH", "/usr/bin:/bin"},
      {"MOCKTAIL_HOST_LD_LIBRARY_PATH", "/opt/lib"},
  };
  g_env = &env;
  std::vector<EnvironmentChange> changes = BrowserEnvironmentChanges(&FakeGetenv);
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[0].name, "PATH");
  EXPECT_EQ(changes[0].value, "/usr/bin:/bin");
  EXPECT_EQ(changes[1].name, "LD_LIBRARY_PATH");
  EXPECT_EQ(changes[1].value, "/opt/lib");

  env.erase("MOCKTAIL_HOST_LD_LIBRARY_PATH");
  changes = BrowserEnvironmentChanges(&FakeGetenv);
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[1].name, "LD_LIBRARY_PATH");
  EXPECT_FALSE(changes[1].value.has_value());
  g_env = nullptr;
}

TEST(RobloxBrowserEnvironmentTest, LeavesTheEnvironmentAloneWithoutTheLauncher) {
  std::map<std::string, std::string> env = {
      {"PATH", "/usr/bin"},
      {"LD_LIBRARY_PATH", "/home/user/lib"},
  };
  g_env = &env;
  EXPECT_TRUE(BrowserEnvironmentChanges(&FakeGetenv).empty());
  g_env = nullptr;
}

}
}
}
