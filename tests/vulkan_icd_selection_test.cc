#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "runtime/graphics_launch_policy.h"

#include "compat/guest_abi.h"

namespace mocktail {
namespace runtime {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_icd_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created != nullptr) root_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

void WriteManifest(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << R"({"ICD":{"library_path":"libGLX_nvidia.so.0"}})";
}

TEST(VulkanIcdSelectionTest, PrefersTheNativeArchitectureManifest) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nvidia_icd.i686.json");
  const std::string native_name =
      "nvidia_icd." + std::string(compat::kGuestCpuName) + ".json";
  WriteManifest(temporary.root() / native_name);

  EXPECT_EQ(SelectVulkanIcdManifest({temporary.root()}, "nvidia_icd"),
            (temporary.root() / native_name).string());
}

TEST(VulkanIcdSelectionTest, NeverSelectsAForeignArchitectureManifest) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nvidia_icd.i686.json");
#if defined(__aarch64__)
  WriteManifest(temporary.root() / "radeon_icd.x86_64.json");
#else
  WriteManifest(temporary.root() / "radeon_icd.aarch64.json");
#endif

  EXPECT_TRUE(
      SelectVulkanIcdManifest({temporary.root()}, "nvidia_icd").empty());
  EXPECT_TRUE(
      SelectVulkanIcdManifest({temporary.root()}, "radeon_icd").empty());
}

TEST(VulkanIcdSelectionTest, AcceptsAnUnqualifiedManifest) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nvidia_icd.json");

  EXPECT_EQ(SelectVulkanIcdManifest({temporary.root()}, "nvidia_icd"),
            (temporary.root() / "nvidia_icd.json").string());
}

TEST(VulkanIcdSelectionTest, KeepsDirectoryPrecedenceOverName) {
  TemporaryDirectory temporary;
  const std::filesystem::path first = temporary.root() / "first";
  const std::filesystem::path second = temporary.root() / "second";
  WriteManifest(first / "nvidia_icd.json");
  WriteManifest(second / ("nvidia_icd." +
                          std::string(compat::kGuestCpuName) + ".json"));

  EXPECT_EQ(SelectVulkanIcdManifest({first, second}, "nvidia_icd"),
            (first / "nvidia_icd.json").string());
}

TEST(VulkanIcdSelectionTest, IsStableWhateverTheDirectoryOrderIs) {
  TemporaryDirectory temporary;
  const std::string native_name =
      "nvidia_icd." + std::string(compat::kGuestCpuName) + ".json";
  WriteManifest(temporary.root() / native_name);
  WriteManifest(temporary.root() / "nvidia_icd.json");
  WriteManifest(temporary.root() / "nvidia_icd.i686.json");

  const std::string selected =
      SelectVulkanIcdManifest({temporary.root()}, "nvidia_icd");
  EXPECT_EQ(selected, (temporary.root() / native_name).string());
  EXPECT_EQ(selected, SelectVulkanIcdManifest({temporary.root()},
                                              "nvidia_icd"));
}

TEST(VulkanIcdSelectionTest, IgnoresAMissingDirectory) {
  TemporaryDirectory temporary;
  EXPECT_TRUE(SelectVulkanIcdManifest({temporary.root() / "absent"},
                                      "nvidia_icd")
                  .empty());
  EXPECT_TRUE(SelectVulkanIcdManifest({temporary.root()}, "").empty());
}

constexpr unsigned int kAmd = 0x1002;
constexpr unsigned int kNvidia = 0x10de;
constexpr unsigned int kIntel = 0x8086;

TEST(HostVulkanIcdTest, NvidiaKernelDriverSelectsTheNvidiaManifest) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nvidia_icd.json");
  WriteManifest(temporary.root() / "nouveau_icd.json");

  const VulkanIcdSelection selection =
      SelectHostVulkanIcd({{kNvidia, "nvidia"}}, {temporary.root()}, true);
  EXPECT_EQ(selection.manifest,
            (temporary.root() / "nvidia_icd.json").string());
  EXPECT_TRUE(selection.skipped.empty());
}

TEST(HostVulkanIcdTest, NouveauKernelDriverIgnoresAnInstalledNvidiaManifest) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nvidia_icd.json");
  WriteManifest(temporary.root() / "nouveau_icd.json");

  const VulkanIcdSelection selection =
      SelectHostVulkanIcd({{kNvidia, "nouveau"}}, {temporary.root()}, true);
  EXPECT_EQ(selection.manifest,
            (temporary.root() / "nouveau_icd.json").string());
}

TEST(HostVulkanIcdTest, UnknownKernelDriverTriesNvidiaThenNouveau) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nouveau_icd.json");

  EXPECT_EQ(
      SelectHostVulkanIcd({{kNvidia, ""}}, {temporary.root()}, true).manifest,
      (temporary.root() / "nouveau_icd.json").string());

  WriteManifest(temporary.root() / "nvidia_icd.json");
  EXPECT_EQ(
      SelectHostVulkanIcd({{kNvidia, ""}}, {temporary.root()}, true).manifest,
      (temporary.root() / "nvidia_icd.json").string());
}

TEST(HostVulkanIcdTest, CardWithoutItsVulkanDriverFallsBackToTheIntelChip) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nvidia_icd.json");
  WriteManifest(temporary.root() / "intel_icd.json");

  const VulkanIcdSelection selection = SelectHostVulkanIcd(
      {{kIntel, "i915"}, {kNvidia, "nouveau"}}, {temporary.root()}, true);
  EXPECT_EQ(selection.manifest,
            (temporary.root() / "intel_icd.json").string());
  ASSERT_EQ(selection.skipped.size(), 1u);
  EXPECT_NE(selection.skipped[0].find("nouveau"), std::string::npos);
}

TEST(HostVulkanIcdTest, RadeonKernelDriverFallsBackToTheIntelChip) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "radeon_icd.json");
  WriteManifest(temporary.root() / "intel_icd.json");

  const VulkanIcdSelection selection = SelectHostVulkanIcd(
      {{kIntel, "i915"}, {kAmd, "radeon"}}, {temporary.root()}, true);
  EXPECT_EQ(selection.manifest,
            (temporary.root() / "intel_icd.json").string());
}

TEST(HostVulkanIcdTest, RadeonKernelDriverAloneSelectsNothingAndSaysWhy) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "radeon_icd.json");

  const VulkanIcdSelection selection =
      SelectHostVulkanIcd({{kAmd, "radeon"}}, {temporary.root()}, true);
  EXPECT_TRUE(selection.manifest.empty());
  ASSERT_EQ(selection.skipped.size(), 1u);
  EXPECT_NE(selection.skipped[0].find("radeon"), std::string::npos);
}

TEST(HostVulkanIcdTest, AmdgpuKernelDriverSelectsTheRadeonManifest) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "radeon_icd.json");

  EXPECT_EQ(SelectHostVulkanIcd({{kAmd, "amdgpu"}}, {temporary.root()}, true)
                .manifest,
            (temporary.root() / "radeon_icd.json").string());
}

TEST(HostVulkanIcdTest, PrefersTheIntelChipWhenTheDiscreteCardIsNotWanted) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nvidia_icd.json");
  WriteManifest(temporary.root() / "intel_icd.json");

  EXPECT_EQ(SelectHostVulkanIcd({{kIntel, "i915"}, {kNvidia, "nvidia"}},
                                {temporary.root()}, false)
                .manifest,
            (temporary.root() / "intel_icd.json").string());
}

TEST(HostVulkanIcdTest, NoKnownCardSelectsNothingAndSkipsNothing) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nvidia_icd.json");

  const VulkanIcdSelection selection =
      SelectHostVulkanIcd({}, {temporary.root()}, true);
  EXPECT_TRUE(selection.manifest.empty());
  EXPECT_TRUE(selection.skipped.empty());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
