// FieldWorld: the camera, screen fades, map jumps and the battle hand-off (request, veil, disguise, take and end).
#include "game/field_world.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>

#include "audio/bgm_table.h"
#include "audio/voice.h"
#include "core/log.h"
#include "game/cheats.h"
#include "game/field_world_internal.h"

namespace oc::game {

using namespace field_internal;

namespace {

// Fade back after a failed map jump (no retail rule: the jump cannot fail there).
constexpr int kFadeTicks = 20;
// Slot-0 screen fades, stepped once per tick: FE 02 kind 0 = 0x50 ticks out / 0x30 in, kind 2 = 0x40 out / 0x20 in; every field entry
// (battle return, loaded save) fades in over 0x30 ticks from black.
constexpr int kRoomFadeOut = 0x50, kRoomFadeIn = 0x30, kWhiteFadeOut = 0x40, kWhiteFadeIn = 0x20;

}  // namespace

void FieldWorld::camera_set_position(s32 x, s32 y, int frames) { camera_.set_position(view_, vm_.global_flags(), x, y, frames); }

void FieldWorld::camera_to_actor(int frames) { camera_.to_actor(view_, vm_.global_flags(), pose_of(player_entity()), frames); }

void FieldWorld::camera_follow(int ent, int frames) { camera_.follow(view_, vm_.global_flags(), pose_of(ent), ent, frames); }

bool FieldWorld::camera_busy() { return camera_.busy(); }

void FieldWorld::camera_limits(s32 x0, s32 x1, s32 y0, s32 y1) { camera_.set_limits(x0, x1, y0, y1); }

void FieldWorld::camera_auto_follow(bool on) {
  // Op 86 drives the party followers (they replay the leader's trail), not the scroll.
  // 86 mode 0 sets the follow-freeze flag (`on` here), mode 1 waits for the formation and clears it. The follower logic is skipped
  // while the flag is set, so `on` freezes the followers.
  party_follow_ = !on;
  if (on && party_ent_[0] >= 0) {
    // Mode 0 also records where each follower has to stand: the party V around the leader's current spot (opn_a01i: the row of the
    // opening after the run-out); mode 1 waits until they got there (formation_done) and then releases the follow logic.
    const Ent& le = ents_[static_cast<size_t>(party_ent_[0])];
    const ActorPose& lp = le.actor.current();
    for (int slot = 1; slot < 3; ++slot) {
      float x = 0, z = 0;
      party_v_spot(slot, lp.x, lp.z, (le.actor.facing() - 0x800) & 0xFFF, x, z);
      // The slot records also carry the leader's facing, written on arrival like the 84 / DF records: the
      // followers walk a step back to their spots (opn_a01i lift: about 20 units behind them) and must turn back to the leader's way,
      // not keep facing the walk (they ended up facing backwards as the lift arrived).
      if (view_.walk().empty() || view_.walk().find_triangle(x, z, lp.y) >= 0) walk_party_member(slot, x, z, to_script(le.actor.facing()));
    }
  }
}

std::pair<s32, s32> FieldWorld::camera_position() { return camera_.position(); }

void FieldWorld::set_camera_origin(s32 x, s32 y) { camera_.set_origin(x, y); }

void FieldWorld::set_controlled_entity(int ent) {
  log::debug("field: tick {} controlled entity -> {} (script of ent {})", tick_count_, ent, vm_.current_entity());
  controlled_override_ = ent;
  controlled_cleared_ = ent < 0;
}

void FieldWorld::shake(int x, int y, int ticks) {
  const int count = std::max(1, (ticks & 0xFFFF) + 1);
  shake_.frames = count;
  shake_.dx = (x * 65536 - shake_.ax) / count;
  shake_.dy = (y * 65536 - shake_.ay) / count;
  shake_.tx = x;
  shake_.ty = y;
  log::debug("field: tick {} SETUP_SHAKE {} {} over {} ticks", tick_count_, x, y, count);
}

// (rand * a + 1 >> 15) - a / 2 with rand = 0..0x7FFF (the SLPS rand), a = the integer amplitude: uniform in [-a/2, a - a/2].
int FieldWorld::shake_offset(s32 amp) {
  const int amplitude = amp >> 16;
  if (amplitude <= 0) return 0;
  shake_.rng = shake_.rng * 1664525u + 1013904223u;
  const int random = static_cast<int>((shake_.rng >> 16) & 0x7FFF);
  return ((random * amplitude + 1) >> 15) - amplitude / 2;
}

// (rand * a >> 16) + a / 2 with rand = 0..0x7FFF: the picture moves right / down by [a/2, a) pixels.
int FieldWorld::display_shake_offset(int amp) {
  if (amp <= 0) return 0;
  shake_.rng = shake_.rng * 1664525u + 1013904223u;
  const int random = static_cast<int>((shake_.rng >> 16) & 0x7FFF);
  return ((random * amp) >> 16) + amp / 2;
}


// A8: mode 0 fades back to the picture, any other mode to black, over `arg` ticks (0 = at once), with the room change's subtractive
// slot-0 fade. It acts only when it flips the direction of the last slot-0 fade, and then starts from the far end: a fade to black
// while the screen is black or going black (room exit, room entry before the entry fade, an earlier A8) does nothing, and so does a
// fade back while the screen is coming back.
void FieldWorld::fade(int mode, int arg) {
  const bool to_black = mode != 0;
  if (to_black == fade_black_) return;
  fade_black_ = to_black;
  fade_target_ = to_black ? 1.0f : 0.0f;
  fade_add_ = false;
  fade_hold_ = 0;
  if (arg <= 0) {
    fade_ = fade_target_;
  } else {
    fade_ = to_black ? 0.0f : 1.0f;
    fade_step_ = 1.0f / static_cast<float>(arg);
  }
}

void FieldWorld::begin_entry_fade(int kind) {
  fade_hold_ = 0;
  fade_ = 1.0f;
  fade_target_ = 0.0f;
  fade_step_ = 1.0f / static_cast<float>(kind == 2 ? kWhiteFadeIn : kRoomFadeIn);
  fade_add_ = kind == 2;
  fade_black_ = false;
  fade_kind_ = kind;
}

void FieldWorld::draw_sync() {}

// ------------------------------------------------------------------------------------------------ host: flow
// Cheats "No encounters": the decision is game::encounter_auto_won / game::model_swap_skipped.
// The story bytes they read are global script variables (byte ids), read through game::story_byte_from_vars; -1 (outside the
// global array) errs on the side of the fight.
bool FieldWorld::skip_battle(int arg, bool c9, int ent) {
  if (!platform::cheats().no_encounters) return false;
  if (!game::encounter_auto_won(map_id_, ent, arg & 0x7FF, c9, [this](unsigned var) { return game::story_byte_from_vars(vm_.variables(), var); })) return false;
  handoff_.last_battle_code = 2;
  log::info("field: cheat No encounters: {} {:#x} skipped in {} (entity {})", c9 ? "C9" : "A3", arg & 0x7FF, view_.room_name(), ent);
  return true;
}
bool FieldWorld::skip_model_swap(int ent) {
  return platform::cheats().no_encounters && game::model_swap_skipped(map_id_, ent, [this](unsigned var) { return game::story_byte_from_vars(vm_.variables(), var); });
}

void FieldWorld::request_battle(int battle_id, int field, int flags) {
  if (handoff_.battle_state != BattleState::None) return;
  handoff_.battle_req = {battle_id, field, flags};
  // A3 / C9 handler: unless flag 1 (C9: keep the field music) the field song is suspended at once
  // - the encounter effect plays over silence and its own SFX; then the anchor entity is FE 70's entity (else the leader) and the
  // 'enct' task starts.
  // The field keeps running; the battle starts once the 'enct' task is gone (sim_tick).
  // There is no screen fade on the field side: the battle's opening clears the screen to white (BattleView::begin_transition).
  if (!(flags & 1) && mixer_) {
    // The song id and its position are saved here and restored when the same song is started again (the post-battle restore),
    // so the field music resumes where the encounter cut it (remastered source only).
    handoff_.resume_song = bgm_remastered_ ? mixer_->current_song() : -1;
    handoff_.resume_seconds = static_cast<double>(mixer_->bgm_position()) / audio::Mixer::kRate;
    mixer_->stop();
    handoff_.battle_music_stopped = true;
    // The battle song starts DURING the encounter effect, not at the battle load; BattleAudio::start_battle_theme then finds it playing.
    // Flag 0x40 picks the boss theme; the battle adopts the same song so there is no restart at the load.
    start_song(audio::battle_song(flags, bgm_remastered_), 127);
  }
  handoff_.battle_state = BattleState::Intro;
  handoff_.intro_tick = tick_count_;
  const bool enct = sequences_ && sequences_->start("enct");
  // No 'enct' task: nothing to wait for.
  if (!enct) handoff_.battle_state = BattleState::Ready;
  log::info("field: battle requested id {} field {} flags {:#x} in {}; encounter effect 'enct' {} (anchor ent {})", battle_id, field, flags,
            view_.room_name(), enct ? "started" : "unavailable", battle_intro_anchor());
}

bool FieldWorld::transition_white() const { return handoff_.transition_white(tick_count_); }

bool FieldWorld::transition_prefreeze() const { return handoff_.transition_prefreeze(tick_count_); }

int FieldWorld::transition_main_clock() const { return handoff_.transition_main_clock(tick_count_); }

FieldWorld::TransitionStrips FieldWorld::transition_strips() const { return handoff_.transition_strips(tick_count_); }

bool FieldWorld::battle_active() { return handoff_.battle_state != BattleState::None; }

void FieldWorld::apply_battle_disguise() { handoff_.apply_battle_disguise(gs_, party_chars_, tick_count_); }

FieldWorld::BattleRequest FieldWorld::take_battle() {
  handoff_.battle_state = BattleState::Running;
  apply_battle_disguise();
  // The engine saves the field and tears the effect system down; the field is rebuilt on the way back, so
  // nothing of the encounter effect (enc2, the e041 disc with life 293) survives the battle.
  if (sequences_) {
    sequences_->stop("enct");
    sequences_->stop("enc2");
  }
  if (emitters_)
    for (const auto& emitter : fenc_fx_.emitters) emitters_->kill(emitter.name);
  seq_dim_ = {};
  seq_post_ = {};
  return handoff_.battle_req;
}

void FieldWorld::end_battle(int result_code, int fade_delay) {
  vm_.set_var(script::var::kBattleResult, result_code);
  handoff_.last_battle_code = result_code;
  // A wipe in a fight that is not losable (BATTLE flag 2 clear) is the host's: it loads map 9 (the
  // game-over room) instead of returning here (App::finish_field_battle); nothing of the field resumes.
  if (result_code == 1 && !(handoff_.battle_req.flags & 2)) log::debug("field: battle lost (not losable): the host loads map 9");
  handoff_.battle_state = BattleState::None;
  // The disguise bytes again, then the party entities show them.
  if (handoff_.battle_req.flags & 4) {
    apply_battle_disguise();
    rebind_party();
  }
  // Field re-entry: a subtractive fade from black over 0x30 ticks.
  begin_entry_fade(0);
  fade_hold_ = fade_delay;
  // The saved field song is restored after EVERY battle, flag 1 included
  // (the victory song 0x16 / 0x17 and the battle theme are replaced); only BATTLE flag 0x20 skips it (the script plays the next
  // song itself). A song that is still playing is left alone by start_song.
  if (last_song_ >= 0 && !(handoff_.battle_req.flags & 0x20))
    start_song(last_song_, bgm_volume_, last_song_ == handoff_.resume_song ? handoff_.resume_seconds : 0.0);
  handoff_.resume_song = -1;
  handoff_.battle_music_stopped = false;
  log::info("field: battle ended, result {}", result_code);
}


void FieldWorld::request_map_jump(int room, int param) {
  if (jump_.active) return;
  jump_ = {true, room, param};
  log::info("field: map jump requested -> room id {} entrance {}", room, param);
}


void FieldWorld::snap_camera_to_player() {
  camera_.snap(view_, vm_.global_flags(), pose_of(player_entity()));
  view_.set_scroll(camera_.cam.x, camera_.cam.y, true);
}

void FieldWorld::step_camera() {
  camera_.advance(view_, vm_.global_flags(), pose_of(player_entity()), pose_of(camera_.cam.follow_ent));
  // Screen shake: advance the amplitude ramp, then offset the scroll (re-clamped to the pan limits unless limits are off).
  if (shake_.frames > 0) {
    shake_.ax += shake_.dx;
    shake_.ay += shake_.dy;
    --shake_.frames;
  } else {
    shake_.ax = shake_.tx * 65536;
    shake_.ay = shake_.ty * 65536;
  }
  // One camera: the retail integer scroll above is the only eased value. The display (RoomView::scroll_at) interpolates it between
  // ticks, sub-pixel in HD art. A second float-eased copy would drift from it (the integer ease stalls).
  int sx = camera_.cam.x, sy = camera_.cam.y;
  if (shake_.ax != 0) sx = camera_.clamp_x(view_, vm_.global_flags(), camera_.cam.x + shake_offset(shake_.ax));
  if (shake_.ay != 0) sy = camera_.clamp_y(view_, vm_.global_flags(), camera_.cam.y + shake_offset(shake_.ay));
  // Sequence shake (op 2E): the display itself moves, so the offset is not held to the pan limits.
  if (sequences_) {
    int amp_x = 0, amp_y = 0;
    sequences_->shake_amplitude(amp_x, amp_y);
    sx -= display_shake_offset(amp_x);
    sy -= display_shake_offset(amp_y);
  }
  // Damage-floor jolt: 8 frames, n = 7 .. 0 (decremented before drawing). The host draws it as a 50 % ghost of the previous
  // picture (jolt_view); the new picture is not moved.
  if (fstat_.jolt > 0) {
    --fstat_.jolt;
    jolt_n_ = fstat_.jolt;
  } else {
    jolt_n_ = -1;
  }
  view_.set_scroll(sx, sy, false);
}

void FieldWorld::step_fade() {
  if (fog_.left > 0) {
    for (size_t i = 0; i < 3; ++i) fog_.cur[i] += fog_.step[i];
    if (--fog_.left == 0) fog_.cur = fog_.target;
  }
  if (fade_hold_ > 0)
    --fade_hold_;
  else if (fade_ < fade_target_)
    fade_ = std::min(fade_target_, fade_ + fade_step_);
  else if (fade_ > fade_target_)
    fade_ = std::max(fade_target_, fade_ - fade_step_);
  // The fade kind resets after the fade-in.
  if (fade_ <= 0.0f && trans_ == Trans::None) fade_kind_ = 0;
}


void FieldWorld::begin_jump(int, int) {
  trans_ = Trans::Out;
  // The kind is set by FE 02 (forced 0 while the 8F wave runs), then reset to 0; the next room's fade-in
  // uses the same kind. 0 = subtractive fade to black, 2 = additive fade to white.
  // 1 / 3 / 4 / 5 / 6: no fade out; the shown frame is captured and drawn as 5 screen-space tiles
  // over everything, frozen for 4 frames, then the room switches (step_capture continues on the new room).
  fade_kind_ = wave_.active ? 0 : exit_kind_;
  exit_kind_ = 0;
  // Debug: OC_EXIT_KIND=n forces the kind of every room change (routes check kinds the data rarely or never uses: 3..6).
  if (const char* env = std::getenv("OC_EXIT_KIND"); env && *env) fade_kind_ = std::clamp(std::atoi(env), 0, 6);
  if (capture_kind(fade_kind_)) {
    capture_.want = true;
    capture_.ready = capture_.active = capture_.dirty = false;
    capture_.kind = fade_kind_;
    capture_.hold = 0;
    return;
  }
  // The colour ramps 0 -> 0xFF in n steps (0x50 black, 0x40 white); the room switches on the tick the screen is fully black / white.
  // A screen an A8 already took (or is taking) to black keeps that fade.
  if (fade_black_) return;
  fade_black_ = true;
  fade_target_ = 1.0f;
  fade_step_ = 1.0f / static_cast<float>(fade_kind_ == 2 ? kWhiteFadeOut : kRoomFadeOut);
  fade_add_ = fade_kind_ == 2;
}

void FieldWorld::step_transition() {
  const bool captured = capture_kind(fade_kind_);
  if (captured) {
    // 2 frames of the frozen picture, the STP pass, 2 more = 4 frames (8 ticks) before the room load. Without a frame from
    // the host (headless runs) the switch goes ahead after 16 ticks and nothing is drawn.
    ++capture_.hold;
    if (capture_.hold < 8 || (!capture_.ready && capture_.hold < 16)) return;
  } else if (fade_ < 1.0f - 1e-4f) {
    return;
  }
  // Fully black: load the target.
  const PendingJump pending = jump_;
  jump_ = {};
  trans_ = Trans::None;
  view_.set_interp_frozen(false);
  // DiscCheck: the story variable 0x42 names the disc the next room is on.
  if (const int need = vm_.var(0x42) + 1; need != disc_ && (need == 1 || need == 2)) set_disc(need);
  const std::string target = room_stem_for_id(pending.room);
  const auto& rooms = view_.rooms();
  if (target.empty() || std::find(rooms.begin(), rooms.end(), target) == rooms.end()) {
    finish_jump_failed(std::format("room id {} ({}) is not a field room", pending.room, target.empty() ? "unknown" : target));
    return;
  }
  const std::string from = view_.room_name();
  const int entry_kind = fade_kind_;
  if (!load_room(target, pending.param)) {
    finish_jump_failed(std::format("loading {} failed", target));
    return;
  }
  // Kind 0 fades in over 0x30 ticks (subtractive, from black), kind 2 over 0x20 (additive, from white).
  begin_entry_fade(entry_kind);
  if (captured) {
    // The new room fades in from black over n ticks (kind 1 0x30, 3 0x20, 4 0x30, 5 / 6 0 -> 1), while
    // step_capture adds the old frame over it at brightness b / 128 (0x80 falling by 0x800000 / D per
    // tick: D = 0x60 kind 1, 0x20 kinds 3 / 4) or slides it away (kinds 5 / 6). Kinds 1 / 3 also wait for the load
    // with b held at 0x80; loads are instant here.
    static constexpr int kFadeIn[7] = {0, 0x30, 0, 0x20, 0x30, 1, 1};
    static constexpr int kDissolve[7] = {0, 0x60, 0, 0x20, 0x20, 0, 0};
    const int kind = capture_.kind;
    fade_step_ = 1.0f / static_cast<float>(kFadeIn[kind]);
    capture_.want = false;
    capture_.active = capture_.ready;
    capture_.acc = 0x800000;
    capture_.step = kDissolve[kind] ? 0x800000 / kDissolve[kind] : 0;
    capture_.slide = 0;
  }
  log::info("field: {} -> {} (entrance {})", from, target, pending.param);
}


void FieldWorld::finish_jump_failed(const std::string& why) {
  status_ = "exit unavailable: " + why;
  log::warn("field: {}", status_);
  capture_.want = capture_.ready = capture_.active = false;
  fade_black_ = false;
  fade_target_ = 0.0f;
  fade_step_ = 1.0f / static_cast<float>(kFadeTicks);
  // Unpark the triggered scripts (the exit's ON_COLLISION ends in STOP_EXEC) and give the controls back.
  for (size_t i = 0; i < ents_.size(); ++i) {
    script::Entity& script_ent = vm_.entity(static_cast<int>(i));
    for (auto& record : script_ent.rec)
      if (record.state >= 2 && record.state <= 4) {
        record.state = script::kRecIdle;
        record.id = 0xFF;
        record.phase = 0;
      }
    script_ent.flags270 &= ~(script::flag270::kInputLock | script::flag270::kActionRunning);
  }
  update_input_lock();
}

}  // namespace oc::game
