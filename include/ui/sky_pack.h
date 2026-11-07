#ifndef MOCKTAIL_UI_SKY_PACK_H_
#define MOCKTAIL_UI_SKY_PACK_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace ui {

// The skybox in the asset overlay: the six sky512_<face>.tex files Roblox
// loads in a game, and the indoor512_<face>.tex files the start screen and
// FFlagDebugSkyGray draw, unless those were put there by hand.

std::filesystem::path SkyFolder(const std::filesystem::path& overlay_dir);

bool HasCustomSky(const std::filesystem::path& overlay_dir);

// True when the installed sky owns indoor512 faces that are missing, as with
// a sky installed before it replaced the indoor sky. Installing it again
// fills them in.
bool IndoorSkyMissing(const std::filesystem::path& overlay_dir);

// Searches `source` a few folders deep for one PNG, JPEG, WebP, DDS or KTX
// image per face, named *_bk, *_dn, *_ft, *_lf, *_rt and *_up (.tex, .png,
// .jpg, .jpeg, .webp, .dds or .ktx). Where a
// face has several, sky512_* wins. Nothing is copied unless all six are found.
bool InstallSky(const std::filesystem::path& source,
                const std::filesystem::path& overlay_dir, std::string* error);

bool RemoveCustomSky(const std::filesystem::path& overlay_dir,
                     std::string* error);

// The sky library holds one pack per folder: <library>/<name>/sky512_*.
std::vector<std::string> ListSkyLibrary(const std::filesystem::path& library);

struct SkyImport {
  std::vector<std::string> added;
  // Packs found but left out, each with why.
  std::vector<std::string> skipped;
  std::string error;
  bool cancelled = false;
};

struct SkyImportLimits {
  // Bytes extracted or copied; a 65-sky collection needs about 800 MB.
  std::uintmax_t max_bytes = std::uintmax_t{2} << 30;
  // Entries looked at, so picking a home folder stops instead of searching
  // it all.
  std::size_t max_files = 50000;
};

// Adds each complete pack in `source`, a folder or a .zip, to the library,
// opening zips inside it two levels deep, so a collection imports whole.
// A pack is named after its zip or top folder, and replaces one of that name
// unless that one was kept from the overlay. Setting `stop` cancels it; packs
// already added stay.
SkyImport ImportSkies(const std::filesystem::path& source,
                      const std::filesystem::path& library,
                      const SkyImportLimits& limits = {},
                      const std::atomic<bool>* stop = nullptr);

// Both first copy a sky that is not from the library into it, naming it in
// `kept`, so a hand-placed sky is never lost.
bool UseLibrarySky(std::string_view name,
                   const std::filesystem::path& library,
                   const std::filesystem::path& overlay_dir,
                   std::string* error, std::string* kept = nullptr);
bool UseRobloxSky(const std::filesystem::path& library,
                  const std::filesystem::path& overlay_dir, std::string* error,
                  std::string* kept = nullptr);

// Refuses the sky in use, so the overlay never holds a sky the library lost.
bool RemoveLibrarySky(std::string_view name,
                      const std::filesystem::path& library,
                      const std::filesystem::path& overlay_dir,
                      std::string* error);

// The face shown as the pack's preview, or empty.
std::filesystem::path SkyPreviewFace(const std::filesystem::path& library,
                                     std::string_view name);

// Empty for Roblox's own sky, the library sky's name, or nullopt for a sky
// that is not from the library.
std::optional<std::string> InstalledSky(
    const std::filesystem::path& overlay_dir,
    const std::filesystem::path& library);

}
}

#endif
