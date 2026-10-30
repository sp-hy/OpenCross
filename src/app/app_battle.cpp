// The field -> battle hand-off (field A3 / C9): the field has faded to black; the BattleView runs on the field's party, then the
// result is handed back (rewards, HP, the game-over room).
#include "app/app.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>

#include "battle/rng.h"
#include "battle/rules.h"
#include "core/log.h"
#include "game/game_session.h"

namespace oc::app {

FieldBattleArgs App::make_field_battle_args(const game::FieldWorld::BattleRequest& rq) {
  FieldBattleArgs battle_args;
  battle_args.rq = rq;
  // Reproducible fights: the view reads its seed from this variable.
  if (config.debug_seed) {
    const std::string seed = std::to_string(config.debug_seed * 2654435761u + ++battle_counter * 40503u);
    _putenv_s("OC_BATTLE_SEED", seed.c_str());
  }
  if (tables_ok) {
    // The encounter's variant roll comes before the battle's generator starts: a stream of its own, derived from the battle seed so
    // seeded fights repeat their enemy group too.
    u32 seed = static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count());
    if (const char* env = std::getenv("OC_BATTLE_SEED")) seed = static_cast<u32>(std::strtoul(env, nullptr, 10)) ^ 0x5CE7E5u;
    battle::GameRng rng(seed, tables.rng_table);
    battle_args.scene = battle::encounter_scene(rng, tables, rq.battle_id);
  }
  std::vector<battle::PartyMember> party;
  std::vector<int> battle_ids = world.party_chars();
  {  // Relief Charm: outside battle 0x3DD the relief member takes every battle slot holding Serge
    const int relief = world.game_state().slot.head[ui::kReliefHead];
    if (rq.battle_id != 0x3DD && relief != 0xFF && std::find(battle_ids.begin(), battle_ids.end(), relief) == battle_ids.end())
      for (int& id : battle_ids)
        if (id == 0) id = relief;
  }
  if (tables_ok)
    for (int id : battle_ids) party.push_back(game::party_member_from_state(tables, flow_ready ? flow.game_state() : world.game_state(), id,
                                                                                       flow_ready ? &flow.machine().custom_names() : nullptr));
  game::BattleView::StartOptions& bopt = battle_args.bopt;
  bopt.flags = rq.flags;
  // An id > 0x1FF plays the stage's 'boss' intro.
  bopt.battle_id = rq.battle_id;
  // The A3 / C9 battlefield operand picks the stage (battle/bg/bg_NN); the scene default applies only when it is out of range.
  bopt.battlefield = rq.field;
  if (tables_ok) {
    std::vector<int> ids;
    for (const battle::PartyMember& member : party) ids.push_back(member.char_id);
    const ui::GameState& gstate = flow_ready ? flow.game_state() : world.game_state();
    game::BattleSetup setup = game::battle_setup(tables, gstate, ids, growth_progress, flow_ready ? &flow.machine().custom_names() : nullptr);
    bopt.ng_plus = setup.ng_plus;
    bopt.growth = setup.growth;
    bopt.bench = std::move(setup.bench);
    bopt.dopple_mask = world.dopple_mask();
    bopt.piggy_bank = world.vm().var(0x176);
    // The last Grand Slam picks; only a monster-party scene uses them.
    bopt.monster_party = world.monster_party();
  }
  battle_args.party = std::move(party);
  return battle_args;
}

// The load runs in slices of the frozen hold (the PS1 loads behind the white); the take-over frame then only finishes the start.
void App::field_battle_load_step() {
  if (!battle_ready) return;
  if (!fb_args) {
    fb_args = make_field_battle_args(world.peek_battle());
    if (fb_args->scene >= 0) battle.begin_load(fb_args->scene, fb_args->party, fb_args->bopt);
  }
  const bool loaded = fb_args->scene >= 0 && battle.load_step(3.0);
  // The battle 3D is drawn at the scene target's size: its effect targets are made one per frame once the load is done or the swirl's
  // first second has passed (they do not depend on the load, which can end only a few frames before the battle starts).
  if (fb_args->scene < 0 || config.ps1_res || !(loaded || fb_req_frames >= 60)) return;
  u32 tw = 0, th = 0;
  scene_extent(static_cast<float>(ctx.extent().width), static_cast<float>(ctx.extent().height), tw, th);
  battle.prepare_fx_targets(tw, th);
}

void App::start_field_battle() {
  // Read before take_battle() ends the intro.
  const int strip_clock = world.transition_main_clock();
  const game::FieldWorld::BattleRequest rq = world.take_battle();
  battle_flags = rq.flags;
  if (!fb_args) fb_args = make_field_battle_args(rq);
  FieldBattleArgs fa = std::move(*fb_args);
  fb_args.reset();
  const int scene = fa.scene;
  std::vector<battle::PartyMember> party = std::move(fa.party);
  const game::BattleView::StartOptions bopt = std::move(fa.bopt);
  if (battle_ready && scene >= 0 && battle.start(scene, std::move(party), bopt)) {
    // The field's last feed-back output (fb_rt[fb_cur]) is the 'previous output' of the battle's first frames: no black between the two.
    // The anchor is the last finished strip update's output.
    const render::TexRef fb_anchor = fb_flip_next ? fb_set[fb_cur] : fb_set[1 - fb_cur];
    battle.begin_transition(ctx, fb_last_n >= 0 && fb_anchor ? fb_anchor : render::TexRef{}, strip_clock);
    battle_in_field = true;
    battle_scene = scene;
    ++battle_starts;
    log::info("battle: field encounter id {:#x} -> scene {}", rq.battle_id, scene);
  } else {
    log::warn("battle: could not start encounter id {:#x} (scene {}); skipped as fled", rq.battle_id, scene);
    world.end_battle(2);
  }
}

void App::finish_field_battle() {
  int code = 0;
  switch (battle.outcome()) {
    case battle::Outcome::Victory: code = 0; break;
    case battle::Outcome::Wiped: code = 1; break;
    // A scripted end (op 83) gives its own code (3 / 4 ...).
    case battle::Outcome::Scripted: code = battle.battle() ? battle.battle()->result_code() : 3; break;
    default: code = 2; break;
  }
  battle_in_field = false;
  // RDE's battle counter: +1 when a fight ends in result 0 or 3, capped.
  if (code == 0 || code == 3) ext_host.battles = std::min<u32>(ext_host.battles + 1, 0xFFFF);
  achievements.battle_ended(battle_scene, code, ext_host.battles);
  // A monster-party fight (the Grand Slam) writes nothing back: no rewards, records, HP or Dopplegang pool.
  const bool written_back = battle.battle() && !battle.battle()->monster_party();
  if (written_back) game::apply_battle_result(world.game_state(), tables, *battle.battle(), &growth_progress);
  // The Dopplegang pool goes back to the globals.
  if (written_back) world.set_dopple_mask(battle.battle()->dopple_mask());
  // The results screen's HP: growth gain + heal menu.
  if (std::array<int, battle::kPcSlots> hp; results_ok && written_back && results.take_final_hp(hp)) {
    const auto& pm = battle.battle()->party();
    for (size_t i = 0; i < pm.size() && i < hp.size(); ++i)
      if (hp[i] >= 0 && pm[i].char_id >= 0 && pm[i].char_id < save::kNumChars) {
        save::SaveChar& save_char = world.game_state().slot.chars[static_cast<size_t>(pm[i].char_id)];
        save_char.cur_hp = static_cast<u16>(std::clamp(hp[i], 1, static_cast<int>(save_char.max_hp)));
      }
  }
  world.end_battle(code);
  if (code == 1 && !(battle_flags & 2)) {
    // Party wiped in a fight that is not losable (BATTLE flag 2 clear): the next map becomes 9 (the game-over room, map digit 009 = the
    // room file after the title room 008) and is loaded; its own script shows the screen and leaves. The title is only the fallback
    // when that room cannot be loaded.
    const std::string over = world.room_stem_for_id(9);
    log::info("game over: map 9 ({})", over.empty() ? "?" : over);
    mixer.stop();
    if ((over.empty() || !world.load_room(over, 0)) && flow_ready) flow.start(game::FlowState::Title);
  }
}

// Title scene (attract demo) battle request. dem_06's `C9 #415` is reachable only with ROOM_PARAM 3 (the attract chain enters it with
// 1 = the cutscene path), i.e. never from the title scene; a request that still arrives in scene mode is never a real battle (the dummy
// title party would be wiped and GAME OVER would show): it is dropped and the script is told "won" (result 0).
void App::scene_battle() {
  if (!world.battle_requested() || !world.battle_ready()) return;
  log::info("title scene: battle request {} dropped, reported as won", world.peek_battle().battle_id);
  fb_args.reset();
  world.take_battle();
  world.end_battle(0);
}

}  // namespace oc::app
