// BattleView: one action group from begin to finish (group setup, hits, return, release, finish).
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"

#include <algorithm>
#include <cstdlib>
#include <format>

#include "core/log.h"

namespace oc::game {

using namespace bv;

// ------------------------------------------------------------------------------------------------------------- action
void BattleView::begin_group() {
  act_ = Act{};
  links_.clear();
  // The action's setup: op 0x68 starts any timeline again; the next action clears the system message window; the command setup rewrites the
  // acting list.
  bseq::Vm::reset_sequence();
  if (smsg_.sys) smsg_ = ScriptMsg{};
  script_actor_ = -1;
  act_.ev = std::move(queue_.front());
  queue_.pop_front();
  const battle::ActionEvent& e0 = act_.ev.front();
  // Dev: OC_DEATH_TRACE.
  if (std::getenv("OC_DEATH_TRACE")) {
    std::string trace;
    for (const battle::ActionEvent& event : act_.ev) trace += std::format(" ->{}(k{} code{} ko{} dc{} sil{})", event.target, static_cast<int>(event.kind), event.code, event.ko, event.death_cast, event.silent);
    log::info("deathtrace: tick {} group actor {} element {} mode {}{}", tick_count_, e0.actor, e0.element, e0.kind == battle::ActionEvent::Kind::Element && static_cast<size_t>(e0.element) < tables_->elements.size() ? tables_->elements[static_cast<size_t>(e0.element)].mode() : -1, trace);
  }
  act_.actor = e0.actor;
  // An enemy action while a PC stands at its target resets that PC's menu: its next attack confirms the target again.
  if (engaged_ && e0.actor >= battle::kPcSlots) engage_interrupted_ = true;
  // Pip's form element (mode 45): tumaru 0x181 swaps the model.
  if (e0.evolve_form >= 0) pending_evolve_unit_ = e0.actor;
  Visual& actor_visual = vis_[static_cast<size_t>(act_.actor)];
  audio_.activate(actor_visual.sound_key);
  camera_.stop_sequences();
  camera_.set_bank(actor_visual.scripts ? &actor_visual.scripts->cam : &bank_);
  act_.ticks = 0;
  act_.hit = 0;
  route_events_.push_back("a");
  log::debug("battle: action {} actor {} kind {} events {} at tick {}", e0.action, e0.actor, static_cast<int>(e0.kind), act_.ev.size(), tick_count_);
  for (const battle::ActionEvent& event : act_.ev)
    if (event.target >= 0 && std::find(act_.targets.begin(), act_.targets.end(), event.target) == act_.targets.end()) act_.targets.push_back(event.target);
  // An empty target list (no target survived the target mask) is filled with the first ACTING unit, so every reader of the target list
  // (camera refs, the facing pass) sees the caster itself.
  if (act_.targets.empty()) act_.targets.push_back(act_.actor);
  // The acting list is [actor, FF ...]; for a MONSTER element with a caster group (element byte 0x17 != 0xFF), for k < that count, entry k = the
  // k-th byte of the queue_attack's group list (the 8D slots of the script) unless it is FF, and the unit's variant byte = k. The list ends at the
  // first FF. 2 unit entries: taiatari 426 (6), gattai 393 (6: u8 first), bakunage 434, k_monk 340.
  act_.casters = {act_.actor};
  if (e0.kind == battle::ActionEvent::Kind::Element && act_.actor >= battle::kPcSlots && e0.element > 0 && static_cast<size_t>(e0.element) < tables_->elements.size()) {
    const int caster_count = tables_->elements[static_cast<size_t>(e0.element)].b(0x17);
    if (caster_count != 0 && caster_count != 0xFF) {
      std::array<int, 6> list{act_.actor, -1, -1, -1, -1, -1};
      for (int k = 0; k < std::min(caster_count, 6); ++k)
        if (e0.group[static_cast<size_t>(k)] < battle::kUnits) list[static_cast<size_t>(k)] = e0.group[static_cast<size_t>(k)];
      act_.casters.clear();
      for (int caster : list) {
        if (caster < 0) break;
        act_.casters.push_back(caster);
      }
      if (act_.casters.size() > 1) log::debug("battle: group cast element {} caster list {} units (first {})", e0.element, act_.casters.size(), act_.casters.front());
    }
  }
  // A PC's double / triple tech (element byte 0x14 != 0xFF): the list is the casting PCs in the order of the record's character bytes, entry k
  // with variant k, which need not start with the PC that chose it.
  if (e0.kind == battle::ActionEvent::Kind::Element && act_.actor < battle::kPcSlots && e0.element > 0 &&
      static_cast<size_t>(e0.element) < tables_->elements.size() && tables_->elements[static_cast<size_t>(e0.element)].b(0x14) != 0xFF) {
    act_.casters.clear();
    for (const u8 caster : e0.group) {
      if (caster >= battle::kPcSlots) break;
      act_.casters.push_back(caster);
    }
    if (act_.casters.empty()) act_.casters = {act_.actor};
    log::debug("battle: combo tech {} caster list {} units (first {})", e0.element, act_.casters.size(), act_.casters.front());
  }

  // The home pass outside the actor's own end of queue: after an ELEMENT command of a PC was issued every eligible unit goes home before the
  // cast starts. There is NO such pass at an ENEMY's action start: a bystander that was turned to Serge by his earlier attack keeps its yaw
  // through the enemy's whole cast.
  if (e0.kind == battle::ActionEvent::Kind::Element && act_.actor < battle::kPcSlots)
    for (int i = 0; i < battle::kUnits; ++i) {
      Visual& visual = vis_[static_cast<size_t>(i)];
      if (!battle_->unit(i).present || visual.ko_shown || visual.move.on || (engaged_ && i == active_pc_)) continue;
      visual.turn.on = false;
      visual.face_x = visual.pface_x = visual.home_fx;
      visual.face_z = visual.pface_z = visual.home_fz;
    }

  switch (e0.kind) {
    case battle::ActionEvent::Kind::Weapon: {
      act_.targets = {e0.target};
      face_toward(act_.actor, e0.target);
      // Weapon pick: both face each other (unless a unit's pose word is 8 = dead), and the target KEEPS that facing afterwards (the Sand Squirt
      // stays turned from the approach through the hit and the HIT% box).
      if (e0.target != act_.actor) face_toward(e0.target, act_.actor);
      // Already standing at the target: hit at once (the approach shot again only when an enemy action interrupted the turn).
      if (engaged_ && act_.actor == active_pc_ && e0.target == engaged_target_) {
        if (engage_interrupted_) select_approach();
        engage_interrupted_ = false;
        act_.phase = Act::Phase::Hit;
        start_hit();
        break;
      }
      select_approach();
      if (actor_visual.scripts && actor_visual.scripts->has_s5 && start_script(act_.actor, &actor_visual.scripts->s5, 2, 0, kApproach)) {
        act_.phase = Act::Phase::Approach;
      } else {
        act_.phase = Act::Phase::Hit;
        start_hit();
      }
      break;
    }
    case battle::ActionEvent::Kind::Element:
      // The camera stays where it is (zen0 / the command shot) until the charge pack picks one of ch10..ch13 (14 frames of the wide shot without
      // the HUD, then the cut); elm0 belongs to the element menu, not to the cast.
      {
        // Facing both ways (element pick, as for a weapon). Several targets: every one of them turns to the caster and the caster faces the
        // midpoint of their bounding box (Tornado: all three enemies turn toward Serge, Serge straight at the group, 1024). A self
        // target faces the world origin (below).
        face_cast_targets(true);
      }
      fx_charge_ = fx_ready_ = false;
      // The element confirm (and the item confirm) hops home ONLY if the PC is engaged (standing at the enemy); a PC at home goes straight to
      // the cast pose, no hop. The hop starts S5 entry 3 (return: crouch, the 16-tick arc hop to the stand position, clips 4 / 17 / 16 / 1)
      // on every acting unit and the cast waits for it, so a PC standing at the enemy hops home first and casts from there. The script
      // itself has no distance test (JumpT always arcs), the decision is the caller's flag. Enemies never reach this (menu path).
      // When the PC stands at the enemy the confirm also cuts to the approach shot - the camera follows the hop home
      // instead of staying on the enemy chain shot. A PC at home keeps the command shot.
      if (engaged_ && act_.actor == active_pc_) select_approach();
      if (engaged_ && act_.actor == active_pc_ && actor_visual.scripts && actor_visual.scripts->has_s5 && start_script(act_.actor, &actor_visual.scripts->s5, 3, 0, kReturn)) {
        act_.phase = Act::Phase::Return;
        act_.cast_after_return = true;
        // The PC is home after this: no second return at the end of its turn.
        engaged_ = false;
        engage_arrive_ = false;
        engaged_after_cast_ = false;
      } else {
        act_.phase = Act::Phase::Cast;
      }
      break;
    case battle::ActionEvent::Kind::Defend:
      // The guard bit is set and the idle script (S5 entry 1) is restarted: it reads the flag (op 0x49) and holds the block pose itself.
      // No separate clip is forced here (a forced clip would never hand back to the idle script).
      start_idle(act_.actor);
      act_.phase = Act::Phase::Defend;
      break;
    case battle::ActionEvent::Kind::Run:
      for (int i = 0; i < battle::kPcSlots; ++i) {
        const battle::Unit& unit = battle_->unit(i);
        if (!unit.present || !unit.alive()) continue;
        Visual& pv = vis_[static_cast<size_t>(i)];
        // S5 entry 7: turn away (RotateT) + run clip + PosMoveT.
        if (pv.scripts && pv.scripts->has_s5) start_script(i, &pv.scripts->s5, 7, 0, kRun);
        else begin_move(i, pv.x - 4000.0f * pv.face_x, pv.z - 4000.0f * pv.face_z, 80, false);
      }
      play_tag_sound("esc", 127);
      act_.phase = Act::Phase::Run;
      break;
  }
}

// Facing both ways at the cast (see begin_group): every other target turns to the caster (`turn_targets`), the caster faces the target / the
// midpoint of several / the world origin for a self target. Run again when the return hop home finishes (the caster stands at home then: a PC
// who cast Cure on himself from the enemy side faced the origin from the enemy position, i.e. the wrong way).
// The confirm itself only turns the caster to the FIRST target (toward it, or the stored home yaw 1024 when it is the caster himself). The group
// facing (midpoint of the targets' bounding box, the origin stand-in for a self target) is a later turn: the charge camera ch10..ch13 (ref 1/1 =
// the caster's frame, taken when selected, flag bit 0 clear) is framed at the first-target yaw (traps: frame yaw 1326 = toward unit 3 while the
// unit reads 1024 from the same sample on; Cure: frame 1024, unit 1083), so the turn runs right after the camera select (fx_charge_begin).
void BattleView::face_cast_targets(bool turn_targets) {
  face_pending_ = false;
  const Visual& actor_visual = vis_[static_cast<size_t>(act_.actor)];
  float lo_x = 1e9f, hi_x = -1e9f, lo_z = 1e9f, hi_z = -1e9f;
  int others = 0, first_other = -1, first = -1;
  for (int target : act_.targets) {
    if (target < 0 || target >= battle::kUnits) continue;
    const Visual& target_visual = vis_[static_cast<size_t>(target)];
    lo_x = std::min(lo_x, target_visual.x), hi_x = std::max(hi_x, target_visual.x);
    lo_z = std::min(lo_z, target_visual.z), hi_z = std::max(hi_z, target_visual.z);
    // The target list is rewritten in ascending slot order.
    if (first < 0 || target < first) first = target;
    if (target == act_.actor) continue;
    if (first_other < 0) first_other = target;
    ++others;
    if (turn_targets && act_.actor < battle::kPcSlots) face_toward(target, act_.actor);
  }
  // The group facing point.
  float fx = 0, fz = 0;
  bool group = false;
  if (act_.actor >= battle::kPcSlots) {
    // An ENEMY cast reaches the cast only through the queue executor: the caster turns to the first target (home yaw for a self target) and that
    // one target turns back; the menu's group facing (midpoint / origin stand-in) never runs (a self-target Hide keeps the formation yaw 3072).
    if (first < 0) return;
    // A GROUP cast (queue type 2): the caster still turns to the first target but the TARGET never turns back (it keeps its yaw over the whole
    // cast; single-target casts turn it) and the other listed units are turned by the pack's own Face ops as they launch.
    Visual& av = vis_[static_cast<size_t>(act_.actor)];
    if (first != act_.actor) {
      face_toward(act_.actor, first);
      if (turn_targets && act_.casters.size() <= 1) face_toward(first, act_.actor);
    } else if (!av.ko_shown) {
      av.face_x = av.pface_x = av.home_fx;
      av.face_z = av.pface_z = av.home_fz;
    }
    return;
  }
  if (others == 1) {
    fx = vis_[static_cast<size_t>(first_other)].x, fz = vis_[static_cast<size_t>(first_other)].z, group = true;
  } else if (others > 1) {
    fx = (lo_x + hi_x) * 0.5f, fz = (lo_z + hi_z) * 0.5f, group = true;
  } else if (!act_.targets.empty() && !actor_visual.ko_shown) {
    // Self target (Tablet, Heal on Serge: yaw 1083 = 1024 + 59, atan2 of Serge (2816, -256) minus the world origin): the stand-in
    // for the second unit of the later turn is a position (0, 0).
    group = true;
  }
  if (!group || first < 0) return;
  Visual& av = vis_[static_cast<size_t>(act_.actor)];
  if (first != act_.actor) {
    face_toward(act_.actor, first);
  // A self target takes the stored home yaw.
  } else if (!av.ko_shown) {
    av.face_x = av.pface_x = av.home_fx;
    av.face_z = av.pface_z = av.home_fz;
  }
  // The first target is the whole group.
  if (others == 1 && first == first_other) return;
  face_pending_ = true;
  face_px_ = fx;
  face_pz_ = fz;
}

void BattleView::start_hit() {
  Visual& actor_visual = vis_[static_cast<size_t>(act_.actor)];
  const battle::ActionEvent& hit_event = act_.ev[act_.hit];
  act_.targets = {hit_event.target};
  act_.ticks = 0;
  // The hand toggle flips when an attack starts; the script reads the flipped value.
  hand_toggle_ ^= 1;
  act_.hand = hand_toggle_;
  const bseq::Table* table = actor_visual.scripts && actor_visual.scripts->has_atk ? &actor_visual.scripts->atk : &model_cache_.fallback_atk;
  if (!start_script(act_.actor, table, 0, 0, kAttack)) start_script(act_.actor, &model_cache_.fallback_atk, 0, 0, kAttack);
}

void BattleView::start_return() {
  Visual& actor_visual = vis_[static_cast<size_t>(act_.actor)];
  act_.phase = Act::Phase::Return;
  act_.ticks = 0;
  if (!(actor_visual.scripts && actor_visual.scripts->has_s5 && start_script(act_.actor, &actor_visual.scripts->s5, 3, 0, kReturn))) begin_move(act_.actor, actor_visual.home_x, actor_visual.home_z, 20, true);
}

void BattleView::start_release() {
  Visual& actor_visual = vis_[static_cast<size_t>(act_.actor)];
  act_.phase = Act::Phase::Release;
  act_.ticks = 0;
  fx_charge_end();
  if (!act_.ev.empty() && act_.ev.front().kind == battle::ActionEvent::Kind::Element && !act_.ev.front().caught)
    cast_elements_.push_back(act_.ev.front().element);
  // The pack starts the release itself (op 0x4F) and lands the hits.
  if (fx_begin()) return;
  if (battle_audit() && !act_.ev.empty() && act_.ev.front().kind == battle::ActionEvent::Kind::Element && dbg_.effects)
    battle_audit_note(std::format("element {} (unit {}) has no effect pack: generic release", act_.ev.front().element, act_.actor));
  if (actor_visual.scripts && actor_visual.scripts->has_s5) start_script(act_.actor, &actor_visual.scripts->s5, 6, 0, kRelease);
  int target_index = 0;
  for (const battle::ActionEvent& event : act_.ev) {
    if (event.target < 0) continue;
    pending_.push_back({event.target, 4 + 6 * target_index, event});
    ++target_index;
  }
}

void BattleView::tick_action() {
  if (battle_audit()) audit_tick();
  // Delayed reactions (element casts hit their targets one after the other).
  for (Pending& pending : pending_) --pending.delay;
  for (size_t i = 0; i < pending_.size();) {
    if (pending_[i].delay > 0) {
      ++i;
      continue;
    }
    const Pending due = pending_[i];
    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
    // Element hits (generic path / leftovers of a cut effect): CallDamage only, no reaction script (see fx_fire).
    vis_[static_cast<size_t>(due.unit)].react_ev = due.ev;
    on_damage(due.unit);
  }

  if (act_.phase == Act::Phase::None) {
    if (queue_.empty() || ui_ == Ui::Intro) return;
    // The executor serialises actions: a PC standing at its target (the chain menu's `engaged_`) ends its turn - the return walk
    // (S5 entry 3) - before another actor's queued action starts.
    if (engaged_ && queue_.front().front().actor != active_pc_) {
      begin_disengage();
      return;
    }
    // The script has not reached this queue_attack yet (a window / delay before it).
    if (script_gated(queue_.front().front())) return;
    // The field ring's shift after the last action runs to its end first.
    if (!ring_settled()) return;
    begin_group();
    return;
  }
  ++act_.ticks;
  Visual& actor_visual = vis_[static_cast<size_t>(act_.actor)];
  switch (act_.phase) {
    case Act::Phase::Approach:
      if ((actor_visual.script_done && !actor_visual.move.on) || act_.ticks > 600) {
        if (battle_audit() && !(actor_visual.script_done && !actor_visual.move.on)) battle_audit_note(std::format("approach of unit {} left by the 600-tick timeout (script_done {} moving {})", act_.actor, actor_visual.script_done, actor_visual.move.on));
        actor_visual.vm.stop();
        // Engage: arrived, the percentage menu opens.
        if (act_.ev.empty()) {
          act_ = Act{};
          engage_arrive_ = false;
          // No idle restart: the approach script's last SetAnim (clip 2, the ready stance) loops at the enemy (engaged_stance).
          engaged_ = true;
          break;
        }
        act_.phase = Act::Phase::Hit;
        start_hit();
      }
      break;
    case Act::Phase::Hit:
      // Between two hits of one scripted plan: the script is still in a window / delay before the next queue_attack.
      if (act_.hit_wait) {
        act_.ticks = 0;
        if (script_gated(act_.ev[act_.hit])) break;
        act_.hit_wait = false;
        start_hit();
        break;
      }
      if (actor_visual.script_done || act_.ticks > 900) {
        if (battle_audit() && !actor_visual.script_done)
          battle_audit_note(std::format("hit {} of action {} (unit {} -> {}) left by the 900-tick timeout: the attack script never finished", act_.hit, act_.ev.front().action, act_.actor, act_.ev[act_.hit].target));
        actor_visual.vm.stop();
        ++act_.hit;
        if (act_.hit < act_.ev.size()) {
          if (script_gated(act_.ev[act_.hit])) act_.hit_wait = true;
          else start_hit();
        // Stay at the enemy until the PC's turn ends.
        } else if (engaged_ && act_.actor == active_pc_) {
          act_.phase = Act::Phase::Settle;
          act_.ticks = 0;
        } else start_return();
      }
      break;
    case Act::Phase::Return:
      if ((actor_visual.script_done && !actor_visual.move.on) || act_.ticks > 600) {
        if (battle_audit() && !(actor_visual.script_done && !actor_visual.move.on)) battle_audit_note(std::format("return of unit {} left by the 600-tick timeout (script_done {} moving {})", act_.actor, actor_visual.script_done, actor_visual.move.on));
        actor_visual.vm.stop();
        // The return has run, now the cast.
        if (act_.cast_after_return) {
          act_.cast_after_return = false;
          // Face from the home position now; the apr confirm cut is over: the charge shot (ch10..) replaces it at the cast start.
          face_cast_targets(false);
          camera_.finish_shot();
          act_.phase = Act::Phase::Cast;
          act_.ticks = 0;
          break;
        }
        act_.phase = Act::Phase::Settle;
        act_.ticks = 0;
      }
      break;
    case Act::Phase::Cast:
      if (act_.ticks == kCastLead) fx_charge_begin();
      if (act_.ticks > kCastLead && (fx_charge_done() || act_.ticks > kCastLead + 600)) {
        if (battle_audit() && !fx_charge_done()) battle_audit_note(std::format("charge of element {} (unit {}) never finished: released by the {}-tick timeout", act_.ev.front().element, act_.actor, kCastLead + 600));
        start_release();
      }
      break;
    case Act::Phase::Release:
      // A running pack ends itself by its own cap (BattleFxRun::kTickCap): the true ending's chain of sequences runs 5605 ticks.
      if ((actor_visual.script_done && pending_.empty() && !fx_) || act_.ticks > (fx_ ? BattleFxRun::kTickCap : 600)) {
        if (battle_audit() && !(actor_visual.script_done && pending_.empty() && !fx_))
          battle_audit_note(std::format("release of element {} (unit {}) left by the {}-tick timeout (script_done {} pending {} pack {})", act_.ev.front().element, act_.actor,
                                        fx_ ? BattleFxRun::kTickCap : 600, actor_visual.script_done, pending_.size(), fx_ != nullptr));
        actor_visual.vm.stop();
        act_.phase = Act::Phase::Settle;
        act_.ticks = 0;
      }
      break;
    case Act::Phase::Defend:
      // The pose change is visible, then the next PC's menu opens.
      if (act_.ticks > 24) {
        act_.phase = Act::Phase::Settle;
        act_.ticks = 0;
      }
      break;
    case Act::Phase::Run: {
      // The party runs with the camera untouched; then the battle ends (the outro camera follows, tick_finish). Dropped loot holds the
      // escape and opens the system window at (32, 144); the window ends with the hold, where the battle loop ends.
      const bool dropped = !act_.ev.empty() && act_.ev.front().sys_msg >= 0;
      if (dropped && act_.ticks == kRunTicks + kLootDropMsg) {
        if (const std::string* row = lang_tables_.battle_message(static_cast<u32>(act_.ev.front().sys_msg), lang_); row && !row->empty()) {
          ScriptMsgReq req;
          req.sys = true;
          req.unit = act_.actor;
          req.text = *row + std::format("{{wend({})}}", kLootDropWait - kLootDropMsg);
          req.has_pos = true;
          req.x = 32;
          req.y = 144;
          smsg_open(req);
        }
      }
      if (act_.ticks > kRunTicks + (dropped ? kLootDropWait : 0)) {
        act_.phase = Act::Phase::Settle;
        act_.ticks = 0;
      }
      break;
    }
    case Act::Phase::Settle: {
      bool calm = true;
      for (int i = 0; i < battle::kUnits; ++i) {
        const Visual& visual = vis_[static_cast<size_t>(i)];
        // The fleeing party keeps running into the outro.
        if (!battle_->unit(i).present || visual.script_kind == kRun) continue;
        if (visual.vm.running() && visual.script_kind != kIdle) calm = false;
        if (visual.move.on) calm = false;
      }
      if (battle_audit() && !calm && act_.ticks > 300) battle_audit_note(std::format("settle after action {} left by the 300-tick timeout (a script or move still runs)", act_.ev.empty() ? -1 : act_.ev.front().action));
      if (calm || act_.ticks > 300) end_group();
      break;
    }
    case Act::Phase::None: break;
  }
}

void BattleView::end_group() {
  links_.clear();
  // After a cast the last pack shot stays until the next command's camera takes over (the pack's shot holds, then the next PC's orbit cuts in,
  // no zen0 in between); the effect end hook selects no shot.
  const bool cast_end = !act_.ev.empty() && act_.ev.front().kind == battle::ActionEvent::Kind::Element && queue_.empty();
  if (battle_audit() && (fx_ || !pending_.empty()))
    battle_audit_note(std::format("group of action {} ended with {} (fx pack running {}, {} pending reactions)", act_.ev.empty() ? -1 : act_.ev.front().action,
                                  fx_ ? "the effect cut short" : "reactions unshown", fx_ != nullptr, pending_.size()));
  // Cut short: the hits it has not landed are shown by sync_shown below.
  if (fx_) {
    std::fill(fx_fired_.begin(), fx_fired_.end(), true);
    fx_end();
    fx_fog_reset();
  }
  fx_charge_ = fx_ready_ = false;
  // The dying unit's death cast ended (its last queued group; the element's mode does not matter: Bomber's / Potpourri's cast hits the party).
  if (act_.actor >= 0 && !act_.ev.empty() && act_.ev.front().death_cast &&
      std::none_of(queue_.begin(), queue_.end(), [&](const std::vector<battle::ActionEvent>& group) { return !group.empty() && group.front().death_cast && group.front().actor == act_.actor; }))
    vis_[static_cast<size_t>(act_.actor)].death_owed = false;
  // The fleeing party's run scripts (S5 entry 7: run off, fade out, hide) play on into the outro.
  for (size_t i = 0; i < vis_.size(); ++i) {
    Visual& visual = vis_[i];
    if (!visual.vm.running() || visual.script_kind == kIdle || visual.script_kind == kRun) continue;
    visual.vm.stop();
    anm_unit_reset(static_cast<int>(i));
  }
  // Only the ACTOR gets its yaw set to the home facing (byte << 6), once its whole queue is done. Targets keep the facing they were given (the PC
  // an enemy attacked keeps it into the top menu and long after; the enemy is home). A pack script's absolute RotateT / op 0x76 turn on the actor
  // is overwritten here too. A PC engaged at the enemy keeps facing it.
  if (queue_.empty() && finish_ == Finish::None && !engaged_ && act_.actor >= 0) {
    const int i = act_.actor;
    Visual& visual = vis_[static_cast<size_t>(i)];
    if (battle_->unit(i).present && !visual.ko_shown && !visual.move.on) {
      visual.turn.on = false;
      visual.face_x = visual.pface_x = visual.home_fx;
      visual.face_z = visual.pface_z = visual.home_fz;
    }
  }
  // The shot goes to zen0 here (fx_tick covers a held pack shot).
  if (finish_ == Finish::None && !engaged_ && !cast_end && fx_fog_on_) {
    fx_state_log("group end");
    fx_fog_on_ = false;
    fx_unit_fog_.fill(false);
  }
  // The actor's at0.prd closes at the end of the action (its still-running sounds are cut).
  audio_.end_actor();
  // A cut-short hensin / tumaru effect never reached its swap op: the form still changes.
  if (pending_form_ != -2 || pending_evolve_unit_ >= 0) fx_form_phase(2);
  // The Chrono Cross colour row takes the colour history of the action's last element cast and plays that colour's chime.
  static constexpr std::array<int, 7> kColourChime = {0xAB, 0xB3, 0xA7, 0xA7, 0xA3, 0xAF, 0xB7};
  for (auto event = act_.ev.rbegin(); event != act_.ev.rend(); ++event) {
    if (event->kind != battle::ActionEvent::Kind::Element || event->seq_colour < 0) continue;
    if (battle_->colour_row_shown()) {
      colour_row_ = event->colour_seq;
      audio_.play_menu(kColourChime[static_cast<size_t>(event->seq_colour)], 0x400000);
    }
    break;
  }
  act_ = Act{};
  // The flagged record opens the plate at (32, 144) and arms its 0x78-tick timer.
  if (sys_pending_) {
    sys_pending_ = false;
    smsg_open(sys_req_);
  }
  if (finish_ == Finish::None && !engaged_ && !cast_end) camera_.stop_sequences();
  if (queue_.empty()) sync_shown();
  // A unit whose script was cut still has to settle on its idle / KO pose.
  for (int i = 0; i < battle::kUnits; ++i)
    if (battle_->unit(i).present && vis_[static_cast<size_t>(i)].script_kind != kIdle && vis_[static_cast<size_t>(i)].script_kind != kVictory &&
        vis_[static_cast<size_t>(i)].script_kind != kRun && !engaged_stance(i))
      start_idle(i);
}

// ------------------------------------------------------------------------------------------------------------ outcome
void BattleView::begin_finish() {
  const battle::Outcome outcome = battle_->outcome();
  log::info("battle: outcome {} at view tick {}", static_cast<int>(outcome), tick_count_);
  // ocbattle soak lists the same keys.
  for (const auto& [key, count] : battle_->stub_hits()) log::warn("battle: stub hit {} x{}", key, count);
  sync_shown();
  finish_ticks_ = 0;
  ui_ = Ui::Wait;
  // The depth cue a last cast left on must not dim the victory / defeat camera.
  fx_fog_on_ = false;
  fx_unit_fog_.fill(false);
  camera_.stop_sequences();
  if (outcome == battle::Outcome::Victory) {
    finish_ = Finish::Victory;
    // Song 0x17 (boss) / 0x16 plays after a win unless the battle flags have 0x10 or the scene is a special one.
    // The battle theme plays on, fades to silence and the victory song follows (tick_finish).
    if (!(start_flags_ & 0x10) && scene_id_ != 0x1D7 && scene_id_ != 0x1D8 && scene_id_ != 0x1DD) victory_song_ = boss_scene_ ? 0x17 : 0x16;
    // No victory voice is requested.
    for (int i = 0; i < battle::kPcSlots; ++i) {
      const battle::Unit& unit = battle_->unit(i);
      Visual& visual = vis_[static_cast<size_t>(i)];
      if (!unit.present || !unit.alive() || !visual.scripts || !visual.scripts->has_s5) continue;
      // Victory pose (model state 0xB).
      start_script(i, &visual.scripts->s5, 11, 0, kVictory);
    }
    camera_.set_bank(&bank_);
  // Scripted: op 83 ended the fight (no result screen).
  } else if (outcome == battle::Outcome::Fled) {
    finish_ = Finish::Fled;
    fled_outro_ = false;
    camera_.set_bank(&bank_);
  } else if (outcome == battle::Outcome::Scripted) {
    // No outro: the view ends ~60 ticks later.
    finish_ = Finish::Fled;
    fled_outro_ = false;
    finish_ticks_ = kOutroDelay + 1;
  } else {
    // Party wiped: the loop ends at once (no pose, no fanfare), the UI idles for 14 + 14 frames, then `bto0` plays while the exit fades to black
    // over 0x3C ticks and skips its screen. There is no game-over panel in the battle overlays: the host loads map 9 unless the fight was losable.
    finish_ = Finish::Wiped;
  }
}

void BattleView::tick_finish() {
  ++finish_ticks_;
  if (victory_song_ >= 0) {
    if (finish_ticks_ == kVictoryThemeFade) audio_.fade_music(0, kVictoryThemeFadeTicks);
    if (finish_ticks_ == kVictoryThemeFade + kVictorySongDelay) {
      audio_.play_song(victory_song_);
      victory_song_ = -1;
    }
  }
  switch (finish_) {
    case Finish::Victory:
      // After the 14 + 14 teardown frames the `bti0` intro layer (bg_00's sea, birds, fish) returns and the stage outro `bto0` plays.
      if (finish_ticks_ == kOutroDelay) {
        fx_stage_layer("bti0", true, true);
        camera_.play_sequence("bto0");
      }
      if (ui_ != Ui::Result && finish_ticks_ > 120 && (!camera_.sequence_running() || finish_ticks_ > 360)) {
        camera_.stop_sequences();
        // The host's BattleResults screen follows; no in-battle panel.
        if (external_results_) {
          finished_ = true;
          break;
        }
        ui_ = Ui::Result;
        result_age_ = 0;
        log::info("battle: result panel at view tick {}", tick_count_);
        // Global sound 0x14 when the panel opens.
        audio_.play_global(0x14);
      }
      break;
    case Finish::Wiped:
      // The same teardown as the other endings: the `bti0` layer and the stage outro `bto0`, under the exit's fade to black.
      if (finish_ticks_ == kWipeIdle) {
        camera_.set_bank(&bank_);
        fx_stage_layer("bti0", true, true);
        camera_.play_sequence("bto0");
        camera_.start_fade(1.0f, kWipeFade);
      }
      if (!finished_ && finish_ticks_ > kWipeIdle + kWipeFade) {
        audio_.stop_music();
        audio_.stop_sounds();
        finished_ = true;
      }
      break;
    case Finish::Fled: {
      // After the 14 + 14 teardown frames the `bti0` layer returns and the stage outro `bto0` plays (one of its shots at random; it ends with
      // its own fade to black); a stage without one fades to black.
      if (finish_ticks_ == kOutroDelay) {
        fx_stage_layer("bti0", true, true);
        fled_outro_ = camera_.play_sequence("bto0");
        if (!fled_outro_) camera_.start_fade(1.0f, kFledFade);
      }
      const bool outro_done = fled_outro_ ? !camera_.sequence_running() || finish_ticks_ > kOutroDelay + kOutroCap : finish_ticks_ > kOutroDelay + kFledFade;
      if (!finished_ && finish_ticks_ > kOutroDelay && outro_done) {
        // In the field the battle music plays on until the field song replaces it.
        if (!external_results_) audio_.stop_music();
        audio_.stop_sounds();
        finished_ = true;
      }
      break;
    }
    case Finish::None: break;
  }
}

}  // namespace oc::game
