#include "ui/sound_slots.h"

#include <unistd.h>

#include <array>
#include <cstring>
#include <fstream>
#include <system_error>

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
  return head[0] == 0xff && (head[1] & 0xe0) == 0xe0;
}

std::filesystem::path Staged(const std::filesystem::path& target) {
  return target.parent_path() /
         ("." + target.filename().string() + "." + std::to_string(getpid()));
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
  std::error_code error;
  for (const std::filesystem::path& file : SoundFiles(overlay_dir, slot)) {
    if (std::filesystem::is_regular_file(file, error)) return file;
  }
  return {};
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
  const std::vector<std::filesystem::path> targets =
      SoundFiles(overlay_dir, slot);
  std::filesystem::create_directories(targets.front().parent_path(), failure);
  for (const std::filesystem::path& target : targets) {
    if (failure) break;
    std::filesystem::copy_file(
        source, Staged(target),
        std::filesystem::copy_options::overwrite_existing, failure);
  }
  for (const std::filesystem::path& target : targets) {
    if (failure) break;
    std::filesystem::rename(Staged(target), target, failure);
  }
  if (failure) {
    *error = "Cannot save the sound: " + failure.message();
    std::error_code ignored;
    for (const std::filesystem::path& target : targets) {
      std::filesystem::remove(Staged(target), ignored);
    }
    return false;
  }
  return true;
}

bool RemoveSound(const std::filesystem::path& overlay_dir, SoundSlot slot,
                 std::string* error) {
  std::error_code failure;
  for (const std::filesystem::path& file : SoundFiles(overlay_dir, slot)) {
    std::filesystem::remove(file, failure);
    if (failure) {
      *error = "Cannot remove the sound: " + failure.message();
      return false;
    }
  }
  return true;
}

}
}
