#include "ui/sound_slots.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace mocktail {
namespace ui {
namespace {

const std::string kOgg = std::string("OggS\0\2\0\0\0\0\0\0", 12) + "vorbis";
const std::string kWav = std::string("RIFF\x24\0\0\0WAVEfmt ", 16);
const std::string kMp3 = std::string("\xff\xfb\x90\x64\0\0\0\0\0\0\0\0", 12);
const std::string kTaggedMp3 = std::string("ID3\x04\0\0\0\0\0\x0a\0\0", 12);

class SoundSlotsTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("sound_slots_test_" + std::to_string(getpid()) + "_" +
             testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
    overlay_ = root_ / "overlay";
    sounds_ = overlay_ / "content" / "sounds";
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  std::filesystem::path Write(const std::string& name,
                              const std::string& bytes) {
    const std::filesystem::path path = root_ / name;
    std::ofstream(path, std::ios::binary) << bytes;
    return path;
  }

  static std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }

  std::filesystem::path root_;
  std::filesystem::path overlay_;
  std::filesystem::path sounds_;
};

TEST_F(SoundSlotsTest, EverySlotHasAKeyThatFindsIt) {
  for (const SoundSlot slot : kSoundSlots) {
    EXPECT_EQ(SoundSlotForKey(SoundSlotKey(slot)), slot);
    EXPECT_FALSE(SoundFiles(overlay_, slot).empty());
  }
  EXPECT_EQ(SoundSlotForKey("sky"), std::nullopt);
}

TEST_F(SoundSlotsTest, DeathWritesEveryNameRobloxAsksFor) {
  std::string error;
  ASSERT_TRUE(InstallSound(Write("my scream.ogg", kOgg), overlay_,
                           SoundSlot::kDeath, &error))
      << error;
  for (const char* name : {"uuhhh.mp3", "ouch.ogg", "oof.ogg"}) {
    EXPECT_EQ(Read(sounds_ / name), kOgg) << name;
  }
  EXPECT_EQ(CustomSound(overlay_, SoundSlot::kDeath), sounds_ / "uuhhh.mp3");
  EXPECT_TRUE(CustomSound(overlay_, SoundSlot::kJump).empty());
}

TEST_F(SoundSlotsTest, TakesOggMp3AndWavUnderAnyName) {
  std::string error;
  EXPECT_TRUE(InstallSound(Write("a.wav", kWav), overlay_, SoundSlot::kJump,
                           &error))
      << error;
  EXPECT_EQ(Read(sounds_ / "action_jump.mp3"), kWav);
  EXPECT_TRUE(InstallSound(Write("b.mp3", kMp3), overlay_, SoundSlot::kJump,
                           &error))
      << error;
  EXPECT_EQ(Read(sounds_ / "action_jump.mp3"), kMp3);
  EXPECT_TRUE(InstallSound(Write("c.mp3", kTaggedMp3), overlay_,
                           SoundSlot::kSplash, &error))
      << error;
  EXPECT_EQ(Read(sounds_ / "impact_water.mp3"), kTaggedMp3);
}

TEST_F(SoundSlotsTest, RefusesWhatIsNotASoundAndKeepsTheOneInUse) {
  std::string error;
  ASSERT_TRUE(InstallSound(Write("good.ogg", kOgg), overlay_,
                           SoundSlot::kFalling, &error));
  EXPECT_FALSE(InstallSound(Write("notes.mp3", "just some text here"),
                            overlay_, SoundSlot::kFalling, &error));
  EXPECT_NE(error.find("notes.mp3"), std::string::npos);
  EXPECT_FALSE(InstallSound(Write("tiny.ogg", "Ogg"), overlay_,
                            SoundSlot::kFalling, &error));
  EXPECT_FALSE(InstallSound(
      Write("song.mp3", std::string("\xff\xf1\x50\x80\0\0\0\0\0\0\0\0", 12)),
      overlay_, SoundSlot::kFalling, &error));
  EXPECT_FALSE(InstallSound(root_ / "missing.ogg", overlay_,
                            SoundSlot::kFalling, &error));
  EXPECT_EQ(Read(sounds_ / "action_falling.ogg"), kOgg);
  EXPECT_EQ(Read(sounds_ / "action_falling.mp3"), kOgg);
}

TEST_F(SoundSlotsTest, RefusesASoundThatIsTooLarge) {
  const std::filesystem::path big = Write("big.ogg", kOgg);
  std::filesystem::resize_file(big, kMaxSoundBytes + 1);
  std::string error;
  EXPECT_FALSE(InstallSound(big, overlay_, SoundSlot::kJump, &error));
  EXPECT_NE(error.find("big.ogg"), std::string::npos);
  EXPECT_TRUE(CustomSound(overlay_, SoundSlot::kJump).empty());
}

TEST_F(SoundSlotsTest, RemoveGoesBackToRobloxsAndLeavesTheOthers) {
  std::string error;
  ASSERT_TRUE(InstallSound(Write("d.ogg", kOgg), overlay_, SoundSlot::kDeath,
                           &error));
  ASSERT_TRUE(InstallSound(Write("j.ogg", kOgg), overlay_, SoundSlot::kJump,
                           &error));
  EXPECT_TRUE(RemoveSound(overlay_, SoundSlot::kDeath, &error)) << error;
  EXPECT_TRUE(CustomSound(overlay_, SoundSlot::kDeath).empty());
  EXPECT_FALSE(CustomSound(overlay_, SoundSlot::kJump).empty());
  EXPECT_TRUE(RemoveSound(overlay_, SoundSlot::kDeath, &error)) << error;
}

TEST_F(SoundSlotsTest, AFailedInstallPutsEveryNameBack) {
  std::filesystem::create_directories(sounds_ / "ouch.ogg");
  std::ofstream(sounds_ / "uuhhh.mp3", std::ios::binary) << kMp3;
  std::string error;
  EXPECT_FALSE(InstallSound(Write("new.ogg", kOgg), overlay_,
                            SoundSlot::kDeath, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(Read(sounds_ / "uuhhh.mp3"), kMp3);
  EXPECT_TRUE(std::filesystem::is_directory(sounds_ / "ouch.ogg"));
  EXPECT_FALSE(std::filesystem::exists(sounds_ / "oof.ogg"));
  int entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(sounds_)) {
    (void)entry;
    ++entries;
  }
  EXPECT_EQ(entries, 2);
}

TEST_F(SoundSlotsTest, AHandPlacedFileCountsAsCustom) {
  std::filesystem::create_directories(sounds_);
  std::ofstream(sounds_ / "ouch.ogg") << kOgg;
  EXPECT_EQ(CustomSound(overlay_, SoundSlot::kDeath), sounds_ / "ouch.ogg");
}

}
}
}
