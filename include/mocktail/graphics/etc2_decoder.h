#ifndef MOCKTAIL_GRAPHICS_ETC2_DECODER_H_
#define MOCKTAIL_GRAPHICS_ETC2_DECODER_H_

#include <cstddef>
#include <cstdint>

namespace mocktail::graphics {

// ETC2 and EAC block encodings from the OpenGL ES 3.0 / Vulkan core formats.
// sRGB variants share the unorm encoding; the colour space belongs to the
// destination format.
enum class EtcFormat {
  kEtc2Rgb8,
  kEtc2Rgb8A1,
  kEtc2Rgba8,
  kEacR11,
  kEacR11Signed,
  kEacRg11,
  kEacRg11Signed,
};

// Compressed bytes per 4x4 block.
std::size_t EtcBlockBytes(EtcFormat format);

// Decoded bytes per texel: 4 (RGBA8) for ETC2 formats, 2 per channel
// (16-bit, little-endian) for EAC formats.
std::size_t EtcDecodedTexelBytes(EtcFormat format);

// Decodes a width x height image stored as ceil(width/4) x ceil(height/4)
// blocks into row-major texels. Returns false, writing nothing, when a buffer
// is smaller than the image requires.
bool DecodeEtcImage(EtcFormat format, const std::uint8_t* source,
                    std::size_t source_bytes, std::uint32_t width,
                    std::uint32_t height, std::uint8_t* destination,
                    std::size_t destination_bytes);

// Decodes block rows [first_block_row, first_block_row + block_row_count) of
// the image DecodeEtcImage describes, writing only the texel rows those blocks
// cover. Bands that together cover every block row produce DecodeEtcImage's
// output. Returns false, writing nothing, when a buffer is too small or the
// rows lie outside the image.
bool DecodeEtcImageBlockRows(EtcFormat format, const std::uint8_t* source,
                             std::size_t source_bytes, std::uint32_t width,
                             std::uint32_t height,
                             std::uint32_t first_block_row,
                             std::uint32_t block_row_count,
                             std::uint8_t* destination,
                             std::size_t destination_bytes);

// One DecodeEtcImage call. Jobs must not share destination bytes.
struct EtcDecodeJob {
  EtcFormat format = EtcFormat::kEtc2Rgb8;
  const std::uint8_t* source = nullptr;
  std::size_t source_bytes = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t* destination = nullptr;
  std::size_t destination_bytes = 0;
  // Set to DecodeEtcImage's result.
  bool ok = false;
};

// Decodes every job before returning. Large batches are split into block-row
// bands decoded on up to worker_count threads, the caller included; small
// batches decode on the caller only.
void DecodeEtcJobs(EtcDecodeJob* jobs, std::size_t count,
                   unsigned worker_count);

}  // namespace mocktail::graphics

#endif  // MOCKTAIL_GRAPHICS_ETC2_DECODER_H_
