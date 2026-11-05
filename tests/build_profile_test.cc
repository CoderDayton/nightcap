#include "compat/build_profile.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#ifndef MOCKTAIL_TEST_SOURCE_DIR
#error "MOCKTAIL_TEST_SOURCE_DIR must point at the Mocktail source tree"
#endif

namespace mocktail::compat {
namespace {

const std::string kManifestPath =
    std::string(MOCKTAIL_TEST_SOURCE_DIR) + "/config/roblox_compatibility.json";
constexpr char kCurrentBuildId[] = "1f8b6cc6597d6ae49104d32f0a30c79d029f400d";

nlohmann::json ReadManifest() {
  nlohmann::json document;
  std::ifstream input(kManifestPath);
  input >> document;
  return document;
}

nlohmann::json& CurrentEntry(nlohmann::json& document) {
  auto& profiles = document["profiles"];
  const auto found = std::find_if(
      profiles.begin(), profiles.end(), [](const auto& profile) {
        return profile.value("elf_build_id", "") == kCurrentBuildId;
      });
  if (found == profiles.end()) {
    throw std::runtime_error("manifest has no current payload profile");
  }
  return *found;
}

TEST(BuildProfileTest, FindsCurrentPayloadAsSupported) {
  const ProfileLookupResult result =
      FindBuildProfile(kManifestPath, kCurrentBuildId);

  ASSERT_TRUE(result) << result.error;
  ASSERT_TRUE(result.profile.has_value());
  EXPECT_EQ(result.profile->version_name, "2.746.1266");
  EXPECT_EQ(result.profile->version_code, 20836);
  EXPECT_EQ(result.profile->status, BuildStatus::kSupported);
  EXPECT_TRUE(result.profile->default_allowed);
  EXPECT_FALSE(result.profile->allow_legacy_binary_patches);
  EXPECT_TRUE(result.profile->allow_host_abi_bridges);
  EXPECT_TRUE(result.profile->allow_host_constructor_replay);
  ASSERT_TRUE(
      result.profile->user_game_settings_fullscreen_setter_rva.has_value());
  EXPECT_EQ(*result.profile->user_game_settings_fullscreen_setter_rva,
            0x5889620U);
  ASSERT_TRUE(result.profile->fmod_output_device_bridge.has_value());
  const auto& bridge = *result.profile->fmod_output_device_bridge;
  EXPECT_EQ(bridge.vtable_rva, 0x69ebad0U);
  EXPECT_EQ(bridge.string_constructor_rva, 0x1a9781eU);
  EXPECT_EQ(bridge.count_method_rva, 0x2a44ef2U);
  EXPECT_EQ(bridge.info_method_rva, 0x2a44f92U);
  EXPECT_EQ(bridge.current_method_rva, 0x2a44f42U);
  EXPECT_EQ(bridge.select_method_rva, 0x2a447e8U);
  ASSERT_TRUE(bridge.has_input_devices());
  EXPECT_EQ(bridge.input_method_rvas[0], 0x2a45342U);
  EXPECT_EQ(bridge.current_vtable_index(), 8U);
}

TEST(BuildProfileTest, ReportsUnknownBuildWithoutInventingProfile) {
  const ProfileLookupResult result = FindBuildProfile(
      kManifestPath, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

  ASSERT_TRUE(result) << result.error;
  EXPECT_FALSE(result.profile.has_value());
}

TEST(BuildProfileTest, RejectsInvalidBuildId) {
  const ProfileLookupResult result = FindBuildProfile(kManifestPath, "oops");

  EXPECT_FALSE(result);
  EXPECT_FALSE(result.error.empty());
}

TEST(BuildProfileTest, RejectsInvalidManifest) {
  const ProfileLookupResult result =
      FindBuildProfile("/dev/null", kCurrentBuildId);

  EXPECT_FALSE(result);
  EXPECT_FALSE(result.error.empty());
}

TEST(BuildProfileTest, RejectsPartialInputProfilesAndUpgradesCachedLayouts) {
  nlohmann::json document = ReadManifest();
  auto& entry = CurrentEntry(document);
  auto& bridge = entry["fmod_output_device_bridge"];
  char path[] = "/tmp/mocktail-input-profile-XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);
  const auto read = [&] {
    {
      std::ofstream output(path);
      output << document;
    }
    return FindBuildProfile(path, entry["elf_build_id"].get<std::string>());
  };
  bridge.erase("input_count_method_rva");
  EXPECT_FALSE(read());
  for (const auto* name : {"input_info_method_rva", "input_current_method_rva",
                           "input_select_method_rva"})
    bridge.erase(name);
  auto result = read();
  ASSERT_TRUE(result && result.profile);
  EXPECT_FALSE(result.profile->fmod_output_device_bridge->has_input_devices());
  // A 2.738.1397 manifest cached before input devices gains its routes.
  entry["elf_build_id"] = "5f0704edd9064f566ee3d6df2bd2fabbcc709f03";
  bridge["vtable_rva"] = "0x6cd3ce0";
  result = read();
  ASSERT_TRUE(result && result.profile);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->input_method_rvas[3],
            0x320bd18U);
  bridge["vtable_rva"] = "0x1000";
  result = read();
  ASSERT_TRUE(result && result.profile);
  EXPECT_FALSE(result.profile->fmod_output_device_bridge->has_input_devices());
  unlink(path);
}

TEST(BuildProfileTest, ParsesOnlyKnownFmodVtableLayouts) {
  char path[] = "/tmp/mocktail-fmod-profile-XXXXXX";
  const int descriptor = mkstemp(path);
  ASSERT_GE(descriptor, 0);
  close(descriptor);
  for (const std::string layout : {"1", "2", "0", "3", "true", "\"2\"", "null"}) {
    nlohmann::json document = ReadManifest();
    CurrentEntry(document)["fmod_output_device_bridge"]
                          ["vtable_layout_version"] =
                              nlohmann::json::parse(layout);
    { std::ofstream output(path); output << document; }
    const auto result = FindBuildProfile(path, kCurrentBuildId);
    if (layout == "1" || layout == "2") {
      EXPECT_TRUE(result) << result.error;
      if (result.profile && result.profile->fmod_output_device_bridge) {
        const auto& bridge = *result.profile->fmod_output_device_bridge;
        EXPECT_EQ(bridge.current_vtable_index(), layout == "2" ? 8U : 7U);
        EXPECT_EQ(bridge.select_vtable_index(), layout == "2" ? 19U : 17U);
      } else {
        ADD_FAILURE() << "parsed layout lost its FMOD profile";
      }
    } else {
      EXPECT_FALSE(result) << layout;
    }
  }
  unlink(path);
}

}  // namespace
}  // namespace mocktail::compat
