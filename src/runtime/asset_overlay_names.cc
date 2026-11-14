#include "runtime/asset_overlay_names.h"

#include <algorithm>
#include <cstring>
#include <system_error>

namespace mocktail {
namespace runtime {

std::vector<std::string> OverlayRbxAssetNames(
    const std::filesystem::path& overlay_dir) {
  std::vector<std::string> names;
  if (overlay_dir.empty()) {
    return names;
  }
  const std::filesystem::path content = overlay_dir / "content";
  std::error_code error;
  std::filesystem::recursive_directory_iterator it(
      content, std::filesystem::directory_options::skip_permission_denied,
      error);
  const std::filesystem::recursive_directory_iterator end;
  for (; !error && it != end; it.increment(error)) {
    std::error_code entry_error;
    if (!it->is_regular_file(entry_error)) {
      continue;
    }
    const std::filesystem::path relative =
        it->path().lexically_relative(content);
    names.push_back("rbxasset://" + relative.generic_string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string> WithoutShippedAssets(
    std::vector<std::string> names, const std::filesystem::path& asset_root) {
  if (asset_root.empty()) {
    return names;
  }
  constexpr std::string_view kScheme = "rbxasset://";
  const auto shipped = [&](const std::string& name) {
    if (name.compare(0, kScheme.size(), kScheme) != 0) {
      return false;
    }
    const std::filesystem::path relative = name.substr(kScheme.size());
    for (const std::filesystem::path& candidate :
         {asset_root / relative, asset_root / "content" / relative,
          asset_root / "ExtraContent" / relative}) {
      std::error_code error;
      if (std::filesystem::is_regular_file(candidate, error)) {
        return true;
      }
    }
    return false;
  };
  names.erase(std::remove_if(names.begin(), names.end(), shipped),
              names.end());
  return names;
}

std::size_t DisableAssetNameMappings(std::uint8_t* data, std::size_t size,
                                     const std::vector<std::string>& names) {
  std::size_t changed = 0;
  if (data == nullptr) {
    return changed;
  }
  std::uint8_t* const end = data + size;
  for (const std::string& name : names) {
    // The trailing NUL is part of the needle, so a longer string never matches.
    const std::size_t needle_size = name.size() + 1;
    if (name.empty() || needle_size > size) {
      continue;
    }
    std::uint8_t* cursor = data;
    while (cursor + needle_size <= end) {
      auto* found = static_cast<std::uint8_t*>(
          memmem(cursor, static_cast<std::size_t>(end - cursor), name.c_str(),
                 needle_size));
      if (found == nullptr) {
        break;
      }
      if (found == data || found[-1] == 0) {
        found[0] = '!';
        ++changed;
      }
      cursor = found + needle_size;
    }
  }
  return changed;
}

bool OverlayHasSkyBody(const std::filesystem::path& overlay_dir,
                       std::string_view body) {
  if (overlay_dir.empty()) return false;
  std::error_code error;
  return std::filesystem::is_regular_file(
      overlay_dir / "content" / "sky" / (std::string(body) + ".jpg"), error);
}

bool OverlayHasCustomSky(const std::filesystem::path& overlay_dir) {
  if (overlay_dir.empty()) return false;
  const std::filesystem::path sky =
      overlay_dir / "content" / "android" / "textures" / "sky";
  for (const char* prefix : {"sky512_", "indoor512_"}) {
    for (const char* face : {"bk", "dn", "ft", "lf", "rt", "up"}) {
      std::error_code error;
      if (!std::filesystem::is_regular_file(
              sky / (std::string(prefix) + face + ".tex"), error)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace runtime
}  // namespace mocktail
