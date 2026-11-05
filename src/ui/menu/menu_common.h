#pragma once
// Pieces shared by the in-game main menu (menu/main_menu*.cpp) and the shops (shop/shop_screen.cpp), which draw the same way: a command
// list on the 1280 x 960 canvas, textures cached by path (HD files) or by key (PS1 disc art decoded by menu/ps1_art.h), the dial needle
// resampled into one re-uploaded texture (menu/hand.h), the directional auto-repeat and the pulsing cursor colour. Header-only because
// oc_ui lists its sources explicitly.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "import/text.h"
#include "import/tim.h"
#include "render/sprite_batch.h"
#include "render/texture.h"
#include "ui/localized_path.h"
#include "ui/menu/hand.h"
#include "ui/menu/ps1_art.h"
#include "vfs/vfs.h"

namespace oc::ui::menudraw {

constexpr float kCanvasW = 1280.0f, kCanvasH = 960.0f;
// Rows of the scrolling lists (elements, equipment candidates, shop lists).
constexpr int kVisibleRows = 11;
constexpr u32 kWhite = 0xFFFFFFFFu;

inline u32 rgb(int r, int g, int b, int a = 255) {
  return render::pack_rgba(static_cast<u8>(r), static_cast<u8>(g), static_cast<u8>(b), static_cast<u8>(a));
}

// Flat colour of an innate / element colour bit (0x04 green .. 0x80 blue) for dots and chips.
inline u32 element_colour(int bit, int a = 255) {
  switch (bit) {
    case 0x04: return rgb(60, 200, 70, a);
    case 0x08: return rgb(236, 236, 236, a);
    case 0x10: return rgb(96, 96, 120, a);
    case 0x20: return rgb(232, 64, 64, a);
    case 0x40: return rgb(240, 212, 64, a);
    case 0x80: return rgb(76, 116, 244, a);
    default: return rgb(150, 150, 150, a);
  }
}

inline std::u32string utf32(std::string_view utf8) { return import::utf8_to_u32(utf8); }

inline std::string trim(std::string text) {
  const char* whitespace = " \t\r\n";
  const size_t first = text.find_first_not_of(whitespace);
  if (first == std::string::npos) return {};
  return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

// "%*d": the number right-aligned in `width` cells.
inline std::string padded_number(int value, int width) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%*d", width, value);
  return buf;
}

// Gold as the "xxx,xxx,xxx" template: thousands groups, right-aligned in 11 cells.
inline std::string grouped_gold(u32 value) {
  const int millions = static_cast<int>(value / 1000000 % 1000), thousands = static_cast<int>(value / 1000 % 1000), units = static_cast<int>(value % 1000);
  char buf[24];
  if (millions) std::snprintf(buf, sizeof buf, "%d,%03d,%03d", millions, thousands, units);
  else if (thousands) std::snprintf(buf, sizeof buf, "%d,%03d", thousands, units);
  else std::snprintf(buf, sizeof buf, "%d", units);
  return std::string(11 - std::min<size_t>(11, std::strlen(buf)), ' ') + buf;
}

// hd/menu/shapes/menu-main-NNN<suffix>.png: the HD panel shapes, NNN = the PS1 sheet frame in decimal.
inline std::string shape_path(int frame, const char* suffix) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "hd/menu/shapes/menu-main-%03d%s.png", frame, suffix);
  return buf;
}

// hd/face/<set>/NN_<stem>.png of every character id NN listed under hd/ for the portrait set ("1999" / "2021").
inline std::map<int, std::string> list_face_files(const vfs::Vfs& vfs, const std::string& set) {
  std::map<int, std::string> files;
  const std::string prefix = "face/" + set + "/";
  for (const std::string& name : vfs.list("hd")) {
    if (name.rfind(prefix, 0) != 0) continue;
    const std::string file = name.substr(prefix.size());
    if (file.size() > 3 && std::isdigit(static_cast<unsigned char>(file[0])) && std::isdigit(static_cast<unsigned char>(file[1])) && file[2] == '_')
      files[(file[0] - '0') * 10 + (file[1] - '0')] = "hd/" + name;
  }
  return files;
}

// The cursor's pulse: a 96-tick triangle wave 0x90 .. 0xF1 .. 0x90 of the red triangle's red channel.
inline int cursor_pulse(int frames) {
  const int phase = frames % 96;
  return 0x90 + (phase < 48 ? phase * 2 : (95 - phase) * 2);
}
// Colour of the cursor sprite that replaces the PS1 triangle: the triangle colour halved (flat (0x80, 0, 0) -> (0x40, 0, 0)), pulsing
// with cursor_pulse() while `active`.
inline u32 cursor_sprite_colour(bool active, int frames) {
  if (!active) return rgb(127, 0, 0);
  const int level = cursor_pulse(frames);
  const int green_blue = std::min(255, ((level - 0x80) >> 1) * 255 / 128);
  return rgb(std::min(255, (level >> 1) * 255 / 128), green_blue, green_blue);
}

struct Tex {
  render::Texture tex;
  render::TexRef set = {};
  float w = 0, h = 0;
};

// Textures of one screen: HD files by path (through Vfs::cache_key, so a mod switch loads the file again), PS1 disc art and generated
// images by key. Owns the two samplers and the 1 x 1 white texture the rectangles use. Entries that failed to load stay as null so a
// missing file is reported once.
class TexCache {
 public:
  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, std::string log_prefix) {
    ctx_ = &ctx;
    vfs_ = &vfs;
    sprites_ = &sprites;
    log_prefix_ = std::move(log_prefix);
    if (!linear_.create(ctx, true) || !nearest_.create(ctx, false)) return false;
    const u8 white_px[4] = {255, 255, 255, 255};
    if (!white_.create(ctx, 1, 1, render::TextureFormat::Rgba8, white_px)) return false;
    white_set_ = render::TexRef(white_, nearest_);
    return true;
  }
  void shutdown() {
    texs_.clear();
    white_set_ = {};
    white_.destroy();
    linear_.destroy();
    nearest_.destroy();
    ctx_ = nullptr;
    sprites_ = nullptr;
  }

  render::TexRef white_set() const { return white_set_; }
  const render::Sampler& linear() const { return linear_; }
  const render::Sampler& nearest() const { return nearest_; }

  Tex* find(const std::string& key) {
    const auto it = texs_.find(key);
    return it != texs_.end() ? it->second.get() : nullptr;
  }

  // A file of the vfs: PNG / BMP, or a PS1 TIM (palette -> RGBA, drawn nearest).
  Tex* load(const std::string& path, bool use_linear = true) {
    const std::string key = vfs_->cache_key(path);
    if (auto it = texs_.find(key); it != texs_.end()) return it->second.get();
    auto raw = vfs_->read(path);
    render::DecodedImage img;
    bool decoded = false;
    if (raw && path.ends_with(".tim")) {
      if (auto tim = import::parse_tim(*raw))
        if (auto indexed = import::decode_tim(*tim)) {
          const import::Image rgba = import::to_rgba(*indexed);
          img.width = rgba.width, img.height = rgba.height, img.rgba.assign(rgba.pixels.begin(), rgba.pixels.end());
          decoded = true;
        }
    } else if (raw) {
      decoded = render::decode_image(*raw, img);
    }
    std::unique_ptr<Tex> tex;
    if (decoded) {
      tex = create(img.width, img.height, img.rgba.data(), use_linear);
    } else {
      log::warn("{}: {} not available", log_prefix_, path);
    }
    Tex* result = tex.get();
    texs_[key] = std::move(tex);
    return result;
  }

  // PS1 disc art (menu/ps1_art.h) decoded by `make` once and kept under `key`, nearest-sampled.
  template <class MakeFn>
  Tex* ps1(const std::string& key, MakeFn&& make) {
    if (auto it = texs_.find(key); it != texs_.end()) return it->second.get();
    const ps1::Rgba image = make();
    std::unique_ptr<Tex> tex;
    if (image) tex = create(image.w, image.h, image.px.data(), false);
    else log::warn("{}: PS1 art {} not available", log_prefix_, key);
    Tex* result = tex.get();
    texs_[key] = std::move(tex);
    return result;
  }
  // The PS1 art when `ps1_art` is set and the disc has it, else the HD file.
  template <class MakeFn>
  Tex* ps1_or_hd(bool ps1_art, const std::string& key, MakeFn&& make, const std::string& hd_path) {
    if (ps1_art)
      if (Tex* disc = ps1(key, std::forward<MakeFn>(make))) return disc;
    return load(hd_path);
  }

  // A generated RGBA image under `key`: created the first time, then updated in place (same size every time). Returns the entry, or
  // null when it could not be created; `*updated` tells whether the pixels were uploaded.
  Tex* upload(const std::string& key, u32 w, u32 h, const u8* rgba, bool use_linear, bool* updated) {
    if (auto it = texs_.find(key); it != texs_.end() && it->second) {
      *updated = it->second->tex.update(*ctx_, rgba);
      return it->second.get();
    }
    std::unique_ptr<Tex> tex = create(w, h, rgba, use_linear);
    *updated = tex != nullptr;
    Tex* result = tex.get();
    if (tex) texs_[key] = std::move(tex);
    return result;
  }

  // hd/menu/shapes/menu-main-NNN[_lang].png: the language file is tried first; `large` = the "-large" 1:1 variant.
  Tex* shape(int frame, import::Lang lang, bool large = false) {
    if (!large) {
      const std::string localized = shape_path(frame, lang_suffix(lang));
      if (vfs_->exists(localized))
        if (Tex* tex = load(localized)) return tex;
    }
    return load(shape_path(frame, large ? "-large" : ""));
  }
  // Element plate menutim_14_<colour_tim>_01.png (184 x 24), or the PS1 colour-sheet frame 1 composed from menutim.prd.
  Tex* plate(int colour_tim, bool ps1_art) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "hd/menu/menutim/menutim_14_%02d_01.png", colour_tim);
    return ps1_or_hd(ps1_art, "ps1:plate" + std::to_string(colour_tim), [&] { return ps1::plate(*vfs_, colour_tim, 1); }, buf);
  }
  // The "roll" sheet (scroll arrows, side pointers): menutim_31.png is the 4x redraw of menutim.prd "roll" with the same layout.
  Tex* roll(bool ps1_art) {
    return ps1_or_hd(ps1_art, "ps1:roll", [&] { return ps1::menutim(*vfs_, "roll"); }, "hd/menu/menutim/menutim_31.png");
  }

 private:
  std::unique_ptr<Tex> create(u32 w, u32 h, const u8* rgba, bool use_linear) {
    auto tex = std::make_unique<Tex>();
    if (!tex->tex.create(*ctx_, w, h, render::TextureFormat::Rgba8, rgba)) return nullptr;
    tex->set = render::TexRef(tex->tex, use_linear ? linear_ : nearest_);
    tex->w = static_cast<float>(w);
    tex->h = static_cast<float>(h);
    return tex;
  }

  render::GpuContext* ctx_ = nullptr;
  const vfs::Vfs* vfs_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  std::string log_prefix_;
  render::Sampler linear_, nearest_;
  render::Texture white_;
  render::TexRef white_set_ = {};
  std::map<std::string, std::unique_ptr<Tex>> texs_;
};

// The dial needle: handle.png turned by the needle angle and resampled (render_hand) into ONE texture, re-uploaded in place when the
// angle changes - the eased needle passes through many angles, so a texture per angle would be wasteful. `sx` / `sy` = canvas px per PS1 px of the screen; the image is centred on the pivot with half extents half_w / half_h.
class NeedleTexture {
 public:
  struct Quad {
    const Tex* tex = nullptr;
    float half_w = 0, half_h = 0;
  };
  void reset() { valid_ = false; }
  Quad get(TexCache& cache, const vfs::Vfs& vfs, int angle, float sx, float sy) {
    Quad quad;
    if (valid_ && angle == angle_)
      if (const Tex* tex = cache.find("hand")) {
        quad.tex = tex;
        quad.half_w = half_w_, quad.half_h = half_h_;
        return quad;
      }
    if (src_.rgba.empty()) {
      auto raw = vfs.read("hd/menu/handle.png");
      if (!raw || !render::decode_image(*raw, src_)) return quad;
    }
    const HandImage image = render_hand(src_.rgba.data(), src_.width, src_.height, angle, sx, sy);
    bool updated = false;
    quad.tex = cache.upload("hand", image.w, image.h, image.rgba.data(), true, &updated);
    if (updated) angle_ = angle, valid_ = true;
    half_w_ = quad.half_w = image.half_w, half_h_ = quad.half_h = image.half_h;
    return quad;
  }

 private:
  render::DecodedImage src_;
  int angle_ = 0;
  bool valid_ = false;
  float half_w_ = 0, half_h_ = 0;
};

// Keyboard / pad auto-repeat of N held directions: fires on the press, then every 70 ms after a 350 ms delay.
template <int kDirections>
class DirectionRepeat {
 public:
  void reset() {
    for (bool& held : held_) held = false;
  }
  void poll(const bool (&down)[kDirections], bool (&fire)[kDirections]) {
    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < kDirections; ++i) {
      fire[i] = false;
      if (!down[i]) {
        held_[i] = false;
        continue;
      }
      if (!held_[i]) {
        held_[i] = true;
        hold_since_[i] = now;
        fired_[i] = 0;
        fire[i] = true;
        continue;
      }
      const double held_seconds = std::chrono::duration<double>(now - hold_since_[i]).count();
      const int due = held_seconds < 0.35 ? 0 : 1 + static_cast<int>((held_seconds - 0.35) / 0.07);
      if (due > fired_[i]) {
        fired_[i] = due;
        fire[i] = true;
      }
    }
  }

 private:
  bool held_[kDirections]{};
  std::chrono::steady_clock::time_point hold_since_[kDirections]{};
  int fired_[kDirections]{};
};

}  // namespace oc::ui::menudraw
