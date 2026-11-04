// Extended opcode page (0xFE prefix): one handler per sub-opcode.
#include "script/field_vm.h"

#include <algorithm>
#include <array>

#include "core/log.h"

namespace oc::script {

void FieldVM::exec_ext(u8 sub) {
  Entity& self = *self_;
  const u16 key = static_cast<u16>(kExtPage | sub);
  if (const OpInfo* info = find_op(key); info && info->conf == '?') ++stats_.uncertain_ops[key];
  // `base_` is the sub-opcode byte; advance(n) = n bytes from it (instruction length - 1).
  // Yield always, rewind while busy.
  auto idle_gate = [&]() {
    yield_ = true;
    return host_.system_ready();
  };
  // The screen ops open their screen through the field's launcher and step on; the FE 61 after each waits until it has closed.
  auto open_screen = [&](MenuKind kind, int arg, int length) {
    if (!idle_gate()) return;
    host_.request_menu(kind, arg);
    advance(length);
  };
  switch (sub) {
    case 0x00:
      if (byte_at(1) == 0)
        self.flags270 |= 0x20000100;
      else
        gflags_ |= 0x80;
      advance(2);
      break;
    case 0x01: set_flag(self.flags270, 9, byte_at(1) & 1); advance(2); break;
    case 0x02: set_global(Global::ExitTransition, operand_u(1)); advance(3); break;
    case 0x03:
      if (!(self.flags270 & 0x800)) {
        self.flags270 |= 0x800;
        const std::array<s32, 2> a = {byte_at(1), operand_s(2, 4, 0x80)};
        host_.entity_effect(0xFE03, ent_, a);
      }
      advance(5);
      break;
    case 0x04: set_global_bit(8, byte_at(1) & 1); advance(2); break;
    case 0x05: set_global(Global::FrameVsyncs, operand_u(1)); advance(3); break;
    case 0x06: set_flag(self.flags38, 13, byte_at(1) == 0); advance(2); break;
    case 0x07: turn_flag_13(self, byte_at(1) & 1); advance(2); break;
    case 0x08:
    case 0x09:
    case 0x0A: {
      const int target = entity_operand(1);
      const u32 idx = static_cast<u32>(operand_u(2)) & 0xFF;
      if (target >= 0) ent_ref_tables_[sub - 8][idx] = static_cast<u8>(target);
      advance(4);
      break;
    }
    case 0x0B: opaque(0xFE0B, 1, 1); advance(2); break;
    case 0x0C: {
      const int target = entity_operand(2);
      if (target >= 0) turn_flag_13(ents_[static_cast<size_t>(target)], byte_at(1) & 1);
      advance(3);
      break;
    }
    case 0x0D:
      for (auto& entity : ents_) entity.scale[0] = entity.scale[1] = entity.scale[2] = static_cast<u32>(operand_u(1));
      advance(3);
      break;
    case 0x0E: self.v10c = static_cast<u16>(operand_u(1)); advance(3); break;
    case 0x0F: {
      const int target = entity_operand(1);
      if (target >= 0) ents_[static_cast<size_t>(target)].v10c = static_cast<u16>(operand_u(2));
      advance(4);
      break;
    }
    case 0x10:
      budget_ += 0x40;
      if (host_.battle_active()) {
        yield_ = true;
      } else {
        self.flags270 &= ~flag270::kInputLock;
        advance(1);
      }
      break;
    case 0x11: set_global_bit(9, operand_u(1) & 1); advance(3); break;
    case 0x12: set_global(Global::OverlayGridA0, operand_u(1)); set_global(Global::OverlayGridA1, operand_u(3)); set_global(Global::OverlayGridA2, operand_u(5)); advance(7); break;
    case 0x13: set_global(Global::OverlayGridB0, operand_u(1)); set_global(Global::OverlayGridB1, operand_u(3)); set_global(Global::OverlayGridB2, operand_u(5)); advance(7); break;
    case 0x14: set_global_bit(10, byte_at(1) & 1); advance(2); break;
    case 0x15: set_global(Global::OverlayOt, operand_u(1)); advance(3); break;
    // 16: the handler is a bare return: no advance, the script stalls for good.
    case 0x16: park(); break;
    case 0x17: set_global_bit(11, byte_at(1) & 1); advance(2); break;
    case 0x18:
    case 0x7F: set_global_bit(12, byte_at(1) & 1); advance(2); break;
    case 0x19: set_flag(self.flags270, 19, byte_at(1) & 1); advance(2); break;
    case 0x1A: opaque(0xFE1A, 0, 0); advance(1); break;
    case 0x1B:
      if (!host_.system_ready()) {
        yield_ = true;
        break;
      }
      write_var(var::kCurrentFrame, operand_u(1));
      host_.table_op(0xFE1B, operand_u(1), 0);
      advance(3);
      break;
    case 0x1C:
      if (!host_.system_ready())
        yield_ = true;
      else
        advance(1);
      break;
    case 0x1D: host_.fade_master_volume(operand_u(1), operand_u(3) & 0x7F); advance(5); break;
    case 0x1E: opaque(0xFE1E, 1, 4); advance(5); break;
    case 0x1F: opaque(0xFE1F, 1, 4); advance(5); break;
    case 0x20: opaque(0xFE20, 1, 4); advance(5); break;
    case 0x21: host_.shake(operand_u(1), operand_u(3), operand_u(5)); advance(7); break;
    case 0x22: host_.sfx_volume(operand_u(1), operand_u(3), operand_u(5)); advance(7); break;
    // FE 23: pan slide of the slot's sounds.
    case 0x23: host_.sfx_pan(operand_u(1), operand_u(3), operand_u(5)); advance(7); break;
    case 0x24: opaque(0xFE24, 1, 6); advance(7); break;
    case 0x25: set_flag(self.flags270, 21, byte_at(1) & 1); advance(2); break;
    case 0x26: opaque(0xFE26, 1, 1); advance(2); break;
    case 0x27: opaque(0xFE27, 0, 0); advance(1); break;
    case 0x28: write_var(word_at(1), host_.query(Query::GlobalDiff)); advance(3); break;
    case 0x29: {
      const u32 bit = static_cast<u32>(((operand_u(1) & 7) + 8));
      set_global(Global::KeepSfxSlots, global(Global::KeepSfxSlots) | (1 << bit));
      advance(3);
      break;
    }
    case 0x2A: set_flag(self.flags270, 22, byte_at(1) & 1); advance(2); break;
    case 0x2B: {
      const u8 which = byte_at(1);
      if (which >= 0x10 && which <= 0x12) {
        const Global target = which == 0x10 ? Global::FootstepVolume : which == 0x11 ? Global::FootstepDryType : Global::FootstepWaterType;
        set_global(target, static_cast<u16>(operand_u(2)));
      }
      advance(4);
      break;
    }
    case 0x2C: set_flag(self.flags270, 23, byte_at(1) & 1); advance(2); break;
    case 0x2D: self.v114 = static_cast<u16>(operand_u(1)); self.flags270 |= 0x2000000; advance(3); break;
    case 0x2E: self.flags270 &= ~0x2000000u; advance(1); break;
    case 0x2F:
      std::fill(vars_.begin(), vars_.begin() + 0x1E8, u16{0});
      std::fill(vars_.begin() + kGlobalVarSlots, vars_.end(), u16{0});
      // MAP_ID = 8 is written before the clear, so variable 6 ends 0.
      host_.field_reset();
      room_reg_ = 8;
      advance(1);
      break;
    case 0x30: {  // head turn: target (x S@1, y S@5, z S@3) of the executing entity
      if (!(self.flags38 & flag38::kNo3dObject))
        host_.head_op(HeadOp::Point, ent_, -1, operand_s(1, 7, 0x80), operand_s(5, 7, 0x20), operand_s(3, 7, 0x40), self.head_turn_rate);
      advance(8);
      break;
    }
    case 0x31: {  // the executing entity looks at attach point 1 of entity c
      const int target = entity_operand(1);
      if (target >= 0 && !(self.flags38 & flag38::kNo3dObject) && !(ents_[static_cast<size_t>(target)].flags38 & flag38::kNo3dObject))
        host_.head_op(HeadOp::LookAtHead, ent_, target, 0, 0, 0, self.head_turn_rate);
      advance(2);
      break;
    }
    case 0x32:  // head-turn reset on the executing entity
      if (!(self.flags38 & flag38::kNo3dObject)) host_.head_op(HeadOp::Reset, ent_, -1, 0, 0, 0, self.head_turn_rate);
      advance(1);
      break;
    case 0x33: set_flag(self.flags270, 26, byte_at(1) & 1); advance(2); break;
    case 0x34:
      set_global(Global::FootstepListenerX, operand_s(1, 11, 0x80));
      set_global(Global::FootstepListenerZ, operand_s(3, 11, 0x40));
      set_global(Global::FootstepListenerY, operand_s(5, 11, 0x20));
      set_global(Global::FootstepNear, operand_s(7, 11, 0x10));
      set_global(Global::FootstepFar, operand_s(9, 11, 0x08));
      advance(12);
      break;
    case 0x35:  // facing target, y = S@1
      if (!(self.flags38 & flag38::kNo3dObject)) host_.head_op(HeadOp::Facing, ent_, -1, 0, operand_s(1, 3, 0x80), 0, self.head_turn_rate);
      advance(4);
      break;
    case 0x36: host_.heal_group(operand_u(1)); advance(3); break;
    case 0x37: write_var(word_at(1), host_.char_life(operand_u(3))); advance(5); break;
    case 0x38:
    case 0x39:
    case 0x3A:
      host_.change_char_life(sub == 0x38 ? LifeOp::SetClamped : sub == 0x39 ? LifeOp::AddCapped : LifeOp::Damage, operand_u(3),
                             operand_s(1, 5, 0x80));
      advance(6);
      break;
    case 0x3B: write_var(word_at(1), host_.char_status(operand_u(3)) & 0xFF); advance(5); break;
    case 0x3C: self.head_turn_rate = static_cast<u16>(operand_u(1)); advance(3); break;
    // FE 3D: inflict (OR).
    case 0x3D: host_.change_status(operand_u(1), operand_u(3), true); advance(5); break;
    // FE 3E: cure (AND NOT, low byte).
    case 0x3E: host_.change_status(operand_u(1), operand_u(3), false); advance(5); break;
    case 0x3F: opaque(0xFE3F, 0, 0); advance(1); break;
    case 0x40:
      if (!(self.flags38 & 0x400000)) {
        self.v116 = static_cast<u16>(operand_u(1));
        self.flags270 |= 0x1000000;
      }
      advance(3);
      break;
    case 0x41:
      for (int i = 0; i < 3; ++i) {
        const int party_ent = host_.party_entity(i);
        if (party_ent >= 0 && party_ent < static_cast<int>(ents_.size())) set_flag(ents_[static_cast<size_t>(party_ent)].flags270, 24, byte_at(1) & 1);
      }
      advance(2);
      break;
    case 0x42: set_global_bit(14, byte_at(1) & 1); advance(2); break;
    case 0x43: {
      const std::array<s32, 2> args = {operand_u(1), operand_u(3)};
      host_.entity_effect(0xFE43, ent_, args);
      advance(5);
      break;
    }
    case 0x44: host_.draw_sync(); advance(1); break;
    case 0x45: {
      const int target = entity_operand(2);
      const bool short_form = byte_at(1) == 0;
      if (target >= 0) {
        Entity& entity = ents_[static_cast<size_t>(target)];
        // Operands {flag, U@3}; U@3 only in the long form (-1 = none).
        if (!(entity.flags38 & flag38::kNo3dObject)) {
          const std::array<s32, 2> args = {byte_at(1), short_form ? -1 : operand_u(3)};
          host_.entity_effect(0xFE45, target, args);
        }
        set_flag(entity.flags270, 27, byte_at(1) & 1);
      }
      advance(short_form ? 3 : 5);
      break;
    }
    case 0x46:
    case 0x47: {
      const std::array<s32, 3> a = {operand_s(1, 7, 0x80), operand_s(3, 7, 0x40), operand_s(5, 7, 0x20)};
      host_.entity_effect(key, ent_, a);
      advance(8);
      break;
    }
    case 0x48: {
      // World position of attach point U@2 of the model of entity c; var@4 = x, var@6 = z, var@8 = y (the script's
      // x z y order). No entity: all three 0. (An entity without a model is not guarded.)
      const int target = entity_operand(1);
      Vec3 position;
      if (target < 0 || !host_.attach_point(target, operand_u(2), position)) position = {};
      write_var(word_at(4), position.x);
      write_var(word_at(6), position.z);
      write_var(word_at(8), position.y);
      advance(10);
      break;
    }
    case 0x49:  // facing target with yaw offset S@3, y = S@1
      if (!(self.flags38 & flag38::kNo3dObject)) host_.head_op(HeadOp::Facing, ent_, -1, operand_s(3, 5, 0x40), operand_s(1, 5, 0x80), 0, self.head_turn_rate);
      advance(6);
      break;
    case 0x4A: open_screen(MenuKind::NameEntry, operand_u(1), 3); break;
    case 0x4B: set_flag(self.flags38, 20, byte_at(1) != 0); advance(2); break;
    case 0x4C: {
      const int target = entity_operand(1);
      if (target >= 0) {
        for (auto& entity : ents_) entity.flags38 &= ~flag38::kSelected;
        ents_[static_cast<size_t>(target)].flags38 |= flag38::kSelected;
        host_.set_controlled_entity(target);
      }
      advance(2);
      break;
    }
    case 0x4D: set_global(Global::Byte4D, byte_at(1)); advance(2); break;
    case 0x4E: set_global_bit(15, byte_at(1) & 1); advance(2); break;
    case 0x4F: {
      const int target = entity_operand(1);
      if (target >= 0) slot_ent_[static_cast<size_t>(operand_u(2)) & 7] = static_cast<u8>(target);
      advance(4);
      break;
    }
    case 0x50:
      host_.set_camera_origin(read_var(0x330), read_var(0x332));
      world_continue_ = true;
      advance(1);
      break;
    case 0x51: opaque(0xFE51, 1, 2); advance(3); break;
    case 0x52: host_.swap_leader(operand_u(1)); advance(3); break;
    case 0x53: {  // entity c@6, yaw offset S@3, y = S@1
      const int target = entity_operand(6);
      if (target >= 0 && !(ents_[static_cast<size_t>(target)].flags38 & flag38::kNo3dObject))
        host_.head_op(HeadOp::Facing, target, -1, operand_s(3, 5, 0x40), operand_s(1, 5, 0x80), 0, ents_[static_cast<size_t>(target)].head_turn_rate);
      advance(7);
      break;
    }
    case 0x54: {  // entity c@8, target (x S@1, y S@5, z S@3); the rate is the executing entity's head-turn rate
      const int target = entity_operand(8);
      if (target >= 0 && !(ents_[static_cast<size_t>(target)].flags38 & flag38::kNo3dObject))
        host_.head_op(HeadOp::Point, target, -1, operand_s(1, 7, 0x80), operand_s(5, 7, 0x20), operand_s(3, 7, 0x40), self.head_turn_rate);
      advance(9);
      break;
    }
    case 0x55: {  // entity c@2 looks at attach point 1 of entity c@1
      const int looked_at = entity_operand(1), looker = entity_operand(2);
      if (looked_at >= 0 && looker >= 0 && !(ents_[static_cast<size_t>(looked_at)].flags38 & flag38::kNo3dObject) &&
          !(ents_[static_cast<size_t>(looker)].flags38 & flag38::kNo3dObject))
        host_.head_op(HeadOp::LookAtHead, looker, looked_at, 0, 0, 0, ents_[static_cast<size_t>(looker)].head_turn_rate);
      advance(3);
      break;
    }
    case 0x56: {  // head-turn reset on entity c
      const int target = entity_operand(1);
      if (target >= 0 && !(ents_[static_cast<size_t>(target)].flags38 & flag38::kNo3dObject))
        host_.head_op(HeadOp::Reset, target, -1, 0, 0, 0, ents_[static_cast<size_t>(target)].head_turn_rate);
      advance(2);
      break;
    }
    case 0x57: {  // entity c@4, facing target, y = S@1
      const int target = entity_operand(4);
      if (target >= 0 && !(ents_[static_cast<size_t>(target)].flags38 & flag38::kNo3dObject))
        host_.head_op(HeadOp::Facing, target, -1, 0, operand_s(1, 3, 0x80), 0, ents_[static_cast<size_t>(target)].head_turn_rate);
      advance(5);
      break;
    }
    case 0x58:
      if (byte_at(1) == 0) {
        self.flags270 &= ~0x40000000u;
      } else {
        self.flags270 |= 0x40000000;
        if (byte_at(1) >= 2) self.flags274 = (self.flags274 & ~0xFFu) | byte_at(1);
      }
      advance(2);
      break;
    case 0x59:
      if (idle_gate()) {
        host_.table_op(0xFE59, 3, 0);
        set_global(Global::MenuPreamble3, 1);
        advance(1);
      }
      break;
    case 0x5A: self.flags270 = (self.flags270 & 0x7FFFFFFFu) | (static_cast<u32>(byte_at(1) & 1) << 31); advance(2); break;
    case 0x5B: open_screen(MenuKind::PartyScreen, 0, 1); break;
    case 0x5C: {
      const int target = entity_operand(2);
      if (target >= 0) {
        Entity& entity = ents_[static_cast<size_t>(target)];
        const s32 value = operand_u(3);
        entity.array25a[byte_at(1) & 7] = static_cast<u16>(value);
        // The footstep bit goes to the EXECUTING entity.
        set_flag(self.flags270, 24, value != 0xFF);
      }
      advance(5);
      break;
    }
    case 0x5D: host_.table_op(0xFE5D, byte_at(1), operand_u(2)); advance(4); break;
    case 0x5E: open_screen(MenuKind::ElementShop, operand_u(1), 3); break;
    case 0x5F: set_flag(self.flags274, 9, byte_at(1) & 1); advance(2); break;
    case 0x60: host_.table_op(0xFE60, byte_at(1), operand_u(2)); advance(4); break;
    // FE 61 opens nothing: it waits in place while a screen opened by the op before it is pending or open.
    case 0x61:
      if (host_.menu_active()) yield_ = true;
      else advance(1);
      break;
    case 0x62: {  // entity c head-turn rate (used by the next head op) = U
      const int target = entity_operand(1);
      if (target >= 0) ents_[static_cast<size_t>(target)].head_turn_rate = static_cast<u16>(operand_u(2));
      advance(4);
      break;
    }
    case 0x63:
      if (!host_.system_ready()) {
        yield_ = true;
        break;
      }
      set_global(Global::MusicSwapArmed, 1);
      yield_ = true;
      advance(1);
      break;
    case 0x64: {
      const int target = entity_operand(2);
      if (target >= 0) set_flag(ents_[static_cast<size_t>(target)].flags274, 10, byte_at(1) & 1);
      advance(3);
      break;
    }
    case 0x65:
      if (byte_at(1) == 0) {
        if (menu_request(MenuKind::ChangeDisc, operand_u(2))) advance(4);
      } else if (host_.menu_active()) {
        yield_ = true;
      } else {
        advance(2);
      }
      break;
    case 0x66: opaque(0xFE66, 0, 0); advance(1); break;
    case 0x67:
      if (idle_gate()) {
        set_global(Global::TradingPostRow, operand_u(1));
        set_global(Global::TradingPostCue, 1);
        host_.table_op(0xFE67, 0xBD, 0);
        host_.request_menu(MenuKind::TradingPost, operand_u(1));
        advance(3);
      }
      break;
    case 0x68: open_screen(MenuKind::WeaponShop, operand_u(1), 3); break;
    case 0x69: host_.table_op(0xFE69, byte_at(1), 0); advance(2); break;
    case 0x6A: set_global(Global::Reg6A, operand_u(1)); advance(3); break;
    case 0x6B: set_global_bit(16, byte_at(1) & 1); advance(2); break;
    case 0x6C: set_global_bit(17, byte_at(1) & 1); advance(2); break;
    case 0x6D: opaque(0xFE6D, 0, 0); advance(1); break;
    case 0x6E: write_var(word_at(1), host_.query(Query::CurrentSong)); advance(3); break;
    case 0x6F: host_.table_op(0xFE6F, byte_at(1), 0); advance(2); break;
    case 0x70: {
      const int target = entity_operand(1);
      if (target >= 0) set_global(Global::BattleIntroAnchor, target);
      advance(2);
      break;
    }
    case 0x71: set_global_bit(18, byte_at(1) & 1); advance(2); break;
    case 0x72:
      if (idle_gate()) {
        set_global(Global::SavePointPreamble, 1);
        host_.table_op(0xFE72, 3, 0);
        host_.request_menu(MenuKind::MainMenu, 0);
        advance(1);
      }
      break;
    case 0x73: host_.learn_limit(2, operand_u(1), operand_u(3)); advance(5); break;
    case 0x74: host_.learn_limit(1, operand_u(1), operand_u(3)); advance(5); break;
    case 0x79: host_.learn_limit(0, operand_u(1), operand_u(3)); advance(5); break;
    case 0x75:
      if (!host_.system_ready()) {
        yield_ = true;
        break;
      }
      set_global(Global::Selection, operand_u(1) & 0xFF);
      set_global(Global::SelectionFlag, 1);
      yield_ = true;
      advance(3);
      break;
    case 0x76: write_var(word_at(1), host_.query(Query::Field56)); advance(3); break;
    case 0x77: set_global(Global::PadMask, word_at(1)); advance(3); break;
    case 0x78:
      if (!host_.system_ready() || !host_.play_song_ex(operand_u(1), operand_u(3))) {
        yield_ = true;
        break;
      }
      advance(5);
      break;
    case 0x7A: {
      const s32 v = operand_u(1);
      if (v >= 0 && v < 16) set_global(Global::WindowFrames, global(Global::WindowFrames) | (1 << v));
      advance(3);
      break;
    }
    case 0x7B: host_.set_limit_mode(operand_u(1)); advance(3); break;
    case 0x7C: host_.table_op(0xFE7C, operand_u(1), operand_u(3)); advance(5); break;
    case 0x7D: host_.table_op(0xFE7D, operand_u(1), 0); advance(3); break;
    case 0x7E: opaque(0xFE7E, 0, 0); advance(1); break;
    case 0x80: set_global_bit(13, byte_at(1) & 1); advance(2); break;
    case 0x81: set_global_bit(19, byte_at(1) & 1); advance(2); break;
    case 0x82: write_var(word_at(1), host_.query(Query::StarsTotal)); advance(3); break;
    case 0x83:
      vars_[0x3D2 >> 1] &= static_cast<u16>(~0x20u);
      opaque(0xFE83, 0, 0);
      advance(1);
      break;
    // FE 84 / FE 85: the launcher mode 0x30 New Game+ title list with the cleared game kept in the playinit buffer / 4 save list after
    // the gear is stripped and variable 0x7E = 3. Both are used once, in the ending room.
    case 0x84: open_screen(MenuKind::NewGamePlus, 0, 1); break;
    case 0x85: open_screen(MenuKind::Save, 0, 1); break;
    default: {
      const OpInfo* info = find_op(key);
      if (!info) {
        note_unknown(key);
        // Unused FE entries are "pc += 1" no-ops behind the dispatcher's own +1.
        advance(1);
      } else {
        ++stats_.unimplemented;
        if (logged_.insert(key).second) log::warn("field script: opcode FE {:02X} ({}) has no handler", sub, info->name);
        advance(1);
      }
      break;
    }
  }
}

}  // namespace oc::script
