#include "runtime/roblox_sky_hook.h"

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kNamePrefix = "Sky.Skybox";
// The first eight bytes of the name, which each caller loads as an immediate
// to build the short string in place.
constexpr std::string_view kNameImmediate = "Sky.Skyb";
// The call follows the immediate by 0x38 to 0x42 bytes in build 20836.
constexpr std::size_t kMaxCallDistance = 0x80;
// Twelve in build 20836: six in each of the two sky renderers.
constexpr std::size_t kMinCallers = 6;

// push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx;
// sub rsp,imm32. The load function opens with it.
constexpr std::array<std::uint8_t, 16> kPrologue = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC};
// push rbp; mov rbp,rsp; push r15: whole instructions, none RIP-relative,
// so they run unchanged from the trampoline.
constexpr std::size_t kRelocatedPrologueBytes = 6;

constexpr std::array<std::string_view, 6> kFaces = {"bk", "dn", "ft",
                                                    "lf", "rt", "up"};

// Roblox's Content: a type, worked out from the ID while its low four bits
// are 0, then a libc++ std::string in its long form.
struct GuestContent {
  std::uint64_t type = 0;
  std::uint64_t capacity = 0;
  std::uint64_t size = 0;
  const char* data = nullptr;
};
static_assert(sizeof(GuestContent) == 0x20);

// The result goes through `out`; the float and both stack arguments are
// passed on as they came.
using LoadFunction = std::uintptr_t (*)(std::uintptr_t out,
                                        std::uintptr_t manager,
                                        std::uintptr_t content,
                                        std::uintptr_t fallback,
                                        std::uintptr_t d, std::uintptr_t e,
                                        float f, std::uintptr_t debug_name,
                                        std::uintptr_t g);

LoadFunction g_original_load = nullptr;
std::atomic<bool> g_installed{false};
std::array<std::string, kFaces.size()>& g_face_ids =
    *new std::array<std::string, kFaces.size()>;
std::array<GuestContent, kFaces.size()> g_face_content;
// Hash of the content last logged for each face: one line per sky, not per load.
std::array<std::atomic<std::size_t>, kFaces.size()> g_face_logged;

bool Fail(std::string* error, const char* message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

std::uintptr_t LoadHook(std::uintptr_t out, std::uintptr_t manager,
                        std::uintptr_t content, std::uintptr_t fallback,
                        std::uintptr_t d, std::uintptr_t e, float f,
                        std::uintptr_t debug_name, std::uintptr_t g) {
  const std::string_view face = SkyFaceOfLoad(GuestString(debug_name));
  if (!face.empty()) {
    for (std::size_t index = 0; index < kFaces.size(); ++index) {
      if (kFaces[index] != face) {
        continue;
      }
      const std::string_view replaced =
          content != 0 ? GuestString(content + 8) : std::string_view();
      const std::size_t hash = std::hash<std::string_view>{}(replaced) | 1;
      if (g_face_logged[index].exchange(hash) != hash) {
        std::fprintf(stderr, "  [sky] custom sky face %s drawn in place of %.*s\n",
                     kFaces[index].data(), static_cast<int>(replaced.size()),
                     replaced.data());
      }
      content = reinterpret_cast<std::uintptr_t>(&g_face_content[index]);
      break;
    }
  }
  return g_original_load(out, manager, content, fallback, d, e, f, debug_name,
                         g);
}

}  // namespace

std::string_view SkyFaceOfLoad(std::string_view debug_name) {
  if (debug_name.size() != kNamePrefix.size() + 2 ||
      debug_name.substr(0, kNamePrefix.size()) != kNamePrefix) {
    return {};
  }
  for (const std::string_view face : kFaces) {
    if (debug_name[kNamePrefix.size()] == face[0] - 'a' + 'A' &&
        debug_name[kNamePrefix.size() + 1] == face[1]) {
      return face;
    }
  }
  return {};
}

std::string CustomSkyFaceContent(std::string_view face) {
  return "rbxasset://textures/sky/sky512_" + std::string(face) + ".tex";
}

std::optional<std::uintptr_t> LocateSkyTextureLoad(
    const std::vector<CursorHookSegment>& segments, std::string* error) {
  std::size_t callers = 0;
  std::uintptr_t load = 0;
  for (const CursorHookSegment& code : segments) {
    if (!code.executable) {
      continue;
    }
    std::size_t offset = 2;
    while (offset + kNameImmediate.size() <= code.size) {
      const void* match =
          memmem(code.data + offset, code.size - offset, kNameImmediate.data(),
                 kNameImmediate.size());
      if (match == nullptr) {
        break;
      }
      const std::size_t at = static_cast<const std::uint8_t*>(match) - code.data;
      offset = at + 1;
      // movabs r64,imm64
      if ((code.data[at - 2] & 0xFE) != 0x48 ||
          (code.data[at - 1] & 0xF8) != 0xB8) {
        continue;
      }
      const std::size_t begin = at + kNameImmediate.size();
      const std::size_t end = std::min(code.size, begin + kMaxCallDistance);
      for (std::size_t call = begin; call + 5 <= end; ++call) {
        if (code.data[call] != 0xE8) {
          continue;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, code.data + call + 1, sizeof(displacement));
        const std::uintptr_t target =
            code.address + call + 5 + static_cast<std::intptr_t>(displacement);
        const CursorHookSegment* callee = SegmentContaining(segments, target);
        if (callee == nullptr || !callee->executable ||
            target - callee->address + kPrologue.size() > callee->size ||
            std::memcmp(callee->data + (target - callee->address),
                        kPrologue.data(), kPrologue.size()) != 0) {
          continue;
        }
        if (load != 0 && load != target) {
          Fail(error, "sky face loads call more than one function");
          return std::nullopt;
        }
        load = target;
        ++callers;
        break;
      }
    }
  }
  if (callers < kMinCallers) {
    Fail(error, "sky face loads not found");
    return std::nullopt;
  }
  return load;
}

bool InstallRobloxSkyHook(std::uintptr_t image_base, std::string* error) {
  if (g_installed.load(std::memory_order_acquire)) {
    return Fail(error, "already installed");
  }
  std::uintptr_t image_end = image_base;
  const std::vector<CursorHookSegment> segments =
      LoadedImageSegments(image_base, &image_end);
  if (segments.empty()) {
    return Fail(error, "not an x86-64 ELF image");
  }
  const std::optional<std::uintptr_t> load =
      LocateSkyTextureLoad(segments, error);
  if (!load.has_value()) {
    return false;
  }
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return Fail(error, "no page size");
  }
  std::uint8_t* stub = AllocateNearImage(image_base, image_end,
                                         static_cast<std::size_t>(page_size));
  if (stub == nullptr) {
    return Fail(error, "no memory within reach of the image");
  }
  const auto stub_address = reinterpret_cast<std::uintptr_t>(stub);
  // [0] the relocated prologue, then a jump back into the load function.
  // [16] an absolute jump to the hook.
  constexpr std::size_t kHookSlot = 16;
  std::memcpy(stub, reinterpret_cast<const void*>(*load),
              kRelocatedPrologueBytes);
  EncodeRel32(0xE9, stub_address + kRelocatedPrologueBytes,
              *load + kRelocatedPrologueBytes, stub + kRelocatedPrologueBytes);
  EncodeAbsoluteJump(reinterpret_cast<std::uintptr_t>(&LoadHook),
                     stub + kHookSlot);
  if (!FitsRel32(*load, stub_address + kHookSlot) ||
      !FitsRel32(stub_address + kRelocatedPrologueBytes,
                 *load + kRelocatedPrologueBytes) ||
      mprotect(stub, static_cast<std::size_t>(page_size),
               PROT_READ | PROT_EXEC) != 0) {
    munmap(stub, static_cast<std::size_t>(page_size));
    return Fail(error, "trampoline out of reach");
  }

  for (std::size_t index = 0; index < kFaces.size(); ++index) {
    g_face_ids[index] = CustomSkyFaceContent(kFaces[index]);
    GuestContent& content = g_face_content[index];
    // The low bit marks the long form; Roblox never frees or grows this one.
    content.capacity = (g_face_ids[index].size() + 1) | 1;
    content.size = g_face_ids[index].size();
    content.data = g_face_ids[index].c_str();
  }
  g_original_load = reinterpret_cast<LoadFunction>(stub_address);

  // A jump over push rbp, mov rbp,rsp; the byte left of push r15 becomes nop.
  std::uint8_t entry[kRelocatedPrologueBytes];
  EncodeRel32(0xE9, *load, stub_address + kHookSlot, entry);
  entry[5] = 0x90;
  if (!WriteCode(*load, entry, sizeof(entry))) {
    return Fail(error, "could not patch the load function");
  }
  g_installed.store(true, std::memory_order_release);
  std::fprintf(stderr, "  [sky] custom sky hook installed: load @+0x%lx\n",
               static_cast<unsigned long>(*load - image_base));
  return true;
}

}  // namespace runtime
}  // namespace mocktail
