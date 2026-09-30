#ifndef MOCKTAIL_COMPAT_WEB_VIEW_PROTOCOL_CONTRACT_H_
#define MOCKTAIL_COMPAT_WEB_VIEW_PROTOCOL_CONTRACT_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

// Roblox's Subsystem<IWebViewProtocol> getter is not exported. The exported
// WebViewProtocol.initializeAndroidWebViewProtocol calls it first, into a
// stack slot, then reads the protocol from the returned holder:
//
//   x86-64:  lea slot(%rbp),%rbx; mov %rbx,%rdi; call getter;
//            mov (%rbx),%rbx; mov 0x18(%rbx),%rax
//   AArch64: add x8, sp, #slot; bl getter; ldr xN, [sp, #slot];
//            ldr x8, [xN, #0x18]
//
// These functions find that call site in the initializer's first bytes and
// check the getter's own code. Only relocated or build-varying fields (stack
// slots, frame sizes, PC-relative offsets, the call target) are masked.
// Verified on x86_64 2.734.917, 2.736.1408 and 2.738.1397, and on arm64-v8a
// 2.738.1397 and 2.740.931.

namespace mocktail::compat {

// Covers the call site in every verified build (x86-64 +0x27, AArch64 +0x34).
inline constexpr std::size_t kWebViewProtocolInitializerScanBytes = 128;
inline constexpr std::size_t kWebViewProtocolGetterBytesX86_64 = 44;
inline constexpr std::size_t kWebViewProtocolGetterBytesArm64 = 60;

namespace web_view_protocol_contract_detail {

inline bool MatchesMasked(std::string_view code, std::size_t offset,
                          std::string_view pattern,
                          std::string_view wildcard) {
  if (offset > code.size() || code.size() - offset < pattern.size()) {
    return false;
  }
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    if (wildcard[index] == 'x') continue;
    if (code[offset + index] != pattern[index]) return false;
  }
  return true;
}

// AArch64 instructions are little-endian words.
inline std::uint32_t Word(std::string_view code, std::size_t index) {
  std::uint32_t word = 0;
  for (std::size_t byte = 0; byte < 4; ++byte) {
    word |= static_cast<std::uint32_t>(
                static_cast<unsigned char>(code[index * 4 + byte]))
            << (8 * byte);
  }
  return word;
}

// One AArch64 instruction word: bits outside `mask` may vary.
struct MaskedWord {
  std::uint32_t mask;
  std::uint32_t value;
};

inline constexpr std::uint32_t kExact = 0xffffffffU;
// adrp Xd, page: immlo and immhi vary.
inline constexpr std::uint32_t kAdrpMask = 0x9f00001fU;
// add Xd, Xn, #imm12: imm12 varies.
inline constexpr std::uint32_t kAddImmediateMask = 0xffc003ffU;

}  // namespace web_view_protocol_contract_detail

// Returns the getter address called from the x86-64 initializer at
// `address`, or nullopt unless the frame setup and exactly one call site
// match.
inline std::optional<std::uintptr_t> FindWebViewProtocolGetterCallX86_64(
    std::string_view code, std::uintptr_t address) {
  using web_view_protocol_contract_detail::MatchesMasked;
  // push %rbp; mov %rsp,%rbp
  if (!MatchesMasked(code, 0, "\x55\x48\x89\xe5", "....")) {
    return std::nullopt;
  }
  static constexpr std::string_view kCallSite(
      "\x48\x8d\x5d\x00\x48\x89\xdf\xe8\x00\x00\x00\x00\x48\x8b\x1b\x48\x8b\x43"
      "\x18",
      19);
  static constexpr std::string_view kCallSiteWildcard("...x....xxxx.......");
  static constexpr std::size_t kCallDisplacement = 8;
  const std::string_view window =
      code.substr(0, kWebViewProtocolInitializerScanBytes);
  std::optional<std::size_t> call_site;
  for (std::size_t offset = 0; offset < window.size(); ++offset) {
    if (MatchesMasked(window, offset, kCallSite, kCallSiteWildcard)) {
      if (call_site.has_value()) return std::nullopt;
      call_site = offset;
    }
  }
  if (!call_site.has_value()) return std::nullopt;
  std::int32_t displacement = 0;
  std::memcpy(&displacement, window.data() + *call_site + kCallDisplacement,
              sizeof(displacement));
  const std::uintptr_t next_instruction =
      address + *call_site + kCallDisplacement + sizeof(displacement);
  return next_instruction +
         static_cast<std::uintptr_t>(static_cast<std::intptr_t>(displacement));
}

// The x86-64 getter stores the holder returned by the subsystem's acquire
// into its result slot (%rdi) and returns that slot.
inline bool HasWebViewProtocolGetterContractX86_64(std::string_view code) {
  return web_view_protocol_contract_detail::MatchesMasked(
      code, 0,
      std::string_view(
          "\x55\x48\x89\xe5\x53\x50\x48\x89\xfb\x8a\x05\x00\x00\x00\x00\x84\xc0"
          "\x74\x19\x48\x8d\x3d\x00\x00\x00\x00\xe8\x59\x00\x00\x00\x48\x89\x03"
          "\x48\x89\xd8\x48\x83\xc4\x08\x5b\x5d\xc3",
          kWebViewProtocolGetterBytesX86_64),
      "...........xxxx.......xxxx..................");
}

// Returns the getter address called from the AArch64 initializer at
// `address`, or nullopt unless exactly one call site matches.
inline std::optional<std::uintptr_t> FindWebViewProtocolGetterCallArm64(
    std::string_view code, std::uintptr_t address) {
  using web_view_protocol_contract_detail::Word;
  const std::size_t words =
      code.substr(0, kWebViewProtocolInitializerScanBytes).size() / 4;
  std::optional<std::uintptr_t> getter;
  for (std::size_t index = 0; index + 4 <= words; ++index) {
    const std::uint32_t slot_address = Word(code, index);
    const std::uint32_t call = Word(code, index + 1);
    const std::uint32_t handle_load = Word(code, index + 2);
    const std::uint32_t protocol_load = Word(code, index + 3);
    // add x8, sp, #slot
    if ((slot_address & 0xffc003ffU) != 0x910003e8U) continue;
    // bl getter
    if ((call & 0xfc000000U) != 0x94000000U) continue;
    // ldr xN, [sp, #slot]
    if ((handle_load & 0xffc003e0U) != 0xf94003e0U) continue;
    const std::uint32_t slot = (slot_address >> 10) & 0xfffU;
    if (((handle_load >> 10) & 0xfffU) * 8 != slot) continue;
    // ldr x8, [xN, #0x18]
    const std::uint32_t handle_register = handle_load & 0x1fU;
    if (protocol_load != (0xf9400c08U | (handle_register << 5))) continue;
    if (getter.has_value()) return std::nullopt;
    // imm26 is a signed word offset from the bl instruction.
    const std::int64_t imm26 = call & 0x03ffffffU;
    const std::int64_t words_from_call =
        (imm26 & 0x02000000) != 0 ? imm26 - 0x04000000 : imm26;
    getter = address + (index + 1) * 4 +
             static_cast<std::uintptr_t>(words_from_call * 4);
  }
  return getter;
}

// The AArch64 getter saves its result slot (x8) and stores the holder
// returned by the subsystem's acquire there.
inline bool HasWebViewProtocolGetterContractArm64(std::string_view code) {
  using namespace web_view_protocol_contract_detail;
  static constexpr MaskedWord kGetter[] = {
      {kExact, 0xa9be7bfdU},              // stp x29, x30, [sp, #-0x20]!
      {kExact, 0xf9000bf3U},              // str x19, [sp, #0x10]
      {kExact, 0x910003fdU},              // mov x29, sp
      {kAdrpMask, 0x90000009U},           // adrp x9, guard
      {kAddImmediateMask, 0x91000129U},   // add x9, x9, :lo12:guard
      {kExact, 0xaa0803f3U},              // mov x19, x8
      {kExact, 0x08dffd29U},              // ldarb w9, [x9]
      {kExact, 0x36000109U},              // tbz w9, #0, initialize
      {kAdrpMask, 0x90000000U},           // adrp x0, holder
      {kAddImmediateMask, 0x91000000U},   // add x0, x0, :lo12:holder
      {kExact, 0x94000017U},              // bl acquire
      {kExact, 0xf9000260U},              // str x0, [x19]
      {kExact, 0xf9400bf3U},              // ldr x19, [sp, #0x10]
      {kExact, 0xa8c27bfdU},              // ldp x29, x30, [sp], #0x20
      {kExact, 0xd65f03c0U},              // ret
  };
  static_assert(sizeof(kGetter) / sizeof(kGetter[0]) * 4 ==
                kWebViewProtocolGetterBytesArm64);
  if (code.size() < kWebViewProtocolGetterBytesArm64) return false;
  for (std::size_t index = 0; index < sizeof(kGetter) / sizeof(kGetter[0]);
       ++index) {
    if ((Word(code, index) & kGetter[index].mask) != kGetter[index].value) {
      return false;
    }
  }
  return true;
}

}  // namespace mocktail::compat

#endif  // MOCKTAIL_COMPAT_WEB_VIEW_PROTOCOL_CONTRACT_H_
