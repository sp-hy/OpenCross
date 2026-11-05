#include "audio/sfx.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>

#include "audio/byte_io.h"
#include "audio/mixer.h"
#include "core/log.h"
#include "vfs/vfs.h"

namespace oc::audio {

void LogSfx::play(int mode, int id, int volume, int pan, int extra) {
  if (logged_++ < 16) log::debug("sfx: (stub) play mode {} id {} vol {} pan {} extra {}", mode, id, volume, pan, extra);
}

namespace {
// Splits a yos0/yos1 room subfile: tag4, u32 size-8, u32 count, u32 off[count+1] (relative to byte 8). Empty slots dropped
// only when `keep_empty` is false.
std::vector<ByteSpan> split_yos(ByteSpan data, bool keep_empty) {
  std::vector<ByteSpan> out;
  if (data.size() < 16) return out;
  const u32 count = read_u32(data, 8);
  if (count > 64 || 12 + size_t(count + 1) * 4 > data.size()) return out;
  out.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    const size_t begin = size_t(read_u32(data, 12 + size_t(i) * 4)) + 8, end = size_t(read_u32(data, 16 + size_t(i) * 4)) + 8;
    if (begin < end && end <= data.size() && end - begin >= 4 && std::memcmp(&data[begin], "AKAO", 4) == 0)
      out.push_back(data.subspan(begin, end - begin));
    else if (keep_empty)
      out.emplace_back();
  }
  return out;
}

// C6 volume 0..127 -> modifier (1.0 = 0x7F); pan 0..127 (-1 = the programme's own) -> the modifier the channel takes.
float sfx_volume_modifier(int volume) { return std::clamp(float(volume) / 127.0f, 0.0f, 1.5f); }
int sfx_pan_modifier(int pan) { return pan >= 0 ? (pan & 0x7F) : -1; }

// Runs of slots 176..239 by the bank id their entry came from, '*' = its sample RAM was overwritten by a later upload
// ("176-207:1* 208-223:201 ..."; diagnostics).
std::string slot_banks(const InstrumentTable& table) {
  std::string out;
  auto key = [&](int slot) {
    const Instrument* inst = table.peek(slot);
    return inst ? std::format("{}{}", inst->bank_id, inst->overwritten ? "*" : "") : std::string("-");
  };
  int run_start = 176;
  for (int slot = 177; slot <= 240; ++slot) {
    if (slot <= 239 && key(slot) == key(run_start)) continue;
    out += std::format("{}{}-{}:{}", out.empty() ? "" : " ", run_start, slot - 1, key(run_start));
    run_start = slot;
  }
  return out;
}
}  // namespace

bool SfxBank::load(const vfs::Vfs& fs) {
  auto effect00 = fs.read("cdrom/sound/effect/common/effect00.dat");
  auto wave0000 = fs.read("cdrom/sound/wave/wave0000.dat");
  if (!effect00 || !wave0000) return false;
  return load(std::move(*effect00), *wave0000);
}

bool SfxBank::load(Bytes effect00, const Bytes& wave0000) {
  if (effect00.size() < 0x810 || std::memcmp(effect00.data(), "AKAO", 4) != 0) return false;
  auto bank = parse_bank(wave0000);
  if (!bank) return false;
  pcs_.resize(256);
  chain_.resize(256);
  for (size_t i = 0; i < 256; ++i) {
    pcs_[i] = {read_u16(&effect00[0x10 + i * 4]), read_u16(&effect00[0x12 + i * 4])};
    chain_[i] = read_u16(&effect00[0x610 + i * 2]);
  }
  auto table = std::make_shared<InstrumentTable>();
  table->overlay(bank);
  base_ = table;
  table_ = table;
  room_banks_.clear();
  effect_ = std::make_shared<AkaoCode>();
  effect_->data = std::move(effect00);
  frames_.clear();
  return true;
}

// The room's banks overwrite their slots of the table the previous loaders left; a room without banks keeps that table, so a global
// effect reading a room slot (208..223) plays the sample the last bank put there. A battle that ended since the last sound left its own
// banks on the table: the room being left uploads its banks again first (the field's resume), so a bankless room inherits them.
void SfxBank::set_room(ByteSpan yos0, ByteSpan yos1, std::shared_ptr<const InstrumentTable> current) {
  frames_.clear();
  if (!base_) return;
  if (current && current != table_) reload_room(current);
  room_banks_.clear();
  for (ByteSpan bank : split_yos(yos0, false))
    if (auto parsed = parse_bank(bank)) room_banks_.push_back(std::move(parsed));
  reload_room(table_);
  for (ByteSpan frame : split_yos(yos1, true)) {
    if (frame.empty()) {
      frames_.push_back(nullptr);
      continue;
    }
    auto code = std::make_shared<AkaoCode>();
    code->data.assign(frame.begin(), frame.end());
    frames_.push_back(std::move(code));
  }
}

void SfxBank::reload_room(std::shared_ptr<const InstrumentTable> current) {
  if (!current) return;
  if (room_banks_.empty()) {
    table_ = std::move(current);
    return;
  }
  auto table = std::make_shared<InstrumentTable>(*current);
  for (const auto& bank : room_banks_) table->overlay(bank);
  table_ = std::move(table);
}

int SfxBank::defined_ids() const {
  int count = 0;
  for (auto& pcs : pcs_) count += (pcs.first != 0xFFFF || pcs.second != 0xFFFF);
  return count;
}

// The instrument table is one global array: the room's yos0 bank overwrites instruments 208..223 and the effect00 programs use
// 208..211 (footsteps, etc.), so a global effect plays the sample of the last room that loaded a bank.
bool SfxBank::make_global(int id, SfxProgram& out, std::vector<SfxProgram>* extra) const {
  if (!effect_ || id < 0 || id > 255) return false;
  const auto [pc_a, pc_b] = pcs_[size_t(id)];
  out = SfxProgram{};
  out.code = effect_;
  out.table = table_ ? table_ : base_;
  out.tag = "g" + std::to_string(id);
  for (u16 rel : {pc_a, pc_b})
    if (rel != 0xFFFF && 0x810u + rel < effect_->data.size()) out.pc[out.count++] = 0x810u + rel;
  if (out.count == 0) return false;
  if (extra)
    for (int k = 1; k <= int(chain_[size_t(id)]) && id + k < 256; ++k) {
      SfxProgram chained;
      if (make_global(id + k, chained, nullptr)) extra->push_back(chained);
    }
  return true;
}

bool make_pointer_programs(const std::shared_ptr<const AkaoCode>& code, const std::shared_ptr<const InstrumentTable>& table, SfxProgram& first,
                           std::vector<SfxProgram>* extra) {
  first = SfxProgram{};
  if (!code || code->data.size() < 0x18) return false;
  const Bytes& data = code->data;
  const u32 entry_count = read_u32(&data[4]);
  if (entry_count == 0 || entry_count > 64 || 0x10 + size_t(entry_count) * 4 > data.size()) return false;
  const size_t base = 0x10 + size_t(entry_count) * 4;
  bool have_first = false;
  for (u32 k = 0; k < entry_count; ++k) {
    const size_t entry = base + read_u32(&data[0x10 + size_t(k) * 4]);
    if (entry + 4 > data.size()) continue;
    SfxProgram program;
    program.code = code;
    program.table = table;
    for (size_t i = 0; i < 2; ++i) {
      const u16 rel = read_u16(&data[entry + i * 2]);
      if (rel != 0xFFFF && entry + 4 + rel < data.size()) program.pc[program.count++] = u32(entry + 4 + rel);
    }
    if (program.count == 0) continue;
    if (!have_first) {
      first = std::move(program);
      have_first = true;
    } else if (extra) {
      extra->push_back(std::move(program));
    }
  }
  return have_first;
}

bool SfxBank::make_room(int frame, SfxProgram& out, std::vector<SfxProgram>* extra) const {
  if (frame < 0 || size_t(frame) >= frames_.size() || !frames_[size_t(frame)]) return false;
  if (!make_pointer_programs(frames_[size_t(frame)], table_, out, extra)) return false;
  out.tag = "r" + std::to_string(frame);
  if (extra)
    for (SfxProgram& program : *extra) program.tag = out.tag;
  return true;
}

std::shared_ptr<const SfxTriggers> make_sfx_triggers(const SfxBank& bank, const AkaoSeq& seq) {
  auto triggers = std::make_shared<SfxTriggers>();
  for (int id : akao_sfx_triggers(seq)) {
    SfxProgram first;
    std::vector<SfxProgram> programs;
    if (!bank.make_global(id, first, &programs)) continue;
    programs.insert(programs.begin(), first);
    triggers->programs.emplace_back(id, std::move(programs));
  }
  return triggers->programs.empty() ? nullptr : triggers;
}

void SfxTriggers::play(AkaoPlayer& sfx_player, const std::vector<AkaoPlayer::SfxRequest>& requests) const {
  for (const AkaoPlayer::SfxRequest& request : requests) {
    const auto found = std::find_if(programs.begin(), programs.end(), [&](const auto& entry) { return entry.first == request.id; });
    if (found == programs.end()) continue;
    sfx_player.evict_sfx(request.mask);
    for (SfxProgram program : found->second) {
      program.volume = float(request.volume & 0x7F) / 127.0f;
      program.pan = request.pan >> 1;
      program.mask = request.mask;
      sfx_player.start_sfx(program);
    }
  }
}

void AkaoSfx::stop_all() {
  if (mixer_) mixer_->stop_sfx();
}

void AkaoSfx::set_room(ByteSpan yos0, ByteSpan yos1) {
  if (!bank_) return;
  bank_->set_room(yos0, yos1, mixer_ ? mixer_->sfx_instruments() : nullptr);
  if (mixer_) mixer_->set_sfx_instruments(bank_->table());
}

void AkaoSfx::resume_shared() {
  const std::shared_ptr<const InstrumentTable>& shared = mixer_->sfx_instruments();
  if (!shared || shared == bank_->table()) return;
  bank_->reload_room(shared);
  mixer_->set_sfx_instruments(bank_->table());
  log::debug("sfx: room banks uploaded again over the table another loader left; slots 176..239 by bank id: {}", slot_banks(*bank_->table()));
}

void AkaoSfx::play_programs(SfxProgram& first, std::vector<SfxProgram>& extra, float volume, int pan, u32 mask) {
  extra.insert(extra.begin(), first);
  for (SfxProgram& program : extra) {
    program.volume = volume;
    program.pan = pan;
    program.mask = mask;
    mixer_->play_sfx(program);
  }
}

void AkaoSfx::play_room(int frame, int volume, int pan) {
  if (!mixer_ || !bank_) return;
  resume_shared();
  SfxProgram program;
  std::vector<SfxProgram> extra;
  if (!bank_->make_room(frame, program, &extra)) return;
  play_programs(program, extra, sfx_volume_modifier(volume), sfx_pan_modifier(pan), 0);
}

// The C6 pan is written into the channel for both forms (0x80 = centre by default).
void AkaoSfx::play(int /*mode*/, int id, int volume, int pan, int) {
  if (!mixer_ || !bank_) return;
  resume_shared();
  SfxProgram program;
  std::vector<SfxProgram> extra;
  if (!bank_->make_global(id, program, &extra)) return;
  play_programs(program, extra, sfx_volume_modifier(volume), sfx_pan_modifier(pan), 0);
}

void AkaoSfx::play_menu(int id, u32 mask) {
  if (!mixer_ || !bank_) return;
  SfxProgram program;
  std::vector<SfxProgram> extra;
  if (!bank_->make_global(id, program, &extra)) return;
  mixer_->evict_sfx(mask);
  play_programs(program, extra, 1.0f, -1, mask);
}

void AkaoSfx::set_field_mods(u32 mask, int volume, int pan) {
  if (mixer_) mixer_->set_sfx_mods(mask, volume, pan);
}

bool AkaoSfx::field_active(u32 mask) const { return mixer_ && mixer_->sfx_active(mask); }

void AkaoSfx::evict_field(u32 mask) {
  if (mixer_ && mask) mixer_->evict_sfx(mask);
}

// Untagged sounds (effects without a known mask) end with the room as well.
void AkaoSfx::leave_room(u32 keep_slots) {
  if (!mixer_) return;
  const u32 mask = (((keep_slots & 0xFF00) ^ 0xFF00) | room_masks_ | 0xFF00FF) & 0xFFFFFF;
  mixer_->evict_sfx(mask, true);
  room_masks_ = 0;
}

// A room sound records its mask in room_masks_, a global one clears it (the room-change eviction reads it).
void AkaoSfx::play_field(int id, int volume, int pan, u32 mask) {
  if (!mixer_ || !bank_) return;
  if (mask) mixer_->evict_sfx(mask);
  else mixer_->evict_sfx();
  if (id < 0x100) room_masks_ |= mask;
  else room_masks_ &= ~mask;
  resume_shared();
  SfxProgram program;
  std::vector<SfxProgram> extra;
  if (id < 0x100) {
    if (!bank_->make_room(id, program, &extra)) return;
  } else {
    if (!bank_->make_global(id & 0xFF, program, &extra)) return;
  }
  play_programs(program, extra, sfx_volume_modifier(volume), sfx_pan_modifier(pan), mask);
}

}  // namespace oc::audio
