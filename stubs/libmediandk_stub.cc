// libmediandk for Roblox. Video decoders run on FFmpeg in byte-buffer mode
// and hand back I420 (COLOR_FormatYUV420Planar), the layout Roblox's
// VideoFrame reads through the stride, slice-height and crop keys. Encoders
// are not provided.

#include <dlfcn.h>
#include <sys/types.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if MOCKTAIL_MEDIANDK_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}
#endif

using media_status_t = int32_t;
static constexpr media_status_t AMEDIA_OK = 0;
static constexpr media_status_t AMEDIA_ERROR_UNKNOWN = -10000;
static constexpr media_status_t AMEDIA_ERROR_INVALID_PARAMETER = -10007;

struct AMediaFormat {
  std::map<std::string, int32_t> ints;
  std::map<std::string, float> floats;
  std::map<std::string, std::string> strings;
  std::map<std::string, std::vector<uint8_t>> buffers;
  std::string text;
};

struct AMediaCodecBufferInfo {
  int32_t offset;
  int32_t size;
  int64_t presentationTimeUs;
  uint32_t flags;
};

namespace {

constexpr ssize_t kTryAgainLater = -1;
constexpr ssize_t kOutputFormatChanged = -2;
constexpr uint32_t kFlagCodecConfig = 2;
constexpr uint32_t kFlagEndOfStream = 4;
constexpr int32_t kColorFormatYuv420Planar = 19;
constexpr int kInputBuffers = 4;
// Decoded frames waiting for Roblox; input is held back past this.
constexpr size_t kMaxReadyOutputs = 4;
constexpr size_t kMinInputBytes = size_t{4} << 20;

}

#if MOCKTAIL_MEDIANDK_FFMPEG

namespace {

// FFmpeg is opened on the first decoder, not linked: Roblox imports these
// functions, so a missing or different libavcodec must cost only video, not
// the whole game. The sonames come from the headers built against, which
// fixes the struct layouts this file reads.
struct Ffmpeg {
  decltype(&avcodec_find_decoder) find_decoder;
  decltype(&avcodec_alloc_context3) alloc_context;
  decltype(&avcodec_free_context) free_context;
  decltype(&avcodec_open2) open;
  decltype(&avcodec_send_packet) send_packet;
  decltype(&avcodec_receive_frame) receive_frame;
  decltype(&avcodec_flush_buffers) flush_buffers;
  decltype(&av_packet_alloc) packet_alloc;
  decltype(&av_packet_free) packet_free;
  decltype(&av_packet_unref) packet_unref;
  decltype(&av_new_packet) new_packet;
  decltype(&av_frame_alloc) frame_alloc;
  decltype(&av_frame_free) frame_free;
  decltype(&av_frame_unref) frame_unref;
  decltype(&av_get_pix_fmt_name) pix_fmt_name;
};

const Ffmpeg* LoadFfmpeg() {
  static const Ffmpeg* loaded = [] () -> const Ffmpeg* {
    const std::string avcodec_name =
        "libavcodec.so." + std::to_string(LIBAVCODEC_VERSION_MAJOR);
    const std::string avutil_name =
        "libavutil.so." + std::to_string(LIBAVUTIL_VERSION_MAJOR);
    void* avutil = dlopen(avutil_name.c_str(), RTLD_NOW | RTLD_LOCAL);
    void* avcodec = dlopen(avcodec_name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (avutil == nullptr || avcodec == nullptr) {
      std::fprintf(stderr,
                   "  [media] video needs %s and %s (FFmpeg): %s\n",
                   avcodec_name.c_str(), avutil_name.c_str(), dlerror());
      return nullptr;
    }
    static Ffmpeg api;
    bool ok = true;
    const auto bind = [&](void* library, const char* name, auto* slot) {
      *slot = reinterpret_cast<std::remove_pointer_t<decltype(slot)>>(
          dlsym(library, name));
      ok = ok && *slot != nullptr;
    };
    bind(avcodec, "avcodec_find_decoder", &api.find_decoder);
    bind(avcodec, "avcodec_alloc_context3", &api.alloc_context);
    bind(avcodec, "avcodec_free_context", &api.free_context);
    bind(avcodec, "avcodec_open2", &api.open);
    bind(avcodec, "avcodec_send_packet", &api.send_packet);
    bind(avcodec, "avcodec_receive_frame", &api.receive_frame);
    bind(avcodec, "avcodec_flush_buffers", &api.flush_buffers);
    bind(avcodec, "av_packet_alloc", &api.packet_alloc);
    bind(avcodec, "av_packet_free", &api.packet_free);
    bind(avcodec, "av_packet_unref", &api.packet_unref);
    bind(avcodec, "av_new_packet", &api.new_packet);
    bind(avutil, "av_frame_alloc", &api.frame_alloc);
    bind(avutil, "av_frame_free", &api.frame_free);
    bind(avutil, "av_frame_unref", &api.frame_unref);
    bind(avutil, "av_get_pix_fmt_name", &api.pix_fmt_name);
    if (!ok) {
      std::fprintf(stderr, "  [media] %s is missing functions video needs\n",
                   avcodec_name.c_str());
      return nullptr;
    }
    return &api;
  }();
  return loaded;
}

// Set before the first codec exists; every codec function runs after that.
const Ffmpeg* ff = nullptr;

}

struct AMediaCodec {
  struct Output {
    std::vector<uint8_t> data;
    int64_t pts = 0;
    uint32_t flags = 0;
    int width = 0;
    int height = 0;
    bool in_use = false;
  };

  std::string mime;
  const AVCodec* codec = nullptr;
  AVCodecContext* context = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;

  std::mutex mutex;
  std::condition_variable changed;
  std::vector<uint8_t> inputs[kInputBuffers];
  bool input_free[kInputBuffers] = {};
  std::vector<uint8_t> csd;     // from configure, resent after each flush
  std::vector<uint8_t> config;
  std::vector<Output> outputs;
  std::deque<size_t> ready;
  // A frame whose size differs from the reported one is preceded by
  // INFO_OUTPUT_FORMAT_CHANGED.
  int decoded_width = 0;
  int decoded_height = 0;
  int reported_width = 0;
  int reported_height = 0;
  bool opened = false;
  bool logged_format = false;
  bool draining = false;
};

namespace {

const AVCodec* FindDecoder(const char* mime) {
  if (mime == nullptr) return nullptr;
  const std::string m(mime);
  if (m == "video/avc") return ff->find_decoder(AV_CODEC_ID_H264);
  if (m == "video/hevc") return ff->find_decoder(AV_CODEC_ID_HEVC);
  if (m == "video/x-vnd.on2.vp8") return ff->find_decoder(AV_CODEC_ID_VP8);
  if (m == "video/x-vnd.on2.vp9") return ff->find_decoder(AV_CODEC_ID_VP9);
  return nullptr;
}

// I420 stride and slice height, kept even so the chroma planes divide evenly.
int Even(int value) { return (value + 1) & ~1; }

size_t TakeOutputSlot(AMediaCodec* c) {
  for (size_t i = 0; i < c->outputs.size(); ++i) {
    if (!c->outputs[i].in_use) {
      c->outputs[i].in_use = true;
      return i;
    }
  }
  c->outputs.emplace_back();
  c->outputs.back().in_use = true;
  return c->outputs.size() - 1;
}

// Copies one decoded frame into an I420 output buffer. Caller holds mutex.
void EmitFrame(AMediaCodec* c, const AVFrame* f) {
  const auto format = static_cast<AVPixelFormat>(f->format);
  if (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_YUVJ420P) {
    if (!c->logged_format) {
      const char* name = ff->pix_fmt_name(format);
      std::fprintf(stderr, "  [media] %s frame format %s is not supported\n",
                   c->mime.c_str(), name != nullptr ? name : "unknown");
      c->logged_format = true;
    }
    return;
  }
  if (f->width != c->decoded_width || f->height != c->decoded_height) {
    c->decoded_width = f->width;
    c->decoded_height = f->height;
    std::fprintf(stderr, "  [media] %s decoding %dx%d\n", c->mime.c_str(),
                 f->width, f->height);
  }
  const int stride = Even(f->width);
  const size_t luma = size_t(stride) * Even(f->height);
  const size_t slot = TakeOutputSlot(c);
  AMediaCodec::Output& out = c->outputs[slot];
  out.width = f->width;
  out.height = f->height;
  out.data.assign(luma + luma / 2, 128);
  out.pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp
                                                      : f->pts;
  out.flags = 0;
  uint8_t* dst[3] = {out.data.data(), out.data.data() + luma,
                     out.data.data() + luma + luma / 4};
  const int dst_stride[3] = {stride, stride / 2, stride / 2};
  for (int plane = 0; plane < 3; ++plane) {
    const int w = plane == 0 ? f->width : (f->width + 1) / 2;
    const int h = plane == 0 ? f->height : (f->height + 1) / 2;
    for (int y = 0; y < h; ++y) {
      std::memcpy(dst[plane] + size_t(y) * dst_stride[plane],
                  f->data[plane] + size_t(y) * f->linesize[plane], w);
    }
  }
  c->ready.push_back(slot);
}

void ReceiveFrames(AMediaCodec* c) {
  for (;;) {
    const int result = ff->receive_frame(c->context, c->frame);
    if (result == AVERROR_EOF) {
      const size_t slot = TakeOutputSlot(c);
      c->outputs[slot].data.clear();
      c->outputs[slot].pts = 0;
      c->outputs[slot].width = 0;
      c->outputs[slot].height = 0;
      c->outputs[slot].flags = kFlagEndOfStream;
      c->ready.push_back(slot);
      return;
    }
    if (result < 0) return;
    EmitFrame(c, c->frame);
    ff->frame_unref(c->frame);
  }
}

void Send(AMediaCodec* c, const uint8_t* data, size_t size, int64_t pts) {
  ff->packet_unref(c->packet);
  if (ff->new_packet(c->packet, static_cast<int>(c->config.size() + size)) < 0) {
    return;
  }
  if (!c->config.empty()) {
    std::memcpy(c->packet->data, c->config.data(), c->config.size());
  }
  if (size > 0) std::memcpy(c->packet->data + c->config.size(), data, size);
  c->config.clear();
  c->packet->pts = pts;
  if (ff->send_packet(c->context, c->packet) == AVERROR(EAGAIN)) {
    ReceiveFrames(c);
    ff->send_packet(c->context, c->packet);
  }
  ReceiveFrames(c);
}

void ResetBuffers(AMediaCodec* c) {
  for (bool& free : c->input_free) free = true;
  for (AMediaCodec::Output& out : c->outputs) out.in_use = false;
  c->ready.clear();
  c->config = c->csd;
  c->draining = false;
}

}

extern "C" {

AMediaCodec* AMediaCodec_createDecoderByType(const char* mime) {
  ff = LoadFfmpeg();
  if (ff == nullptr) return nullptr;
  const AVCodec* decoder = FindDecoder(mime);
  if (decoder == nullptr) {
    std::fprintf(stderr, "  [media] no decoder for %s\n",
                 mime != nullptr ? mime : "(null)");
    return nullptr;
  }
  auto* c = new AMediaCodec;
  c->mime = mime;
  c->codec = decoder;
  c->context = ff->alloc_context(decoder);
  c->packet = ff->packet_alloc();
  c->frame = ff->frame_alloc();
  if (c->context == nullptr || c->packet == nullptr || c->frame == nullptr) {
    ff->free_context(&c->context);
    ff->packet_free(&c->packet);
    ff->frame_free(&c->frame);
    delete c;
    return nullptr;
  }
  std::fprintf(stderr, "  [media] decoder %s -> %s\n", mime, decoder->name);
  return c;
}

media_status_t AMediaCodec_configure(AMediaCodec* c, const AMediaFormat* format,
                                     void* surface, void*, uint32_t) {
  if (c == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  if (surface != nullptr) {
    std::fprintf(stderr, "  [media] %s: surface output is not supported\n",
                 c->mime.c_str());
    return AMEDIA_ERROR_UNKNOWN;
  }
  // A codec configured again after stop() needs a fresh FFmpeg context.
  if (c->opened) {
    ff->free_context(&c->context);
    c->context = ff->alloc_context(c->codec);
    c->opened = false;
    if (c->context == nullptr) return AMEDIA_ERROR_UNKNOWN;
  }
  size_t input_bytes = kMinInputBytes;
  if (format != nullptr) {
    const auto size = format->ints.find("max-input-size");
    if (size != format->ints.end() && size->second > 0) {
      input_bytes = std::max(input_bytes, size_t(size->second));
    }
    for (const char* key : {"width", "height"}) {
      const auto v = format->ints.find(key);
      if (v != format->ints.end()) {
        (key[0] == 'w' ? c->context->width : c->context->height) = v->second;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(c->mutex);
    c->csd.clear();
    for (const char* key : {"csd-0", "csd-1", "csd-2"}) {
      if (format == nullptr) break;
      const auto csd = format->buffers.find(key);
      if (csd != format->buffers.end()) {
        c->csd.insert(c->csd.end(), csd->second.begin(), csd->second.end());
      }
    }
    c->config = c->csd;
  }
  for (std::vector<uint8_t>& input : c->inputs) {
    input.assign(input_bytes, 0);
  }
  c->context->thread_count = 0;
  c->context->pkt_timebase = AVRational{1, 1000000};
  if (ff->open(c->context, c->codec, nullptr) < 0) {
    std::fprintf(stderr, "  [media] %s: could not open %s\n", c->mime.c_str(),
                 c->codec->name);
    return AMEDIA_ERROR_UNKNOWN;
  }
  c->opened = true;
  std::fprintf(stderr, "  [media] %s configured %dx%d\n", c->mime.c_str(),
               c->context->width, c->context->height);
  return AMEDIA_OK;
}

media_status_t AMediaCodec_start(AMediaCodec* c) {
  if (c == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  if (!c->opened) return AMEDIA_ERROR_UNKNOWN;
  std::lock_guard<std::mutex> lock(c->mutex);
  ResetBuffers(c);
  c->decoded_width = c->decoded_height = 0;
  c->reported_width = c->reported_height = 0;
  return AMEDIA_OK;
}

media_status_t AMediaCodec_flush(AMediaCodec* c) {
  if (c == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(c->mutex);
  if (c->opened) ff->flush_buffers(c->context);
  ResetBuffers(c);
  c->changed.notify_all();
  return AMEDIA_OK;
}

media_status_t AMediaCodec_stop(AMediaCodec* c) { return AMediaCodec_flush(c); }

media_status_t AMediaCodec_delete(AMediaCodec* c) {
  if (c == nullptr) return AMEDIA_OK;
  ff->free_context(&c->context);
  ff->packet_free(&c->packet);
  ff->frame_free(&c->frame);
  delete c;
  return AMEDIA_OK;
}

ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec* c, int64_t timeout_us) {
  if (c == nullptr) return kTryAgainLater;
  std::unique_lock<std::mutex> lock(c->mutex);
  const auto free_input = [&]() -> ssize_t {
    if (c->draining || c->ready.size() >= kMaxReadyOutputs) return -1;
    for (int i = 0; i < kInputBuffers; ++i) {
      if (c->input_free[i]) return i;
    }
    return -1;
  };
  ssize_t index = free_input();
  if (index < 0 && timeout_us != 0) {
    const auto wait = timeout_us < 0 ? std::chrono::microseconds(100000)
                                     : std::chrono::microseconds(timeout_us);
    c->changed.wait_for(lock, wait, [&] { return free_input() >= 0; });
    index = free_input();
  }
  if (index < 0) return kTryAgainLater;
  c->input_free[index] = false;
  return index;
}

uint8_t* AMediaCodec_getInputBuffer(AMediaCodec* c, size_t index,
                                    size_t* size) {
  if (c == nullptr || index >= kInputBuffers) return nullptr;
  if (size != nullptr) *size = c->inputs[index].size();
  return c->inputs[index].empty() ? nullptr : c->inputs[index].data();
}

media_status_t AMediaCodec_queueInputBuffer(AMediaCodec* c, size_t index,
                                            off_t offset, size_t size,
                                            uint64_t time_us, uint32_t flags) {
  if (c == nullptr || index >= kInputBuffers || offset < 0 ||
      size_t(offset) > c->inputs[index].size() ||
      size > c->inputs[index].size() - size_t(offset)) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  std::lock_guard<std::mutex> lock(c->mutex);
  const uint8_t* data = c->inputs[index].data() + offset;
  if (c->draining) {
    // Input after end-of-stream is dropped, as MediaCodec does.
  } else if (flags & kFlagCodecConfig) {
    c->config.insert(c->config.end(), data, data + size);
  } else if (size > 0) {
    Send(c, data, size, static_cast<int64_t>(time_us));
  }
  if ((flags & kFlagEndOfStream) && !c->draining) {
    c->draining = true;
    ff->send_packet(c->context, nullptr);
    ReceiveFrames(c);
  }
  c->input_free[index] = true;
  c->changed.notify_all();
  return AMEDIA_OK;
}

ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec* c,
                                        AMediaCodecBufferInfo* info,
                                        int64_t timeout_us) {
  if (c == nullptr || info == nullptr) return kTryAgainLater;
  std::unique_lock<std::mutex> lock(c->mutex);
  if (c->ready.empty() && timeout_us != 0) {
    const auto wait = timeout_us < 0 ? std::chrono::microseconds(100000)
                                     : std::chrono::microseconds(timeout_us);
    c->changed.wait_for(lock, wait, [&] { return !c->ready.empty(); });
  }
  if (c->ready.empty()) return kTryAgainLater;
  const size_t slot = c->ready.front();
  const AMediaCodec::Output& out = c->outputs[slot];
  if (out.width != 0 && (out.width != c->reported_width ||
                         out.height != c->reported_height)) {
    c->reported_width = out.width;
    c->reported_height = out.height;
    return kOutputFormatChanged;
  }
  c->ready.pop_front();
  c->changed.notify_all();
  info->offset = 0;
  info->size = static_cast<int32_t>(out.data.size());
  info->presentationTimeUs = out.pts;
  info->flags = out.flags;
  return static_cast<ssize_t>(slot);
}

uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec* c, size_t index,
                                     size_t* size) {
  if (c == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(c->mutex);
  if (index >= c->outputs.size()) return nullptr;
  if (size != nullptr) *size = c->outputs[index].data.size();
  return c->outputs[index].data.data();
}

media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec* c, size_t index,
                                               bool) {
  if (c == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(c->mutex);
  if (index < c->outputs.size()) c->outputs[index].in_use = false;
  c->changed.notify_all();
  return AMEDIA_OK;
}

AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec* c) {
  auto* format = new AMediaFormat;
  if (c == nullptr) return format;
  std::lock_guard<std::mutex> lock(c->mutex);
  format->strings["mime"] = "video/raw";
  const int width = c->reported_width;
  const int height = c->reported_height;
  format->ints["width"] = width;
  format->ints["height"] = height;
  format->ints["color-format"] = kColorFormatYuv420Planar;
  format->ints["stride"] = Even(width);
  format->ints["slice-height"] = Even(height);
  format->ints["crop-left"] = 0;
  format->ints["crop-top"] = 0;
  format->ints["crop-right"] = width - 1;
  format->ints["crop-bottom"] = height - 1;
  return format;
}

}  // extern "C"

#else  // !MOCKTAIL_MEDIANDK_FFMPEG

struct AMediaCodec {};

extern "C" {

AMediaCodec* AMediaCodec_createDecoderByType(const char* mime) {
  std::fprintf(stderr, "  [media] no decoder for %s (built without FFmpeg)\n",
               mime != nullptr ? mime : "(null)");
  return nullptr;
}
media_status_t AMediaCodec_configure(AMediaCodec*, const AMediaFormat*, void*,
                                     void*, uint32_t) {
  return AMEDIA_ERROR_UNKNOWN;
}
media_status_t AMediaCodec_start(AMediaCodec*) { return AMEDIA_ERROR_UNKNOWN; }
media_status_t AMediaCodec_stop(AMediaCodec*) { return AMEDIA_OK; }
media_status_t AMediaCodec_flush(AMediaCodec*) { return AMEDIA_OK; }
media_status_t AMediaCodec_delete(AMediaCodec*) { return AMEDIA_OK; }
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec*, size_t, size_t*) {
  return nullptr;
}
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec*, size_t, size_t*) {
  return nullptr;
}
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t) { return -1; }
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec*, AMediaCodecBufferInfo*,
                                        int64_t) {
  return -1;
}
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec*, size_t, off_t,
                                            size_t, uint64_t, uint32_t) {
  return AMEDIA_ERROR_UNKNOWN;
}
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec*, size_t, bool) {
  return AMEDIA_OK;
}
AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec*) {
  return new AMediaFormat;
}

}  // extern "C"

#endif

extern "C" {

// Key names Roblox links against as data symbols.
const char* AMEDIAFORMAT_KEY_MIME = "mime";
const char* AMEDIAFORMAT_KEY_WIDTH = "width";
const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
const char* AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";
const char* AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
const char* AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
const char* AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
const char* AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval";
const char* AMEDIAFORMAT_KEY_STRIDE = "stride";

AMediaFormat* AMediaFormat_new() { return new AMediaFormat; }
media_status_t AMediaFormat_delete(AMediaFormat* format) {
  delete format;
  return AMEDIA_OK;
}
bool AMediaFormat_getInt32(AMediaFormat* format, const char* name,
                           int32_t* out) {
  if (format == nullptr || name == nullptr) return false;
  const auto value = format->ints.find(name);
  if (value == format->ints.end()) return false;
  if (out != nullptr) *out = value->second;
  return true;
}
bool AMediaFormat_getBuffer(AMediaFormat* format, const char* name,
                            void** data, size_t* size) {
  if (format == nullptr || name == nullptr) return false;
  auto value = format->buffers.find(name);
  if (value == format->buffers.end()) return false;
  if (data != nullptr) *data = value->second.data();
  if (size != nullptr) *size = value->second.size();
  return true;
}
void AMediaFormat_setInt32(AMediaFormat* format, const char* name,
                           int32_t value) {
  if (format != nullptr && name != nullptr) format->ints[name] = value;
}
void AMediaFormat_setFloat(AMediaFormat* format, const char* name,
                           float value) {
  if (format != nullptr && name != nullptr) format->floats[name] = value;
}
void AMediaFormat_setString(AMediaFormat* format, const char* name,
                            const char* value) {
  if (format != nullptr && name != nullptr && value != nullptr) {
    format->strings[name] = value;
  }
}
void AMediaFormat_setBuffer(AMediaFormat* format, const char* name,
                            const void* data, size_t size) {
  if (format == nullptr || name == nullptr) return;
  const auto* bytes = static_cast<const uint8_t*>(data);
  format->buffers[name].assign(bytes, bytes + (bytes != nullptr ? size : 0));
}
const char* AMediaFormat_toString(AMediaFormat* format) {
  if (format == nullptr) return "{}";
  std::string& text = format->text;
  text = "{";
  for (const auto& [key, value] : format->strings) {
    text += key + "=" + value + ", ";
  }
  for (const auto& [key, value] : format->ints) {
    text += key + "=" + std::to_string(value) + ", ";
  }
  text += "}";
  return text.c_str();
}

AMediaCodec* AMediaCodec_createEncoderByType(const char* mime) {
  std::fprintf(stderr, "  [media] no encoder for %s\n",
               mime != nullptr ? mime : "(null)");
  return nullptr;
}

}  // extern "C"
