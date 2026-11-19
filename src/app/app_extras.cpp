// The Extras pages of the OpenCross row ([Game] OpenCrossMenu): the song and movie lists they show, the music of the jukebox and the
// movies of the theatre in place of the field's own, and the unlock hooks (a song that sounded, a movie the game started, a face a
// dialogue window showed). Unlocks go to the collection file; a --config run keeps them in memory.
#include <algorithm>
#include <memory>

#include "app/app.h"
#include "audio/bgm_table.h"
#include "core/log.h"

namespace oc::app {

namespace {

// The remastered tracks' sample rate: a song's pass in seconds.
constexpr double kTrackRate = 48000.0;

const char* unlock_name(save::Unlock kind) {
  switch (kind) {
    case save::Unlock::Song: return "song";
    case save::Unlock::Movie: return "movie";
    case save::Unlock::Face: return "face";
  }
  return "?";
}

}  // namespace

// The songs of the song table in id order, one pass each (to the loop end, or the whole one-shot track); the movies the two discs' movie
// tables play whole, each once, in table order (disc 1, then disc 2), played through their first such row.
void App::init_extras() {
  extras_player.app = this;
  extras_catalog = {};
  for (int song = 0; song < audio::kBgmSongCount; ++song)
    if (const audio::BgmEntry* entry = audio::bgm_entry(song)) {
      const u32 samples = entry->loop_end > 0 ? entry->loop_end + 1 : entry->total_samples;
      extras_catalog.songs.push_back({song, static_cast<double>(samples) / kTrackRate, entry->loop_end > 0});
    }
  for (int disc = 1; disc <= 2; ++disc)
    for (int row = 0; const game::DiscIndex::MovieRow* rec = game::DiscIndex::movie_row(disc, row); ++row) {
      u32 start = 0, end = 0;
      game::DiscIndex::movie_range(disc, row, start, end);
      const int key = save::movie_key(disc, rec->number);
      const bool listed = std::any_of(extras_catalog.movies.begin(), extras_catalog.movies.end(), [&](const ui::ExtrasCatalog::Movie& movie) { return movie.key == key; });
      if (start || end || listed || world.disc_index().movie_name_of(disc, rec->number).empty()) continue;
      extras_catalog.movies.push_back({key, disc, row});
    }
  if (menu_ok) menu.set_extras(&extras_catalog, &extras_player);
  world.set_on_face([this](int face) { unlock_extra(save::Unlock::Face, face); });
}

void App::update_extras() {
  if (extras_music && !theatre_open && !(menu_ok && menu.extras_shown())) give_music_back();
  if (!extras_music && mixer.live() && mixer.sounding()) unlock_extra(save::Unlock::Song, mixer.current_song());
}

void App::unlock_extra(save::Unlock kind, int index) {
  if (!save::unlock(collection, kind, index)) return;
  log::info("extras: unlocked {} {}", unlock_name(kind), index);
  save_collection();
}

void App::give_music_back() {
  if (!extras_music) return;
  extras_music = false;
  if (world_ok) world.resume_room_song();
  else mixer.stop();
  log::info("extras: the field's music is back");
}

void App::end_theatre() {
  movie_player.close();
  movie_open = theatre_open = false;
  ctx.set_clear_color(0.08f, 0.09f, 0.14f);
  give_music_back();
  log::info("extras: theatre movie finished");
}

// The song starts from its beginning whatever played (the same song too: a one-shot song plays again).
bool App::ExtrasPlayer::play_song(int song, bool remastered) {
  if (remastered) {
    const audio::BgmEntry* entry = audio::bgm_entry(song);
    if (!entry) return false;
    const auto bytes = app->files.read(audio::bgm_path(entry->file_no));
    auto stream = std::make_shared<audio::HcaStream>();
    if (!bytes || !stream->open(*bytes)) {
      log::warn("extras: could not load {}", audio::bgm_path(entry->file_no));
      return false;
    }
    app->mixer.stop();
    app->mixer.play(std::move(stream), song, 127);
  } else {
    app->mixer.stop();
    if (!app->mixer.play_akao_bgm(song, 127)) return false;
  }
  app->extras_music = true;
  log::info("extras: jukebox song {} on the {} music", song, remastered ? "remastered" : "original");
  return true;
}

void App::ExtrasPlayer::switch_song_source(bool remastered) {
  if (app->extras_music) app->mixer.switch_bgm_source(remastered);
}

void App::ExtrasPlayer::stop_song() { app->give_music_back(); }

// The movie plays as its row does in the game (its range, volume and the staff roll's layout) with the field standing still and silent.
bool App::ExtrasPlayer::play_movie(int disc, int row) {
  game::FieldWorld::MovieRequest request;
  request.index = row;
  request.name = app->world.disc_index().movie_name(disc, row);
  if (request.name.empty() || !app->open_movie(request, disc)) return false;
  app->theatre_open = true;
  app->extras_music = true;
  app->mixer.stop();
  return true;
}

bool App::ExtrasPlayer::remastered_music() const { return app->config.bgm_source != "original"; }

}  // namespace oc::app
