#include "ui/menu/extras.h"

#include <algorithm>

#include "ui/dialogue_box.h"

namespace oc::ui {

int shuffle_next(const std::vector<int>& songs, int current, u32& state) {
  if (songs.empty()) return -1;
  std::vector<int> others;
  for (const int song : songs)
    if (song != current) others.push_back(song);
  if (others.empty()) return songs.front();
  if (state == 0) state = 1;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return others[state % others.size()];
}

std::vector<GalleryFace> gallery_faces() {
  std::vector<GalleryFace> faces;
  for (int id = 0; id < kDialogueFaces; ++id) {
    const std::string art = dialogue_face_art(id);
    const auto it = std::find_if(faces.begin(), faces.end(), [&](const GalleryFace& face) { return face.art == art; });
    if (it == faces.end()) faces.push_back({art, {id}});
    else it->ids.push_back(id);
  }
  return faces;
}

}  // namespace oc::ui
