// Decodes small clips through the libmediandk shim the way Roblox's
// VideoFrame does: byte-buffer mode, I420 output read via stride,
// slice-height and crop keys, and 16-bit PCM for the audio track.

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <sys/types.h>

extern "C" {
#include <libavcodec/version_major.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

struct AMediaCodec;
struct AMediaFormat;
struct AMediaCodecBufferInfo {
  int32_t offset;
  int32_t size;
  int64_t presentationTimeUs;
  uint32_t flags;
};

extern "C" {
AMediaFormat* AMediaFormat_new();
void AMediaFormat_delete(AMediaFormat*);
bool AMediaFormat_getInt32(AMediaFormat*, const char*, int32_t*);
void AMediaFormat_setInt32(AMediaFormat*, const char*, int32_t);
void AMediaFormat_setString(AMediaFormat*, const char*, const char*);
void AMediaFormat_setBuffer(AMediaFormat*, const char*, const void*, size_t);
AMediaCodec* AMediaCodec_createDecoderByType(const char*);
int32_t AMediaCodec_delete(AMediaCodec*);
int32_t AMediaCodec_configure(AMediaCodec*, const AMediaFormat*, void*, void*,
                              uint32_t);
int32_t AMediaCodec_start(AMediaCodec*);
int32_t AMediaCodec_stop(AMediaCodec*);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t);
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec*, size_t, size_t*);
int32_t AMediaCodec_queueInputBuffer(AMediaCodec*, size_t, off_t, size_t,
                                     uint64_t, uint32_t);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec*, AMediaCodecBufferInfo*,
                                        int64_t);
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec*, size_t, size_t*);
AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec*);
int32_t AMediaCodec_releaseOutputBuffer(AMediaCodec*, size_t, bool);
}

namespace {

constexpr ssize_t kTryAgainLater = -1;
constexpr ssize_t kFormatChanged = -2;
constexpr uint32_t kCodecConfig = 2;
constexpr uint32_t kEndOfStream = 4;
constexpr int32_t kPcm16Bit = 2;
constexpr int32_t kColorFormatYuv420Planar = 19;

std::vector<uint8_t> ReadFixture(const std::string& name) {
  std::ifstream in(std::string(MOCKTAIL_MEDIA_FIXTURE_DIR) + "/" + name,
                   std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Splits an Annex-B stream at access unit delimiters (NAL type 9).
std::vector<std::vector<uint8_t>> H264AccessUnits(
    const std::vector<uint8_t>& stream) {
  std::vector<size_t> starts;
  for (size_t i = 0; i + 4 < stream.size(); ++i) {
    if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 0 &&
        stream[i + 3] == 1 && (stream[i + 4] & 0x1f) == 9) {
      starts.push_back(i);
    }
  }
  starts.push_back(stream.size());
  std::vector<std::vector<uint8_t>> units;
  for (size_t i = 0; i + 1 < starts.size(); ++i) {
    units.emplace_back(stream.begin() + starts[i],
                       stream.begin() + starts[i + 1]);
  }
  return units;
}

// Moves SPS and PPS NAL units out of the access units into csd-0 and csd-1,
// as an MP4 demuxer hands them over.
std::vector<std::vector<uint8_t>> SplitH264Csd(
    std::vector<std::vector<uint8_t>>& units) {
  std::vector<std::vector<uint8_t>> csd(2);
  for (std::vector<uint8_t>& unit : units) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 4 < unit.size(); ++i) {
      if (unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 0 &&
          unit[i + 3] == 1) {
        starts.push_back(i);
      }
    }
    starts.push_back(unit.size());
    std::vector<uint8_t> rest;
    for (size_t i = 0; i + 1 < starts.size(); ++i) {
      const auto begin = unit.begin() + starts[i];
      const auto end = unit.begin() + starts[i + 1];
      const int type = unit[starts[i] + 4] & 0x1f;
      if (type == 7 || type == 8) {
        csd[type - 7].insert(csd[type - 7].end(), begin, end);
      } else {
        rest.insert(rest.end(), begin, end);
      }
    }
    unit = rest;
  }
  return csd;
}

std::vector<std::vector<uint8_t>> IvfFrames(const std::vector<uint8_t>& ivf) {
  std::vector<std::vector<uint8_t>> frames;
  size_t at = 32;
  while (at + 12 <= ivf.size()) {
    uint32_t size = 0;
    std::memcpy(&size, ivf.data() + at, 4);
    at += 12;
    if (at + size > ivf.size()) break;
    frames.emplace_back(ivf.begin() + at, ivf.begin() + at + size);
    at += size;
  }
  return frames;
}

// Splits ADTS into raw AAC frames and builds the AudioSpecificConfig an MP4
// demuxer would hand over as csd-0.
std::vector<std::vector<uint8_t>> AdtsFrames(const std::vector<uint8_t>& adts,
                                             std::vector<uint8_t>* asc) {
  std::vector<std::vector<uint8_t>> frames;
  size_t at = 0;
  while (at + 7 <= adts.size() && adts[at] == 0xff &&
         (adts[at + 1] & 0xf0) == 0xf0) {
    const uint8_t* h = adts.data() + at;
    const size_t header = (h[1] & 1) ? 7 : 9;
    const size_t length =
        (size_t(h[3] & 3) << 11) | (size_t(h[4]) << 3) | (h[5] >> 5);
    if (length < header || at + length > adts.size()) break;
    if (asc->empty()) {
      const int object = (h[2] >> 6) + 1;
      const int rate = (h[2] >> 2) & 0xf;
      const int channels = ((h[2] & 1) << 2) | (h[3] >> 6);
      *asc = {uint8_t(object << 3 | rate >> 1),
              uint8_t((rate & 1) << 7 | channels << 3)};
    }
    frames.emplace_back(adts.begin() + at + header, adts.begin() + at + length);
    at += length;
  }
  return frames;
}

struct DecodedAudio {
  int32_t sample_rate = 0;
  int32_t channels = 0;
  int32_t pcm_encoding = 0;
  int format_changes = 0;
  std::vector<int16_t> pcm;  // interleaved
  bool sizes_whole_frames = true;
  bool saw_end = false;
};

// csd_in_format: csd-0 on the format, else queued as a codec-config buffer.
DecodedAudio DecodeAac(bool csd_in_format) {
  DecodedAudio out;
  std::vector<uint8_t> asc;
  const auto frames = AdtsFrames(ReadFixture("sine_l_48k.aac"), &asc);
  EXPECT_GE(frames.size(), 8u);
  AMediaCodec* codec = AMediaCodec_createDecoderByType("audio/mp4a-latm");
  EXPECT_NE(codec, nullptr);
  if (codec == nullptr) return out;
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, "mime", "audio/mp4a-latm");
  AMediaFormat_setInt32(format, "sample-rate", 48000);
  AMediaFormat_setInt32(format, "channel-count", 2);
  if (csd_in_format) {
    AMediaFormat_setBuffer(format, "csd-0", asc.data(), asc.size());
  }
  EXPECT_EQ(AMediaCodec_configure(codec, format, nullptr, nullptr, 0), 0);
  AMediaFormat_delete(format);
  EXPECT_EQ(AMediaCodec_start(codec), 0);

  std::vector<std::vector<uint8_t>> in;
  std::vector<uint32_t> flags;
  if (!csd_in_format) {
    in.push_back(asc);
    flags.push_back(kCodecConfig);
  }
  for (const auto& frame : frames) {
    in.push_back(frame);
    flags.push_back(0);
  }
  size_t next = 0;
  bool sent_end = false;
  for (int spins = 0; spins < 1000 && !out.saw_end; ++spins) {
    if (!sent_end) {
      const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 0);
      if (index >= 0) {
        uint8_t* buffer = AMediaCodec_getInputBuffer(codec, index, nullptr);
        const size_t size = next < in.size() ? in[next].size() : 0;
        if (size > 0) std::memcpy(buffer, in[next].data(), size);
        const uint32_t f = next < in.size() ? flags[next] : kEndOfStream;
        EXPECT_EQ(AMediaCodec_queueInputBuffer(codec, index, 0, size,
                                               next * 21333, f),
                  0);
        sent_end = next == in.size();
        ++next;
      }
    }
    AMediaCodecBufferInfo info{};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 1000);
    if (index == kFormatChanged) {
      AMediaFormat* f = AMediaCodec_getOutputFormat(codec);
      AMediaFormat_getInt32(f, "sample-rate", &out.sample_rate);
      AMediaFormat_getInt32(f, "channel-count", &out.channels);
      AMediaFormat_getInt32(f, "pcm-encoding", &out.pcm_encoding);
      AMediaFormat_delete(f);
      ++out.format_changes;
      continue;
    }
    if (index == kTryAgainLater || index < 0) continue;
    size_t size = 0;
    const uint8_t* data = AMediaCodec_getOutputBuffer(codec, index, &size);
    if (info.size > 0 && data != nullptr) {
      if (out.channels == 0 || info.size % (2 * out.channels) != 0) {
        out.sizes_whole_frames = false;
      }
      const size_t at = out.pcm.size();
      out.pcm.resize(at + info.size / 2);
      std::memcpy(out.pcm.data() + at, data + info.offset, info.size / 2 * 2);
    }
    out.saw_end = (info.flags & kEndOfStream) != 0;
    AMediaCodec_releaseOutputBuffer(codec, index, false);
  }
  AMediaCodec_stop(codec);
  AMediaCodec_delete(codec);
  return out;
}

// The fixture is a 0.25 s, 1 kHz sine on the left channel and silence on
// the right, so a swapped or planar layout shows up as signal on the right.
void ExpectLeftSine(const DecodedAudio& d) {
  EXPECT_EQ(d.format_changes, 1);
  EXPECT_EQ(d.sample_rate, 48000);
  EXPECT_EQ(d.channels, 2);
  EXPECT_EQ(d.pcm_encoding, kPcm16Bit);
  EXPECT_TRUE(d.sizes_whole_frames);
  EXPECT_TRUE(d.saw_end);
  ASSERT_GE(d.pcm.size(), size_t(2 * 48000 / 5));
  int left_peak = 0, right_peak = 0;
  for (size_t i = 0; i + 1 < d.pcm.size(); i += 2) {
    left_peak = std::max(left_peak, std::abs(int(d.pcm[i])));
    right_peak = std::max(right_peak, std::abs(int(d.pcm[i + 1])));
  }
  EXPECT_GT(left_peak, 3000);
  EXPECT_LT(right_peak, 300);
}

struct Decoded {
  int32_t color_format = 0;
  int32_t width = 0;
  int32_t height = 0;
  int32_t stride = 0;
  int32_t slice_height = 0;
  int32_t crop_right = 0;
  int32_t crop_bottom = 0;
  int frames = 0;
  // Y, U and V of the centre texel of each frame.
  std::vector<int> y, u, v;
  bool saw_end = false;
};

Decoded Decode(const char* mime, const std::vector<std::vector<uint8_t>>& in,
               const std::vector<std::vector<uint8_t>>& csd = {}) {
  Decoded out;
  AMediaCodec* codec = AMediaCodec_createDecoderByType(mime);
  EXPECT_NE(codec, nullptr) << mime;
  if (codec == nullptr) return out;
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, "mime", mime);
  AMediaFormat_setInt32(format, "width", 64);
  AMediaFormat_setInt32(format, "height", 48);
  for (size_t i = 0; i < csd.size(); ++i) {
    const std::string key = "csd-" + std::to_string(i);
    AMediaFormat_setBuffer(format, key.c_str(), csd[i].data(), csd[i].size());
  }
  EXPECT_EQ(AMediaCodec_configure(codec, format, nullptr, nullptr, 0), 0);
  AMediaFormat_delete(format);
  EXPECT_EQ(AMediaCodec_start(codec), 0);

  size_t next = 0;
  bool sent_end = false;
  for (int spins = 0; spins < 1000 && !out.saw_end; ++spins) {
    if (!sent_end) {
      const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 0);
      if (index >= 0) {
        size_t capacity = 0;
        uint8_t* buffer = AMediaCodec_getInputBuffer(codec, index, &capacity);
        EXPECT_NE(buffer, nullptr);
        if (next < in.size()) {
          EXPECT_GE(capacity, in[next].size());
          std::memcpy(buffer, in[next].data(), in[next].size());
          EXPECT_EQ(AMediaCodec_queueInputBuffer(codec, index, 0,
                                                 in[next].size(),
                                                 next * 33333, 0),
                    0);
          ++next;
        } else {
          EXPECT_EQ(AMediaCodec_queueInputBuffer(codec, index, 0, 0, 0,
                                                 kEndOfStream),
                    0);
          sent_end = true;
        }
      }
    }
    AMediaCodecBufferInfo info{};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 1000);
    if (index == kFormatChanged) {
      AMediaFormat* f = AMediaCodec_getOutputFormat(codec);
      AMediaFormat_getInt32(f, "color-format", &out.color_format);
      AMediaFormat_getInt32(f, "width", &out.width);
      AMediaFormat_getInt32(f, "height", &out.height);
      AMediaFormat_getInt32(f, "stride", &out.stride);
      AMediaFormat_getInt32(f, "slice-height", &out.slice_height);
      AMediaFormat_getInt32(f, "crop-right", &out.crop_right);
      AMediaFormat_getInt32(f, "crop-bottom", &out.crop_bottom);
      AMediaFormat_delete(f);
      continue;
    }
    if (index == kTryAgainLater || index < 0) continue;
    size_t size = 0;
    const uint8_t* data = AMediaCodec_getOutputBuffer(codec, index, &size);
    if (info.size > 0 && data != nullptr && out.stride > 0) {
      const uint8_t* frame = data + info.offset;
      const size_t luma = size_t(out.stride) * out.slice_height;
      const size_t chroma_stride = size_t(out.stride) / 2;
      const size_t x = 32, yy = 24;
      out.y.push_back(frame[yy * out.stride + x]);
      out.u.push_back(frame[luma + (yy / 2) * chroma_stride + x / 2]);
      out.v.push_back(
          frame[luma + luma / 4 + (yy / 2) * chroma_stride + x / 2]);
      ++out.frames;
    }
    out.saw_end = (info.flags & kEndOfStream) != 0;
    AMediaCodec_releaseOutputBuffer(codec, index, false);
  }
  AMediaCodec_stop(codec);
  AMediaCodec_delete(codec);
  return out;
}

void ExpectSolid(const Decoded& d, int y, int u, int v) {
  EXPECT_EQ(d.color_format, kColorFormatYuv420Planar);
  EXPECT_EQ(d.width, 64);
  EXPECT_EQ(d.height, 48);
  EXPECT_GE(d.stride, 64);
  EXPECT_GE(d.slice_height, 48);
  EXPECT_EQ(d.crop_right, 63);
  EXPECT_EQ(d.crop_bottom, 47);
  EXPECT_EQ(d.frames, 4);
  EXPECT_TRUE(d.saw_end);
  for (int i = 0; i < d.frames; ++i) {
    EXPECT_NEAR(d.y[i], y, 2) << i;
    EXPECT_NEAR(d.u[i], u, 2) << i;
    EXPECT_NEAR(d.v[i], v, 2) << i;
  }
}

// Linking FFmpeg would keep the game from starting where that exact
// libavcodec is missing, so it is opened on the first decoder instead.
TEST(MediaNdkDecoderTest, LoadsFfmpegOnlyWhenADecoderIsCreated) {
  const std::string soname =
      "libavcodec.so." + std::to_string(LIBAVCODEC_VERSION_MAJOR);
  EXPECT_EQ(dlopen(soname.c_str(), RTLD_NOW | RTLD_NOLOAD), nullptr);
  AMediaCodec* codec = AMediaCodec_createDecoderByType("video/avc");
  ASSERT_NE(codec, nullptr);
  void* loaded = dlopen(soname.c_str(), RTLD_NOW | RTLD_NOLOAD);
  EXPECT_NE(loaded, nullptr);
  if (loaded != nullptr) dlclose(loaded);
  AMediaCodec_delete(codec);
}

TEST(MediaNdkDecoderTest, DecodesH264ToI420) {
  const auto units = H264AccessUnits(ReadFixture("red_64x48.h264"));
  ASSERT_EQ(units.size(), 4u);
  ExpectSolid(Decode("video/avc", units), 81, 90, 240);
}

TEST(MediaNdkDecoderTest, DecodesH264WithCsdFromFormat) {
  auto units = H264AccessUnits(ReadFixture("red_64x48.h264"));
  ASSERT_EQ(units.size(), 4u);
  const auto csd = SplitH264Csd(units);
  ASSERT_FALSE(csd[0].empty());
  ASSERT_FALSE(csd[1].empty());
  ExpectSolid(Decode("video/avc", units, csd), 81, 90, 240);
}

// Frames already decoded at the old size are handed out under the old format;
// the new format is reported just before the first frame at the new size.
TEST(MediaNdkDecoderTest, ReportsResolutionChangeInOrder) {
  auto in = IvfFrames(ReadFixture("blue_64x48.ivf"));
  ASSERT_GE(in.size(), 2u);
  in.resize(2);
  const auto small = IvfFrames(ReadFixture("green_32x24.ivf"));
  ASSERT_FALSE(small.empty());
  in.push_back(small[0]);

  AMediaCodec* codec = AMediaCodec_createDecoderByType("video/x-vnd.on2.vp9");
  ASSERT_NE(codec, nullptr);
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setInt32(format, "width", 64);
  AMediaFormat_setInt32(format, "height", 48);
  ASSERT_EQ(AMediaCodec_configure(codec, format, nullptr, nullptr, 0), 0);
  AMediaFormat_delete(format);
  ASSERT_EQ(AMediaCodec_start(codec), 0);
  for (size_t i = 0; i <= in.size(); ++i) {
    const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 100000);
    ASSERT_GE(index, 0) << i;
    uint8_t* buffer = AMediaCodec_getInputBuffer(codec, index, nullptr);
    const size_t size = i < in.size() ? in[i].size() : 0;
    if (size > 0) std::memcpy(buffer, in[i].data(), size);
    ASSERT_EQ(AMediaCodec_queueInputBuffer(codec, index, 0, size, i * 33333,
                                           i < in.size() ? 0 : kEndOfStream),
              0);
  }

  std::vector<int32_t> widths;
  int32_t width = 0, height = 0, stride = 0, slice_height = 0;
  int frames = 0;
  for (int spins = 0; spins < 100; ++spins) {
    AMediaCodecBufferInfo info{};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 1000);
    if (index == kFormatChanged) {
      AMediaFormat* f = AMediaCodec_getOutputFormat(codec);
      AMediaFormat_getInt32(f, "width", &width);
      AMediaFormat_getInt32(f, "height", &height);
      AMediaFormat_getInt32(f, "stride", &stride);
      AMediaFormat_getInt32(f, "slice-height", &slice_height);
      AMediaFormat_delete(f);
      widths.push_back(width);
      continue;
    }
    if (index < 0) continue;
    if (info.size > 0) {
      EXPECT_EQ(size_t(info.size), size_t(stride) * slice_height * 3 / 2)
          << "frame " << frames << " under " << width << "x" << height;
      ++frames;
    }
    const bool end = (info.flags & kEndOfStream) != 0;
    AMediaCodec_releaseOutputBuffer(codec, index, false);
    if (end) break;
  }
  EXPECT_EQ(frames, 3);
  EXPECT_EQ(widths, (std::vector<int32_t>{64, 32}));
  AMediaCodec_stop(codec);
  AMediaCodec_delete(codec);
}

TEST(MediaNdkDecoderTest, DecodesVp9ToI420) {
  const auto frames = IvfFrames(ReadFixture("blue_64x48.ivf"));
  ASSERT_EQ(frames.size(), 4u);
  ExpectSolid(Decode("video/x-vnd.on2.vp9", frames), 41, 240, 110);
}

// VideoFrame drops the whole video when its audio decoder can't be made.
TEST(MediaNdkDecoderTest, DecodesAacToInterleavedPcm16) {
  ExpectLeftSine(DecodeAac(/*csd_in_format=*/true));
}

TEST(MediaNdkDecoderTest, DecodesAacWithCsdAsCodecConfigBuffer) {
  ExpectLeftSine(DecodeAac(/*csd_in_format=*/false));
}

// MediaCodec's output format holds the configured values until the first
// INFO_OUTPUT_FORMAT_CHANGED.
TEST(MediaNdkDecoderTest, ReportsConfiguredAudioFormatBeforeOutput) {
  AMediaCodec* codec = AMediaCodec_createDecoderByType("audio/mp4a-latm");
  ASSERT_NE(codec, nullptr);
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setInt32(format, "sample-rate", 48000);
  AMediaFormat_setInt32(format, "channel-count", 2);
  ASSERT_EQ(AMediaCodec_configure(codec, format, nullptr, nullptr, 0), 0);
  AMediaFormat_delete(format);
  ASSERT_EQ(AMediaCodec_start(codec), 0);
  AMediaFormat* out = AMediaCodec_getOutputFormat(codec);
  int32_t rate = 0, channels = 0;
  AMediaFormat_getInt32(out, "sample-rate", &rate);
  AMediaFormat_getInt32(out, "channel-count", &channels);
  AMediaFormat_delete(out);
  EXPECT_EQ(rate, 48000);
  EXPECT_EQ(channels, 2);
  AMediaCodec_stop(codec);
  AMediaCodec_delete(codec);
}

TEST(MediaNdkDecoderTest, RejectsUnknownMime) {
  EXPECT_EQ(AMediaCodec_createDecoderByType("video/unknown"), nullptr);
}

}
