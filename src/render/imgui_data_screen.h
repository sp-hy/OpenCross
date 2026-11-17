#pragma once
// The screens shown before the game starts when its data is not settled, drawn with ImGui's own font and no game assets: "no game
// files found" (what was searched, the formats read, Retry / Browse... / Quit) and the version picker for players who have both the
// Japanese and the USA discs. Each call draws one frame inside an ImGui frame and returns what the player clicked.
#include <string>
#include <vector>

namespace oc::render {

struct NotFoundView {
  // The places searched, one line each.
  std::vector<std::string> searched;
  // Files found there that cannot be played, one line each.
  std::vector<std::string> notes;
  // Seconds until the next automatic search.
  double rescan_in = 0.0;
};

enum class NotFoundAction {
  None,
  Retry,
  Browse,
  Quit,
};

NotFoundAction draw_not_found(const NotFoundView& view);

enum class VersionPick {
  None,
  Japanese,
  Usa,
  Quit,
};

// `warning`: a line shown under the question (an unknown build), empty for none.
VersionPick draw_version_picker(const std::string& warning);

}  // namespace oc::render
