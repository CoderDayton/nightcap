#ifndef MOCKTAIL_UI_CURSOR_SLOTS_H_
#define MOCKTAIL_UI_CURSOR_SLOTS_H_

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace ui {

enum class CursorSlot { kArrow, kPointer, kText };

inline constexpr std::array<CursorSlot, 3> kCursorSlots = {
    CursorSlot::kArrow, CursorSlot::kPointer, CursorSlot::kText};

inline constexpr int kMaxCursorSize = 256;

std::string_view CursorSlotLabel(CursorSlot slot);

std::vector<std::filesystem::path> CursorFiles(
    const std::filesystem::path& overlay_dir, CursorSlot slot);

std::filesystem::path CustomCursor(const std::filesystem::path& overlay_dir,
                                   CursorSlot slot);

bool HasCustomCursor(const std::filesystem::path& overlay_dir,
                     CursorSlot slot);

bool InstallCursor(const std::filesystem::path& source,
                   const std::filesystem::path& overlay_dir, CursorSlot slot,
                   std::string* error);

bool RemoveCursor(const std::filesystem::path& overlay_dir, CursorSlot slot,
                  std::string* error);

}
}

#endif
