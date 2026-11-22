#ifndef MOCKTAIL_UI_SOUND_SLOTS_H_
#define MOCKTAIL_UI_SOUND_SLOTS_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace ui {

enum class SoundSlot {
  kDeath,
  kJump,
  kLanding,
  kFootsteps,
  kFalling,
  kSwimming,
  kGettingUp,
  kSplash,
};

inline constexpr std::array<SoundSlot, 8> kSoundSlots = {
    SoundSlot::kDeath,    SoundSlot::kJump,     SoundSlot::kLanding,
    SoundSlot::kFootsteps, SoundSlot::kFalling, SoundSlot::kSwimming,
    SoundSlot::kGettingUp, SoundSlot::kSplash};

inline constexpr std::uintmax_t kMaxSoundBytes = 8 * 1024 * 1024;

std::string_view SoundSlotKey(SoundSlot slot);
std::optional<SoundSlot> SoundSlotForKey(std::string_view key);

std::vector<std::filesystem::path> SoundFiles(
    const std::filesystem::path& overlay_dir, SoundSlot slot);

std::filesystem::path CustomSound(const std::filesystem::path& overlay_dir,
                                  SoundSlot slot);

bool InstallSound(const std::filesystem::path& source,
                  const std::filesystem::path& overlay_dir, SoundSlot slot,
                  std::string* error);

bool RemoveSound(const std::filesystem::path& overlay_dir, SoundSlot slot,
                 std::string* error);

}
}

#endif
