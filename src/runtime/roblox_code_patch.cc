#include "runtime/roblox_code_patch.h"

#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace mocktail {
namespace runtime {

std::vector<CursorHookSegment> LoadedImageSegments(std::uintptr_t image_base,
                                                   std::uintptr_t* image_end) {
  std::vector<CursorHookSegment> segments;
  *image_end = image_base;
  const auto* header = reinterpret_cast<const Elf64_Ehdr*>(image_base);
  if (image_base == 0 ||
      std::memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
      header->e_machine != EM_X86_64) {
    return segments;
  }
  const auto* program_headers =
      reinterpret_cast<const Elf64_Phdr*>(image_base + header->e_phoff);
  for (std::size_t index = 0; index < header->e_phnum; ++index) {
    const Elf64_Phdr& segment = program_headers[index];
    if (segment.p_type != PT_LOAD || segment.p_filesz == 0) {
      continue;
    }
    const std::uintptr_t address = image_base + segment.p_vaddr;
    segments.push_back({address, reinterpret_cast<const std::uint8_t*>(address),
                        segment.p_filesz, (segment.p_flags & PF_X) != 0});
    *image_end = std::max<std::uintptr_t>(*image_end, address + segment.p_memsz);
  }
  return segments;
}

const CursorHookSegment* SegmentContaining(
    const std::vector<CursorHookSegment>& segments, std::uintptr_t address) {
  for (const CursorHookSegment& segment : segments) {
    if (address >= segment.address && address - segment.address < segment.size) {
      return &segment;
    }
  }
  return nullptr;
}

std::string_view GuestString(std::uintptr_t string) {
  if (string == 0) {
    return {};
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(string);
  if ((bytes[0] & 1) == 0) {
    return {reinterpret_cast<const char*>(bytes + 1),
            static_cast<std::size_t>(bytes[0] >> 1)};
  }
  std::size_t size = 0;
  const char* data = nullptr;
  std::memcpy(&size, bytes + 8, sizeof(size));
  std::memcpy(&data, bytes + 16, sizeof(data));
  if (data == nullptr || size > 4096) {
    return {};
  }
  return {data, size};
}

std::uint8_t* AllocateNearImage(std::uintptr_t low, std::uintptr_t high,
                                std::size_t size) {
  constexpr std::uintptr_t kStep = 1u << 20;
  constexpr std::uintptr_t kReach = 0x7FF00000;
  const std::uintptr_t above = (high + kStep - 1) & ~(kStep - 1);
  const std::uintptr_t below = low & ~(kStep - 1);
  for (std::uintptr_t distance = 0; distance < 0x40000000;
       distance += kStep) {
    for (const std::uintptr_t hint :
         {above + distance,
          below > distance + kStep ? below - distance - kStep : 0}) {
      if (hint == 0) {
        continue;
      }
      void* memory = mmap(reinterpret_cast<void*>(hint), size,
                          PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                          -1, 0);
      if (memory == MAP_FAILED) {
        continue;
      }
      const auto address = reinterpret_cast<std::uintptr_t>(memory);
      const std::uintptr_t end = address + size;
      if (address >= high ? end - low < kReach : high - address < kReach) {
        return static_cast<std::uint8_t*>(memory);
      }
      munmap(memory, size);
    }
  }
  return nullptr;
}

bool WriteCode(std::uintptr_t address, const std::uint8_t* bytes,
               std::size_t size) {
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return false;
  }
  const std::uintptr_t mask = static_cast<std::uintptr_t>(page_size) - 1;
  const std::uintptr_t begin = address & ~mask;
  const std::size_t span = ((address + size + mask) & ~mask) - begin;
  if (mprotect(reinterpret_cast<void*>(begin), span, PROT_READ | PROT_WRITE) !=
      0) {
    return false;
  }
  std::memcpy(reinterpret_cast<void*>(address), bytes, size);
  __builtin___clear_cache(reinterpret_cast<char*>(address),
                          reinterpret_cast<char*>(address + size));
  return mprotect(reinterpret_cast<void*>(begin), span,
                  PROT_READ | PROT_EXEC) == 0;
}

void EncodeRel32(std::uint8_t opcode, std::uintptr_t from,
                 std::uintptr_t target, std::uint8_t* out) {
  const auto displacement = static_cast<std::int32_t>(
      static_cast<std::intptr_t>(target) -
      static_cast<std::intptr_t>(from + 5));
  out[0] = opcode;
  std::memcpy(out + 1, &displacement, sizeof(displacement));
}

void EncodeAbsoluteJump(std::uintptr_t target, std::uint8_t* out) {
  constexpr std::uint8_t kJump[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
  std::memcpy(out, kJump, sizeof(kJump));
  std::memcpy(out + sizeof(kJump), &target, sizeof(target));
}

bool FitsRel32(std::uintptr_t from, std::uintptr_t target) {
  const std::intptr_t distance = static_cast<std::intptr_t>(target) -
                                 static_cast<std::intptr_t>(from + 5);
  return distance >= std::numeric_limits<std::int32_t>::min() &&
         distance <= std::numeric_limits<std::int32_t>::max();
}

}  // namespace runtime
}  // namespace mocktail
