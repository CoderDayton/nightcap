#include "ui/sound_slots.h"

#include <array>
#include <cstring>
#include <fstream>
#include <system_error>

#include "ui/slot_files.h"

namespace mocktail {
namespace ui {
namespace {

struct SlotFiles {
  SoundSlot slot;
  std::string_view key;
  std::array<std::string_view, 3> files;
};

constexpr SlotFiles kSlotFiles[] = {
    {SoundSlot::kDeath, "sound_death", {"uuhhh.mp3", "ouch.ogg", "oof.ogg"}},
    {SoundSlot::kJump, "sound_jump", {"action_jump.mp3"}},
    {SoundSlot::kLanding, "sound_landing", {"action_jump_land.mp3"}},
    {SoundSlot::kFootsteps,
     "sound_footsteps",
     {"action_footsteps_plastic.mp3"}},
    {SoundSlot::kFalling,
     "sound_falling",
     {"action_falling.ogg", "action_falling.mp3"}},
    {SoundSlot::kSwimming, "sound_swimming", {"action_swim.mp3"}},
    {SoundSlot::kGettingUp, "sound_getting_up", {"action_get_up.mp3"}},
    {SoundSlot::kSplash, "sound_splash", {"impact_water.mp3"}},
};

const SlotFiles& FilesOf(SoundSlot slot) {
  for (const SlotFiles& entry : kSlotFiles) {
    if (entry.slot == slot) return entry;
  }
  return kSlotFiles[0];
}

bool IsSound(const std::filesystem::path& source) {
  std::array<unsigned char, 12> head{};
  std::ifstream in(source, std::ios::binary);
  in.read(reinterpret_cast<char*>(head.data()), head.size());
  if (in.gcount() < static_cast<std::streamsize>(head.size())) return false;
  if (std::memcmp(head.data(), "OggS", 4) == 0) return true;
  if (std::memcmp(head.data(), "RIFF", 4) == 0 &&
      std::memcmp(head.data() + 8, "WAVE", 4) == 0) {
    return true;
  }
  if (std::memcmp(head.data(), "ID3", 3) == 0) return true;
  return head[0] == 0xff && (head[1] & 0xe0) == 0xe0 && (head[1] & 0x06) != 0;
}

}

std::string_view SoundSlotKey(SoundSlot slot) { return FilesOf(slot).key; }

std::optional<SoundSlot> SoundSlotForKey(std::string_view key) {
  for (const SlotFiles& entry : kSlotFiles) {
    if (entry.key == key) return entry.slot;
  }
  return std::nullopt;
}

std::vector<std::filesystem::path> SoundFiles(
    const std::filesystem::path& overlay_dir, SoundSlot slot) {
  std::vector<std::filesystem::path> files;
  for (const std::string_view name : FilesOf(slot).files) {
    if (!name.empty()) files.push_back(overlay_dir / "content" / "sounds" / name);
  }
  return files;
}

std::filesystem::path CustomSound(const std::filesystem::path& overlay_dir,
                                  SoundSlot slot) {
  return FirstRegularFile(SoundFiles(overlay_dir, slot));
}

bool InstallSound(const std::filesystem::path& source,
                  const std::filesystem::path& overlay_dir, SoundSlot slot,
                  std::string* error) {
  const std::string name = source.filename().string();
  if (!IsSound(source)) {
    *error = name + " is not an OGG, MP3 or WAV sound.";
    return false;
  }
  std::error_code failure;
  if (std::filesystem::file_size(source, failure) > kMaxSoundBytes ||
      failure) {
    *error = name + " is too large. A sound can be at most 8 MB.";
    return false;
  }
  if (!ReplaceAllWith(source, SoundFiles(overlay_dir, slot), &failure)) {
    *error = "Cannot save the sound: " + failure.message();
    return false;
  }
  return true;
}

bool RemoveSound(const std::filesystem::path& overlay_dir, SoundSlot slot,
                 std::string* error) {
  std::error_code failure;
  if (!RemoveAll(SoundFiles(overlay_dir, slot), &failure)) {
    *error = "Cannot remove the sound: " + failure.message();
    return false;
  }
  return true;
}

}
}
