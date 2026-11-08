// BattleView: the emitters the anmseq scripts start themselves (anmseq op 0x1D).
//
// `1D flags point tag tag ox oy oz rx ry rz`: every type-0x0A emitter named `tag` in the loaded resources (the attacker's at0.prd, the
// resident battle.prd, and the running element pack's own emitters of the name too: a monster pack carries "0m00" ...) is attached to a follower
// matrix built on attach point `point` of the unit running the script and restarted. Hit sparks ("_s00" / "_m00" / "_l00": the reaction script runs
// on the target with the attacker's table), weapon trails, breath, projectiles. flags & 0x7F = rotation mode (0: RotMatrix(angles), 1: angles + the
// model's yaw, 2: angles x the joint matrix), flags & 0x80 = the follower is rebuilt every tick; it lives for the first emitter header's duration.
// Each model resource (at0.prd, with the weapon class for PCs: the type-0x0A entries "<class><s|m|l>.." are renamed to "_<s|m|l>..")
// gets its own EmitterSystem so equal tags of different models never mix. The timelines of a unit's at0.prd (a heavy hit's) start the same
// emitters, a per-target repeat a clone per target. Sprite quads and the resident mesh sprites are drawn (no model particles).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>

#include "core/log.h"
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"
#include "game/effects/fx_math.h"
#include "import/container.h"
#include "import/room_effects.h"

namespace oc::game {

using namespace bv;

struct BattleView::AnmFx {
  std::shared_ptr<const FxPack> pack;
  // The unit's nested effect drps (their emitters join the bank).
  std::vector<std::shared_ptr<const assets::RoomEffects>> subpacks;
  effects::EffectBank bank;
  std::unique_ptr<effects::EmitterSystem> em;
  struct Follower {
    int unit = 0, point = 0, mode = 0;
    bool follow = false;
    std::array<s16, 3> off{}, ang{};
    int life = 0;
    effects::Basis basis;
  };
  std::map<int, Follower> followers;
  int next_id = 1;
  std::vector<effects::DrawQuad> quads;
};

namespace {
bool anm_trace() {
  static const bool on = std::getenv("OC_ANM_TRACE") != nullptr;
  return on;
}
}  // namespace

void BattleView::anm_reset() {
  anm_.clear();
  trail_.reset();
}

// Follower matrix: translation = the attach point's world position + (ox, oy, oz); rotation by mode. PS1 world axes (y down).
// The model's yaw is added for modes >= 1.
template <class Follower>
static bool anm_basis(const Follower& follower, effects::Basis& out, const std::function<bool(int, int, effects::Vec3&)>& attach,
                      const std::function<float(int)>& yaw_of) {
  effects::Vec3 attach_pos{};
  if (!attach(follower.unit, follower.point, attach_pos)) return false;
  int ay = follower.ang[1];
  if ((follower.mode & 0x7F) >= 1) ay += static_cast<int>(std::lround(yaw_of(follower.unit) * 4096.0f / (2.0f * kPi)));
  out.m = effects::Mat3::from_angles(follower.ang[0], ay, follower.ang[2]);
  out.t = {attach_pos[0] + follower.off[0], attach_pos[1] + follower.off[1], attach_pos[2] + follower.off[2]};
  return true;
}

BattleView::AnmFx& BattleView::anm_bank(int src) {
  const Visual& sv = vis_[static_cast<size_t>(src)];
  const battle::Unit& su = battle_->unit(src);
  const int cls = su.pc ? weapon_digit(su) : -1;
  const void* key_ptr = sv.scripts ? static_cast<const void*>(sv.scripts.get()) : nullptr;
  const std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(key_ptr)) + "#" + std::to_string(cls);
  std::shared_ptr<AnmFx>& slot = anm_[key];
  if (!slot) {
    auto anm = std::make_shared<AnmFx>();
    auto pack = std::make_shared<FxPack>();
    pack->eid = 9000 + anm_pack_seq_++;
    pack->resident = fxlib_.resident();
    if (sv.scripts) pack->prd = sv.scripts->at0;
    static const import::Vram kEmpty;
    if (!pack->prd.empty()) pack->fx = import::load_battle_effects(pack->prd, {}, pack->resident ? pack->resident->vram : kEmpty, {});
    if (cls >= 0 && !pack->prd.empty()) {
      // A PC's hit emitters ("<class digit><s|m|l>00", several headers each) sit in the nested drp at0.prd > hitm > "<d><d><d>h", one per weapon
      // class; the attacker's class ones are renamed to "_<s|m|l>00". Same digit choice as the sounds (battle_audio.cpp): the class,
      // else the nearest lower one that exists.
      const import::Vram vram = pack->build_vram();
      std::function<ByteSpan(ByteSpan, const std::string&)> find = [&](ByteSpan bytes, const std::string& entry_name) -> ByteSpan {
        auto drp = import::parse_drp(bytes);
        if (!drp) return {};
        for (const auto& entry : drp->entries) {
          if (entry.type != 1 || !import::is_drp(entry.data)) continue;
          if (entry.name == entry_name) return entry.data;
          if (ByteSpan found = find(entry.data, entry_name); !found.empty()) return found;
        }
        return {};
      };
      for (int digit = std::min(cls, 9); digit >= 0; --digit) {
        const ByteSpan nested = find(pack->prd, std::string(3, static_cast<char>('0' + digit)) + "h");
        if (nested.empty()) continue;
        auto extra = std::make_shared<assets::RoomEffects>(import::load_battle_effects(nested, {}, vram, {}));
        for (assets::Emitter& em : extra->emitters)
          if (em.name.size() == 4 && em.name[0] == static_cast<char>('0' + digit)) em.name[0] = '_';
        pack->extra = extra;
        break;
      }
    }
    // The same rename on the unit's own entries (monsters carry "_s00" already).
    for (assets::Emitter& em : pack->fx.emitters)
      if (cls >= 0 && em.name.size() == 4 && em.name[0] == static_cast<char>('0' + std::min(cls, 9)) && (em.name[1] == 's' || em.name[1] == 'm' || em.name[1] == 'l'))
        em.name[0] = '_';
    if (anm_trace()) {
      std::string names;
      for (const assets::Emitter& em : pack->fx.emitters) names += em.name + " ";
      if (pack->extra) for (const assets::Emitter& em : pack->extra->emitters) names += em.name + " ";
      log::info("anm: pack of unit {} (class {}): emitters {}", src, cls, names);
    }
    anm->pack = pack;
    // Registered stopped: they run when the script starts them.
    anm->bank.add_library(&pack->fx);
    if (pack->extra) anm->bank.add_library(pack->extra.get());
    // The nested drps that carry timelines (a fierce attack's at0.prd > <model> > fir1 / fir2 / fir3) link their emitters into the same registry.
    if (!pack->prd.empty()) {
      const import::Vram vram = pack->build_vram();
      std::function<void(ByteSpan)> link = [&](ByteSpan bytes) {
        auto drp = import::parse_drp(bytes);
        if (!drp) return;
        for (const auto& entry : drp->entries) {
          if (entry.type != 1 || !import::is_drp(entry.data)) continue;
          auto inner = import::parse_drp(entry.data);
          if (inner && std::any_of(inner->entries.begin(), inner->entries.end(), [](const import::DrpEntry& sub) { return sub.type == 0x10; }))
            anm->subpacks.push_back(std::make_shared<assets::RoomEffects>(import::load_battle_effects(entry.data, {}, vram, {})));
          link(entry.data);
        }
      };
      link(pack->prd);
      for (const auto& subpack : anm->subpacks) anm->bank.add_library(subpack.get());
    }
    if (pack->resident)
      for (const auto& resident_pack : pack->resident->packs) anm->bank.add_library(resident_pack.get());
    anm->bank.add_library(&fxlib_.particle_lib());
    anm_add_s6(anm->bank);
    anm_start_system(*anm);
    slot = std::move(anm);
  }
  return *slot;
}

void BattleView::anm_effect_for_target(int unit, const std::string& name, int target) {
  if (unit < 0 || unit >= battle::kUnits || name.empty()) return;
  AnmFx& anm = anm_bank(unit);
  const bool started = anm.em->start_for_target(name, target);
  if (anm_trace()) log::info("anm: tick {} unit {} timeline emitter '{}' for target {} -> {}", tick_count_, unit, name, target, started);
}

void BattleView::anm_effect(int unit, const bseq::Event& event) {
  if (unit < 0 || unit >= battle::kUnits || event.tag == 0) return;
  const std::string name = bseq::tag_str(event.tag);
  // The resources: a reaction runs the ATTACKER's table on the target, so its emitters are the attacker's; every other script is the unit's own.
  int src = unit;
  if (vis_[static_cast<size_t>(unit)].script_kind == kReact && act_.actor >= 0) src = act_.actor;
  const battle::Unit& su = battle_->unit(src);
  AnmFx& anm = anm_bank(src);
  // While an element pack runs, a MONSTER attacker's own at0 headers are not in the registry the pack's reaction script walks
  // (the attacker's at0 carries "0s00 / 0m00 / 0l00" but none of them is drawn; the sparks come from the pack).
  const bool pack_react = fx_pack_ && vis_[static_cast<size_t>(unit)].script_kind == kReact && !su.pc;
  if (!pack_react && !anm_attach(anm, unit, event, name, src)) return;
  // The whole effect registry is walked: the running element pack's own emitters of that name join the attacker's. A monster's
  // pack carries its hit sparks itself ("0s00" / "0m00" / "0l00", Just 4 Kicks has only the medium one: the kick that lands with class
  // 1 sparks, the small and large ones find no header) - the reaction script of the pack runs on the target and names them.
  if (fx_pack_ && (pack_react || std::any_of(fx_pack_->fx.emitters.begin(), fx_pack_->fx.emitters.end(), [&](const assets::Emitter& em) { return em.name == name; }))) {
    const std::string pkey = "pack#" + std::to_string(fx_pack_->eid);
    std::shared_ptr<AnmFx>& pslot = anm_[pkey];
    if (!pslot) {
      auto pa = std::make_shared<AnmFx>();
      pa->pack = fx_pack_;
      pa->bank.add_library(&fx_pack_->fx);
      if (fx_pack_->extra) pa->bank.add_library(fx_pack_->extra.get());
      if (fx_pack_->resident)
        for (const auto& resident_pack : fx_pack_->resident->packs) pa->bank.add_library(resident_pack.get());
      pa->bank.add_library(&fxlib_.particle_lib());
      anm_add_s6(pa->bank);
      anm_start_system(*pa);
      pslot = std::move(pa);
    }
    anm_attach(*pslot, unit, event, name, src);
  }
}

// The S6 DRP of every loaded unit model is a resident file of the registry. Units of one model share the loaded model, and
// a drp that already has an id is not opened again, so each distinct model registers its headers once.
void BattleView::anm_add_s6(effects::EffectBank& bank) const {
  std::vector<const assets::RoomEffects*> seen;
  for (int unit = 0; unit < battle::kUnits; ++unit) {
    const Visual& visual = vis_[static_cast<size_t>(unit)];
    if (!visual.scripts || !visual.scripts->s6fx || !battle_->unit(unit).present) continue;
    const assets::RoomEffects* fx = visual.scripts->s6fx.get();
    if (std::find(seen.begin(), seen.end(), fx) != seen.end()) continue;
    seen.push_back(fx);
    bank.add_library(fx);
  }
}

void BattleView::anm_start_system(AnmFx& anm) {
  {
    effects::EmitterSystem::Config cfg;
    cfg.seed = 1;
    cfg.pos_shift = 5;
    anm.em = std::make_unique<effects::EmitterSystem>(anm.bank, cfg);
    AnmFx* raw = &anm;
    effects::ActorHooks ah;
    ah.anchor_basis = [raw](int id, effects::Basis& out) {
      const auto it = raw->followers.find(id);
      if (it == raw->followers.end()) return false;
      out = it->second.basis;
      return true;
    };
    // One point: the unit's yaw at the point. A second point (c != 0, a projectile flying from the actor to a target): the look-at basis of
    // the effect packs, local +X from the first point to the second, pitched toward it, the X / Z columns scaled by the distance / 2048,
    // the origin at the midpoint.
    ah.actor_basis = [this](int sel, int joint, int second_sel, int second_joint, effects::Basis& out, int& scale) {
      scale = 4096;
      out = effects::Basis{};
      effects::Vec3 point{};
      float yaw = 0;
      if (!anm_point(sel, joint, point, yaw)) return true;
      effects::Vec3 point2{};
      float yaw2 = 0;
      if (second_sel != 0 && anm_point(second_sel, second_joint, point2, yaw2)) {
        const float dx = point2[0] - point[0], dy = point2[1] - point[1], dz = point2[2] - point[2];
        const float hd = std::hypot(dx, dz);
        const int yaw_a = -effects::ratan2(static_cast<int>(dz), static_cast<int>(dx));
        const int pitch_a = effects::ratan2(static_cast<int>(dy), static_cast<int>(hd));
        const float cp = std::max(0.05f, static_cast<float>(effects::rcos(pitch_a)) / 4096.0f);
        const float stretch = std::max(0.05f, hd * 2.0f / cp / 4096.0f);
        out.m = effects::Mat3::from_angles(0, yaw_a, pitch_a).scaled_columns(stretch, 1.0f, stretch);
        out.t = {(point[0] + point2[0]) * 0.5f, (point[1] + point2[1]) * 0.5f, (point[2] + point2[2]) * 0.5f};
        return true;
      }
      const float cs = std::cos(yaw), sn = std::sin(yaw);
      out.m.m = {cs, 0, sn, 0, 1, 0, -sn, 0, cs};
      out.t = point;
      return true;
    };
    anm.em->set_hooks(std::move(ah));
    anm.em->register_all();
  }
}

bool BattleView::anm_point(int sel, int joint, effects::Vec3& pos, float& yaw) {
  std::vector<int> targets = act_.targets;
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  int unit = -1;
  // The caster selectors are acting-list entry 0, which the scripted camera / actor ops rewrite
  // (script_actor_), so a scripted cutscene's play_anim after camera_target puts the unit (not the last action's actor) on the selector (Bomber's hiba).
  if (sel == 1 || sel == 9) unit = script_actor_ >= 0 ? script_actor_ : act_.actor;
  else if (sel == 2 || (sel >= 0xD && sel <= 0x15)) {
    const size_t index = sel == 2 ? 0 : static_cast<size_t>(sel - 0xD);
    unit = index < targets.size() ? targets[index] : -1;
  }
  if (anm_trace()) log::info("anm: op 0x39 selector {} joint {} -> unit {} (act actor {}, targets {})", sel, joint, unit, act_.actor, targets.size());
  if (unit < 0 || unit >= battle::kUnits) return false;
  if (!fx_attach_unit(unit, std::max(0, joint - 2), pos)) return false;
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  yaw = std::atan2(-visual.face_x, -visual.face_z);
  return true;
}

bool BattleView::anm_attach(AnmFx& anm, int unit, const bseq::Event& event, const std::string& name, int src) {
  AnmFx::Follower follower;
  follower.unit = unit;
  follower.point = event.b;
  follower.mode = event.a;
  follower.follow = (event.a & 0x80) != 0;
  for (int k = 0; k < 3; ++k) follower.off[static_cast<size_t>(k)] = event.off[k], follower.ang[static_cast<size_t>(k)] = event.ang[k];
  const auto attach = [this](int attach_unit, int point, effects::Vec3& out) { return fx_attach_unit(attach_unit, point, out); };
  const auto yaw_of = [this](int yaw_unit) {
    const Visual& visual = vis_[static_cast<size_t>(yaw_unit)];
    return std::atan2(-visual.face_x, -visual.face_z);
  };
  if (!anm_basis(follower, follower.basis, attach, yaw_of)) {
    if (anm_trace()) log::info("anm: unit {} op 1D {} point {}: no attach point", unit, name, event.b);
    return false;
  }
  const int id = anm.next_id++;
  int duration = 0;
  const int header_count = anm.em->attach_all(name, id, duration);
  follower.life = duration;
  if (header_count > 0) anm.followers[id] = follower;
  // Dev trace: which unit's at0 carries the name.
  if (header_count == 0 && anm_trace()) {
    for (int other = 0; other < battle::kUnits; ++other) {
      const Visual& uv = vis_[static_cast<size_t>(other)];
      if (!uv.scripts || uv.scripts->at0.empty() || !battle_->unit(other).present) continue;
      static const import::Vram kE;
      const assets::RoomEffects fxu = import::load_battle_effects(uv.scripts->at0, {}, kE, {});
      for (const assets::Emitter& em : fxu.emitters)
        if (em.name == name) { log::info("anm: '{}' exists in the at0 of unit {}", name, other); break; }
    }
  }
  if (anm_trace())
    log::info("anm: tick {} unit {} (res unit {}) op 1D '{}' mode {} point {} off ({},{},{}) -> {} emitter header(s), life {}", tick_count_, unit, src, name, event.a,
              event.b, event.off[0], event.off[1], event.off[2], header_count, duration);
  return true;
}

void BattleView::anm_each_system(const std::function<void(const effects::EmitterSystem&)>& fn) const {
  for (const auto& [key, ap] : anm_)
    if (ap->em) fn(*ap->em);
}

void BattleView::anm_tick() {
  trail_tick();
  if (anm_.empty()) return;
  const auto attach = [this](int attach_unit, int point, effects::Vec3& out) { return fx_attach_unit(attach_unit, point, out); };
  const auto yaw_of = [this](int yaw_unit) {
    const Visual& visual = vis_[static_cast<size_t>(yaw_unit)];
    return std::atan2(-visual.face_x, -visual.face_z);
  };
  for (auto& [key, ap] : anm_) {
    AnmFx& anm = *ap;
    for (auto& [id, follower] : anm.followers) {
      if (follower.follow) anm_basis(follower, follower.basis, attach, yaw_of);
      if (follower.life > 0 && --follower.life == 0) follower.follow = false;
    }
    anm.em->tick();
  }
}

// Mesh sprites (type 14) of the unit emitters name the resident battle.prd meshes (the `maru` orbs a rod's heavy hit fires at every target):
// they go through the stage pipeline as the effect packs' resident meshes do (object colour, blend, OT bias, ext cull sides, depth cue),
// before the sprite quads.
void BattleView::anm_draw_meshes(const render::Frame& frame, const render::Letterbox& lb, const effects::ViewParams& view) {
  std::vector<render::StageInstance> instances;
  FxGpu* resident = nullptr;
  const assets::StageAsset* asset = nullptr;
  for (auto& [key, ap] : anm_) {
    AnmFx& anm = *ap;
    std::vector<effects::EmitterSystem::MeshInstance> meshes;
    anm.em->collect_meshes(view, alpha_, meshes);
    if (meshes.empty()) continue;
    FxGpu* gpu = fx_gpu_resident(anm.pack);
    if (!gpu || !gpu->stage) continue;
    resident = gpu;
    asset = anm.pack->resident->meshes.get();
    for (const auto& mi : meshes) {
      if (!mi.name || mi.sprite_type != 14) continue;
      const auto part = gpu->parts.find(*mi.name);
      if (part == gpu->parts.end()) continue;
      if (static_cast<size_t>(part->second) < asset->parts.size() && mi.off_screen(asset->parts[static_cast<size_t>(part->second)].cull_points, view)) continue;
      render::StageInstance in;
      in.part = part->second;
      for (size_t k = 0; k < 9; ++k) in.cam[k] = mi.mat.m[k];
      for (size_t k = 0; k < 3; ++k) in.cam[9 + k] = mi.centre[k];
      for (size_t k = 0; k < 3; ++k) in.rgb[k] = mi.rgb[k];
      in.blend = mi.blend;
      in.ext = mi.ext_path;
      in.bias = mi.bias;
      in.fixed_slot = mi.fixed_slot;
      in.depth_cue = (mi.flags >> 17) & 1;
      // Ext bit 5: both sides, bit 4: the reverse side.
      if (mi.ext & 0x20) in.cull = 2;
      else if (mi.ext & 0x10) in.cull = 1;
      instances.push_back(in);
    }
  }
  if (instances.empty()) return;
  // Opaque instances first, the semi-transparent ones over them.
  std::stable_partition(instances.begin(), instances.end(), [](const render::StageInstance& in) { return in.blend == 0; });
  const float fw = static_cast<float>(frame.extent.width), fh = static_cast<float>(frame.extent.height);
  const float sx0 = std::clamp(std::round(lb.left()), 0.0f, fw), sy0 = std::clamp(std::round(lb.oy), 0.0f, fh);
  const render::Rect scissor{static_cast<int32_t>(sx0), static_cast<int32_t>(sy0),
                             static_cast<u32>(std::min(fw - sx0, std::round(lb.width()))), static_cast<u32>(std::min(fh - sy0, std::round(kBufferRows * lb.scale_y)))};
  render::StagePipeline::DepthCue cue;
  cue.active = fx_fog_on_;
  cue.r = fog_cur_.r, cue.g = fog_cur_.g, cue.b = fog_cur_.b, cue.near = fog_cur_.near, cue.far = fog_cur_.far;
  stage_pipe_.begin(frame, scissor);
  stage_pipe_.set_gte(view.h, view.ox, view.oy);
  stage_pipe_.set_depth_cue(cue);
  stage_pipe_.draw(*resident->stage, render::StagePipeline::projection(lb, view.h, view.ox, view.oy), instances, linear_filter_ < 0 ? hd_ : linear_filter_ != 0, asset);
  stage_pipe_.end();
  frame.set_scissor(scissor);
}

void BattleView::anm_draw(const render::Frame& frame, const render::Letterbox& lb) {
  trail_draw(frame, lb);
  if (anm_.empty()) return;
  const effects::ViewParams view = fx_view();
  anm_draw_meshes(frame, lb, view);
  bool any = false;
  for (auto& [key, ap] : anm_) {
    AnmFx& anm = *ap;
    anm.quads.clear();
    anm.em->collect_quads(view, alpha_, anm.quads);
    if (anm.quads.empty()) continue;
    FxGpu* gpu = fx_gpu_for(anm.pack);
    if (!gpu || !gpu->sprites) continue;
    effects::sort_draw_order(anm.quads);
    if (!any) {
      particles_.begin(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), fx_particle_pass_);
      fx_particle_pass_ = true;
      any = true;
    }
    for (const effects::DrawQuad& quad : anm.quads) gpu->sprites->draw_quad(particles_, quad, lb);
  }
  if (any) particles_.end();
}

}  // namespace oc::game
