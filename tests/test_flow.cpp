// Checks for the title / name entry / flow logic: name encoding round trip (game font <-> Unicode, save record), name rules,
// the keyboard pages, the NameEntry cursor / buffer / validation and the FlowMachine state transitions.
#include <cstdio>
#include <utility>
#include <vector>

#include "game/disc_index.h"
#include "game/game_flow.h"
#include "game/game_session.h"
#include "data/tables.h"
#include "game/party_editor.h"
#include "ui/menu/game_state.h"
#include "ui/title/title_screen.h"

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

const import::JpGlyphs& glyphs() {
  static const import::JpGlyphs glyph_set;
  return glyph_set;
}

void test_codec() {
  ui::GameFontCodec codec(glyphs());
  // Serge in the game font: F827 F842 F84E F868.
  const std::vector<u8> serge = codec.encode(U"セルジュ");
  CHECK((serge == std::vector<u8>{0xF8, 0x27, 0xF8, 0x42, 0xF8, 0x4E, 0xF8, 0x68}));
  CHECK(codec.decode(serge) == U"セルジュ");
  // Latin names use the single-byte ASCII part of the font.
  const auto rec = codec.pack_record(U"Serge");
  CHECK(codec.unpack_record(rec) == U"Serge");
  CHECK(rec[5] == 0 && rec[15] == 0);
  // 6 symbols at most.
  CHECK(codec.pack_record(U"Abcdefghij")[6] == 0);
  CHECK(codec.unpack_record(codec.pack_record(U"Abcdefghij")) == U"Abcdef");
  CHECK(!codec.can_encode(U'é') || codec.unpack_record(codec.pack_record(U"é")) == U"é");
  // Unencodable characters are skipped, not fatal.
  CHECK(codec.unpack_record(codec.pack_record(U"Zoé")).size() >= 2);
  // Mixed kana + digits survive a round trip through the 16-byte record.
  CHECK(codec.unpack_record(codec.pack_record(U"キッド")) == U"キッド");
  CHECK(codec.unpack_record(codec.pack_record(U"ひらがな12")) == U"ひらがな12");
}

void test_rules() {
  CHECK(ui::is_reserved_name(U"Chrono") && ui::is_reserved_name(U"CRONO") && ui::is_reserved_name(U"クロノ") &&
        ui::is_reserved_name(U"くろの"));
  CHECK(!ui::is_reserved_name(U"Chronos"));
  CHECK(ui::validate_name(U"   ").empty());
  CHECK(ui::validate_name(U"Serge  ") == U"Serge");
  CHECK(ui::validate_name(U"ひろ　　") == U"ひろ");
  CHECK(ui::validate_name(U"chrono ").empty());
  CHECK(ui::max_name_length(0, import::Lang::En, true) == 12);
  CHECK(ui::max_name_length(0, import::Lang::En, false) == 6);
  CHECK(ui::max_name_length(5, import::Lang::En, false) == 4);
  CHECK(ui::max_name_length(0, import::Lang::Jp, true) == 6);
}

void test_pages() {
  const auto& rows = ui::latin_rows();
  CHECK(rows.size() == 10);
  for (auto row : rows) CHECK(row.size() == 14);
  // The RDE grid: row 5 ends "Ü #", row 7 "ü ß".
  CHECK(rows[5][12] == U'#' && rows[7][12] == U'ß');
  const ui::Ps1Grid kata = ui::ps1_page(ui::Ps1Page::Katakana, glyphs());
  const ui::Ps1Grid hira = ui::ps1_page(ui::Ps1Page::Hiragana, glyphs());
  const ui::Ps1Grid alnum = ui::ps1_page(ui::Ps1Page::Alnum, glyphs());
  CHECK(kata[0][0] == U'ア' && kata[0][4] == U'オ' && kata[1][0] == U'カ');
  CHECK(hira[0][0] == U'あ' && hira[0][4] == U'お' && hira[9][4] == U'ん');
  // The row reads ya _ yu _ yo.
  CHECK(kata[7][1] == 0 && kata[7][2] != 0);
  CHECK(alnum[0][0] == U'Ａ' && alnum[0][5] == U'Ｆ' && alnum[2][5] == U'Ｚ' && alnum[3][0] == U'ａ' && alnum[6][9] == U'９');
}

ui::NameEntry::Setup latin_setup() {
  ui::NameEntry::Setup setup;
  setup.char_id = 0;
  setup.lang = import::Lang::En;
  setup.mode = ui::ArtMode::Hd;
  setup.default_name = U"Serge";
  setup.glyphs = &glyphs();
  return setup;
}

void test_name_entry() {
  ui::NameEntry entry;
  entry.begin(latin_setup());
  CHECK(entry.max_len() == 12 && entry.buffer().size() == 12 && entry.buffer().substr(0, 5) == U"Serge" && entry.buffer()[5] == U' ');
  CHECK(!entry.in_buttons() && entry.rows() == 10 && entry.cols() == 14 && entry.cell(0, 0) == U'A' && entry.cell(1, 12) == 0);
  // Insert: A at slot 0, cursor advances.
  entry.press(ui::NameKey::Confirm);
  CHECK(entry.buffer()[0] == U'A' && entry.cursor_pos() == 1);
  // Walk off the grid edges into the button column and back.
  entry.press(ui::NameKey::Left);
  CHECK(entry.in_buttons());
  // Change Name / Confirm / Default: all reachable.
  CHECK(entry.buttons().size() == 3);
  CHECK(entry.button() == ui::NameEntry::Button::Alnum);
  // "Change Name" goes back into the grid.
  entry.press(ui::NameKey::Confirm);
  CHECK(!entry.in_buttons());
  entry.press(ui::NameKey::Left);
  CHECK(entry.in_buttons() && entry.button() == ui::NameEntry::Button::Alnum);
  // Up wraps to Default; Down wraps back to Change Name.
  entry.press(ui::NameKey::Up);
  CHECK(entry.button() == ui::NameEntry::Button::Default);
  entry.press(ui::NameKey::Down);
  CHECK(entry.button() == ui::NameEntry::Button::Alnum);
  entry.press(ui::NameKey::Down);
  CHECK(entry.button() == ui::NameEntry::Button::Confirm);
  entry.press(ui::NameKey::Down);
  CHECK(entry.button() == ui::NameEntry::Button::Default);
  // Default restores the name and the cursor.
  entry.press(ui::NameKey::Confirm);
  CHECK(entry.buffer().substr(0, 5) == U"Serge" && entry.cursor_pos() == 0);
  // Start accepts from the grid.
  { ui::NameEntry start_entry; start_entry.begin(latin_setup()); start_entry.press(ui::NameKey::Start); CHECK(start_entry.done() && start_entry.result() == U"Serge"); }
  // Fill every slot: the cursor stays on the last slot and overwrites it. Left from the buttons goes to the grid's right edge (col 13).
  entry.press(ui::NameKey::Left);
  CHECK(!entry.in_buttons() && entry.col() == 13);
  entry.press(ui::NameKey::PosRight);
  for (int i = 0; i < 20; ++i) entry.press(ui::NameKey::Confirm);
  CHECK(entry.cursor_pos() == 11 && entry.buffer()[11] == U'N');
  // Backspace blanks the slot and steps back.
  entry.press(ui::NameKey::Backspace);
  CHECK(entry.buffer()[11] == U' ' && entry.cursor_pos() == 10);
  // Reserved name is rejected.
  entry.begin(latin_setup());
  for (int i = 0; i < 12; ++i) entry.press(ui::NameKey::Backspace);
  entry.press(ui::NameKey::PosLeft);
  CHECK(entry.cursor_pos() == 0);
  const char32_t chrono[] = U"Chrono";
  (void)chrono;
  ui::NameEntry reserved_entry;
  auto reserved_setup = latin_setup();
  reserved_setup.default_name = U"Chrono";
  reserved_entry.begin(reserved_setup);
  // Grid col 0 -> the button column (Change Name), then down to Confirm.
  reserved_entry.press(ui::NameKey::Left);
  reserved_entry.press(ui::NameKey::Down);
  reserved_entry.press(ui::NameKey::Confirm);
  CHECK(!reserved_entry.done() && reserved_entry.reject_flash() > 0);
  // Accepting a normal name returns it trimmed.
  ui::NameEntry ok;
  ok.begin(latin_setup());
  ok.press(ui::NameKey::Left);
  ok.press(ui::NameKey::Down);
  ok.press(ui::NameKey::Confirm);
  CHECK(ok.done() && ok.result() == U"Serge");

  // Original art = the same RDE keyboard (no graphics-mode test): 12 characters, Latin grid.
  auto original_setup = latin_setup();
  original_setup.mode = ui::ArtMode::Original;
  ui::NameEntry original_entry;
  original_entry.begin(original_setup);
  CHECK(original_entry.max_len() == 12 && !original_entry.in_buttons() && original_entry.buttons().size() == 3 && original_entry.cols() == 14);
  // JP: the kana pages (katakana / hiragana / alphanumeric), 6 characters, Viper 4.
  auto pj = latin_setup();
  pj.lang = import::Lang::Jp;
  original_entry.begin(pj);
  CHECK(original_entry.max_len() == 6 && original_entry.in_buttons() && original_entry.buttons().size() == 5 && original_entry.cols() == 10);
  // The Hiragana tab: the page follows the focus.
  original_entry.press(ui::NameKey::Down);
  CHECK(original_entry.button() == ui::NameEntry::Button::Hiragana && original_entry.page() == 1);
  original_entry.press(ui::NameKey::Confirm);
  CHECK(!original_entry.in_buttons() && original_entry.cell(0, 0) == U'あ');
  pj.char_id = 5;
  original_entry.begin(pj);
  CHECK(original_entry.max_len() == 4);
  auto jp = latin_setup();
  jp.lang = import::Lang::Jp;
  jp.default_name = U"セルジュ";
  ui::NameEntry jp_entry;
  jp_entry.begin(jp);
  CHECK(jp_entry.max_len() == 6 && jp_entry.button() == ui::NameEntry::Button::Katakana && jp_entry.buffer() == U"セルジュ　　");
}

void test_flow() {
  ui::GameFontCodec codec(glyphs());
  game::FlowMachine flow;
  flow.set_tables(nullptr, &codec);
  CHECK(flow.state() == game::FlowState::Title);
  // Continue needs a save.
  flow.on_title(ui::TitleChoice::Continue);
  CHECK(flow.state() == game::FlowState::Title);
  CHECK(flow.on_title(ui::TitleChoice::Settings).open_settings);
  CHECK(flow.on_title(ui::TitleChoice::Quit).quit);

  // New game: Title -> NameEntry -> Opening (opn_a01i, param 0) -> Field.
  flow.on_title(ui::TitleChoice::NewGame);
  CHECK(flow.state() == game::FlowState::NameEntry);
  CHECK(flow.on_name(U"  ").load_room == false && flow.state() == game::FlowState::NameEntry);
  const game::FlowAction action = flow.on_name(U"Serge");
  // a02koryu plays first, the room load follows it.
  CHECK(action.movie_row == game::kNewGameMovieRow && action.load_room);
  CHECK(action.load_room && action.room == "opn_a01i" && action.param == 0 && action.state_changed && action.stop_music);
  CHECK(std::string(import::room_name_for_id(game::kNewGameRoomId)) == game::kNewGameRoom);
  CHECK(flow.state() == game::FlowState::Opening);
  const ui::GameState& gs = flow.game_state();
  // The title script adds Serge, Kid and a random member.
  CHECK(gs.party().size() == 3 && gs.party()[0] == 0 && gs.party()[1] == 1);
  CHECK(gs.roster().size() == 3 && gs.roster()[0] == 0 && gs.roster()[1] == 1);
  CHECK(game::FlowMachine::new_game_slot(nullptr, nullptr, U"Zed", 5).party() == game::FlowMachine::new_game_slot(nullptr, nullptr, U"Zed", 5).party());
  // Gold 100 (the boot / dream state, game/new_game.h).
  CHECK(gs.gold() == 100 && gs.slot.location() == 22);
  CHECK(gs.slot.dream_records && gs.slot.stars_total() == 20 && gs.slot.head[0x59F] == 0xFF && gs.slot.var(0x3D2) == 0x3F);
  // The Time Shifter is owned; the vibration page was not chosen.
  CHECK(gs.has_key_item(62) && !gs.option(ui::opt::kVibration));
  CHECK(codec.unpack_record(gs.slot.chars[0].name) == U"Serge");
  CHECK(flow.custom_names().at(0) == U"Serge");
  flow.on_field("opn_a01i");
  CHECK(flow.state() == game::FlowState::Opening);
  flow.on_field("alu_a01f");
  CHECK(flow.state() == game::FlowState::Field);

  // Continue: Title -> SlotSelect -> Field at the saved room; cancel returns to the title.
  game::FlowMachine continued;
  continued.set_tables(nullptr, &codec);
  continued.set_save_available(true);
  continued.on_title(ui::TitleChoice::Continue);
  CHECK(continued.state() == game::FlowState::SlotSelect);
  continued.on_slot_cancel();
  CHECK(continued.state() == game::FlowState::Title);
  continued.on_title(ui::TitleChoice::Continue);
  ui::SlotEntry slot_entry;
  slot_entry.slot = 3;
  slot_entry.block = 4;
  slot_entry.data = game::FlowMachine::new_game_slot(nullptr, &codec, U"Zed");
  // Room 252 = wld_a01.
  slot_entry.data.head[save::head::kLocation] = 0xFC;
  slot_entry.data.head[save::head::kLocation + 1] = 0;
  // The leader position is the global variables 0x12 / 0x14 (head 0x188 + id): X = 0x1234, Z = -2.
  slot_entry.data.head[0x188 + 0x12] = 0x34, slot_entry.data.head[0x188 + 0x13] = 0x12, slot_entry.data.head[0x188 + 0x14] = 0xFE, slot_entry.data.head[0x188 + 0x15] = 0xFF;
  slot_entry.data.set_gold(1234);
  const game::FlowAction load_action = continued.on_slot(slot_entry);
  CHECK(load_action.load_room && load_action.room == "wld_a01" && load_action.restore_position && load_action.x == 0x1234 && load_action.z == -2 && load_action.state_changed);
  CHECK(continued.state() == game::FlowState::Field && continued.game_state().gold() == 1234);
  CHECK(continued.custom_names().at(0) == U"Zed");

  // In-game menu Save/Load > Load: from the Field state, same restore as Continue.
  ui::SlotEntry e2 = slot_entry;
  e2.slot = 5;
  e2.data.set_gold(777);
  const game::FlowAction l2 = continued.on_load(e2);
  CHECK(l2.load_room && l2.room == "wld_a01" && l2.state_changed && continued.state() == game::FlowState::Field && continued.game_state().gold() == 777);
  // RDE's load hook: every loaded game owns the Time Shifter (key item 62).
  CHECK((continued.game_state().slot.key_items() >> 61) & 1);
  // A location id without a known room is still handed to the host as a room load (it resolves the id or returns to the title).
  ui::SlotEntry unknown = slot_entry;
  unknown.data.head[save::head::kLocation] = 0xFF;
  unknown.data.head[save::head::kLocation + 1] = 0xFF;
  const game::FlowAction l3 = continued.on_load(unknown);
  CHECK(l3.load_room && l3.room.empty() && l3.room_id == 0xFFFF);
  // Pip's battle growth column follows his form; the grid layout stays record 43's.
  {
    data::GameTables tb;
    data::CharSet cs;
    for (int i = 0; i < data::kNumChars; ++i) cs.rec[static_cast<size_t>(i)].raw[3] = static_cast<u8>(i >= 43 && i <= 48 ? 3 + (i >= 46 ? i - 45 : 0) : 0);
    tb.char_sets.emplace("char", cs);
    tb.growth.assign(100, std::array<u8, 8>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    ui::MenuRules mr;
    mr.tables = &tb;
    ui::GameState pg(save::SaveSlot{}, &mr);
    pg.slot.chars[43].model_id = 0x2F;
    pg.slot.chars[43].max_hp = 100;
    CHECK(game::party_member_from_state(tb, pg, 43).growth_col == 5);
    pg.slot.chars[43].model_id = 0x2B;
    CHECK(game::party_member_from_state(tb, pg, 43).growth_col == 3);
  }
  // The title's vibration page: New Game -> page 1 -> Off -> the new game starts with options bit 0x400 clear; On sets it.
  {
    ui::TitleScreen title;
    title.begin(import::Lang::En, ui::ArtMode::Hd, false);
    title.skip_to_menu();
    ui::Nav ok;
    ok.confirm = ok.any = true;
    ui::Nav down;
    down.down = true;
    ui::Nav back;
    back.cancel = back.any = true;
    title.update(ok, 0.016f);
    CHECK(title.page() == 1 && title.selected() == 0 && title.take_choice() == ui::TitleChoice::None);
    // Cross goes back to the list.
    title.update(back, 0.016f);
    CHECK(title.page() == 0 && title.phase() == ui::TitleScreen::Phase::Menu);
    title.update(ok, 0.016f);
    title.update(down, 0.016f);
    CHECK(title.page() == 1 && title.selected() == 1);
    title.update(ok, 0.016f);
    ui::TitleChoice got = ui::TitleChoice::None;
    // The choice arrives after the fade-out.
    for (int i = 0; i < 60 && got == ui::TitleChoice::None; ++i) title.update(ui::Nav{}, 0.016f), got = title.take_choice();
    CHECK(got == ui::TitleChoice::NewGame && !title.vibration());
    game::FlowMachine vibration_flow;
    vibration_flow.set_tables(nullptr, &codec);
    vibration_flow.set_vibration(true);
    vibration_flow.new_game(U"Serge");
    CHECK(vibration_flow.game_state().option(ui::opt::kVibration));
  }
  // The prompt press plays the three chime sounds on their own layers and fades into the menu; Cancel there brings the prompt back.
  {
    ui::TitleScreen title;
    std::vector<std::pair<int, int>> layered;
    std::vector<int> plain;
    title.set_layered_sfx([&layered](int id, int layer) { layered.emplace_back(id, layer); });
    title.set_sfx([&plain](int id) { plain.push_back(id); });
    title.begin(import::Lang::En, ui::ArtMode::Hd, false, -1, true);
    ui::Nav press;
    press.any = true;
    title.update(press, 0.016f);
    CHECK((layered == std::vector<std::pair<int, int>>{{59, 0}, {61, 1}, {62, 2}}));
    CHECK(plain.empty());
    for (int i = 0; i < 120; ++i) title.update(ui::Nav{}, 0.016f);
    CHECK(title.phase() == ui::TitleScreen::Phase::Menu && title.fade_alpha() == 0.0f);
    ui::Nav back;
    back.cancel = back.any = true;
    title.update(back, 0.016f);
    CHECK((plain == std::vector<int>{6}));
    for (int i = 0; i < 60 && title.phase() == ui::TitleScreen::Phase::Menu; ++i) title.update(ui::Nav{}, 0.016f);
    CHECK(title.phase() == ui::TitleScreen::Phase::Prompt);
  }

  // Events in the wrong state do nothing.
  CHECK(!continued.on_name(U"x").load_room && !continued.on_title(ui::TitleChoice::Quit).quit);

  std::optional<game::FlowState> st;
  bool enabled = true;
  CHECK(game::parse_flow_start("title", st, enabled) && enabled && st == game::FlowState::Title);
  CHECK(game::parse_flow_start("Name", st, enabled) && st == game::FlowState::NameEntry);
  CHECK(game::parse_flow_start("slots", st, enabled) && st == game::FlowState::SlotSelect);
  CHECK(game::parse_flow_start("field", st, enabled) && !enabled);
  CHECK(!game::parse_flow_start("bogus", st, enabled) && !enabled);
}


// Party screen rules.
void test_party_editor() {
  using Key = game::PartyEditor::Key;
  using Mode = game::PartyEditor::Mode;
  game::PartyEditor editor;
  // Serge is locked (the new-game value).
  editor.begin({5, 0, 1, 22, 9}, {0, 1, -1}, {1u, 0u});
  CHECK(editor.roster() == (std::vector<int>{0, 1, 5, 9, 22}));
  // A slot is empty and 2 != 5 grid characters: no leaving.
  CHECK(editor.press(Key::Cancel) == 0x18 && !editor.done());
  // Slot 0 holds the locked leader.
  CHECK(editor.press(Key::Confirm) == 0x18 && editor.mode() == Mode::Slots);
  CHECK(editor.press(Key::Right) == 1 && editor.slot() == 1);
  CHECK(editor.press(Key::Right) == 1 && editor.slot() == 2);
  // The slot cursor wraps.
  CHECK(editor.press(Key::Right) == 1 && editor.slot() == 0);
  CHECK(editor.press(Key::Left) == 1 && editor.slot() == 2);
  // An empty slot shows no stats.
  CHECK(editor.shown() == -1);
  // The grid cursor starts on cell 0.
  CHECK(editor.press(Key::Confirm) == 8 && editor.mode() == Mode::Pick && editor.cursor() == 0);
  CHECK(editor.shown() == 0);
  // Columns wrap.
  CHECK(editor.press(Key::Left) == 1 && editor.column() == 3);
  CHECK(editor.press(Key::Right) == 1 && editor.column() == 0);
  CHECK(editor.press(Key::Right) == 1 && editor.cursor() == 1);
  // Kid (slot 1) swaps with the empty slot 2; the slot cursor follows to slot 1 and the grid stays open.
  CHECK(editor.press(Key::Confirm) == 8 && editor.slots()[1] == -1 && editor.slots()[2] == 1);
  CHECK(editor.slot() == 1 && editor.mode() == Mode::Pick);
  // Viper (not in the party) goes into slot 1.
  CHECK(editor.press(Key::Right) == 1 && editor.press(Key::Confirm) == 8 && editor.slots()[1] == 5);
  // Confirming on the slot's own character is refused.
  CHECK(editor.press(Key::Confirm) == 0x18);
  // An empty cell is refused.
  CHECK(editor.press(Key::Down) == 1 && editor.cursor() == 6 && editor.press(Key::Confirm) == 0x18);
  CHECK(editor.press(Key::Cancel) == 6 && editor.mode() == Mode::Slots);
  CHECK(editor.press(Key::Cancel) == 6 && editor.done() && editor.result() == (std::vector<int>{0, 5, 1}));
  // scrolling: row 4 + down scrolls (18 frames, input ignored), rows 0..4 visible, scroll 0..6
  game::PartyEditor scroller;
  std::vector<int> all;
  for (int i = 0; i < 44; ++i) all.push_back(i);
  scroller.begin(all, {0, 1, 2});
  scroller.press(Key::Confirm);
  for (int i = 0; i < 4; ++i) scroller.press(Key::Down);
  CHECK(scroller.row() == 4 && scroller.scroll() == 0);
  CHECK(scroller.press(Key::Down) == 1 && scroller.scroll() == 1 && scroller.scrolling() && scroller.row() == 4);
  // Input is ignored while scrolling.
  CHECK(scroller.press(Key::Down) == 0);
  for (int i = 0; i < 17; ++i) scroller.tick();
  CHECK(scroller.yscroll() == -0x22 && scroller.scrolling());
  scroller.tick();
  CHECK(!scroller.scrolling() && scroller.cursor() == 20);
  // a two-member roster may leave with two slots filled
  game::PartyEditor two;
  two.begin({0, 1}, {0, 1, -1});
  CHECK(two.press(Key::Cancel) == 6 && two.done() && two.result() == (std::vector<int>{0, 1}));
  // begin packs the party (empty slots dropped)
  game::PartyEditor gap;
  gap.begin({0, 1, 2}, {-1, 2, 0});
  CHECK(gap.slots()[0] == 2 && gap.slots()[1] == 0);
}

// The disc indexes: movie number -> file and room id -> map stem per disc.
void test_disc_index() {
  auto rec = [](std::vector<std::string> names, size_t total) {
    Bytes bytes(total * 0x40, 0);
    for (size_t i = 0; i < names.size(); ++i) std::copy(names[i].begin(), names[i].end(), bytes.begin() + static_cast<long>(i * 0x40));
    return bytes;
  };
  std::vector<std::string> d1(6100), d2(6100);
  d1[368 + 3 * 459] = "C:/work/cdrom/map/mapbin/bak_a02f.bin";
  d2[368 + 3 * 459] = "C:/work/cdrom/map/mapbin/fin_06.bin";
  d2[368 + 3 * 460] = "c:/work/cdrom/dummy/dummy.dat";
  d1[6035 + 1] = "c:/movie/title.stv";
  d1[6035 + 19] = "c:/movie/newop.stv";
  d2[6035] = "c:/movie/final.stv";
  d2[6035 + 10] = "c:/movie/ending_s.stv";
  game::DiscIndex idx;
  const Bytes b1 = rec(d1, 6100), b2 = rec(d2, 6100);
  CHECK(idx.from_bytes(b1, b2) && idx.loaded());
  CHECK(idx.room_stem(1, 459) == "bak_a02f" && idx.room_stem(2, 459) == "fin_06");
  CHECK(idx.room_stem(2, 460).empty() && idx.room_stem(1, 99999).empty());
  CHECK(idx.movie_name_of(1, 1) == "title" && idx.movie_name_of(1, 19) == "a01op" && idx.movie_name_of(1, 2).empty());
  // D4 / 7E operand = row of the field script's movie table: row 0 plays number 19 (the opening), row 1 number 1.
  CHECK(idx.movie_name(1, 0) == "a01op" && idx.movie_name(1, 1) == "title" && idx.movie_name(1, 24).empty());
  CHECK(idx.movie_name(2, 0) == "final" && idx.movie_name(2, 12) == "ending_s" && idx.movie_name(2, 13).empty());
  CHECK(idx.movie_name_of(2, 0) == "final" && idx.movie_name_of(2, 10) == "ending_s" && idx.movie_name_of(2, 1).empty());
  {
    u32 range_start = 9, range_end = 9;
    game::DiscIndex::movie_range(1, 0, range_start, range_end);
    // The opening plays whole.
    CHECK(range_start == 0 && range_end == 0);
    game::DiscIndex::movie_range(1, 20, range_start, range_end);
    CHECK(range_start == 306 && range_end == 489);
    game::DiscIndex::movie_range(1, 22, range_start, range_end);
    CHECK(range_start == 281 && range_end == 417);
    game::DiscIndex::movie_range(2, 12, range_start, range_end);
    CHECK(range_start == 1 && range_end == 0);
  }
  // A disc-1 movie is no movie on disc 2.
  CHECK(idx.movie_name_of(2, 19).empty());
}

void test_location_key() {
  CHECK(ui::GameState::location_key(0) == 1 && ui::GameState::location_key(7) == 1 && ui::GameState::location_key(8) == 2);
  CHECK(ui::GameState::location_key(44) == 8);
  CHECK(ui::GameState::location_key(52) == ui::GameState::location_key(63));
  CHECK(ui::GameState::location_key(300) == 29);
  // Head 0x40 holds the whole u16 of variable 0, so the last threshold (999) is reachable.
  save::SaveSlot slot;
  slot.set_progression(1000);
  CHECK(slot.head[0x40] == 0xE8 && slot.head[0x41] == 0x03 && slot.progression() == 1000);
  CHECK(ui::GameState::location_key(slot.progression()) == 30);
}

}  // namespace

// Any button (Start included) ends an attract demo room; the settings / art-mode hotkeys do not.
void test_any_button() {
  platform::Input in;
  CHECK(!game::GameFlow::any_button(in));
  in.pad_pressed[SDL_GAMEPAD_BUTTON_START] = true;
  CHECK(game::GameFlow::any_button(in));
  in = platform::Input{};
  in.key_pressed[SDL_SCANCODE_P] = true;
  CHECK(game::GameFlow::any_button(in));
  in = platform::Input{};
  in.key_pressed[SDL_SCANCODE_ESCAPE] = true;
  CHECK(!game::GameFlow::any_button(in));
}

// RDE's boot logo timing: 180 frames per image, 30-frame fades, a skip jumps to the next image.
void test_boot_logo() {
  ui::BootLogo logo;
  CHECK(!logo.active());
  logo.begin(1);
  CHECK(logo.active() && logo.image() == 0 && logo.alpha() == 0.0f);
  for (int i = 0; i < 15; ++i) logo.step();
  CHECK(logo.alpha() == 0.5f);
  for (int i = 15; i < 30; ++i) logo.step();
  CHECK(logo.alpha() == 1.0f);
  for (int i = 30; i < 150; ++i) logo.step();
  CHECK(logo.counter() == 150 && logo.alpha() == 1.0f);
  logo.step();
  // Alpha is 1 - 1/30.
  CHECK(logo.alpha() > 0.96f && logo.alpha() < 0.97f);
  for (int i = 151; i < 179; ++i) logo.step();
  // Frame 179 is still drawn (alpha 1/30).
  CHECK(logo.active());
  logo.step();
  CHECK(!logo.active() && logo.counter() == 180);
  // A press ends the image at once (no fade-out).
  logo.begin(1);
  for (int i = 0; i < 40; ++i) logo.step();
  logo.request_skip();
  CHECK(logo.active());
  logo.step();
  CHECK(!logo.active());
  // Japanese: three images; a skip moves on to the next one, which fades in from 0.
  logo.begin(3);
  for (int i = 0; i < 10; ++i) logo.step();
  logo.request_skip();
  logo.step();
  CHECK(logo.active() && logo.image() == 1 && logo.alpha() == 0.0f);
  for (int i = 0; i < 360; ++i) logo.step();
  CHECK(!logo.active());
  logo.begin(0);
  CHECK(!logo.active());
}

int main() {
  test_codec();
  test_any_button();
  test_boot_logo();
  test_rules();
  test_pages();
  test_name_entry();
  test_flow();
  test_party_editor();
  test_disc_index();
  test_location_key();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("flow tests passed\n");
  return 0;
}
