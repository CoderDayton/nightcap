#include "runtime/asset_overlay_bridge.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace mocktail {
namespace runtime {

namespace {

struct RedirectCandidate {
  const char* target_uri;
  const char* replacement_uri;
  const char* filename;
  const char* subpath;
};

constexpr RedirectCandidate kRedirectCandidates[] = {
    // IBeam cursor variants
    {"rbxasset://textures/Cursors/KeyboardMouse/IBeamCursor.png",
     "rbxasset://tx_patch/Cursors/KeyboardMouse/IBeamCursor.png",
     "IBeamCursor.png",
     "textures/Cursors/KeyboardMouse/IBeamCursor.png"},

    {"rbxasset://textures/Cursors/Gamepad/IBeamCursor.png",
     "rbxasset://tx_patch/Cursors/Gamepad/IBeamCursor.png",
     "IBeamCursor.png",
     "textures/Cursors/Gamepad/IBeamCursor.png"},

    {"rbxasset://textures/IBeamCursor.png",
     "rbxasset://tx_patch/IBeamCursor.png",
     "IBeamCursor.png",
     "textures/Cursors/KeyboardMouse/IBeamCursor.png"},

    // Shift lock cursor
    {"rbxasset://textures/MouseLockedCursor.png",
     "rbxasset://tx_patch/MouseLockedCursor.png",
     "MouseLockedCursor.png",
     "textures/Cursors/KeyboardMouse/MouseLockedCursor.png"},

    // Standard arrow cursors and fallbacks
    {"rbxasset://textures/ArrowFarCursor.png",
     "rbxasset://tx_patch/ArrowFarCursor.png",
     "ArrowFarCursor.png",
     "textures/Cursors/KeyboardMouse/ArrowFarCursor.png"},

    {"rbxasset://textures/ArrowCursorDecalDrag.png",
     "rbxasset://tx_patch/ArrowCursorDecalDrag.png",
     "ArrowCursorDecalDrag.png",
     nullptr},

    // Gamepad pointer cursors
    {"rbxasset://textures/Cursors/Gamepad/PointerOver.png",
     "rbxasset://tx_patch/Cursors/Gamepad/PointerOver.png",
     "PointerOver.png",
     "textures/Cursors/Gamepad/PointerOver.png"},

    {"rbxasset://textures/Cursors/Gamepad/PointerOver@2x.png",
     "rbxasset://tx_patch/Cursors/Gamepad/PointerOver@2x.png",
     "PointerOver@2x.png",
     "textures/Cursors/Gamepad/PointerOver@2x.png"},

    // Drag detector and interaction cursors
    {"rbxasset://textures/Cursors/CrossMouseIcon.png",
     "rbxasset://tx_patch/Cursors/CrossMouseIcon.png",
     "CrossMouseIcon.png",
     "textures/Cursors/CrossMouseIcon.png"},

    {"rbxasset://textures/Cursors/DragDetector/ActivatedCursor.png",
     "rbxasset://tx_patch/Cursors/DragDetector/ActivatedCursor.png",
     "ActivatedCursor.png",
     "textures/Cursors/DragDetector/ActivatedCursor.png"},

    {"rbxasset://textures/Cursors/DragDetector/HoverCursor.png",
     "rbxasset://tx_patch/Cursors/DragDetector/HoverCursor.png",
     "HoverCursor.png",
     "textures/Cursors/DragDetector/HoverCursor.png"},

    {"rbxasset://textures/Cursors/mouseIconCameraTrack.png",
     "rbxasset://tx_patch/Cursors/mouseIconCameraTrack.png",
     "mouseIconCameraTrack.png",
     "textures/Cursors/mouseIconCameraTrack.png"},

    // Tool cursors
    {"rbxasset://textures/MaterialCursor.png",
     "rbxasset://tx_patch/MaterialCursor.png",
     "MaterialCursor.png",
     nullptr},

    {"rbxasset://textures/AnchorCursor.png",
     "rbxasset://tx_patch/AnchorCursor.png",
     "AnchorCursor.png",
     nullptr},

    {"rbxasset://textures/UnanchorCursor.png",
     "rbxasset://tx_patch/UnanchorCursor.png",
     "UnanchorCursor.png",
     nullptr},

    {"rbxasset://textures/LockCursor.png",
     "rbxasset://tx_patch/LockCursor.png",
     "LockCursor.png",
     nullptr},

    {"rbxasset://textures/UnlockCursor.png",
     "rbxasset://tx_patch/UnlockCursor.png",
     "UnlockCursor.png",
     nullptr},

    {"rbxasset://textures/WeldCursor.png",
     "rbxasset://tx_patch/WeldCursor.png",
     "WeldCursor.png",
     nullptr},

    {"rbxasset://textures/GlueCursor.png",
     "rbxasset://tx_patch/GlueCursor.png",
     "GlueCursor.png",
     nullptr},

    {"rbxasset://textures/HingeCursor.png",
     "rbxasset://tx_patch/HingeCursor.png",
     "HingeCursor.png",
     nullptr},

    {"rbxasset://textures/MotorCursor.png",
     "rbxasset://tx_patch/MotorCursor.png",
     "MotorCursor.png",
     nullptr},

    {"rbxasset://textures/FlatCursor.png",
     "rbxasset://tx_patch/FlatCursor.png",
     "FlatCursor.png",
     nullptr},

    {"rbxasset://textures/FillCursor.png",
     "rbxasset://tx_patch/FillCursor.png",
     "FillCursor.png",
     nullptr},

    {"rbxasset://textures/ConstraintCursor.png",
     "rbxasset://tx_patch/ConstraintCursor.png",
     "ConstraintCursor.png",
     nullptr},

    {"rbxasset://textures/advCursor-default.png",
     "rbxasset://tx_patch/advCursor-default.png",
     "advCursor-default.png",
     nullptr},

    {"rbxasset://textures/advCursor-openedHand.png",
     "rbxasset://tx_patch/advCursor-openedHand.png",
     "advCursor-openedHand.png",
     nullptr},

    {"rbxasset://textures/advCursor-white.png",
     "rbxasset://tx_patch/advCursor-white.png",
     "advCursor-white.png",
     nullptr},

    // VirtualCursor variants
    {"rbxasset://textures/ui/VirtualCursor/cursorArrow.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorArrow.png",
     "cursorArrow.png",
     "textures/ui/VirtualCursor/cursorArrow.png"},

    {"rbxasset://textures/ui/VirtualCursor/cursorArrow@2x.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorArrow@2x.png",
     "cursorArrow@2x.png",
     "textures/ui/VirtualCursor/cursorArrow@2x.png"},

    {"rbxasset://textures/ui/VirtualCursor/cursorArrow@3x.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorArrow@3x.png",
     "cursorArrow@3x.png",
     "textures/ui/VirtualCursor/cursorArrow@3x.png"},

    {"rbxasset://textures/ui/VirtualCursor/cursorDefault.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorDefault.png",
     "cursorDefault.png",
     "textures/ui/VirtualCursor/cursorDefault.png"},

    {"rbxasset://textures/ui/VirtualCursor/cursorDefault@2x.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorDefault@2x.png",
     "cursorDefault@2x.png",
     "textures/ui/VirtualCursor/cursorDefault@2x.png"},

    {"rbxasset://textures/ui/VirtualCursor/cursorDefault@3x.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorDefault@3x.png",
     "cursorDefault@3x.png",
     "textures/ui/VirtualCursor/cursorDefault@3x.png"},

    {"rbxasset://textures/ui/VirtualCursor/cursorHover.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorHover.png",
     "cursorHover.png",
     "textures/ui/VirtualCursor/cursorHover.png"},

    {"rbxasset://textures/ui/VirtualCursor/cursorHover@2x.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorHover@2x.png",
     "cursorHover@2x.png",
     "textures/ui/VirtualCursor/cursorHover@2x.png"},

    {"rbxasset://textures/ui/VirtualCursor/cursorHover@3x.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorHover@3x.png",
     "cursorHover@3x.png",
     "textures/ui/VirtualCursor/cursorHover@3x.png"},

    {"rbxasset://textures/ui/VirtualCursor/cursorPressed.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorPressed.png",
     "cursorPressed.png",
     "textures/ui/VirtualCursor/cursorPressed.png"},

    {"rbxasset://textures/ui/VirtualCursor/cursorPressed@2x.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorPressed@2x.png",
     "cursorPressed@2x.png",
     "textures/ui/VirtualCursor/cursorPressed@2x.png"},

    {"rbxasset://textures/ui/VirtualCursor/cursorPressed@3x.png",
     "rbxasset://tx_patch/ui/VirtualCursor/cursorPressed@3x.png",
     "cursorPressed@3x.png",
     "textures/ui/VirtualCursor/cursorPressed@3x.png"},
};

constexpr std::size_t kDefaultSearchOffset = 0x200000;
constexpr std::size_t kDefaultSearchLength = 0x1e00000;

inline std::string ToLower(std::string_view s) {
  std::string result;
  result.reserve(s.size());
  for (char c : s) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return result;
}

bool PatchReadOnlyMemory(void* dest, const void* src, std::size_t len) {
  if (dest == nullptr || src == nullptr || len == 0) {
    return false;
  }
  const uintptr_t addr = reinterpret_cast<uintptr_t>(dest);
  const long page_size = sysconf(_SC_PAGESIZE);
  const uintptr_t page_mask = ~(static_cast<uintptr_t>(page_size) - 1);
  const uintptr_t page_start = addr & page_mask;
  const uintptr_t page_end = ((addr + len + page_size - 1) & page_mask);

  if (mprotect(reinterpret_cast<void*>(page_start), page_end - page_start,
               PROT_READ | PROT_WRITE) != 0) {
    return false;
  }
  std::memcpy(dest, src, len);
  mprotect(reinterpret_cast<void*>(page_start), page_end - page_start, PROT_READ);
  return true;
}

}  // namespace

bool OverlayHasAsset(const std::filesystem::path& overlay_dir,
                     const char* filename,
                     const char* subpath) {
  std::error_code ec;
  if (!std::filesystem::is_directory(overlay_dir, ec)) {
    return false;
  }
  const std::string fn(filename);
  std::vector<std::filesystem::path> candidates = {
      overlay_dir / fn,
      overlay_dir / "content" / fn,
      overlay_dir / "content" / "textures" / fn,
      overlay_dir / "content" / "textures" / "Cursors" / "KeyboardMouse" / fn,
      overlay_dir / "content" / "textures" / "Cursors" / "Gamepad" / fn,
      overlay_dir / "textures" / fn,
      overlay_dir / "textures" / "Cursors" / "KeyboardMouse" / fn,
      overlay_dir / "textures" / "Cursors" / "Gamepad" / fn,
      overlay_dir / "Cursors" / "KeyboardMouse" / fn,
  };
  if (subpath != nullptr && subpath[0] != '\0') {
    std::string_view sp(subpath);
    if (sp.rfind("rbxasset://", 0) == 0) {
      sp.remove_prefix(11);
    }
    candidates.push_back(overlay_dir / sp);
    candidates.push_back(overlay_dir / "content" / sp);
    if (sp.rfind("textures/", 0) == 0) {
      std::string_view rest = sp.substr(9);
      candidates.push_back(overlay_dir / rest);
      candidates.push_back(overlay_dir / "content" / rest);
      candidates.push_back(overlay_dir / "content" / "textures" / rest);
    }
  }
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate, ec)) {
      return true;
    }
  }
  return false;
}

bool NeutralizeOnlineAssetRedirect(uintptr_t base_address,
                                   std::size_t search_offset,
                                   std::size_t search_length,
                                   const char* target_uri,
                                   const char* replacement_uri) {
  if (base_address == 0 || target_uri == nullptr || replacement_uri == nullptr) {
    return false;
  }
  const std::size_t target_len = std::strlen(target_uri);
  const std::size_t repl_len = std::strlen(replacement_uri);
  if (target_len != repl_len || target_len == 0) {
    return false;
  }

  const void* search_start = reinterpret_cast<const void*>(base_address + search_offset);
  void* match = ::memmem(search_start, search_length, target_uri, target_len);
  if (match == nullptr) {
    return false;
  }

  return PatchReadOnlyMemory(match, replacement_uri, target_len);
}

std::size_t NeutralizeOnlineAssetRedirectsForOverlay(
    uintptr_t libroblox_base,
    const char* override_overlay_dir,
    std::size_t search_length) {
  if (libroblox_base == 0) {
    return 0;
  }

  const char* overlay_dir_str = override_overlay_dir;
  if (overlay_dir_str == nullptr || overlay_dir_str[0] == '\0') {
    overlay_dir_str = std::getenv("MOCKTAIL_ASSET_OVERLAY_DIR");
  }
  if (overlay_dir_str == nullptr || overlay_dir_str[0] == '\0') {
    return 0;
  }

  const std::filesystem::path overlay_path(overlay_dir_str);
  std::error_code ec;
  if (!std::filesystem::is_directory(overlay_path, ec)) {
    return 0;
  }

  // Pre-index overlay directory for rapid in-memory lookups.
  std::unordered_set<std::string> overlay_index;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(overlay_path, ec)) {
    if (entry.is_regular_file(ec)) {
      const auto rel = std::filesystem::relative(entry.path(), overlay_path, ec).generic_string();
      const std::string lower_rel = ToLower(rel);
      const std::string lower_fn = ToLower(entry.path().filename().generic_string());
      overlay_index.insert(lower_rel);
      overlay_index.insert(lower_fn);
      if (lower_rel.rfind("content/", 0) == 0) {
        overlay_index.insert(lower_rel.substr(8));
        if (lower_rel.rfind("content/textures/", 0) == 0) {
          overlay_index.insert(lower_rel.substr(17));
        }
      } else if (lower_rel.rfind("textures/", 0) == 0) {
        overlay_index.insert(lower_rel.substr(9));
      }
    }
  }

  const std::size_t scan_len = (search_length != 0) ? search_length : kDefaultSearchLength;
  std::size_t neutralized_count = 0;
  std::unordered_set<std::string> patched_uris;

  // 1. Check explicit redirect candidates
  for (const auto& candidate : kRedirectCandidates) {
    if (OverlayHasAsset(overlay_path, candidate.filename, candidate.subpath)) {
      if (NeutralizeOnlineAssetRedirect(libroblox_base,
                                         kDefaultSearchOffset,
                                         scan_len,
                                         candidate.target_uri,
                                         candidate.replacement_uri)) {
        std::fprintf(stderr,
                     "  [overlay] neutralized online redirect for %s (overridden by local asset)\n",
                     candidate.target_uri);
        patched_uris.insert(candidate.target_uri);
        ++neutralized_count;
      }
    }
  }

  // 2. Dynamic sweep across .rodata for any additional "rbxasset://textures/" redirects
  const char* const kPrefix = "rbxasset://textures/";
  constexpr std::size_t kPrefixLen = 19;
  const uint8_t* const search_start =
      reinterpret_cast<const uint8_t*>(libroblox_base + kDefaultSearchOffset);

  std::size_t search_pos = 0;
  while (search_pos + kPrefixLen <= scan_len) {
    const void* match = ::memmem(search_start + search_pos, scan_len - search_pos,
                                 kPrefix, kPrefixLen);
    if (match == nullptr) {
      break;
    }

    const uint8_t* match_ptr = static_cast<const uint8_t*>(match);
    search_pos = (match_ptr - search_start) + kPrefixLen;

    // Determine string length up to null terminator (max 256 bytes)
    std::size_t str_len = 0;
    while (search_pos - kPrefixLen + str_len < scan_len &&
           match_ptr[str_len] != '\0' && str_len < 256) {
      ++str_len;
    }
    if (str_len >= 256 || match_ptr[str_len] != '\0') {
      continue;
    }

    const std::string uri(reinterpret_cast<const char*>(match_ptr), str_len);
    if (patched_uris.count(uri) != 0) {
      continue;
    }

    // Extract subpath and filename
    const std::string subpath = uri.substr(11);           // "textures/..."
    const std::string sub_notex = uri.substr(kPrefixLen); // "..."
    const std::size_t last_slash = uri.rfind('/');
    const std::string filename = (last_slash != std::string::npos) ? uri.substr(last_slash + 1) : uri;

    const std::string lower_sub = ToLower(subpath);
    const std::string lower_notex = ToLower(sub_notex);
    const std::string lower_fn = ToLower(filename);

    const bool is_cursor = (lower_fn.find("cursor") != std::string::npos ||
                            lower_sub.find("cursor") != std::string::npos);

    bool should_neutralize = false;
    if (overlay_index.count(lower_sub) != 0 ||
        overlay_index.count(lower_notex) != 0 ||
        overlay_index.count("content/" + lower_sub) != 0 ||
        overlay_index.count("content/textures/" + lower_notex) != 0) {
      should_neutralize = true;
    } else if (is_cursor && overlay_index.count(lower_fn) != 0) {
      should_neutralize = true;
    }

    if (should_neutralize) {
      // Patch "textures/" (9 bytes) -> "tx_patch/" in-place
      void* patch_dest = const_cast<uint8_t*>(match_ptr) + 11;
      if (PatchReadOnlyMemory(patch_dest, "tx_patch/", 9)) {
        std::fprintf(stderr,
                     "  [overlay] neutralized online redirect for %s (overridden by local asset)\n",
                     uri.c_str());
        patched_uris.insert(uri);
        ++neutralized_count;
      }
    }
  }

  return neutralized_count;
}

}  // namespace runtime
}  // namespace mocktail
