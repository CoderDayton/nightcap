#include "runtime/roblox_http_failure_body_hook.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>

#include "mocktail/platform/log_redaction.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kTraceFormat = "status:%d %s bodySize:%zu";
// 0x25c bytes in build 20836, 0x260 in 22294.
constexpr std::size_t kMaxBodyReadDistance = 0x400;
// mov r15,rax; lea rax,[rip+cap]; movsxd r12,[rax] in build 22294.
constexpr std::size_t kMaxCompareDistance = 24;
// The response fields, as 2.742 through 2.748 lay them out.
constexpr std::uint8_t kBodyOffset = 0x48;
constexpr std::size_t kStatusOffset = 0x18;
constexpr std::size_t kTransportErrorOffset = 0x88;
constexpr std::size_t kMaxLoggedBytes = 300;
// Distinct bodies remembered; later ones are logged every time.
constexpr std::size_t kMaxSeen = 64;

struct GuestBytes {
  const char* data;
  std::size_t size;
};
using BodyGetter = GuestBytes (*)(std::uintptr_t body);

BodyGetter g_getter = nullptr;
std::atomic<bool> g_installed{false};
HttpFailureBodySeen& g_seen = *new HttpFailureBodySeen;

bool Fail(std::string* error, const char* message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

std::int32_t ReadInt32(std::uintptr_t address) {
  std::int32_t value = 0;
  std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
  return value;
}

GuestBytes BodyHook(std::uintptr_t body) {
  const GuestBytes bytes = g_getter(body);
  const std::uintptr_t response = body - kBodyOffset;
  const std::int32_t status = ReadInt32(response + kStatusOffset);
  if (bytes.data == nullptr || bytes.size == 0 || status < 400 ||
      status > 599 || ReadInt32(response + kTransportErrorOffset) != 0) {
    return bytes;
  }
  const std::string_view text(bytes.data, bytes.size);
  if (g_seen.FirstTime(status, text)) {
    std::fprintf(stderr, "  [http] %d response body: %s\n", status,
                 FormatHttpFailureBody(text).c_str());
  }
  return bytes;
}

std::optional<std::uintptr_t> FindTraceFormat(
    const std::vector<CursorHookSegment>& segments) {
  for (const CursorHookSegment& segment : segments) {
    std::size_t offset = 0;
    while (offset < segment.size) {
      const void* match =
          memmem(segment.data + offset, segment.size - offset,
                 kTraceFormat.data(), kTraceFormat.size());
      if (match == nullptr) {
        break;
      }
      const std::size_t at =
          static_cast<const std::uint8_t*>(match) - segment.data;
      if (at == 0 || segment.data[at - 1] == 0) {
        return segment.address + at;
      }
      offset = at + 1;
    }
  }
  return std::nullopt;
}

// The `lea r64,[rip+disp32]` instructions that load `target`.
std::vector<std::uintptr_t> RipLeasOf(const CursorHookSegment& code,
                                      std::uintptr_t target) {
  std::vector<std::uintptr_t> leas;
  std::size_t at = 1;
  while (at + 6 <= code.size) {
    const void* match = std::memchr(code.data + at, 0x8D, code.size - at - 5);
    if (match == nullptr) {
      break;
    }
    at = static_cast<const std::uint8_t*>(match) - code.data;
    const std::uint8_t rex = code.data[at - 1];
    if ((rex == 0x48 || rex == 0x4C) && (code.data[at + 1] & 0xC7) == 0x05) {
      std::int32_t displacement = 0;
      std::memcpy(&displacement, code.data + at + 2, sizeof(displacement));
      const std::uintptr_t next = code.address + at + 6;
      if (next + static_cast<std::intptr_t>(displacement) == target) {
        leas.push_back(code.address + at - 1);
      }
    }
    ++at;
  }
  return leas;
}

// cmp rdx,r64 in either encoding.
bool ComparesRdx(const std::uint8_t* code) {
  if ((code[0] == 0x48 || code[0] == 0x4C) && code[1] == 0x39 &&
      (code[2] & 0xC7) == 0xC2) {
    return true;
  }
  return (code[0] == 0x48 || code[0] == 0x49) && code[1] == 0x3B &&
         (code[2] & 0xF8) == 0xD0;
}

// lea rdi,[reg+0x48]; call rel32; then cmp rdx,... within reach.
std::optional<HttpFailureBodyCall> BodyReadAfter(
    const std::vector<CursorHookSegment>& segments,
    const CursorHookSegment& code, std::size_t from) {
  const std::size_t end =
      std::min(code.size, from + kMaxBodyReadDistance + 9 +
                              kMaxCompareDistance + 3);
  for (std::size_t at = from; at + 9 + kMaxCompareDistance + 3 <= end; ++at) {
    const std::uint8_t* lea = code.data + at;
    if ((lea[0] != 0x48 && lea[0] != 0x49) || lea[1] != 0x8D ||
        (lea[2] & 0xF8) != 0x78 || (lea[2] & 0x07) == 0x04 ||
        lea[3] != kBodyOffset || lea[4] != 0xE8) {
      continue;
    }
    bool compares = false;
    for (std::size_t next = 9; next < 9 + kMaxCompareDistance; ++next) {
      compares = compares || ComparesRdx(lea + next);
    }
    if (!compares) {
      continue;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, lea + 5, sizeof(displacement));
    const std::uintptr_t call = code.address + at + 4;
    const std::uintptr_t getter =
        call + 5 + static_cast<std::intptr_t>(displacement);
    const CursorHookSegment* target = SegmentContaining(segments, getter);
    if (target == nullptr || !target->executable) {
      continue;
    }
    return HttpFailureBodyCall{call, getter};
  }
  return std::nullopt;
}

}  // namespace

std::optional<HttpFailureBodyCall> LocateHttpFailureBodyCall(
    const std::vector<CursorHookSegment>& segments, std::string* error) {
  const std::optional<std::uintptr_t> format = FindTraceFormat(segments);
  if (!format.has_value()) {
    Fail(error, "HTTP trace format not found");
    return std::nullopt;
  }
  std::optional<HttpFailureBodyCall> found;
  for (const CursorHookSegment& code : segments) {
    if (!code.executable) {
      continue;
    }
    for (const std::uintptr_t lea : RipLeasOf(code, *format)) {
      const std::optional<HttpFailureBodyCall> read =
          BodyReadAfter(segments, code, lea - code.address);
      if (!read.has_value()) {
        continue;
      }
      if (found.has_value()) {
        Fail(error, "more than one HTTP trace body read");
        return std::nullopt;
      }
      found = read;
    }
  }
  if (!found.has_value()) {
    Fail(error, "HTTP trace body read not found");
  }
  return found;
}

std::string FormatHttpFailureBody(std::string_view body) {
  static constexpr char kHex[] = "0123456789abcdef";
  const std::string_view shown = body.substr(0, kMaxLoggedBytes);
  std::string out;
  out.reserve(shown.size());
  for (const char byte : shown) {
    const auto value = static_cast<unsigned char>(byte);
    if (byte == '\n') {
      out += "\\n";
    } else if (value < 0x20 || value >= 0x7F) {
      out += "\\x";
      out += kHex[value >> 4];
      out += kHex[value & 0x0F];
    } else {
      out += byte;
    }
  }
  if (shown.size() < body.size()) {
    out += "... (" + std::to_string(body.size()) + " bytes)";
  }
  return platform::RedactJoinTickets(out);
}

bool HttpFailureBodySeen::FirstTime(int status, std::string_view body) {
  const std::size_t key =
      std::hash<std::string_view>{}(body) ^ (std::hash<int>{}(status) << 1);
  std::lock_guard<std::mutex> lock(mutex_);
  for (const std::size_t seen : seen_) {
    if (seen == key) {
      return false;
    }
  }
  if (seen_.size() < kMaxSeen) {
    seen_.push_back(key);
  }
  return true;
}

bool InstallRobloxHttpFailureBodyHook(std::uintptr_t image_base,
                                      std::string* error) {
  if (g_installed.load(std::memory_order_acquire)) {
    return Fail(error, "already installed");
  }
  const auto started = std::chrono::steady_clock::now();
  std::uintptr_t image_end = image_base;
  const std::vector<CursorHookSegment> segments =
      LoadedImageSegments(image_base, &image_end);
  if (segments.empty()) {
    return Fail(error, "not an x86-64 ELF image");
  }
  const std::optional<HttpFailureBodyCall> site =
      LocateHttpFailureBodyCall(segments, error);
  if (!site.has_value()) {
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
  EncodeAbsoluteJump(reinterpret_cast<std::uintptr_t>(&BodyHook), stub);
  if (!FitsRel32(site->call, stub_address) ||
      mprotect(stub, static_cast<std::size_t>(page_size),
               PROT_READ | PROT_EXEC) != 0) {
    munmap(stub, static_cast<std::size_t>(page_size));
    return Fail(error, "stub out of reach");
  }
  g_getter = reinterpret_cast<BodyGetter>(site->getter);
  std::uint8_t call[5];
  EncodeRel32(0xE8, site->call, stub_address, call);
  if (!WriteCode(site->call, call, sizeof(call))) {
    return Fail(error, "could not patch the body read");
  }
  g_installed.store(true, std::memory_order_release);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  std::fprintf(stderr,
               "  [http] failed-response body logging on: call @+0x%lx "
               "(found in %lld ms)\n",
               static_cast<unsigned long>(site->call - image_base),
               static_cast<long long>(elapsed.count()));
  return true;
}

}  // namespace runtime
}  // namespace mocktail
