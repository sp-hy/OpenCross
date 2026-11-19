#include "game/oc_achievements_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <format>
#include <optional>
#include <span>

namespace oc::game {

namespace {

constexpr double kPopupTicks = 240.0;
constexpr double kFadeTicks = 12.0;
constexpr size_t kPopupLines = 3;

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

}  // namespace

ui::AchievementList achievement_list(const OcAchievements& achievements) {
  ui::AchievementList list;
  const std::span<const OcRow> rows = achievements.rows();
  list.rows.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    const OcRow& row = rows[i];
    ui::AchievementRow entry;
    entry.title = row.title;
    entry.description = row.description;
    entry.category = static_cast<int>(row.category);
    entry.spoiler = row.spoiler;
    entry.unlocked = achievements.unlocked(i);
    if (entry.unlocked) entry.date = local_date(achievements.unlock_time(i));
    else if (const std::optional<OcAchievements::Progress> progress = achievements.progress(i))
      entry.progress = std::format("{} / {}", std::min(progress->have, progress->need), progress->need);
    list.unlocked += entry.unlocked ? 1 : 0;
    list.rows.push_back(std::move(entry));
  }
  return list;
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
