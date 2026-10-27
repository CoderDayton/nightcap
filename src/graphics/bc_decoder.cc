#include "mocktail/graphics/bc_decoder.h"

#include <algorithm>
#include <utility>

namespace mocktail::graphics {
namespace {

constexpr std::uint16_t kPartition2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80,
    0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE,
    0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A,
    0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C,
    0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};

constexpr std::uint8_t kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
    15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
    6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};

constexpr std::uint32_t kWeights2[4] = {0, 21, 43, 64};
constexpr std::uint32_t kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::uint32_t kWeights4[16] = {0,  4,  9,  13, 17, 21, 26, 30,
                                         34, 38, 43, 47, 51, 55, 60, 64};

class BitReader {
 public:
  explicit BitReader(const std::uint8_t* bytes) : bytes_(bytes) {}
  std::uint32_t Read(std::uint32_t count) {
    std::uint32_t value = 0;
    for (std::uint32_t i = 0; i < count; ++i, ++position_) {
      value |= ((bytes_[position_ >> 3] >> (position_ & 7)) & 1U) << i;
    }
    return value;
  }

 private:
  const std::uint8_t* bytes_;
  std::uint32_t position_ = 0;
};

std::uint32_t Weight(std::uint32_t bits, std::uint32_t index) {
  return bits == 2 ? kWeights2[index]
                   : bits == 3 ? kWeights3[index] : kWeights4[index];
}

std::uint32_t Interpolate(std::uint32_t e0, std::uint32_t e1,
                          std::uint32_t weight) {
  return ((64 - weight) * e0 + weight * e1 + 32) >> 6;
}

std::uint32_t Dequantize(std::uint32_t value, std::uint32_t bits) {
  value <<= 8 - bits;
  return (value | (value >> bits)) & 0xFF;
}

bool DecodeBc7(const std::uint8_t* block, std::uint8_t* texels) {
  std::uint32_t mode = 0;
  while (mode < 8 && ((block[0] >> mode) & 1U) == 0) {
    ++mode;
  }
  if (mode != 1 && mode != 4 && mode != 5 && mode != 6) {
    return false;
  }
  BitReader bits(block);
  bits.Read(mode + 1);
  const std::uint32_t subsets = mode == 1 ? 2 : 1;
  const std::uint32_t partition = mode == 1 ? bits.Read(6) : 0;
  const std::uint32_t rotation = mode == 4 || mode == 5 ? bits.Read(2) : 0;
  const std::uint32_t index_select = mode == 4 ? bits.Read(1) : 0;
  const std::uint32_t color_bits =
      mode == 1 ? 6 : mode == 4 ? 5 : 7;
  const std::uint32_t alpha_bits =
      mode == 4 ? 6 : mode == 5 ? 8 : mode == 6 ? 7 : 0;
  const std::uint32_t endpoints = subsets * 2;

  std::uint32_t e[4][4] = {};
  for (std::uint32_t c = 0; c < 3; ++c) {
    for (std::uint32_t i = 0; i < endpoints; ++i) {
      e[i][c] = bits.Read(color_bits);
    }
  }
  for (std::uint32_t i = 0; i < endpoints; ++i) {
    e[i][3] = alpha_bits != 0 ? bits.Read(alpha_bits) : 255;
  }
  if (mode == 1) {
    for (std::uint32_t s = 0; s < 2; ++s) {
      const std::uint32_t p = bits.Read(1);
      for (std::uint32_t i = s * 2; i < s * 2 + 2; ++i) {
        for (std::uint32_t c = 0; c < 3; ++c) {
          e[i][c] = Dequantize((e[i][c] << 1) | p, 7);
        }
      }
    }
  } else if (mode == 6) {
    for (std::uint32_t i = 0; i < 2; ++i) {
      const std::uint32_t p = bits.Read(1);
      for (std::uint32_t c = 0; c < 4; ++c) {
        e[i][c] = (e[i][c] << 1) | p;
      }
    }
  } else {
    for (std::uint32_t i = 0; i < 2; ++i) {
      for (std::uint32_t c = 0; c < 3; ++c) {
        e[i][c] = Dequantize(e[i][c], color_bits);
      }
      e[i][3] = Dequantize(e[i][3], alpha_bits);
    }
  }

  const std::uint32_t index_bits = mode == 1 ? 3 : mode == 6 ? 4 : 2;
  const std::uint32_t mask = kPartition2[partition];
  const std::uint32_t anchor = mode == 1 ? kAnchor2[partition] : 0;
  std::uint32_t primary[16];
  for (std::uint32_t i = 0; i < 16; ++i) {
    const bool is_anchor = i == 0 || (mode == 1 && i == anchor);
    primary[i] = bits.Read(index_bits - (is_anchor ? 1 : 0));
  }
  std::uint32_t secondary[16] = {};
  const std::uint32_t secondary_bits = mode == 4 ? 3 : mode == 5 ? 2 : 0;
  for (std::uint32_t i = 0; i < 16 && secondary_bits != 0; ++i) {
    secondary[i] = bits.Read(secondary_bits - (i == 0 ? 1 : 0));
  }

  for (std::uint32_t i = 0; i < 16; ++i) {
    std::uint32_t out[4];
    if (mode == 4 || mode == 5) {
      const bool swapped = index_select != 0;
      const std::uint32_t color_index = swapped ? secondary[i] : primary[i];
      const std::uint32_t alpha_index = swapped ? primary[i] : secondary[i];
      const std::uint32_t color_weight =
          Weight(swapped ? secondary_bits : index_bits, color_index);
      const std::uint32_t alpha_weight =
          Weight(swapped ? index_bits : secondary_bits, alpha_index);
      for (std::uint32_t c = 0; c < 3; ++c) {
        out[c] = Interpolate(e[0][c], e[1][c], color_weight);
      }
      out[3] = Interpolate(e[0][3], e[1][3], alpha_weight);
      if (rotation != 0) {
        std::swap(out[3], out[rotation - 1]);
      }
    } else {
      const std::uint32_t subset = mode == 1 ? (mask >> i) & 1U : 0;
      const std::uint32_t weight = Weight(index_bits, primary[i]);
      for (std::uint32_t c = 0; c < 4; ++c) {
        out[c] = Interpolate(e[subset * 2][c], e[subset * 2 + 1][c], weight);
      }
    }
    for (std::uint32_t c = 0; c < 4; ++c) {
      texels[i * 4 + c] = static_cast<std::uint8_t>(out[c]);
    }
  }
  return true;
}

std::int32_t RoundDivide(std::int64_t numerator, std::int64_t denominator) {
  return static_cast<std::int32_t>(
      numerator >= 0 ? (numerator + denominator / 2) / denominator
                     : -((-numerator + denominator / 2) / denominator));
}

// One BC4 channel into 16 texels spaced `stride` bytes apart.
void DecodeBc4(const std::uint8_t* block, bool is_signed, std::uint8_t* out,
               std::size_t stride) {
  std::int32_t r0 = block[0];
  std::int32_t r1 = block[1];
  if (is_signed) {
    r0 = r0 >= 128 ? std::max(r0 - 256, -127) : r0;
    r1 = r1 >= 128 ? std::max(r1 - 256, -127) : r1;
  }
  const std::int32_t low = is_signed ? -127 : 0;
  const std::int32_t high = is_signed ? 127 : 255;
  const bool eight = r0 > r1;
  const std::int32_t steps = eight ? 7 : 5;
  std::uint64_t index_bits = 0;
  for (std::uint32_t i = 0; i < 6; ++i) {
    index_bits |= std::uint64_t{block[2 + i]} << (8 * i);
  }
  for (std::uint32_t i = 0; i < 16; ++i) {
    const std::int32_t index = static_cast<std::int32_t>((index_bits >> (3 * i)) & 7);
    std::int32_t level = 0;
    if (index == 0) {
      level = r0 * steps;
    } else if (index == 1) {
      level = r1 * steps;
    } else if (!eight && index == 6) {
      level = low * steps;
    } else if (!eight && index == 7) {
      level = high * steps;
    } else {
      level = (steps + 1 - index) * r0 + (index - 1) * r1;
    }
    const std::int32_t value =
        is_signed ? RoundDivide(std::int64_t{level} * 32767, steps * 127)
                  : RoundDivide(std::int64_t{level} * 65535, steps * 255);
    const std::uint32_t bits = static_cast<std::uint32_t>(value) & 0xFFFF;
    out[i * stride] = static_cast<std::uint8_t>(bits);
    out[i * stride + 1] = static_cast<std::uint8_t>(bits >> 8);
  }
}

}

BcFormat BcFormatFor(EtcFormat format) {
  switch (format) {
    case EtcFormat::kEacR11:
      return BcFormat::kBc4;
    case EtcFormat::kEacR11Signed:
      return BcFormat::kBc4Signed;
    case EtcFormat::kEacRg11:
      return BcFormat::kBc5;
    case EtcFormat::kEacRg11Signed:
      return BcFormat::kBc5Signed;
    default:
      return BcFormat::kBc7;
  }
}

std::size_t BcBlockBytes(BcFormat format) {
  return format == BcFormat::kBc4 || format == BcFormat::kBc4Signed ? 8 : 16;
}

bool DecodeBcBlock(BcFormat format, const std::uint8_t* block,
                   std::uint8_t* texels) {
  switch (format) {
    case BcFormat::kBc7:
      return DecodeBc7(block, texels);
    case BcFormat::kBc4:
    case BcFormat::kBc4Signed:
      DecodeBc4(block, format == BcFormat::kBc4Signed, texels, 2);
      return true;
    case BcFormat::kBc5:
    case BcFormat::kBc5Signed:
      DecodeBc4(block, format == BcFormat::kBc5Signed, texels, 4);
      DecodeBc4(block + 8, format == BcFormat::kBc5Signed, texels + 2, 4);
      return true;
  }
  return false;
}

}
