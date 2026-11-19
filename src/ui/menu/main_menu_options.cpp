// The Options pages of the OpenCross row: the Chronicle's (Hints, Show all, Reset), the Extras' (Unlock all) and the Achievements' (the set
// on, Hide notices, Show spoilers). All are laid out and driven as the stock Customize screen: the Customize panel, one row per setting with its choices side by side and the chosen one on
// the highlight bar, Up / Down between rows, Left / Right to change the row's setting. Reset asks Yes / No below the rows first.
#include <string>
#include <vector>

#include "ui/menu/main_menu_impl.h"
#include "ui/menu/menu_common.h"

namespace oc::ui {

namespace {

// The words of the Customize screen's Vibration Function row (Off / On) and of the menu's Yes / No question; the tables below are their
// fallbacks.
constexpr u32 kOffWord = 0x801F4BAE, kOnWord = 0x801F4BC0, kYesWord = 0x801F27E4, kNoWord = 0x801F27EC;
const Str6 kOff = {{"Off", "Non", "No", "Aus", "No", "オフ"}};
const Str6 kOn = {{"On", "Oui", "Sì", "Ein", "Sí", "オン"}};
const Str6 kYes = {{"Yes", "Oui", "Sì", "Ja", "Sí", "はい"}};
const Str6 kNo = {{"No", "Non", "No", "Nein", "No", "いいえ"}};
const Str6 kHints = {{"Hints", "Indices", "Indizi", "Hinweise", "Pistas", "ヒント"}};
const Str6 kFull = {{"Full", "Complets", "Completi", "Alle", "Completas", "すべて"}};
const Str6 kShowAll = {{"Show all", "Tout afficher", "Mostra tutto", "Alles zeigen", "Mostrar todo", "すべてみせる"}};
const Str6 kReset = {{"Reset", "Réinitialiser", "Azzera", "Zurücksetzen", "Reiniciar", "リセット"}};
const Str6 kUnlockAll = {{"Unlock all", "Tout débloquer", "Sblocca tutto", "Alles freischalten", "Desbloquear todo", "すべてかいほう"}};
const Str6 kAchievements = {{"Achievements", "Succès", "Obiettivi", "Erfolge", "Logros", "じっせき"}};
const Str6 kHideNotices = {{"Hide notices", "Masquer les avis", "Nascondi avvisi", "Meldungen aus", "Ocultar avisos", "つうちをかくす"}};
const Str6 kShowSpoilers = {{"Show spoilers", "Voir les spoilers", "Mostra spoiler", "Spoiler zeigen", "Mostrar spoilers", "ネタバレをみせる"}};
const Str6 kResetAsk = {{"Forget every record? Grid presets stay.", "Tout oublier ? Les préréglages restent.",
                         "Dimenticare tutto? I preset restano.", "Alles vergessen? Presets bleiben.", "¿Olvidar todo? Los presets se quedan.",
                         "きろくをすべてけしますか？プリセットはのこります。"}};

// The Customize panel's places (PS1 px): the panel, the first row, the row step, the label, the rightmost choice and the choice step,
// the highlight bar.
constexpr float kPanelX = 48, kPanelY = 44, kFirstRow = 60, kRowStep = 24, kLabelX = 72, kLastChoiceX = 376, kChoiceStep = 98;
constexpr float kBarW = 82, kBarH = 20;
// Room for the caption in the panel's notch.
constexpr float kNotchWidth = 51;

}  // namespace

void MainMenu::Impl::enter_options() {
  opt_cur = 0;
  opt_ask = false;
}

// Up / Down wrap over the rows (sfx 1); Left / Right step the row's setting, wrapping (sfx 1); OK on Reset asks (sfx 8); Cancel returns
// to the page list, the Achievements' to the Achievements page as it was left (sfx 6). The question: Left / Right between Yes and No
// (sfx 1), OK answers (Yes sfx 8, No sfx 1), Cancel is No (sfx 6).
void MainMenu::Impl::update_options(const Nav& nav) {
  const OcGroup owner = shown_def().group;
  const bool chronicle = owner == OcGroup::Chronicle;
  if (opt_ask) {
    if (nav.left || nav.right) opt_yes = !opt_yes, sfx(1);
    if (nav.cancel) {
      opt_ask = false;
      sfx(6);
    } else if (nav.ok) {
      opt_ask = false;
      sfx(opt_yes ? 8 : 1);
      if (opt_yes && oc_reset) oc_reset();
    }
    return;
  }
  const int rows = owner == OcGroup::Extras ? 1 : 3;
  if (nav.up) opt_cur = wrap(opt_cur - 1, rows), sfx(1);
  if (nav.down) opt_cur = wrap(opt_cur + 1, rows), sfx(1);
  if (nav.cancel) {
    if (owner == OcGroup::Achievements) oc_shown = achievement_page_row(false);
    else leave_page();
    sfx(6);
    return;
  }
  const int step = (nav.right ? 1 : 0) - (nav.left ? 1 : 0);
  if (chronicle && opt_cur == 2) {
    if (nav.ok) opt_ask = true, opt_yes = false, sfx(8);
    return;
  }
  if (step == 0) return;
  sfx(1);
  if (owner == OcGroup::Achievements) {
    if (opt_cur == 2) {
      ach_spoilers = !ach_spoilers;
      return;
    }
    if (opt_cur == 0) achievements_on = !achievements_on;
    else hide_achievement_notices = !hide_achievement_notices;
  } else if (!chronicle) {
    extras_unlock_all = !extras_unlock_all;
  } else if (opt_cur == 0) {
    records_hints = wrap(records_hints + step, 3);
  } else {
    records_show_all = !records_show_all;
  }
  if (oc_options_changed) oc_options_changed({records_hints, records_show_all, extras_unlock_all, achievements_on, hide_achievement_notices});
}

// The Customize panel with "Options" in its notch; row i at y 60 + 24 i: the label at x 72, the choices ending at x 376, 98 apart, the
// chosen one on the bar and in the chosen colour; the hand at x 40 on the cursor's row. The question takes the fifth row and its Yes / No
// the sixth, the hand on the answer.
void MainMenu::Impl::build_options() {
  const OcGroup owner = shown_def().group;
  const bool disc = !texs.hd();
  const u32 chosen = ps1_colour(disc && !jp() ? 0x707070 : 0x004048);
  const u32 bar = disc ? rgb(0xA0, 0x60, 0) : ps1_colour(0x0060A0);
  const auto row_y = [](int row) { return kFirstRow + kRowStep * static_cast<float>(row); };
  const auto word = [&](float x, float y, const std::u32string& str, u32 colour) {
    if (disc) PT(x, y, str, colour);
    else TP(x + kRowDx, y - 1.4f, str, colour, kRowRel);
  };
  const auto game_word = [&](u32 address, const Str6& fallback) { return utf32(trim(menu_str(address, S6(fallback)))); };
  struct Row {
    std::u32string label;
    std::vector<std::u32string> choices;
    int chosen = 0;
  };
  std::vector<Row> rows;
  const std::u32string off = game_word(kOffWord, kOff), on = game_word(kOnWord, kOn);
  if (owner == OcGroup::Chronicle) {
    rows.push_back({utf32(S6(kHints)), {off, utf32(S6(kHints)), utf32(S6(kFull))}, records_hints});
    rows.push_back({utf32(S6(kShowAll)), {off, on}, records_show_all ? 1 : 0});
    rows.push_back({utf32(S6(kReset)), {}, 0});
  } else if (owner == OcGroup::Achievements) {
    rows.push_back({utf32(S6(kAchievements)), {off, on}, achievements_on ? 1 : 0});
    rows.push_back({utf32(S6(kHideNotices)), {off, on}, hide_achievement_notices ? 1 : 0});
    rows.push_back({utf32(S6(kShowSpoilers)), {off, on}, ach_spoilers ? 1 : 0});
  } else {
    rows.push_back({utf32(S6(kUnlockAll)), {off, on}, extras_unlock_all ? 1 : 0});
  }
  Frame(64, kPanelX, kPanelY);
  if (!disc) {
    const std::u32string caption = utf32(S6(shown_def().title));
    float scale = 0.31f;
    text->prepare(face_gn, caption);
    const float width = text->measure(face_gn, caption, scale) / xscale;
    if (width > kNotchWidth) scale *= kNotchWidth / width;
    HT(face_gn, kPanelX + 9.7f, kPanelY + 0.5f, caption, kText, scale);
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& row = rows[i];
    const float y = row_y(static_cast<int>(i));
    word(kLabelX, y, row.label, kText);
    const int count = static_cast<int>(row.choices.size());
    for (int k = 0; k < count; ++k) {
      const float x = kLastChoiceX - kChoiceStep * static_cast<float>(count - 1 - k);
      if (k == row.chosen) rect(PX(x - 1), PY(y - 2), PX(kBarW), PY(kBarH), bar);
      word(x, y, row.choices[static_cast<size_t>(k)], k == row.chosen ? chosen : kText);
    }
  }
  if (!opt_ask) {
    tri(40, row_y(opt_cur), true);
    return;
  }
  const std::u32string ask = utf32(S6(kResetAsk));
  if (disc) PT(kLabelX, row_y(4), ask, kText);
  else TPfit(kLabelX + kRowDx, row_y(4) - 1.4f, ask, kLastChoiceX + kBarW - kLabelX, kText, kRowRel);
  const float yes_x = kLastChoiceX - kChoiceStep;
  word(yes_x, row_y(5), game_word(kYesWord, kYes), kText);
  word(kLastChoiceX, row_y(5), game_word(kNoWord, kNo), kText);
  tri((opt_yes ? yes_x : kLastChoiceX) - 32, row_y(5), true);
}

}  // namespace oc::ui
