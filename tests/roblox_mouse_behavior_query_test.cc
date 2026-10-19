#include "runtime/roblox_mouse_behavior_query.h"

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace {

using mocktail::runtime::FindLockCenterCompare;
using mocktail::runtime::LocateLockCenterCompare;
using mocktail::runtime::PrepareLockCenterCompare;

const std::vector<std::uint8_t> kInlinedQuery = {
    0x48, 0x8b, 0x83, 0x10, 0x0a, 0x00, 0x00,
    0x83, 0xb8, 0x88, 0x00, 0x00, 0x00, 0x01,
    0x0f, 0x94, 0xc3,
    0x5b, 0x5d, 0xc3};

const std::vector<std::uint8_t> kHelperQuery = {
    0x49, 0x8b, 0x86, 0xf0, 0x09, 0x00, 0x00,
    0x44, 0x8b, 0xb0, 0x88, 0x00, 0x00, 0x00,
    0xbe, 0x01, 0x00, 0x00, 0x00,
    0x41, 0x83, 0xfe, 0x01,
    0x0f, 0x94, 0xc0,
    0x5b, 0x5d, 0xc3};

TEST(RobloxMouseBehaviorQueryTest, FindsTheCompareAgainstAMemoryOperand) {
  EXPECT_EQ(FindLockCenterCompare(kInlinedQuery.data(), kInlinedQuery.size()),
            std::optional<std::size_t>(13));
}

TEST(RobloxMouseBehaviorQueryTest, FindsTheCompareAgainstARegister) {
  EXPECT_EQ(FindLockCenterCompare(kHelperQuery.data(), kHelperQuery.size()),
            std::optional<std::size_t>(22));
}

TEST(RobloxMouseBehaviorQueryTest, IgnoresAOneThatIsNotACompare) {
  const std::vector<std::uint8_t> code = {0xbe, 0x01, 0x0f, 0x94, 0xc0, 0xc3};
  EXPECT_EQ(FindLockCenterCompare(code.data(), code.size()), std::nullopt);
}

TEST(RobloxMouseBehaviorQueryTest, FollowsACallIntoTheHelper) {
  std::vector<std::uint8_t> code = {
      0xbf, 0x04, 0x00, 0x00, 0x00,
      0xe8, 0x00, 0x00, 0x00, 0x00,
      0x48, 0x89, 0xc7,
      0xe8, 0x00, 0x00, 0x00, 0x00,
      0x5b, 0x5d, 0xc3};
  const std::size_t getter = code.size();
  code.insert(code.end(), {0x31, 0xc0, 0x5d, 0xc3});
  const std::size_t helper = code.size();
  code.insert(code.end(), kHelperQuery.begin(), kHelperQuery.end());
  const auto rel = [&](std::size_t call, std::size_t target) {
    const auto value =
        static_cast<std::int32_t>(target) - static_cast<std::int32_t>(call + 5);
    std::memcpy(&code[call + 1], &value, sizeof(value));
  };
  rel(5, getter);
  rel(13, helper);
  const std::uint8_t* end = code.data() + code.size();
  EXPECT_EQ(LocateLockCenterCompare(code.data(), code.data(), end),
            code.data() + helper + 22);
}

TEST(RobloxMouseBehaviorQueryTest, SkipsACallThatLeavesTheCode) {
  std::vector<std::uint8_t> code = {0xe8, 0x00, 0x00, 0x00, 0x40,
                                    0x5b, 0x5d, 0xc3};
  const std::size_t helper = code.size();
  code.insert(code.end(), kHelperQuery.begin(), kHelperQuery.end());
  EXPECT_EQ(
      LocateLockCenterCompare(code.data(), code.data(), code.data() + helper),
      nullptr);
}

TEST(RobloxMouseBehaviorQueryTest, StopsReadingAtTheEndOfTheCode) {
  std::vector<std::uint8_t> code = {0x90, 0x90};
  code.insert(code.end(), kHelperQuery.begin(), kHelperQuery.end());
  EXPECT_EQ(LocateLockCenterCompare(code.data(), code.data(), code.data() + 20),
            nullptr);
}

TEST(RobloxMouseBehaviorQueryTest, PreparedCompareCanBeRewrittenAndStillRuns) {
  const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  void* memory = mmap(nullptr, page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(memory, MAP_FAILED);
  std::memcpy(memory, kHelperQuery.data(), kHelperQuery.size());
  ASSERT_EQ(mprotect(memory, page, PROT_READ | PROT_EXEC), 0);

  std::uint8_t* compare = PrepareLockCenterCompare(memory);
  ASSERT_EQ(compare, static_cast<std::uint8_t*>(memory) + 22);
  *compare = 2;
  EXPECT_EQ(*compare, 2);
  *compare = 1;
  munmap(memory, page);
}

TEST(RobloxMouseBehaviorQueryTest, FindsNothingWithoutACompare) {
  std::vector<std::uint8_t> code(300, 0x90);
  code.insert(code.end(), kHelperQuery.begin(), kHelperQuery.end());
  const std::uint8_t* end = code.data() + code.size();
  EXPECT_EQ(LocateLockCenterCompare(code.data(), code.data(), end), nullptr);
}

}
