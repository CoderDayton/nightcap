#include <gtest/gtest.h>

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "mocktail/vr/emoji_catalog.h"

namespace mocktail::vr {
namespace {

const Emoji* Find(const EmojiCatalog& catalog, const std::string& utf8) {
  for (const EmojiGroup& group : catalog.groups)
    for (const Emoji& emoji : group.emoji)
      if (emoji.utf8 == utf8) return &emoji;
  return nullptr;
}

TEST(EmojiCatalogTest, SkinTonesBecomeVariantsOfTheirBase) {
  const EmojiCatalog catalog = BuildEmojiCatalog({
      U"\U0001F44D",              // thumbs up
      U"\U0001F44D\U0001F3FB",    // light skin tone
      U"\U0001F44D\U0001F3FF",    // dark skin tone
      U"\U0001F600",              // grinning face
      U"\U0001F9D1\U0001F3FD",    // person, no toneless base given
  });
  EXPECT_EQ(catalog.size(), 3u);
  const Emoji* thumbs = Find(catalog, "\U0001F44D");
  ASSERT_NE(thumbs, nullptr);
  ASSERT_EQ(thumbs->variants.size(), 3u);
  EXPECT_EQ(thumbs->variants[0], "\U0001F44D");  // its own text first
  EXPECT_EQ(thumbs->variants[1], "\U0001F44D\U0001F3FB");
  EXPECT_TRUE(Find(catalog, "\U0001F600")->variants.empty());
  // Without a base the toned sequence stands alone.
  EXPECT_NE(Find(catalog, "\U0001F9D1\U0001F3FD"), nullptr);
}

TEST(EmojiCatalogTest, VariationSelectorSpellingsAreOneEmoji) {
  const EmojiCatalog catalog = BuildEmojiCatalog({
      U"\U0001F3F3\U0001F308‍",  // not real, just distinct
      U"\U0001F3F3‍\U0001F308",
      U"\U0001F3F3️‍\U0001F308",
  });
  EXPECT_EQ(catalog.size(), 2u);
  // The fully qualified spelling wins.
  EXPECT_NE(Find(catalog, "\U0001F3F3️‍\U0001F308"), nullptr);
  EXPECT_EQ(Find(catalog, "\U0001F3F3‍\U0001F308"), nullptr);
}

TEST(EmojiCatalogTest, FacesComeFirstAndFlagsGetTheirOwnTab) {
  const EmojiCatalog catalog = BuildEmojiCatalog({
      U"\U0001F1FA\U0001F1F8",  // flag
      U"❤",                // heart (older symbol)
      U"\U0001F600",            // face
      U"\U0001F680",            // rocket
  });
  ASSERT_EQ(catalog.groups.size(), 4u);
  EXPECT_EQ(catalog.groups.front().emoji.front().utf8, "\U0001F600");
  EXPECT_EQ(catalog.groups.back().emoji.front().utf8,
            "\U0001F1FA\U0001F1F8");
}

TEST(EmojiCatalogTest, MissingFontGivesAnEmptyCatalog) {
  EXPECT_TRUE(LoadEmojiCatalog({"/nonexistent/emoji.ttf"}).empty());
}

// With a Roblox payload (MOCKTAIL_TEST_ASSET_PATH, its content directory),
// the catalog comes from Roblox's own emoji font.
TEST(EmojiCatalogTest, ReadsEveryEmojiFromRobloxsFont) {
  const char* content = std::getenv("MOCKTAIL_TEST_ASSET_PATH");
  if (content == nullptr || content[0] == '\0')
    GTEST_SKIP() << "set MOCKTAIL_TEST_ASSET_PATH to a payload content dir";
  setenv("MOCKTAIL_ASSET_PATH", content, 1);
  const std::vector<std::filesystem::path> fonts = PayloadEmojiFonts();
  unsetenv("MOCKTAIL_ASSET_PATH");
  ASSERT_FALSE(fonts.empty());
  const EmojiCatalog catalog = LoadEmojiCatalog(fonts);
  EXPECT_GT(catalog.size(), 1000u);
  EXPECT_NE(Find(catalog, "\U0001F600"), nullptr);
  EXPECT_NE(Find(catalog, "\U0001F1FA\U0001F1F8"), nullptr);
  const Emoji* family = Find(
      catalog, "\U0001F468‍\U0001F469‍\U0001F467");
  EXPECT_NE(family, nullptr);
  const Emoji* thumbs = Find(catalog, "\U0001F44D");
  ASSERT_NE(thumbs, nullptr);
  EXPECT_EQ(thumbs->variants.size(), 6u);
  // Components never stand alone.
  EXPECT_EQ(Find(catalog, "\U0001F3FB"), nullptr);
  EXPECT_EQ(Find(catalog, "\U0001F1FA"), nullptr);
  std::set<std::string> seen;
  for (const EmojiGroup& group : catalog.groups)
    for (const Emoji& emoji : group.emoji)
      EXPECT_TRUE(seen.insert(emoji.utf8).second) << emoji.utf8;
}

}  // namespace
}  // namespace mocktail::vr
