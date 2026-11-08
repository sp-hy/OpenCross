// BattleView: the effect pack's PSEUDO-ACTOR.
//
// Some overlays register one more model of their own, the type-0x14 resource `act0` (not a unit of either list): its anmseq script runs on the same VM as a unit's,
// it has its own pose, mover and draw, it is hidden until the script shows it and it lives as long as the pack.
//  * ghost1 (Dragon Rider 181 / 184) and jigoku (Dinner Guest 207): the pack's type-0x0B `act0` Mdl (ryu_c / ryu_z, 28 joints;
//    another.prd), texture TIM (0x1C0, 0x100) CLUT (0x100, 0xE2) = the name binding "e297" (jigoku: (0x280, 0x100) CLUT (0x100, 0xEE) = "ee9a"); the script is the
//    model's OWN S5 entry 0.
//  * oomono2 (Big Catch 192): model N of file group 0xE (files 3N + 2 LTD / 3N + 3 OBJ = the battle model table, model_table.h), the pack's
//    type-0x1A `act0` table, entry 0 variant 0.
// The scripts read the caster's script words (the global anmseq words 128+) and start the pack's sequences (`Sequence zmn0`).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

#include "core/log.h"
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"
#include "game/battle/model_table.h"
#include "import/container.h"

namespace oc::game {

using namespace bv;

void BattleView::pa_begin(const std::shared_ptr<const FxPack>& pack) {
  pa_ = PseudoActor{};
  const bseq::Table* table = nullptr;
  std::string what;
  // `sky` (tech/sky.prd, element 162) sets the same model up: "act0" / "act1" (by the first caster) with its texture + CLUT, run from its own S5 entry 0, registered
  // as slot A; it is the big blue figure next to Serge and every `hik*` / `kem*` bit-7 object of the pack rides its joint (selector 8, see BattleFxRun::actor_basis).
  // tumaru (Pip's form changes) and ghost2 (Shadow Cat's Shadow, the ghost techs): the pack's type-0x0B `act0` Mdl at "e297", its own S5 entry 0;
  // canon the same at "ea9a". Hidden until its script shows it.
  if (pack->overlay == "ghost1" || pack->overlay == "jigoku" || pack->overlay == "sky" || pack->overlay == "tumaru" || pack->overlay == "ghost2" ||
      pack->overlay == "canon") {
    if (FxGpu* gpu = fx_gpu_for(pack)) {
      std::string an = "act0";
      if (pack->overlay == "sky") {
        // The acting list's first unit picks the model: act1 (texture page 0x200, CLUT row 0xE3, the fat figure) when its equipped weapon item id is 0x26
        // = the Mastermune (EN "Mastermune", JP "Grandream"); everyone else keeps act0. Dev knob `battleview ... --equip 0x26`.
        const int first = !act_.casters.empty() ? act_.casters.front() : act_.actor;
        const bool mastermune = first >= 0 && first < battle::kUnits && battle_->unit(first).pc && battle_->unit(first).eq.weapon_id == 0x26;
        if (mastermune) an = "act1";
        if (gpu->models.find(an) == gpu->models.end()) an = an == "act0" ? "act1" : "act0";
      }
      if (const auto it = gpu->models.find(an); it != gpu->models.end()) {
        if (auto drp = import::parse_drp(pack->prd))
          for (const auto& entry : drp->entries)
            if (entry.type == 0x0B && entry.name == an) bseq::load_model_table(entry.data, pa_.own_table);
        pa_.fm = it->second.get();
        pa_.pose = &pa_.fm->pose;
        pa_.asset = &pa_.fm->asset;
        pa_.gpu = pa_.fm->gpu.get();
        if (!pa_.own_table.empty()) table = &pa_.own_table;
        what = "pack type-0x0B " + an;
      }
    }
  } else if (pack->overlay == "oomono2") {
    if (const auto it = pack->tables.find("act0"); it != pack->tables.end()) table = &it->second;
    // Model number N: Big Catch picks the creature at random from the Dopplegang pool when the cast resolves; when the 1 in 17 roll fails (or the pool is
    // empty) N stays 0 = model-table entry 0 (Serge) and the cast misses. The rules (battle::Battle, case 39) put the number on the ActionEvent.
    // OC_ACT0_MODEL overrides.
    int model_number = 0;
    if (!act_.ev.empty() && act_.ev.front().creature >= 0) model_number = act_.ev.front().creature;
    if (const char* env = std::getenv("OC_ACT0_MODEL")) model_number = std::atoi(env);
    if (model_number >= 0 && static_cast<size_t>(model_number) < kBattleModels.size() && !kBattleModels[static_cast<size_t>(model_number)].empty()) {
      pa_.handle = load_model_for(std::string(kBattleModels[static_cast<size_t>(model_number)]));
      if (pa_.handle && pa_.handle->gpu) {
        pa_.own_pose.set_model(&pa_.handle->asset);
        pa_.pose = &pa_.own_pose;
        pa_.asset = &pa_.handle->asset;
        pa_.gpu = pa_.handle->gpu.get();
        what = std::format("group 0xE model {} ({})", model_number, kBattleModels[static_cast<size_t>(model_number)]);
      }
    }
    if (what.empty()) what = "no model";
  } else if (FxGpu* gpu = fx_gpu_for(pack)) {
    // EVERY type-0x0B Mdl named 'act0' becomes slot A when the pack loads, placed at the origin; it runs a script only when its overlay starts one
    // (the cases above). The camera's case 8 frames it.
    std::string slot_a;
    if (auto drp = import::parse_drp(pack->prd))
      for (const auto& entry : drp->entries)
        if (effects::fx_slot_a_model(entry.name, entry.type)) slot_a = entry.name;
    if (const auto it = gpu->models.find(slot_a); !slot_a.empty() && it != gpu->models.end()) {
      pa_.fm = it->second.get();
      pa_.pose = &pa_.fm->pose;
      pa_.asset = &pa_.fm->asset;
      pa_.gpu = pa_.fm->gpu.get();
      pa_.registered = true;
      log::debug("battle fx: pack act0 Mdl registered as slot A (no script)");
    }
  }
  // Result code 2 (a miss: Big Catch's broken line) starts the pack sequence `mis2`; any other code starts nothing here (the act0 script starts `zmn0`).
  if (fx_ && pack->overlay == "oomono2" && !act_.ev.empty() && act_.ev.front().creature >= 0 && act_.ev.front().code == 2) fx_->start_sequence("mis2");
  if (!table) return;
  pa_.on = pa_.vm.start(table, 0, 0);
  if (pa_.on && std::getenv("OC_SEQ_TRACE") != nullptr)
    pa_.vm.trace = [this](int pc, const s16* words, int len) {
      std::string text;
      for (int k = 0; k < len; ++k) text += std::format(" {:X}", words[k]);
      log::debug("seq: tick {} pseudo-actor pc {}:{}", tick_count_, pc, text);
    };
  log::debug("battle fx: pseudo-actor {} script {}", what, pa_.on ? "started" : "missing");
}

void BattleView::pa_tick() {
  if (!pa_.on) return;
  PseudoActor& actor = pa_;
  actor.px = actor.x, actor.pz = actor.z, actor.py = actor.y, actor.pface_x = actor.face_x, actor.pface_z = actor.face_z;
  ++actor.ticks;
  // The mover (as tick_units): linear x / z, y linear or the ballistic arc of JumpT.
  if (actor.move.on) {
    --actor.move.left;
    const float blend = std::clamp(1.0f - static_cast<float>(actor.move.left) / static_cast<float>(actor.move.total), 0.0f, 1.0f);
    actor.x = lerp(actor.move.fx, actor.move.tx, blend);
    actor.z = lerp(actor.move.fz, actor.move.tz, blend);
    if (actor.move.jump) {
      float yd = -actor.y;
      if (actor.move.first) yd -= actor.move.vy * 0.5f, actor.move.first = false;
      yd += actor.move.vy;
      actor.move.vy += actor.move.g;
      actor.y = -yd;
    } else {
      actor.y = lerp(actor.move.fy, actor.move.ty, blend);
    }
    if (actor.move.left <= 0) {
      actor.y = actor.move.ty;
      actor.move.jump = false;
      actor.move.on = false;
    }
  }
  if (actor.turn.on) {
    --actor.turn.left;
    const float blend = std::clamp(1.0f - static_cast<float>(actor.turn.left) / static_cast<float>(actor.turn.total), 0.0f, 1.0f);
    const float yaw = lerp(actor.turn.from, actor.turn.to, blend);
    actor.face_x = -std::sin(yaw);
    actor.face_z = -std::cos(yaw);
    if (actor.turn.left <= 0) actor.turn.on = false;
  }
  // Angle += step per tick, written back & 0xFFF; the task ends after n ticks without snapping.
  if (actor.xz.left > 0) {
    float* ang[2] = {&actor.rx, &actor.rz};
    for (int k = 0; k < 2; ++k) {
      actor.xz.cur[k] += actor.xz.step[k];
      *ang[k] = static_cast<float>((actor.xz.cur[k] >> 12) & 0xFFF) * (2.0f * kPi / 4096.0f);
    }
    --actor.xz.left;
  }
  if (actor.scale_left > 0) {
    for (size_t k = 0; k < 3; ++k) actor.scale[k] += actor.scale_step[k];
    if (--actor.scale_left <= 0)
      for (size_t k = 0; k < 3; ++k) actor.scale[k] = actor.scale_to[k];
  }
  if (actor.fade.on) {
    for (size_t k = 0; k < 3; ++k) {
      actor.fade.cur[k] = std::clamp(actor.fade.cur[k] + actor.fade.step[k], 0, 0xFF0);
      actor.rgb[k] = actor.fade.cur[k] >> 4;
    }
    if (--actor.fade.left <= 0) {
      for (size_t k = 0; k < 3; ++k) actor.rgb[k] = actor.fade.to[k];
      if (!actor.fade.plain) actor.abr = -1;
      actor.fade = {};
    }
  }
  if (actor.vm.running()) {
    bseq::Ctx ctx;
    ctx.pos_x = static_cast<int>(std::lround(actor.x));
    ctx.pos_z = static_cast<int>(std::lround(actor.z));
    ctx.pos_y = static_cast<int>(std::lround(-actor.y));
    ctx.yaw = static_cast<int>(std::lround(std::atan2(-actor.face_x, -actor.face_z) * (4096.0f / (2.0f * kPi)))) & 0xFFF;
    ctx.monster = true;
    UnitPose* pose = actor.pose;
    ctx.clip_frames = [pose](int id) { return pose ? pose->frames_of(id) : 8; };
    // The casters' / targets' origin; the approach modes (5, 0x100 + walker attach n of op 0x97) stop short of the unit by the pseudo-actor's
    // attach-point reach plus the unit's attach 16 reach, as a unit's approach does (Garai's Shadow: act0 slides to ~half way, not onto the target).
    ctx.get_pos = [this, &actor](int list, int idx, int mode, int& ox, int& oz) {
      const int unit = script_list_unit(list, idx);
      if (unit < 0) return false;
      const Visual& other = vis_[static_cast<size_t>(unit)];
      float px = other.x, pz = other.z;
      if (mode == 5 || mode >= 0x100) {
        const float dx = other.x - actor.x, dz = other.z - actor.z, dist = std::hypot(dx, dz);
        if (dist >= 1.0f) {
          float r15 = 0, r16 = other.reach16, ax = 0, ay = 0, az = 0;
          if (actor.pose && actor.pose->attach_now(mode >= 0x100 ? mode - 0x100 : 15, 1.0f, ax, ay, az)) r15 = std::hypot(ax, az);
          if (other.has_model && other.pose.attach_now(16, 1.0f, ax, ay, az)) r16 = std::hypot(ax, az);
          float reach = r15 + r16;
          if (reach < 1.0f) reach = 450.0f;
          const float run = std::max(0.0f, dist - reach);
          px = actor.x + dx / dist * run;
          pz = actor.z + dz / dist * run;
        }
      } else if (mode == 6 && !act_.targets.empty()) {
        float lx = 1e9f, hx = -1e9f, lz = 1e9f, hz = -1e9f;
        for (int target : act_.targets) {
          lx = std::min(lx, vis_[static_cast<size_t>(target)].x), hx = std::max(hx, vis_[static_cast<size_t>(target)].x);
          lz = std::min(lz, vis_[static_cast<size_t>(target)].z), hz = std::max(hz, vis_[static_cast<size_t>(target)].z);
        }
        px = std::trunc((std::round(hx) + std::round(lx)) * 0.5f);
        pz = std::trunc((std::round(hz) + std::round(lz)) * 0.5f);
      }
      ox = static_cast<int>(std::lround(px));
      oz = static_cast<int>(std::lround(pz));
      return true;
    };
    ctx.get_pos_y = [this](int list, int idx) {
      const int unit = script_list_unit(list, idx);
      return unit < 0 ? 0 : static_cast<int>(std::lround(-unit_world(unit, 1.0f)[1]));
    };
    std::vector<bseq::Event> events;
    actor.vm.tick(ctx, events);
    for (const bseq::Event& event : events) pa_event(event);
    if (!actor.vm.running()) log::debug("battle fx: pseudo-actor script done at tick {} (pack tick {})", tick_count_, actor.ticks);
  }
  if (actor.pose) actor.pose->tick();
}

void BattleView::pa_event(const bseq::Event& event) {
  using K = bseq::Event;
  PseudoActor& actor = pa_;
  switch (event.kind) {
    case K::SetAnim:
      if (actor.pose) actor.pose->play(event.a, event.b & 0x1FFF, (event.b & 0x8000) != 0, (event.b & 0x6000) >> 13);
      break;
    case K::Speed:
      if (actor.pose) actor.pose->set_speed(event.a);
      break;
    case K::Loop:
      if (actor.pose) actor.pose->set_loop(event.a != 0);
      break;
    case K::ModelShow: actor.hidden = event.a == 0; break;
    case K::ModelColor:
      for (size_t k = 0; k < 3; ++k) actor.rgb[k] = event.off[k];
      break;
    case K::ModelAbr: actor.abr = event.a == 0 ? -1 : event.a - 1; break;
    case K::ModelScale:
      for (size_t k = 0; k < 3; ++k) actor.scale_to[k] = static_cast<float>(event.off[k]) / 4096.0f;
      if (event.d < 1) {
        for (size_t k = 0; k < 3; ++k) actor.scale[k] = actor.scale_to[k];
        actor.scale_left = 0;
      } else {
        for (size_t k = 0; k < 3; ++k) actor.scale_step[k] = (actor.scale_to[k] - actor.scale[k]) / static_cast<float>(event.d);
        actor.scale_left = event.d;
      }
      break;
    // ModelFade: plain colour ramp + semi-transparency mode. ColorRamp with b = 1: the soft fade (flash draw while it runs); b = 0 is the GTE back colour, not the model's.
    case K::ModelFade:
    case K::ColorRamp: {
      if (event.kind == K::ColorRamp && event.b == 0) break;
      const bool plain = event.kind == K::ModelFade;
      actor.fade = {};
      if (plain) actor.abr = event.b == 0 ? -1 : event.b - 1;
      if (event.a < 1) {
        for (size_t k = 0; k < 3; ++k) actor.rgb[k] = event.ang[k];
        break;
      }
      actor.fade.on = true;
      actor.fade.plain = plain;
      actor.fade.left = event.a;
      for (size_t k = 0; k < 3; ++k) {
        actor.fade.cur[k] = event.off[k] << 4;
        actor.fade.to[k] = event.ang[k];
        actor.fade.step[k] = ((event.ang[k] - event.off[k]) * 16) / event.a;
        actor.rgb[k] = event.off[k];
      }
      break;
    }
    case K::SetPos:  // at once (no interpolation across the jump)
      actor.move.on = false;
      actor.x = actor.px = static_cast<float>(event.a);
      actor.z = actor.pz = static_cast<float>(event.b);
      actor.y = actor.py = -static_cast<float>(event.c);
      break;
    case K::SetRot: {  // yaw at once + the x / z rotation words
      actor.turn.on = false;
      const float yaw = static_cast<float>(event.a & 0xFFF) * (2.0f * kPi / 4096.0f);
      actor.face_x = actor.pface_x = -std::sin(yaw);
      actor.face_z = actor.pface_z = -std::cos(yaw);
      actor.rx = static_cast<float>(event.off[0]) * (2.0f * kPi / 4096.0f);
      actor.rz = static_cast<float>(event.off[1]) * (2.0f * kPi / 4096.0f);
      break;
    }
    case K::MovePos: {
      actor.move = {};
      actor.move.on = true;
      actor.move.fx = actor.x, actor.move.fz = actor.z, actor.move.fy = actor.y;
      actor.move.tx = static_cast<float>(event.a), actor.move.tz = static_cast<float>(event.b), actor.move.ty = -static_cast<float>(event.c);
      actor.move.total = actor.move.left = std::max(1, event.d);
      break;
    }
    case K::Jump: {  // begin_jump
      const int ticks = std::max(1, event.d);
      const float gravity = 40140.0f / 4096.0f, ty = -static_cast<float>(event.c), dy_down = -(ty - actor.y);
      actor.move = {};
      actor.move.on = actor.move.first = actor.move.jump = true;
      actor.move.fx = actor.x, actor.move.fz = actor.z, actor.move.fy = actor.y;
      actor.move.tx = static_cast<float>(event.a), actor.move.tz = static_cast<float>(event.b), actor.move.ty = ty;
      actor.move.total = actor.move.left = ticks;
      actor.move.g = gravity;
      actor.move.vy = dy_down / static_cast<float>(ticks) - gravity * static_cast<float>(ticks / 2);
      if (event.tag == 1) actor.move.g = -2.0f * dy_down / static_cast<float>(ticks * ticks), actor.move.vy = -actor.move.g * static_cast<float>(ticks);
      else if (event.tag == 2) actor.move.g = 2.0f * dy_down / static_cast<float>(ticks * ticks), actor.move.vy = 0.0f;
      break;
    }
    case K::Rotate: {
      actor.turn.on = true;
      actor.turn.from = std::atan2(-actor.face_x, -actor.face_z);
      actor.turn.to = rotate_end(actor.turn.from, event.a & 0xFFF);
      actor.turn.total = actor.turn.left = std::max(1, event.b);
      // x / z: the shortest signed difference of the 12-bit angles / n, from the current words (SetRot / earlier ramps).
      {
        const int cur[2] = {static_cast<int>(std::lround(actor.rx * (4096.0f / (2.0f * kPi)))) & 0xFFF, static_cast<int>(std::lround(actor.rz * (4096.0f / (2.0f * kPi)))) & 0xFFF};
        const int to[2] = {event.off[0] & 0xFFF, event.off[1] & 0xFFF};
        for (int k = 0; k < 2; ++k) {
          int diff = (to[k] - cur[k]) & 0xFFF;
          if (diff > 0x800) diff -= 0x1000;
          actor.xz.cur[k] = cur[k] << 12;
          actor.xz.step[k] = (diff << 12) / actor.turn.total;
        }
        actor.xz.left = actor.turn.total;
      }
      break;
    }
    case K::Face: {
      const int target = target_unit(event.a);
      if (target < 0) break;
      const float dx = vis_[static_cast<size_t>(target)].x - actor.x, dz = vis_[static_cast<size_t>(target)].z - actor.z, length = std::hypot(dx, dz);
      if (length < 1.0f) break;
      actor.face_x = actor.pface_x = dx / length;
      actor.face_z = actor.pface_z = dz / length;
      break;
    }
    case K::Sequence:
      if (fx_) fx_->start_sequence(bseq::tag_str(event.tag));
      break;
    case K::Sfx: play_tag_sound(bseq::tag_str(event.tag), event.a); break;
    // Shadow starts off: only oomono2's script sets it.
    case K::Shadow: actor.shadow = event.a != 0; break;
    // Lights, trails, parts: not used by the three scripts.
    default: break;
  }
}

// Model -> world as a unit's: translate * yaw * (y mirrored) * the SetRot x / z turns * scale (the model's own axes).
render::Mat4 BattleView::pa_matrix(float alpha) const {
  const PseudoActor& actor = pa_;
  const float x = lerp(actor.px, actor.x, alpha), z = lerp(actor.pz, actor.z, alpha), y = lerp(actor.py, actor.y, alpha);
  const float fx = lerp(actor.pface_x, actor.face_x, alpha), fz = lerp(actor.pface_z, actor.face_z, alpha);
  render::Mat4 flip;
  flip.at(1, 1) = -1.0f;
  render::Mat4 model = render::mat4_translation(x, y, z) * render::mat4_rotation_y(std::atan2(-fx, -fz));
  // R = Rz * Ry * Rx, then scale. In our y-up world (the PS1 y mirrored) Rz and Rx
  // turn the other way; the yaw stays between them.
  if (actor.rz != 0.0f) {
    render::Mat4 rot;
    const float cos_a = std::cos(actor.rz), sin_a = std::sin(actor.rz);
    rot.at(0, 0) = cos_a, rot.at(0, 1) = sin_a, rot.at(1, 0) = -sin_a, rot.at(1, 1) = cos_a;
    model = render::mat4_translation(x, y, z) * rot * render::mat4_rotation_y(std::atan2(-fx, -fz));
  }
  if (actor.rx != 0.0f) {
    render::Mat4 rot;
    const float cos_a = std::cos(actor.rx), sin_a = std::sin(actor.rx);
    rot.at(1, 1) = cos_a, rot.at(1, 2) = sin_a, rot.at(2, 1) = -sin_a, rot.at(2, 2) = cos_a;
    model = model * rot;
  }
  model = model * flip;
  render::Mat4 scale;
  scale.at(0, 0) = actor.scale[0], scale.at(1, 1) = actor.scale[1], scale.at(2, 2) = actor.scale[2];
  return model * scale;
}

// oomono2's line ring hangs the far end of the line on attach point 0x10 of the act0 model (the type-0x14 resource named "act0"). The model as of the
// last tick (the effect ticks read the game state, not the interpolated one).
bool BattleView::pa_attach(int point, effects::Vec3& out) {
  const PseudoActor& actor = pa_;
  if (!actor.on || !actor.pose || !actor.asset) return false;
  float mx = 0, my = 0, mz = 0;
  if (!actor.pose->attach_or_root(point, 1.0f, mx, my, mz)) return false;
  const render::Mat4 model = pa_matrix(1.0f);
  effects::Vec3 world{};
  for (int row = 0; row < 3; ++row) world[static_cast<size_t>(row)] = model.at(row, 0) * mx + model.at(row, 1) * my + model.at(row, 2) * mz + model.at(row, 3);
  out = {world[0], -world[1], world[2]};
  return true;
}

void BattleView::pa_draw(const render::Frame& frame, const render::Letterbox& lb) {
  const PseudoActor& actor = pa_;
  if (!actor.on || actor.hidden || !actor.gpu || !actor.pose || !actor.asset || !meshes_) return;
  const float fw = static_cast<float>(frame.extent.width), fh = static_cast<float>(frame.extent.height);
  const float x0 = std::clamp(std::round(lb.left()), 0.0f, fw), y0 = std::clamp(std::round(lb.oy), 0.0f, fh);
  const render::Rect scissor{static_cast<int32_t>(x0), static_cast<int32_t>(y0),
                             static_cast<u32>(std::min(fw - x0, std::round(lb.width()))), static_cast<u32>(std::min(fh - y0, std::round(kBufferRows * lb.scale_y)))};
  const render::Mat4 model = pa_matrix(alpha_);
  actor.pose->palette(alpha_, palette_);
  render::MeshPipeline::DrawParams dp;
  dp.view_proj = cam_.view_proj(lb);
  dp.view = cam_.view();
  dp.model = model;
  dp.palette = palette_;
  dp.linear = linear_filter_ < 0 ? hd_ : linear_filter_ != 0;
  dp.shading = character_shading_;
  dp.hd = actor.fm ? false : hd_;
  for (size_t channel = 0; channel < 3; ++channel) dp.tint[channel] = static_cast<float>(actor.rgb[channel]) / 128.0f;
  // A soft fade draws B - F (white CLUT) then B + F, both with the model colour.
  if (actor.fade.on && !actor.fade.plain) dp.flash = true;
  else if (actor.abr >= 0) dp.model_abr = actor.abr;
  // The pseudo-actor is a model block drawn by the stock pass, so its shading entries apply like a unit's: sky's act0 / act1
  // parts carry rgb 0xFC (texels at ~2x) and flag 0x42 / 0x52 (semi-transparent + the stock env page laid over them, ABR 0), giving the
  // chrome-blue figure. ghost1's act0 (0xF0, flag 0xA3: own-page env) likewise.
  shading_params(*actor.asset, dp);
  unit_ot_faces(*actor.asset, dp);
  meshes_->begin(frame, scissor);
  meshes_->draw(*actor.gpu, dp);
  meshes_->end();
}

}  // namespace oc::game
