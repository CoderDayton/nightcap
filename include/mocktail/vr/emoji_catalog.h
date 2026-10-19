#ifndef MOCKTAIL_VR_EMOJI_CATALOG_H_
#define MOCKTAIL_VR_EMOJI_CATALOG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mocktail::vr {

// One emoji the VR keyboard can type, with its skin-tone variants.
struct Emoji {
  std::string utf8;
  std::vector<std::string> variants;
};

// Emoji grouped by Unicode block of their first code point, so faces,
// people, nature, travel, symbols and flags each get a tab.
struct EmojiGroup {
  std::vector<Emoji> emoji;
};

struct EmojiCatalog {
  std::vector<EmojiGroup> groups;
  std::size_t size() const;
  bool empty() const { return groups.empty(); }
};

// Every emoji Roblox can draw is a colour glyph in its emoji font: single
// code points from the cmap, and sequences (flags, keycaps, ZWJ families,
// skin tones) from the font's GSUB ligatures. Reads them from the font, so
// the list follows whatever Roblox build is installed. Returns an empty
// catalog when the font is missing or unreadable.
EmojiCatalog LoadEmojiCatalog(const std::vector<std::filesystem::path>& fonts);

// Groups and orders code point sequences the way LoadEmojiCatalog does.
// Exposed for tests.
EmojiCatalog BuildEmojiCatalog(std::vector<std::u32string> sequences);

// The colour (emoji) fonts in the payload named by MOCKTAIL_ASSET_PATH,
// found by their COLR table rather than by name, the largest first. Empty
// without a payload.
std::vector<std::filesystem::path> PayloadEmojiFonts();

}  // namespace mocktail::vr

#endif  // MOCKTAIL_VR_EMOJI_CATALOG_H_
