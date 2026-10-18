#ifndef MOCKTAIL_SERVICES_CURSOR_PICTURE_SOURCE_H_
#define MOCKTAIL_SERVICES_CURSOR_PICTURE_SOURCE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "services/http_client.h"

namespace mocktail {
namespace services {

// The path inside Roblox's content folder that an rbxasset:// cursor names,
// such as textures/DragCursor.png. Empty for anything else.
std::string LocalCursorAssetPath(std::string_view content);

// The asset ID a downloaded cursor names: rbxassetid://123 or a roblox.com
// asset URL with ?id=123.
std::optional<std::uint64_t> CursorAssetId(std::string_view content);

// Where a local cursor picture may be, the asset overlay first, as the
// Android asset stub looks for it.
std::vector<std::filesystem::path> LocalCursorPictureCandidates(
    std::string_view relative_path, const std::filesystem::path& overlay_dir,
    const std::filesystem::path& asset_root);

// The bytes of the cursor pictures Roblox draws, for the desktop to draw
// instead. A local picture comes from the overlay or Roblox's files. A
// downloaded one comes from Roblox's asset service, signed in with the saved
// Roblox cookie, which is sent to that service only and never to the CDN it
// redirects to.
class CursorPictureSource {
 public:
  CursorPictureSource(HttpClient* http, std::filesystem::path cookie_file,
                      std::filesystem::path overlay_dir,
                      std::function<std::filesystem::path()> asset_root);

  // Empty when the picture cannot be had. Blocking.
  std::string Fetch(const std::string& content);

 private:
  std::string Download(std::uint64_t asset_id);

  HttpClient* http_ = nullptr;
  std::filesystem::path cookie_file_;
  std::filesystem::path overlay_dir_;
  std::function<std::filesystem::path()> asset_root_;
};

}  // namespace services
}  // namespace mocktail

#endif  // MOCKTAIL_SERVICES_CURSOR_PICTURE_SOURCE_H_
