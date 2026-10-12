#include "runtime/asset_overlay_bridge.h"

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

class AssetOverlayBridgeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char dir_template[] = "/tmp/mocktail_bridge_test_XXXXXX";
    char* dir = mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr);
    temp_dir_ = dir;
  }

  void TearDown() override {
    if (!temp_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(temp_dir_, ec);
    }
  }

  void CreateFile(const std::string& rel_path, const std::string& content = "dummy") {
    const std::filesystem::path full_path = temp_dir_ / rel_path;
    std::filesystem::create_directories(full_path.parent_path());
    std::ofstream ofs(full_path);
    ofs << content;
  }

  std::filesystem::path temp_dir_;
};

TEST_F(AssetOverlayBridgeTest, OverlayHasAssetDetectsVariousPaths) {
  EXPECT_FALSE(OverlayHasAsset(temp_dir_, "MouseLockedCursor.png",
                               "textures/Cursors/KeyboardMouse/MouseLockedCursor.png"));

  // 1. Content/textures/Cursors/KeyboardMouse/ path
  CreateFile("content/textures/Cursors/KeyboardMouse/MouseLockedCursor.png");
  EXPECT_TRUE(OverlayHasAsset(temp_dir_, "MouseLockedCursor.png",
                              "textures/Cursors/KeyboardMouse/MouseLockedCursor.png"));

  // 2. Content/textures/ path
  CreateFile("content/textures/IBeamCursor.png");
  EXPECT_TRUE(OverlayHasAsset(temp_dir_, "IBeamCursor.png",
                              "textures/Cursors/KeyboardMouse/IBeamCursor.png"));

  // 3. Bare root path
  CreateFile("AnchorCursor.png");
  EXPECT_TRUE(OverlayHasAsset(temp_dir_, "AnchorCursor.png"));

  // 4. Nonexistent file
  EXPECT_FALSE(OverlayHasAsset(temp_dir_, "NonexistentCursor.png"));
}

TEST_F(AssetOverlayBridgeTest, NeutralizeOnlineAssetRedirectModifiesReadOnlyMemory) {
  const long page_size = sysconf(_SC_PAGESIZE);
  const size_t alloc_size = static_cast<size_t>(page_size) * 2;

  void* buffer = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(buffer, MAP_FAILED);

  // Write candidate string at offset
  const char target[] = "rbxasset://textures/MouseLockedCursor.png";
  const char repl[] = "rbxasset://tx_patch/MouseLockedCursor.png";
  const size_t target_len = std::strlen(target);
  const size_t offset = 100;

  std::memcpy(static_cast<char*>(buffer) + offset, target, target_len + 1);

  // Set memory to PROT_READ only
  ASSERT_EQ(mprotect(buffer, alloc_size, PROT_READ), 0);

  // Attempt to patch
  const uintptr_t base = reinterpret_cast<uintptr_t>(buffer);
  EXPECT_TRUE(NeutralizeOnlineAssetRedirect(base, 0, alloc_size, target, repl));

  // Verify memory changed to repl
  EXPECT_STREQ(static_cast<const char*>(buffer) + offset, repl);

  munmap(buffer, alloc_size);
}

TEST_F(AssetOverlayBridgeTest, NeutralizeRedirectsOnlyPatchesPresentOverlayAssets) {
  // Allocate simulated .rodata buffer of 0x400000 bytes (4MB)
  const size_t alloc_size = 0x400000;

  void* buffer = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(buffer, MAP_FAILED);

  const char mouselocked[] = "rbxasset://textures/MouseLockedCursor.png";
  const char ibeam[] = "rbxasset://textures/IBeamCursor.png";
  const char ibeam_km[] = "rbxasset://textures/Cursors/KeyboardMouse/IBeamCursor.png";
  const char arrowfar[] = "rbxasset://textures/ArrowFarCursor.png";

  // Place strings after default search offset (0x200000)
  const size_t mouselocked_offset = 0x220000;
  const size_t ibeam_offset = 0x240000;
  const size_t ibeam_km_offset = 0x260000;
  const size_t arrowfar_offset = 0x280000;

  std::memcpy(static_cast<char*>(buffer) + mouselocked_offset, mouselocked, std::strlen(mouselocked) + 1);
  std::memcpy(static_cast<char*>(buffer) + ibeam_offset, ibeam, std::strlen(ibeam) + 1);
  std::memcpy(static_cast<char*>(buffer) + ibeam_km_offset, ibeam_km, std::strlen(ibeam_km) + 1);
  std::memcpy(static_cast<char*>(buffer) + arrowfar_offset, arrowfar, std::strlen(arrowfar) + 1);

  ASSERT_EQ(mprotect(buffer, alloc_size, PROT_READ), 0);

  // Create overlay files for MouseLocked and IBeam, but NOT ArrowFar
  CreateFile("content/textures/Cursors/KeyboardMouse/MouseLockedCursor.png");
  CreateFile("content/textures/IBeamCursor.png");

  const uintptr_t base = reinterpret_cast<uintptr_t>(buffer);
  const size_t count = NeutralizeOnlineAssetRedirectsForOverlay(
      base, temp_dir_.c_str(), alloc_size - 0x200000);

  EXPECT_EQ(count, 3);

  // MouseLocked should be patched to tx_patch
  EXPECT_STREQ(static_cast<const char*>(buffer) + mouselocked_offset,
               "rbxasset://tx_patch/MouseLockedCursor.png");

  // IBeam bare should be patched to tx_patch
  EXPECT_STREQ(static_cast<const char*>(buffer) + ibeam_offset,
               "rbxasset://tx_patch/IBeamCursor.png");

  // IBeam KeyboardMouse should be patched to tx_patch
  EXPECT_STREQ(static_cast<const char*>(buffer) + ibeam_km_offset,
               "rbxasset://tx_patch/Cursors/KeyboardMouse/IBeamCursor.png");

  // ArrowFar was not in overlay, so it MUST remain unmodified
  EXPECT_STREQ(static_cast<const char*>(buffer) + arrowfar_offset, arrowfar);

  munmap(buffer, alloc_size);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
