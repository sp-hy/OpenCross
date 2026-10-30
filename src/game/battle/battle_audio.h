#pragma once
// Battle sound: the battle theme, the cast-time music ducking and the AKAO sound effects the animation scripts and effect sequences name by
// tag. Sounds are looked up like the engine's one resource list (Resident / Actor / Pack groups, newest first; closing a group cuts its
// sounds), the chant plays at the start of an element cast with mask 0x100 and the theme is ducked until the pack ends.
// Music: the field starts a battle with its battle song (the boss theme 0x31 with flag 0x40, else the battle theme 2 / 0x14) unless the
// battle request keeps the field music (flag 1). There is no victory voice; after a win the view fades the theme out and starts the
// victory song (BattleView::tick_finish).
#include <bitset>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/akao_bank.h"
#include "audio/akao_player.h"
#include "audio/mixer.h"
#include "audio/sfx.h"
#include "core/types.h"
#include "import/container.h"
#include "vfs/vfs.h"

namespace oc::game {

class BattleAudio {
 public:
  void init(audio::Mixer* mixer, const vfs::Vfs* vfs);
  // [Audio] BgmSource switch: a battle song this battle started is restarted from the other source (live switch).
  // Returns true when this battle owns the running music (the field must not switch it too).
  bool set_remastered(bool on);

  // Battle sounds carry this channel mask; stop_sounds() cuts what is still sounding (the engine frees the unit / effect AKAO files at the
  // end of the battle and ends the channels of each file, E0-protected ones at their loop end). Without it a long or looping sound
  // (lasdes2 se90 runs > 40 s) outlives the fight.
  static constexpr uint32_t kBattleMask = 0x80000000u;
  // Sounds of a group that leaves the list are cut with it (the channels running that file end): the actor's file at its action's end, the
  // pack's file at the pack's end.
  static constexpr uint32_t kActorMask = 0x20000000u, kPackMask = 0x40000000u;
  void stop_sounds();

  // Per battle.
  void begin_battle();
  // What a group is in the engine's list: Resident = opened for the whole battle (battle.prd, the stage); Actor = the acting unit's at0.prd
  // (one at a time: opened at its action's start, closed at its end); Pack = a spell's effect pack.
  enum class Kind { Resident, Actor, Pack };
  // Parses a drp once into a named sound group: every type 0x16 entry (recursing into nested drps) with the type 5 banks of its drp overlaid
  // on the shared table; duplicate tags inside the drp resolve like the engine's list (entry order below). `wp_class` >= 0: the
  // "<digit><s|m|l>.." entries of that class (falling back to lower digits) get "_" as first letter. `kind` defaults to Actor when
  // wp_class >= 0 (a unit's at0.prd), else Resident.
  void load_group(const std::string& key, ByteSpan drp, int wp_class = -1) { load_group(key, drp, wp_class, wp_class >= 0 ? Kind::Actor : Kind::Resident); }
  void load_group(const std::string& key, ByteSpan drp, int wp_class, Kind kind);
  // Puts a loaded group at the head of the list. An Actor group first closes the previous actor's (its sounds are cut), a Pack group the
  // previous pack's; activating a group that is already in the list changes nothing.
  void activate(const std::string& key);
  // Closes a group: it leaves the list and the sounds it started are cut. end_pack() / end_actor() close the live one of that kind.
  void release(const std::string& key);
  void end_pack();
  void end_actor();
  bool has(const std::string& tag) const { return find(tag) != nullptr; }
  // The key of the live group a tag resolves to ("" = none); tools and tests.
  std::string resolve(const std::string& tag) const;
  // pan: the pan modifier / 2 (0..127, 64 = centre; the engine passes pan8 = 0x80 +- 0x30 for positional sounds), -1 = centre.
  // `mask` = the engine's channel mask (low 24 bits): a non-zero mask first ends the sounds that carry it, e.g. the cast chant "sing" / "msin"
  // uses 0x100, so the next chant replaces the previous one.
  bool play(const std::string& tag, int volume = 127, int pan = -1, uint32_t mask = 0);
  // Tools: renders a registered sound offline (44.1 kHz stereo float) without a mixer.
  // `hits` (optional) receives the instrument slots the sound read.
  bool render(const std::string& tag, std::vector<float>& pcm, int frames, int volume = 127, int pan = -1, std::bitset<256>* hits = nullptr) const;
  // The live instrument table (tools / tests).
  const audio::InstrumentTable& instrument_table() const { return *live_tbl_; }
  void play_global(int effect_id);
  // Battle menu sounds: mask 0x800000 (the Chrono Cross colour chime 0x400000), centre pan, full volume; a new one cuts the last of the same
  // mask.
  void play_menu(int effect_id, uint32_t mask = 0x800000);
  // The global SFX table (effect00.dat ids; null without a mixer / bank).
  audio::Sfx* sfx() { return sfx_.get(); }

  // Music.
  // Starts the battle song of the request's flags (audio::battle_song: the boss theme 0x31 with flag 0x40, else the battle theme 2 / 0x14);
  // with keep_field_music the field song continues instead.
  void start_battle_theme(int battle_flags, bool keep_field_music);
  // Music ducking of an element cast: the cast start fades the theme to 0x66 over 0x6D frames, the effect end back to 0x7F over 0x60 frames.
  // restore_cast() is a no-op unless duck_cast() ran.
  void duck_cast();
  void restore_cast();
  // Effect sequence op 0x54: the music fades to `volume` (low 7 bits) over `ticks` frames; the effect end's restore_cast() still applies.
  void fade_music(int volume, int ticks);
  // The pack opens (~1.85 s after the chant): the chant (mask 0x100) fades out over 0x3C frames and its channels stay silent until replaced.
  void fade_chant();
  // A one-shot theme (the victory songs 0x16 / 0x17).
  void play_song(int song);
  void stop_music();

  // Offline check of one sound (tools): plays every program of `tag` for `seconds` and reports when the last voice stopped and what
  // is still running at the end (a stuck note = a voice that never ends; parked = a channel whose program loops silently).
  struct Analysis {
    // End of the last audible voice.
    double last_voice_s = 0;
    int voices_left = 0, channels_left = 0;
    int programs = 0;
  };
  bool analyse(const std::string& tag, double seconds, Analysis& analysis) const;
  std::vector<std::string> tags() const;
  // Tools: the tags whose AKAO file contains the byte pattern (identify a sound seen in a PS1 RAM dump).
  std::vector<std::string> find_code(const std::vector<uint8_t>& pattern) const;

 private:
  struct Sound {
    std::shared_ptr<audio::AkaoCode> code;
  };
  struct Group {
    std::map<std::string, Sound> sounds;
    // The type 5 instrument banks of the file, in file order (loaded when it opens).
    std::vector<std::shared_ptr<const audio::Bank>> banks;
    Kind kind = Kind::Resident;
  };
  const Sound* find(const std::string& tag, const Group** from = nullptr) const;
  uint32_t group_mask(Kind kind) const { return kBattleMask | (kind == Kind::Actor ? kActorMask : kind == Kind::Pack ? kPackMask : 0u); }
  // Closes every live group of that kind.
  void release_kind(Kind kind);
  // Starts `song` on the current source; false (optionally with a warning) when the source cannot play it.
  bool start_song(int song, bool warn_on_failure);
  audio::Mixer* mixer_ = nullptr;
  const vfs::Vfs* vfs_ = nullptr;
  bool remastered_ = true;
  bool started_music_ = false;
  // True between duck_cast() and restore_cast().
  bool ducked_ = false;
  // The song this battle started (for the live BgmSource switch).
  int song_ = -1;
  // base_: wave0000 alone, the start table when no field published one (tools, the battle harness).
  // live_tbl_: the one live table, from the field's; every opened file's banks overlay it, nothing is restored.
  std::shared_ptr<const audio::InstrumentTable> base_;
  std::shared_ptr<audio::InstrumentTable> live_tbl_;
  bool make_program(const std::string& tag, audio::SfxProgram& program, std::vector<audio::SfxProgram>* extra = nullptr) const;
  void scan(ByteSpan drp, int wp_class, Group& group);
  void collect(const import::Drp& parsed, int wp_class, int digit, std::map<std::string, Sound>& out, std::vector<std::shared_ptr<const audio::Bank>>& banks);
  // groups_: every parsed group.
  // live_: the engine's resource list, oldest first (the back is the head that is searched first).
  std::map<std::string, Group> groups_;
  std::vector<std::string> live_;
  std::shared_ptr<audio::SfxBank> bank_;
  std::unique_ptr<audio::AkaoSfx> sfx_;
};

}  // namespace oc::game
