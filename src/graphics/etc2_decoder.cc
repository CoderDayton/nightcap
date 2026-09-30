#include "mocktail/graphics/etc2_decoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <pthread.h>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <vector>

namespace mocktail::graphics {
namespace {

constexpr std::array<int, 8> kDistance = {3, 6, 11, 16, 23, 32, 41, 64};

// Indexed by (msb << 1) | lsb.
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

std::uint8_t Clamp255(int value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

int Expand4(int value) { return (value << 4) | value; }
int Expand5(int value) { return (value << 3) | (value >> 2); }
int Expand6(int value) { return (value << 2) | (value >> 4); }
int Expand7(int value) { return (value << 1) | (value >> 6); }

inline std::uint32_t MakeRgba32(int r, int g, int bl, int a) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  __m128i v = _mm_set_epi32(a, bl, g, r);
  __m128i p16 = _mm_packs_epi32(v, v);
  __m128i p8 = _mm_packus_epi16(p16, p16);
  return static_cast<std::uint32_t>(_mm_cvtsi128_si32(p8));
#else
  return static_cast<std::uint32_t>(Clamp255(r)) |
         (static_cast<std::uint32_t>(Clamp255(g)) << 8) |
         (static_cast<std::uint32_t>(Clamp255(bl)) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) << 24);
#endif
}

#if defined(__SSSE3__) || defined(__x86_64__) || defined(_M_X64)
struct ColumnShuffleTable {
  alignas(16) std::uint8_t masks[256][16];
  constexpr ColumnShuffleTable() : masks{} {
    for (int m = 0; m < 16; ++m) {
      for (int l = 0; l < 16; ++l) {
        const int idx = (m << 4) | l;
        for (int p = 0; p < 4; ++p) {
          const int color_idx = (((m >> p) & 1) << 1) | ((l >> p) & 1);
          for (int b = 0; b < 4; ++b) {
            masks[idx][p * 4 + b] = static_cast<std::uint8_t>(color_idx * 4 + b);
          }
        }
      }
    }
  }
};
inline constexpr ColumnShuffleTable kColShuffleTable{};
#endif

// Decodes one ETC2 RGB block into 16 column-major RGBA texels (x * 4 + y).
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(_M_X64))
__attribute__((target("ssse3")))
#endif
void DecodeRgbBlock(const std::uint8_t* b, bool punchthrough,
                    std::uint32_t* out32) {
  static constexpr int kDelta[8] = {0, 1, 2, 3, -4, -3, -2, -1};
  const bool diff_or_opaque = (b[3] & 2) != 0;
  const bool opaque = !punchthrough || diff_or_opaque;
  const int msb = (b[4] << 8) | b[5];
  const int lsb = (b[6] << 8) | b[7];
  auto index_of = [msb, lsb](int texel) {
    return (((msb >> texel) & 1) << 1) | ((lsb >> texel) & 1);
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
    std::uint32_t paint32[4];
    for (int i = 0; i < 4; ++i) {
      paint32[i] = MakeRgba32(paint[i][0], paint[i][1], paint[i][2], 255);
    }
    for (int texel = 0; texel < 16; ++texel) {
      const int index = index_of(texel);
      out32[texel] = (!opaque && index == 2) ? 0 : paint32[index];
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
        out32[x * 4 + y] = MakeRgba32(
            (x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2,
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

  // Basis Universal palette precalculation: 8 colors per block instead of 48 additions/clamps.
  std::uint32_t palette[2][4];
  for (int sub = 0; sub < 2; ++sub) {
    for (int idx = 0; idx < 4; ++idx) {
      const int modifier =
          (!opaque && idx == 0) ? 0 : kEtc1Modifiers[tables[sub]][idx];
      palette[sub][idx] = MakeRgba32(base[sub][0] + modifier,
                                     base[sub][1] + modifier,
                                     base[sub][2] + modifier, 255);
    }
  }

  if (opaque) {
#if defined(__SSSE3__) || defined(__x86_64__) || defined(_M_X64)
    const __m128i pal0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(palette[0]));
    const __m128i pal1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(palette[1]));
    if (!flipped) {
      for (int x = 0; x < 2; ++x) {
        const int combo = (((msb >> (x * 4)) & 15) << 4) | ((lsb >> (x * 4)) & 15);
        const __m128i mask = _mm_load_si128(
            reinterpret_cast<const __m128i*>(kColShuffleTable.masks[combo]));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out32 + x * 4),
                         _mm_shuffle_epi8(pal0, mask));
      }
      for (int x = 2; x < 4; ++x) {
        const int combo = (((msb >> (x * 4)) & 15) << 4) | ((lsb >> (x * 4)) & 15);
        const __m128i mask = _mm_load_si128(
            reinterpret_cast<const __m128i*>(kColShuffleTable.masks[combo]));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out32 + x * 4),
                         _mm_shuffle_epi8(pal1, mask));
      }
    } else {
      for (int x = 0; x < 4; ++x) {
        const int combo = (((msb >> (x * 4)) & 15) << 4) | ((lsb >> (x * 4)) & 15);
        const __m128i mask = _mm_load_si128(
            reinterpret_cast<const __m128i*>(kColShuffleTable.masks[combo]));
        const __m128i col0 = _mm_shuffle_epi8(pal0, mask);
        const __m128i col1 = _mm_shuffle_epi8(pal1, mask);
        const __m128i blended =
            _mm_unpacklo_epi64(col0, _mm_srli_si128(col1, 8));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out32 + x * 4), blended);
      }
    }
#else
    if (!flipped) {
      for (int x = 0; x < 2; ++x) {
        const std::uint32_t* pal = palette[0];
        out32[x * 4 + 0] = pal[index_of(x * 4 + 0)];
        out32[x * 4 + 1] = pal[index_of(x * 4 + 1)];
        out32[x * 4 + 2] = pal[index_of(x * 4 + 2)];
        out32[x * 4 + 3] = pal[index_of(x * 4 + 3)];
      }
      for (int x = 2; x < 4; ++x) {
        const std::uint32_t* pal = palette[1];
        out32[x * 4 + 0] = pal[index_of(x * 4 + 0)];
        out32[x * 4 + 1] = pal[index_of(x * 4 + 1)];
        out32[x * 4 + 2] = pal[index_of(x * 4 + 2)];
        out32[x * 4 + 3] = pal[index_of(x * 4 + 3)];
      }
    } else {
      for (int x = 0; x < 4; ++x) {
        out32[x * 4 + 0] = palette[0][index_of(x * 4 + 0)];
        out32[x * 4 + 1] = palette[0][index_of(x * 4 + 1)];
        out32[x * 4 + 2] = palette[1][index_of(x * 4 + 2)];
        out32[x * 4 + 3] = palette[1][index_of(x * 4 + 3)];
      }
    }
#endif
  } else {
    for (int x = 0; x < 4; ++x) {
      for (int y = 0; y < 4; ++y) {
        const int texel = x * 4 + y;
        const int index = index_of(texel);
        if (index == 2) {
          out32[texel] = 0;
        } else {
          const int sub = flipped ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
          out32[texel] = palette[sub][index];
        }
      }
    }
  }
}

[[maybe_unused]] inline void DecodeRgbBlock(
    const std::uint8_t* b, bool punchthrough,
    std::array<std::uint8_t, 64>* out) {
  DecodeRgbBlock(b, punchthrough,
                 reinterpret_cast<std::uint32_t*>(out->data()));
}

// EAC 3-bit index for column-major texel (x * 4 + y).
inline std::uint64_t ReadEacBits(const std::uint8_t* b) {
  std::uint64_t raw;
  std::memcpy(&raw, b, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return __builtin_bswap64(raw) & 0x0000FFFFFFFFFFFFULL;
#else
  return raw & 0x0000FFFFFFFFFFFFULL;
#endif
}

inline int EacIndexFromBits(std::uint64_t bits, int texel) {
  return static_cast<int>((bits >> (45 - texel * 3)) & 7);
}

[[maybe_unused]] int EacIndex(const std::uint8_t* b, int texel) {
  return EacIndexFromBits(ReadEacBits(b), texel);
}

[[maybe_unused]] int EacModifier(const std::uint8_t* b, int texel) {
  return kEacModifiers[b[1] & 15][EacIndex(b, texel)];
}

inline std::uint16_t DecodeR11Modifier(const std::uint8_t* b, int multiplier,
                                       const int* modifier_table,
                                       int modifier_idx, bool is_signed) {
  const int modifier = modifier_table[modifier_idx];
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

inline std::uint16_t DecodeR11WithBits(const std::uint8_t* b, std::uint64_t bits,
                                       int multiplier, const int* modifier_table,
                                       int texel, bool is_signed) {
  return DecodeR11Modifier(b, multiplier, modifier_table,
                           EacIndexFromBits(bits, texel), is_signed);
}

[[maybe_unused]] std::uint16_t DecodeR11(const std::uint8_t* b, int texel, bool is_signed) {
  const int multiplier = b[1] >> 4;
  return DecodeR11WithBits(b, ReadEacBits(b), multiplier,
                           kEacModifiers[b[1] & 15], texel, is_signed);
}

}  // namespace

std::size_t EtcBlockBytes(EtcFormat format) {
  switch (format) {
    case EtcFormat::kEtc2Rgba8:
    case EtcFormat::kEacRg11:
    case EtcFormat::kEacRg11Signed:
      return 16;
    default:
      return 8;
  }
}

std::size_t EtcDecodedTexelBytes(EtcFormat format) {
  switch (format) {
    case EtcFormat::kEacR11:
    case EtcFormat::kEacR11Signed:
      return 2;
    default:
      return 4;
  }
}

namespace {

// Blocks per band handed to one worker: about a 128x128 texel image.
constexpr std::uint64_t kBlocksPerBand = 4096;
// Batches smaller than a 128x128 texel image decode on the caller. Waking a
// pooled worker costs microseconds, so the floor only has to cover that.
constexpr std::uint64_t kParallelMinimumBlocks = 4096;

bool ImageFits(EtcFormat format, const std::uint8_t* source,
               std::size_t source_bytes, std::uint32_t width,
               std::uint32_t height, const std::uint8_t* destination,
               std::size_t destination_bytes) {
  if (source == nullptr || destination == nullptr || width == 0 ||
      height == 0) {
    return false;
  }
  const std::uint64_t blocks_wide = (static_cast<std::uint64_t>(width) + 3) / 4;
  const std::uint64_t blocks_high =
      (static_cast<std::uint64_t>(height) + 3) / 4;
  return blocks_wide * blocks_high * EtcBlockBytes(format) <= source_bytes &&
         static_cast<std::uint64_t>(width) * height *
                 EtcDecodedTexelBytes(format) <=
             destination_bytes;
}

}  // namespace

bool DecodeEtcImage(EtcFormat format, const std::uint8_t* source,
                    std::size_t source_bytes, std::uint32_t width,
                    std::uint32_t height, std::uint8_t* destination,
                    std::size_t destination_bytes) {
  return DecodeEtcImageBlockRows(format, source, source_bytes, width, height, 0,
                                 (height + 3) / 4, destination,
                                 destination_bytes);
}

namespace {

struct DecodeBand {
  EtcDecodeJob* job;
  std::uint32_t first_block_row;
  std::uint32_t block_row_count;
};

struct DecodeBandQueue {
  std::vector<DecodeBand> bands;
  std::atomic<std::size_t> next{0};

  void Run() {
    for (;;) {
      const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
      if (index >= bands.size()) {
        return;
      }
      const DecodeBand& band = bands[index];
      const EtcDecodeJob& job = *band.job;
      DecodeEtcImageBlockRows(job.format, job.source, job.source_bytes,
                              job.width, job.height, band.first_block_row,
                              band.block_row_count, job.destination,
                              job.destination_bytes);
    }
  }
};

// Decode workers outlive the batches that use them: creating and joining a
// thread per batch costs more than decoding a small one. The threads park on
// a condition variable and are never joined, so the pool is leaked at exit.
class DecodePool {
 public:
  static DecodePool& Instance() {
    static DecodePool* pool = new DecodePool();
    return *pool;
  }

  // Calls run(context) on the calling thread and on pooled workers, `workers`
  // calls in all. Returns false without calling it when another batch
  // already holds the pool or no worker could be started, leaving the whole
  // batch to the caller.
  bool Run(void (*run)(void*), void* context, std::size_t workers) {
    if (workers < 2) {
      return false;
    }
    std::unique_lock<std::mutex> batch(batch_mutex_, std::try_to_lock);
    if (!batch.owns_lock()) {
      return false;
    }
    const std::size_t helpers = Reserve(workers - 1);
    if (helpers == 0) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      run_ = run;
      context_ = context;
      unclaimed_ = helpers;
      running_.store(helpers, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < helpers; ++i) {
      ready_.notify_one();
    }
    run(context);
    if (running_.load(std::memory_order_acquire) > 0) {
      std::unique_lock<std::mutex> lock(done_mutex_);
      done_.wait(lock, [this] {
        return running_.load(std::memory_order_acquire) == 0;
      });
    }
    run_ = nullptr;
    context_ = nullptr;
    return true;
  }

 private:
  DecodePool() = default;

  // Grows the pool towards `wanted` and reports how many workers exist, so a
  // pthread_create failure degrades instead of deadlocking the wait below.
  std::size_t Reserve(std::size_t wanted) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (threads_ < wanted) {
      pthread_t worker;
      if (pthread_create(&worker, nullptr, &DecodePool::RunWorker, this) != 0) {
        break;
      }
      pthread_detach(worker);
      ++threads_;
    }
    return std::min(threads_, wanted);
  }

  static void* RunWorker(void* pool) {
    static_cast<DecodePool*>(pool)->WorkerLoop();
    return nullptr;
  }

  void WorkerLoop() {
    for (;;) {
      void (*run)(void*) = nullptr;
      void* context = nullptr;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return unclaimed_ > 0; });
        --unclaimed_;
        run = run_;
        context = context_;
      }
      run(context);
      if (running_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(done_mutex_);
        done_.notify_one();
      }
    }
  }

  std::mutex batch_mutex_;
  std::mutex mutex_;
  std::mutex done_mutex_;
  std::condition_variable ready_;
  std::condition_variable done_;
  void (*run_)(void*) = nullptr;
  void* context_ = nullptr;
  std::size_t unclaimed_ = 0;
  std::atomic<std::size_t> running_{0};
  std::size_t threads_ = 0;
};

}  // namespace

void DecodeEtcJobs(EtcDecodeJob* jobs, std::size_t count,
                   unsigned worker_count) {
  if (jobs == nullptr) {
    return;
  }
  DecodeBandQueue queue;
  std::vector<DecodeBand>& bands = queue.bands;
  std::uint64_t total_blocks = 0;
  for (std::size_t index = 0; index < count; ++index) {
    EtcDecodeJob& job = jobs[index];
    job.ok = ImageFits(job.format, job.source, job.source_bytes, job.width,
                       job.height, job.destination, job.destination_bytes);
    if (!job.ok) {
      continue;
    }
    const std::uint32_t blocks_wide = (job.width + 3) / 4;
    const std::uint32_t blocks_high = (job.height + 3) / 4;
    const std::uint32_t rows_per_band = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, kBlocksPerBand / blocks_wide));
    for (std::uint32_t row = 0; row < blocks_high; row += rows_per_band) {
      bands.push_back(
          {&job, row, std::min(rows_per_band, blocks_high - row)});
    }
    total_blocks += static_cast<std::uint64_t>(blocks_wide) * blocks_high;
  }

  const std::size_t threads =
      total_blocks < kParallelMinimumBlocks
          ? 1
          : std::min<std::size_t>(std::max(worker_count, 1U), bands.size());
  RunOnDecodeWorkers(
      [](void* context) { static_cast<DecodeBandQueue*>(context)->Run(); },
      &queue, static_cast<unsigned>(threads));
}

void RunOnDecodeWorkers(void (*run)(void*), void* context,
                        unsigned worker_count) {
  if (run == nullptr) {
    return;
  }
  // The pool declines when it is already busy; the caller drains the whole
  // batch itself then, exactly as a single-threaded batch does.
  if (!DecodePool::Instance().Run(run, context, worker_count)) {
    run(context);
  }
}

bool DecodeEtcImageBlockRows(EtcFormat format, const std::uint8_t* source,
                             std::size_t source_bytes, std::uint32_t width,
                             std::uint32_t height,
                             std::uint32_t first_block_row,
                             std::uint32_t block_row_count,
                             std::uint8_t* destination,
                             std::size_t destination_bytes) {
  if (!ImageFits(format, source, source_bytes, width, height, destination,
                 destination_bytes)) {
    return false;
  }
  const std::uint64_t blocks_wide = (static_cast<std::uint64_t>(width) + 3) / 4;
  const std::uint64_t blocks_high =
      (static_cast<std::uint64_t>(height) + 3) / 4;
  if (static_cast<std::uint64_t>(first_block_row) + block_row_count >
      blocks_high) {
    return false;
  }
  const std::size_t texel_bytes = EtcDecodedTexelBytes(format);
  const std::size_t block_bytes = EtcBlockBytes(format);

  alignas(16) std::uint32_t cols[16];
  const std::uint64_t end_block_row =
      static_cast<std::uint64_t>(first_block_row) + block_row_count;
  const bool is_rgba_family = (format == EtcFormat::kEtc2Rgb8 ||
                               format == EtcFormat::kEtc2Rgb8A1 ||
                               format == EtcFormat::kEtc2Rgba8);

  for (std::uint64_t by = first_block_row; by < end_block_row; ++by) {
    for (std::uint64_t bx = 0; bx < blocks_wide; ++bx) {
      const std::uint8_t* block =
          source + (by * blocks_wide + bx) * block_bytes;
      const bool interior = (by * 4 + 4 <= height) && (bx * 4 + 4 <= width);

      if (is_rgba_family) {
        if (format == EtcFormat::kEtc2Rgb8 || format == EtcFormat::kEtc2Rgb8A1) {
          DecodeRgbBlock(block, format == EtcFormat::kEtc2Rgb8A1, cols);
        } else {
          DecodeRgbBlock(block + 8, false, cols);
          const std::uint64_t alpha_bits = ReadEacBits(block);
          const int alpha_base = block[0];
          const int alpha_multiplier = block[1] >> 4;
          const auto& alpha_table = kEacModifiers[block[1] & 15];
          std::uint8_t alpha_palette[8];
          for (int i = 0; i < 8; ++i) {
            alpha_palette[i] =
                Clamp255(alpha_base + alpha_table[i] * alpha_multiplier);
          }
          for (int texel = 0; texel < 16; ++texel) {
            cols[texel] =
                (cols[texel] & 0x00FFFFFF) |
                (static_cast<std::uint32_t>(
                     alpha_palette[EacIndexFromBits(alpha_bits, texel)])
                 << 24);
          }
        }

        if (interior) {
          std::uint32_t* dst_row = reinterpret_cast<std::uint32_t*>(
              destination + (by * 4 * width + bx * 4) * 4);
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
          __m128 r0 = _mm_load_ps(reinterpret_cast<const float*>(cols));
          __m128 r1 = _mm_load_ps(reinterpret_cast<const float*>(cols + 4));
          __m128 r2 = _mm_load_ps(reinterpret_cast<const float*>(cols + 8));
          __m128 r3 = _mm_load_ps(reinterpret_cast<const float*>(cols + 12));
          _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
          _mm_storeu_ps(reinterpret_cast<float*>(dst_row), r0);
          dst_row += width;
          _mm_storeu_ps(reinterpret_cast<float*>(dst_row), r1);
          dst_row += width;
          _mm_storeu_ps(reinterpret_cast<float*>(dst_row), r2);
          dst_row += width;
          _mm_storeu_ps(reinterpret_cast<float*>(dst_row), r3);
#else
          for (int y = 0; y < 4; ++y) {
            dst_row[0] = cols[y];
            dst_row[1] = cols[4 + y];
            dst_row[2] = cols[8 + y];
            dst_row[3] = cols[12 + y];
            dst_row += width;
          }
#endif
        } else {
          for (std::uint32_t y = 0; y < 4 && by * 4 + y < height; ++y) {
            for (std::uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
              const int texel = static_cast<int>(x * 4 + y);
              std::uint32_t* out = reinterpret_cast<std::uint32_t*>(
                  destination + ((by * 4 + y) * width + bx * 4 + x) * 4);
              *out = cols[texel];
            }
          }
        }
      } else {
        const bool is_signed = format == EtcFormat::kEacR11Signed ||
                               format == EtcFormat::kEacRg11Signed;
        const int channels = static_cast<int>(texel_bytes / 2);
        std::uint64_t channel_bits[2] = {0, 0};
        std::uint16_t channel_palette[2][8];
        for (int c = 0; c < channels; ++c) {
          const std::uint8_t* cb = block + c * 8;
          channel_bits[c] = ReadEacBits(cb);
          const int mult = cb[1] >> 4;
          const int* table = kEacModifiers[cb[1] & 15];
          for (int i = 0; i < 8; ++i) {
            channel_palette[c][i] =
                DecodeR11Modifier(cb, mult, table, i, is_signed);
          }
        }
        for (std::uint32_t y = 0; y < 4 && by * 4 + y < height; ++y) {
          for (std::uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
            const int texel = static_cast<int>(x * 4 + y);
            std::uint8_t* out =
                destination + ((by * 4 + y) * width + bx * 4 + x) * texel_bytes;
            for (int channel = 0; channel < channels; ++channel) {
              const std::uint16_t value =
                  channel_palette[channel][EacIndexFromBits(channel_bits[channel],
                                                            texel)];
              out[channel * 2] = static_cast<std::uint8_t>(value);
              out[channel * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
            }
          }
        }
      }
    }
  }
  return true;
}

}  // namespace mocktail::graphics
