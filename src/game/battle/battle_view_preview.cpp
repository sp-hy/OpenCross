// BattleView spell preview (the OpenCross menu's Elements page): a battle without its script whose lead PC casts one element over and
// over, drawn as its effects alone (no stage, units or HUD) into a box of the window.
#include "game/battle/battle_view.h"

#include <algorithm>

namespace oc::game {

void BattleView::set_preview(int element) {
  preview_element_ = std::max(0, element);
  preview_wait_ = 0;
  preview_busy_ = true;
  if (preview_element_ == 0) return;
  dbg_ = Debug{};
  dbg_.cast_element = preview_element_;
  dbg_.no_script = true;
  dbg_.inv_party = dbg_.inv_enemies = true;
}

bool BattleView::preview_recast(int element) {
  if (preview_element_ == 0 || element <= 0 || !battle_ || preview_busy_) return false;
  preview_element_ = element;
  preview_wait_ = 0;
  dbg_.cast_element = element;
  return true;
}

// The next cast is armed once nothing has been presented for kPreviewGap seconds.
void BattleView::preview_tick(bool busy, float dt) {
  preview_busy_ = busy || fx_ || ui_ == Ui::Intro;
  if (dbg_.cast_element > 0) return;
  if (preview_busy_) {
    preview_wait_ = 0;
    return;
  }
  preview_wait_ += dt;
  if (preview_wait_ < kPreviewGap) return;
  preview_wait_ = 0;
  dbg_.cast_element = preview_element_;
}

void BattleView::draw_preview(const render::Frame& frame, const render::Rect& rect, bool scene) {
  if (rect.w == 0 || rect.h == 0) return;
  const float x = static_cast<float>(rect.x), y = static_cast<float>(rect.y), w = static_cast<float>(rect.w), h = static_cast<float>(rect.h);
  if (scene && (preview_rt_.width() != rect.w || preview_rt_.height() != rect.h) && !preview_rt_.create(*ctx_, rect.w, rect.h, true)) scene = false;
  if (scene) {
    prepare_fx_targets(rect.w, rect.h);
    render::Letterbox lb;
    lb.vw = w, lb.vh = h;
    lb.scale_y = h / kBufferRows;
    lb.scale = lb.scale_y * render::kFieldRows / render::kPresentRows;
    lb.ox = 0.5f * w - kGeomOffsetX * lb.scale;
    lb.x0 = -lb.ox / lb.scale;
    lb.w = w / lb.scale;
    render::Frame out;
    const float black[4] = {0, 0, 0, 1};
    ctx_->begin_target(preview_rt_, frame, out, black);
    draw_scene(out, lb, true);
    ctx_->end_target(preview_rt_);
  }
  sprites_->begin(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), true);
  sprites_->draw(white_.set, x, y, w, h, 0, 0, 1, 1, 0xFF000000u);
  if (scene) sprites_->draw(preview_rt_.ref(linear_), x, y, w, h);
  sprites_->end();
}

}  // namespace oc::game
