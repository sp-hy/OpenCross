#include "ui/menu/main_menu_impl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "platform/keymap.h"
#include "ui/menu/menu_common.h"
#include "ui/menu/ps1_art.h"
#include "ui/ps1_font.h"

namespace oc::ui {

namespace {

const Str6 kMainLabels[6] = {
    {{"Status", "Statut", "Stato", "Status", "Estado", "ステータス"}},
    {{"Elements", "Éléments", "Elementi", "Elemente", "Elementos", "エレメント"}},
    {{"Equip.", "Équip.", "Equipaggiamento", "Ausrüstung", "Equipo", "そうび"}},
    {{"Items", "Objets", "Oggetti", "Inventar", "Objetos", "アイテム"}},
    {{"Customize", "Personnaliser", "Modifica", "Optionen", "Personalizar", "カスタマイズ"}},
    {{"Save/Load", "Sauv./Charger", "Salva/Carica", "Speichern/Laden", "Guardar/Cargar", "ファイル"}}};
const Str6 kPickLabels[2] = {{{"Use", "Utiliser", "Usa", "Einsetzen", "Usar", "つかう"}},
                             {{"Allocate", "Assigner", "Assegna", "Zuteilen", "Asignar", "はいち"}}};

}  // namespace

void MainMenu::Impl::update_main(const Nav& nav) {
  const int party_size = static_cast<int>(party().size());
  if (sub == 0) {
    if (nav.up) main_cur = wrap(main_cur - 1, 6), sfx(1);
    if (nav.down) main_cur = wrap(main_cur + 1, 6), sfx(1);
    if (nav.cancel) {
      mem_cur = main_cur;
      closed = true;
      sfx(6);
    } else if (nav.ok) {
      sfx(8);
      switch (main_cur) {
        // Status and Equip pick the member first.
        case 0: sub = 1, pick_for = 0, pick_cur = party_pick_start(); break;
        case 1: enter(MenuScreen::ElementChoice); break;
        case 2: sub = 1, pick_for = 2, pick_cur = party_pick_start(); break;
        case 3: enter(MenuScreen::KeyItems); break;
        case 4: enter(MenuScreen::Settings); break;
        // Load is always available; Save is gated inside.
        case 5: enter(MenuScreen::Save); break;
      }
    }
  } else if (roster != Roster::Off) {
    update_roster(nav);
  } else {
    // Left past the first member opens the roster.
    if (nav.left && pick_cur == 0) roster = Roster::In, roster_anim = kRosterSlide, roster_ask = false, sfx(1);
    else if (nav.left || nav.up) pick_cur = wrap(pick_cur - 1, party_size), sfx(1);
    if (nav.right || nav.down) pick_cur = wrap(pick_cur + 1, party_size), sfx(1);
    if (roster != Roster::Off) return;
    if (nav.cancel) sub = 0, sfx(6);
    else if (nav.ok && party_size > 0) {
      member = pick_cur;
      sfx(8);
      enter(pick_for == 0 ? MenuScreen::Status : pick_for == 1 ? MenuScreen::ElementGrid : MenuScreen::Equip);
      sub = 0;
    }
  }
}

// Roster: up / down walk the 44 characters (17 rows visible), right or cancel slide back to the member pick, confirm opens the picked
// screen on a recruited character. On the Grid and Equip picks Triangle asks to remove all of that character's elements / accessories.
// Input waits while the list slides.
void MainMenu::Impl::update_roster(const Nav& nav) {
  if (roster == Roster::In) {
    if (roster_anim == 0) roster = Roster::On;
    return;
  }
  if (roster == Roster::Out) {
    if (roster_anim == 0) roster = Roster::Off, pick_cur = 0;
    return;
  }
  const int character = roster_top + roster_row;
  if (roster_ask) {
    if (nav.left || nav.right) roster_no ^= 1, sfx(1);
    if (nav.ok) {
      if (roster_no != 0) sfx(1);
      else if (pick_for == 2) gs->unequip_accessories(character), sfx(0x48);
      else gs->clear_all(character), sfx(0x17);
      roster_ask = false;
    }
    return;
  }
  if (nav.tri && pick_for != 0 && gs->available(character)) {
    roster_ask = true;
    sfx(1);
    return;
  }
  if (nav.up) {
    if (roster_row > 0) --roster_row;
    else if (roster_top > 0) --roster_top;
    sfx(1);
  }
  if (nav.down) {
    if (roster_row < kRosterRows - 1) ++roster_row;
    else if (roster_top < save::kNumChars - kRosterRows) ++roster_top;
    sfx(1);
  }
  if (nav.right || nav.cancel) {
    roster = Roster::Out, roster_anim = kRosterSlide;
    sfx(nav.cancel ? 6 : 1);
  } else if (nav.ok) {
    sfx(8);
    const int picked = roster_top + roster_row;
    if (!gs->available(picked)) {
      sfx(0x18);
      return;
    }
    roster = Roster::Off;
    sub = 0;
    stand_in = picked;
    member = 0;
    enter(pick_for == 0 ? MenuScreen::Status : pick_for == 1 ? MenuScreen::ElementGrid : MenuScreen::Equip);
  }
}

// Confirm switches between the profile and the equipment; changing member starts again on the profile with the model's camera reset.
void MainMenu::Impl::update_status(const Nav& nav) {
  if (nav.l1) cycle_member(-1), profile = true, restart_model();
  if (nav.r1) cycle_member(1), profile = true, restart_model();
  if (nav.ok) profile = !profile;
  if (nav.cancel) back_to_main(0), sfx(6);
}

// The menu model of a character: its record byte 0x2D (the current model) up to `last`, else the character's own.
int MainMenu::Impl::model_number(int character, int last) const {
  const int model = gs->slot.chars[static_cast<size_t>(character)].model_id;
  return model <= last ? model : character;
}

void MainMenu::Impl::restart_model() {
  model_cam = MenuCamera{};
  model_cam_manual = false;
  model_pose = 0;
  ++model_session;
}

// Once per menu-model tick (every second 60 Hz frame): the model turns by itself (+8), or with the pad camera the held directions move it - with L2 Left / Right / Up /
// Down pan by 8 (at most 0xAA either way), with R2 Up / Down tilt by 16 (-0x200..0), otherwise Left / Right turn by 16 and Up / Down move the
// camera in / out by 32. The distance stays between the model's own nearest distance and 0x960. Square (pad camera only) steps the pose
// 0 -> 1 -> 2 -> 0, Triangle switches between the turning model and the pad camera.
void MainMenu::Impl::step_model_camera(const Nav& nav, int ticks) {
  constexpr int kAutoTurn = 8, kTurn = 16, kZoom = 32, kTilt = 16, kPan = 8, kPanLimit = 0xAA, kTiltLimit = -0x200, kFarthest = 0x960;
  const int nearest = model_view->min_distance(model_number(cur_char(), kLastMenuModel));
  auto axis = [](bool minus, bool plus, int step) { return (plus ? step : 0) - (minus ? step : 0); };
  model_cam.distance = std::min(std::max(model_cam.distance, nearest), kFarthest);
  const int steps = (model_frames + ticks) / 2 - model_frames / 2;
  model_frames += ticks;
  for (int tick = 0; tick < steps; ++tick) {
    if (!model_cam_manual) {
      model_cam.yaw += kAutoTurn;
    } else if (nav.l2) {
      model_cam.pan_x = std::clamp(model_cam.pan_x + axis(nav.held_left, nav.held_right, kPan), -kPanLimit, kPanLimit);
      model_cam.pan_y = std::clamp(model_cam.pan_y + axis(nav.held_up, nav.held_down, kPan), -kPanLimit, kPanLimit);
    } else if (nav.r2) {
      model_cam.pitch = std::clamp(model_cam.pitch + axis(nav.held_up, nav.held_down, kTilt), kTiltLimit, 0);
    } else {
      model_cam.yaw += axis(nav.held_left, nav.held_right, kTurn);
      model_cam.distance += axis(nav.held_up, nav.held_down, kZoom);
    }
    model_cam.distance = std::min(std::max(model_cam.distance, nearest), kFarthest);
  }
  if (nav.sq && model_cam_manual) model_pose = (model_pose + 1) % 3;
  if (nav.tri) model_cam_manual = !model_cam_manual;
}

void MainMenu::Impl::update_choice(const Nav& nav) {
  if (nav.up || nav.left) choice_cur = wrap(choice_cur - 1, 2), sfx(1);
  if (nav.down || nav.right) choice_cur = wrap(choice_cur + 1, 2), sfx(1);
  if (nav.cancel) back_to_main(1), sfx(6);
  else if (nav.ok) {
    sfx(8);
    if (choice_cur == 0) enter(MenuScreen::ElementUse);
    else {
      screen = MenuScreen::Main;
      sub = 1;
      pick_for = 1;
      pick_cur = party_pick_start();
    }
  }
}

// Dial + needle of the main list / Use-Allocate picker: the menutim_02 swirl scaled over the dial spot, the needle at `hand_angle`
// (eased per frame by hand_tick). Both art modes.
void MainMenu::Impl::clock_dial() {
  if (Tex* deco = texs.load("hd/menu/menutim/menutim_02.png")) Img(deco, PX(382), PY(293), PX(128), PY(149), 0, 0, 1, 1, rgb(172, 172, 182));
  Hand(446, 367, hand_angle);
}

// The main list (6 entries) and the Use / Allocate picker (2 entries) are style-7 entries right-packed against the dial: they sit
// at x = base - 9 i - maxW + dx(L), where maxW is the widest label of the list in PS1 px (trunc(measure / 2.5)) and dx(L) a
// per-language offset. English is placed by ink position; other languages move by their differences in maxW
// and dx(L), and JP sits 1 px lower.
float MainMenu::Impl::list_maxw(int list_id, int lang_idx) {
  const MtStyle style = mt_style(7, lang_idx);
  const float scale = font::slot(style.slot).scale * style.mul;
  float widest = 0;
  for (int entry = 0; entry < (list_id == 0 ? 6 : 2); ++entry) {
    const Str6& strings = list_id == 0 ? kMainLabels[entry] : kPickLabels[entry];
    const std::u32string label = utf32(strings.by_lang[lang_idx]);
    fonts.prepare(style.slot, label);
    widest = std::max(widest, std::trunc(fonts.measure(style.slot, label, scale) / 2.5f));
  }
  return widest;
}

float MainMenu::Impl::list_dx(int list_id, int lang_idx, int entry) {
  if (list_id == 0) {
    static const float kMainDx[6] = {-0x19, -5, 0xc, 0xc, 3, 0};
    return kMainDx[lang_idx];
  }
  static const float kPickDx[6] = {-0x22, -0x22, -0x24, -0x1b, -0x27, -0x25};
  return kPickDx[lang_idx] + ((lang_idx == 3 || lang_idx == 5) ? 0.0f : (entry ? 2.0f : 0.0f));
}

// `list_id` 0 = main list, 1 = Use / Allocate picker. Entry 5 (Save/Load) is never dimmed: loading is always possible.
void MainMenu::Impl::list_and_bar(int list_id, int sel, bool bar) {
  // hd: labels are drawn by their ink box (both art modes; false only without the bn / kb faces).
  const bool hd = hd_ink();
  const float rel = 0.9f;
  const u32 line_colour = rgb(200, 190, 190);
  clock_dial();
  const int count = list_id == 0 ? 6 : 2;
  float maxw = 0;
  for (int entry = 0; entry < count; ++entry) maxw = std::max(maxw, twp(utf32(S6(list_id == 0 ? kMainLabels[entry] : kPickLabels[entry])), rel));
  const float dmax = hd ? list_maxw(list_id, 0) - list_maxw(list_id, lang_index()) : 0.0f;
  const MtStyle list_style = mt_style(7, lang_index());
  const render::FaceId list_face = slot_face(list_style.slot);
  const float list_scale = font::slot(list_style.slot).scale * list_style.mul;
  // Use / Allocate picker: ink left / cap top per entry, leader at cap + 13.3 from ink - 2.6 to the bend, then on to the ring.
  static const float kPickX[2] = {296.6f, 279.6f}, kPickY[2] = {313.6f, 346.8f}, kPickBend[2] = {384.4f, 359.5f}, kPickEnd[2] = {397.0f, 386.2f};
  for (int entry = 0; entry < count; ++entry) {
    const std::u32string label = utf32(S6(list_id == 0 ? kMainLabels[entry] : kPickLabels[entry]));
    const u32 colour = entry == sel ? kSel : kText;
    if (hd && list_id == 1) {
      const float x = kPickX[entry] + dmax + list_dx(1, lang_index(), entry) - list_dx(1, 0, entry), y = kPickY[entry] + (jp() ? 1.0f : 0.0f);
      HT(list_face, x, y, label, colour, list_scale);
      leader_raw(x - 2.6f, kPickY[entry] + 13.3f, kPickBend[entry], kPickEnd[entry], line_colour);
      continue;
    }
    if (hd && list_id == 0) {
      // Main list: left-aligned ink at 295.3 - 9.1 i, cap top 303.4 + 16 i
      const float x = 295.3f - 9.1f * static_cast<float>(entry) + dmax + list_dx(0, lang_index(), entry) - list_dx(0, 0, entry),
                  y = 303.4f + 15.95f * static_cast<float>(entry) + (jp() ? 1.0f : 0.0f);
      HT(list_face, x, y, label, colour, list_scale);
      leader(entry, x, y - (jp() ? 1.0f : 0.0f), line_colour);
      continue;
    }
    // No bn / kb faces (fallback only): the same formula on the label face.
    const float x = list_id == 0 ? 382.0f - 9.0f * static_cast<float>(entry) - maxw + list_dx(0, lang_index(), entry)
                                 : static_cast<float>(0x15 - entry) * 18.0f - maxw + list_dx(1, lang_index(), entry);
    const float y = 303.0f + (list_id == 0 ? 16.0f : 32.0f) * static_cast<float>(entry);
    TP(x, y, label, colour, rel);
    if (list_id == 0) leader(entry, x, y + 0.4f, line_colour);
    else leader_raw(x - 2.8f, y + 12.4f, kPickBend[entry], kPickEnd[entry], line_colour);
  }
  bar_and_title(list_id == 0 ? sel : 1, bar);
}

// Grid minimap of a member card: 8 level columns x 11 rows of 5 x 3 tiles at (x, y), step (4, 2), one tile per unlocked grid cell.
// menutim_03.png is the 4x redraw of the PS1 "gg" sheet with the same layout.
void MainMenu::Impl::mini_grid(int character, float x, float y) {
  if (Tex* tile = texs.ps1_or_hd(ps1_screen(), "ps1:gg", [&] { return ps1::menutim(*vfs, "gg"); }, "hd/menu/menutim/menutim_03.png")) {
    for (int row = 0; row < GameState::kRows; ++row)
      for (int col = 0; col < GameState::kCols; ++col)
        if (gs->cell_exists(character, row, col) && gs->cell_available(character, row, col))
          Img(tile, PX(x + 4.0f * static_cast<float>(row)), PY(y + 2.0f * static_cast<float>(col)), PX(5), PY(3), 0, 0, 20.0f / 48.0f, 1);
  }
}

// Member card body (card-relative PS1 px, ink boxes calibrated for English): name, HP / HP, six stats, INNATE + dot,
// optional stamina row, grid minimap. The captions are cmnmes rows placed with per-key, per-language offsets relative to
// English; `group` = the frame's sheet group (7 main-list card, 0x34 status / Elements-use panel, 0x2F equip header), which only the
// Italian M.RES offset depends on. The JP rows sit 1 px lower and their captions take the JP-specific offsets.
void MainMenu::Impl::card_hd(float card_x, float card_y, int character, bool stats, bool stamina, int group_id) {
  const save::SaveChar& ch = gs->slot.chars[static_cast<size_t>(character)];
  const float number_scale = 0.275f;
  const int lang_idx = lang_index();
  HL(card_x + 7.0f, card_y + 11.9f, char_name(character), kName, 0.35f);
  HS(0x4, card_x + 7.7f, card_y + 30.7f, capt("\xEF\xBC\xA8\xEF\xBC\xB0", "HP"));
  const u32 hp_colour = ch.cur_hp < ch.max_hp / 8 ? kSel : kText;
  HN(card_x + 51.9f, card_y + 30.7f, std::to_string(std::min<int>(ch.cur_hp, 999)), hp_colour, 0.29f);
  HT(face_kb, card_x + 73.1f, card_y + 30.7f, utf32("/"), kText, 0.29f);
  HN(card_x + 100.1f, card_y + 30.7f, std::to_string(std::min<int>(ch.max_hp, 999)), kText, 0.29f);
  static const float kInnateDx[6] = {0, 0, 0, -10, -8, -2}, kInnateDy[6] = {2, 2, 2, 2, 2, 3};
  HS(0x64, card_x + 120.0f, card_y + 34.9f, capt("\xE5\x85\x88\xE5\xA4\xA9\xE5\xB1\x9E\xE6\x80\xA7", "INNATE"), kText, dl(kInnateDx, lang_idx),
     dl(kInnateDy, lang_idx));
  const int innate = gs->innate(character);
  Disc(PX(card_x + 166.2f), PY(card_y + 37.5f), 13.5f, innate ? element_colour(innate) : rgb(120, 120, 130));
  if (stats) {
    static const char* const kJpKey[6] = {"\xE8\x85\x95\xE5\x8A\x9B", "\xE6\xAD\xA3\xE7\xA2\xBA\xE3\x81\x95", "\xE9\xAD\x94\xE5\x8A\x9B",
                                          "\xE8\x80\x90\xE4\xB9\x85\xE5\x8A\x9B", "\xE8\xBA\xAB\xE8\xBB\xBD\xE3\x81\x95", "\xE6\x8A\x97\xE9\xAD\x94"};
    static const char* const kEnglish[6] = {"STR.", "ACC.", "MAG.", "RES.", "AGI.", "M.RES"};
    static const float kLabelX[3] = {7.0f, 61.8f, 115.3f}, kNumberX[3] = {55.9f, 109.6f, 165.8f};
    static const float kColumnDx[3][6] = {{3, 4, 3, 3, 3, 3}, {-4, -2, -3, -3, -3, 1}, {6, 9, 5, 5, 5, -1}};
    static const float kLabelDx[6] = {-2, -1, -6, 3, -1, -6};
    const int values[6] = {std::min<int>(ch.atk, 99), std::min<int>(ch.hit, 99), std::min<int>(ch.mag, 99), std::min<int>(ch.def, 99),
                           std::min<int>(ch.evd, 99), std::min<int>(ch.mdef, 99)};
    for (int stat = 0; stat < 6; ++stat) {
      const float y = card_y + (stat < 3 ? 47.1f : 63.5f);
      float dx = lang_idx == 5 ? -kLabelDx[stat] : 0.0f, dy = lang_idx == 5 ? 2.0f : 0.0f;
      if (stat == 5) {
        static const float kMagicResDy[6] = {1, 1, 2, 1, 1, 3};
        dx = (lang_idx == 5 ? 6.0f : (lang_idx == 2 && group_id != 0x2F) ? -1.0f : 0.0f);
        dy = dl(kMagicResDy, lang_idx);
      }
      HS(stat == 5 ? 0x65 : 0x2, card_x + kLabelX[stat % 3], y, capt(kJpKey[stat], kEnglish[stat]), kText, dx, dy);
      HN(card_x + kNumberX[stat % 3] + dl(kColumnDx[stat % 3], lang_idx), y + (lang_idx == 5 ? 1.0f : 0.0f), std::to_string(values[stat]), kText, number_scale);
    }
  }
  if (stamina) {
    static const float kStaminaDx[6] = {-26, 20, -26, -38, -45, -12}, kStaminaDy[6] = {2, 2, 2, 2, 4, 2};
    HS(0x66, card_x + 49.2f, card_y + 80.6f, capt("\xE3\x82\xB9\xE3\x82\xBF\xE3\x83\x9F\xE3\x83\x8A\xE5\x9B\x9E\xE5\xBE\xA9\xE5\x8A\x9B", "STAMINA RECOVERY"), kText,
       dl(kStaminaDx, lang_idx), dl(kStaminaDy, lang_idx));
    static const float kThirdColumnDx[6] = {6, 9, 5, 5, 5, -1};
    HN(card_x + 167.0f + dl(kThirdColumnDx, lang_idx), card_y + 80.6f + (lang_idx == 5 ? 1.0f : 0.0f), std::to_string(std::min(gs->stamina_recovery(character), 99)),
       kText, number_scale);
  }
  mini_grid(character, card_x + 0x83, card_y + 6);
}

// One party card of the main list at PS1 (x, y).
void MainMenu::Impl::member_card(int character, float x, float y) {
  Frame(7, x, y);
  card_hd(x, y, character, true, false);
}

// Main list: the portrait strip and the member cards (left group; they sit against each other, so they move out together), the list
// with its dial and the title (right group), the bottom bar (left) and the message (centre). Portraits are dimmed to 0x40 except the
// member being picked. While the roster is out it covers the portraits and the bar is not drawn.
void MainMenu::Impl::build_main() {
  const bool toggle = screen == MenuScreen::ElementChoice;
  const bool listing = roster != Roster::Off;
  const bool picking = screen == MenuScreen::Main && sub == 1 && !listing;
  const std::vector<int> members = party();
  // Opening Allocate elements / Equip: the other members slide off to the left, the cards off to the right.
  float away = 0;
  if (slide_t > 0) {
    const float t = static_cast<float>(slide_t) / static_cast<float>(kSlideAway);
    away = t * t * (3.0f - 2.0f * t);
  }
  group(Left);
  size_t chosen = members.size();
  for (size_t i = 0; i < members.size(); ++i)
    if (members[i] == cur_char()) chosen = i;
  for (size_t i = 0; i < members.size() && i < 3; ++i) {
    const float x = 16.0f + 72.0f * static_cast<float>(i);
    const bool bright = picking && static_cast<int>(i) == pick_cur;
    // On a character screen every portrait but the chosen one slides off the bottom (the strip is two 224-unit halves: the canvas is 448 tall).
    // The chosen one moves to the first slot, where the character screens draw it.
    const bool mine = portraits_away && i == chosen;
    const float dy = portraits_away && i != chosen ? 460.0f * away : 0.0f;
    portrait_strip(members[i], mine ? x + (16.0f - x) * away : x, grey(bright ? 0x80 : 0x40), dy);
    if (bright) cursor(PX(x) - 40.0f, PY(224) - 8.0f);
  }
  for (size_t i = 0; i < members.size() && i < 3; ++i)
    member_card(members[i], 250.0f + 16.0f * static_cast<float>(i) + 500.0f * away, 42.0f + 84.0f * static_cast<float>(i));
  group(Right);
  if (toggle) list_and_bar(1, choice_cur);
  else list_and_bar(0, main_cur, !listing);
  if (listing) build_roster();
  group(Centre);
  message(PX(200), PY(380));
}

// Roster panel (frame 141 at (8, 32), slid in from x -250 / out to it with the rows hidden): per row (17 from roster_top, 20 px apart)
// a recruited character's name, HP label and current / max HP (capped 999); the cursor on the left, the bouncing pointer on the right
// edge (right returns to the member pick), scroll arrows, and the footer hints or the remove-all prompt.
void MainMenu::Impl::build_roster() {
  group(Left);
  if (roster != Roster::On) {
    const float progress = static_cast<float>(kRosterSlide - roster_anim) / static_cast<float>(kRosterSlide);
    Frame(141, roster == Roster::In ? -250.0f + 250.0f * progress : -250.0f * progress, 32);
    return;
  }
  Frame(141, 8, 32);
  pointer(240.0f + static_cast<float>(((frames % 23) * 0x3000) >> 16), 202, false);
  if (roster_top + kRosterRows < save::kNumChars) scroll_arrow(237, 354, false);
  if (roster_top > 0) scroll_arrow(237, 50, true);
  for (int row = 0; row < kRosterRows; ++row) {
    const int character = roster_top + row;
    if (!gs->available(character)) continue;
    const save::SaveChar& ch = gs->slot.chars[static_cast<size_t>(character)];
    const float cap = 42.2f + 20.0f * static_cast<float>(row);
    const std::u32string name = char_name(character);
    text->prepare(face_gn, name);
    const float width = text->measure(face_gn, name, 0.35f) / xscale;
    HL(24, cap, name, kName, width < 98.0f ? 0.35f : 0.35f * 97.0f / width);
    HS(0x4, 124, cap, capt("\xEF\xBC\xA8\xEF\xBC\xB0", "HP"));
    HN(176, cap, std::to_string(std::min<int>(ch.cur_hp, 999)), ch.cur_hp < ch.max_hp / 8 ? kSel : kText, 0.29f);
    HT(face_kb, 184, cap, utf32("/"), kText, 0.29f);
    HN(224, cap, std::to_string(std::min<int>(ch.max_hp, 999)), kText, 0.29f);
  }
  tri(0, 42.0f + 20.0f * static_cast<float>(roster_row), !roster_ask);
  auto line = [&](float x, float y, const std::string& text_line) { TPfit(x, y, utf32(text_line), 232.0f - x, kText, 0.8f); };
  if (roster_ask) {
    line(24, 388, menu_str(0x801F27D0, "Alright to proceed?"));
    line(88, 412, menu_str(0x801F27E4, "Yes"));
    line(168, 412, menu_str(0x801F27EC, "No"));
    tri(roster_no ? 136.0f : 56.0f, 412, true);
    return;
  }
  btn(0x10, 0x182, 0);
  line(40, 388, menu_str(0x801F2798, "to choose/confirm."));
  if (pick_for != 0) {
    btn(0x10, 0x19A, 3);
    line(40, 412, pick_for == 2 ? menu_str(0x801F27B0, "to unequip all accessories.") : menu_str(0x801F27A0, "to unequip all Elements."));
  }
}

// Status / Elements-use member panel (frame 52): the card with stats and stamina, plus the status-ailment badges (frames 91..94 for
// poison / burn / sprain / flu, drawn at x - 0x11).
void MainMenu::Impl::status_panel(int character, float panel_x, float panel_y) {
  const save::SaveChar& ch = gs->slot.chars[static_cast<size_t>(character)];
  Frame(52, panel_x, panel_y);
  card_hd(panel_x, panel_y, character, true, true, 0x34);
  static const struct {
    u8 bit;
    int frame;
    float dx;
  } kBadge[4] = {{0x01, 91, 0x10}, {0x10, 92, 0x38}, {0x20, 93, 0x60}, {0x08, 94, 0x88}};
  for (const auto& badge : kBadge)
    if (ch.status & badge.bit)
      if (Tex* tex = texs.shape(badge.frame, lang)) Img(tex, PX(panel_x + badge.dx - 0x11), PY(panel_y + 0x66), PX(tex->w / 4.0f), PY(tex->h / 4.0f));
}

// ---- status: panel (40, 42), equipment frames below it, title + bottom bar, member-switch glyphs. The PS1 draws a live 3D model on
// the right standing on the parchment with no panel, casting a flat floor shadow (the host's MenuModelView draws it after the sprite pass);
// without one the member portrait stands in for it. This screen sits 2 PS1 px left of the other screens' coordinates.
// Profile of a character (ome.txt): up to 7 lines, keyed by the menu model the character wears (form byte up to 0x31, e.g. Serge's
// cat form or Pip's forms), else by the character id.
std::vector<std::u32string> MainMenu::Impl::profile_lines(int character) {
  std::vector<std::u32string> lines;
  const int form = gs->slot.chars[static_cast<size_t>(character)].model_id & 0xFF;
  const import::TextTable* table = lt.table("ome");
  const std::string* cell = table ? table->get(static_cast<u64>(form <= 0x31 ? form : character), lang) : nullptr;
  if (!cell) return lines;
  size_t start = 0;
  while (start <= cell->size() && lines.size() < 7) {
    const size_t end = std::min(cell->find('\n', start), cell->size());
    std::string line = cell->substr(start, end - start);
    std::erase(line, '\r');
    lines.push_back(utf32(line));
    start = end + 1;
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

void MainMenu::Impl::build_status() {
  const int character = cur_char();
  const float panel_x = 40.0f;
  group(Left);
  status_panel(character, panel_x, 42);
  if (profile) {
    Frame(63, panel_x, 180);
    notch_caption(panel_x, 180, 6);
    // 0.35 up to 155 px wide; wider lines shrink by a slope that eases off with the width.
    const std::vector<std::u32string> lines = profile_lines(character);
    for (size_t i = 0; i < lines.size(); ++i) {
      text->prepare(face_gn, lines[i]);
      const float width = text->measure(face_gn, lines[i], 0.35f) / xscale;
      const float slope = width < 155.0f ? 0.0f : width < 236.0f ? 0.002f : width < 286.0f ? 0.0015f : 0.0012f;
      HT(face_gn, 52.0f, 198.2f + 24.0f * static_cast<float>(i), lines[i], kText, 0.35f + (155.0f - width) * slope);
    }
  } else {
    Frame(49, panel_x, 180);
    Frame(50, panel_x, 228);
    Frame(111, panel_x, 276);
    static const float kFrameY[3] = {180.0f, 228.0f, 276.0f};
    for (int kind = 0; kind < 3; ++kind) notch_caption(panel_x, kFrameY[kind], kind);
    for (int slot = 0; slot < 5; ++slot) {
      const int id = gs->equipped(character, slot);
      const float y = slot == 0 ? 198.2f : slot == 1 ? 246.8f : 294.5f + 24.0f * static_cast<float>(slot - 2);
      if (id > 0) HL(80.0f, y, item_name(id), kName, 0.39f);
    }
  }
  if (model_view) {
    model_wanted = true;
    if (model_cam_manual) camera_legend();
  } else {
    group(Right);
    portrait_strip(character, 388.0f, kWhite);
    group(Left);
  }
  btn(0xE1 - 2, 0x172, 4);
  // Widescreen: the R1 glyph flanks the model on the right, so it moves out with the right group.
  group(Right);
  btn(0x1C9 - 2, 0x172, 5);
  group(Left);
  bar_and_title(0, true, -1.7f);
}

// A square glyph of the pad button (or key cap) bound to `action`, 12 x 14 PS1 px at (x, y): ui/gamepad.png cells (13 x 4, the plain rows) d-pad
// up 0, left 1, right 2, down 3; LT 21, RT 22. A key cap is drawn smaller than the cell and lower than a pad glyph.
void MainMenu::Impl::action_glyph(platform::Action action, float x, float y) {
  using platform::Action;
  constexpr float kGlyph = 12.0f, kGlyphH = 14.0f, kKeyCapDx = 1.2f, kKeyCapDy = 1.4f, kKeyCapW = 10.4f, kKeyCapH = 11.7f;
  if (kb_mode) {
    if (Tex* keys = texs.load(std::string("ui/keyboard_") + "efigsj"[std::clamp(lang_index(), 0, 5)] + ".png")) {
      const int cell = platform::keyboard_cell(platform::action_vk(action));
      Img(keys, PX(x + kKeyCapDx), PY(y + kKeyCapDy), PX(kKeyCapW), PY(kKeyCapH), static_cast<float>(cell & 15) / 16.0f, static_cast<float>(cell >> 4) / 7.0f,
          static_cast<float>((cell & 15) + 1) / 16.0f, static_cast<float>((cell >> 4) + 1) / 7.0f);
    }
    return;
  }
  const int index = action == Action::Up ? 0 : action == Action::Left ? 1 : action == Action::Right ? 2 : action == Action::Down ? 3
                    : action == Action::SlowSpeed ? 21 : 22;
  if (Tex* pad = texs.load("ui/gamepad.png")) {
    const int col = index % 13, row = (index / 13) * 2 + 1;
    Img(pad, PX(x), PY(y), PX(kGlyph), PY(kGlyphH), static_cast<float>(col) / 13.0f, static_cast<float>(row) / 4.0f,
        static_cast<float>(col + 1) / 13.0f, static_cast<float>(row + 1) / 4.0f);
  }
}

// The pad camera's legend (sheet frame 1 at (0x1D6, 0x32) and the L2 / R2 sprites 0xB8 / 0xB9 at (0x1BE, 0x4A) / (0x1BE, 0x53)). PS1 screens
// draw those disc frames. Otherwise the HD legend: menutim camera.png on the frame's camera part (-120, -28, 24 x 24) and four text lines
// from cmnmes, {Button..} = a square pad glyph or key cap: "{Up}{Down}: Zoom In/Out" and "{Left}{Right}: Rotate" from the frame's part
// x - 42, the L2 / R2 combinations right-aligned against ": Shift" / ": Tilt" at part x - 20 (French 29 px further left), one glyph height
// per line.
void MainMenu::Impl::camera_legend() {
  constexpr float kFrameX = 0x1D6, kFrameY = 0x32, kComboX = 0x1BE;
  group(Right);
  if (ps1_screen()) {
    const struct {
      int frame;
      float x, y;
    } kSprites[3] = {{1, kFrameX, kFrameY}, {0xB8, kComboX, 0x4A}, {0xB9, kComboX, 0x53}};
    for (const auto& sprite : kSprites)
      if (Tex* tex = texs.ps1("ps1:sheet" + std::to_string(sprite.frame), [&] { return ps1::sheet_frame(*vfs, sprite.frame); }))
        Img(tex, PX(sprite.x - ps1::kSheetFrameOriginX), PY(sprite.y - ps1::kSheetFrameOriginY), PX(ps1::kSheetFrameW), PY(ps1::kSheetFrameH));
    group(Left);
    return;
  }
  // A glyph is a square of 12 PS1 columns (14 rows); the lines are one glyph apart.
  constexpr float kGlyph = 12.0f, kGlyphH = 14.0f, kGlyphToText = 3.2f, kTextToGlyph = -1.4f, kLineTop = 46.6f, kCapDrop = 4.2f;
  constexpr float kGlyphLead = 2.4f, kLabelInk = 1.3f, kTextScale = 0.23f;
  const float shift_x = lang == import::Lang::Fr ? -29.0f : 0.0f;
  if (Tex* camera = texs.load("hd/menu/menutim/camera.png")) Img(camera, PX(kFrameX - 120 + shift_x), PY(kFrameY - 28), PX(24), PY(24));
  using platform::Action;
  struct Piece {
    bool glyph = false;
    Action action = Action::Up;
    std::u32string text;
  };
  const render::FaceId face = slot_face(mt_style(3, lang_index()).slot);
  auto pieces = [&](const std::string& row) {
    static const struct {
      std::string_view name;
      Action action;
    } kTokens[6] = {{"ButtonUp", Action::Up},       {"ButtonDown", Action::Down},     {"ButtonLeft", Action::Left},
                    {"ButtonRight", Action::Right}, {"ButtonL2", Action::SlowSpeed}, {"ButtonR2", Action::FastSpeed}};
    std::vector<Piece> out;
    std::string text_run;
    auto flush = [&] {
      if (text_run.find_first_not_of(' ') != std::string::npos) out.push_back({false, Action::Up, utf32(text_run)});
      text_run.clear();
    };
    for (size_t i = 0; i < row.size(); ++i) {
      const size_t close = row[i] == '{' ? row.find('}', i) : std::string::npos;
      if (close == std::string::npos) {
        text_run += row[i];
        continue;
      }
      flush();
      const std::string_view name(row.data() + i + 1, close - i - 1);
      for (const auto& token : kTokens)
        if (token.name == name) out.push_back({true, token.action, {}});
      i = close;
    }
    flush();
    for (Piece& piece : out) {
      if (piece.glyph) continue;
      const size_t first = piece.text.find_first_not_of(U' ');
      piece.text = piece.text.substr(first);
    }
    return out;
  };
  auto text_width = [&](const std::u32string& str) {
    text->prepare(face, str);
    return text->measure(face, str, kTextScale) / xscale;
  };
  auto width = [&](const std::vector<Piece>& line) {
    float total = 0;
    for (size_t i = 0; i < line.size(); ++i)
      total += line[i].glyph ? kGlyph : kGlyphToText + text_width(line[i].text) + (i + 1 < line.size() ? kTextToGlyph : 0.0f);
    return total;
  };
  auto glyph = [&](Action action, float x, float y) { action_glyph(action, x, y); };
  auto draw_line = [&](const std::vector<Piece>& line, float pen, int index) {
    const float top = kLineTop + kGlyphH * static_cast<float>(index);
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i].glyph) {
        glyph(line[i].action, pen, top);
        pen += kGlyph;
        continue;
      }
      HT(face, pen + kGlyphToText, top + kCapDrop, line[i].text, kText, kTextScale);
      pen += kGlyphToText + text_width(line[i].text) + kTextToGlyph;
    }
  };
  const float left = kFrameX - 42 - kGlyphLead + shift_x, label = kFrameX - 20 + kLabelInk - kGlyphToText + shift_x;
  const float combo_end = kComboX + kGlyphLead + shift_x;
  draw_line(pieces(cm("上下：Ｚｏｏｍ")), left, 0);
  draw_line(pieces(cm("左右：回転")), left, 1);
  const char* const kCombo[2] = {"＋Ｌ２ボタン", "＋Ｒ２ボタン"};
  const char* const kLabel[2] = {"：Ｓｈｉｆｔ", "：Ｔｉｌｔ"};
  for (int k = 0; k < 2; ++k) {
    const std::vector<Piece> combo = pieces(cm(kCombo[k]));
    draw_line(combo, combo_end - width(combo), 2 + k);
    draw_line(pieces(cm(kLabel[k])), label, 2 + k);
  }
  group(Left);
}

}  // namespace oc::ui
