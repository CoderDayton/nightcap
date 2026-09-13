#include "mocktail/graphics/etc2_decoder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace mocktail::graphics {
namespace {

using Block8 = std::array<std::uint8_t, 8>;

struct Rgba {
  int r;
  int g;
  int b;
  int a;
};

std::vector<std::uint8_t> Decode(EtcFormat format,
                                 const std::vector<std::uint8_t>& source,
                                 std::uint32_t width, std::uint32_t height) {
  std::vector<std::uint8_t> output(
      static_cast<std::size_t>(width) * height * EtcDecodedTexelBytes(format),
      0xAB);
  EXPECT_TRUE(DecodeEtcImage(format, source.data(), source.size(), width,
                             height, output.data(), output.size()));
  return output;
}

std::vector<std::uint8_t> Bytes(const Block8& block) {
  return {block.begin(), block.end()};
}

void ExpectRgba(const std::vector<std::uint8_t>& texels, std::uint32_t width,
                std::uint32_t x, std::uint32_t y, Rgba expected) {
  const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
  ASSERT_LT(offset + 3, texels.size());
  EXPECT_EQ(texels[offset], expected.r) << "x=" << x << " y=" << y;
  EXPECT_EQ(texels[offset + 1], expected.g) << "x=" << x << " y=" << y;
  EXPECT_EQ(texels[offset + 2], expected.b) << "x=" << x << " y=" << y;
  EXPECT_EQ(texels[offset + 3], expected.a) << "x=" << x << " y=" << y;
}

std::uint16_t Unsigned16(const std::vector<std::uint8_t>& texels,
                         std::size_t index) {
  return static_cast<std::uint16_t>(texels[index * 2] |
                                    (texels[index * 2 + 1] << 8));
}

std::int16_t Signed16(const std::vector<std::uint8_t>& texels,
                      std::size_t index) {
  return static_cast<std::int16_t>(Unsigned16(texels, index));
}

TEST(Etc2DecoderTest, ReportsBlockAndTexelSizes) {
  EXPECT_EQ(EtcBlockBytes(EtcFormat::kEtc2Rgb8), 8U);
  EXPECT_EQ(EtcBlockBytes(EtcFormat::kEtc2Rgb8A1), 8U);
  EXPECT_EQ(EtcBlockBytes(EtcFormat::kEtc2Rgba8), 16U);
  EXPECT_EQ(EtcBlockBytes(EtcFormat::kEacR11), 8U);
  EXPECT_EQ(EtcBlockBytes(EtcFormat::kEacR11Signed), 8U);
  EXPECT_EQ(EtcBlockBytes(EtcFormat::kEacRg11), 16U);
  EXPECT_EQ(EtcBlockBytes(EtcFormat::kEacRg11Signed), 16U);
  EXPECT_EQ(EtcDecodedTexelBytes(EtcFormat::kEtc2Rgb8), 4U);
  EXPECT_EQ(EtcDecodedTexelBytes(EtcFormat::kEtc2Rgb8A1), 4U);
  EXPECT_EQ(EtcDecodedTexelBytes(EtcFormat::kEtc2Rgba8), 4U);
  EXPECT_EQ(EtcDecodedTexelBytes(EtcFormat::kEacR11), 2U);
  EXPECT_EQ(EtcDecodedTexelBytes(EtcFormat::kEacRg11Signed), 4U);
}

TEST(Etc2DecoderTest, DecodesIndividualMode) {
  // Base colours 8 (left sub-block) and 0 (right), codeword 0 (2/8).
  // Pixel (0,0) index 1 (+8), pixels in column 1 index 2 (-2).
  const Block8 block = {0x80, 0x80, 0x80, 0x00, 0x00, 0xF0, 0x00, 0x01};
  const auto texels = Decode(EtcFormat::kEtc2Rgb8, Bytes(block), 4, 4);
  ExpectRgba(texels, 4, 0, 0, {144, 144, 144, 255});
  ExpectRgba(texels, 4, 0, 1, {138, 138, 138, 255});
  ExpectRgba(texels, 4, 1, 0, {134, 134, 134, 255});
  ExpectRgba(texels, 4, 2, 0, {2, 2, 2, 255});
}

TEST(Etc2DecoderTest, DecodesDifferentialMode) {
  // R/G/B = 16 with delta +1: base colours 132 and 140, modifier +2.
  const Block8 block = {0x81, 0x81, 0x81, 0x02, 0x00, 0x00, 0x00, 0x00};
  const auto texels = Decode(EtcFormat::kEtc2Rgb8, Bytes(block), 4, 4);
  ExpectRgba(texels, 4, 0, 0, {134, 134, 134, 255});
  ExpectRgba(texels, 4, 2, 3, {142, 142, 142, 255});
}

TEST(Etc2DecoderTest, DecodesTMode) {
  // Red overflows: colour 1 (0,255,0), colour 2 (0,255,136), distance 11.
  const Block8 block = {0x04, 0xF0, 0x0F, 0x86, 0x00, 0x12, 0x00, 0x03};
  const auto texels = Decode(EtcFormat::kEtc2Rgb8, Bytes(block), 4, 4);
  ExpectRgba(texels, 4, 0, 0, {11, 255, 147, 255});
  ExpectRgba(texels, 4, 0, 1, {0, 244, 125, 255});
  ExpectRgba(texels, 4, 1, 0, {0, 255, 136, 255});
  ExpectRgba(texels, 4, 1, 1, {0, 255, 0, 255});
}

TEST(Etc2DecoderTest, DecodesHMode) {
  // Green overflows: colour 1 (0,0,0), colour 2 (255,17,255), distance 3.
  const Block8 block = {0x00, 0x04, 0x78, 0xFA, 0x80, 0x01, 0x80, 0x00};
  const auto texels = Decode(EtcFormat::kEtc2Rgb8, Bytes(block), 4, 4);
  ExpectRgba(texels, 4, 0, 0, {255, 20, 255, 255});
  ExpectRgba(texels, 4, 3, 3, {252, 14, 252, 255});
  ExpectRgba(texels, 4, 1, 0, {3, 3, 3, 255});
}

TEST(Etc2DecoderTest, DecodesPlanarMode) {
  // Blue overflows: O=(130,64,0), H=(0,255,0), V=(255,0,255).
  const Block8 block = {0x40, 0x40, 0x04, 0x02, 0xFE, 0x07, 0xE0, 0x3F};
  const auto texels = Decode(EtcFormat::kEtc2Rgb8, Bytes(block), 4, 4);
  ExpectRgba(texels, 4, 0, 0, {130, 64, 0, 255});
  ExpectRgba(texels, 4, 3, 0, {33, 207, 0, 255});
  ExpectRgba(texels, 4, 0, 3, {224, 16, 191, 255});
  ExpectRgba(texels, 4, 3, 3, {126, 159, 191, 255});
}

TEST(Etc2DecoderTest, DecodesPunchthroughAlpha) {
  // Opaque bit clear: index 2 is transparent black, index 0 has no modifier.
  const Block8 block = {0x81, 0x81, 0x81, 0x00, 0x00, 0x01, 0x00, 0x02};
  const auto texels = Decode(EtcFormat::kEtc2Rgb8A1, Bytes(block), 4, 4);
  ExpectRgba(texels, 4, 0, 0, {0, 0, 0, 0});
  ExpectRgba(texels, 4, 0, 1, {140, 140, 140, 255});
  ExpectRgba(texels, 4, 1, 0, {132, 132, 132, 255});
  ExpectRgba(texels, 4, 2, 0, {140, 140, 140, 255});
}

TEST(Etc2DecoderTest, DecodesRgba8WithAlphaBlockFirst) {
  // Alpha: base 100, multiplier 2, table 0; pixel (0,0) index 7 (+14).
  const std::vector<std::uint8_t> block = {
      100,  0x20, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x88, 0x88, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const auto texels = Decode(EtcFormat::kEtc2Rgba8, block, 4, 4);
  ExpectRgba(texels, 4, 0, 0, {138, 138, 138, 128});
  ExpectRgba(texels, 4, 1, 0, {138, 138, 138, 94});
}

TEST(Etc2DecoderTest, DecodesUnsignedR11) {
  const Block8 block = {128, 0x10, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00};
  const auto texels = Decode(EtcFormat::kEacR11, Bytes(block), 4, 4);
  EXPECT_EQ(Unsigned16(texels, 0), 36497);
  EXPECT_EQ(Unsigned16(texels, 1), 32143);

  const Block8 zero_multiplier = {0, 0x00, 0, 0, 0, 0, 0, 0};
  const auto flat = Decode(EtcFormat::kEacR11, Bytes(zero_multiplier), 1, 1);
  EXPECT_EQ(Unsigned16(flat, 0), 32);
}

TEST(Etc2DecoderTest, DecodesSignedR11) {
  const Block8 block = {0x80, 0x10, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00};
  const auto texels = Decode(EtcFormat::kEacR11Signed, Bytes(block), 4, 4);
  EXPECT_EQ(Signed16(texels, 0), -28956);
  EXPECT_EQ(Signed16(texels, 1), -32767);
}

TEST(Etc2DecoderTest, DecodesRg11AsTwoChannels) {
  const std::vector<std::uint8_t> block = {
      128, 0x10, 0xE0, 0, 0, 0, 0, 0,
      0,   0x00, 0x00, 0, 0, 0, 0, 0,
  };
  const auto texels = Decode(EtcFormat::kEacRg11, block, 1, 1);
  EXPECT_EQ(Unsigned16(texels, 0), 36497);
  EXPECT_EQ(Unsigned16(texels, 1), 32);
}

TEST(Etc2DecoderTest, DecodesPartialAndMultipleBlocks) {
  const Block8 t_mode = {0x04, 0xF0, 0x0F, 0x86, 0x00, 0x12, 0x00, 0x03};
  const Block8 differential = {0x81, 0x81, 0x81, 0x02, 0, 0, 0, 0};
  std::vector<std::uint8_t> source = Bytes(t_mode);
  source.insert(source.end(), differential.begin(), differential.end());
  const auto texels = Decode(EtcFormat::kEtc2Rgb8, source, 5, 1);
  ExpectRgba(texels, 5, 0, 0, {11, 255, 147, 255});
  ExpectRgba(texels, 5, 1, 0, {0, 255, 136, 255});
  ExpectRgba(texels, 5, 4, 0, {134, 134, 134, 255});
}

TEST(Etc2DecoderTest, RejectsShortBuffers) {
  const std::vector<std::uint8_t> source(8, 0);
  std::vector<std::uint8_t> output(4 * 4 * 4, 0);
  EXPECT_FALSE(DecodeEtcImage(EtcFormat::kEtc2Rgb8, source.data(), 7, 4, 4,
                              output.data(), output.size()));
  EXPECT_FALSE(DecodeEtcImage(EtcFormat::kEtc2Rgb8, source.data(),
                              source.size(), 4, 4, output.data(),
                              output.size() - 1));
  EXPECT_FALSE(DecodeEtcImage(EtcFormat::kEtc2Rgb8, source.data(),
                              source.size(), 5, 4, output.data(),
                              output.size()));
  EXPECT_FALSE(DecodeEtcImage(EtcFormat::kEtc2Rgb8, nullptr, 0, 0, 0,
                              output.data(), output.size()));
}

}  // namespace
}  // namespace mocktail::graphics
