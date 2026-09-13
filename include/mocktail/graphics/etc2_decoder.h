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

}  // namespace mocktail::graphics

#endif  // MOCKTAIL_GRAPHICS_ETC2_DECODER_H_
