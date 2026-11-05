#ifndef MOCKTAIL_RUNTIME_ASSET_OVERLAY_NAMES_H_
#define MOCKTAIL_RUNTIME_ASSET_OVERLAY_NAMES_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace runtime {

// The rbxasset:// name of every regular file under <overlay_dir>/content, such
// as rbxasset://textures/MouseLockedCursor.png. Sorted.
std::vector<std::string> OverlayRbxAssetNames(
    const std::filesystem::path& overlay_dir);

// Roblox opens no local file for an rbxasset:// name it maps to a download.
// Writes '!' over the first byte of each whole NUL-terminated copy of a name
// in `data`, which unmaps it. Returns the number of copies changed.
std::size_t DisableAssetNameMappings(std::uint8_t* data, std::size_t size,
                                     const std::vector<std::string>& names);

// Under FFlagDebugSkyGray every game draws the indoor512 sky in place of its
// own. A sky picked in Settings replaces all six sky512 and indoor512 faces;
// indoor512 alone is a start-screen sky and leaves games alone.
bool OverlayHasCustomSky(const std::filesystem::path& overlay_dir);

// Adds FFlagDebugSkyGray=True to the client-settings overrides unless they
// set it already. `gray_sky` gets the value that applies.
bool MergeCustomSkyClientSettingsOverrides(std::string_view base_json,
                                           std::string* merged_json,
                                           bool* gray_sky, std::string* error);

}
}

#endif
