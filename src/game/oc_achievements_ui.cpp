#include "game/oc_achievements_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <format>

#include "render/imgui_layer.h"

namespace oc::game {

namespace {

constexpr double kPopupTicks = 240.0;
constexpr double kFadeTicks = 12.0;
constexpr size_t kPopupLines = 3;

constexpr const char* kCategoryNames[] = {"All", "Story and places", "Bosses", "Recruits", "Elements and techs", "Battle", "Items and endgame"};

// The local date of a stored UTC time (YYYY-MM-DDTHH:MMZ).
std::string local_date(const std::string& utc) {
  std::tm when{};
  if (std::sscanf(utc.c_str(), "%4d-%2d-%2dT%2d:%2dZ", &when.tm_year, &when.tm_mon, &when.tm_mday, &when.tm_hour, &when.tm_min) != 5) return utc;
  when.tm_year -= 1900;
  when.tm_mon -= 1;
  const std::time_t stamp = _mkgmtime(&when);
  std::tm local{};
  if (stamp == -1 || localtime_s(&local, &stamp) != 0) return utc.substr(0, 10);
  return std::format("{:04}-{:02}-{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

// A tick for an unlocked row, a padlock for a locked one, in a square of the text height at the cursor.
void draw_mark(bool done) {
  const float size = ImGui::GetTextLineHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImDrawList* list = ImGui::GetWindowDrawList();
  if (done) {
    const ImU32 green = IM_COL32(110, 220, 120, 255);
    const ImVec2 points[] = {{at.x + size * 0.15f, at.y + size * 0.55f}, {at.x + size * 0.4f, at.y + size * 0.8f}, {at.x + size * 0.85f, at.y + size * 0.2f}};
    list->AddPolyline(points, 3, green, ImDrawFlags_None, size * 0.14f);
  } else {
    const ImU32 grey = IM_COL32(150, 150, 160, 255);
    list->AddRectFilled({at.x + size * 0.2f, at.y + size * 0.45f}, {at.x + size * 0.8f, at.y + size * 0.9f}, grey, size * 0.08f);
    list->PathArcTo({at.x + size * 0.5f, at.y + size * 0.45f}, size * 0.2f, 3.14159265f, 2.0f * 3.14159265f);
    list->PathStroke(grey, ImDrawFlags_None, size * 0.1f);
  }
  ImGui::Dummy({size, size});
}

}  // namespace

bool draw_oc_achievements_tab(const OcAchievements& achievements, OcAchievementsTab& state, bool& hide_notices) {
  const std::span<const OcRow> rows = achievements.rows();
  render::ImGuiLayer::section("Achievements");
  // The tab body scrolls sideways, so the text wraps at the visible right edge rather than at the content width.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float scrollbar = ImGui::GetScrollMaxY() > 0.0f ? style.ScrollbarSize : 0.0f;
  ImGui::PushTextWrapPos(ImGui::GetScrollX() + ImGui::GetWindowWidth() - style.WindowPadding.x - scrollbar);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::Text("%d extra goals made for OpenCross, on top of the game's 37 Steam achievements: story moments, places, bosses, every recruit, "
              "summons and double techs, battle feats and endings. Progress carries across all your saves and New Game+ runs, so choices "
              "that rule each other out can be earned over several playthroughs.",
              static_cast<int>(rows.size()));
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::Dummy({0.0f, ImGui::GetTextLineHeight() * 0.5f});
  ImGui::Text("%d / %d unlocked", achievements.unlocked_count(), static_cast<int>(rows.size()));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
  ImGui::Combo("##oc_ach_category", &state.category, kCategoryNames, static_cast<int>(std::size(kCategoryNames)));
  ImGui::SameLine();
  ImGui::Checkbox("Show spoilers", &state.show_spoilers);
  ImGui::SameLine();
  const bool changed = ImGui::Checkbox("Hide notifications", &hide_notices);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("No popup when an achievement unlocks, for these and Steam's alike. They are still earned and listed here.");
  if (!ImGui::BeginTable("oc_achievements", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) return changed;
  ImGui::TableSetupColumn("mark", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight());
  ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
  for (size_t i = 0; i < rows.size(); ++i) {
    const OcRow& row = rows[i];
    if (state.category != 0 && static_cast<int>(row.category) != state.category - 1) continue;
    const bool done = achievements.unlocked(i);
    const bool hidden = !done && row.spoiler && !state.show_spoilers;
    ImGui::PushID(static_cast<int>(i));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    draw_mark(done);
    ImGui::TableNextColumn();
    ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, {0.0f, ImGui::GetTextLineHeight()});
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(hidden ? "???" : row.title);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", hidden ? "???" : row.description);
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    if (done) {
      ImGui::TextUnformatted(local_date(achievements.unlock_time(i)).c_str());
    } else if (const std::optional<OcAchievements::Progress> progress = achievements.progress(i)) {
      ImGui::Text("%llu / %llu", static_cast<unsigned long long>(std::min(progress->have, progress->need)), static_cast<unsigned long long>(progress->need));
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
  return changed;
}

void OcAchievementPopup::clear() {
  queue_.clear();
  panel_.clear();
  shown_ = 0.0;
}

const std::vector<std::string>& OcAchievementPopup::update(double ticks, bool held) {
  if (held) return panel_;
  if (panel_.empty()) {
    panel_ = std::move(queue_);
    queue_.clear();
    shown_ = 0.0;
    if (panel_.empty()) return panel_;
  }
  shown_ += ticks;
  if (shown_ >= kPopupTicks) {
    panel_ = std::move(queue_);
    queue_.clear();
    shown_ = 0.0;
  }
  return panel_;
}

void OcAchievementPopup::draw(double ticks, bool held) {
  const std::vector<std::string>& titles = update(ticks, held);
  if (titles.empty() || held) return;
  const float alpha = static_cast<float>(std::clamp(std::min(shown_, kPopupTicks - shown_) / kFadeTicks, 0.0, 1.0));
  const std::string heading = titles.size() == 1 ? "Achievement unlocked" : std::format("{} achievements unlocked", titles.size());
  std::vector<std::string> lines(titles.begin(), titles.begin() + static_cast<std::ptrdiff_t>(std::min(titles.size(), kPopupLines)));
  if (titles.size() > kPopupLines) lines.push_back(std::format("and {} more", titles.size() - kPopupLines));
  ImDrawList* list = ImGui::GetForegroundDrawList();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float pad = ImGui::GetFontSize() * 0.6f;
  const float line_height = ImGui::GetTextLineHeight();
  float width = ImGui::CalcTextSize(heading.c_str()).x;
  for (const std::string& line : lines) width = std::max(width, ImGui::CalcTextSize(line.c_str()).x);
  width += pad * 2.0f;
  const float height = line_height * static_cast<float>(lines.size() + 1) + pad * 2.2f;
  const ImVec2 top_left(display.x - pad - width, pad), bottom_right(display.x - pad, pad + height);
  list->AddRectFilled(top_left, bottom_right, IM_COL32(16, 18, 28, static_cast<int>(225 * alpha)), pad * 0.5f);
  list->AddRect(top_left, bottom_right, IM_COL32(200, 170, 80, static_cast<int>(200 * alpha)), pad * 0.5f);
  list->AddText({top_left.x + pad, top_left.y + pad}, IM_COL32(200, 170, 80, static_cast<int>(255 * alpha)), heading.c_str());
  float y = top_left.y + pad * 1.2f + line_height;
  for (const std::string& line : lines) {
    list->AddText({top_left.x + pad, y}, IM_COL32(235, 238, 250, static_cast<int>(255 * alpha)), line.c_str());
    y += line_height;
  }
}

}  // namespace oc::game
