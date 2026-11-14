// Nav columns and the settings window's bumper taps (imgui_nav.h).
//
// Every move: ImGui scores every navigable item of the window; while a move is scored, the other columns of the cursor's group are
// pushed as not navigable, so only its own column (and items outside the group) compete. Left / Right need it too: ImGui counts an item
// a few pixels off the cursor's row as about one pixel away however far it lies, so a control of the neighbouring column almost level
// with the cursor would beat the one beside it in the same row.
//
// Left / Right out of a column: ImGui only takes an item whose middle band lies level with the cursor, which the rows of a neighbouring
// column often miss. When a Left / Right move finds nothing in the cursor's column and that column has a neighbour that way, the move is
// carried on over the next frames as two vertical scans of the neighbour alone: one down from the cursor's top, one up from its bottom,
// both from the column edge the move enters by, so that of a row's controls the one nearest that edge is taken. The nearer find wins
// (the down scan runs once more when it is the one to apply). After the crossing the preferred position is cleared, so the next Up / Down
// starts from the item that was reached.
#include "render/imgui_nav.h"

#include <algorithm>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <imgui_internal.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace oc::render {

namespace {

struct ColumnKey {
  ImGuiID group = 0;
  int column = 0;
  bool operator==(const ColumnKey&) const = default;
};

struct OpenColumn {
  ColumnKey key;
  bool alive_before = false;
  bool restricted = false;
  bool target = false;
  ImGuiID local_before = 0, other_before = 0;
};

enum class Scan { None, Below, Above, BelowAgain };

// A Left / Right move carried into a neighbouring column. rightward: entered by its left edge; top / bottom: the cursor item's edges
// (screen y); below: the distance of the down scan's find (negative: none); found: the item the running scan's column made the best
// candidate.
struct Crossing {
  Scan scan = Scan::None;
  int frame = -1;
  ColumnKey target;
  bool rightward = true;
  float top = 0.0f, bottom = 0.0f;
  float below = -1.0f;
  ImGuiID found = 0;
  int reset_frame = -1;
};

constexpr float kFar = 1.0e9f;

// homes: the columns that held the navigation item in the last frame (innermost first); seen: the ones found so far in `seen_frame`;
// drawn: the columns drawn in `seen_frame`.
std::vector<OpenColumn> open_columns;
std::vector<ColumnKey> homes, seen, drawn;
int seen_frame = -1;
Crossing crossing;

// At the first column of a frame, the last frame's finds become the homes; older ones are dropped.
void roll_frame(int frame) {
  if (seen_frame == frame) return;
  homes.clear();
  if (seen_frame == frame - 1) homes.swap(seen);
  seen.clear();
  drawn.clear();
  seen_frame = frame;
}

bool scanning() { return crossing.scan != Scan::None && crossing.frame == ImGui::GetFrameCount(); }

// The scoring rectangle that puts every item outside the move's direction.
ImRect dead_rect() {
  const float y = GImGui->NavMoveDir == ImGuiDir_Up ? -kFar : kFar;
  return ImRect(0.0f, y, 0.0f, y);
}

void forward(ImGuiDir dir, Scan scan) {
  const ImGuiContext& g = *GImGui;
  ImGui::NavMoveRequestForward(dir, dir, g.NavMoveFlags, g.NavMoveScrollFlags);
  crossing.scan = scan;
  crossing.frame = ImGui::GetFrameCount() + 1;
}

void finish(bool keep) {
  if (!keep) ImGui::NavMoveRequestCancel();
  crossing.scan = Scan::None;
  crossing.reset_frame = keep ? ImGui::GetFrameCount() + 1 : -1;
}

// The result ImGui applies at the next frame, or null.
const ImGuiNavItemData* move_result() {
  const ImGuiContext& g = *GImGui;
  if (g.NavMoveResultLocal.ID != 0) return &g.NavMoveResultLocal;
  if (g.NavMoveResultOther.ID != 0) return &g.NavMoveResultOther;
  return nullptr;
}

// The running scan's find and its distance from the cursor item, or a negative distance.
float scan_distance() {
  const ImGuiNavItemData* result = move_result();
  if (!result || crossing.found == 0 || result->ID != crossing.found) return -1.0f;
  const ImRect rect = ImGui::WindowRectRelToAbs(result->Window, result->RectRel);
  return crossing.scan == Scan::Above ? std::max(0.0f, crossing.bottom - rect.Max.y) : std::max(0.0f, rect.Min.y - crossing.top);
}

void continue_crossing() {
  const float distance = scan_distance();
  switch (crossing.scan) {
    case Scan::Below:
      crossing.below = distance;
      forward(ImGuiDir_Up, Scan::Above);
      break;
    case Scan::Above:
      if (distance >= 0.0f && (crossing.below < 0.0f || distance < crossing.below)) finish(true);
      else if (crossing.below >= 0.0f) forward(ImGuiDir_Down, Scan::BelowAgain);
      else finish(false);
      break;
    case Scan::BelowAgain:
      finish(distance >= 0.0f);
      break;
    default:
      break;
  }
}

// A Left / Right move that found nothing, from inside a column with a neighbour that way: starts the scans.
void start_crossing() {
  const ImGuiContext& g = *GImGui;
  const bool horizontal = g.NavMoveDir == ImGuiDir_Left || g.NavMoveDir == ImGuiDir_Right;
  if (!g.NavMoveSubmitted || !horizontal || move_result() || !g.NavWindow ||
      (g.NavMoveFlags & (ImGuiNavMoveFlags_Forwarded | ImGuiNavMoveFlags_IsTabbing)) != 0 || seen_frame != ImGui::GetFrameCount())
    return;
  const int step = g.NavMoveDir == ImGuiDir_Right ? 1 : -1;
  for (const ColumnKey& home : seen) {
    const ColumnKey target{home.group, home.column + step};
    if (std::find(drawn.begin(), drawn.end(), target) == drawn.end()) continue;
    const ImRect cursor = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]);
    crossing.target = target;
    crossing.rightward = step > 0;
    crossing.top = cursor.Min.y;
    crossing.bottom = cursor.Max.y;
    crossing.below = -1.0f;
    forward(ImGuiDir_Down, Scan::Below);
    return;
  }
}

}  // namespace

void begin_nav_column(ImGuiID group, int column) {
  ImGuiContext& g = *GImGui;
  roll_frame(ImGui::GetFrameCount());
  const ColumnKey key{group, column};
  drawn.push_back(key);
  OpenColumn open{key, g.NavIdIsAlive, false, false, 0, 0};
  if (scanning()) {
    open.restricted = group == crossing.target.group && !(key == crossing.target);
    open.target = key == crossing.target;
    if (open.target) {
      const float left = ImGui::GetCursorScreenPos().x, right = std::max(left, left + ImGui::GetContentRegionAvail().x);
      const float x = crossing.rightward ? left : right;
      const float y = crossing.scan == Scan::Above ? crossing.bottom : crossing.top;
      g.NavScoringRect = ImRect(x, y, x, y);
      open.local_before = g.NavMoveResultLocal.ID;
      open.other_before = g.NavMoveResultOther.ID;
    } else {
      g.NavScoringRect = dead_rect();
    }
  } else {
    const bool moving = g.NavMoveScoringItems && g.NavMoveDir != ImGuiDir_None && (g.NavMoveFlags & ImGuiNavMoveFlags_IsTabbing) == 0;
    const auto home = std::find_if(homes.begin(), homes.end(), [&](const ColumnKey& item) { return item.group == group; });
    open.restricted = moving && home != homes.end() && home->column != column;
  }
  if (open.restricted) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  open_columns.push_back(open);
}

void end_nav_column() {
  if (open_columns.empty()) return;
  ImGuiContext& g = *GImGui;
  const OpenColumn open = open_columns.back();
  open_columns.pop_back();
  if (open.restricted) ImGui::PopItemFlag();
  if (open.target) {
    crossing.found = g.NavMoveResultLocal.ID != open.local_before   ? g.NavMoveResultLocal.ID
                     : g.NavMoveResultOther.ID != open.other_before ? g.NavMoveResultOther.ID
                                                                    : 0;
    g.NavScoringRect = dead_rect();
  }
  if (!open.alive_before && g.NavIdIsAlive) seen.push_back(open.key);
}

void end_nav_frame() {
  ImGuiContext& g = *GImGui;
  const int frame = ImGui::GetFrameCount();
  if (crossing.reset_frame == frame && g.NavWindow) {
    g.NavWindow->RootWindowForNav->NavPreferredScoringPosRel[g.NavLayer] = ImVec2(FLT_MAX, FLT_MAX);
    crossing.reset_frame = -1;
  }
  if (scanning()) {
    continue_crossing();
    crossing.found = 0;
    return;
  }
  crossing.scan = Scan::None;
  start_crossing();
}

void nav_to_first_item() {
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  ImGui::FocusWindow(window);
  ImGui::NavInitWindow(window, true);
  ImGui::SetNavCursorVisibleAfterMove();
}

int BumperTap::poll(bool left, bool right) {
  if (left && !left_) left_alone_ = !right;
  if (right && !right_) right_alone_ = !left;
  if (left && right) left_alone_ = right_alone_ = false;
  const int step = (!left && left_ && left_alone_) ? -1 : (!right && right_ && right_alone_) ? 1 : 0;
  left_ = left, right_ = right;
  return step;
}

}  // namespace oc::render
