#include "render/imgui_update_tab.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include "core/version.h"
#include "platform/updater.h"
#include "render/imgui_layer.h"

namespace oc::render {

void draw_update_tab(platform::Updater& updater, bool* restart) {
  using platform::Updater;
  ImGuiLayer::section("Nexus Mods");
  if (ImGui::Button("Open Nexus Mods Page")) SDL_OpenURL(platform::kNexusUrl);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    ImGuiLayer::tooltip("Opens the OpenCross page on Nexus Mods in your web browser (downloads, changelog, comments and bug reports).");
  ImGui::SameLine();
  if (ImGui::Button("Report a bug")) SDL_OpenURL(platform::kNexusBugsUrl);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    ImGuiLayer::tooltip("Opens the Bugs tab of the OpenCross page on Nexus Mods in your web browser.");

  ImGuiLayer::section("Updates");
  const Updater::Status st = updater.status();
  if (ImGuiLayer::begin_rows("update_rows")) {
    ImGuiLayer::row("Current version", "The version of this opencross.exe (also shown in Explorer: Properties > Details).");
    ImGui::Text("v%s", OC_VERSION_STRING);
    if (!st.latest.empty()) {
      ImGuiLayer::row("Latest release", "The newest release on GitHub (sp-hy/OpenCross); test builds (pre-releases) are not offered.");
      if (st.published.empty()) ImGui::TextUnformatted(st.latest.c_str());
      else ImGui::Text("%s  (%s)", st.latest.c_str(), st.published.c_str());
    }
    ImGuiLayer::end_rows();
  }

  const bool busy = st.state == Updater::State::Checking || st.state == Updater::State::Downloading;
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Check for updates")) updater.check();
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
    ImGuiLayer::tooltip("Asks GitHub for the newest OpenCross release. Nothing is downloaded until you choose to.");

  const ImVec4 good(0.55f, 0.85f, 0.55f, 1.0f), bad(0.95f, 0.55f, 0.45f, 1.0f);
  switch (st.state) {
    case Updater::State::Idle: break;
    case Updater::State::Checking: ImGui::TextUnformatted("Checking GitHub..."); break;
    case Updater::State::UpToDate: ImGui::TextColored(good, "You have the latest version."); break;
    case Updater::State::Available:
      if (st.launcher_only) ImGui::TextColored(good, "The launcher button is missing or out of date.");
      else ImGui::TextColored(good, "%s is available.", st.latest.c_str());
      if (ImGui::Button("Download and install")) updater.download();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (st.launcher_only)
          ImGuiLayer::tooltip("Downloads XINPUT9_1_0.dll into the game folder. It adds an OpenCross button to the Radical Dreamers Edition "
                              "launcher and changes no game file.");
        else
          ImGuiLayer::tooltip("Downloads the new opencross.exe and puts it in place of this one (the old exe is kept as opencross.exe.old "
                              "until the next start), with the launcher's OpenCross button (XINPUT9_1_0.dll). Your settings, saves and "
                              "mods are not touched. You can keep playing; the new version runs after a restart.");
      }
      break;
    case Updater::State::Downloading: {
      ImGui::TextUnformatted("Downloading...");
      ImGui::ProgressBar(st.progress, ImVec2(-1.0f, 0.0f));
      break;
    }
    case Updater::State::Installed:
      if (st.launcher_only) {
        ImGui::TextColored(good, "The launcher button is installed.");
        break;
      }
      ImGui::TextColored(good, "%s is installed. It runs from the next start.", st.latest.c_str());
      if (ImGui::Button("Restart now")) *restart = true;
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGuiLayer::tooltip("Closes OpenCross and starts the new version. Progress since your last save is lost.");
      break;
    case Updater::State::Failed: {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(bad, "%s", st.message.c_str());
      ImGui::PopTextWrapPos();
      break;
    }
  }
}

}  // namespace oc::render
