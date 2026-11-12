// BattleView rendering: HUD op replay and the scene / UI draw passes.
#include "game/battle/battle_view.h"
#include "game/free_cam.h"
#include "game/encounter_strips.h"
#include "game/battle/battle_view_util.h"
#include "game/battle/battle_view_op.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "import/text.h"
namespace oc::game {

using namespace bv;

namespace {

// Prism weapon lighting (GTE NCCS, no specular). Three light rows turn every tick by angles x += 0x20, z += 0x40 of 4096
// (RotMatrixZYX applied to (0, 1, 0), (-0.707, -0.707, 0), (0.707, -0.707, 0)); the light matrix is the weapon's PS1 world rotation W times
// those rows and it lights the NEGATED vertex normal; the colour matrix sends light 0 to blue, 1 to green, 2 to red at 0.75; back colour 0.75.
// `world` maps the weapon mesh to this y-up world (W = world with its y row negated); the shader dots dir_i with world * normal, so
// dir_i = -world^-T * (W L)_i, and world^-T = world / scale^2 for a uniformly scaled rotation.
void prism_weapon_light(const render::Mat4& world, float ticks, render::MeshPipeline::DrawParams& wd) {
  constexpr float kTurn = 360.0f / 4096.0f, kDiag = 2896.0f / 4096.0f;
  constexpr float kBase[3][3] = {{0.0f, 1.0f, 0.0f}, {-kDiag, -kDiag, 0.0f}, {kDiag, -kDiag, 0.0f}};
  const assets::Mat3 spin = assets::rotation_zyx({std::fmod(32.0f * ticks, 4096.0f) * kTurn, 0.0f, std::fmod(64.0f * ticks, 4096.0f) * kTurn});
  float rows[3][3];
  for (size_t j = 0; j < 3; ++j)
    for (size_t k = 0; k < 3; ++k) rows[j][k] = spin[k * 3] * kBase[j][0] + spin[k * 3 + 1] * kBase[j][1] + spin[k * 3 + 2] * kBase[j][2];
  const float scale2 = world.at(0, 0) * world.at(0, 0) + world.at(1, 0) * world.at(1, 0) + world.at(2, 0) * world.at(2, 0);
  if (scale2 <= 0.0f) return;
  wd.light.on = true;
  wd.light.spec = false;
  for (int i = 0; i < 3; ++i) {
    float llm[3];
    for (size_t k = 0; k < 3; ++k) {
      llm[k] = 0.0f;
      for (int j = 0; j < 3; ++j) llm[k] += (i == 1 ? -world.at(i, j) : world.at(i, j)) * rows[j][k];
    }
    for (int r = 0; r < 3; ++r)
      wd.light.dir[i][r] = -(world.at(r, 0) * llm[0] + world.at(r, 1) * llm[1] + world.at(r, 2) * llm[2]) / scale2;
    wd.light.bk[i] = 0.75f;
    for (size_t c = 0; c < 3; ++c) wd.light.rgb[i][c] = c == static_cast<size_t>(2 - i) ? 0.75f : 0.0f;
  }
}

}  // namespace

void BattleView::replay(const render::Frame&, const render::Letterbox& lb, float alpha) {
  // Canvas unit -> pixels (1280 canvas = 4 x the 320 layer width).
  const float canvas_scale = lb.scale / 4.0f;
  // The battle picture's rectangle in the view: the window's field frame, or in the PS1-resolution pass rows 2..238 of the 240-row target.
  auto picture_rect = [&]() {
    if (!ui_pass_) return picture(lb.vw, lb.vh);
    render::Letterbox rect = lb;
    rect.oy += render::kFieldCrop * rect.scale;
    rect.scale_y = rect.scale * (render::kLayerH - 2.0f * render::kFieldCrop) / render::kFieldRows;
    return rect;
  };
  auto to_x = [&](float x) { return lb.ox + x * canvas_scale; };
  auto to_y = [&](float y) { return lb.oy + y * canvas_scale; };
  // Screen fade (battle scripts 0x2B / 0x2C, run-away, game over): over the scene, under the HUD.
  const float fa = camera_.fade_alpha(alpha);
  if (fa > 0.003f) {
    const auto rgb = camera_.fade_rgb();
    // Over the battle picture (widescreen: all its visible columns).
    const render::Letterbox pic = picture_rect();
    sprites_->draw(white_.set, pic.left(), pic.oy, pic.width(), kBufferRows * pic.scale_y, 0, 0, 1, 1,
                   col(rgb[0], rgb[1], rgb[2], static_cast<u8>(255 * std::clamp(fa, 0.0f, 1.0f))));
  }
  // [Video] WideHudEdges: in widescreen the left group sits at the window's left edge, the right group at the right edge (the 4:3 canvas
  // shifts by the picture's extra columns, 4 canvas units per column).
  // [Video] WideHud: default 0, 16:9 = min(e, 49), full = e.
  const float edge_dx = wide_hud_cols_ * 4.0f;
  for (const Op& op : ops_) {
    const float dx = op.grp == 2 || edge_dx <= 0.0f ? 0.0f : static_cast<float>(op.grp) * edge_dx + op.shift;
    switch (op.kind) {
      case Op::Rect:
        // A full-width screen (game over): widescreen = the picture's whole width.
        if (op.x <= 0.0f && op.x + op.w >= kCanvasW) {
          const render::Letterbox pic = picture_rect();
          const float left = std::min(to_x(op.x + dx), pic.left()), right = std::max(to_x(op.x + dx + op.w), pic.left() + pic.width());
          sprites_->draw(white_.set, left, to_y(op.y), right - left, op.h * canvas_scale, 0, 0, 1, 1, op.color);
          break;
        }
        sprites_->draw(white_.set, to_x(op.x + dx), to_y(op.y), op.w * canvas_scale, op.h * canvas_scale, 0, 0, 1, 1, op.color);
        break;
      case Op::Img:
        // Blend 3 = PS1 ABR 2 (B - F).
        if (op.blend) sprites_->set_blend(op.blend == 1 ? render::BlendMode::Multiply : op.blend == 3 ? render::BlendMode::Subtract : render::BlendMode::Add);
        sprites_->draw(op.tex->set, to_x(op.x + dx), to_y(op.y), op.w * canvas_scale, op.h * canvas_scale, op.u0, op.v0, op.u1, op.v1, op.color);
        if (op.blend) sprites_->set_blend(render::BlendMode::Alpha);
        break;
      case Op::Plate: {
        const Tex& plate = plates_[static_cast<size_t>(std::clamp(op.plate, 0, 7))];
        // menuelem_N.png, drawn in both art modes.
        if (plate.set) sprites_->draw(plate.set, to_x(op.x + dx), to_y(op.y), op.w * canvas_scale, op.h * canvas_scale, 0, 0, 1, 1, op.color);
        break;
      }
      case Op::Panel:
        // battle-win4-large, both art modes.
        if (win_base_.set && win_frame_.set) {
          // Plain stretch of the subtractive base plate (as the element description window draws it), then the 9-sliced frame.
          sprites_->set_blend(render::BlendMode::Subtract);
          sprites_->draw(win_base_.set, to_x(op.x + dx), to_y(op.y), op.w * canvas_scale, op.h * canvas_scale, 0, 0, 1, 1, 0xFFFFFFFFu);
          sprites_->set_blend(render::BlendMode::Alpha);
          nine_slice(win_frame_.set, win_frame_.w, win_frame_.h, to_x(op.x + dx), to_y(op.y), op.w * canvas_scale, op.h * canvas_scale, 28.0f, 14.0f * canvas_scale, 0xFFFFFFFFu);
        } else {
          sprites_->draw(white_.set, to_x(op.x + dx), to_y(op.y), op.w * canvas_scale, op.h * canvas_scale, 0, 0, 1, 1, col(16, 20, 90, 225));
          const float border = 4 * canvas_scale;
          const u32 edge = col(225, 225, 245);
          sprites_->draw(white_.set, to_x(op.x + dx), to_y(op.y), op.w * canvas_scale, border, 0, 0, 1, 1, edge);
          sprites_->draw(white_.set, to_x(op.x + dx), to_y(op.y + op.h) - border, op.w * canvas_scale, border, 0, 0, 1, 1, edge);
          sprites_->draw(white_.set, to_x(op.x + dx), to_y(op.y), border, op.h * canvas_scale, 0, 0, 1, 1, edge);
          sprites_->draw(white_.set, to_x(op.x + dx + op.w) - border, to_y(op.y), border, op.h * canvas_scale, 0, 0, 1, 1, edge);
        }
        break;
      case Op::Text:  // canvas -> FontSet pen units (canvas_scale view px per canvas unit, letterbox origin folded into the pen)
        fonts_.draw(op.slot, op.text, op.x + dx + lb.ox / canvas_scale, op.y + lb.oy / canvas_scale, op.ink, op.scale, op.align, canvas_scale);
        break;
    }
  }

  // Damage / heal / miss numbers (particles, battle_numbers.h): every digit is a view-space sprite at
  // (x, y, z + H << 5) projected with the live H about the projection centre (cam_.ox / oy, incl. the screen shake), its numf quad
  // (x, y, w, 15) << 5 scaled and turned by the particle's own matrix (Rx Ry Rz), then put into the picture as Cam::project does.
  // The quad is drawn as its screen bounding box (the in-plane z turn of gurv / dam2 is not drawn).
  {
    const float focal = std::max(1.0f, cam_.h);
    auto canvas_x = [](float sx) { return sx * (kCanvasW / render::kLayerW); };
    auto canvas_y = [](float sy) { return (render::kFieldCrop + sy * (render::kLayerH - 2.0f * render::kFieldCrop) / kBufferRows) * (kCanvasH / render::kLayerH); };
    const bool sheet = hud_[kHDigits].set && digit_x_.size() == 11;
    for (const Pop& pop : pops_) {
      // Debug free camera, "Hide overlays": no damage digits either.
      if (free_cam().hide_overlays()) break;
      const float age = lerp(pop.age_prev, pop.age, alpha);
      const int t0 = static_cast<int>(std::floor(age));
      const float fr = age - static_cast<float>(t0);
      const bnum::Flight& fl = bnum::flight_for_code(pop.code);
      // numf frame: digit + 1, 11 = "miss!".
      std::vector<int> frames;
      if (pop.kind == 3) frames.push_back(11);
      else
        for (char ch : std::to_string(std::max(0, pop.value))) frames.push_back(ch - '0' + 1);
      const int digit_count = static_cast<int>(frames.size());
      float all_x0 = 1e9f, all_x1 = -1e9f, all_y0 = 1e9f;
      u32 all_c = 0;
      for (int kk = 0; kk < digit_count; ++kk) {
        // Spawn order: the units digit first, at 192 n, each next one 384 to the left.
        const int digit_pos = digit_count - 1 - kk;
        const float x0 = static_cast<float>((pop.kind == 3 ? 0 : 192 * digit_count - 384 * digit_pos) + (pop.ax << bnum::kShift));
        const float y0 = static_cast<float>(pop.ay << bnum::kShift);
        const int delay = pop.kind == 3 ? pop.delay : 4 * (kk + 1) + pop.delay;
        const bnum::State from = bnum::digit_state(fl, x0, y0, delay, t0);
        if (!from.visible) continue;
        bnum::State to = bnum::digit_state(fl, x0, y0, delay, t0 + 1);
        if (!to.visible) to = from;
        bnum::State state = from;
        state.x = lerp(from.x, to.x, fr);
        state.y = lerp(from.y, to.y, fr);
        state.z = lerp(from.z, to.z, fr);
        for (size_t k = 0; k < 3; ++k) {
          state.scale[k] = lerp(from.scale[k], to.scale[k], fr);
          state.col[k] = lerp(from.col[k], to.col[k], fr);
          state.ang[k] = lerp(from.ang[k], to.ang[k], fr);
        }
        const bnum::Glyph& glyph = bnum::numf_glyph(frames[static_cast<size_t>(kk)]);
        float cos_ang[3], sin_ang[3];
        for (size_t k = 0; k < 3; ++k) {
          const float radians = state.ang[k] * (2.0f * kPi / 4096.0f);
          cos_ang[k] = std::cos(radians);
          sin_ang[k] = std::sin(radians);
        }
        float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
        bool ok = true;
        for (int corner = 0; corner < 4; ++corner) {
          const float qx = static_cast<float>((glyph.x + ((corner & 1) ? glyph.w : 0)) << bnum::kShift) * state.scale[0] / 4096.0f;
          const float qy = static_cast<float>((glyph.y + ((corner & 2) ? 15 : 0)) << bnum::kShift) * state.scale[1] / 4096.0f;
          // R = Rx * Ry * Rz applied to (qx, qy, 0): Rz first, then Ry (z in = 0), then Rx.
          const float zx = qx * cos_ang[2] - qy * sin_ang[2], zy = qx * sin_ang[2] + qy * cos_ang[2];
          const float yx = zx * cos_ang[1], yz = -zx * sin_ang[1];
          const float rx = yx, ry = zy * cos_ang[0] - yz * sin_ang[0], rz = zy * sin_ang[0] + yz * cos_ang[0];
          const float zc = focal * static_cast<float>(1 << bnum::kShift) + state.z + rz;
          if (zc < 1.0f) {
            ok = false;
            break;
          }
          const float sx = cam_.ox + focal * (state.x + rx) / zc, sy = cam_.oy + focal * (state.y + ry) / zc;
          bx0 = std::min(bx0, sx);
          bx1 = std::max(bx1, sx);
          by0 = std::min(by0, sy);
          by1 = std::max(by1, sy);
        }
        if (!ok) continue;
        // texel x (col >> 4) / 0x80: the 12-bit colour, 2048 = neutral (damv ends white, gurv red, gaiv green)
        auto cb = [&](size_t channel) { return static_cast<u8>(std::clamp(state.col[channel] / 2048.0f, 0.0f, 1.0f) * 255.0f); };
        const u32 digit_colour = col(cb(0), cb(1), cb(2), 255);
        all_x0 = std::min(all_x0, bx0);
        all_x1 = std::max(all_x1, bx1);
        all_y0 = std::min(all_y0, by0);
        all_c = digit_colour;
        if (!sheet) continue;
        // The "numf" digit sheet with HD texels (battle_33[_lang].tim, both art modes): the HD glyph run (4 texels per PS1 px) centred in the
        // PS1 quad, at the quad's scale.
        const size_t gi = static_cast<size_t>(frames[static_cast<size_t>(kk)] - 1);
        const float tw = hud_[kHDigits].w;
        const int u0 = digit_x_[gi].first - 1, u1 = digit_x_[gi].second + 1;
        const float fx = (bx1 - bx0) / static_cast<float>(glyph.w);
        const float run = static_cast<float>(u1 - u0 + 1) * 0.25f * fx;
        const float mid = 0.5f * (bx0 + bx1);
        const float cx0 = canvas_x(mid - 0.5f * run), cx1 = canvas_x(mid + 0.5f * run), cy0 = canvas_y(by0), cy1 = canvas_y(by1);
        auto quad = [&]() {
          sprites_->draw(hud_[kHDigits].set, to_x(cx0), to_y(cy0), (cx1 - cx0) * canvas_scale, (cy1 - cy0) * canvas_scale, static_cast<float>(u0) / tw, 0, static_cast<float>(u1 + 1) / tw, 1, digit_colour);
        };
        // Lighten: the frame twice, B - F drawn first, then B + F = max(B, F).
        if (state.lighten) {
          sprites_->set_blend(render::BlendMode::Subtract);
          quad();
          sprites_->set_blend(render::BlendMode::Add);
          quad();
          sprites_->set_blend(render::BlendMode::Alpha);
        } else {
          quad();
        }
      }
      if (sheet || all_x1 < all_x0) continue;
      // no numf sheet in the data: the battle font (only when hd.dat lacks battle_33)
      const std::u32string label = pop.kind == 3 ? U"Miss" : import::utf8_to_u32(std::to_string(pop.value));
      const ui::font::Ink ink{all_c, col(33, 33, 33, 255), true, false};
      fonts_.draw(ui::font::kBattle, label, canvas_x(0.5f * (all_x0 + all_x1)) + lb.ox / canvas_scale, canvas_y(all_y0) + lb.oy / canvas_scale, ink, 0.7f, ui::font::kCentre, canvas_scale);
    }
  }

}

void BattleView::draw(const render::Frame& frame, bool sprites_resume) {
  if (!scene_visible()) return;
  draw_scene(frame, picture(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height)), sprites_resume);
  // Resumes the depth-cue pass of this frame if there was one (fog_pass_), else starts the sprite buffer.
  draw_ui(frame, sprites_resume);
}

void BattleView::draw_scene(const render::Frame& frame, const render::Letterbox& lb, bool sprites_resume) {
  if (!scene_visible()) return;
  fx_particle_pass_ = false;
  // Before the warp and cross-fade targets: both draw the scene from this one mask.
  collect_shadow_casters();
  cast_shadow_pass(frame);
  // Effect screen warp (ops 0x3E-0x40): the scene into a target, then copied + warped.
  if (fx_warp_begin(frame)) {
    draw_scene_3d(fx_warp_frame_, lb, sprites_resume);
    fx_warp_end(frame, lb);
  } else if (!fx_cross_draw(frame, lb, sprites_resume)) {
    draw_scene_3d(frame, lb, sprites_resume);
  }
  fx_tint_draw(frame, lb);
}

void BattleView::draw_scene_3d(const render::Frame& frame, const render::Letterbox& lb, bool sprites_resume) {
  // The 320 x 224 draw area (widescreen: its visible columns lb.x0 .. lb.x0 + lb.w), clamped to the frame.
  const float fw = static_cast<float>(frame.extent.width), fh = static_cast<float>(frame.extent.height);
  const float x0 = std::clamp(std::round(lb.left()), 0.0f, fw), y0 = std::clamp(std::round(lb.oy), 0.0f, fh);
  const render::Rect scissor{static_cast<int32_t>(x0), static_cast<int32_t>(y0),
                             static_cast<u32>(std::min(fw - x0, std::round(lb.width()))), static_cast<u32>(std::min(fh - y0, std::round(kBufferRows * lb.scale_y)))};
  apply_camera(alpha_);
  const render::Mat4 vp = cam_.view_proj(lb), view = cam_.view();

  fx_draw_fog(frame, lb, true, sprites_resume);
  if (stage_scene_.ready()) {
    {
      // GTE depth cue of the stage objects (effects op 0x2D ramps it; the fog flag is set by ops 0x24 / 0x6C).
      render::StagePipeline::DepthCue cue;
      cue.r = fog_cur_.r, cue.g = fog_cur_.g, cue.b = fog_cur_.b, cue.near = fog_cur_.near, cue.far = fog_cur_.far;
      cue.active = fx_fog_on_;
      stage_scene_.set_depth_cue(cue);
    }
    stage_scene_.draw(stage_pipe_, frame, scissor, fx_view(), lb, alpha_, linear_filter_ < 0 ? hd_ : linear_filter_ != 0);
  }
  // Otherwise the stage objects depth-cue per packet (StageScene::set_depth_cue).
  if (!stage_scene_.ready()) fx_draw_fog(frame, lb, false, sprites_resume);

  meshes_->begin(frame, scissor);
  for (int i = 0; i < battle::kUnits; ++i) {
    const battle::Unit& unit = bunit(i);
    const Visual& visual = vis_[static_cast<size_t>(i)];
    if (showcase_on_ || !unit.present || !visual.has_model || !visual.model->gpu) continue;
    const float fade = lerp(visual.fade_prev, visual.fade, alpha_), flash = lerp(visual.flash_prev, visual.flash, alpha_);
    if (!unit.pc && fade <= 0.0f) continue;
    // unplaced_: a PC placed after a held intro.
    if (fx_hidden_[static_cast<size_t>(i)] || unplaced_[static_cast<size_t>(i)]) continue;
    // Script ops 34 / 35 / 5B hide the model (no model, no shadow); an entry the pack played shows it.
    if (ghosted(i) && !entry_shown_[static_cast<size_t>(i)]) continue;
    // DYNAMIC target camera: the eye is the acting PC's own attach point 0, i.e. inside its model. The GTE/GPU drop the polygons that
    // close to the eye (SZ ~ 0, packets wider than 1023 px), so none of its hair / hat is seen; the model is skipped while the eye is in it.
    if (const CamPose& eye_pose = camera_.current(); i == active_pc_ && ui_ == Ui::Target && cam_dynamic_ && dyn_target_ >= 0 &&
        std::hypot(eye_pose.eye[0] - visual.x, eye_pose.eye[2] - visual.z) < 400.0f && eye_pose.eye[1] < visual.y + visual.height + 300.0f)
      continue;
    const float yl = lerp(visual.py, visual.y, alpha_);
    // A bespoke effect overlay that steers this model: its block fields replace the placement.
    const effects::FxModel* om = fx_ && fx_->bespoke_models() && static_cast<size_t>(i) < fx_->bespoke_models()->units.size()
                                     ? &fx_->bespoke_models()->units[static_cast<size_t>(i)]
                                     : nullptr;
    if (om && om->hidden) continue;
    const render::Mat4 model_mat = unit_model_matrix(i, om);
    visual.pose.palette(alpha_, palette_);
    render::MeshPipeline::DrawParams dp;
    dp.view_proj = vp;
    dp.view = view;
    dp.model = model_mat;
    dp.palette = palette_;
    dp.linear = linear_filter_ < 0 ? hd_ : linear_filter_ != 0;
    dp.hd = hd_;
    dp.shading = unit_shading(i);
    // [Enhancements] Lighting.
    if (spell_lights_shown_ && spell_.set.active()) dp.lights = &spell_.set;
    // A knocked-out PC keeps its full model colour; only a dying enemy fades out.
    const float dim = unit.pc ? 1.0f : fade;
    dp.tint[0] = dim * (1.0f + 0.8f * flash);
    dp.tint[1] = dim * (1.0f - 0.5f * flash);
    dp.tint[2] = dim * (1.0f - 0.5f * flash);
    // The model colour (0x80 = 1.0) multiplies every part colour: the overlay's, and effect op 0x58's.
    if (om && om->tinted)
      for (size_t channel = 0; channel < 3; ++channel) dp.tint[channel] *= static_cast<float>(om->rgb[channel]) / 128.0f;
    if (fx_unit_[static_cast<size_t>(i)].tinted)
      for (size_t channel = 0; channel < 3; ++channel) dp.tint[channel] *= fx_unit_[static_cast<size_t>(i)].rgb[channel] / 128.0f;
    // The depth-cued renderer is used only after effect op 0x24.
    if (fx_unit_fog_[static_cast<size_t>(i)]) {
      dp.fog_color[0] = fog_cur_.r / 128.0f;
      dp.fog_color[1] = fog_cur_.g / 128.0f;
      dp.fog_color[2] = fog_cur_.b / 128.0f;
      dp.fog_near = fog_cur_.near;
      dp.fog_far = fog_cur_.far;
    }
    // One ordering-table slot per face, flag-0x10 parts by their nearest corner.
    unit_ot_faces(visual.model->asset, dp);
    shading_params(visual.model->asset, dp);
    // An overlay's part flags.
    if (om) dp.hidden_meshes = (dp.hidden_meshes | om->part_hide) & ~om->part_show;
    if (dp.env_meshes && fx_ && fx_->bespoke_models() && fx_->bespoke_models()->env_page) dp.env_set = fx_vram_page_set(0x95, 0x3BC0);
    // anmseq ops 0x19-0x1C 0x1F 0x20 0x8E: hidden model / parts, model colour, semi-transparency.
    if (!anm_unit_apply(i, dp)) continue;
    // DYNAMIC target camera: all units but the cursor unit are translucent.
    if (dp.model_abr < 0 && !dp.flash && dynamic_translucent(i)) dp.model_abr = 0;
    // The GTE-lit packer.
    if (fx_lit_[static_cast<size_t>(i)]) fx_light_params(i, model_mat, dp);
    meshes_->draw(*visual.model->gpu, dp);
    // The model's two attached-object slots (both draw the same weapon mesh at mount record 0 and 1; the hide words are set by anmseq
    // ops 0x79 / 0x7A n and by the battle-start script, S5 entry 8: most PCs hide slot 1 there, Van / Mel swap hands with them, Glenn's second
    // form shows two). Slot 1 needs a record (all-zero = no second mount). A thrown weapon is hidden here and drawn by its unit emitter
    // (anm_draw_meshes).
    for (int slot = 0; slot < 2; ++slot) {
      render::Mat4 wm;
      if (weapon_matrix(i, slot, wm)) {
        render::MeshPipeline::DrawParams wd = dp;
        // Op 0xA0's per-part colours are the unit model's shading entries, not the weapon's; the weapon always files its faces by their average
        // z; the weapon renderers light by their own rule instead of the model's.
        wd.part_mask = wd.part_semi = 0;
        wd.near_meshes = wd.env_meshes = 0;
        wd.light.on = false;
        // A plain weapon adds a specular glint per corner (a prism weapon is lit instead, below): light = the unit vector from the unit toward the stage light (straight down when any
        // component exceeds 0x4000; the default light is 3072 above the arena centre), eye = the live camera eye. The glint is drawn only while
        // the slot's camera-space position lies within 4096 of the eye's world position (y down) on every axis: the two spaces are compared as
        // they are, so it shows in close shots and in the command orbit and not in wide ones.
        if (!visual.weapon_prism) {
          const render::Mat4 at_eye = cam_.view() * (wd.model * wm);
          const float eye_ps1[3] = {cam_.eye[0], -cam_.eye[1], cam_.eye[2]};
          bool near_eye = true;
          for (int k = 0; k < 3; ++k) near_eye = near_eye && std::fabs(eye_ps1[k] - at_eye.at(k, 3)) < 4096.0f;
          static const bool glint_trace = std::getenv("OC_GLINT_TRACE") != nullptr;
          if (glint_trace)
            log::debug("weapon glint: frame {} unit {} slot {} shot '{}' {} pos {:.0f} {:.0f} {:.0f} eye {:.0f} {:.0f} {:.0f}", frame_count_, i, slot, camera_.shot_name(),
                       near_eye ? "on" : "off", at_eye.at(0, 3), at_eye.at(1, 3), at_eye.at(2, 3), eye_ps1[0], eye_ps1[1], eye_ps1[2]);
          // The stage light, y-up world (PS1 (0, -0xC00, 0) default = (0, 3072, 0) here).
          const std::array<float, 3> light = stage_scene_.light();
          float to_light[3] = {light[0] - visual.x, light[1] - yl, light[2] - visual.z};
          if (std::fabs(to_light[0]) > 16384.0f || std::fabs(to_light[1]) > 16384.0f || std::fabs(to_light[2]) > 16384.0f) to_light[0] = 0.0f, to_light[1] = 1.0f, to_light[2] = 0.0f;
          const float len = std::sqrt(to_light[0] * to_light[0] + to_light[1] * to_light[1] + to_light[2] * to_light[2]);
          wd.light.spec = near_eye && len > 0.0f;
          // The specular routine draws only the GT3 / GT4 groups (Lynx's scythe has 4 FT3 faces the stock dispatcher draws).
          wd.gouraud_textured_only = near_eye;
          for (size_t k = 0; k < 3; ++k) {
            wd.light.spec_dir[k] = len > 0.0f ? to_light[k] / len : 0.0f;
            wd.light.spec_eye[k] = cam_.eye[k];
          }
        }
        wd.palette = std::span<const render::Mat4>(&wm, 1);
        if (visual.weapon_prism) prism_weapon_light(wd.model * wm, static_cast<float>(tick_count_) + alpha_, wd);
        meshes_->draw(*visual.weapon->gpu, wd);
      }
    }
  }
  showcase_draw(vp, view);
  meshes_->end();
  // The stage's smoke / dust sheets.
  stage_scene_.draw_sprites(frame, scissor, fx_view(), lb, alpha_);
  // Effect quads stay inside the draw area too.
  frame.set_scissor(scissor);
  fx_draw(frame, lb);
  anm_draw(frame, lb);
  frame.set_scissor(frame.full());
  // The miya `fade` quad lives in the effect OT, UNDER the 3E strip pass (which runs last in the frame), so it is part of the picture the
  // strips feed back: drawn here, inside the warp target when the bracket is on. SUBTRACTIVE, colour 4090 - 34 t of 4096, t = 27 + 0.65 * battle
  // tick (the emitter spawns one tick after the battle starts: the first battle frame is unfaded). Under the RDE hand-over the stage intro runs
  // live and the 'fade' timeline's own emitter draws the flat.
  float fade_level = 0.0f;
  if (!rde_hand_ && white_ticks_ >= 2) {
    const float fade_tick = kStartFadeLead + kStartFadeRate * static_cast<float>(white_ticks_);
    fade_level = std::clamp((4090.0f - 34.0f * fade_tick) / 4096.0f, 0.0f, 1.0f);
  }
  if (fade_level > 0.0f && white_.set) {
    const float vw = static_cast<float>(frame.extent.width), vh = static_cast<float>(frame.extent.height);
    sprites_->begin(frame, vw, vh, true);
    // The screen in use (widescreen: the widened frame).
    const render::Letterbox::Rect veil = render::Letterbox::veil_rect(vw, vh);
    const u8 grey = static_cast<u8>(255.0f * fade_level + 0.5f);
    sprites_->set_blend(render::BlendMode::Subtract);
    sprites_->draw(white_.set, veil.x, veil.y, veil.w, veil.h, 0, 0, 1, 1, render::pack_rgba(grey, grey, grey, 255));
    sprites_->set_blend(render::BlendMode::Alpha);
    sprites_->end();
  }
}

void BattleView::draw_ui(const render::Frame& frame, bool sprites_resume, const render::Letterbox* pass) {
  if (!scene_visible()) return;
  const float vw = pass ? pass->vw : static_cast<float>(frame.extent.width), vh = pass ? pass->vh : static_cast<float>(frame.extent.height);
  // The HUD's 1280 x 960 canvas = the 4:3 fit.
  const render::Letterbox lb = pass ? *pass : render::Letterbox::fit(vw, vh);
  apply_camera(alpha_);
  ui_pass_ = pass != nullptr;
  sprites_->begin(frame, vw, vh, sprites_resume || fog_pass_);
  replay(frame, lb, alpha_);
  sprites_->end();
  ui_pass_ = false;
}

}  // namespace oc::game
