#ifndef MOCKTAIL_GRAPHICS_TEXTURE_OVERRIDE_H_
#define MOCKTAIL_GRAPHICS_TEXTURE_OVERRIDE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mocktail::graphics {

// Tightly packed RGBA8 pixels, row-major, top row first.
struct RgbaImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;
};

// FNV-1a over the bytes; the texture identity used for dump and override
// file names.
std::uint64_t HashBytes(const std::uint8_t* data, std::size_t size);
// 16 lowercase hex digits.
std::string HashName(std::uint64_t hash);

bool ReadPngRgba(const std::string& path, RgbaImage* image);
bool WritePngRgba(const std::string& path, const RgbaImage& image);

// Writes width x height RGBA8 pixels to destination, each destination pixel
// the average of the source box it covers. Growing repeats source pixels.
void ResampleRgba(const RgbaImage& source, std::uint32_t width,
                  std::uint32_t height, std::uint8_t* destination);
void ResampleRgba(const std::uint8_t* source, std::uint32_t source_width,
                  std::uint32_t source_height, std::uint32_t width,
                  std::uint32_t height, std::uint8_t* destination);

// Replacement textures keyed by the hash of a texture's level-0 compressed
// bytes. An override lives at <override_dir>/<hash>.png. With a dump
// directory, every level-0 texture seen is written once as
// <dump_dir>/<hash>_<width>x<height>.png so the hash can be found.
// Thread-safe.
class TextureOverrides {
 public:
  TextureOverrides(std::string override_dir, std::string dump_dir);
  // MOCKTAIL_TEXTURE_OVERRIDE_DIR and MOCKTAIL_TEXTURE_DUMP_DIR.
  static TextureOverrides FromEnvironment();

  bool enabled() const { return !override_dir_.empty() || !dump_dir_.empty(); }
  bool dumping() const { return !dump_dir_.empty(); }

  // Null when no override file exists; lookups are cached either way.
  std::shared_ptr<const RgbaImage> Lookup(std::uint64_t hash);
  void Dump(std::uint64_t hash, std::uint32_t width, std::uint32_t height,
            const std::uint8_t* rgba);

 private:
  std::string override_dir_;
  std::string dump_dir_;
  std::mutex mutex_;
  std::unordered_map<std::uint64_t, std::shared_ptr<const RgbaImage>> cache_;
  std::unordered_set<std::uint64_t> dumped_;
};

}  // namespace mocktail::graphics

#endif  // MOCKTAIL_GRAPHICS_TEXTURE_OVERRIDE_H_
