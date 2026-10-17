#ifndef MOCKTAIL_RUNTIME_ROBLOX_CURSOR_HOOK_H_
#define MOCKTAIL_RUNTIME_ROBLOX_CURSOR_HOOK_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mocktail/cursor_image.h"

namespace mocktail {
namespace runtime {

// What Roblox draws as the mouse cursor, by the system cursor that can stand
// in for it. kCustom is a picture Roblox would draw, a game's own or one of
// Roblox's, that the desktop draws instead. kOther is the shift lock cursor,
// a cursor whose picture is not ready, or none: Roblox keeps drawing those.
enum class RobloxCursorKind : std::uint8_t {
  kOther,
  kArrow,
  kPointer,
  kText,
  kCustom,
};

// Classifies the content ID of the cursor Roblox draws, such as
// rbxasset://textures/Cursors/KeyboardMouse/ArrowFarCursor.png. Only the
// stock keyboard-and-mouse cursors have a system equivalent.
RobloxCursorKind ClassifyRobloxCursorContent(std::string_view content);

// True for a cursor the desktop can draw from Roblox's picture: any but none
// and the shift lock cursor, which Roblox draws at the center of the window.
bool IsHardwareCursorContent(std::string_view content);

// Decodes a cursor picture the way Roblox draws it: each side shrunk to at
// most `max_dimension` pixels when that is above 0, with the hot spot in the
// middle. Null for anything but a PNG, or one too large for a desktop cursor.
std::shared_ptr<CursorImage> DecodeCustomCursor(const std::uint8_t* data,
                                                std::size_t size,
                                                int max_dimension);

// True for an asset path that names one of those stock cursor textures, with
// or without a scheme or a leading directory, such as
// android/textures/Cursors/KeyboardMouse/IBeamCursor.png.
bool IsStockSystemCursorAsset(std::string_view path);

// The rbxasset:// names of the stock cursor textures. Unmapping them makes
// Roblox open the local file for one it would otherwise download.
std::vector<std::string> StockSystemCursorAssetNames();

// A fully transparent 64x64 PNG that replaces each stock cursor texture, so
// Roblox draws nothing under the system cursor.
const std::uint8_t* TransparentCursorPng(std::size_t* size);

// One loaded ELF segment, addressed as the code that runs from it sees it.
struct CursorHookSegment {
  std::uintptr_t address = 0;
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
  bool executable = false;
};

// Where the hooks go in Roblox's per-frame cursor draw.
struct CursorRenderSite {
  // Entry of the function that picks and draws the cursor.
  std::uintptr_t render_function = 0;
  // The `call` that reads the type of the cursor content just before the
  // draw. Its argument points at that content.
  std::uintptr_t content_type_call = 0;
  // What that call reaches.
  std::uintptr_t content_type_function = 0;
  // Roblox's MaxCursorIconDimensions flag, an int, or 0 when not found.
  std::uintptr_t max_dimension_flag = 0;
};

// Finds the draw function through the one instruction that loads its
// "[FLog::CursorCache] Invalid asset ID" message, then checks the code shape
// both hooks rely on. Fails rather than guess when any part does not match.
std::optional<CursorRenderSite> LocateCursorRenderSite(
    const std::vector<CursorHookSegment>& segments, std::string* error);

// The bytes of the picture a cursor content ID names, or empty. Blocking: it
// may read a file or download. Called on a worker thread of its own.
using RobloxCursorPictureFetcher = std::string (*)(void* context,
                                                   const std::string& content);

struct RobloxCursorOptions {
  // The stock arrow, hand and text cursors come from the desktop's theme.
  bool system_theme = false;
  // Roblox's own cursor pictures, a game's included, are drawn by the
  // desktop instead of into the game. Needs `fetch`.
  bool hardware = false;
  RobloxCursorPictureFetcher fetch = nullptr;
  void* fetch_context = nullptr;
  // Whether the desktop would show a cursor taken from Roblox right now.
  // While it would not, as under shift lock, Roblox keeps drawing it. Null
  // means always.
  bool (*desktop_shows_cursor)() = nullptr;
};

// `image` is set for kCustom only.
using RobloxCursorObserver =
    void (*)(RobloxCursorKind kind,
             const std::shared_ptr<const CursorImage>& image);

// Hooks the cursor draw of the libroblox.so loaded at `image_base`, which no
// code may have run from yet. The observer is called from Roblox's render
// thread each time Roblox draws or skips the cursor. Returns false, changing
// nothing, when the code does not match.
bool InstallRobloxCursorHooks(std::uintptr_t image_base,
                              RobloxCursorOptions options,
                              RobloxCursorObserver observer,
                              std::string* error);

bool RobloxCursorHooksInstalled();

// True once the hooks are in with the system theme on: Roblox's stock cursor
// textures are then replaced by transparent ones.
bool RobloxCursorThemeActive();

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_CURSOR_HOOK_H_
