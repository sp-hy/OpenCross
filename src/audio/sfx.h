#pragma once
// Sound-effect interface used by the field script host (op C6 PLAY_SOUNDFX) and the battle sequencer; LogSfx records the requests
// when no audio is available, AkaoSfx plays the original AKAO effects.
#include <memory>
#include <vector>

#include "audio/akao_player.h"
#include "core/types.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::audio {

class Mixer;

class Sfx {
 public:
  virtual ~Sfx() = default;
  // C6: mode 0 = plain (id, volume), otherwise the extended form with pan and one more value.
  virtual void play(int mode, int id, int volume, int pan, int extra) = 0;
  // C6 with an id below 256: the current room's SFX frame (yos1 index). Ids from 256 are global (effect00 id - 256), see FieldWorld::play_sfx.
  virtual void play_room(int /*frame*/, int /*volume*/, int /*pan*/) {}
  // Silences everything (room change).
  virtual void stop_all() {}
  // One field sound: only the channels started with an overlapping channel mask end, then the sound tagged with `mask` starts:
  // id >= 0x100 = effect00 program id & 0xFF, else the room's yos1 frame. volume 0..127, pan 0..127 (64 = centre).
  // Masks: C6 PLAY_SFX 1 << (slot + 8), footsteps 0x40 / 0x80.
  virtual void play_field(int id, int volume, int pan, u32 mask) {
    (void)mask;
    if (id >= 0x100) play(1, id & 0xFF, volume, pan, 0);
    else play_room(id, volume, pan);
  }
  // Installs the current room's sound banks (subfile 10 / 9 bytes; empty = none).
  virtual void set_room(ByteSpan /*yos0*/, ByteSpan /*yos1*/) {}
  // Live volume / pan of the running sounds started with an overlapping mask (the positional sounds are updated every tick).
  // -1 keeps a value.
  virtual void set_field_mods(u32 /*mask*/, int /*volume*/, int /*pan*/) {}
  // A sound started with this mask is still running.
  virtual bool field_active(u32 /*mask*/) const { return false; }
  // C7: the sounds started with an overlapping channel mask end (no new sound).
  virtual void evict_field(u32 /*mask*/) {}
  // Field teardown (every room change): every sound ends except the C6 slots `keep_slots` (bits 8..15, FE 29 of the room being
  // left) that play a global (effect00) sound; the sounds whose last start was a room (yos1) sound end since their bank goes away.
  virtual void leave_room(u32 /*keep_slots*/) { stop_all(); }
};

// Default implementation: logs the first few requests, plays nothing.
class LogSfx final : public Sfx {
 public:
  void play(int mode, int id, int volume, int pan, int extra) override;

 private:
  int logged_ = 0;
};

// An AKAO file played by pointer (battle tag sounds, room yos1 frames). Header +4 = n, the u32 offsets [n] at +0x10 are relative
// to +0x10 + 4 n; each entry is u16 a, u16 b and the two programs start at entry + 4 + a / b (0xFFFF = none). Every entry is
// started; `first` gets the first entry that has a program, the others go to `extra`. False if the file holds none.
bool make_pointer_programs(const std::shared_ptr<const AkaoCode>& code, const std::shared_ptr<const InstrumentTable>& table, SfxProgram& first,
                           std::vector<SfxProgram>* extra);

// SFX data: the global program blob effect00.dat (+ shared bank wave0000) and the current room's yos0 banks / yos1 frames.
// Pure data, no audio device: make_global()/make_room() build the SfxProgram that Mixer::play_sfx() or an AkaoPlayer plays.
class SfxBank {
 public:
  // Loads effect00.dat and wave0000.dat from the game. False if either is missing.
  bool load(const vfs::Vfs& fs);
  // Same from raw images (tests/tools).
  bool load(Bytes effect00, const Bytes& wave0000);
  // Room sound from the room's subfile 10 (yos0, instrument banks) and subfile 9 (yos1, SFX frames). The frames replace the previous
  // room's; the banks are uploaded over `current` (the table the last loader left; null = this bank's own) and nothing restores
  // wave0000, so a room without banks keeps the last ones. When `current` is another loader's table the room being left uploads its
  // banks over it first, so the next room inherits them and not the other loader's.
  void set_room(ByteSpan yos0, ByteSpan yos1, std::shared_ptr<const InstrumentTable> current = nullptr);
  // The field resumes after another loader (a battle) wrote the shared table: the room's banks are uploaded again over `current`.
  void reload_room(std::shared_ptr<const InstrumentTable> current);
  // Battle: the live instrument table (the field's, then every opened battle bank) replaces the table for the global effects; effect00
  // programs read slots 177..181 (battle.prd's bank) and 208..211 (the stage / unit / pack bank). nullptr = wave0000 only.
  void set_table(std::shared_ptr<const InstrumentTable> table) { table_ = table ? std::move(table) : base_; }
  const std::shared_ptr<const InstrumentTable>& table() const { return table_; }

  // Global SFX ids (effect00 indices) run from 0 to kGlobalCount - 1.
  static constexpr int kGlobalCount = 256;
  // Global SFX id (effect00 index). Returns false if the id is undefined. Chained ids (chain_count) are appended via
  // `extra` (each a further program).
  bool make_global(int id, SfxProgram& out, std::vector<SfxProgram>* extra = nullptr) const;
  // Room-local SFX frame (index into yos1).
  bool make_room(int frame, SfxProgram& out, std::vector<SfxProgram>* extra = nullptr) const;

  bool loaded() const { return effect_ != nullptr; }
  int defined_ids() const;

 private:
  std::shared_ptr<AkaoCode> effect_;
  std::vector<u16> chain_;
  std::vector<std::pair<u16, u16>> pcs_;
  std::shared_ptr<const InstrumentTable> base_;  // wave0000
  std::shared_ptr<const InstrumentTable> table_;  // what every bank loaded so far left
  std::vector<std::shared_ptr<const Bank>> room_banks_;  // the current room's yos0 banks
  std::vector<std::shared_ptr<AkaoCode>> frames_;  // yos1 frames (null = empty slot)
};

// Sfx sink that plays the original AKAO effects through a Mixer (SFX bus). Not owning the mixer.
class AkaoSfx final : public Sfx {
 public:
  AkaoSfx(Mixer* mixer, SfxBank* bank) : mixer_(mixer), bank_(bank) {}
  // C6 PLAY_SOUNDFX: volume 0..127 (clamped), pan 0..127 (-1/absent keeps the programme's pan).
  void play(int mode, int id, int volume, int pan, int extra) override;
  void play(int id) { play(0, id, 127, -1, 0); }
  // Overlay menu sound: the menus use mask 0x40 (the field and battle exit 1, the battle 0x800000); the running sounds of the same
  // mask are evicted first, so a new cursor sound cuts the last.
  void play_menu(int id, u32 mask = 0x40);
  void play_room(int frame, int volume, int pan) override;
  void set_room(ByteSpan yos0, ByteSpan yos1) override;
  void stop_all() override;
  void play_field(int id, int volume, int pan, u32 mask) override;
  void set_field_mods(u32 mask, int volume, int pan) override;
  bool field_active(u32 mask) const override;
  void evict_field(u32 mask) override;
  void leave_room(u32 keep_slots) override;

 private:
  // Sets volume / pan / mask on every program and queues them on the mixer, `first` before `extra`.
  void play_programs(SfxProgram& first, std::vector<SfxProgram>& extra, float volume, int pan, u32 mask);
  // Before a field sound: when another loader (a battle) published a table since this bank's last upload, the room's banks are
  // uploaded again over it, as the field reloads its room after a battle.
  void resume_shared();

  Mixer* mixer_;
  SfxBank* bank_;
  u32 room_masks_ = 0;  // masks whose last field start was a room (yos1) sound
};

}  // namespace oc::audio
