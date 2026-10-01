#include "mocktail/graphics/etc2_decoder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <random>
#include <set>
#include <thread>
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

std::vector<std::uint8_t> RandomBlocks(EtcFormat format, std::uint32_t width,
                                       std::uint32_t height,
                                       std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<std::uint8_t> source(((width + 3) / 4) * ((height + 3) / 4) *
                                   EtcBlockBytes(format));
  for (std::uint8_t& byte : source) {
    byte = static_cast<std::uint8_t>(rng());
  }
  return source;
}

TEST(Etc2DecoderTest, DecodesBlockRowBandsLikeWholeImage) {
  const std::uint32_t width = 13;
  const std::uint32_t height = 10;
  const auto source = RandomBlocks(EtcFormat::kEtc2Rgba8, width, height, 1);
  const auto whole = Decode(EtcFormat::kEtc2Rgba8, source, width, height);

  std::vector<std::uint8_t> banded(whole.size(), 0xAB);
  ASSERT_TRUE(DecodeEtcImageBlockRows(EtcFormat::kEtc2Rgba8, source.data(),
                                      source.size(), width, height, 1, 2,
                                      banded.data(), banded.size()));
  ASSERT_TRUE(DecodeEtcImageBlockRows(EtcFormat::kEtc2Rgba8, source.data(),
                                      source.size(), width, height, 0, 1,
                                      banded.data(), banded.size()));
  EXPECT_EQ(banded, whole);

  EXPECT_FALSE(DecodeEtcImageBlockRows(EtcFormat::kEtc2Rgba8, source.data(),
                                       source.size(), width, height, 2, 2,
                                       banded.data(), banded.size()));
}

TEST(Etc2DecoderTest, DecodesJobsOnWorkersLikeSerialDecode) {
  struct Case {
    EtcFormat format;
    std::uint32_t width;
    std::uint32_t height;
  };
  const Case cases[] = {{EtcFormat::kEtc2Rgb8, 1030, 770},
                        {EtcFormat::kEtc2Rgba8, 517, 1024},
                        {EtcFormat::kEacRg11, 300, 301},
                        {EtcFormat::kEtc2Rgb8A1, 3, 5}};
  std::vector<std::vector<std::uint8_t>> sources;
  std::vector<std::vector<std::uint8_t>> expected;
  std::vector<std::vector<std::uint8_t>> outputs;
  std::vector<EtcDecodeJob> jobs;
  for (std::size_t index = 0; index < std::size(cases); ++index) {
    const Case& c = cases[index];
    sources.push_back(RandomBlocks(c.format, c.width, c.height,
                                   static_cast<std::uint32_t>(index + 2)));
    expected.push_back(Decode(c.format, sources.back(), c.width, c.height));
    outputs.emplace_back(expected.back().size(), 0xAB);
  }
  for (std::size_t index = 0; index < std::size(cases); ++index) {
    const Case& c = cases[index];
    jobs.push_back({c.format, sources[index].data(), sources[index].size(),
                    c.width, c.height, outputs[index].data(),
                    outputs[index].size()});
  }
  std::vector<std::uint8_t> short_output(4 * 4 * 4 - 1, 0xAB);
  jobs.push_back({EtcFormat::kEtc2Rgb8, sources[0].data(), sources[0].size(),
                  4, 4, short_output.data(), short_output.size()});

  DecodeEtcJobs(jobs.data(), jobs.size(), 4);

  for (std::size_t index = 0; index < std::size(cases); ++index) {
    EXPECT_TRUE(jobs[index].ok) << index;
    EXPECT_EQ(outputs[index], expected[index]) << index;
  }
  EXPECT_FALSE(jobs.back().ok);
  EXPECT_EQ(short_output, std::vector<std::uint8_t>(4 * 4 * 4 - 1, 0xAB));
}

// Batches of small textures stay under the old parallel floor; the pool has
// to decode them exactly as the caller would.
TEST(Etc2DecoderTest, DecodesManySmallJobsLikeSerialDecode) {
  constexpr std::size_t kJobs = 24;
  constexpr std::uint32_t kWidth = 64;
  constexpr std::uint32_t kHeight = 64;
  std::vector<std::vector<std::uint8_t>> sources;
  std::vector<std::vector<std::uint8_t>> expected;
  std::vector<std::vector<std::uint8_t>> outputs;
  for (std::size_t index = 0; index < kJobs; ++index) {
    sources.push_back(RandomBlocks(EtcFormat::kEtc2Rgb8, kWidth, kHeight,
                                   static_cast<std::uint32_t>(index + 11)));
    expected.push_back(
        Decode(EtcFormat::kEtc2Rgb8, sources.back(), kWidth, kHeight));
    outputs.emplace_back(expected.back().size(), 0xAB);
  }
  std::vector<EtcDecodeJob> jobs;
  for (std::size_t index = 0; index < kJobs; ++index) {
    jobs.push_back({EtcFormat::kEtc2Rgb8, sources[index].data(),
                    sources[index].size(), kWidth, kHeight,
                    outputs[index].data(), outputs[index].size()});
  }

  DecodeEtcJobs(jobs.data(), jobs.size(), 8);

  for (std::size_t index = 0; index < kJobs; ++index) {
    EXPECT_TRUE(jobs[index].ok) << index;
    EXPECT_EQ(outputs[index], expected[index]) << index;
  }
}

// The workers outlive each batch, so a later batch must find them idle.
TEST(Etc2DecoderTest, DecodesCorrectlyAcrossRepeatedBatches) {
  constexpr std::uint32_t kWidth = 260;
  constexpr std::uint32_t kHeight = 132;
  const std::vector<std::uint8_t> source =
      RandomBlocks(EtcFormat::kEtc2Rgba8, kWidth, kHeight, 7);
  const std::vector<std::uint8_t> expected =
      Decode(EtcFormat::kEtc2Rgba8, source, kWidth, kHeight);

  for (int round = 0; round < 40; ++round) {
    std::vector<std::uint8_t> output(expected.size(), 0xAB);
    EtcDecodeJob job{EtcFormat::kEtc2Rgba8, source.data(), source.size(),
                     kWidth,                kHeight,      output.data(),
                     output.size()};
    DecodeEtcJobs(&job, 1, 8);
    ASSERT_TRUE(job.ok) << round;
    ASSERT_EQ(output, expected) << round;
  }
}

std::size_t ThreadCount() {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator("/proc/self/task"),
                    std::filesystem::directory_iterator()));
}

// Other batch work (ETC2 emit) drains its own queue across the decode
// workers. The function runs worker_count times, once of them on the caller,
// and a second batch reuses the threads the first one started.
TEST(Etc2DecoderTest, RunsAFunctionAcrossPooledWorkersWithoutNewThreads) {
  struct Probe {
    std::mutex mutex;
    std::set<std::thread::id> threads;
    std::atomic<int> calls{0};
  };
  const auto run = [](void* context) {
    auto* probe = static_cast<Probe*>(context);
    probe->calls.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->threads.insert(std::this_thread::get_id());
  };

  Probe first;
  RunOnDecodeWorkers(run, &first, 3);
  EXPECT_EQ(first.calls.load(), 3);
  // An idle worker may claim two of the slots.
  EXPECT_GE(first.threads.size(), 2u);
  EXPECT_LE(first.threads.size(), 3u);
  EXPECT_EQ(first.threads.count(std::this_thread::get_id()), 1u);

  const std::size_t pooled = ThreadCount();
  Probe second;
  RunOnDecodeWorkers(run, &second, 3);
  EXPECT_EQ(second.calls.load(), 3);
  EXPECT_EQ(ThreadCount(), pooled);
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

namespace reference {

constexpr std::array<int, 8> kDistance = {3, 6, 11, 16, 23, 32, 41, 64};

constexpr int kEtc1Modifiers[8][4] = {
    {2, 8, -2, -8},     {5, 17, -5, -17},   {9, 29, -9, -29},
    {13, 42, -13, -42}, {18, 60, -18, -60}, {24, 80, -24, -80},
    {33, 106, -33, -106}, {47, 183, -47, -183},
};

constexpr int kEacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},  {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},  {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},  {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},   {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},   {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},    {-3, -5, -7, -9, 2, 4, 6, 8},
};

inline std::uint8_t Clamp255(int value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

inline int Expand4(int value) { return (value << 4) | value; }
inline int Expand5(int value) { return (value << 3) | (value >> 2); }
inline int Expand6(int value) { return (value << 2) | (value >> 4); }
inline int Expand7(int value) { return (value << 1) | (value >> 6); }

inline void DecodeRgbBlock(const std::uint8_t* b, bool punchthrough,
                           std::array<std::uint8_t, 64>* out) {
  static constexpr int kDelta[8] = {0, 1, 2, 3, -4, -3, -2, -1};
  const bool diff_or_opaque = (b[3] & 2) != 0;
  const bool opaque = !punchthrough || diff_or_opaque;
  const int msb = (b[4] << 8) | b[5];
  const int lsb = (b[6] << 8) | b[7];
  auto index_of = [msb, lsb](int texel) {
    return (((msb >> texel) & 1) << 1) | ((lsb >> texel) & 1);
  };
  auto write = [&](int texel, int r, int g, int bl, int a) {
    (*out)[texel * 4] = Clamp255(r);
    (*out)[texel * 4 + 1] = Clamp255(g);
    (*out)[texel * 4 + 2] = Clamp255(bl);
    (*out)[texel * 4 + 3] = static_cast<std::uint8_t>(a);
  };

  const int r_sum = (b[0] >> 3) + kDelta[b[0] & 7];
  const int g_sum = (b[1] >> 3) + kDelta[b[1] & 7];
  const int b_sum = (b[2] >> 3) + kDelta[b[2] & 7];
  const bool individual = !punchthrough && !diff_or_opaque;

  if (!individual && (r_sum < 0 || r_sum > 31 || g_sum < 0 || g_sum > 31)) {
    std::array<std::array<int, 3>, 4> paint{};
    if (r_sum < 0 || r_sum > 31) {
      const std::array<int, 3> c1 = {
          Expand4((((b[0] >> 3) & 3) << 2) | (b[0] & 3)),
          Expand4(b[1] >> 4), Expand4(b[1] & 15)};
      const std::array<int, 3> c2 = {Expand4(b[2] >> 4), Expand4(b[2] & 15),
                                     Expand4(b[3] >> 4)};
      const int d = kDistance[(((b[3] >> 2) & 3) << 1) | (b[3] & 1)];
      for (int c = 0; c < 3; ++c) {
        paint[0][c] = c1[c];
        paint[1][c] = c2[c] + d;
        paint[2][c] = c2[c];
        paint[3][c] = c2[c] - d;
      }
    } else {
      const std::array<int, 3> c1 = {
          Expand4((b[0] >> 3) & 15),
          Expand4(((b[0] & 7) << 1) | ((b[1] >> 4) & 1)),
          Expand4((b[1] & 8) | ((b[1] & 3) << 1) | (b[2] >> 7))};
      const std::array<int, 3> c2 = {
          Expand4((b[2] >> 3) & 15), Expand4(((b[2] & 7) << 1) | (b[3] >> 7)),
          Expand4((b[3] >> 3) & 15)};
      const int v1 = (c1[0] << 16) | (c1[1] << 8) | c1[2];
      const int v2 = (c2[0] << 16) | (c2[1] << 8) | c2[2];
      const int d =
          kDistance[(b[3] & 4) | ((b[3] & 1) << 1) | (v1 >= v2 ? 1 : 0)];
      for (int c = 0; c < 3; ++c) {
        paint[0][c] = c1[c] + d;
        paint[1][c] = c1[c] - d;
        paint[2][c] = c2[c] + d;
        paint[3][c] = c2[c] - d;
      }
    }
    for (int texel = 0; texel < 16; ++texel) {
      const int index = index_of(texel);
      if (!opaque && index == 2) {
        write(texel, 0, 0, 0, 0);
      } else {
        write(texel, paint[index][0], paint[index][1], paint[index][2], 255);
      }
    }
    return;
  }

  if (!individual && (b_sum < 0 || b_sum > 31)) {
    const int ro = Expand6((b[0] >> 1) & 63);
    const int go = Expand7(((b[0] & 1) << 6) | ((b[1] >> 1) & 63));
    const int bo = Expand6(((b[1] & 1) << 5) | (b[2] & 0x18) |
                           ((b[2] & 3) << 1) | (b[3] >> 7));
    const int rh = Expand6(((b[3] & 0x7c) >> 1) | (b[3] & 1));
    const int gh = Expand7(b[4] >> 1);
    const int bh = Expand6(((b[4] & 1) << 5) | (b[5] >> 3));
    const int rv = Expand6(((b[5] & 7) << 3) | (b[6] >> 5));
    const int gv = Expand7(((b[6] & 31) << 2) | (b[7] >> 6));
    const int bv = Expand6(b[7] & 63);
    for (int x = 0; x < 4; ++x) {
      for (int y = 0; y < 4; ++y) {
        write(x * 4 + y, (x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2,
              (x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2,
              (x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2, 255);
      }
    }
    return;
  }

  std::array<std::array<int, 3>, 2> base{};
  for (int c = 0; c < 3; ++c) {
    if (individual) {
      base[0][c] = Expand4(b[c] >> 4);
      base[1][c] = Expand4(b[c] & 15);
    } else {
      base[0][c] = Expand5(b[c] >> 3);
      base[1][c] = Expand5((b[c] >> 3) + kDelta[b[c] & 7]);
    }
  }
  const int tables[2] = {b[3] >> 5, (b[3] >> 2) & 7};
  const bool flipped = (b[3] & 1) != 0;
  for (int x = 0; x < 4; ++x) {
    for (int y = 0; y < 4; ++y) {
      const int texel = x * 4 + y;
      const int index = index_of(texel);
      if (!opaque && index == 2) {
        write(texel, 0, 0, 0, 0);
        continue;
      }
      const int sub = flipped ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
      const int modifier =
          (!opaque && index == 0) ? 0 : kEtc1Modifiers[tables[sub]][index];
      write(texel, base[sub][0] + modifier, base[sub][1] + modifier,
            base[sub][2] + modifier, 255);
    }
  }
}

inline int EacIndex(const std::uint8_t* b, int texel) {
  std::uint64_t bits = 0;
  for (int i = 2; i < 8; ++i) {
    bits = (bits << 8) | b[i];
  }
  return static_cast<int>((bits >> (45 - texel * 3)) & 7);
}

inline int EacModifier(const std::uint8_t* b, int texel) {
  return kEacModifiers[b[1] & 15][EacIndex(b, texel)];
}

inline std::uint16_t DecodeR11(const std::uint8_t* b, int texel, bool is_signed) {
  const int multiplier = b[1] >> 4;
  const int modifier = EacModifier(b, texel);
  const int scaled = multiplier != 0 ? modifier * multiplier * 8 : modifier;
  if (!is_signed) {
    const int value = std::clamp(b[0] * 8 + 4 + scaled, 0, 2047);
    return static_cast<std::uint16_t>((value << 5) | (value >> 6));
  }
  const int base = std::max(-127, static_cast<int>(static_cast<std::int8_t>(b[0])));
  const int value = std::clamp(base * 8 + scaled, -1023, 1023);
  const int magnitude = value < 0 ? -value : value;
  const int expanded = (magnitude << 5) | (magnitude >> 5);
  return static_cast<std::uint16_t>(
      static_cast<std::int16_t>(value < 0 ? -expanded : expanded));
}

inline void DecodeReference(EtcFormat format, const std::uint8_t* source,
                            std::uint32_t width, std::uint32_t height,
                            std::uint8_t* destination) {
  const std::size_t texel_bytes = EtcDecodedTexelBytes(format);
  const std::size_t block_bytes = EtcBlockBytes(format);
  const std::uint64_t blocks_wide = (static_cast<std::uint64_t>(width) + 3) / 4;
  const std::uint64_t blocks_high = (static_cast<std::uint64_t>(height) + 3) / 4;
  std::array<std::uint8_t, 64> rgba{};

  for (std::uint64_t by = 0; by < blocks_high; ++by) {
    for (std::uint64_t bx = 0; bx < blocks_wide; ++bx) {
      const std::uint8_t* block = source + (by * blocks_wide + bx) * block_bytes;
      if (format == EtcFormat::kEtc2Rgb8 || format == EtcFormat::kEtc2Rgb8A1) {
        DecodeRgbBlock(block, format == EtcFormat::kEtc2Rgb8A1, &rgba);
      } else if (format == EtcFormat::kEtc2Rgba8) {
        DecodeRgbBlock(block + 8, false, &rgba);
      }
      for (std::uint32_t y = 0; y < 4 && by * 4 + y < height; ++y) {
        for (std::uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
          const int texel = static_cast<int>(x * 4 + y);
          std::uint8_t* out = destination + ((by * 4 + y) * width + bx * 4 + x) * texel_bytes;
          switch (format) {
            case EtcFormat::kEtc2Rgb8:
            case EtcFormat::kEtc2Rgb8A1:
            case EtcFormat::kEtc2Rgba8:
              std::copy_n(rgba.data() + texel * 4, 4, out);
              if (format == EtcFormat::kEtc2Rgba8) {
                const int alpha = block[0] + EacModifier(block, texel) * (block[1] >> 4);
                out[3] = Clamp255(alpha);
              }
              break;
            case EtcFormat::kEacR11:
            case EtcFormat::kEacR11Signed:
            case EtcFormat::kEacRg11:
            case EtcFormat::kEacRg11Signed: {
              const bool is_signed = format == EtcFormat::kEacR11Signed || format == EtcFormat::kEacRg11Signed;
              const int channels = static_cast<int>(texel_bytes / 2);
              for (int channel = 0; channel < channels; ++channel) {
                const std::uint16_t value = DecodeR11(block + channel * 8, texel, is_signed);
                out[channel * 2] = static_cast<std::uint8_t>(value);
                out[channel * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
              }
              break;
            }
          }
        }
      }
    }
  }
}

}  // namespace reference

TEST(Etc2DecoderTest, ComparesBitExactAgainstScalarReferenceAcrossAllFormats) {
  const EtcFormat formats[] = {
      EtcFormat::kEtc2Rgb8,
      EtcFormat::kEtc2Rgb8A1,
      EtcFormat::kEtc2Rgba8,
      EtcFormat::kEacR11,
      EtcFormat::kEacR11Signed,
      EtcFormat::kEacRg11,
      EtcFormat::kEacRg11Signed,
  };
  const struct {
    std::uint32_t width;
    std::uint32_t height;
  } sizes[] = {
      {1, 1},
      {4, 4},
      {7, 5},
      {16, 16},
      {67, 59},
      {128, 128},
  };
  for (std::size_t f = 0; f < std::size(formats); ++f) {
    const EtcFormat format = formats[f];
    for (std::size_t s = 0; s < std::size(sizes); ++s) {
      const std::uint32_t width = sizes[s].width;
      const std::uint32_t height = sizes[s].height;
      const auto source = RandomBlocks(
          format, width, height,
          static_cast<std::uint32_t>(0xABCD1234 + f * 100 + s));
      const auto actual = Decode(format, source, width, height);
      std::vector<std::uint8_t> expected(actual.size(), 0);
      reference::DecodeReference(format, source.data(), width, height,
                                 expected.data());
      EXPECT_EQ(actual, expected)
          << "Mismatch in format=" << static_cast<int>(format)
          << " width=" << width << " height=" << height;
    }
  }
}

}  // namespace
}  // namespace mocktail::graphics
