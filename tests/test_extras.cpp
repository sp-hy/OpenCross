// The Extras page's unlocks and rules: the three bit-set chunks of the collection file (layout, round trip, bounds, other chunks
// untouched), the shuffle's picks and the portrait gallery's grouping of the dialogue faces.
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "save/extras_unlocks.h"
#include "ui/dialogue_box.h"
#include "ui/menu/extras.h"

using namespace oc;
using save::Collection;
using save::Unlock;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static void test_bits() {
  Collection collection;
  CHECK(!save::unlocked(collection, Unlock::Song, 0));
  CHECK(save::unlock(collection, Unlock::Song, 0));
  CHECK(!save::unlock(collection, Unlock::Song, 0));
  CHECK(save::unlock(collection, Unlock::Song, 9));
  CHECK(save::unlocked(collection, Unlock::Song, 0) && save::unlocked(collection, Unlock::Song, 9) && !save::unlocked(collection, Unlock::Song, 8));
  const Bytes* songs = collection.chunk(save::kSongChunk);
  CHECK(songs && songs->size() == 2 && (*songs)[0] == 0x01 && (*songs)[1] == 0x02);
  CHECK(!save::unlock(collection, Unlock::Face, -1) && !save::unlock(collection, Unlock::Face, save::kUnlockBits));
  CHECK(!save::unlocked(collection, Unlock::Face, -1) && !save::unlocked(collection, Unlock::Face, 1000));
  CHECK(save::unlock(collection, Unlock::Face, save::kUnlockBits - 1));
  CHECK(collection.chunk(save::kFaceChunk)->size() == save::kUnlockBits / 8);
  CHECK(!save::unlocked(collection, Unlock::Song, 255) && !save::unlocked(collection, Unlock::Movie, 255));
  CHECK(save::movie_key(1, 19) == 19 && save::movie_key(2, 0) == 32 && save::movie_key(2, 10) == 42);
  CHECK(save::unlock(collection, Unlock::Movie, save::movie_key(2, 4)));
  CHECK(save::unlocked(collection, Unlock::Movie, 36) && !save::unlocked(collection, Unlock::Movie, 4));
  CHECK(collection.mark_enemy(3, Collection::kEnemyMet) && collection.enemy_met(3));
}

static void test_round_trip() {
  Collection collection;
  save::unlock(collection, Unlock::Song, 67);
  save::unlock(collection, Unlock::Movie, 33);
  save::unlock(collection, Unlock::Face, 91);
  collection.mark_enemy(7, Collection::kEnemyDefeated);
  const Collection back = Collection::parse(collection.serialize());
  CHECK(save::unlocked(back, Unlock::Song, 67) && save::unlocked(back, Unlock::Movie, 33) && save::unlocked(back, Unlock::Face, 91));
  CHECK(!save::unlocked(back, Unlock::Song, 33) && !save::unlocked(back, Unlock::Face, 67));
  CHECK(back.enemy_defeated(7));
  CHECK(back.serialize() == collection.serialize());
  // The tags as the file stores them.
  CHECK(save::kSongChunk == 0x474E4F53u && save::kMovieChunk == 0x49564F4Du && save::kFaceChunk == 0x45434146u);
}

static void test_shuffle() {
  u32 state = 0;
  CHECK(ui::shuffle_next({}, 3, state) == -1);
  CHECK(ui::shuffle_next({5}, 5, state) == 5);
  CHECK(ui::shuffle_next({5}, -1, state) == 5);
  const std::vector<int> songs = {1, 4, 9, 20};
  std::set<int> seen;
  int current = 4;
  for (int i = 0; i < 200; ++i) {
    const int next = ui::shuffle_next(songs, current, state);
    CHECK(next != current && (next == 1 || next == 4 || next == 9 || next == 20));
    seen.insert(next);
    current = next;
  }
  CHECK(seen.size() == songs.size());
  u32 first = 0, second = 0;
  CHECK(ui::shuffle_next(songs, 4, first) == ui::shuffle_next(songs, 4, second));
}

static void test_gallery() {
  const std::vector<ui::GalleryFace> faces = ui::gallery_faces();
  size_t ids = 0;
  std::set<std::string> arts;
  for (const ui::GalleryFace& face : faces) {
    ids += face.ids.size();
    arts.insert(face.art);
    for (const int id : face.ids) CHECK(face.art == ui::dialogue_face_art(id));
  }
  CHECK(ids == static_cast<size_t>(ui::kDialogueFaces));
  CHECK(arts.size() == faces.size());
  CHECK(faces.size() == 86);
  CHECK(faces.front().art == "00_seljyu" && faces.front().ids.size() == 1);
  bool placeholder = false;
  for (const ui::GalleryFace& face : faces)
    if (face.art == "kao00") placeholder = face.ids.size() == 7 && face.ids.front() == 83;
  CHECK(placeholder);
  CHECK(std::string(ui::dialogue_face_art(-1)).empty() && std::string(ui::dialogue_face_art(92)).empty());
}

int main() {
  test_bits();
  test_round_trip();
  test_shuffle();
  test_gallery();
  if (g_fail) {
    std::printf("%d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("extras: all passed\n");
  return 0;
}
