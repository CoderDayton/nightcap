#include "mocktail/vr/virtual_keyboard.h"

#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

namespace mocktail::vr {
namespace {

struct Rgb {
  std::uint8_t r, g, b;
};

constexpr Rgb kBackground{24, 25, 30};
constexpr Rgb kKey{58, 60, 70};
constexpr Rgb kSpecialKey{42, 44, 52};
constexpr Rgb kHover{92, 96, 112};
constexpr Rgb kAccent{82, 132, 255};
constexpr Rgb kAccentHover{120, 160, 255};
constexpr Rgb kLabel{240, 242, 248};
constexpr Rgb kField{12, 13, 17};
constexpr int kFieldPadding = 24;
constexpr int kGap = 8;
constexpr int kRadius = 12;
constexpr float kLetterPoints = 40.f;
constexpr float kSpecialPoints = 28.f;

// fontconfig's best sans face; MOCKTAIL_TEXT_FONT wins like the TextBox
// overlay.
std::string ResolveSansFont() {
  const char* configured = std::getenv("MOCKTAIL_TEXT_FONT");
  if (configured != nullptr && configured[0] != '\0') return configured;
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
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) dots += "\u2022";
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

bool InsideRoundedRect(int x, int y, int left, int top, int right, int bottom,
                       int radius) {
  const int cx = std::clamp(x, left + radius, right - 1 - radius);
  const int cy = std::clamp(y, top + radius, bottom - 1 - radius);
  const int dx = x - cx;
  const int dy = y - cy;
  return dx * dx + dy * dy <= radius * radius;
}

}  // namespace

struct VirtualKeyboardRasterizer::Impl {
  bool font_attempted = false;
  bool ttf_initialized = false;
  TTF_Font* letter_font = nullptr;
  TTF_Font* special_font = nullptr;
  std::map<std::pair<std::string, bool>, AlphaMask> masks;
  // Last rendered field text; not cached by content, since it is sensitive.
  AlphaMask field;

  ~Impl() {
    Scrub(&field.alpha);
    if (letter_font != nullptr) TTF_CloseFont(letter_font);
    if (special_font != nullptr) TTF_CloseFont(special_font);
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
    }
    if (letter_font == nullptr || special_font == nullptr) {
      std::fprintf(stderr,
                   "  [vr-keyboard] no usable font (%s); keys are unlabelled\n",
                   file.empty() ? "fontconfig found none" : file.c_str());
      return false;
    }
    std::fprintf(stderr, "  [vr-keyboard] labels use %s\n", file.c_str());
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

  const AlphaMask* Mask(const std::string& label, bool special) {
    if (label.empty() || !EnsureFonts()) return nullptr;
    const auto key = std::make_pair(label, special);
    auto found = masks.find(key);
    if (found != masks.end()) return &found->second;
    return &masks
                .emplace(key, Render(special ? special_font : letter_font,
                                     label))
                .first->second;
  }
};

VirtualKeyboardRasterizer::VirtualKeyboardRasterizer() : impl_(new Impl) {}
VirtualKeyboardRasterizer::~VirtualKeyboardRasterizer() { delete impl_; }

bool VirtualKeyboardRasterizer::Draw(const VirtualKeyboard& keyboard,
                                     std::vector<std::uint8_t>* rgba) {
  constexpr int width = VirtualKeyboard::kTextureWidth;
  constexpr int height = VirtualKeyboard::kTextureHeight;
  rgba->resize(static_cast<std::size_t>(width) * height * 4);
  std::uint8_t* pixels = rgba->data();
  auto put = [&](int x, int y, Rgb color) {
    std::uint8_t* p = pixels + (static_cast<std::size_t>(y) * width + x) * 4;
    p[0] = color.r;
    p[1] = color.g;
    p[2] = color.b;
    p[3] = 255;
  };
  auto blend = [&](int x, int y, Rgb color, std::uint8_t alpha) {
    if (x < 0 || y < 0 || x >= width || y >= height || alpha == 0) return;
    std::uint8_t* p = pixels + (static_cast<std::size_t>(y) * width + x) * 4;
    const int a = alpha;
    p[0] = static_cast<std::uint8_t>((color.r * a + p[0] * (255 - a)) / 255);
    p[1] = static_cast<std::uint8_t>((color.g * a + p[1] * (255 - a)) / 255);
    p[2] = static_cast<std::uint8_t>((color.b * a + p[2] * (255 - a)) / 255);
  };
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) put(x, y, kBackground);

  // Text field: the focused TextBox's contents and caret, scrolled so the
  // caret stays visible.
  {
    const int left = kGap / 2;
    const int right = width - kGap / 2;
    const int top = kGap / 2;
    const int bottom = static_cast<int>(std::lround(
                           VirtualKeyboard::kFieldFraction * height)) -
                       kGap / 2;
    for (int y = top; y < bottom; ++y)
      for (int x = left; x < right; ++x)
        if (InsideRoundedRect(x, y, left, top, right, bottom, kRadius))
          put(x, y, kField);
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
    const AlphaMask& text = impl_->field;
    for (int y = 0; y < text.height; ++y)
      for (int x = 0; x < text.width; ++x) {
        const int px = clip_left + x - scroll;
        if (px < clip_left || px >= clip_right) continue;
        blend(px, origin_y + y, kLabel,
              text.alpha[static_cast<std::size_t>(y) * text.width + x]);
      }
    const int caret = clip_left + caret_x - scroll;
    const int caret_top = origin_y + line_height / 8;
    const int caret_bottom = origin_y + line_height - line_height / 8;
    for (int y = caret_top; y < caret_bottom; ++y)
      for (int x = caret; x < caret + 3; ++x)
        if (x >= clip_left - 2 && x < clip_right + 2) put(x, y, kAccent);
  }

  bool labelled = true;
  const auto& keys = keyboard.keys();
  for (std::size_t index = 0; index < keys.size(); ++index) {
    const VirtualKey& key = keys[index];
    const int left = static_cast<int>(std::lround(key.u0 * width)) + kGap / 2;
    const int right = static_cast<int>(std::lround(key.u1 * width)) - kGap / 2;
    const int top = static_cast<int>(std::lround(key.v0 * height)) + kGap / 2;
    const int bottom =
        static_cast<int>(std::lround(key.v1 * height)) - kGap / 2;
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
    Rgb fill = special ? kSpecialKey : kKey;
    if (latched) fill = kAccent;
    if (hovered) fill = latched ? kAccentHover : kHover;
    if (pressed) fill = kAccent;
    for (int y = top; y < bottom; ++y)
      for (int x = left; x < right; ++x)
        if (InsideRoundedRect(x, y, left, top, right, bottom, kRadius))
          put(x, y, fill);

    const AlphaMask* mask = impl_->Mask(key.label, special);
    if (!key.label.empty() && (mask == nullptr || mask->alpha.empty())) {
      labelled = false;
      continue;
    }
    if (mask == nullptr) continue;
    const int origin_x = (left + right - mask->width) / 2;
    const int origin_y = (top + bottom - mask->height) / 2;
    for (int y = 0; y < mask->height; ++y)
      for (int x = 0; x < mask->width; ++x)
        blend(origin_x + x, origin_y + y, kLabel,
              mask->alpha[static_cast<std::size_t>(y) * mask->width + x]);
  }

  // Pointer dots, one per hand over the panel.
  for (int hand = 0; hand < 2; ++hand) {
    float u = 0.f, v = 0.f;
    if (!keyboard.cursor(hand, &u, &v)) continue;
    const int cx = static_cast<int>(std::lround(u * width));
    const int cy = static_cast<int>(std::lround(v * height));
    constexpr int kOuter = 11, kInner = 8;
    for (int y = -kOuter; y <= kOuter; ++y)
      for (int x = -kOuter; x <= kOuter; ++x) {
        const int d2 = x * x + y * y;
        if (d2 <= kInner * kInner)
          blend(cx + x, cy + y, Rgb{255, 255, 255}, 255);
        else if (d2 <= kOuter * kOuter)
          blend(cx + x, cy + y, Rgb{0, 0, 0}, 200);
      }
  }
  return labelled;
}

}  // namespace mocktail::vr
