#include "mocktail/graphics/texture_override.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace mocktail::graphics {
namespace {

class TempDir {
 public:
  TempDir() {
    char pattern[] = "/tmp/mocktail-texture-override-XXXXXX";
    const char* made = mkdtemp(pattern);
    path_ = made != nullptr ? made : "";
  }
  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

RgbaImage Gradient(std::uint32_t width, std::uint32_t height) {
  RgbaImage image;
  image.width = width;
  image.height = height;
  image.pixels.resize(static_cast<std::size_t>(width) * height * 4);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t* pixel = image.pixels.data() + (y * width + x) * 4;
      pixel[0] = static_cast<std::uint8_t>(x * 7);
      pixel[1] = static_cast<std::uint8_t>(y * 11);
      pixel[2] = static_cast<std::uint8_t>(x + y);
      pixel[3] = static_cast<std::uint8_t>(255 - x);
    }
  }
  return image;
}

TEST(TextureOverrideTest, HashIsStableAndDependsOnBytes) {
  const std::uint8_t a[] = {1, 2, 3, 4};
  const std::uint8_t b[] = {1, 2, 3, 5};
  EXPECT_EQ(HashBytes(a, sizeof(a)), HashBytes(a, sizeof(a)));
  EXPECT_NE(HashBytes(a, sizeof(a)), HashBytes(b, sizeof(b)));
  EXPECT_EQ(HashName(0x0123456789abcdefULL), "0123456789abcdef");
}

TEST(TextureOverrideTest, PngRoundTripKeepsEveryPixel) {
  TempDir dir;
  ASSERT_FALSE(dir.path().empty());
  const RgbaImage image = Gradient(9, 5);
  const std::string path = dir.path() + "/image.png";
  ASSERT_TRUE(WritePngRgba(path, image));
  RgbaImage read;
  ASSERT_TRUE(ReadPngRgba(path, &read));
  EXPECT_EQ(read.width, image.width);
  EXPECT_EQ(read.height, image.height);
  EXPECT_EQ(read.pixels, image.pixels);
}

TEST(TextureOverrideTest, ReadPngFailsForMissingFile) {
  RgbaImage read;
  EXPECT_FALSE(ReadPngRgba("/nonexistent/mocktail.png", &read));
}

TEST(TextureOverrideTest, ResampleAveragesSourceBoxes) {
  RgbaImage source;
  source.width = 2;
  source.height = 2;
  source.pixels = {0,   0,   0,   0,    100, 100, 100, 100,
                   200, 200, 200, 200,  100, 100, 100, 100};
  std::uint8_t one[4] = {};
  ResampleRgba(source, 1, 1, one);
  EXPECT_EQ(one[0], 100);
  EXPECT_EQ(one[3], 100);

  std::uint8_t four[16] = {};
  ResampleRgba(source, 2, 2, four);
  EXPECT_EQ(std::vector<std::uint8_t>(four, four + 16), source.pixels);

  RgbaImage single;
  single.width = 1;
  single.height = 1;
  single.pixels = {9, 8, 7, 6};
  std::uint8_t grown[16] = {};
  ResampleRgba(single, 2, 2, grown);
  for (int pixel = 0; pixel < 4; ++pixel) {
    EXPECT_EQ(grown[pixel * 4 + 0], 9);
    EXPECT_EQ(grown[pixel * 4 + 3], 6);
  }
}

TEST(TextureOverrideTest, LookupFindsPngNamedByHashAndCachesMisses) {
  TempDir dir;
  ASSERT_FALSE(dir.path().empty());
  const RgbaImage image = Gradient(4, 4);
  const std::uint64_t hash = 0xfeedfacecafebeefULL;
  ASSERT_TRUE(WritePngRgba(dir.path() + "/" + HashName(hash) + ".png", image));

  TextureOverrides overrides(dir.path(), "");
  EXPECT_TRUE(overrides.enabled());
  const auto found = overrides.Lookup(hash);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->pixels, image.pixels);
  EXPECT_EQ(overrides.Lookup(hash), found);
  EXPECT_EQ(overrides.Lookup(hash + 1), nullptr);

  TextureOverrides disabled("", "");
  EXPECT_FALSE(disabled.enabled());
  EXPECT_EQ(disabled.Lookup(hash), nullptr);
}

TEST(TextureOverrideTest, DumpWritesOnePngPerHash) {
  TempDir dir;
  ASSERT_FALSE(dir.path().empty());
  const RgbaImage image = Gradient(3, 2);
  TextureOverrides overrides("", dir.path());
  EXPECT_TRUE(overrides.enabled());
  const std::uint64_t hash = 0x1234ULL;
  overrides.Dump(hash, image.width, image.height, image.pixels.data());
  overrides.Dump(hash, image.width, image.height, image.pixels.data());
  const std::string expected =
      dir.path() + "/" + HashName(hash) + "_3x2.png";
  ASSERT_TRUE(std::filesystem::exists(expected));
  RgbaImage read;
  ASSERT_TRUE(ReadPngRgba(expected, &read));
  EXPECT_EQ(read.pixels, image.pixels);
  std::size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
    static_cast<void>(entry);
    ++files;
  }
  EXPECT_EQ(files, 1u);
}

}  // namespace
}  // namespace mocktail::graphics
