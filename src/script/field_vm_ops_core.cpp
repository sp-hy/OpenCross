// Main opcode page, part 1: flow control, variables and arithmetic, position and animation, movement, camera.
#include "script/field_vm.h"

#include <array>
#include <cmath>

namespace oc::script {

namespace {

const std::array<s16, 4096>& sin_table() {
  static const auto table = [] {
    std::array<s16, 4096> values{};
    for (size_t i = 0; i < values.size(); ++i)
      values[i] = static_cast<s16>(std::lround(std::sin(static_cast<double>(i) * 6.283185307179586 / 4096.0) * 4096.0));
    return values;
  }();
  return table;
}

}  // namespace

bool FieldVM::exec_core(u8 op) {
  u32& pc = rec_->pc;
  Entity& self = *self_;
  switch (op) {
    // ---------------------------------------------------------------- flow
    case 0x01:  // JUMP
      pc = word_at(1);
      break;
    case 0x02: {  // IF_JUMP: compare two T operands (mode bits 7 / 6 = literal) with the condition in mode & 0xF; jumps when false
      struct Operand {
        s32 value = 0;
        bool literal = false;
        bool is_unsigned_var = false;
      };
      const u8 mode = byte_at(5);
      auto operand = [&](u32 offset, u32 mask) {
        Operand result;
        const u16 raw = word_at(offset);
        if (mode & mask) {
          result.value = static_cast<s16>(raw);
          result.literal = true;
        } else {
          result.value = read_var(raw);
          result.is_unsigned_var = var_unsigned(raw);
        }
        return result;
      };
      Operand lhs = operand(1, 0x80), rhs = operand(3, 0x40);
      if (lhs.literal && rhs.is_unsigned_var) lhs.value = static_cast<u16>(lhs.value);
      if (rhs.literal && lhs.is_unsigned_var) rhs.value = static_cast<u16>(rhs.value);
      // Two variables: the right value takes the left variable's signedness, not its own.
      if (!lhs.literal && !rhs.literal)
        rhs.value = lhs.is_unsigned_var ? static_cast<s32>(static_cast<u16>(rhs.value))
                                        : static_cast<s32>(static_cast<s16>(rhs.value));
      bool taken = false;
      switch (mode & 0xF) {
        case 0: taken = lhs.value == rhs.value; break;
        case 1: taken = lhs.value != rhs.value; break;
        case 2: taken = lhs.value > rhs.value; break;
        case 3: taken = lhs.value < rhs.value; break;
        case 4: taken = lhs.value >= rhs.value; break;
        case 5: taken = lhs.value <= rhs.value; break;
        case 6: taken = (lhs.value & rhs.value) != 0; break;
        case 7: taken = lhs.value != rhs.value; break;
        case 8: taken = (lhs.value | rhs.value) != 0; break;
        case 9: taken = (lhs.value & rhs.value) != 0; break;
        case 10: taken = (~lhs.value & rhs.value) != 0; break;
        default: break;
      }
      jump_or_advance(!taken, 6, 8);
      break;
    }
    case 0x03:  // STOP_RESET_UPDATE: ends this record and every class-7 (UPDATE) record of the entity
      for (auto& record : self.rec)
        if (record.state == 7) {
          record.state = kRecIdle;
          record.id = 0xFF;
        }
      rec_->state = kRecIdle;
      rec_->id = 0xFF;
      yield_ = true;
      ended_ = true;
      break;
    case 0x04:  // KILL_ALL_SCRIPTS: resets every record of the entity; the call depth and action bit it clears are entity 8's
      for (auto& record : self.rec) record = ScriptRecord{};
      if (ents_.size() > 8) {
        Entity& eighth = ents_[8];
        eighth.depth = 0;
        eighth.flags270 &= ~flag270::kActionRunning;
      }
      self.flags270 &= ~flag270::kInputLock;
      rec_->pc = 0;
      yield_ = true;
      ended_ = true;
      break;
    case 0x05:  // CALL: stalls for good when the 4-entry stack is full
      if (self.depth >= kCallDepth) {
        ++stats_.call_overflows;
        park();
      } else {
        self.stack[self.depth++] = pc + 3;
        pc = word_at(1);
      }
      break;
    case 0x06:  // NOP06
    case 0x0C:  // NOP0C
      advance(1);
      break;
    case 0x07:  // RUN_SCRIPT_ASYNC
    case 0x08:  // RUN_SCRIPT_AND_WAIT: waits until the child is the target's running record or idle
    case 0x09: {  // RUN_SCRIPT_BLOCKING: waits for the child to finish
      // An inactive target (a party slot nobody occupies, a character not in the party, a disabled monster) is skipped
      // outright, and an 08 / 09 already waiting on it releases its hold and goes on; the scheduler never runs such an
      // entity, so waiting would last for ever. The blocking forms spawn the child held so its record is not reused
      // before the parent saw it. An 08 whose child keeps looping ends its wait as soon as the child became the target's
      // running record. All three wait in place (also while waiting for the child) as long as the system gate holds; a song
      // change alone does not hold them.
      if (!busy_gate()) break;
      const int target_ent = entity_operand(1);
      const int id = byte_at(2) & 0x1F, cls = byte_at(2) >> 5;
      if (target_ent >= 0 && (ents_[static_cast<size_t>(target_ent)].flags38 & flag38::kInactive)) {
        if (rec_->phase != 0 && rec_->child_ent >= 0 && rec_->child_rec >= 0)
          ents_[static_cast<size_t>(rec_->child_ent)].rec[static_cast<size_t>(rec_->child_rec)].hold = 0;
        forget_child();
        advance(3);
        break;
      }
      if (op == 0x07 || target_ent < 0) {
        if (target_ent < 0) {
          advance(3);
          break;
        }
        int rec_index = -1;
        if (spawn(target_ent, id, cls, false, &rec_index) == Spawn::NoRecord) break;
        advance(3);
        break;
      }
      if (rec_->phase == 0) {
        int rec_index = -1;
        const Spawn spawned = spawn(target_ent, id, cls, true, &rec_index);
        if (spawned == Spawn::NoRecord) break;
        if (spawned != Spawn::Spawned) {
          advance(3);
          break;
        }
        rec_->phase = 1;
        rec_->child_ent = static_cast<s16>(target_ent);
        rec_->child_rec = static_cast<s8>(rec_index);
        yield_ = true;
        break;
      }
      const Entity& target = ents_[static_cast<size_t>(rec_->child_ent)];
      ScriptRecord& child = ents_[static_cast<size_t>(rec_->child_ent)].rec[static_cast<size_t>(rec_->child_rec)];
      if (child.state == kRecIdle || (op == 0x08 && target.cur == static_cast<u8>(rec_->child_rec))) {
        child.hold = 0;
        forget_child();
        advance(3);
      } else {
        yield_ = true;
      }
      break;
    }
    case 0x0A:  // UPDATE_CHARACTER
      host_.update_entity(ent_);
      advance(1);
      break;
    case 0x0B:  // UPDATE_CHARACTER_STOP
      host_.update_entity(ent_);
      park();
      break;
    case 0x0D:  // RETURN: with an empty stack the record ends
      if (self.depth == 0) {
        release();
        yield_ = true;
      } else {
        pc = self.stack[--self.depth];
      }
      break;
    case 0x0E:  // SETUP_PC
      host_.setup_pc(ent_, byte_at(1), byte_at(2));
      advance(3);
      break;
    case 0x0F:  // SETUP_NPC
      host_.setup_npc(ent_, operand_u(1));
      advance(3);
      break;
    // ---------------------------------------------------------------- variables
    case 0x10:  // SET
    case 0x13:  // ADD
    case 0x14:  // SUB
    case 0x15:  // SET_BIT
    case 0x16:  // CLEAR_BIT
    case 0x19:  // AND
    case 0x1A:  // OR
    case 0x1B:  // XOR
    case 0x20:  // MUL
    case 0x21:  // DIV
      alu(op);
      break;
    case 0x11:  // SET_1
      write_var(word_at(1), 1);
      advance(3);
      break;
    case 0x12:  // SET_0
      write_var(word_at(1), 0);
      advance(3);
      break;
    case 0x17:  // INC
      write_var(word_at(1), read_var(word_at(1)) + 1);
      advance(3);
      break;
    case 0x18:  // DEC
      write_var(word_at(1), read_var(word_at(1)) - 1);
      advance(3);
      break;
    case 0x1C:  // SHL
      write_var(word_at(1), static_cast<s32>(static_cast<u32>(read_var(word_at(1))) << (operand_u(3) & 31)));
      advance(5);
      break;
    case 0x1D:  // SHR
      write_var(word_at(1), read_var(word_at(1)) >> (operand_u(3) & 31));
      advance(5);
      break;
    case 0x1E:  // RAND: 0..0x7FFF
      write_var(word_at(1), static_cast<s32>(draw_rand()));
      advance(3);
      break;
    case 0x1F: {  // RAND_MOD: rand * (U + 1) >> 15, so U is the inclusive maximum (RAND_MOD r,#1 is a coin flip)
      const s32 draw = static_cast<s32>(draw_rand());
      write_var(word_at(1), draw * (operand_u(3) + 1) >> 15);
      advance(5);
      break;
    }
    case 0x22:  // SIN: amplitude * sin(angle), 12-bit angle, result >> 12
    case 0x23: {  // COS
      const s32 angle = operand_s(3, 7, 0x40), amplitude = operand_s(5, 7, 0x20);
      const s32 index = (angle + (op == 0x23 ? 1024 : 0)) & 0xFFF;
      write_var(word_at(1), (sin_table()[static_cast<size_t>(index)] * amplitude) >> 12);
      advance(8);
      break;
    }
    case 0x24: {  // ATAN2, 12-bit result
      const double angle = std::atan2(static_cast<double>(operand_s(3, 7, 0x40)), static_cast<double>(operand_s(5, 7, 0x20)));
      write_var(word_at(1), static_cast<s32>(std::lround(angle * 4096.0 / 6.283185307179586)) & 0xFFF);
      advance(8);
      break;
    }
    // ---------------------------------------------------------------- position, facing, flags
    case 0x25:  // SET_POSITION_XZ: Y snaps to the ground
      host_.set_position_xz(ent_, operand_s(1, 5, 0x80), operand_s(3, 5, 0x40), operand_u(6));
      self.flags38 &= ~0x40u;
      self.flags270 |= flag270::kMoving;
      advance(8);
      break;
    case 0x26:  // SET_POSITION_XYZ: the height is free from now on
      host_.set_position_xyz(ent_, operand_s(1, 7, 0x80), operand_s(5, 7, 0x20), operand_s(3, 7, 0x40));
      self.flags38 |= 0x40;
      self.flags270 |= flag270::kMoving;
      advance(8);
      break;
    case 0x27:  // SET_SELF_AXIS_0
      self.axis[0] = static_cast<u16>(operand_u(1));
      advance(3);
      break;
    case 0x28:  // SET_SELF_AXIS_1
      self.axis[1] = static_cast<u16>(operand_u(1));
      advance(3);
      break;
    case 0x29:  // SET_SELF_AXIS_2
      self.axis[2] = static_cast<u16>(operand_u(1));
      advance(3);
      break;
    case 0x2A:  // SET_SELF_WORD_B8
      self.word_b8 = operand_s(1, 3, 0x80);
      advance(4);
      break;
    case 0x2B:  // ENABLE_CONTROLS: b != 0 releases every entity's input lock, else only this entity's
      if (byte_at(1))
        for (auto& entity : ents_) entity.flags270 &= ~flag270::kInputLock;
      else
        self.flags270 &= ~flag270::kInputLock;
      advance(2);
      break;
    case 0x2C:  // DISABLE_CONTROLS
      self.flags270 |= flag270::kInputLock;
      gflags_ |= gflag::kInputLock;
      budget_ += 0x40;
      advance(1);
      break;
    case 0x2D: {  // WAIT: loads the counter on the first visit, then counts down one per tick (or ends on the host's music cue)
      const int cue = host_.music_wait(operand_u(1));
      if (cue > 0) {
        advance(3);
      } else if (cue < 0) {
        if (rec_->wait == 0)
          rec_->wait = static_cast<u16>(operand_u(1));
        else
          --rec_->wait;
        if (rec_->wait == 0) advance(3);
      }
      yield_ = true;
      break;
    }
    case 0x2E: {  // TURN_TO_ENTITY
      const int target = entity_operand(1);
      if (target >= 0) {
        host_.turn_to_entity(ent_, target);
        self.flags270 |= 0x4000;
      }
      advance(2);
      break;
    }
    case 0x2F:  // SET_ROTATION_SIMPLE: b * 512
      host_.set_facing(ent_, (byte_at(1) << 9) & 0xFFF);
      self.flags270 |= 0x4000;
      advance(2);
      break;
    case 0x30:  // SET_ROTATION
      host_.set_facing(ent_, operand_u(1) & 0xFFF);
      self.flags270 |= 0x4000;
      advance(3);
      break;
    case 0x31: {  // TURN_TO_DIRECTION: sets entity c's facing
      const int target = entity_operand(1);
      if (target >= 0) {
        host_.set_facing(target, operand_u(2) & 0xFFF);
        ents_[static_cast<size_t>(target)].flags270 |= 0x4000;
      }
      advance(4);
      break;
    }
    case 0x32: {  // ADJUST_ROTATION: b == 0 adds the delta, else subtracts it
      s32 delta = operand_u(2);
      if (delta > 0x7FF) delta -= 2;
      const int facing = host_.get_facing(ent_);
      host_.set_facing(ent_, (byte_at(1) == 0 ? facing + delta : facing - delta) & 0xFFF);
      self.flags270 |= 0x4000;
      advance(4);
      break;
    }
    case 0x33: {  // TURN_TO_ENTITY_2: entity c1 turns to entity c2
      const int turner = entity_operand(1), target = entity_operand(2);
      if (turner >= 0 && target >= 0) {
        host_.turn_to_entity(turner, target);
        ents_[static_cast<size_t>(turner)].flags270 |= 0x4000;
      }
      advance(3);
      break;
    }
    case 0x34:  // TOGGLE_COLLISION
      if (byte_at(1) == 0) {
        self.flags270 |= 0x20000000;
        self.flags38 &= ~0x20u;
      } else {
        self.flags38 |= 0x20;
      }
      advance(2);
      break;
    case 0x35: {  // SET_HIDDEN: the collision toggle of entity c
      const int target = entity_operand(2);
      if (target >= 0) {
        Entity& entity = ents_[static_cast<size_t>(target)];
        if (byte_at(1) == 0) {
          entity.flags38 &= ~0x20u;
          entity.flags270 |= 0x20000000;
        } else {
          entity.flags38 |= 0x20;
        }
      }
      advance(3);
      break;
    }
    case 0x36: {  // SET_VISIBLE: showing an entity with a 3D object also puts its stand clip back
      const int target = entity_operand(2);
      if (target >= 0) {
        set_flag(ents_[static_cast<size_t>(target)].flags38, 4, byte_at(1) == 0);
        if (byte_at(1) != 0 && has_model(target)) host_.entity_effect(0x36, target, std::span<const s32>());
      }
      advance(3);
      break;
    }
    case 0x37:  // SET_FLAG_02
      set_flag(self.flags38, 1, byte_at(1) == 0);
      advance(2);
      break;
    case 0x38:  // SET_NO_COLLISION_TRIGGER
      set_flag(self.flags38, 2, byte_at(1) == 0);
      advance(2);
      break;
    case 0x39:  // SET_NOT_TALKABLE
      set_flag(self.flags38, 3, byte_at(1) == 0);
      advance(2);
      break;
    case 0x3A:  // WAIT_FRAME
      yield_ = true;
      advance(1);
      break;
    case 0x3B:  // STOP_EXEC
      park();
      break;
    case 0x3C:  // SET_ANIMATION: waits out the room-entry window; one extra instruction this tick for a model entity
      if (!run_mode_ && host_.entry_window()) {
        yield_ = true;
        break;
      }
      host_.set_animation(ent_, operand_u(1));
      if (!(self.flags38 & flag38::kNo3dObject)) budget_ += 1;
      advance(3);
      break;
    case 0x3D:  // SET_WALK_ANIMATION (mode 0..8)
      if (byte_at(1) < 9) host_.set_walk_animation(ent_, byte_at(1), operand_u(2));
      advance(4);
      break;
    case 0x3E:  // SKIP_FWD: skips U 3-byte instructions
      advance(3 + 3 * operand_u(1));
      break;
    case 0x3F:  // WALK_TO (b == 0 start form, else wait form); frees the height on every form, as B8 / B9 / BA do
    case 0x40:  // WALK_TO_2: as WALK_TO plus a speed / time operand
      self.flags38 |= 0x40;
      if (byte_at(1) == 0) {
        MotionRequest request;
        request.kind = op == 0x3F ? MotionKind::Walk : MotionKind::Walk2;
        request.mode = byte_at(1);
        request.x = operand_s(2, 8, 0x80);
        request.z = operand_s(4, 8, 0x40);
        request.y = operand_s(6, 8, 0x20);
        if (op == 0x40) request.params[0] = operand_u(9);
        begin_motion(request);
        advance(op == 0x3F ? 9 : 11);
      } else {
        wait_or_advance(host_.motion_wait(ent_), 2);
      }
      break;
    case 0x41:  // SET_MOVE_SPEED
      host_.set_move_speed(ent_, operand_u(1));
      advance(3);
      break;
    case 0x42:  // WALK_TO_XZ (b == 1 wait form)
    case 0x43:  // WALK_TO_XZ_2: frames plus a second parameter
      if (byte_at(1) == 1) {
        wait_or_advance(host_.motion_wait(ent_), 2);
      } else {
        MotionRequest request;
        request.kind = op == 0x42 ? MotionKind::WalkXZ : MotionKind::WalkXZ2;
        request.mode = byte_at(1);
        request.x = operand_s(2, 6, 0x80);
        request.z = operand_s(4, 6, 0x40);
        request.params[0] = operand_u(7);
        if (op == 0x43) request.params[1] = operand_u(9);
        begin_motion(request);
        advance(op == 0x42 ? 9 : 11);
      }
      break;
    case 0x44:  // WALK_FOLLOW_ENT (b == 1 wait form); a missing target skips the companion wait form too
    case 0xB8: {  // WALK_FOLLOW_ENT_3D
      const int target = op == 0x44 ? entity_operand(2) : resolve_character(byte_at(2));
      if (op == 0xB8) self.flags38 |= 0x40;
      if (byte_at(1) == 1) {
        wait_or_advance(target >= 0 && host_.motion_busy(ent_), 3);
      } else if (target < 0) {
        advance(6);
      } else {
        MotionRequest request;
        request.kind = op == 0x44 ? MotionKind::FollowEnt : MotionKind::FollowEnt3D;
        request.mode = byte_at(1);
        request.target = target;
        begin_motion(request);
        advance(3);
      }
      break;
    }
    case 0x45:  // WALK_TO_ENT_TIMED (b != 0 wait form); a missing target skips the companion wait form too
    case 0xB9: {  // WALK_TO_ENT_3D_TIMED
      const int target = entity_operand(2);
      if (op == 0xB9) self.flags38 |= 0x40;
      if (byte_at(1) != 0) {
        wait_or_advance(target >= 0 && host_.motion_busy(ent_), 3);
      } else if (target < 0) {
        advance(8);
      } else {
        MotionRequest request;
        request.kind = op == 0x45 ? MotionKind::FollowEntTimed : MotionKind::FollowEnt3DTimed;
        request.target = target;
        request.params[0] = operand_u(3);
        begin_motion(request);
        advance(5);
      }
      break;
    }
    case 0x46:  // MOVE: direction and frames (b == 1 wait form)
    case 0xBA: {  // MOVE_3D: heading, pitch and frames (b != 0 wait form); frees the height like WALK_TO
      if (op == 0xBA) self.flags38 |= 0x40;
      if (op == 0x46 ? byte_at(1) == 1 : byte_at(1) != 0) {
        wait_or_advance(host_.motion_busy(ent_), 2);
        break;
      }
      MotionRequest request;
      request.kind = op == 0x46 ? MotionKind::Move : MotionKind::Move3D;
      request.mode = byte_at(1);
      request.params[0] = operand_u(2);
      request.params[1] = operand_u(4);
      if (op == 0xBA) request.params[2] = operand_u(6);
      begin_motion(request);
      advance(op == 0x46 ? 6 : 8);
      break;
    }
    case 0x47:  // SET_SLOT_FLAG
      rec_->flag47 = byte_at(1);
      advance(2);
      break;
    case 0x48: {  // GET_POSITION: the variables receive X, Z, Y in that order
      const int target = entity_operand(1);
      const Vec3 pos = target >= 0 ? host_.get_position(target) : Vec3{};
      write_var(word_at(2), pos.x);
      write_var(word_at(4), pos.z);
      write_var(word_at(6), pos.y);
      advance(8);
      break;
    }
    case 0x49:  // GET_OWN_ROTATION
      write_var(word_at(1), host_.get_facing(ent_) & 0xFFF);
      advance(3);
      break;
    case 0x4A: {  // GET_ROTATION of entity c
      const int target = entity_operand(1);
      write_var(word_at(2), target >= 0 ? host_.get_facing(target) & 0xFFF : 0);
      advance(4);
      break;
    }
    case 0x4B:  // DEBUG print
      advance(3);
      break;
    case 0x4C: {  // GET_DISTANCE_2D
      const s32 x0 = operand_s(3, 11, 0x40), y0 = operand_s(5, 11, 0x20), x1 = operand_s(7, 11, 0x10), y1 = operand_s(9, 11, 0x08);
      write_var(word_at(1), isqrt_dist(x1 - x0, y1 - y0, 0));
      advance(12);
      break;
    }
    case 0x4D: {  // GET_DISTANCE_3D
      const s32 a0 = operand_s(3, 15, 0x40), a1 = operand_s(5, 15, 0x20), a2 = operand_s(7, 15, 0x10), b0 = operand_s(9, 15, 0x08),
                b1 = operand_s(11, 15, 0x04), b2 = operand_s(13, 15, 0x02);
      write_var(word_at(1), isqrt_dist(b0 - a0, b2 - a2, b1 - a1));
      advance(16);
      break;
    }
    case 0x4E:  // ENTITY_DISTANCE (2D)
    case 0x4F: {  // GET_DISTANCE_3D_ENTITIES
      const int first = entity_operand(3), second = entity_operand(4);
      s32 dist = 0;
      if (first >= 0 && second >= 0) {
        const Vec3 pos_a = host_.get_position(first), pos_b = host_.get_position(second);
        dist = isqrt_dist(pos_a.x - pos_b.x, op == 0x4F ? pos_a.y - pos_b.y : 0, pos_a.z - pos_b.z);
      }
      write_var(word_at(1), dist);
      advance(5);
      break;
    }
    case 0x50:  // JUMP_IF_NOT_KEY
      jump_or_advance((word_at(1) & host_.keys_held()) == 0, 3, 5);
      break;
    case 0x51:  // JUMP_IF_NOT_KEY_LATCH
      jump_or_advance((word_at(1) & host_.key_latch()) == 0, 3, 5);
      break;
    case 0x52:  // JUMP_IF_NOT_KEY_EQUAL: not used by the shipped data, logged and stepped over
    case 0x53:  // JUMP_IF_NOT_KEY_LATCH_EQUAL: not used by the shipped data, logged and stepped over
      opaque(op, 1, 4);
      advance(5);
      break;
    case 0x54:  // CLEAR_KEY_LATCH
      host_.clear_key_latch();
      advance(1);
      break;
    case 0x55:  // REGION_TEST_JUMP: jumps when the entity is outside the region
    case 0x56:  // REGION_BAND_TEST_JUMP
      if (host_.region_test(ent_, byte_at(1), op == 0x56)) {
        advance(4);
      } else {
        budget_ += 1;
        pc = word_at(2);
      }
      break;
    case 0x57:  // REGION_CALL: not used by the shipped data, logged and stepped over
    case 0x58:  // REGION_BAND_CALL: not used by the shipped data, logged and stepped over
      opaque(op, 1, 3);
      advance(4);
      break;
    case 0x59:  // TICK_BONUS
      budget_ += 0x40;
      advance(1);
      break;
    case 0x5A:  // SET_NEXT_ROOM: locks input, waits for the system gate, stores PREVIOUS_MAP_ID and requests the jump
      self.flags270 |= flag270::kInputLock;
      gflags_ |= gflag::kInputLock;
      if (!system_gate()) break;
      note_room_change(operand_u(1));
      host_.request_map_jump(operand_u(1), operand_u(3) & 0xFF);
      advance(5);
      break;
    case 0x5B:  // MOVE_TWEEN ((b & 3) == 3 wait form); frees the height on every form
    case 0x5C: {  // JUMP_MOVE: the same hop, landing on the floor; leaves the free-height flag alone
      if (op == 0x5B) self.flags38 |= 0x40;
      if ((byte_at(1) & 3) == 3) {
        wait_or_advance(host_.motion_busy(ent_), 2);
        break;
      }
      const u32 mode_off = op == 0x5B ? 10 : 8;
      MotionRequest request;
      request.kind = op == 0x5B ? MotionKind::Tween : MotionKind::Jump;
      request.mode = byte_at(1) & 3;
      request.x = operand_s(2, mode_off, 0x80);
      request.z = operand_s(4, mode_off, 0x40);
      request.y = operand_s(6, mode_off, 0x20);
      request.params[0] = op == 0x5B ? operand_s(8, 10, 0x10) : operand_u(9);
      begin_motion(request);
      advance(11);
      yield_ = true;
      break;
    }
    case 0x5D:  // SET_CAMERA_POSITION (b == 0 start form: 11 bytes when the frame count is 0, else 9; b != 0 wait form)
      if (byte_at(1) == 0) {
        const s32 frames = operand_s(6, 8, 0x20);
        host_.camera_set_position(operand_s(2, 8, 0x80), operand_s(4, 8, 0x40), frames);
        advance(frames == 0 ? 11 : 9);
      } else if (host_.camera_wait(CameraWait::Position, operand_s(static_cast<u32>(-7), static_cast<u32>(-1), 0x80),
                                   operand_s(static_cast<u32>(-5), static_cast<u32>(-1), 0x40))) {
        advance(2);
      }
      yield_ = true;
      break;
    case 0x5E:  // SET_CAMERA_TO_ACTOR (b == 0 start form: 6 bytes when the frame count is 0, else 4; b != 0 wait form)
      if (byte_at(1) == 0) {
        const s32 frames = operand_u(2);
        host_.camera_to_actor(frames);
        advance(frames == 0 ? 6 : 4);
      } else if (host_.camera_wait(CameraWait::Actor, 0, 0)) {
        advance(2);
      }
      yield_ = true;
      break;
    case 0x5F:  // SET_CONTROLLED_ENTITY: the scroll follows entity c
      host_.set_camera_target(entity_operand(1));
      advance(2);
      break;
    case 0x60:  // CLEAR_CONTROLLED_ENTITY: the scroll follows the controlled entity again
      host_.set_camera_target(-1);
      advance(1);
      break;
    case 0x61:  // CAMERA_FOLLOW (b == 0 start form: 7 bytes without a target or frames, else 5; b != 0 wait form)
      if (byte_at(1) == 0) {
        const int target = entity_operand(2);
        const s32 frames = operand_u(3);
        host_.camera_follow(target, frames);
        advance(target < 0 || frames == 0 ? 7 : 5);
      } else if (host_.camera_wait(CameraWait::Follow, 0, 0)) {
        advance(2);
      }
      yield_ = true;
      break;
    default:
      return false;
  }
  return true;
}

}  // namespace oc::script
