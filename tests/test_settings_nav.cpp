// The settings window under a pad, headless: the real General and Graphics tabs drawn by ImGuiLayer::options_window, driven with D-pad
// and bumper presses. Wide (two columns): Down from the tab bar lands on a body item, Down walks one column top to bottom without
// leaving it, Right crosses to the other column, Left from the right-hand check box of a grid row goes to the one beside it (reached by
// Right, by Down, or crossing from the left column), Up climbs back to the tab bar. Run with ImGui's own font and with Segoe UI at the
// UI scales of 1080p and 1440p windows, at several window sizes. Narrow (one column): every Down goes further down.
// RB / LB taps switch to the next / previous tab with the cursor on the new tab's first item; LB + RB together switch nothing.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <imgui.h>
#include <imgui_internal.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "platform/config.h"
#include "render/imgui_layer.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

constexpr ImGuiKey kPadKeys[] = {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                                 ImGuiKey_GamepadL1, ImGuiKey_GamepadR1};

struct Harness {
  render::ImGuiLayer ui;
  platform::Config config;

  // One frame with the keys in `down` held.
  void frame(std::initializer_list<ImGuiKey> down = {}) {
    ImGuiIO& io = ImGui::GetIO();
    for (const ImGuiKey key : kPadKeys) io.AddKeyEvent(key, std::find(down.begin(), down.end(), key) != down.end());
    ImGui::NewFrame();
    ui.options_window(config, {});
    ImGui::Render();
  }
  void press(std::initializer_list<ImGuiKey> keys) {
    frame(keys);
    for (int i = 0; i < 5; ++i) frame();
  }
};

ImRect cursor() {
  const ImGuiContext& g = *GImGui;
  return g.NavWindow ? ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]) : ImRect();
}
// The cursor item's top in its window's content (unmoved by scrolling).
float content_y() {
  const ImGuiContext& g = *GImGui;
  return g.NavWindow ? g.NavWindow->NavRectRel[g.NavLayer].Min.y : 0.0f;
}
bool in_body() { return GImGui->NavWindow && (GImGui->NavWindow->Flags & ImGuiWindowFlags_ChildWindow) != 0 && GImGui->NavId != 0; }

// Opens the window at `tab` with the cursor on the tab bar.
void open_at(Harness& harness, const char* tab) {
  harness.ui.options_open = true;
  harness.ui.open(tab);
  for (int i = 0; i < 4; ++i) harness.frame();
  harness.press({ImGuiKey_GamepadDpadDown});
  for (int i = 0; i < 40 && in_body(); ++i) harness.press({ImGuiKey_GamepadDpadUp});
}

void check_wide(ImVec2 display, int width = -1, int height = -1, bool widescreen = false) {
  ImGui::GetIO().DisplaySize = display;
  Harness harness;
  harness.config.settings_w = width;
  harness.config.settings_h = height;
  harness.config.widescreen = widescreen;
  open_at(harness, "Graphics");
  CHECK(harness.ui.current_tab() == "Graphics");
  CHECK(!in_body());

  harness.press({ImGuiKey_GamepadDpadDown});
  CHECK(in_body());
  const float middle = GImGui->NavWindow->Rect().GetCenter().x;
  CHECK(cursor().GetCenter().x < middle);

  // Down to the bottom of the left column: always further down, never into the right column.
  int steps = 0;
  bool monotonic = true, stayed_left = true;
  for (; steps < 60; ++steps) {
    const ImGuiID before = GImGui->NavId;
    const float y = content_y();
    harness.press({ImGuiKey_GamepadDpadDown});
    if (GImGui->NavId == before) break;
    monotonic = monotonic && content_y() > y;
    stayed_left = stayed_left && cursor().GetCenter().x < middle;
  }
  std::printf("graphics, wide: %d steps down the left column\n", steps);
  CHECK(steps >= 8 && steps < 60);
  CHECK(monotonic);
  CHECK(stayed_left);

  // Right crosses over; Down then walks the right column only.
  harness.press({ImGuiKey_GamepadDpadRight});
  CHECK(cursor().GetCenter().x > middle);
  bool stayed_right = true;
  for (int i = 0; i < 5; ++i) {
    harness.press({ImGuiKey_GamepadDpadDown});
    stayed_right = stayed_right && cursor().GetCenter().x > middle;
  }
  CHECK(stayed_right);
  harness.press({ImGuiKey_GamepadDpadLeft});
  CHECK(cursor().GetCenter().x < middle);

  // The right column's rows that hold two controls (the check box grids, the radio pairs), found going down on the left-hand control of
  // each row and pressing Right (kept over the runs: a control's id is the same at every size). Then Down only, from the top of the
  // column, as a player walks it: on the right-hand control of such a row (Down keeps the horizontal position, so it lands there from a
  // control above at that height), Left goes to the left-hand one beside it, not to the other column.
  static std::vector<ImGuiID> right_hand;
  const auto to_right_column_top = [&] {
    for (int i = 0; i < 80 && in_body(); ++i) harness.press({ImGuiKey_GamepadDpadUp});
    harness.press({ImGuiKey_GamepadDpadDown});
    harness.press({ImGuiKey_GamepadDpadRight});
  };
  const auto same_row = [](const ImRect& lhs, const ImRect& rhs) { return lhs.Max.y > rhs.Min.y && lhs.Min.y < rhs.Max.y; };
  to_right_column_top();
  CHECK(cursor().GetCenter().x > middle);
  for (int i = 0; i < 60; ++i) {
    const ImGuiID start = GImGui->NavId;
    const ImRect from = cursor();
    harness.press({ImGuiKey_GamepadDpadRight});
    if (GImGui->NavId != start) {
      if (same_row(cursor(), from) && std::find(right_hand.begin(), right_hand.end(), GImGui->NavId) == right_hand.end())
        right_hand.push_back(GImGui->NavId);
      harness.press({ImGuiKey_GamepadDpadLeft});
    }
    harness.press({ImGuiKey_GamepadDpadDown});
    if (GImGui->NavId == start) break;
  }
  int checked = 0;
  bool stayed = true;
  to_right_column_top();
  for (int i = 0; i < 60 && stayed; ++i) {
    const ImGuiID start = GImGui->NavId;
    if (std::find(right_hand.begin(), right_hand.end(), start) != right_hand.end()) {
      const ImRect from = cursor();
      harness.press({ImGuiKey_GamepadDpadLeft});
      stayed = cursor().GetCenter().x > middle && same_row(cursor(), from);
      if (!stayed)
        std::printf("left from (%.0f, %.0f) went to (%.0f, %.0f)\n", static_cast<double>(from.Min.x), static_cast<double>(from.Min.y),
                    static_cast<double>(cursor().Min.x), static_cast<double>(cursor().Min.y));
      ++checked;
      harness.press({ImGuiKey_GamepadDpadRight});
      stayed = stayed && GImGui->NavId == start;
    }
    harness.press({ImGuiKey_GamepadDpadDown});
    if (GImGui->NavId == start) break;
  }
  // From each control of the left column, Right into the right column; landing on the right-hand control of a pair, Left goes to the
  // left-hand one beside it.
  for (int i = 0; i < 80 && in_body(); ++i) harness.press({ImGuiKey_GamepadDpadUp});
  harness.press({ImGuiKey_GamepadDpadDown});
  for (int i = 0; i < 60 && stayed; ++i) {
    const ImGuiID start = GImGui->NavId;
    harness.press({ImGuiKey_GamepadDpadRight});
    const ImGuiID landed = GImGui->NavId;
    if (std::find(right_hand.begin(), right_hand.end(), landed) != right_hand.end()) {
      const ImRect from = cursor();
      harness.press({ImGuiKey_GamepadDpadLeft});
      stayed = cursor().GetCenter().x > middle && same_row(cursor(), from);
      if (!stayed)
        std::printf("crossed: left from (%.0f, %.0f) went to (%.0f, %.0f)\n", static_cast<double>(from.Min.x),
                    static_cast<double>(from.Min.y), static_cast<double>(cursor().Min.x), static_cast<double>(cursor().Min.y));
      ++checked;
    }
    for (int j = 0; j < 4 && cursor().GetCenter().x > middle; ++j) harness.press({ImGuiKey_GamepadDpadLeft});
    const ImGuiID before = GImGui->NavId;
    harness.press({ImGuiKey_GamepadDpadDown});
    if (GImGui->NavId == before || GImGui->NavId == start) break;
  }
  std::printf("graphics, wide: %zu two-control rows known, %d checked on the way down\n", right_hand.size(), checked);
  CHECK(right_hand.size() >= 8);
  CHECK(checked >= 1);
  CHECK(stayed);

  // Up climbs back to the tab bar.
  for (int i = 0; i < 80 && in_body(); ++i) harness.press({ImGuiKey_GamepadDpadUp});
  CHECK(!in_body());

  // RB: the next tab (Mods and the others are not registered here, so General follows Graphics), the cursor on its first item.
  harness.press({ImGuiKey_GamepadR1});
  CHECK(harness.ui.current_tab() == "General");
  CHECK(in_body());
  harness.press({ImGuiKey_GamepadL1});
  CHECK(harness.ui.current_tab() == "Graphics");
  CHECK(in_body());
  harness.frame({ImGuiKey_GamepadL1});
  harness.frame({ImGuiKey_GamepadL1, ImGuiKey_GamepadR1});
  harness.frame({ImGuiKey_GamepadR1});
  harness.press({});
  CHECK(harness.ui.current_tab() == "Graphics");
  harness.ui.options_open = false;
  harness.frame();
}

void check_narrow() {
  ImGui::GetIO().DisplaySize = ImVec2(800, 600);
  Harness harness;
  harness.config.settings_w = harness.config.settings_h = -1;
  open_at(harness, "Graphics");
  harness.press({ImGuiKey_GamepadDpadDown});
  CHECK(in_body());
  int steps = 0;
  bool monotonic = true;
  for (; steps < 120; ++steps) {
    const ImGuiID before = GImGui->NavId;
    const float y = content_y();
    harness.press({ImGuiKey_GamepadDpadDown});
    if (GImGui->NavId == before) break;
    monotonic = monotonic && content_y() > y;
  }
  std::printf("graphics, narrow: %d steps down\n", steps);
  CHECK(steps >= 20 && steps < 120);
  CHECK(monotonic);
  harness.ui.options_open = false;
  harness.frame();
}
}  // namespace

int main() {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DeltaTime = 1.0f / 60.0f;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  unsigned char* pixels = nullptr;
  int width = 0, height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  check_wide(ImVec2(1600, 900));
  // The player's look: Segoe UI at 17 px and the window's spacing, at the UI scale of a 1920 x 1080 window (1.25) and of a 2560 x 1440
  // one (1.5); the settings window at its default size, at a saved size just wide enough for two columns, and short enough to scroll.
  if (const char* windir = std::getenv("WINDIR")) {
    const std::string font = std::string(windir) + "/Fonts/segoeui.ttf";
    if (io.Fonts->AddFontFromFileTTF(font.c_str(), 32.0f)) {
      io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
      io.FontDefault = io.Fonts->Fonts.back();
      for (const float scale : {1.25f, 1.5f}) {
        io.FontGlobalScale = 17.0f * scale / 32.0f;
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();
        style.WindowPadding = ImVec2(16, 14);
        style.FramePadding = ImVec2(10, 5);
        style.ItemSpacing = ImVec2(10, 8);
        style.ItemInnerSpacing = ImVec2(8, 6);
        style.CellPadding = ImVec2(8, 5);
        style.IndentSpacing = 18;
        style.ScrollbarSize = 12;
        style.GrabMinSize = 10;
        style.WindowBorderSize = 1;
        style.FrameBorderSize = 0;
        style.TabBorderSize = 0;
        style.SeparatorTextBorderSize = 1;
        style.SeparatorTextPadding = ImVec2(0, 4);
        style.ScaleAllSizes(scale);
        const ImVec2 display = scale < 1.4f ? ImVec2(1920, 1080) : ImVec2(2560, 1440);
        check_wide(display);
        check_wide(display, static_cast<int>(960 * scale), static_cast<int>(561 * scale));
        check_wide(display, static_cast<int>(960 * scale), static_cast<int>(561 * scale), true);
        check_wide(display, static_cast<int>(1400 * scale / 1.5f), static_cast<int>(640 * scale / 1.5f));
      }
    }
  }
  check_narrow();

  ImGui::DestroyContext();
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("settings_nav: all passed\n");
  return failures ? 1 : 0;
}
