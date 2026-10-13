#include "runtime/asset_overlay_names.h"

#include <algorithm>
#include <cstring>
#include <system_error>

namespace mocktail {
namespace runtime {

std::vector<std::string> OverlayRbxAssetNames(
    const std::filesystem::path& overlay_dir) {
  std::vector<std::string> names;
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

}  // namespace runtime
}  // namespace mocktail
