// The Characters, Elements, Key items and Treasure pages of the OpenCross row: one list each in the roster panel (what the open game
// holds, plus what the collection file remembers from every save; the rest "???"), the chosen entry on the right, and what the room-script
// scan tells about it (records_info.h): where it is found, how, until which story point, whether the chance is gone. The Hints setting
// decides how much of that a page shows.
#include <algorithm>
#include <cstdio>

#include "core/element_icon.h"
#include "ui/menu/main_menu_impl.h"
#include "ui/menu/menu_common.h"

namespace oc::ui {

namespace {

constexpr int kRows = 17;
constexpr int kCharacters = RecordsInfo::kCharacters;
constexpr int kLastElement = 0x7E;
constexpr int kLastKeyItem = 64;
// Hints setting values: off, hints, full.
constexpr int kHintsOff = 0, kHintsFull = 2;

const Str6 kWait = {{"Reading the game...", "Lecture du jeu...", "Lettura del gioco...", "Spiel wird gelesen...", "Leyendo el juego...", "よみこみちゅう"}};
const Str6 kMissed = {{"Missed", "Manqué", "Perso", "Verpasst", "Perdido", "のがした"}};
const Str6 kSoon = {{"Closes soon", "Bientôt trop tard", "Presto troppo tardi", "Bald zu spät", "Pronto será tarde", "もうすぐおわり"}};
const Str6 kWhere = {{"Where", "Où", "Dove", "Wo", "Dónde", "ばしょ"}};
const Str6 kHow = {{"How", "Comment", "Come", "Wie", "Cómo", "ほうほう"}};
const Str6 kUntil = {{"Until", "Jusqu'à", "Fino a", "Bis", "Hasta", "きげん"}};
const Str6 kStory = {{"With the story", "Avec l'histoire", "Con la storia", "Mit der Geschichte", "Con la historia", "ストーリーで"}};
const Str6 kChoice = {{"A choice in a talk", "Un choix en parlant", "Una scelta parlando", "Eine Wahl im Gespräch", "Una elección al hablar", "かいわでえらぶ"}};
const Str6 kWithItem = {{"With", "Avec", "Con", "Mit", "Con", "もちもの："}};
const Str6 kWithParty = {{"With in party", "Avec dans l'équipe", "Con in squadra", "Mit in der Gruppe", "Con en el grupo", "パーティー："}};
const Str6 kAlone = {{"If another did not join", "Si un autre n'est pas venu", "Se un altro non si è unito", "Wenn ein anderer fehlt", "Si otro no se unió", "ほかがいないとき"}};
const Str6 kHeld = {{"Held", "Possédés", "Posseduti", "Im Besitz", "En posesión", "しょじ"}};
const Str6 kUsable = {{"Usable by", "Utilisable par", "Usabile da", "Nutzbar von", "Usable por", "つかえる"}};
const Str6 kEveryone = {{"Everyone", "Tout le monde", "Tutti", "Alle", "Todos", "みんな"}};
const Str6 kNoOne = {{"No one yet", "Personne encore", "Nessuno per ora", "Noch niemand", "Nadie aún", "まだいない"}};
const Str6 kShops = {{"Shops", "Boutiques", "Negozi", "Läden", "Tiendas", "おみせ"}};
const Str6 kEnemies = {{"Enemies", "Ennemis", "Nemici", "Gegner", "Enemigos", "エネミー"}};
const Str6 kChest = {{"Chest", "Coffre", "Forziere", "Truhe", "Cofre", "たからばこ"}};
const Str6 kPickup = {{"Pickup", "Trouvaille", "Raccolta", "Fundstück", "Hallazgo", "ひろいもの"}};
const Str6 kChests = {{"Chests", "Coffres", "Forzieri", "Truhen", "Cofres", "たからばこ"}};
const Str6 kPickups = {{"Pickups", "Trouvailles", "Raccolte", "Fundstücke", "Hallazgos", "ひろいもの"}};
const Str6 kAllAreas = {{"All areas", "Partout", "Ovunque", "Überall", "En total", "ぜんぶ"}};
const Str6 kElsewhere = {{"Elsewhere", "Ailleurs", "Altrove", "Anderswo", "Otros lugares", "そのほか"}};
const Str6 kUnopened = {{"unopened", "pas ouverts", "non aperti", "ungeöffnet", "sin abrir", "みかいふう"}};
const Str6 kPage = {{"Page", "Page", "Pagina", "Seite", "Página", "ページ"}};

}  // namespace

// ===================================================== shared
// Up / Down one row, Left / Right one screen (no wrap, sfx 1 when the cursor moves); Cancel returns to the page list.
void MainMenu::Impl::update_records(const Nav& nav) {
  const int count = static_cast<int>(rc_ids.size());
  const int before = rc_cur;
  if (nav.up) rc_cur = std::max(rc_cur - 1, 0);
  if (nav.down) rc_cur = std::min(rc_cur + 1, std::max(count - 1, 0));
  if (nav.left) rc_cur = std::max(rc_cur - kRows, 0);
  if (nav.right) rc_cur = std::min(rc_cur + kRows, std::max(count - 1, 0));
  if (rc_cur != before) rc_part = 0, sfx(1);
  scroll_to(rc_cur, rc_top, kRows);
  if (nav.cancel) leave_page(), sfx(6);
}

// The list in the roster panel (frame 141 at (8, 32), 17 rows 20 px apart): the number (right edge x 48), the name (from x 56, or from
// x 26 for a row without a number, shrunk to fit before the tag), the tag right-aligned at x 230; the cursor and the scroll arrows; below
// the panel the count line.
void MainMenu::Impl::record_list(const std::vector<RecordRow>& rows, const std::u32string& count_line) {
  const int count = static_cast<int>(rows.size());
  group(Left);
  Frame(141, 8, 32);
  if (rc_top + kRows < count) scroll_arrow(237, 354, false);
  if (rc_top > 0) scroll_arrow(237, 50, true);
  for (int row = 0; row < kRows && rc_top + row < count; ++row) {
    const RecordRow& entry = rows[static_cast<size_t>(rc_top + row)];
    const float cap = 42.2f + 20.0f * static_cast<float>(row);
    float left = 56, room = 170.0f;
    if (entry.number > 0) {
      char number[8];
      std::snprintf(number, sizeof number, "%03d", entry.number);
      HN(48, cap, number, kName, 0.29f);
    } else {
      left = 26, room = 200.0f;
    }
    if (!entry.tag.empty()) {
      text->prepare(face_gn, entry.tag);
      const float tag_width = text->measure(face_gn, entry.tag, 0.27f) / xscale;
      HL(230, cap + 1.5f, entry.tag, entry.tag_colour, 0.27f, true);
      room = std::max(60.0f, room - tag_width - 6.0f);
    }
    if (entry.icon >= 0 && element_symbols()) {
      symbol(entry.icon, PX(62), PY(cap + 6.0f), 36.0f);
      left += 14, room -= 14;
    }
    text->prepare(face_gn, entry.name);
    const float width = text->measure(face_gn, entry.name, 0.35f) / xscale;
    HL(left, cap, entry.name, entry.seen ? kText : kName, width < room ? 0.35f : 0.35f * room / width);
  }
  if (count > 0) tri(0, 42.0f + 20.0f * static_cast<float>(rc_cur - rc_top), true);
  TPfit(24, 388, count_line, 208, kText, 0.8f);
}

// The short wait line while the scan runs.
void MainMenu::Impl::records_wait(float x, float y) { TPfit(x, y, utf32(S6(kWait)), 220, kName, 0.8f); }

// A line of the description panel (frame 0x3E at (246, 318)): the label at x 258 and the value at x 342, shrunk to fit.
void MainMenu::Impl::panel_line(int line, const std::u32string& label, const std::u32string& value, u32 colour) {
  const float y = 327.0f + 18.5f * static_cast<float>(line);
  if (!label.empty()) TPfit(258, y, label, 78, kCap, 0.8f);
  TPfit(label.empty() ? 258.0f : 342.0f, y, value, label.empty() ? 232.0f : 148.0f, colour, 0.8f);
}

std::u32string MainMenu::Impl::area_list(const std::vector<std::string>& areas) {
  std::string out;
  for (const std::string& area : areas) out += (out.empty() ? "" : ", ") + area;
  return utf32(out.empty() ? "?" : out);
}

// The save caption of the story point a story value belongs to (its first line).
std::u32string MainMenu::Impl::story_caption(int story) { return utf32(location_lines(static_cast<u32>(story)).first); }

// ===================================================== Characters page
bool MainMenu::Impl::character_seen(int character) const {
  return records_show_all || (gs && gs->slot.recruited(character)) || (collection && collection->had(save::Collection::kCharacterChunk, character));
}

void MainMenu::Impl::enter_characters() {
  rc_ids.clear();
  for (int character = 0; character < kCharacters; ++character) rc_ids.push_back(character);
  rc_cur = rc_top = rc_part = 0;
}

// Left: the 44 characters by id ("???" until recruited in some save; tagged "Missed" once the chance is gone, "Closes soon" with hints),
// the count of those seen. Right: the battle model in the box (250, 44)..(488, 222), a black silhouette until seen; the member card
// (frame 7 at (282, 228)): the open game's card while recruited there, else the name (or "???") with the numbers hidden; the description
// panel with what the scan tells, by the Hints setting.
void MainMenu::Impl::build_characters() {
  const RecordsInfo* info = records_info;
  const save::SaveSlot& slot = gs->slot;
  std::vector<RecordRow> rows;
  int seen = 0;
  for (const int character : rc_ids) {
    RecordRow row;
    row.number = character + 1;
    row.seen = character_seen(character);
    seen += row.seen ? 1 : 0;
    row.name = row.seen ? char_name(character) : utf32("???");
    if (info && character_missed(*info, slot, character)) row.tag = utf32(S6(kMissed));
    else if (info && records_hints != kHintsOff && character_closing(*info, slot, character)) row.tag = utf32(S6(kSoon));
    rows.push_back(std::move(row));
  }
  record_list(rows, utf32(std::to_string(seen) + " / " + std::to_string(kCharacters)));
  if (rc_ids.empty()) return;

  group(Right);
  const int character = rc_ids[static_cast<size_t>(rc_cur)];
  const bool seen_here = character_seen(character);
  // The panel's lines: why a character can no longer join, or with hints where, how and until when; none for a recruited one.
  struct PanelLine {
    std::u32string label, value;
    u32 colour = kText;
  };
  std::vector<PanelLine> panel;
  if (!slot.recruited(character)) {
    if (!info) {
      panel.push_back({{}, utf32(S6(kWait)), kName});
    } else {
      const CharacterInfo& entry = info->characters[static_cast<size_t>(character)];
      const int story = slot.var(0);
      if (character_missed(*info, slot, character)) {
        panel.push_back({{}, utf32(S6(kMissed)), kSel});
        if (records_hints == kHintsFull) panel.push_back({utf32(S6(kUntil)), story_caption(entry.last_story)});
      } else if (records_hints != kHintsOff) {
        // The places still open, the first of them for how and until.
        std::vector<std::string> areas;
        const RecruitSite* first = nullptr;
        for (const RecruitSite& site : entry.sites) {
          if ((site.story_hi < story && !story_reopens(*info, site.story_hi)) || site.area.empty()) continue;
          if (std::find(areas.begin(), areas.end(), site.area) == areas.end()) areas.push_back(site.area);
          if (!first) first = &site;
        }
        if (!areas.empty()) panel.push_back({utf32(S6(kWhere)), area_list(areas)});
        if (records_hints == kHintsFull && first) {
          std::u32string how = utf32(S6(kStory));
          if (first->how & kHowItem) how = utf32(S6(kWithItem)) + U" " + key_item_name(first->key_item);
          else if (first->how & kHowParty) how = utf32(S6(kWithParty)) + U" " + char_name(first->partner);
          else if (first->how & kHowChoice) how = utf32(S6(kChoice));
          else if (first->how & kHowAlone) how = utf32(S6(kAlone));
          panel.push_back({utf32(S6(kHow)), how});
          if (entry.last_story < kStoryOpen) panel.push_back({utf32(S6(kUntil)), story_caption(entry.last_story)});
        }
        if (character_closing(*info, slot, character)) {
          if (panel.size() > 3) panel.resize(3);
          panel.push_back({{}, utf32(S6(kSoon)), kSel});
        }
      }
    }
  }
  // The model box fills the right column above the card; the card sits at the bottom when no panel is drawn.
  const bool has_panel = !panel.empty();
  const float kBoxX = 246, kBoxY = 38, kBoxW = 254, kCardX = 282, kCardY = has_panel ? 228.0f : 340.0f, kBoxH = kCardY - 2 - kBoxY;
  enemy_slot = EnemyModelSlot{};
  enemy_slot.model = model_number(character);
  // The weapon the character holds in the open game, as the Status screen picks it.
  MenuModelSlot gear;
  model_gear(character, gear);
  enemy_slot.weapon_part = gear.weapon_part;
  enemy_slot.silhouette = !seen_here;
  enemy_slot.box_x = kBoxX, enemy_slot.box_y = kBoxY, enemy_slot.box_w = kBoxW, enemy_slot.box_h = kBoxH;
  enemy_wanted = enemy_view != nullptr;
  Frame(7, kCardX, kCardY);
  if (slot.recruited(character)) {
    card_hd(kCardX, kCardY, character, true, false);
  } else {
    CardValues values;
    values.name = seen_here ? char_name(character) : utf32("???");
    values.innate = seen_here ? gs->innate(character) : 0;
    values.known = false;
    card_body(kCardX, kCardY, values, true, false, 7);
  }
  if (!has_panel) return;
  Frame(0x3E, 246, 318);
  for (size_t line = 0; line < panel.size(); ++line)
    panel_line(static_cast<int>(line), panel[line].label, panel[line].value, panel[line].colour);
}

// ===================================================== Elements page
int MainMenu::Impl::element_held(int id) const {
  int held = gs->element_count(id);
  for (int character = 0; character < kCharacters; ++character)
    if (gs->slot.recruited(character)) held += gs->copies_placed(character, id);
  return held;
}

bool MainMenu::Impl::element_seen(int id) const {
  return records_show_all || element_held(id) > 0 || (collection && collection->had(save::Collection::kElementChunk, id));
}

// Every element id with a name (1..0x7E: the elements, the summons, the traps and the Chrono Cross).
void MainMenu::Impl::enter_elements() {
  rc_ids.clear();
  for (int id = 1; id <= kLastElement; ++id)
    if (rules && rules->elem(id) && !trim(import::u32_to_utf8(element_name(id))).empty()) rc_ids.push_back(id);
  rc_cur = rc_top = rc_part = 0;
}

// Left: the elements by id with their colour symbol (while the symbols are on), "???" until held in some save, "Missed" for a one-off
// no longer obtainable; the count of those seen. Right: the element's row plate with its name and the copies held (stock and the
// recruited characters' grids), its level, reach and description, who of the recruited characters can place it; the panel: where it is
// given (the scan's places, the element shops, enemies), with hints also for one never held.
void MainMenu::Impl::build_elements() {
  const RecordsInfo* info = records_info;
  std::vector<RecordRow> rows;
  int seen = 0;
  for (const int id : rc_ids) {
    RecordRow row;
    row.number = id;
    row.seen = element_seen(id);
    seen += row.seen ? 1 : 0;
    row.name = row.seen ? element_name(id) : utf32("???");
    row.icon = row.seen ? colour_index(0, id) : -1;
    if (row.icon > 5) row.icon = -1;
    if (info)
      if (const auto found = info->elements.find(id); found != info->elements.end() && !row.seen && give_missed(*info, found->second, gs->slot))
        row.tag = utf32(S6(kMissed));
    rows.push_back(std::move(row));
  }
  record_list(rows, utf32(std::to_string(seen) + " / " + std::to_string(rc_ids.size())));
  if (rc_ids.empty()) return;

  group(Right);
  const int id = rc_ids[static_cast<size_t>(rc_cur)];
  const bool seen_here = element_seen(id);
  if (seen_here) {
    plate(0, id, 275, 42);
    const std::u32string name = element_name(id);
    const float rel = fit_rel(name, 140, 0.9f);
    TP(301.0f + (128.0f - twp(name, rel)) * 0.5f, 47, name, kText, rel);
    TPfit(258, 78, utf32(S6(kHeld)), 100, kCap, 0.8f);
    Count(453, 78, std::min(element_held(id), 99));
    element_info(id, 260, 102, 138, 19.5f);
    std::u32string users;
    int eligible = 0, recruited = 0;
    for (int character = 0; character < kCharacters; ++character) {
      if (!gs->slot.recruited(character)) continue;
      ++recruited;
      if (!gs->eligible(character, id)) continue;
      ++eligible;
      users += (users.empty() ? U"" : U", ") + char_name(character);
    }
    TPfit(258, 186, utf32(S6(kUsable)), 220, kCap, 0.8f);
    TPfit(258, 205, eligible == 0 ? utf32(S6(kNoOne)) : eligible == recruited ? utf32(S6(kEveryone)) : users, 222, kText, 0.8f);
  } else {
    empty_box(275, 42);
    TP(301.0f + (128.0f - twp(utf32("???"), 0.9f)) * 0.5f, 47, utf32("???"), kName, 0.9f);
  }
  Frame(0x3E, 246, 318);
  if (!info) {
    panel_line(0, {}, utf32(S6(kWait)), kName);
    return;
  }
  const auto found = info->elements.find(id);
  if (found == info->elements.end()) return;
  const GiveInfo& give = found->second;
  int line = 0;
  if (!seen_here && give_missed(*info, give, gs->slot)) panel_line(line++, {}, utf32(S6(kMissed)), kSel);
  if (!seen_here && records_hints == kHintsOff) return;
  if (!give.areas.empty()) panel_line(line++, utf32(S6(kWhere)), area_list(give.areas));
  std::u32string other;
  if (give.shop) other = utf32(S6(kShops));
  if (give.enemy) other += (other.empty() ? U"" : U", ") + utf32(S6(kEnemies));
  if (!other.empty()) panel_line(line++, give.areas.empty() ? utf32(S6(kWhere)) : std::u32string(), other);
  if (records_hints == kHintsFull && !seen_here && !give.repeatable && !give.shop && !give.enemy && !give.one_offs.empty()) {
    int last = 0;
    for (const GiveInfo::OneOff& one_off : give.one_offs) last = std::max(last, one_off.story_hi);
    if (last < kStoryOpen && line < 4) panel_line(line++, utf32(S6(kUntil)), story_caption(last));
  }
}

// ===================================================== Key items page
bool MainMenu::Impl::key_item_seen(int id) const {
  return records_show_all || gs->has_key_item(id) || (collection && collection->had(save::Collection::kKeyItemChunk, id));
}

void MainMenu::Impl::enter_key_items() {
  rc_ids.clear();
  for (int id = 1; id <= kLastKeyItem; ++id)
    if (!trim(import::u32_to_utf8(key_item_name(id))).empty()) rc_ids.push_back(id);
  rc_cur = rc_top = rc_part = 0;
}

// Left: the key items by id, "???" until held in some save, "Missed" for one no longer obtainable; the count of those seen. Right: the
// Key Items screen's picture at 160 x 160 (black until seen), the scan's lines under it (where it is given; with hints also for one never
// held), and in the description panel the item's own three lines once seen.
void MainMenu::Impl::build_key_items() {
  const RecordsInfo* info = records_info;
  std::vector<RecordRow> rows;
  int seen = 0;
  for (const int id : rc_ids) {
    RecordRow row;
    row.number = id;
    row.seen = key_item_seen(id);
    seen += row.seen ? 1 : 0;
    row.name = row.seen ? key_item_name(id) : utf32("???");
    if (info)
      if (const auto found = info->key_items.find(id); found != info->key_items.end() && !row.seen && give_missed(*info, found->second, gs->slot))
        row.tag = utf32(S6(kMissed));
    rows.push_back(std::move(row));
  }
  record_list(rows, utf32(std::to_string(seen) + " / " + std::to_string(rc_ids.size())));
  if (rc_ids.empty()) return;

  group(Right);
  const int id = rc_ids[static_cast<size_t>(rc_cur)];
  const bool seen_here = key_item_seen(id);
  char path[48];
  std::snprintf(path, sizeof path, "hd/menu/keyitem/%02d.png", id);
  if (const Tex* tex = texs.load(path)) Img(tex, PX(289), PY(40), PX(160), PY(160), 0, 0, 1, 1, seen_here ? kWhite : rgb(0, 0, 0));
  Frame(0x3E, 246, 318);
  if (seen_here) {
    const float top = font_build(face_gn, import::Ps1Font::Build::UsaMenu) ? 335.0f : 334.0f;
    for (int line = 0; line < 3; ++line) {
      const std::string description = trim(table_row("keyitem", id * 3 + line));
      if (!description.empty()) key_item_line(description, ps1_text() ? 254.0f : 252.8f, top + 24.0f * static_cast<float>(line), 228);
    }
  }
  if (!info) {
    records_wait(258, 212);
    return;
  }
  const auto found = info->key_items.find(id);
  if (found == info->key_items.end()) return;
  const GiveInfo& give = found->second;
  float y = 212;
  if (!seen_here && give_missed(*info, give, gs->slot)) TPfit(258, y, utf32(S6(kMissed)), 222, kSel, 0.8f), y += 19;
  if (!seen_here && records_hints == kHintsOff) return;
  if (!give.areas.empty()) {
    TPfit(258, y, utf32(S6(kWhere)), 78, kCap, 0.8f);
    TPfit(342, y, area_list(give.areas), 140, kText, 0.8f);
    y += 19;
  }
  if (records_hints == kHintsFull && !seen_here && !give.repeatable && !give.one_offs.empty()) {
    int last = 0;
    for (const GiveInfo::OneOff& one_off : give.one_offs) last = std::max(last, one_off.story_hi);
    if (last < kStoryOpen) {
      TPfit(258, y, utf32(S6(kUntil)), 78, kCap, 0.8f);
      TPfit(342, y, story_caption(last), 140, kText, 0.8f);
    }
  }
}

// ===================================================== Treasure page
void MainMenu::Impl::enter_treasure() {
  rc_ids.clear();
  rc_cur = rc_top = rc_part = 0;
}

// The areas fill in once the scan is ready; L1 / R1 turn the pages of the chosen area's treasure list (wrap, sfx 1).
void MainMenu::Impl::update_treasure(const Nav& nav) {
  if (records_info && rc_ids.size() != records_info->areas.size()) {
    rc_ids.clear();
    for (size_t area = 0; area < records_info->areas.size(); ++area) rc_ids.push_back(static_cast<int>(area));
  }
  if ((nav.l1 || nav.r1) && records_info && !rc_ids.empty()) {
    const int pages = (std::max(treasure_rows(rc_ids[static_cast<size_t>(rc_cur)]), 1) + kTreasureRows - 1) / kTreasureRows;
    if (pages > 1) rc_part = wrap(rc_part + (nav.l1 ? -1 : 1), pages), sfx(1);
  }
  update_records(nav);
}

// The rows the chosen area lists: every treasure with hints, else only the opened ones.
int MainMenu::Impl::treasure_rows(int area) const {
  int rows = 0;
  for (const TreasureInfo& treasure : records_info->treasures)
    if (treasure.area == area && (records_hints != kHintsOff || records_show_all || treasure.flag.is_set(gs->slot))) ++rows;
  return rows;
}

std::u32string MainMenu::Impl::give_name(int id) {
  if (id >= 0x400) return key_item_name(id - 0x400);
  if (id >= 0x200) return element_name(id - 0x200);
  return item_name(id);
}

// Left: the areas by name and world, each with its opened / total, the count over every area below. Right: the chosen area's treasures, 13 rows 20
// px apart from y 44 (the kind at x 258, what it gave at x 330 once opened; unopened ones are listed with hints, as "???", or with full
// hints by what they hold, dimmed); when the area has more than one page, the L1 / R1 glyphs with the page ("Page 1/2") below the list;
// the panel: chests, pickups and their unopened count.
void MainMenu::Impl::build_treasure() {
  const RecordsInfo* info = records_info;
  if (!info) {
    group(Left);
    Frame(141, 8, 32);
    records_wait(24, 42);
    return;
  }
  const save::SaveSlot& slot = gs->slot;
  const std::vector<TreasureTally> tally = tally_treasures(*info, slot);
  std::vector<RecordRow> rows;
  int opened = 0, total = 0;
  for (const int area : rc_ids) {
    RecordRow row;
    const TreasureArea& place = info->areas[static_cast<size_t>(area)];
    row.name = place.name.empty() ? utf32(S6(kElsewhere)) : with_world(utf32(place.name), place.world);
    const TreasureTally& count = tally[static_cast<size_t>(area)];
    row.seen = count.opened > 0;
    row.tag = utf32(std::to_string(count.opened) + "/" + std::to_string(count.total));
    row.tag_colour = count.opened == count.total ? kText : kName;
    opened += count.opened, total += count.total;
    rows.push_back(std::move(row));
  }
  record_list(rows, utf32(S6(kAllAreas)) + U"  " + utf32(std::to_string(opened) + " / " + std::to_string(total)));
  if (rc_ids.empty()) return;

  group(Right);
  const int area = rc_ids[static_cast<size_t>(rc_cur)];
  int index = 0, chests = 0, chests_open = 0, pickups = 0, pickups_open = 0;
  for (const TreasureInfo& treasure : info->treasures) {
    if (treasure.area != area) continue;
    const bool is_open = records_show_all || treasure.flag.is_set(slot);
    (treasure.chest ? chests : pickups) += 1;
    (treasure.chest ? chests_open : pickups_open) += is_open ? 1 : 0;
    if (!is_open && records_hints == kHintsOff) continue;
    const int row = index++ - rc_part * kTreasureRows;
    if (row < 0 || row >= kTreasureRows) continue;
    const float y = 44.0f + 20.0f * static_cast<float>(row);
    TPfit(258, y, utf32(S6(treasure.chest ? kChest : kPickup)), 68, kCap, 0.8f);
    std::u32string gives;
    for (const int id : treasure.gives) gives += (gives.empty() ? U"" : U" / ") + give_name(id);
    const bool named = is_open || records_hints == kHintsFull;
    TPfit(330, y, named ? gives : utf32("???"), 156, is_open ? kText : kName, 0.8f);
  }
  const int pages = (std::max(index, 1) + kTreasureRows - 1) / kTreasureRows;
  if (pages > 1) {
    // Below the list, as the Bestiary's variant hint: the L1 / R1 glyphs (the keys in keyboard mode) and the page of the chosen area.
    group(Left);
    btn(0x10, 0x19A, 4);
    btn(0x28, 0x19A, 5);
    TPfit(64, 412, utf32(S6(kPage)) + U" " + utf32(std::to_string(rc_part + 1) + "/" + std::to_string(pages)), 168, kText, 0.8f);
    group(Right);
  }
  Frame(0x3E, 246, 318);
  panel_line(0, utf32(S6(kChests)), utf32(std::to_string(chests_open) + " / " + std::to_string(chests)));
  panel_line(1, utf32(S6(kPickups)), utf32(std::to_string(pickups_open) + " / " + std::to_string(pickups)));
  const int unopened = chests + pickups - chests_open - pickups_open;
  if (unopened > 0) panel_line(2, {}, utf32(std::to_string(unopened) + " " + S6(kUnopened)), kName);
}

}  // namespace oc::ui
