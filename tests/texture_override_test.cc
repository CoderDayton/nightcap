#include "mocktail/graphics/texture_override.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
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

  // 4x4 image with row stride = 16 bytes, but we only resample the top-left 2x2 region
  std::vector<std::uint8_t> padded(4 * 16, 0);
  for (std::uint32_t y = 0; y < 2; ++y) {
    for (std::uint32_t x = 0; x < 2; ++x) {
      padded[y * 16 + x * 4 + 0] = source.pixels[(y * 2 + x) * 4 + 0];
      padded[y * 16 + x * 4 + 1] = source.pixels[(y * 2 + x) * 4 + 1];
      padded[y * 16 + x * 4 + 2] = source.pixels[(y * 2 + x) * 4 + 2];
      padded[y * 16 + x * 4 + 3] = source.pixels[(y * 2 + x) * 4 + 3];
    }
  }
  std::uint8_t strided_four[16] = {};
  ResampleRgba(padded.data(), 2, 2, 16, 2, 2, strided_four);
  EXPECT_EQ(std::vector<std::uint8_t>(strided_four, strided_four + 16),
            source.pixels);
}

// Rounded mean of the source box [x*sw/w, (x+1)*sw/w) per axis, at least one
// texel wide.
std::vector<std::uint8_t> ReferenceBoxResample(const RgbaImage& source,
                                               std::uint32_t width,
                                               std::uint32_t height) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4);
  const auto span = [](std::uint32_t i, std::uint32_t from, std::uint32_t to) {
    const std::uint64_t first = static_cast<std::uint64_t>(i) * from / to;
    const std::uint64_t last =
        std::max<std::uint64_t>(first + 1, (i + 1ULL) * from / to);
    return std::pair{first, last};
  };
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto [y0, y1] = span(y, source.height, height);
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto [x0, x1] = span(x, source.width, width);
      for (int channel = 0; channel < 4; ++channel) {
        std::uint64_t sum = 0;
        for (std::uint64_t sy = y0; sy < y1; ++sy) {
          for (std::uint64_t sx = x0; sx < x1; ++sx) {
            sum += source.pixels[(sy * source.width + sx) * 4 + channel];
          }
        }
        const std::uint64_t count = (y1 - y0) * (x1 - x0);
        out[(static_cast<std::size_t>(y) * width + x) * 4 + channel] =
            static_cast<std::uint8_t>((sum + count / 2) / count);
      }
    }
  }
  return out;
}

TEST(TextureOverrideTest, ResampleMatchesBoxFilterForMixedSizes) {
  const std::pair<std::uint32_t, std::uint32_t> sources[] = {
      {1, 1}, {3, 5}, {48, 48}, {60, 33}, {64, 17}};
  const std::pair<std::uint32_t, std::uint32_t> targets[] = {
      {1, 1}, {2, 7}, {5, 3}, {48, 48}, {192, 192}, {240, 132}, {31, 200}};
  for (const auto& [source_width, source_height] : sources) {
    const RgbaImage source = Gradient(source_width, source_height);
    for (const auto& [width, height] : targets) {
      std::vector<std::uint8_t> actual(
          static_cast<std::size_t>(width) * height * 4, 0);
      ResampleRgba(source, width, height, actual.data());
      ASSERT_EQ(actual, ReferenceBoxResample(source, width, height))
          << source_width << "x" << source_height << " -> " << width << "x"
          << height;
    }
  }
}

std::vector<std::uint8_t> Upscale(const RgbaImage& source, std::uint32_t width,
                                  std::uint32_t height) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4);
  UpscaleRgba(source.pixels.data(), source.width, source.height,
              static_cast<std::size_t>(source.width) * 4, width, height,
              out.data());
  return out;
}

RgbaImage Filled(std::uint32_t width, std::uint32_t height) {
  RgbaImage image;
  image.width = width;
  image.height = height;
  image.pixels.resize(static_cast<std::size_t>(width) * height * 4);
  return image;
}

// Pixel-art style edges between two flat colours stay hard.
TEST(TextureOverrideTest, UpscaleKeepsTwoToneEdgesNearest) {
  RgbaImage source = Filled(4, 4);
  for (std::uint32_t i = 0; i < 16; ++i) {
    const bool red = (i % 4 + i / 4) % 2 == 0;
    const std::uint8_t texel[4] = {static_cast<std::uint8_t>(red ? 220 : 10),
                                   20, static_cast<std::uint8_t>(red ? 0 : 230),
                                   255};
    std::memcpy(source.pixels.data() + i * 4, texel, 4);
  }
  std::vector<std::uint8_t> nearest(16 * 16 * 4);
  ResampleRgba(source, 16, 16, nearest.data());
  EXPECT_EQ(Upscale(source, 16, 16), nearest);
}

// Smooth content is interpolated, and sharpening never overshoots the
// source texels around an output texel.
TEST(TextureOverrideTest, UpscaleSmoothsGradientsWithoutOvershoot) {
  RgbaImage source = Filled(8, 8);
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const std::uint8_t texel[4] = {static_cast<std::uint8_t>(x * x * 3 + 20),
                                     static_cast<std::uint8_t>(100 + y * 5),
                                     90, 255};
      std::memcpy(source.pixels.data() + (y * 8 + x) * 4, texel, 4);
    }
  }
  const std::vector<std::uint8_t> out = Upscale(source, 32, 32);
  std::set<int> reds;
  for (std::uint32_t y = 0; y < 32; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      const std::uint8_t* texel = out.data() + (y * 32 + x) * 4;
      reds.insert(texel[0]);
      const int sx = std::clamp(static_cast<int>(std::floor((x + 0.5) / 4 - 0.5)), 0, 7);
      const int sy = std::clamp(static_cast<int>(std::floor((y + 0.5) / 4 - 0.5)), 0, 7);
      for (int c = 0; c < 3; ++c) {
        int low = 255;
        int high = 0;
        for (int dy = 0; dy < 2; ++dy) {
          for (int dx = 0; dx < 2; ++dx) {
            const int v = source.pixels[(std::min(sy + dy, 7) * 8 +
                                         std::min(sx + dx, 7)) * 4 + c];
            low = std::min(low, v);
            high = std::max(high, v);
          }
        }
        EXPECT_GE(texel[c], low - 1) << x << "," << y << " channel " << c;
        EXPECT_LE(texel[c], high + 1) << x << "," << y << " channel " << c;
      }
      if (x > 0) {
        EXPECT_GE(texel[0] + 1, texel[-4]) << x << "," << y;
      }
    }
  }
  EXPECT_GT(reds.size(), 24u);
}

TEST(TextureOverrideTest, UpscaleKeepsAFlatImageFlat) {
  RgbaImage source = Filled(5, 3);
  for (std::size_t i = 0; i < source.pixels.size(); i += 4) {
    source.pixels[i] = 10;
    source.pixels[i + 1] = 200;
    source.pixels[i + 2] = 30;
    source.pixels[i + 3] = 128;
  }
  const std::vector<std::uint8_t> out = Upscale(source, 20, 12);
  for (std::size_t i = 0; i < out.size(); i += 4) {
    ASSERT_NEAR(out[i], 10, 1) << i;
    ASSERT_NEAR(out[i + 1], 200, 1) << i;
    ASSERT_NEAR(out[i + 2], 30, 1) << i;
    ASSERT_EQ(out[i + 3], 128) << i;
  }
}

// Interpolation weighs colour by alpha, so the colour stored under
// transparent texels does not bleed into the visible ones.
TEST(TextureOverrideTest, UpscaleIgnoresTheColourOfTransparentTexels) {
  RgbaImage source = Filled(8, 8);
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const std::uint8_t alpha = static_cast<std::uint8_t>(x < 3 ? 0 : x * 30 + y);
      const std::uint8_t texel[4] = {static_cast<std::uint8_t>(alpha ? 200 : 0),
                                     static_cast<std::uint8_t>(alpha ? 0 : 255),
                                     0, alpha};
      std::memcpy(source.pixels.data() + (y * 8 + x) * 4, texel, 4);
    }
  }
  const std::vector<std::uint8_t> out = Upscale(source, 32, 32);
  for (std::size_t i = 0; i < out.size(); i += 4) {
    if (out[i + 3] >= 8) {
      EXPECT_LE(out[i + 1], 2) << i / 4;
    }
  }
}

TEST(TextureOverrideTest, LookupFindsPngNamedByHashAndCachesMisses) {
  TempDir dir;
  ASSERT_FALSE(dir.path().empty());
  const RgbaImage image = Gradient(4, 4);
  const std::uint64_t hash = 0xfeedfacecafebeefULL;
  ASSERT_TRUE(
      WritePngRgba(dir.path() + "/" + HashName(hash) + "_4x4.png", image));

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

// The runtime always points the override dir at ~/.config/mocktail/textures,
// so a folder without a usable override must not count as overrides in use.
// A bare <hash>.png says nothing about which textures it fits, so it is
// skipped.
TEST(TextureOverrideTest, DisabledWithoutASizedOverridePng) {
  TempDir dir;
  ASSERT_FALSE(dir.path().empty());
  EXPECT_FALSE(TextureOverrides(dir.path() + "/missing", "").enabled());
  EXPECT_FALSE(TextureOverrides(dir.path(), "").enabled());
  std::ofstream(dir.path() + "/readme.txt") << "x";
  EXPECT_FALSE(TextureOverrides(dir.path(), "").enabled());
  ASSERT_TRUE(WritePngRgba(dir.path() + "/" + HashName(1) + ".png",
                           Gradient(2, 2)));
  TextureOverrides bare(dir.path(), "");
  EXPECT_FALSE(bare.enabled());
  EXPECT_EQ(bare.Lookup(1), nullptr);
  ASSERT_TRUE(WritePngRgba(dir.path() + "/" + HashName(1) + "_2x2.png",
                           Gradient(2, 2)));
  EXPECT_TRUE(TextureOverrides(dir.path(), "").enabled());
}

TEST(TextureOverrideTest, SizedNameLimitsWhichTexturesMayBeReplaced) {
  TempDir dir;
  ASSERT_FALSE(dir.path().empty());
  const RgbaImage image = Gradient(4, 4);
  const std::uint64_t hash = 0x75dcd1166a6cbaa9ULL;
  ASSERT_TRUE(
      WritePngRgba(dir.path() + "/" + HashName(hash) + "_32x32.png", image));
  TextureOverrides sized(dir.path(), "");
  EXPECT_TRUE(sized.enabled());
  EXPECT_TRUE(sized.MayReplace(32, 32));
  EXPECT_FALSE(sized.MayReplace(64, 64));
  const auto found = sized.Lookup(hash);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->pixels, image.pixels);

  TempDir dump;
  EXPECT_TRUE(TextureOverrides("", dump.path()).MayReplace(64, 64));
  EXPECT_FALSE(TextureOverrides("", "").MayReplace(32, 32));
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
