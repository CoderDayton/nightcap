#include "mocktail/vr/emoji_catalog.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace mocktail::vr {
namespace {

// Unicode emoji components: they build other emoji and are not offered on
// their own.
constexpr char32_t kZwj = 0x200D;
constexpr char32_t kVariationSelector16 = 0xFE0F;
constexpr char32_t kKeycap = 0x20E3;
bool IsSkinTone(char32_t c) { return c >= 0x1F3FB && c <= 0x1F3FF; }
bool IsRegionalIndicator(char32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }
bool IsTag(char32_t c) { return c >= 0xE0020 && c <= 0xE007F; }
bool IsHairComponent(char32_t c) { return c >= 0x1F9B0 && c <= 0x1F9B3; }
bool IsComponent(char32_t c) {
  return c < 0x80 || c == kZwj || c == kVariationSelector16 || c == kKeycap ||
         IsSkinTone(c) || IsRegionalIndicator(c) || IsTag(c) ||
         IsHairComponent(c) || (c >= 0xE000 && c <= 0xF8FF);
}

// Bounds-checked big-endian reads; out of range reads give 0, which every
// caller treats as an empty table.
class Reader {
 public:
  explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}
  bool Has(std::size_t offset, std::size_t size) const {
    return offset <= data_.size() && size <= data_.size() - offset;
  }
  std::uint16_t U16(std::size_t offset) const {
    if (!Has(offset, 2)) return 0;
    return static_cast<std::uint16_t>(data_[offset] << 8 | data_[offset + 1]);
  }
  std::uint32_t U32(std::size_t offset) const {
    if (!Has(offset, 4)) return 0;
    return static_cast<std::uint32_t>(data_[offset]) << 24 |
           static_cast<std::uint32_t>(data_[offset + 1]) << 16 |
           static_cast<std::uint32_t>(data_[offset + 2]) << 8 |
           data_[offset + 3];
  }

 private:
  const std::vector<std::uint8_t>& data_;
};

std::size_t FindTable(const Reader& font, const char (&tag)[5]) {
  const std::uint16_t tables = font.U16(4);
  for (std::uint16_t i = 0; i < tables; ++i) {
    const std::size_t record = 12 + static_cast<std::size_t>(i) * 16;
    const std::uint32_t found = font.U32(record);
    const std::uint32_t wanted =
        static_cast<std::uint32_t>(tag[0]) << 24 | tag[1] << 16 | tag[2] << 8 |
        tag[3];
    if (found == wanted) return font.U32(record + 8);
  }
  return 0;
}

// Code point -> glyph from the best Unicode cmap subtable (format 12, else 4).
std::map<char32_t, std::uint16_t> ReadCmap(const Reader& font) {
  std::map<char32_t, std::uint16_t> map;
  const std::size_t cmap = FindTable(font, "cmap");
  if (cmap == 0) return map;
  std::size_t format12 = 0, format4 = 0;
  const std::uint16_t subtables = font.U16(cmap + 2);
  for (std::uint16_t i = 0; i < subtables; ++i) {
    const std::size_t record = cmap + 4 + static_cast<std::size_t>(i) * 8;
    const std::size_t table = cmap + font.U32(record + 4);
    const std::uint16_t format = font.U16(table);
    if (format == 12 && format12 == 0) format12 = table;
    if (format == 4 && format4 == 0) format4 = table;
  }
  if (format12 != 0) {
    const std::uint32_t groups = font.U32(format12 + 12);
    for (std::uint32_t g = 0; g < groups && font.Has(format12 + 16 + g * 12, 12);
         ++g) {
      const std::size_t group = format12 + 16 + static_cast<std::size_t>(g) * 12;
      const std::uint32_t start = font.U32(group);
      const std::uint32_t end = std::min<std::uint32_t>(font.U32(group + 4),
                                                        0x10FFFF);
      const std::uint32_t glyph = font.U32(group + 8);
      for (std::uint32_t c = start; c <= end && end - start < 0x10000; ++c)
        map[c] = static_cast<std::uint16_t>(glyph + (c - start));
    }
    return map;
  }
  if (format4 != 0) {
    const std::uint16_t segments = font.U16(format4 + 6) / 2;
    const std::size_t ends = format4 + 14;
    const std::size_t starts = ends + segments * 2 + 2;
    const std::size_t deltas = starts + segments * 2;
    const std::size_t ranges = deltas + segments * 2;
    for (std::uint16_t s = 0; s < segments; ++s) {
      const std::uint16_t end = font.U16(ends + s * 2);
      const std::uint16_t start = font.U16(starts + s * 2);
      const std::uint16_t delta = font.U16(deltas + s * 2);
      const std::uint16_t range = font.U16(ranges + s * 2);
      for (std::uint32_t c = start; c <= end && c != 0xFFFF; ++c) {
        std::uint16_t glyph = 0;
        if (range == 0) {
          glyph = static_cast<std::uint16_t>(c + delta);
        } else {
          const std::size_t at = ranges + s * 2 + range + (c - start) * 2;
          glyph = font.U16(at);
          if (glyph != 0) glyph = static_cast<std::uint16_t>(glyph + delta);
        }
        if (glyph != 0) map[c] = glyph;
      }
    }
  }
  return map;
}

// Glyphs with colour layers (COLR v0 base records, or a v1 base glyph list).
std::unordered_set<std::uint16_t> ReadColourGlyphs(const Reader& font) {
  std::unordered_set<std::uint16_t> glyphs;
  const std::size_t colr = FindTable(font, "COLR");
  if (colr == 0) return glyphs;
  const std::uint16_t version = font.U16(colr);
  const std::uint16_t base_count = font.U16(colr + 2);
  const std::size_t base_records = colr + font.U32(colr + 4);
  for (std::uint16_t i = 0; i < base_count; ++i)
    glyphs.insert(font.U16(base_records + static_cast<std::size_t>(i) * 6));
  if (version >= 1) {
    const std::uint32_t list_offset = font.U32(colr + 14);
    if (list_offset != 0) {
      const std::size_t list = colr + list_offset;
      const std::uint32_t count = font.U32(list);
      for (std::uint32_t i = 0; i < count && font.Has(list + 4 + i * 6, 6); ++i)
        glyphs.insert(font.U16(list + 4 + static_cast<std::size_t>(i) * 6));
    }
  }
  return glyphs;
}

std::vector<std::uint16_t> ReadCoverage(const Reader& font, std::size_t at) {
  std::vector<std::uint16_t> glyphs;
  const std::uint16_t format = font.U16(at);
  if (format == 1) {
    const std::uint16_t count = font.U16(at + 2);
    for (std::uint16_t i = 0; i < count; ++i)
      glyphs.push_back(font.U16(at + 4 + static_cast<std::size_t>(i) * 2));
  } else if (format == 2) {
    const std::uint16_t count = font.U16(at + 2);
    for (std::uint16_t i = 0; i < count; ++i) {
      const std::size_t range = at + 4 + static_cast<std::size_t>(i) * 6;
      const std::uint16_t start = font.U16(range);
      const std::uint16_t end = font.U16(range + 2);
      for (std::uint32_t g = start; g <= end; ++g)
        glyphs.push_back(static_cast<std::uint16_t>(g));
    }
  }
  return glyphs;
}

struct Ligature {
  std::vector<std::uint16_t> input;
  std::uint16_t output = 0;
};

// Every GSUB ligature substitution (lookup type 4, also inside extensions).
std::vector<Ligature> ReadLigatures(const Reader& font) {
  std::vector<Ligature> ligatures;
  const std::size_t gsub = FindTable(font, "GSUB");
  if (gsub == 0) return ligatures;
  const std::size_t lookups = gsub + font.U16(gsub + 8);
  const std::uint16_t lookup_count = font.U16(lookups);
  for (std::uint16_t l = 0; l < lookup_count; ++l) {
    const std::size_t lookup = lookups + font.U16(lookups + 2 + l * 2);
    const std::uint16_t type = font.U16(lookup);
    const std::uint16_t subtables = font.U16(lookup + 4);
    for (std::uint16_t s = 0; s < subtables; ++s) {
      std::size_t subtable = lookup + font.U16(lookup + 6 + s * 2);
      std::uint16_t subtable_type = type;
      if (type == 7) {
        subtable_type = font.U16(subtable + 2);
        subtable += font.U32(subtable + 4);
      }
      if (subtable_type != 4 || font.U16(subtable) != 1) continue;
      const std::vector<std::uint16_t> first =
          ReadCoverage(font, subtable + font.U16(subtable + 2));
      const std::uint16_t sets = font.U16(subtable + 4);
      for (std::uint16_t i = 0; i < sets && i < first.size(); ++i) {
        const std::size_t set = subtable + font.U16(subtable + 6 + i * 2);
        const std::uint16_t count = font.U16(set);
        for (std::uint16_t k = 0; k < count; ++k) {
          const std::size_t entry = set + font.U16(set + 2 + k * 2);
          Ligature ligature;
          ligature.output = font.U16(entry);
          const std::uint16_t components = font.U16(entry + 2);
          ligature.input.push_back(first[i]);
          for (std::uint16_t c = 1; c < components; ++c)
            ligature.input.push_back(font.U16(entry + 2 + c * 2));
          ligatures.push_back(std::move(ligature));
        }
      }
    }
  }
  return ligatures;
}

std::string Utf8(const std::u32string& sequence) {
  std::string out;
  for (const char32_t c : sequence) {
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

std::u32string Without(const std::u32string& sequence, bool (*drop)(char32_t)) {
  std::u32string out;
  for (const char32_t c : sequence)
    if (!drop(c)) out += c;
  return out;
}

bool IsVariationSelector(char32_t c) { return c == kVariationSelector16; }

// Tab order: faces first, then people and more faces, nature and objects,
// travel, newer pictographs, older symbols, flags, and anything else.
int GroupOf(const std::u32string& sequence) {
  for (const char32_t c : sequence)
    if (IsRegionalIndicator(c) || IsTag(c)) return 6;
  const char32_t c = sequence.front();
  if (c >= 0x1F600 && c <= 0x1F64F) return 0;
  if (c >= 0x1F900 && c <= 0x1F9FF) return 1;
  if (c >= 0x1F300 && c <= 0x1F5FF) return 2;
  if (c >= 0x1F680 && c <= 0x1F6FF) return 3;
  if (c >= 0x1FA70 && c <= 0x1FAFF) return 4;
  if (c >= 0x2000 && c <= 0x2BFF) return 5;
  return 7;
}

}  // namespace

std::size_t EmojiCatalog::size() const {
  std::size_t count = 0;
  for (const EmojiGroup& group : groups) count += group.emoji.size();
  return count;
}

EmojiCatalog BuildEmojiCatalog(std::vector<std::u32string> sequences) {
  // One entry per emoji: a sequence with and without U+FE0F is the same one;
  // keep the fully qualified (longer) spelling.
  std::map<std::u32string, std::u32string> unique;
  for (std::u32string& sequence : sequences) {
    if (sequence.empty()) continue;
    const std::u32string key = Without(sequence, IsVariationSelector);
    if (key.empty()) continue;
    auto [it, inserted] = unique.emplace(key, sequence);
    if (!inserted && sequence.size() > it->second.size()) it->second = sequence;
  }
  // Skin-tone sequences become variants of their toneless base when it
  // exists; otherwise they stand alone.
  std::map<std::u32string, std::vector<std::u32string>> variants;
  std::vector<std::u32string> bases;
  for (const auto& [key, sequence] : unique) {
    const std::u32string toneless = Without(key, IsSkinTone);
    if (toneless != key && unique.count(toneless) != 0) {
      variants[toneless].push_back(sequence);
    } else {
      bases.push_back(key);
    }
  }
  std::map<int, EmojiGroup> groups;
  for (const std::u32string& key : bases) {
    const std::u32string& sequence = unique[key];
    Emoji emoji;
    emoji.utf8 = Utf8(sequence);
    const auto found = variants.find(key);
    if (found != variants.end()) {
      emoji.variants.push_back(emoji.utf8);
      for (const std::u32string& variant : found->second)
        emoji.variants.push_back(Utf8(variant));
    }
    groups[GroupOf(key)].emoji.push_back(std::move(emoji));
  }
  EmojiCatalog catalog;
  for (auto& [index, group] : groups) catalog.groups.push_back(std::move(group));
  return catalog;
}

namespace {

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
}

// Adds the colour glyph sequences of one font.
void CollectSequences(const std::vector<std::uint8_t>& data,
                      std::vector<std::u32string>* sequences_out) {
  std::vector<std::u32string>& sequences = *sequences_out;
  const Reader font(data);
  const std::map<char32_t, std::uint16_t> cmap = ReadCmap(font);
  const std::unordered_set<std::uint16_t> colour = ReadColourGlyphs(font);
  if (cmap.empty() || colour.empty()) return;
  // Glyph -> code point, preferring the lowest code point of a shared glyph.
  std::unordered_map<std::uint16_t, char32_t> reverse;
  for (const auto& [code_point, glyph] : cmap) reverse.emplace(glyph, code_point);

  for (const auto& [code_point, glyph] : cmap)
    if (colour.count(glyph) != 0 && !IsComponent(code_point))
      sequences.push_back(std::u32string(1, code_point));
  for (const Ligature& ligature : ReadLigatures(font)) {
    if (colour.count(ligature.output) == 0) continue;
    std::u32string sequence;
    for (const std::uint16_t glyph : ligature.input) {
      const auto found = reverse.find(glyph);
      if (found == reverse.end()) {
        sequence.clear();
        break;
      }
      sequence += found->second;
    }
    if (sequence.size() > 1) sequences.push_back(std::move(sequence));
  }
}

}  // namespace

EmojiCatalog LoadEmojiCatalog(const std::vector<std::filesystem::path>& fonts) {
  std::vector<std::u32string> sequences;
  for (const std::filesystem::path& font : fonts)
    CollectSequences(ReadFile(font), &sequences);
  return BuildEmojiCatalog(std::move(sequences));
}

std::vector<std::filesystem::path> PayloadEmojiFonts() {
  std::vector<std::pair<std::size_t, std::filesystem::path>> found;
  const char* content = std::getenv("MOCKTAIL_ASSET_PATH");
  if (content == nullptr || content[0] == '\0') return {};
  std::error_code error;
  std::filesystem::directory_iterator it(
      std::filesystem::path(content) / "fonts", error);
  for (; !error && it != std::filesystem::directory_iterator();
       it.increment(error)) {
    const std::string extension = it->path().extension().string();
    if (extension != ".ttf" && extension != ".otf") continue;
    const std::vector<std::uint8_t> data = ReadFile(it->path());
    const std::size_t glyphs = ReadColourGlyphs(Reader(data)).size();
    if (glyphs > 0) found.emplace_back(glyphs, it->path());
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first > b.first : a.second < b.second;
  });
  std::vector<std::filesystem::path> fonts;
  for (auto& entry : found) fonts.push_back(std::move(entry.second));
  return fonts;
}

}  // namespace mocktail::vr
