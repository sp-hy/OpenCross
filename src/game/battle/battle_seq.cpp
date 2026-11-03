#include "game/battle/battle_seq.h"

#include "import/container.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace oc::game::bseq {

namespace {

// Advance in words of every handler, ops 0..0xAC.
constexpr u8 kLen[0xAD] = {
    1, 2, 2, 1, 5, 2, 5, 2, 2, 2, 1, 3, 3, 4, 5, 2,
    2, 3, 3, 3, 3, 3, 2, 1, 1, 1, 1, 2, 2, 11, 8, 3,
    2, 1, 1, 5, 3, 1, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4,
    3, 4, 4, 4, 3, 2, 2, 1, 4, 4, 4, 4, 2, 2, 1, 3,
    3, 1, 2, 3, 2, 2, 2, 3, 3, 3, 3, 3, 1, 2, 2, 2,
    3, 1, 7, 7, 3, 3, 3, 3, 3, 2, 1, 3, 3, 5, 8, 2,
    1, 2, 2, 3, 6, 2, 1, 2, 3, 3, 2, 2, 4, 3, 4, 2,
    4, 3, 1, 1, 5, 2, 3, 3, 8, 2, 2, 4, 4, 3, 1, 3,
    3, 5, 2, 5, 6, 3, 1, 1, 4, 1, 2, 5, 5, 2, 6, 6,
    2, 5, 3, 3, 3, 3, 2, 5, 9, 3, 2, 6, 1, 2, 1, 3,
    8, 3, 5, 5, 4, 4, 2, 2, 2, 3, 3, 1, 2,
};

// Wait-counter step per 60 Hz tick.
constexpr int kTick = 1;

int s16v(int value) { return static_cast<s16>(value); }

}  // namespace

std::array<s16, 128> Vm::globals_{};
int Vm::prev_action_ = -1;
u32 Vm::seq_last_ = 0;

// ---------------------------------------------------------------------------------------------------------------- table
bool Table::parse(ByteSpan block) {
  words_.clear();
  entry_off_.clear();
  if (block.size() < 8) return false;
  auto rd32 = [&](size_t offset) { return offset + 4 <= block.size() ? static_cast<u32>(block[offset]) | block[offset + 1] << 8 | block[offset + 2] << 16 | static_cast<u32>(block[offset + 3]) << 24 : 0u; };
  const u32 count = rd32(0);
  if (count == 0 || count > 512 || 4 + 4 * (static_cast<size_t>(count) + 1) > block.size()) return false;
  for (u32 i = 0; i < count; ++i) {
    const u32 offset = rd32(4 + 4 * static_cast<size_t>(i));
    if (offset + 16 > block.size()) return false;
    entry_off_.push_back(offset);
  }
  words_.resize(block.size() / 2);
  std::memcpy(words_.data(), block.data(), words_.size() * 2);
  return true;
}

int Table::initial_weapon_hide() const {
  int hide = 0;
  if (entries() <= 8) return 0;
  int pc = start(8, 0);
  // Ops up to the first control / wait op (jumps, branches, waits 08 09 45 0A 42 4E 4D 8A and the call / return / end ops) run in the first tick.
  for (int guard = 0; pc >= 0 && pc < static_cast<int>(words_.size()) && guard < 64; ++guard) {
    const int op = words_[static_cast<size_t>(pc)];
    if (op < 0 || op >= 0xAD) break;
    if (op >= 1 && op <= 9) break;
    if (op == 0x00 || op == 0x0A || op == 0x42 || op == 0x45 || op == 0x4C || op == 0x4D || op == 0x4E || op == 0x3E || op == 0x8A) break;
    if (op == 0x79 || op == 0x7A) {
      const int slot_arg = pc + 1 < static_cast<int>(words_.size()) ? words_[static_cast<size_t>(pc + 1)] : -1;
      for (int slot = 0; slot < 2; ++slot)
        if (slot_arg < 0 || slot_arg > 1 || slot_arg == slot) hide = op == 0x7A ? (hide | (1 << slot)) : (hide & ~(1 << slot));
    }
    pc += kLen[op];
  }
  return hide;
}

// Same first-tick walk as initial_weapon_hide.
int Table::initial_shadow() const {
  int shadow = -1;
  if (entries() <= 8) return shadow;
  int pc = start(8, 0);
  for (int guard = 0; pc >= 0 && pc < static_cast<int>(words_.size()) && guard < 64; ++guard) {
    const int op = words_[static_cast<size_t>(pc)];
    if (op < 0 || op >= 0xAD) break;
    if (op >= 1 && op <= 9) break;
    if (op == 0x00 || op == 0x0A || op == 0x42 || op == 0x45 || op == 0x4C || op == 0x4D || op == 0x4E || op == 0x3E || op == 0x8A) break;
    if (op == 0x21) shadow = 1;
    if (op == 0x22) shadow = 0;
    pc += kLen[op];
  }
  return shadow;
}

void Table::sequence_tags(std::set<u32>& out) const {
  for (int entry = 0; entry < entries(); ++entry) {
    const u32 base = entry_off_[static_cast<size_t>(entry)];
    u32 end = static_cast<u32>(words_.size() * 2);
    for (const u32 offset : entry_off_)
      if (offset > base) end = std::min(end, offset);
    for (int variant = 0; variant < 8; ++variant)
      for (int pc = start(entry, variant); pc >= 0 && static_cast<u32>(pc) * 2 < end;) {
        const int op = words_[static_cast<size_t>(pc)];
        if (op < 0 || op >= 0xAD) break;
        if ((op == 0x68 || op == 0x7D) && static_cast<size_t>(pc) + 2 < words_.size())
          out.insert(tag_of(words_[static_cast<size_t>(pc) + 1], words_[static_cast<size_t>(pc) + 2]));
        pc += kLen[op];
      }
  }
}

int Table::start(int entry, int variant) const {
  if (entry < 0 || entry >= entries() || variant < 0 || variant > 7) return -1;
  const u32 base = entry_off_[static_cast<size_t>(entry)];
  const int off = words_[base / 2 + static_cast<size_t>(variant)];
  if (off == 0) return -1;
  const int pc = static_cast<int>((base + static_cast<u32>(off)) / 2);
  return pc >= 0 && pc < static_cast<int>(words_.size()) ? pc : -1;
}

bool load_model_table(ByteSpan obj, Table& out) {
  if (obj.size() < 0x20) return false;
  auto rd32 = [&](size_t offset) { return static_cast<u32>(obj[offset]) | obj[offset + 1] << 8 | obj[offset + 2] << 16 | static_cast<u32>(obj[offset + 3]) << 24; };
  const u32 s5 = rd32(4 + 4 * 4), s6 = rd32(4 + 5 * 4);
  const u32 end = s6 ? s6 : static_cast<u32>(obj.size());
  if (s5 == 0 || s5 + 8 > end || end > obj.size()) return false;
  return out.parse(obj.subspan(s5, end - s5));
}

bool load_atk_table(ByteSpan prd, Table& out) {
  auto drp = import::parse_drp(prd);
  if (!drp) return false;
  for (const import::DrpEntry& entry : drp->entries)
    if (entry.type == 0x1A && entry.name == "atk") return out.parse(entry.data);
  return false;
}

// ------------------------------------------------------------------------------------------------------------------- vm
bool Vm::start(const Table* table, int entry, int variant) {
  running_ = false;
  if (!table) return false;
  const int start_pc = table->start(entry, variant);
  if (start_pc < 0) return false;
  table_ = table;
  variant_ = variant;
  pc_ = start_pc;
  counter_ = 0;
  oval_.on = false;
  sp_ = 0;
  running_ = true;
  return true;
}

int Vm::rd(int idx) const {
  idx &= 0xFF;
  return (idx & 0x80) ? globals_[static_cast<size_t>(idx & 0x7F)] : vars_[static_cast<size_t>(idx & 0x7F)];
}
void Vm::wr(int idx, int value) {
  idx &= 0xFF;
  ((idx & 0x80) ? globals_ : vars_)[static_cast<size_t>(idx & 0x7F)] = static_cast<s16>(value);
}

// Relative wait. counter < 1: arm (counter += ticks) and finish at once only if the sum is already <= 0.
bool Vm::wait_ticks(int ticks) {
  if (s16v(counter_) < 1) {
    counter_ = s16v(counter_ + ticks);
    return ticks > 0 && counter_ <= 0;
  }
  counter_ = s16v(counter_ - kTick);
  return counter_ <= 0;
}

// Total wait (total = (frames - 1) * 4 for an animation, or the move's tick count): wait until `total - param` ticks have passed.
bool Vm::wait_total(int total, int param) {
  if (s16v(counter_) < 1) {
    const int remaining = total - param;
    const int armed = counter_ + remaining;
    if (remaining > 0) {
      counter_ = s16v(armed);
      return counter_ <= 0;
    }
    return true;
  }
  counter_ = s16v(counter_ - kTick);
  return counter_ <= 0;
}

void Vm::tick(const Ctx& ctx, std::vector<Event>& out) {
  if (!running_ || !table_) return;
  // OvalMoveS task: angle += step, position = centre + the oval point, the unit faces its direction of travel, the angle goes to its variable.
  if (oval_.on) {
    Oval& oval = oval_;
    oval.angle = static_cast<s16>(oval.angle + oval.step);
    const double radians = oval.angle * 6.283185307179586 / 4096.0;
    // The major axis runs from the target back to the unit (the unit starts at its own end).
    const double px = oval.cx - oval.ax * oval.rx * std::cos(radians) - oval.az * oval.rz * std::sin(radians);
    const double pz = oval.cz - oval.az * oval.rx * std::cos(radians) + oval.ax * oval.rz * std::sin(radians);
    const double vx = px - ctx.pos_x, vz = pz - ctx.pos_z;
    out.push_back({Event::MovePos, static_cast<int>(std::lround(px)), static_cast<int>(std::lround(pz)), 0, 1});
    if (std::hypot(vx, vz) > 0.5) out.push_back({Event::Rotate, static_cast<int>(std::lround(std::atan2(-vx, -vz) * 4096.0 / 6.283185307179586)) & 0xFFF, 1});
    wr(oval.out, oval.angle);
    // The model velocity = the normalised step (4096 = 1 unit per tick), read by op 0F; zeroed when the task ends.
    {
      const double length = std::hypot(vx, vz);
      if (length > 0.5) vel_x_ = static_cast<int>(std::lround(vx / length * 4096.0)), vel_z_ = static_cast<int>(std::lround(vz / length * 4096.0));
    }
    if (static const bool dbg = std::getenv("OC_OVAL_DEBUG") != nullptr; dbg) std::fprintf(stderr, "oval: angle %d left %d pos %.0f %.0f\n", oval.angle, oval.left, px, pz);
    if (--oval.left <= 0) oval.on = false, zero_vel_ = true;
  }
  // The task zeroes the velocity at its last step, after the script of that tick has run (the dash's GetVec at the oval's end still reads the tangent).
  struct ZeroVel {
    Vm& vm;
    ~ZeroVel() {
      if (vm.zero_vel_) vm.vel_x_ = vm.vel_z_ = 0, vm.zero_vel_ = false;
    }
  } zero_at_exit{*this};
  // SetPos moves the model at once, so a LookPos / PosMoveT / GetPos after it in the same run read the NEW position (e.g. `9A` then `99` LookPos
  // on the pseudo-actor). The Events are applied after the run, so the run keeps its own copy of the unit position.
  Ctx lctx = ctx;
  live_ = &lctx;
  struct ClearLive {
    Vm& vm;
    ~ClearLive() { vm.live_ = nullptr; }
  } clear_live{*this};
  for (int i = 0; i < kMaxOpsPerTick && running_; ++i) {
    const int adv = step(lctx, out);
    if (adv == 0) return;
    pc_ += adv;
    if (pc_ < 0 || pc_ >= static_cast<int>(table_->words().size())) {
      running_ = false;
      out.push_back({Event::Done});
      return;
    }
  }
}

// One op. Returns the advance in words; 0 = blocked / ended.
int Vm::step(const Ctx& ctx, std::vector<Event>& out) {
  const auto& words = table_->words();
  const int pc = pc_;
  auto arg = [&](int offset) -> int { return pc + offset < static_cast<int>(words.size()) ? words[static_cast<size_t>(pc + offset)] : 0; };
  const int op = arg(0);
  auto end = [&]() {
    running_ = false;
    out.push_back({Event::Done});
    return 0;
  };
  // Unknown op: the engine would run off the table.
  if (op < 0 || op > 0xAC) return end();
  const int len = kLen[op];
  if (trace) trace(pc, &words[static_cast<size_t>(pc)], std::min(len, static_cast<int>(words.size()) - pc));
  auto emit = [&](Event::Kind kind, int a = 0, int b = 0, int c = 0, int d = 0, u32 tag = 0) {
    Event event;
    event.kind = kind;
    event.a = a;
    event.b = b;
    event.c = c;
    event.d = d;
    event.tag = tag;
    out.push_back(event);
  };
  auto cond = [&]() {
    const int lhs = s16v(rd(arg(1))), rhs = s16v(rd(arg(2)));
    switch (arg(3)) {
      case 0: return lhs == rhs;
      case 1: return lhs < rhs;
      case 2: return rhs < lhs;
      case 3: return !(rhs < lhs);
      case 4: return !(lhs < rhs);
      case 5: return (lhs ^ rhs) != 0;
      case 6: return (lhs & rhs) != 0;
      default: return false;
    }
  };

  switch (op) {
    // ---- flow
    case 0x00: case 0x3E: case 0x4C: return end();
    case 0x01: pc_ = pc + arg(1); return 1;
    case 0x07: pc_ = pc + arg(1); return 2;
    case 0x05: pc_ = pc + arg(1); return 2;
    case 0x02:
      if (sp_ < static_cast<int>(stack_.size())) stack_[static_cast<size_t>(sp_++)] = pc;
      pc_ = pc + arg(1);
      return 1;
    case 0x03:
      if (sp_ <= 0) return end();
      pc_ = stack_[static_cast<size_t>(--sp_)];
      return 2;
    case 0x04: return cond() ? 5 : arg(4);
    case 0x06: return cond() ? 5 : arg(4) + 2;
    case 0x08: return 2;
    case 0x09: return wait_ticks(arg(1)) ? 2 : 0;
    case 0x45:  // op 09, and a finished wait of anything but the attacker's own program clears the chain state of GetHokanFrame
      if (!wait_ticks(arg(1))) return 0;
      if (!(table_->is_atk() && variant_ == 0)) prev_action_ = -1;
      return 2;
    case 0x8A: return wait_ticks(rd(arg(1)) & 0x1FFF) ? 2 : 0;
    case 0x0A: return wait_total((anim_frames_ - 1) * 4, 0) ? 1 : 0;
    case 0x42: return wait_total((anim_frames_ - 1) * 4, arg(1)) ? 2 : 0;
    case 0x4E:  // op 42, same reset as 0x45
      if (!wait_total((anim_frames_ - 1) * 4, arg(1))) return 0;
      if (!(table_->is_atk() && variant_ == 0)) prev_action_ = -1;
      return 2;
    case 0x4D: return wait_total(move_ticks_, arg(1)) ? 2 : 0;

    // ---- variables
    case 0x40: wr(arg(1), arg(2)); return 3;
    case 0x4B: wr(arg(1), rd(arg(2))); return 3;
    case 0x26: wr(arg(1), rd(arg(1)) + 1); return 2;
    case 0x27: wr(arg(1), rd(arg(1)) - 1); return 2;
    case 0x28: wr(arg(1), rd(arg(1)) + rd(arg(2))); return 3;
    case 0x29: wr(arg(1), rd(arg(1)) - rd(arg(2))); return 3;
    case 0x2A: { const int divisor = s16v(rd(arg(2))); if (divisor) wr(arg(1), s16v(rd(arg(1))) / divisor); return 3; }
    case 0x2B: wr(arg(1), rd(arg(1)) * rd(arg(2))); return 3;
    case 0x2C: { const int divisor = s16v(rd(arg(2))); if (divisor) wr(arg(1), s16v(rd(arg(1))) % divisor); return 3; }
    case 0x2D: wr(arg(2), static_cast<int>(std::lround(std::sin(static_cast<double>(arg(1) & 0xFFF) * 6.283185307 / 4096.0) * 4096.0))); return 3;
    case 0x2E: wr(arg(2), static_cast<int>(std::lround(std::cos(static_cast<double>(arg(1) & 0xFFF) * 6.283185307 / 4096.0) * 4096.0))); return 3;
    case 0x30: {  // Rand m out; dev: OC_SEQ_RAND=k forces k % m
      const int modulus = arg(1);
      if (modulus > 0) {
        static const char* forced = std::getenv("OC_SEQ_RAND");
        wr(arg(2), forced ? std::atoi(forced) % modulus : (ctx.rand ? ctx.rand() : std::rand()) % modulus);
      }
      return 3;
    }
    case 0x76: wr(arg(1), rd(arg(1)) & rd(arg(2))); return 3;
    case 0x77: wr(arg(1), rd(arg(1)) | rd(arg(2))); return 3;

    // ---- animation
    case 0x15: case 0x3F: {
      const int clip = op == 0x15 ? arg(1) : s16v(rd(arg(1)));
      const int flags = op == 0x15 ? arg(2) : s16v(rd(arg(2)));
      anim_frames_ = ctx.clip_frames ? std::max(1, ctx.clip_frames(clip)) : 8;
      emit(Event::SetAnim, clip, flags);
      return 3;
    }
    case 0x16: emit(Event::Speed, arg(1)); return 2;
    case 0x17: loop_ = 1; emit(Event::Loop, 1); return 1;
    case 0x18: loop_ = 0; emit(Event::Loop, 0); return 1;

    // ---- queries (outputs go to variables)
    case 0x43: wr(arg(1), ctx.action_a); wr(arg(2), ctx.action_b); return 3;
    case 0x44: {  // GetHokanFrame: blend ticks between consecutive hits = byte of the S5 entry-0 table [prev action][hand][action][hand]
      const Table* model_table = table_->model_table();
      // Without a model table (the generic fallback atk) the value is a constant.
      int blend = 4;
      if (model_table) {
        const int hand = ctx.action_b & 1, act = ctx.action_a;
        blend = model_table->hokan_word((((hand ^ 1) + (prev_action_ + 1) * 2) * 8 + act * 2 + hand)) & 0xFF;
      }
      wr(arg(1), blend);
      prev_action_ = ctx.action_a;
      return 2;
    }
    case 0x47: wr(arg(2), ctx.hit_code); return 3;
    case 0x48: wr(arg(2), ctx.dead ? 1 : 0); return 3;
    case 0x49: wr(arg(2), static_cast<int>(ctx.flags & 0xFFFF)); return 3;
    case 0x4A: wr(arg(2), static_cast<int>(ctx.flags >> 16)); return 3;
    case 0x54: wr(arg(2), static_cast<int>(ctx.status_lo & 0xFFFF)); return 3;
    case 0x55: wr(arg(2), static_cast<int>(ctx.status_lo >> 16)); return 3;
    case 0x56: wr(arg(2), static_cast<int>(ctx.status_hi & 0xFFFF)); return 3;
    case 0x57: wr(arg(2), static_cast<int>(ctx.status_hi >> 16)); return 3;
    case 0x58: wr(arg(2), ctx.attrib); return 3;
    case 0x63: wr(arg(2), ctx.monster ? 1 : 0); return 3;
    case 0x65: wr(arg(1), ctx.pose); return 2;
    case 0x67: wr(arg(1), ctx.model_num); return 2;
    case 0x69: wr(arg(2), 0); return 3;
    case 0x75: wr(arg(1), 0); return 2;
    // GetRot: vars [v] = x rot, [v+1] = z rot, [v+2] = yaw; RotateT reads the same layout, so a flinch "yaw += k; RotateT; yaw -= k; RotateT" turns around the current facing
    case 0x61: wr(arg(1), 0); wr(arg(1) + 1, 0); wr(arg(1) + 2, ctx.yaw); return 2;
    case 0x6F: wr(arg(1), ctx.stand_x); wr(arg(1) + 1, ctx.stand_z); wr(arg(1) + 2, 0); return 2;

    // ---- engine requests
    // Op 0x33 is positional (pan / volume from the unit's screen x and depth); op 0x7B has centre pan and volume = operand 3 & 0x7F.
    case 0x33: emit(Event::Sfx, 127, 1, 0, 0, tag_of(static_cast<s16>(arg(1)), static_cast<s16>(arg(2)))); return len;
    case 0x7B: emit(Event::Sfx, arg(3) & 0x7F, 0, 0, 0, tag_of(static_cast<s16>(arg(1)), static_cast<s16>(arg(2)))); return len;
    case 0x7D: emit(Event::Sequence, 0, 0, 0, 0, tag_of(static_cast<s16>(arg(1)), static_cast<s16>(arg(2)))); return len;
    case 0x68: {  // as 0x7D, but not again while the last name started (first three bytes) is the same one; 0x7E forgets it
      const u32 tag = tag_of(static_cast<s16>(arg(1)), static_cast<s16>(arg(2)));
      if ((seq_last_ & 0xFFFFFF) != (tag & 0xFFFFFF)) {
        emit(Event::Sequence, 0, 0, 0, 0, tag);
        seq_last_ = tag;
      }
      return len;
    }
    case 0x7E: seq_last_ = 0; return len;
    // The script pointer moves on one word and the script yields (a one-tick wait).
    case 0x9C: pc_ = pc + 1; return 0;
    case 0x50: emit(Event::Camera, 0, 0, 0, 0, tag_of(static_cast<s16>(arg(1)), static_cast<s16>(arg(2)))); return len;
    case 0x1D: {
      emit(Event::Effect, arg(1) & 0xFF, arg(2), 0, 0, tag_of(static_cast<s16>(arg(3)), static_cast<s16>(arg(4))));
      Event& event = out.back();
      for (int k = 0; k < 3; ++k) event.off[k] = static_cast<s16>(arg(5 + k)), event.ang[k] = static_cast<s16>(arg(8 + k));
      return len;
    }
    case 0x23: {  // trail node colours = u32(w1, w2), u32(w3, w4) (bytes r g b)
      emit(Event::TrailColors);
      Event& event = out.back();
      event.off[0] = static_cast<s16>(arg(1) & 0xFF), event.off[1] = static_cast<s16>((arg(1) >> 8) & 0xFF), event.off[2] = static_cast<s16>(arg(2) & 0xFF);
      event.ang[0] = static_cast<s16>(arg(3) & 0xFF), event.ang[1] = static_cast<s16>((arg(3) >> 8) & 0xFF), event.ang[2] = static_cast<s16>(arg(4) & 0xFF);
      return len;
    }
    case 0x46: emit(Event::TrailMode, arg(1)); return len;
    case 0x5B: {  // trail colour change = u32(w1, w2) as signed chars
      emit(Event::TrailFade);
      Event& event = out.back();
      event.off[0] = static_cast<s8>(arg(1) & 0xFF), event.off[1] = static_cast<s8>((arg(1) >> 8) & 0xFF), event.off[2] = static_cast<s8>(arg(2) & 0xFF);
      return len;
    }
    case 0x24: case 0x85: emit(Event::TrailOn, op == 0x85 ? 1 : 0, arg(1), arg(2)); return len;
    case 0x25: case 0x86: emit(Event::TrailOff, op == 0x86 ? 1 : 0); return len;
    case 0x64: case 0x8E: {  // colour ramp: n, from = u32(w2, w3), to = u32(w4, w5)
      emit(Event::ColorRamp, arg(1), op == 0x8E ? 1 : 0);
      Event& event = out.back();
      event.off[0] = static_cast<s16>(arg(2) & 0xFF), event.off[1] = static_cast<s16>((arg(2) >> 8) & 0xFF), event.off[2] = static_cast<s16>(arg(3) & 0xFF);
      event.ang[0] = static_cast<s16>(arg(4) & 0xFF), event.ang[1] = static_cast<s16>((arg(4) >> 8) & 0xFF), event.ang[2] = static_cast<s16>(arg(5) & 0xFF);
      return len;
    }
    // Blink on / off (the view applies the gate: party faces only; off also sets eye frame 0), and Eye sets eye frame w1.
    case 0x72: emit(Event::Blink, 1); return 1;
    case 0x73: emit(Event::Blink, 0); return 1;
    case 0x4F: emit(Event::Eye, arg(1)); return 2;
    case 0x5D: {  // light slot w1: position := (w2, w3, w4) as words
      emit(Event::LightAbs, arg(1));
      Event& event = out.back();
      for (int k = 0; k < 3; ++k) event.off[k] = static_cast<s16>(arg(2 + k));
      return len;
    }
    case 0x5C: emit(Event::LightPos, arg(1), (arg(2) & 0xFFFF) >> 8, arg(2) & 0xFF); return len;
    case 0x5E: {
      emit(Event::LightSet, arg(1), s16v(arg(4)), s16v(arg(5)), s16v(arg(6)));
      Event& event = out.back();
      event.off[0] = static_cast<s16>(arg(2) & 0xFF), event.off[1] = static_cast<s16>((arg(2) >> 8) & 0xFF), event.off[2] = static_cast<s16>(arg(3) & 0xFF);
      event.ang[0] = static_cast<s16>(arg(7));
      return len;
    }
    case 0x62: {  // SetRot v: x rot := [v], z rot := [v+1], yaw := [v+2], at once (RotateT eases there)
      const int base = arg(1);
      emit(Event::SetRot, rd(base + 2) & 0xFFF);
      Event& event = out.back();
      event.off[0] = static_cast<s16>(rd(base)), event.off[1] = static_cast<s16>(rd(base + 1));
      return len;
    }
    case 0x21: emit(Event::Shadow, 1); return len;
    case 0x22: emit(Event::Shadow, 0); return len;
    case 0x82: emit(Event::SlowK, arg(1) & 0xFF); return len;
    case 0x83: {  // the scale words ramp (integer step (target - now) / n per tick) to (w1, w2, w3) over w4 ticks
      emit(Event::ModelScale, 0, 0, 0, arg(4));
      Event& event = out.back();
      event.off[0] = static_cast<s16>(arg(1)), event.off[1] = static_cast<s16>(arg(2)), event.off[2] = static_cast<s16>(arg(3));
      return len;
    }
    case 0x52: case 0x53: {  // ticks, abr mode, from rgb = u32(w3, w4), to rgb = u32(w5, w6) (bytes r g b)
      emit(op == 0x52 ? Event::ScreenFade : Event::ModelFade, arg(1), arg(2));
      Event& event = out.back();
      event.off[0] = static_cast<s16>(arg(3) & 0xFF), event.off[1] = static_cast<s16>((arg(3) >> 8) & 0xFF), event.off[2] = static_cast<s16>(arg(4) & 0xFF);
      event.ang[0] = static_cast<s16>(arg(5) & 0xFF), event.ang[1] = static_cast<s16>((arg(5) >> 8) & 0xFF), event.ang[2] = static_cast<s16>(arg(6) & 0xFF);
      return len;
    }
    case 0x8F: {  // element w1's scale words ramp to (w2, w3, w4) over w5 ticks
      emit(Event::PartScale, arg(1), 0, 0, arg(5));
      Event& event = out.back();
      event.off[0] = static_cast<s16>(arg(2)), event.off[1] = static_cast<s16>(arg(3)), event.off[2] = static_cast<s16>(arg(4));
      return len;
    }
    case 0x90: emit(Event::PartScaleStop, arg(1)); return len;
    case 0x91: emit(Event::PartBlink, arg(1), arg(2), arg(3), arg(4)); return len;
    case 0x92: emit(Event::PartBlinkStop, arg(1), arg(2)); return len;
    case 0x6A: emit(Event::ElemMotion, arg(1), 0); return len;
    case 0x6B: emit(Event::ElemMotion, arg(1), 1); return len;
    case 0x6C: case 0x6D: {  // 6C rotates to vars [w2..] over w3 ticks, 6D turns by the rates in vars [w2..] every tick; vars x, z, yaw = [v], [v+1], [v+2]
      const int base = arg(2);
      // The vars are read in order x, z, yaw but stored as off = x, y (yaw), z.
      emit(op == 0x6C ? Event::ElemRot : Event::ElemRotRate, arg(1), 0, 0, op == 0x6C ? arg(3) : 0);
      Event& event = out.back();
      event.off[0] = static_cast<s16>(op == 0x6C ? rd(base) & 0xFFF : rd(base)), event.off[1] = static_cast<s16>(op == 0x6C ? rd(base + 2) & 0xFFF : rd(base + 2)),
      event.off[2] = static_cast<s16>(op == 0x6C ? rd(base + 1) & 0xFFF : rd(base + 1));
      return len;
    }
    case 0x8C: {  // PitchDown list idx point ticks: dir = normalise(target attach point - own attach 9); the handler turns it about y by
      // -(-ratan2(-dx, -dz)) so the horizontal part lies on -z (z' = -h, y' = dy), and element 0's z rotation walks to ratan2(z', -y') = atan2(-h, -dy) (4096 = turn):
      // -0x400 for a level target, towards 0 as the target rises above, towards -0x800 as it falls below
      int ax = 0, az = 0, ay = 0, bx = 0, bz = 0, by = 0;
      if (ctx.get_attach_self && ctx.get_attach && ctx.get_attach_self(9, ax, az, ay) && ctx.get_attach(arg(1), arg(2), arg(3), bx, bz, by)) {
        const double dx = bx - ax, dy = by - ay, dz = bz - az, dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        int ang = 0;
        if (dist > 0.0) {
          const double horizontal = std::hypot(dx, dz) / dist;
          ang = static_cast<int>(std::lround(std::atan2(-horizontal, -dy / dist) * (4096.0 / (2.0 * 3.14159265358979323846))));
        }
        emit(Event::ElemTilt, 0, 0, 0, arg(4));
        out.back().off[2] = static_cast<s16>(ang);
      }
      return len;
    }
    // Back to the z saved by 8C over w1 ticks, motion on at the end.
    case 0x8D: emit(Event::ElemTiltBack, 0, 0, 0, arg(1)); return len;
    case 0x8B: {  // GetKabuShockPos list idx point out: the dive spot of Vege Might (the turnip): 2048 units out from the target's attach point P on the caster's side (horizontal, d = caster - P)
      // and 0x1000 sin 60 deg = 3547 BELOW it (y +, under the floor: the turnip burrows). dir = normalise(own pos - P) in x / z, rotated by (x 0x2AA, y ratan2(-dx, -dz), z 0)
      // applied to (0, 0, -0x1000) = (-cos60 sin y, +sin60, -cos60 cos y) * 0x1000 = (+0.5 dx/h, +0.866, +0.5 dz/h) * 0x1000 (y + = down), added to P
      int x = 0, z = 0, y = 0;
      if (ctx.get_attach && ctx.get_attach(arg(1), arg(2), arg(3), x, z, y)) {
        const double dx = ctx.pos_x - x, dz = ctx.pos_z - z, horizontal = std::hypot(dx, dz);
        wr(arg(4), x + (horizontal > 0.5 ? static_cast<int>(std::lround(dx / horizontal * 2048.0)) : 0));
        wr(arg(4) + 1, z + (horizontal > 0.5 ? static_cast<int>(std::lround(dz / horizontal * 2048.0)) : 0));
        wr(arg(4) + 2, y + 3547);
      }
      return len;
    }
    case 0xA0: {  // part, ticks, abr mode, from rgb = u32(w4, w5), to rgb = u32(w6, w7)
      emit(Event::PartColor, arg(1), arg(3), 0, arg(2));
      Event& event = out.back();
      event.off[0] = static_cast<s16>(arg(4) & 0xFF), event.off[1] = static_cast<s16>((arg(4) >> 8) & 0xFF), event.off[2] = static_cast<s16>(arg(5) & 0xFF);
      event.ang[0] = static_cast<s16>(arg(6) & 0xFF), event.ang[1] = static_cast<s16>((arg(6) >> 8) & 0xFF), event.ang[2] = static_cast<s16>(arg(7) & 0xFF);
      return len;
    }
    // GetCommon list idx out: vars [out] := the unit's common word.
    case 0x9F: wr(arg(2), ctx.common); return len;
    // CheckEquip v: vars [v] := one equipment bit of the running unit, set once at battle init from the equipment. 0xA6 "CheckEquipGrandorion" bit 13 =
    // weapon item 0x26 (the Mastermune); 0xA7 "CheckEquipBraveShield" bit 14 = an accessory 0xC1 (Hero's Shield); 0xA8 "CheckEquipBraveGoods" bit 15 =
    // Hero's Blade 0x3D with Hero's Shield 0xC1 + Hero's Medal 0xC0. sky's `main` branches on 0xA6.
    case 0xA6: wr(arg(1), static_cast<int>((ctx.flags >> 13) & 1)); return len;
    case 0xA7: wr(arg(1), static_cast<int>((ctx.flags >> 14) & 1)); return len;
    case 0xA8: wr(arg(1), static_cast<int>((ctx.flags >> 15) & 1)); return len;
    case 0x19: emit(Event::ModelShow, 1); return len;
    case 0x1A: emit(Event::ModelShow, 0); return len;
    // 0x79 / 0x7A `n`: the attached-object slot n (0 / 1: the weapon mounts) is shown / hidden; n outside 0..1 writes both. Wanda In hides the wand
    // while the pack's mesh sprite of it flies, and shows it again before the catch clip.
    case 0x79: emit(Event::WeaponShow, 1, arg(1) >= 0 && arg(1) < 2 ? arg(1) : -1); return len;
    case 0x7A: emit(Event::WeaponShow, 0, arg(1) >= 0 && arg(1) < 2 ? arg(1) : -1); return len;
    case 0x1B: emit(Event::PartShow, 1, arg(1)); return len;
    case 0x1C: emit(Event::PartShow, 0, arg(1)); return len;
    case 0x1F: {
      emit(Event::ModelColor);
      Event& event = out.back();
      event.off[0] = static_cast<s16>(arg(1) & 0xFF), event.off[1] = static_cast<s16>((arg(1) >> 8) & 0xFF), event.off[2] = static_cast<s16>(arg(2) & 0xFF);
      return len;
    }
    case 0x20: emit(Event::ModelAbr, s16v(arg(1))); return len;
    case 0x41: emit(Event::Damage); return 1;
    case 0x59: emit(Event::React, arg(1)); return 2;
    case 0x5A: emit(Event::React, -1); return 1;
    case 0x12: {  // Face target idx ticks: turns toward the target AND sets the velocity to the normalised direction, as 0x99 does;
      // op 0F GetVec after it returns that direction (taiatari / gattai: `12 0 10; 0F 3; 71 ..; 0D 3 ..` - the leap after the run-in goes toward Serge)
      emit(Event::Face, arg(1), arg(2));
      int tx = 0, tz = 0;
      if (ctx.get_pos && ctx.get_pos(1, arg(1), 0, tx, tz)) {
        const double dx = tx - ctx.pos_x, dz = tz - ctx.pos_z, length = std::hypot(dx, dz);
        if (length >= 1.0) {
          vel_x_ = static_cast<int>(dx / length * 4096.0 * 256.0);
          vel_z_ = static_cast<int>(dz / length * 4096.0 * 256.0);
        }
      }
      return 3;
    }
    case 0x11: {  // RotateT: rotate to the absolute angles held in vars [v] (x), [v+2] (y = yaw), [v+1] (z) over b ticks
      const int base = arg(1);
      emit(Event::Rotate, rd(base + 2) & 0xFFF, std::max(1, arg(2)));
      // The x / z targets: all three angles step, but units only draw the yaw (the pseudo-actor ramps them too).
      out.back().off[0] = static_cast<s16>(rd(base) & 0xFFF), out.back().off[1] = static_cast<s16>(rd(base + 1) & 0xFFF);
      return 3;
    }
    case 0x0B: oval_.on = false; emit(Event::MoveTime, arg(1), arg(2)); move_ticks_ = arg(2); return 3;
    case 0x0C:
      oval_.on = false;
      move_ticks_ = ctx.move_ticks ? ctx.move_ticks(arg(1), arg(2)) : 30;
      emit(Event::MoveSpeed, arg(1), arg(2), move_ticks_);
      return 3;
    case 0x13: oval_.on = false; emit(Event::MoveHome, 0, arg(2)); move_ticks_ = arg(2); return 3;
    case 0x6E: {  // JumpT: x = [v], z = [v+1], y = [v+2] over c ticks, ballistic y; the apex position goes to vars [arg(2)..+2]
      const int base = arg(1), ticks = std::max(1, arg(3));
      const int tx = s16v(rd(base)), tz = s16v(rd(base + 1)), ty = s16v(rd(base + 2));
      move_ticks_ = arg(3);
      emit(Event::Jump, tx, tz, ty, ticks);
      // out vars: the position after n / 2 ticks (x, z linear; y = y0 + vy h + g h^2 / 2 with h = n / 2, vy = dy / n - g h)
      const int half = ticks / 2;
      const double gravity = 40140.0 / 4096.0, dy = ty - ctx.pos_y, vy = dy / ticks - gravity * half;
      wr(arg(2), static_cast<int>(std::lround(ctx.pos_x + (tx - ctx.pos_x) / static_cast<double>(ticks) * half)));
      wr(arg(2) + 1, static_cast<int>(std::lround(ctx.pos_z + (tz - ctx.pos_z) / static_cast<double>(ticks) * half)));
      wr(arg(2) + 2, static_cast<int>(std::lround(ctx.pos_y + vy * half + gravity / 2.0 * half * half)));
      return 4;
    }
    case 0x81: {  // GetELPos list idx point out: the world position of the posed attach point into vars [out] x, [out+1] z, [out+2] y
      int x = 0, z = 0, y = 0;
      if (ctx.get_attach && ctx.get_attach(arg(1), arg(2), arg(3), x, z, y)) {
        wr(arg(4), x);
        wr(arg(4) + 1, z);
        wr(arg(4) + 2, y);
      }
      return 5;
    }
    case 0x7F: case 0x80: {  // JumpUpT / JumpDownT: x = [v], z = [v+1], y = [v+2], ticks = w2. x / z linear; y a parabola with
      // a = -2 dy / n^2 and vy0 = 2 dy / n (7F: rises fast and arrives at the target with vy 0 = a decelerating climb, Hair-Cutter's leap to y -2048) or
      // vy0 = 0 and a = 2 dy / n^2 (80: an accelerating fall / climb that arrives at full speed). The end position is snapped.
      // Both also write the x / z velocity, read back by op 0F.
      const int base = arg(1), ticks = std::max(1, arg(2));
      const int tx = s16v(rd(base)), tz = s16v(rd(base + 1)), ty = s16v(rd(base + 2));
      oval_.on = false;
      vel_x_ = (tx - ctx.pos_x) * 0x1000 / ticks;
      vel_z_ = (tz - ctx.pos_z) * 0x1000 / ticks;
      move_ticks_ = arg(2);
      emit(Event::Jump, tx, tz, ty, ticks, op == 0x7F ? 1u : 2u);
      return 3;
    }
    case 0x71: {  // PosMoveT: x = [v], z = [v+1], y (down) = [v+2]; velocity = (target - position) / ticks, kept for op 0F
      const int base = arg(1), ticks = arg(2);
      oval_.on = false;
      const int tx = s16v(rd(base)), tz = s16v(rd(base + 1)), ty = s16v(rd(base + 2));
      if (ticks != 0) {
        vel_x_ = (tx * 0x1000 - ctx.pos_x * 0x1000) / ticks;
        vel_z_ = (tz * 0x1000 - ctx.pos_z * 0x1000) / ticks;
      }
      emit(Event::MovePos, tx, tz, ty, ticks);
      move_ticks_ = ticks;
      return 3;
    }
    case 0x0D: {  // VecMoveT v ticks scale: velocity = (vars [v], [v+1]) * scale (20.12 per tick), the move ends at position + velocity * ticks
      const int base = arg(1), ticks = arg(2), scale = arg(3);
      oval_.on = false;
      vel_x_ = s16v(rd(base)) * scale;
      vel_z_ = s16v(rd(base + 1)) * scale;
      emit(Event::MovePos, ctx.pos_x + static_cast<int>((static_cast<long long>(vel_x_) * ticks) >> 12), ctx.pos_z + static_cast<int>((static_cast<long long>(vel_z_) * ticks) >> 12), ctx.pos_y, ticks);
      move_ticks_ = ticks;
      return 4;
    }
    case 0x0F: {  // GetVec: the unit vector (4096 = 1) of the current velocity -> vars [v] x, [v+1] z, [v+2] y
      const double dx = vel_x_ / 256.0, dz = vel_z_ / 256.0, length = std::hypot(dx, dz);
      const int base = arg(1);
      wr(base, length > 0 ? static_cast<int>(std::lround(dx / length * 4096.0)) : 0);
      wr(base + 1, length > 0 ? static_cast<int>(std::lround(dz / length * 4096.0)) : 0);
      wr(base + 2, 0);
      return 2;
    }
    case 0x78: {  // OvalMoveS list idx mode radius step out total: orbit the ellipse between the unit and the target point for [total] / |step| ticks
      int tx = 0, tz = 0;
      if (!ctx.get_pos || !ctx.get_pos(arg(1), arg(2), arg(3), tx, tz) || arg(5) == 0) return 8;
      const double dx = tx - ctx.pos_x, dz = tz - ctx.pos_z, dist = std::hypot(dx, dz);
      if (dist < 1.0) return 8;
      Oval& oval = oval_;
      oval.ax = dx / dist;
      oval.az = dz / dist;
      oval.rx = dist / 2.0;
      oval.rz = dist * arg(4) / 8192.0;
      oval.cx = ctx.pos_x + oval.ax * oval.rx;
      oval.cz = ctx.pos_z + oval.az * oval.rx;
      oval.step = s16v(arg(5));
      oval.out = arg(6);
      oval.angle = static_cast<s16>(oval.step * std::abs(s16v(counter_)));
      oval.left = std::max(1, s16v(counter_) + s16v(rd(arg(7))) / std::abs(oval.step));
      oval.on = true;
      move_ticks_ = oval.left;
      return 8;
    }
    case 0x74: {  // GetPos list idx mode out: world x / z (and y 0) of an actor into vars [out], [out+1], [out+2]
      int x = 0, z = 0;
      if (ctx.get_pos && ctx.get_pos(arg(1), arg(2), arg(3), x, z)) {
        wr(arg(4), x);
        wr(arg(4) + 1, z);
        wr(arg(4) + 2, ctx.get_pos_y ? ctx.get_pos_y(arg(1), arg(2)) : 0);
      }
      return 5;
    }
    case 0x7C: {  // GetApproachPos list idx out: the engaged spot = this unit + the direction to the target * (distance - both reaches,
      // attach 15 / 16) = GetPos mode 5; vars [out] x, [out+1] z, [out+2] y. Every enemy physical attack
      // pack (sid_tuki, asi_doku, mgarai1, paresol ...) runs in with `7C 1 0 13` + PosMoveT 13.
      int x = 0, z = 0;
      if (ctx.get_pos && ctx.get_pos(arg(1), arg(2), 5, x, z)) {
        wr(arg(3), x);
        wr(arg(3) + 1, z);
        wr(arg(3) + 2, ctx.pos_y);
      }
      return 4;
    }
    case 0x99: {  // LookPos v n: turn toward the point (vars [v] x, [v+1] z) over n ticks; the unit's velocity is set to the
      // normalised direction to it, which op 0F GetVec then returns (Lunge / dashtuki: GetVec after LookPos = the dash direction; yaw = ratan2 - 0x800)
      const int px = s16v(rd(arg(1))), pz = s16v(rd(arg(1) + 1));
      const double dx = px - ctx.pos_x, dz = pz - ctx.pos_z, length = std::hypot(dx, dz);
      if (length >= 1.0) {
        vel_x_ = static_cast<int>(dx / length * 4096.0 * 256.0);
        vel_z_ = static_cast<int>(dz / length * 4096.0 * 256.0);
        const int yaw = static_cast<int>(std::lround(std::atan2(dx, dz) * 4096.0 / 6.283185307179586)) - 0x800;
        emit(Event::Rotate, yaw & 0xFFF, std::max(1, arg(2)));
      }
      return 3;
    }
    case 0x98: {  // LinkEid list idx ownAttach otherAttach x y z ticks: the OTHER unit is moved every tick so that its attach point sits on this unit's
      Event event;
      event.kind = Event::Link;
      event.a = arg(1);
      event.b = arg(2);
      event.c = arg(3);
      event.d = arg(4);
      event.off[0] = static_cast<s16>(arg(8));
      out.push_back(event);
      return 9;
    }
    case 0x9A: {  // SetPos v: the unit is put at (vars [v], [v+1], [v+2]) at once
      const int base = arg(1);
      emit(Event::SetPos, s16v(rd(base)), s16v(rd(base + 1)), s16v(rd(base + 2)));
      if (live_) live_->pos_x = s16v(rd(base)), live_->pos_z = s16v(rd(base + 1)), live_->pos_y = s16v(rd(base + 2));
      return 2;
    }
    case 0x97: {  // GetApproachPos2 list idx out attach: as 7C with the attach point of the walker given (15 for most packs, kage_atk 19)
      int x = 0, z = 0;
      if (ctx.get_pos && ctx.get_pos(arg(1), arg(2), 0x100 + (arg(4) & 0xFF), x, z)) {
        wr(arg(3), x);
        wr(arg(3) + 1, z);
        wr(arg(3) + 2, ctx.pos_y);
      }
      return 5;
    }
    case 0x9B: {  // GetLenPos list idx ang len out: the actor's position moved len units back along angle [ang]: x - sin * len, z - cos * len
      int x = 0, z = 0;
      if (ctx.get_pos && ctx.get_pos(arg(1), arg(2), 0, x, z)) {
        const double radians = s16v(rd(arg(3))) * 6.283185307179586 / 4096.0;
        wr(arg(5), x - static_cast<int>(std::lround(std::sin(radians) * arg(4))));
        wr(arg(5) + 1, z - static_cast<int>(std::lround(std::cos(radians) * arg(4))));
        wr(arg(5) + 2, ctx.get_pos_y ? ctx.get_pos_y(arg(1), arg(2)) : 0);
      }
      return 6;
    }
    case 0x84: {  // GetRaitoPos list idx mode out ratio: the point `ratio / 4096` of the way from this unit to the GetPos point
      // (same list / idx / mode); vars [out] x, [out+1] z, [out+2] y. The enemy physical attacks (sun_kiss ratio 3396 = 0.829, the engaged spot in front of the
      // target) run in with PosMoveT to it.
      int tx = 0, tz = 0;
      if (ctx.get_pos && ctx.get_pos(arg(1), arg(2), arg(3), tx, tz)) {
        const int ty = ctx.get_pos_y ? ctx.get_pos_y(arg(1), arg(2)) : 0;
        const long long ratio = arg(5);
        wr(arg(4), ctx.pos_x + static_cast<int>(((tx - ctx.pos_x) * ratio) / 4096));
        wr(arg(4) + 1, ctx.pos_z + static_cast<int>(((tz - ctx.pos_z) * ratio) / 4096));
        wr(arg(4) + 2, ctx.pos_y + static_cast<int>(((ty - ctx.pos_y) * ratio) / 4096));
      }
      return 6;
    }
    // Everything else: a length-correct no-op (see the header).
    default: return len;
  }
}

}  // namespace oc::game::bseq
