// BattleView: replaying the rules' ActionEvents with the models' animation-sequence scripts.
//
// One player command / enemy turn = one Act. Weapon: the actor runs its S5 entry 2 (approach: turn, run clip, MoveS to the target), then its
// atk script (entry 0, variant 0) once per hit with a = strength and b = hand toggle, whose React op starts the target's reaction
// (the ATTACKER's atk table, variant 7, run on the target model; CallDamage inside it makes the number appear), then S5 entry 3 (return).
// Element: kCastLead ticks after the confirm the charge starts (S5 entry 5 cast pose, the chant, the
// colour's "ch0N" charge pack with its own cameras ch10..ch13); its op 0x41 hands over to the element pack, whose ops
// start the release (0x4F -> S5 entry 6), the target reactions (0x46 -> the pack's 0x1A table, variant 7) and CallDamage (0x47). Without a
// pack: entry 6 + the 4 + 6k stagger. Defend: block clip. Run: S5 entry 7.
#include "game/battle/battle_view.h"
#include "game/cheats.h"
#include "game/battle/battle_view_util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

#include "core/log.h"
#include "import/text.h"

namespace oc::game {

using namespace bv;

// ----------------------------------------------------------------------------------------------------------- update
void BattleView::update(float dt, const platform::Input& in) {
  // The pause screen: nothing ticks (battle_view.h set_paused).
  if (paused_) return;
  if (trans_wait_ || trans_run_) update_transition(dt);
  // The transition holds the battle (the field freeze).
  if (!battle_ || trans_run_ || trans_wait_) return;
  time_ += dt;
  // Dev: OC_DEV_VAR="0x448=1,..." writes script variables (byte offsets) once, after the slot-0 init pass: Fire Dragon
  // 453's damage budget (var 0x448, init = the unit's HP) at 1 makes the first hit start the henshin: `OC_DEV_VAR=0x448=1 opencross battleview 453`.
  if (const char* dv = std::getenv("OC_DEV_VAR"); (dv || std::getenv("OC_DEV_CHIST")) && !dev_var_done_ && brain_.vm()) {
    dev_var_done_ = true;
    for (std::string rest(dv ? dv : ""); !rest.empty();) {
      const size_t comma = rest.find(','), equals = rest.find('=');
      if (equals != std::string::npos && equals < comma) brain_.vm()->set_var(static_cast<int>(std::strtol(rest.c_str(), nullptr, 0)), std::atoi(rest.c_str() + equals + 1));
      rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
    }
    // OC_DEV_CHIST="5,4,1,6,3,2,7": the colour history entries (colour + 1) as if those casts had been made, oldest first (op AE's true-ending test).
    if (const char* history = std::getenv("OC_DEV_CHIST"))
      for (std::string rest(history); !rest.empty();) {
        const size_t comma = rest.find(',');
        brain_.vm()->note_cast_colour(std::atoi(rest.c_str()) - 1);
        rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
      }
  }
  battle_->set_invincible(dbg_.inv_party, dbg_.inv_enemies);
  battle_->set_cheats(battle_cheats());
  const auto st = stepper_.advance(static_cast<double>(dt));
  // The ticks see the stepper's remainder as before (fx attach / skin reads of alpha_).
  alpha_ = tick_alpha_;
  for (int i = 0; i < st.ticks; ++i) tick();
  tick_alpha_ = st.alpha;
  alpha_ = fx_slow_alpha(st.alpha);
  for (LogLine& line : log_) line.ttl -= dt;
  std::erase_if(log_, [](const LogLine& line) { return line.ttl <= 0; });
  if (ui_ == Ui::Result) result_age_ += dt;

  // The menu's hand-over steps after a PC's element command hold the next menu (not the end of a finished battle).
  const bool handing_over = cast_handover_ > 0 && battle_->outcome() == battle::Outcome::Ongoing;
  const bool busy = !queue_.empty() || act_.phase != Act::Phase::None || !pending_.empty() || script_busy() || handing_over;
  // Dev (route battle_dash): every grid cell of PC 0 = element <id>, the other enemies down, the last one at 1 HP when an element is confirmed.
  if (const char* re = std::getenv("OC_ROUTE_ELEM"); re && !route_elem_done_) {
    route_elem_done_ = true;
    if (std::getenv("OC_ROUTE_GAUGE")) battle_->unit_mut(0).gauge = battle_->unit(0).gauge_max;
    for (int cell = 0; cell < data::kGridCells; ++cell) if (battle_->unit(0).cells[static_cast<size_t>(cell)] & 0x3FFF) battle_->unit_mut(0).cells[static_cast<size_t>(cell)] = static_cast<u16>(std::atoi(re));
    for (int i = battle::kPcSlots + 1; i < battle::kUnits; ++i) if (battle_->unit(i).present) { battle_->unit_mut(i).hp = 0; battle_->unit_mut(i).lo |= battle::st::kKO; }
  }
  // Dev: the harness action waits for the spawn scripts (S5 entry 8, start_spawn) - the first command of a real battle comes after them; an
  // action at tick 2 would cut Serge's entry 8 while the model is still hidden, leaving him invisible for the whole fight.
  bool spawning = false;
  for (int i = 0; i < battle::kUnits; ++i) spawning = spawning || (battle_->unit(i).present && vis_[static_cast<size_t>(i)].spawn && vis_[static_cast<size_t>(i)].vm.running());
  // Dev: a pending harness cast does not wait for a button on the scene's script windows.
  if (dbg_.cast_element > 0 && ui_ != Ui::Intro) smsg_input(true);
  // Dev: ocbattleview --cast / --ecast / --weapon.
  if ((dbg_.cast_element > 0 || dbg_.weapon >= 0) && ui_ != Ui::Intro && !busy && !spawning && finish_ == Finish::None) {
    const int el = dbg_.cast_element, weapon = dbg_.weapon;
    dbg_.cast_element = 0;
    dbg_.weapon = -1;
    // Dev: OC_GHOST="u:flag,..." sets the script ghost word of those units before the cast (1 = off-stage, 0 = shown), so a ghost-pair scene's
    // harness cast starts from the state a real script run would have reached (scene 431: Bunyip's two forms, u3 hidden / u4 shown).
    if (const char* ghost_env = std::getenv("OC_GHOST"); ghost_env && brain_.vm())
      for (std::string rest(ghost_env); !rest.empty();) {
        const size_t comma = rest.find(','), colon = rest.find(':');
        if (colon != std::string::npos && colon < comma) {
          const int gu = std::atoi(rest.c_str());
          const bool on = std::atoi(rest.c_str() + colon + 1) != 0;
          brain_.vm()->set_hidden(gu, on);
          if (gu >= 0 && gu < battle::kUnits) ghost_view_[static_cast<size_t>(gu)] = on;
        }
        rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
      }
    // Dev: OC_DEV_FORM is the creature slot (-1 = own form).
    if (const char* form_env = std::getenv("OC_DEV_FORM"); form_env && el == 238) pending_form_ = std::atoi(form_env), pending_form_unit_ = std::max(0, dbg_.cast_actor);
    int first = -1;
    for (int i = battle::kPcSlots; i < battle::kUnits && first < 0; ++i)
      if (battle_->unit(i).present && battle_->unit(i).alive()) first = i;
    // Dev: OC_DEV_WTGT=<unit> aims the --weapon attack at that unit, OC_DEV_HP1=1 leaves it with 1 HP first (a scripted death / form-2 kill:
    // `OC_GHOST=3:1,4:0 OC_DEV_WTGT=4 OC_DEV_HP1=1 ocbattleview 431 --weapon 2`).
    if (weapon >= 0) {
      if (const char* wt = std::getenv("OC_DEV_WTGT"); wt && *wt) first = std::atoi(wt);
      // "owedK" = the K-th (0-based) enemy unit the AI scripts have marked with op 5C.
      if (const char* wt = std::getenv("OC_DEV_WTGT"); wt && !std::strncmp(wt, "owed", 4)) {
        int owed_index = std::atoi(wt + 4);
        first = -1;
        for (int i = battle::kPcSlots; i < battle::kUnits; ++i)
          if (battle_->unit(i).present && brain_.death_owed(i) && owed_index-- == 0) first = i;
        log::info("deathtrace: owed unit {}", first);
      }
      if (first >= 0 && first < battle::kUnits && std::getenv("OC_DEV_HP1") != nullptr) battle_->unit_mut(first).hp = 1;
      // Dev: OC_DEV_ONLY=1 also downs every other enemy unit (hp 0 + KO, no kill script), so the one kill ends the fight (victory check of a scripted death)
      if (first >= 0 && std::getenv("OC_DEV_ONLY") != nullptr)
        for (int i = battle::kPcSlots; i < battle::kUnits; ++i)
          if (i != first && battle_->unit(i).present) battle_->unit_mut(i).hp = 0, battle_->unit_mut(i).lo |= battle::st::kKO;
    }
    if (weapon >= 0 && first >= 0) {
      battle_->do_weapon_attack(dbg_.lead, first, weapon);
      // Reactions + the end test, as Battle::apply does after a command.
      battle_->finish_external_action();
      collect_events();
      ui_ = Ui::Wait;
    } else if (dbg_.cast_actor >= 0 && dbg_.cast_target >= 0 && static_cast<size_t>(el) < tables_->elements.size()) {
      // Debug tab: an explicit caster and target. An all-targets element (pgdat +4 bit 0x10) hits every present unit on the target's side that
      // the all-target cast mask allows.
      const int ca = dbg_.cast_actor, ct = dbg_.cast_target;
      dbg_.cast_actor = dbg_.cast_target = -1;
      if (ca < battle::kUnits && ct < battle::kUnits && battle_->unit(ca).present && battle_->unit(ct).present) {
        const battle::TargetSpec ts = battle::target_spec(tables_->elements[static_cast<size_t>(el)]);
        const bool pc_side = ct < battle::kPcSlots;
        // Party + enemies + all (elements 46 53 65 71 77 83 84 ...) is every unit of both sides (Serge faces the bounding-box midpoint
        // of all units, yaw 1024, and the charge camera frame follows).
        const bool both = ts.all && ts.party && ts.enemies;
        const int lo = both ? 0 : ts.all ? (pc_side ? 0 : battle::kPcSlots) : ct, hi = both ? battle::kUnits : ts.all ? (pc_side ? battle::kPcSlots : battle::kUnits) : ct + 1;
        bool ring = true;
        const u32 reach = battle_->cast_mask();
        dev_group_hint(ca, el);
        // Dev: OC_CAST_PARTY=n limits a party-side all-targets cast to the first n PCs. With Serge alone (units 0, 3, 4, 5) a per-target
        // sequence loop (op 0x4D) runs once, and three times with the full party.
        static const int party_cap = std::getenv("OC_CAST_PARTY") ? std::atoi(std::getenv("OC_CAST_PARTY")) : 99;
        // A cast the other side's trap holds is caught instead (ocbattleview --etrap / --ptrap).
        const bool caught = battle_->catch_in_trap(ca, el, 0, ct);
        for (int i = lo; i < hi && !caught; ++i) {
          if (!battle_->unit(i).present) continue;
          if (i < battle::kPcSlots && i >= party_cap) continue;
          if (ts.all && i >= battle::kPcSlots && !(reach >> i & 1u)) continue;
          battle_->do_element(ca, el, 0, i, ring);
          ring = false;
        }
        battle_->set_group_hint(std::array<u8, 6>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
        // A kill by the Debug tab's cast ends the battle like any other.
        battle_->finish_external_action();
        collect_events();
        ui_ = Ui::Wait;
      }
    } else if (dbg_.enemy_cast && first >= 0 && static_cast<size_t>(el) < tables_->elements.size()) {
      // The enemy's own side for the party-side elements (party-only target spec), else the first PC.
      const battle::TargetSpec ts = battle::target_spec(tables_->elements[static_cast<size_t>(el)]);
      dev_group_hint(first, el);
      if (const int target = ts.party && !ts.enemies ? first : 0; !battle_->catch_in_trap(first, el, 0, target)) battle_->do_element(first, el, 0, target, true);
      battle_->set_group_hint(std::array<u8, 6>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
      battle_->finish_external_action();
      collect_events();
      ui_ = Ui::Wait;
    } else if (first >= 0 && static_cast<size_t>(el) < tables_->elements.size()) {
      const battle::TargetSpec ts = battle::target_spec(tables_->elements[static_cast<size_t>(el)]);
      // ally: the party side.
      const bool ally = ts.party && !ts.enemies;
      const int lo = ally ? 0 : first, hi = ally || !ts.enemies ? battle::kPcSlots : battle::kUnits;
      const bool caught = battle_->catch_in_trap(dbg_.lead, el, 0, lo);
      for (int i = lo; i < hi && !caught; ++i) {
        if (!battle_->unit(i).present || (ts.ko_only ? battle_->unit(i).alive() : !battle_->unit(i).alive()) || (!ts.all && i != lo)) continue;
        battle_->do_element(dbg_.lead, el, 0, i, i == lo);
      }
      battle_->finish_external_action();
      collect_events();
      ui_ = Ui::Wait;
    }
    handle_input(in);
    apply_camera(alpha_);
    build_hud();
    return;
  }
  // The PC's turn is over: the menu passes on (the next PC's window grows while any enemy actions run).
  if (turn_open_ && turn_over()) {
    turn_open_ = false;
    if (battle_->outcome() == battle::Outcome::Ongoing && finish_ == Finish::None) begin_menu_entry(menu_pc_after());
  }
  if (ui_ != Ui::Intro && !busy && finish_ == Finish::None) {
    if (battle_->outcome() == battle::Outcome::Ongoing) {
      if (ui_ == Ui::Wait && engage_arrive_) {
        // approach act still owns the PC (handled by tick_action)
      } else if (ui_ == Ui::Wait && engaged_ && active_pc_ >= 0) {
        sync_shown();
        const bool more = battle_->can_act(active_pc_) && battle_->unit(engaged_target_).alive() && !engaged_after_cast_;
        if (more) {
          ui_ = Ui::Chain;
          chain_auto_cursor();
        } else {
          engaged_after_cast_ = false;
          begin_disengage();
        }
      } else if (ui_ == Ui::Wait && battle_->menu_locked()) {
        // Op AF: no command menu opens while the script lock is set (evt409 locks it until its first message is read).
      } else if (ui_ == Ui::Wait) {
        ui_ = Ui::Top;
        top_cursor_ = 0;
        sync_shown();
        // The top menu reopens on the PC that attacked while it can still act - after its chain ends, an enemy turn in between or the
        // target's death; after an Element / Defend command (or when it cannot act) it goes to the next PC in slot order (actor + 1,
        // actor + 2, the actor itself last). -1 = nobody: handle_input settles / re-picks.
        active_pc_ = menu_pc_after();
        if (cursor_memory_ && active_pc_ >= 0 && active_pc_ < battle::kPcSlots) top_cursor_ = top_mem_[active_pc_];
        // The first menu of the battle, or the PC the turn's end picked can no longer act: the window opens for this PC.
        if (active_pc_ >= 0 && active_pc_ != entry_pc_) begin_menu_entry(active_pc_);
      }
      bool any = false;
      for (int i = 0; i < battle::kPcSlots; ++i) any = any || battle_->can_act(i);
      // Nobody can open a menu: let the rules run on.
      if (!any) {
        battle_->settle();
        collect_events();
      }
    } else {
      begin_finish();
    }
  }
  handle_input(in);
  // The PC whose top menu has taken over gives up its guard, however the menu came to it; the idle script then drops the block pose.
  if (menu_live() && battle::is_pc_slot(active_pc_) && !battle_->menu_locked()) battle_->open_menu(active_pc_);
  sync_menu_facing();
  // HUD projections use the current camera.
  apply_camera(alpha_);
  build_hud();
}

// ------------------------------------------------------------------------------------------------------------ events
void BattleView::collect_events() {
  std::vector<battle::ActionEvent> ev = battle_->drain_events();
  for (battle::ActionEvent& event : ev) {
    if (queue_.empty() || queue_.back().front().action != event.action) queue_.emplace_back();
    queue_.back().push_back(event);
  }
  // The panel shows each event's own line when it is presented.
  for (const std::string& line : battle_->drain_log()) log::debug("battle: {}", line);
  // Battle-script presentation requests (pac_vm.h Event): presented by script_event when the rules emit them (the scripts run inside the
  // rules' turn, so a request shows when that turn is collected, not in step with the replay of its hits).
  for (const battle::pac::Event& event : brain_.drain_events()) {
    log::debug("battle script event {} unit {} a {} b {} vt {} seq {}", static_cast<int>(event.kind), event.unit, event.a, event.b, event.vt, event.seq);
    // The battle script's first run belongs to the command phase (after the bti / pli intro): its shot (ev01 ...) and window come after
    // the party intro, not over it: script_pump waits while Ui::Intro.
    script_stream_.push_back(event);
  }
}

void BattleView::begin_engage(int pc, int target) {
  act_ = Act{};
  act_.actor = pc;
  act_.targets = {target};
  engaged_target_ = target;
  engage_arrive_ = true;
  engage_interrupted_ = false;
  Visual& visual = vis_[static_cast<size_t>(pc)];
  audio_.activate(visual.sound_key);
  camera_.stop_sequences();
  camera_.set_bank(visual.scripts ? &visual.scripts->cam : &bank_);
  face_toward(pc, target);
  select_approach();
  act_.phase = Act::Phase::Approach;
  if (!(visual.scripts && visual.scripts->has_s5 && start_script(pc, &visual.scripts->s5, 2, 0, kApproach))) {
    visual.script_done = true;
    const float dist = approach_distance(pc, target);
    const Visual& target_visual = vis_[static_cast<size_t>(target)];
    const float dx = target_visual.x - visual.x, dz = target_visual.z - visual.z, length = std::max(1.0f, std::hypot(dx, dz));
    begin_move(pc, visual.x + dx / length * dist, visual.z + dz / length * dist, 30, false);
  }
  ui_ = Ui::Wait;
}

// The way home ends the camera's tasks first; a cancelled HIT% menu then starts the approach shot again for the hop, any other end of the turn
// leaves the pose to the menu camera.
void BattleView::begin_disengage(bool approach_shot) {
  const int pc = active_pc_;
  const bool was_engaged = engaged_;
  engaged_ = false;
  engage_arrive_ = false;
  if (pc < 0 || !act_.ev.empty() || act_.phase != Act::Phase::None) return;
  if (was_engaged) {
    end_camera_tasks();
    if (approach_shot) select_approach();
  }
  Visual& visual = vis_[static_cast<size_t>(pc)];
  if (!battle_->unit(pc).alive()) {
    begin_move(pc, visual.home_x, visual.home_z, 0, true);
    return;
  }
  act_.actor = pc;
  start_return();
  ui_ = Ui::Wait;
}

// A pick of the percentage menu: one hit at once, from where the PC stands.
void BattleView::chain_pick(int strength) {
  battle::Command cmd;
  cmd.kind = battle::Command::Kind::Attack;
  cmd.actor = active_pc_;
  cmd.target = engaged_target_;
  cmd.chain = {strength};
  execute(cmd);
}

void BattleView::execute(const battle::Command& cmd) {
  std::string why;
  if (std::getenv("OC_ROUTE_ELEM") && cmd.kind == battle::Command::Kind::Element) battle_->unit_mut(cmd.target).hp = 1;
  if (!battle_->apply(cmd, &why)) {
    log_.push_back({import::utf8_to_u32("Cannot: " + why), 4.0f});
    battle_audit_note(std::format("apply refused: actor {} kind {} cell {} target {}: {}", cmd.actor, static_cast<int>(cmd.kind), cmd.cell, cmd.target, why));
    ui_ = Ui::Top;
    return;
  }
  collect_events();
  chain_.clear();
  // The engaged PC keeps its menu while it can still act; otherwise update() picks the next PC (next_pc_after).
  active_pc_ = cmd.actor;
  last_actor_ = cmd.actor;
  last_kind_ = cmd.kind;
  turn_open_ = true;
  ui_ = Ui::Wait;
}

// The Dopplegang list: entry 0 = the caster's character, then every pool slot 0..255 whose bit is set, ascending, no dedupe (two slots
// may share a name, e.g. the two Dodo rows) - also the bits past the 220-row creature table (the pick refuses those). The debug flag
// lists the table's non-zero `val` rows instead (`OC_DOPPLE_SLOTS=debug`).
void BattleView::open_dopple() {
  dop_list_.assign(1, -1);
  for (const int slot : battle_->dopple_pool()) dop_list_.push_back(slot);
  dop_cur_ = dop_top_ = 0;
  // The scroll limit = 1 + (n8 - 11) - (n8 - 11) / 2 (C division), negative -> 0; n8 = the creature count in a BYTE, so all 256
  // pool bits wrap to 0 and the list cannot scroll (0 with 256 bits, 12 with 32).
  const int n8 = static_cast<int>(dop_list_.size() - 1) & 0xFF;
  dop_max_ = std::max(0, 1 + (n8 - 11) - (n8 - 11) / 2);
  dop_len_.clear();
  for (int k = 0; k < static_cast<int>(dop_list_.size()); ++k) dop_len_.push_back(static_cast<int>(dop_name(k).size()));
  dop_named_ = dop_typing_ = 0;
  dop_left_ = dop_len_.empty() ? 0 : dop_len_[0];
  // The info window and the target cursor show the caster while the list is open.
  tgt_mode_ = Tgt::Self;
  targets_.assign(1, active_pc_);
  target_cursor_ = 0;
  ui_ = Ui::Dopple;
}

// A form change: the Unit record is swapped by Battle::transform; the model, scripts, sounds and weapon follow, the idle clip restarts.
void BattleView::apply_form(int unit, int form) {
  if (unit < 0 || unit >= battle::kPcSlots || !battle_->transform(unit, form)) return;
  bind_unit_model(unit);
  form_show_unit_ = unit;
  Visual& visual = vis_[static_cast<size_t>(unit)];
  visual.script_pose = 1;
  if (visual.has_model) {
    visual.pose.play(1, 0);
    start_idle(unit);
    float hx = 0, hy = 0, hz = 0;
    // The height word of the new model.
    if (visual.pose.attach_now(14, 1.0f, hx, hy, hz) && -hy > 50.0f) visual.height = -hy;
  }
  trail_eye_reset(unit);
  log::info("battle: unit {} form -> {} ({})", unit, form, battle_->unit(unit).name);
}

void BattleView::apply_pip_form(int unit) {
  if (unit < 0 || unit >= battle::kPcSlots) return;
  // The model is rebuilt but the unit keeps its hidden flag: the caster's anmseq hid it (op 0x1A at the `chng` start) while the pack's act0
  // figure stands in, so the unit stays hidden from the hide through the swap until the action ends (`form_show_unit_`).
  const bool hid = anm_hidden(unit);
  bind_unit_model(unit);
  anm_set_hidden(unit, hid);
  form_show_unit_ = unit;
  Visual& visual = vis_[static_cast<size_t>(unit)];
  visual.script_pose = 1;
  if (visual.has_model) {
    visual.pose.play(1, 0);
    start_idle(unit);
    float hx = 0, hy = 0, hz = 0;
    // The height word of the new model.
    if (visual.pose.attach_now(14, 1.0f, hx, hy, hz) && -hy > 50.0f) visual.height = -hy;
  }
  trail_eye_reset(unit);
  log::info("battle: tick {} unit {} form -> kind {:#x} ({})", tick_count_, unit, battle_->unit(unit).kind, unit_model_stem(battle_->unit(unit)));
}

void BattleView::fx_form_phase(int phase) {
  log::debug("battle: hensin form phase {} (pending form {})", phase, pending_form_);
  // tumaru 0x181 or the action's end.
  if (pending_evolve_unit_ >= 0) {
    const int unit = pending_evolve_unit_;
    pending_evolve_unit_ = -1;
    apply_pip_form(unit);
    return;
  }
  // Phase 1 = hensin 0x181 (the swap), 2 = 0x182 / 0x183 or the action's end (the fallback).
  if (pending_form_ == -2) return;
  const int form = pending_form_, unit = pending_form_unit_;
  pending_form_ = -2;
  apply_form(unit, form);
}

void BattleView::fx_set_pose(int unit, int pose) {
  if (unit < 0 || unit >= battle::kUnits || !battle_->unit(unit).present) return;
  Visual& visual = vis_[static_cast<size_t>(unit)];
  visual.script_pose = pose;
  // A running idle picks the word up; one that ran out stays on its last pose.
  if (visual.script_kind == kIdle && visual.vm.running()) start_idle(unit);
}

int BattleView::next_pc_after(int pc) const {
  if (pc < 0) pc = battle::kPcSlots - 1;
  for (int k = 1; k <= battle::kPcSlots; ++k) {
    const int slot = (pc + k) % battle::kPcSlots;
    if (battle_->can_act(slot)) return slot;
  }
  return -1;
}

int BattleView::menu_pc_after() const {
  if (last_actor_ >= 0 && last_kind_ == battle::Command::Kind::Attack && battle_->can_act(last_actor_)) return last_actor_;
  return next_pc_after(last_actor_ >= 0 ? last_actor_ : active_pc_);
}

std::string BattleView::route_menu() const {
  std::string out = menu_live() && battle::is_pc_slot(active_pc_) ? std::to_string(active_pc_) : std::string("-");
  out += ' ';
  for (int i = 0; i < battle::kPcSlots; ++i) out += battle_ && battle_->unit(i).guard ? '1' : '0';
  return out;
}

std::string BattleView::route_facing() const {
  std::string out;
  for (int i = 0; i < battle::kPcSlots; ++i) {
    const Visual& visual = vis_[static_cast<size_t>(i)];
    const int yaw = static_cast<int>(std::lround(std::atan2(-visual.face_x, -visual.face_z) * (4096.0f / (2.0f * kPi)))) & 0xFFF;
    out += (i ? " " : "") + (battle_ && battle_->unit(i).present ? std::to_string(yaw) : std::string("-"));
  }
  return out;
}

// In the top command menu Right / Square / R1 pick the NEXT PC and Left / L1 the PREVIOUS one: the three slots are walked from active +- 1
// (wrapping 0..2) and the first that can act is taken (a character, no KO / blocking status 0x20304080, stamina > 9); the third candidate is the
// active PC itself = nobody else can act: the menu stays on it, with no sound. Either way the PC's idle clip restarts (a guarding PC's block
// pose plays again until its top menu takes over) and the menu entry starts again. A change also ends the camera's tasks, plays SFX 0x16 and
// restores the PC's cursor memory (the facing pass is sync_menu_facing).
void BattleView::switch_active_pc(int dir) {
  if (ui_ != Ui::Top || active_pc_ < 0 || dir == 0 || engaged_) return;
  int slot = active_pc_;
  for (int k = 1; k < battle::kPcSlots; ++k) {
    const int next = ((active_pc_ + (dir > 0 ? k : -k)) % battle::kPcSlots + battle::kPcSlots) % battle::kPcSlots;
    if (!battle_->can_act(next)) continue;
    slot = next;
    break;
  }
  if (slot != active_pc_) {
    end_camera_tasks();
    active_pc_ = slot;
    top_cursor_ = cursor_memory_ ? top_mem_[slot] : 0;
    audio_.play_menu(0x16);
  }
  start_idle(slot);
  begin_menu_entry(slot);
}

void BattleView::begin_menu_entry(int pc) {
  entry_pc_ = pc;
  entry_age_ = 0;
  entry_ready_ = false;
  entry_live_ = -1;
}

// Once a tick: the window grows; then the menu state steps on and the top menu takes over, each on a tick when the menu is free (the
// actions in between hold both).
void BattleView::tick_menu_entry() {
  if (entry_pc_ < 0) return;
  if (entry_live_ >= 0) {
    entry_live_ = std::min(entry_live_ + 1, 2);
    return;
  }
  const bool free = ui_ == Ui::Top && active_pc_ == entry_pc_;
  if (entry_ready_) {
    if (free) entry_live_ = 0;
  } else if (entry_age_ < kEntryGrowTicks) {
    ++entry_age_;
  } else if (free) {
    entry_ready_ = true;
  }
}

bool BattleView::turn_over() const {
  if (ui_ != Ui::Wait || engaged_ || engage_arrive_ || cast_handover_ > 0) return false;
  if (!queue_.empty() && queue_.front().front().actor == last_actor_) return false;
  const bool defending = !act_.ev.empty() && act_.ev.front().kind == battle::ActionEvent::Kind::Defend;
  return act_.phase == Act::Phase::None || act_.actor != last_actor_ || defending;
}

// The facing of the units outside an action.
//  * Entering the top menu turns nobody home; the home pass belongs to the step after a PC's ELEMENT command (tick_action).
//  * While the target ring is open every cursor placement (opening, or a move) turns the active PC to the cursor unit (atan2 of the position
//    difference); the cursor on the PC itself gives the home facing byte. Dead units are skipped.
//    Cancelling back to the top menu does NOT run it (the PC keeps the cursor yaw).
void BattleView::sync_menu_facing() {
  const bool idle = act_.phase == Act::Phase::None && queue_.empty() && finish_ == Finish::None && !engage_arrive_;
  if (!idle || active_pc_ < 0 || !battle_) {
    menu_face_ui_ = ui_;
    tgt_face_unit_ = -2;
    return;
  }
  // Entering the top menu runs NO home pass: the home pass is the step AFTER AN ELEMENT COMMAND was issued; an attack and Defend only
  // re-enter the menu, and so do the L1 / R1 switch and the end of an enemy turn.
  // So a unit keeps the yaw an action gave it (the PC an enemy attacked stays turned to the attacker) until its own action ends (end_group)
  // or the step after a PC's element command sweeps everyone (tick_action).
  if (ui_ == Ui::Target && !targets_.empty()) {
    const int cursor_unit = targets_[static_cast<size_t>(std::clamp(target_cursor_, 0, static_cast<int>(targets_.size()) - 1))];
    if (cursor_unit != tgt_face_unit_ || menu_face_ui_ != Ui::Target) {
      tgt_face_unit_ = cursor_unit;
      Visual& active = vis_[static_cast<size_t>(active_pc_)];
      if (cursor_unit == active_pc_) {
        if (!active.ko_shown) {
          active.turn.on = false;
          active.face_x = active.pface_x = active.home_fx;
          active.face_z = active.pface_z = active.home_fz;
        }
      } else {
        active.turn.on = false;
        face_toward(active_pc_, cursor_unit);
      }
    }
  } else {
    tgt_face_unit_ = -2;
  }
  menu_face_ui_ = ui_;
  menu_face_pc_ = active_pc_;
}

// In the DYNAMIC target camera (toggled with Triangle) EVERY present unit is semi-transparent except the cursor unit; leaving the target menu
// (confirm, cancel, PC switch) clears the flag on all and unhides the living, non-ghost ones. The blend is the model's own ABR, set at unit
// load to ABR 0 = (B + F) / 2.
bool BattleView::dynamic_translucent(int unit) const {
  if (ui_ != Ui::Target || !cam_dynamic_ || targets_.empty()) return false;
  return unit != targets_[static_cast<size_t>(std::clamp(target_cursor_, 0, static_cast<int>(targets_.size()) - 1))];
}

void BattleView::sync_shown() {
  for (int i = 0; i < battle::kUnits; ++i) {
    const battle::Unit& unit = battle_->unit(i);
    Visual& visual = vis_[static_cast<size_t>(i)];
    if (!unit.present) continue;
    visual.hp_shown = unit.hp;
    const bool ko = !unit.alive();
    if (ko != visual.ko_shown) {
      visual.ko_shown = ko;
      if (!ko) visual.ko_hit = visual.ko_reacted = false, visual.zero_hp_actions = 0;
      // Re-pick the idle / KO pose.
      if (visual.script_done && visual.script_kind == kIdle) start_idle(i);
    }
  }
}

}  // namespace oc::game
