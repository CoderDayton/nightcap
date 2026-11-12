#include "ui/sky_pack.h"

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <minizip/zip.h>
#include <stdlib.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::string g_fail_to;
std::string g_fail_from;

}  // namespace

// Takes the place of libc's for std::filesystem::rename in this binary.
extern "C" int rename(const char* from, const char* to) noexcept {
  const auto ends_with = [](std::string_view text, std::string_view end) {
    return text.size() >= end.size() &&
           text.substr(text.size() - end.size()) == end;
  };
  if (!g_fail_to.empty() && ends_with(to, g_fail_to) &&
      ends_with(from, g_fail_from)) {
    errno = EIO;
    return -1;
  }
  using Rename = int (*)(const char*, const char*);
  static const auto real =
      reinterpret_cast<Rename>(dlsym(RTLD_NEXT, "rename"));
  return real(from, to);
}

namespace mocktail {
namespace ui {
namespace {

struct FailRename {
  FailRename(std::string from, std::string to) {
    g_fail_from = std::move(from);
    g_fail_to = std::move(to);
  }
  ~FailRename() { g_fail_to.clear(); }
};

constexpr std::string_view kFaces[] = {"bk", "dn", "ft", "lf", "rt", "up"};
std::string BigEndian(std::uint32_t value) {
  return {static_cast<char>(value >> 24), static_cast<char>(value >> 16),
          static_cast<char>(value >> 8), static_cast<char>(value)};
}
std::string LittleEndian(std::uint32_t value) {
  return {static_cast<char>(value), static_cast<char>(value >> 8),
          static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
}
// A PNG signature and IHDR chunk header; the pixels never get decoded.
std::string PngOf(std::uint32_t width, std::uint32_t height) {
  return std::string("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16) +
         BigEndian(width) + BigEndian(height) + "pixels";
}
std::string KtxOf(std::uint32_t width, std::uint32_t height) {
  return std::string("\xabKTX 11\xbb\r\n\x1a\n", 12) +
         LittleEndian(0x04030201) + std::string(20, '\0') +
         LittleEndian(width) + LittleEndian(height) + "blocks";
}
// A JPEG whose size sits after a long APP1 segment, as with EXIF data.
std::string JpegOf(std::uint32_t width, std::uint32_t height) {
  const std::string app1 = std::string("\xff\xe1\x03\xea", 4) +
                           std::string(1000, 'x');
  const std::string sof =
      std::string("\xff\xc2\x00\x11\x08", 5) +
      BigEndian(height).substr(2) + BigEndian(width).substr(2) +
      std::string(10, '\0');
  return std::string("\xff\xd8", 2) + app1 + sof + "scan";
}
std::string Riff(const std::string& chunk) {
  return "RIFF" + LittleEndian(static_cast<std::uint32_t>(chunk.size() + 4)) +
         "WEBP" + chunk;
}
std::string Le24(std::uint32_t value) { return LittleEndian(value).substr(0, 3); }
// Extended WebP, which stores the size minus one in 24 bits.
std::string WebpOf(std::uint32_t width, std::uint32_t height) {
  return Riff("VP8X" + LittleEndian(10) + std::string("\x10\0\0\0", 4) +
              Le24(width - 1) + Le24(height - 1));
}
std::string LossyWebpOf(std::uint32_t width, std::uint32_t height) {
  return Riff("VP8 " + LittleEndian(10) + std::string("\0\0\0\x9d\x01\x2a", 6) +
              LittleEndian(width).substr(0, 2) +
              LittleEndian(height).substr(0, 2));
}
std::string LosslessWebpOf(std::uint32_t width, std::uint32_t height) {
  return Riff("VP8L" + LittleEndian(5) + "\x2f" +
              LittleEndian((width - 1) | ((height - 1) << 14)));
}
std::string DdsOf(std::uint32_t width, std::uint32_t height) {
  return "DDS " + LittleEndian(124) + LittleEndian(0x1007) +
         LittleEndian(height) + LittleEndian(width) + "blocks";
}
const std::string kPng = PngOf(512, 512);
const std::string kKtx = KtxOf(512, 512);

class SkyPackTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "sky_pack_test_XXXXXX")
            .string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    root_ = pattern;
    source_ = root_ / "pack";
    overlay_ = root_ / "asset_overlay";
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  static void Write(const std::filesystem::path& path,
                    const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
  }

  static std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
  }

  void WritePack(const std::filesystem::path& folder, std::string_view prefix,
                 std::string_view extension, const std::string& bytes) {
    for (const std::string_view face : kFaces) {
      Write(folder / (std::string(prefix) + std::string(face) +
                      std::string(extension)),
            bytes + std::string(face));
    }
  }

  std::filesystem::path Installed(std::string_view face) const {
    return SkyFolder(overlay_) / ("sky512_" + std::string(face) + ".tex");
  }

  std::filesystem::path Indoor(std::string_view face) const {
    return SkyFolder(overlay_) / ("indoor512_" + std::string(face) + ".tex");
  }

  void ExpectOnlyTheSky(const std::string& bytes) const {
    for (const std::string_view face : kFaces) {
      EXPECT_EQ(Read(Installed(face)), bytes + std::string(face)) << face;
      EXPECT_EQ(Read(Indoor(face)), bytes + std::string(face)) << face;
    }
    int files = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(SkyFolder(overlay_))) {
      EXPECT_EQ(entry.path().extension(), ".tex") << entry.path();
      ++files;
    }
    EXPECT_EQ(files, 12);
  }

  std::filesystem::path root_;
  std::filesystem::path source_;
  std::filesystem::path overlay_;
};

TEST_F(SkyPackTest, SkyFolderIsWhereRobloxLooks) {
  EXPECT_EQ(SkyFolder("/o"), "/o/content/android/textures/sky");
}

TEST_F(SkyPackTest, InstallsSixPngFacesAsSky512Tex) {
  WritePack(source_, "Aurora_", ".png", kPng);
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  for (const std::string_view face : kFaces) {
    EXPECT_EQ(Read(Installed(face)), kPng + std::string(face)) << face;
  }
  EXPECT_TRUE(HasCustomSky(overlay_));
}

// Reflections come from indoor512, so it gets the sky too, and goes with it.
TEST_F(SkyPackTest, TheSkyAlsoReplacesTheIndoorSky) {
  WritePack(source_, "Aurora_", ".png", kPng);
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  ExpectOnlyTheSky(kPng);
  ASSERT_TRUE(RemoveCustomSky(overlay_, &error)) << error;
  EXPECT_TRUE(std::filesystem::is_empty(SkyFolder(overlay_)));
}

TEST_F(SkyPackTest, AHandMadeIndoorFaceIsKept) {
  Write(Indoor("up"), kKtx + "hand");
  WritePack(source_, "Aurora_", ".png", kPng);
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  EXPECT_EQ(Read(Indoor("up")), kKtx + "hand");
  EXPECT_FALSE(std::filesystem::exists(Indoor("bk")));
  ASSERT_TRUE(RemoveCustomSky(overlay_, &error)) << error;
  EXPECT_EQ(Read(Indoor("up")), kKtx + "hand");
}

// A sky installed before indoor512 was part of it needs installing again.
TEST_F(SkyPackTest, ASkyWithoutItsIndoorFacesIsIncomplete) {
  EXPECT_FALSE(IndoorSkyMissing(overlay_));
  WritePack(source_, "Aurora_", ".png", kPng);
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  EXPECT_FALSE(IndoorSkyMissing(overlay_));
  std::filesystem::remove(Indoor("up"));
  EXPECT_TRUE(IndoorSkyMissing(overlay_));
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  EXPECT_FALSE(IndoorSkyMissing(overlay_));
  std::filesystem::remove(Indoor("up"));
  Write(Indoor("up"), kKtx + "hand");
  EXPECT_FALSE(IndoorSkyMissing(overlay_));
}

TEST_F(SkyPackTest, FindsFacesInAPacksSubfolder) {
  WritePack(source_ / "content" / "android" / "textures" / "sky", "sky512_",
            ".tex", kKtx);
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  EXPECT_EQ(Read(Installed("up")), kKtx + "up");
}

TEST_F(SkyPackTest, PrefersSky512OverIndoorFaces) {
  WritePack(source_, "indoor512_", ".tex", kKtx + "indoor");
  WritePack(source_, "sky512_", ".tex", kKtx);
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  EXPECT_EQ(Read(Installed("bk")), kKtx + "bk");
}

TEST_F(SkyPackTest, AMissingFaceCopiesNothing) {
  WritePack(source_, "sky_", ".png", kPng);
  std::filesystem::remove(source_ / "sky_up.png");
  std::string error;
  EXPECT_FALSE(InstallSky(source_, overlay_, &error));
  EXPECT_NE(error.find("up"), std::string::npos) << error;
  EXPECT_FALSE(HasCustomSky(overlay_));
}

TEST_F(SkyPackTest, TwoCandidatesForAFaceAreRefused) {
  WritePack(source_ / "day", "sky_", ".png", kPng);
  WritePack(source_ / "night", "sky_", ".png", kPng);
  std::string error;
  EXPECT_FALSE(InstallSky(source_, overlay_, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(HasCustomSky(overlay_));
}

TEST_F(SkyPackTest, AFileThatIsNotAnImageIsRefused) {
  WritePack(source_, "sky_", ".png", kPng);
  Write(source_ / "sky_dn.png", "not an image");
  std::string error;
  EXPECT_FALSE(InstallSky(source_, overlay_, &error));
  EXPECT_NE(error.find("sky_dn.png"), std::string::npos) << error;
  EXPECT_FALSE(HasCustomSky(overlay_));
}

TEST_F(SkyPackTest, ReplacesAnInstalledSky) {
  WritePack(source_, "old_", ".png", kPng + "old");
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  const std::filesystem::path next = root_ / "next";
  WritePack(next, "new_", ".png", kPng + "new");
  ASSERT_TRUE(InstallSky(next, overlay_, &error)) << error;
  EXPECT_EQ(Read(Installed("lf")), kPng + "newlf");
  int files = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(SkyFolder(overlay_))) {
    EXPECT_EQ(entry.path().extension(), ".tex") << entry.path();
    ++files;
  }
  EXPECT_EQ(files, 12);
}

// The fourth face of six fails, so three are already done when it does.
TEST_F(SkyPackTest, AFailedSwapPutsTheOldSkyBackWhole) {
  WritePack(source_, "old_", ".png", kPng + "old");
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  const std::filesystem::path next = root_ / "next";
  WritePack(next, "new_", ".png", kPng + "new");
  {
    const FailRename fail(".new", "sky512_lf.tex");
    EXPECT_FALSE(InstallSky(next, overlay_, &error));
  }
  ExpectOnlyTheSky(kPng + "old");
}

TEST_F(SkyPackTest, AFailedMoveAsidePutsTheOldSkyBackWhole) {
  WritePack(source_, "old_", ".png", kPng + "old");
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  const std::filesystem::path next = root_ / "next";
  WritePack(next, "new_", ".png", kPng + "new");
  {
    const FailRename fail("sky512_lf.tex", "sky512_lf.tex.old");
    EXPECT_FALSE(InstallSky(next, overlay_, &error));
  }
  ExpectOnlyTheSky(kPng + "old");
}

TEST_F(SkyPackTest, AFailedRemoveKeepsTheWholeSky) {
  WritePack(source_, "old_", ".png", kPng + "old");
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  {
    const FailRename fail("sky512_lf.tex", "sky512_lf.tex.old");
    EXPECT_FALSE(RemoveCustomSky(overlay_, &error));
  }
  ExpectOnlyTheSky(kPng + "old");
}

// A texture pack has files like arrow_up.png beside its sky.
TEST_F(SkyPackTest, FacesMustShareAFolderAndName) {
  WritePack(source_ / "sky", "sky_", ".png", kPng);
  Write(source_ / "ui" / "arrow_up.png", kPng + "arrow");
  Write(source_ / "ui" / "scroll_dn.png", kPng + "scroll");
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  EXPECT_EQ(Read(Installed("up")), kPng + "up");

  std::filesystem::remove(source_ / "sky" / "sky_up.png");
  EXPECT_FALSE(InstallSky(source_, overlay_, &error));
  EXPECT_NE(error.find("up"), std::string::npos) << error;
  EXPECT_EQ(Read(Installed("up")), kPng + "up");
}

// Roblox reads a .tex sky only as KTX 1.
TEST_F(SkyPackTest, Ktx2IsRefused) {
  WritePack(source_, "sky_", ".ktx",
            std::string("\xabKTX 20\xbb\r\n\x1a\n", 12) + "blocks");
  std::string error;
  EXPECT_FALSE(InstallSky(source_, overlay_, &error));
  EXPECT_FALSE(HasCustomSky(overlay_));
}

TEST_F(SkyPackTest, RemoveLeavesOtherOverlayFiles) {
  WritePack(source_, "sky_", ".png", kPng);
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  const std::filesystem::path indoor = SkyFolder(overlay_) / "indoor512_up.tex";
  Write(indoor, kKtx);
  ASSERT_TRUE(RemoveCustomSky(overlay_, &error)) << error;
  EXPECT_FALSE(HasCustomSky(overlay_));
  EXPECT_TRUE(std::filesystem::exists(indoor));
}

// What a swap killed partway leaves: three faces moved aside, a copy half in.
void InterruptSwap(const std::filesystem::path& sky) {
  for (const char* face : {"bk", "dn", "ft"}) {
    const std::filesystem::path installed =
        sky / (std::string("sky512_") + face + ".tex");
    std::filesystem::rename(installed, installed.string() + ".old");
  }
  std::ofstream(sky / "sky512_lf.tex.new") << "half";
}

TEST_F(SkyPackTest, RemovingAfterAnInterruptedSwapLeavesNothingBehind) {
  WritePack(source_, "sky_", ".png", kPng);
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  InterruptSwap(SkyFolder(overlay_));
  ASSERT_TRUE(RemoveCustomSky(overlay_, &error)) << error;
  EXPECT_TRUE(std::filesystem::is_empty(SkyFolder(overlay_)));
}

TEST_F(SkyPackTest, InstallingAfterAnInterruptedSwapLeavesOnlyTheNewSky) {
  WritePack(source_, "sky_", ".png", kPng + "old");
  std::string error;
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  InterruptSwap(SkyFolder(overlay_));
  WritePack(source_, "sky_", ".png", kPng);
  ASSERT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  ExpectOnlyTheSky(kPng);
  for (const auto& entry :
       std::filesystem::directory_iterator(SkyFolder(overlay_))) {
    EXPECT_EQ(entry.path().extension(), ".tex") << entry.path();
  }
}

TEST_F(SkyPackTest, RemovingNoSkyIsNotAnError) {
  std::string error;
  EXPECT_TRUE(RemoveCustomSky(overlay_, &error)) << error;
}

using Entries = std::vector<std::pair<std::string, std::string>>;

void Zip(const std::filesystem::path& path, const Entries& entries) {
  std::filesystem::create_directories(path.parent_path());
  zipFile archive = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
  ASSERT_NE(archive, nullptr);
  for (const auto& [name, contents] : entries) {
    ASSERT_EQ(zipOpenNewFileInZip64(archive, name.c_str(), nullptr, nullptr,
                                    0, nullptr, 0, nullptr, Z_DEFLATED,
                                    Z_DEFAULT_COMPRESSION, 1),
              ZIP_OK);
    ASSERT_EQ(zipWriteInFileInZip(archive, contents.data(),
                                  static_cast<unsigned>(contents.size())),
              ZIP_OK);
    ASSERT_EQ(zipCloseFileInZip(archive), ZIP_OK);
  }
  ASSERT_EQ(zipClose(archive, nullptr), ZIP_OK);
}

Entries PackEntries(const std::string& folder, const std::string& bytes) {
  Entries entries;
  for (const std::string_view face : kFaces) {
    entries.emplace_back(folder + "sky512_" + std::string(face) + ".tex",
                         bytes + std::string(face));
  }
  return entries;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

TEST_F(SkyPackTest, ImportsAFolderUnderItsName) {
  WritePack(root_ / "Aurora", "Aurora_", ".png", kPng);
  const std::filesystem::path library = root_ / "skies";
  const SkyImport imported = ImportSkies(root_ / "Aurora", library);
  ASSERT_TRUE(imported.error.empty()) << imported.error;
  EXPECT_EQ(imported.added, std::vector<std::string>{"Aurora"});
  EXPECT_EQ(ListSkyLibrary(library), std::vector<std::string>{"Aurora"});
}

// macOS zips carry an AppleDouble ._ file for each face.
TEST_F(SkyPackTest, AppleDoubleFilesAreNotAPack) {
  Entries entries = PackEntries("Aurora/", kPng);
  for (const std::string_view face : kFaces) {
    entries.emplace_back(
        "__MACOSX/Aurora/._sky512_" + std::string(face) + ".tex",
        std::string("\0\x05\x16\x07", 4));
  }
  Zip(root_ / "Aurora.zip", entries);
  const SkyImport imported = ImportSkies(root_ / "Aurora.zip", root_ / "skies");
  ASSERT_TRUE(imported.error.empty()) << imported.error;
  EXPECT_EQ(imported.added, std::vector<std::string>{"Aurora"});
  EXPECT_TRUE(imported.skipped.empty()) << imported.skipped.front();
}

// Like skyboxcolectionrobloxbynirv.zip: zips of packs in many layouts.
TEST_F(SkyPackTest, ImportsEveryPackInAZipOfZips) {
  const std::filesystem::path packs = root_ / "packs";
  Zip(packs / "Aurora.zip", PackEntries("Aurora/", kPng + "aurora"));
  Zip(packs / "castle grounds skybox.zip",
      PackEntries("castle grounds skybox/PlatformContent/pc/textures/sky/",
                  kPng + "castle"));
  Zip(packs / "MCSkybox.zip",
      PackEntries("PlatformContent/pc/textures/sky/", kPng + "mc"));
  Entries collection = {{"Pre-view Images/Aurora.png", kPng},
                        {"Tutorial.mp4", "video"}};
  for (const char* pack :
       {"Aurora.zip", "castle grounds skybox.zip", "MCSkybox.zip"}) {
    collection.emplace_back(std::string("Skyboxes/") + pack,
                            ReadFile(packs / pack));
  }
  Zip(root_ / "collection.zip", collection);

  const std::filesystem::path library = root_ / "skies";
  const SkyImport imported = ImportSkies(root_ / "collection.zip", library);
  ASSERT_TRUE(imported.error.empty()) << imported.error;
  EXPECT_EQ(ListSkyLibrary(library),
            (std::vector<std::string>{"Aurora", "castle grounds skybox",
                                      "MCSkybox"}));
  EXPECT_EQ(imported.added.size(), 3u);
  std::string error;
  ASSERT_TRUE(UseLibrarySky("MCSkybox", library, overlay_, &error)) << error;
  EXPECT_EQ(Read(Installed("rt")), kPng + "mcrt");
}

TEST_F(SkyPackTest, PackZipsAreNamedAfterTheZipNotTheirFolder) {
  const std::filesystem::path packs = root_ / "packs";
  Zip(packs / "Winter.zip", PackEntries("grimnight/", kPng + "winter"));
  Zip(packs / "Dusk.zip", PackEntries("grimnight/", kPng + "dusk"));
  const std::filesystem::path library = root_ / "skies";
  const SkyImport imported = ImportSkies(packs, library);
  ASSERT_TRUE(imported.error.empty()) << imported.error;
  EXPECT_EQ(ListSkyLibrary(library),
            (std::vector<std::string>{"Dusk", "Winter"}));
}

TEST_F(SkyPackTest, TheIndoorSkyBesideAPackIsNotItsOwnPack) {
  WritePack(root_ / "Storm", "sky512_", ".png", kPng);
  WritePack(root_ / "Storm", "indoor512_", ".png", kPng);
  const std::filesystem::path library = root_ / "skies";
  const SkyImport imported = ImportSkies(root_ / "Storm", library);
  ASSERT_TRUE(imported.error.empty()) << imported.error;
  EXPECT_EQ(ListSkyLibrary(library), std::vector<std::string>{"Storm"});
  EXPECT_TRUE(imported.skipped.empty());
}

TEST_F(SkyPackTest, PacksSharingAFolderAreNamedAfterTheirFaces) {
  WritePack(root_ / "Packs", "Aurora_", ".png", kPng);
  WritePack(root_ / "Packs", "Night_", ".png", kPng);
  const std::filesystem::path library = root_ / "skies";
  const SkyImport imported = ImportSkies(root_ / "Packs", library);
  ASSERT_TRUE(imported.error.empty()) << imported.error;
  EXPECT_EQ(ListSkyLibrary(library),
            (std::vector<std::string>{"Aurora", "Night"}));
}

TEST_F(SkyPackTest, OnlyTheIndoorSkyBesideAPackIsLeftOut) {
  WritePack(root_ / "Storm", "sky512_", ".png", kPng);
  WritePack(root_ / "Storm", "Aurora_", ".png", kPng);
  const std::filesystem::path library = root_ / "skies";
  const SkyImport imported = ImportSkies(root_ / "Storm", library);
  ASSERT_TRUE(imported.error.empty()) << imported.error;
  EXPECT_EQ(ListSkyLibrary(library),
            (std::vector<std::string>{"Aurora", "Storm"}));
}

TEST_F(SkyPackTest, ABrokenPackIsSkippedAndTheRestImport) {
  WritePack(root_ / "packs" / "Good", "sky512_", ".png", kPng);
  WritePack(root_ / "packs" / "Broken", "sky512_", ".png", kPng);
  Write(root_ / "packs" / "Broken" / "sky512_up.png", "not an image");
  const std::filesystem::path library = root_ / "skies";
  const SkyImport imported = ImportSkies(root_ / "packs", library);
  ASSERT_TRUE(imported.error.empty()) << imported.error;
  EXPECT_EQ(ListSkyLibrary(library), std::vector<std::string>{"Good"});
  ASSERT_EQ(imported.skipped.size(), 1u);
  EXPECT_NE(imported.skipped[0].find("Broken"), std::string::npos)
      << imported.skipped[0];
}

TEST_F(SkyPackTest, AFolderWithNoSkyIsAnError) {
  Write(root_ / "empty" / "readme.txt", "x");
  const SkyImport imported = ImportSkies(root_ / "empty", root_ / "skies");
  EXPECT_FALSE(imported.error.empty());
  EXPECT_TRUE(ListSkyLibrary(root_ / "skies").empty());
}

TEST_F(SkyPackTest, ImportingAgainReplacesThePack) {
  WritePack(root_ / "Night", "sky_", ".png", kPng + "old");
  const std::filesystem::path library = root_ / "skies";
  ASSERT_TRUE(ImportSkies(root_ / "Night", library).error.empty());
  WritePack(root_ / "Night", "sky_", ".png", kPng + "new");
  ASSERT_TRUE(ImportSkies(root_ / "Night", library).error.empty());
  EXPECT_EQ(ListSkyLibrary(library), std::vector<std::string>{"Night"});
  std::string error;
  ASSERT_TRUE(UseLibrarySky("Night", library, overlay_, &error)) << error;
  EXPECT_EQ(Read(Installed("bk")), kPng + "newbk");
}

TEST_F(SkyPackTest, KnowsWhichLibrarySkyIsInstalled) {
  const std::filesystem::path library = root_ / "skies";
  WritePack(root_ / "Day", "sky_", ".png", kPng + "day");
  WritePack(root_ / "Dusk", "sky_", ".png", kPng + "dusky");
  ASSERT_TRUE(ImportSkies(root_ / "Day", library).error.empty());
  ASSERT_TRUE(ImportSkies(root_ / "Dusk", library).error.empty());
  EXPECT_EQ(InstalledSky(overlay_, library), std::optional<std::string>(""));

  std::string error;
  ASSERT_TRUE(UseLibrarySky("Dusk", library, overlay_, &error)) << error;
  EXPECT_EQ(InstalledSky(overlay_, library),
            std::optional<std::string>("Dusk"));

  Write(Installed("up"), kPng + "hand-made");
  EXPECT_EQ(InstalledSky(overlay_, library), std::nullopt);

  ASSERT_TRUE(RemoveCustomSky(overlay_, &error)) << error;
  EXPECT_EQ(InstalledSky(overlay_, library), std::optional<std::string>(""));
}

void WriteHandMadeSky(const std::filesystem::path& overlay) {
  std::filesystem::create_directories(SkyFolder(overlay));
  for (const std::string_view face : kFaces) {
    std::ofstream(SkyFolder(overlay) / ("sky512_" + std::string(face) + ".tex"),
                  std::ios::binary)
        << kPng + "hand" + std::string(face);
  }
}

TEST_F(SkyPackTest, AHandMadeSkyIsKeptWhenALibrarySkyIsPicked) {
  const std::filesystem::path library = root_ / "skies";
  WritePack(root_ / "Day", "sky_", ".png", kPng + "day");
  ASSERT_TRUE(ImportSkies(root_ / "Day", library).error.empty());
  WriteHandMadeSky(overlay_);
  std::string error;
  std::string kept;
  ASSERT_TRUE(UseLibrarySky("Day", library, overlay_, &error, &kept))
      << error;
  EXPECT_EQ(kept, "Custom sky");
  EXPECT_EQ(ListSkyLibrary(library),
            (std::vector<std::string>{"Custom sky", "Day"}));
  EXPECT_EQ(Read(library / "Custom sky" / "sky512_bk.tex"), kPng + "handbk");
  EXPECT_EQ(Read(Installed("bk")), kPng + "daybk");
}

TEST_F(SkyPackTest, AHandMadeSkyIsKeptWhenRobloxsIsPicked) {
  const std::filesystem::path library = root_ / "skies";
  WriteHandMadeSky(overlay_);
  std::string error;
  std::string kept;
  ASSERT_TRUE(UseRobloxSky(library, overlay_, &error, &kept)) << error;
  EXPECT_EQ(kept, "Custom sky");
  EXPECT_FALSE(HasCustomSky(overlay_));
  EXPECT_EQ(ListSkyLibrary(library), std::vector<std::string>{"Custom sky"});
}

TEST_F(SkyPackTest, ALibrarySkyIsNotKeptTwice) {
  const std::filesystem::path library = root_ / "skies";
  WritePack(root_ / "Day", "sky_", ".png", kPng);
  ASSERT_TRUE(ImportSkies(root_ / "Day", library).error.empty());
  std::string error;
  std::string kept;
  ASSERT_TRUE(UseLibrarySky("Day", library, overlay_, &error)) << error;
  ASSERT_TRUE(UseRobloxSky(library, overlay_, &error, &kept)) << error;
  EXPECT_TRUE(kept.empty());
  EXPECT_EQ(ListSkyLibrary(library), std::vector<std::string>{"Day"});
}

TEST_F(SkyPackTest, ImportStopsAtItsSizeLimit) {
  WritePack(root_ / "packs" / "A", "sky512_", ".png", kPng);
  WritePack(root_ / "packs" / "B", "sky512_", ".png", kPng);
  SkyImportLimits limits;
  limits.max_bytes = 6 * (kPng.size() + 2) + 1;
  const SkyImport imported =
      ImportSkies(root_ / "packs", root_ / "skies", limits);
  EXPECT_EQ(imported.added, std::vector<std::string>{"A"});
  ASSERT_FALSE(imported.skipped.empty());
  EXPECT_NE(imported.skipped.back().find("limit"), std::string::npos)
      << imported.skipped.back();
}

// Where a sky came from is not recorded when it was put there by hand.
TEST_F(SkyPackTest, AHandMadeCopyOfALibrarySkyIsThatSky) {
  const std::filesystem::path library = root_ / "skies";
  WritePack(root_ / "Day", "sky_", ".png", kPng + "day");
  WritePack(root_ / "Night", "sky_", ".png", kPng + "night");
  ASSERT_TRUE(ImportSkies(root_ / "Day", library).error.empty());
  ASSERT_TRUE(ImportSkies(root_ / "Night", library).error.empty());
  std::string error;
  ASSERT_TRUE(InstallSky(root_ / "Day", overlay_, &error)) << error;
  EXPECT_EQ(InstalledSky(overlay_, library), std::optional<std::string>("Day"));
  std::string kept;
  ASSERT_TRUE(UseLibrarySky("Night", library, overlay_, &error, &kept))
      << error;
  EXPECT_TRUE(kept.empty());
  EXPECT_EQ(ListSkyLibrary(library),
            (std::vector<std::string>{"Day", "Night"}));
}

TEST_F(SkyPackTest, AnEditedFaceOfTheSameSizeIsNotTheLibrarys) {
  const std::filesystem::path library = root_ / "skies";
  WritePack(root_ / "Day", "sky_", ".png", kPng + "day");
  ASSERT_TRUE(ImportSkies(root_ / "Day", library).error.empty());
  std::string error;
  ASSERT_TRUE(UseLibrarySky("Day", library, overlay_, &error)) << error;
  Write(Installed("up"), kPng + "dax" + "up");
  EXPECT_EQ(InstalledSky(overlay_, library), std::nullopt);
}

TEST_F(SkyPackTest, AnImportNeverReplacesAKeptSky) {
  const std::filesystem::path library = root_ / "skies";
  WriteHandMadeSky(overlay_);
  std::string error;
  std::string kept;
  ASSERT_TRUE(UseRobloxSky(library, overlay_, &error, &kept)) << error;
  ASSERT_EQ(kept, "Custom sky");
  WritePack(root_ / "Custom sky", "sky_", ".png", kPng + "pack");
  const SkyImport imported = ImportSkies(root_ / "Custom sky", library);
  ASSERT_TRUE(imported.error.empty()) << imported.error;
  EXPECT_EQ(imported.added, std::vector<std::string>{"Custom sky 2"});
  EXPECT_EQ(Read(library / "Custom sky" / "sky512_bk.tex"), kPng + "handbk");
}

TEST_F(SkyPackTest, ImportClearsWhatAKilledImportLeft) {
  const std::filesystem::path library = root_ / "skies";
  Write(library / ".import-123-0" / "Aurora" / "sky512_bk.png", kPng);
  Write(library / ".Aurora.new" / "sky512_bk.png", kPng);
  WritePack(root_ / "Day", "sky_", ".png", kPng);
  ASSERT_TRUE(ImportSkies(root_ / "Day", library).error.empty());
  EXPECT_FALSE(std::filesystem::exists(library / ".import-123-0"));
  EXPECT_FALSE(std::filesystem::exists(library / ".Aurora.new"));
}

TEST_F(SkyPackTest, ACancelledImportAddsNothingMore) {
  WritePack(root_ / "packs" / "A", "sky512_", ".png", kPng);
  const std::atomic<bool> stop{true};
  const SkyImport imported =
      ImportSkies(root_ / "packs", root_ / "skies", {}, &stop);
  EXPECT_TRUE(imported.cancelled);
  EXPECT_TRUE(imported.added.empty());
  EXPECT_TRUE(ListSkyLibrary(root_ / "skies").empty());
}

TEST_F(SkyPackTest, ImportStopsInAFolderOfTooManyFiles) {
  for (int file = 0; file < 30; ++file) {
    Write(root_ / "home" / ("note" + std::to_string(file) + ".txt"), "x");
  }
  WritePack(root_ / "home" / "zz" / "Day", "sky_", ".png", kPng);
  SkyImportLimits limits;
  limits.max_files = 10;
  const SkyImport imported =
      ImportSkies(root_ / "home", root_ / "skies", limits);
  EXPECT_FALSE(imported.error.empty());
  ASSERT_FALSE(imported.skipped.empty());
  EXPECT_NE(imported.skipped.back().find("too many files"), std::string::npos)
      << imported.skipped.back();
}

TEST_F(SkyPackTest, FacesLargerThan2048PxAreRefused) {
  WritePack(source_, "sky_", ".png", PngOf(2048, 2048));
  std::string error;
  EXPECT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  Write(source_ / "sky_up.png", PngOf(4096, 4096));
  EXPECT_FALSE(InstallSky(source_, overlay_, &error));
  EXPECT_NE(error.find("4096"), std::string::npos) << error;
  WritePack(source_, "sky_", ".ktx", KtxOf(1024, 4096));
  for (const std::string_view face : kFaces) {
    std::filesystem::remove(source_ / ("sky_" + std::string(face) + ".png"));
  }
  EXPECT_FALSE(InstallSky(source_, overlay_, &error));
  EXPECT_NE(error.find("4096"), std::string::npos) << error;
}

TEST_F(SkyPackTest, InstallsJpegAndDdsFaces) {
  std::string error;
  WritePack(source_, "sky_", ".tex", JpegOf(256, 256));
  EXPECT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  WritePack(source_, "sky_", ".tex", DdsOf(512, 512));
  EXPECT_TRUE(InstallSky(source_, overlay_, &error)) << error;

  const std::filesystem::path jpg = root_ / "jpg";
  const std::filesystem::path dds = root_ / "dds";
  std::filesystem::create_directories(jpg);
  std::filesystem::create_directories(dds);
  WritePack(jpg, "sky_", ".jpg", JpegOf(1024, 1024));
  WritePack(dds, "sky_", ".dds", DdsOf(1024, 1024));
  EXPECT_TRUE(InstallSky(jpg, overlay_, &error)) << error;
  EXPECT_TRUE(InstallSky(dds, overlay_, &error)) << error;
}

TEST_F(SkyPackTest, JpegAndDdsFacesLargerThan2048PxAreRefused) {
  std::string error;
  WritePack(source_, "sky_", ".tex", JpegOf(4096, 4096));
  EXPECT_FALSE(InstallSky(source_, overlay_, &error));
  EXPECT_NE(error.find("4096"), std::string::npos) << error;
  WritePack(source_, "sky_", ".tex", DdsOf(512, 4096));
  EXPECT_FALSE(InstallSky(source_, overlay_, &error));
  EXPECT_NE(error.find("4096"), std::string::npos) << error;
}

TEST_F(SkyPackTest, InstallsWebpFaces) {
  std::string error;
  for (const std::string& face :
       {WebpOf(432, 432), LossyWebpOf(512, 512), LosslessWebpOf(1024, 1024)}) {
    WritePack(source_, "sky_", ".tex", face);
    EXPECT_TRUE(InstallSky(source_, overlay_, &error)) << error;
  }
  const std::filesystem::path webp = root_ / "webp";
  std::filesystem::create_directories(webp);
  WritePack(webp, "sky_", ".webp", WebpOf(256, 256));
  EXPECT_TRUE(InstallSky(webp, overlay_, &error)) << error;
}

TEST_F(SkyPackTest, WebpFacesLargerThan2048PxAreRefused) {
  std::string error;
  for (const std::string& face : {WebpOf(4096, 4096), LossyWebpOf(512, 4096),
                                  LosslessWebpOf(4096, 512)}) {
    WritePack(source_, "sky_", ".tex", face);
    EXPECT_FALSE(InstallSky(source_, overlay_, &error));
    EXPECT_NE(error.find("4096"), std::string::npos) << error;
  }
}

TEST_F(SkyPackTest, AJpegWithoutItsSizeIsRefused) {
  WritePack(source_, "sky_", ".tex",
            std::string("\xff\xd8\xff\xe0\x00\x10", 6) + "JFIF");
  std::string error;
  EXPECT_FALSE(InstallSky(source_, overlay_, &error));
}

TEST_F(SkyPackTest, APngWithoutItsHeaderIsRefused) {
  WritePack(source_, "sky_", ".png",
            std::string("\x89PNG\r\n\x1a\n", 8) + "pixels");
  std::string error;
  EXPECT_FALSE(InstallSky(source_, overlay_, &error));
}

TEST_F(SkyPackTest, AFaceTooLargeToExtractSaysSo) {
  Entries entries = PackEntries("Big/", kPng);
  entries[0].second.resize((std::size_t{64} << 20) + 1);
  Zip(root_ / "Big.zip", entries);
  const SkyImport imported = ImportSkies(root_ / "Big.zip", root_ / "skies");
  ASSERT_FALSE(imported.skipped.empty());
  EXPECT_NE(imported.skipped.front().find("too large"), std::string::npos)
      << imported.skipped.front();
}

TEST_F(SkyPackTest, RemovesALibrarySkyButNotTheOneInUse) {
  const std::filesystem::path library = root_ / "skies";
  WritePack(root_ / "Day", "sky_", ".png", kPng);
  WritePack(root_ / "Dusk", "sky_", ".png", kPng + "dusk");
  ASSERT_TRUE(ImportSkies(root_ / "Day", library).error.empty());
  ASSERT_TRUE(ImportSkies(root_ / "Dusk", library).error.empty());
  std::string error;
  ASSERT_TRUE(UseLibrarySky("Dusk", library, overlay_, &error)) << error;
  EXPECT_FALSE(RemoveLibrarySky("Dusk", library, overlay_, &error));
  EXPECT_NE(error.find("in use"), std::string::npos) << error;
  ASSERT_TRUE(RemoveLibrarySky("Day", library, overlay_, &error)) << error;
  EXPECT_EQ(ListSkyLibrary(library), std::vector<std::string>{"Dusk"});
  EXPECT_FALSE(RemoveLibrarySky("../skies", library, overlay_, &error));
}

TEST_F(SkyPackTest, APacksPreviewIsItsFrontFace) {
  const std::filesystem::path library = root_ / "skies";
  WritePack(root_ / "Day", "sky_", ".png", kPng);
  ASSERT_TRUE(ImportSkies(root_ / "Day", library).error.empty());
  EXPECT_EQ(Read(SkyPreviewFace(library, "Day")), kPng + "ft");
  EXPECT_TRUE(SkyPreviewFace(library, "Gone").empty());
}

TEST_F(SkyPackTest, LibraryNamesCannotLeaveTheLibrary) {
  const std::filesystem::path library = root_ / "skies";
  std::string error;
  EXPECT_FALSE(UseLibrarySky("../escape", library, overlay_, &error));
  EXPECT_FALSE(UseLibrarySky("", library, overlay_, &error));
}

}  // namespace
}  // namespace ui
}  // namespace mocktail
