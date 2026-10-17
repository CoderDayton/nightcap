#include "runtime/roblox_cursor_hook.h"

#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kAnchor = "[FLog::CursorCache] Invalid asset ID: %s";

// push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx;
// sub rsp,imm32. The draw function opens with it.
constexpr std::array<std::uint8_t, 16> kPrologue = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC};
// push rbp; mov rbp,rsp; push r15: whole instructions, none RIP-relative,
// so they run unchanged from the trampoline.
constexpr std::size_t kRelocatedPrologueBytes = 6;
// The anchor sits 0x997 bytes into the function in build 3222.
constexpr std::size_t kMaxAnchorDistance = 0x1000;
// The draw site sits 0x141b bytes into it.
constexpr std::size_t kMaxDrawSiteDistance = 0x2800;
// mov rdi,[rbp+disp32]; call rel32; cmp ax,2; je rel32. The content type 2
// is empty content, which Roblox does not draw.
constexpr std::size_t kDrawSiteBytes = 18;
constexpr std::size_t kDrawSiteCallOffset = 7;
constexpr std::uint16_t kEmptyContentType = 2;

constexpr std::string_view kStockCursorDirectories[] = {
    "textures/",
    "textures/Cursors/KeyboardMouse/",
};

struct StockCursor {
  std::string_view file;
  RobloxCursorKind kind;
};
constexpr StockCursor kStockCursors[] = {
    {"ArrowFarCursor.png", RobloxCursorKind::kArrow},
    {"ArrowCursor.png", RobloxCursorKind::kPointer},
    {"IBeamCursor.png", RobloxCursorKind::kText},
};

struct SystemCursorName {
  std::string_view name;
  RobloxCursorKind kind;
};
// Names Roblox picks when its ImprovedCursors flag is on.
constexpr SystemCursorName kSystemCursorNames[] = {
    {"SystemCursors/Arrow", RobloxCursorKind::kArrow},
    {"SystemCursors/PointingHand", RobloxCursorKind::kPointer},
    {"SystemCursors/OpenHand", RobloxCursorKind::kPointer},
    {"SystemCursors/ClosedHand", RobloxCursorKind::kPointer},
    {"SystemCursors/IBeam", RobloxCursorKind::kText},
};

constexpr std::uint8_t kTransparentCursorPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40,
    0x08, 0x06, 0x00, 0x00, 0x00, 0xaa, 0x69, 0x71, 0xde, 0x00, 0x00, 0x00,
    0x27, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0xed, 0xc1, 0x01, 0x0d, 0x00,
    0x00, 0x00, 0xc2, 0xa0, 0xf7, 0x4f, 0x6d, 0x0e, 0x37, 0xa0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x77, 0x03, 0x40, 0x40, 0x00, 0x01, 0xaf, 0x7a, 0x0e, 0xe8,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

bool ConsumePrefix(std::string_view* text, std::string_view prefix) {
  if (text->substr(0, prefix.size()) != prefix) {
    return false;
  }
  text->remove_prefix(prefix.size());
  return true;
}

bool EndsWithPathSuffix(std::string_view path, std::string_view suffix) {
  if (path.size() < suffix.size() ||
      path.substr(path.size() - suffix.size()) != suffix) {
    return false;
  }
  return path.size() == suffix.size() ||
         path[path.size() - suffix.size() - 1] == '/';
}

std::int32_t ReadInt32(const std::uint8_t* bytes) {
  std::int32_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
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

std::vector<std::uintptr_t> FindAnchorStrings(
    const std::vector<CursorHookSegment>& segments) {
  std::vector<std::uintptr_t> found;
  const std::string needle = std::string(kAnchor) + '\0';
  for (const CursorHookSegment& segment : segments) {
    std::size_t offset = 0;
    while (offset + needle.size() <= segment.size) {
      const void* match =
          memmem(segment.data + offset, segment.size - offset, needle.data(),
                 needle.size());
      if (match == nullptr) {
        break;
      }
      const std::size_t at =
          static_cast<const std::uint8_t*>(match) - segment.data;
      if (at == 0 || segment.data[at - 1] == '\0') {
        found.push_back(segment.address + at);
      }
      offset = at + 1;
    }
  }
  return found;
}

// Every `lea r64,[rip+disp32]` that loads `target`.
std::vector<std::uintptr_t> FindRipRelativeLoads(
    const std::vector<CursorHookSegment>& segments, std::uintptr_t target) {
  std::vector<std::uintptr_t> found;
  for (const CursorHookSegment& segment : segments) {
    if (!segment.executable || segment.size < 7) {
      continue;
    }
    const std::uint8_t* const data = segment.data;
    std::size_t offset = 1;
    while (offset + 6 <= segment.size) {
      const void* opcode =
          std::memchr(data + offset, 0x8D, segment.size - 6 - offset);
      if (opcode == nullptr) {
        break;
      }
      const std::size_t at =
          static_cast<const std::uint8_t*>(opcode) - data - 1;
      offset = at + 2;
      if ((data[at] & 0xFB) != 0x48 || (data[at + 2] & 0xC7) != 0x05) {
        continue;
      }
      const std::uintptr_t next = segment.address + at + 7;
      if (next + static_cast<std::intptr_t>(ReadInt32(data + at + 3)) ==
          target) {
        found.push_back(segment.address + at);
      }
    }
  }
  return found;
}

bool Fail(std::string* error, const char* message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

// Hook state. Roblox draws the cursor from one render thread, and the draw
// function reaches the content-type call on that same thread.
using ForwardFunction = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t,
                                           std::uintptr_t, std::uintptr_t,
                                           std::uintptr_t, std::uintptr_t);
ForwardFunction g_original_render = nullptr;
ForwardFunction g_original_content_type = nullptr;
RobloxCursorObserver g_observer = nullptr;
std::atomic<bool> g_installed{false};
bool g_in_render = false;
RobloxCursorKind g_drawn = RobloxCursorKind::kOther;
bool g_trace = false;
std::string g_traced_content;
std::string g_drawn_content;

// The content is a 16-bit type followed by a libc++ std::string.
std::string_view GuestContentId(std::uintptr_t content) {
  if (content == 0) {
    return {};
  }
  const auto* string = reinterpret_cast<const std::uint8_t*>(content + 8);
  if ((string[0] & 1) == 0) {
    return {reinterpret_cast<const char*>(string + 1),
            static_cast<std::size_t>(string[0] >> 1)};
  }
  std::size_t size = 0;
  const char* data = nullptr;
  std::memcpy(&size, string + 8, sizeof(size));
  std::memcpy(&data, string + 16, sizeof(data));
  if (data == nullptr || size > 4096) {
    return {};
  }
  return {data, size};
}

std::uintptr_t ContentTypeHook(std::uintptr_t a, std::uintptr_t b,
                               std::uintptr_t c, std::uintptr_t d,
                               std::uintptr_t e, std::uintptr_t f) {
  const std::uintptr_t type = g_original_content_type(a, b, c, d, e, f);
  if (g_in_render &&
      static_cast<std::uint16_t>(type) != kEmptyContentType) {
    const std::string_view content = GuestContentId(a);
    g_drawn = ClassifyRobloxCursorContent(content);
    if (g_trace) {
      g_drawn_content.assign(content.data(), content.size());
    }
  }
  return type;
}

std::uintptr_t RenderHook(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c,
                          std::uintptr_t d, std::uintptr_t e,
                          std::uintptr_t f) {
  g_drawn = RobloxCursorKind::kOther;
  g_drawn_content.clear();
  g_in_render = true;
  const std::uintptr_t result = g_original_render(a, b, c, d, e, f);
  g_in_render = false;
  if (g_trace && g_drawn_content != g_traced_content) {
    g_traced_content = g_drawn_content;
    std::fprintf(stderr, "  [cursor] Roblox draws %s\n",
                 g_traced_content.empty() ? "no cursor"
                                          : g_traced_content.c_str());
  }
  if (g_observer != nullptr) {
    g_observer(g_drawn);
  }
  return result;
}

// Memory for the trampoline and jump slots, within a rel32 jump of every
// address in [low, high).
std::uint8_t* AllocateNear(std::uintptr_t low, std::uintptr_t high,
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

// jmp qword [rip+0]; .quad target
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

}  // namespace

RobloxCursorKind ClassifyRobloxCursorContent(std::string_view content) {
  std::string_view path = content;
  // The asset overlay unmaps a download name by overwriting its first byte.
  const bool local_asset = ConsumePrefix(&path, "rbxasset://") ||
                           ConsumePrefix(&path, "!bxasset://");
  for (const SystemCursorName& name : kSystemCursorNames) {
    if (path == name.name) {
      return name.kind;
    }
  }
  if (!local_asset) {
    return RobloxCursorKind::kOther;
  }
  for (const std::string_view directory : kStockCursorDirectories) {
    std::string_view file = path;
    if (!ConsumePrefix(&file, directory)) {
      continue;
    }
    for (const StockCursor& cursor : kStockCursors) {
      if (file == cursor.file) {
        return cursor.kind;
      }
    }
  }
  return RobloxCursorKind::kOther;
}

bool IsStockSystemCursorAsset(std::string_view path) {
  for (const std::string_view directory : kStockCursorDirectories) {
    for (const StockCursor& cursor : kStockCursors) {
      if (EndsWithPathSuffix(path, std::string(directory) +
                                       std::string(cursor.file))) {
        return true;
      }
    }
  }
  return false;
}

std::vector<std::string> StockSystemCursorAssetNames() {
  std::vector<std::string> names;
  for (const std::string_view directory : kStockCursorDirectories) {
    for (const StockCursor& cursor : kStockCursors) {
      names.push_back("rbxasset://" + std::string(directory) +
                      std::string(cursor.file));
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

const std::uint8_t* TransparentCursorPng(std::size_t* size) {
  if (size != nullptr) {
    *size = sizeof(kTransparentCursorPng);
  }
  return kTransparentCursorPng;
}

std::optional<CursorRenderSite> LocateCursorRenderSite(
    const std::vector<CursorHookSegment>& segments, std::string* error) {
  const std::vector<std::uintptr_t> strings = FindAnchorStrings(segments);
  if (strings.size() != 1) {
    Fail(error, "cursor cache message not found exactly once");
    return std::nullopt;
  }
  const std::vector<std::uintptr_t> loads =
      FindRipRelativeLoads(segments, strings.front());
  if (loads.size() != 1) {
    Fail(error, "cursor cache message not loaded exactly once");
    return std::nullopt;
  }
  const std::uintptr_t anchor = loads.front();
  const CursorHookSegment* code = SegmentContaining(segments, anchor);
  if (code == nullptr) {
    Fail(error, "cursor cache message load is outside the image");
    return std::nullopt;
  }

  const std::size_t anchor_offset = anchor - code->address;
  std::optional<std::size_t> start;
  for (std::size_t back = 0;
       back <= std::min(anchor_offset, kMaxAnchorDistance); ++back) {
    const std::size_t at = anchor_offset - back;
    if (at + kPrologue.size() <= code->size &&
        std::memcmp(code->data + at, kPrologue.data(), kPrologue.size()) ==
            0) {
      start = at;
      break;
    }
  }
  if (!start.has_value()) {
    Fail(error, "cursor draw function prologue not found");
    return std::nullopt;
  }

  const std::size_t end =
      std::min(code->size, *start + kMaxDrawSiteDistance) - kDrawSiteBytes;
  std::optional<std::size_t> site;
  for (std::size_t at = anchor_offset; at <= end; ++at) {
    const std::uint8_t* bytes = code->data + at;
    if (bytes[0] == 0x48 && bytes[1] == 0x8B && bytes[2] == 0xBD &&
        bytes[7] == 0xE8 && bytes[12] == 0x66 && bytes[13] == 0x83 &&
        bytes[14] == 0xF8 && bytes[15] == kEmptyContentType &&
        bytes[16] == 0x0F && bytes[17] == 0x84) {
      if (site.has_value()) {
        Fail(error, "cursor draw site is ambiguous");
        return std::nullopt;
      }
      site = at;
    }
  }
  if (!site.has_value()) {
    Fail(error, "cursor draw site not found");
    return std::nullopt;
  }

  CursorRenderSite result;
  result.render_function = code->address + *start;
  result.content_type_call = code->address + *site + kDrawSiteCallOffset;
  result.content_type_function =
      result.content_type_call + 5 +
      static_cast<std::intptr_t>(
          ReadInt32(code->data + *site + kDrawSiteCallOffset + 1));
  const CursorHookSegment* callee =
      SegmentContaining(segments, result.content_type_function);
  if (callee == nullptr || !callee->executable) {
    Fail(error, "cursor content type call leaves the code");
    return std::nullopt;
  }
  return result;
}

bool InstallRobloxCursorHooks(std::uintptr_t image_base,
                              RobloxCursorObserver observer,
                              std::string* error) {
  if (image_base == 0 || observer == nullptr) {
    return Fail(error, "no image or observer");
  }
  if (g_installed.load(std::memory_order_acquire)) {
    return Fail(error, "already installed");
  }
  const auto* header = reinterpret_cast<const Elf64_Ehdr*>(image_base);
  if (std::memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
      header->e_machine != EM_X86_64) {
    return Fail(error, "not an x86-64 ELF image");
  }
  const auto* program_headers =
      reinterpret_cast<const Elf64_Phdr*>(image_base + header->e_phoff);
  std::vector<CursorHookSegment> segments;
  std::uintptr_t image_end = image_base;
  for (std::size_t index = 0; index < header->e_phnum; ++index) {
    const Elf64_Phdr& segment = program_headers[index];
    if (segment.p_type != PT_LOAD || segment.p_filesz == 0) {
      continue;
    }
    const std::uintptr_t address = image_base + segment.p_vaddr;
    segments.push_back({address, reinterpret_cast<const std::uint8_t*>(address),
                        segment.p_filesz, (segment.p_flags & PF_X) != 0});
    image_end = std::max<std::uintptr_t>(image_end, address + segment.p_memsz);
  }
  const std::optional<CursorRenderSite> site =
      LocateCursorRenderSite(segments, error);
  if (!site.has_value()) {
    return false;
  }

  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return Fail(error, "no page size");
  }
  std::uint8_t* stub = AllocateNear(image_base, image_end,
                                    static_cast<std::size_t>(page_size));
  if (stub == nullptr) {
    return Fail(error, "no memory within reach of the image");
  }
  const auto stub_address = reinterpret_cast<std::uintptr_t>(stub);
  // [0] the relocated prologue, then a jump back into the draw function.
  // [16] and [32] absolute jumps to the two hooks.
  constexpr std::size_t kRenderSlot = 16;
  constexpr std::size_t kContentTypeSlot = 32;
  std::memcpy(stub, reinterpret_cast<const void*>(site->render_function),
              kRelocatedPrologueBytes);
  EncodeRel32(0xE9, stub_address + kRelocatedPrologueBytes,
              site->render_function + kRelocatedPrologueBytes,
              stub + kRelocatedPrologueBytes);
  EncodeAbsoluteJump(reinterpret_cast<std::uintptr_t>(&RenderHook),
                     stub + kRenderSlot);
  EncodeAbsoluteJump(reinterpret_cast<std::uintptr_t>(&ContentTypeHook),
                     stub + kContentTypeSlot);
  if (!FitsRel32(site->render_function, stub_address + kRenderSlot) ||
      !FitsRel32(site->content_type_call, stub_address + kContentTypeSlot) ||
      !FitsRel32(stub_address + kRelocatedPrologueBytes,
                 site->render_function + kRelocatedPrologueBytes) ||
      mprotect(stub, static_cast<std::size_t>(page_size),
               PROT_READ | PROT_EXEC) != 0) {
    munmap(stub, static_cast<std::size_t>(page_size));
    return Fail(error, "trampoline out of reach");
  }

  g_original_render = reinterpret_cast<ForwardFunction>(stub_address);
  g_original_content_type =
      reinterpret_cast<ForwardFunction>(site->content_type_function);
  g_observer = observer;
  g_trace = std::getenv("MOCKTAIL_CURSOR_TRACE") != nullptr;

  std::uint8_t call[5];
  EncodeRel32(0xE8, site->content_type_call, stub_address + kContentTypeSlot,
              call);
  // A jump over push rbp, mov rbp,rsp; the byte left of push r15 becomes nop.
  std::uint8_t entry[kRelocatedPrologueBytes];
  EncodeRel32(0xE9, site->render_function, stub_address + kRenderSlot, entry);
  entry[5] = 0x90;
  if (!WriteCode(site->content_type_call, call, sizeof(call))) {
    return Fail(error, "could not patch the draw site");
  }
  if (!WriteCode(site->render_function, entry, sizeof(entry))) {
    // The draw-site hook alone only records what is drawn; with no render
    // hook nothing reads it or reports it.
    return Fail(error, "could not patch the draw function");
  }
  g_installed.store(true, std::memory_order_release);
  std::fprintf(stderr,
               "  [cursor] system cursor hooks installed: draw @+0x%lx, "
               "content @+0x%lx\n",
               static_cast<unsigned long>(site->render_function - image_base),
               static_cast<unsigned long>(site->content_type_call -
                                          image_base));
  return true;
}

bool RobloxCursorHooksInstalled() {
  return g_installed.load(std::memory_order_acquire);
}

}  // namespace runtime
}  // namespace mocktail

// The Android asset stub asks this before it opens a file. Returns the
// transparent stand-in for a stock cursor texture once the hooks are in, so
// Roblox draws nothing under the system cursor; null otherwise.
extern "C" __attribute__((visibility("default"))) const unsigned char*
mocktail_system_cursor_asset(const char* path, std::size_t* size) {
  if (path == nullptr || size == nullptr ||
      !mocktail::runtime::RobloxCursorHooksInstalled() ||
      !mocktail::runtime::IsStockSystemCursorAsset(path)) {
    return nullptr;
  }
  return mocktail::runtime::TransparentCursorPng(size);
}
