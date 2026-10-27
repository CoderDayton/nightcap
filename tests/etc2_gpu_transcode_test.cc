#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "mocktail/graphics/bc_decoder.h"
#include "mocktail/graphics/etc2_gpu_decoder.h"
#include "mocktail/graphics/texture_override.h"
#include "vulkan_test_device.h"

namespace mocktail::graphics {
namespace {

using mocktail::testing::BeginTestCommands;
using mocktail::testing::BindAndMap;
using mocktail::testing::CloseTestDevices;
using mocktail::testing::CreateHostBuffer;
using mocktail::testing::DestroyHostBuffer;
using mocktail::testing::DestroyTestCommands;
using mocktail::testing::FindMemoryType;
using mocktail::testing::HostBuffer;
using mocktail::testing::HostReadBarrier;
using mocktail::testing::OpenTestDevices;
using mocktail::testing::SubmitAndWait;
using mocktail::testing::TestCommands;
using mocktail::testing::VulkanTestDevice;

constexpr EtcFormat kAllFormats[] = {
    EtcFormat::kEtc2Rgb8,    EtcFormat::kEtc2Rgb8A1,   EtcFormat::kEtc2Rgba8,
    EtcFormat::kEacR11,      EtcFormat::kEacR11Signed, EtcFormat::kEacRg11,
    EtcFormat::kEacRg11Signed};

bool IsColor(EtcFormat format) {
  return format == EtcFormat::kEtc2Rgb8 || format == EtcFormat::kEtc2Rgb8A1 ||
         format == EtcFormat::kEtc2Rgba8;
}

bool IsSigned(EtcFormat format) {
  return format == EtcFormat::kEacR11Signed ||
         format == EtcFormat::kEacRg11Signed;
}

std::vector<std::uint8_t> Transcode(const VulkanTestDevice& vk,
                                    Etc2GpuDecoder& decoder,
                                    const std::vector<std::uint8_t>& source,
                                    const Etc2GpuJob& job) {
  const VkDeviceSize destination_bytes =
      job.destination_offset + Etc2GpuOutputBytes(job);
  HostBuffer src =
      CreateHostBuffer(vk, source.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  HostBuffer dst = CreateHostBuffer(vk, destination_bytes,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  BindAndMap(vk, &src);
  BindAndMap(vk, &dst);
  std::memcpy(src.mapped, source.data(), source.size());
  std::memset(dst.mapped, 0, destination_bytes);
  std::vector<std::uint8_t> out;
  Etc2GpuBinding binding;
  if (decoder.CreateBinding(src.buffer, dst.buffer, &binding)) {
    TestCommands commands = BeginTestCommands(vk);
    const bool recorded = decoder.Record(commands.buffer, binding, &job, 1);
    HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT);
    vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
    if (recorded && SubmitAndWait(vk, {commands.buffer})) {
      const auto* bytes = static_cast<const std::uint8_t*>(dst.mapped);
      out.assign(bytes, bytes + destination_bytes);
    }
    DestroyTestCommands(vk, commands);
    decoder.DestroyBinding(binding);
  }
  DestroyHostBuffer(vk, src);
  DestroyHostBuffer(vk, dst);
  return out;
}

Etc2GpuJob TightJob(EtcFormat format, std::uint32_t blocks) {
  Etc2GpuJob job;
  job.format = format;
  job.blocks_wide = blocks;
  job.blocks_high = 1;
  job.source_row_blocks = blocks;
  job.source_layer_blocks = blocks;
  job.transcode = true;
  return job;
}

std::vector<std::uint8_t> RandomBlocks(EtcFormat format, std::uint32_t blocks,
                                       std::uint64_t seed) {
  std::vector<std::uint8_t> out(std::size_t{blocks} * EtcBlockBytes(format));
  std::mt19937_64 random(seed);
  for (std::uint8_t& byte : out) {
    byte = static_cast<std::uint8_t>(random());
  }
  return out;
}

// Texels of one ETC block, in the row-major order DecodeBcBlock writes.
std::vector<std::uint8_t> DecodeEtcBlock(EtcFormat format,
                                         const std::uint8_t* block) {
  std::vector<std::uint8_t> out(16 * EtcDecodedTexelBytes(format));
  EXPECT_TRUE(DecodeEtcImage(format, block, EtcBlockBytes(format), 4, 4,
                             out.data(), out.size()));
  return out;
}

// Signed EAC channels come back as int16.
std::vector<int> Channels(EtcFormat format, const std::uint8_t* texels,
                          int texel) {
  std::vector<int> out;
  if (IsColor(format)) {
    for (int c = 0; c < 4; ++c) {
      out.push_back(texels[texel * 4 + c]);
    }
    return out;
  }
  const int channels =
      format == EtcFormat::kEacRg11 || format == EtcFormat::kEacRg11Signed ? 2
                                                                           : 1;
  for (int c = 0; c < channels; ++c) {
    const std::uint8_t* p = texels + (texel * channels + c) * 2;
    const int raw = p[0] | (p[1] << 8);
    out.push_back(IsSigned(format) ? static_cast<std::int16_t>(raw) : raw);
  }
  return out;
}

struct Quality {
  double psnr = 0;
  int max_error = 0;
  std::size_t undecodable = 0;
  std::size_t cutout_changes = 0;
  std::size_t opaque_changes = 0;
};

// Error against the ETC decode, colour weighted by the reference alpha as it
// is blended on screen. PSNR peaks are 255 for colour and 65535 for EAC.
Quality Measure(EtcFormat format, const std::vector<std::uint8_t>& etc,
                const std::uint8_t* bc, std::uint32_t blocks) {
  Quality q;
  const BcFormat bc_format = BcFormatFor(format);
  const std::size_t etc_bytes = EtcBlockBytes(format);
  const std::size_t bc_bytes = BcBlockBytes(bc_format);
  double squared = 0;
  double samples = 0;
  std::vector<std::uint8_t> decoded(16 * EtcDecodedTexelBytes(format));
  for (std::uint32_t b = 0; b < blocks; ++b) {
    const std::vector<std::uint8_t> reference =
        DecodeEtcBlock(format, etc.data() + b * etc_bytes);
    if (!DecodeBcBlock(bc_format, bc + b * bc_bytes, decoded.data())) {
      ++q.undecodable;
      continue;
    }
    for (int t = 0; t < 16; ++t) {
      const std::vector<int> want = Channels(format, reference.data(), t);
      const std::vector<int> got = Channels(format, decoded.data(), t);
      const double weight = IsColor(format) ? (want[3] + 1) / 256.0 : 1.0;
      for (std::size_t c = 0; c < want.size(); ++c) {
        const int error = std::abs(want[c] - got[c]);
        const double w = IsColor(format) && c < 3 ? weight : 1.0;
        squared += w * error * error;
        samples += 1;
        if (IsColor(format) && c < 3 && want[3] == 0) {
          continue;
        }
        q.max_error = std::max(q.max_error, error);
      }
      if (IsColor(format)) {
        if (format == EtcFormat::kEtc2Rgb8A1 &&
            (want[3] < 128) != (got[3] < 128)) {
          ++q.cutout_changes;
        }
        if (format == EtcFormat::kEtc2Rgb8 && got[3] != 255) {
          ++q.opaque_changes;
        }
      }
    }
  }
  const double peak = IsColor(format) ? 255.0 : 65535.0;
  const double mse = samples > 0 ? squared / samples : 0;
  q.psnr = mse > 0 ? 10 * std::log10(peak * peak / mse) : 99;
  return q;
}

// Individual or differential ETC2 RGB block whose texels all decode without
// clamping: two halves, each one base colour plus a grey offset.
bool IsUnclampedHalves(const std::uint8_t* b) {
  static constexpr int kDelta[8] = {0, 1, 2, 3, -4, -3, -2, -1};
  static constexpr int kTables[8][2] = {{2, 8},   {5, 17},  {9, 29},
                                        {13, 42}, {18, 60}, {24, 80},
                                        {33, 106}, {47, 183}};
  const bool differential = (b[3] & 2) != 0;
  int base[2][3];
  for (int c = 0; c < 3; ++c) {
    if (differential) {
      const int a = b[c] >> 3;
      const int sum = a + kDelta[b[c] & 7];
      if (sum < 0 || sum > 31) {
        return false;
      }
      base[0][c] = (a << 3) | (a >> 2);
      base[1][c] = (sum << 3) | (sum >> 2);
    } else {
      base[0][c] = (b[c] >> 4) * 17;
      base[1][c] = (b[c] & 15) * 17;
    }
  }
  const int tables[2] = {b[3] >> 5, (b[3] >> 2) & 7};
  for (int s = 0; s < 2; ++s) {
    const int reach = kTables[tables[s]][1];
    for (int c = 0; c < 3; ++c) {
      if (base[s][c] - reach < 0 || base[s][c] + reach > 255) {
        return false;
      }
    }
  }
  return true;
}

class Etc2GpuTranscodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    devices_ = OpenTestDevices();
    if (devices_.empty()) {
      GTEST_SKIP() << "no Vulkan device with a compute queue";
    }
  }
  void TearDown() override { CloseTestDevices(&devices_); }

  std::unique_ptr<Etc2GpuDecoder> Decoder(const VulkanTestDevice& vk) {
    std::string error;
    std::unique_ptr<Etc2GpuDecoder> decoder =
        Etc2GpuDecoder::Create(vk.device, vk.get_device_proc_addr, &error);
    EXPECT_NE(decoder, nullptr) << error;
    EXPECT_TRUE(decoder == nullptr || decoder->CanTranscode());
    return decoder;
  }

  std::vector<VulkanTestDevice> devices_;
};

TEST(Etc2GpuTranscodeSizeTest, OutputIsOneBcBlockPerEtcBlock) {
  Etc2GpuJob job;
  job.format = EtcFormat::kEtc2Rgb8;
  job.blocks_wide = 3;
  job.blocks_high = 2;
  job.layers = 5;
  job.transcode = true;
  EXPECT_EQ(Etc2GpuOutputBytes(job), 5U * 3 * 2 * 16);
  job.format = EtcFormat::kEacR11Signed;
  EXPECT_EQ(Etc2GpuOutputBytes(job), 5U * 3 * 2 * 8);
  job.format = EtcFormat::kEacRg11;
  EXPECT_EQ(Etc2GpuOutputBytes(job), 5U * 3 * 2 * 16);
  job.transcode = false;
  EXPECT_EQ(Etc2GpuOutputBytes(job), 5U * 12 * 8 * 4);
}

// Upscaled textures reach the encoder as RGBA8 texels rather than ETC2
// blocks. The image mixes smooth gradients, a hard two-colour edge and an
// alpha ramp, as an adaptively upscaled texture does.
TEST_F(Etc2GpuTranscodeTest, EncodesRgbaTexelsAsBc7) {
  constexpr std::uint32_t kSize = 64;
  constexpr std::uint32_t kPitch = 72;
  std::vector<std::uint8_t> texels(std::size_t{kPitch} * kSize * 4 + 64);
  constexpr std::size_t kOffset = 64;
  for (std::uint32_t y = 0; y < kSize; ++y) {
    for (std::uint32_t x = 0; x < kSize; ++x) {
      std::uint8_t* t = texels.data() + kOffset + (std::size_t{y} * kPitch + x) * 4;
      const bool edge = x >= 40 && y < 24;
      t[0] = edge ? (((x / 4 + y / 4) % 2) ? 230 : 20)
                  : static_cast<std::uint8_t>(x * 3 + y);
      t[1] = static_cast<std::uint8_t>(edge ? 40 : 200 - y * 2);
      t[2] = static_cast<std::uint8_t>(edge ? 90 : (x * y) / 20);
      t[3] = static_cast<std::uint8_t>(y < 48 ? 255 : 255 - (y - 48) * 15);
    }
  }
  for (const VulkanTestDevice& vk : devices_) {
    SCOPED_TRACE(vk.name);
    std::unique_ptr<Etc2GpuDecoder> decoder = Decoder(vk);
    ASSERT_NE(decoder, nullptr);
    Etc2GpuJob job;
    job.format = EtcFormat::kEtc2Rgba8;
    job.rgba_source = true;
    job.transcode = true;
    job.blocks_wide = kSize / 4;
    job.blocks_high = kSize / 4;
    job.source_offset = kOffset;
    job.source_row_blocks = kPitch / 4;
    job.source_layer_blocks = kPitch / 4 * (kSize / 4);
    const std::vector<std::uint8_t> out = Transcode(vk, *decoder, texels, job);
    ASSERT_EQ(out.size(), Etc2GpuOutputBytes(job));
    double squared = 0;
    std::uint8_t decoded[64];
    for (std::uint32_t b = 0; b < job.blocks_wide * job.blocks_high; ++b) {
      ASSERT_TRUE(DecodeBcBlock(BcFormat::kBc7, out.data() + b * 16, decoded));
      const std::uint32_t bx = b % job.blocks_wide;
      const std::uint32_t by = b / job.blocks_wide;
      for (int i = 0; i < 64; ++i) {
        const std::uint32_t x = bx * 4 + (i / 4) % 4;
        const std::uint32_t y = by * 4 + i / 16;
        const double want =
            texels[kOffset + (std::size_t{y} * kPitch + x) * 4 + i % 4];
        squared += (want - decoded[i]) * (want - decoded[i]);
      }
    }
    const double mse = squared / (kSize * kSize * 4);
    EXPECT_GE(10 * std::log10(255.0 * 255.0 / std::max(mse, 1e-9)), 40.0);
  }
}

// The scale job matches ScaleRgba: the adaptive upscale when growing, the
// box resample otherwise, padded out to whole blocks with the edge texels.
TEST_F(Etc2GpuTranscodeTest, ScalesTexelsLikeTheCpu) {
  constexpr std::uint32_t kWidth = 48;
  constexpr std::uint32_t kHeight = 40;
  std::vector<std::uint8_t> source(16 + std::size_t{kWidth} * kHeight * 4);
  const std::uint32_t header[4] = {kWidth, kHeight, 0, 0};
  std::memcpy(source.data(), header, sizeof(header));
  std::uint8_t* texels = source.data() + 16;
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      std::uint8_t* t = texels + (std::size_t{y} * kWidth + x) * 4;
      const bool edge = x >= 30 && y < 20;
      t[0] = edge ? (((x / 3 + y / 5) % 2) ? 230 : 20)
                  : static_cast<std::uint8_t>(x * 5 + y);
      t[1] = static_cast<std::uint8_t>(edge ? 40 : 200 - y * 4);
      t[2] = static_cast<std::uint8_t>(edge ? 90 : (x * y) / 8);
      t[3] = static_cast<std::uint8_t>(y < 30 ? 255 : (x % 7) * 40);
    }
  }
  struct Case {
    std::uint32_t width;
    std::uint32_t height;
    bool exact;
  };
  constexpr Case kCases[] = {
      {192, 160, false}, {190, 158, false}, {48, 40, true}, {12, 10, true}};
  for (const VulkanTestDevice& vk : devices_) {
    SCOPED_TRACE(vk.name);
    std::unique_ptr<Etc2GpuDecoder> decoder = Decoder(vk);
    ASSERT_NE(decoder, nullptr);
    for (const Case& c : kCases) {
      SCOPED_TRACE(c.width);
      std::vector<std::uint8_t> expected(std::size_t{c.width} * c.height * 4);
      if (c.exact) {
        ResampleRgba(texels, kWidth, kHeight, c.width, c.height,
                     expected.data());
      } else {
        UpscaleRgba(texels, kWidth, kHeight, std::size_t{kWidth} * 4, c.width,
                    c.height, expected.data());
      }
      Etc2GpuJob job;
      job.format = EtcFormat::kEtc2Rgba8;
      job.scale = true;
      job.scale_width = c.width;
      job.scale_height = c.height;
      job.blocks_wide = (c.width + 3) / 4;
      job.blocks_high = (c.height + 3) / 4;
      const std::vector<std::uint8_t> out =
          Transcode(vk, *decoder, source, job);
      ASSERT_EQ(out.size(), Etc2GpuOutputBytes(job));
      const std::uint32_t pitch = job.blocks_wide * 4;
      double squared = 0;
      for (std::uint32_t y = 0; y < job.blocks_high * 4; ++y) {
        for (std::uint32_t x = 0; x < pitch; ++x) {
          const std::uint8_t* got = out.data() + (std::size_t{y} * pitch + x) * 4;
          const std::uint8_t* want =
              expected.data() + (std::size_t{std::min(y, c.height - 1)} *
                                     c.width +
                                 std::min(x, c.width - 1)) *
                                    4;
          for (int ch = 0; ch < 4; ++ch) {
            if (c.exact) {
              ASSERT_EQ(got[ch], want[ch]) << x << "," << y;
            }
            squared += (got[ch] - want[ch]) * double(got[ch] - want[ch]);
          }
        }
      }
      const double mse = squared / (out.size());
      EXPECT_GE(10 * std::log10(255.0 * 255.0 / std::max(mse, 1e-9)), 50.0);
    }
  }
}

// A decode job's output can feed the scale job in place: the texels sit
// apart from the header, in rows of the pitch the header gives.
TEST_F(Etc2GpuTranscodeTest, ScalesTexelsAtAnOffsetAndPitch) {
  constexpr std::uint32_t kWidth = 20;
  constexpr std::uint32_t kHeight = 12;
  constexpr std::uint32_t kPitch = 24;
  constexpr std::size_t kTexels = 256;
  std::vector<std::uint8_t> source(kTexels + std::size_t{kPitch} * kHeight * 4,
                                   0xAB);
  const std::uint32_t header[4] = {kWidth, kHeight, kPitch, 0};
  std::memcpy(source.data(), header, sizeof(header));
  std::vector<std::uint8_t> tight(std::size_t{kWidth} * kHeight * 4);
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const std::uint8_t texel[4] = {static_cast<std::uint8_t>(x * 12),
                                     static_cast<std::uint8_t>(y * 20),
                                     static_cast<std::uint8_t>((x ^ y) * 9),
                                     255};
      std::memcpy(tight.data() + (std::size_t{y} * kWidth + x) * 4, texel, 4);
      std::memcpy(source.data() + kTexels + (std::size_t{y} * kPitch + x) * 4,
                  texel, 4);
    }
  }
  std::vector<std::uint8_t> expected(std::size_t{kWidth} * kHeight * 16 * 4);
  UpscaleRgba(tight.data(), kWidth, kHeight, std::size_t{kWidth} * 4,
              kWidth * 4, kHeight * 4, expected.data());
  for (const VulkanTestDevice& vk : devices_) {
    SCOPED_TRACE(vk.name);
    std::unique_ptr<Etc2GpuDecoder> decoder = Decoder(vk);
    ASSERT_NE(decoder, nullptr);
    Etc2GpuJob job;
    job.format = EtcFormat::kEtc2Rgba8;
    job.scale = true;
    job.scale_width = kWidth * 4;
    job.scale_height = kHeight * 4;
    job.scale_texels_offset = kTexels;
    job.blocks_wide = kWidth;
    job.blocks_high = kHeight;
    const std::vector<std::uint8_t> out = Transcode(vk, *decoder, source, job);
    ASSERT_EQ(out.size(), expected.size());
    double squared = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
      squared += (out[i] - expected[i]) * double(out[i] - expected[i]);
    }
    const double mse = squared / out.size();
    EXPECT_GE(10 * std::log10(255.0 * 255.0 / std::max(mse, 1e-9)), 50.0);
  }
}

TEST_F(Etc2GpuTranscodeTest, RandomBlocksMeetTheQualityFloor) {
  constexpr std::uint32_t kBlocks = 1 << 16;
  struct Floor {
    EtcFormat format;
    double psnr;
  };
  // Colour floors are 0.4 dB under the measured 28.9, 25.2 and 24.3 dB, which
  // are about 1.7 dB under bc7enc at its highest quality (30.5, 26.5, 26.1).
  // The encoder trades that for running as fast as plain decoding. EAC
  // floors are 1 dB under an exhaustive BC4 search (37.8 dB).
  constexpr Floor kFloors[] = {
      {EtcFormat::kEtc2Rgb8, 28.5},    {EtcFormat::kEtc2Rgb8A1, 24.8},
      {EtcFormat::kEtc2Rgba8, 23.9},   {EtcFormat::kEacR11, 36.8},
      {EtcFormat::kEacR11Signed, 36.8}, {EtcFormat::kEacRg11, 36.8},
      {EtcFormat::kEacRg11Signed, 36.8}};
  for (VulkanTestDevice& vk : devices_) {
    SCOPED_TRACE(vk.name);
    std::unique_ptr<Etc2GpuDecoder> decoder = Decoder(vk);
    ASSERT_NE(decoder, nullptr);
    for (const Floor& floor : kFloors) {
      SCOPED_TRACE(static_cast<int>(floor.format));
      const std::vector<std::uint8_t> etc =
          RandomBlocks(floor.format, kBlocks, 11 + static_cast<int>(floor.format));
      const std::vector<std::uint8_t> bc =
          Transcode(vk, *decoder, etc, TightJob(floor.format, kBlocks));
      ASSERT_FALSE(bc.empty());
      const Quality q = Measure(floor.format, etc, bc.data(), kBlocks);
      EXPECT_EQ(q.undecodable, 0U);
      EXPECT_EQ(q.cutout_changes, 0U);
      EXPECT_EQ(q.opaque_changes, 0U);
      EXPECT_GE(q.psnr, floor.psnr);
      std::printf("  format %d: %.2f dB, max error %d\n",
                  static_cast<int>(floor.format), q.psnr, q.max_error);
    }
  }
}

// Measured 43.2 dB, worst texel 17 levels. Random halves use the widest
// modifier tables far more than real textures do (see the APK test).
TEST_F(Etc2GpuTranscodeTest, UnclampedHalvesKeepTheirDetail) {
  constexpr std::uint32_t kBlocks = 1 << 16;
  std::vector<std::uint8_t> etc;
  std::mt19937_64 random(21);
  while (etc.size() < kBlocks * 8) {
    std::uint8_t block[8];
    for (std::uint8_t& byte : block) {
      byte = static_cast<std::uint8_t>(random());
    }
    if (IsUnclampedHalves(block)) {
      etc.insert(etc.end(), block, block + 8);
    }
  }
  for (VulkanTestDevice& vk : devices_) {
    SCOPED_TRACE(vk.name);
    std::unique_ptr<Etc2GpuDecoder> decoder = Decoder(vk);
    ASSERT_NE(decoder, nullptr);
    const std::vector<std::uint8_t> bc =
        Transcode(vk, *decoder, etc, TightJob(EtcFormat::kEtc2Rgb8, kBlocks));
    ASSERT_FALSE(bc.empty());
    const Quality q = Measure(EtcFormat::kEtc2Rgb8, etc, bc.data(), kBlocks);
    EXPECT_GE(q.psnr, 42.8);
    EXPECT_LE(q.max_error, 24);
    std::printf("  unclamped halves: %.2f dB, max error %d\n", q.psnr,
                q.max_error);
  }
}

// ETC1 textures shipped in the Roblox APK; skipped when no payload is
// installed.
std::vector<std::filesystem::path> ApkTextures() {
  std::vector<std::filesystem::path> out;
  const char* home = std::getenv("HOME");
  if (home == nullptr) {
    return out;
  }
  const std::filesystem::path payloads =
      std::filesystem::path(home) / ".local/share/mocktail/payloads";
  std::error_code error;
  for (const auto& payload :
       std::filesystem::directory_iterator(payloads, error)) {
    const std::filesystem::path textures =
        payload.path() / "assets/android/textures";
    for (auto it = std::filesystem::recursive_directory_iterator(textures, error);
         !error && it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
      if (it->path().extension() == ".ktx") {
        out.push_back(it->path());
      }
    }
    if (!out.empty()) {
      break;
    }
  }
  return out;
}

// Level-0 ETC1 blocks of a KTX 1 file, or nothing for any other format.
std::vector<std::uint8_t> KtxEtc1Level0(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  const auto u32 = [&](std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
  };
  constexpr std::uint32_t kEtc1Rgb8 = 0x8D64;
  if (bytes.size() < 68 || u32(12) != 0x04030201 || u32(28) != kEtc1Rgb8) {
    return {};
  }
  const std::size_t data = 64 + u32(60);
  if (bytes.size() < data + 4) {
    return {};
  }
  const std::size_t size = u32(data);
  if (bytes.size() < data + 4 + size) {
    return {};
  }
  return {bytes.begin() + data + 4, bytes.begin() + data + 4 + size};
}

// Measured 47.65 dB. bc7enc at its highest quality scores 48.3 dB on these,
// and the best BC1 encode 35.5 dB.
TEST_F(Etc2GpuTranscodeTest, ApkTexturesKeepTheirDetail) {
  std::vector<std::uint8_t> etc;
  for (const std::filesystem::path& path : ApkTextures()) {
    const std::vector<std::uint8_t> blocks = KtxEtc1Level0(path);
    etc.insert(etc.end(), blocks.begin(), blocks.end());
  }
  if (etc.empty()) {
    GTEST_SKIP() << "no Roblox payload with ETC1 textures installed";
  }
  const std::uint32_t blocks = static_cast<std::uint32_t>(etc.size() / 8);
  for (VulkanTestDevice& vk : devices_) {
    SCOPED_TRACE(vk.name);
    std::unique_ptr<Etc2GpuDecoder> decoder = Decoder(vk);
    ASSERT_NE(decoder, nullptr);
    const std::vector<std::uint8_t> bc =
        Transcode(vk, *decoder, etc, TightJob(EtcFormat::kEtc2Rgb8, blocks));
    ASSERT_FALSE(bc.empty());
    const Quality q = Measure(EtcFormat::kEtc2Rgb8, etc, bc.data(), blocks);
    EXPECT_GE(q.psnr, 47.5);
    std::printf("  %u APK blocks: %.2f dB, max error %d\n", blocks, q.psnr,
                q.max_error);
  }
}

TEST_F(Etc2GpuTranscodeTest, StridesLayersAndOffsetsMatchATightJob) {
  for (VulkanTestDevice& vk : devices_) {
    SCOPED_TRACE(vk.name);
    std::unique_ptr<Etc2GpuDecoder> decoder = Decoder(vk);
    ASSERT_NE(decoder, nullptr);
    for (EtcFormat format : kAllFormats) {
      SCOPED_TRACE(static_cast<int>(format));
      const std::size_t etc_bytes = EtcBlockBytes(format);
      const std::size_t bc_bytes = BcBlockBytes(BcFormatFor(format));
      Etc2GpuJob job;
      job.format = format;
      job.blocks_wide = 7;
      job.blocks_high = 5;
      job.layers = 3;
      job.source_offset = 64;
      job.source_row_blocks = 9;
      job.source_layer_blocks = 9 * 5 + 2;
      job.destination_offset = 256;
      job.transcode = true;
      const std::vector<std::uint8_t> source = RandomBlocks(
          format, 64 / etc_bytes + job.source_layer_blocks * job.layers,
          31 + static_cast<int>(format));
      const std::vector<std::uint8_t> strided =
          Transcode(vk, *decoder, source, job);
      ASSERT_FALSE(strided.empty());

      std::vector<std::uint8_t> tight;
      for (std::uint32_t layer = 0; layer < job.layers; ++layer) {
        for (std::uint32_t row = 0; row < job.blocks_high; ++row) {
          const std::size_t at =
              job.source_offset + (layer * job.source_layer_blocks +
                                   row * job.source_row_blocks) *
                                      etc_bytes;
          tight.insert(tight.end(), source.begin() + at,
                       source.begin() + at + job.blocks_wide * etc_bytes);
        }
      }
      const std::uint32_t count = job.blocks_wide * job.blocks_high * job.layers;
      const std::vector<std::uint8_t> expected =
          Transcode(vk, *decoder, tight, TightJob(format, count));
      ASSERT_EQ(expected.size(), count * bc_bytes);
      EXPECT_EQ(0, std::memcmp(strided.data() + job.destination_offset,
                               expected.data(), expected.size()));
    }
  }
}

struct HardwareFormat {
  EtcFormat etc;
  VkFormat bc;
  VkFormat blit_target;
  int tolerance;
};

// A nearest blit from the BC image is the device's own decode. Returns
// nothing when the device cannot blit these formats.
std::vector<std::uint8_t> HardwareDecode(const VulkanTestDevice& vk,
                                         const HardwareFormat& format,
                                         const std::vector<std::uint8_t>& bc,
                                         std::uint32_t blocks_wide,
                                         std::uint32_t blocks_high,
                                         std::size_t texel_bytes) {
  VkFormatProperties bc_properties{};
  VkFormatProperties target_properties{};
  const auto properties =
      vk.Instance<PFN_vkGetPhysicalDeviceFormatProperties>(
          "vkGetPhysicalDeviceFormatProperties");
  properties(vk.physical_device, format.bc, &bc_properties);
  properties(vk.physical_device, format.blit_target, &target_properties);
  if ((bc_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ==
          0 ||
      (target_properties.optimalTilingFeatures &
       VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0) {
    return {};
  }
  const std::uint32_t width = blocks_wide * 4;
  const std::uint32_t height = blocks_high * 4;
  const auto create_image = vk.Device<PFN_vkCreateImage>("vkCreateImage");
  const auto requirements_of = vk.Device<PFN_vkGetImageMemoryRequirements>(
      "vkGetImageMemoryRequirements");
  const auto allocate = vk.Device<PFN_vkAllocateMemory>("vkAllocateMemory");
  const auto bind = vk.Device<PFN_vkBindImageMemory>("vkBindImageMemory");
  VkImage images[2] = {};
  VkDeviceMemory memories[2] = {};
  for (int i = 0; i < 2; ++i) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = i == 0 ? format.bc : format.blit_target;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    EXPECT_EQ(create_image(vk.device, &info, nullptr, &images[i]), VK_SUCCESS);
    VkMemoryRequirements requirements{};
    requirements_of(vk.device, images[i], &requirements);
    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex =
        FindMemoryType(vk, requirements.memoryTypeBits, 0);
    EXPECT_EQ(allocate(vk.device, &allocate_info, nullptr, &memories[i]),
              VK_SUCCESS);
    bind(vk.device, images[i], memories[i], 0);
  }
  HostBuffer upload = CreateHostBuffer(vk, bc.size(),
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  const VkDeviceSize readback_bytes = VkDeviceSize{width} * height * texel_bytes;
  HostBuffer readback = CreateHostBuffer(vk, readback_bytes,
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  BindAndMap(vk, &upload);
  BindAndMap(vk, &readback);
  std::memcpy(upload.mapped, bc.data(), bc.size());

  TestCommands commands = BeginTestCommands(vk);
  const auto barrier = vk.Device<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
  const auto transition = [&](VkImage image, VkImageLayout from,
                              VkImageLayout to, VkAccessFlags source_access,
                              VkAccessFlags destination_access) {
    VkImageMemoryBarrier image_barrier{};
    image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_barrier.srcAccessMask = source_access;
    image_barrier.dstAccessMask = destination_access;
    image_barrier.oldLayout = from;
    image_barrier.newLayout = to;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = image;
    image_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier(commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &image_barrier);
  };
  transition(images[0], VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
             VK_ACCESS_TRANSFER_WRITE_BIT);
  transition(images[1], VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
             VK_ACCESS_TRANSFER_WRITE_BIT);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {width, height, 1};
  vk.Device<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(
      commands.buffer, upload.buffer, images[0],
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  transition(images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_ACCESS_TRANSFER_READ_BIT);
  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstSubresource = blit.srcSubresource;
  blit.srcOffsets[1] = {static_cast<std::int32_t>(width),
                        static_cast<std::int32_t>(height), 1};
  blit.dstOffsets[1] = blit.srcOffsets[1];
  vk.Device<PFN_vkCmdBlitImage>("vkCmdBlitImage")(
      commands.buffer, images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
      VK_FILTER_NEAREST);
  transition(images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_ACCESS_TRANSFER_READ_BIT);
  vk.Device<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
      commands.buffer, images[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      readback.buffer, 1, &copy);
  HostReadBarrier(vk, commands.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT);
  vk.Device<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands.buffer);
  std::vector<std::uint8_t> out;
  if (SubmitAndWait(vk, {commands.buffer})) {
    const auto* bytes = static_cast<const std::uint8_t*>(readback.mapped);
    out.assign(bytes, bytes + readback_bytes);
  }
  DestroyTestCommands(vk, commands);
  DestroyHostBuffer(vk, upload);
  DestroyHostBuffer(vk, readback);
  for (int i = 0; i < 2; ++i) {
    vk.Device<PFN_vkDestroyImage>("vkDestroyImage")(vk.device, images[i],
                                                    nullptr);
    vk.Device<PFN_vkFreeMemory>("vkFreeMemory")(vk.device, memories[i],
                                                nullptr);
  }
  return out;
}

// BC7 decodes exactly. BC4 interpolation precision is up to the device: the
// RTX 4090 lands within 3 steps of 8-bit and llvmpipe within 1.3, so BC4
// and BC5 allow 4 steps.
TEST_F(Etc2GpuTranscodeTest, HardwareDecodesOutputLikeTheCpuDecoder) {
  constexpr HardwareFormat kFormats[] = {
      {EtcFormat::kEtc2Rgb8, VK_FORMAT_BC7_UNORM_BLOCK,
       VK_FORMAT_R8G8B8A8_UNORM, 0},
      {EtcFormat::kEtc2Rgba8, VK_FORMAT_BC7_UNORM_BLOCK,
       VK_FORMAT_R8G8B8A8_UNORM, 0},
      {EtcFormat::kEtc2Rgb8A1, VK_FORMAT_BC7_UNORM_BLOCK,
       VK_FORMAT_R8G8B8A8_UNORM, 0},
      {EtcFormat::kEacR11, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R32_SFLOAT,
       4 * 257},
      {EtcFormat::kEacR11Signed, VK_FORMAT_BC4_SNORM_BLOCK,
       VK_FORMAT_R32_SFLOAT, 4 * 258},
      {EtcFormat::kEacRg11, VK_FORMAT_BC5_UNORM_BLOCK,
       VK_FORMAT_R32G32_SFLOAT, 4 * 257},
      {EtcFormat::kEacRg11Signed, VK_FORMAT_BC5_SNORM_BLOCK,
       VK_FORMAT_R32G32_SFLOAT, 4 * 258}};
  constexpr std::uint32_t kWide = 32;
  constexpr std::uint32_t kHigh = 16;
  for (VulkanTestDevice& vk : devices_) {
    SCOPED_TRACE(vk.name);
    std::unique_ptr<Etc2GpuDecoder> decoder = Decoder(vk);
    ASSERT_NE(decoder, nullptr);
    for (const HardwareFormat& format : kFormats) {
      SCOPED_TRACE(static_cast<int>(format.etc));
      const BcFormat bc_format = BcFormatFor(format.etc);
      const std::vector<std::uint8_t> etc =
          RandomBlocks(format.etc, kWide * kHigh, 41 + static_cast<int>(format.etc));
      Etc2GpuJob job = TightJob(format.etc, kWide);
      job.blocks_high = kHigh;
      job.source_layer_blocks = kWide * kHigh;
      const std::vector<std::uint8_t> bc = Transcode(vk, *decoder, etc, job);
      ASSERT_FALSE(bc.empty());
      const bool color = IsColor(format.etc);
      const std::size_t channels = color ? 4 : bc_format == BcFormat::kBc5 ||
                                                       bc_format == BcFormat::kBc5Signed
                                                   ? 2
                                                   : 1;
      const std::size_t texel_bytes = color ? 4 : channels * 4;
      const std::vector<std::uint8_t> hardware =
          HardwareDecode(vk, format, bc, kWide, kHigh, texel_bytes);
      if (hardware.empty()) {
        std::printf("  %s cannot blit format %d; skipped\n", vk.name.c_str(),
                    static_cast<int>(format.bc));
        continue;
      }
      int worst = 0;
      std::vector<std::uint8_t> texels(16 * EtcDecodedTexelBytes(format.etc));
      for (std::uint32_t by = 0; by < kHigh; ++by) {
        for (std::uint32_t bx = 0; bx < kWide; ++bx) {
          const std::size_t block = by * kWide + bx;
          ASSERT_TRUE(DecodeBcBlock(
              bc_format, bc.data() + block * BcBlockBytes(bc_format),
              texels.data()));
          for (int t = 0; t < 16; ++t) {
            const std::size_t x = bx * 4 + t % 4;
            const std::size_t y = by * 4 + t / 4;
            const std::uint8_t* hw =
                hardware.data() + (y * kWide * 4 + x) * texel_bytes;
            const std::vector<int> ours = Channels(format.etc, texels.data(), t);
            for (std::size_t c = 0; c < channels; ++c) {
              int value = 0;
              if (color) {
                value = hw[c];
              } else {
                float f = 0;
                std::memcpy(&f, hw + c * 4, 4);
                value = static_cast<int>(
                    std::lround(f * (IsSigned(format.etc) ? 32767.0 : 65535.0)));
              }
              worst = std::max(worst, std::abs(value - ours[c]));
            }
          }
        }
      }
      EXPECT_LE(worst, format.tolerance);
    }
  }
}

TEST_F(Etc2GpuTranscodeTest, SelfTestCoversTranscoding) {
  for (VulkanTestDevice& vk : devices_) {
    SCOPED_TRACE(vk.name);
    std::unique_ptr<Etc2GpuDecoder> decoder = Decoder(vk);
    ASSERT_NE(decoder, nullptr);
    std::string error;
    EXPECT_TRUE(decoder->SelfTest(vk.queue, vk.family, vk.memory, &error))
        << error;
    EXPECT_TRUE(decoder->CanTranscode());
  }
}

}
}
