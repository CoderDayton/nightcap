#ifndef MOCKTAIL_VR_MIRROR_SHM_H_
#define MOCKTAIL_VR_MIRROR_SHM_H_

#include <cstdint>

namespace mocktail::vr {

// Shared-memory transport for the desktop two-eye mirror. The producer (XR
// backend, inside the client process) publishes the latest complete eye pair
// under a seqlock; the consumer (desktop viewer) reads the newest pair without
// any per-frame filesystem transport, image encode/decode, or allocation.
//
// Layout is fixed and little-endian.
// sizeof(VrMirrorHeader) == kVrMirrorHeaderBytes.
struct VrMirrorHeader {
  std::uint32_t magic;          // kVrMirrorMagic
  std::uint32_t header_bytes;   // kVrMirrorHeaderBytes (pixel area offset)
  std::uint64_t seq;            // seqlock: odd while writing, even when stable
  std::uint64_t frame;          // XR frame id the pair belongs to
  std::uint32_t width;          // eye width in pixels
  std::uint32_t height;         // eye height in pixels
  std::uint32_t format;         // VkFormat of the source eye images
  std::uint32_t flags;          // bit0: a complete, valid pair is present
  std::uint64_t producer_ns;    // CLOCK_MONOTONIC publish timestamp
  std::uint64_t published_pairs;
  std::uint64_t reserved;
};

inline constexpr std::uint32_t kVrMirrorMagic = 0x3152564D;  // 'MVR1'
inline constexpr std::uint32_t kVrMirrorHeaderBytes = 64;
inline constexpr std::uint32_t kVrMirrorFlagValid = 1u << 0;

// Total mapped bytes for a given eye extent (header + two RGBA/BGRA images).
inline std::uint64_t VrMirrorBytes(std::uint32_t width, std::uint32_t height) {
  return static_cast<std::uint64_t>(kVrMirrorHeaderBytes) +
         2ull * static_cast<std::uint64_t>(width) *
             static_cast<std::uint64_t>(height) * 4ull;
}

}  // namespace mocktail::vr

#endif  // MOCKTAIL_VR_MIRROR_SHM_H_
