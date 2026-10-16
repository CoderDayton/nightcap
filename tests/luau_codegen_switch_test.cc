#include "runtime/luau_codegen_switch.h"

#include <elf.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mocktail::runtime {
namespace {

constexpr char kStrings[] = "\0FFlagOther\0DebugLuauCodegenAll\0tail";

struct alignas(8) FlagRecord {
  std::uint8_t value = 1;
  std::uint8_t padding[7] = {};
  const char* name = nullptr;
  std::uint64_t name_length = 0;
  std::uint64_t next = 0;
};

const char* NameInStrings() {
  return kStrings + 12;
}

std::vector<LoadedImageSegment> Segments(std::vector<FlagRecord>& records) {
  return {
      {reinterpret_cast<std::uintptr_t>(kStrings), sizeof(kStrings), false},
      {reinterpret_cast<std::uintptr_t>(records.data()),
       records.size() * sizeof(FlagRecord), true},
  };
}

TEST(LuauCodegenSwitchTest, FindsTheFlagRecordByName) {
  ASSERT_EQ(std::string(NameInStrings()), "DebugLuauCodegenAll");
  std::vector<FlagRecord> records(3);
  records[1].name = NameInStrings();
  records[1].name_length = 19;
  const auto segments = Segments(records);
  EXPECT_EQ(FindFastFlagValue(segments, kLuauCodegenAllFastFlagName),
            &records[1].value);
}

TEST(LuauCodegenSwitchTest, RejectsWrongLengthMissingOrDuplicateRecords) {
  std::vector<FlagRecord> records(3);
  records[1].name = NameInStrings();
  records[1].name_length = 18;
  auto segments = Segments(records);
  EXPECT_EQ(FindFastFlagValue(segments, kLuauCodegenAllFastFlagName), nullptr);

  records[1].name_length = 19;
  EXPECT_EQ(FindFastFlagValue(segments, "DebugLuauCodegen"), nullptr);

  records[2].name = NameInStrings();
  records[2].name_length = 19;
  EXPECT_EQ(FindFastFlagValue(segments, kLuauCodegenAllFastFlagName), nullptr);
}

TEST(LuauCodegenSwitchTest, IgnoresRecordsInReadOnlySegments) {
  std::vector<FlagRecord> records(1);
  records[0].name = NameInStrings();
  records[0].name_length = 19;
  auto segments = Segments(records);
  segments[1].writable = false;
  EXPECT_EQ(FindFastFlagValue(segments, kLuauCodegenAllFastFlagName), nullptr);
}

TEST(LuauCodegenSwitchTest, ReadsLoadSegmentsFromTheElfHeader) {
  alignas(8) std::uint8_t image[0x400] = {};
  auto* header = reinterpret_cast<Elf64_Ehdr*>(image);
  std::memcpy(header->e_ident, ELFMAG, SELFMAG);
  header->e_ident[EI_CLASS] = ELFCLASS64;
  header->e_phoff = sizeof(Elf64_Ehdr);
  header->e_phnum = 3;
  auto* program = reinterpret_cast<Elf64_Phdr*>(image + header->e_phoff);
  program[0] = {PT_LOAD, PF_R | PF_X, 0, 0, 0, 0x200, 0x200, 0x1000};
  program[1] = {PT_DYNAMIC, PF_R | PF_W, 0, 0x200, 0, 0x10, 0x10, 8};
  program[2] = {PT_LOAD, PF_R | PF_W, 0, 0x200, 0, 0x80, 0x180, 0x1000};
  const auto base = reinterpret_cast<std::uintptr_t>(image);
  const auto segments = LoadedImageSegments(base);
  ASSERT_EQ(segments.size(), 2u);
  EXPECT_EQ(segments[0].address, base);
  EXPECT_EQ(segments[0].size, 0x200u);
  EXPECT_FALSE(segments[0].writable);
  EXPECT_EQ(segments[1].address, base + 0x200);
  EXPECT_EQ(segments[1].size, 0x180u);
  EXPECT_TRUE(segments[1].writable);

  std::memset(image, 0, SELFMAG);
  EXPECT_TRUE(LoadedImageSegments(base).empty());
}

TEST(LuauCodegenSwitchTest, DetectsAUserSetting) {
  EXPECT_FALSE(ClientSettingsSetLuauCodegenAll("{}"));
  EXPECT_FALSE(ClientSettingsSetLuauCodegenAll(R"({"FIntOther":1})"));
  EXPECT_FALSE(ClientSettingsSetLuauCodegenAll("not-json"));
  EXPECT_TRUE(
      ClientSettingsSetLuauCodegenAll(R"({"FFlagDebugLuauCodegenAll":true})"));
  EXPECT_TRUE(
      ClientSettingsSetLuauCodegenAll(R"({"FFlagDebugLuauCodegenAll":"False"})"));
}

TEST(LuauCodegenSwitchTest, EnablesCodegenOnlyWhileBrowsing) {
  EXPECT_TRUE(LuauCodegenAllForPhase(RobloxExperiencePresencePhase::kBrowsing));
  EXPECT_FALSE(LuauCodegenAllForPhase(RobloxExperiencePresencePhase::kJoining));
  EXPECT_FALSE(LuauCodegenAllForPhase(RobloxExperiencePresencePhase::kPlaying));
}

struct Forwarded {
  int count = 0;
  RobloxExperiencePresencePhase phase = RobloxExperiencePresencePhase::kBrowsing;
};

void RecordForward(void* context, RobloxExperiencePresencePhase phase,
                   const RobloxExperienceLaunchRequest*) {
  auto* forwarded = static_cast<Forwarded*>(context);
  ++forwarded->count;
  forwarded->phase = phase;
}

TEST(LuauCodegenSwitchTest, WritesTheFlagPerPhaseAndForwards) {
  std::uint8_t flag = 1;
  int locate_calls = 0;
  Forwarded forwarded;
  LuauCodegenMenuSwitch menu_switch(
      [&] {
        ++locate_calls;
        return &flag;
      },
      {&forwarded, &RecordForward});
  const RobloxExperiencePresenceObserver observer = menu_switch.observer();
  ASSERT_TRUE(observer.valid());

  observer.notify(observer.context, RobloxExperiencePresencePhase::kJoining,
                  nullptr);
  EXPECT_EQ(flag, 0);
  observer.notify(observer.context, RobloxExperiencePresencePhase::kPlaying,
                  nullptr);
  EXPECT_EQ(flag, 0);
  observer.notify(observer.context, RobloxExperiencePresencePhase::kBrowsing,
                  nullptr);
  EXPECT_EQ(flag, 1);
  EXPECT_EQ(locate_calls, 1);
  EXPECT_EQ(forwarded.count, 3);
  EXPECT_EQ(forwarded.phase, RobloxExperiencePresencePhase::kBrowsing);
}

TEST(LuauCodegenSwitchTest, LeavesUnexpectedValuesAndMissingFlagsAlone) {
  std::uint8_t flag = 7;
  LuauCodegenMenuSwitch menu_switch([&] { return &flag; }, {});
  menu_switch.OnPhase(RobloxExperiencePresencePhase::kJoining, nullptr);
  EXPECT_EQ(flag, 7);

  LuauCodegenMenuSwitch missing([] { return nullptr; }, {});
  missing.OnPhase(RobloxExperiencePresencePhase::kJoining, nullptr);
  EXPECT_TRUE(missing.observer().valid());
}

}  // namespace
}  // namespace mocktail::runtime
