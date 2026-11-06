#include "game/effects/gpu_effects.h"
#include "core/log.h"

#include <algorithm>
#include <chrono>

namespace oc::effects {

bool sheet_atlas_image(const assets::SpriteSheet& sheet, u32& w, u32& h, std::vector<u8>& img) {
  w = 1;
  h = 0;
  for (const auto& frame : sheet.frames)
    for (const auto& sprite : frame) {
      w = std::max<u32>(w, sprite.w);
      h += sprite.h;
    }
  if (h == 0) return false;
  img.assign(static_cast<size_t>(w) * h * 4, 0);
  u32 y = 0;
  for (const auto& frame : sheet.frames)
    for (const auto& sprite : frame) {
      for (u32 j = 0; j < sprite.h; ++j)
        std::copy_n(sprite.rgba.begin() + static_cast<std::ptrdiff_t>(j) * sprite.w * 4, static_cast<std::ptrdiff_t>(sprite.w) * 4,
                    img.begin() + (static_cast<std::ptrdiff_t>(y + j) * w) * 4);
      y += sprite.h;
    }
  return true;
}

namespace {

constexpr u32 kHdScale = 4;

bool has_hd_sprite(const assets::SpriteSheet& sheet) {
  for (const auto& frame : sheet.frames)
    for (const auto& sprite : frame)
      if (!sprite.hd_rgba.empty()) return true;
  return false;
}

// The sheet_atlas_image layout at 4x: each sprite's HD image, or its 1x texels repeated 4 x 4.
void hd_atlas_image(const assets::SpriteSheet& sheet, u32 w, u32 h, std::vector<u8>& img) {
  const u32 atlas_w = w * kHdScale;
  img.assign(static_cast<size_t>(atlas_w) * h * kHdScale * 4, 0);
  u32 y = 0;
  for (const auto& frame : sheet.frames)
    for (const auto& sprite : frame) {
      const u32 sprite_w = sprite.w * kHdScale;
      for (u32 j = 0; j < sprite.h * kHdScale; ++j)
        for (u32 i = 0; i < sprite_w; ++i) {
          const u8* src = sprite.hd_rgba.empty() ? &sprite.rgba[(static_cast<size_t>(j / kHdScale) * sprite.w + i / kHdScale) * 4]
                                                 : &sprite.hd_rgba[(static_cast<size_t>(j) * sprite_w + i) * 4];
          std::copy_n(src, 4, &img[(static_cast<size_t>(y * kHdScale + j) * atlas_w + i) * 4]);
        }
      y += sprite.h;
    }
}

}  // namespace

render::TextureKey effect_sheet_key(const assets::SpriteSheet& sheet, u32 w, u32 h, const u8* rgba) {
  return {"effects", sheet.name.empty() ? "sheet" : sheet.name, -1, render::texture_content_tag(w, h, rgba)};
}

bool GpuEffects::init(render::GpuContext& ctx, render::SpriteBatch& sprites) {
  ctx_ = &ctx;
  sprites_ = &sprites;
  return sampler_.create(ctx, /*linear=*/false) && sampler_lin_.create(ctx, /*linear=*/true);
}

void GpuEffects::clear() {
  atlases_.clear();
  entries_.clear();
  white_.reset();
}

bool GpuEffects::load(const EffectBank& bank, Which which) {
  if (!load_begin()) return false;
  bool done = false;
  while (!done)
    if (!load_more(bank, which, 1e9, done)) return false;
  return true;
}

bool GpuEffects::load_begin() {
  if (!ctx_ || !sprites_) return false;
  clear();
  cur_pack_ = cur_sheet_ = 0;
  auto white = std::make_unique<Atlas>();
  const u8 pixel[4] = {255, 255, 255, 255};
  if (white->tex.create(*ctx_, 1, 1, render::TextureFormat::Rgba8, pixel)) white->set = render::TexRef(white->tex, sampler_);
  if (white->set) white_ = std::move(white);
  return true;
}

// All the atlases of one call share one upload batch (one queue wait). The sprite UVs are fractions of the original atlas, so a
// texture-registry replacement of any size (the same frame layout scaled uniformly) lines up; the registry is only consulted when a
// dump or a replacement can happen, since its content tag hashes the pixels.
bool GpuEffects::load_more(const EffectBank& bank, Which which, double budget_ms, bool& done) {
  const auto started = std::chrono::steady_clock::now();
  done = false;
  render::UploadBatch batch;
  for (; cur_pack_ < bank.pack_count(); ++cur_pack_, cur_sheet_ = 0) {
    const size_t pack_index = cur_pack_;
    if (which == Which::LibrariesOnly && !bank.is_library(pack_index)) continue;
    if (which == Which::SkipLibraries && bank.is_library(pack_index)) continue;
    const auto& sheets = bank.pack(pack_index).sheets;
    for (; cur_sheet_ < sheets.size(); ++cur_sheet_) {
      if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() >= budget_ms) {
        batch.flush(*ctx_);
        return true;
      }
      const auto& sheet = sheets[cur_sheet_];
      u32 w = 1, h = 0;
      std::vector<u8> img;
      if (!sheet_atlas_image(sheet, w, h, img)) continue;
      auto atlas = std::make_unique<Atlas>();
      std::vector<std::pair<const assets::EffectSprite*, Entry>> pending;
      u32 y = 0;
      for (const auto& frame : sheet.frames)
        for (const auto& sprite : frame) {
          Entry entry;
          entry.u0 = 0.0f;
          entry.u1 = static_cast<float>(sprite.w) / static_cast<float>(w);
          entry.v0 = static_cast<float>(y) / static_cast<float>(h);
          entry.v1 = static_cast<float>(y + sprite.h) / static_cast<float>(h);
          pending.emplace_back(&sprite, entry);
          y += sprite.h;
        }
      u32 upload_w = w, upload_h = h;
      bool replaced = false;
      if (render::texture_registry::active()) {
        const render::TextureKey key = effect_sheet_key(sheet, w, h, img.data());
        render::texture_registry::dump(key, w, h, img.data());
        replaced = render::texture_registry::replace(key, upload_w, upload_h, img);
      }
      if (hd_ && !replaced && has_hd_sprite(sheet)) {
        hd_atlas_image(sheet, w, h, img);
        upload_w = w * kHdScale, upload_h = h * kHdScale;
      }
      if (!atlas->tex.create(*ctx_, upload_w, upload_h, render::TextureFormat::Rgba8, img.data(), &batch)) {
        log::warn("effects: sheet atlas {}x{} not created", w, h);
        return false;
      }
      atlas->set = render::TexRef(atlas->tex, sampler_);
      if (!atlas->set) {
        log::warn("effects: no texture for the sheet atlas {}x{}", w, h);
        return false;
      }
      atlas->set_lin = render::TexRef(atlas->tex, sampler_lin_);
      for (auto& [sprite, entry] : pending) {
        entry.set = atlas->set;
        entry.set_lin = atlas->set_lin;
        entries_[sprite] = entry;
      }
      atlases_.push_back(std::move(atlas));
    }
  }
  batch.flush(*ctx_);
  done = true;
  return true;
}

void GpuEffects::draw(render::ParticleBatch& batch, const std::vector<DrawQuad>& quads, const render::Letterbox& letterbox) const {
  for (const DrawQuad& quad : quads) draw_quad(batch, quad, letterbox);
}

void GpuEffects::draw_quad(render::ParticleBatch& batch, const DrawQuad& quad, const render::Letterbox& letterbox) const {
  if (quad.untextured) {
    if (!white_) return;
    float x[4], y[4];
    for (int k = 0; k < 4; ++k) {
      x[k] = letterbox.ox + quad.x[k] * letterbox.scale;
      y[k] = letterbox.oy + quad.y[k] * letterbox.scale_y;
    }
    const float u[4] = {0, 1, 0, 1}, v[4] = {0, 0, 1, 1};
    if (quad.untextured == 2) {
      const u8 colours[4][3] = {{static_cast<u8>(quad.r / 2), static_cast<u8>(quad.g / 2), static_cast<u8>(quad.b / 2)},
                                {static_cast<u8>(quad.r1 / 2), static_cast<u8>(quad.g1 / 2), static_cast<u8>(quad.b1 / 2)},
                                {static_cast<u8>(quad.r / 2), static_cast<u8>(quad.g / 2), static_cast<u8>(quad.b / 2)},
                                {static_cast<u8>(quad.r1 / 2), static_cast<u8>(quad.g1 / 2), static_cast<u8>(quad.b1 / 2)}};
      batch.quad(white_->set, quad.blend, x, y, u, v, colours, static_cast<float>(quad.slot) / render::kDepthRange);
      return;
    }
    batch.quad(white_->set, quad.blend, x, y, u, v, static_cast<u8>(quad.r / 2), static_cast<u8>(quad.g / 2), static_cast<u8>(quad.b / 2),
               static_cast<float>(quad.slot) / render::kDepthRange);
    return;
  }
  auto found = entries_.find(quad.sprite);
  if (found == entries_.end()) {
    if (shared_) shared_->draw_quad(batch, quad, letterbox);
    return;
  }
  const Entry& entry = found->second;
  float x[4], y[4];
  for (int k = 0; k < 4; ++k) {
    x[k] = letterbox.ox + quad.x[k] * letterbox.scale;
    y[k] = letterbox.oy + quad.y[k] * letterbox.scale_y;
  }
  const float u[4] = {entry.u0, entry.u1, entry.u0, entry.u1}, v[4] = {entry.v0, entry.v0, entry.v1, entry.v1};
  batch.quad(linear_ && entry.set_lin ? entry.set_lin : entry.set, quad.blend, x, y, u, v, quad.r, quad.g, quad.b,
             static_cast<float>(quad.slot) / render::kDepthRange);
}

}  // namespace oc::effects
