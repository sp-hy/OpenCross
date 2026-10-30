// FieldWorld: the party, roster, items, money, character stats and models, and the save-state capture / restore.
#include "game/field_world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>

#include "core/log.h"
#include "game/field_world_internal.h"
#include "game/new_game_plus.h"

namespace oc::game {

using namespace field_internal;

int FieldWorld::party_entity(int slot) { return slot >= 0 && slot < 3 ? party_ent_[static_cast<size_t>(slot)] : -1; }

int FieldWorld::character_entity(int char_id) {
  for (int slot = 0; slot < 3; ++slot)
    if (slot < static_cast<int>(party_chars_.size()) && party_chars_[static_cast<size_t>(slot)] == char_id) return party_ent_[static_cast<size_t>(slot)];
  return -1;
}

int FieldWorld::party_slot_of_entity(int ent) {
  for (int slot = 0; slot < 3; ++slot)
    if (party_ent_[static_cast<size_t>(slot)] == ent && ent >= 0) return slot;
  return -1;
}

void FieldWorld::bind_party_entity(int slot) {
  const int ent = party_ent_[static_cast<size_t>(slot)];
  if (ent < 0) return;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  script::Entity& ve = vm_.entity(ent);
  // An empty party slot: the entity stays out of play.
  if (slot >= static_cast<int>(party_chars_.size())) {
    ent_state.char_id = -1;
    ent_state.actor.set_model(nullptr);
    ve.flags38 |= script::flag38::kInactive;
    ent_state.slot_inactive = true;
    return;
  }
  // The slot was emptied by an earlier bind (7D) and has an occupant again (7B / 7C, or the others moved up).
  if (ent_state.slot_inactive) {
    ve.flags38 &= ~script::flag38::kInactive;
    ent_state.slot_inactive = false;
  }
  ent_state.char_id = party_chars_[static_cast<size_t>(slot)];
  ent_state.char_model = model_byte(ent_state.char_id);
  if (auto model = character_model(ent_state.char_model)) ent_state.actor.set_model(model);
  // In another body: the character's own model is made ready for the switch back.
  if (ent_state.char_model != ent_state.char_id) prefetch_character_model(ent_state.char_id);
  // Solid; bit 22 marks the player class that attr 4 / 5 walls block.
  ve.flags38 |= 0x400100;
}

void FieldWorld::rebind_party() {
  for (int slot = 0; slot < 3; ++slot) bind_party_entity(slot);
}

void FieldWorld::setup_pc(int ent, int char_ref, int) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return;
  // SETUP_PC: slot = first party slot holding the referenced character, so `0E FD` binds the leader and `0E FF` the third member
  // (opn_a01i / dem_01: entity 1 = FD runs the leader's SET_CONTROLLED_ENTITY THIS).
  int slot = -1;
  if (const int char_id = char_from_ref(party_chars_, char_ref); char_id >= 0)
    for (size_t party_slot = 0; party_slot < party_chars_.size() && slot < 0; ++party_slot)
      if (party_chars_[party_slot] == char_id) slot = static_cast<int>(party_slot);
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  ent_state.pc_slot = slot;
  if (slot < 0) {
    // That character is not in the party.
    vm_.entity(ent).flags38 |= script::flag38::kInactive;
    return;
  }
  party_ent_[static_cast<size_t>(slot)] = ent;
  bind_party_entity(slot);
  const bool lead_ready = slot == 0 || (party_ent_[0] >= 0 && ents_[static_cast<size_t>(party_ent_[0])].placed);
  if (slot < static_cast<int>(party_chars_.size()) && !ent_state.placed && lead_ready) place_party_spawn(slot);
}

void FieldWorld::setup_npc(int ent, int model) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  ent_state.model = model;
  if (auto handle = room_model(model)) {
    ent_state.actor.set_model(handle);
    if (!ent_state.actor.has_anim_seq()) ent_state.actor.stand_on_clip_zero();
  }
  // SETUP_NPC and D1: solid; bit 21 marks the NPC class that attr 3 / 5 walls block.
  vm_.entity(ent).flags38 = (vm_.entity(ent).flags38 & ~0x4000u) | 0x200100;
}

// D1: the model is room model `model` (the room's own model list, the one 0F uses too); D1 also re-places the entity on floor
// level 0 and gives it the next ordinal of the room's D1 counter (zeroed at room init), its bit in the defeated-monster mask
// (the VM sets the flags).
int FieldWorld::load_model(int ent, int model) {
  setup_npc(ent, model);
  if (ent >= 0 && ent < static_cast<int>(ents_.size())) {
    Ent& ent_state = ents_[static_cast<size_t>(ent)];
    const ActorPose& pose = ent_state.actor.current();
    if (const WalkMesh& walk = view_.walk(); !walk.empty()) {
      const int tri = walk.find_triangle_level(pose.x, pose.z, 0);
      if (tri >= 0) ent_state.actor.set_height(walk.height(tri, pose.x, pose.z), !ent_state.placed);
    }
  }
  return d1_counter_++;
}


void FieldWorld::formation(u16 key, std::span<const s32> args) {
  // 84: one (x, z) per slot (+ facings); DF: the leader's (x, z) and engine heading, slots 1 and 2 in the party V around it. The members
  // walk there (straight steps at their own speed) and op 85 / 86 wait for all three.
  float tx[3] = {0, 0, 0}, tz[3] = {0, 0, 0};
  if (key == 0xDF && args.size() >= 3) {
    tx[0] = static_cast<float>(args[0]);
    tz[0] = static_cast<float>(args[1]);
    for (int slot = 1; slot < 3; ++slot) party_v_spot(slot, tx[0], tz[0], args[2] & 0xFFF, tx[slot], tz[slot]);
  } else if (args.size() >= 6) {
    for (int slot = 0; slot < 3; ++slot) {
      tx[slot] = static_cast<float>(args[static_cast<size_t>(2 * slot)]);
      tz[slot] = static_cast<float>(args[static_cast<size_t>(2 * slot + 1)]);
    }
  } else {
    return;
  }
  // The facing of each slot record (op 84 U@14 / U@16 / U@18, DF the heading operand for all three) is applied when the
  // member arrives, unless the caller slot's flag is set or the member's turn rate is 0.
  int caller_slot = 0;
  if (const int caller = vm_.current_entity(); caller >= 0) caller_slot = vm_.entity(caller).cur & 7;
  for (int slot = 0; slot < 3; ++slot) {
    int fin = -1;
    if (key == 0xDF && args.size() >= 3) fin = static_cast<int>(args[2]) & 0xFFF;
    else if (key == 0x84 && args.size() >= 9) fin = static_cast<int>(args[static_cast<size_t>(6 + slot)]) & 0xFFF;
    walk_party_member(slot, tx[slot], tz[slot], fin, caller_slot);
  }
}

void FieldWorld::walk_party_member(int slot, float x, float z, int final_facing, int gate_slot) {
  const int ent = party_ent_[static_cast<size_t>(slot)];
  if (ent < 0 || slot >= static_cast<int>(party_chars_.size())) return;
  script::MotionRequest request;
  request.kind = script::MotionKind::WalkXZ;
  request.x = static_cast<s32>(std::lround(x));
  request.z = static_cast<s32>(std::lround(z));
  start_motion(ent, request);
  // The formation walk steps the position itself.
  ents_[static_cast<size_t>(ent)].motion.direct = true;
  ents_[static_cast<size_t>(ent)].motion.final_facing = final_facing;
  ents_[static_cast<size_t>(ent)].motion.gate_slot = gate_slot;
}

// Op BE: sets the follow-hold flag while it waits (follow rule 1, the back-off, is skipped then) and a tick counter (0xF0 at
// the first call) counts down. It ends when all three slots are empty or not in the walk / run clip, or at 0; the flag is
// cleared then.
bool FieldWorld::follow_wait() {
  if (!follow_hold_) {
    follow_hold_ = true;
    follow_hold_ticks_ = 0xF0;
  }
  int calm = 0;
  for (int slot = 0; slot < 3; ++slot) {
    const int ent = party_ent_[static_cast<size_t>(slot)];
    if (ent < 0 || !(ents_[static_cast<size_t>(ent)].motion.walking || ents_[static_cast<size_t>(ent)].motion.running)) ++calm;
  }
  if (calm < 3 && follow_hold_ticks_ > 0) return false;
  follow_hold_ = false;
  return true;
}

bool FieldWorld::formation_done() {
  for (int slot = 0; slot < 3; ++slot) {
    const int ent = party_ent_[static_cast<size_t>(slot)];
    if (ent >= 0 && ents_[static_cast<size_t>(ent)].motion.active) return false;
  }
  return true;
}

// ------------------------------------------------------------------------------------------------ host: dialogue
// 62: x, y, cols, lines, flags, kept in the running entity's context until a script changes them.
// x / y = centre of the window in PS1 pixels (0 = placed by the engine), cols * 12 = width, lines = line count, flags: bit 1 label
// (centred at y 0x41 / 0xB7), bit 2-3 placement (0 auto, 1 below the speaker, 2 above, 3 screen centre), 0x10 frameless label,
// 0x40 no portrait, 0x800 / 0x1000 force the portrait to the right / left.

int FieldWorld::party_member(int slot) {
  return slot >= 0 && slot < static_cast<int>(party_chars_.size()) ? party_chars_[static_cast<size_t>(slot)] : -1;
}
bool FieldWorld::in_party(int char_id) { return std::find(party_chars_.begin(), party_chars_.end(), char_id) != party_chars_.end(); }

void FieldWorld::sync_party() {
  party_chars_ = gs_->party();
  if (party_chars_.empty()) party_chars_.push_back(0);
}
void FieldWorld::write_party() {
  std::array<u8, 3> ids{0xFF, 0xFF, 0xFF};
  for (size_t i = 0; i < party_chars_.size() && i < 3; ++i) ids[i] = static_cast<u8>(party_chars_[i]);
  gs_->slot.set_party(ids);
}

bool FieldWorld::party_add(int char_id) {
  // Ids >= 0x2C, a character already in the party and a full party are refused.
  if (in_party(char_id) || party_chars_.size() >= 3 || char_id < 0 || char_id >= 0x2C || char_id >= save::kNumChars) return false;
  party_chars_.push_back(char_id);
  gs_->slot.set_recruited(char_id, true);
  write_party();
  return true;
}

void FieldWorld::party_remove(int char_ref) {
  // 7D: the slot of the referenced character (first match) is emptied and the slots behind it move up;
  // the party may become empty (the attract demo dem_01 removes FF, FE, FD, then adds Serge, Kid and Glenn). A title-screen save
  // slot reads as party {0, 0, 0}: every removal finds Serge in slot 0, so three removals still empty it.
  const int char_id = char_from_ref(party_chars_, char_ref);
  auto it = std::find(party_chars_.begin(), party_chars_.end(), char_id);
  if (char_id >= 0 && it != party_chars_.end()) party_chars_.erase(it);
  write_party();
  // The party entities belong to the slots and the model blocks move up with the character ids:
  // the entity of the emptied last slot loses its model, the others show their new occupant.
  refresh_party_models();
}

// B2: enter saves the party entity table and the character table, then sorts both together in three bubble passes: slots j / j+1
// swap when t[j+1] == key, or when t[j] != key and t[j+1] < t[j] (signed, 0xFF last): the key character leads, the rest in
// ascending id order. Leave restores them. Both then clear the selected flag on every party entity and make the entity of the
// new slot-0 character the controlled one.
void FieldWorld::party_override(bool enter, int key) {
  if (enter) {
    saved_party_chars_ = party_chars_;
    saved_party_ent_ = party_ent_;
    saved_party_room_ = room_loads_;
    party_override_on_ = true;
    std::array<int, 3> chars{0xFF, 0xFF, 0xFF};
    for (size_t i = 0; i < party_chars_.size() && i < 3; ++i) chars[i] = party_chars_[i];
    std::array<int, 3> en = party_ent_;
    for (int pass = 0; pass < 3; ++pass)
      for (size_t j = 0; j < 2; ++j)
        if (chars[j + 1] == key || (chars[j] != key && chars[j + 1] < chars[j])) {
          std::swap(chars[j], chars[j + 1]);
          std::swap(en[j], en[j + 1]);
        }
    party_chars_.clear();
    for (int char_id : chars)
      if (char_id != 0xFF) party_chars_.push_back(char_id);
    party_ent_ = en;
  } else {
    if (!party_override_on_) return;
    party_override_on_ = false;
    party_chars_ = saved_party_chars_;
    if (saved_party_room_ == room_loads_) party_ent_ = saved_party_ent_;
  }
  write_party();
  for (int slot = 0; slot < 3; ++slot) {
    const int ent = party_ent_[static_cast<size_t>(slot)];
    if (ent < 0 || ent >= static_cast<int>(ents_.size())) continue;
    ents_[static_cast<size_t>(ent)].pc_slot = slot < static_cast<int>(party_chars_.size()) ? slot : -1;
    vm_.entity(ent).flags38 &= ~0x2u;
    if (slot == 0) vm_.entity(ent).flags38 |= script::flag38::kSelected;
  }
  log::debug("field: tick {} party override {} key {}: leader ent {}", tick_count_, enter ? "on" : "off", key, party_ent_[0]);
}

// FE 52: U = a character id; only when it is in party slot k = 1 or 2: the two slots trade their model buffers, textures,
// CLUT rows and character ids. The entity table is not swapped: the
// entities keep their places (slot 0's entity, the controlled one, now shows character U).
void FieldWorld::swap_leader(int char_id) {
  int target_slot = -1;
  for (int slot = 1; slot < 3 && slot < static_cast<int>(party_chars_.size()); ++slot)
    if (party_chars_[static_cast<size_t>(slot)] == char_id) target_slot = slot;
  if (target_slot < 0) return;
  std::swap(party_chars_[0], party_chars_[static_cast<size_t>(target_slot)]);
  write_party();
  bind_party_entity(0);
  bind_party_entity(target_slot);
  log::debug("field: tick {} swap leader with slot {} (char {})", tick_count_, target_slot, char_id);
}

// Select: slot 0 <- slot 1, slot 1 <- slot 2, slot 2 <- the old leader (two members: they trade places). Like FE 52 the characters
// move, not the entities: every party entity keeps its place, its followers' trail and the control, and shows its slot's new
// character. The per-slot words of variables 0x170 / 0x172 / 0x174 (the disguise model bytes the main menu shows) move with their
// characters. Refused while the controls are not free, with fewer than two members, while B2 has the party sorted (global bit
// 0x40), while global bit 0x800 (FE 17) is set and while variable 0x36 is not 0.
void FieldWorld::rotate_party() {
  if (party_chars_.size() < 2 || !controls_free() || menu_active() || battle_active()) return;
  if ((vm_.global_flags() & 0x840u) != 0 || vm_.var(0x36) != 0) return;
  const size_t count = std::min<size_t>(party_chars_.size(), 3);
  std::rotate(party_chars_.begin(), party_chars_.begin() + 1, party_chars_.begin() + static_cast<std::ptrdiff_t>(count));
  for (size_t slot = 0; slot + 1 < count; ++slot) {
    const u16 here = static_cast<u16>(0x170 + 2 * slot), next = static_cast<u16>(here + 2);
    const s32 word = vm_.var(here);
    vm_.set_var(here, vm_.var(next));
    vm_.set_var(next, word);
  }
  write_party();
  rebind_party();
  log::info("field: tick {} party rotated (Select): leader char {}, leader ent {}", tick_count_, party_chars_[0], party_ent_[0]);
}
void FieldWorld::roster_add(int char_id) {
  if (char_id >= 0 && char_id < save::kNumChars) gs_->slot.set_recruited(char_id, true);
}
void FieldWorld::roster_remove(int char_id) {
  if (char_id >= 0 && char_id < save::kNumChars) gs_->slot.set_recruited(char_id, false);
}

// Script item ids (BB / B4 / D5): < 0x200 gear and materials, < 0x400 elements (id - 0x200), 0x400.. key items (id - 0x400 = key
// item number, 1-based as in GameState::has_key_item).
int FieldWorld::item_count(int id) {
  if (id < 0) return 0;
  if (id < 0x200) return gs_->item_count(id);
  if (id < 0x400) return gs_->element_count(id - 0x200);
  return gs_->has_key_item(id - 0x400) ? 1 : 0;
}
void FieldWorld::give_item(int id) {
  if (id <= 0) return;
  if (on_item_given_) on_item_given_(id);
  if (id < 0x200) {
    gs_->add_item(id);
  } else if (id < 0x400) {
    gs_->add_element(id - 0x200);
  } else if (const int key_item = id - 0x400; key_item >= 1 && key_item <= 64) {
    const bool had = gs_->has_key_item(key_item);
    gs_->slot.set_key_items(gs_->slot.key_items() | (1ull << (key_item - 1)));
    if (!had && on_key_item_) on_key_item_(key_item);
  }
}
void FieldWorld::take_item(int id) {
  if (id <= 0) return;
  if (id < 0x200) {
    gs_->take_item(id);
  } else if (id < 0x400) {
    gs_->take_element(id - 0x200);
  } else if (const int key_item = id - 0x400; key_item >= 1 && key_item <= 64) {
    gs_->slot.set_key_items(gs_->slot.key_items() & ~(1ull << (key_item - 1)));
  }
}
// Field key-item list. Only Tele-Porter (1), Relief Charm (61) and Smith Spirit (63) are usable, and only when the script allows it.
bool FieldWorld::key_item_usable(int item) const {
  if (item != 1 && item != 61 && item != 63) return true;
  return vm_.var(0x46) == 1 && vm_.var(0x3C) == 1;
}
void FieldWorld::use_key_item(int item) {
  if (item < 1 || item > 64) return;
  // USED_KEY_ITEM
  vm_.set_var(0x2A, 0x400 + item);
  if (item != 1 && item != 61 && item != 63) {
    key_item_shown_ = true;
    return;
  }
  if (!key_item_usable(item) || menu_state_ != MenuState::None) return;
  // Tele-Porter: the FE 5B party screen. Smith Spirit: the FE 68 smith (shop id = var 0x4A). Relief Charm: party screen to
  // pick the relief member (arg 1).
  if (item == 1) {
    menu_req_ = {script::MenuKind::PartyScreen, 0};
  } else if (item == 63) {
    menu_req_ = {script::MenuKind::WeaponShop, vm_.var(0x4A)};
  } else {
    menu_req_ = {script::MenuKind::PartyScreen, 1};
  }
  menu_state_ = MenuState::Requested;
  log::info("field: key item {} used -> menu kind {} arg {}", item,static_cast<int>(menu_req_.kind), menu_req_.arg);
}
s32 FieldWorld::money() { return static_cast<s32>(gs_->gold()); }
void FieldWorld::set_money(s32 amount) { gs_->set_gold(static_cast<u32>(std::clamp(amount, 0, 9999999))); }

// FE 37 / 38 / 39 / 3A: HP of a character record; 38 clamps to [1, max], 39 adds with a cap and no floor,
// 3A damages and never kills (floor 1).
// FE 37-3B take a character reference: FD / FE / FF = the member in slot 0 / 1 / 2.
int FieldWorld::char_life(int char_ref) {
  const int char_id = char_from_ref(party_chars_, char_ref);
  if (char_id < 0 || char_id >= save::kNumChars) return 0;
  return gs_->slot.chars[static_cast<size_t>(char_id)].cur_hp;
}
void FieldWorld::change_char_life(script::LifeOp op, int char_ref, s32 value) {
  const int char_id = char_from_ref(party_chars_, char_ref);
  if (char_id < 0 || char_id >= save::kNumChars) return;
  save::SaveChar& character = gs_->slot.chars[static_cast<size_t>(char_id)];
  const int max = character.max_hp;
  int hp = character.cur_hp;
  switch (op) {
    case script::LifeOp::SetClamped: hp = std::clamp<int>(value, 1, std::max(1, max)); break;
    case script::LifeOp::AddCapped: hp = std::min<int>(max, hp + value); break;
    case script::LifeOp::Damage: hp = std::max(1, hp - value * ((character.status & 8) ? 2 : 1)); break;
  }
  character.cur_hp = static_cast<u16>(std::max(0, hp));
}
int FieldWorld::char_status(int char_ref) {
  const int char_id = char_from_ref(party_chars_, char_ref);
  if (char_id < 0 || char_id >= save::kNumChars) return 0;
  return gs_->slot.chars[static_cast<size_t>(char_id)].status;
}
// The character operand of FE 36 / 3D / 3E: 0xF9 = every recruited character (chars 0..43), 0xFA = the three party slots (empty slots skipped), 0xFD / 0xFE / 0xFF = party slot 0 / 1 / 2, any other
// value < 0x2C = that character id; everything else (0xFB, 0xFC, 0x80 ...) does nothing.
void FieldWorld::for_char_targets(int ref, const std::function<void(save::SaveChar&)>& fn) {
  auto one = [&](int id) {
    if (id >= 0 && id < save::kNumChars && id < 0x2C) fn(gs_->slot.chars[static_cast<size_t>(id)]);
  };
  if (ref == 0xF9) {
    for (int id = 0; id < 0x2C && id < save::kNumChars; ++id)
      if (gs_->slot.recruited(id)) one(id);
  } else if (ref == 0xFA) {
    for (int id : party_chars_) one(id);
  } else if (ref >= 0xFD && ref <= 0xFF) {
    one(char_from_ref(party_chars_, ref));
  } else if (ref >= 0 && ref < 0x2C) {
    one(ref);
  }
}
// FE 36 HEAL: current HP = max HP of the targets. Status is not touched (scripts cure it with FE 3E).
void FieldWorld::heal_group(int ref) {
  for_char_targets(ref, [](save::SaveChar& character) { character.cur_hp = character.max_hp; });
}
// FE 3D: status word |= mask (inflict: 1 poison, 8 burn, 0x10 sprain, 0x20 flu); FE 3E:
// status &= ~(mask & 0xFF) (cure). The save keeps the low byte; higher bits of an inflicted mask are not stored.
void FieldWorld::change_status(int ref, int mask, bool set) {
  for_char_targets(ref, [&](save::SaveChar& character) {
    if (set) character.status = static_cast<u8>(character.status | (mask & 0xFF));
    else character.status = static_cast<u8>(character.status & ~(mask & 0xFF));
  });
  log::debug("field: tick {} {} status {:#x} on {:#x}", tick_count_, set ? "SET" : "CURE", mask, ref);
}
// FE 79 / FE 74 / FE 73 (level 0 / 1 / 2): the tech `value - 0x200` goes into the cell that closes star level 3 / 5 / 7.
void FieldWorld::learn_limit(int level, int char_id, int value) {
  if (!gs_ || char_id < 0) return;
  const int cell = gs_->learn_tech(char_id, 3 + 2 * level, value - 0x200);
  log::debug("field: tick {} char {} learns tech {:#x} in cell {}", tick_count_, char_id, value - 0x200, cell);
}
// FE 7B: Serge's innate colour byte keeps bits 0..2 and gets the single colour bit 0x08 << mode (0 white, 1 black, 2 red, 3 yellow,
// 4 blue; a larger mode leaves no colour bit).
void FieldWorld::set_limit_mode(int mode) {
  if (!gs_) return;
  u8& colour = gs_->slot.chars[0].colour_bit;
  colour = static_cast<u8>((colour & 7) | static_cast<u8>(1u << ((mode + 3) & 31)));
}
// D9 / DA / DB: the saved character record model byte selects the field model: obj file 2b + 2, tim
// 2b + 3, normally the character id, 0x31 = Serge's Lynx body, 0x3B / 0x3C.. other bodies.
int FieldWorld::model_byte(int char_id) const {
  if (char_id < 0 || char_id >= save::kNumChars) return char_id;
  const int model_id = gs_->slot.chars[static_cast<size_t>(char_id)].model_id;
  // A record OpenCross' new-game builder did not fill (byte 0 on a character other than Serge) shows the character's own model.
  return model_id == 0 && char_id != 0 && gs_->slot.chars[static_cast<size_t>(char_id)].max_hp == 0 ? char_id : model_id;
}

std::string FieldWorld::model_folder(int model_byte) const {
  // Files 2b + 2 of the character ids are these folders.
  if (model_byte >= 0 && model_byte < 44) return kCharFolders[model_byte];
  if (model_byte < 0 || model_byte >= 0x100 || !discs_.loaded()) return {};
  constexpr int kFidToRecord = DiscIndex::kMapBase - 0x83;
  const std::string& rec = discs_.record(disc_, 2 * model_byte + 2 + kFidToRecord);
  const size_t folder_at = rec.find("fieldobj/player/");
  if (folder_at == std::string::npos || !rec.ends_with(".obj")) return {};
  const size_t folder_end = rec.find('/', folder_at + 16);
  return folder_end == std::string::npos ? std::string() : rec.substr(folder_at + 16, folder_end - folder_at - 16);
}

// D9: char = U@1 (FD / FE / FF = slot 0 / 1 / 2, <= 0x2B an id, else none). Nothing to do (none, or the
// byte already b) -> pc += 6. Else the byte is written; a character outside the party -> pc += 6; a party member's model is replaced:
// files read, pc += 5 onto the DA, which installs the model on that slot's entity once loaded.
int FieldWorld::set_character_model(int char_ref, int value) {
  const int char_id = char_ref == 0xFC ? -1 : char_from_ref(party_chars_, char_ref);
  if (char_id < 0 || char_id > 0x2B || char_id >= save::kNumChars) return 0;
  save::SaveChar& rec = gs_->slot.chars[static_cast<size_t>(char_id)];
  if (rec.model_id == (value & 0x1FF)) return 0;
  int slot = -1;
  for (size_t party_slot = 0; party_slot < party_chars_.size() && party_slot < 3; ++party_slot)
    if (party_chars_[party_slot] == char_id) slot = static_cast<int>(party_slot);
  // A replacement is already pending.
  if (slot >= 0 && model_swap_slot_ >= 0) return -1;
  rec.model_id = static_cast<u16>(value & 0x1FF);
  log::debug("field: tick {} D9 char {} model byte {} (slot {})", tick_count_, char_id, value, slot);
  if (slot < 0) return 0;
  model_swap_slot_ = slot;
  return 1;
}
// The files are read synchronously, so a swap is never busy.
bool FieldWorld::model_swap_busy() { return false; }
void FieldWorld::model_swap_commit() {
  if (model_swap_slot_ >= 0) bind_party_entity(model_swap_slot_);
  model_swap_slot_ = -1;
}
// DB: the model byte of the referenced character, 0 when there is none.
int FieldWorld::character_model_byte(int char_ref) {
  const int char_id = char_ref == 0xFC ? -1 : char_from_ref(party_chars_, char_ref);
  if (char_id < 0 || char_id > 0x2B || char_id >= save::kNumChars) return 0;
  return gs_->slot.chars[static_cast<size_t>(char_id)].model_id;
}

void FieldWorld::refresh_party_models() {
  // 7C / DA: the party table changed; rebind the party entities (the original re-runs the entities' scripts).
  rebind_party();
  for (int slot = 0; slot < 3; ++slot)
    if (party_ent_[static_cast<size_t>(slot)] >= 0 && slot < static_cast<int>(party_chars_.size()) &&
        !ents_[static_cast<size_t>(party_ent_[static_cast<size_t>(slot)])].placed)
      place_party_spawn(slot);
}


void FieldWorld::set_seed(u32 seed) {
  seed_ = seed;
  rng_ = seed ? (seed * 2654435761u) | 1u : 0x2545F491u;
}

void FieldWorld::set_game_state(ui::GameState* gs) {
  gs_ = gs ? gs : &own_state_;
  sync_party();
}

void FieldWorld::set_custom_names(const std::map<int, std::u32string>* names) {
  custom_names_ = names;
  install_name_hooks();
}

// {PARTYn%} (control 0x12) names the character in party slot n: slot -> character id (GameState) ->
// the typed name, else the character's default name. The text layer's own fallback would read the slot number as a character id.
void FieldWorld::install_name_hooks() {
  // A save record of a never-renamed character holds the JP default name in game-font codes (kana); outside the JP language the
  // font cannot show it (blank glyphs: "Miss    , I cannot", an empty speaker line, empty choice lines), so it counts as unset and
  // the sysmes default of the language applies. Blank / undecodable records are unset in every language.
  auto custom = [this](int id) -> const std::u32string* {
    if (!custom_names_) return nullptr;
    auto it = custom_names_->find(id);
    if (it == custom_names_->end() || it->second.empty()) return nullptr;
    const std::u32string& name = it->second;
    if (name.find_first_not_of(U"　 ") == std::u32string::npos) return nullptr;
    if (lang_ != import::Lang::Jp)
      for (char32_t ch : name)
        if (ch >= 0x3000) return nullptr;
    return &name;
  };
  auto char_name = [this, custom](int id) -> std::u32string {
    if (const std::u32string* name = custom(id)) return *name;
    return dialogue_.tables().default_name(id, lang_);
  };
  auto wire = [&](ui::DialogueDemo& dialogue) {
    dialogue.char_name = char_name;
    dialogue.party_name = [this, char_name](int slot) -> std::u32string {
      if (slot < 0 || slot >= static_cast<int>(party_chars_.size())) return {};
      return char_name(party_chars_[static_cast<size_t>(slot)]);
    };
    dialogue.speaker_name = [char_name](int speaker) -> std::u32string { return char_name(speaker); };
    dialogue.script_var = [this](int var) -> int { return vm_.var(static_cast<u16>(var)); };
    // script_set serves {fset(n)} (variable 0x22).
    dialogue.script_set = [this](int var, int value) { vm_.set_var(static_cast<u16>(var), value); };
  };
  wire(dialogue_);
  for (auto& window : extra_win_)
    if (window.box) wire(*window.box);
}

// The global variables 0x000..0x3FF are the save head block at 0x188 + id (MAP_ID 6 = location 0x18E, the party
// ids 0x0C..0x10 = the hidden copy at 0x194; 0x3D0..0x3FF end exactly at the play time word 0x588).
namespace {
// Preview name of the save header (block 0x28, 12 bytes): standard Shift-JIS; kana to their double-byte codes, ASCII as is.
std::array<u8, 12> sjis_preview_name(std::u32string_view text) {
  std::array<u8, 12> out{};
  size_t count = 0;
  for (char32_t ch : text) {
    if (ch >= 0x20 && ch < 0x7F) {
      if (count + 1 > 11) break;
      out[count++] = static_cast<u8>(ch);
      continue;
    }
    u16 code = 0;
    if (ch >= 0x3041 && ch <= 0x3093) code = static_cast<u16>(0x829F + (ch - 0x3041));
    else if (ch >= 0x30A1 && ch <= 0x30F6) code = static_cast<u16>(0x8340 + (ch - 0x30A1) + (ch - 0x30A1 >= 0x3F ? 1 : 0));
    else if (ch == 0x30FC) code = 0x815B;
    else if (ch == 0x3000) code = 0x8140;
    if (!code) continue;
    if (count + 2 > 11) break;
    out[count++] = static_cast<u8>(code >> 8);
    out[count++] = static_cast<u8>(code & 0xFF);
  }
  return out;
}
}  // namespace

void FieldWorld::capture_state() {
  save::SaveSlot& sl = gs_->slot;
  // The location word (head 0x18E) is the room id Continue loads.
  if (map_id_ >= 0) {
    sl.head[save::head::kLocation] = static_cast<u8>(map_id_ & 0xFF);
    sl.head[save::head::kLocation + 1] = static_cast<u8>((map_id_ >> 8) & 0xFF);
  }
  sl.head[save::head::kPreviewStars] = sl.head[save::head::kStarsTotal];
  // Head 0x40 mirrors SCENARIO_FLAG.
  sl.set_progression(static_cast<u16>(vm_.var(0)));
  if (custom_names_ && !party_chars_.empty()) {
    auto it = custom_names_->find(party_chars_[0]);
    if (it != custom_names_->end() && !it->second.empty()) {
      const auto nm = sjis_preview_name(it->second);
      std::copy(nm.begin(), nm.end(), sl.head.begin() + save::head::kLeaderName);
    }
  }
  if (const int player = player_entity(); player >= 0 && ents_[static_cast<size_t>(player)].placed) {
    const ActorPose& pose = ents_[static_cast<size_t>(player)].actor.current();
    vm_.set_var(0x12, static_cast<s32>(std::lround(pose.x)));
    vm_.set_var(0x14, static_cast<s32>(std::lround(pose.z)));
    vm_.set_var(0x16, static_cast<s32>(std::lround(pose.y)));
    vm_.set_var(0x0A, to_script(ents_[static_cast<size_t>(player)].actor.facing()));
  }
  for (int id = 0; id < 0x400; id += 2) {
    const u16 value = static_cast<u16>(vm_.var(static_cast<u16>(id)));
    sl.head[0x188 + static_cast<size_t>(id)] = static_cast<u8>(value & 0xFF);
    sl.head[0x188 + static_cast<size_t>(id) + 1] = static_cast<u8>(value >> 8);
  }
  // The location word is variable 6 (head 0x18E): the room id, written into the state only - the VM's MAP_ID is the room load's
  // (FE 2F leaves it 0 in New Game+ until the MAP_JUMP).
  if (map_id_ >= 0) {
    sl.head[save::head::kLocation] = static_cast<u8>(map_id_ & 0xFF);
    sl.head[save::head::kLocation + 1] = static_cast<u8>((map_id_ >> 8) & 0xFF);
  }
  // Summary flag bits (head 0x42 bits 0-1) = variable 0x7E & 3: 2 New Game+, 3 cleared.
  sl.head[save::head::kFlags42] = static_cast<u8>((sl.head[save::head::kFlags42] & ~3u) | (vm_.var(ngplus::kFlagVar) & 3));
  write_party();
  // A state that never came from a card gets the PS1 save header ("SC", icon flag, 1 block).
  if (sl.head[0] == 0) {
    sl.head[0] = 'S';
    sl.head[1] = 'C';
    sl.head[2] = 0x11;
    sl.head[3] = 0x01;
  }
}

std::array<u16, 16> FieldWorld::dopple_mask() const {
  std::array<u16, 16> mask{};
  for (size_t i = 0; i < 16; ++i) mask[i] = static_cast<u16>(vm_.var(static_cast<u16>(0x3E0 + 2 * i)));
  return mask;
}

void FieldWorld::set_dopple_mask(const std::array<u16, 16>& mask) {
  for (size_t i = 0; i < 16; ++i) {
    const u16 id = static_cast<u16>(0x3E0 + 2 * i);
    vm_.set_var(id, mask[i]);
    gs_->slot.head[0x188 + id] = static_cast<u8>(mask[i] & 0xFF);
    gs_->slot.head[0x188 + id + 1] = static_cast<u8>(mask[i] >> 8);
  }
}

void FieldWorld::restore_vars() {
  const save::SaveSlot& sl = gs_->slot;
  for (int id = 0; id < 0x400; id += 2) {
    const u16 value = static_cast<u16>(sl.head[0x188 + static_cast<size_t>(id)] | (sl.head[0x188 + static_cast<size_t>(id) + 1] << 8));
    vm_.set_var(static_cast<u16>(id), value);
  }
  // A save made on disc 2 continues on disc 2.
  if (const int need = vm_.var(0x42) + 1; need == 1 || need == 2) set_disc(need);
  sync_party();
  // The defeated-monster cache is not in the save: it starts empty after every load.
  defeated_ = {};
}

bool FieldWorld::place_leader(int x, int z) {
  const int ent = party_ent_[0];
  if (ent < 0 || !view_.loaded()) return false;
  const WalkMesh& walk = view_.walk();
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  const int tri = walk.find_triangle(static_cast<float>(x), static_cast<float>(z), 0.0f);
  if (tri < 0) {
    log::warn("field: saved position {}, {} is off the walk mesh of {}; keeping the entrance", x, z, view_.room_name());
    return false;
  }
  ent_state.actor.place(walk, tri, static_cast<float>(x), static_cast<float>(z), ent_state.actor.facing());
  ent_state.placed = true;
  for (int slot = 1; slot < 3; ++slot)
    if (party_ent_[static_cast<size_t>(slot)] >= 0) {
      ents_[static_cast<size_t>(party_ent_[static_cast<size_t>(slot)])].placed = false;
      place_party_spawn(slot);
    }
  // The same spread as a room entry: a V spot that fails (off the mesh, another floor) leaves the member on the leader's spot, and
  // nothing walked it out here, so a Continue would show the party stacked on the leader. Resets the rings too.
  settle_spawned_followers();
  snap_camera_to_player();
  return true;
}

void FieldWorld::reset_for_new_game() {
  // Variables 0..0x3CF = 0; 0x3D0..0x3FE come from the new game's state (var
  // 0x3D2 = 0x3F; game/new_game.h) - zeros only for a state that never had them.
  for (u16 id = 0; id < 0x3D0; id += 2) vm_.set_var(id, 0);
  for (u16 id = 0x3D0; id < 0x400; id += 2) vm_.set_var(id, gs_ ? gs_->slot.var(id) : 0);
  last_song_ = -1;
  menu_state_ = MenuState::None;
  movie_state_ = MovieState::None;
  set_disc(1);
  sync_party();
  // A new game zeroes the status counters and the defeated-monster cache.
  fstat_.poison_ticks = fstat_.flu_ticks = fstat_.floor_acc = 0;
  defeated_ = {};
}

}  // namespace oc::game
