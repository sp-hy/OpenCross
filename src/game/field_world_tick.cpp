// FieldWorld: script registers and predicates, key latches, visuals and the watchdog, the tick orchestration, the debug dumps and
// the field status effects.
#include "game/field_world.h"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

#include "audio/voice.h"
#include "core/log.h"
#include "game/cheats.h"
#include "game/field_world_internal.h"
#include "game/new_game_plus.h"

namespace oc::game {

using namespace field_internal;

void FieldWorld::table_op(u16 key, int arg, int index) {
  // FE 5D / FE 60: arg == 0 sets, else clears bit `index` (< 0x2C) of the party lock bitmap or the party-screen exclusion bitmap
  // (both saved in the slot).
  if ((key == 0xFE5D || key == 0xFE60) && index >= 0 && index < 0x2C) {
    u32& word = gs_->slot.unk_e1b8[static_cast<size_t>((key == 0xFE5D ? 0 : 2) + (index >> 5))];
    const u32 bit = 1u << (index & 31);
    word = arg == 0 ? (word | bit) : (word & ~bit);
    return;
  }
  // C7: the sounds C6 started on slot (U & 7) end.
  if (key == 0xC7) {
    if (sfx_) sfx_->evict_field(1u << ((arg & 7) + 8));
    return;
  }
  if (key == 0xFE69 && gs_) {
    // FE 69, character record 1 (Kid): b == 0 SAVE empties her element grid cell by cell - every cell whose id (& 0x1FF) is a real
    // element (< 0x80) keeps only its flag bits (& 0xC000) - and remembers (id, copies) WITHOUT refunding the stock; b != 0 RESTORE adds each
    // remembered element to the element STOCK (not back into the grid; she must place them again) and forgets them.
    // The pending list is kept in memory only: a save made inside the scene loses it.
    save::SaveChar& ch = gs_->slot.chars[1];
    if (arg == 0) {
      fe69_saved_.clear();
      for (size_t k = 0; k < ch.grid.size(); ++k) {
        const u16 cell = ch.grid[k];
        if ((cell & 0x1FF) == 0 || (cell & 0x1FF) >= 0x80) continue;
        fe69_saved_.emplace_back(cell & 0x1FF, std::max<int>(1, ch.equipped_qty[k]));
        ch.grid[k] = static_cast<u16>(cell & 0xC000);
        ch.equipped_qty[k] = 0;
      }
    } else {
      for (const auto& [id, copies] : fe69_saved_)
        for (int i = 0; i < copies; ++i) gs_->add_element(id);
      fe69_saved_.clear();
    }
    return;
  }
  if (key == 0xFE6F && gs_) {
    // FE 6F on the recruited roster (44 bits): b == 0 SAVE refunds every recruited character's element grid
    // and accessories to the stock, saves the roster, leaves only Serge recruited and
    // sets the party lock bitmap to 1 (the rest 0); b != 0 RESTORE ORs the saved roster back. Grids and accessories stay
    // empty.
    save::SaveSlot& sl = gs_->slot;
    if (arg == 0) {
      u32 lo = 0, hi = 0;
      for (int char_id = 0; char_id < 0x2C; ++char_id) {
        if (!sl.recruited(char_id)) continue;
        (char_id < 32 ? lo : hi) |= 1u << (char_id & 31);
        gs_->clear_all(char_id);
        gs_->unequip_accessories(char_id);
      }
      sl.unk_e1e0 = lo;
      sl.unk_e1e4 = hi;
      for (int char_id = 1; char_id < 0x2C; ++char_id) sl.set_recruited(char_id, false);
      sl.set_recruited(0, true);
      sl.unk_e1b8 = {1u, 0u, 0u, 0u};
    } else {
      for (int char_id = 0; char_id < 0x2C; ++char_id)
        if ((char_id < 32 ? sl.unk_e1e0 : sl.unk_e1e4) >> (char_id & 31) & 1u) sl.set_recruited(char_id, true);
    }
    return;
  }
  // FE 7C: character `arg` gets weapon `index`; the weapon it held goes to the item stock.
  if (key == 0xFE7C && gs_ && arg >= 0 && arg < save::kNumChars) {
    save::SaveChar& ch = gs_->slot.chars[static_cast<size_t>(arg)];
    if (ch.weapon) gs_->add_item(ch.weapon);
    ch.weapon = static_cast<u16>(index);
    return;
  }
  // FE 7D: character `arg` leaves its grid elements and its three accessories in the stock (FE 6F's per-character step).
  if (key == 0xFE7D && gs_ && arg >= 0 && arg < save::kNumChars) {
    gs_->clear_all(arg);
    gs_->unequip_accessories(arg);
    return;
  }
  // FE 1B: window skin = U (variable 0x44 too, set by the VM).
  if (key == 0xFE1B) {
    gs_->skin = static_cast<u8>(std::clamp(arg, 0, 14));
    return;
  }
  stub(std::format("table_op({:X})", key).c_str());
}

// ------------------------------------------------------------------------------------------------ host: input / catch-alls
// Held keys filtered by the script's pad mask (FE 77).
u16 FieldWorld::keys_held() { return input_.keys_held(); }
u16 FieldWorld::key_latch() { return input_.latched_keys(); }
void FieldWorld::clear_key_latch() { input_.clear_key_latch(); }

// Jump predicates (true = jump for 95 / E5 / E6 / E8). The character bitmaps are 44-bit words: the recruited roster
// (SaveSlot::recruited) and two saved bitmaps (unk_e1e0/e4 and unk_e1ec/f0; a New Game+ start ORs the
// three together). Ids >= 0x2C always jump.
bool FieldWorld::predicate(script::Predicate op, int arg) {
  using P = script::Predicate;
  const auto bit = [&](u32 lo, u32 hi) { return arg >= 0 && arg < 0x2C && (((arg < 32 ? lo : hi) >> (arg & 31)) & 1u) != 0; };
  const auto recruited = [&] { return arg >= 0 && arg < 0x2C && arg < save::kNumChars && gs_->slot.recruited(arg); };
  switch (op) {
    // 95: jump unless recruited. E5: jump unless the bit is set in the saved roster. E6: also unless recruited.
    // E8: test-and-clear in the second saved bitmap.
    case P::Op95: return !recruited();
    case P::OpE5: return !bit(gs_->slot.unk_e1e0, gs_->slot.unk_e1e4);
    case P::OpE6: return !bit(gs_->slot.unk_e1e0, gs_->slot.unk_e1e4) && !recruited();
    case P::OpE8: {
      const bool set = bit(gs_->slot.unk_e1ec, gs_->slot.unk_e1f0) && !recruited();
      if (arg >= 0 && arg < 0x2C) (arg < 32 ? gs_->slot.unk_e1ec : gs_->slot.unk_e1f0) &= ~(1u << (arg & 31));
      return !set;
    }
    // D2: result code of the last battle (0 won, 1 wiped).
    case P::GameStateLt2: return handoff_.last_battle_code < 2;
    case P::BoxOutside:
      // D0: analog pad with the LEFT stick (unsigned bytes) outside the
      // 0x4C..0xB4 box -> fall through; centred stick or a digital pad -> jump.
      return input_.pad_analog;
  }
  stub(std::format("predicate({})", static_cast<int>(op)).c_str());
  return false;
}

s32 FieldWorld::query(script::Query which) {
  // FE 82: total stars.
  if (which == script::Query::StarsTotal) return gs_->slot.stars_total();
  // FE 6E / FE 76 save the playing song (id, -1 = none) and its volume so a jingle can be played over it and the music
  // restored by FE 78.
  if (which == script::Query::CurrentSong) return last_song_ >= 0 ? last_song_ : -1;
  if (which == script::Query::Field56) return last_song_ >= 0 ? bgm_volume_ : 0;
  stub(std::format("query({})", static_cast<int>(which)).c_str());
  return 0;
}

void FieldWorld::set_global(script::Global which, s32 value) {
  using script::Global;
  switch (which) {
    case Global::WindowFrames:
      if (gs_) gs_->slot.frames = static_cast<u16>(value);
      break;
    case Global::ControlYaw: control_yaw_ = value & 0xFFF; break;
    case Global::CameraYOffset: camera_.camera_y_offset = static_cast<float>(value); break;
    case Global::SubmergedOtPush: water_bias_ = value & 0x7FFF; break;
    case Global::PushStrength: push_strength_ = static_cast<s16>(value); break;
    case Global::PadMask: input_.input_mask = static_cast<u16>(value); break;
    case Global::ExitTransition: exit_kind_ = static_cast<s16>(value); break;
    case Global::KeepSfxSlots: sfx_keep_slots_ = static_cast<u32>(value); break;
    case Global::BattleIntroAnchor: handoff_.battle_anchor = value; break;
    // FE 75: the round, then the flag that opens the monster picker (the FE 61 after it waits for it).
    case Global::Selection: picker_round_ = value; break;
    case Global::SelectionFlag: request_menu(script::MenuKind::MonsterPicker, picker_round_); break;
    // Footstep listener, distance band, floor types and volume, read by play_footstep.
    case Global::FootstepVolume: footstep_.volume = value; break;
    case Global::FootstepDryType: footstep_.dry_type = value; break;
    case Global::FootstepWaterType: footstep_.water_type = value; break;
    case Global::FootstepListenerX: footstep_.listener_x = value; break;
    case Global::FootstepListenerY: footstep_.listener_y = value; break;
    case Global::FootstepListenerZ: footstep_.listener_z = value; break;
    case Global::FootstepNear: footstep_.nearest = value; break;
    case Global::FootstepFar: footstep_.farthest = value; break;
    // FE 63 arms a load-then-fade music swap: a load order, nothing to do for the streamed BGM. FE 05 is the minimum VSyncs per
    // drawn frame: display rate only; logic stays 60 Hz with interpolation. FE 15 is the draw-order slot of the FE 12-14 overlay
    // grid; every use in the data writes the room default 0.
    case Global::MusicSwapArmed:
    case Global::FrameVsyncs:
    case Global::OverlayOt:
      break;
    default:
      stub(std::format("set_global({})", script::global_name(which)).c_str());
      break;
  }
}

void FieldWorld::opaque_op(u16 key, std::span<const u8>) {
  // FE 6D: current stars = total stars.
  if (key == 0xFE6D) {
    gs_->slot.head[save::head::kStarsCurrent] = gs_->slot.head[save::head::kStarsTotal];
    return;
  }
  // FE 66: the follower rings restart where the party stands now: ring 0 filled with the controlled entity, ring 1 with the slot 1
  // member (the scripts run it after placing the party, so the followers do not walk back along the old trail).
  if (key == 0xFE66) {
    if (party_ent_[0] >= 0) {
      const int lead = player_entity();
      if (lead >= 0) trail_fill(0, trail_entry_of(ents_[static_cast<size_t>(lead)].actor));
    }
    if (party_ent_[1] >= 0) trail_fill(1, trail_entry_of(ents_[static_cast<size_t>(party_ent_[1])].actor));
    return;
  }
  // FE 7E: Mojo's record name, when it is still the default "Lucky Dan" (game-font kana), becomes "Happy Dan" (two glyphs change).
  if (key == 0xFE7E && gs_) {
    constexpr size_t kMojo = 27;
    constexpr std::array<u8, 12> kLucky = {0xF8, 0x40, 0xF8, 0x66, 0xF8, 0x20, 0xCA, 0xF8, 0x52, 0xF8, 0x47, 0x00};
    std::array<u8, 16>& name = gs_->slot.chars[kMojo].name;
    if (std::equal(kLucky.begin(), kLucky.end(), name.begin())) {
      name[1] = 0x33;
      name[5] = 0x5D;
    }
    return;
  }
  // FE 1A: the three defeated-monster records are forgotten (the monsters respawn).
  if (key == 0xFE1A) {
    defeated_ = {};
    return;
  }
  if (key == 0xFE3F && gs_) {
    // FE 3F: the dream party is dropped. A
    // New Game+ (variable 0x7E != 0) keeps the carried state and gets the Relief Charm + Time Shifter; a normal game first gets the real
    // records (char.bin instead of the dream's char20.bin, 0 stars, growth, masks, nibbles 5, gold 100) and
    // frames 0x203.
    const bool plus = vm_.var(ngplus::kFlagVar) != 0;
    ngplus::fe3f(*gs_, plus, static_cast<u32>(tick_count_) * 2654435761u | 1u);
    if (!plus) {
      const auto globals = ngplus::new_game_globals();
      for (size_t i = 0; i < globals.size(); ++i) vm_.set_var(static_cast<u16>(0x3D0 + 2 * i), globals[i]);
    }
    defeated_ = {};
    // The field's own party table (party_chars_) is not touched: the script's 7D / 7B that follow edit it.
    log::info("field: tick {} FE 3F start-of-game reset ({})", tick_count_, plus ? "New Game+: carried state kept" : "fresh data");
    return;
  }
  stub(std::format("opaque_op({:X})", key).c_str());
}

// ------------------------------------------------------------------------------------------------ simulation

void FieldWorld::step_visuals() {
  const int player = player_entity();
  const WalkMesh& walk = view_.walk();
  for (int i = 0; i < static_cast<int>(ents_.size()); ++i) {
    Ent& ent_state = ents_[static_cast<size_t>(i)];
    const script::Entity& script_ent = vm_.entity(i);
    // Per-entity numbers scripts set: scale (op 82), trigger radius (8B) and the script move speed.
    // Op 82 is per axis (model space). The VM entity starts at the engine default 0x200 per
    // axis, so a 0 is always an explicit 0 (an entity can rise from y = 0 with AXIS_SCALE).
    ent_state.actor.scale = 1.0f;
    for (int k = 0; k < 3; ++k) ent_state.actor.axis_scale[k] = static_cast<float>(static_cast<s16>(script_ent.scale[k])) / 4096.0f;
    ent_state.actor.radius = ent_radius(i);
    ent_state.actor.height = ent_height(i);
    // Op AD: the flag selects the single RotMatrix({pitch, yaw, roll}) path. Op B0 / B1: a model y offset. Ops 27 / 28 / 29 / AA: pitch / model yaw / roll.
    ent_state.actor.tilt_world = (script_ent.flags270 & (1u << 5)) != 0;
    ent_state.actor.y_offset = static_cast<float>(static_cast<s16>(script_ent.v110));
    ent_state.actor.pitch = static_cast<float>(script_ent.axis[0]);
    ent_state.actor.local_yaw = static_cast<float>(script_ent.axis[1]);
    ent_state.actor.roll = static_cast<float>(script_ent.axis[2]);
    ent_state.actor.anim_rate = script_ent.v10e ? static_cast<float>(static_cast<s16>(script_ent.v10e)) / 4096.0f : 1.0f;
    {
      const bool idle_ok = ent_state.pc_slot >= 0 && !ent_state.motion.active && (i == player ? controls_free() : ent_state.updating);
      if (idle_ok && !ent_state.actor.auto_idle) ent_state.actor.seed_rng(static_cast<u32>(tick_count_) + static_cast<u32>(i) * 977u);
      ent_state.actor.auto_idle = idle_ok;
    }
    // Actor::tick already animated the player.
    if (i == player && !ent_state.motion.active && ent_state.placed) continue;
    // Stair / ladder tiles turn the actor and pick the clip.
    if (ent_state.placed && !walk.empty()) ent_state.actor.set_tile(walk, walk_flags(i));
    {
      // The entity flag 0x100 (FE 00 0) or the global flag 0x80 (FE 00 1) orients the model to the ground plane; flag 0x20000000 (set by
      // FE 00 0 and op 34 0) skips the smoothing for the frame and is cleared after it.
      script::Entity& sv = vm_.entity(i);
      const bool on = (sv.flags270 & 0x100u) != 0 || (vm_.global_flags() & 0x80u) != 0;
      ent_state.actor.update_ground_tilt(walk, on && ent_state.placed, (sv.flags270 & 0x20000000u) != 0);
      if (on) sv.flags270 &= ~0x20000000u;
    }
    // Followers animate in step_followers' wake.
    if (ent_state.pc_slot >= 1 && !ent_state.motion.active) {
      ent_state.actor.step_visual(ent_state.motion.walking, ent_state.motion.running);
      continue;
    }
    // Scripted walks play the walk slot while they move and the stand slot once they arrive (the move speed does not pick the run slot). Short hops
    // keep the walk clip because the switch back to the stand clip waits for the running cross-fade (Actor::step_visual).
    ent_state.actor.step_visual(ent_state.motion.active && ent_state.motion.walking && !ent_state.motion.paused, false);
  }
  // After every pose of the tick (attachments read the parents' joint matrices).
  step_attached();
  for (int i = 0; i < static_cast<int>(ents_.size()); ++i) {
    Ent& ent_state = ents_[static_cast<size_t>(i)];
    // Followers at rest, and every other model entity (NPCs): a tick standing off the walkable mesh (route `assert offmesh`).
    // Free-height entities (flag 0x40: flyers, lifts) and hidden ones do not count.
    {
      // The low walk-flag bits pick the zone light; FE 5A: the target is 0x80 on every channel.
      const u32 lf = walk_flags(i);
      const bool flat = (vm_.entity(i).flags270 & 0x80000000u) != 0;
      if (!ent_state.light_init) {
        lighting_.snap(ent_state.light, lf, flat);
        ent_state.light_init = true;
      } else {
        lighting_.tick(ent_state.light, lf, flat);
      }
    }
    const script::Entity& ve = vm_.entity(i);
    const bool npc_counted = ent_state.pc_slot < 0 && ent_state.actor.loaded() && ent_state.placed && !(ve.flags38 & 0x40) && !ent_hidden(i);
    if (((ent_state.pc_slot >= 1 && !ent_state.motion.active) || npc_counted) && !walk.empty()) {
      const ActorPose& pose = ent_state.actor.current();
      int tri = ent_state.actor.triangle();
      // A follower carries the ring entry's tile: its point may lie in a neighbouring triangle.
      if (tri >= 0 && !walk.contains(tri, pose.x, pose.z)) tri = walk.find_triangle(pose.x, pose.z, pose.y);
      // NPCs: only a tick on which the entity MOVED off the mesh counts (props / NPCs placed off the mesh by the script stand still).
      const ActorPose p0 = ent_state.actor.pose(0.0f);
      const bool moved = ent_state.pc_slot >= 1 || std::hypot(pose.x - p0.x, pose.z - p0.z) > 0.01f;
      if (moved && (tri < 0 || walk.blocked(tri, 0))) {
        ++ent_state.off_mesh_ticks;
        log::debug("field: tick {} ent {} off the walk mesh at {:.0f},{:.0f} (tri {}, motion {}, count {})", tick_count_, i, static_cast<double>(pose.x),
                   static_cast<double>(pose.z), tri, ent_state.motion.active ? 1 : 0, ent_state.off_mesh_ticks);
      }
    }
    if (ent_state.actor.clip_name() != ent_state.last_clip) {
      ent_state.last_clip = ent_state.actor.clip_name();
      log::debug("field: tick {} ent {} clip {} (moving {}, requested {})", tick_count_, i, ent_state.last_clip, ent_state.actor.moving(), ent_state.actor.requested_anim());
    }
  }
}

void FieldWorld::step_watchdog() {
  const bool user_busy = menu_state_ != MenuState::None || handoff_.battle_state != BattleState::None || movie_state_ != MovieState::None ||
                         modal_window_open() || trans_ != Trans::None;
  const bool locked = (vm_.global_flags() & script::gflag::kInputLock) != 0;
  watchdog_.step(user_busy, locked, tick_count_, view_.room_name(), vm_, [this](const script::FieldVM::StuckInfo& stuck) { return describe_stuck(stuck); });
}

std::string FieldWorld::describe_stuck(const script::FieldVM::StuckInfo& stuck) {
  std::string detail;
  if (stuck.strict && (stuck.key == 0xE2 || stuck.key == 0xE0 || stuck.key == 0xE3 || stuck.key == 0xE4))
    detail = std::format("; windows open {}, own window {}", any_window_open(), window_owned_by(stuck.ent) != nullptr);
  // WAIT_ROTATION: where the yaw is and why it does not arrive.
  else if (stuck.key == 0xCB || stuck.key == 0xCC) {
    const int re = stuck.key == 0xCB ? stuck.ent : -1;
    const int we = re >= 0 ? re : stuck.ent;
    const Ent& ent_state = ents_[static_cast<size_t>(we)];
    detail = std::format("; facing {:#x} shown {:#x} turn rate {} (+0x24E) flags270 {:#x} turning {}", ent_state.actor.facing(), ent_state.actor.shown_facing(),
                         vm_.entity(we).v24e, vm_.entity(we).flags270, ent_state.actor.turning());
  // The party's formation walks.
  } else if (stuck.strict && (stuck.key == 0x85 || stuck.key == 0xBE)) {
    for (int k = 0; k < 3; ++k) {
      const int pe = party_ent_[static_cast<size_t>(k)];
      if (pe < 0 || !ents_[static_cast<size_t>(pe)].motion.active) continue;
      const Ent& we = ents_[static_cast<size_t>(pe)];
      detail += std::format("; slot {} (ent {}) walks, {:.0f} units to ({:.0f},{:.0f})", k, pe,
                            static_cast<double>(std::hypot(we.motion.tx - we.actor.current().x, we.motion.tz - we.actor.current().z)),
                            static_cast<double>(we.motion.tx), static_cast<double>(we.motion.tz));
    }
  } else if (stuck.strict && (vm_.entity(stuck.ent).flags270 & script::flag270::kMoving)) {
    const Ent& we = ents_[static_cast<size_t>(stuck.ent)];
    detail = std::format("; motion active {}, {:.0f} units from its target ({:.0f},{:.0f}), paused {}", we.motion.active,
                         static_cast<double>(std::hypot(we.motion.tx - we.actor.current().x, we.motion.tz - we.actor.current().z)),
                         static_cast<double>(we.motion.tx), static_cast<double>(we.motion.tz), we.motion.paused);
  }
  return detail;
}

void FieldWorld::sweep_talk(int ticks, const std::function<void()>& settle) {
  if (!initialised_ || ents_.empty()) return;
  const std::string room(view_.room_name());
  watchdog_.sweep_probe = true;
  int probes = 0, unfinished = 0, reloads = 0;
  for (int pass = 0; pass < 2; ++pass) {
    for (int ent = 1; ent < static_cast<int>(ents_.size()); ++ent) {
      if (ents_[static_cast<size_t>(ent)].pc_slot >= 0) continue;
      if (!(pass == 0 ? vm_.trigger_action(ent) : vm_.trigger_collision(ent))) continue;
      ++probes;
      bool reload = false;
      platform::Input in;
      for (int tick = 0; tick < ticks && !reload; ++tick) {
        in.key_pressed[SDL_SCANCODE_RETURN] = tick % 10 == 5;
        update(in, 1);
        if (battle_ready()) {
          take_battle();
          end_battle(0);
        }
        if (movie_pending()) {
          take_movie();
          movie_finished();
        }
        if (menu_pending()) {
          take_menu();
          menu_closed();
        }
        // The script left the room: that probe is over.
        if (jump_.active) {
          jump_.active = false;
          reload = true;
        }
      }
      const auto& records = vm_.entity(ent).rec;
      const bool action_live =
          std::any_of(records.begin(), records.end(), [](const script::ScriptRecord& record) { return record.id == 2 && record.state != script::kRecIdle; });
      if (!reload && pass == 0 && action_live) {
        ++unfinished;
        log::info("sweep talk: room {} ent {} ON_ACTION still running after {} ticks:\n{}", room, ent, ticks, vm_.describe_records());
      }
      // A probe that left scripts running would disturb the next one: start from a fresh room.
      if (reload || vm_.triggered_busy()) {
        ++reloads;
        load_room(room, 0);
        settle();
        watchdog_.sweep_probe = true;
        if (static_cast<size_t>(ent) >= ents_.size()) break;
      }
    }
  }
  watchdog_.sweep_probe = false;
  log::info("sweep talk: room {}: {} probes, {} unfinished, {} reloads", room, probes, unfinished, reloads);
}

void FieldWorld::sim_tick() {
  ++tick_count_;
  // Cheats: max money (the in-game cap).
  if (platform::cheats().max_money && gs_) gs_->set_gold(9999999);
  view_.set_interp_frozen(exit_frozen());
  if (debug_.auto_walk && tick_count_ == static_cast<u64>(debug_.action_tick)) input_.action_pending = true;
  if (debug_.show_message >= 0 && tick_count_ == 30) {
    open_dialogue(-1, debug_.show_message, 0);
    if (debug_.show_face >= 0) {
      dialogue_face_ = debug_.show_face;
      dialogue_.set_portrait(dialogue_face_);
    }
    if (debug_.show_choice >= 0) dialogue_choice(debug_.show_choice_first, debug_.show_choice);
  }

  // The field keeps running while the battle overlay loads (the picture still changes with enc2's post ramp).
  if (handoff_.battle_state == BattleState::Hold && ++handoff_.hold_ticks >= kEncounterHoldTicks) {
    handoff_.battle_state = BattleState::Ready;
    return;
  }
  // The whole field (scripts, movers, followers, camera) keeps ticking while the room-exit fade runs; only the leader stops
  // (his heading is cleared) and the input lock is set. The kinds that show a frozen capture (1, 3..6) hold the world for their 4 frames.
  if (exit_frozen()) {
    step_fade();
    step_transition();
    return;
  }
  step_fade();
  // step_capture: FE 02 kinds 1 / 3 / 4 / 5 / 6 on the new room.
  step_capture();
  for (auto& ent_state : ents_) {
    ent_state.actor.begin_tick();
    ent_state.updating = false;
  }
  step_cull();
  vm_.tick();
  step_watchdog();
  // An entity the scripts left inactive (SET_VISIBLE 0) neither turns nor picks a clip this tick.
  for (size_t i = 0; i < ents_.size(); ++i)
    ents_[i].actor.set_inactive((vm_.entity(static_cast<int>(i)).flags38 & script::flag38::kInactive) != 0);
  step_motions();
  step_player();
  // step_status: poison / flu / damage-floor block (after the pad move of the tick).
  step_status();
  step_followers();
  step_triggers();
  step_visuals();
  // Room entry: INIT's first SET_ANIMATION (the curtains of alu_a03i, $0080 bit 1 -> clip 2) replaces the stand clip submitted at room start
  // while the picture is still fading in; the 0x20 blend must not play on screen as a visible move.
  if (entry_cut_ > 0) {
    --entry_cut_;
    for (auto& ent_state : ents_) ent_state.actor.finish_fade();
  }
  // step_push: terrain push (after the walks of the tick). step_bone_emitters: anim-sequence op 1D (the sequences ran in
  // step_visuals). step_sfx_slides: FE 22 / FE 23.
  step_push();
  step_bone_emitters();
  step_sound_emitters();
  step_wave();
  step_sfx_slides();
  step_camera();
  if (emitters_) emitters_->tick();
  step_wakes();
  step_shadows();
  if (sequences_) sequences_->tick();
  bk_step();
  // Dim tasks (sequence ops 2B / 2C).
  for (SeqDim& dim : seq_dim_) seq_dim_step(dim);
  if (handoff_.battle_state == BattleState::Intro || handoff_.battle_state == BattleState::Hold) {
    const u64 elapsed = tick_count_ - handoff_.intro_tick;
    // At the battle load every task dies and 3E goes off. The discs / sparks are tasks of the same effect system. 3E stays on in the
    // displayed frames: the one-frame gap does not change the picture.
    if (elapsed == static_cast<u64>(kEncounterTearDown)) {
      if (sequences_) {
        sequences_->stop("enct");
        sequences_->stop("enc2");
      }
      if (emitters_)
        for (const auto& em : fenc_fx_.emitters) emitters_->kill(em.name);
      seq_dim_ = {};
    // The battle intro starts its embedded sequence `main`: 3E (parameters reset).
    } else if (elapsed == static_cast<u64>(kEncounterMain3E)) {
      seq_post_ = {};
      seq_post_.on = true;
    // `main` op 40: 57 ticks to tz -2289, colour (190, 191, 179), ABR 0.
    } else if (elapsed == static_cast<u64>(kEncounterMainRamp)) {
      seq_post_.start = seq_post_.cur;
      seq_post_.rgb_start = seq_post_.rgb;
      seq_post_.target = {};
      seq_post_.target[5] = -2289;
      seq_post_.rgb_target = {190, 191, 179};
      seq_post_.ticks = 57;
      seq_post_.elapsed = 0;
      seq_post_.ramp = true;
    }
  }
  seq_post_step();
  // The battle starts once the 'enct' task is gone.
  if (handoff_.battle_state == BattleState::Intro && !(sequences_ && sequences_->running("enct"))) {
    handoff_.battle_state = BattleState::Hold;
    handoff_.hold_ticks = 0;
    // Leaving the field for the battle evicts every sound but mask bit 0: every
    // field sound (room frames, sound emitters, footsteps, the global ones of kept slots) ends at once; only mask bit 0 survives - the encounter
    // effect's own `enct` sounds (op 0x51, mask 1) play on through the load. Until here the field's sounds ran over the swirl.
    if (mixer_) mixer_->evict_sfx(0xFFFFFE, true);
    log::info("field: tick {} encounter effect 'enct' ended after {} ticks", tick_count_, tick_count_ - handoff_.intro_tick);
  }
  // Display segments of the pose (Actor::end_tick): scripts that step an actor every 2nd-4th tick.
  for (auto& ent_state : ents_) ent_state.actor.end_tick();
  if (static const bool drift_log = std::getenv("OC_ATTACH_DRIFT") != nullptr; drift_log) check_attach_drift();
  // The fade ran at the top of this tick: switch rooms once it is black / white.
  if (jump_.active && trans_ == Trans::None && !watchdog_.sweep_probe) begin_jump(jump_.room, jump_.param);
  else if (trans_ == Trans::Out) step_transition();
}


std::string FieldWorld::layer_report(std::string* notes) const {
  std::string out;
  const script::FieldScriptModule& script_module = vm_.module();
  std::set<u32> groups, shown;
  std::set<u32> seen;
  std::vector<u32> work;
  for (size_t ent = 0; ent < script_module.entity_count(); ++ent)
    for (int slot = 0; slot < script::kScriptSlots; ++slot)
      if (const u16 ptr = script_module.script_ptr(ent, static_cast<size_t>(slot))) work.push_back(ptr);
  while (!work.empty()) {
    u32 off = work.back();
    work.pop_back();
    while (off < script_module.code.size() && seen.insert(off).second) {
      const auto insn = script::decode_insn(script_module.code, off);
      if (!insn) break;
      if (insn->key == 0x80 || insn->key == 0x81 || insn->key == 0xC2)
        if (!insn->ops.empty() && insn->ops[0].kind == script::OperandKind::Imm) {
          groups.insert(static_cast<u32>(insn->ops[0].value));
          if (insn->key == 0x80) shown.insert(static_cast<u32>(insn->ops[0].value));
        }
      for (const auto& target : insn->targets) work.push_back(target.first);
      if (insn->flow != script::Flow::Next) break;
      off += insn->length;
    }
  }
  // A group number is a bit of the 32-bit mask; naming a group without tiles only toggles an unused bit (a note).
  std::string note;
  for (u32 group : groups) {
    if (group >= 32) out += std::format("layer op names group {} (the mask has 32 bits); ", group);
    else if (!view_.has_group(group)) note += std::format("group {} has no tiles; ", group);
  }
  if (view_.group_count() > 0 && view_.visible_group_images() == 0) {
    // Blank rooms are legitimate (cutscene black rooms turn their only group off); it is a problem when a script shows a group that does not exist.
    const bool wants = std::any_of(shown.begin(), shown.end(), [&](u32 group) { return !view_.has_group(group); });
    if (wants) out += "no background group is visible after INIT and a layer op shows a group the room lacks; ";
    else note += "blank room (every group hidden after INIT); ";
  }
  if (notes) *notes = note;
  return out;
}


void FieldWorld::update(const platform::Input& in, int ticks) {
  if (!initialised_) return;
  // ---- input snapshot
  auto [confirm, up, down, left, right, repeat, select] = input_.sample(in, ticks);
  // Select rotates the party order, once per frame before the ticks; not in the first field frames after a room load.
  if (input_.select_wait > 0) input_.select_wait = std::max(0, input_.select_wait - ticks);
  else if (select) rotate_party();
  // Number entry: up / down step the cursor digit, left / right move the cursor; a held direction auto-repeats.
  const auto stepping = [&](bool pressed, u32 bit) { return pressed || (repeat & bit) != 0 ? 1 : 0; };
  const int digit_step = stepping(up, 1) - stepping(down, 4), digit_shift = stepping(right, 2) - stepping(left, 8);

  if (debug_.auto_walk && debug_.auto_confirm > 0 && any_window_open() && tick_count_ % static_cast<u64>(debug_.auto_confirm) < static_cast<u64>(std::max(ticks, 1))) confirm = true;
  if (modal_window_open()) {
    // The modal windows (all of them: a choice and the page-turn of a plain message share the confirm key).
    if (dialogue_.is_open() && !label_) {
      if (up) dialogue_.move_choice(-1);
      if (down) dialogue_.move_choice(1);
      if (dialogue_.digit_entry() && (digit_step != 0 || digit_shift != 0)) dialogue_.move_digit(digit_step, digit_shift);
      if (confirm) dialogue_.confirm();
    }
    for (auto& window : extra_win_) {
      if (!window.box || !window.box->is_open() || window.label) continue;
      if (up) window.box->move_choice(-1);
      if (down) window.box->move_choice(1);
      if (window.box->digit_entry() && (digit_step != 0 || digit_shift != 0)) window.box->move_digit(digit_step, digit_shift);
      if (confirm) window.box->confirm();
    }
    input_.action_pending = false;
  } else if (confirm) {
    input_.action_pending = true;
  }
  // A frameless window (flag 0x10, `label_` here) is an ordinary window to the engine: a `{mend}` message waits for the button. While a
  // script has the controls locked the button must reach it, or the
  // scene waits for ever; with the controls free the world-map style labels close by themselves ({wend}) and the key stays an action.
  if (confirm && (vm_.global_flags() & script::gflag::kInputLock)) {
    if (dialogue_.is_open() && label_) dialogue_.confirm();
    for (auto& window : extra_win_)
      if (window.box && window.box->is_open() && window.label) window.box->confirm();
  }
  dialogue_.update(static_cast<float>(ticks) / 60.0f);
  for (auto& window : extra_win_)
    if (window.box) window.box->update(static_cast<float>(ticks) / 60.0f);

  for (int i = 0; i < ticks; ++i) {
    sim_tick();
    // A key edge lasts one tick.
    input_.action_pending = false;
  }
}

// ------------------------------------------------------------------------------------------------ game state / save

std::string FieldWorld::dump_entities() const {
  std::string out = std::format("  controls_free {}: trans {} fade {:.2f} jump {} dialogue {} (label {}) gflags {:X} tint {}\n", controls_free(),
                                static_cast<int>(trans_), static_cast<double>(fade_), jump_.active, dialogue_.is_open(), label_,
                                vm_.global_flags(), tint_active());
  if (const assets::RoomScene* sc = view_.scene())
    for (size_t zi = 0; zi < sc->zones.size(); ++zi) {
      const auto& zone = sc->zones[zi];
      out += std::format("  zone {}: ({},{}) ({},{}) ({},{}) ({},{}) y {}\n", zi, zone.corner[0][0], zone.corner[0][2], zone.corner[1][0], zone.corner[1][2], zone.corner[2][0],
                         zone.corner[2][2], zone.corner[3][0], zone.corner[3][2], zone.corner[0][1]);
    }
  out += std::format("  camera: tween {} frames {} at {},{} -> {},{} mode {} limits {}\n", camera_.cam.tween, camera_.cam.frames, camera_.cam.x, camera_.cam.y, camera_.cam.to_x, camera_.cam.to_y,
                     static_cast<int>(camera_.cam_mode), camera_.cam.has_limits);
  {
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (const auto& vertex : view_.walk().mesh().vertices) {
      lo[0] = std::min(lo[0], vertex[0]);
      hi[0] = std::max(hi[0], vertex[0]);
      lo[1] = std::min(lo[1], vertex[2]);
      hi[1] = std::max(hi[1], vertex[2]);
    }
    const int pe = player_entity();
    out += std::format("  walk mesh {} tris, x {:.0f}..{:.0f} z {:.0f}..{:.0f}; player ent {} tri {}\n", view_.walk().triangle_count(),
                       static_cast<double>(lo[0]), static_cast<double>(hi[0]), static_cast<double>(lo[1]), static_cast<double>(hi[1]), pe,
                       pe >= 0 ? ents_[static_cast<size_t>(pe)].actor.triangle() : -2);
  }
  out += view_.describe_items();
  for (size_t i = 0; i < ents_.size(); ++i) {
    const Ent& ent_state = ents_[i];
    const script::Entity& script_ent = vm_.entity(static_cast<int>(i));
    const ActorPose& pose = ent_state.actor.current();
    std::string sc;
    for (const auto& record : script_ent.rec)
      if (record.state != script::kRecIdle) sc += std::format(" {}:c{}@{:X}", record.id, record.state, record.pc);
    out += std::format("  ent {:2}  {:6.0f},{:6.0f},{:6.0f}  f38 {:06X} f270 {:X} r {} rect {}x{}  {}{}{}\n", i, static_cast<double>(pose.x),
                       static_cast<double>(pose.z), static_cast<double>(pose.y), script_ent.flags38, script_ent.flags270, script_ent.trigger_radius, script_ent.rect_w, script_ent.rect_h,
                       describe_ent(static_cast<int>(i)), ent_state.actor.loaded() ? "loaded" : "", sc);
  }
  return out;
}

std::string FieldWorld::describe_ent(int i) const {
  const Ent& ent_state = ents_[static_cast<size_t>(i)];
  std::string text;
  if (ent_state.pc_slot >= 0) text += std::format("PC slot {} char {} ", ent_state.pc_slot, ent_state.char_id);
  else if (ent_state.model >= 0) text += std::format("NPC model {} ", ent_state.model);
  if (ent_state.motion.active) text += "moving ";
  if (ent_state.actor.loaded()) {
    render::FieldCamera::Projected pr;
    const ActorPose& pose = ent_state.actor.current();
    if (view_.camera().project({pose.x, pose.y, pose.z}, pr))
      text += std::format("face {:03X} yaw {:.0f} pitch {:.0f} local-yaw {:.0f} roll {:.0f} scale {:.3f}/{:.3f}/{:.3f} proj {:.0f},{:.0f} cam-z {:.0f} (depth {:.0f}) (scroll {},{}) ", ent_state.actor.facing(), static_cast<double>(ent_state.actor.current().yaw), static_cast<double>(ent_state.actor.pitch), static_cast<double>(ent_state.actor.local_yaw), static_cast<double>(ent_state.actor.roll), static_cast<double>(ent_state.actor.scale * ent_state.actor.axis_scale[0]), static_cast<double>(ent_state.actor.scale * ent_state.actor.axis_scale[1]), static_cast<double>(ent_state.actor.scale * ent_state.actor.axis_scale[2]), static_cast<double>(pr.sx), static_cast<double>(pr.sy), static_cast<double>(pr.z), static_cast<double>(pr.z / 16.0f), view_.scroll_x(), view_.scroll_y());
    else
      text += "proj behind-camera ";
  }
  if (ent_state.actor.loaded()) text += std::format("clip '{}' t {:.2f} rate {:.2f} req {} ", ent_state.actor.clip_name(), static_cast<double>(ent_state.actor.anim_time()),
                                                    static_cast<double>(ent_state.actor.anim_rate), ent_state.actor.requested_anim());
  text += std::format("tri {} attr {:X} wfl {:X} ", ent_state.actor.triangle(), ent_state.actor.triangle() >= 0 ? view_.walk().flags(ent_state.actor.triangle()) : 0u,
                      walk_flags(i));
  return text;
}

// ================================================================================================ field status / HP effects

int FieldWorld::debug_char_stat(int id, int what) const {
  if (!gs_ || id < 0 || id >= save::kNumChars) return -1;
  const save::SaveChar& character = gs_->slot.chars[static_cast<size_t>(id)];
  return what == 0 ? character.cur_hp : what == 1 ? character.max_hp : character.status;
}
void FieldWorld::debug_set_char_stat(int id, int what, int value) {
  if (!gs_ || id < 0 || id >= save::kNumChars) return;
  save::SaveChar& character = gs_->slot.chars[static_cast<size_t>(id)];
  if (what == 0) character.cur_hp = static_cast<u16>(std::clamp(value, 0, 9999));
  else if (what == 2) character.status = static_cast<u8>(value);
  // Route `set model`: the D9 model byte is set and the party entities rebind, as DA would.
  else if (what == 3) {
    character.model_id = static_cast<u16>(value & 0x1FF);
    refresh_party_models();
  }
}

int FieldWorld::party_status_bits() const {
  int bits = 0;
  if (!gs_) return 0;
  for (int id : party_chars_)
    if (id >= 0 && id < save::kNumChars) bits |= gs_->slot.chars[static_cast<size_t>(id)].status;
  return bits;
}

// Status effects, once per logic tick. The flu wobble phase advances every tick (+= 0x20 + (rand() & 0x1F)); the rest only while
// the input lock is clear:
//  - flu: a counter counts up; past 0x100 + a random value (rand() & 0x1FF, re-rolled) it restarts and script variable 0x40 is
//    set to 1 if a member has flu (no script or engine code reads it back);
//  - poison: a counter counts up; past 0xF0 (every 241 ticks) it restarts and every poisoned member loses 4 HP, never below 1;
//  - damage floor: while the accumulator (status_on_move) is past 0x80 it drops by 0x80, the jolt
//    starts (8 frames) and every member loses 2 HP (4 with burn), never below 1.
// HP below 1 becomes 1 (a member at 0 HP comes back at 1): the field never kills, so there is no field game over.
void FieldWorld::step_status() {
  auto rnd = [&] {
    u32& state = fstat_.rng;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  fstat_.flu_phase += 0x20 + (rnd() & 0x1F);
  if (!gs_ || (vm_.global_flags() & script::gflag::kInputLock)) return;
  auto drain = [&](int need, int amount, int amount_burn) {
    for (int id : party_chars_) {
      if (id < 0 || id >= save::kNumChars) continue;
      save::SaveChar& character = gs_->slot.chars[static_cast<size_t>(id)];
      if (need && !(character.status & need)) continue;
      const int hp = static_cast<int>(character.cur_hp) - ((character.status & kStBurn) ? amount_burn : amount);
      character.cur_hp = static_cast<u16>(std::max(1, hp));
      log::debug("field: tick {} char {} HP {} ({})", tick_count_, id, character.cur_hp, need ? "poison" : "damage floor");
    }
  };
  if (++fstat_.flu_ticks > fstat_.flu_next + 0x100) {
    fstat_.flu_ticks = 0;
    fstat_.flu_next = static_cast<int>(rnd() & 0x1FF);
    if (party_status_bits() & kStFlu) vm_.set_var(0x40, 1);
  }
  if (++fstat_.poison_ticks > 0xF0) {
    fstat_.poison_ticks = 0;
    if (party_status_bits() & kStPoison) {
      ++fstat_.poison_hits;
      drain(kStPoison, 4, 4);
    }
  }
  if (fstat_.floor_acc > 0x80) {
    fstat_.floor_acc -= 0x80;
    fstat_.jolt = 8;
    ++fstat_.floor_hits;
    log::info("field: tick {} damage floor: party HP -2 (-4 with burn), jolt", tick_count_);
    drain(0, 2, 4);
  }
}

// Each tick the pad moves the controlled entity: walk-mesh flag 0x10000 on the triangle it stands on
// adds 2 to the damage-floor accumulator walking, 5 running; moving on any other triangle resets it to 0; standing still
// keeps it. Rooms make floors hurt by editing the flag table (op 75 SETUP_FLAG lane 2 bit 0: Hydra Marshes hid_b* water 0x20000 /
// 0x40000 -> 0x30000 / 0x50000; no room ships attr 81 = 0x10000 itself).
void FieldWorld::status_on_move(int, u32 tri_flags, bool run) {
  if (tri_flags & 0x10000) fstat_.floor_acc += run ? 5 : 2;
  else fstat_.floor_acc = 0;
}
// Flu: while the pad moves the player and a member has flu, the heading gets (rsin(phase) << 9) >> 12 added
// (up to +-0x200 = 45 degrees, the phase stepping 0x20..0x3F per tick).
int FieldWorld::status_flu_wobble() { return (party_status_bits() & kStFlu) ? (ps1_sin(static_cast<int>(fstat_.flu_phase)) << 9) >> 12 : 0; }
// Sprain: with a sprained member the player always walks (the run button / auto-run setting are ignored).
bool FieldWorld::status_no_run() const { return (party_status_bits() & kStSprain) != 0; }

}  // namespace oc::game
