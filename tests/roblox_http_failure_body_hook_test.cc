#include "runtime/roblox_http_failure_body_hook.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using mocktail::runtime::CursorHookSegment;
using mocktail::runtime::FormatHttpFailureBody;
using mocktail::runtime::HttpFailureBodyCall;
using mocktail::runtime::HttpFailureBodySeen;
using mocktail::runtime::LocateHttpFailureBodyCall;

constexpr char kFormat[] =
    "status:%d %s bodySize:%zu%s%s url:{ %s } ip:%s external:%d "
    "numberOfTimesRetried:%d";

// The trace function's shape in Roblox 2.748: a lea of the format string,
// then 0x260 bytes on, the body read and the cap comparison.
class TraceImage {
 public:
  static constexpr std::uintptr_t kCodeAddress = 0x400000;
  static constexpr std::uintptr_t kDataAddress = 0x100000;
  static constexpr std::size_t kFormatAt = 0x40;
  static constexpr std::size_t kFormatLea = 0x200;
  static constexpr std::size_t kBodyRead = kFormatLea + 0x260;
  static constexpr std::size_t kGetter = 0x2000;

  TraceImage() : code_(0x3000, 0xCC), data_(0x1000, 0) {
    std::memcpy(data_.data() + kFormatAt, kFormat, sizeof(kFormat));
    WriteRipLea(kFormatLea, {0x48, 0x8D, 0x35},
                kDataAddress + kFormatAt);
    WriteBodyRead(0x48);
    const std::uint8_t getter[] = {0x55, 0x48, 0x89, 0xE5};
    std::memcpy(code_.data() + kGetter, getter, sizeof(getter));
  }

  void WriteRipLea(std::size_t at, std::vector<std::uint8_t> opcode,
                   std::uintptr_t target) {
    std::memcpy(code_.data() + at, opcode.data(), opcode.size());
    const std::uintptr_t next = kCodeAddress + at + opcode.size() + 4;
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(next));
    std::memcpy(code_.data() + at + opcode.size(), &displacement,
                sizeof(displacement));
  }

  // lea rdi,[r14+offset]; call getter; mov r15,rax; lea rax,[rip+0];
  // movsxd r12,[rax]; cmp rdx,r12
  void WriteBodyRead(std::uint8_t offset) {
    const std::uint8_t lea[] = {0x49, 0x8D, 0x7E, offset};
    std::memcpy(code_.data() + kBodyRead, lea, sizeof(lea));
    const std::size_t call = kBodyRead + sizeof(lea);
    code_[call] = 0xE8;
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::intptr_t>(kGetter) -
        static_cast<std::intptr_t>(call + 5));
    std::memcpy(code_.data() + call + 1, &displacement, sizeof(displacement));
    const std::uint8_t after[] = {0x49, 0x89, 0xC7, 0x48, 0x8D, 0x05, 0x00,
                                  0x00, 0x00, 0x00, 0x4C, 0x63, 0x20, 0x4C,
                                  0x39, 0xE2};
    std::memcpy(code_.data() + call + 5, after, sizeof(after));
  }

  std::vector<CursorHookSegment> Segments() const {
    return {{kDataAddress, data_.data(), data_.size(), false},
            {kCodeAddress, code_.data(), code_.size(), true}};
  }

  std::vector<std::uint8_t>& code() { return code_; }
  std::vector<std::uint8_t>& data() { return data_; }

 private:
  std::vector<std::uint8_t> code_;
  std::vector<std::uint8_t> data_;
};

TEST(RobloxHttpFailureBodyHookTest, LocatesTheBodyReadAfterTheTraceLine) {
  TraceImage image;
  std::string error;
  const std::optional<HttpFailureBodyCall> found =
      LocateHttpFailureBodyCall(image.Segments(), &error);
  ASSERT_TRUE(found.has_value()) << error;
  EXPECT_EQ(found->call, TraceImage::kCodeAddress + TraceImage::kBodyRead + 4);
  EXPECT_EQ(found->getter, TraceImage::kCodeAddress + TraceImage::kGetter);
}

TEST(RobloxHttpFailureBodyHookTest, NeedsTheTraceFormat) {
  TraceImage image;
  image.data()[TraceImage::kFormatAt] = 'x';
  std::string error;
  EXPECT_FALSE(LocateHttpFailureBodyCall(image.Segments(), &error).has_value());
  EXPECT_FALSE(error.empty());
}

TEST(RobloxHttpFailureBodyHookTest, NeedsTheResponseBodyField) {
  TraceImage image;
  image.WriteBodyRead(0x50);
  std::string error;
  EXPECT_FALSE(LocateHttpFailureBodyCall(image.Segments(), &error).has_value());
}

TEST(RobloxHttpFailureBodyHookTest, NeedsTheSizeComparison) {
  TraceImage image;
  // cmp rdx,r12 becomes cmp rcx,r12.
  image.code()[TraceImage::kBodyRead + 9 + 15] = 0xE1;
  std::string error;
  EXPECT_FALSE(LocateHttpFailureBodyCall(image.Segments(), &error).has_value());
}

TEST(RobloxHttpFailureBodyHookTest, IgnoresABodyReadTooFarFromTheTraceLine) {
  TraceImage image;
  // Move the lea of the format string so the body read is out of range.
  std::memset(image.code().data() + TraceImage::kFormatLea, 0xCC, 7);
  image.WriteRipLea(0x10, {0x48, 0x8D, 0x35},
                    TraceImage::kDataAddress + TraceImage::kFormatAt);
  std::string error;
  EXPECT_FALSE(LocateHttpFailureBodyCall(image.Segments(), &error).has_value());
}

TEST(RobloxHttpFailureBodyHookTest, FormatsAJsonErrorAsIs) {
  EXPECT_EQ(FormatHttpFailureBody(
                R"({"errors":[{"code":0,"message":"Token Validation Failed"}]})"),
            R"({"errors":[{"code":0,"message":"Token Validation Failed"}]})");
}

TEST(RobloxHttpFailureBodyHookTest, EscapesControlAndNonAsciiBytes) {
  EXPECT_EQ(FormatHttpFailureBody(std::string("a\nb\x01\xC3\xA9", 6)),
            "a\\nb\\x01\\xc3\\xa9");
}

TEST(RobloxHttpFailureBodyHookTest, CutsALongBody) {
  const std::string body(1000, 'a');
  EXPECT_EQ(FormatHttpFailureBody(body),
            std::string(300, 'a') + "... (1000 bytes)");
}

TEST(RobloxHttpFailureBodyHookTest, RedactsJoinTickets) {
  EXPECT_EQ(FormatHttpFailureBody("joinScriptUrl ticket=abc123 rest"),
            "joinScriptUrl ticket=[redacted] rest");
}

TEST(RobloxHttpFailureBodyHookTest, LogsEachBodyOnce) {
  HttpFailureBodySeen seen;
  EXPECT_TRUE(seen.FirstTime(403, "denied"));
  EXPECT_FALSE(seen.FirstTime(403, "denied"));
  EXPECT_TRUE(seen.FirstTime(429, "denied"));
  EXPECT_TRUE(seen.FirstTime(403, "other"));
}

}  // namespace
