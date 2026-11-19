// The OpenCross row of the main menu: its place in the list (above Save/Load, while any of its pages is on), its two list levels (the
// groups Chronicle, Extras, Dialogue log and Achievements, then a group's pages, both drawn in the main list's slots with the dial and
// needle), the group and page tables (each of Chronicle and Extras ends on its Options page, main_menu_options.cpp; the Achievements'
// Options page is opened from the Achievements page, main_menu_achievements.cpp), the Bestiary page (one entry per species
// numbered in story order, three reveal stages read from the collection file, the creature's battle model turning above its card, and a
// lower panel with the loot, the places or the traits) and the DialogueLog page (the dialogue log window over the menu).
#include <algorithm>
#include <cstdio>
#include <map>

#include "core/log.h"
#include "ui/menu/main_menu_impl.h"
#include "ui/menu/menu_common.h"
#include "ui/ps1_font.h"

namespace oc::ui {

namespace mainmenu {
const Str6 kOpenCrossLabel = {{"OpenCross", "OpenCross", "OpenCross", "OpenCross", "OpenCross", "オープンクロス"}};
}  // namespace mainmenu

// The groups in list order.
const MainMenu::Impl::OcGroupDef MainMenu::Impl::kOcGroups[] = {
    {OcGroup::Chronicle, {{"Chronicle", "Chronique", "Cronaca", "Chronik", "Crónica", "クロニクル"}}},
    {OcGroup::Extras, {{"Extras", "Bonus", "Extra", "Extras", "Extras", "エクストラ"}}},
    {OcGroup::DialogueLog, {{"Dialogue log", "Dialogues", "Dialoghi", "Dialoge", "Diálogos", "かいわログ"}}},
    {OcGroup::Achievements, {{"Achievements", "Succès", "Obiettivi", "Erfolge", "Logros", "じっせき"}}},
};

// The pages in list order, grouped; a page is one row here. The titles are Latin in every language, as the stock screen titles are.
const MainMenu::Impl::OcPageDef MainMenu::Impl::kOcPages[] = {
    {OcPage::Bestiary, OcGroup::Chronicle, 0, {{"Bestiary", "Bestiaire", "Bestiario", "Bestiarium", "Bestiario", "モンスターずかん"}},
     {{"Bestiary", "Bestiaire", "Bestiario", "Bestiarium", "Bestiario", "Bestiary"}}, &Impl::enter_enemies, &Impl::update_enemies,
     &Impl::build_enemies},
    {OcPage::Characters, OcGroup::Chronicle, 0, {{"Characters", "Personnages", "Personaggi", "Figuren", "Personajes", "キャラクター"}},
     {{"Characters", "Personnages", "Personaggi", "Figuren", "Personajes", "Characters"}}, &Impl::enter_characters, &Impl::update_records,
     &Impl::build_characters},
    {OcPage::Elements, OcGroup::Chronicle, 0, {{"Elements", "Éléments", "Elementi", "Elemente", "Elementos", "エレメント"}},
     {{"Elements", "Éléments", "Elementi", "Elemente", "Elementos", "Elements"}}, &Impl::enter_elements, &Impl::update_records,
     &Impl::build_elements},
    {OcPage::KeyItems, OcGroup::Chronicle, 0, {{"Key items", "Objets clés", "Oggetti chiave", "Schlüsselobjekte", "Objetos clave", "だいじなもの"}},
     {{"Key Items", "Objets clés", "Oggetti chiave", "Schlüsselobjekte", "Objetos clave", "Key Items"}}, &Impl::enter_key_items,
     &Impl::update_records, &Impl::build_key_items},
    {OcPage::Treasure, OcGroup::Chronicle, 0, {{"Treasure", "Trésors", "Tesori", "Schätze", "Tesoros", "たからばこ"}},
     {{"Treasure", "Trésors", "Tesori", "Schätze", "Tesoros", "Treasure"}}, &Impl::enter_treasure, &Impl::update_treasure,
     &Impl::build_treasure},
    {OcPage::Bestiary, OcGroup::Chronicle, 0, {{"Options", "Options", "Opzioni", "Optionen", "Opciones", "オプション"}},
     {{"Options", "Options", "Opzioni", "Optionen", "Opciones", "Options"}}, &Impl::enter_options, &Impl::update_options, &Impl::build_options},
    {OcPage::Extras, OcGroup::Extras, 0, {{"Jukebox", "Juke-box", "Jukebox", "Jukebox", "Gramola", "ジュークボックス"}},
     {{"Jukebox", "Juke-box", "Jukebox", "Jukebox", "Gramola", "Jukebox"}}, &Impl::enter_extras, &Impl::update_extras, &Impl::build_jukebox},
    {OcPage::Extras, OcGroup::Extras, 1, {{"Theatre", "Cinéma", "Cinema", "Kino", "Cine", "シアター"}},
     {{"Theatre", "Cinéma", "Cinema", "Kino", "Cine", "Theatre"}}, &Impl::enter_extras, &Impl::update_extras, &Impl::build_theatre},
    {OcPage::Extras, OcGroup::Extras, 2, {{"Portraits", "Portraits", "Ritratti", "Porträts", "Retratos", "ポートレート"}},
     {{"Portraits", "Portraits", "Ritratti", "Porträts", "Retratos", "Portraits"}}, &Impl::enter_extras, &Impl::update_extras,
     &Impl::build_portraits},
    {OcPage::Extras, OcGroup::Extras, 3, {{"Models", "Modèles", "Modelli", "Modelle", "Modelos", "モデル"}},
     {{"Models", "Modèles", "Modelli", "Modelle", "Modelos", "Models"}}, &Impl::enter_extras, &Impl::update_extras, &Impl::build_models},
    {OcPage::Extras, OcGroup::Extras, 0, {{"Options", "Options", "Opzioni", "Optionen", "Opciones", "オプション"}},
     {{"Options", "Options", "Opzioni", "Optionen", "Opciones", "Options"}}, &Impl::enter_options, &Impl::update_options, &Impl::build_options},
    {OcPage::DialogueLog, OcGroup::DialogueLog, 0, {{"Dialogue log", "Dialogues", "Dialoghi", "Dialoge", "Diálogos", "かいわログ"}},
     {{"Dialogue log", "Dialogues", "Dialoghi", "Dialoge", "Diálogos", "Dialogue log"}}, &Impl::enter_dialogue_log, &Impl::update_dialogue_log,
     &Impl::build_dialogue_log},
    {OcPage::Achievements, OcGroup::Achievements, 0, {{"Achievements", "Succès", "Obiettivi", "Erfolge", "Logros", "じっせき"}},
     {{"Achievements", "Succès", "Obiettivi", "Erfolge", "Logros", "Achievements"}}, &Impl::enter_achievements, &Impl::update_achievements,
     &Impl::build_achievements},
    {OcPage::Achievements, OcGroup::Achievements, 0, {{"Options", "Options", "Opzioni", "Optionen", "Opciones", "オプション"}},
     {{"Options", "Options", "Opzioni", "Optionen", "Opciones", "Options"}}, &Impl::enter_options, &Impl::update_options, &Impl::build_options,
     false},
};

const MainMenu::Impl::OcPageDef& MainMenu::Impl::shown_def() const {
  return kOcPages[std::clamp(oc_shown, 0, kOcPageCount - 1)];
}

namespace {

constexpr int kSlots = 7;

const Str6 kLootLabels[4] = {{{"Drop", "Butin", "Bottino", "Beute", "Botín", "ドロップ"}},
                             {{"Rare drop", "Butin rare", "Bottino raro", "Seltene Beute", "Botín raro", "レアドロップ"}},
                             {{"Steal", "Vol", "Furto", "Diebesgut", "Robo", "ぬすむ"}},
                             {{"Rare steal", "Vol rare", "Furto raro", "Seltenes Diebesgut", "Robo raro", "レアぬすむ"}}};
const Str6 kWeak = {{"Weak", "Faible", "Debole", "Schwäche", "Débil", "じゃくてん"}};
const Str6 kOrders[3] = {{{"Order: number", "Ordre : numéro", "Ordine: numero", "Reihenfolge: Nummer", "Orden: número", "ならび：ばんごう"}},
                         {{"Order: name", "Ordre : nom", "Ordine: nome", "Reihenfolge: Name", "Orden: nombre", "ならび：なまえ"}},
                         {{"Order: area", "Ordre : lieu", "Ordine: luogo", "Reihenfolge: Ort", "Orden: lugar", "ならび：ばしょ"}}};
// The world names of RDE's world-map signs without their word for "world" (the Japanese sign is Latin).
const Str6 kHome = {{"Home", "Origine", "Origine", "Heimat", "Original", "Home"}};
const Str6 kAnother = {{"Another", "Autre", "Alternativo", "Andere", "Alternativo", "Another"}};
const Str6 kVariants = {{"Variants", "Variantes", "Varianti", "Varianten", "Variantes", "タイプ"}};
const Str6 kPanels[3] = {{{"Loot", "Butin", "Bottino", "Beute", "Botín", "アイテム"}},
                         {{"Places", "Lieux", "Luoghi", "Orte", "Lugares", "ばしょ"}},
                         {{"Traits", "Traits", "Tratti", "Merkmale", "Rasgos", "とくせい"}}};
const Str6 kReading = {{"Reading the game...", "Lecture du jeu...", "Lettura del gioco...", "Spiel wird gelesen...", "Leyendo el juego...",
                        "ゲームをよみこみちゅう…"}};
const Str6 kStatusWeak = {{"Weak status", "Altérations", "Alterazioni", "Anfällig für", "Estados", "よわいじょうたい"}};
const Str6 kDopple[3] = {{{"Cannot copy", "Non copiable", "Non copiabile", "Nicht kopierbar", "No copiable", "コピーできない"}},
                         {{"Can copy", "Copiable", "Copiabile", "Kopierbar", "Copiable", "コピーできる"}},
                         {{"In the pool", "Déjà copié", "Già copiato", "Schon kopiert", "Ya copiado", "コピーずみ"}}};
// The element whose name labels the Dopplegang row.
constexpr int kDopplegangElement = 0xEE;
// The statuses the weakness masks can name (record +0x04, lo word): the bit and the shared-strings key of its name.
struct StatusName {
  u32 bit;
  const char* key;
};
constexpr StatusName kStatusNames[] = {{0x1, "毒"},       {0x8, "やけど"},    {0x10, "ねんざ"},      {0x20, "風邪"},
                                       {0x100, "臆病"},   {0x2000, "過労"},   {0x4000, "睡眠"},      {0x20000, "暗闇"},
                                       {0x40000, "呪い"}, {0x100000, "混乱"}, {0x2000000, "めまい"}, {0x20000000, "凍結"}};

// The colour opposite an innate colour bit (green / yellow, white / black, red / blue); 0 = none.
int opposite_colour(int bit) {
  switch (bit) {
    case 0x04: return 0x40;
    case 0x08: return 0x10;
    case 0x10: return 0x08;
    case 0x20: return 0x80;
    case 0x40: return 0x04;
    case 0x80: return 0x20;
    default: return 0;
  }
}

}  // namespace

// ===================================================== the row and its lists
std::vector<int> MainMenu::Impl::main_rows() const {
  std::vector<int> rows = {0, 1, 2, 3, 4};
  if (oc_row()) rows.push_back(kOcRow);
  rows.push_back(5);
  return rows;
}

int MainMenu::Impl::main_pos(int row) const {
  const std::vector<int> rows = main_rows();
  const auto it = std::find(rows.begin(), rows.end(), row);
  return it == rows.end() ? static_cast<int>(rows.size()) - 1 : static_cast<int>(it - rows.begin());
}

std::vector<MainMenu::Impl::OcGroup> MainMenu::Impl::oc_groups() const {
  std::vector<OcGroup> groups;
  for (const OcGroupDef& def : kOcGroups)
    if (!oc_group_pages(def.group).empty()) groups.push_back(def.group);
  return groups;
}

std::vector<int> MainMenu::Impl::oc_group_pages(OcGroup group) const {
  std::vector<int> pages;
  for (size_t i = 0; i < static_cast<size_t>(kOcPageCount); ++i)
    if (kOcPages[i].group == group && kOcPages[i].listed && page_on(kOcPages[i])) pages.push_back(static_cast<int>(i));
  return pages;
}

std::vector<std::u32string> MainMenu::Impl::oc_labels() const {
  std::vector<std::u32string> labels;
  if (oc_group) {
    for (int row : oc_group_pages(*oc_group)) labels.push_back(utf32(S6(kOcPages[static_cast<size_t>(row)].label)));
    return labels;
  }
  for (OcGroup group : oc_groups())
    for (const OcGroupDef& def : kOcGroups)
      if (def.group == group) labels.push_back(utf32(S6(def.label)));
  return labels;
}

int MainMenu::Impl::oc_slot(int index) const {
  const int count = std::min(static_cast<int>(oc_labels().size()), kSlots);
  return (kSlots - count) / 2 + std::clamp(index, 0, kSlots - 1);
}

// With one group the group list is passed over: its page list, or its page when it has one (through the open transition).
void MainMenu::Impl::enter_oc() {
  const std::vector<OcGroup> groups = oc_groups();
  if (groups.size() == 1 && oc_group_pages(groups.front()).size() == 1) {
    oc_group.reset();
    oc_shown = oc_group_pages(groups.front()).front();
    enter(MenuScreen::OcPage);
    return;
  }
  enter(MenuScreen::OpenCross);
  if (groups.size() == 1) oc_group = groups.front();
}

// A group's page list on its first entry, at once; a group of one page opens that page through the open transition.
void MainMenu::Impl::open_oc_group(OcGroup group) {
  const std::vector<int> pages = oc_group_pages(group);
  if (pages.size() == 1) {
    oc_shown = pages.front();
    enter(MenuScreen::OcPage);
    return;
  }
  oc_group = group;
  oc_page_cur = 0;
}

// Both lists: Up / Down wrap (sfx 1); OK opens the entry (sfx 8); Cancel (sfx 6) goes back one level, on the entry it came from: from a
// page list to the group list, from the group list (or a page list that stands for it) to the main list on the OpenCross row. A page list
// whose pages have all gone returns to the group list, a group list with no group to the main list.
void MainMenu::Impl::update_oc_list(const Nav& nav) {
  const std::vector<OcGroup> groups = oc_groups();
  if (oc_group && oc_group_pages(*oc_group).empty()) oc_group.reset();
  if (groups.empty()) {
    back_to_main(kOcRow);
    return;
  }
  const bool skip_groups = groups.size() == 1;
  if (oc_group) {
    const std::vector<int> pages = oc_group_pages(*oc_group);
    const int count = static_cast<int>(pages.size());
    oc_page_cur = std::clamp(oc_page_cur, 0, count - 1);
    if (nav.up) oc_page_cur = wrap(oc_page_cur - 1, count), sfx(1);
    if (nav.down) oc_page_cur = wrap(oc_page_cur + 1, count), sfx(1);
    if (nav.cancel) {
      if (skip_groups) back_to_main(kOcRow);
      else oc_group.reset();
      sfx(6);
    } else if (nav.ok) {
      sfx(8);
      oc_shown = pages[static_cast<size_t>(oc_page_cur)];
      enter(MenuScreen::OcPage);
    }
    return;
  }
  const int count = static_cast<int>(groups.size());
  oc_cur = std::clamp(oc_cur, 0, count - 1);
  if (nav.up) oc_cur = wrap(oc_cur - 1, count), sfx(1);
  if (nav.down) oc_cur = wrap(oc_cur + 1, count), sfx(1);
  if (nav.cancel) {
    back_to_main(kOcRow);
    sfx(6);
  } else if (nav.ok) {
    sfx(8);
    open_oc_group(groups[static_cast<size_t>(oc_cur)]);
  }
}

// The list shown in place of the main list: each entry's label in its slot (left-aligned at the slot, shrunk to end before the leader's
// bend) with the slot's leader, the chosen one in the selection colour; the dial, the title and the bar as on the main list.
void MainMenu::Impl::oc_list() {
  clock_dial();
  const std::vector<std::u32string> labels = oc_labels();
  const MtStyle style = mt_style(7, lang_index());
  const render::FaceId face = slot_face(style.slot);
  const float scale = font::slot(style.slot).scale * style.mul;
  const u32 line_colour = rgb(200, 190, 190);
  for (size_t i = 0; i < labels.size() && i < static_cast<size_t>(kSlots); ++i) {
    const int slot = oc_slot(static_cast<int>(i));
    const std::u32string& label = labels[i];
    const u32 colour = static_cast<int>(i) == oc_list_cur() ? kSel : kText;
    const float x = slot_x(slot), y = slot_y(slot);
    if (!texs.hd()) {
      PT(x, y, label, colour);
    } else {
      const float room = 383.0f - 12.4f * static_cast<float>(slot) - x - 4.0f;
      text->prepare(face, label);
      const float width = text->measure(face, label, scale) / xscale;
      HT(face, x, y + (jp() ? 1.0f : 0.0f), label, colour, width > room ? scale * room / width : scale);
    }
    leader(slot, x, y, line_colour);
  }
  bar_and_title(kOcRow);
}

void MainMenu::Impl::enter_page() { (this->*shown_def().enter)(); }

// A page switched off while it is shown goes back to the list it was opened from.
void MainMenu::Impl::update_page(const Nav& nav) {
  const OcPageDef& def = shown_def();
  if (!page_on(def)) {
    leave_page();
    return;
  }
  (this->*def.update)(nav);
}

void MainMenu::Impl::build_page() {
  const OcPageDef& def = shown_def();
  (this->*def.build)();
  screen_title(utf32(S6(def.title)));
}

// Back through the back transition to the list the page was opened from, on this page's entry; to the main list when the OpenCross row
// opened the page itself.
void MainMenu::Impl::leave_page() {
  const std::vector<OcGroup> groups = oc_groups();
  const bool from_row = !oc_group && groups.size() <= 1;
  back_to_main(kOcRow);
  back_to_oc = !from_row;
}

// ===================================================== Bestiary page
bool MainMenu::Impl::enemy_met(int monster) const {
  return records_show_all || (collection && (collection->enemy_met(monster) || collection->enemy_defeated(monster)));
}

bool MainMenu::Impl::enemy_beaten(int monster) const { return records_show_all || (collection && collection->enemy_defeated(monster)); }

bool MainMenu::Impl::enemy_looted(int monster, int slot) const {
  return records_show_all || (collection && collection->enemy_loot(monster, slot));
}

bool MainMenu::Impl::species_met(const Species& entry) const {
  return std::any_of(entry.records.begin(), entry.records.end(), [&](int monster) { return enemy_met(monster); });
}

// The enemy-name category of sysmes (0x19), else the tables' English name.
std::u32string MainMenu::Impl::enemy_name(int monster) {
  if (const std::string* row = lt.sysmes((0x19u << 16) | static_cast<u32>(monster), lang); row && !trim(*row).empty()) return utf32(trim(*row));
  return utf32(tables->enemy_name(monster));
}

// A drop-row word: bit 15 = an element id, else an item id.
std::u32string MainMenu::Impl::loot_name(u16 word) { return (word & 0x8000) ? element_name(word & 0x7FFF) : item_name(word); }

std::u32string MainMenu::Impl::with_world(const std::u32string& name, data::World world) {
  if (world == data::World::None || name.empty()) return name;
  return name + U" (" + utf32(S6(world == data::World::Home ? kHome : kAnother)) + U")";
}

// A story area's name in the text language (data::place_name) with the world the scan gives it, read once per language.
std::u32string MainMenu::Impl::place_name(int area) {
  if (place_lang != lang) place_names.clear(), place_lang = lang;
  if (const auto found = place_names.find(area); found != place_names.end()) return found->second;
  const data::World world =
      enemy_order && static_cast<size_t>(area) < enemy_order->worlds.size() ? enemy_order->worlds[static_cast<size_t>(area)] : data::World::None;
  return place_names[area] = with_world(utf32(data::place_name(lt, area, lang)), world);
}

std::vector<std::u32string> MainMenu::Impl::record_places(int monster) {
  std::vector<std::u32string> out;
  if (!enemy_order || monster < 0 || static_cast<size_t>(monster) >= enemy_order->places.size()) return out;
  for (int area : enemy_order->places[static_cast<size_t>(monster)]) {
    std::u32string name = place_name(area);
    if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
  }
  return out;
}

std::vector<std::u32string> MainMenu::Impl::status_weaknesses(int monster) {
  std::vector<std::u32string> out;
  const u32 mask = tables->enemies[static_cast<size_t>(monster)].rd32(0x04);
  for (const StatusName& status : kStatusNames)
    if (mask & status.bit) out.push_back(utf32(cm(status.key)));
  return out;
}

// The pool is the 256-bit mask in variables 0x3E0..0x3FE; a record flagged uncopyable (+0xA6 bit 7) has no bit, the others their slot +0x2B.
int MainMenu::Impl::dopple_state(int monster) const {
  const data::StatRec& rec = tables->enemies[static_cast<size_t>(monster)];
  if (rec.raw[0xA6] & 0x80) return 0;
  const int slot = rec.raw[0x2B];
  return gs && (gs->slot.var(0x3E0 + 2 * (slot >> 4)) >> (slot & 15) & 1) ? 2 : 1;
}

// The species in number order: the scan service's order (data::bestiary_order over the unmodified game), then any record a mod's encounter
// groups place that it does not list and does not leave out, grouped by name. Nothing before the scan has finished.
void MainMenu::Impl::enter_enemies() {
  species.clear();
  en_cur = en_top = en_variant = en_sort = 0;
  en_built_from = enemy_order;
  if (!tables || !enemy_order) return;
  std::vector<bool> listed(tables->enemies.size(), false);
  for (size_t record = 0; record < listed.size() && record < enemy_order->left_out.size(); ++record) listed[record] = enemy_order->left_out[record];
  for (const std::vector<int>& records : enemy_order->species) {
    Species entry;
    for (int monster : records)
      if (monster >= 0 && static_cast<size_t>(monster) < listed.size()) entry.records.push_back(monster), listed[static_cast<size_t>(monster)] = true;
    if (!entry.records.empty()) species.push_back(std::move(entry));
  }
  std::map<std::string, size_t> extra;
  const size_t first_extra = species.size();
  for (const data::SceneRec& scene : tables->scenes)
    for (int group = 0; group < 3; ++group) {
      const size_t id = scene.enemy_id(group);
      if (scene.enemy_count(group) == 0 || id >= listed.size() || listed[id]) continue;
      listed[id] = true;
      const std::string& name = tables->enemy_name(static_cast<int>(id));
      const auto [it, fresh] = extra.try_emplace(name.empty() ? "#" + std::to_string(id) : data::species_key(name), species.size());
      if (fresh) species.push_back({});
      species[it->second].records.push_back(static_cast<int>(id));
    }
  for (size_t index = first_extra; index < species.size(); ++index) std::sort(species[index].records.begin(), species[index].records.end());
  log::info("menu: bestiary page: {} species", species.size());
  sort_enemies();
  show_species(0);
}

// By number: the species in their own order. By name: the met species by their name in the text language, then the others by number (an
// unmet name stays hidden). By area: the species whose places are shown (a variant beaten) by the name of their first place, then by
// number; the others after them by number.
void MainMenu::Impl::sort_enemies() {
  en_order.resize(species.size());
  for (size_t i = 0; i < species.size(); ++i) en_order[i] = static_cast<int>(i);
  if (en_sort == 0) return;
  std::vector<std::u32string> keys(species.size());
  std::vector<bool> shown(species.size());
  for (size_t i = 0; i < species.size(); ++i) {
    const Species& entry = species[i];
    if (en_sort == 1) {
      shown[i] = species_met(entry);
      if (shown[i]) keys[i] = enemy_name(entry.records.front());
      continue;
    }
    for (int monster : entry.records) {
      if (!enemy_beaten(monster)) continue;
      const std::vector<std::u32string> places = record_places(monster);
      if (places.empty()) continue;
      if (!shown[i] || places.front() < keys[i]) keys[i] = places.front();
      shown[i] = true;
    }
  }
  std::stable_sort(en_order.begin(), en_order.end(), [&](int a, int b) {
    const size_t ia = static_cast<size_t>(a), ib = static_cast<size_t>(b);
    if (shown[ia] != shown[ib]) return static_cast<bool>(shown[ia]);
    return shown[ia] && keys[ia] < keys[ib];
  });
}

// The cursor's species starts on its first met variant.
void MainMenu::Impl::show_species(int index) {
  en_variant = 0;
  if (index < 0 || static_cast<size_t>(index) >= en_order.size()) return;
  const Species& entry = species[static_cast<size_t>(en_order[static_cast<size_t>(index)])];
  for (size_t k = 0; k < entry.records.size(); ++k)
    if (enemy_met(entry.records[k])) {
      en_variant = static_cast<int>(k);
      return;
    }
}

// Up / Down move one row, Left / Right one screen (no wrap, sfx 1 when the cursor moves); L1 / R1 step through the species' variants
// (wrap); Square cycles the order (number / name / area), keeping the cursor on its species; Triangle cycles the lower panel (loot /
// places / traits); Cancel returns to the page list. The list is built again when the scan finishes while the page is shown.
void MainMenu::Impl::update_enemies(const Nav& nav) {
  if (enemy_order != en_built_from) enter_enemies();
  const int count = static_cast<int>(en_order.size());
  const int before = en_cur;
  if (nav.up) en_cur = std::max(en_cur - 1, 0);
  if (nav.down) en_cur = std::min(en_cur + 1, std::max(count - 1, 0));
  if (nav.left) en_cur = std::max(en_cur - kEnemyRows, 0);
  if (nav.right) en_cur = std::min(en_cur + kEnemyRows, std::max(count - 1, 0));
  if (en_cur != before) show_species(en_cur), sfx(1);
  if (nav.sq && count > 0) {
    const int shown = en_order[static_cast<size_t>(en_cur)];
    en_sort = (en_sort + 1) % 3;
    sort_enemies();
    en_cur = static_cast<int>(std::find(en_order.begin(), en_order.end(), shown) - en_order.begin());
    sfx(1);
  }
  if (nav.tri && count > 0) en_panel = (en_panel + 1) % 3, sfx(1);
  if ((nav.l1 || nav.r1) && count > 0) {
    const int variants = static_cast<int>(species[static_cast<size_t>(en_order[static_cast<size_t>(en_cur)])].records.size());
    if (variants > 1) en_variant = wrap(en_variant + (nav.l1 ? -1 : 1), variants), sfx(1);
  }
  scroll_to(en_cur, en_top, kEnemyRows);
  if (nav.cancel) leave_page(), sfx(6);
}

// Left group: the list in the roster panel cut to 12 rows (frame 141 at (8, 32): the species number, its name, "???" while no variant
// was met, and the Dopplegang mark of a met species), the cursor and the scroll arrows; under it the key-item description panel (frame
// 0x3E at (8, 294)) with the loot, the places or the traits, and the button hints; before the scan has finished, the wait line in the
// list. Right group: the shown variant's battle model in the box (268, 38)..(500, 338) with the variant count at its top left and the
// weakness at its top right, and the member card (frame 7 at (282, 340)) with the variant's name, HP, stats, innate colour and gold.
void MainMenu::Impl::build_enemies() {
  const int count = static_cast<int>(en_order.size());
  // Left column: the list cut to its rows, the loot panel under it, the button hints under that.
  constexpr float kLootX = 8, kLootY = 32 + kEnemyListH + 4;
  float loot_h = 100;
  if (Tex* shape = texs.hd() ? texs.shape(0x3E, lang, false) : nullptr) loot_h = shape->h / 4.0f;
  const float hint_y = kLootY + loot_h + 6;
  group(Left);
  Frame(141, 8, 32, kWhite, nullptr, nullptr, false, kEnemyListH);
  if (!enemy_order) {
    TPfit(24, 42, utf32(S6(kReading)), 200, kName, 0.8f);
    return;
  }
  if (en_top + kEnemyRows < count) scroll_arrow(237, 32 + kEnemyListH - 16, false);
  if (en_top > 0) scroll_arrow(237, 50, true);
  for (int row = 0; row < kEnemyRows && en_top + row < count; ++row) {
    const int index = en_order[static_cast<size_t>(en_top + row)];
    const Species& entry = species[static_cast<size_t>(index)];
    const float cap = 42.2f + 20.0f * static_cast<float>(row);
    char number[8];
    std::snprintf(number, sizeof number, "%03d", index + 1);
    HN(48, cap, number, kName, 0.29f);
    const bool met = species_met(entry);
    const std::u32string name = met ? enemy_name(entry.records.front()) : utf32("???");
    text->prepare(face_gn, name);
    const float width = text->measure(face_gn, name, 0.35f) / xscale;
    HL(56, cap, name, met ? kText : kName, width < 164.0f ? 0.35f : 0.35f * 163.0f / width);
    // The Dopplegang mark: a full dot when the pool holds a variant, a small dim one when it can hold one.
    int dopple = 0;
    if (met)
      for (int monster : entry.records) dopple = std::max(dopple, dopple_state(monster));
    if (dopple) Disc(PX(226), PY(cap + 6.5f), dopple == 2 ? 5.5f : 4.0f, dopple == 2 ? kText : kName);
  }
  if (count > 0) tri(0, 42.0f + 20.0f * static_cast<float>(en_cur - en_top), true);
  btn(0x10, hint_y - 2, 2);
  TPfit(40, hint_y, utf32(S6(kOrders[en_sort])), 84, kText, 0.8f);
  btn(0x80, hint_y - 2, 3);
  TPfit(152, hint_y, utf32(S6(kPanels[en_panel])), 80, kText, 0.8f);
  btn(0x10, hint_y + 22, 4);
  btn(0x28, hint_y + 22, 5);
  TPfit(64, hint_y + 24, utf32(S6(kVariants)), 168, kText, 0.8f);
  if (count == 0) return;

  group(Right);
  const Species& entry = species[static_cast<size_t>(en_order[static_cast<size_t>(en_cur)])];
  const int monster = entry.records[static_cast<size_t>(std::clamp(en_variant, 0, static_cast<int>(entry.records.size()) - 1))];
  const data::StatRec& rec = tables->enemies[static_cast<size_t>(monster)];
  const bool met = enemy_met(monster), beaten = enemy_beaten(monster);
  constexpr float kBoxX = 268, kBoxY = 38, kBoxW = 232, kCardX = 282, kCardY = 340, kBoxH = kCardY - 2 - kBoxY;
  enemy_slot = EnemyModelSlot{};
  enemy_slot.monster = monster;
  enemy_slot.model = rec.model();
  // The weapon as the battle attaches it: weapon item and weapon model both set, the pack part from the weapon's tier.
  if (rec.weapon() != 0 && rec.weapon_model() != 0) {
    const data::GearRec* gear = tables->gear_rec(rec.weapon());
    enemy_slot.weapon_part = gear ? gear->tier() & 0x7F : 0;
  }
  enemy_slot.silhouette = !met;
  enemy_slot.box_x = kBoxX, enemy_slot.box_y = kBoxY, enemy_slot.box_w = kBoxW, enemy_slot.box_h = kBoxH;
  enemy_wanted = enemy_view != nullptr;
  if (entry.records.size() > 1) {
    const std::string shown = std::to_string(en_variant + 1) + "/" + std::to_string(entry.records.size());
    HT(face_kb != render::kNoFace ? face_kb : face_gn, kBoxX + 12, kBoxY + 6, utf32(shown), kText, 0.29f);
  }
  if (const int weak = beaten ? opposite_colour(rec.innate()) : 0) {
    Disc(PX(kBoxX + kBoxW - 6), PY(kBoxY + 12), 13.5f, element_colour(weak));
    HL(kBoxX + kBoxW - 16, kBoxY + 6, utf32(S6(kWeak)), kText, 0.3f, true);
  }
  CardValues values;
  // A variant with no name of its own (one fielded beside its species) carries the species' name.
  const std::u32string own_name = enemy_name(monster);
  values.name = !met ? utf32("???") : own_name.empty() ? enemy_name(entry.records.front()) : own_name;
  values.cur_hp = values.max_hp = rec.max_hp();
  values.wide_hp = true;
  values.hp_unknown = data::hp_is_placeholder(rec.max_hp()) ||
                      (enemy_order && static_cast<size_t>(monster) < enemy_order->counted_end.size() && enemy_order->counted_end[static_cast<size_t>(monster)]);
  values.innate = met ? rec.innate() : 0;
  values.stats = {rec.atk(), rec.hit(), rec.mag(), rec.def(), rec.evd(), rec.mdef()};
  values.known = beaten;
  Frame(7, kCardX, kCardY);
  card_body(kCardX, kCardY, values, true, false, 7);
  if (beaten) {
    // The gold inside the card's corner box (card x 128..164, y 2..24).
    HN(kCardX + 151, kCardY + 6.5f, std::to_string(rec.gold()), kText, 0.24f);
    HS(0x77, kCardX + 152, kCardY + 7.0f, utf32("G"));
  }
  // The loot, places or traits panel under the list.
  group(Left);
  Frame(0x3E, kLootX, kLootY);
  auto row_y = [](int row) { return kLootY + 9.0f + 18.5f * static_cast<float>(row); };
  if (en_panel == 1) {
    // Two columns of four places in story order, the eighth cell "+n" when there are more. Unbeaten: "???".
    const std::vector<std::u32string> places = beaten ? record_places(monster) : std::vector<std::u32string>{};
    if (places.empty()) TPfit(kLootX + 12, row_y(0), utf32(beaten ? "-" : "???"), 228, kText, 0.8f);
    for (size_t i = 0; i < places.size() && i < 8; ++i) {
      const std::u32string line = i == 7 && places.size() > 8 ? utf32("+" + std::to_string(places.size() - 7)) : places[i];
      TPfit(kLootX + 12 + 116 * static_cast<float>(i / 4), row_y(static_cast<int>(i % 4)), line, 110, kText, 0.8f);
    }
    return;
  }
  if (en_panel == 2) {
    // The statuses the record is weak to (once beaten) and the Dopplegang pool (once met).
    std::u32string weak;
    for (const std::u32string& name : beaten ? status_weaknesses(monster) : std::vector<std::u32string>{}) weak += (weak.empty() ? U"" : U", ") + name;
    if (weak.empty()) weak = utf32(beaten ? "-" : "???");
    TPfit(kLootX + 12, row_y(0), utf32(S6(kStatusWeak)), 78, kCap, 0.8f);
    TPfit(kLootX + 96, row_y(0), weak, 148, kText, 0.8f);
    std::u32string dopple_label = element_name(kDopplegangElement);
    if (!dopple_label.empty() && dopple_label.front() == U'β') dopple_label.erase(0, 1);
    TPfit(kLootX + 12, row_y(1), dopple_label, 78, kCap, 0.8f);
    TPfit(kLootX + 96, row_y(1), met ? utf32(S6(kDopple[dopple_state(monster)])) : utf32("???"), 148, kText, 0.8f);
    return;
  }
  const size_t drop_index = static_cast<size_t>(rec.drop_row() & 0x3FFF);
  const data::DropRow* drops = drop_index < tables->drops.size() ? &tables->drops[drop_index] : nullptr;
  for (int slot = 0; slot < 4; ++slot) {
    const u16 word = drops ? drops->slot[static_cast<size_t>(slot)] : 0;
    // A received slot names its loot; an empty slot reads "-" once the variant is beaten; anything else "???".
    const std::u32string loot = enemy_looted(monster, slot) && word ? loot_name(word) : beaten && !word ? utf32("-") : utf32("???");
    TPfit(kLootX + 12, row_y(slot), utf32(S6(kLootLabels[slot])), 78, kCap, 0.8f);
    TPfit(kLootX + 96, row_y(slot), loot, 148, kText, 0.8f);
  }
}

// ===================================================== DialogueLog page
void MainMenu::Impl::enter_dialogue_log() {
  if (log_view) log_view->open();
}

// Up / Down one entry, Left / Right, L1 / R1 and PageUp / PageDown a screen, Home / End the ends; OK hears a logged voice again; Square
// asks to clear the log (the window answers it); Cancel returns to the list it was opened from.
void MainMenu::Impl::update_dialogue_log(const Nav& nav) {
  if (log_view) {
    DialogueLogView::Nav log_nav;
    log_nav.move = (nav.down ? 1 : 0) - (nav.up ? 1 : 0);
    log_nav.page = (nav.right || nav.r1 || nav.page_down ? 1 : 0) - (nav.left || nav.l1 || nav.page_up ? 1 : 0);
    log_nav.first = nav.home;
    log_nav.last = nav.end;
    log_nav.replay = nav.ok;
    log_nav.clear = nav.sq;
    log_nav.cancel = nav.cancel;
    const bool asking = log_view->asking();
    if (const int sound = log_view->input(log_nav)) sfx(sound);
    if (asking) return;
  }
  if (nav.cancel) {
    if (log_view) log_view->close();
    leave_page();
    sfx(6);
  }
}

void MainMenu::Impl::build_dialogue_log() {}

}  // namespace oc::ui
