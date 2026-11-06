// BattleView: per-unit animation and state (stances, spawns, moves, jumps, damage reactions, link and unit ticks).
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

#include "core/log.h"
#include "import/model.h"
#include "import/text.h"
#include "import/tim.h"

namespace oc::game {

using namespace bv;

int BattleView::target_unit(int idx) const {
  if (act_.targets.empty()) return -1;
  if (idx >= 0 && idx < static_cast<int>(act_.targets.size())) return act_.targets[static_cast<size_t>(idx)];
  return act_.targets.front();
}

// ------------------------------------------------------------------------------------------------------------ scripts
bool BattleView::start_script(int unit, const bseq::Table* table, int entry, int variant, Script kind) {
  Visual& visual = vis_[static_cast<size_t>(unit)];
  // Starting a script replaces the running one; the model keeps what entry 8 has written so far.
  if (visual.spawn && visual.vm.running())
    log::info("battle: unit {} spawn script (S5 entry 8) cut by entry {} variant {} at tick {}", unit, entry, variant, tick_count_);
  visual.spawn = visual.idle_owed = visual.victory_owed = false;
  if (!visual.has_model || !table || !visual.vm.start(table, entry, variant)) {
    visual.script_done = true;
    return false;
  }
  // Dev: OC_SEQ_TRACE logs every op of every script this unit runs.
  if (static const bool tr = std::getenv("OC_SEQ_TRACE") != nullptr; tr)
    visual.vm.trace = [unit, this](int pc, const s16* words, int len) {
      std::string text;
      for (int k = 0; k < len; ++k) text += std::format(" {:X}", words[k]);
      log::debug("seq: tick {} unit {} pc {}:{}", tick_count_, unit, pc, text);
    };
  visual.script_kind = kind;
  visual.script_done = false;
  if (kind == kApproach) move_wait_ = 1;
  else if (kind == kReturn) move_wait_ = 2;
  if (std::getenv("OC_SCRIPT_LOG")) log::debug("battle script: tick {} unit {} kind {} entry {} variant {}", tick_count_, unit, static_cast<int>(kind), entry, variant);
  return true;
}

// Ready stance at the enemy: the idle is restarted when the turn ends and by the top menu, never by the HIT% menu; a script end (ops 00 / 3E / 4C)
// restarts nothing. S5 entry 2 and the atk entry 0 variant 0 of all 51 PC models with scripts end on SetAnim 2 (yamaneko 1), and that clip loops
// until the return (S5 entry 3): clip 2 from the approach end to the pick, the at0 swing clip 0, clip 2 again, and clip 1 only after the return hop.
bool BattleView::engaged_stance(int unit) const {
  if (!engaged_ || unit != active_pc_ || unit < 0) return false;
  const int kind = vis_[static_cast<size_t>(unit)].script_kind;
  return kind == kApproach || kind == kAttack;
}

void BattleView::start_idle(int unit) {
  Visual& visual = vis_[static_cast<size_t>(unit)];
  if (!visual.has_model) return;
  // The spawn script runs the idle's own setup after its opening wait, then fades the model in: the restart waits for its end.
  if (visual.spawn && visual.vm.running()) {
    visual.idle_owed = true;
    return;
  }
  visual.has_react = false;
  visual.guard_shown = battle_->unit(unit).guard;
  const bseq::Table* table = visual.scripts && visual.scripts->has_s5 ? &visual.scripts->s5 : nullptr;
  if (!start_script(unit, table, 1, 0, kIdle)) {
    visual.script_kind = kIdle;
    visual.script_done = true;
    if (visual.pose.current_id() < 0) visual.pose.play(1, 0);
  }
}

// The spawn script. Every unit starts hidden on clip 1 and plays S5 entry 8 (an enemy only when the scene does not seed it off-stage: those run it
// when the battle script brings them on, play_anim_of - the Bulbs, 437). Entry 8 of all 191 battle models: tick 0 the battle-long model words
// (weapon slot 1 hide 0x7A, shadow 0x22, part hides 0x1C / blink 0x91, element tasks 0x6A / 0x6D) and Rand 4 (0x30) -> an opening wait of
// 4 / 8 / 12 / 0 ticks (the random stagger of the units' appearance), the idle's own setup (clip 1 loop, eye blink), colour 0 + ABR 3
// (0x1F / 0x20), wait 4, the 0x8E soft fade 0 -> 128 over 32 ticks with show 0x19, wait 32, colour 128 + ABR off, end (0x4C: no idle restart:
// the clip keeps looping). The clip stands still while the model is hidden; the element task of 0x6D turns on (7star element 16 turns
// 64 per tick from the spawn, hidden or not).
void BattleView::start_spawn(int unit) {
  Visual& visual = vis_[static_cast<size_t>(unit)];
  if (!visual.has_model || !visual.scripts || !visual.scripts->has_s5 || brain_.hidden(unit) || !start_script(unit, &visual.scripts->s5, 8, 0, kIdle)) {
    start_idle(unit);
    return;
  }
  // Hidden until the script's op 0x19 shows it.
  anm_set_hidden(unit, true);
  // The party also gets the eye blink armed before the script's own setup has run.
  if (unit < battle::kPcSlots) anm_eye_arm(unit);
  visual.spawn = true;
  visual.idle_owed = false;
  visual.has_react = false;
  visual.guard_shown = battle_->unit(unit).guard;
}

void BattleView::set_rot_now(int unit, int yaw4096) {
  Visual& visual = vis_[static_cast<size_t>(unit)];
  // A running RotateT is dropped; the facing is (-sin yaw, -cos yaw).
  visual.turn.on = false;
  const float yaw = static_cast<float>(yaw4096 & 0xFFF) * (2.0f * kPi / 4096.0f);
  visual.face_x = -std::sin(yaw);
  visual.face_z = -std::cos(yaw);
  visual.pface_x = visual.face_x;
  visual.pface_z = visual.face_z;
}

// Dev casts (ocbattleview --cast / --ecast, the Debug tab) skip the script that would queue the attack; a monster element with a caster group
// takes the list the script's 8D slots before its type-2 queue_attack give (Vm::probe_group, the vars as they stand). A PC's double / triple tech
// takes the party members the record's character bytes name, in that order.
void BattleView::dev_group_hint(int actor, int element) {
  if (element <= 0 || static_cast<size_t>(element) >= tables_->elements.size()) return;
  if (actor < battle::kPcSlots) {
    const auto& record = tables_->elements[static_cast<size_t>(element)];
    if (record.b(0x14) == 0xFF) return;
    std::array<u8, 6> group{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (int k = 0; k < 3; ++k)
      for (int pc = 0; pc < battle::kPcSlots; ++pc)
        if (battle_->unit(pc).present && battle_->unit(pc).src_id == record.b(0x14 + k)) group[static_cast<size_t>(k)] = static_cast<u8>(pc);
    battle_->set_group_hint(group);
    return;
  }
  if (!brain_.vm()) return;
  const int caster_count = tables_->elements[static_cast<size_t>(element)].b(0x17);
  if (caster_count == 0 || caster_count == 0xFF) return;
  if (const auto group = brain_.vm()->probe_group(element, actor)) battle_->set_group_hint(*group);
}

int BattleView::script_list_unit(int list, int idx) const {
  if (list == 2) return idx >= 0 && idx < battle::kUnits ? idx : -1;
  if (list == 0) {
    // Op 0x86 camera_target rewrites the acting list = {unit} after the command setup: while script_actor_ is set the list is that unit alone
    // (the Bulbs' arrival: GetPos list 0 idx 0 = the Bulb itself, not the Tree's last action).
    if (script_actor_ >= 0) return idx == 0 ? script_actor_ : -1;
    // The acting list (Act::casters): [actor], or a monster cast's group list (the script's 8D slots).
    if (act_.casters.empty()) return idx <= 0 ? act_.actor : -1;
    return idx >= 0 && static_cast<size_t>(idx) < act_.casters.size() ? act_.casters[static_cast<size_t>(idx)] : -1;
  }
  return idx >= 0 && idx < static_cast<int>(act_.targets.size()) ? act_.targets[static_cast<size_t>(idx)] : -1;
}

void BattleView::fill_ctx(int unit, bseq::Ctx& ctx) const {
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  const battle::Unit& unit_rec = battle_->unit(unit);
  if (act_.phase != Act::Phase::None && unit == act_.actor && !act_.ev.empty()) {
    const battle::ActionEvent& hit_event = act_.ev[std::min(act_.hit, act_.ev.size() - 1)];
    ctx.action_a = hit_event.strength;
    ctx.action_b = act_.hand;
  }
  const bool reacting = visual.script_kind == kReact && visual.has_react;
  ctx.hit_code = reacting ? visual.react_ev.code : 0;
  // Op 0x47 (GetHitType) reads the hit code of the entity running the script - for the CASTER's pack script that is its own. A steal writes the
  // result code (0 landed / 2 failed or nothing) into the three PC blocks AND the target's, so Pilfer's caster branches on it (steal_k main:
  // code 2 = sfx 'miss' + the frustration clip 0x82 instead of the bag clip 0x81). Other actions leave the caster's word as it was (not modelled: 0).
  // creature >= 0 = Big Catch (mode 39): its miss writes the code into the caster's block too.
  if (!reacting && unit == act_.actor && act_.phase != Act::Phase::None && !act_.ev.empty() && (act_.ev.front().steal_msg >= 0 || act_.ev.front().creature >= 0))
    ctx.hit_code = act_.ev[std::min(act_.hit, act_.ev.size() - 1)].code;
  // The scripts see the *presented* state: a unit the rules already killed stays alive on screen until the hit that kills it lands (ko_shown).
  // A table a pack / overlay started on the unit (kReact without a hit event: death tables, fx_model_requests) reads the state the rules
  // already wrote (hp 0 since the cast): that is where the "dead" voices of a multi-kill spell come from (Volcano: two "dead" voices after the
  // chant; with the presented state they never played).
  const bool pack_table = visual.script_kind == kReact && !visual.has_react;
  ctx.dead = reacting ? (visual.react_ev.ko || visual.ko_shown) : (pack_table ? (!unit_rec.alive() || visual.ko_shown) : visual.ko_shown);
  // A unit that owes a scripted death (op 5C, death cast parked by Battle::kill) has no death bit set: its idle / reaction scripts keep it
  // standing until the death cast has run.
  if (visual.death_owed) ctx.dead = false;
  // HP a battle script wrote to 0 is not a KO: the idle script stays up and reads the low-HP bit.
  if (unit_rec.hp_written_zero && unit_rec.hp <= 0) ctx.dead = false;
  // Flags: bit 0 dead, bit 9 (0x200) guarding (set by Defend, cleared when the PC's command menu opens: the idle script picks the block pose
  // from it), bit 31 HP <= max / 4 (or set by battle-script op 84 until the next hit).
  ctx.flags = (ctx.dead ? 1u : 0u) | (unit_rec.guard ? 0x200u : 0u) |
              (((visual.zero_hp_actions >= 2 || visual.low_hp_forced || visual.hp_shown * 4 <= unit_rec.max_hp) && !ctx.dead) ? 0x80000000u : 0u);
  // Equipment bits set once per party unit (read by anmseq ops 0xA6 / 0xA7 / 0xA8 and by the sky pack): 0x2000 weapon
  // item 0x26 (Mastermune), 0x4000 any accessory 0xC1 (Hero's Shield), 0x8000 the hero set (Unit::hero_set).
  if (unit_rec.pc) {
    if (unit_rec.eq.weapon_id == 0x26) ctx.flags |= 0x2000u;
    if (std::find(unit_rec.eq.acc.begin(), unit_rec.eq.acc.end(), u16{0xC1}) != unit_rec.eq.acc.end()) ctx.flags |= 0x4000u;
    if (unit_rec.hero_set) ctx.flags |= 0x8000u;
  }
  // Op 0x9F GetCommon (mode 41): the unit's own common word.
  ctx.common = unit_rec.ctrl12;
  ctx.option = unit_rec.gender;
  // Op 0xA9: a reaction reads its own hit's number (a miss: -1); the caster reads the word its element mode wrote.
  if (reacting) ctx.result = visual.react_ev.code == 2 ? -1 : visual.react_ev.value;
  if (act_.phase != Act::Phase::None && unit == act_.actor)
    for (const battle::ActionEvent& event : act_.ev)
      if (event.caster_word >= 0) ctx.result = event.caster_word;
  // Op 0xA1: the running action's target list.
  if (act_.phase != Act::Phase::None) {
    ctx.target_count = static_cast<int>(act_.targets.size());
    const auto listed = std::find(act_.targets.begin(), act_.targets.end(), unit);
    if (listed != act_.targets.end()) ctx.target_index = static_cast<int>(listed - act_.targets.begin());
  }
  // The spawn script's Rand 4 (its opening wait, the units' appearance stagger) draws from its own per-battle stream, so the battle's other
  // draws (the intro pick, later scripts: the Bulbs' own entry 8 in 437) stay where they were.
  if (visual.spawn) ctx.rand = [this]() { return static_cast<int>(spawn_rng_() & 0x7FFF); };
  ctx.status_lo = unit_rec.lo;
  ctx.status_hi = unit_rec.hi;
  ctx.attrib = visual.attrib;
  // A Dopplegang creature form runs the creature's scripts.
  ctx.monster = !unit_rec.pc || unit_rec.form >= 0;
  ctx.model_num = ModelCache::model_number(*tables_, unit_rec);
  ctx.yaw = static_cast<int>(std::lround(std::atan2(-visual.face_x, -visual.face_z) * (4096.0f / (2.0f * kPi)))) & 0xFFF;
  ctx.stand_x = static_cast<int>(visual.home_x);
  ctx.stand_z = static_cast<int>(visual.home_z);
  // Script op 3D sets the pose; 1 = stand-by.
  ctx.pose = visual.script_pose;
  ctx.clip_frames = [&visual](int id) { return visual.pose.frames_of(id); };
  ctx.pos_x = static_cast<int>(std::lround(visual.x));
  ctx.pos_z = static_cast<int>(std::lround(visual.z));
  ctx.pos_y = static_cast<int>(std::lround(-visual.y));
  // Op 0x81 GetELPos (fx_attach_unit already returns PS1 y down).
  ctx.get_attach = [this](int list, int idx, int point, int& ox, int& oz, int& oy) {
    const int listed = script_list_unit(list, idx);
    effects::Vec3 world{};
    if (listed < 0 || !const_cast<BattleView*>(this)->fx_attach_unit(listed, point, world, true)) return false;
    ox = static_cast<int>(std::lround(world[0]));
    oy = static_cast<int>(std::lround(world[1]));
    oz = static_cast<int>(std::lround(world[2]));
    return true;
  };
  // Op 0x8C: attach point of the running unit's own model.
  ctx.get_attach_self = [this, unit](int point, int& ox, int& oz, int& oy) {
    effects::Vec3 world{};
    if (!const_cast<BattleView*>(this)->fx_attach_unit(unit, point, world, true)) return false;
    ox = static_cast<int>(std::lround(world[0]));
    oy = static_cast<int>(std::lround(world[1]));
    oz = static_cast<int>(std::lround(world[2]));
    return true;
  };
  ctx.get_pos_y = [this](int list, int idx) {
    const int listed = script_list_unit(list, idx);
    return listed < 0 ? 0 : static_cast<int>(std::lround(-unit_world(listed, 1.0f)[1]));
  };
  // Op 74: list 0 = the action's casters, else its targets.
  ctx.get_pos = [this, unit](int list, int idx, int mode, int& ox, int& oz) {
    const int listed = script_list_unit(list, idx);
    if (listed < 0) return false;
    const Visual& other = vis_[static_cast<size_t>(listed)];
    float px = other.x, pz = other.z;
    // Modes 1-4: origin + R(yaw) * the joint extent (max x / min x / min z / max z).
    if (mode >= 1 && mode <= 4 && other.has_model) {
      std::vector<render::Mat4> pal;
      other.pose.palette(1.0f, pal);
      float mn_x = 1e9f, mx_x = -1e9f, mn_z = 1e9f, mx_z = -1e9f;
      for (const render::Mat4& joint : pal) {
        mn_x = std::min(mn_x, joint.at(0, 3));
        mx_x = std::max(mx_x, joint.at(0, 3));
        mn_z = std::min(mn_z, joint.at(2, 3));
        mx_z = std::max(mx_z, joint.at(2, 3));
      }
      if (!pal.empty()) {
        const float lx = mode == 1 ? mx_x : (mode == 2 ? mn_x : 0.0f), lz = mode == 3 ? mn_z : (mode == 4 ? mx_z : 0.0f);
        const float yaw = std::atan2(-other.face_x, -other.face_z), cs = std::cos(yaw), sn = std::sin(yaw);
        px += cs * lx + sn * lz;
        pz += -sn * lx + cs * lz;
      }
    // Toward the other model by the distance minus both reaches (mode 0x100 + n: walker attach n, op 0x97).
    } else if ((mode == 5 || mode >= 0x100) && listed != unit) {
      const Visual& me = vis_[static_cast<size_t>(unit)];
      const float dx = other.x - me.x, dz = other.z - me.z, dist = std::hypot(dx, dz);
      if (dist >= 1.0f) {
        const float run = approach_distance(unit, listed, mode >= 0x100 ? mode - 0x100 : 15);
        px = me.x + dx / dist * run;
        pz = me.z + dz / dist * run;
      }
    // Mode 6: the midpoint of the targets' bounding box, not their mean.
    } else if (mode == 6 && !act_.targets.empty()) {
      float lx = 1e9f, hx = -1e9f, lz = 1e9f, hz = -1e9f;
      for (int target : act_.targets) {
        lx = std::min(lx, vis_[static_cast<size_t>(target)].x);
        hx = std::max(hx, vis_[static_cast<size_t>(target)].x);
        lz = std::min(lz, vis_[static_cast<size_t>(target)].z);
        hz = std::max(hz, vis_[static_cast<size_t>(target)].z);
      }
      px = std::trunc((std::round(hx) + std::round(lx)) * 0.5f);
      pz = std::trunc((std::round(hz) + std::round(lz)) * 0.5f);
    }
    ox = static_cast<int>(std::lround(px));
    oz = static_cast<int>(std::lround(pz));
    return true;
  };
  ctx.move_ticks = [this, unit](int idx, int speed) {
    const int target = target_unit(idx);
    if (target < 0 || speed <= 0) return 30;
    return std::max(1, static_cast<int>(std::ceil(approach_distance(unit, target) / static_cast<float>(speed))));
  };
}

// The walk ends where the attacker's attach point 15 meets the target's attach point 16, i.e. distance minus both reaches.
float BattleView::approach_distance(int self, int target, int attach) const {
  const Visual& src = vis_[static_cast<size_t>(self)];
  const Visual& dst = vis_[static_cast<size_t>(target)];
  const float dist = std::hypot(dst.x - src.x, dst.z - src.z);
  // The POSED attach points are measured (on the model's current clip frame; attach 15 sits on the root joint 1408 units
  // in front, so a clip that pitches / turns the root shortens it: G-Force's first run-in stops at posed 1367 + 222, not rest 1408 + 222).
  // Op 0x97 names the walker's attach point (15 elsewhere).
  float r15 = src.reach15, r16 = dst.reach16, ax = 0, ay = 0, az = 0;
  if (src.has_model && src.pose.attach_now(attach, 1.0f, ax, ay, az)) r15 = std::hypot(ax, az);
  if (dst.has_model && dst.pose.attach_now(16, 1.0f, ax, ay, az)) r16 = std::hypot(ax, az);
  // Dev: OC_APPROACH_TRACE logs the reach terms of every approach.
  if (static const bool tr = std::getenv("OC_APPROACH_TRACE") != nullptr; tr)
    log::info("approach: tick {} unit {} -> {} dist {:.0f} r15 {:.0f} r16 {:.0f} attach {} clip {}", tick_count_, self, target, dist, r15, r16, attach, src.pose.current_clip());
  // Dev: OC_APPROACH_CLIPS logs the reach of attach 15 in every clip, frame by frame (clip ids: plain 0.. (monster hosts) and 65536.. (the
  // pack-hosted ids)), then the target's attach 16 (the other reach term) per clip frame.
  if (static const bool tr2 = std::getenv("OC_APPROACH_CLIPS") != nullptr; tr2 && src.has_model)
    for (int clip = 0; clip < 65536 + 24; clip = (clip == 23 ? 65536 : clip + 1)) {
      std::string row;
      for (int frame = 0; frame < 40; ++frame) {
        float qx, qy, qz;
        if (!src.pose.attach_in_clip(15, clip, static_cast<float>(frame), qx, qy, qz)) break;
        row += std::format(" {:.0f}", std::hypot(qx, qz));
      }
      if (!row.empty()) log::info("approach clip {}: {}", clip, row);
    }
  if (static const bool tr3 = std::getenv("OC_APPROACH_CLIPS") != nullptr; tr3 && dst.has_model)
    for (int clip = 0; clip < 24; ++clip) {
      std::string row;
      for (int frame = 0; frame < 40; ++frame) {
        float qx, qy, qz;
        if (!dst.pose.attach_in_clip(16, clip, static_cast<float>(frame), qx, qy, qz)) break;
        row += std::format(" {:.0f}", std::hypot(qx, qz));
      }
      if (!row.empty()) log::info("approach tgt16 clip {} (now {}): {}", clip, dst.pose.current_clip(), row);
    }
  float reach = r15 + r16;
  // Models without attach points: a fixed standoff.
  if (reach < 1.0f) reach = 450.0f;
  return std::max(0.0f, dist - reach);
}

void BattleView::begin_move(int unit, float tx, float tz, int ticks, bool home, float ty) {
  Visual& visual = vis_[static_cast<size_t>(unit)];
  visual.move.on = ticks > 0;
  visual.move.fx = visual.x;
  visual.move.fz = visual.z;
  visual.move.tx = tx;
  visual.move.tz = tz;
  visual.move.fy = visual.y;
  visual.move.ty = ty;
  visual.move.jump = false;
  visual.move.total = visual.move.left = std::max(1, ticks);
  visual.move.to_home = home;
  if (ticks <= 0) {
    visual.x = tx;
    visual.z = tz;
    // No yaw write on a home move (see tick: RetMoveT keeps the facing).
    visual.y = ty;
  }
}

// Op 6E JumpT (g = 0x9ccc): x / z are linear over `ticks`, y (PS1 y down, 20.12) starts with
// vy = dy / n - g * (n / 2) and each tick does y += vy; vy += g after a half-step back on the first tick (y -= vy / 2), so the unit rises by about
// g n^2 / 8 (n 16: 313 units) and lands exactly on the target (the task snaps the final position, then clears the velocity).
void BattleView::begin_jump(int unit, float tx, float tz, float ty, int ticks, int mode) {
  Visual& visual = vis_[static_cast<size_t>(unit)];
  const int steps = std::max(1, ticks);
  // g = 0x9ccc >> 12 units per tick^2; dy_down = target - current, PS1 y down.
  const float gravity = 40140.0f / 4096.0f;
  const float dy_down = -(ty - visual.y);
  visual.move.on = true;
  visual.move.fx = visual.x;
  visual.move.fz = visual.z;
  visual.move.tx = tx;
  visual.move.tz = tz;
  visual.move.fy = visual.y;
  visual.move.ty = ty;
  visual.move.total = visual.move.left = steps;
  visual.move.to_home = false;
  visual.move.jump = true;
  visual.move.first = true;
  visual.move.g = gravity;
  visual.move.vy = dy_down / static_cast<float>(steps) - gravity * static_cast<float>(steps / 2);
  // Ops 0x7F / 0x80: the same task with a parabola fitted to dy over n ticks instead of the fixed gravity.
  // 7F: a = -2 dy / n^2 (PS1 y down), vy0 = -a n = 2 dy / n.
  if (mode == 1) {
    visual.move.g = -2.0f * dy_down / static_cast<float>(steps * steps);
    visual.move.vy = -visual.move.g * static_cast<float>(steps);
  // 80: vy0 = 0, a = 2 dy / n^2.
  } else if (mode == 2) {
    visual.move.g = 2.0f * dy_down / static_cast<float>(steps * steps);
    visual.move.vy = 0.0f;
  }
}

std::array<float, 3> BattleView::unit_world(int unit, float alpha) const {
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  if (const effects::FxModels* models = fx_ ? fx_->bespoke_models() : nullptr; models && static_cast<size_t>(unit) < models->units.size() && models->units[static_cast<size_t>(unit)].steer) {
    const effects::FxModelPose op = effects::fx_model_pose(models->units[static_cast<size_t>(unit)], alpha);
    return {op.pos[0], op.pos[1], op.pos[2]};
  }
  return {lerp(visual.px, visual.x, alpha), lerp(visual.py, visual.y, alpha), lerp(visual.pz, visual.z, alpha)};
}

void BattleView::face_toward(int unit, int target) {
  if (target < 0) return;
  const Visual& target_visual = vis_[static_cast<size_t>(target)];
  face_point(unit, target_visual.x, target_visual.z);
}

void BattleView::face_point(int unit, float px, float pz) {
  Visual& visual = vis_[static_cast<size_t>(unit)];
  // A unit that has died (its pose word is 8) is not turned.
  if (visual.ko_shown) return;
  const float dx = px - visual.x, dz = pz - visual.z;
  const float length = std::hypot(dx, dz);
  if (length < 1.0f) return;
  visual.face_x = dx / length;
  visual.face_z = dz / length;
  visual.pface_x = visual.face_x;
  visual.pface_z = visual.face_z;
}

void BattleView::play_tag_sound(const std::string& tag, int volume) {
  if (!audio_.play(tag, volume)) log::debug("battle audio: no sound '{}'", tag);
}

void BattleView::on_script_event(int unit, const bseq::Event& event) {
  using K = bseq::Event;
  Visual& visual = vis_[static_cast<size_t>(unit)];
  switch (event.kind) {
    case K::SetAnim:
      // The idle script re-asks for the clip it already plays.
      if (visual.script_kind == kIdle && event.a == visual.pose.current_id()) break;
      if (static const bool tr = std::getenv("OC_REACH_TRACE") != nullptr; tr)
        log::info("setanim: tick {} unit {} id {} flags {:X} (was id {} clip {}) kind {}", tick_count_, unit, event.a, event.b, visual.pose.current_id(), visual.pose.current_clip(), static_cast<int>(visual.script_kind));
      visual.pose.play(event.a, event.b & 0x1FFF, (event.b & 0x8000) != 0, (event.b & 0x6000) >> 13);
      break;
    case K::Speed: visual.pose.set_speed(event.a); break;
    case K::Loop: visual.pose.set_loop(event.a != 0); break;
    case K::Sfx: {
      // Op 0x7B: centre pan, the operand's volume.
      if (event.b == 0) {
        play_tag_sound(bseq::tag_str(event.tag), event.a);
        break;
      }
      // Op 0x33: the unit's anchor (part 0x10) projected: pan = (clamp(sx - 160, +-160) * 0x30) / 0xA0 + 0x80,
      // volume = ((0x1FFF - min(SZ3 >> 2, 0x1FFF)) * 0x60 >> 13) + 0x20. The anchor is attach point 0x10 of the posed model in the PS1 world
      // through fx_attach_unit; a model without that point falls back to the middle of the body.
      int pan8 = 0x80, vol = 127;
      float sx = 160.0f, sy = 0.0f;
      float ax = visual.x, ay = visual.height * 0.5f, az = visual.z;
      // PS1 y down -> the camera's y up.
      if (effects::Vec3 ap{}; fx_attach_unit(unit, 0x10, ap)) ax = ap[0], ay = -ap[1], az = ap[2];
      if (cam_.screen(ax, ay, az, sx, sy)) {
        const float zc = cam_.fwd[0] * (ax - cam_.eye[0]) + cam_.fwd[1] * (ay - cam_.eye[1]) + cam_.fwd[2] * (az - cam_.eye[2]);
        const int rel = std::clamp(static_cast<int>(sx) - 0xA0, -0xA0, 0xA0);
        pan8 = rel * 0x30 / 0xA0 + 0x80;
        const int dist = std::min(static_cast<int>(zc) >> 2, 0x1FFF);
        vol = std::clamp((((0x1FFF - dist) * 0x60) >> 13) + 0x20, 0, 0x7F);
      }
      if (!audio_.play(bseq::tag_str(event.tag), vol, pan8 / 2)) log::debug("battle audio: no sound '{}'", bseq::tag_str(event.tag));
      break;
    }
    case K::Sequence:
      // anmseq 0x68 / 0x7D: the running effect pack's sequence of that tag first (techs without 'main' are
      // driven this way), else the battle's camera timelines.
      if (std::getenv("OC_FXDBG")) log::debug("fxdbg: tick {} unit {} Sequence '{}' (pack run {})", tick_count_, unit, bseq::tag_str(event.tag), fx_ != nullptr);
      if (fx_ && fx_->start_sequence(bseq::tag_str(event.tag))) break;
      // S5 entry 5 of a monster runs only inside the charge (colour byte set); a colourless cast (the Portalgheist door,
      // element 359) never starts the model's 'opb2' camera - the previous shot is held there.
      if (visual.script_kind == kCast && unit >= battle::kPcSlots && !model_cam_) break;
      if (!camera_.play_sequence(bseq::tag_str(event.tag))) {
        // The resident S6 timeline of a present unit (see K::Camera).
        if (const CamBank* bank = resident_cam_bank(bseq::tag_str(event.tag), true)) {
          camera_.set_bank(bank);
          camera_.play_sequence(bseq::tag_str(event.tag));
        } else {
          log::debug("battle camera: no timeline '{}'", bseq::tag_str(event.tag));
        }
      }
      break;
    case K::Camera: {
      const std::string cam_name = bseq::tag_str(event.tag);
      if (camera_.select(cam_name)) break;
      // Every model's S6 DRP is a resident registry file, so the by-name lookups (shots and timelines) also find a present unit's own S6 entry
      // when the active bank is another model's (Bomber's `bom0` shot, Family Tree's `hanl` timeline + shots: a cutscene's play_anim runs with no
      // action, so the bank is the battle-wide one). The model's own bank (parent = the battle bank) stays active, like an action of that model,
      // so the timeline's own later shot commands resolve too; the next action start resets it.
      if (const CamBank* bank = resident_cam_bank(cam_name, false)) {
        camera_.set_bank(bank);
        camera_.select(cam_name);
      }
      break;
    }
    // CallDamage shows the calling unit's own result in the running action. A unit no event of the action reaches has none (an attack script
    // that calls it on the attacker after its target's reaction): nothing shows, no flash, and its HP display stays.
    case K::Damage:
      if (fx_land(unit)) break;
      if (act_.phase != Act::Phase::None && std::none_of(act_.ev.begin(), act_.ev.end(), [unit](const battle::ActionEvent& ev) { return ev.target == unit; })) break;
      on_damage(unit);
      break;
    case K::React: {
      if (act_.ev.empty()) break;
      // CallDamTarget(All) of a pack's host script: the targets run entry 0x80 variant 7 of the pack's atk table.
      if (fx_ && act_.phase != Act::Phase::Hit) {
        fx_react(event.a, unit);
        break;
      }
      const battle::ActionEvent& he = act_.ev[std::min(act_.hit, act_.ev.size() - 1)];
      if (act_.phase == Act::Phase::Hit) {
        // Targets of this hit (one event = one target).
        if (he.target >= 0) {
          if (he.target == unit) {
            vis_[static_cast<size_t>(unit)].react_ev = he;
            on_damage(unit);
          } else {
            start_reaction(he.target, he);
          }
        }
      }
      break;
    }
    case K::MoveSpeed:
    case K::MoveTime: {
      const int target = target_unit(event.a);
      if (target < 0) break;
      const Visual& target_visual = vis_[static_cast<size_t>(target)];
      const float dx = target_visual.x - visual.x, dz = target_visual.z - visual.z;
      const float dist = std::hypot(dx, dz);
      if (dist < 1.0f) break;
      const float run = approach_distance(unit, target);
      const int ticks = event.kind == K::MoveSpeed ? std::max(1, event.c) : std::max(1, event.b);
      begin_move(unit, visual.x + dx / dist * run, visual.z + dz / dist * run, ticks, false);
      break;
    }
    case K::MoveHome: begin_move(unit, visual.home_x, visual.home_z, std::max(1, event.b), true); break;
    case K::MovePos: begin_move(unit, static_cast<float>(event.a), static_cast<float>(event.b), std::max(1, event.d), false, -static_cast<float>(event.c)); break;
    case K::Link: {
      const int victim = script_list_unit(event.a, event.b);
      if (victim < 0 || victim == unit) break;
      // A new link first frees an older link of the same unit.
      std::erase_if(links_, [victim](const Link& link) { return link.victim == victim; });
      links_.push_back({unit, victim, event.c, event.d, event.off[0] > 0 ? event.off[0] : -1});
      break;
    }
    case K::SetPos: begin_move(unit, static_cast<float>(event.a), static_cast<float>(event.b), 0, false, -static_cast<float>(event.c)); break;
    // SetRot: the yaw is set at once, but applied once the tick's effect pass has run (set_rot_now): a pack's camera shot of the same tick is
    // framed from the yaw the unit had when the pack timeline started it.
    case K::SetRot:
      pending_rot_.push_back({unit, event.a});
      break;
    // Shadow (op 0x21 / 0x22): set / clear, stays for the battle.
    case K::Shadow: shadow_off_[static_cast<size_t>(unit)] = event.a == 0; break;
    // SlowK (op 0x82): pack op 0x56's k.
    case K::SlowK: fx_slow_x_ = event.a; fx_slow_phase_ = 0; break;
    case K::ScreenFade: {  // op 0x52: a full-screen quad ramping from -> to (bytes) over a ticks; a < 1 makes no task
      if (event.a < 1) break;
      ScreenColour screen;
      for (size_t k = 0; k < 3; ++k) screen.from[k] = static_cast<float>(event.off[k]), screen.to[k] = static_cast<float>(event.ang[k]), screen.cur[k] = screen.from[k];
      screen.mode = std::clamp(event.b, 0, 4);
      screen.ticks = event.a;
      screen.ramping = true;
      anm_screen_.push_back(screen);
      break;
    }
    case K::Jump: begin_jump(unit, static_cast<float>(event.a), static_cast<float>(event.b), -static_cast<float>(event.c), event.d, static_cast<int>(event.tag)); break;
    case K::Face: face_toward(unit, target_unit(event.a)); break;
    // RotateT: yaw a (4096 = turn) reached over b ticks; facing = (-sin yaw, -cos yaw): the model forward is local -Z rotated by RotY(yaw).
    case K::Rotate: {
      visual.turn.on = true;
      if (std::getenv("OC_FXDBG")) log::debug("fxdbg: tick {} unit {} RotateT yaw {} over {} ticks (script kind {})", tick_count_, unit, event.a, event.b, static_cast<int>(visual.script_kind));
      visual.turn.from = std::atan2(-visual.face_x, -visual.face_z);
      visual.turn.to = rotate_end(visual.turn.from, event.a & 0xFFF);
      visual.turn.total = visual.turn.left = std::max(1, event.b);
      break;
    }
    case K::TrailColors: case K::TrailMode: case K::TrailFade: case K::TrailOn: case K::TrailOff: case K::ColorRamp: case K::LightPos: case K::LightSet: case K::ModelShow: case K::PartShow: case K::ModelColor: case K::ModelAbr: case K::WeaponShow: case K::Blink: case K::Eye:
    case K::ModelScale: case K::ModelFade: case K::PartScale: case K::PartScaleStop: case K::PartBlink: case K::PartBlinkStop: case K::ElemMotion: case K::ElemRot: case K::ElemRotRate: case K::ElemTilt: case K::ElemTiltBack: case K::PartColor:
      trail_event(unit, event);
      break;
    // Op 0x1D: emitters on an attach point (battle_view_anm.cpp).
    case K::Effect: anm_effect(unit, event); break;
    case K::PoseWord: visual.script_pose = event.a; break;
    case K::HeadLook: {
      visual.head_unit = script_list_unit(event.a, event.b);
      visual.head_point = event.c;
      float target[3];
      if (head_target_model(unit, target)) visual.pose.head_look(target, 0xE00);
      break;
    }
    case K::HeadRelease:
      visual.head_unit = -1;
      visual.pose.head_release();
      break;
    case K::TexRect:
      tex_flip_setup_.rect[static_cast<size_t>(event.b & 1)] = {event.off[0], event.off[1], event.off[2], event.d};
      break;
    case K::TexDest:
      tex_flip_setup_.unit = script_list_unit(event.a == 0 ? 0 : 1, event.b);
      tex_flip_setup_.dx = event.c;
      tex_flip_setup_.dy = event.d;
      break;
    case K::TexFlip:
      tex_flips_.push_back({tex_flip_setup_, {event.a, event.b}, 0, 1, event.c});
      break;
    case K::Done:
      visual.script_done = true;
      if (event.a == 0x3E) move_wait_ = 0;
      break;
  }
}

void BattleView::start_reaction(int unit, const battle::ActionEvent& event) {
  Visual& target_visual = vis_[static_cast<size_t>(unit)];
  if (!target_visual.has_model) {
    target_visual.react_ev = event;
    on_damage(unit);
    return;
  }
  target_visual.react_ev = event;
  target_visual.has_react = true;
  const Visual& actor_visual = vis_[static_cast<size_t>(act_.actor)];
  const bseq::Table* table = actor_visual.scripts && actor_visual.scripts->has_atk ? &actor_visual.scripts->atk : &model_cache_.fallback_atk;
  if (!start_script(unit, table, 0, 7, kReact) && !start_script(unit, &model_cache_.fallback_atk, 0, 7, kReact)) on_damage(unit);
}

// CallDamage's number spawn. The offset layout applies when the target list has a second entry (a group hit), or for op 0x6B (the caster's own
// number). Without it every digit spawns at the view-space origin, i.e. the number is drawn around the projection centre whatever the camera and
// the unit's size (a "3" rests at view x 192 / y -24 = 6 px right of the centre). With it the unit's attach point 9 is projected with the current
// camera, clamped to x +-130 / y -90..40 px and the digits are shifted by it (<< 5); every live earlier offset number it overlaps delays it 16 ticks.
void BattleView::spawn_pop(int unit, int value, int kind, int code) {
  Pop pop;
  pop.value = value;
  pop.kind = kind;
  pop.code = code;
  const int digit_count = kind == 3 ? 1 : static_cast<int>(std::to_string(std::max(0, value)).size());
  if (pop_caster_layout_ || act_.targets.size() >= 2) {
    std::array<float, 3> world{};
    float yaw = 0;
    if (unit_ref_point(unit, 0xB, world, yaw)) {
      const float to_eye[3] = {world[0] - cam_.eye[0], world[1] - cam_.eye[1], world[2] - cam_.eye[2]};
      auto dot3 = [&](const std::array<float, 3>& axis) { return axis[0] * to_eye[0] + axis[1] * to_eye[1] + axis[2] * to_eye[2]; };
      // PS1 view axes (Cam::screen).
      const std::array<int, 2> anchor = bnum::anchor_px(dot3(cam_.right), dot3(cam_.down), dot3(cam_.fwd), cam_.h);
      pop.ax = anchor[0];
      pop.ay = anchor[1];
    }
    // The miss branch passes 3 digits.
    pop.delay = pop_rects_.add(pop.ax, pop.ay, kind == 3 ? 3 : digit_count);
  }
  // Digit k (0 = leftmost) of n waits 4 (k + 1) + delay ticks; the miss glyph waits only the overlap delay.
  pop.end = bnum::digit_ticks(bnum::flight_for_code(code), kind == 3 ? pop.delay : 4 * digit_count + pop.delay);
  log::debug("battle: number unit {} value {} code {} anchor ({}, {}) delay {} at tick {}", unit, value, code, pop.ax, pop.ay, pop.delay, tick_count_);
  pops_.push_back(pop);
}

// Op 0x41 CallDamage: the hit lands on this unit.
void BattleView::on_damage(int unit, bool number) {
  Visual& visual = vis_[static_cast<size_t>(unit)];
  const battle::Unit& unit_rec = battle_->unit(unit);
  const battle::ActionEvent& event = visual.react_ev;
  // Mode 50: no number, no HP change, no flash (kamei 411 / the death casts).
  if (event.silent) return;
  if (!number) {
    // The reaction script that landed it has no Damage op: no number, no flash - only the HP / KO below.
  } else if (event.code == 2) {
    spawn_pop(unit, 0, 3, event.code);
  } else if (event.code == 3 && event.value > 0) {
    spawn_pop(unit, event.value, 2, event.code);
  } else if (event.value > 0 || event.gated) {
    spawn_pop(unit, event.value, unit_rec.pc ? 1 : 0, event.code);
    // A reaction script lights the unit itself (ops 0x64 / 0x5C / 0x5E).
    if (visual.script_kind != kReact) visual.flash = 1.0f;
  }
  if (event.code != 2) {
    visual.hp_shown = event.hp_after;
    visual.low_hp_forced = false;
  }
  // Pilfer or a status / field element (Frozen row 6): the record's btmes row opens the system message window
  // when the action ends (window 0 at (32, 144), 120 ticks); a whole-side cast queues one record (sys_req_ is shared).
  if (const int msg = event.steal_msg >= 0 ? event.steal_msg : event.sys_msg; msg >= 0) {
    ScriptMsgReq req;
    req.sys = true;
    req.unit = event.actor;
    if (const std::string* row = lang_tables_.battle_message(static_cast<u32>(msg), lang_); row && !row->empty()) {
      // The record's timer: 0x78 ticks once the action has ended.
      req.text = *row + "{wend(120)}";
      if (event.steal_msg >= 0 && event.steal_msg <= 1) {
        const int id = event.steal_word & 0x7FFF;
        const std::string* nm = event.steal_msg == 1 ? lang_tables_.sysmes((0x17u << 16) | static_cast<u32>(id), lang_) : nullptr;
        req.item = event.steal_msg == 1 ? (nm ? *nm : tables_->element_name(id)) : import::u32_to_utf8(lang_tables_.item_name(id, lang_));
      }
      req.has_pos = true;
      req.x = 32;
      req.y = 144;
      sys_req_ = req;
      sys_pending_ = true;
    }
  }
  if (event.ko) {
    visual.ko_shown = visual.ko_hit = true;
    visual.ko_reacted = visual.script_kind == kReact && visual.has_react && visual.react_ev.ko;
    if (event.ko_owed) visual.death_owed = true;
    // No reaction script (element hit): the idle script re-reads the death flag (op 0x48) and plays the death clips; restart it if it had ended.
    if (!visual.has_react && visual.script_kind == kIdle && visual.script_done) start_idle(unit);
  }
}

// -------------------------------------------------------------------------------------------------------------- units
void BattleView::tick_links() {
  for (Link& link : links_) {
    effects::Vec3 carrier_pos{}, victim_pos{};
    const bool okc = fx_attach_unit(link.carrier, link.carrier_attach, carrier_pos, true), okv = fx_attach_unit(link.victim, link.victim_attach, victim_pos, true);
    if (std::getenv("OC_FXDBG")) log::debug("fxdbg: link {} -> {} attach {} {} ok {} {} c {:.0f} {:.0f} {:.0f} w {:.0f} {:.0f} {:.0f}", link.carrier, link.victim, link.carrier_attach, link.victim_attach, okc, okv, carrier_pos[0], carrier_pos[1], carrier_pos[2], victim_pos[0], victim_pos[1], victim_pos[2]);
    if (!okc || !okv) continue;
    Visual& visual = vis_[static_cast<size_t>(link.victim)];
    visual.x += carrier_pos[0] - victim_pos[0];
    visual.z += carrier_pos[2] - victim_pos[2];
    // PS1 y down.
    visual.y -= carrier_pos[1] - victim_pos[1];
    visual.px = visual.x;
    visual.pz = visual.z;
    visual.py = visual.y;
    if (link.left > 0) --link.left;
  }
  std::erase_if(links_, [](const Link& link) { return link.left == 0; });
}

bool BattleView::head_target_model(int unit, float target[3]) {
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  effects::Vec3 world{};
  if (visual.head_unit < 0 || !visual.has_model || !fx_attach_unit(visual.head_unit, visual.head_point, world)) return false;
  // fx_attach_unit gives PS1 y down; the unit matrix maps model space into the render world (y up): invert its affine part.
  const render::Mat4 model = fx_unit_matrix(unit);
  const float rel[3] = {world[0] - model.at(0, 3), -world[1] - model.at(1, 3), world[2] - model.at(2, 3)};
  auto at = [&](int row, int col) { return model.at(row, col); };
  const float det = at(0, 0) * (at(1, 1) * at(2, 2) - at(1, 2) * at(2, 1)) - at(0, 1) * (at(1, 0) * at(2, 2) - at(1, 2) * at(2, 0)) +
                    at(0, 2) * (at(1, 0) * at(2, 1) - at(1, 1) * at(2, 0));
  if (std::fabs(det) < 1e-9f) return false;
  const float inverse[9] = {(at(1, 1) * at(2, 2) - at(1, 2) * at(2, 1)) / det, (at(0, 2) * at(2, 1) - at(0, 1) * at(2, 2)) / det,
                            (at(0, 1) * at(1, 2) - at(0, 2) * at(1, 1)) / det, (at(1, 2) * at(2, 0) - at(1, 0) * at(2, 2)) / det,
                            (at(0, 0) * at(2, 2) - at(0, 2) * at(2, 0)) / det, (at(0, 2) * at(1, 0) - at(0, 0) * at(1, 2)) / det,
                            (at(1, 0) * at(2, 1) - at(1, 1) * at(2, 0)) / det, (at(0, 1) * at(2, 0) - at(0, 0) * at(2, 1)) / det,
                            (at(0, 0) * at(1, 1) - at(0, 1) * at(1, 0)) / det};
  for (int row = 0; row < 3; ++row) target[row] = inverse[row * 3] * rel[0] + inverse[row * 3 + 1] * rel[1] + inverse[row * 3 + 2] * rel[2];
  return true;
}

void BattleView::tick_tex_flips() {
  // Each copy is a GPU packet the task puts at the head of the frame's draw list: of two copies in one tick the older task's lands last.
  for (auto it = tex_flips_.rbegin(); it != tex_flips_.rend(); ++it) {
    TexFlip& flip = *it;
    if (--flip.counter <= 0) {
      flip.sel ^= 1;
      flip.counter = flip.hold[static_cast<size_t>(flip.sel)];
      tex_flip_paste(flip.setup.unit, flip.setup.dx, flip.setup.dy, flip.setup.rect[static_cast<size_t>(flip.sel)]);
    }
    --flip.left;
  }
  std::erase_if(tex_flips_, [](const TexFlip& flip) { return flip.left <= 0; });
}

void BattleView::tex_flip_paste(int unit, int dx, int dy, const std::array<int, 4>& rect) {
  if (unit < 0 || unit >= battle::kUnits || !ctx_ || !fx_vram_ready()) return;
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  if (!visual.has_model || !visual.model || !visual.model->gpu || visual.model->asset.textures.empty()) return;
  assets::ModelAsset& asset = visual.model->asset;
  render::MeshModel& gpu = *visual.model->gpu;
  // A replacement texture has its own layout: the PS1 rect means nothing there.
  if (asset.has_mod(0) || gpu.textures.empty() || !gpu.textures[0]) return;
  import::Image& image = asset.textures[0];
  if (image.indexed || image.pixels.size() < static_cast<size_t>(image.width) * image.height * 4u) return;
  // The texels are CLUT indices: the model's own CLUT row 0 colours them, as its primitives draw them.
  std::optional<import::AtimMain> own;
  for (const char* ext : {".ltc", ".ltd"})
    if (!own)
      if (const auto bytes = vfs_->read_disc("cdrom/" + asset.source + ext)) own = import::load_atim_main(*bytes);
  if (!own || own->clut.empty()) return;
  const import::Vram& vram = fx_textures_.vram;
  for (int row = 0; row < rect[3]; ++row)
    for (int col = 0; col < rect[2] * 2; ++col) {
      const int x = dx * 2 + col, y = dy + row;
      if (x < 0 || y < 0 || x >= static_cast<int>(image.width) || y >= static_cast<int>(image.height)) continue;
      const u16 index = vram.texel(rect[0] * 2 + col, rect[1] + row, 8);
      import::store_rgba(&image.pixels[(static_cast<size_t>(y) * image.width + static_cast<size_t>(x)) * 4],
                         index < own->clut.size() ? import::bgr555_to_rgba_stp(own->clut[index]) : 0u);
    }
  log::debug("battle: tick {} unit {} texture rect ({}, {}) {} x {} -> ({}, {})", tick_count_, unit, rect[0], rect[1], rect[2], rect[3], dx, dy);
  gpu.textures[0]->update(*ctx_, render::bleed_transparent(image.pixels, image.width, image.height).data());
  // The eye-blink variants are texture 0 with the shut-eye frames over it.
  if (gpu.eye_base >= 0)
    for (int frame = 1; frame <= 2; ++frame) {
      const size_t index = static_cast<size_t>(gpu.eye_base + frame - 1);
      const std::vector<u8> pixels = asset.eye_frame_pixels(image.pixels, image.width, image.height, frame);
      if (index < gpu.textures.size() && gpu.textures[index] && !pixels.empty())
        gpu.textures[index]->update(*ctx_, render::bleed_transparent(pixels, image.width, image.height).data());
    }
  tex_flip_paste_hd(*visual.model, dx, dy, rect);
}

// The same copy into the unit's HD texture: the pack's HD texels where it has them (FxPack::hd_texels), else its texels enlarged, coloured
// with the palette of the unit's HD image.
void BattleView::tex_flip_paste_hd(ModelHandle& model, int dx, int dy, const std::array<int, 4>& rect) {
  assets::ModelAsset& asset = model.asset;
  render::MeshModel& gpu = *model.gpu;
  if (!asset.has_hd(0) || asset.hd_palette.empty() || gpu.hd_textures.empty() || !gpu.hd_textures[0]) return;
  import::Image& image = asset.hd_textures[0];
  const int scale = static_cast<int>(asset.hd_scale);
  if (image.indexed || scale < 1 || image.pixels.size() < static_cast<size_t>(image.width) * image.height * 4u) return;
  const FxPack::HdTexels* texels = fx_pack_ && fx_pack_->hd_texels ? &*fx_pack_->hd_texels : nullptr;
  constexpr int kTexelScale = FxPack::HdTexels::kScale;
  const import::Vram& vram = fx_textures_.vram;
  for (int row = 0; row < rect[3] * scale; ++row)
    for (int col = 0; col < rect[2] * 2 * scale; ++col) {
      const int x = dx * 2 * scale + col, y = dy * scale + row;
      if (x < 0 || y < 0 || x >= static_cast<int>(image.width) || y >= static_cast<int>(image.height)) continue;
      const int src_x = rect[0] * 2 * scale + col, src_y = rect[1] * scale + row;
      u16 index = vram.texel(src_x / scale, src_y / scale, 8);
      if (texels) {
        const int hd_x = (src_x - texels->x * 2 * scale) * kTexelScale / scale, hd_y = (src_y - texels->y * scale) * kTexelScale / scale;
        if (hd_x >= 0 && hd_y >= 0 && hd_x < static_cast<int>(texels->indices.width) && hd_y < static_cast<int>(texels->indices.height))
          index = texels->indices.pixels[static_cast<size_t>(hd_y) * texels->indices.width + static_cast<size_t>(hd_x)];
      }
      import::store_rgba(&image.pixels[(static_cast<size_t>(y) * image.width + static_cast<size_t>(x)) * 4],
                         index < asset.hd_palette.size() ? asset.hd_palette[index] : 0u);
    }
  gpu.hd_textures[0]->update(*ctx_, render::bleed_transparent(image.pixels, image.width, image.height).data());
  const size_t strip = asset.main_texture_count;
  if (gpu.eye_base >= 0)
    for (int frame = 1; frame <= 2; ++frame) {
      const size_t index = static_cast<size_t>(gpu.eye_base + frame - 1);
      const std::vector<u8> pixels = asset.eye_frame_pixels(image.pixels, image.width, image.height, frame, asset.has_hd(strip) ? &asset.hd_textures[strip] : nullptr);
      if (index < gpu.hd_textures.size() && gpu.hd_textures[index] && !pixels.empty())
        gpu.hd_textures[index]->update(*ctx_, render::bleed_transparent(pixels, image.width, image.height).data());
    }
}

void BattleView::tick_units() {
  for (int i = 0; i < battle::kUnits; ++i) {
    Visual& visual = vis_[static_cast<size_t>(i)];
    const battle::Unit& unit_rec = battle_->unit(i);
    if (!unit_rec.present) continue;
    // Vary Innate (element mode 42) writes the caster's control-block colour together with its innate byte.
    if (act_.phase != Act::Phase::None && act_.actor == i && !act_.ev.empty()) {
      const size_t element = static_cast<size_t>(act_.ev.front().element);
      if (element < tables_->elements.size() && tables_->elements[element].mode() == 42) visual.attrib = unit_rec.innate;
    }
    visual.px = visual.x;
    visual.pz = visual.z;
    visual.py = visual.y;
    visual.pface_x = visual.face_x;
    visual.pface_z = visual.face_z;
    visual.flash_prev = visual.flash;
    visual.fade_prev = visual.fade;
    visual.flash = std::max(0.0f, visual.flash - 1.0f / 20.0f);
    // An enemy's death fade is the tail of the reaction script that took the killing hit (its dead branch: death clips, ColorRamp, ModelShow 0).
    // This fallback only covers a kill no reaction script took: it starts after the action and runs on its own. HP a script wrote to 0 never fades.
    const bool action_over = act_.phase == Act::Phase::None && !fx_active();
    // A boss with a scripted death (op 5C) owns its death presentation: no fade while its death cast is still owed / queued (the pack hides it).
    const bool death_cast = visual.death_owed || battle_->death_pending(i) || (!queue_.empty() && visual.fade >= 1.0f && battle_->unit(i).hp <= 0 && !unit_rec.pc);
    if (!unit_rec.pc && !death_cast && !visual.ko_reacted && !unit_rec.hp_written_zero && visual.ko_shown && visual.script_done && visual.script_kind == kIdle && (visual.fade < 1.0f || (action_over && (visual.ko_hit || battle_->outcome() == battle::Outcome::Ongoing)))) visual.fade = std::max(0.0f, visual.fade - 1.0f / 40.0f);
    // Dev: OC_DEATH_TRACE logs the KO / fade state of a dying monster per tick.
    if (static const bool dt = std::getenv("OC_DEATH_TRACE") != nullptr; dt && !unit_rec.pc && (unit_rec.hp <= 0 || visual.death_owed)) {
      static std::array<std::string, battle::kUnits> last;
      const effects::FxModels* tm = fx_ ? fx_->bespoke_models() : nullptr;
      const effects::FxModel* om = tm && static_cast<size_t>(i) < tm->units.size() ? &tm->units[static_cast<size_t>(i)] : nullptr;
      const std::string state = std::format("hp {} owed {} ko_shown {} fade {:.2f} script {} done {} ghosted {} shown {} fxhid {} fx {} ovl {} fin {} act {} q {} out {} brainowed {} pend {} ui {} anmhid {} model {}", unit_rec.hp, visual.death_owed, visual.ko_shown,
                                            visual.fade, static_cast<int>(visual.script_kind), visual.script_done, ghosted(i), entry_shown_[static_cast<size_t>(i)], fx_hidden_[static_cast<size_t>(i)],
                                            fx_ != nullptr, om ? std::format("t{} h{} rgb{},{},{}", om->tinted, om->hidden, om->rgb[0], om->rgb[1], om->rgb[2]) : std::string("-"),
                                            static_cast<int>(finish_), static_cast<int>(act_.phase), queue_.size(), static_cast<int>(battle_->outcome()), brain_.death_owed(i), battle_->death_pending(i), static_cast<int>(ui_), anm_hidden(i), visual.has_model);
      if (state != last[static_cast<size_t>(i)]) log::info("deathtrace: tick {} unit {} {}", tick_count_, i, state), last[static_cast<size_t>(i)] = state;
    }
    const float target = static_cast<float>(visual.hp_shown);
    const float step = std::max(1.0f, static_cast<float>(unit_rec.max_hp) * 0.012f);
    visual.hp_disp = visual.hp_disp < target ? std::min(target, visual.hp_disp + step) : std::max(target, visual.hp_disp - step);

    if (visual.move.on) {
      --visual.move.left;
      const float blend = 1.0f - static_cast<float>(visual.move.left) / static_cast<float>(visual.move.total);
      visual.x = lerp(visual.move.fx, visual.move.tx, std::clamp(blend, 0.0f, 1.0f));
      visual.z = lerp(visual.move.fz, visual.move.tz, std::clamp(blend, 0.0f, 1.0f));
      // Jump: y (down) -= vy / 2 once, then y += vy; vy += g per tick.
      if (visual.move.jump) {
        float yd = -visual.y;
        if (visual.move.first) yd -= visual.move.vy * 0.5f, visual.move.first = false;
        yd += visual.move.vy;
        visual.move.vy += visual.move.g;
        visual.y = -yd;
      } else {
        visual.y = lerp(visual.move.fy, visual.move.ty, std::clamp(blend, 0.0f, 1.0f));
      }
      if (visual.move.left <= 0) {
        visual.y = visual.move.ty;
        visual.move.jump = false;
        visual.move.on = false;
        // RetMoveT and its move task write no yaw: a unit that walks home keeps the facing it had (the Portalgheist pup slid out at the door's
        // yaw 2560 and stays at 2560 after the glide home).
        // The actor's yaw goes home at its queue end (end_group), a target's at the next PC element command (begin_group).
      }
    }

    // Script ops B0 / B1: one tween pass every second tick, pass k moving the model by k steps.
    if (visual.lift.dir != 0) {
      ++visual.lift.tick;
      if (visual.lift.tick % 2 == 1) visual.y -= static_cast<float>(visual.lift.dir * (visual.lift.tick + 1) / 2 * visual.lift.step);
      if (visual.lift.tick >= battle::pac::kLiftTicks) visual.lift.dir = 0;
    }

    if (visual.turn.on) {
      --visual.turn.left;
      const float blend = 1.0f - static_cast<float>(visual.turn.left) / static_cast<float>(visual.turn.total);
      const float yaw = lerp(visual.turn.from, visual.turn.to, std::clamp(blend, 0.0f, 1.0f));
      visual.face_x = -std::sin(yaw);
      visual.face_z = -std::cos(yaw);
      if (visual.turn.left <= 0) visual.turn.on = false;
    }

    if (visual.vm.running()) {
      bseq::Ctx ctx;
      fill_ctx(i, ctx);
      std::vector<bseq::Event> ev;
      visual.vm.tick(ctx, ev);
      for (const bseq::Event& event : ev) {
        on_script_event(i, event);
        // The elements the spawn script gave a task are battle-long (anm_unit_reset keeps them).
        using K = bseq::Event::Kind;
        if (visual.spawn && (event.kind == K::ElemMotion || event.kind == K::ElemRotRate || event.kind == K::ElemRot) && event.a >= 0 && event.a < 64) visual.spawn_joints |= 1ull << event.a;
      }
      if (!visual.vm.running()) visual.script_done = true;
      // The spawn script's end (0x4C): the clip-1 loop it set goes on; an idle restart that waited runs now.
      if (!visual.vm.running() && visual.spawn) {
        visual.spawn = false;
        if (visual.victory_owed) start_script(i, &visual.scripts->s5, 11, 0, kVictory);
        else if (visual.idle_owed) start_idle(i);
      }
    }
    // When the guard flag changes (Defend set it, the PC's command menu opening clears it) the idle script is restarted so it re-picks the pose.
    if (visual.script_kind == kIdle && visual.script_done && visual.guard_shown != unit_rec.guard && unit_rec.pc) start_idle(i);

    // Back to the idle script once an action script is over (the actor is released by the action phases).
    const bool held = act_.phase != Act::Phase::None && act_.phase != Act::Phase::Settle && i == act_.actor;
    const bool waiting = std::any_of(pending_.begin(), pending_.end(), [i](const Pending& pending) { return pending.unit == i; });
    if (visual.script_done && visual.script_kind != kIdle && visual.script_kind != kNone && visual.script_kind != kVictory && visual.script_kind != kRun && !held && !waiting &&
        !engaged_stance(i))
      start_idle(i);
    // A bespoke overlay that set the model's animation speed to 0 (fnoise 0x180, rakugaki, hensin) freezes its pose.
    const effects::FxModels* fxm = fx_ ? fx_->bespoke_models() : nullptr;
    const bool frozen = fxm && static_cast<size_t>(i) < fxm->units.size() && fxm->units[static_cast<size_t>(i)].anim_speed == 0;
    // A hidden model (a script ghost: ops 34 / 35 / the spawn seed) skips the motion step and the joint pass, so its pose and attach points stay
    // what the last visible pass left (the Bomber, off-stage all fight, is clip 13 / motion 0 while `hiba` spawns at attach 31 of its clip-1
    // frame-0 pose)
    // ... and the spawn script's hidden opening (hidden until op 0x19; the clip position stays 0 until the show), whose element tasks turn on
    // meanwhile (7star element 16 turns 64 per tick while hidden).
    // A model an animation script hid (Vital Force's jumps) is held the same way: an attach point read while it is hidden (op 0xAA's own
    // attach 9) is where the model stood at its last visible pass, before the move that came with the hide.
    const bool hidden_model = visual.has_model && ((ghosted(i) && !entry_shown_[static_cast<size_t>(i)]) || anm_hidden(i));
    if (visual.has_model && !hidden_model) {
      visual.hold_x = visual.x, visual.hold_y = visual.y, visual.hold_z = visual.z;
      visual.hold_fx = visual.face_x, visual.hold_fz = visual.face_z;
    }
    if (visual.has_model) visual.pose.hold(hidden_model);
    // Op 0x88's task: the head target follows the other unit's attach point.
    if (float head_target[3]; visual.pose.head_active() && head_target_model(i, head_target)) visual.pose.head_target(head_target);
    // The element tasks are anm tasks (rotation, scale), not the motion step, so they also run on a script-ghosted model (the hidden Bomber's
    // wick joints scale 4096 -> 0 while it is off-stage).
    if (visual.has_model && !frozen && !hidden_model) visual.pose.tick();
    else if (hidden_model && !frozen) visual.pose.tick_elements();
    // Dev: OC_REACH_TRACE logs the posed reach of attach 15 per tick.
    if (static const bool tr = std::getenv("OC_REACH_TRACE") != nullptr; tr && visual.has_model && i == act_.actor) {
      float qx, qy, qz;
      if (visual.pose.attach_now(15, 1.0f, qx, qy, qz)) log::info("reach15: tick {} unit {} clip {} {:.1f}", tick_count_, i, visual.pose.current_clip(), std::hypot(qx, qz));
    }
  }
}

}  // namespace oc::game
