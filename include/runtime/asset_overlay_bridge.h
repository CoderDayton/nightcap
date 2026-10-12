#ifndef MOCKTAIL_RUNTIME_ASSET_OVERLAY_BRIDGE_H_
#define MOCKTAIL_RUNTIME_ASSET_OVERLAY_BRIDGE_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace mocktail {
namespace runtime {

// Checks whether an asset file matching filename/subpath exists within the overlay directory.
bool OverlayHasAsset(const std::filesystem::path& overlay_dir,
                     const char* filename,
                     const char* subpath = nullptr);

// In-place patch a specific string within a memory buffer, temporarily modifying page permissions.
bool NeutralizeOnlineAssetRedirect(uintptr_t base_address,
                                   std::size_t search_offset,
                                   std::size_t search_length,
                                   const char* target_uri,
                                   const char* replacement_uri);

// Inspects the active asset overlay directory (via override_overlay_dir or MOCKTAIL_ASSET_OVERLAY_DIR)
// and neutralizes hardcoded assetdelivery.roblox.com URI redirects in libroblox.so's .rodata.
// Returns the number of redirects neutralized.
std::size_t NeutralizeOnlineAssetRedirectsForOverlay(
    uintptr_t libroblox_base,
    const char* override_overlay_dir = nullptr,
    std::size_t search_length = 0);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ASSET_OVERLAY_BRIDGE_H_
