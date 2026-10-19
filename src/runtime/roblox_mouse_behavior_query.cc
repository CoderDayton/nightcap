#include "runtime/roblox_mouse_behavior_query.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace mocktail::runtime {

namespace {

constexpr std::size_t kMaxFunctionBytes = 256;
constexpr std::uint8_t kCmpImm8 = 0x83;

bool IsCompareRegister(std::uint8_t modrm) { return (modrm & 0xf8) == 0xf8; }

bool IsCompareMemory(std::uint8_t modrm, std::uint8_t mod) {
  return (modrm & 0xf8) == (mod | 0x38) && (modrm & 0x07) != 0x04;
}

bool EndsCompare(const std::uint8_t* code, std::size_t immediate) {
  if (immediate >= 2 && code[immediate - 2] == kCmpImm8 &&
      IsCompareRegister(code[immediate - 1])) {
    return true;
  }
  if (immediate >= 3 && code[immediate - 3] == kCmpImm8 &&
      IsCompareMemory(code[immediate - 2], 0x40)) {
    return true;
  }
  return immediate >= 6 && code[immediate - 6] == kCmpImm8 &&
         IsCompareMemory(code[immediate - 5], 0x80);
}

std::size_t FunctionSize(const std::uint8_t* code, const std::uint8_t* end) {
  const auto available = static_cast<std::size_t>(end - code);
  const std::size_t limit =
      available < kMaxFunctionBytes ? available : kMaxFunctionBytes;
  for (std::size_t i = 1; i < limit; ++i) {
    if (code[i - 1] == 0x5d && code[i] == 0xc3) {
      return i + 1;
    }
  }
  return limit;
}

bool ExecutableRange(std::uintptr_t address, std::uintptr_t* begin,
                     std::uintptr_t* end) {
  std::FILE* maps = std::fopen("/proc/self/maps", "re");
  if (maps == nullptr) {
    return false;
  }
  std::uintptr_t run_begin = 0;
  std::uintptr_t run_end = 0;
  bool found = false;
  char line[512];
  while (std::fgets(line, sizeof(line), maps) != nullptr) {
    std::uintptr_t low = 0;
    std::uintptr_t high = 0;
    char perms[5] = {};
    if (std::sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &low, &high,
                    perms) != 3) {
      continue;
    }
    if (perms[2] != 'x') {
      if (found) {
        break;
      }
      run_end = 0;
      continue;
    }
    if (low != run_end) {
      if (found) {
        break;
      }
      run_begin = low;
    }
    run_end = high;
    found = found || (address >= low && address < high);
  }
  std::fclose(maps);
  if (found) {
    *begin = run_begin;
    *end = run_end;
  }
  return found;
}

}

std::optional<std::size_t> FindLockCenterCompare(const std::uint8_t* code,
                                                 std::size_t size) {
  for (std::size_t i = 0; i + 3 <= size; ++i) {
    if (code[i] == 0x01 && code[i + 1] == 0x0f && code[i + 2] == 0x94 &&
        EndsCompare(code, i)) {
      return i;
    }
  }
  return std::nullopt;
}

std::uint8_t* LocateLockCenterCompare(void* function, const std::uint8_t* begin,
                                      const std::uint8_t* end) {
  auto* code = static_cast<std::uint8_t*>(function);
  if (code < begin || code >= end) {
    return nullptr;
  }
  const std::size_t size = FunctionSize(code, end);
  if (const auto found = FindLockCenterCompare(code, size)) {
    return code + *found;
  }
  for (std::size_t i = 0; i + 5 <= size; ++i) {
    if (code[i] != 0xe8) {
      continue;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, code + i + 1, sizeof(rel));
    const auto address = reinterpret_cast<std::uintptr_t>(code + i + 5) +
                         static_cast<std::intptr_t>(rel);
    if (address < reinterpret_cast<std::uintptr_t>(begin) ||
        address >= reinterpret_cast<std::uintptr_t>(end)) {
      continue;
    }
    auto* target = reinterpret_cast<std::uint8_t*>(address);
    if (const auto found =
            FindLockCenterCompare(target, FunctionSize(target, end))) {
      return target + *found;
    }
  }
  return nullptr;
}

std::uint8_t* PrepareLockCenterCompare(void* function) {
  if (function == nullptr) {
    return nullptr;
  }
  std::uintptr_t begin = 0;
  std::uintptr_t end = 0;
  if (!ExecutableRange(reinterpret_cast<std::uintptr_t>(function), &begin,
                       &end)) {
    return nullptr;
  }
  std::uint8_t* compare =
      LocateLockCenterCompare(function, reinterpret_cast<std::uint8_t*>(begin),
                              reinterpret_cast<std::uint8_t*>(end));
  const long page_size = sysconf(_SC_PAGESIZE);
  if (compare == nullptr || page_size <= 0) {
    return nullptr;
  }
  const auto page = reinterpret_cast<std::uintptr_t>(compare) &
                    ~(static_cast<std::uintptr_t>(page_size) - 1);
  if (mprotect(reinterpret_cast<void*>(page),
               static_cast<std::size_t>(page_size),
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    return nullptr;
  }
  return compare;
}

}
