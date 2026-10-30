// Main opcode page, part 2: dialog window, globals, screen color, scenario, arrays and bits, party, movie, background
// layers, formation, camera follow, song, roster jumps and dialog face.
#include "script/field_vm.h"

#include <array>

namespace oc::script {

bool FieldVM::exec_scene(u8 op) {
  u32& pc = rec_->pc;
  Entity& self = *self_;
  switch (op) {
    case 0x62: {  // SETUP_DIALOG_WINDOW: x, y, w, h, flags
      const std::array<s32, 5> args = {operand_u(1), operand_u(3), operand_u(5), operand_u(7), operand_u(9)};
      host_.setup_dialog_window(ent_, args);
      advance(11);
      break;
    }
    case 0x63:  // SET_CONTROL_YAW
      set_global(Global::ControlYaw, operand_u(1));
      advance(3);
      break;
    case 0x64:  // SET_CAMERA_Y_OFFSET
      set_global(Global::CameraYOffset, operand_s(1, 3, 0x80));
      advance(4);
      break;
    case 0x65:  // STORE_GLOBAL_TO_VAR
      write_var(word_at(1), host_.query(Query::ScriptGlobal));
      advance(3);
      break;
    case 0x66:  // SET_WATER_LEVEL: water level of slot b
      table_9fdc_[byte_at(1)] = static_cast<s16>(operand_s(2, 4, 0x80));
      advance(5);
      break;
    case 0x67: {  // SET_SHADOW_COLOR: 16 RGBX entries at code offset w
      std::array<u8, 64> colors{};
      const u32 base = word_at(1);
      for (u32 i = 0; i < 64; ++i) colors[i] = mod_.code_byte(base + i);
      host_.set_shadow_colors(colors);
      advance(3);
      break;
    }
    case 0x68:  // SET_SCREEN_FOG
      host_.set_screen_rgb(operand_u(1), operand_u(3), operand_u(5), operand_u(7), operand_u(9));
      advance(11);
      break;
    case 0x69:  // JUMP_IF_SCENARIO_GE
    case 0x6A:  // JUMP_IF_SCENARIO_LE
    case 0x6B:  // JUMP_IF_SCENARIO_NE
    case 0xCD:  // JUMP_IF_SCENARIO_GT
    case 0xCE: {  // JUMP_IF_SCENARIO_LT
      const s32 scenario = read_var(var::kScenario), value = operand_u(1);
      bool jump = false;
      switch (op) {
        case 0x69: jump = !(scenario < value); break;
        case 0x6A: jump = !(value < scenario); break;
        case 0x6B: jump = scenario != value; break;
        case 0xCD: jump = value < scenario; break;
        default: jump = scenario < value; break;
      }
      jump_or_advance(jump, 3, 5);
      break;
    }
    case 0x6C:  // SET_SCENARIO
      write_var(var::kScenario, operand_u(1));
      budget_ += 0x20;
      advance(3);
      break;
    case 0x6D:  // GET_SCENARIO
      write_var(word_at(1), read_var(var::kScenario));
      advance(3);
      break;
    case 0x6E: {  // SWAP
      const s32 first = read_var(word_at(1)), second = read_var(word_at(3));
      write_var(word_at(1), second);
      write_var(word_at(3), first);
      advance(5);
      break;
    }
    case 0x6F:  // ARRAY8_GET: byte at code offset w + U
      write_var(word_at(3), mod_.code_byte((word_at(1) + static_cast<u32>(operand_u(5))) & 0xFFFF));
      advance(7);
      break;
    case 0x70: {  // ARRAY16_GET: word at code offset w + U, signed when b != 0
      const u32 addr = (word_at(1) + static_cast<u32>(operand_u(5))) & 0xFFFF;
      const u16 value = static_cast<u16>(mod_.code_byte(addr) | (mod_.code_byte(addr + 1) << 8));
      write_var(word_at(3), byte_at(7) == 0 ? static_cast<s32>(value) : static_cast<s32>(static_cast<s16>(value)));
      advance(8);
      break;
    }
    case 0x71:  // SET_VAR_BIT: operand = (id << 4) | bit
    case 0x72: {  // CLEAR_VAR_BIT
      const u32 bit_ref = static_cast<u32>(operand_s(1, 3, 0x80));
      u16& slot = vars_[(bit_ref >> 5) & (kVarSlots - 1)];
      if (op == 0x71) {
        slot = static_cast<u16>(slot | (1u << (bit_ref & 0xF)));
        host_.var_bit_set(bit_ref);
      } else
        slot = static_cast<u16>(slot & ~(1u << (bit_ref & 0xF)));
      advance(4);
      break;
    }
    case 0x73:  // JUMP_IF_BIT_CLEAR
      jump_or_advance(!var_bit(static_cast<u32>(operand_s(1, 3, 0x80))), 4, 6);
      break;
    case 0x74: {  // SET_TARGET_10E
      const int target = entity_operand(4);
      if (target >= 0) ents_[static_cast<size_t>(target)].v10e = static_cast<u16>(operand_s(1, 3, 0x80));
      advance(5);
      break;
    }
    case 0x75:  // SETUP_FLAG: byte lane b2 of flag-table word b1 = U
    case 0x76:  // FLAG_BYTE_OR
    case 0x77:  // FLAG_BYTE_GET
    case 0x78: {  // FLAG_BYTE_AND
      const u32 word = byte_at(1), lane = byte_at(2);
      if (lane <= 3) {
        u32& entry = flag_table_[word];
        const u32 shift = lane * 8;
        const u32 cur = (entry >> shift) & 0xFF;
        if (op == 0x77) {
          write_var(word_at(3), static_cast<s32>(cur));
        } else {
          const u32 value = static_cast<u32>(operand_u(3));
          const u32 lane_value = op == 0x75 ? value : op == 0x76 ? cur | value : cur & value;
          entry = (entry & ~(0xFFu << shift)) | (lane_value << shift);
          ++flag_writes_;
        }
      }
      advance(5);
      break;
    }
    case 0x79:  // JUMP_UNLESS_NEAR: jumps unless entity c is within U of this entity
    case 0x7A: {  // JUMP_IF_TILE_ATTR_NE
      const int target = entity_operand(1);
      if (target < 0) {
        pc = word_at(4);
        break;
      }
      if (op == 0x79) {
        const Vec3 self_pos = host_.get_position(ent_), target_pos = host_.get_position(target);
        const bool near = isqrt_dist(self_pos.x - target_pos.x, self_pos.y - target_pos.y, self_pos.z - target_pos.z) < operand_u(2);
        jump_or_advance(!near, 4, 6);
      } else {
        jump_or_advance(host_.entity_state(target) != operand_u(2), 4, 6);
      }
      break;
    }
    case 0x7B:  // ADD_TO_PARTY: a refused add also skips the 7C that follows
      advance(host_.party_add(operand_u(1)) ? 3 : 4);
      break;
    case 0x7C:  // COMMIT_PARTY_MODEL: refreshes the party models once no model swap is pending
      if (host_.model_swap_busy()) {
        yield_ = true;
      } else {
        host_.refresh_party_models();
        advance(1);
      }
      break;
    case 0x7D:  // REMOVE_FROM_PARTY
      host_.party_remove(byte_at(1));
      advance(2);
      break;
    case 0x7E:  // PLAY_MOVIE: index in the low byte, variant in the high byte
      if (!system_gate()) break;
      host_.play_movie(operand_u(1) & 0xFF, (operand_u(1) >> 8) & 0xFF, false);
      advance(3);
      break;
    case 0x7F:  // WAIT_MOVIE
      wait_or_advance(host_.movie_playing(), 1);
      break;
    case 0x80:  // BG_GROUP_ON
      host_.bg_layer(operand_u(1), true);
      advance(3);
      break;
    case 0x81:  // BG_GROUP_OFF
      host_.bg_layer(operand_u(1), false);
      advance(3);
      break;
    case 0x82: {  // AXIS_SCALE: axes selected by the bits of b
      const int target = entity_operand(2);
      if (target >= 0) {
        Entity& entity = ents_[static_cast<size_t>(target)];
        for (int i = 0; i < 3; ++i)
          if (byte_at(1) & (1 << i)) entity.scale[i] = static_cast<u32>(operand_u(3));
      }
      advance(5);
      break;
    }
    case 0x83:  // SET_CAMERA_LIMITS: camera limits
      host_.camera_limits(operand_s(1, 9, 0x80), operand_s(3, 9, 0x40), operand_s(5, 9, 0x20), operand_s(7, 9, 0x10));
      advance(10);
      break;
    case 0x84:  // SETUP_FORMATION: waits while a formation is running, then starts one and yields
    case 0xDF: {  // SETUP_FORMATION_ANGLE
      if (gflags_ & gflag::kFormation) {
        yield_ = true;
        break;
      }
      gflags_ |= 0x18;
      if (op == 0x84) {
        const std::array<s32, 12> args = {operand_s(1, 13, 0x80), operand_s(3, 13, 0x40), operand_s(5, 13, 0x20), operand_s(7, 13, 0x10),
                                          operand_s(9, 13, 0x08), operand_s(11, 13, 0x04), operand_u(14),         operand_u(16),
                                          operand_u(18),          byte_at(20),           byte_at(21),         byte_at(22)};
        host_.formation(0x84, args);
        advance(23);
      } else {
        const std::array<s32, 3> args = {operand_s(1, 5, 0x80), operand_s(3, 5, 0x40), word_at(6)};
        host_.formation(0xDF, args);
        advance(9);
      }
      yield_ = true;
      break;
    }
    case 0x85:  // GO_TO_FORMATION: waits for the formation to finish
      if (host_.formation_done()) {
        gflags_ &= ~gflag::kFormation;
        advance(1);
      }
      yield_ = true;
      break;
    case 0x86:  // CAMERA_AUTO_FOLLOW: b == 1 waits for the formation then switches it off, else switches it on
      if (byte_at(1) == 1) {
        if (host_.formation_done()) {
          host_.camera_auto_follow(false);
          advance(2);
        }
      } else {
        host_.camera_auto_follow(true);
        advance(2);
      }
      yield_ = true;
      break;
    case 0x87:  // SET_UNDERWATER_RGB
      host_.set_underwater_rgb(operand_u(1), operand_u(3), operand_u(5));
      advance(7);
      break;
    case 0x88:  // PLAY_SONG: retried while the system is busy or the host refuses
      if (!system_gate() || !host_.play_song(operand_u(1))) {
        yield_ = true;
        break;
      }
      budget_ += 1;
      advance(3);
      break;
    case 0x89:  // PLAY_SONG_POLL: plays the song like 88
      if (!system_gate() || !host_.play_song(operand_u(1))) {
        yield_ = true;
        break;
      }
      advance(5);
      break;
    case 0x8A:  // WAIT_SONG
      wait_or_advance(host_.song_changing(), 1);
      break;
    case 0x8B:  // SET_RADIUS_HEIGHT: trigger radius and height
      self.trigger_radius = static_cast<u16>(operand_u(1));
      self.trigger_height = static_cast<u16>(operand_u(3));
      advance(5);
      break;
    case 0x8C:  // RECT_SIZE
      self.rect_w = static_cast<u16>(operand_u(1));
      self.rect_h = static_cast<u16>(operand_u(3));
      advance(5);
      break;
    case 0x8D:  // SET_BOX_FLAGS
      switch (byte_at(1)) {
        case 0: self.flags38 |= 0x200; break;
        case 1: self.flags38 &= ~0x200u; break;
        case 2: self.flags38 |= 0x1200; break;
        case 3: self.flags38 &= ~0x1200u; break;
        default: break;
      }
      advance(2);
      break;
    case 0x8E:  // SET_SOLID
      set_flag(self.flags38, 8, byte_at(1) == 0);
      advance(2);
      break;
    case 0x8F: {  // BG_WAVE_ON: the 8 U operands
      const std::array<s32, 8> args = {operand_u(1), operand_u(3), operand_u(5), operand_u(7), operand_u(9), operand_u(11), operand_u(13), operand_u(15)};
      host_.bg_wave_start(args);
      advance(17);
      break;
    }
    case 0x90: {  // BG_WAVE_OFF: modes 0..3 advance when the host says so (mode 0 carries a U operand), 4.. never
      const bool done = host_.bg_wave(byte_at(1), operand_u(2));
      if (byte_at(1) < 4 && done) advance(byte_at(1) == 0 ? 4 : 2);
      yield_ = true;
      break;
    }
    case 0x91:  // SET_GF5
      set_global_bit(5, byte_at(1) & 1);
      advance(2);
      break;
    case 0x92: {  // BG_GROUP_SEMITRANS
      const std::array<s32, 2> args = {byte_at(1), operand_u(2)};
      host_.entity_effect(0x92, ent_, args);
      advance(4);
      break;
    }
    case 0x93:  // SET_NO_SHADOW
      set_flag(self.flags38, 10, byte_at(1) != 0);
      advance(2);
      break;
    case 0x94:  // JUMP_IF_NOT_IN_PARTY
      jump_or_advance(!host_.in_party(operand_u(1)), 3, 5);
      break;
    case 0x95:  // JUMP_IF_NOT_RECRUITED
    case 0xE5:  // JUMP_IF_NOT_IN_CARRYOVER
    case 0xE6:  // JUMP_IF_NOT_RECRUITED_OR_CARRYOVER
    case 0xE8: {  // TEST_CLEAR_CARRYOVER_JUMP
      const Predicate pred = op == 0x95 ? Predicate::Op95 : op == 0xE5 ? Predicate::OpE5 : op == 0xE6 ? Predicate::OpE6 : Predicate::OpE8;
      jump_or_advance(host_.predicate(pred, operand_u(1)), 3, 5);
      break;
    }
    case 0x96:  // ADD_TO_ROSTER
      host_.roster_add(operand_u(1));
      advance(3);
      break;
    case 0x97:  // REMOVE_FROM_ROSTER
      host_.roster_remove(operand_u(1));
      advance(3);
      break;
    // 98 / 99 / 9A read the party character table, whose empty slots hold 0xFF; 99 writes that raw word (scripts test
    // the variable against #255 to detect an empty slot).
    case 0x98: {  // FIND_PARTY_SLOT: slot holding character U, -1 when absent
      s32 found = -1;
      for (int i = 0; i < 3 && found < 0; ++i)
        if (party_table(i) == operand_u(3)) found = i;
      write_var(word_at(1), found);
      advance(5);
      break;
    }
    case 0x99: {  // GET_PARTY_CHARACTER
      const s32 slot = operand_u(3);
      write_var(word_at(1), party_table(slot >= 0 && slot < 3 ? slot : 0));
      advance(5);
      break;
    }
    case 0x9A: {  // JUMP_IF_LEADER_IS: jumps when this entity's party slot holds character U
      const int slot = host_.party_slot_of_entity(ent_);
      jump_or_advance(slot >= 0 && party_table(slot) == operand_u(1), 3, 5);
      break;
    }
    case 0x9B: {
      const int target = entity_operand(5);
      host_.named_effect(NamedOp::AttachToEntity5, std::string_view(reinterpret_cast<const char*>(&mod_.code[base_ + 1]), 4), target);
      advance(6);
      break;
    }
    case 0x9C:
    case 0xB6:
    case 0xD3:
    case 0xD7:
    case 0xE7: {
      const NamedOp named_op = op == 0x9C ? NamedOp::ModeZero : op == 0xB6 ? NamedOp::Activate : op == 0xD3 ? NamedOp::PlayEffect
                      : op == 0xD7 ? NamedOp::StopEffect : NamedOp::ModeOne;
      host_.named_effect(named_op, std::string_view(reinterpret_cast<const char*>(&mod_.code[base_ + 1]), 4), ent_);
      advance(5);
      break;
    }
    case 0x9D: {
      const int target = entity_operand(1);
      if (target >= 0) {
        const std::array<s32, 0> none{};
        host_.entity_effect(0x9D, target, none);
      }
      advance(2);
      break;
    }
    case 0x9E: host_.set_dialog_face(operand_u(1)); advance(3); break;
    case 0x9F: host_.set_dialog_face(-1); advance(1); break;
    default:
      return false;
  }
  return true;
}

}  // namespace oc::script
