#ifndef MOCKTAIL_RUNTIME_ROBLOX_CODE_PATCH_H_
#define MOCKTAIL_RUNTIME_ROBLOX_CODE_PATCH_H_

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mocktail {
namespace runtime {

// One loaded ELF segment, addressed as the code that runs from it sees it.
struct CursorHookSegment {
  std::uintptr_t address = 0;
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
  bool executable = false;
};

// The file-backed PT_LOAD segments of the x86-64 ELF image at `image_base`,
// with `image_end` set past the last one. Empty for anything else.
std::vector<CursorHookSegment> LoadedImageSegments(std::uintptr_t image_base,
                                                   std::uintptr_t* image_end);

const CursorHookSegment* SegmentContaining(
    const std::vector<CursorHookSegment>& segments, std::uintptr_t address);

// A libc++ std::string in Roblox's memory, or empty for one that is not
// readable as such.
std::string_view GuestString(std::uintptr_t string);

// Memory for trampolines and jump slots, within a rel32 jump of every
// address in [low, high). Null when there is none.
std::uint8_t* AllocateNearImage(std::uintptr_t low, std::uintptr_t high,
                                std::size_t size);

bool WriteCode(std::uintptr_t address, const std::uint8_t* bytes,
               std::size_t size);

// `opcode` rel32, as a 5-byte call (0xE8) or jump (0xE9) at `from`.
void EncodeRel32(std::uint8_t opcode, std::uintptr_t from,
                 std::uintptr_t target, std::uint8_t* out);

// jmp qword [rip+0]; .quad target. 14 bytes.
void EncodeAbsoluteJump(std::uintptr_t target, std::uint8_t* out);

bool FitsRel32(std::uintptr_t from, std::uintptr_t target);

}  // namespace runtime
}  // namespace mocktail

#endif
