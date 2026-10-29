#ifndef MOCKTAIL_GRAPHICS_BC_DECODER_H_
#define MOCKTAIL_GRAPHICS_BC_DECODER_H_

#include <cstddef>
#include <cstdint>

#include "mocktail/graphics/etc2_decoder.h"

namespace mocktail::graphics {

enum class BcFormat {
  kBc4,
  kBc4Signed,
  kBc5,
  kBc5Signed,
  kBc7,
};

// The BC format an ETC2/EAC format is transcoded to: BC7 for colour, BC4 for
// one EAC channel and BC5 for two.
BcFormat BcFormatFor(EtcFormat format);

std::size_t BcBlockBytes(BcFormat format);

// Decodes one block into 16 row-major texels, in the layout DecodeEtcImage
// writes for the matching ETC2/EAC format: RGBA8 for BC7, 16-bit unorm or
// snorm per channel for BC4 and BC5. BC7 covers modes 1, 4, 5 and 6, the
// ones the GPU transcoder writes; any other mode returns false.
bool DecodeBcBlock(BcFormat format, const std::uint8_t* block,
                   std::uint8_t* texels);

}

#endif
