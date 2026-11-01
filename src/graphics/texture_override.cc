#include "mocktail/graphics/texture_override.h"

#include <png.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace mocktail::graphics {

std::uint64_t HashBytes(const std::uint8_t* data, std::size_t size) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

std::string HashName(std::uint64_t hash) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx",
                static_cast<unsigned long long>(hash));
  return text;
}

bool ReadPngRgba(const std::string& path, RgbaImage* image) {
  if (image == nullptr) {
    return false;
  }
  png_image png{};
  png.version = PNG_IMAGE_VERSION;
  if (png_image_begin_read_from_file(&png, path.c_str()) == 0) {
    return false;
  }
  png.format = PNG_FORMAT_RGBA;
  RgbaImage result;
  result.width = png.width;
  result.height = png.height;
  result.pixels.resize(PNG_IMAGE_SIZE(png));
  if (png_image_finish_read(&png, nullptr, result.pixels.data(), 0,
                            nullptr) == 0) {
    png_image_free(&png);
    return false;
  }
  *image = std::move(result);
  return true;
}

bool WritePngRgba(const std::string& path, const RgbaImage& image) {
  if (image.width == 0 || image.height == 0 ||
      image.pixels.size() !=
          static_cast<std::size_t>(image.width) * image.height * 4) {
    return false;
  }
  png_image png{};
  png.version = PNG_IMAGE_VERSION;
  png.width = image.width;
  png.height = image.height;
  png.format = PNG_FORMAT_RGBA;
  const int written = png_image_write_to_file(&png, path.c_str(), 0,
                                              image.pixels.data(), 0, nullptr);
  png_image_free(&png);
  return written != 0;
}

void ResampleRgba(const RgbaImage& source, std::uint32_t width,
                  std::uint32_t height, std::uint8_t* destination) {
  if (source.pixels.size() <
      static_cast<std::size_t>(source.width) * source.height * 4) {
    return;
  }
  ResampleRgba(source.pixels.data(), source.width, source.height,
               static_cast<std::size_t>(source.width) * 4, width, height,
               destination);
}

void ResampleRgba(const std::uint8_t* source, std::uint32_t source_width,
                  std::uint32_t source_height, std::uint32_t width,
                  std::uint32_t height, std::uint8_t* destination) {
  ResampleRgba(source, source_width, source_height,
               static_cast<std::size_t>(source_width) * 4, width, height,
               destination);
}

void ResampleRgba(const std::uint8_t* source, std::uint32_t source_width,
                  std::uint32_t source_height, std::size_t source_stride_bytes,
                  std::uint32_t width, std::uint32_t height,
                  std::uint8_t* destination) {
  if (source == nullptr || destination == nullptr || width == 0 ||
      height == 0 || source_width == 0 || source_height == 0 ||
      source_stride_bytes < static_cast<std::size_t>(source_width) * 4) {
    return;
  }
  // Source box of destination column x is [columns[2x], columns[2x + 1]).
  thread_local std::vector<std::uint32_t> columns;
  if (columns.size() < static_cast<std::size_t>(width) * 2) {
    columns.resize(static_cast<std::size_t>(width) * 2);
  }
  bool one_texel_columns = true;
  for (std::uint32_t x = 0; x < width; ++x) {
    const std::uint32_t x0 = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(x) * source_width / width);
    const std::uint32_t x1 = std::max<std::uint32_t>(
        x0 + 1, static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(x) + 1) * source_width / width));
    columns[2 * static_cast<std::size_t>(x)] = x0;
    columns[2 * static_cast<std::size_t>(x) + 1] = x1;
    one_texel_columns = one_texel_columns && x1 == x0 + 1;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
  std::uint64_t previous_y0 = 0;
  std::uint64_t previous_y1 = 0;
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint64_t y0 = static_cast<std::uint64_t>(y) * source_height / height;
    std::uint64_t y1 =
        (static_cast<std::uint64_t>(y) + 1) * source_height / height;
    if (y1 <= y0) {
      y1 = y0 + 1;
    }
    std::uint8_t* out_row = destination + y * row_bytes;
    if (y > 0 && y0 == previous_y0 && y1 == previous_y1) {
      std::memcpy(out_row, out_row - row_bytes, row_bytes);
      continue;
    }
    previous_y0 = y0;
    previous_y1 = y1;
    if (one_texel_columns && y1 == y0 + 1) {
      const std::uint8_t* source_row = source + y0 * source_stride_bytes;
      for (std::uint32_t x = 0; x < width; ++x) {
        std::uint32_t pixel;
        std::memcpy(&pixel, source_row + columns[2 * static_cast<std::size_t>(x)] * 4, 4);
        std::memcpy(out_row + static_cast<std::size_t>(x) * 4, &pixel, 4);
      }
      continue;
    }
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint64_t x0 = columns[2 * static_cast<std::size_t>(x)];
      const std::uint64_t x1 = columns[2 * static_cast<std::size_t>(x) + 1];
      std::uint64_t sum[4] = {0, 0, 0, 0};
      for (std::uint64_t sy = y0; sy < y1; ++sy) {
        const std::uint8_t* row_ptr = source + sy * source_stride_bytes;
        for (std::uint64_t sx = x0; sx < x1; ++sx) {
          const std::uint8_t* pixel = row_ptr + sx * 4;
          for (int channel = 0; channel < 4; ++channel) {
            sum[channel] += pixel[channel];
          }
        }
      }
      const std::uint64_t count = (y1 - y0) * (x1 - x0);
      std::uint8_t* out = destination + (static_cast<std::size_t>(y) * width + x) * 4;
      if (count == 1) {
        for (int channel = 0; channel < 4; ++channel) {
          out[channel] = static_cast<std::uint8_t>(sum[channel]);
        }
      } else {
        const std::uint64_t half = count / 2;
        for (int channel = 0; channel < 4; ++channel) {
          out[channel] =
              static_cast<std::uint8_t>((sum[channel] + half) / count);
        }
      }
    }
  }
}

namespace {

// Keys cubic with a = -0.5, the common bicubic kernel.
float Cubic(float x) {
  x = std::fabs(x);
  if (x < 1) {
    return (1.5f * x - 2.5f) * x * x + 1;
  }
  if (x < 2) {
    return ((-0.5f * x + 2.5f) * x - 4) * x + 2;
  }
  return 0;
}

float SourcePosition(std::uint32_t i, std::uint32_t source,
                     std::uint32_t size) {
  return (static_cast<float>(i) + 0.5f) * static_cast<float>(source) /
             static_cast<float>(size) -
         0.5f;
}

// Weights over a run of source texels for one output position.
struct Filter {
  static constexpr int kMaxTaps = 12;
  std::uint32_t first = 0;
  std::uint32_t count = 0;
  float weight[kMaxTaps] = {};

  void Add(std::uint32_t index, float value) {
    if (count == 0) {
      first = index;
      count = 1;
    }
    while (index < first) {
      for (std::uint32_t k = count; k > 0; --k) {
        weight[k] = weight[k - 1];
      }
      weight[0] = 0;
      --first;
      ++count;
    }
    while (index >= first + count) {
      weight[count++] = 0;
    }
    weight[index - first] += value;
  }
};

// Per output position along one axis: the bicubic filter, and the bicubic
// filter followed by the edge-clamped 7-tap Gaussian of the unsharp mask,
// folded into one filter over the source.
void AxisFilters(std::uint32_t source, std::uint32_t size, float sigma,
                 std::vector<Filter>* cubic, std::vector<Filter>* blurred) {
  cubic->assign(size, Filter{});
  blurred->assign(size, Filter{});
  for (std::uint32_t i = 0; i < size; ++i) {
    const float at = SourcePosition(i, source, size);
    const int base = static_cast<int>(std::floor(at)) - 1;
    for (int k = 0; k < 4; ++k) {
      (*cubic)[i].Add(static_cast<std::uint32_t>(std::clamp(
                          base + k, 0, static_cast<int>(source) - 1)),
                      Cubic(at - static_cast<float>(base + k)));
    }
  }
  float gaussian[7];
  float sum = 0;
  for (int k = 0; k < 7; ++k) {
    gaussian[k] = std::exp(-static_cast<float>((k - 3) * (k - 3)) /
                           (2 * sigma * sigma));
    sum += gaussian[k];
  }
  for (std::uint32_t i = 0; i < size; ++i) {
    for (int k = 0; k < 7; ++k) {
      const Filter& tap = (*cubic)[static_cast<std::size_t>(std::clamp(
          static_cast<int>(i) + k - 3, 0, static_cast<int>(size) - 1))];
      for (std::uint32_t t = 0; t < tap.count; ++t) {
        (*blurred)[i].Add(tap.first + t, gaussian[k] / sum * tap.weight[t]);
      }
    }
  }
}

// Sharpened, clamped premultiplied texel to straight-alpha RGBA8.
inline void FinishTexel(const float* plain, const float* soft,
                        const float* low, const float* high, float amount,
                        std::uint8_t* out) {
#if defined(__SSE2__)
  const __m128 p = _mm_loadu_ps(plain);
  __m128 v = _mm_add_ps(
      p, _mm_mul_ps(_mm_set1_ps(amount), _mm_sub_ps(p, _mm_loadu_ps(soft))));
  v = _mm_min_ps(_mm_max_ps(v, _mm_loadu_ps(low)), _mm_loadu_ps(high));
  const float alpha =
      _mm_cvtss_f32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3)));
  const float scale = alpha > 0 ? 255.0f / alpha : 0.0f;
  v = _mm_mul_ps(v, _mm_setr_ps(scale, scale, scale, 1.0f));
  v = _mm_add_ps(_mm_min_ps(v, _mm_set1_ps(255.0f)), _mm_set1_ps(0.5f));
  __m128i packed = _mm_cvttps_epi32(v);
  packed = _mm_packs_epi32(packed, packed);
  packed = _mm_packus_epi16(packed, packed);
  const int word = _mm_cvtsi128_si32(packed);
  std::memcpy(out, &word, 4);
#else
  float v[4];
  for (int ch = 0; ch < 4; ++ch) {
    v[ch] = std::min(std::max(plain[ch] + amount * (plain[ch] - soft[ch]),
                              low[ch]),
                     high[ch]);
  }
  const float scale = v[3] > 0 ? 255.0f / v[3] : 0.0f;
  for (int ch = 0; ch < 4; ++ch) {
    const float straight = ch == 3 ? v[ch] : v[ch] * scale;
    out[ch] = static_cast<std::uint8_t>(std::min(straight, 255.0f) + 0.5f);
  }
#endif
}

}  // namespace

void UpscaleRgba(const std::uint8_t* source, std::uint32_t source_width,
                 std::uint32_t source_height, std::size_t source_stride_bytes,
                 std::uint32_t width, std::uint32_t height,
                 std::uint8_t* destination) {
  if (source == nullptr || destination == nullptr || source_width == 0 ||
      source_height == 0 || width < source_width || height < source_height ||
      source_stride_bytes < static_cast<std::size_t>(source_width) * 4) {
    ResampleRgba(source, source_width, source_height, source_stride_bytes,
                 width, height, destination);
    return;
  }
  constexpr float kAmount = 1.2f;
  constexpr float kRadius = 1.5f;
  // Channel distance for "the same flat colour", and the least contrast
  // that makes two of them an edge rather than a smooth patch.
  constexpr float kFlat = 6;
  constexpr float kEdge = 3 * kFlat;
  const std::uint32_t sw = source_width;
  const std::uint32_t sh = source_height;
  thread_local std::vector<float> premultiplied;
  thread_local std::vector<float> rows;
  thread_local std::vector<float> blurred_rows;
  thread_local std::vector<std::uint8_t> two_tone;
  thread_local std::vector<Filter> cubic_x;
  thread_local std::vector<Filter> blurred_x;
  thread_local std::vector<Filter> cubic_y;
  thread_local std::vector<Filter> blurred_y;
  thread_local std::vector<std::uint32_t> column;
  thread_local std::vector<float> plain_row;
  thread_local std::vector<float> soft_row;
  thread_local std::vector<float> low;
  thread_local std::vector<float> high;
  premultiplied.resize(std::size_t{sw} * sh * 4);
  for (std::uint32_t y = 0; y < sh; ++y) {
    for (std::uint32_t x = 0; x < sw; ++x) {
      const std::uint8_t* in = source + y * source_stride_bytes + x * 4;
      float* out = premultiplied.data() + (std::size_t{y} * sw + x) * 4;
      const float alpha = in[3];
      for (int c = 0; c < 3; ++c) {
        out[c] = in[c] * alpha / 255.0f;
      }
      out[3] = alpha;
    }
  }
  const auto texel = [&](std::uint32_t x, std::uint32_t y) {
    return premultiplied.data() + (std::size_t{y} * sw + x) * 4;
  };

  // The unsharp radius follows the scale, as at the default 4x.
  AxisFilters(sw, width,
              kRadius * static_cast<float>(width) / static_cast<float>(sw) / 4,
              &cubic_x, &blurred_x);
  AxisFilters(sh, height,
              kRadius * static_cast<float>(height) / static_cast<float>(sh) /
                  4,
              &cubic_y, &blurred_y);
  rows.resize(std::size_t{sh} * width * 4);
  blurred_rows.resize(rows.size());
  for (std::uint32_t y = 0; y < sh; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      float plain[4] = {0, 0, 0, 0};
      float soft[4] = {0, 0, 0, 0};
      const Filter& c = cubic_x[x];
      for (std::uint32_t t = 0; t < c.count; ++t) {
        const float* p = texel(c.first + t, y);
        for (int ch = 0; ch < 4; ++ch) {
          plain[ch] += c.weight[t] * p[ch];
        }
      }
      const Filter& b = blurred_x[x];
      for (std::uint32_t t = 0; t < b.count; ++t) {
        const float* p = texel(b.first + t, y);
        for (int ch = 0; ch < 4; ++ch) {
          soft[ch] += b.weight[t] * p[ch];
        }
      }
      const std::size_t at = (std::size_t{y} * width + x) * 4;
      std::memcpy(rows.data() + at, plain, sizeof(plain));
      std::memcpy(blurred_rows.data() + at, soft, sizeof(soft));
    }
  }

  two_tone.assign(std::size_t{sw} * sh, 0);
  for (std::uint32_t y = 0; y < sh; ++y) {
    for (std::uint32_t x = 0; x < sw; ++x) {
      const float* around[9];
      int low = 0;
      int high = 0;
      float low_sum = 0;
      float high_sum = 0;
      for (int i = 0; i < 9; ++i) {
        around[i] = texel(static_cast<std::uint32_t>(std::clamp(
                              static_cast<int>(x) + i % 3 - 1, 0,
                              static_cast<int>(sw) - 1)),
                          static_cast<std::uint32_t>(std::clamp(
                              static_cast<int>(y) + i / 3 - 1, 0,
                              static_cast<int>(sh) - 1)));
        const float sum =
            around[i][0] + around[i][1] + around[i][2] + around[i][3];
        if (i == 0 || sum < low_sum) {
          low = i;
          low_sum = sum;
        }
        if (i == 0 || sum > high_sum) {
          high = i;
          high_sum = sum;
        }
      }
      const auto distance = [](const float* a, const float* b) {
        float most = 0;
        for (int c = 0; c < 4; ++c) {
          most = std::max(most, std::fabs(a[c] - b[c]));
        }
        return most;
      };
      bool flat = distance(around[low], around[high]) > kEdge;
      for (int i = 0; i < 9 && flat; ++i) {
        flat = distance(around[i], around[low]) <= kFlat ||
               distance(around[i], around[high]) <= kFlat;
      }
      two_tone[std::size_t{y} * sw + x] = flat ? 1 : 0;
    }
  }

  // Per source texel, the range of the 2x2 texels from it down and right;
  // sharpening is clamped to it.
  low.resize(premultiplied.size());
  high.resize(premultiplied.size());
  for (std::uint32_t y = 0; y < sh; ++y) {
    const std::uint32_t y1 = std::min(y + 1, sh - 1);
    for (std::uint32_t x = 0; x < sw; ++x) {
      const std::uint32_t x1 = std::min(x + 1, sw - 1);
      const float* corners[4] = {texel(x, y), texel(x1, y), texel(x, y1),
                                 texel(x1, y1)};
      const std::size_t at = (std::size_t{y} * sw + x) * 4;
      for (int ch = 0; ch < 4; ++ch) {
        low[at + ch] = std::min({corners[0][ch], corners[1][ch],
                                 corners[2][ch], corners[3][ch]});
        high[at + ch] = std::max({corners[0][ch], corners[1][ch],
                                  corners[2][ch], corners[3][ch]});
      }
    }
  }

  // Left footprint column and nearest column of each output column.
  column.resize(std::size_t{width} * 2);
  for (std::uint32_t x = 0; x < width; ++x) {
    column[2 * std::size_t{x}] = static_cast<std::uint32_t>(
        std::clamp(static_cast<int>(std::floor(SourcePosition(x, sw, width))),
                   0, static_cast<int>(sw) - 1));
    column[2 * std::size_t{x} + 1] =
        static_cast<std::uint32_t>(std::uint64_t{x} * sw / width);
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint32_t y0 = static_cast<std::uint32_t>(
        std::clamp(static_cast<int>(std::floor(SourcePosition(y, sh, height))),
                   0, static_cast<int>(sh) - 1));
    const std::uint32_t near_y =
        static_cast<std::uint32_t>(std::uint64_t{y} * sh / height);
    // Rows are filtered whole, which keeps the inner loops contiguous.
    const std::size_t row_floats = std::size_t{width} * 4;
    plain_row.assign(row_floats, 0.0f);
    soft_row.assign(row_floats, 0.0f);
    const Filter& c = cubic_y[y];
    for (std::uint32_t t = 0; t < c.count; ++t) {
      const float w = c.weight[t];
      const float* in = rows.data() + (c.first + t) * row_floats;
      float* out = plain_row.data();
      for (std::size_t i = 0; i < row_floats; ++i) {
        out[i] += w * in[i];
      }
    }
    const Filter& b = blurred_y[y];
    for (std::uint32_t t = 0; t < b.count; ++t) {
      const float w = b.weight[t];
      const float* in = blurred_rows.data() + (b.first + t) * row_floats;
      float* out = soft_row.data();
      for (std::size_t i = 0; i < row_floats; ++i) {
        out[i] += w * in[i];
      }
    }
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t* out = destination + (std::size_t{y} * width + x) * 4;
      const std::uint32_t near_x = column[2 * std::size_t{x} + 1];
      if (two_tone[std::size_t{near_y} * sw + near_x] != 0) {
        std::memcpy(out, source + near_y * source_stride_bytes + near_x * 4,
                    4);
        continue;
      }
      const std::size_t cell =
          (std::size_t{y0} * sw + column[2 * std::size_t{x}]) * 4;
      FinishTexel(plain_row.data() + std::size_t{x} * 4,
                  soft_row.data() + std::size_t{x} * 4, low.data() + cell,
                  high.data() + cell, kAmount, out);
    }
  }
}

namespace {

std::string EnvironmentString(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? value : "";
}

}  // namespace

TextureOverrides::TextureOverrides(std::string override_dir,
                                   std::string dump_dir)
    : override_dir_(std::move(override_dir)), dump_dir_(std::move(dump_dir)) {
  // An override costs matching textures their GPU decode and BC storage, so
  // the folder only counts once it holds a PNG.
  std::error_code error;
  if (!override_dir_.empty()) {
    for (std::filesystem::directory_iterator it(override_dir_, error), end;
         !error && it != end; it.increment(error)) {
      if (it->path().extension() != ".png") {
        continue;
      }
      const std::string stem = it->path().stem().string();
      unsigned long long hash = 0;
      unsigned width = 0;
      unsigned height = 0;
      char tail = 0;
      if (stem.size() > 17 && stem[16] == '_' &&
          std::sscanf(stem.c_str(), "%16llx_%ux%u%c", &hash, &width, &height,
                      &tail) == 3) {
        files_[hash] = it->path().string();
        sizes_.insert(std::uint64_t{width} << 32 | height);
      } else {
        std::fprintf(stderr,
                     "  [vulkan] texture override skipped: %s; name it "
                     "<hash>_<width>x<height>.png with the original size, as "
                     "MOCKTAIL_TEXTURE_DUMP_DIR writes it\n",
                     it->path().c_str());
      }
    }
  }
  if (files_.empty()) {
    override_dir_.clear();
  }
}

bool TextureOverrides::MayReplace(std::uint32_t width,
                                  std::uint32_t height) const {
  return dumping() ||
         sizes_.count(std::uint64_t{width} << 32 | height) != 0;
}

TextureOverrides TextureOverrides::FromEnvironment() {
  return TextureOverrides(EnvironmentString("MOCKTAIL_TEXTURE_OVERRIDE_DIR"),
                          EnvironmentString("MOCKTAIL_TEXTURE_DUMP_DIR"));
}

std::shared_ptr<const RgbaImage> TextureOverrides::Lookup(std::uint64_t hash) {
  if (override_dir_.empty()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto cached = cache_.find(hash);
  if (cached != cache_.end()) {
    return cached->second;
  }
  std::shared_ptr<const RgbaImage> loaded;
  RgbaImage image;
  const auto named = files_.find(hash);
  if (named == files_.end()) {
    return nullptr;
  }
  const std::string& path = named->second;
  if (ReadPngRgba(path, &image) && image.width > 0 && image.height > 0) {
    loaded = std::make_shared<const RgbaImage>(std::move(image));
    std::fprintf(stderr, "  [vulkan] texture override loaded: %s (%ux%u)\n",
                 path.c_str(), loaded->width, loaded->height);
  }
  cache_[hash] = loaded;
  return loaded;
}

void TextureOverrides::Dump(std::uint64_t hash, std::uint32_t width,
                            std::uint32_t height, const std::uint8_t* rgba) {
  if (dump_dir_.empty() || rgba == nullptr || width == 0 || height == 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dumped_.insert(hash).second) {
      return;
    }
  }
  std::error_code error;
  std::filesystem::create_directories(dump_dir_, error);
  RgbaImage image;
  image.width = width;
  image.height = height;
  image.pixels.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
  const std::string path = dump_dir_ + "/" + HashName(hash) + "_" +
                           std::to_string(width) + "x" +
                           std::to_string(height) + ".png";
  if (!WritePngRgba(path, image)) {
    std::fprintf(stderr, "  [vulkan] texture dump failed: %s\n", path.c_str());
  }
}

}  // namespace mocktail::graphics
