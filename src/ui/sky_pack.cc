#include "ui/sky_pack.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "update/zip_archive.h"

namespace mocktail {
namespace ui {
namespace {

constexpr std::array<std::string_view, 6> kFaces = {"bk", "dn", "ft",
                                                    "lf", "rt", "up"};
// Packs nest faces as deep as <pack>/PlatformContent/pc/textures/sky/.
constexpr int kSearchDepth = 6;
// A 2048 px RGBA face is 16 MiB.
constexpr std::uintmax_t kMaxFaceBytes = std::uintmax_t{64} << 20;
// Six 2048 px faces take 96 MiB of video memory, which a 2 GB GTX 950 can
// spare; a 65-sky collection tops out at 2048.
constexpr std::uint32_t kMaxFacePixels = 2048;
// The largest pack in a 65-sky collection is 14 MB.
constexpr std::size_t kMaxPackZipBytes = std::size_t{256} << 20;
// A collection zip of pack zips is two levels.
constexpr int kMaxZipLevel = 2;
constexpr char kInstalledMarker[] = ".installed";
// In a pack copied from the overlay, which an import must never replace.
constexpr char kKeptMarker[] = ".kept";

using Path = std::filesystem::path;

// Six faces that share a folder and a name, such as Aurora_bk … Aurora_up.
struct FaceSet {
  std::array<std::vector<Path>, kFaces.size()> faces;

  bool complete() const {
    return std::all_of(faces.begin(), faces.end(),
                       [](const auto& face) { return face.size() == 1; });
  }
  std::size_t found() const {
    return static_cast<std::size_t>(
        std::count_if(faces.begin(), faces.end(),
                      [](const auto& face) { return !face.empty(); }));
  }
};

// Keyed by folder and the name before the face.
using Sets = std::map<std::pair<Path, std::string>, FaceSet>;

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

// Index into kFaces and the name before the face, or kFaces.size().
std::pair<std::size_t, std::string> FaceOf(const Path& path) {
  const std::string extension = Lower(path.extension().string());
  if (extension != ".tex" && extension != ".png" && extension != ".ktx" &&
      extension != ".jpg" && extension != ".jpeg" && extension != ".dds" &&
      extension != ".webp") {
    return {kFaces.size(), {}};
  }
  const std::string stem = Lower(path.stem().string());
  // Hidden files, such as macOS's ._ AppleDouble files, are not faces.
  if (stem.empty() || stem.front() == '.') {
    return {kFaces.size(), {}};
  }
  for (std::size_t face = 0; face < kFaces.size(); ++face) {
    const std::string suffix = "_" + std::string(kFaces[face]);
    if (stem.size() > suffix.size() &&
        stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) ==
            0) {
      return {face, stem.substr(0, stem.size() - suffix.size())};
    }
  }
  return {kFaces.size(), {}};
}

std::uint32_t ReadU32(std::string_view bytes, std::size_t at,
                      bool big_endian) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    const auto byte = static_cast<unsigned char>(
        bytes[at + (big_endian ? index : 3 - index)]);
    value = (value << 8) | byte;
  }
  return value;
}

// Reads the size from a JPEG's frame header, which follows any APPn data.
bool JpegSize(std::ifstream& in, std::uint32_t* width,
              std::uint32_t* height) {
  in.clear();
  in.seekg(2);
  for (int segment = 0; segment < 256; ++segment) {
    std::array<unsigned char, 4> marker{};
    if (!in.read(reinterpret_cast<char*>(marker.data()), marker.size()) ||
        marker[0] != 0xff) {
      return false;
    }
    const int type = marker[1];
    const std::streamoff length = (marker[2] << 8) | marker[3];
    if (length < 2 || type == 0xda || type == 0xd9) return false;
    // SOF0 to SOF15; C4, C8 and CC are other segments.
    if (type >= 0xc0 && type <= 0xcf && type != 0xc4 && type != 0xc8 &&
        type != 0xcc) {
      std::array<unsigned char, 5> frame{};
      if (!in.read(reinterpret_cast<char*>(frame.data()), frame.size())) {
        return false;
      }
      *height = (frame[1] << 8) | frame[2];
      *width = (frame[3] << 8) | frame[4];
      return true;
    }
    in.seekg(length - 2, std::ios::cur);
  }
  return false;
}

// Reads the size from the first chunk of a WebP: extended, lossy or lossless.
bool WebpSize(std::string_view bytes, std::uint32_t* width,
              std::uint32_t* height) {
  if (bytes.size() < 25) return false;
  const auto at = [&](std::size_t index) -> std::uint32_t {
    return static_cast<unsigned char>(bytes[index]);
  };
  const std::string_view chunk = bytes.substr(12, 4);
  if (chunk == "VP8X" && bytes.size() >= 30) {
    *width = 1 + (at(24) | at(25) << 8 | at(26) << 16);
    *height = 1 + (at(27) | at(28) << 8 | at(29) << 16);
  } else if (chunk == "VP8 " && bytes.size() >= 30 &&
             bytes.substr(23, 3) == "\x9d\x01\x2a") {
    *width = (at(26) | at(27) << 8) & 0x3fff;
    *height = (at(28) | at(29) << 8) & 0x3fff;
  } else if (chunk == "VP8L" && at(20) == 0x2f) {
    const std::uint32_t bits = ReadU32(bytes, 21, false);
    *width = 1 + (bits & 0x3fff);
    *height = 1 + ((bits >> 14) & 0x3fff);
  } else {
    return false;
  }
  return true;
}

// Empty for a PNG, JPEG, WebP, DDS or KTX 1 face (the containers Roblox reads
// a .tex sky from) of at most kMaxFacePixels a side; otherwise why not.
std::string CheckFace(const Path& path) {
  const std::string name = path.filename().string();
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) return "Cannot read " + name + ".";
  if (size > kMaxFaceBytes) return name + " is over 64 MiB.";
  std::array<char, 44> head{};
  std::ifstream in(path, std::ios::binary);
  in.read(head.data(), head.size());
  const std::string_view bytes(head.data(),
                               static_cast<std::size_t>(in.gcount()));
  constexpr std::string_view kPng("\x89PNG\r\n\x1a\n", 8);
  constexpr std::string_view kKtx1("\xabKTX 11\xbb\r\n\x1a\n", 12);
  const std::string unknown =
      name + " is not a PNG, JPEG, WebP, DDS or KTX image.";
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  if (bytes.size() >= 24 && bytes.substr(0, 8) == kPng &&
      bytes.substr(12, 4) == "IHDR") {
    width = ReadU32(bytes, 16, true);
    height = ReadU32(bytes, 20, true);
  } else if (bytes.size() >= 44 && bytes.substr(0, 12) == kKtx1) {
    const bool big_endian = ReadU32(bytes, 12, true) == 0x04030201;
    if (!big_endian && ReadU32(bytes, 12, false) != 0x04030201) {
      return unknown;
    }
    width = ReadU32(bytes, 36, big_endian);
    height = ReadU32(bytes, 40, big_endian);
  } else if (bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" &&
             bytes.substr(8, 4) == "WEBP") {
    if (!WebpSize(bytes, &width, &height)) return name + " is a damaged WebP.";
  } else if (bytes.size() >= 20 && bytes.substr(0, 4) == "DDS ") {
    height = ReadU32(bytes, 12, false);
    width = ReadU32(bytes, 16, false);
  } else if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xff\xd8\xff") {
    if (!JpegSize(in, &width, &height)) return name + " is a damaged JPEG.";
  } else {
    return unknown;
  }
  if (width == 0 || height == 0) return name + " has no pixels.";
  if (width > kMaxFacePixels || height > kMaxFacePixels) {
    return name + " is " + std::to_string(width) + "×" +
           std::to_string(height) + " px; a sky face can be at most " +
           std::to_string(kMaxFacePixels) + " px.";
  }
  return {};
}

// Empty when all six faces are usable.
std::string BadImage(const FaceSet& set) {
  for (const auto& face : set.faces) {
    if (std::string why = CheckFace(face.front()); !why.empty()) return why;
  }
  return {};
}

bool SameBytes(const Path& a, const Path& b) {
  std::error_code a_error;
  std::error_code b_error;
  if (std::filesystem::file_size(a, a_error) !=
          std::filesystem::file_size(b, b_error) ||
      a_error || b_error) {
    return false;
  }
  std::ifstream a_in(a, std::ios::binary);
  std::ifstream b_in(b, std::ios::binary);
  std::array<char, 1 << 16> a_chunk;
  std::array<char, 1 << 16> b_chunk;
  while (a_in && b_in) {
    a_in.read(a_chunk.data(), a_chunk.size());
    b_in.read(b_chunk.data(), b_chunk.size());
    if (a_in.gcount() != b_in.gcount() ||
        !std::equal(a_chunk.begin(), a_chunk.begin() + a_in.gcount(),
                    b_chunk.begin())) {
      return false;
    }
  }
  return a_in.eof() && b_in.eof();
}

std::string MissingFaces(const FaceSet& set) {
  std::string missing;
  for (std::size_t face = 0; face < kFaces.size(); ++face) {
    if (set.faces[face].size() != 1) {
      missing += (missing.empty() ? "" : ", ") + std::string(kFaces[face]);
    }
  }
  return missing;
}

Path FacePath(const Path& overlay_dir, std::string_view face) {
  return SkyFolder(overlay_dir) / ("sky512_" + std::string(face) + ".tex");
}

// Reflections come from indoor512.
Path IndoorPath(const Path& overlay_dir, std::string_view face) {
  return SkyFolder(overlay_dir) / ("indoor512_" + std::string(face) + ".tex");
}

struct SkyFile {
  Path path;
  std::size_t face;
};

// The files the installed sky owns: its sky512 faces, and the indoor512 faces
// when each is missing or matches it. If any was put there by hand, all six
// stay, so the indoor sky never mixes two skies.
std::vector<SkyFile> SkyFiles(const Path& overlay_dir) {
  std::vector<SkyFile> files;
  for (std::size_t face = 0; face < kFaces.size(); ++face) {
    files.push_back({FacePath(overlay_dir, kFaces[face]), face});
  }
  for (std::size_t face = 0; face < kFaces.size(); ++face) {
    const Path indoor = IndoorPath(overlay_dir, kFaces[face]);
    std::error_code error;
    const bool exists = std::filesystem::exists(indoor, error);
    if (error ||
        (exists && !SameBytes(indoor, FacePath(overlay_dir, kFaces[face])))) {
      files.resize(kFaces.size());
      return files;
    }
    files.push_back({indoor, face});
  }
  return files;
}

Path WithSuffix(Path path, const char* suffix) {
  path += suffix;
  return path;
}

std::string SetName(const Path& folder, const std::string& prefix,
                    const Path& source) {
  const Path relative = folder.lexically_relative(source);
  return (relative.empty() || relative == "." ? Path(prefix + "_*")
                                              : relative / (prefix + "_*"))
      .string();
}

void RemoveQuietly(const Path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

// What an import may still do; InstallSky searches without limits.
struct ScanLimits {
  std::size_t files_left = 0;
  const std::atomic<bool>* stop = nullptr;
  bool cancelled = false;
  bool too_many_files = false;

  // False once the import is cancelled or has looked at too many files.
  bool Next() {
    if (stop != nullptr && stop->load()) cancelled = true;
    if (files_left == 0) too_many_files = true;
    if (cancelled || too_many_files) return false;
    --files_left;
    return true;
  }
};

bool FindFaceSets(const Path& source, Sets* sets, std::string* error,
                  ScanLimits* limits = nullptr) {
  std::error_code fs_error;
  std::filesystem::recursive_directory_iterator it(
      source, std::filesystem::directory_options::skip_permission_denied,
      fs_error);
  for (; !fs_error && it != std::filesystem::recursive_directory_iterator();
       it.increment(fs_error)) {
    if (limits != nullptr && !limits->Next()) {
      sets->clear();
      *error = limits->cancelled ? "cancelled"
                                 : "too many files to search; choose the "
                                   "folder that holds the sky packs";
      return false;
    }
    std::error_code entry_error;
    if (it->is_directory(entry_error) && it.depth() >= kSearchDepth) {
      it.disable_recursion_pending();
    }
    if (!it->is_regular_file(entry_error)) {
      continue;
    }
    const auto [face, prefix] = FaceOf(it->path());
    if (face < kFaces.size()) {
      (*sets)[{it->path().parent_path(), prefix}].faces[face].push_back(
          it->path());
    }
  }
  if (fs_error) {
    *error = "Cannot read " + source.string() + ": " + fs_error.message();
    return false;
  }
  return true;
}

// A swap killed partway leaves <face>.old (the whole old sky, moved aside
// before any new face goes in) and <face>.new. Puts the old sky back.
void FinishInterruptedSwap(const Path& overlay_dir) {
  for (const std::string_view face : kFaces) {
    for (const Path& installed :
         {FacePath(overlay_dir, face), IndoorPath(overlay_dir, face)}) {
      RemoveQuietly(WithSuffix(installed, ".new"));
      std::error_code error;
      if (std::filesystem::exists(WithSuffix(installed, ".old"), error)) {
        std::filesystem::rename(WithSuffix(installed, ".old"), installed,
                                error);
      }
    }
  }
}

// Moves the installed files aside as <file>.old. On failure, puts back the
// ones already moved.
bool MoveFacesAside(const std::vector<SkyFile>& files,
                    std::vector<Path>* moved, std::error_code* error) {
  for (const SkyFile& file : files) {
    const Path& installed = file.path;
    if (!std::filesystem::exists(installed, *error)) {
      if (*error) break;
      continue;
    }
    std::filesystem::rename(installed, WithSuffix(installed, ".old"), *error);
    if (*error) break;
    moved->push_back(installed);
  }
  if (!*error) return true;
  for (const Path& installed : *moved) {
    std::error_code ignored;
    std::filesystem::rename(WithSuffix(installed, ".old"), installed, ignored);
  }
  moved->clear();
  return false;
}

bool ValidLibraryName(std::string_view name) {
  return !name.empty() && name.front() != '.' &&
         name.find('/') == std::string_view::npos;
}

std::string LibraryName(std::string name) {
  for (char& c : name) {
    if (c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20) c = '-';
  }
  const auto first = name.find_first_not_of(" .");
  name = first == std::string::npos ? std::string() : name.substr(first);
  name.erase(name.find_last_not_of(' ') + 1);
  if (name.size() > 80) name.resize(80);
  return name.empty() ? "Sky" : name;
}

// Folders that say where a sky goes, not which sky it is.
bool IsLayoutFolder(const Path& folder) {
  const std::string name = Lower(folder.string());
  return name == "platformcontent" || name == "content" || name == "pc" ||
         name == "android" || name == "textures" || name == "sky";
}

std::string PackName(const Path& root, const Path& set_folder,
                     const std::string& hint) {
  const Path relative = set_folder.lexically_relative(root);
  if (relative.empty() || relative == "." || IsLayoutFolder(*relative.begin())) {
    return hint;
  }
  return LibraryName(relative.begin()->string());
}

Path LibraryFace(const Path& pack, std::string_view face) {
  for (const char* extension :
       {".tex", ".png", ".ktx", ".jpg", ".jpeg", ".dds", ".webp"}) {
    const Path path = pack / ("sky512_" + std::string(face) + extension);
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) return path;
  }
  return {};
}

bool AddToLibrary(const FaceSet& set, const Path& library,
                  const std::string& name, std::string* error) {
  if (*error = BadImage(set); !error->empty()) return false;
  const Path staging = library / ("." + name + ".new");
  std::error_code fs_error;
  std::filesystem::remove_all(staging, fs_error);
  std::filesystem::create_directories(staging, fs_error);
  for (std::size_t face = 0; face < kFaces.size() && !fs_error; ++face) {
    const Path& from = set.faces[face].front();
    std::filesystem::copy_file(
        from,
        staging / ("sky512_" + std::string(kFaces[face]) +
                   Lower(from.extension().string())),
        fs_error);
  }
  const Path pack = library / name;
  if (!fs_error) std::filesystem::remove_all(pack, fs_error);
  if (!fs_error) std::filesystem::rename(staging, pack, fs_error);
  if (fs_error) {
    std::error_code ignored;
    std::filesystem::remove_all(staging, ignored);
    *error = fs_error.message();
    return false;
  }
  return true;
}

std::uintmax_t SetBytes(const FaceSet& set) {
  std::uintmax_t bytes = 0;
  for (const auto& face : set.faces) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(face.front(), error);
    if (!error) bytes += size;
  }
  return bytes;
}

bool IsKept(const Path& pack) {
  std::error_code error;
  return std::filesystem::exists(pack / kKeptMarker, error);
}

class Importer {
 public:
  Importer(const Path& library, SkyImport* result, std::uintmax_t max_bytes,
           ScanLimits* scan)
      : library_(library),
        result_(result),
        bytes_left_(max_bytes),
        scan_(scan) {}

  void Folder(const Path& folder, const std::string& hint, int zip_level) {
    if (Stopped()) return;
    Sets sets;
    std::string error;
    if (!FindFaceSets(folder, &sets, &error, scan_)) {
      if (!scan_->cancelled) result_->skipped.push_back(hint + ": " + error);
      return;
    }
    // Beside a sky512 set, indoor512 is its reflection map.
    std::set<Path> has_sky512;
    for (const auto& [key, set] : sets) {
      if (key.second == "sky512" && set.complete()) has_sky512.insert(key.first);
    }
    std::vector<const Sets::value_type*> packs;
    std::map<Path, int> packs_in_folder;
    for (const auto& entry : sets) {
      const auto& [key, set] = entry;
      if (key.second == "indoor512" && has_sky512.count(key.first) != 0) {
        continue;
      }
      if (set.complete()) {
        packs.push_back(&entry);
        ++packs_in_folder[key.first];
      } else if (set.found() >= 3) {
        result_->skipped.push_back(PackName(folder, key.first, hint) +
                                   ": check " + MissingFaces(set));
      }
    }
    for (const auto* entry : packs) {
      const auto& [key, set] = *entry;
      // Pack zips often keep a folder named after another pack.
      std::string base =
          packs.size() == 1 ? hint : PackName(folder, key.first, hint);
      if (packs.size() > 1 && packs_in_folder[key.first] > 1 &&
          key.second != "sky512") {
        // The prefix as written, from Aurora_bk.png.
        const std::string stem = set.faces[0].front().stem().string();
        base = LibraryName(stem.substr(0, stem.size() - 3));
      }
      if (Stopped()) return;
      std::string name = base;
      for (int copy = 2; IsKept(library_ / name) || !names_.insert(name).second;
           ++copy) {
        name = base + " " + std::to_string(copy);
      }
      if (!Spend(SetBytes(set))) return;
      if (AddToLibrary(set, library_, name, &error)) {
        result_->added.push_back(name);
      } else {
        result_->skipped.push_back(name + ": " + error);
      }
    }
    if (zip_level >= kMaxZipLevel) return;
    std::error_code fs_error;
    std::filesystem::recursive_directory_iterator it(
        folder, std::filesystem::directory_options::skip_permission_denied,
        fs_error);
    for (; !fs_error && it != std::filesystem::recursive_directory_iterator();
         it.increment(fs_error)) {
      if (!scan_->Next()) {
        if (scan_->too_many_files) {
          result_->skipped.push_back(
              hint + ": too many files to search; choose the folder that "
                     "holds the sky packs");
        }
        return;
      }
      std::error_code entry_error;
      if (it->is_directory(entry_error) && it.depth() >= kSearchDepth) {
        it.disable_recursion_pending();
      }
      if (it->is_regular_file(entry_error) &&
          Lower(it->path().extension().string()) == ".zip") {
        Zip(it->path(), zip_level + 1);
        if (Stopped()) return;
      }
    }
  }

  void Zip(const Path& zip, int zip_level) {
    const std::string hint = LibraryName(zip.stem().string());
    const update::ZipListResult listed = update::ListZipEntries(zip);
    if (!listed) {
      result_->skipped.push_back(hint + ": " + listed.error);
      return;
    }
    static std::atomic<unsigned> counter{0};
    const Path temporary =
        library_ / (".import-" + std::to_string(getpid()) + "-" +
                    std::to_string(counter.fetch_add(1)));
    for (const update::ZipEntry& entry : listed.entries) {
      if (Stopped()) break;
      if (entry.directory) continue;
      const Path name(entry.name);
      std::uintmax_t max_bytes = 0;
      if (FaceOf(name).first < kFaces.size()) {
        max_bytes = kMaxFaceBytes;
      } else if (zip_level < kMaxZipLevel &&
                 Lower(name.extension().string()) == ".zip") {
        max_bytes = kMaxPackZipBytes;
      }
      if (max_bytes == 0) continue;
      std::string error;
      if (entry.uncompressed_size > max_bytes) {
        error = "too large (" + std::to_string(entry.uncompressed_size >> 20) +
                " MiB)";
      } else if (!Spend(entry.uncompressed_size)) {
        break;
      } else {
        update::ExtractZipEntry(zip, entry.name, temporary / name, max_bytes,
                                &error);
      }
      if (!error.empty()) {
        result_->skipped.push_back(hint + ": " + entry.name + " " + error);
      }
    }
    Folder(temporary, hint, zip_level);
    std::error_code ignored;
    std::filesystem::remove_all(temporary, ignored);
  }

 private:
  bool Stopped() {
    if (scan_->stop != nullptr && scan_->stop->load()) scan_->cancelled = true;
    return scan_->cancelled || scan_->too_many_files || stopped_;
  }

  // Counts what an import writes, extracted or copied, against its limit.
  bool Spend(std::uintmax_t bytes) {
    if (bytes <= bytes_left_) {
      bytes_left_ -= bytes;
      return true;
    }
    if (!stopped_) {
      stopped_ = true;
      result_->skipped.push_back("Stopped at the import size limit");
    }
    return false;
  }

  const Path& library_;
  SkyImport* result_;
  std::uintmax_t bytes_left_;
  ScanLimits* scan_;
  bool stopped_ = false;
  std::set<std::string> names_;
};

}  // namespace

Path SkyFolder(const Path& overlay_dir) {
  return overlay_dir / "content" / "android" / "textures" / "sky";
}

bool HasCustomSky(const Path& overlay_dir) {
  std::error_code error;
  return std::any_of(kFaces.begin(), kFaces.end(), [&](std::string_view face) {
    return std::filesystem::exists(FacePath(overlay_dir, face), error);
  });
}

bool IndoorSkyMissing(const Path& overlay_dir) {
  if (!HasCustomSky(overlay_dir)) return false;
  const std::vector<SkyFile> files = SkyFiles(overlay_dir);
  return std::any_of(files.begin() + kFaces.size(), files.end(),
                     [](const SkyFile& file) {
                       std::error_code error;
                       return !std::filesystem::exists(file.path, error);
                     });
}

bool InstallSky(const Path& source, const Path& overlay_dir,
                std::string* error) {
  std::error_code fs_error;
  if (!std::filesystem::is_directory(source, fs_error)) {
    *error = source.string() + " is not a folder.";
    return false;
  }
  Sets sets;
  if (!FindFaceSets(source, &sets, error)) return false;

  std::vector<const Sets::value_type*> complete;
  const Sets::value_type* closest = nullptr;
  for (const auto& entry : sets) {
    if (entry.second.complete()) complete.push_back(&entry);
    if (closest == nullptr || entry.second.found() > closest->second.found()) {
      closest = &entry;
    }
  }
  if (complete.size() > 1) {
    std::vector<const Sets::value_type*> sky512;
    for (const auto* entry : complete) {
      if (entry->first.second == "sky512") sky512.push_back(entry);
    }
    if (!sky512.empty()) complete = std::move(sky512);
  }
  if (complete.empty()) {
    if (closest == nullptr) {
      *error = "No sky faces (*_bk, *_dn, *_ft, *_lf, *_rt, *_up) in " +
               source.string() + ".";
      return false;
    }
    *error = SetName(closest->first.first, closest->first.second, source) +
             " needs exactly one of each face; check " +
             MissingFaces(closest->second) + ".";
    return false;
  }
  if (complete.size() > 1) {
    std::string names;
    for (const auto* entry : complete) {
      names += (names.empty() ? "" : ", ") +
               SetName(entry->first.first, entry->first.second, source);
    }
    *error = "More than one sky in " + source.string() + ": " + names +
             ". Choose the folder of one.";
    return false;
  }
  const FaceSet& chosen = complete.front()->second;
  if (*error = BadImage(chosen); !error->empty()) return false;

  FinishInterruptedSwap(overlay_dir);
  const std::vector<SkyFile> files = SkyFiles(overlay_dir);
  const Path folder = SkyFolder(overlay_dir);
  std::filesystem::create_directories(folder, fs_error);
  if (fs_error) {
    *error = "Cannot create " + folder.string() + ": " + fs_error.message();
    return false;
  }
  std::vector<Path> copies;
  const auto discard = [&copies] {
    for (const Path& copy : copies) RemoveQuietly(copy);
  };
  for (const SkyFile& file : files) {
    const Path& source = chosen.faces[file.face].front();
    copies.push_back(WithSuffix(file.path, ".new"));
    if (!std::filesystem::copy_file(
            source, copies.back(),
            std::filesystem::copy_options::overwrite_existing, fs_error)) {
      discard();
      *error = "Cannot copy " + source.filename().string() + ": " +
               fs_error.message();
      return false;
    }
  }
  // The old files stay as .old until all the new ones are in, so a failure
  // puts the old sky back whole.
  std::vector<Path> moved;
  if (!MoveFacesAside(files, &moved, &fs_error)) {
    discard();
    *error = "Cannot install the sky: " + fs_error.message();
    return false;
  }
  for (std::size_t index = 0; index < files.size(); ++index) {
    std::filesystem::rename(copies[index], files[index].path, fs_error);
    if (fs_error) {
      for (std::size_t done = 0; done < index; ++done) {
        RemoveQuietly(files[done].path);
      }
      for (const Path& installed : moved) {
        std::error_code ignored;
        std::filesystem::rename(WithSuffix(installed, ".old"), installed,
                                ignored);
      }
      discard();
      *error = "Cannot install the sky: " + fs_error.message();
      return false;
    }
  }
  for (const Path& installed : moved) {
    RemoveQuietly(WithSuffix(installed, ".old"));
  }
  return true;
}

bool RemoveCustomSky(const Path& overlay_dir, std::string* error) {
  FinishInterruptedSwap(overlay_dir);
  std::vector<Path> moved;
  std::error_code fs_error;
  if (!MoveFacesAside(SkyFiles(overlay_dir), &moved, &fs_error)) {
    *error = "Cannot remove the custom sky: " + fs_error.message();
    return false;
  }
  for (const Path& installed : moved) {
    RemoveQuietly(WithSuffix(installed, ".old"));
  }
  return true;
}

std::vector<std::string> ListSkyLibrary(const Path& library) {
  std::vector<std::string> names;
  std::error_code error;
  for (std::filesystem::directory_iterator it(library, error), end;
       !error && it != end; it.increment(error)) {
    const std::string name = it->path().filename().string();
    std::error_code entry_error;
    if (!ValidLibraryName(name) || !it->is_directory(entry_error)) continue;
    if (std::all_of(kFaces.begin(), kFaces.end(), [&](std::string_view face) {
          return !LibraryFace(it->path(), face).empty();
        })) {
      names.push_back(name);
    }
  }
  std::sort(names.begin(), names.end(),
            [](const std::string& a, const std::string& b) {
              return Lower(a) < Lower(b);
            });
  return names;
}

// What a killed import or keep leaves: .import-* and .<name>.new folders.
void RemoveLeftovers(const Path& library) {
  std::error_code error;
  for (std::filesystem::directory_iterator it(library, error), end;
       !error && it != end; it.increment(error)) {
    const std::string name = it->path().filename().string();
    const bool staging = name.size() > 5 && name.front() == '.' &&
                         name.compare(name.size() - 4, 4, ".new") == 0;
    std::error_code entry_error;
    if ((name.rfind(".import-", 0) == 0 || staging) &&
        it->is_directory(entry_error)) {
      std::error_code ignored;
      std::filesystem::remove_all(it->path(), ignored);
    }
  }
}

SkyImport ImportSkies(const Path& source, const Path& library,
                      const SkyImportLimits& limits,
                      const std::atomic<bool>* stop) {
  SkyImport result;
  Path clean = source.lexically_normal();
  if (clean.filename().empty()) clean = clean.parent_path();
  std::error_code fs_error;
  std::filesystem::create_directories(library, fs_error);
  if (fs_error) {
    result.error = "Cannot create " + library.string() + ": " +
                   fs_error.message();
    return result;
  }
  RemoveLeftovers(library);
  ScanLimits scan;
  scan.files_left = limits.max_files;
  scan.stop = stop;
  Importer importer(library, &result, limits.max_bytes, &scan);
  if (std::filesystem::is_directory(clean, fs_error)) {
    importer.Folder(clean, LibraryName(clean.filename().string()), 0);
  } else if (std::filesystem::is_regular_file(clean, fs_error) &&
             Lower(clean.extension().string()) == ".zip") {
    importer.Zip(clean, 1);
  } else {
    result.error = "Choose a folder or a .zip file.";
    return result;
  }
  result.cancelled = scan.cancelled;
  if (result.added.empty() && !result.cancelled) {
    result.error = result.skipped.empty()
                       ? "No sky packs in " + clean.filename().string() + "."
                       : "No sky in " + clean.filename().string() +
                             " could be imported.";
  }
  return result;
}

// Copies a sky not from the library into it, as "Custom sky", so replacing
// it loses nothing.
static bool KeepHandMadeSky(const Path& library, const Path& overlay_dir,
                            std::string* kept, std::string* error) {
  if (InstalledSky(overlay_dir, library)) return true;
  std::string name = "Custom sky";
  std::error_code fs_error;
  for (int copy = 2; std::filesystem::exists(library / name, fs_error);
       ++copy) {
    name = "Custom sky " + std::to_string(copy);
  }
  const Path staging = library / ("." + name + ".new");
  std::filesystem::remove_all(staging, fs_error);
  std::filesystem::create_directories(staging, fs_error);
  for (std::size_t face = 0; face < kFaces.size() && !fs_error; ++face) {
    const Path installed = FacePath(overlay_dir, kFaces[face]);
    std::error_code missing;
    if (!std::filesystem::exists(installed, missing)) continue;
    std::filesystem::copy_file(installed, staging / installed.filename(),
                               fs_error);
  }
  if (!fs_error) {
    std::ofstream(staging / kKeptMarker, std::ios::trunc);
    std::filesystem::rename(staging, library / name, fs_error);
  }
  if (fs_error) {
    std::error_code ignored;
    std::filesystem::remove_all(staging, ignored);
    *error = "Cannot keep the current sky in the library: " +
             fs_error.message();
    return false;
  }
  if (kept != nullptr) *kept = name;
  return true;
}

bool UseRobloxSky(const Path& library, const Path& overlay_dir,
                  std::string* error, std::string* kept) {
  if (!KeepHandMadeSky(library, overlay_dir, kept, error)) return false;
  return RemoveCustomSky(overlay_dir, error);
}

bool UseLibrarySky(std::string_view name, const Path& library,
                   const Path& overlay_dir, std::string* error,
                   std::string* kept) {
  const Path pack = library / std::string(name);
  std::error_code fs_error;
  if (!ValidLibraryName(name) || !std::filesystem::is_directory(pack, fs_error)) {
    *error = "There is no sky named " + std::string(name) + ".";
    return false;
  }
  if (!KeepHandMadeSky(library, overlay_dir, kept, error)) return false;
  if (!InstallSky(pack, overlay_dir, error)) return false;
  std::ofstream(library / kInstalledMarker, std::ios::trunc) << name;
  return true;
}

bool RemoveLibrarySky(std::string_view name, const Path& library,
                      const Path& overlay_dir, std::string* error) {
  const Path pack = library / std::string(name);
  std::error_code fs_error;
  if (!ValidLibraryName(name) ||
      !std::filesystem::is_directory(pack, fs_error)) {
    *error = "There is no sky named " + std::string(name) + ".";
    return false;
  }
  if (InstalledSky(overlay_dir, library) == name) {
    *error = std::string(name) + " is in use. Pick another sky first.";
    return false;
  }
  std::filesystem::remove_all(pack, fs_error);
  if (fs_error) {
    *error = "Cannot remove " + std::string(name) + ": " + fs_error.message();
    return false;
  }
  return true;
}

Path SkyPreviewFace(const Path& library, std::string_view name) {
  if (!ValidLibraryName(name)) return {};
  return LibraryFace(library / std::string(name), "ft");
}

std::optional<std::string> InstalledSky(const Path& overlay_dir,
                                        const Path& library) {
  if (!HasCustomSky(overlay_dir)) return std::string();
  const auto matches = [&](const std::string& name) {
    return std::all_of(kFaces.begin(), kFaces.end(), [&](std::string_view face) {
      const Path packed = LibraryFace(library / name, face);
      return !packed.empty() && SameBytes(packed, FacePath(overlay_dir, face));
    });
  };
  std::ifstream marker(library / kInstalledMarker);
  std::string marked;
  std::getline(marker, marked);
  if (ValidLibraryName(marked) && matches(marked)) return marked;
  // A sky put in the overlay by hand, or picked before the marker existed,
  // is found by its bytes; sizes rule out most packs without a read.
  for (const std::string& name : ListSkyLibrary(library)) {
    if (name != marked && matches(name)) return name;
  }
  return std::nullopt;
}

}  // namespace ui
}  // namespace mocktail
