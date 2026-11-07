#include "runtime/custom_font.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

bool IsFontFile(const std::filesystem::path& path) {
  const std::string extension = Lower(path.extension().string());
  return extension == ".ttf" || extension == ".otf";
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

std::vector<std::filesystem::path> SortedFiles(
    const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> files;
  std::error_code error;
  for (std::filesystem::directory_iterator it(dir, error), end;
       !error && it != end; it.increment(error)) {
    std::error_code entry_error;
    if (it->is_regular_file(entry_error)) {
      files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

}  // namespace

std::filesystem::path InstalledCustomFont(const std::filesystem::path& dir) {
  for (const std::filesystem::path& file : SortedFiles(dir)) {
    if (IsFontFile(file)) {
      return file;
    }
  }
  return {};
}

bool IsReplaceableRobloxFont(std::string_view file_name) {
  const std::string name = Lower(file_name);
  return IsFontFile(std::filesystem::path(name)) &&
         name.find("emoji") == std::string::npos && !StartsWith(name, "noto");
}

bool InstallCustomFont(const std::filesystem::path& font,
                       const std::filesystem::path& dir, std::string* error) {
  std::error_code fs_error;
  if (!IsFontFile(font) || !std::filesystem::is_regular_file(font, fs_error)) {
    *error = "not a .ttf or .otf file: " + font.filename().string();
    return false;
  }
  std::filesystem::create_directories(dir, fs_error);
  // Copied beside the old pick first, so a failed copy keeps it.
  const std::filesystem::path staged = dir / ".incoming";
  std::filesystem::copy_file(font, staged,
                             std::filesystem::copy_options::overwrite_existing,
                             fs_error);
  if (fs_error) {
    *error = "cannot copy " + font.filename().string() + ": " +
             fs_error.message();
    return false;
  }
  if (!RemoveCustomFont(dir, error)) {
    return false;
  }
  std::filesystem::rename(staged, dir / font.filename(), fs_error);
  if (fs_error) {
    *error = "cannot install " + font.filename().string() + ": " +
             fs_error.message();
    return false;
  }
  return true;
}

bool RemoveCustomFont(const std::filesystem::path& dir, std::string* error) {
  for (const std::filesystem::path& file : SortedFiles(dir)) {
    if (!IsFontFile(file)) {
      continue;
    }
    std::error_code fs_error;
    std::filesystem::remove(file, fs_error);
    if (fs_error) {
      *error = "cannot remove " + file.filename().string() + ": " +
               fs_error.message();
      return false;
    }
  }
  return true;
}

bool BuildCustomFontOverlay(const std::filesystem::path& font,
                            const std::filesystem::path& stock_fonts_dir,
                            const std::filesystem::path& out_dir,
                            std::size_t* replaced, std::string* error) {
  *replaced = 0;
  std::error_code fs_error;
  if (!std::filesystem::is_regular_file(font, fs_error) || !IsFontFile(font)) {
    *error = "no font file at " + font.string();
    return false;
  }
  const std::filesystem::path fonts = out_dir / "content" / "fonts";
  const std::filesystem::path families = fonts / "families";
  std::filesystem::remove_all(out_dir, fs_error);
  if (fs_error) {
    *error = "cannot clear " + out_dir.string() + ": " + fs_error.message();
    return false;
  }
  if (!std::filesystem::create_directories(families, fs_error)) {
    *error = "cannot create " + families.string() + ": " + fs_error.message();
    return false;
  }
  const std::string own_name = std::string(kCustomFontAssetStem) +
                               Lower(font.extension().string());
  // A copy, so a new pick in Settings leaves a running session's fonts whole.
  std::filesystem::copy_file(font, fonts / own_name, fs_error);
  if (fs_error) {
    *error = "cannot copy " + font.string() + ": " + fs_error.message();
    return false;
  }
  const auto link = [&](const std::filesystem::path& at) {
    std::filesystem::create_symlink(own_name, at, fs_error);
    if (fs_error) {
      *error = "cannot link " + at.string() + ": " + fs_error.message();
      return false;
    }
    return true;
  };
  for (const std::filesystem::path& file : SortedFiles(stock_fonts_dir)) {
    const std::string name = file.filename().string();
    if (!IsReplaceableRobloxFont(name)) {
      continue;
    }
    if (!link(fonts / name)) {
      return false;
    }
    ++*replaced;
  }
  const std::string asset_id = "rbxasset://fonts/" + own_name;
  for (const std::filesystem::path& file :
       SortedFiles(stock_fonts_dir / "families")) {
    if (Lower(file.extension().string()) != ".json" ||
        StartsWith(Lower(file.filename().string()), "noto")) {
      continue;
    }
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    nlohmann::json family = nlohmann::json::parse(text.str(), nullptr, false);
    if (family.is_discarded() || !family.is_object() ||
        !family.contains("faces") || !family["faces"].is_array()) {
      continue;
    }
    for (nlohmann::json& face : family["faces"]) {
      if (face.is_object()) {
        face["assetId"] = asset_id;
      }
    }
    std::ofstream out(families / file.filename(), std::ios::binary);
    out << family.dump(1, '\t');
    if (!out) {
      *error = "cannot write " + (families / file.filename()).string();
      return false;
    }
    ++*replaced;
  }
  return true;
}

}  // namespace runtime
}  // namespace mocktail
