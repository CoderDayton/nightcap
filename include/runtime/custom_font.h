#ifndef MOCKTAIL_RUNTIME_CUSTOM_FONT_H_
#define MOCKTAIL_RUNTIME_CUSTOM_FONT_H_

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

inline constexpr std::string_view kCustomFontAssetStem = "NightcapCustomFont";

std::filesystem::path InstalledCustomFont(const std::filesystem::path& dir);

// False for the emoji fonts and the Noto fonts for other scripts, which a
// Latin font cannot stand in for.
bool IsReplaceableRobloxFont(std::string_view file_name);

bool InstallCustomFont(const std::filesystem::path& font,
                       const std::filesystem::path& dir, std::string* error);

bool RemoveCustomFont(const std::filesystem::path& dir, std::string* error);

// Family files are rewritten too, so faces Roblox downloads get `font` as well.
bool BuildCustomFontOverlay(const std::filesystem::path& font,
                            const std::filesystem::path& stock_fonts_dir,
                            const std::filesystem::path& out_dir,
                            std::size_t* replaced, std::string* error);

}
}

#endif
