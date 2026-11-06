// Battle sound: the AKAO sound groups of the open battle files (resident / acting unit / effect pack) searched newest first, the battle theme
// on either music source, the cast-time ducking and the offline renderers the tools use.
#include "game/battle/battle_audio.h"

#include <algorithm>
#include <cctype>

#include "audio/bgm_table.h"
#include "audio/hca_stream.h"
#include "core/log.h"
#include "import/container.h"

namespace oc::game {

namespace {

using audio::kPs1BattleSong;
using audio::kRdeBattleSong;

bool weapon_name(const std::string& name) {
  return name.size() == 4 && std::isdigit(static_cast<unsigned char>(name[0])) && (name[1] == 's' || name[1] == 'm' || name[1] == 'l');
}
// Does any type 0x16 entry of the drp tree carry the weapon-class digit ("<digit><s|m|l>..")?
bool has_weapon_digit(const import::Drp& drp, int digit) {
  for (const import::DrpEntry& entry : drp.entries) {
    if (entry.type == 0x16 && weapon_name(entry.name) && entry.name[0] == '0' + digit) return true;
    if (entry.type == 1 && import::is_drp(entry.data))
      if (auto sub = import::parse_drp(entry.data))
        if (has_weapon_digit(*sub, digit)) return true;
  }
  return false;
}

}  // namespace

void BattleAudio::init(audio::Mixer* mixer, const vfs::Vfs* vfs) {
  mixer_ = mixer;
  vfs_ = vfs;
  if (!vfs_) return;
  base_ = audio::load_akao_base_table(*vfs_);
  if (base_) live_tbl_ = std::make_shared<audio::InstrumentTable>(*base_);
  if (mixer_) {
    mixer_->set_vfs(vfs_);
    bank_ = std::make_shared<audio::SfxBank>();
    if (bank_->load(*vfs_)) sfx_ = std::make_unique<audio::AkaoSfx>(mixer_, bank_.get());
    else bank_.reset();
  }
}

bool BattleAudio::set_remastered(bool on) {
  if (on == remastered_) return false;
  remastered_ = on;
  if (!mixer_ || !started_music_ || song_ < 0) return false;
  // The field restored its own song over a finished battle's: nothing to switch.
  if (mixer_->playing() && mixer_->current_song() != song_) return false;
  int song = song_;
  if (on && song == kPs1BattleSong) song = kRdeBattleSong;
  else if (!on && song == kRdeBattleSong) song = kPs1BattleSong;
  // The jingle ran out: nothing to carry, the battle still owns the music.
  if (!mixer_->sounding()) return true;
  if (mixer_->switch_bgm_source(on, song)) song_ = song;
  return true;
}

void BattleAudio::stop_sounds() {
  if (mixer_) mixer_->evict_sfx(kBattleMask);
}

void BattleAudio::begin_battle() {
  stop_sounds();
  // The battle's banks upload over the table and sound RAM the field left (its room's banks, the last battle's battle.prd).
  const std::shared_ptr<const audio::InstrumentTable> field = mixer_ ? mixer_->sfx_instruments() : nullptr;
  if (field || base_) live_tbl_ = std::make_shared<audio::InstrumentTable>(*(field ? field : base_));
  if (bank_) bank_->set_table(live_tbl_ ? std::make_shared<audio::InstrumentTable>(*live_tbl_) : nullptr);
  live_.clear();
  groups_.clear();
  started_music_ = false;
  ducked_ = false;
}

void BattleAudio::load_group(const std::string& key, ByteSpan drp, int wp_class, Kind kind) {
  if (groups_.count(key)) return;
  Group group;
  group.kind = kind;
  scan(drp, wp_class, group);
  groups_[key] = std::move(group);
}

// Opening a file loads its instrument banks into the one live table: each bank overwrites the slots from its own first index, whatever an
// earlier file put there, and closing the file restores nothing. One acting unit's file and one pack are open at a time.
void BattleAudio::activate(const std::string& key) {
  auto it = groups_.find(key);
  if (it == groups_.end()) return;
  if (std::find(live_.begin(), live_.end(), key) != live_.end()) return;
  const Kind kind = it->second.kind;
  if (kind != Kind::Resident) release_kind(kind);
  live_.push_back(key);
  if (live_tbl_ && !it->second.banks.empty()) {
    for (const auto& bank : it->second.banks) live_tbl_->overlay(bank);
    // Published: the field resumes on what the battle's last upload left.
    auto published = std::make_shared<const audio::InstrumentTable>(*live_tbl_);
    if (bank_) bank_->set_table(published);
    if (mixer_) mixer_->set_sfx_instruments(published);
  }
}

void BattleAudio::release(const std::string& key) {
  auto at = std::find(live_.begin(), live_.end(), key);
  if (at == live_.end()) return;
  live_.erase(at);
  const Kind kind = groups_[key].kind;
  if (kind != Kind::Resident && mixer_) mixer_->evict_sfx(kind == Kind::Actor ? kActorMask : kPackMask);
}

void BattleAudio::release_kind(Kind kind) {
  std::vector<std::string> old;
  for (const std::string& key : live_)
    if (groups_[key].kind == kind) old.push_back(key);
  for (const std::string& key : old) release(key);
}

void BattleAudio::end_pack() { release_kind(Kind::Pack); }

void BattleAudio::end_actor() { release_kind(Kind::Actor); }

const BattleAudio::Sound* BattleAudio::find(const std::string& tag, const Group** from) const {
  for (auto key = live_.rbegin(); key != live_.rend(); ++key) {
    const Group& group = groups_.at(*key);
    auto it = group.sounds.find(tag);
    if (it != group.sounds.end()) {
      if (from) *from = &group;
      return &it->second;
    }
  }
  return nullptr;
}

std::string BattleAudio::resolve(const std::string& tag) const {
  for (auto key = live_.rbegin(); key != live_.rend(); ++key)
    if (groups_.at(*key).sounds.count(tag)) return *key;
  return {};
}

// Parses one drp into a group in the engine's list order (a file's entries are inserted at the head in file order, then the nested drps are
// opened from the head, i.e. last entry first, each pushing its own entries in front): a drp's nested drps come first (in file order, each by
// this same rule), then its own entries from the last to the first; the first match wins. The weapon-class group is the attacker's class
// digit, falling back to lower digits until one exists in the file.
void BattleAudio::scan(ByteSpan drp, int wp_class, Group& group) {
  if (!base_) return;
  auto parsed = import::parse_drp(drp);
  if (!parsed) return;
  int digit = -1;
  if (wp_class >= 0)
    for (int candidate = std::min(wp_class, 9); candidate >= 0 && digit < 0; --candidate)
      if (has_weapon_digit(*parsed, candidate)) digit = candidate;
  collect(*parsed, wp_class, digit, group.sounds, group.banks);
}

void BattleAudio::collect(const import::Drp& parsed, int wp_class, int digit, std::map<std::string, Sound>& out,
                          std::vector<std::shared_ptr<const audio::Bank>>& banks) {
  for (const import::DrpEntry& entry : parsed.entries)
    if (entry.type == 5)
      if (auto bank = audio::parse_bank(entry.data)) banks.push_back(bank);
  for (const import::DrpEntry& entry : parsed.entries)
    if (entry.type == 1 && import::is_drp(entry.data))
      if (auto sub = import::parse_drp(entry.data)) collect(*sub, wp_class, digit, out, banks);
  for (auto it = parsed.entries.rbegin(); it != parsed.entries.rend(); ++it) {
    const import::DrpEntry& entry = *it;
    if (entry.type != 0x16 || entry.data.size() < 0x18 || entry.data[0] != 'A') continue;
    std::string tag = entry.name;
    if (wp_class >= 0 && weapon_name(entry.name)) {
      // The other classes keep their names ("2s00") and are never asked for.
      if (entry.name[0] - '0' != digit) continue;
      tag = "_" + entry.name.substr(1);
    }
    if (out.count(tag)) continue;
    Sound sound;
    sound.code = std::make_shared<audio::AkaoCode>();
    sound.code->data.assign(entry.data.begin(), entry.data.end());
    out.emplace(std::move(tag), std::move(sound));
  }
}

// Every entry of the sound's AKAO file becomes a program (not only the first). The instruments resolve against the live table as it is now,
// so a copy of it keeps what this sound started with when a later file overwrites slots under the running mixer.
bool BattleAudio::make_program(const std::string& tag, audio::SfxProgram& program, std::vector<audio::SfxProgram>* extra) const {
  const Group* group = nullptr;
  const Sound* sound = find(tag, &group);
  if (!sound) return false;
  std::shared_ptr<const audio::InstrumentTable> snapshot = live_tbl_ ? std::make_shared<audio::InstrumentTable>(*live_tbl_) : base_;
  if (!audio::make_pointer_programs(sound->code, snapshot, program, extra)) return false;
  const uint32_t mask = group_mask(group->kind);
  program.tag = tag;
  program.mask = mask;
  if (extra)
    for (audio::SfxProgram& extra_program : *extra) {
      extra_program.tag = tag;
      extra_program.mask = mask;
    }
  return true;
}

bool BattleAudio::play(const std::string& tag, int volume, int pan, uint32_t mask) {
  if (!mixer_) return false;
  audio::SfxProgram program;
  std::vector<audio::SfxProgram> programs;
  if (!make_program(tag, program, &programs)) return false;
  mask &= 0xFFFFFF;
  if (mask) mixer_->evict_sfx(mask);
  programs.insert(programs.begin(), program);
  for (audio::SfxProgram& each : programs) {
    each.mask |= mask;
    each.volume = static_cast<float>(volume & 0x7F) / 127.0f;
    each.pan = pan;
    mixer_->play_sfx(each);
  }
  return true;
}

bool BattleAudio::render(const std::string& tag, std::vector<float>& pcm, int frames, int volume, int pan, std::bitset<256>* hits) const {
  audio::SfxProgram program;
  std::vector<audio::SfxProgram> extra;
  if (!make_program(tag, program, &extra)) return false;
  program.volume = static_cast<float>(volume & 0x7F) / 127.0f;
  program.pan = pan;
  for (audio::SfxProgram& extra_program : extra) extra_program.volume = program.volume, extra_program.pan = pan;
  audio::AkaoPlayer player(44100, true);
  player.start_sfx(program);
  for (const audio::SfxProgram& extra_program : extra) player.start_sfx(extra_program);
  pcm.assign(static_cast<size_t>(frames) * 2, 0.0f);
  player.render(pcm.data(), static_cast<u32>(frames));
  if (hits && program.table) *hits = program.table->hits();
  return true;
}

bool BattleAudio::analyse(const std::string& tag, double seconds, Analysis& analysis) const {
  audio::SfxProgram program;
  std::vector<audio::SfxProgram> extra;
  if (!make_program(tag, program, &extra)) return false;
  audio::AkaoPlayer player(44100, true);
  player.start_sfx(program);
  analysis = Analysis{};
  analysis.programs = program.count;
  for (const audio::SfxProgram& extra_program : extra) {
    player.start_sfx(extra_program);
    analysis.programs += extra_program.count;
  }
  std::vector<float> buf(2048 * 2);
  const u32 total = static_cast<u32>(seconds * 44100.0);
  for (u32 done = 0; done < total; done += 2048) {
    if (audio::sfx_trace_on()) audio::g_sfx_trace_clock.fetch_add(2048 * 48000 / 44100);
    player.render(buf.data(), 2048);
    if (player.active_voices() > 0) analysis.last_voice_s = (done + 2048) / 44100.0;
  }
  analysis.voices_left = player.active_voices();
  analysis.channels_left = player.active_channels();
  return true;
}

std::vector<std::string> BattleAudio::tags() const {
  std::vector<std::string> out;
  for (const std::string& key : live_)
    for (const auto& [tag, sound] : groups_.at(key).sounds)
      if (std::find(out.begin(), out.end(), tag) == out.end()) out.push_back(tag);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> BattleAudio::find_code(const std::vector<uint8_t>& pattern) const {
  std::vector<std::string> matches;
  for (const std::string& tag : tags()) {
    const Sound* sound = find(tag);
    if (!sound || !sound->code) continue;
    const auto& code = sound->code->data;
    if (std::search(code.begin(), code.end(), pattern.begin(), pattern.end()) != code.end()) matches.push_back(tag);
  }
  return matches;
}

void BattleAudio::play_global(int effect_id) {
  if (sfx_) sfx_->play(effect_id);
}

void BattleAudio::play_menu(int effect_id, uint32_t mask) {
  if (sfx_) sfx_->play_menu(effect_id, mask);
}

bool BattleAudio::start_song(int song, bool warn_on_failure) {
  if (remastered_) {
    const audio::BgmEntry* entry = audio::bgm_entry(song);
    if (!entry) return false;
    auto bytes = vfs_->read(audio::bgm_path(entry->file_no));
    auto stream = std::make_shared<audio::HcaStream>();
    if (!bytes || !stream->open(*bytes)) {
      if (warn_on_failure) log::warn("battle audio: could not load {}", audio::bgm_path(entry->file_no));
      return false;
    }
    mixer_->play(std::move(stream), song, 127);
  } else if (!mixer_->play_akao_bgm(song, 127)) {
    if (warn_on_failure) log::warn("battle audio: AKAO battle theme unavailable");
    return false;
  }
  started_music_ = true;
  song_ = song;
  return true;
}

void BattleAudio::play_song(int song) {
  if (!mixer_ || !vfs_) return;
  if (start_song(song, false)) log::info("battle audio: song {}", song);
}

// The field starts the battle theme itself on an encounter, so a theme already playing is adopted; a kept field song (C9 flag 1, also when the
// room's own song is the battle theme's id, and when no mixer state is observable) or any other song playing is left alone.
void BattleAudio::start_battle_theme(int battle_flags, bool keep_field_music) {
  if (!mixer_ || !vfs_) return;
  if (keep_field_music) {
    log::info("battle audio: field music kept (C9 flag 1; song {})", mixer_->current_song());
    return;
  }
  const int theme = audio::battle_song(battle_flags, remastered_);
  if (mixer_->playing() && mixer_->current_song() == theme) {
    started_music_ = true;
    song_ = mixer_->current_song();
    return;
  }
  if (mixer_->playing() && mixer_->current_song() != theme) {
    log::debug("battle audio: music kept (song {})", mixer_->current_song());
    return;
  }
  if (start_song(theme, true)) log::info("battle audio: battle theme (song {})", theme);
}

void BattleAudio::duck_cast() {
  if (!mixer_ || !(started_music_ || mixer_->playing())) return;
  mixer_->fade(0x66, 0x6D / 60.0);
  ducked_ = true;
}

void BattleAudio::fade_chant() {
  if (mixer_) mixer_->fade_sfx_volume(0x100, 0x3C, 0);
}

void BattleAudio::restore_cast() {
  if (!ducked_) return;
  ducked_ = false;
  if (!mixer_ || !(started_music_ || mixer_->playing())) return;
  mixer_->fade(0x7F, 0x60 / 60.0);
}

void BattleAudio::fade_music(int volume, int ticks) {
  if (!mixer_ || !(started_music_ || mixer_->playing())) return;
  mixer_->fade(volume & 0x7F, std::max(0, ticks) / 60.0);
}

void BattleAudio::stop_music() {
  if (mixer_ && started_music_) mixer_->stop();
  started_music_ = false;
  song_ = -1;
}

}  // namespace oc::game
