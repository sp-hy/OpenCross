// FieldWorld: the screen. Background layers and floor shadows (SpriteBatch pass), the skinned actors with their reflected copies
// (3D pass), the overlay (screen fog, sequence colour, veils, windows, the room fade, the captured-frame transitions) and the party
// status icons, plus the script-driven screen state behind them: layer masks and tints, the 8F ripple, the fog and entity colours.
#include "game/field_world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "game/field_world_internal.h"
#include "game/mirror_copy.h"
#include "import/tim.h"
#include "render/texture.h"

namespace oc::game {

using namespace field_internal;

// ------------------------------------------------------------------------------------------------ script screen ops
void FieldWorld::bg_layer(int layer, bool on) {
  if (layer < 0 || layer >= 32) return;
  u32 mask = view_.layer_mask();
  mask = on ? (mask | (1u << layer)) : (mask & ~(1u << layer));
  view_.set_layer_mask(mask);
}

void FieldWorld::set_picker_backdrop(bool on) {
  const u32 mask = view_.layer_mask();
  if (on) {
    picker_scroll_x_ = view_.scroll_x(), picker_scroll_y_ = view_.scroll_y();
    view_.set_layer_mask((mask | 2u) & ~1u);
    view_.set_scroll(0, 0, true);
  } else {
    view_.set_layer_mask((mask | 1u) & ~2u);
    view_.set_scroll(picker_scroll_x_, picker_scroll_y_, true);
  }
}

void FieldWorld::draw_room(render::SpriteBatch& sprites, float vw, float vh) {
  if (view_.loaded()) view_.draw(sprites, vw, vh, 1.0f);
}

// C2: the poly colour (RGB, 128 = neutral) the background group's tiles are modulated with.
void FieldWorld::set_layer_opacity(int layer, int r, int g, int b) {
  if (layer < 0) return;
  view_.set_group_tint(static_cast<size_t>(layer), r, g, b);
}

// 8F screen ripple: the six parameters (x / y amplitude in pixels, x / y phase step per column / row, x / y phase advance per tick)
// ramp linearly (16.16) to their targets over p[6] ticks; while active, each column of the warp grid is shifted by
// sin(phase_x + col * step_x) * amp_x and each row by sin(phase_y + row * step_y) * amp_y, the frame drawn so far re-drawn warped at
// ordering-table index p[7] (>= 7). The state and timing run here; the host draws the warp from wave_view().
void FieldWorld::bg_wave_start(std::span<const s32> params) {
  if (params.size() < 8) return;
  wave_.active = true;
  wave_.ot = std::max(7, static_cast<int>(params[7]));
  wave_.frames = std::max(1, static_cast<int>(params[6]));
  for (size_t i = 0; i < 6; ++i) wave_.step[i] = ((static_cast<s64>(params[i]) << 16) - wave_.cur[i]) / wave_.frames;
  wave_.stop_at_end = false;
  log::debug("field: tick {} BG_WAVE amp {} {} step {} {} speed {} {} over {} ticks, ot {}", tick_count_, params[0], params[1], params[2],
             params[3], params[4], params[5], params[6], params[7]);
}

// 90: mode 0 ramps every parameter to 0 over `arg` ticks and switches off when the ramp ends; 1 waits for the ripple to be off; 2 / 3
// switch it off at once; modes >= 4 never advance.
bool FieldWorld::bg_wave(int mode, int arg) {
  switch (mode) {
    case 0:
      wave_.frames = std::max(1, arg);
      for (size_t i = 0; i < 6; ++i) wave_.step[i] = -wave_.cur[i] / wave_.frames;
      wave_.stop_at_end = true;
      return true;
    case 1: return !wave_.active;
    case 2:
    case 3:
      wave_.active = false;
      return true;
    default: return false;
  }
}

// A ramp tick while the count is above 0; with the stop flag the tick AFTER the count reached 0 zeroes the six values and switches
// the ripple off, so the last ramp value is shown one more tick. The phases advance every tick (by 0 on the switch-off tick).
void FieldWorld::step_wave() {
  if (!wave_.active) return;
  if (wave_.frames > 0) {
    for (size_t i = 0; i < 6; ++i) wave_.cur[i] += wave_.step[i];
    --wave_.frames;
  } else if (wave_.stop_at_end) {
    wave_.cur.fill(0);
    wave_.active = false;
    wave_.stop_at_end = false;
  }
  wave_.phase_x += static_cast<s32>(wave_.cur[4] >> 16);
  wave_.phase_y += static_cast<s32>(wave_.cur[5] >> 16);
}

// The integer parts (high halfwords) of the ramped 16.16 values; the phases are 16-bit accumulators.
FieldWorld::WaveView FieldWorld::wave_view() const {
  WaveView view;
  view.on = wave_.active;
  view.ot = std::max(7, wave_.ot);
  auto hi = [&](size_t i) { return static_cast<int>(static_cast<s16>(static_cast<u16>(static_cast<u64>(wave_.cur[i]) >> 16))); };
  view.warp.amp_x = hi(0);
  view.warp.amp_y = hi(1);
  view.warp.step_x = hi(2);
  view.warp.step_y = hi(3);
  view.warp.phase_x = static_cast<s16>(static_cast<u16>(wave_.phase_x));
  view.warp.phase_y = static_cast<s16>(static_cast<u16>(wave_.phase_y));
  return view;
}

void FieldWorld::set_draw_slab(float lo, float hi) {
  slab_min_ = lo;
  slab_max_ = hi;
  view_.set_slab(lo, hi);
}

void FieldWorld::set_shadow_colors(std::span<const u8> rgbx16) {
  lighting_.set_table(rgbx16);
  for (auto& entity : ents_) entity.light_init = false;
}

// 68: the screen fog ramps to (r, g, b) over `frames` ticks (0 counts as 1) in semi-transparency mode `abr`.
void FieldWorld::set_screen_rgb(int abr, int r, int g, int b, int frames) {
  const int ticks = std::max(1, frames);
  const std::array<float, 3> target = {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)};
  for (size_t i = 0; i < 3; ++i) {
    fog_.target[i] = target[i];
    fog_.step[i] = (target[i] - fog_.cur[i]) / static_cast<float>(ticks);
  }
  fog_.left = ticks;
  fog_.abr = abr;
  static int logged = 0;
  if (logged++ < 4) log::debug("field: tick {} screen fog abr {} rgb {} {} {} over {} frames", tick_count_, abr, r, g, b, ticks);
}

void FieldWorld::set_underwater_rgb(int r, int g, int b) {
  underwater_rgb_ = {r, g, b};
  static int logged = 0;
  if (logged++ < 4) log::debug("field: tick {} underwater RGB {} {} {}", tick_count_, r, g, b);
}

void FieldWorld::set_entity_rgb(int ent, int r, int g, int b) {
  if (valid_ent(ent)) ents_[static_cast<size_t>(ent)].rgb = {static_cast<u8>(r), static_cast<u8>(g), static_cast<u8>(b)};
  static int logged = 0;
  if (logged++ < 4) log::debug("field: tick {} entity {} RGB {} {} {}", tick_count_, ent, r, g, b);
}

bool FieldWorld::tint_active() const { return fog_.cur[0] > 0.5f || fog_.cur[1] > 0.5f || fog_.cur[2] > 0.5f; }

// ------------------------------------------------------------------------------------------------ shadows and background
// Per actor, once per tick: the shadow strength eases by 2 toward (R + G + B) / 6 of the entity colour (128 each -> 64),
// toward 0 on a triangle with any of the flags 0x6E000 (stairs, water); an actor that is not drawn has none.
void FieldWorld::step_shadows() {
  static constexpr float kSlopeEase = 0.2f, kMaxSlope = 1.0f;
  for (size_t index = 0; index < ents_.size(); ++index) {
    Ent& entity = ents_[index];
    // Off the mesh (a ladder) the last slope stays.
    if (entity.actor.loaded() && entity.actor.triangle() >= 0) {
      float normal[3];
      ActorShadows::floor_normal(view_.walk(), entity.actor.triangle(), normal);
      if (normal[1] != 0.0f) {
        entity.shadow_slope_x += (std::clamp(-normal[0] / normal[1], -kMaxSlope, kMaxSlope) - entity.shadow_slope_x) * kSlopeEase;
        entity.shadow_slope_z += (std::clamp(-normal[2] / normal[1], -kMaxSlope, kMaxSlope) - entity.shadow_slope_z) * kSlopeEase;
      }
    }
    if (vm_.entity(static_cast<int>(index)).flags38 & 0x10C420) {
      entity.shadow_grey = 0;
      continue;
    }
    const u32 tri_flags = walk_flags(static_cast<int>(index));
    const int target = (tri_flags & 0x6E000) ? 0 : (entity.rgb[0] + entity.rgb[1] + entity.rgb[2]) / 6;
    const int step = 2;
    entity.shadow_grey = entity.shadow_grey < target ? std::min(target, entity.shadow_grey + step) : std::max(target, entity.shadow_grey - step);
  }
}

// Ordering-table index below which a semi-transparent layer is nearer than every drawn actor (an actor's faces sit at camera z >> 4 +
// its layer word; 16 slots of margin for the model's extent): such layers are drawn after the actors.
float FieldWorld::late_layer_z() const {
  float nearest = 1e9f;
  for (size_t index = 0; index < ents_.size(); ++index) {
    const Ent& entity = ents_[index];
    if (!entity.actor.loaded() || ent_not_drawn(static_cast<int>(index))) continue;
    const ActorPose& pose = entity.actor.current();
    render::FieldCamera::Projected projected;
    if (!view_.camera().project({pose.x, pose.y, pose.z}, projected)) continue;
    nearest = std::min(nearest, std::floor(std::max(projected.z, 0.0f) / 16.0f) + static_cast<float>(vm_.entity(static_cast<int>(index)).word_b8));
  }
  return nearest > 1e8f ? -1e9f : nearest - 16.0f;
}

void FieldWorld::draw_background_late(render::SpriteBatch& sprites, float vw, float vh, float alpha) {
  alpha = interp_alpha(alpha);
  if (!view_.loaded()) return;
  view_.set_late(late_layer_z(), true);
  view_.draw(sprites, vw, vh, alpha);
  view_.set_late(-1e9f, false);
}

// The background layers, then one subtractive textured quad per drawn actor on its floor plane, in the ordering-table slot of its
// corner, after the layers (nearer opaque layers hide it; it never writes depth, so it cannot cut into models).
void FieldWorld::draw_background(render::SpriteBatch& sprites, float vw, float vh, float alpha) {
  alpha = interp_alpha(alpha);
  if (!view_.loaded()) return;
  view_.set_late(late_layer_z(), false);
  view_.draw(sprites, vw, vh, alpha);
  view_.set_late(-1e9f, false);
  static const bool no_shadows = std::getenv("OC_FIELD_SHADOWS") && std::getenv("OC_FIELD_SHADOWS")[0] == '0';
  if (!shadows_ok_ || no_shadows) return;
  std::vector<ShadowParams> shadows;
  for (size_t index = 0; index < ents_.size(); ++index) {
    const Ent& entity = ents_[index];
    if (!casts_shadow(index)) continue;
    const script::Entity& vm_ent = vm_.entity(static_cast<int>(index));
    const ActorPose pose = entity.actor.pose(alpha);
    const bool jumping = entity.motion.active && (entity.motion.kind == script::MotionKind::Jump || entity.motion.kind == script::MotionKind::Tween);
    ShadowParams shadow;
    shadow.x = pose.x;
    shadow.y = jumping ? ground_y(entity) : pose.y;
    shadow.z = pose.z;
    ActorShadows::floor_normal(view_.walk(), entity.actor.triangle(), shadow.normal);
    shadow.scale_x = static_cast<float>(static_cast<s16>(vm_ent.scale[0]));
    shadow.scale_z = static_cast<float>(static_cast<s16>(vm_ent.scale[2]));
    shadow.grey = entity.shadow_grey;
    shadow.layer = static_cast<float>(vm_ent.word_b8);
    if (render::FieldCamera::Projected projected; view_.camera().project({shadow.x, shadow.y, shadow.z}, projected) &&
                                                  !in_slab(std::floor(std::max(projected.z, 0.0f) / 16.0f) + shadow.layer))
      continue;
    if (entity.shadow_tile >= 0) {
      shadow.tile_half = entity.shadow_half;
      shadow.tile_uv = render::ShadowPipeline::tile_uv(entity.shadow_tile);
      shadow.tile_slope_x = entity.shadow_slope_x;
      shadow.tile_slope_z = entity.shadow_slope_z;
    }
    shadows.push_back(shadow);
  }
  const render::Letterbox lb = view_.letterbox(vw, vh);
  float scroll_x, scroll_y;
  view_.scroll_at(alpha, scroll_x, scroll_y);
  shadows_.draw(sprites, view_.camera(), lb, scroll_x, scroll_y, shadows, hd_ && view_.art_mode_hd(), shadow_tiles_.ready() ? shadow_tiles_.tiles() : render::TexRef{});
}

bool FieldWorld::casts_shadow(size_t index) const {
  const Ent& entity = ents_[index];
  return entity.actor.loaded() && !ent_not_drawn(static_cast<int>(index)) && entity.shadow_grey > 0 && !(vm_.entity(static_cast<int>(index)).flags38 & 0x10C420);
}

// [Enhancements] Lighting: each actor flattened straight down onto its own floor height, one tile per actor. The tile is sized to the pose, so a raised arm
// stays inside it; an actor past the mask's tiles keeps the blob. The strength is the PS1 shadow's (water and stairs fade it out the same way).
void FieldWorld::prepare_shadows(const render::Frame& frame, float alpha) {
  for (Ent& entity : ents_) entity.shadow_tile = -1;
  if (!cast_shadows_ || shadow_tiles_failed_ || !shadows_ok_ || !view_.loaded()) return;
  if (!shadow_tiles_.ready() && !shadow_tiles_.init(*ctx_)) {
    log::warn("field: shadow pipeline failed, blob shadows only");
    shadow_tiles_failed_ = true;
    return;
  }
  alpha = interp_alpha(alpha);
  static constexpr float kPoseMargin = 72.0f, kMinHalf = 96.0f, kMaxHalf = 640.0f;
  std::vector<render::Mat4> palette;
  std::vector<float> halves;
  int tile = 0;
  for (size_t index = 0; index < ents_.size() && tile < static_cast<int>(render::ShadowPipeline::kMaxCasters); ++index) {
    Ent& entity = ents_[index];
    if (!casts_shadow(index) || !entity.actor.gpu()) continue;
    if (tile == 0) shadow_tiles_.begin_tiles(frame);
    const ActorPose pose = entity.actor.pose(alpha);
    const bool jumping = entity.motion.active && (entity.motion.kind == script::MotionKind::Jump || entity.motion.kind == script::MotionKind::Tween);
    const render::Mat4 model = entity.actor.model_matrix(alpha);
    entity.actor.palette(alpha, palette);
    float reach = 0.0f;
    for (const render::Mat4& joint : palette) {
      const render::Mat4 world = model * joint;
      reach = std::max({reach, std::fabs(world.at(0, 3) - pose.x), std::fabs(world.at(2, 3) - pose.z)});
    }
    entity.shadow_half = std::clamp(reach + kPoseMargin, kMinHalf, kMaxHalf);
    entity.shadow_tile = tile;
    halves.push_back(entity.shadow_half);
    shadow_tiles_.place_tile(tile++, pose.x, pose.z, jumping ? ground_y(entity) : pose.y, entity.shadow_half);
    std::array<render::ShadowPipeline::Cast, render::ShadowPipeline::kChannels> casts;
    casts[0] = {{0.0f, 1.0f, 0.0f}, std::clamp(static_cast<float>(entity.shadow_grey) / 90.0f, 0.0f, 1.0f)};
    shadow_tiles_.cast(*entity.actor.gpu(), model, palette, entity.actor.hidden_parts(), 0, casts);
  }
  if (tile > 0) {
    shadow_tiles_.end_mask();
    shadow_tiles_.soften_tiles(frame, halves);
  }
}

// ------------------------------------------------------------------------------------------------ actors
// Each actor in the field ordering-table mode of the mesh pipeline: one slot per face plus the actor's layer word, the shading flags of
// the instance, and the water rule: on a water triangle the faces below the class's water level take the underwater colour and are
// pushed back by the water bias (the room default = layer 0's z), so the semi-transparent water layer blends over them. `wet` selects
// the submerged faces (drawn before the background) or the rest. FE 64 actors take the lit path from the three light slots at their
// waist, with the room's back colour as ambient. An FE 03 entity is drawn a second time reflected across its mirror plane, filed far
// behind (layer 0's z) and flat / untinted, so the nearer layers cover or blend over the copy.
void FieldWorld::draw_actors(render::MeshPipeline& meshes, const render::Frame& frame, float alpha, render::MeshPipeline::DrawParams::WetFaces wet) {
  alpha = interp_alpha(alpha);
  if (!view_.loaded()) return;
  using Wet = render::MeshPipeline::DrawParams::WetFaces;
  const render::Letterbox lb = view_.letterbox(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
  float scroll_x, scroll_y;
  view_.scroll_at(alpha, scroll_x, scroll_y);
  const render::Rect scissor = view_.clip_rect(lb, scroll_x, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
  const render::Mat4 view_proj = view_.camera().view_proj(scroll_x, scroll_y, lb), view = view_.camera().view();
  float water_bias = static_cast<float>(water_bias_);
  if (water_bias_ < 0) {
    water_bias = 8;
    if (const assets::RoomScene* scene = view_.scene(); scene && !scene->layers.empty() && !scene->layers[0].tiles.empty())
      water_bias = static_cast<float>(std::max<int>(8, scene->layers[0].tiles[0].z));
  }
  static const bool no_model_fx = std::getenv("OC_NO_MODEL_FX") != nullptr;
  std::vector<render::Mat4> palette;
  bool begun = false;
  for (size_t index = 0; index < ents_.size(); ++index) {
    Ent& entity = ents_[index];
    if (!entity.actor.loaded() || ent_not_drawn(static_cast<int>(index))) continue;
    const script::Entity& vm_ent = vm_.entity(static_cast<int>(index));
    if (vm_ent.flags38 & 0x100000u) continue;
    const u32 tri_flags = walk_flags(static_cast<int>(index));
    const bool wet_actor = RoomLighting::water_slot(tri_flags) >= 0 && !(vm_ent.flags38 & 0x10C020);
    const int copy_axis = mirror_copy::axis_index(entity.mirror_axis);
    const bool has_copy = copy_axis >= 0 && mirror_copy::drawn(vm_ent.flags270);
    if (wet == Wet::WetOnly && !wet_actor && !has_copy) continue;
    if (!begun) {
      meshes.begin(frame, scissor);
      begun = true;
    }
    entity.actor.use_hd = hd_;
    entity.actor.palette(alpha, palette);
    render::MeshPipeline::DrawParams dp;
    dp.view_proj = view_proj;
    dp.view = view;
    dp.model = entity.actor.model_matrix(alpha);
    dp.palette = palette;
    dp.linear = view_.linear_filter;
    dp.hd = entity.actor.use_hd;
    dp.ot_faces = true;
    dp.ot_bias = static_cast<float>(vm_ent.word_b8);
    dp.water_bias = water_bias;
    dp.hidden_meshes = entity.actor.hidden_parts();
    dp.near_meshes = entity.actor.near_parts();
    const auto tint = RoomLighting::tint(entity.light, entity.rgb);
    for (size_t channel = 0; channel < 3; ++channel) dp.tint[channel] = tint[channel];
    if (const int slot = RoomLighting::water_slot(tri_flags); slot >= 0 && !(vm_ent.flags38 & 0x10C020)) {
      dp.water_level = static_cast<float>(vm_.water_level(static_cast<size_t>(slot)));
      for (size_t channel = 0; channel < 3; ++channel) dp.water_tint[channel] = static_cast<float>(underwater_rgb_[channel]) / 128.0f;
    }
    dp.wet_faces = wet;
    dp.model_abr = no_model_fx ? -1 : entity.model_abr;
    if (vm_ent.flags274 & 0x400u) {
      float waist[3];
      if (entity.actor.attach_world(9, waist)) {
        const effects::LightMatrices lm = effects::light_matrices(lights_, {waist[0], waist[1], waist[2]}, true);
        dp.light.on = true;
        for (size_t channel = 0; channel < 3; ++channel) dp.light.bk[channel] = static_cast<float>(bk_.cur[channel]) / 256.0f;
        for (size_t light = 0; light < 3; ++light)
          for (size_t channel = 0; channel < 3; ++channel) {
            dp.light.dir[light][channel] = lm.dir[light][channel];
            dp.light.rgb[light][channel] = lm.rgb[light][channel];
          }
      }
    }
    dp.flash = !no_model_fx && entity.flash;
    dp.slot_min = slab_min_;
    dp.slot_max = slab_max_;
    if (wet != Wet::WetOnly || wet_actor) meshes.draw(*entity.actor.gpu(), dp);
    if (has_copy && wet != Wet::DryOnly) {
      render::Mat4 reflection;
      reflection.at(copy_axis, copy_axis) = -1.0f;
      reflection.at(copy_axis, 3) = static_cast<float>(entity.mirror_plane2);
      render::MeshPipeline::DrawParams copy = dp;
      copy.model = reflection * dp.model;
      copy.ot_bias = mirror_copy::slot(static_cast<float>(vm_ent.word_b8), water_bias);
      copy.water_level = 1e9f;
      copy.wet_faces = Wet::Mixed;
      for (float& channel : copy.tint) channel = 1.0f;
      copy.light.on = false;
      copy.mirror = true;
      copy.flash = false;
      meshes.draw(*entity.actor.gpu(), copy);
    }
  }
  if (begun) meshes.end();
}

// ------------------------------------------------------------------------------------------------ overlay
// Screen fog (68): a full-screen flat quad over the picture columns in the PS1 semi-transparency mode of the fog (0 = half add, 1 add,
// 2 subtract, 3 quarter add). Sequence screen colour (2B / 2C): a quad over the whole visible frame, opaque or blended by its mode,
// skipped while black. Then (with `windows`) the transition veil, the windows, the room fade and the captured frame.
void FieldWorld::draw_overlay(render::SpriteBatch& sprites, float vw, float vh, bool windows) {
  if (tint_active() && white_set_) {
    auto to_u8 = [&](float value) { return static_cast<u8>(std::lround(std::clamp(value, 0.0f, 255.0f))); };
    float gain = 1.0f;
    render::BlendMode mode = render::BlendMode::Subtract;
    switch (fog_.abr) {
      case 0: mode = render::BlendMode::Add; gain = 0.5f; break;
      case 1: mode = render::BlendMode::Add; break;
      case 3: mode = render::BlendMode::Add; gain = 0.25f; break;
      default: break;
    }
    const render::Letterbox lb = view_.letterbox(vw, vh);
    float col0, col1;
    view_.picture_columns(lb, static_cast<float>(view_.scroll_x()), col0, col1);
    sprites.set_blend(mode);
    sprites.draw(white_set_, lb.ox + col0 * lb.scale, lb.oy, (col1 - col0) * lb.scale, render::kFieldRows * lb.scale_y, 0, 0, 1, 1,
                 render::pack_rgba(to_u8(fog_.cur[0] * gain), to_u8(fog_.cur[1] * gain), to_u8(fog_.cur[2] * gain), 255));
    sprites.set_blend(render::BlendMode::Alpha);
  }
  for (const SeqDim& dim : seq_dim_) {
    if (dim.cur == std::array<int, 3>{} || !white_set_) continue;
    const render::Letterbox lb = view_.letterbox(vw, vh);
    render::BlendMode mode = render::BlendMode::Alpha;
    float gain = 1.0f;
    u8 alpha = 255;
    switch (dim.mode) {
      case 0: break;
      case 1: alpha = 128; break;
      case 2: mode = render::BlendMode::Add; break;
      case 3: mode = render::BlendMode::Subtract; break;
      default: mode = render::BlendMode::Add; gain = 0.25f; break;
    }
    auto to_u8 = [&](int value) { return static_cast<u8>(std::clamp(static_cast<int>(static_cast<float>(value) * gain + 0.5f), 0, 255)); };
    sprites.set_blend(mode);
    sprites.draw(white_set_, lb.left(), lb.oy, lb.width(), render::kFieldRows * lb.scale_y, 0, 0, 1, 1,
                 render::pack_rgba(to_u8(dim.cur[0]), to_u8(dim.cur[1]), to_u8(dim.cur[2]), alpha));
    sprites.set_blend(render::BlendMode::Alpha);
  }
  draw_mod_ui(sprites, vw, vh);
  if (windows) {
    draw_veil_transition(sprites, vw, vh);
    draw_windows(sprites, vw, vh);
    draw_veil_fade(sprites, vw, vh);
    draw_capture(sprites, vw, vh);
  }
}

// Rects (filled, or a one-pixel outline; translucent ones at 5 / 8 alpha) and lines in their palette colours, in element order; items are not
// drawn; the cursor on its item: an outline 2 pixels out pulsing 1 pixel every 8 ticks (style 0), a translucent white fill (1) or nothing (2).
// Coordinates are field screen pixels, as the dialogue windows'.
void FieldWorld::draw_mod_ui(render::SpriteBatch& sprites, float vw, float vh) {
  using script::ModUi;
  const ModUi& ui = vm_.mod_ui();
  if (!ui.shown || !white_set_) return;
  const render::Letterbox lb = view_.letterbox(vw, vh);
  const auto sx = [&](float x) { return lb.ox + x * lb.scale; };
  const auto sy = [&](float y) { return lb.oy + y * lb.scale_y; };
  const auto colour = [&](u8 index, u8 alpha) {
    const std::array<u8, 3>& rgb = ui.palette[index % ModUi::kPalette];
    return render::pack_rgba(rgb[0], rgb[1], rgb[2], alpha);
  };
  const auto fill = [&](float x0, float y0, float x1, float y1, u32 rgba) {
    sprites.draw(white_set_, sx(x0), sy(y0), (x1 - x0) * lb.scale, (y1 - y0) * lb.scale_y, 0, 0, 1, 1, rgba);
  };
  // One field pixel wide.
  const auto line = [&](float x0, float y0, float x1, float y1, u32 rgba) {
    const float dx = x1 - x0, dy = y1 - y0, length = std::hypot(dx, dy);
    if (length <= 0.0f) return;
    const float nx = -dy / length * 0.5f, ny = dx / length * 0.5f;
    const float xy[8] = {sx(x0 + nx), sy(y0 + ny), sx(x1 + nx), sy(y1 + ny), sx(x0 - nx), sy(y0 - ny), sx(x1 - nx), sy(y1 - ny)};
    const float uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    sprites.draw_quad(white_set_, xy, uv, rgba);
  };
  const auto outline = [&](float x0, float y0, float x1, float y1, u32 rgba) {
    fill(x0, y0, x1, y0 + 1, rgba);
    fill(x0, y1 - 1, x1, y1, rgba);
    fill(x0, y0, x0 + 1, y1, rgba);
    fill(x1 - 1, y0, x1, y1, rgba);
  };
  for (const ModUi::Element& element : ui.elements) {
    if (!element.visible) continue;
    if (element.kind == ModUi::kRect) {
      const u32 rgba = colour(element.colour, element.flags & ModUi::kTranslucent ? 0xA0 : 0xFF);
      const float x1 = static_cast<float>(element.x + element.w), y1 = static_cast<float>(element.y + element.h);
      if (element.flags & ModUi::kOutline) outline(element.x, element.y, x1, y1, rgba);
      else fill(element.x, element.y, x1, y1, rgba);
    } else if (element.kind == ModUi::kLine) {
      line(element.x, element.y, element.w, element.h, colour(element.colour, 0xFF));
    }
  }
  if (ui.selectable(ui.cursor) && ui.cursor_style != ModUi::kCursorNone) {
    const ModUi::Element& item = ui.elements[static_cast<size_t>(ui.cursor)];
    if (ui.cursor_style == ModUi::kCursorHighlight) {
      fill(item.x, item.y, static_cast<float>(item.x + item.w), static_cast<float>(item.y + item.h), render::pack_rgba(255, 255, 255, 0x50));
    } else {
      const float pad = (tick_count_ >> 3) & 1 ? 3.0f : 2.0f;
      outline(item.x - pad, item.y - pad, item.x + item.w + pad, item.y + item.h + pad, render::pack_rgba(255, 255, 0, 255));
    }
  }
}

// One message window per text element; a hidden or cleared element (and a room change, which clears the UI) closes its window.
void FieldWorld::sync_mod_ui_windows() {
  using script::ModUi;
  const ModUi& ui = vm_.mod_ui();
  const auto wanted = [&](int id) {
    const ModUi::Element& element = ui.elements[static_cast<size_t>(id)];
    return ui.shown && element.kind == ModUi::kText && element.visible && element.msg >= 0;
  };
  for (ModUiWindow& window : mod_ui_win_) {
    if (window.element < 0 || wanted(window.element)) continue;
    if (window.box) window.box->close();
    window.element = -1;
  }
  if (!ui.shown || !ctx_ || !files_ || !sprites_) return;
  for (int id = 0; id < ModUi::kElements; ++id) {
    if (!wanted(id)) continue;
    const ModUi::Element& element = ui.elements[static_cast<size_t>(id)];
    ModUiWindow* window = nullptr;
    for (ModUiWindow& candidate : mod_ui_win_)
      if (candidate.element == id) window = &candidate;
    if (!window) {
      for (ModUiWindow& candidate : mod_ui_win_)
        if (!window && candidate.element < 0) window = &candidate;
      if (!window) {
        if (!mod_ui_full_logged_) log::warn("field: mod UI: more than {} text elements (the rest are not shown)", mod_ui_win_.size());
        mod_ui_full_logged_ = true;
        return;
      }
      window->revision = static_cast<u16>(element.revision - 1);
    }
    if (window->element == id && window->revision == element.revision) continue;
    if (!window->box) {
      auto box = std::make_unique<ui::DialogueDemo>();
      if (!box->init(*ctx_, *files_, *sprites_, lang_)) return;
      box->show_status(false);
      box->set_portrait_set(portrait_set_);
      box->set_mode(hd_ ? ui::ArtMode::Hd : ui::ArtMode::Original);
      window->box = std::move(box);
      install_name_hooks();
    }
    window->element = id;
    window->revision = element.revision;
    ui::DialogueDemo& box = *window->box;
    box.set_label(!(element.flags & ModUi::kFramed));
    box.set_portrait(-1);
    box.show(room_id_, element.msg);
    box.set_ps1_window(element.x, element.y);
    box.skip_grow();
  }
}

// The veils and the captured frame after the PS1-resolution window pass, so that the windows sit under the fade as in the other order.
void FieldWorld::draw_overlay_tail(render::SpriteBatch& sprites, float vw, float vh) {
  draw_veil_transition(sprites, vw, vh);
  draw_veil_fade(sprites, vw, vh);
  draw_capture(sprites, vw, vh);
}

// The encounter hand-over's white clear. A 4:3-composed room over a widescreen 3D backdrop (the title) gets the whole widened frame.
void FieldWorld::draw_veil_transition(render::SpriteBatch& sprites, float vw, float vh) {
  if (!transition_white() || !white_set_) return;
  const render::Letterbox lb = view_.letterbox(vw, vh);
  const bool whole = render::screen_extra() > render::wide_extra();
  const render::Letterbox::Rect rect = render::Letterbox::veil_rect(vw, vh);
  const float x = whole ? rect.x : lb.left(), y = whole ? rect.y : lb.oy;
  const float width = whole ? rect.w : lb.width(), height = whole ? rect.h : render::kFieldRows * lb.scale_y;
  sprites.draw(white_set_, x, y, width, height, 0, 0, 1, 1, render::pack_rgba(255, 255, 255, 255));
}

// The slot-0 screen fade over the screen in use (the 4:3 fit, or the whole widened frame in widescreen): it adds toward white for a
// kind 2 room change and subtracts toward black otherwise (room changes, A8, menu returns).
void FieldWorld::draw_veil_fade(render::SpriteBatch& sprites, float vw, float vh) {
  if (fade_ <= 0.001f || !white_set_) return;
  const u8 level = static_cast<u8>(std::lround(std::min(1.0f, fade_) * 255.0f));
  const render::Letterbox::Rect rect = render::Letterbox::veil_rect(vw, vh);
  sprites.set_blend(fade_add_ ? render::BlendMode::Add : render::BlendMode::Subtract);
  sprites.draw(white_set_, rect.x, rect.y, rect.w, rect.h, 0, 0, 1, 1, render::pack_rgba(level, level, level, 255));
  sprites.set_blend(render::BlendMode::Alpha);
}

void FieldWorld::draw_windows(render::SpriteBatch& sprites, float vw, float vh, bool ps1) {
  draw_status_icons(sprites, vw, vh, ps1);
  const render::CanvasFit ps1_fit{0.25f, 0.0f, 0.0f};
  const render::CanvasFit* fit = ps1 ? &ps1_fit : nullptr;
  // The mod UI's text windows under the scripts' own message windows.
  for (auto& window : mod_ui_win_)
    if (window.box && window.element >= 0) window.box->draw(vw, vh, fit);
  dialogue_.draw(vw, vh, fit);
  for (auto& window : extra_win_)
    if (window.box) window.box->draw(vw, vh, fit);
}

// ------------------------------------------------------------------------------------------------ FE 02 captured-frame transitions
void FieldWorld::set_captured_frame(std::vector<u8>&& rgba, u32 width, u32 height) {
  if (!capture_.want || !ctx_ || !sprites_ || width == 0 || height == 0 || rgba.size() < size_t(width) * height * 4) return;
  bool ok = true;
  if (capture_.set && (capture_.tex.width() != width || capture_.tex.height() != height)) {
    capture_.set = {};
    capture_.tex.destroy();
  }
  if (!capture_.set) {
    ok = capture_.tex.create(*ctx_, width, height, render::TextureFormat::Rgba8, rgba.data());
    if (ok) capture_.set = render::TexRef(capture_.tex, white_sampler_);
    ok = ok && capture_.set;
  } else {
    ok = capture_.tex.update(*ctx_, rgba.data());
  }
  capture_.w = width;
  capture_.h = height;
  capture_.ready = ok;
  capture_.want = false;
}

// Kinds 5 / 6 slide the old picture out by 12 pixels per tick (done past 0x140 / 0xF0); the other kinds dim it by the dissolve step
// per tick until the brightness drops below 0.
void FieldWorld::step_capture() {
  if (!capture_.active) return;
  switch (capture_.kind) {
    case 5:
    case 6:
      capture_.slide += 12;
      if (capture_.slide > (capture_.kind == 5 ? 0x140 : 0xF0)) capture_.active = false;
      break;
    default:
      capture_.acc -= capture_.step;
      if (capture_.acc < 0) capture_.active = false;
      break;
  }
}

// The captured old picture over the new room, 1:1 with the field letterbox of the capture's size. In widescreen it covers every
// visible column and the kind-5 slide crosses that whole width in the same ticks. Kinds 5 / 6 draw it opaque, kinds 1 / 3 / 4 additive
// with the brightness (0x80 = full) modulating the texels.
void FieldWorld::draw_capture(render::SpriteBatch& sprites, float vw, float vh) {
  if (!capture_.active || !capture_.set || capture_.w == 0 || capture_.h == 0) return;
  const render::Letterbox dst = view_.letterbox(vw, vh);
  const float capture_w = static_cast<float>(capture_.w), capture_h = static_cast<float>(capture_.h);
  const render::Letterbox src = render::Letterbox::field(capture_w, capture_h);
  const float cols = std::min(dst.w, src.w);
  const float slide_x = capture_.kind == 5 ? static_cast<float>(capture_.slide) * cols / render::kLayerW : 0.0f;
  const float slide_y = capture_.kind == 6 ? static_cast<float>(capture_.slide) : 0.0f;
  const float w = cols - slide_x, h = render::kFieldRows - slide_y;
  if (w <= 0.0f || h <= 0.0f) return;
  const float u0 = src.left() / capture_w, v0 = src.oy / capture_h;
  const float u1 = (src.left() + w * src.scale) / capture_w, v1 = (src.oy + h * src.scale_y) / capture_h;
  if (capture_.kind == 5 || capture_.kind == 6) {
    sprites.draw(capture_.set, dst.left() + slide_x * dst.scale, dst.oy + slide_y * dst.scale_y, w * dst.scale, h * dst.scale_y, u0, v0, u1, v1);
    return;
  }
  const int brightness = std::clamp(capture_.acc >> 16, 0, 0x80);
  const u8 level = static_cast<u8>(std::min(255, brightness * 255 / 128));
  sprites.set_blend(render::BlendMode::Add);
  sprites.draw(capture_.set, dst.left(), dst.oy, w * dst.scale, h * dst.scale_y, u0, v0, u1, v1, render::pack_rgba(level, level, level, 255));
  sprites.set_blend(render::BlendMode::Alpha);
}

// ------------------------------------------------------------------------------------------------ jolt / post pass
// The sequence post pass (3E / 40, on from 3E with identity parameters): the picture zoomed by 4096 / (4096 + trans z) and shifted
// by the translation (scaled like the picture) plus the screen offset, blended by its mode and colour; only a translation / zoom is
// drawn (every rotation the encounter ramp carries is 0). Otherwise the damage jolt: for 8 frames after a hit the previous picture
// is pasted over the new one at 50 %, shifted by (rsin(n * 64) >> 10, rcos(n * 0x39) >> 10) PS1 pixels and scaled by
// 0x1010 + (rsin(n * 128) << 4 >> 12) / 0x1000 about the centre, n = 7 .. 0.
FieldWorld::JoltView FieldWorld::jolt_view() const {
  JoltView jolt;
  if (seq_post_.on && jolt_n_ < 0) {
    const SeqPost& post = seq_post_;
    const float z = 4096.0f + static_cast<float>(post.cur[5]);
    if (z > 64.0f) {
      jolt.on = jolt.post = true;
      jolt.n = static_cast<int>(tick_count_ & 0x3FFFFFFF);
      jolt.scale = 4096.0f / z;
      jolt.tx = static_cast<float>(post.cur[3]) * 0.25f * jolt.scale + static_cast<float>(post.cur[6]);
      jolt.ty = static_cast<float>(post.cur[4]) * 0.25f * jolt.scale + static_cast<float>(post.cur[7]);
      jolt.mode = post.abr;
      jolt.rgb = post.rgb;
      return jolt;
    }
  }
  if (jolt_n_ < 0) return jolt;
  jolt.on = true;
  jolt.n = jolt_n_;
  jolt.tx = static_cast<float>(ps1_sin(jolt_n_ * 64) >> 10);
  jolt.ty = static_cast<float>(ps1_cos(jolt_n_ * 0x39) >> 10);
  jolt.scale = static_cast<float>(((ps1_sin(jolt_n_ * 128) << 4) >> 12) + 0x1010) / 4096.0f;
  return jolt;
}

// ------------------------------------------------------------------------------------------------ status icons
// PS1: the four 16-row icons of the systim.bin TIM (flu, burn, sprain, poison, at the u offsets below). HD: the
// hd/map/etc/system-cold|burn|sprain|poison[_<lang>].png images, drawn at a quarter of their size.
void FieldWorld::load_status_icons(bool hd) {
  bool& tried = hd ? icons_tried_hd_ : icons_tried_ps1_;
  if (tried || !files_ || !ctx_ || !sprites_) return;
  tried = true;
  auto& icons = hd ? icons_hd_ : icons_ps1_;
  static constexpr int kU[4] = {0x58, 0x35, 0x12, 0x00}, kW[4] = {0x19, 0x23, 0x23, 0x12};
  if (!hd) {
    auto raw = files_->read("cdrom/map/etc/systim.bin");
    if (!raw || raw->size() < 0x24) return;
    const u8* bytes = raw->data();
    const u32 off = u32(bytes[0x20]) | u32(bytes[0x21]) << 8 | u32(bytes[0x22]) << 16 | u32(bytes[0x23]) << 24;
    if (off >= raw->size()) return;
    auto tim = import::parse_tim(ByteSpan(bytes + off, raw->size() - off));
    if (!tim) return;
    auto image = import::decode_tim(*tim);
    if (!image) return;
    const import::Image rgba = import::to_rgba(*image);
    for (int k = 0; k < 4; ++k) {
      const int w = kW[k], h = 16;
      if (static_cast<u32>(kU[k] + w) > rgba.width || static_cast<u32>(h) > rgba.height) continue;
      std::vector<u8> px(static_cast<size_t>(w) * h * 4);
      for (int y = 0; y < h; ++y)
        std::memcpy(&px[static_cast<size_t>(y) * w * 4], &rgba.pixels[(static_cast<size_t>(y) * rgba.width + static_cast<size_t>(kU[k])) * 4],
                    static_cast<size_t>(w) * 4);
      StatusIcon& icon = icons[static_cast<size_t>(k)];
      if (!icon.tex.create(*ctx_, static_cast<u32>(w), static_cast<u32>(h), render::TextureFormat::Rgba8, px.data())) continue;
      icon.set = render::TexRef(icon.tex, white_sampler_);
      icon.w = static_cast<float>(w);
      icon.h = static_cast<float>(h);
    }
    return;
  }
  static const char* const kName[4] = {"cold", "burn", "sprain", "poison"};
  if (!icon_sampler_.handle()) icon_sampler_.create(*ctx_, true);
  for (int k = 0; k < 4; ++k) {
    const std::string base = std::string("hd/map/etc/system-") + kName[k];
    std::optional<Bytes> raw;
    if (lang_ != import::Lang::Jp) raw = files_->read(base + "_" + import::lang_code(lang_) + ".png");
    if (!raw) raw = files_->read(base + ".png");
    render::DecodedImage image;
    if (!raw || !render::decode_image(ByteSpan(raw->data(), raw->size()), image) || image.width == 0) continue;
    StatusIcon& icon = icons[static_cast<size_t>(k)];
    if (!icon.tex.create(*ctx_, image.width, image.height, render::TextureFormat::Rgba8, image.rgba.data())) continue;
    icon.set = render::TexRef(icon.tex, icon_sampler_);
    icon.w = static_cast<float>(image.width) / 4.0f;
    icon.h = static_cast<float>(image.height) / 4.0f;
  }
}

void FieldWorld::free_status_icons() {
  for (auto* icons : {&icons_ps1_, &icons_hd_})
    for (StatusIcon& icon : *icons) {
      icon.set = {};
      icon.tex.destroy();
    }
  icon_sampler_.destroy();
  icons_tried_ps1_ = icons_tried_hd_ = false;
}

// While the input lock is clear and no room change is pending, whenever a party member has a status with an icon.
bool FieldWorld::status_icons_shown() const {
  return view_.loaded() && !(vm_.global_flags() & script::gflag::kInputLock) && !jump_.active && trans_ == Trans::None &&
         (party_status_bits() & (kStFlu | kStBurn | kStSprain | kStPoison));
}

// One icon per status any party member has (flu, burn, sprain, poison), right-aligned from x 0x130 in that order, y 8..24. Without HD
// art the PS1 icons are used.
void FieldWorld::draw_status_icons(render::SpriteBatch& sprites, float vw, float vh, bool ps1) {
  if (!status_icons_shown()) return;
  const int bits = party_status_bits();
  load_status_icons(hd_);
  const auto* icons = &(hd_ ? icons_hd_ : icons_ps1_);
  if (hd_ && !(*icons)[3].set) {
    load_status_icons(false);
    icons = &icons_ps1_;
  }
  static constexpr int kBit[4] = {kStFlu, kStBurn, kStSprain, kStPoison};
  const render::Letterbox lb = ps1 ? render::Letterbox::identity(vw, vh) : view_.letterbox(vw, vh);
  float acc = 0;
  for (size_t k = 0; k < 4; ++k) {
    const StatusIcon& icon = (*icons)[k];
    if (!(bits & kBit[k]) || !icon.set) continue;
    acc += icon.w;
    const float x = 304.0f - acc, y = 8.0f;
    sprites.draw(icon.set, lb.ox + x * lb.scale, lb.oy + y * lb.scale_y, icon.w * lb.scale, icon.h * lb.scale_y);
  }
}

}  // namespace oc::game
