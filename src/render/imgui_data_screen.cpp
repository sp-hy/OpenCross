#include "render/imgui_data_screen.h"

#include <imgui.h>

#include <cmath>

namespace oc::render {

namespace {

// A window over the whole viewport with no title bar, padded.
bool begin_full_screen(const char* id) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32.0f, 28.0f));
  const bool open = ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar();
  return open;
}

// A bullet line that wraps at the window's edge (long paths).
void bullet_wrapped(const std::string& line) {
  ImGui::Bullet();
  ImGui::SameLine();
  ImGui::TextWrapped("%s", line.c_str());
}

}  // namespace

NotFoundAction draw_not_found(const NotFoundView& view) {
  NotFoundAction action = NotFoundAction::None;
  if (begin_full_screen("##not_found")) {
    ImGui::TextUnformatted("OpenCross: no game files found");
    ImGui::Separator();
    ImGui::TextWrapped(
        "OpenCross plays Chrono Cross from the Radical Dreamers Edition on Steam, or from your own Chrono Cross PlayStation discs "
        "(Japanese or USA). Neither was found.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Searched:");
    for (const std::string& line : view.searched) bullet_wrapped(line);
    if (!view.notes.empty()) {
      ImGui::Spacing();
      ImGui::TextUnformatted("Found but not usable:");
      for (const std::string& line : view.notes) bullet_wrapped(line);
    }
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Disc images read: .cue + .bin, .bin / .img, .iso, .ccd + .img, .mds + .mdf, .chd, .m3u playlists and unencrypted .pbp. "
        "Put them next to opencross.exe, or pick them with Browse... (both discs at once if you have them).");
    ImGui::Spacing();
    if (ImGui::Button("Retry")) action = NotFoundAction::Retry;
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) action = NotFoundAction::Browse;
    ImGui::SameLine();
    if (ImGui::Button("Quit")) action = NotFoundAction::Quit;
    ImGui::SameLine();
    ImGui::TextDisabled("Searching again in %d s", static_cast<int>(std::ceil(view.rescan_in)));
  }
  ImGui::End();
  return action;
}

VersionPick draw_version_picker(const std::string& warning) {
  VersionPick pick = VersionPick::None;
  if (begin_full_screen("##version_picker")) {
    ImGui::TextUnformatted("OpenCross: which version?");
    ImGui::Separator();
    ImGui::TextWrapped("Discs of the Japanese and the USA version of Chrono Cross were found. Pick the one to play; the choice is remembered "
                       "([Disc] Region in opencross.ini).");
    if (!warning.empty()) {
      ImGui::Spacing();
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", warning.c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button("Japanese")) pick = VersionPick::Japanese;
    ImGui::SameLine();
    if (ImGui::Button("USA")) pick = VersionPick::Usa;
    ImGui::SameLine();
    if (ImGui::Button("Quit")) pick = VersionPick::Quit;
  }
  ImGui::End();
  return pick;
}

}  // namespace oc::render
