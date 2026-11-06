// FieldWorld: music, sound effects and movies. The BGM goes through the mixer (remastered streams or the original AKAO sequences),
// the scripted sounds through the Sfx sink (C6 slots with their volume / pan slides, the footsteps of the walking actors), and a
// movie is handed to the host as a request it reports back on.
#include "game/field_world.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "audio/bgm_table.h"
#include "audio/hca_stream.h"
#include "core/log.h"
#include "core/timer.h"
#include "game/field_world_internal.h"

namespace oc::game {

using namespace field_internal;

namespace {

// Footstep sound of floor type `type` (0x11 dry / 0x12 water registers) for walker `who` (the party member's character id, or the
// value a script set), `left` = foot event bit 0. Values >= 0x100 are effect00 program ids & 0xFF, smaller ones room frames.
int footstep_sound(int type, int who, int left) {
  switch (type) {
    case 0:
    case 1:
      if (who == 0x0C) return left + 0x12E;
      if (who == 0x19) return left + 0x1BB;
      return left + (type == 0 ? 0x102 : 0x12A);
    case 2: return left + 0x12C;
    case 3: return left;
    case 4: return left + 2;
    default: return 0;
  }
}

}  // namespace

audio::Sfx& FieldWorld::sfx_out() { return sfx_ ? *sfx_ : static_cast<audio::Sfx&>(log_sfx_); }

void FieldWorld::set_audio(audio::Mixer* mixer, audio::Sfx* sfx) {
  mixer_ = mixer;
  sfx_ = sfx;
}

// Live switch of the BGM source: the song that plays now continues on the other source at the same musical position, with its
// volume / fade state. `restart` = false when a battle owns the music (it switched that one itself).
void FieldWorld::set_bgm_remastered(bool on, bool restart) {
  if (on == bgm_remastered_) return;
  bgm_remastered_ = on;
  pending_song_ = -1;
  if (mixer_ && restart) mixer_->switch_bgm_source(on);
}

// Starts `song` unless it already plays (then only its volume is re-armed, so a song carries across rooms without a gap).
void FieldWorld::start_song(int song, int volume, double start_seconds) {
  if (!mixer_ || !files_) return;
  if (mixer_->current_song() == song || pending_song_ == song) {
    mixer_->fade(volume, 0.0);
    return;
  }
  if (!bgm_remastered_) {
    log::debug("audio: tick {} play song {} (AKAO) volume {}", tick_count_, song, volume);
    mixer_->play_akao_bgm(song, volume);
    if (mixer_->device_open()) {
      pending_song_ = song;
      pending_ticks_ = 0;
    }
    return;
  }
  if (play_stream(song, volume, start_seconds) && mixer_->device_open()) {
    pending_song_ = song;
    pending_ticks_ = 0;
  }
}

// The remastered stream of `song` on the mixer; false when it cannot be loaded.
bool FieldWorld::play_stream(int song, int volume, double start_seconds) {
  const audio::BgmEntry* entry = audio::bgm_entry(song);
  if (!entry) {
    log::warn("audio: song {} has no BGM entry", song);
    return false;
  }
  std::optional<Bytes> bytes;
  {
    ScopedTimer read_timer("audio: bgm zip read");
    bytes = files_->read(audio::bgm_path(entry->file_no));
  }
  auto stream = std::make_shared<audio::HcaStream>();
  ScopedTimer open_timer("audio: bgm HcaStream::open");
  if (!bytes || !stream->open(*bytes)) {
    log::warn("audio: could not load {}", audio::bgm_path(entry->file_no));
    return false;
  }
  log::debug("audio: tick {} play song {} (BGM{:03}) volume {}", tick_count_, song, entry->file_no, volume);
  cue_song_ = -1;
  mixer_->play(std::move(stream), song, volume, start_seconds);
  return true;
}

// PLAY_SONG always starts at full volume; only FE 78 keeps its own volume operand.
bool FieldWorld::play_song(int song) { return play_song_at(song, 127); }

bool FieldWorld::play_song_ex(int song, int volume) {
  log::debug("field: tick {} FE 78 PLAY_SONG_EX song {} volume {}", tick_count_, song, volume & 0x7F);
  return play_song_at(song, volume & 0x7F, true);
}

namespace {

// Song change while another song is registered (60 Hz ticks from the request): the music volume slides linearly to 0 over 32 ticks
// (PLAY_SONG) or 16 (FE 78), the old song is cut at tick 18 and the new one starts at tick 22.
constexpr int kSongFadeTicks = 32, kSongShortFadeTicks = 16, kSongCutTick = 18, kSongStartTick = 22;

}  // namespace

// Song 0xFF is a STOP (scripts that want silence use it); 0xFFFF (FE 6E's "no song was playing") starts nothing. While a song change
// runs no other request is taken (false: the op tries again next tick); the stop always is.
bool FieldWorld::play_song_at(int song, int volume, bool short_fade) {
  if (song == 0xFF) {
    log::debug("audio: tick {} PLAY_SONG 0xFF: stop the music", tick_count_);
    if (mixer_) mixer_->stop();
    last_song_ = -1;
    pending_song_ = -1;
    song_change_ = {};
    return true;
  }
  if (song < 0 || song > 0xFF) return true;
  if (song_change_.song >= 0) return false;
  song_volume_ = std::clamp(volume, 0, 127);
  // The script's own song while a music cue replaced it: the cue's song stays and only takes the volume.
  if (cue_song_ >= 0 && song == last_song_ && mixer_ && mixer_->current_song() == cue_song_) {
    bgm_volume_ = song_volume_;
    mixer_->fade(song_volume_, 0.0);
    return true;
  }
  const bool change = mixer_ && last_song_ >= 0 && mixer_->current_song() != song && pending_song_ != song;
  last_song_ = song;
  bgm_volume_ = song_volume_;
  if (!change) {
    start_song(song, song_volume_);
    return true;
  }
  log::debug("audio: tick {} song change to {}: fade-out over {} ticks", tick_count_, song, short_fade ? kSongShortFadeTicks : kSongFadeTicks);
  mixer_->fade(0, (short_fade ? kSongShortFadeTicks : kSongFadeTicks) / 60.0);
  song_change_ = {song, song_volume_, 0};
  return true;
}

void FieldWorld::step_song_change() {
  if (song_change_.song < 0) return;
  ++song_change_.ticks;
  if (song_change_.ticks == kSongCutTick && mixer_) mixer_->stop();
  if (song_change_.ticks < kSongStartTick) return;
  const SongChange change = std::exchange(song_change_, {});
  start_song(change.song, change.volume);
}

// WAIT_SONG: true while a song change runs or the mixer has not yet confirmed the new song (cleared in end_of_tick, with a timeout).
bool FieldWorld::song_changing() { return pending_song_ >= 0 || song_change_.song >= 0; }

namespace {

// Remastered-soundtrack cues: script points RDE times to the streamed music instead of the script's own count. Active only with the
// remastered soundtrack.
// A WAIT of `ticks` while `song` is the requested song ends once the stream position (the song clock modulo the track length) has
// reached `seconds`; until then it holds.
struct WaitCue {
  int song;
  int ticks;
  double seconds;
};
constexpr WaitCue kWaitCues[] = {{0x17, 0x159, 5.75}};

// Op `op` of room `room` whose code word 4 bytes on is `word`: unless `song` already plays, the music fades to 0 over `fade_out`
// ticks while the op holds, the op holds `hold` more ticks, then `song` starts at volume 0 and fades to full over `fade_in` ticks and
// the op runs. The script's own song state is not changed.
struct SongCue {
  std::string_view room;
  int op;
  u32 word;
  int song;
  int fade_out, hold, fade_in;
};
constexpr SongCue kSongCues[] = {{"hes_a04f", 0x86, 0x36012B, 8, 30, 72, 45}};

}  // namespace

int FieldWorld::music_wait(int ticks) {
  if (!bgm_remastered_ || !mixer_ || !mixer_->live()) return -1;
  for (const WaitCue& cue : kWaitCues) {
    if (cue.song != last_song_ || cue.ticks != ticks) continue;
    const audio::BgmEntry* entry = audio::bgm_entry(cue.song);
    if (!entry || mixer_->current_song() != cue.song || !mixer_->sounding()) return 0;
    const double length = static_cast<double>(entry->total_samples) / audio::Mixer::kRate;
    return std::fmod(mixer_->bgm_seconds(), length) >= cue.seconds ? 1 : 0;
  }
  return -1;
}

bool FieldWorld::music_hold(int op, u32 word) {
  if (!bgm_remastered_ || !mixer_ || !mixer_->live()) return false;
  for (const SongCue& cue : kSongCues) {
    if (cue.op != op || cue.word != word || cue.room != view_.room_name()) continue;
    if (song_cue_.step == 0 && mixer_->current_song() != cue.song) {
      mixer_->fade(0, cue.fade_out / 60.0);
      song_cue_ = {1, 0};
      return true;
    }
    if (song_cue_.step == 1 && song_cue_.ticks < cue.hold) {
      ++song_cue_.ticks;
      return true;
    }
    if (song_cue_.step == 1 && play_stream(cue.song, 0, 0.0)) {
      cue_song_ = cue.song;
      mixer_->fade(127, cue.fade_in / 60.0);
    }
    song_cue_ = {};
    return false;
  }
  return false;
}

// C6: id < 256 = frame of the room's yos1 list, id >= 256 = effect00 id - 256. The extended form (mode != 0) is `C6 mode id PAN VOLUME
// slot`: operand 4 (& 0xFF, default 0x80) is the channel pan, operand 6 (& 0x7F, default 0x7F) the volume, operand 8 (& 7) the channel
// slot; the short form (mode 0) is `C6 0 id slot` with the defaults. The slot's running sounds are evicted first, then the sound plays
// tagged with the slot's mask, and a new sound ends the slot's volume / pan slides.
void FieldWorld::play_sfx(int mode, int id, int op4, int op6, int op8) {
  const int pan8 = mode != 0 ? (op4 & 0xFF) : 0x80;
  const int volume = mode != 0 ? (op6 & 0x7F) : 0x7F;
  const int slot = (mode != 0 ? op8 : op4) & 7;
  log::debug("field: tick {} PLAY_SFX id {} mode {} pan {:#x} volume {} slot {} (the slot's running sounds are evicted first)", tick_count_, id,
             mode, pan8, volume, slot);
  sfx_out().play_field(id, volume, pan8 >> 1, 1u << (slot + 8));
  sfx_vol_[static_cast<size_t>(slot)] = {static_cast<float>(volume), static_cast<float>(volume), 0, 0};
  sfx_pan_[static_cast<size_t>(slot)] = {static_cast<float>(pan8 >> 1), static_cast<float>(pan8 >> 1), 0, 0};
}

// FE 22 slot, frames, volume: a volume slide of every sound started with the slot's channel mask, from the volume the C6 set.
void FieldWorld::sfx_volume(int slot, int frames, int volume) {
  SfxSlide& slide = sfx_vol_[static_cast<size_t>(slot & 7)];
  slide.target = static_cast<float>(std::clamp(volume, 0, 127));
  slide.left = std::max(0, frames);
  slide.step = slide.left > 0 ? (slide.target - slide.cur) / static_cast<float>(slide.left) : 0.0f;
  if (slide.left == 0) slide.cur = slide.target;
  sfx_out().set_field_mods(1u << ((slot & 7) + 8), static_cast<int>(std::lround(slide.cur)), -1);
}

// FE 23: the same slide for the pan (64 = centre).
void FieldWorld::sfx_pan(int slot, int frames, int pan) {
  SfxSlide& slide = sfx_pan_[static_cast<size_t>(slot & 7)];
  slide.target = static_cast<float>(std::clamp(pan, 0, 127));
  slide.left = std::max(0, frames);
  slide.step = slide.left > 0 ? (slide.target - slide.cur) / static_cast<float>(slide.left) : 0.0f;
  if (slide.left == 0) slide.cur = slide.target;
  sfx_out().set_field_mods(1u << ((slot & 7) + 8), -1, static_cast<int>(std::lround(slide.cur)));
}

void FieldWorld::step_sfx_slides() {
  audio::Sfx& out = sfx_out();
  for (size_t slot = 0; slot < 8; ++slot) {
    const u32 mask = 1u << (slot + 8);
    if (sfx_vol_[slot].left > 0) {
      SfxSlide& slide = sfx_vol_[slot];
      slide.cur = --slide.left == 0 ? slide.target : slide.cur + slide.step;
      out.set_field_mods(mask, static_cast<int>(std::lround(slide.cur)), -1);
    }
    if (sfx_pan_[slot].left > 0) {
      SfxSlide& slide = sfx_pan_[slot];
      slide.cur = --slide.left == 0 ? slide.target : slide.cur + slide.step;
      out.set_field_mods(mask, -1, static_cast<int>(std::lround(slide.cur)));
    }
  }
}

// A menu screen over the field (main menu, save list, shops, name entry, party): when it opens every running field sound slides to
// volume 0 over 32 frames and keeps running; when it closes the C6 slots' sounds slide back over 30 frames to the volume their
// slot was set to (the C6 volume, or the FE 22 target, whose slide ends there). The other sounds stay silent.
void FieldWorld::fade_screen_sounds(bool opening) {
  audio::Sfx& out = sfx_out();
  if (opening) {
    out.fade_field(0xFFFFFF, 0x20, 0);
    return;
  }
  for (size_t slot = 0; slot < 8; ++slot) {
    SfxSlide& slide = sfx_vol_[slot];
    slide.cur = slide.target;
    slide.left = 0;
    out.fade_field(1u << (slot + 8), 0x1E, static_cast<int>(std::lround(slide.target)));
  }
}

// FE 1D frames, volume: fade the music to `volume` (0..127) over `frames` (0 counts as 1). FE 76 reads the target back.
void FieldWorld::fade_master_volume(int frames, int volume) {
  const int target = std::clamp(volume, 0, 127);
  bgm_volume_ = target;
  log::debug("field: tick {} FE 1D fade music to {} over {} frames", tick_count_, target, std::max(1, frames));
  if (mixer_) mixer_->fade(target, static_cast<double>(std::max(1, frames)) / 60.0);
}

// 7E / D4: movie number -> file through the index of the disc in the drive. The host plays it and calls movie_finished(); 7F WAIT_MOVIE
// polls movie_playing(). Numbers whose record is no movie finish at once.
void FieldWorld::play_movie(int index, int flags) {
  if (movie_state_ != MovieState::None) {
    log::warn("field: tick {} movie row {} ignored in {}: the previous movie is still {}", tick_count_, index, view_.room_name(),
              movie_state_ == MovieState::Requested ? "waiting for the host" : "playing");
    return;
  }
  const std::string name = discs_.movie_name(disc_, index);
  // Flag 8 (any D4 in the data, or a movie that ends in a recorded DC jump) fades the last frame out; without it the frame is held.
  const bool fade = ((flags | (movie_jump_.active ? 8 : 0)) & 8) != 0;
  movie_req_ = {index, fade, name};
  log::info("field: tick {} movie {} ({}) requested{} in {} (disc {})", tick_count_, index, name.empty() ? "no file" : name,
            fade ? ", fades out" : ", held at the end", view_.room_name(), disc_);
  if (name.empty()) {
    start_movie_jump();
    return;
  }
  movie_state_ = MovieState::Requested;
}

bool FieldWorld::movie_playing() { return movie_state_ != MovieState::None; }

FieldWorld::MovieRequest FieldWorld::take_movie() {
  movie_state_ = MovieState::Playing;
  movie_sequence_.clear();
  if (sequences_ && movie_req_.index >= 0) {
    const std::string name = std::string("mo") + (disc_ == 1 ? '1' : '2') + static_cast<char>('a' + movie_req_.index);
    if (sequences_->start(name)) {
      movie_sequence_ = name;
      log::info("field: tick {} movie {} starts room sequence {}", tick_count_, movie_req_.index, name);
    }
  }
  return movie_req_;
}

void FieldWorld::movie_tick() {
  if (sequences_ && !movie_sequence_.empty()) sequences_->tick();
}

void FieldWorld::movie_finished() {
  movie_state_ = MovieState::None;
  if (sequences_ && !movie_sequence_.empty()) sequences_->stop(movie_sequence_);
  movie_sequence_.clear();
  start_movie_jump();
}

// Room init defaults of the footstep registers: master volume 0x7F, distance band 0x7FFF / 0x7FFF, listener 0. The floor types keep
// their value.
void FieldWorld::reset_room_sound_regs() {
  const FootstepRegs defaults;
  footstep_.volume = defaults.volume;
  footstep_.listener_x = defaults.listener_x, footstep_.listener_y = defaults.listener_y, footstep_.listener_z = defaults.listener_z;
  footstep_.nearest = defaults.nearest, footstep_.farthest = defaults.farthest;
}

// Footstep of an actor whose model raised a foot event (bits 4 / 5 = left / right): only for the controlled entity and entities with
// the FE 40 / 41 / 5C bit, never on stairs. The sound is the entity's own (FE 5C), else the floor type's (dry / water register) for the
// walker's character id. Pan follows the actor's screen x; the volume falls from 0x7F at the near distance to 0 at the far one, measured
// from the listener's y / z (its x is ignored), times the master volume. The two footstep channels alternate, each evicting its
// previous sound.
void FieldWorld::play_footstep(int ent, int feet) {
  if (!(feet & 0x30) || !valid_ent(ent)) return;
  const Ent& entity = ents_[static_cast<size_t>(ent)];
  if (!entity.actor.loaded() || entity.actor.triangle() < 0) return;
  const script::Entity& vm_ent = vm_.entity(ent);
  if (vm_ent.flags38 & 0x10C020) return;
  const u32 tri_flags = view_.walk().flags(entity.actor.triangle());
  if (tri_flags & 0xC000) return;
  if (ent != player_entity() && !(vm_ent.flags270 & 0x1000000u)) return;
  int id = vm_ent.array25a[((feet >> 4) - 1) & 1];
  if (id == 0xFF) {
    const int type = static_cast<s16>((tri_flags & 0x60000) ? footstep_.water_type : footstep_.dry_type);
    const int who = entity.pc_slot >= 0 && entity.char_id >= 0 ? entity.char_id : vm_ent.v116;
    if (type == 0xFF || who == 0xFFFF) return;
    id = footstep_sound(type, who & 0x3F, feet & 1);
  }
  if (id == 0xFF) return;
  const ActorPose& pose = entity.actor.current();
  int screen_x = 160;
  render::FieldCamera::Projected projected;
  if (view_.camera().project({pose.x, pose.y, pose.z}, projected)) screen_x = static_cast<int>(std::lround(projected.sx)) + view_.scroll_x();
  const int pan8 = std::clamp(((screen_x * 0x6666) >> 16) + 0x40, 0, 0xFF);
  const s64 dx = std::lround(pose.x);
  const s64 dy = static_cast<s16>(footstep_.listener_y) - std::lround(pose.y);
  const s64 dz = static_cast<s16>(footstep_.listener_z) - std::lround(pose.z);
  const int distance = static_cast<int>(std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz)));
  const s32 nearest = footstep_.nearest, farthest = footstep_.farthest;
  int vol = 0;
  if (distance <= farthest) {
    vol = 0x7F;
    if (nearest <= distance) {
      s32 range = farthest - nearest;
      if (range == 0) range = 1;
      const s32 prod = static_cast<s32>(static_cast<u32>(distance - nearest) * static_cast<u32>(0x800000 / range));
      vol = 0x7F - std::min(0x7F, prod >> 16);
    }
  }
  s32 master = static_cast<s32>(static_cast<s16>(footstep_.volume)) * 0x10000;
  if (master < 0) master += 0x7F;
  const int volume = static_cast<int>((static_cast<s64>(vol) * (master >> 7)) >> 16) & 0x7F;
  const u32 mask = 1u << ((fstat_.step_voice & 1) + 6);
  ++fstat_.step_voice;
  log::debug("field: tick {} ent {} footstep sound {:#x} pan {:#x} volume {} mask {:#x}", tick_count_, ent, id, pan8, volume, mask);
  sfx_out().play_field(id, volume, pan8 >> 1, mask);
}

}  // namespace oc::game
