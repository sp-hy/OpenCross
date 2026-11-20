// The Achievements page of the OpenCross row: OpenCross's own achievements (achievements_info.h) in table order in the roster panel, filtered
// by category, the chosen one's title, state, category and description on the right, and the count in the description panel. Triangle
// opens the page's Options page (main_menu_options.cpp). Titles and descriptions are the table's English text in every language.
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "ui/menu/main_menu_impl.h"
#include "ui/menu/menu_common.h"

namespace oc::ui {

std::vector<int> filter_achievements(const AchievementList& list, int filter) {
  std::vector<int> shown;
  for (size_t i = 0; i < list.rows.size(); ++i)
    if (filter == 0 || list.rows[i].category == filter - 1) shown.push_back(static_cast<int>(i));
  return shown;
}

namespace {

// The roster panel's rows (record_list).
constexpr int kRows = 17;

const Str6 kFilters[kAchievementFilters] = {
    {{"All", "Tous", "Tutti", "Alle", "Todos", "すべて"}},
    {{"Story and places", "Histoire et lieux", "Storia e luoghi", "Story und Orte", "Historia y lugares", "ストーリーとばしょ"}},
    {{"Bosses", "Boss", "Boss", "Bosse", "Jefes", "ボス"}},
    {{"Recruits", "Recrues", "Reclute", "Rekruten", "Reclutas", "なかま"}},
    {{"Elements and techs", "Éléments et techniques", "Elementi e tecniche", "Elemente und Techniken", "Elementos y técnicas", "エレメントとわざ"}},
    {{"Battle", "Combat", "Battaglia", "Kampf", "Batalla", "バトル"}},
    {{"Items and endgame", "Objets et fin", "Oggetti e finale", "Gegenstände und Ende", "Objetos y final", "アイテムとエンディング"}}};
const Str6 kUnlocked = {{"Unlocked", "Débloqués", "Sbloccati", "Freigeschaltet", "Desbloqueados", "かいじょ"}};
const Str6 kLocked = {{"Locked", "Verrouillé", "Bloccato", "Gesperrt", "Bloqueado", "みかいじょ"}};
const Str6 kOptions = {{"Options", "Options", "Opzioni", "Optionen", "Opciones", "オプション"}};
const Str6 kPage = {{"Page", "Page", "Pagina", "Seite", "Página", "ページ"}};

// The description's lines: words joined while the line stays within `width` (a word wider than that has a line of its own).
std::vector<std::u32string> wrap_words(const std::u32string& text, float width, const std::function<float(const std::u32string&)>& measure) {
  std::vector<std::u32string> lines;
  std::u32string line;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(U' ', start);
    if (end == std::u32string::npos) end = text.size();
    const std::u32string word = text.substr(start, end - start);
    const std::u32string joined = line.empty() ? word : line + U" " + word;
    if (!line.empty() && measure(joined) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = joined;
    }
    start = end + 1;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

}  // namespace

int MainMenu::Impl::achievement_page_row(bool options) {
  for (int i = 0; i < kOcPageCount; ++i)
    if (kOcPages[i].page == OcPage::Achievements && kOcPages[i].listed != options) return i;
  return 0;
}

void MainMenu::Impl::filter_achievement_list() {
  rc_ids = achievements ? filter_achievements(*achievements, ach_filter) : std::vector<int>{};
  rc_cur = std::clamp(rc_cur, 0, std::max(static_cast<int>(rc_ids.size()) - 1, 0));
  rc_top = std::clamp(rc_top, 0, rc_cur);
}

void MainMenu::Impl::enter_achievements() {
  ach_filter = 0;
  rc_cur = rc_top = rc_part = 0, rc_rows = kRows;
  filter_achievement_list();
}

// Up / Down one row, Left / Right one screen (update_records); L1 / R1 the previous / next page of 17 rows (wrap, sfx 1); Square steps
// the category filter (wrap, sfx 1) with the cursor on the first row; Triangle opens the Options page (sfx 8); Cancel returns to the list
// the page was opened from.
void MainMenu::Impl::update_achievements(const Nav& nav) {
  filter_achievement_list();
  const int count = static_cast<int>(rc_ids.size());
  if (nav.tri) {
    sfx(8);
    oc_shown = achievement_page_row(true);
    show(MenuScreen::OcPage);
    return;
  }
  if (nav.sq) {
    ach_filter = wrap(ach_filter + 1, kAchievementFilters);
    rc_cur = rc_top = 0;
    filter_achievement_list();
    sfx(1);
    return;
  }
  const int pages = (std::max(count, 1) + kRows - 1) / kRows;
  if ((nav.l1 || nav.r1) && pages > 1) {
    const int next = wrap(rc_cur / kRows + (nav.l1 ? -1 : 1), pages);
    rc_cur = rc_top = next * kRows;
    sfx(1);
    return;
  }
  update_records(nav);
}

// Left: the filtered rows in the roster panel (record_list: the achievement's number in the table, its title, bright once unlocked,
// "???" for a locked spoiler unless Show spoilers; tagged with the date earned, or the progress while locked); below the panel the
// Square glyph with the filter and the Triangle glyph with "Options", and with more than one page the L1 / R1 glyphs with the page
// ("Page 1/11"). Right: the chosen row's title, its state (Unlocked and the date, or Locked and the progress), its category and its
// description in lines of 228 px; the description panel (frame 0x3E at (246, 318)) with the unlocked count of the whole set and, under a
// filter, of the rows it shows.
void MainMenu::Impl::build_achievements() {
  const int count = static_cast<int>(rc_ids.size());
  std::vector<RecordRow> rows;
  int shown_unlocked = 0;
  for (const int index : rc_ids) {
    const AchievementRow& entry = achievements->rows[static_cast<size_t>(index)];
    RecordRow row;
    row.number = index + 1;
    row.seen = entry.unlocked;
    row.name = utf32(achievement_hidden(entry, ach_spoilers) ? "???" : entry.title);
    row.tag = utf32(entry.unlocked ? entry.date : entry.progress);
    row.tag_colour = entry.unlocked ? kText : kName;
    shown_unlocked += entry.unlocked ? 1 : 0;
    rows.push_back(std::move(row));
  }
  record_list(rows, {});
  btn(0x10, 0x182, 2);
  TPfit(40, 388, utf32(S6(kFilters[ach_filter])), 84, kText, 0.8f);
  btn(0x80, 0x182, 3);
  TPfit(152, 388, utf32(S6(kOptions)), 80, kText, 0.8f);
  const int pages = (std::max(count, 1) + kRows - 1) / kRows;
  if (pages > 1) {
    btn(0x10, 0x19A, 4);
    btn(0x28, 0x19A, 5);
    TPfit(64, 412, utf32(S6(kPage)) + U" " + utf32(std::to_string(rc_cur / kRows + 1) + "/" + std::to_string(pages)), 168, kText, 0.8f);
  }

  group(Right);
  Frame(0x3E, 246, 318);
  const int total = achievements ? static_cast<int>(achievements->rows.size()) : 0;
  panel_line(0, utf32(S6(kUnlocked)), utf32(std::to_string(achievements ? achievements->unlocked : 0) + " / " + std::to_string(total)));
  if (ach_filter != 0) panel_line(1, utf32(S6(kFilters[ach_filter])), utf32(std::to_string(shown_unlocked) + " / " + std::to_string(count)));
  if (count == 0) return;
  const AchievementRow& entry = achievements->rows[static_cast<size_t>(rc_ids[static_cast<size_t>(rc_cur)])];
  const bool hidden = achievement_hidden(entry, ach_spoilers);
  constexpr float kLeft = 258, kWidth = 228, kLine = 19, kRel = 0.8f;
  TPfit(kLeft, 46, utf32(hidden ? "???" : entry.title), kWidth, entry.unlocked ? kText : kName, 0.9f);
  if (entry.unlocked) {
    TPfit(kLeft, 74, utf32(S6(kUnlocked)), 110, kSel, kRel);
    TPfit(kLeft + 116, 74, utf32(entry.date), 112, kText, kRel);
  } else {
    TPfit(kLeft, 74, utf32(S6(kLocked)), 110, kName, kRel);
    if (!entry.progress.empty()) TPfit(kLeft + 116, 74, utf32(entry.progress), 112, kText, kRel);
  }
  TPfit(kLeft, 74 + kLine, utf32(S6(kFilters[std::clamp(entry.category + 1, 1, kAchievementFilters - 1)])), kWidth, kCap, kRel);
  const std::vector<std::u32string> lines =
      wrap_words(utf32(hidden ? "???" : entry.description), kWidth, [&](const std::u32string& line) { return twp(line, kRel); });
  for (size_t i = 0; i < lines.size() && i < 9; ++i) TPfit(kLeft, 74 + kLine * static_cast<float>(i + 3), lines[i], kWidth, kText, kRel);
}

}  // namespace oc::ui
