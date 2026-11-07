// Extension range (FE C0..FE FF): ops that turn on engine features for mods. Each instruction is FE sub size payload[size];
// a handler reads its operands from the start of the payload and the VM always steps over the whole instruction, so a payload
// may also carry operands that a later version of the op adds.
#include "script/field_vm.h"

#include <cstdlib>

#include "core/features.h"
#include "core/log.h"

namespace oc::script {

namespace {

u32 payload_size(const OpInfo& info) { return static_cast<u32>(std::atoi(info.lengths)) - 3; }

}  // namespace

void FieldVM::exec_extension(u8 sub) {
  const u16 key = static_cast<u16>(kExtPage | sub);
  // `base_` is the sub-opcode byte: byte 1 is the payload size, the payload starts at byte 2.
  const u32 size = byte_at(1);
  const u32 length = 2 + size;
  const OpInfo* info = find_op(key);
  if (!info) {
    note_unknown(key);
    advance(length);
    return;
  }
  if (size < payload_size(*info)) {
    ++stats_.unknown_ops[key];
    if (logged_.insert(key).second)
      log::warn("field script: extension op FE {:02X} ({}) has a {}-byte payload, needs {} (skipped)", sub, info->name, size,
                payload_size(*info));
    advance(length);
    return;
  }
  // Mod UI operands: element ids and palette indices are taken modulo the table sizes, coordinates as s16.
  const auto id_at = [&](u32 offset) { return static_cast<int>(static_cast<u32>(operand_u(offset)) % ModUi::kElements); };
  const auto coord = [&](u32 offset) { return static_cast<s16>(operand_u(offset)); };
  const auto colour = [&](u32 offset) { return static_cast<u8>(static_cast<u32>(operand_u(offset)) % ModUi::kPalette); };
  const auto byte = [&](u32 offset) { return static_cast<u8>(operand_u(offset)); };
  const auto put = [&](int id, ModUi::Element element) {
    element.revision = ++ui_.serial;
    ui_.elements[static_cast<size_t>(id)] = element;
  };
  switch (sub) {
    case 0xC0: write_var(word_at(2), features::level(operand_u(4))); break;
    case 0xC1: ui_.clear(); break;
    case 0xC2:
      ui_.shown = operand_u(2) != 0;
      if (!ui_.shown) ui_.holds_controls = false;
      break;
    case 0xC3: put(id_at(2), {ModUi::kRect, coord(4), coord(6), coord(8), coord(10), colour(12), byte(14)}); break;
    case 0xC4: put(id_at(2), {ModUi::kLine, coord(4), coord(6), coord(8), coord(10), colour(12), 0}); break;
    case 0xC5: {
      ModUi::Element text{ModUi::kText, coord(6), coord(8), 0, 0, 0, byte(10)};
      text.msg = static_cast<s16>(operand_u(4));
      put(id_at(2), text);
      break;
    }
    case 0xC6: put(id_at(2), {ModUi::kItem, coord(4), coord(6), coord(8), coord(10), 0, byte(12)}); break;
    case 0xC7:
      ui_.cursor = id_at(2);
      ui_.cursor_style = byte(4);
      break;
    case 0xC8: {
      // Blocks until a new press (a key held when it starts counts only once released, so the press that opened the screen is not
      // taken): a D-pad press moves the cursor to the nearest selectable item that way (var = 0x40 | item; a direction with none is
      // ignored), and a held direction steps again as a menu cursor does (ModUi::kRepeatDelay / kRepeatEvery); Circle picks the
      // cursor's item, Cross cancels (0x80). Then the record's turn ends: the next instruction runs
      // next tick. From the first pick on, the shown UI holds the field's controls (ModUi::holds_controls).
      constexpr u16 kCircle = 0x20, kCross = 0x40, kDirMask = 0xF000;
      constexpr std::array<u16, 4> kDirs = {0x1000, 0x2000, 0x4000, 0x8000};
      const u16 held = host_.keys_held();
      if (rec_->phase == 0) {
        host_.clear_key_latch();
        ui_.pick_held |= held;
        ui_.holds_controls = true;
        rec_->phase = 1;
        yield_ = true;
        return;
      }
      // The latch keeps a tap made and released between two ticks.
      const u16 pressed = static_cast<u16>((host_.key_latch() | held) & ~ui_.pick_held);
      host_.clear_key_latch();
      u16 keys = static_cast<u16>(pressed & ~kDirMask);
      if (pressed & kDirMask) {
        keys |= pressed & kDirMask;
        ui_.pick_repeat = 0;
      } else if (held & kDirMask) {
        ++ui_.pick_repeat;
        const int over = ui_.pick_repeat - ModUi::kRepeatDelay;
        if (over >= 0 && over % ModUi::kRepeatEvery == 0) keys |= held & kDirMask;
      } else {
        ui_.pick_repeat = 0;
      }
      ui_.pick_held = held;
      s32 result = -1;
      if (keys & kCircle) {
        result = ui_.cursor;
      } else if (keys & kCross) {
        result = ModUi::kPickCancel;
      } else {
        for (int dir = 0; dir < 4 && result < 0; ++dir) {
          if (!(keys & kDirs[static_cast<size_t>(dir)])) continue;
          const int next = ui_.step(dir);
          if (next >= 0) {
            ui_.cursor = next;
            result = ModUi::kPickMoved | next;
          }
        }
      }
      if (result < 0) {
        yield_ = true;
        return;
      }
      rec_->phase = 0;
      write_var(word_at(2), result);
      yield_ = true;
      break;
    }
    case 0xC9: {
      const auto [x, y] = ui_.centre(id_at(2));
      write_var(word_at(4), x);
      write_var(word_at(6), y);
      break;
    }
    case 0xCA: ui_.set(id_at(2), operand_u(4), operand_u(6)); break;
    case 0xCB: ui_.palette[colour(2)] = {byte(4), byte(6), byte(8)}; break;
    // Party swap's reserve slots (feature 1, level 2).
    case 0xCC: host_.set_reserve_member(operand_u(2), operand_u(4)); break;
    case 0xCD: {
      const int member = host_.reserve_member(operand_u(4));
      write_var(word_at(2), member < 0 ? 0xFF : static_cast<u16>(member));
      break;
    }
    default:
      ++stats_.unimplemented;
      if (logged_.insert(key).second) log::warn("field script: extension op FE {:02X} ({}) has no handler", sub, info->name);
      break;
  }
  advance(length);
}

}  // namespace oc::script
