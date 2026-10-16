#include "runtime/luau_codegen_switch.h"

#include <elf.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kNameSlotOffset = 8;
constexpr std::size_t kRecordTailSize = 16;

std::string_view SegmentBytes(const LoadedImageSegment& segment) {
  return {reinterpret_cast<const char*>(segment.address), segment.size};
}

std::vector<std::uintptr_t> NameAddresses(
    const std::vector<LoadedImageSegment>& segments, std::string_view name) {
  std::string needle(name);
  needle.push_back('\0');
  std::vector<std::uintptr_t> addresses;
  for (const LoadedImageSegment& segment : segments) {
    const std::string_view bytes = SegmentBytes(segment);
    for (std::size_t at = bytes.find(needle); at != std::string_view::npos;
         at = bytes.find(needle, at + 1)) {
      addresses.push_back(segment.address + at);
    }
  }
  return addresses;
}

}  // namespace

std::vector<LoadedImageSegment> LoadedImageSegments(std::uintptr_t image_base) {
  std::vector<LoadedImageSegment> segments;
  if (image_base == 0) {
    return segments;
  }
  const auto* header = reinterpret_cast<const Elf64_Ehdr*>(image_base);
  if (std::memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
      header->e_ident[EI_CLASS] != ELFCLASS64 || header->e_phoff == 0) {
    return segments;
  }
  const auto* program = reinterpret_cast<const Elf64_Phdr*>(
      image_base + static_cast<std::uintptr_t>(header->e_phoff));
  for (std::size_t index = 0; index < header->e_phnum; ++index) {
    if (program[index].p_type != PT_LOAD || program[index].p_memsz == 0) {
      continue;
    }
    segments.push_back({image_base + program[index].p_vaddr,
                        static_cast<std::size_t>(program[index].p_memsz),
                        (program[index].p_flags & PF_W) != 0});
  }
  return segments;
}

std::uint8_t* FindFastFlagValue(const std::vector<LoadedImageSegment>& segments,
                                std::string_view name) {
  const std::vector<std::uintptr_t> names = NameAddresses(segments, name);
  if (names.empty()) {
    return nullptr;
  }
  std::uint8_t* found = nullptr;
  int matches = 0;
  for (const LoadedImageSegment& segment : segments) {
    if (!segment.writable || segment.size < kNameSlotOffset + kRecordTailSize) {
      continue;
    }
    const std::uintptr_t first =
        (segment.address + kNameSlotOffset + 7) & ~std::uintptr_t{7};
    const std::uintptr_t end = segment.address + segment.size;
    for (std::uintptr_t slot = first; slot + kRecordTailSize <= end;
         slot += 8) {
      std::uintptr_t pointer = 0;
      std::memcpy(&pointer, reinterpret_cast<const void*>(slot),
                  sizeof(pointer));
      if (std::find(names.begin(), names.end(), pointer) == names.end()) {
        continue;
      }
      std::uint64_t length = 0;
      std::memcpy(&length, reinterpret_cast<const void*>(slot + 8),
                  sizeof(length));
      if (length != name.size()) {
        continue;
      }
      found = reinterpret_cast<std::uint8_t*>(slot - kNameSlotOffset);
      ++matches;
    }
  }
  return matches == 1 ? found : nullptr;
}

bool ClientSettingsSetLuauCodegenAll(std::string_view overrides_json) {
  const nlohmann::json overrides =
      nlohmann::json::parse(overrides_json, nullptr, false, true);
  return overrides.is_object() &&
         overrides.contains(std::string(kLuauCodegenAllClientSetting));
}

bool LuauCodegenAllForPhase(RobloxExperiencePresencePhase phase) {
  return phase == RobloxExperiencePresencePhase::kBrowsing;
}

LuauCodegenMenuSwitch::LuauCodegenMenuSwitch(
    LocateFlagFn locate_flag, RobloxExperiencePresenceObserver next)
    : locate_flag_(std::move(locate_flag)), next_(next) {}

RobloxExperiencePresenceObserver LuauCodegenMenuSwitch::observer() {
  return {this, &LuauCodegenMenuSwitch::Notify};
}

void LuauCodegenMenuSwitch::OnPhase(
    RobloxExperiencePresencePhase phase,
    const RobloxExperienceLaunchRequest* request) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!located_) {
      located_ = true;
      flag_ = locate_flag_ ? locate_flag_() : nullptr;
    }
    if (flag_ != nullptr) {
      const std::uint8_t current = __atomic_load_n(flag_, __ATOMIC_RELAXED);
      const std::uint8_t wanted = LuauCodegenAllForPhase(phase) ? 1 : 0;
      if (current <= 1 && current != wanted) {
        __atomic_store_n(flag_, wanted, __ATOMIC_RELAXED);
        std::cout << "  [luau-codegen] native code for all scripts "
                  << (wanted != 0 ? "on" : "off") << '\n';
      }
    }
  }
  if (next_.valid()) {
    next_.notify(next_.context, phase, request);
  }
}

void LuauCodegenMenuSwitch::Notify(
    void* context, RobloxExperiencePresencePhase phase,
    const RobloxExperienceLaunchRequest* request) {
  static_cast<LuauCodegenMenuSwitch*>(context)->OnPhase(phase, request);
}

}  // namespace runtime
}  // namespace mocktail
