#include "runtime/runtime_config_editor.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include "runtime/runtime_config_bootstrap.h"
#include "runtime/runtime_config_file.h"

namespace mocktail {
namespace runtime {
namespace {

class EmptyEnvironment final : public Environment {
 public:
  std::optional<std::string> Get(std::string_view) const override {
    return std::nullopt;
  }
};

RuntimeConfigLoadResult Load(std::string_view contents) {
  char pattern[] = "/tmp/mocktail_config_editor_XXXXXX";
  const char* created = mkdtemp(pattern);
  const std::filesystem::path directory = created != nullptr ? created : "";
  const std::filesystem::path file = directory / "config.yaml";
  { std::ofstream(file) << contents; }
  RuntimeConfigLoadResult result = LoadRuntimeConfig(EmptyEnvironment(), file);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  return result;
}

std::size_t LineCount(std::string_view text) {
  return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

bool Contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

TEST(RuntimeConfigEditorTest, ReplacesAnActiveValueAndNothingElse) {
  const std::string before =
      "# top comment\n"
      "graphics:\n"
      "  # String: backend.\n"
      "  backend: direct-vulkan\n"
      "window:\n"
      "  width: 1280\n";

  const RuntimeConfigEdit edit =
      SetRuntimeConfigString(before, "graphics.backend", "opengl");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "# top comment\n"
            "graphics:\n"
            "  # String: backend.\n"
            "  backend: opengl\n"
            "window:\n"
            "  width: 1280\n");
}

TEST(RuntimeConfigEditorTest, TellsSameNamedKeysInDifferentSectionsApart) {
  const std::string before =
      "integrations:\n"
      "  fleasion:\n"
      "    enabled: false\n"
      "  discord_rpc:\n"
      "    enabled: true\n";

  const RuntimeConfigEdit edit =
      SetRuntimeConfigBool(before, "integrations.discord_rpc.enabled", false);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "integrations:\n"
            "  fleasion:\n"
            "    enabled: false\n"
            "  discord_rpc:\n"
            "    enabled: false\n");
}

TEST(RuntimeConfigEditorTest, UncommentsAKeyInPlace) {
  const std::string before =
      "graphics:\n"
      "  backend: direct-vulkan\n"
      "  # Optional presentation override: auto, on, or off.\n"
      "  # vsync: off\n"
      "performance:\n"
      "  gamemode: auto\n";

  const RuntimeConfigEdit edit =
      SetRuntimeConfigString(before, "graphics.vsync", "on");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "graphics:\n"
            "  backend: direct-vulkan\n"
            "  # Optional presentation override: auto, on, or off.\n"
            "  vsync: on\n"
            "performance:\n"
            "  gamemode: auto\n");
}

TEST(RuntimeConfigEditorTest, UncommentsTheSectionAboveANestedKey) {
  const std::string before =
      "integrations:\n"
      "  discord_rpc:\n"
      "    enabled: true\n"
      "    # text:\n"
      "    #   browsing: Browsing experiences\n"
      "    #   playing: \"Playing {place_name}\"\n"
      "    # images:\n"
      "    #   large: \"{place_icon}\"\n";

  const RuntimeConfigEdit edit = SetRuntimeConfigString(
      before, "integrations.discord_rpc.text.playing", "In {place_name}");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "integrations:\n"
            "  discord_rpc:\n"
            "    enabled: true\n"
            "    text:\n"
            "    #   browsing: Browsing experiences\n"
            "      playing: \"In {place_name}\"\n"
            "    # images:\n"
            "    #   large: \"{place_icon}\"\n");
}

TEST(RuntimeConfigEditorTest, PrefersAnActiveKeyOverACommentedOne) {
  const std::string before =
      "graphics:\n"
      "  # vsync: off\n"
      "  vsync: auto\n";

  const RuntimeConfigEdit edit =
      SetRuntimeConfigString(before, "graphics.vsync", "on");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "graphics:\n"
            "  # vsync: off\n"
            "  vsync: on\n");
}

TEST(RuntimeConfigEditorTest, AddsAMissingKeyAtTheEndOfItsSection) {
  const std::string before =
      "input:\n"
      "  touch: false\n"
      "\n"
      "window:\n"
      "  width: 1280\n";

  const RuntimeConfigEdit edit =
      SetRuntimeConfigBool(before, "input.raw_mouse", true);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "input:\n"
            "  touch: false\n"
            "  raw_mouse: true\n"
            "\n"
            "window:\n"
            "  width: 1280\n");
}

TEST(RuntimeConfigEditorTest, AddsAMissingSectionAtTheEndOfTheFile) {
  const RuntimeConfigEdit edit = SetRuntimeConfigInteger(
      "window:\n  width: 1280", "performance.memory_limit_mb", 6144);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "window:\n"
            "  width: 1280\n"
            "performance:\n"
            "  memory_limit_mb: 6144\n");
}

TEST(RuntimeConfigEditorTest, AddsMissingNestedSections) {
  const RuntimeConfigEdit edit = SetRuntimeConfigBool(
      "integrations:\n  fleasion:\n    enabled: false\n",
      "integrations.discord_rpc.join.enabled", false);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "integrations:\n"
            "  fleasion:\n"
            "    enabled: false\n"
            "  discord_rpc:\n"
            "    join:\n"
            "      enabled: false\n");
}

TEST(RuntimeConfigEditorTest, QuotesOnlyStringsThatNeedIt) {
  const std::string before = "window:\n  title: Roblox\n";

  EXPECT_EQ(SetRuntimeConfigString(before, "window.title", "My Game").text,
            "window:\n  title: My Game\n");
  EXPECT_EQ(SetRuntimeConfigString(before, "window.title", "").text,
            "window:\n  title: \"\"\n");
  EXPECT_EQ(SetRuntimeConfigString(before, "window.title", "a: b #c").text,
            "window:\n  title: \"a: b #c\"\n");
  EXPECT_EQ(
      SetRuntimeConfigString(before, "window.title", "say \"hi\" \\ bye").text,
      "window:\n  title: \"say \\\"hi\\\" \\\\ bye\"\n");
  EXPECT_EQ(SetRuntimeConfigString(before, "window.title", " padded ").text,
            "window:\n  title: \" padded \"\n");
}

TEST(RuntimeConfigEditorTest, KeepsACommentAfterTheValue) {
  const RuntimeConfigEdit edit = SetRuntimeConfigInteger(
      "window:\n  width: 1280  # my monitor\n", "window.width", 1920);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text, "window:\n  width: 1920  # my monitor\n");
}

TEST(RuntimeConfigEditorTest, DoesNotTreatAHashInsideQuotesAsAComment) {
  const RuntimeConfigEdit edit = SetRuntimeConfigString(
      "window:\n  title: \"a # b\"\n", "window.title", "plain");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text, "window:\n  title: plain\n");
}

TEST(RuntimeConfigEditorTest, TurnsASectionIntoAValueAndKeepsItsLines) {
  const std::string before =
      "device:\n"
      "  type: mobile\n"
      "  # a note\n"
      "  name: Pixel\n"
      "window:\n"
      "  width: 1280\n";

  const RuntimeConfigEdit edit =
      SetRuntimeConfigString(before, "device", "pc-windows-11");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "device: pc-windows-11\n"
            "#   type: mobile\n"
            "  # a note\n"
            "#   name: Pixel\n"
            "window:\n"
            "  width: 1280\n");
}

TEST(RuntimeConfigEditorTest, TurnsAValueIntoASection) {
  const RuntimeConfigEdit edit = SetRuntimeConfigString(
      "device: pc-windows-11  # preset\nwindow:\n  width: 1280\n",
      "device.type", "mobile");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "device:  # preset\n"
            "  type: mobile\n"
            "window:\n"
            "  width: 1280\n");
}

TEST(RuntimeConfigEditorTest, ASectionCanGoToAValueAndBack) {
  const std::string section = "device:\n  type: mobile\n  name: Pixel\n";

  const RuntimeConfigEdit value =
      SetRuntimeConfigString(section, "device", "pc");
  ASSERT_TRUE(value) << value.error;
  const RuntimeConfigEdit back =
      SetRuntimeConfigString(value.text, "device.type", "console");
  ASSERT_TRUE(back) << back.error;
  EXPECT_EQ(back.text, "device:\n  type: console\n#   name: Pixel\n");
}

TEST(RuntimeConfigEditorTest, DoesNotUncommentASectionTheFileAlreadyHas) {
  // Uncommenting "# device:" would give the file two device keys.
  const std::string before =
      "device: pc\n"
      "# device:\n"
      "#   type: mobile\n";

  const RuntimeConfigEdit edit =
      SetRuntimeConfigString(before, "device.type", "console");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "device:\n"
            "  type: console\n"
            "# device:\n"
            "#   type: mobile\n");
}

TEST(RuntimeConfigEditorTest, FillsASectionThatHoldsOnlyComments) {
  const RuntimeConfigEdit edit = SetRuntimeConfigBool(
      "input:\n  # nothing set\nwindow:\n  width: 1\n", "input.raw_mouse",
      true);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "input:\n  # nothing set\n  raw_mouse: true\nwindow:\n  width: 1\n");
}

TEST(RuntimeConfigEditorTest, UnsetCommentsTheKeyOut) {
  const std::string before =
      "graphics:\n"
      "  backend: opengl\n"
      "  vsync: on\n"
      "window:\n"
      "  width: 1280\n";

  const RuntimeConfigEdit edit =
      UnsetRuntimeConfigValue(before, "graphics.vsync");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "graphics:\n"
            "  backend: opengl\n"
            "  # vsync: on\n"
            "window:\n"
            "  width: 1280\n");
  EXPECT_EQ(SetRuntimeConfigString(edit.text, "graphics.vsync", "on").text,
            before);
}

TEST(RuntimeConfigEditorTest, UnsetOfAnAbsentKeyChangesNothing) {
  const std::string before = "graphics:\n  # vsync: off\n";
  const RuntimeConfigEdit edit =
      UnsetRuntimeConfigValue(before, "graphics.vsync");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text, before);
  EXPECT_EQ(UnsetRuntimeConfigValue(before, "audio.output_device").text,
            before);
}

TEST(RuntimeConfigEditorTest, UnsetOfTheLastKeyCommentsItsSectionOutToo) {
  const std::string before =
      "integrations:\n"
      "  discord_rpc:\n"
      "    enabled: true\n"
      "    text:\n"
      "      playing: \"In {place_name}\"\n";

  const RuntimeConfigEdit edit = UnsetRuntimeConfigValue(
      before, "integrations.discord_rpc.text.playing");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "integrations:\n"
            "  discord_rpc:\n"
            "    enabled: true\n"
            "    # text:\n"
            "      # playing: \"In {place_name}\"\n");

  const RuntimeConfigEdit again = SetRuntimeConfigString(
      edit.text, "integrations.discord_rpc.text.playing", "In {place_name}");
  ASSERT_TRUE(again) << again.error;
  EXPECT_EQ(again.text, before);
}

TEST(RuntimeConfigEditorTest, UnsetOfASectionCommentsOutAllOfIt) {
  const RuntimeConfigEdit edit = UnsetRuntimeConfigValue(
      "a:\n  keep: 1\n  drop:\n    x: 1\n\n    y: |\n      text\n  tail: 2\n",
      "a.drop");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "a:\n  keep: 1\n  # drop:\n  #   x: 1\n\n  #   y: |\n"
            "  #     text\n  tail: 2\n");
}

TEST(RuntimeConfigEditorTest, KeepsWindowsLineEndings) {
  const std::string before =
      "graphics:\r\n  backend: direct-vulkan\r\n  # vsync: off\r\n"
      "window:\r\n  width: 1280\r\n";

  RuntimeConfigEdit edit =
      SetRuntimeConfigString(before, "graphics.backend", "opengl");
  ASSERT_TRUE(edit) << edit.error;
  edit = SetRuntimeConfigString(edit.text, "graphics.vsync", "on");
  ASSERT_TRUE(edit) << edit.error;
  edit = SetRuntimeConfigBool(edit.text, "input.raw_mouse", true);
  ASSERT_TRUE(edit) << edit.error;
  edit = SetRuntimeConfigInteger(edit.text, "window.height", 720);
  ASSERT_TRUE(edit) << edit.error;
  edit = UnsetRuntimeConfigValue(edit.text, "window.width");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "graphics:\r\n  backend: opengl\r\n  vsync: on\r\n"
            "window:\r\n  # width: 1280\r\n  height: 720\r\n"
            "input:\r\n  raw_mouse: true\r\n");
}

TEST(RuntimeConfigEditorTest, KeepsAByteOrderMarkAndOldMacLineEndings) {
  const RuntimeConfigEdit edit = SetRuntimeConfigInteger(
      "\xef\xbb\xbfwindow:\r  width: 1280\r", "window.height", 720);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "\xef\xbb\xbfwindow:\r  width: 1280\r  height: 720\r");

  const RuntimeConfigEdit top =
      SetRuntimeConfigString("\xef\xbb\xbftitle: a\n", "title", "b");
  ASSERT_TRUE(top) << top.error;
  EXPECT_EQ(top.text, "\xef\xbb\xbftitle: b\n");
}

TEST(RuntimeConfigEditorTest, FindsValuesAfterNonAsciiText) {
  const RuntimeConfigEdit edit = SetRuntimeConfigString(
      "# caf\xc3\xa9 \xe2\x9c\x93\nwindow:\n  title: \"\xc2\xb5 \xe6\x97\xa5\"  "
      "# \xc3\xa9\n  width: 1280\n",
      "window.width", "1920");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "# caf\xc3\xa9 \xe2\x9c\x93\nwindow:\n  title: \"\xc2\xb5 "
            "\xe6\x97\xa5\"  # \xc3\xa9\n  width: 1920\n");
}

TEST(RuntimeConfigEditorTest, ReplacesValuesWrittenInAnyYamlStyle) {
  const std::string before =
      "a:\n"
      "  single: 'it''s # here'  # note\n"
      "  literal: |\n"
      "    line one\n"
      "    key: not a key\n"
      "\n"
      "  folded: >-\n"
      "    one\n"
      "    two\n"
      "  spread: first\n"
      "    second\n"
      "  \"quoted key\": 1\n"
      "  last: 2\n";

  RuntimeConfigEdit edit = SetRuntimeConfigString(before, "a.single", "x");
  ASSERT_TRUE(edit) << edit.error;
  edit = SetRuntimeConfigString(edit.text, "a.literal", "y");
  ASSERT_TRUE(edit) << edit.error;
  edit = SetRuntimeConfigString(edit.text, "a.folded", "z");
  ASSERT_TRUE(edit) << edit.error;
  edit = SetRuntimeConfigString(edit.text, "a.spread", "w");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "a:\n"
            "  single: x  # note\n"
            "  literal: y\n"
            "\n"
            "  folded: z\n"
            "  spread: w\n"
            "  \"quoted key\": 1\n"
            "  last: 2\n");
}

TEST(RuntimeConfigEditorTest, IgnoresCommentLookalikesInsideAValue) {
  const std::string before =
      "a:\n"
      "  note: |\n"
      "    # vsync: off\n"
      "  other: 1\n";

  const RuntimeConfigEdit edit = SetRuntimeConfigString(before, "a.vsync", "on");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text,
            "a:\n"
            "  note: |\n"
            "    # vsync: off\n"
            "  other: 1\n"
            "  vsync: on\n");
}

TEST(RuntimeConfigEditorTest, EditsCompactMappings) {
  const std::string before = "graphics: {backend: direct-vulkan, vsync: off}\n";

  EXPECT_EQ(SetRuntimeConfigString(before, "graphics.vsync", "on").text,
            "graphics: {backend: direct-vulkan, vsync: on}\n");
  EXPECT_EQ(SetRuntimeConfigBool(before, "graphics.hdr", true).text,
            "graphics: {backend: direct-vulkan, vsync: off, hdr: true}\n");
  EXPECT_EQ(SetRuntimeConfigBool(before, "graphics.a.b", true).text,
            "graphics: {backend: direct-vulkan, vsync: off, a: {b: true}}\n");
  EXPECT_EQ(SetRuntimeConfigBool(before, "graphics.vsync.deep", true).text,
            "graphics: {backend: direct-vulkan, vsync: {deep: true}}\n");
  EXPECT_EQ(UnsetRuntimeConfigValue(before, "graphics.backend").text,
            "graphics: {vsync: off}\n");
  EXPECT_EQ(UnsetRuntimeConfigValue(before, "graphics.vsync").text,
            "graphics: {backend: direct-vulkan}\n");
  EXPECT_EQ(SetRuntimeConfigBool("graphics: {}\n", "graphics.hdr", true).text,
            "graphics: {hdr: true}\n");
  EXPECT_EQ(SetRuntimeConfigString(before, "graphics", "none").text,
            "graphics: none\n");
}

TEST(RuntimeConfigEditorTest, ReplacesAListWhereASectionIsNeeded) {
  const RuntimeConfigEdit edit = SetRuntimeConfigBool(
      "a:\n  - one\n  - two\nb: 1\n", "a.flag", true);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text, "a:\n#   - one\n#   - two\n  flag: true\nb: 1\n");
}

TEST(RuntimeConfigEditorTest, StartsAFileThatHasNoSettings) {
  EXPECT_EQ(SetRuntimeConfigBool("", "input.raw_mouse", true).text,
            "input:\n  raw_mouse: true\n");
  EXPECT_EQ(
      SetRuntimeConfigBool("# only a comment", "input.raw_mouse", true).text,
      "# only a comment\ninput:\n  raw_mouse: true\n");
}

TEST(RuntimeConfigEditorTest, RefusesAFileItCannotRead) {
  EXPECT_FALSE(SetRuntimeConfigBool("a:\n\tb: 1\n", "a.b", true));
  EXPECT_FALSE(SetRuntimeConfigBool("- a\n- b\n", "a.b", true));
  EXPECT_FALSE(SetRuntimeConfigBool("a: [1, 2\n", "a", true));
  const RuntimeConfigEdit edit =
      SetRuntimeConfigBool("\xff\xfe" "a\0:\0", "a", true);
  EXPECT_FALSE(edit);
  EXPECT_TRUE(edit.text.empty());
}

TEST(RuntimeConfigEditorTest, RejectsAMalformedKey) {
  EXPECT_FALSE(SetRuntimeConfigBool("a: 1\n", "", true));
  EXPECT_FALSE(SetRuntimeConfigBool("a: 1\n", "a..b", true));
  EXPECT_FALSE(SetRuntimeConfigBool("a: 1\n", "a.b c", true));
}

TEST(RuntimeConfigEditorTest, EditsToTheFirstRunFileLoadWithTheNewValues) {
  std::string text(DefaultRuntimeConfigYaml());
  const std::size_t lines = LineCount(text);

  const auto apply = [&text](const RuntimeConfigEdit& edit) {
    ASSERT_TRUE(edit) << edit.error;
    text = edit.text;
  };
  apply(SetRuntimeConfigString(text, "graphics.backend", "opengl"));
  apply(SetRuntimeConfigString(text, "graphics.vsync", "on"));
  apply(SetRuntimeConfigInteger(text, "graphics.frame_rate_limit", 144));
  apply(SetRuntimeConfigBool(text, "input.raw_mouse", true));
  apply(SetRuntimeConfigString(text, "performance.gamemode", "off"));
  apply(SetRuntimeConfigBool(text, "integrations.discord_rpc.enabled", false));
  apply(SetRuntimeConfigString(text, "integrations.discord_rpc.text.playing",
                               "In {place_name}"));
  apply(SetRuntimeConfigString(text, "integrations.discord_rpc.images.small",
                               ""));

  // Every one of these keys ships in the template, active or commented.
  EXPECT_EQ(LineCount(text), lines);
  EXPECT_FALSE(Contains(text, "# vsync:"));

  const RuntimeConfigLoadResult loaded = Load(text);
  ASSERT_TRUE(loaded) << loaded.error;
  const RuntimeConfig& config = loaded.config;
  EXPECT_EQ(config.graphics_backend_name(), "opengl");
  EXPECT_EQ(config.vsync_mode(), "on");
  EXPECT_TRUE(config.input_capabilities().raw_mouse);
  EXPECT_FALSE(config.fleasion_enabled());
  EXPECT_FALSE(config.discord_rpc().enabled);
  EXPECT_EQ(config.discord_rpc().text.playing, "In {place_name}");
  EXPECT_EQ(config.discord_rpc().text.browsing, "Browsing experiences");
  EXPECT_EQ(config.discord_rpc().images.small, "");
  EXPECT_EQ(config.discord_rpc().images.large, "{place_icon}");
}

TEST(RuntimeConfigEditorTest, FirstRunFileLoadsAfterEverySetAndUnset) {
  const std::pair<const char*, const char*> keys[] = {
      {"graphics.backend", "opengl"},
      {"graphics.vsync", "on"},
      {"input.raw_mouse", "true"},
      {"performance.gamemode", "off"},
      {"integrations.discord_rpc.enabled", "false"},
      {"integrations.discord_rpc.text.playing", "In {place_name}"},
      {"integrations.discord_rpc.images.small", ""},
  };
  for (const char* eol : {"\n", "\r\n"}) {
    std::string text;
    for (const char c : DefaultRuntimeConfigYaml()) {
      text += c == '\n' ? std::string(eol) : std::string(1, c);
    }
    for (const auto& [key, value] : keys) {
      for (int round = 0; round < 3; ++round) {
        const RuntimeConfigEdit edit =
            round == 1 ? UnsetRuntimeConfigValue(text, key)
                       : SetRuntimeConfigString(text, key, value);
        ASSERT_TRUE(edit) << key << " round " << round << ": " << edit.error;
        text = edit.text;
        const RuntimeConfigLoadResult loaded = Load(text);
        ASSERT_TRUE(loaded) << key << " round " << round << ": "
                            << loaded.error;
      }
    }
    EXPECT_FALSE(Contains(text, eol[0] == '\r' ? "x\ny" : "\r"));
  }
}

TEST(RuntimeConfigEditorTest, EscapesLineBreaksAndControlCharactersInAValue) {
  const RuntimeConfigEdit edit = SetRuntimeConfigString(
      "k: v\n", "k", "one\ntwo\tx\xc2\x85y\xe2\x80\xa8z");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text, "k: \"one\\x0Atwo\\x09x\\x85y\\Lz\"\n");
}

TEST(RuntimeConfigEditorTest, RefusesAFileWhoseAliasesLoop) {
  EXPECT_FALSE(SetRuntimeConfigString("a: &x [*x]\nb: 2\n", "b", "3"));
  EXPECT_FALSE(
      SetRuntimeConfigString("a: &x\n  y: 1\n  z: *x\nb: 2\n", "b", "3"));
}

TEST(RuntimeConfigEditorTest, RefusesAFileThatSetsAKeyTwice) {
  const RuntimeConfigEdit edit =
      SetRuntimeConfigString("k: 1\nz: 2\nk: 3\n", "z", "4");
  EXPECT_FALSE(edit);
  EXPECT_TRUE(Contains(edit.error, "k is set twice")) << edit.error;
}

TEST(RuntimeConfigEditorTest, ChangesAKeyThatHoldsAnAlias) {
  EXPECT_EQ(SetRuntimeConfigString("a: &x 1\nb: *x\n", "b", "2").text,
            "a: &x 1\nb: 2\n");
  EXPECT_EQ(UnsetRuntimeConfigValue("a: &x 1\nb: *x\nc: 1\n", "b").text,
            "a: &x 1\n# b: *x\nc: 1\n");
  EXPECT_EQ(
      UnsetRuntimeConfigValue("a: &x 1\ng: {p: 1, q: *x}\n", "g.q").text,
      "a: &x 1\ng: {p: 1}\n");
  EXPECT_EQ(SetRuntimeConfigString("a: &x 1\ng: {q: *x, p: 1}\n", "g.q", "2")
                .text,
            "a: &x 1\ng: {q: 2, p: 1}\n");
  EXPECT_EQ(
      SetRuntimeConfigString("base: &b\n  x: 1\nother: *b\n", "other", "v")
          .text,
      "base: &b\n  x: 1\nother: v\n");

  // A key reached through an alias lives in another section's lines.
  const RuntimeConfigEdit through = SetRuntimeConfigString(
      "base: &b\n  x: 1\nother: *b\n", "other.x", "2");
  EXPECT_FALSE(through);
  EXPECT_TRUE(Contains(through.error, "alias")) << through.error;
}

TEST(RuntimeConfigEditorTest, KeepsAnAnchorOnAChangedValue) {
  EXPECT_EQ(SetRuntimeConfigString("a: &x 1\nb: 2\n", "a", "3").text,
            "a: &x 3\nb: 2\n");
  EXPECT_EQ(SetRuntimeConfigString("a: &x\n  y: 1\nb: 2\n", "a", "3").text,
            "a: &x 3\n#   y: 1\nb: 2\n");
  // Changing a value that an alias copies would change the alias too.
  EXPECT_FALSE(SetRuntimeConfigString("a: &x 1\nb: *x\n", "a", "3"));
}

TEST(RuntimeConfigEditorTest, StartsAFileThatHasOnlyADocumentMarker) {
  EXPECT_EQ(SetRuntimeConfigString("---\n", "b", "2").text, "---\nb: 2\n");
}

TEST(RuntimeConfigEditorTest, AddsAKeyBelowABlockValueThatKeepsBlankLines) {
  const RuntimeConfigEdit edit = SetRuntimeConfigInteger(
      "a:\n  k: |+\n    text\n\nz: 1\n", "a.n", 2);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text, "a:\n  k: |+\n    text\n\n  n: 2\nz: 1\n");
}

TEST(RuntimeConfigEditorTest, UnsetsTheOnlyKeyOfACompactMapping) {
  EXPECT_EQ(UnsetRuntimeConfigValue("g: {a: 1,}\n", "g.a").text, "g: {}\n");
  EXPECT_EQ(UnsetRuntimeConfigValue("g: { a: 1 }\n", "g.a").text, "g: {}\n");
}

TEST(RuntimeConfigEditorTest, KeepsCommentsAroundAReplacedBlockValue) {
  EXPECT_EQ(SetRuntimeConfigString(
                "a:\n  k: |\n    text\n\n  # keep me\n  z: 1\n", "a.k", "y")
                .text,
            "a:\n  k: y\n\n  # keep me\n  z: 1\n");
}

TEST(RuntimeConfigEditorTest, KeepsABlockValueThatEndsAFileWithNoLineBreak) {
  // The added line break would otherwise join the value.
  const RuntimeConfigEdit edit =
      SetRuntimeConfigInteger("note: |\n  text", "width", 1);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text, "note: |-\n  text\nwidth: 1\n");
}

TEST(RuntimeConfigEditorTest, DoesNotReadACommentedKeyAsItsUnclesChild) {
  const std::string before =
      "a: 1\n"
      "# b:\n"
      "#   c:\n"
      "#     b: 2\n";

  // "#     b" belongs to the commented c, so b.b is absent.
  const RuntimeConfigEdit edit = SetRuntimeConfigInteger(before, "b.b", 3);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text, before + "b:\n  b: 3\n");
}

TEST(RuntimeConfigEditorTest, UncommentsAKeyAtItsSectionsOwnIndentation) {
  const RuntimeConfigEdit edit = SetRuntimeConfigString(
      "a:\n#   vsync: off\n    backend: x\n", "a.vsync", "on");
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text, "a:\n    vsync: on\n    backend: x\n");
}

TEST(RuntimeConfigEditorTest, LeavesACommentedKeyThatWouldSplitASection) {
  const std::string before =
      "a:\n"
      "# b: 1\n"
      "  c: 2\n";

  const RuntimeConfigEdit edit = SetRuntimeConfigInteger(before, "b", 5);
  ASSERT_TRUE(edit) << edit.error;
  EXPECT_EQ(edit.text, before + "b: 5\n");
}

// Writes random but valid config files in many YAML styles.
class RandomConfig {
 public:
  RandomConfig(unsigned seed, std::string eol)
      : random_(seed), eol_(std::move(eol)) {}

  std::string Build() {
    std::string text;
    if (Chance(4)) {
      text += "# header: not a key" + eol_;
    }
    Mapping(0, 0, &text);
    if (Chance(5) && text.size() >= eol_.size()) {
      text.resize(text.size() - eol_.size());
    }
    return text;
  }

  std::string Key() { return "k" + std::to_string(random_() % 5); }
  bool Chance(unsigned one_in) { return random_() % one_in == 0; }
  unsigned Pick(unsigned count) { return random_() % count; }

 private:
  void Mapping(std::size_t indent, int depth, std::string* text) {
    const std::string pad(indent, ' ');
    bool any = false;
    for (int index = 0; index < 5; ++index) {
      const std::string key = "k" + std::to_string(index);
      if (Chance(3) && (any || index < 4)) {
        if (Chance(3)) {
          *text += pad + "# " + key + ": commented" + eol_;
        }
        continue;
      }
      any = true;
      if (Chance(4)) {
        *text += eol_;
      }
      if (Chance(4)) {
        *text += pad + "# note: about " + key + eol_;
      }
      switch (depth < 2 ? Pick(9) : 2 + Pick(7)) {
        case 0:
        case 1:
          *text += pad + key + ":" + eol_;
          Mapping(indent + 2, depth + 1, text);
          break;
        case 2:
          *text += pad + key + ": plain words" + eol_;
          break;
        case 3:
          *text += pad + key + ": \"quoted # \\\" text\"  # trailing" + eol_;
          break;
        case 4:
          *text += pad + key + ": 'single # ''quote'" + eol_;
          break;
        case 5:
          *text += pad + key + ": |" + eol_ + pad + "  first: line" + eol_ +
                   pad + "  # second" + eol_;
          if (Chance(2)) {
            *text += eol_;
          }
          break;
        case 6:
          *text += pad + key + ":" + eol_;
          break;
        case 7:
          *text += pad + key + ": {k0: 1, k1: \"two\", k2: {k3: x}}" + eol_;
          break;
        default:
          *text += pad + key + ": caf\xc3\xa9 \xe2\x9c\x93" + eol_;
          break;
      }
    }
  }

  std::mt19937 random_;
  std::string eol_;
};

TEST(RuntimeConfigEditorTest, RandomEditsToRandomFilesAlwaysApply) {
  for (unsigned seed = 0; seed < 400; ++seed) {
    const char* const eols[] = {"\n", "\r\n", "\r"};
    RandomConfig random(seed, eols[seed % 3]);
    std::string text = random.Build();
    for (int step = 0; step < 30; ++step) {
      std::string key = random.Key();
      for (unsigned extra = random.Pick(3); extra > 0; --extra) {
        key += "." + random.Key();
      }
      RuntimeConfigEdit edit;
      const unsigned action = random.Pick(5);
      switch (action) {
        case 0:
          edit = UnsetRuntimeConfigValue(text, key);
          break;
        case 1:
          edit = SetRuntimeConfigBool(text, key, random.Chance(2));
          break;
        case 2:
          edit = SetRuntimeConfigInteger(text, key, step - 7);
          break;
        case 3:
          edit = SetRuntimeConfigString(text, key, "a: \"b\" #c \xc2\xb5");
          break;
        default:
          edit = SetRuntimeConfigString(text, key, "word");
          break;
      }
      ASSERT_TRUE(edit) << "seed " << seed << " step " << step << " key "
                        << key << " action " << action << ": " << edit.error
                        << "\n--- file ---\n"
                        << text;
      if (action == 4) {
        EXPECT_EQ(SetRuntimeConfigString(edit.text, key, "word").text,
                  edit.text)
            << "seed " << seed << " step " << step;
      }
      if (action == 0) {
        EXPECT_EQ(UnsetRuntimeConfigValue(edit.text, key).text, edit.text)
            << "seed " << seed << " step " << step;
      }
      text = edit.text;
    }
  }
}

TEST(RuntimeConfigEditorTest, SettingTheSameValueTwiceChangesNothingMore) {
  const std::string once =
      SetRuntimeConfigString(std::string(DefaultRuntimeConfigYaml()),
                             "graphics.vsync", "on")
          .text;
  EXPECT_EQ(SetRuntimeConfigString(once, "graphics.vsync", "on").text, once);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
