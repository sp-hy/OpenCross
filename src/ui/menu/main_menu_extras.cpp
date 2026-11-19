// The Extras pages of the OpenCross row, one section each. The jukebox lists the songs and plays them in place of the field's music
// (either source, looping or shuffled), the theatre plays the movies full screen, the portrait gallery shows every dialogue face of both
// art sets, and the model viewer turns the characters' menu models through their clips. What is unlocked comes from the
// collection file (songs heard, movies watched, faces seen) and from the save (characters recruited); the host plays the music and movies.
#include <algorithm>
#include <cstdio>

#include "core/log.h"
#include "save/extras_unlocks.h"
#include "ui/menu/main_menu_impl.h"
#include "ui/menu/menu_common.h"

namespace oc::ui {

namespace {

const Str6 kNowPlaying = {{"Now playing", "En cours", "In riproduzione", "Läuft", "Sonando", "さいせいちゅう"}};
const Str6 kTime = {{"Time", "Durée", "Tempo", "Zeit", "Tiempo", "じかん"}};
const Str6 kSource = {{"Source", "Source", "Fonte", "Quelle", "Fuente", "おんげん"}};
const Str6 kRemastered = {{"Remastered", "Remasterisée", "Rimasterizzata", "Remastered", "Remasterizada", "リマスター"}};
const Str6 kOriginal = {{"Original", "Originale", "Originale", "Original", "Original", "オリジナル"}};
const Str6 kMode = {{"Mode", "Mode", "Modalità", "Modus", "Modo", "モード"}};
const Str6 kLoop = {{"Loop", "Boucle", "Ripeti", "Wiederholen", "Repetir", "リピート"}};
const Str6 kShuffle = {{"Shuffle", "Aléatoire", "Casuale", "Zufall", "Aleatorio", "シャッフル"}};
const Str6 kMovie = {{"Movie", "Film", "Filmato", "Film", "Vídeo", "ムービー"}};
const Str6 kDisc = {{"Disc", "Disque", "Disco", "Disc", "Disco", "ディスク"}};
const Str6 kPlay = {{"Play", "Lire", "Riproduci", "Abspielen", "Reproducir", "さいせい"}};
const Str6 kArt = {{"Art", "Style", "Stile", "Stil", "Estilo", "スタイル"}};
const Str6 kPose = {{"Pose", "Pose", "Posa", "Pose", "Pose", "ポーズ"}};
const Str6 kHurt = {{"Hurt", "Blessé", "Ferito", "Verletzt", "Herido", "ダメージ"}};
const Str6 kAutoTurn = {{"Auto turn", "Rotation auto", "Rotazione auto", "Autodrehung", "Giro auto", "じどうかいてん"}};

constexpr int kListRows = 17;
// The gallery: 6 x 4 faces a page, canvas units.
constexpr int kFaceCols = 6, kFaceRows = 4, kFacesPerPage = kFaceCols * kFaceRows;
constexpr float kFaceSize = 150.0f, kFaceStepX = 175.0f, kFaceStepY = 170.0f, kFaceTop = 100.0f;
// The menu model's clips: the three Square poses and the hurt pose.
constexpr int kPoses = 4;

std::string two_digits(int value) {
  char buffer[8];
  std::snprintf(buffer, sizeof buffer, "%02d", value);
  return buffer;
}

std::string minutes(double seconds) {
  const int whole = std::max(0, static_cast<int>(seconds));
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%d:%02d", whole / 60, whole % 60);
  return buffer;
}

}  // namespace

bool MainMenu::Impl::song_unlocked(int song) const {
  return extras_unlock_all || (collection && save::unlocked(*collection, save::Unlock::Song, song));
}

bool MainMenu::Impl::movie_unlocked(int key) const {
  return extras_unlock_all || (collection && save::unlocked(*collection, save::Unlock::Movie, key));
}

bool MainMenu::Impl::face_unlocked(const GalleryFace& face) const {
  return extras_unlock_all ||
         (collection && std::any_of(face.ids.begin(), face.ids.end(), [&](int id) { return save::unlocked(*collection, save::Unlock::Face, id); }));
}

// Recruited in the open game, or in any game the Characters page's chunk recorded (read only: one byte per character id, bit 0 set).
bool MainMenu::Impl::character_unlocked(int character) const {
  if (extras_unlock_all || (gs && gs->slot.recruited(character))) return true;
  const Bytes* had = collection ? collection->chunk(save::collection_tag("CHAR")) : nullptr;
  return had && character >= 0 && static_cast<size_t>(character) < had->size() && ((*had)[static_cast<size_t>(character)] & 1) != 0;
}

int MainMenu::Impl::extras_count() const {
  switch (ex_section) {
    case ExtrasSection::Jukebox: return extras_catalog ? static_cast<int>(extras_catalog->songs.size()) : 0;
    case ExtrasSection::Theatre: return extras_catalog ? static_cast<int>(extras_catalog->movies.size()) : 0;
    case ExtrasSection::Portraits: return static_cast<int>(ex_faces.size());
    case ExtrasSection::Models: return save::kNumChars;
  }
  return 0;
}

// Each page keeps its cursors for the session; nothing plays when one opens, and the jukebox starts on the music source the settings
// choose.
void MainMenu::Impl::enter_extras() {
  ex_section = static_cast<ExtrasSection>(shown_def().section);
  if (ex_faces.empty()) ex_faces = gallery_faces();
  ex_frames = frames;
  jb_song = -1;
  jb_ticks = 0;
  mv_cam = mv_cam_prev = MenuCamera{};
  mv_pose = 0;
  mv_auto = true;
  ++mv_session;
  jb_remastered = !extras_host || extras_host->remastered_music();
  log::info("menu: extras page {}: {} songs, {} movies, {} portraits", static_cast<int>(ex_section), extras_catalog ? extras_catalog->songs.size() : 0,
            extras_catalog ? extras_catalog->movies.size() : 0, ex_faces.size());
}

void MainMenu::Impl::play_jukebox(int song) {
  if (!extras_host || song < 0 || !extras_host->play_song(song, jb_remastered)) {
    sfx(0x18);
    return;
  }
  jb_song = song;
  jb_ticks = 0;
  log::info("menu: jukebox plays song {} ({}, {})", song, jb_remastered ? "remastered" : "original", jb_shuffle ? "shuffle" : "loop");
}

void MainMenu::Impl::stop_jukebox() {
  if (jb_song < 0) return;
  jb_song = -1;
  if (extras_host) extras_host->stop_song();
  log::info("menu: jukebox stopped");
}

// One pass of the song (its catalog length) played: shuffle starts another unlocked song; a one-shot song starts again; a song that loops
// goes on by itself.
void MainMenu::Impl::step_jukebox(int ticks) {
  if (jb_song < 0 || !extras_catalog) return;
  jb_ticks += ticks;
  const auto entry = std::find_if(extras_catalog->songs.begin(), extras_catalog->songs.end(), [&](const ExtrasCatalog::Song& song) { return song.id == jb_song; });
  if (entry == extras_catalog->songs.end() || jb_ticks < static_cast<int>(entry->seconds * 60.0)) return;
  jb_ticks = 0;
  if (jb_shuffle) {
    std::vector<int> songs;
    for (const ExtrasCatalog::Song& song : extras_catalog->songs)
      if (song_unlocked(song.id)) songs.push_back(song.id);
    play_jukebox(shuffle_next(songs, jb_song, jb_random));
  } else if (!entry->loops) {
    play_jukebox(jb_song);
  }
}

// Cancel stops the jukebox and returns to the page list (sfx 6). The list pages (the jukebox, the theatre, the model viewer) move by
// Up / Down one row and, but for the model viewer, by Left / Right one screen (no wrap, sfx 1). Jukebox: OK plays the song, or stops it
// when it is the one playing; Square switches the source, Triangle loop / shuffle. Theatre: OK plays the movie. Gallery: the four directions move the cursor over the faces (across the pages), Square switches between the
// 2021 and the 1999 art. Model viewer: Left / Right turn the model, Square steps through its clips, Triangle turns it by itself again.
// A locked entry refuses OK (sfx 0x18).
void MainMenu::Impl::update_extras(const Nav& nav) {
  const int ticks = std::max(frames - ex_frames, 0);
  ex_frames = frames;
  step_jukebox(ticks);
  if (ex_section == ExtrasSection::Models) {
    const int steps = (mv_frames + ticks) / 2 - mv_frames / 2;
    mv_frames += ticks;
    if (nav.held_left || nav.held_right) mv_auto = false;
    for (int tick = 0; tick < steps; ++tick) {
      mv_cam_prev = mv_cam;
      mv_cam.yaw += mv_auto ? 8 : (nav.held_right ? 16 : 0) - (nav.held_left ? 16 : 0);
    }
  }
  if (nav.cancel) {
    stop_jukebox();
    leave_page();
    sfx(6);
    return;
  }
  const int count = extras_count();
  int& cur = ex_cur[static_cast<size_t>(ex_section)];
  const int before = cur;
  if (ex_section == ExtrasSection::Portraits) {
    if (nav.left) cur = std::max(cur - 1, 0);
    if (nav.right) cur = std::min(cur + 1, std::max(count - 1, 0));
    if (nav.up) cur = std::max(cur - kFaceCols, 0);
    if (nav.down) cur = std::min(cur + kFaceCols, std::max(count - 1, 0));
    if (nav.sq) ex_faces_1999 = !ex_faces_1999, sfx(1);
  } else {
    if (nav.up) cur = std::max(cur - 1, 0);
    if (nav.down) cur = std::min(cur + 1, std::max(count - 1, 0));
    if (ex_section != ExtrasSection::Models) {
      if (nav.left) cur = std::max(cur - kListRows, 0);
      if (nav.right) cur = std::min(cur + kListRows, std::max(count - 1, 0));
    }
    scroll_to(cur, ex_top[static_cast<size_t>(ex_section)], kListRows);
  }
  if (cur != before) {
    sfx(1);
    if (ex_section == ExtrasSection::Models) mv_cam = mv_cam_prev = MenuCamera{}, mv_pose = 0, mv_auto = true, ++mv_session;
  }
  if (count == 0) return;
  switch (ex_section) {
    case ExtrasSection::Jukebox: {
      const int song = extras_catalog->songs[static_cast<size_t>(cur)].id;
      if (nav.ok) {
        if (!song_unlocked(song)) sfx(0x18);
        else if (song == jb_song) stop_jukebox();
        else play_jukebox(song);
      }
      if (nav.sq) {
        jb_remastered = !jb_remastered;
        if (jb_song >= 0 && extras_host) extras_host->switch_song_source(jb_remastered);
        sfx(1);
      }
      if (nav.tri) jb_shuffle = !jb_shuffle, sfx(1);
      break;
    }
    case ExtrasSection::Theatre:
      if (nav.ok) {
        const ExtrasCatalog::Movie& movie = extras_catalog->movies[static_cast<size_t>(cur)];
        if (!movie_unlocked(movie.key) || !extras_host) {
          sfx(0x18);
          break;
        }
        if (!extras_host->play_movie(movie.disc, movie.row)) {
          sfx(0x18);
          break;
        }
        sfx(8);
        jb_song = -1;
        log::info("menu: theatre plays movie {} (disc {} row {})", movie.key, movie.disc, movie.row);
      }
      break;
    case ExtrasSection::Portraits: break;
    case ExtrasSection::Models:
      if (nav.sq && character_unlocked(cur)) mv_pose = (mv_pose + 1) % kPoses, sfx(1);
      if (nav.tri) mv_auto = !mv_auto, sfx(1);
      break;
  }
}

// ===================================================== drawing
// The list in the roster panel (frame 141 at (8, 32), 17 rows 20 px apart), with the cursor and the scroll arrows; `line` draws a row.
void MainMenu::Impl::extras_list(int count, const std::function<void(int row, float cap)>& line) {
  const int top = ex_top[static_cast<size_t>(ex_section)], cur = ex_cur[static_cast<size_t>(ex_section)];
  Frame(141, 8, 32);
  if (top + kListRows < count) scroll_arrow(237, 354, false);
  if (top > 0) scroll_arrow(237, 50, true);
  for (int row = 0; row < kListRows && top + row < count; ++row) line(top + row, 42.2f + 20.0f * static_cast<float>(row));
  if (count > 0) tri(0, 42.0f + 20.0f * static_cast<float>(cur - top), true);
}

// The key-item description panel (frame 0x3E at (246, 318)) with up to four label / value rows.
void MainMenu::Impl::extras_panel(const std::array<std::u32string, 4>& labels, const std::array<std::u32string, 4>& values) {
  constexpr float kPanelX = 246;
  Frame(0x3E, kPanelX, 318);
  for (size_t row = 0; row < labels.size(); ++row) {
    const float y = 327.0f + 18.5f * static_cast<float>(row);
    if (!labels[row].empty()) TPfit(kPanelX + 12, y, labels[row], 78, kCap, 0.8f);
    if (!values[row].empty()) TPfit(kPanelX + 96, y, values[row], 148, kText, 0.8f);
  }
}

// Below the list: the glyph of `first_button` (a button of glyph_button: Square or OK; -1 none) with its hint and the Triangle glyph with
// its hint (none when empty).
void MainMenu::Impl::extras_hints(int first_button, const std::u32string& first_hint, const std::u32string& triangle_hint) {
  group(Left);
  auto hint = [&](float x, float y, float right, const std::u32string& line) { TPfit(x, y, line, right - x, kText, 0.8f); };
  if (first_button >= 0) {
    btn(0x10, 0x182, first_button);
    hint(40, 388, triangle_hint.empty() ? 232.0f : 110.0f, first_hint);
  }
  if (!triangle_hint.empty()) {
    btn(0x70, 0x182, 3);
    hint(0x88, 388, 232, triangle_hint);
  }
}

// Jukebox: each song's number (the song id + 1) and its length (one pass), "???" until heard, the playing one in the selection colour;
// right, the playing song's number large in the box and the panel with it, its time, the source and the mode.
void MainMenu::Impl::build_jukebox() {
  group(Left);
  const std::vector<ExtrasCatalog::Song> none;
  const std::vector<ExtrasCatalog::Song>& songs = extras_catalog ? extras_catalog->songs : none;
  extras_list(static_cast<int>(songs.size()), [&](int index, float cap) {
    const ExtrasCatalog::Song& song = songs[static_cast<size_t>(index)];
    const bool open = song_unlocked(song.id);
    HN(48, cap, two_digits(song.id + 1), song.id == jb_song ? kSel : kName, 0.29f);
    HL(56, cap, utf32(open ? minutes(song.seconds) : "???"), song.id == jb_song ? kSel : open ? kText : kName, 0.35f);
  });
  extras_hints(2, utf32(S6(kSource)), utf32(S6(kMode)));
  group(Right);
  const std::string playing = jb_song >= 0 ? two_digits(jb_song + 1) : "-";
  HN(400, 92, playing, jb_song >= 0 ? kSel : kName, 1.0f);
  const auto entry = extras_catalog ? std::find_if(songs.begin(), songs.end(), [&](const ExtrasCatalog::Song& song) { return song.id == jb_song; }) : songs.end();
  const std::string time = entry == songs.end() ? "-" : minutes(static_cast<double>(jb_ticks) / 60.0) + " / " + minutes(entry->seconds);
  extras_panel({utf32(S6(kNowPlaying)), utf32(S6(kTime)), utf32(S6(kSource)), utf32(S6(kMode))},
               {utf32(playing), utf32(time), utf32(S6(jb_remastered ? kRemastered : kOriginal)), utf32(S6(jb_shuffle ? kShuffle : kLoop))});
}

// Theatre: each movie's number and its disc, "???" until watched; right, the cursor's number large and the panel with its disc.
void MainMenu::Impl::build_theatre() {
  group(Left);
  const std::vector<ExtrasCatalog::Movie> none;
  const std::vector<ExtrasCatalog::Movie>& movies = extras_catalog ? extras_catalog->movies : none;
  extras_list(static_cast<int>(movies.size()), [&](int index, float cap) {
    const ExtrasCatalog::Movie& movie = movies[static_cast<size_t>(index)];
    const bool open = movie_unlocked(movie.key);
    HN(48, cap, two_digits(index + 1), kName, 0.29f);
    HL(56, cap, open ? utf32(std::string(S6(kDisc)) + " " + std::to_string(movie.disc)) : utf32("???"), open ? kText : kName, 0.35f);
  });
  extras_hints(0, utf32(S6(kPlay)), {});
  if (movies.empty()) return;
  group(Right);
  const int cur = ex_cur[static_cast<size_t>(ex_section)];
  const ExtrasCatalog::Movie& movie = movies[static_cast<size_t>(cur)];
  const bool open = movie_unlocked(movie.key);
  HN(400, 92, open ? two_digits(cur + 1) : "??", open ? kText : kName, 1.0f);
  extras_panel({utf32(S6(kMovie)), utf32(S6(kDisc)), {}, {}}, {utf32(two_digits(cur + 1)), utf32(open ? std::to_string(movie.disc) : "?"), {}, {}});
}

// Portrait gallery: 6 x 4 faces a page in the chosen art set, a face not seen yet as a black silhouette, the cursor framed in the
// selection colour; below, the cursor face's character name ("???" until seen; none for a face that is not a character's) and the page.
void MainMenu::Impl::build_portraits() {
  const int count = static_cast<int>(ex_faces.size());
  const int cur = ex_cur[static_cast<size_t>(ex_section)];
  const int shown_page = cur / kFacesPerPage, pages = (count + kFacesPerPage - 1) / kFacesPerPage;
  const std::string set = ex_faces_1999 ? "1999" : "2021";
  group(Centre);
  const float left = (kCanvasW - kFaceStepX * (kFaceCols - 1) - kFaceSize) / 2.0f;
  for (int slot = 0; slot < kFacesPerPage && shown_page * kFacesPerPage + slot < count; ++slot) {
    const int index = shown_page * kFacesPerPage + slot;
    const GalleryFace& face = ex_faces[static_cast<size_t>(index)];
    const float x = left + kFaceStepX * static_cast<float>(slot % kFaceCols), y = kFaceTop + kFaceStepY * static_cast<float>(slot / kFaceCols);
    Img(texs.load("hd/face/" + set + "/" + face.art + ".png"), x, y, kFaceSize, kFaceSize, 0, 0, 1, 1, face_unlocked(face) ? kWhite : rgb(0, 0, 0));
    if (index == cur) box_outline(x - 6, y - 6, kFaceSize + 12, kFaceSize + 12, 4, kSel);
  }
  if (count > 0) {
    const GalleryFace& face = ex_faces[static_cast<size_t>(cur)];
    const int character = face.ids.front();
    const std::u32string name = !face_unlocked(face) ? utf32("???") : character < save::kNumChars ? char_name(character) : std::u32string();
    if (!name.empty()) HL(30, 362, name, kText, 0.35f);
    HN(487, 362, std::to_string(shown_page + 1) + "/" + std::to_string(pages), kText, 0.29f);
  }
  extras_hints(2, utf32(std::string(S6(kArt)) + " " + set), {});
}

// Model viewer: each character's name, "???" until recruited, and the clip in the Square hint; right, a recruited character's menu model
// (where the Status screen draws it, with its weapon as equipped) in the chosen clip, turning by itself or by Left / Right.
void MainMenu::Impl::build_models() {
  group(Left);
  extras_list(save::kNumChars, [&](int character, float cap) {
    const bool open = character_unlocked(character);
    HN(48, cap, two_digits(character + 1), kName, 0.29f);
    HL(56, cap, open ? char_name(character) : utf32("???"), open ? kText : kName, 0.35f);
  });
  const std::string clip = mv_pose == 3 ? S6(kHurt) : std::to_string(mv_pose + 1);
  extras_hints(2, utf32(std::string(S6(kPose)) + " " + clip), utf32(S6(kAutoTurn)));
  const int character = ex_cur[static_cast<size_t>(ex_section)];
  if (!character_unlocked(character) || !model_view || !gs) return;
  group(Right);
  mv_slot = MenuModelSlot{};
  mv_slot.char_id = character;
  mv_slot.model = model_number(character, kLastMenuModel);
  mv_slot.weapon_model = model_number(character);
  model_gear(character, mv_slot);
  const int nearest = model_view->min_distance(mv_slot.model);
  mv_cam.distance = std::max(mv_cam.distance, nearest);
  mv_cam_prev.distance = std::max(mv_cam_prev.distance, nearest);
  mv_slot.camera = mv_cam;
  mv_slot.camera_prev = mv_cam_prev;
  mv_slot.session = mv_session;
  mv_slot.pose = mv_pose;
  mv_slot.tick = static_cast<u32>(mv_frames / 2);
  model_wanted = true;
  group(Left);
}

}  // namespace oc::ui
