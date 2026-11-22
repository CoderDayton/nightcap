#include "ui/cursor_slots.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <system_error>

#include "ui/slot_files.h"

namespace mocktail {
namespace ui {
namespace {

constexpr std::uintmax_t kMaxCursorBytes = 4 * 1024 * 1024;
constexpr std::array<unsigned char, 8> kPngSignature = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

std::string_view FileName(CursorSlot slot) {
  switch (slot) {
    case CursorSlot::kArrow:
      return "ArrowFarCursor.png";
    case CursorSlot::kPointer:
      return "ArrowCursor.png";
    case CursorSlot::kText:
      return "IBeamCursor.png";
  }
  return {};
}

std::uint32_t BigEndian(const unsigned char* bytes) {
  return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
         (std::uint32_t{bytes[2]} << 8) | std::uint32_t{bytes[3]};
}

bool CheckPng(const std::filesystem::path& source, std::string* error) {
  const std::string name = source.filename().string();
  std::array<unsigned char, 24> header{};
  std::ifstream in(source, std::ios::binary);
  if (!in.read(reinterpret_cast<char*>(header.data()), header.size()) ||
      std::memcmp(header.data(), kPngSignature.data(), kPngSignature.size()) !=
          0 ||
      std::memcmp(header.data() + 12, "IHDR", 4) != 0) {
    *error = name + " is not a PNG image.";
    return false;
  }
  const std::uint32_t width = BigEndian(header.data() + 16);
  const std::uint32_t height = BigEndian(header.data() + 20);
  if (width == 0 || height == 0) {
    *error = name + " is not a PNG image.";
    return false;
  }
  if (width > kMaxCursorSize || height > kMaxCursorSize) {
    *error = name + " is " + std::to_string(width) + "×" +
             std::to_string(height) + ". A cursor can be at most " +
             std::to_string(kMaxCursorSize) + "×" +
             std::to_string(kMaxCursorSize) + ".";
    return false;
  }
  std::error_code failure;
  if (std::filesystem::file_size(source, failure) > kMaxCursorBytes ||
      failure) {
    *error = name + " is too large for a cursor.";
    return false;
  }
  return true;
}

}

std::string_view CursorSlotLabel(CursorSlot slot) {
  switch (slot) {
    case CursorSlot::kArrow:
      return "Arrow";
    case CursorSlot::kPointer:
      return "Pointer";
    case CursorSlot::kText:
      return "Text";
  }
  return {};
}

std::vector<std::filesystem::path> CursorFiles(
    const std::filesystem::path& overlay_dir, CursorSlot slot) {
  const std::filesystem::path textures = overlay_dir / "content" / "textures";
  return {textures / "Cursors" / "KeyboardMouse" / FileName(slot),
          textures / FileName(slot)};
}

std::filesystem::path CustomCursor(const std::filesystem::path& overlay_dir,
                                   CursorSlot slot) {
  return FirstRegularFile(CursorFiles(overlay_dir, slot));
}

bool HasCustomCursor(const std::filesystem::path& overlay_dir,
                     CursorSlot slot) {
  return !CustomCursor(overlay_dir, slot).empty();
}

bool InstallCursor(const std::filesystem::path& source,
                   const std::filesystem::path& overlay_dir, CursorSlot slot,
                   std::string* error) {
  if (!CheckPng(source, error)) return false;
  std::error_code failure;
  if (!ReplaceAllWith(source, CursorFiles(overlay_dir, slot), &failure)) {
    *error = "Cannot save the cursor: " + failure.message();
    return false;
  }
  return true;
}

bool RemoveCursor(const std::filesystem::path& overlay_dir, CursorSlot slot,
                  std::string* error) {
  std::error_code failure;
  if (!RemoveAll(CursorFiles(overlay_dir, slot), &failure)) {
    *error = "Cannot remove the cursor: " + failure.message();
    return false;
  }
  return true;
}

}
}
