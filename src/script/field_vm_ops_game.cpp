// Main opcode page, part 3: dialog, battle, items, shop, money, monster spawn, fade and miscellaneous opcodes.
#include "script/field_vm.h"

#include <algorithm>
#include <array>

namespace oc::script {

namespace {

constexpr s32 kMaxMoney = 9999999;

}  // namespace

bool FieldVM::exec_game(u8 op) {
  u32& pc = rec_->pc;
  Entity& self = *self_;
  switch (op) {
    case 0xA0: {
      const std::array<s32, 1> a = {byte_at(1)};
      host_.entity_effect(0xA0, ent_, a);
      advance(2);
      break;
    }
    case 0xA1: {
      const int target = entity_operand(1);
      if (target >= 0) host_.set_entity_rgb(target, operand_u(2), operand_u(4), operand_u(6));
      advance(8);
      break;
    }
    case 0xA2: {
      const int target = entity_operand(1);
      if (target >= 0) ents_[static_cast<size_t>(target)].v10c = static_cast<u16>(operand_u(2) << 4);
      advance(4);
      break;
    }
    case 0xA3:
    case 0xC9:
      // Cheats "No encounters": the battle was skipped; run "09 00 02" (entity 0's slot 2, waited for) instead.
      if (rec_->phase >= 2) {
        const int character = resolve_character(0);
        if (rec_->phase == 2) {
          int ri = -1;
          Spawn spawned = Spawn::None;
          if (character >= 0 && !(ents_[static_cast<size_t>(character)].flags38 & flag38::kInactive)) spawned = spawn(character, 2, 0, true, &ri);
          if (spawned == Spawn::NoRecord) {
            // Retried next tick.
            yield_ = true;
            break;
          }
          if (spawned != Spawn::Spawned) {
            rec_->phase = 0;
            advance(7);
            yield_ = true;
            break;
          }
          rec_->phase = 3;
          rec_->child_ent = static_cast<s16>(character);
          rec_->child_rec = static_cast<s8>(ri);
          yield_ = true;
          break;
        }
        ScriptRecord& child = ents_[static_cast<size_t>(rec_->child_ent)].rec[static_cast<size_t>(rec_->child_rec)];
        if (child.state == kRecIdle) {
          child.hold = 0;
          rec_->phase = 0;
          rec_->child_ent = -1;
          rec_->child_rec = -1;
          advance(7);
        }
        yield_ = true;
        break;
      }
      if (rec_->phase == 0 && host_.system_ready() && host_.skip_battle(operand_u(1), op == 0xC9, ent_)) {
        rec_->phase = 2;
        yield_ = true;
        break;
      }
      // A pending battle holds the system gate, so a second A3 / C9 waits here for its own fight. The request steps on at once:
      // the FE 10 that follows every A3 / C9 waits for the battle.
      yield_ = true;
      if (!host_.system_ready()) break;
      self.flags270 |= flag270::kInputLock;
      gflags_ |= gflag::kInputLock;
      host_.request_battle(operand_u(1) + (op == 0xC9 ? 0x200 : 0), operand_u(3), operand_u(5));
      advance(7);
      break;
    case 0xA4: set_global(Global::SubmergedOtPush, operand_u(1)); advance(3); break;
    case 0xA5: self.v24e = static_cast<u16>(operand_u(1)); advance(3); break;
    case 0xA6: set_global(Global::RegisterInvA6, byte_at(1) == 0); advance(2); break;
    case 0xA7: opaque(0xA7, 1, 8); advance(9); break;
    case 0xA8: host_.fade(byte_at(1), operand_u(2)); advance(4); break;
    case 0xA9: {
      if (!run_mode_ && host_.entry_window()) {
        yield_ = true;
        break;
      }
      const int target = entity_operand(1);
      if (target >= 0) {
        host_.play_animation(target, operand_u(2));
        budget_ += 1;
      }
      advance(4);
      break;
    }
    case 0xAA: {
      const int target = entity_operand(2);
      if (target >= 0 && byte_at(1) < 3) ents_[static_cast<size_t>(target)].axis[byte_at(1)] = static_cast<u16>(operand_u(3));
      advance(5);
      break;
    }
    case 0xAB: opaque(0xAB, 1, 4); advance(5); break;
    case 0xAC: {
      const int target = entity_operand(1);
      if (target >= 0 && !(ents_[static_cast<size_t>(target)].flags38 & flag38::kNo3dObject)) {
        const std::array<s32, 1> args = {operand_u(2)};
        host_.entity_effect(0xAC, target, args);
      }
      advance(4);
      break;
    }
    case 0xAD: set_flag(self.flags270, 5, byte_at(1) & 1); advance(2); break;
    case 0xAE: set_flag(self.flags270, 6, byte_at(1) & 1); advance(2); break;
    case 0xAF: {
      const u32 idx = static_cast<u32>(operand_u(6));
      if (idx < pair_table_.size()) pair_table_[idx] = {operand_s(1, 5, 0x80), operand_s(3, 5, 0x40)};
      advance(8);
      break;
    }
    case 0xB0: self.v110 = static_cast<u16>(operand_s(1, 3, 0x80)); advance(4); break;
    case 0xB1:
      if (byte_at(1) == 0) {
        self.timer_acc = 0;
        advance(2);
      } else {
        self.timer_acc += 0x4000;
        self.v110 = static_cast<u16>(self.v110 + (self.timer_acc >> 16));
        if (static_cast<s16>(self.v110) > 0) {
          self.v110 = 0;
          advance(2);
        }
      }
      break;
    case 0xB2:
      if (byte_at(1) == 0) {
        if (gflags_ & 0x40) {
          host_.party_override(false, 0);
          gflags_ &= ~0x40u;
        }
        advance(2);
      } else {
        if (!(gflags_ & 0x40)) {
          gflags_ |= 0x40;
          host_.party_override(true, operand_u(2));
        }
        advance(4);
      }
      break;
    case 0xB3: {
      const int target = entity_operand(1);
      if (target < 0 || (ents_[static_cast<size_t>(target)].flags38 & 0x8000))
        pc = word_at(2);
      else
        advance(4);
      break;
    }
    case 0xB4:
      if (host_.item_count(operand_u(1)) <= 0)
        pc = word_at(3);
      else
        advance(5);
      break;
    case 0xB5: set_global(Global::PushStrength, operand_u(1)); advance(3); break;
    case 0xB7: self.v10e = static_cast<u16>(operand_s(1, 3, 0x80)); advance(4); break;
    case 0xBB:
      if (byte_at(1) == 0)
        host_.give_item(operand_u(2));
      else
        host_.take_item(operand_u(2));
      advance(4);
      break;
    case 0xBC: {
      const s32 thr = static_cast<s32>(static_cast<u32>(byte_at(1)) | (static_cast<u32>(byte_at(2)) << 8) | (static_cast<u32>(byte_at(3)) << 16) |
                                       (static_cast<u32>(byte_at(4)) << 24));
      if (host_.money() < thr)
        pc = word_at(5);
      else
        advance(7);
      break;
    }
    case 0xBD: {
      const s64 cur = host_.money(), amt = operand_u(2);
      const s64 nv = byte_at(1) == 0 ? std::min<s64>(kMaxMoney, cur + amt) : std::max<s64>(0, cur - amt);
      host_.set_money(static_cast<s32>(nv));
      advance(4);
      break;
    }
    case 0xBE:  // CAMERA_AUTO_FOLLOW_WAIT: waits until the party stands (or 0xF0 ticks), then holds it
      if (gflags_ & gflag::kFormation) {
        yield_ = true;
        break;
      }
      if (host_.follow_wait()) {
        gflags_ |= gflag::kFormationHold;
        advance(1);
      }
      yield_ = true;
      break;
    case 0xBF:
    case 0xC1:
      if (byte_at(1) == 0) {
        if (!host_.system_ready()) {
          yield_ = true;
          break;
        }
        // An entity that resolves to none also skips the stop form behind the start form.
        if (const int target = entity_operand(2); target < 0) {
          advance(op == 0xBF ? 5 : 7);
          yield_ = true;
        } else if (op == 0xBF) {
          host_.extra_animation(0xBF, true, target, 0);
          advance(3);
        } else {
          host_.extra_animation(0xC1, true, target, operand_u(3));
          advance(5);
        }
      } else if (host_.extra_animation_busy()) {
        yield_ = true;
      } else {
        host_.extra_animation(op, false, -1, 0);
        advance(2);
      }
      break;
    case 0xC0:
      if (!host_.system_ready() || host_.extra_animation_busy()) {
        yield_ = true;
        break;
      }
      host_.extra_animation(0xC0, false, entity_operand(1), 0);
      advance(2);
      break;
    case 0xC2:
      host_.set_layer_opacity(operand_u(1), operand_u(3), operand_u(5), operand_u(7));
      advance(9);
      yield_ = true;
      break;
    case 0xC3: {
      const auto camera = host_.camera_position();
      write_var(word_at(1), camera.first);
      write_var(word_at(3), camera.second);
      advance(5);
      break;
    }
    case 0xC4: {
      const int target = entity_operand(1);
      if (target < 0 || (ents_[static_cast<size_t>(target)].flags38 & 0x4000) || !host_.animation_busy(target))
        advance(2);
      else
        yield_ = true;
      break;
    }
    case 0xC5: {
      const std::array<s32, 3> a = {operand_u(1), operand_u(3), operand_u(5)};
      host_.entity_effect(0xC5, ent_, a);
      advance(7);
      break;
    }
    case 0xC6:
      if (byte_at(1) == 0) {
        host_.play_sfx(0, operand_u(2), operand_u(4), 0, 0);
        advance(6);
      } else {
        host_.play_sfx(byte_at(1), operand_u(2), operand_u(4), operand_u(6), operand_u(8));
        advance(10);
      }
      break;
    case 0xC7: {
      host_.table_op(0xC7, operand_u(1), 0);
      advance(3);
      break;
    }
    case 0xC8: {
      const std::array<s32, 6> a = {operand_u(1), operand_s(3, 9, 0x40), operand_s(5, 9, 0x20), operand_s(7, 9, 0x10), operand_u(10), operand_u(12)};
      host_.entity_effect(0xC8, ent_, a);
      advance(14);
      break;
    }
    case 0xCA:
      // A one-byte op that reads the operands of the walk two bytes on (`CA CB 42|43 b X Z M ...`): the
      // target (X S@4, Z S@6, literal flags in the mode byte @8; b @3 == 2 = relative to the entity) becomes the entity's
      // facing target = bearing | 0x8000 (gated by the slot flag and the turn rate like the walks' own start forms) and flags270
      // gets bits 13 + 14, the fixed-time scripted turn (which clears bit 14 once it has taken it up). The next call, seeing
      // bit 14 still set, just goes on to the CB that waits for the yaw.
      if (!(self.flags270 & 0x4000)) {
        s32 x = operand_s(4, 8, 0x80), z = operand_s(6, 8, 0x40);
        if (byte_at(3) == 2) {
          const Vec3 position = host_.get_position(ent_);
          x += position.x;
          z += position.z;
        }
        host_.face_point(ent_, x, z);
        self.flags270 |= 0x6000;
      } else {
        advance(1);
      }
      yield_ = true;
      break;
    case 0xCB:  // the finishing pass advances AND yields (asm 1593): the next instruction runs next tick
      if (!host_.rotation_busy(ent_)) advance(1);
      yield_ = true;
      break;
    case 0xCC: {
      const int target = entity_operand(1);
      if (target < 0 || !host_.rotation_busy(target)) advance(2);
      yield_ = true;
      break;
    }
    case 0xCF: {
      const int target = entity_operand(1);
      if (target >= 0) ents_[static_cast<size_t>(target)].v24e = static_cast<u16>(operand_u(2));
      advance(4);
      break;
    }
    case 0xD0:
      if (host_.predicate(Predicate::BoxOutside, 0))
        advance(3);
      else
        pc = word_at(1);
      break;
    case 0xD1: {  // the field monster spawn
      const int ord = host_.load_model(ent_, operand_u(1));
      self.flags38 = (self.flags38 & ~0x4000u) | 0x200100;
      // Beaten in this room already: inactive and hidden.
      if (host_.monster_defeated(ord)) self.flags38 |= 0x200030;
      self.monster_ordinal = static_cast<u16>(ord);
      self.flags274 |= 0x800;
      self.flags270 |= 0x100000;
      advance(3);
      break;
    }
    case 0xD2:  // after a won battle (game state < 2) the entity goes inactive and its defeated bit is set
      budget_ += 0x40;
      if (host_.predicate(Predicate::GameStateLt2, 0)) {
        self.flags38 |= 0x30;
        host_.set_monster_defeated(static_cast<s16>(self.monster_ordinal));
        advance(4);
      } else {
        advance(1);
      }
      break;
    case 0xD4:  // PLAY_MOVIE_SKIPPABLE: table row in the low byte of the first operand, the movie flags in the second
      if (!host_.system_ready()) {
        yield_ = true;
        break;
      }
      host_.play_movie(operand_u(1) & 0xFF, operand_u(3));
      advance(5);
      break;
    case 0xD5: write_var(word_at(1), std::clamp(host_.item_count(operand_u(3)), 0, 255)); advance(5); break;
    case 0xD6:
      if (byte_at(1) == 0) {
        host_.attach_entity(ent_, -1, 0);
        self.flags38 &= ~0x800000u;
      } else if (const int target = entity_operand(1); target >= 0) {
        host_.attach_entity(ent_, target, operand_u(2));
        self.flags38 |= 0x800000;
      }
      advance(4);
      break;
    case 0xD8: {
      const int target = entity_operand(1);
      if (target >= 0 && host_.near_and_facing(ent_, target, operand_u(2), operand_u(4)))
        advance(8);
      else
        pc = word_at(6);
      break;
    }
    case 0xD9:
      if (!host_.system_ready()) {
        yield_ = true;
        break;
      }
      // Cheats "No encounters": the skip advances by 4 although the op is 5 / 6 bytes long.
      if (host_.skip_model_swap(ent_)) {
        advance(4);
        yield_ = true;
        break;
      }
      if (const int r = host_.set_character_model(operand_u(1), operand_u(3)); r > 0) {
        advance(5);
        yield_ = true;
      } else if (r == 0) {
        advance(6);
      } else {
        yield_ = true;
      }
      break;
    case 0xDA:
      if (host_.model_swap_busy()) {
        yield_ = true;
      } else {
        host_.model_swap_commit();
        advance(1);
      }
      break;
    case 0xDB: write_var(word_at(3), host_.character_model_byte(operand_u(1))); advance(5); break;
    case 0xDC:  // MAP_JUMP_DC: records the jump the movie behind it starts when it ends
      host_.set_movie_jump(operand_u(1), operand_u(3));
      advance(5);
      break;
    case 0xDD:
      if (byte_at(1) == 0) {
        if (!host_.system_ready()) {
          yield_ = true;
          break;
        }
        host_.table_op(0xDD, operand_u(2), 0);
        advance(4);
      } else if (host_.extra_animation_busy()) {
        yield_ = true;
      } else {
        advance(2);
      }
      break;
    case 0xE0:
      // Blocked ("-1"): retried by the dispatcher.
      if (host_.dialogue_blocked()) break;
      host_.open_dialogue(-1, word_at(1), byte_at(3));
      advance(4);
      break;
    case 0xE1:
      host_.dialogue_choice(byte_at(1) >> 4, byte_at(1) & 0xF);
      advance(2);
      yield_ = true;
      break;
    case 0xE2:
      if (host_.dialogue_open()) {
        yield_ = true;
        break;
      }
      // var 0x04 = the entity's last choice, which only a confirmed choice writes, so a plain message
      // leaves the entity's previous result.
      if (const int r = host_.dialogue_result(); r >= 0) self_->last_choice = static_cast<u8>(r);
      write_var(var::kChoiceResult, self_->last_choice);
      budget_ += 0x20;
      advance(1);
      yield_ = true;
      break;
    case 0xE3:
    case 0xE4: {
      const int speaker = entity_operand(1);
      // Speaker "none" skips the op and its trailing wait (E3: 5+E2, E4: 8).
      if (speaker < 0) {
        advance(op == 0xE4 ? 8 : 6);
        break;
      }
      if (host_.dialogue_blocked()) break;
      host_.open_dialogue(speaker, word_at(2), byte_at(4));
      advance(5);
      break;
    }
    case 0xE9:
      if (!((global(Global::WindowFrames) >> (byte_at(1) & 0x1F)) & 1))
        pc = word_at(3);
      else
        advance(5);
      break;
    default:
      return false;
  }
  return true;
}

}  // namespace oc::script
