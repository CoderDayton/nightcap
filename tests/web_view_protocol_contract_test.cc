#include "compat/web_view_protocol_contract.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mocktail::compat {
namespace {

std::string FromHex(std::string_view hex) {
  std::string bytes;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    bytes += static_cast<char>(
        std::stoi(std::string(hex.substr(index, 2)), nullptr, 16));
  }
  return bytes;
}

std::string Flipped(std::string code, std::size_t index) {
  code[index] = static_cast<char>(code[index] ^ 0x01);
  return code;
}

// Start of initializeAndroidWebViewProtocol and the getter it calls first,
// from the x86_64 and arm64-v8a libroblox.so of each named Roblox release.
struct KnownBuild {
  const char* name;
  std::uintptr_t initializer;
  const char* initializer_code;
  std::uintptr_t getter;
  const char* getter_code;
};

constexpr KnownBuild kX86_64Builds[] = {
    {"2.734.917", 0x2e7418f,
     "554889e54157415641554154534883ec384c8b2599ce1004498b0424488945d0488d5da8"
     "4889dfe8c9a355ff488b1b488b43184c",
     0x23ce584,
     "554889e553504889fb8a05ede9c10484c07419488d3dbae9c104e8590000004889034889"
     "d84883c4085b5dc3"},
    {"2.736.1408", 0x2e9d241,
     "554889e54157415641554154534883ec484c8b25df291c04498b0424488945d0488d5d98"
     "4889dfe8af2d56ff488b1b488b43184c",
     0x240001c,
     "554889e553504889fb8a05a5b9cc0484c07419488d3d72b9cc04e8590000004889034889"
     "d84883c4085b5dc3"},
    {"2.738.1397", 0x2e6c591,
     "554889e54157415641554154534883ec184c8b2577d52704498b0424488945d0488d5dc0"
     "4889dfe8fbf55cff488b1b488b43184c",
     0x243bbb8,
     "554889e553504889fb8a05f99dd10484c07419488d3dc69dd104e8590000004889034889"
     "d84883c4085b5dc3"},
};

constexpr KnownBuild kArm64Builds[] = {
    {"2.738.1397", 0x2bac4bc,
     "ffc301d1fd7b02a9f91b00f9f85f04a9f65705a9f44f06a9fd83009137e101b0f77a43f9"
     "e80240f9a8831ff8e8430091e82300915244de97f30740f9680e40f9",
     0x233d638,
     "fd7bbea9f30b00f9fd030091e92702f029e13391f30308aa29fddf0809010036e02702f0"
     "0040339117000094600200f9f30b40f9fd7bc2a8c0035fd6"},
    {"2.740.931", 0x2be87d8,
     "ff0302d1fd7b03a9f92300f9f85f05a9f65706a9f44f07a9fdc3009157e801d0f7b244f9"
     "e80240f9a8831ff8e84b0091e82300913a64de97f30740f9680e40f9",
     0x23818f4,
     "fd7bbea9f30b00f9fd030091492f02d029212a91f30308aa29fddf0809010036402f02d0"
     "0080299117000094600200f9f30b40f9fd7bc2a8c0035fd6"},
};

TEST(WebViewProtocolContractTest, FindsX86_64GetterInKnownBuilds) {
  for (const KnownBuild& build : kX86_64Builds) {
    EXPECT_EQ(FindWebViewProtocolGetterCallX86_64(
                  FromHex(build.initializer_code), build.initializer),
              std::optional<std::uintptr_t>(build.getter))
        << build.name;
    const std::string getter = FromHex(build.getter_code);
    ASSERT_EQ(getter.size(), kWebViewProtocolGetterBytesX86_64) << build.name;
    EXPECT_TRUE(HasWebViewProtocolGetterContractX86_64(getter)) << build.name;
  }
}

TEST(WebViewProtocolContractTest, RejectsChangedX86_64CallSite) {
  const KnownBuild& build = kX86_64Builds[2];
  const std::string code = FromHex(build.initializer_code);
  // Frame setup, the handle slot's lea, the handle argument, the call, the
  // handle load and the protocol field offset.
  for (const std::size_t fixed : {0u, 3u, 32u, 34u, 36u, 39u, 44u, 50u}) {
    EXPECT_FALSE(FindWebViewProtocolGetterCallX86_64(Flipped(code, fixed),
                                                     build.initializer)
                     .has_value())
        << fixed;
  }
  EXPECT_FALSE(FindWebViewProtocolGetterCallX86_64(code.substr(0, 50),
                                                   build.initializer)
                   .has_value());
}

TEST(WebViewProtocolContractTest, RejectsAmbiguousX86_64CallSite) {
  const KnownBuild& build = kX86_64Builds[2];
  const std::string code = FromHex(build.initializer_code);
  EXPECT_FALSE(FindWebViewProtocolGetterCallX86_64(code + code.substr(32, 19),
                                                   build.initializer)
                   .has_value());
}

TEST(WebViewProtocolContractTest, RejectsChangedX86_64Getter) {
  const std::string getter = FromHex(kX86_64Builds[2].getter_code);
  // Frame setup, the out-pointer save, the guard test, the holder lea, the
  // local acquire call, the out-pointer store and the return.
  for (const std::size_t fixed : {0u, 8u, 16u, 21u, 27u, 33u, 43u}) {
    EXPECT_FALSE(HasWebViewProtocolGetterContractX86_64(Flipped(getter, fixed)))
        << fixed;
  }
  EXPECT_FALSE(HasWebViewProtocolGetterContractX86_64(
      getter.substr(0, getter.size() - 1)));
}

TEST(WebViewProtocolContractTest, FindsArm64GetterInKnownBuilds) {
  for (const KnownBuild& build : kArm64Builds) {
    EXPECT_EQ(FindWebViewProtocolGetterCallArm64(
                  FromHex(build.initializer_code), build.initializer),
              std::optional<std::uintptr_t>(build.getter))
        << build.name;
    const std::string getter = FromHex(build.getter_code);
    ASSERT_EQ(getter.size(), kWebViewProtocolGetterBytesArm64) << build.name;
    EXPECT_TRUE(HasWebViewProtocolGetterContractArm64(getter)) << build.name;
  }
}

TEST(WebViewProtocolContractTest, RejectsChangedArm64CallSite) {
  const KnownBuild& build = kArm64Builds[0];
  const std::string code = FromHex(build.initializer_code);
  // Words 12..15: add x8, sp, #8; bl getter; ldr x19, [sp, #8];
  // ldr x8, [x19, #0x18]. Byte 0 of each word holds its low register field.
  EXPECT_FALSE(
      FindWebViewProtocolGetterCallArm64(Flipped(code, 48), build.initializer)
          .has_value());
  std::string not_a_call = code;
  not_a_call[55] = static_cast<char>(not_a_call[55] ^ 0x80);  // bl -> b
  EXPECT_FALSE(FindWebViewProtocolGetterCallArm64(not_a_call, build.initializer)
                   .has_value());
  EXPECT_FALSE(
      FindWebViewProtocolGetterCallArm64(Flipped(code, 56), build.initializer)
          .has_value());
  EXPECT_FALSE(
      FindWebViewProtocolGetterCallArm64(Flipped(code, 60), build.initializer)
          .has_value());
  // The handle is read back from a different stack slot than the one passed.
  std::string other_slot = code;
  other_slot[57] = static_cast<char>(other_slot[57] ^ 0x04);
  EXPECT_FALSE(FindWebViewProtocolGetterCallArm64(other_slot, build.initializer)
                   .has_value());
  EXPECT_FALSE(FindWebViewProtocolGetterCallArm64(code.substr(0, 60),
                                                  build.initializer)
                   .has_value());
}

TEST(WebViewProtocolContractTest, RejectsAmbiguousArm64CallSite) {
  const KnownBuild& build = kArm64Builds[0];
  const std::string code = FromHex(build.initializer_code);
  EXPECT_FALSE(FindWebViewProtocolGetterCallArm64(code + code.substr(48, 16),
                                                  build.initializer)
                   .has_value());
}

TEST(WebViewProtocolContractTest, RejectsChangedArm64Getter) {
  const std::string getter = FromHex(kArm64Builds[0].getter_code);
  // Frame setup, mov x19, x8 (the indirect result register), the guard
  // branch, the local acquire call, the result store and the return.
  for (const std::size_t fixed : {0u, 20u, 28u, 40u, 44u, 56u}) {
    EXPECT_FALSE(HasWebViewProtocolGetterContractArm64(Flipped(getter, fixed)))
        << fixed;
  }
  EXPECT_FALSE(HasWebViewProtocolGetterContractArm64(
      getter.substr(0, getter.size() - 1)));
}

TEST(WebViewProtocolContractTest, RejectsOtherArchitectureCode) {
  const KnownBuild& x86_64 = kX86_64Builds[2];
  const KnownBuild& arm64 = kArm64Builds[0];
  EXPECT_FALSE(FindWebViewProtocolGetterCallArm64(
                   FromHex(x86_64.initializer_code), x86_64.initializer)
                   .has_value());
  EXPECT_FALSE(FindWebViewProtocolGetterCallX86_64(
                   FromHex(arm64.initializer_code), arm64.initializer)
                   .has_value());
  EXPECT_FALSE(HasWebViewProtocolGetterContractArm64(
      FromHex(x86_64.getter_code) + std::string(16, '\0')));
  EXPECT_FALSE(HasWebViewProtocolGetterContractX86_64(
      FromHex(arm64.getter_code)));
}

}  // namespace
}  // namespace mocktail::compat
