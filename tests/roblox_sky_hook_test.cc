#include "runtime/roblox_sky_hook.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using mocktail::runtime::CursorHookSegment;
using mocktail::runtime::CustomSkyFaceContent;
using mocktail::runtime::LocateSkyTextureLoad;
using mocktail::runtime::SkyFaceOfLoad;

TEST(RobloxSkyHookTest, NamesTheFaceOfASkyLoad) {
  EXPECT_EQ(SkyFaceOfLoad("Sky.SkyboxBk"), "bk");
  EXPECT_EQ(SkyFaceOfLoad("Sky.SkyboxDn"), "dn");
  EXPECT_EQ(SkyFaceOfLoad("Sky.SkyboxFt"), "ft");
  EXPECT_EQ(SkyFaceOfLoad("Sky.SkyboxLf"), "lf");
  EXPECT_EQ(SkyFaceOfLoad("Sky.SkyboxRt"), "rt");
  EXPECT_EQ(SkyFaceOfLoad("Sky.SkyboxUp"), "up");
}

TEST(RobloxSkyHookTest, LeavesOtherLoadsAlone) {
  EXPECT_EQ(SkyFaceOfLoad(""), "");
  EXPECT_EQ(SkyFaceOfLoad("Sky.SkyboxXx"), "");
  EXPECT_EQ(SkyFaceOfLoad("Sky.SkyboxBkk"), "");
  EXPECT_EQ(SkyFaceOfLoad("Sky.SunTextureId"), "");
  // Reflections, not the sky that is drawn.
  EXPECT_EQ(SkyFaceOfLoad("IndoorSkybox.IndoorSkyboxBk"), "");
}

TEST(RobloxSkyHookTest, FaceContentIsTheFileTheOverlayReplaces) {
  EXPECT_EQ(CustomSkyFaceContent("bk"),
            "rbxasset://textures/sky/sky512_bk.tex");
}

// A code segment with the load function and callers that build a
// "Sky.Skybox.." name and call it, as in Roblox 2.746.
class SkyImage {
 public:
  static constexpr std::uintptr_t kCodeAddress = 0x400000;
  static constexpr std::size_t kLoad = 0x3000;
  static constexpr std::size_t kFirstCaller = 0x100;
  static constexpr std::size_t kCallerSpacing = 0x80;

  SkyImage() : code_(0x4000, 0xCC) {
    WritePrologue(kLoad);
    for (std::size_t caller = 0; caller < 12; ++caller) {
      WriteCaller(kFirstCaller + caller * kCallerSpacing, kLoad);
    }
  }

  void WritePrologue(std::size_t at) {
    const std::uint8_t prologue[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57,
                                     0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                     0x53, 0x48, 0x81, 0xEC, 0xF8, 0x01,
                                     0x00, 0x00};
    std::memcpy(code_.data() + at, prologue, sizeof(prologue));
  }

  // movabs rcx,"Sky.Skyb"; mov [rax+1],rcx; ...; push rax; call target
  void WriteCaller(std::size_t at, std::size_t target) {
    const std::uint8_t build[] = {0x48, 0xB9, 'S',  'k',  'y',  '.',  'S',
                                  'k',  'y',  'b',  0x48, 0x89, 0x48, 0x01,
                                  0xC7, 0x40, 0x09, 'o',  'x',  'L',  'f',
                                  0x6A, 0x00, 0x50};
    std::memcpy(code_.data() + at, build, sizeof(build));
    const std::size_t call = at + sizeof(build);
    code_[call] = 0xE8;
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::intptr_t>(target) -
        static_cast<std::intptr_t>(call + 5));
    std::memcpy(code_.data() + call + 1, &displacement, sizeof(displacement));
  }

  std::vector<CursorHookSegment> Segments() const {
    return {{kCodeAddress, code_.data(), code_.size(), true}};
  }

  std::vector<std::uint8_t>& code() { return code_; }

 private:
  std::vector<std::uint8_t> code_;
};

TEST(RobloxSkyHookTest, LocatesTheLoadEverySkyFaceCalls) {
  SkyImage image;
  std::string error;
  const std::optional<std::uintptr_t> load =
      LocateSkyTextureLoad(image.Segments(), &error);
  ASSERT_TRUE(load.has_value()) << error;
  EXPECT_EQ(*load, SkyImage::kCodeAddress + SkyImage::kLoad);
}

TEST(RobloxSkyHookTest, RefusesCallersThatDisagree) {
  SkyImage image;
  image.WritePrologue(0x3800);
  image.WriteCaller(SkyImage::kFirstCaller, 0x3800);
  std::string error;
  EXPECT_FALSE(LocateSkyTextureLoad(image.Segments(), &error).has_value());
  EXPECT_EQ(error, "sky face loads call more than one function");
}

TEST(RobloxSkyHookTest, RefusesTooFewCallers) {
  SkyImage image;
  for (std::size_t caller = 5; caller < 12; ++caller) {
    std::memset(image.code().data() + SkyImage::kFirstCaller +
                    caller * SkyImage::kCallerSpacing,
                0xCC, SkyImage::kCallerSpacing);
  }
  std::string error;
  EXPECT_FALSE(LocateSkyTextureLoad(image.Segments(), &error).has_value());
  EXPECT_EQ(error, "sky face loads not found");
}

TEST(RobloxSkyHookTest, RefusesAChangedPrologue) {
  SkyImage image;
  // push r14 where the hook relocates push r15.
  image.code()[SkyImage::kLoad + 5] = 0x56;
  std::string error;
  EXPECT_FALSE(LocateSkyTextureLoad(image.Segments(), &error).has_value());
  EXPECT_EQ(error, "sky face loads not found");
}

TEST(RobloxSkyHookTest, RefusesAnImageWithoutSkyLoads) {
  std::vector<std::uint8_t> code(0x100, 0xCC);
  std::string error;
  EXPECT_FALSE(
      LocateSkyTextureLoad({{0x1000, code.data(), code.size(), true}}, &error)
          .has_value());
  EXPECT_EQ(error, "sky face loads not found");
}

}  // namespace
