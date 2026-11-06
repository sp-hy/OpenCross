#include "ui/menu/main_menu_impl.h"

#include <algorithm>
#include <cstdio>

#include "ui/menu/menu_common.h"
#include "ui/menu/ps1_art.h"
#include "ui/ps1_font.h"

namespace oc::ui {

// ---- equip
namespace {
// The row under the list cursor. Rows past the end of the list are blank and act like the reserved row: on an accessory list they take
// the accessory off when the slot is filled.
EquipCand row_at(const std::vector<EquipCand>& candidates, int row, bool fill_ok) {
  if (row >= 0 && row < static_cast<int>(candidates.size())) return candidates[static_cast<size_t>(row)];
  return EquipCand{-1, 0, 0, fill_ok};
}
}  // namespace

// The list cursor is kept between slots and list visits; it resets when the screen opens and when L1 / R1 switch the member, which
// also puts the slot column on the weapon (not while a roster pick stands in). While the slot column has the focus a cursor past
// the end of the slot's list moves to the row just past the last entry.
void MainMenu::Impl::update_equip(const Nav& nav) {
  if ((nav.l1 || nav.r1) && stand_in < 0) {
    cycle_member(nav.l1 ? -1 : 1);
    eq_list = false;
    eslot = eq_cur = eq_scroll = 0;
  }
  const int character = cur_char();
  if (!eq_list) {
    if (nav.up) eslot = wrap(eslot - 1, 5), sfx(1);
    if (nav.down) eslot = wrap(eslot + 1, 5), sfx(1);
    if (nav.cancel) back_to_main(2, true), sfx(6);
    else if (nav.ok) {
      eq_list = true;
      sfx(8);
    }
    if (const int count = static_cast<int>(gs->equip_candidates(cur_char(), eslot).size()); eq_cur > count)
      eq_cur = count, eq_scroll = std::max(0, count - (kVisibleRows - 1));
  } else {
    const std::vector<EquipCand> candidates = gs->equip_candidates(character, eslot);
    const int rows = std::max(static_cast<int>(candidates.size()), kVisibleRows);
    if (nav.up && eq_cur > 0) --eq_cur, sfx(1);
    if (nav.down && eq_cur + 1 < rows) ++eq_cur, sfx(1);
    scroll_to(eq_cur, eq_scroll, kVisibleRows);
    if (nav.cancel) eq_list = false, sfx(6);
    else if (nav.ok) {
      const EquipCand candidate = row_at(candidates, eq_cur, eslot >= 2 && gs->equipped(character, eslot) != 0);
      if (candidate.ok && gs->equip(character, eslot, candidate.item)) {
        sfx(0x48);
        eq_list = false;
      } else {
        sfx(0x18);
      }
    }
  }
}

void MainMenu::Impl::update_keyitems(const Nav& nav) {
  const int count = static_cast<int>(gs->key_items().size());
  if (nav.up && ki_cur > 0) --ki_cur, sfx(1);
  if (nav.down && ki_cur + 1 < count) ++ki_cur, sfx(1);
  scroll_to(ki_cur, ki_scroll, 15);
  if (nav.cancel) back_to_main(3), sfx(6);
}

// Equip stat-box caption in menutest style `flags`: slot scale x the language's multiplier in the style's slot (JP ik), left edge at
// PS1 `left_x`, 16-px row at `y`; key_dx / key_dy = per-language offsets relative to English (PS1 px). These styles
// have no shrink; `maxw` (PS1 px, from the caption's left edge to the preview arrow or the number field) is a guard.
void MainMenu::Impl::CapL(u32 flags, float left_x, float y, std::string_view key, const char* fallback, float key_dx, float key_dy, float maxw) {
  const std::u32string caption = capt(key, fallback);
  const MtStyle style = mt_style(flags, lang_index());
  const render::FaceId face = slot_face(style.slot);
  float scale = font::slot(style.slot).scale * style.mul;
  text->prepare(face, caption);
  if (const float width = text->measure(face, caption, scale); width > maxw * xscale) scale *= maxw * xscale / width;
  Cmd cmd;
  cmd.kind = Cmd::Text;
  cmd.text = caption;
  cmd.face = face;
  cmd.scale = scale;
  cmd.color = kCap;
  cmd.x = PX(left_x + key_dx);
  cmd.y = PY(y + key_dy) + (PY(16) - text->line_height(face, scale)) * 0.5f;
  emit(std::move(cmd));
}

// Header block (modes 2 / 3 of the equip screen): frame, name, HP, base stats, innate dot, grid minimap.
void MainMenu::Impl::header_block(int character, float x, float y, int hmode) {
  Frame(hmode == 3 ? 110 : 47, x, y);
  card_hd(x, y, character, hmode != 3, hmode != 3, hmode == 3 ? 0x6E : 0x2F);
}

// ---- equip: header (42,42), slot backdrops, stat columns, candidate panel (253,27), description (253,332)
void MainMenu::Impl::build_equip() {
  const int character = cur_char();
  const auto cands = gs->equip_candidates(character, eslot);
  const int cnt = static_cast<int>(cands.size());
  // Widescreen: portrait, glyphs, header, slot frames and stat columns left; candidate list and description right.
  group(Left);
  portrait_strip(character, 16.0f, grey(0x40));
  // Member-switch glyphs (L1 / R1).
  btn(0x32, 0x182, 4);
  btn(0xB9, 0x182, 5);
  header_block(character, 42, 42, eq_list ? 3 : 2);
  Frame(0x31, 42, 196);
  Frame(0x32, 42, 244);
  Frame(0x33, 42, 292);
  notch_caption(42, 196, 0);
  notch_caption(42, 244, 1);
  notch_caption(42, 292, 2);
  // equipped names: weapon (82,212), armour (82,260), accessories (82,308 + 24 k)
  for (int k = 0; k < 5; ++k) {
    const int id = gs->equipped(character, k);
    const float y = k == 0 ? 212.0f : k == 1 ? 260.0f : 308.0f + 24.0f * static_cast<float>(k - 2);
    if (id > 0) TPfit(82, y, item_name(id), 128, kText, 0.9f);
    if (k == eslot) tri(52, y, !eq_list);
  }
  // stats: current (lower box) and, in list mode, the preview with its direction colour
  const Stats cur = gs->stats(character);
  struct Pos {
    int Stats::*stat;
    float x, y, py;
    int cap;
  };
  static const Pos kPos[6] = {{&Stats::atk, 107, 137, 89, 999}, {&Stats::def, 107, 153, 105, 999}, {&Stats::mag, 102, 169, 121, 999},
                              {&Stats::mdef, 182, 169, 121, 999}, {&Stats::hit, 180, 137, 89, 99}, {&Stats::evd, 180, 153, 105, 99}};
  Stats pre = cur;
  const EquipCand under = row_at(cands, eq_cur, eslot >= 2 && gs->equipped(character, eslot) != 0);
  const bool preview = eq_list && under.ok;
  if (preview) pre = gs->stats_with(character, eslot, under.item);
  static const struct {
    const char* jp;
    const char* en;
  } kCapNames[6] = {{"攻撃力", "ATK."}, {"防御力", "DEF."}, {"魔法攻撃", "M. ATT."}, {"魔法防御", "M. DEF."}, {"命中率", "HIT%"}, {"回避率", "EVD%"}};
  // Per-language offsets relative to English. Captions (groups 0x30 / 0x6E): ATK. / DEF. x
  // kE530 {-4 x5, 0}, ATK. y kE500 {3 x5, 4}; EVD% / HIT% x kE550 {-8 x5, 0}, HIT% y kE67 {3 3 3 3 5 4}. Numbers
  // (keyed by the print position x 0x6B / 0x66 / 0xB6 / 0xB4): x tables kE3B0 {0 3 6 0 0 0} (ATK., DEF.),
  // kE3F0 {5 8 11 5 5 5} (M. ATT.), kE430 {14 x5, 9} (M. DEF.), kE460 {7 x5, 8} (HIT%, EVD%).
  static const float kE530[6] = {-4, -4, -4, -4, -4, 0}, kE550[6] = {-8, -8, -8, -8, -8, 0}, kE500[6] = {3, 3, 3, 3, 3, 4};
  static const float kE67[6] = {3, 3, 3, 3, 5, 4}, kE3B0[6] = {0, 3, 6, 0, 0, 0}, kE3F0[6] = {5, 8, 11, 5, 5, 5};
  static const float kE430[6] = {14, 14, 14, 14, 14, 9}, kE460[6] = {7, 7, 7, 7, 7, 8};
  const int lang_idx = lang_index();
  const float capdx[6] = {dl(kE530, lang_idx), dl(kE530, lang_idx), 0, 0, dl(kE550, lang_idx), dl(kE550, lang_idx)};
  const float capdy[6] = {dl(kE500, lang_idx), 0, 0, 0, dl(kE67, lang_idx), 0};
  const float numdx[6] = {dl(kE3B0, lang_idx), dl(kE3B0, lang_idx), dl(kE3F0, lang_idx), dl(kE430, lang_idx), dl(kE460, lang_idx), dl(kE460, lang_idx)};
  // Number y: JP differs from the others by -1 on the DEF. / EVD% current rows (y 0x99) and +1 on the preview
  // rows of ATK. / M. ATT. / M. DEF. / HIT% (y 0x59 / 0x79).
  const float curdy[6] = {0, lang_idx == 5 ? -1.0f : 0, 0, 0, 0, lang_idx == 5 ? -1.0f : 0};
  const float predy[6] = {lang_idx == 5 ? 1.0f : 0, 0, lang_idx == 5 ? 1.0f : 0, lang_idx == 5 ? 1.0f : 0, lang_idx == 5 ? 1.0f : 0, 0};
  for (int i = 0; i < 6; ++i) {
    // Style 0x67 for HIT%, else 2 (the force-JP bit is cleared on the cmnmes hit).
    const u32 style = i == 4 ? 0x67u : 2u;
    // English left edge of the caption (the JP sheet caption's x plus the English offset) and the preview arrow that ends its room.
    static const float kCapX[6] = {47, 47, 47, 132, 133, 133}, kArrowX[6] = {98, 98, 98, 183, 179, 179};
    const float left = kCapX[i] + capdx[i];
    CapL(style, kCapX[i], kPos[i].y, kCapNames[i].jp, kCapNames[i].en, capdx[i], capdy[i], kPos[i].x + numdx[i] - left);
    if (eq_list) CapL(style, kCapX[i], kPos[i].py, kCapNames[i].jp, kCapNames[i].en, capdx[i], capdy[i], kArrowX[i] - left);
  }
  for (int i = 0; i < 6; ++i) {
    const Pos& pos = kPos[i];
    Digits(pos.x + numdx[i], pos.y + curdy[i], padded_number(std::min(cur.*(pos.stat), pos.cap), 3), 8);
    if (preview) {
      const int new_value = pre.*(pos.stat), old_value = cur.*(pos.stat);
      Digits(pos.x + numdx[i], pos.py + predy[i], padded_number(std::min(new_value, pos.cap), 3), 8,
             new_value > old_value ? ps1_colour(0x004E77) : new_value < old_value ? ps1_colour(0x9F6D00) : kText);
    }
  }
  // Parts of the header's sheet frame that the panel art leaves out, at PS1 positions: "%" after the HIT / EVD values (the current rows
  // always, the preview rows in list mode, drawn as a number-face glyph over HD art). With the disc art the frame is built from the
  // "wind" sheet, so the divider and, in list mode, the six preview arrows are drawn from that sheet too.
  Tex* wind = ps1_screen() ? texs.ps1("ps1:wind", [&] { return ps1::menutim(*vfs, "wind"); }) : nullptr;
  auto sheet_part = [&](float x, float y, float u, float v, float w, float h) {
    Img(wind, PX(x), PY(y), PX(w), PY(h), u / wind->w, v / wind->h, (u + w) / wind->w, (v + h) / wind->h);
  };
  static const float kPercentY[4] = {137, 153, 89, 105};
  for (int row = 0; row < (eq_list ? 4 : 2); ++row) {
    if (wind) sheet_part(203, kPercentY[row], 153, 20, 12, 10);
    else HT(face_kb != render::kNoFace ? face_kb : face_gn, 205, kPercentY[row] + 1.7f, U"%", kText, 0.275f);
  }
  if (wind) {
    sheet_part(45, 133, 85, 18, 128, 2);
    sheet_part(173, 133, 88, 18, 42, 2);
    static const float kArrows[6][2] = {{98, 90}, {98, 106}, {98, 122}, {179, 90}, {179, 106}, {183, 122}};
    if (eq_list)
      for (const auto& arrow : kArrows) sheet_part(arrow[0], arrow[1], 212, 86, 7, 8);
  }
  // candidate panel + rows
  group(Right);
  Frame(0x3A + std::min(eslot, 2), 253, 27);
  notch_caption(253, 27, eslot);
  for (int row = 0; row < kVisibleRows; ++row) {
    const int idx = eq_scroll + row;
    if (idx >= cnt) break;
    const EquipCand& cand = cands[static_cast<size_t>(idx)];
    if (cand.stock_slot < 0) continue;
    const float y = 43.0f + 24.0f * static_cast<float>(row);
    const u32 col = cand.ok ? kText : rgb(128, 128, 128);
    TPfit(305, y, item_name(cand.item), 138, col, 0.9f);
    // The second Einlanzer (item 0x40) is a pair: its row always shows 2.
    Digits(449, y, padded_number(cand.item == 0x40 ? 2 : std::min(cand.qty, 99), 2), 16, col);
  }
  if (eq_list) tri(273, 43.0f + 24.0f * static_cast<float>(eq_cur - eq_scroll), true);
  const int total = std::max(cnt, kVisibleRows);
  if (eq_scroll > 0) scroll_arrow(485, 35, true);
  if (eq_scroll + kVisibleRows < total) scroll_arrow(485, 311, false);
  // description box (frame 0x36 large: 236x88 at (253,332)) with the waa_spec lines at (268, 340 + 16 l)
  Frame(0x36, 253, 332, kWhite, nullptr, nullptr, true);
  const int item = eq_list ? under.item : gs->equipped(character, eslot);
  if (item > 0)
    for (int ln = 0; ln < 3; ++ln) {
      const std::string spec = trim(table_row("waa_spec", item * 3 + ln));
      if (!spec.empty()) TPfit(268, 340.0f + 16.0f * static_cast<float>(ln), utf32(spec), 205, kText, 0.74f);
    }
}

// One description row of a key item at PS1 (x, y). A row may hold several lines (split at '\n', a '\r' is dropped), kLinePitch rows
// apart, each at text scale kTextScale, shrunk to fit `maxw` PS1 px. {ButtonFast} / {ButtonSlow} (the Time Shifter's speed buttons) stand for the
// key cap bound to the fast / slow speed action (its pad glyph with a pad): a kKeyCap square drawn kKeyCapIndent canvas units right of
// the token and kKeyCapRise canvas units above the line's top, over the run of spaces that follows; the text after the token resumes
// kKeyCapAdvance canvas units further on. Any other {..} stays as written.
void MainMenu::Impl::key_item_line(const std::string& description, float x, float y, float maxw) {
  constexpr float kLinePitch = 24.0f, kKeyCap = 50.0f, kKeyCapIndent = 65.0f, kKeyCapRise = 16.0f, kKeyCapAdvance = 59.0f, kTextScale = 0.95f;
  struct Piece {
    std::u32string text;
    platform::Action action = platform::Action::FastSpeed;
    bool glyph = false;
  };
  float top = y;
  size_t start = 0;
  for (;;) {
    const size_t end = std::min(description.find('\n', start), description.size());
    std::vector<Piece> pieces;
    std::string run;
    auto flush = [&] {
      if (!run.empty()) pieces.push_back({utf32(run)});
      run.clear();
    };
    for (size_t i = start; i < end; ++i) {
      const size_t close = description[i] == '{' ? description.find('}', i) : std::string::npos;
      const std::string_view name = close < end ? std::string_view(description).substr(i + 1, close - i - 1) : std::string_view();
      if (name == "ButtonFast" || name == "ButtonSlow") {
        flush();
        pieces.push_back({{}, name == "ButtonFast" ? platform::Action::FastSpeed : platform::Action::SlowSpeed, true});
        i = close;
      } else if (description[i] != '\r') {
        run += description[i];
      }
    }
    flush();
    auto width = [&](float rel) {
      float total = 0;
      for (const Piece& piece : pieces) total += piece.glyph ? kKeyCapAdvance / xscale : twp(piece.text, rel);
      return total;
    };
    float rel = kTextScale;
    while (rel > 0.4f && width(rel) > maxw) rel -= 0.04f;
    const float cell_w = kKeyCap / kKeyCapW, cell_h = kKeyCap / kKeyCapH;
    float pen = x;
    for (const Piece& piece : pieces) {
      if (piece.glyph) {
        const float cap_x = PX(pen) + kKeyCapIndent, cap_y = PY(top) - kKeyCapRise;
        action_glyph(piece.action, cap_x - kKeyCapLeft * cell_w, cap_y - kKeyCapTop * cell_h, cell_w, cell_h);
        pen += kKeyCapAdvance / xscale;
      } else {
        TP(pen, top, piece.text, kText, rel);
        pen += twp(piece.text, rel);
      }
    }
    if (end == description.size()) break;
    start = end + 1;
    top += kLinePitch;
  }
}

// ---- key items: list panel (35,30), picture (234,36), description panel (238,318)
void MainMenu::Impl::build_keyitems() {
  const auto ids = gs->key_items();
  const int cnt = static_cast<int>(ids.size());
  // Widescreen: the list panel left; picture and description panel right.
  group(Left);
  Frame(0x3D, 35, 30);
  // "Key Items" + icon in the frame's top gap.
  notch_caption(35, 30, 4);
  if (cnt > 0) tri(51, 46.0f + 24.0f * static_cast<float>(ki_cur - ki_scroll), true);
  for (int row = 0; row < 15; ++row) {
    const int idx = ki_scroll + row;
    if (idx >= cnt) break;
    TPfit(75, 46.0f + 24.0f * static_cast<float>(row), key_item_name(ids[static_cast<size_t>(idx)]), 135, kText, 0.9f);
  }
  group(Right);
  Frame(0x3E, 238, 318);
  if (cnt > 0 && ki_cur < cnt) {
    const int id = ids[static_cast<size_t>(ki_cur)];
    {
      // Picture: hd/menu/keyitem/NN.png in both art modes.
      char path[48];
      std::snprintf(path, sizeof path, "hd/menu/keyitem/%02d.png", id);
      // Classic art + Ps1ResMenus: cdrom/menu/keyitem/<name>.tim (255 x 255 texels at (0xEA, 0x24)).
      if (Tex* p1 = ps1_screen() ? texs.ps1("ps1:keyitem" + std::to_string(id), [&] { return ps1::keyitem(*vfs, id); }) : nullptr)
        Img(p1, PX(234), PY(36), PX(255), PY(255), 0, 0, ps1::kKeyItemUv, ps1::kKeyItemUv);
      else if (const Tex* tex = texs.load(path)) Img(tex, PX(234), PY(36), PX(255), PY(255));
    }
    for (int line = 0; line < 3; ++line) {
      const std::string description = trim(table_row("keyitem", id * 3 + line));
      if (!description.empty()) key_item_line(description, 246, 334.0f + 24.0f * static_cast<float>(line), 228);
    }
  }
}

}  // namespace oc::ui
