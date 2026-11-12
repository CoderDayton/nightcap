#ifndef MOCKTAIL_RUNTIME_ROBLOX_SKY_HOOK_H_
#define MOCKTAIL_RUNTIME_ROBLOX_SKY_HOOK_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/roblox_code_patch.h"

namespace mocktail {
namespace runtime {

// A game's Sky names its six faces by asset ID, so Roblox never opens the
// sky512 files a custom sky replaces. Every face, a game's or the default
// sky's, is loaded by one function under a name like Sky.SkyboxBk; the hook
// hands that function the custom sky's face instead.

// "bk" for a load named Sky.SkyboxBk, and so on; empty for any other load.
std::string_view SkyFaceOfLoad(std::string_view debug_name);

std::string CustomSkyFaceContent(std::string_view face);

// Found through the "Sky.Skybox" names its callers build. Fails rather than
// guess when they disagree or its opening is not the one the hook relocates.
std::optional<std::uintptr_t> LocateSkyTextureLoad(
    const std::vector<CursorHookSegment>& segments, std::string* error);

// No code may have run from the image at `image_base` yet. Returns false,
// changing nothing, when the code does not match.
bool InstallRobloxSkyHook(std::uintptr_t image_base, std::string* error);

}
}

#endif  // MOCKTAIL_RUNTIME_ROBLOX_SKY_HOOK_H_
