#include "ui/feature_icons.h"

#include <string>

#include "core/log.h"
#include "render/camera.h"

namespace oc::ui {

namespace {
constexpr float kCanvasW = 1280.0f, kCanvasH = 960.0f;
constexpr float kSize = 53.0f, kStep = 63.0f, kLeft = 20.0f, kBottom = kCanvasH - 83.0f;
constexpr float kBarShift = -93.0f, kBarNeed = 150.0f;
constexpr float kStripX = 83.0f, kStripW = 212.0f;
// Atlas cell size and the inset that keeps the bilinear taps inside a cell.
constexpr float kCell = 0.25f, kInset = 0.001f;
}  // namespace

bool FeatureIcons::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites) {
  ctx_ = &ctx, vfs_ = &vfs, sprites_ = &sprites;
  return linear_.create(ctx, true);
}

void FeatureIcons::shutdown() {
  if (!ctx_) return;
  atlas_set_ = {};
  atlas_.destroy();
  linear_.destroy();
  ctx_ = nullptr;
}

void FeatureIcons::load(import::Lang lang) {
  loaded_lang_ = static_cast<int>(lang);
  atlas_set_ = {};
  atlas_.destroy();
  const std::string name = lang == import::Lang::Jp ? "ui/exticon.png" : std::string("ui/exticon_") + import::lang_code(lang) + ".png";
  render::DecodedImage img;
  for (const std::string& path : {name, std::string("ui/exticon.png")}) {
    if (auto raw = vfs_->read(path); raw && render::decode_image(*raw, img) &&
                                     atlas_.create(*ctx_, img.width, img.height, render::TextureFormat::Rgba8, img.rgba.data())) {
      atlas_set_ = render::TexRef(atlas_, linear_);
      return;
    }
  }
  log::warn("feature icons: {} unavailable", name);
}

void FeatureIcons::draw(const render::Frame& frame, float vw, float vh, const Active& active, import::Lang lang) {
  const auto now = std::chrono::steady_clock::now();
  const double frames = last_ == std::chrono::steady_clock::time_point{} ? 1.0 : std::chrono::duration<double>(now - last_).count() * 60.0;
  last_ = now;
  shown_ = 0;
  if (!ctx_) return;
  const bool any = active.auto_battle || active.boost || active.encounters || active.slow || active.fast;
  if (!any && (autosave_ <= 0.0 || (autosave_ -= frames) <= 0.0)) return;
  if (loaded_lang_ != static_cast<int>(lang)) load(lang);
  if (!atlas_set_) return;
  const render::CanvasFit fit = render::canvas_fit(vw, vh, kCanvasW, kCanvasH);
  const float shift = vw * kCanvasH > (kCanvasW + kBarNeed) * vh ? kBarShift : 0.0f, left = kLeft + shift;
  // Atlas cell (column, row) per icon, bottom of the column first.
  const struct {
    bool on;
    int col, row;
  } icons[] = {{active.auto_battle, 1, 0}, {active.boost, 2, 0}, {active.encounters, 0, 0}, {active.slow, 1, 2}, {active.fast, 0, 2}};
  sprites_->begin(frame, vw, vh, true);
  float top = kBottom - kSize;
  for (const auto& icon : icons) {
    if (!icon.on) continue;
    const float u = static_cast<float>(icon.col) * kCell, v = static_cast<float>(icon.row) * kCell;
    sprites_->draw(atlas_set_, fit.ox + left * fit.s, fit.oy + top * fit.s, kSize * fit.s, kSize * fit.s, u + kInset, v + kInset,
                   u + kCell - kInset, v + kCell - kInset);
    top -= kStep;
    ++shown_;
  }
  if (autosave_ > 0.0 && (autosave_ -= frames) > 0.0)
    sprites_->draw(atlas_set_, fit.ox + (kStripX + shift) * fit.s, fit.oy + (kBottom - kSize) * fit.s, kStripW * fit.s, kSize * fit.s, 0.002f,
                   kCell + 0.002f, 0.998f, 2.0f * kCell - 0.002f, render::pack_rgba(255, 255, 255, 204));
  sprites_->end();
}

}  // namespace oc::ui
