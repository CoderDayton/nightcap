#include "mocktail/platform/android_log_observer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" int __android_log_write(int priority, const char* tag,
                                   const char* text);

namespace {

std::vector<std::string> g_first;
std::vector<std::string> g_second;

void First(int, const char*, const char* message) {
  g_first.emplace_back(message);
}

void Second(int, const char*, const char* message) {
  g_second.emplace_back(message);
}

TEST(AndroidLogObserverTest, EveryObserverSeesEachLine) {
  ASSERT_TRUE(mocktail_android_log_add_observer(&First));
  ASSERT_TRUE(mocktail_android_log_add_observer(&Second));
  __android_log_write(4, "Roblox", "one");
  mocktail_android_log_remove_observer(&First);
  __android_log_write(4, "Roblox", "two");
  mocktail_android_log_remove_observer(&Second);
  __android_log_write(4, "Roblox", "three");

  EXPECT_EQ(g_first, (std::vector<std::string>{"one"}));
  EXPECT_EQ(g_second, (std::vector<std::string>{"one", "two"}));
}

TEST(AndroidLogObserverTest, AddingTheSameObserverTwiceKeepsOneCopy) {
  g_first.clear();
  ASSERT_TRUE(mocktail_android_log_add_observer(&First));
  ASSERT_TRUE(mocktail_android_log_add_observer(&First));
  __android_log_write(4, "Roblox", "once");
  mocktail_android_log_remove_observer(&First);
  EXPECT_EQ(g_first, (std::vector<std::string>{"once"}));
}

}
