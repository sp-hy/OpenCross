// The Elements page's spell: a preview BattleView loaded in slices and drawn into the page's box.
#include "game/battle/spell_preview.h"

#include <cmath>

#include "core/log.h"

namespace oc::game {

void SpellPreview::configure(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::MeshPipeline& meshes,
                             const data::GameTables& tables) {
  ctx_ = &ctx, vfs_ = &vfs, sprites_ = &sprites, meshes_ = &meshes, tables_ = &tables;
}

void SpellPreview::set_art_mode(bool hd, bool force) {
  hd_ = hd;
  if (view_) view_->set_art_mode(hd, force);
}

void SpellPreview::set_language(import::Lang lang) {
  lang_ = lang;
  if (view_) view_->set_language(lang);
}

void SpellPreview::begin(int element, int caster) {
  party_ = {battle::make_party_member(*tables_, caster)};
  caster_ = caster;
  options_ = BattleView::StartOptions{};
  view_->set_preview(element);
  shown_ = element;
  running_ = false;
  loading_ = view_->begin_load(kScene, party_, options_);
  if (!loading_) log::warn("spell preview: element {} could not start its battle", element);
}

void SpellPreview::draw(const render::Frame& frame, float vw, float vh, const ui::SpellViewSlot& slot) {
  if (!ctx_ || failed_ || slot.element <= 0 || static_cast<size_t>(slot.element) >= tables_->elements.size()) return;
  if (!view_) {
    view_ = std::make_unique<BattleView>();
    if (!view_->init(*ctx_, *vfs_, *sprites_, *meshes_, *tables_, nullptr, false)) {
      log::warn("spell preview: the battle view could not be made");
      view_.reset();
      failed_ = true;
      return;
    }
    view_->set_art_mode(hd_, true);
    view_->set_language(lang_);
    view_->warm_up();
  }
  if (slot.element != wanted_) {
    wanted_ = slot.element;
    if (running_ && slot.caster == caster_ && view_->preview_recast(wanted_)) shown_ = wanted_;
    else begin(wanted_, slot.caster);
  }
  if (loading_ && view_->load_step(kLoadBudgetMs)) {
    loading_ = false;
    running_ = view_->start(kScene, party_, options_);
    if (!running_) log::warn("spell preview: element {} could not start its battle", shown_);
  }
  const bool playing = running_ && wanted_ == shown_;
  if (playing) view_->update(slot.dt, platform::Input{});
  const float to_x = static_cast<float>(frame.extent.width) / vw, to_y = static_cast<float>(frame.extent.height) / vh;
  const float x0 = std::round((slot.origin_x + slot.box_x * slot.unit_x) * to_x), y0 = std::round((slot.origin_y + slot.box_y * slot.unit_y) * to_y);
  const float x1 = std::round((slot.origin_x + (slot.box_x + slot.box_w) * slot.unit_x) * to_x);
  const float y1 = std::round((slot.origin_y + (slot.box_y + slot.box_h) * slot.unit_y) * to_y);
  if (x1 <= x0 || y1 <= y0) return;
  view_->draw_preview(frame, {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)}, playing);
}

}  // namespace oc::game
