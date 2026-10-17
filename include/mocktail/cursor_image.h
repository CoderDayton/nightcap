#ifndef MOCKTAIL_CURSOR_IMAGE_H_
#define MOCKTAIL_CURSOR_IMAGE_H_

#include <cstdint>
#include <vector>

namespace mocktail {

// A game's own cursor picture, shown as the desktop cursor. Tightly packed
// RGBA8 rows, top row first, straight alpha. The hot spot is the pixel that
// sits under the pointer.
struct CursorImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t hot_x = 0;
  std::uint32_t hot_y = 0;
  std::vector<std::uint8_t> rgba;
};

}  // namespace mocktail

#endif  // MOCKTAIL_CURSOR_IMAGE_H_
