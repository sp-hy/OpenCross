#pragma once
// Gamepad and keyboard navigation helpers for the settings window.
//
// Nav columns: content laid out side by side (the settings tabs' two columns, the panes of the Cheats and Debug tabs, the Mods tab's list
// and detail) is wrapped column by column in begin_nav_column / end_nav_column, and end_nav_frame() runs once the window is drawn. While
// a move is scored, the other columns of the group that holds the navigation cursor are no candidates, so Up / Down walk one column and
// Left / Right stay in the cursor's row of its column; items outside that group (the tab bar, a button bar or another group under it)
// stay candidates. A Left / Right that finds nothing in the column goes to the vertically nearest item of the neighbouring column. Stacked content (one column when the window
// is narrow) is not wrapped.
//
// BumperTap: the pad's LB / RB switch tabs. A bumper acts when it is let go without the other having been down during its press, so the
// LB + RB chord (the guide overlay) never switches a tab.

// ImGui's item id, declared as imgui.h does so that this header does not pull it in.
typedef unsigned int ImGuiID;

namespace oc::render {

// `group`: an id unique among the column groups drawn in a frame (ImGui::GetID of the table's name); `column`: the column's index in it.
void begin_nav_column(ImGuiID group, int column);
void end_nav_column();
void end_nav_frame();
// Puts the navigation cursor, shown, on the first item submitted after this call in the current window (its child windows' items too
// when they are flattened into it).
void nav_to_first_item();

class BumperTap {
 public:
  // One frame of the LB / RB levels: -1 when a lone LB press ends, +1 for a lone RB press, else 0.
  int poll(bool left, bool right);
  // The presses under way act on nothing (they were used for something else, such as a slider's fine steps).
  void cancel() { left_alone_ = right_alone_ = false; }

 private:
  bool left_ = false, right_ = false;
  bool left_alone_ = false, right_alone_ = false;
};

}  // namespace oc::render
