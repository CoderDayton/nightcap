#include "mocktail/vr/virtual_keyboard.h"

#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <utility>

namespace mocktail::vr {
namespace {

struct Rgb {
  std::uint8_t r, g, b;
};

// Roblox's dark in-experience menu: page, surface and emphasis colours as
// they appear in the Escape menu and the app's home page.
constexpr Rgb kBackground{19, 18, 21};
constexpr std::uint8_t kBackgroundAlpha = 240;
constexpr Rgb kField{32, 34, 38};
constexpr Rgb kKey{53, 56, 64};
constexpr Rgb kSpecialKey{32, 34, 39};
constexpr Rgb kEmphasis{37, 50, 112};
constexpr Rgb kLabel{247, 247, 248};
constexpr Rgb kSpecialLabel{213, 215, 221};
constexpr Rgb kWhite{255, 255, 255};
constexpr Rgb kBlack{0, 0, 0};
// Hover lightens and a press darkens, like Roblox's state overlays.
constexpr std::uint8_t kHoverOverlay = 26;
constexpr std::uint8_t kPressOverlay = 77;
constexpr int kPanelRadius = 20;
constexpr int kFieldPadding = 28;
constexpr int kGap = 10;
constexpr int kRadius = 16;
// Roblox's selection cursor: a white ring just outside the hovered control.
constexpr float kRingGap = 1.f;
constexpr float kRingWidth = 3.f;
constexpr float kLetterPoints = 40.f;
constexpr float kSpecialPoints = 28.f;
constexpr float kIconPoints = 44.f;
constexpr float kPreviewPoints = 60.f;
// Raised surfaces over the keys: the press preview and the accent row.
constexpr Rgb kRaised{70, 74, 84};
// The drag bar under the panel, like Roblox's spatial UI handle.
constexpr Rgb kHandle{213, 215, 221};
constexpr std::uint8_t kHandleAlpha = 140;

std::string ResolveFontconfigSans() {
  std::string file;
  if (!FcInit()) return file;
  FcPattern* pattern =
      FcNameParse(reinterpret_cast<const FcChar8*>("sans:weight=medium"));
  if (pattern == nullptr) return file;
  FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);
  FcResult result = FcResultNoMatch;
  FcPattern* match = FcFontMatch(nullptr, pattern, &result);
  FcPatternDestroy(pattern);
  if (match == nullptr) return file;
  FcChar8* path = nullptr;
  if (FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch &&
      path != nullptr) {
    file = reinterpret_cast<const char*>(path);
  }
  FcPatternDestroy(match);
  return file;
}

// The payload's content directory (MOCKTAIL_ASSET_PATH), without a trailing
// slash so parent_path() is the assets directory.
std::filesystem::path PayloadContent() {
  const char* content = std::getenv("MOCKTAIL_ASSET_PATH");
  if (content == nullptr || content[0] == '\0') return {};
  std::filesystem::path path(content);
  if (!path.has_filename()) path = path.parent_path();
  return path;
}

std::string ExistingFile(const std::filesystem::path& path) {
  std::error_code error;
  return !path.empty() && std::filesystem::is_regular_file(path, error)
             ? path.string()
             : std::string();
}

// MOCKTAIL_TEXT_FONT wins like the TextBox overlay, then Roblox's Builder
// Sans from the payload, then fontconfig's best sans face.
std::string ResolveSansFont() {
  const char* configured = std::getenv("MOCKTAIL_TEXT_FONT");
  if (configured != nullptr && configured[0] != '\0') return configured;
  const std::filesystem::path content = PayloadContent();
  if (!content.empty()) {
    std::string builder =
        ExistingFile(content / "fonts" / "BuilderSans-Medium.otf");
    if (!builder.empty()) return builder;
  }
  return ResolveFontconfigSans();
}

// Roblox's icon font. Icons are ligatures: the text "key-shift" draws the
// Shift glyph.
std::string ResolveIconFont() {
  const std::filesystem::path content = PayloadContent();
  if (content.empty()) return {};
  return ExistingFile(content.parent_path() /
                      "ExtraContent/LuaPackages/Packages/_Index/BuilderIcons/"
                      "BuilderIcons/Font/BuilderIcons-Regular.ttf");
}

const char* IconName(VirtualKeyAction action) {
  switch (action) {
    case VirtualKeyAction::kBackspace:
      return "key-backspace";
    case VirtualKeyAction::kEnter:
      return "key-return";
    case VirtualKeyAction::kTab:
      return "key-tab";
    case VirtualKeyAction::kShift:
      return "key-shift";
    case VirtualKeyAction::kCapsLock:
      return "key-caps-lock";
    case VirtualKeyAction::kLeft:
      return "key-arrow-left";
    case VirtualKeyAction::kRight:
      return "key-arrow-right";
    case VirtualKeyAction::kSpace:
      return "key-space";
    case VirtualKeyAction::kClose:
      return "keyboard";
    case VirtualKeyAction::kText:
    case VirtualKeyAction::kSelectAll:
    case VirtualKeyAction::kPaste:
      break;
  }
  return nullptr;
}

struct AlphaMask {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> alpha;
};

// Field text as drawn: password input shows one dot per code point.
std::string DisplayText(const VirtualKeyboard& keyboard) {
  if (!keyboard.text_password()) return keyboard.text();
  std::string dots;
  for (const char c : keyboard.text())
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) dots += "•";
  return dots;
}

// Byte offset in `text` of the UTF-16 cursor. Password dots are one UTF-16
// unit each, like the BMP characters they stand for.
std::size_t CursorByte(const std::string& text, int cursor_utf16) {
  std::size_t byte = 0;
  int units = 0;
  while (byte < text.size() && units < cursor_utf16) {
    const auto lead = static_cast<unsigned char>(text[byte]);
    const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2
                               : lead < 0xF0 ? 3 : 4;
    units += length == 4 ? 2 : 1;
    byte = std::min(text.size(), byte + length);
  }
  return byte;
}

void Scrub(std::vector<std::uint8_t>* bytes) {
  std::fill(bytes->begin(), bytes->end(), 0);
  bytes->clear();
}

// Signed distance from the pixel centre to a rounded rectangle's edge,
// negative inside.
float RoundedRectDistance(int x, int y, float left, float top, float right,
                          float bottom, float radius) {
  const float px = static_cast<float>(x) + 0.5f;
  const float py = static_cast<float>(y) + 0.5f;
  const float cx = std::clamp(px, left + radius, right - radius);
  const float cy = std::clamp(py, top + radius, bottom - radius);
  const float dx = px - cx;
  const float dy = py - cy;
  if (px >= left + radius && px <= right - radius)
    return std::max(top - py, py - bottom);
  if (py >= top + radius && py <= bottom - radius)
    return std::max(left - px, px - right);
  return std::sqrt(dx * dx + dy * dy) - radius;
}

std::uint8_t Coverage(float distance) {
  return static_cast<std::uint8_t>(
      std::lround(std::clamp(0.5f - distance, 0.f, 1.f) * 255.f));
}

}  // namespace

struct VirtualKeyboardRasterizer::Impl {
  bool font_attempted = false;
  bool ttf_initialized = false;
  TTF_Font* letter_font = nullptr;
  TTF_Font* special_font = nullptr;
  TTF_Font* icon_font = nullptr;
  TTF_Font* preview_font = nullptr;
  std::map<std::pair<std::string, int>, AlphaMask> masks;
  // Last rendered field text; not cached by content, since it is sensitive.
  AlphaMask field;

  ~Impl() {
    Scrub(&field.alpha);
    if (letter_font != nullptr) TTF_CloseFont(letter_font);
    if (special_font != nullptr) TTF_CloseFont(special_font);
    if (icon_font != nullptr) TTF_CloseFont(icon_font);
    if (preview_font != nullptr) TTF_CloseFont(preview_font);
    if (ttf_initialized) TTF_Quit();
  }

  bool EnsureFonts() {
    if (font_attempted) return letter_font != nullptr;
    font_attempted = true;
    if (!TTF_Init()) {
      std::fprintf(stderr, "  [vr-keyboard] SDL3_ttf unavailable: %s\n",
                   SDL_GetError());
      return false;
    }
    ttf_initialized = true;
    const std::string file = ResolveSansFont();
    if (!file.empty()) {
      letter_font = TTF_OpenFont(file.c_str(), kLetterPoints);
      special_font = TTF_OpenFont(file.c_str(), kSpecialPoints);
      preview_font = TTF_OpenFont(file.c_str(), kPreviewPoints);
    }
    if (letter_font == nullptr || special_font == nullptr) {
      std::fprintf(stderr,
                   "  [vr-keyboard] no usable font (%s); keys are unlabelled\n",
                   file.empty() ? "fontconfig found none" : file.c_str());
      return false;
    }
    std::fprintf(stderr, "  [vr-keyboard] labels use %s\n", file.c_str());
    const std::string icons = ResolveIconFont();
    if (!icons.empty()) icon_font = TTF_OpenFont(icons.c_str(), kIconPoints);
    return true;
  }

  static AlphaMask Render(TTF_Font* font, const std::string& text) {
    AlphaMask mask;
    SDL_Surface* rendered = TTF_RenderText_Blended(
        font, text.c_str(), text.size(), SDL_Color{255, 255, 255, 255});
    if (rendered != nullptr) {
      SDL_Surface* rgba = SDL_ConvertSurface(rendered, SDL_PIXELFORMAT_RGBA32);
      if (rgba != nullptr && SDL_LockSurface(rgba)) {
        mask.width = rgba->w;
        mask.height = rgba->h;
        mask.alpha.resize(static_cast<std::size_t>(mask.width) * mask.height);
        for (int y = 0; y < mask.height; ++y) {
          const auto* row = static_cast<const std::uint8_t*>(rgba->pixels) +
                            static_cast<std::size_t>(y) * rgba->pitch;
          for (int x = 0; x < mask.width; ++x) {
            mask.alpha[static_cast<std::size_t>(y) * mask.width + x] =
                row[x * 4 + 3];
          }
        }
        SDL_UnlockSurface(rgba);
      }
      if (rgba != nullptr) SDL_DestroySurface(rgba);
      SDL_DestroySurface(rendered);
    }
    return mask;
  }

  enum Face { kLetter = 0, kSpecial = 1, kIcon = 2, kPreview = 3 };

  const AlphaMask* Mask(const std::string& label, Face face) {
    if (label.empty() || !EnsureFonts()) return nullptr;
    TTF_Font* font = face == kIcon      ? icon_font
                     : face == kSpecial ? special_font
                     : face == kPreview ? preview_font
                                        : letter_font;
    if (font == nullptr) return nullptr;
    const auto key = std::make_pair(label, static_cast<int>(face));
    auto found = masks.find(key);
    if (found != masks.end()) return &found->second;
    AlphaMask mask = Render(font, label);
    // Without ligature shaping the name draws as a row of blank letters,
    // much wider than one icon; fall back to the text label then.
    if (face == kIcon && mask.width > mask.height * 2) mask = AlphaMask{};
    return &masks.emplace(key, std::move(mask)).first->second;
  }

  // An icon for special keys when Roblox's icon font is there, else text.
  const AlphaMask* KeyMask(const VirtualKey& key) {
    if (key.action == VirtualKeyAction::kText) return Mask(key.label, kLetter);
    if (const char* icon = IconName(key.action); icon != nullptr) {
      const AlphaMask* mask = Mask(icon, kIcon);
      if (mask != nullptr && !mask->alpha.empty()) return mask;
    }
    return Mask(key.label, kSpecial);
  }
};

VirtualKeyboardRasterizer::VirtualKeyboardRasterizer() : impl_(new Impl) {}
VirtualKeyboardRasterizer::~VirtualKeyboardRasterizer() { delete impl_; }

bool VirtualKeyboardRasterizer::Draw(const VirtualKeyboard& keyboard,
                                     std::vector<std::uint8_t>* rgba) {
  constexpr int width = VirtualKeyboard::kTextureWidth;
  constexpr int height = VirtualKeyboard::kTextureHeight;
  rgba->assign(static_cast<std::size_t>(width) * height * 4, 0);
  std::uint8_t* pixels = rgba->data();
  // Source-over onto straight (unpremultiplied) alpha.
  auto blend = [&](int x, int y, Rgb color, std::uint8_t alpha) {
    if (x < 0 || y < 0 || x >= width || y >= height || alpha == 0) return;
    std::uint8_t* p = pixels + (static_cast<std::size_t>(y) * width + x) * 4;
    const int src = alpha;
    const int dst = p[3] * (255 - src) / 255;
    const int out = src + dst;
    if (out == 0) return;
    p[0] = static_cast<std::uint8_t>((color.r * src + p[0] * dst) / out);
    p[1] = static_cast<std::uint8_t>((color.g * src + p[1] * dst) / out);
    p[2] = static_cast<std::uint8_t>((color.b * src + p[2] * dst) / out);
    p[3] = static_cast<std::uint8_t>(out);
  };
  auto fill_rounded = [&](float left, float top, float right, float bottom,
                          float radius, Rgb color, std::uint8_t alpha) {
    const int y0 = std::max(0, static_cast<int>(std::floor(top)));
    const int y1 = std::min(height, static_cast<int>(std::ceil(bottom)));
    const int x0 = std::max(0, static_cast<int>(std::floor(left)));
    const int x1 = std::min(width, static_cast<int>(std::ceil(right)));
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const std::uint8_t cover = Coverage(
            RoundedRectDistance(x, y, left, top, right, bottom, radius));
        blend(x, y, color, static_cast<std::uint8_t>(cover * alpha / 255));
      }
  };
  // A ring `gap` outside the rectangle, `thickness` wide.
  auto ring = [&](float left, float top, float right, float bottom,
                  float radius, float gap, float thickness, Rgb color) {
    const float outer = gap + thickness;
    const int y0 = std::max(0, static_cast<int>(std::floor(top - outer)));
    const int y1 = std::min(height, static_cast<int>(std::ceil(bottom + outer)));
    const int x0 = std::max(0, static_cast<int>(std::floor(left - outer)));
    const int x1 = std::min(width, static_cast<int>(std::ceil(right + outer)));
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const float d =
            RoundedRectDistance(x, y, left, top, right, bottom, radius) - gap;
        const float band = std::abs(d - thickness / 2.f) - thickness / 2.f;
        blend(x, y, color, Coverage(band));
      }
  };
  auto blit = [&](const AlphaMask& mask, int origin_x, int origin_y, Rgb color,
                  int clip_left, int clip_right) {
    for (int y = 0; y < mask.height; ++y)
      for (int x = 0; x < mask.width; ++x) {
        const int px = origin_x + x;
        if (px < clip_left || px >= clip_right) continue;
        blend(px, origin_y + y, color,
              mask.alpha[static_cast<std::size_t>(y) * mask.width + x]);
      }
  };

  const float body_bottom = VirtualKeyboard::kBodyFraction * height;
  fill_rounded(0.f, 0.f, width, body_bottom, kPanelRadius, kBackground,
               kBackgroundAlpha);

  // Text field: the focused TextBox's contents and caret, scrolled so the
  // caret stays visible.
  {
    const int left = kGap / 2;
    const int right = width - kGap / 2;
    const int top = kGap / 2;
    const int bottom = static_cast<int>(std::lround(
                           VirtualKeyboard::kFieldFraction * height)) -
                       kGap / 2;
    fill_rounded(left, top, right, bottom, kRadius, kField, 255);
    const int clip_left = left + kFieldPadding;
    const int clip_right = right - kFieldPadding;
    const int inner = clip_right - clip_left;
    const std::string shown = DisplayText(keyboard);
    Scrub(&impl_->field.alpha);
    impl_->field = AlphaMask{};
    int caret_x = 0;
    int line_height = static_cast<int>(kLetterPoints * 1.2f);
    if (impl_->EnsureFonts()) {
      line_height = TTF_GetFontHeight(impl_->letter_font);
      if (!shown.empty()) {
        impl_->field = Impl::Render(impl_->letter_font, shown);
        const std::size_t cursor =
            CursorByte(shown, keyboard.text_cursor_utf16());
        int unused = 0;
        if (cursor > 0)
          (void)TTF_GetStringSize(impl_->letter_font, shown.c_str(), cursor,
                                  &caret_x, &unused);
      }
    }
    const int scroll = std::max(0, caret_x - inner + 4);
    const int origin_y = (top + bottom - line_height) / 2;
    blit(impl_->field, clip_left - scroll, origin_y, kLabel, clip_left,
         clip_right);
    const int caret = clip_left + caret_x - scroll;
    const int caret_top = origin_y + line_height / 8;
    const int caret_bottom = origin_y + line_height - line_height / 8;
    for (int y = caret_top; y < caret_bottom; ++y)
      for (int x = caret; x < caret + 3; ++x)
        if (x >= clip_left - 2 && x < clip_right + 2) blend(x, y, kLabel, 255);
  }

  bool labelled = true;
  const auto& keys = keyboard.keys();
  for (std::size_t index = 0; index < keys.size(); ++index) {
    const VirtualKey& key = keys[index];
    const float left = key.u0 * width + kGap / 2.f;
    const float right = key.u1 * width - kGap / 2.f;
    const float top = key.v0 * height + kGap / 2.f;
    const float bottom = key.v1 * height - kGap / 2.f;
    const int i = static_cast<int>(index);
    const bool pressed =
        keyboard.pressed_key(0) == i || keyboard.pressed_key(1) == i;
    const bool hovered =
        keyboard.hovered_key(0) == i || keyboard.hovered_key(1) == i;
    const bool special = key.action != VirtualKeyAction::kText;
    // Latched modifiers stay lit; hover still shows on top of them.
    const bool latched =
        (key.action == VirtualKeyAction::kShift && keyboard.shifted()) ||
        (key.action == VirtualKeyAction::kCapsLock && keyboard.caps_lock());
    const bool emphasis = latched || key.action == VirtualKeyAction::kEnter;
    const Rgb fill = emphasis ? kEmphasis : special ? kSpecialKey : kKey;
    fill_rounded(left, top, right, bottom, kRadius, fill, 255);
    if (pressed)
      fill_rounded(left, top, right, bottom, kRadius, kBlack, kPressOverlay);
    else if (hovered)
      fill_rounded(left, top, right, bottom, kRadius, kWhite, kHoverOverlay);
    if (hovered || pressed)
      ring(left, top, right, bottom, kRadius, kRingGap, kRingWidth, kWhite);

    const AlphaMask* mask = impl_->KeyMask(key);
    if (!key.label.empty() && (mask == nullptr || mask->alpha.empty())) {
      labelled = false;
      continue;
    }
    if (mask == nullptr) continue;
    const int origin_x =
        static_cast<int>(std::lround((left + right - mask->width) / 2.f));
    const int origin_y =
        static_cast<int>(std::lround((top + bottom - mask->height) / 2.f));
    blit(*mask, origin_x, origin_y,
         special && !emphasis ? kSpecialLabel : kLabel, 0, width);
  }

  auto label_at = [&](const AlphaMask* mask, float left, float top,
                      float right, float bottom, Rgb color) {
    if (mask == nullptr) return;
    blit(*mask, static_cast<int>(std::lround((left + right - mask->width) / 2.f)),
         static_cast<int>(std::lround((top + bottom - mask->height) / 2.f)),
         color, 0, width);
  };

  // Long-press accents: a raised row above the held key; the one under the
  // pointer is lit.
  const VirtualKeyboardAccents& accents = keyboard.accents();
  if (accents.hand >= 0 && !accents.cells.empty()) {
    const VirtualKey& first = accents.cells.front();
    const VirtualKey& last = accents.cells.back();
    fill_rounded(first.u0 * width, first.v0 * height + kGap / 2.f,
                 last.u1 * width, first.v1 * height, kRadius, kRaised, 255);
    for (std::size_t cell = 0; cell < accents.cells.size(); ++cell) {
      const VirtualKey& key = accents.cells[cell];
      const float left = key.u0 * width + kGap / 2.f;
      const float right = key.u1 * width - kGap / 2.f;
      const float top = key.v0 * height + kGap;
      const float bottom = key.v1 * height - kGap / 2.f;
      if (static_cast<int>(cell) == accents.selected)
        fill_rounded(left, top, right, bottom, kRadius, kEmphasis, 255);
      label_at(impl_->Mask(key.label, Impl::kLetter), left, top, right, bottom,
               kLabel);
    }
  }

  // Press preview: the held character, enlarged above its key.
  for (int hand = 0; hand < 2; ++hand) {
    const int pressed = keyboard.pressed_key(hand);
    if (pressed < 0 || accents.hand == hand) continue;
    const VirtualKey& key = keys[pressed];
    if (key.action != VirtualKeyAction::kText) continue;
    const float key_width = (key.u1 - key.u0) * width - kGap;
    const float key_height = (key.v1 - key.v0) * height - kGap;
    const float centre = (key.u0 + key.u1) / 2.f * width;
    const float bubble_width = key_width + 24.f;
    const float bubble_height = key_height * 1.1f;
    const float left = std::clamp(centre - bubble_width / 2.f, 0.f,
                                  width - bubble_width);
    const float bottom = key.v0 * height + kGap / 2.f + key_height * 0.25f;
    const float top = std::max(0.f, bottom - bubble_height);
    fill_rounded(left, top, left + bubble_width, bottom, kRadius, kRaised, 255);
    label_at(impl_->Mask(key.label, Impl::kPreview), left, top,
             left + bubble_width, bottom, kLabel);
  }

  // Drag bar under the panel; it brightens while hovered or dragging.
  {
    const bool active = keyboard.handle_hovered() || keyboard.dragging();
    const float bar_width = active ? 220.f : 180.f;
    const float strip_middle = (body_bottom + height) / 2.f;
    fill_rounded(width / 2.f - bar_width / 2.f, strip_middle - 7.f,
                 width / 2.f + bar_width / 2.f, strip_middle + 7.f, 7.f,
                 active ? kWhite : kHandle, active ? 255 : kHandleAlpha);
  }

  // Pointer dots, one per hand over the panel.
  for (int hand = 0; hand < 2; ++hand) {
    float u = 0.f, v = 0.f;
    if (!keyboard.cursor(hand, &u, &v)) continue;
    const int cx = static_cast<int>(std::lround(u * width));
    const int cy = static_cast<int>(std::lround(v * height));
    constexpr float kOuter = 11.f, kInner = 8.f;
    for (int y = cy - 12; y <= cy + 12; ++y)
      for (int x = cx - 12; x <= cx + 12; ++x) {
        const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        const float d = std::sqrt(dx * dx + dy * dy);
        blend(x, y, kBlack, static_cast<std::uint8_t>(Coverage(d - kOuter) * 200 / 255));
        blend(x, y, kWhite, Coverage(d - kInner));
      }
  }
  return labelled;
}

}  // namespace mocktail::vr
