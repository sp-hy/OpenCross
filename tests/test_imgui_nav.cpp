// render/imgui_nav.h: settings-window navigation, driven headless through ImGui with pad presses. Nav columns keep Up / Down in the
// column of the cursor and Left / Right in its row, before they cross to the nearest item of the neighbouring column (entering by its
// edge, so the row's first control); a flattened tab body takes Down from the tab bar straight onto its first item;
// nav_to_first_item lands on the first item; BumperTap reports lone LB / RB taps and never the LB + RB chord.
#include <cstdio>
#include <functional>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <imgui.h>
#include <imgui_internal.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "render/imgui_nav.h"

using namespace oc::render;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

// One frame of a fixed window holding `body`, with `key` down (ImGuiKey_None: no key).
void frame(const std::function<void()>& body, ImGuiKey key = ImGuiKey_None) {
  ImGuiIO& io = ImGui::GetIO();
  for (const ImGuiKey pad : {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight})
    io.AddKeyEvent(pad, pad == key);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(800, 600));
  ImGui::Begin("nav", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
  body();
  ImGui::End();
  end_nav_frame();
  ImGui::Render();
}

// Presses and releases `key`; returns the item the cursor is on once a crossing's scans are over.
ImGuiID press(const std::function<void()>& body, ImGuiKey key) {
  frame(body, key);
  for (int i = 0; i < 5; ++i) frame(body);
  return GImGui->NavId;
}

// Two columns: A1 and, far below it, A2 on the left; B1 a little below A1 on the right (level with neither).
struct Columns {
  bool nav_columns = true;
  ImGuiID a1 = 0, a2 = 0, b1 = 0, focus = 0;
  void operator()() {
    const ImGuiID group = ImGui::GetID("cols");
    if (!ImGui::BeginTable("cols", 2)) return;
    ImGui::TableNextColumn();
    if (nav_columns) begin_nav_column(group, 0);
    ImGui::Button("A1");
    a1 = ImGui::GetItemID();
    if (focus == a1) ImGui::SetFocusID(a1, ImGui::GetCurrentWindow());
    ImGui::Dummy(ImVec2(10, 120));
    ImGui::Button("A2");
    a2 = ImGui::GetItemID();
    if (focus == a2) ImGui::SetFocusID(a2, ImGui::GetCurrentWindow());
    if (nav_columns) end_nav_column();
    ImGui::TableNextColumn();
    if (nav_columns) begin_nav_column(group, 1);
    ImGui::Dummy(ImVec2(10, 30));
    ImGui::Button("B1");
    b1 = ImGui::GetItemID();
    if (nav_columns) end_nav_column();
    ImGui::EndTable();
    focus = 0;
  }
};

// Two columns: a wide control far left; on the right, two check boxes side by side in a row 2 px lower.
struct Row {
  ImGuiID wide = 0, near = 0, far = 0, focus = 0;
  void operator()() {
    const ImGuiID group = ImGui::GetID("row");
    if (!ImGui::BeginTable("row", 2)) return;
    ImGui::TableNextColumn();
    begin_nav_column(group, 0);
    ImGui::Button("Wide", ImVec2(-FLT_MIN, 0));
    wide = ImGui::GetItemID();
    end_nav_column();
    ImGui::TableNextColumn();
    begin_nav_column(group, 1);
    ImGui::Dummy(ImVec2(10, 2));
    ImGui::Button("Near");
    near = ImGui::GetItemID();
    ImGui::SameLine(0, 200);
    ImGui::Button("Far");
    far = ImGui::GetItemID();
    if (focus == far) ImGui::SetFocusID(far, ImGui::GetCurrentWindow());
    end_nav_column();
    ImGui::EndTable();
    focus = 0;
  }
};

// A tab bar over a body child holding two buttons; `focus_body` asks for the body's first item.
struct Tabs {
  ImGuiChildFlags child = ImGuiChildFlags_NavFlattened;
  ImGuiID tab = 0, first = 0, focus = 0;
  bool focus_body = false;
  void operator()() {
    if (!ImGui::BeginTabBar("tabs")) return;
    if (ImGui::BeginTabItem("One")) {
      tab = ImGui::GetItemID();
      if (focus == tab) ImGui::SetFocusID(tab, ImGui::GetCurrentWindow());
      if (ImGui::BeginChild("body", ImVec2(0, 0), child)) {
        if (focus_body) nav_to_first_item();
        ImGui::Button("First");
        first = ImGui::GetItemID();
        ImGui::Button("Second");
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    focus = 0;
    focus_body = false;
  }
};

void check_columns() {
  Columns layout;
  const auto body = [&] { layout(); };
  frame(body);
  layout.focus = layout.a1;
  frame(body);
  frame(body);
  CHECK(GImGui->NavId == layout.a1);
  CHECK(press(body, ImGuiKey_GamepadDpadDown) == layout.a2);
  CHECK(press(body, ImGuiKey_GamepadDpadUp) == layout.a1);
  CHECK(press(body, ImGuiKey_GamepadDpadRight) == layout.b1);
  CHECK(press(body, ImGuiKey_GamepadDpadDown) == layout.b1);
  CHECK(press(body, ImGuiKey_GamepadDpadLeft) == layout.a1);
  CHECK(press(body, ImGuiKey_GamepadDpadDown) == layout.a2);
  CHECK(press(body, ImGuiKey_GamepadDpadRight) == layout.b1);

  // Without nav columns ImGui's own scoring takes Down from A1 across to the nearer B1.
  layout.nav_columns = false;
  layout.focus = layout.a1;
  frame(body);
  frame(body);
  CHECK(press(body, ImGuiKey_GamepadDpadDown) == layout.b1);
}

// ImGui alone takes the wide control a few pixels off the row over the check box beside the cursor; nav columns keep Left in the row.
void check_row() {
  Row layout;
  const auto body = [&] { layout(); };
  frame(body);
  layout.focus = layout.far;
  frame(body);
  frame(body);
  CHECK(GImGui->NavId == layout.far);
  CHECK(press(body, ImGuiKey_GamepadDpadLeft) == layout.near);
  CHECK(press(body, ImGuiKey_GamepadDpadLeft) == layout.wide);
  CHECK(press(body, ImGuiKey_GamepadDpadRight) == layout.near);
}

void check_tabs() {
  Tabs layout;
  const auto body = [&] { layout(); };
  frame(body);
  layout.focus = layout.tab;
  frame(body);
  frame(body);
  CHECK(GImGui->NavId == layout.tab);
  CHECK(press(body, ImGuiKey_GamepadDpadDown) == layout.first);
  CHECK(press(body, ImGuiKey_GamepadDpadUp) == layout.tab);

  // A plain child is one item of its own: Down stops on the child, not on the button inside it.
  layout.child = ImGuiChildFlags_None;
  layout.focus = layout.tab;
  frame(body);
  frame(body);
  const ImGuiID landed = press(body, ImGuiKey_GamepadDpadDown);
  CHECK(landed != layout.first && landed != layout.tab);

  layout.child = ImGuiChildFlags_NavFlattened;
  layout.focus = layout.tab;
  frame(body);
  frame(body);
  layout.focus_body = true;
  frame(body);
  frame(body);
  CHECK(GImGui->NavId == layout.first);
  CHECK(GImGui->NavCursorVisible);
}

void check_bumpers() {
  BumperTap tap;
  CHECK(tap.poll(true, false) == 0);
  CHECK(tap.poll(true, false) == 0);
  CHECK(tap.poll(false, false) == -1);
  CHECK(tap.poll(false, true) == 0);
  CHECK(tap.poll(false, false) == 1);
  // The guide chord, pressed together or one after the other, and let go in either order.
  CHECK(tap.poll(true, true) == 0);
  CHECK(tap.poll(false, true) == 0);
  CHECK(tap.poll(false, false) == 0);
  CHECK(tap.poll(true, false) == 0);
  CHECK(tap.poll(true, true) == 0);
  CHECK(tap.poll(true, false) == 0);
  CHECK(tap.poll(false, false) == 0);
  // A cancelled press acts on nothing; the next one does.
  CHECK(tap.poll(false, true) == 0);
  tap.cancel();
  CHECK(tap.poll(false, false) == 0);
  CHECK(tap.poll(true, false) == 0);
  CHECK(tap.poll(false, false) == -1);
}
}  // namespace

int main() {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(800, 600);
  io.DeltaTime = 1.0f / 60.0f;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  unsigned char* pixels = nullptr;
  int width = 0, height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  check_columns();
  check_row();
  check_tabs();
  check_bumpers();

  ImGui::DestroyContext();
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("imgui_nav: all passed\n");
  return failures ? 1 : 0;
}
