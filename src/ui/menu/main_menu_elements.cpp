#include "ui/menu/main_menu_impl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/menu/menu_common.h"
#include "ui/menu/ps1_art.h"

namespace oc::ui {

// A refused action only plays sfx 0x18: the PS1 menus show no text for it.
void MainMenu::Impl::update_use(const Nav& nav) {
  const std::vector<int> list = gs->usable_elements();
  const int count = static_cast<int>(list.size());
  if (!use_target) {
    if (nav.up && use_cur > 0) --use_cur, sfx(1);
    if (nav.down && use_cur + 1 < count) ++use_cur, sfx(1);
    scroll_to(use_cur, use_scroll, kVisibleRows);
    if (nav.cancel) back_to_main(1), sfx(6);
    else if (nav.ok && count > 0) use_target = true, target_cur = 0, sfx(8);
  } else {
    const int party_size = static_cast<int>(party().size());
    if (nav.up) target_cur = wrap(target_cur - 1, party_size), sfx(1);
    if (nav.down) target_cur = wrap(target_cur + 1, party_size), sfx(1);
    if (nav.cancel) use_target = false, sfx(6);
    else if (nav.ok && count > 0 && party_size > 0) {
      const int id = gs->slot.elements[static_cast<size_t>(list[static_cast<size_t>(use_cur)])].id;
      if (gs->use_element(id, party()[static_cast<size_t>(target_cur)]) == Why::Ok) {
        sfx(8);
        if (gs->element_count(id) == 0) use_target = false, use_cur = use_scroll = 0;
      } else {
        sfx(0x18);
      }
    }
  }
}

// ---- element grid
// The grid editor's list: -1 = the two label rows, then the occupied stock slots in stock order (192 rows at most).
std::vector<int> MainMenu::Impl::grid_list() const {
  constexpr size_t kListRows = 192;
  std::vector<int> rows{-1, -1};
  for (size_t slot = 0; slot < gs->slot.elements.size() && rows.size() < kListRows; ++slot) {
    const save::ItemStack& stack = gs->slot.elements[slot];
    if (stack.id != 0 && stack.qty > 0) rows.push_back(static_cast<int>(slot));
  }
  return rows;
}

// Element of a list row; 0 for the label rows and for the row past the end, where the cursor stays after the last copy of the bottom
// element was placed.
int MainMenu::Impl::list_element(int index) const {
  const std::vector<int> list = grid_list();
  if (index < 0 || index >= static_cast<int>(list.size()) || list[static_cast<size_t>(index)] < 0) return 0;
  return gs->slot.elements[static_cast<size_t>(list[static_cast<size_t>(index)])].id;
}

// The element a pending pick carries: the picked list row's, the swap source's, or the picked cell's.
int MainMenu::Impl::held_element() const {
  const int character = cur_char();
  if (pick_element) return list_element(list_cur);
  if (swapping) return gs->cell_element(character, swap_row, swap_col);
  if (pick_cell) return gs->cell_element(character, cell_row, cell_col);
  return 0;
}

// Deepest grid row with an unlocked cell (0 when none).
int MainMenu::Impl::deepest_open_row(int character) const {
  int deepest = 0;
  for (int row = 0; row < GameState::kRows; ++row)
    if (gs->row_has_open_cell(character, row)) deepest = row;
  return deepest;
}

// A listed element is dimmed when the member may not hold it, when a picked cell's row lies outside its level window, or when its
// reach (level row +- the low nibble of the window byte) misses every row between the member's first and deepest unlocked rows.
bool MainMenu::Impl::element_dim(int character, int id) const {
  if (!gs->eligible(character, id)) return true;
  if (pick_cell && !swapping && !rules->window_ok(id, cell_row)) return true;
  const data::ElemRec* record = rules->elem(id);
  const int reach = record ? record->raw[0x12] & 0xF : 0, level_row = rules->el_level(id) - 1;
  return level_row - reach > deepest_open_row(character) || level_row + reach < gs->first_open_row(character);
}

namespace {

// Frames, counted from the press, until the grid screen takes input again: after opening (or a Remove All / Auto Allocate / Fill
// Blanks / Reorder, which rebuild the screen), after moving from the list to the grid (the row's names are rebuilt, one per frame), after
// a cancelled cell pick, after a list-side fill, after a grid-side fill (the list's text is rebuilt), after a swap, after a column move,
// after a grid-to-list move, and after a row change (the 30-frame slide, then the panel redraw). A move from the list hands the input
// over after kHandOver frames, so a row change started from the list slides that much later.
constexpr int kLockOpen = 27, kLockToGrid = 18, kLockCancelPick = 9, kLockListFill = 17, kLockGridFill = 15, kLockSwap = 10,
              kLockColumn = 9, kLockToList = 4, kLockRow = 40, kHandOver = 3;
// Row slide: 15 PS1 px per frame, out to 225 px on frames 1..15, the new row in from 210 px on frames 16..30.
constexpr int kSlideStep = 15, kSlideOut = 15, kSlideIn = 210, kSlideFrames = 30;

}  // namespace

void MainMenu::Impl::lock_grid(int ticks) { grid_unlock = frames + ticks; }

// The cell strip's x offset (PS1 px) of a running row slide and the row it shows (the old one before and while it slides out); 0 when
// none runs.
int MainMenu::Impl::slide_offset(int& shown_row) const {
  shown_row = cell_row;
  const int elapsed = frames - slide_from;
  if (elapsed < 0) shown_row = slide_row;
  if (elapsed < 0 || elapsed > kSlideFrames) return 0;
  if (elapsed <= kSlideOut) {
    shown_row = slide_row;
    return -slide_dir * kSlideStep * elapsed;
  }
  return slide_dir * (kSlideIn - kSlideStep * (elapsed - kSlideOut - 1));
}

void MainMenu::Impl::grid_reset() {
  focus = 0, list_cur = list_scroll = 0, cell_col = 0, prompt_cur = 0, swap_row = swap_col = -1;
  arrow_side = pick_element = pick_cell = swapping = list_dim = grid_dim = false;
  prompt = GridPrompt::None;
  cell_row = gs->first_open_row(cur_char());
  labels_from = slide_from = -100;
  lock_grid(kLockOpen);
}

// Hands the input to the grid on `row`; the column cursor moves on to the next unlocked cell when its own one is not. Another row
// slides in from the left (the old row out to the right) once the grid has the input, and holds it for the whole slide.
void MainMenu::Impl::to_grid(int row, int lock_ticks, bool rebuild_labels) {
  const int character = cur_char();
  if (gs->row_has_open_cell(character, row) && row != cell_row) {
    slide_from = frames + kHandOver, slide_dir = -1, slide_row = cell_row;
    cell_row = row;
    lock_ticks = kHandOver + kLockRow;
  }
  for (int step = 0; step < GameState::kCols && !gs->cell_available(character, cell_row, cell_col); ++step)
    cell_col = wrap(cell_col + 1, GameState::kCols);
  if (rebuild_labels) labels_from = frames + 2;
  lock_grid(lock_ticks);
  focus = 1;
  grid_dim = false;
}

void MainMenu::Impl::to_list() {
  focus = 0;
  list_dim = false;
  lock_grid(kLockToList);
}

void MainMenu::Impl::move_cell_col(int step) {
  const int character = cur_char();
  for (int distance = 1; distance <= GameState::kCols; ++distance) {
    const int col = wrap(cell_col + step * distance, GameState::kCols);
    if (gs->cell_available(character, cell_row, col)) {
      cell_col = col;
      break;
    }
  }
  lock_grid(kLockColumn);
  sfx(1);
}

// Left / Right on the grid: the next lower / higher row with an unlocked cell, wrapping. Wrapping past the bottom or top row returns
// to the list instead, unless an element or a swap is pending; a pending element must be legal in the new row or the move is ignored.
// A new row puts the cursor on its first unlocked cell after column 0.
void MainMenu::Impl::move_cell_row(int step) {
  const int character = cur_char();
  int row = cell_row;
  for (int distance = 1; distance <= GameState::kRows; ++distance) {
    row = wrap(cell_row + step * distance, GameState::kRows);
    if (gs->row_has_open_cell(character, row)) break;
  }
  if (const int held = held_element(); held != 0 && !rules->window_ok(held, row)) return;
  const bool wrapped = step < 0 ? row > cell_row : row < cell_row;
  if (wrapped && !pick_element && !swapping) {
    sfx(1);
    to_list();
    grid_dim = true;
    return;
  }
  slide_from = frames, slide_dir = step, slide_row = cell_row;
  cell_row = row;
  cell_col = 0;
  for (int distance = 0; distance < GameState::kCols; ++distance) {
    cell_col = wrap(cell_col + 1, GameState::kCols);
    if (gs->cell_available(character, cell_row, cell_col)) break;
  }
  lock_grid(kLockRow);
  sfx(0xC);
}

// The stock list shrank: the cursor steps back when the list now ends above it, and the window scrolls up when it shows rows past the end.
void MainMenu::Impl::list_changed() {
  const int count = static_cast<int>(grid_list().size());
  int row = list_cur - list_scroll;
  if (count < list_cur && --row < 0) row = 0, list_scroll = std::max(0, list_scroll - 1);
  if (count < list_scroll + kVisibleRows && list_scroll > 0) --list_scroll;
  list_cur = list_scroll + row;
}

// Puts the list row's choice into the cell at the grid cursor (sfx 0x17): Remove empties it, an element replaces the occupant and
// returns it to the stock. An element that is not legal in the row or that the member may not hold, or a fixed occupant, refuses with
// 0x18 and keeps the pick. Then the half that made the second choice gives the input back: a list row picked first returns to the list,
// a cell picked first to the grid.
void MainMenu::Impl::apply_to_cell() {
  const int character = cur_char();
  sfx(0x17);
  if (list_cur == 1) {
    if (gs->remove(character, cell_row, cell_col) == Why::Fixed) {
      sfx(0x18);
      pick_cell = false;
      return;
    }
    list_cur -= list_scroll, list_scroll = 0;
  } else {
    const int id = list_element(list_cur);
    if (id == 0) return;
    if (!rules->window_ok(id, cell_row) || !gs->eligible(character, id) || gs->place(character, cell_row, cell_col, id) != Why::Ok) {
      sfx(0x18);
      return;
    }
    list_changed();
  }
  if (pick_cell) {
    pick_cell = false;
    to_grid(cell_row, kLockListFill, false);
    list_dim = true;
  } else {
    pick_element = false;
    to_list();
    grid_dim = true;
    lock_grid(kLockGridFill);
  }
}

// The question in the description panel: Remove All asks Yes / No (Left / Right, No first); Auto Allocate and Fill Blanks ask this
// member / the whole party (Up / Down; the party is refused while a roster character stands in); Reorder offers the three sort orders.
void MainMenu::Impl::update_grid_prompt(const Nav& nav) {
  const int character = cur_char();
  switch (prompt) {
    case GridPrompt::RemoveAll:
      if (nav.left || nav.right) prompt_cur ^= 1, sfx(1);
      if (nav.ok) {
        prompt = GridPrompt::None;
        if (prompt_cur == 0) {
          gs->clear_all(character);
          list_cur -= list_scroll, list_scroll = 0;
          lock_grid(kLockOpen);
          sfx(0x17);
        }
        return;
      }
      break;
    case GridPrompt::AutoAllocate:
    case GridPrompt::FillBlanks:
      if (nav.up || nav.down) prompt_cur ^= 1, sfx(1);
      if (nav.ok) {
        if (stand_in >= 0 && prompt_cur == 1) {
          sfx(0x18);
          return;
        }
        sfx(1);
        const bool reset = prompt == GridPrompt::AutoAllocate;
        if (prompt_cur == 0) gs->auto_fill(character, reset);
        else gs->auto_fill_party(reset);
        sfx(0x17);
        list_cur -= list_scroll, list_scroll = 0;
        lock_grid(kLockOpen);
        prompt = GridPrompt::None;
        return;
      }
      break;
    case GridPrompt::Sort:
      if (nav.up) prompt_cur = wrap(prompt_cur - 1, 3), sfx(1);
      if (nav.down) prompt_cur = wrap(prompt_cur + 1, 3), sfx(1);
      if (nav.ok) {
        sfx(1);
        gs->sort_elements(static_cast<SortMode>(prompt_cur), character);
        list_cur -= list_scroll, list_scroll = 0;
        lock_grid(kLockOpen);
        prompt = GridPrompt::None;
        sfx(8);
        return;
      }
      break;
    default: break;
  }
  if (nav.cancel) prompt = GridPrompt::None, sfx(6);
}

// The list half. Right shows the second pair of label rows (a second Right, like Left, moves to the grid); Triangle opens Reorder. OK on
// Remove All / Auto Allocate / Fill Blanks asks its question; on Remove or an element it picks the row and moves to the grid, an element
// to its own level row clamped to the unlocked rows (refused silently when no unlocked row is in its reach). With a cell picked first,
// OK puts the row into that cell and Left takes the cell back to the grid to be swapped.
void MainMenu::Impl::update_grid_list(const Nav& nav) {
  const int character = cur_char();
  const int count = static_cast<int>(grid_list().size());
  if (nav.up && list_cur > 0) --list_cur, sfx(1);
  if (nav.down && list_cur + 1 < count) ++list_cur, sfx(1);
  scroll_to(list_cur, list_scroll, kVisibleRows);
  if (nav.left) {
    sfx(1);
    if (arrow_side) {
      arrow_side = false;
    } else {
      if (pick_cell) swapping = true, swap_row = cell_row, swap_col = cell_col;
      to_grid(cell_row, kLockToGrid, true);
      list_dim = true;
    }
  } else if (nav.right && !pick_cell) {
    sfx(1);
    if (!arrow_side) {
      arrow_side = true;
    } else {
      arrow_side = false;
      to_grid(cell_row, kLockToGrid, true);
      list_dim = true;
    }
  } else if (nav.tri) {
    prompt = GridPrompt::Sort, prompt_cur = 0;
    sfx(1);
  } else if (nav.ok) {
    if (pick_cell) {
      if (list_cur != 0 && (!arrow_side || list_cur > 1)) apply_to_cell();
      else sfx(0x18);
      return;
    }
    sfx(1);
    if (list_cur == 0) {
      prompt = arrow_side ? GridPrompt::AutoAllocate : GridPrompt::RemoveAll;
      prompt_cur = arrow_side ? 0 : 1;
    } else if (list_cur == 1 && arrow_side) {
      prompt = GridPrompt::FillBlanks, prompt_cur = 0;
    } else if (const int id = list_element(list_cur); id == 0) {
      pick_element = true;
      to_grid(cell_row, kLockToGrid, true);
    } else if (gs->eligible(character, id)) {
      const data::ElemRec* record = rules->elem(id);
      const int reach = record ? record->raw[0x12] & 0xF : 0, level_row = rules->el_level(id) - 1;
      const int first = gs->first_open_row(character), deepest = deepest_open_row(character);
      if (level_row - reach <= deepest && first <= level_row + reach) {
        const int row = std::max(std::min(level_row, deepest), first);
        if (row != cell_row) sfx(0xC);
        pick_element = true;
        to_grid(row, kLockToGrid, true);
      }
    }
  } else if (nav.cancel) {
    sfx(6);
    if (pick_cell) {
      pick_cell = swapping = false;
      to_grid(cell_row, kLockCancelPick, false);
      list_dim = true;
    } else {
      back_to_main(1, true);
    }
  }
}

// The grid half. Up / Down move along the row, Left / Right change rows. OK on a cell picks it and moves to the list, or completes a
// pending list pick or swap; a fixed occupant refuses (0x18). Cancel drops a swap (staying on the grid) or a list pick (back to the list),
// else returns to the list.
void MainMenu::Impl::update_grid_cells(const Nav& nav) {
  const int character = cur_char();
  if (nav.up) move_cell_col(-1);
  if (nav.down) move_cell_col(1);
  if (nav.left) move_cell_row(-1);
  else if (nav.right) move_cell_row(1);
  if (focus != 1) return;
  if (nav.ok) {
    if (const int occupant = gs->cell_element(character, cell_row, cell_col); occupant != 0 && rules->el_fixed(occupant)) {
      sfx(0x18);
    } else if (swapping) {
      sfx(0x17);
      const Why why = gs->swap(character, swap_row, swap_col, cell_row, cell_col);
      if (why == Why::Ok || why == Why::Same) {
        pick_cell = swapping = false;
        labels_from = frames - 1;
        lock_grid(kLockSwap);
      } else {
        sfx(0x18);
      }
    } else if (pick_element) {
      apply_to_cell();
    } else {
      pick_cell = true;
      to_list();
      sfx(1);
    }
  } else if (nav.cancel) {
    sfx(6);
    if (swapping) {
      pick_cell = swapping = false;
      list_dim = true;
      lock_grid(kLockCancelPick);
    } else {
      pick_element = false;
      to_list();
      grid_dim = true;
    }
  }
}

// L1 / R1 (not while a roster character stands in) re-enter the screen with the next member.
void MainMenu::Impl::update_grid(const Nav& nav) {
  if (frames < grid_unlock) return;
  if ((nav.l1 || nav.r1) && stand_in < 0) {
    cycle_member(nav.l1 ? -1 : 1);
    grid_reset();
    return;
  }
  if (prompt != GridPrompt::None) update_grid_prompt(nav);
  else if (focus == 0) update_grid_list(nav);
  else update_grid_cells(nav);
}

// ---- Elements: use: three member panels (47, 27 + 132 i), dim portraits, list panel (253,27), description (253,332)
void MainMenu::Impl::build_use() {
  const auto list = gs->usable_elements();
  const int cnt = static_cast<int>(list.size());
  const auto members = party();
  // Widescreen: portraits, member panels and their cursors left; the element list and description right.
  group(Left);
  for (size_t i = 0; i < members.size() && i < 3; ++i) portrait_strip(members[i], 16.0f + 72.0f * static_cast<float>(i), grey(0x40));
  for (size_t i = 0; i < members.size() && i < 3; ++i) status_panel(members[i], 47.0f, 27.0f + 132.0f * static_cast<float>(i));
  if (use_target) {
    tri(30, 37.0f + 132.0f * static_cast<float>(target_cur), true);
    // Roll frame 3.
    pointer(38.0f - static_cast<float>(((frames % 23) * 0x3000) >> 16), 224.0f, true);
  }
  group(Right);
  Frame(186, 253, 27);
  // "Elements" + icon in the list frame's top gap.
  notch_caption(253, 27, 3);
  for (int row = 0; row < kVisibleRows; ++row) {
    const int idx = use_scroll + row;
    if (idx >= cnt) break;
    const auto& stack = gs->slot.elements[static_cast<size_t>(list[static_cast<size_t>(idx)])];
    const float y = 47.0f + 24.0f * static_cast<float>(row);
    plate(cur_char(), stack.id, 275, 42.0f + 24.0f * static_cast<float>(row));
    const std::u32string nm = element_name(stack.id);
    const float rel = fit_rel(nm, 140, 0.9f);
    TP(301.0f + (128.0f - twp(nm, rel)) * 0.5f, y, nm, idx == use_cur && !use_target ? ps1_colour(0x7A6AA5) : kText, rel);
    Digits(453, y, padded_number(std::min<int>(stack.qty, 99), 2), 16, kText);
  }
  const int total = std::max(cnt, kVisibleRows);
  if (use_scroll > 0) scroll_arrow(485, 35, true);
  if (use_scroll + kVisibleRows < total) scroll_arrow(485, 311, false);
  Frame(0x36, 253, 332, kWhite, nullptr, nullptr, true);
  if (cnt > 0) tri(251, 47.0f + 24.0f * static_cast<float>(use_cur - use_scroll), !use_target);
  if (cnt > 0 && use_cur < cnt) {
    const int id = gs->slot.elements[static_cast<size_t>(list[static_cast<size_t>(use_cur)])].id;
    element_info(id, 260, 340, 378.5f, 19.5f);
  }
  group(Centre);
  message(PX(268), PY(412));
}

// Row tint behind an element name: sprite 0x5FF + 4k + 3 = menutim_14_<t>_01 (184x24), t by colour k.
void MainMenu::Impl::element_row_tint(int id, float x, float y) {
  const int bit = rules->el_colour(id);
  int colour = -1;
  for (int bit_index = 2; bit_index < 8; ++bit_index)
    if (bit & (1 << bit_index)) colour = bit_index - 2;
  if (colour < 0) return;
  static const int kTile[6] = {9, 12, 10, 11, 13, 8};
  if (Tex* tex = plate_tex(kTile[colour])) Img(tex, PX(x), PY(y), PX(184), PY(24), 0, 0, 1, 1, grey(0x70));
}

// menutim_14_<tile>_01.png (184 x 24), or in classic art + Ps1ResMenus the PS1 colour-sheet frame 1 composed from menutim.prd.
Tex* MainMenu::Impl::plate_tex(int tile) {
  if (ps1_screen())
    if (Tex* ps1_tex = texs.ps1("ps1:plate" + std::to_string(tile), [&] { return ps1::plate(*vfs, tile, 1); })) return ps1_tex;
  char path[64];
  std::snprintf(path, sizeof path, "hd/menu/menutim/menutim_14_%02d_01.png", tile);
  return texs.load(path);
}

// Element info lines of the Elements screens from x: "Lv", the level, plus-minus and the level reach, then the target text (description
// line 0, e.g. "Single Ally (Consum.)") at y0, wrapped at a space onto the next line (x, y0 + 18) when it would pass x 462; the two
// description lines at y1 (+ pitch).
void MainMenu::Impl::element_info(int id, float x, float y0, float y1, float pitch) {
  constexpr float kRel = 0.97f, kTargetX = 80, kRightEdge = 462, kWrapPitch = 18;
  auto description = [&](int line) {
    const std::string* row = lt.sysmes((0x1Au << 16) | static_cast<u32>(id * 3 + line), lang);
    return row ? trim(*row) : std::string();
  };
  TP(x, y0, utf32(menu_str(0x801F29B0, "Lv")), kName, kRel);
  Digits(x + 26, y0, padded_number(rules->el_level(id), 1), 16, kName);
  if (const data::ElemRec* record = rules->elem(id)) {
    TP(x + 48.5f, y0, U"\u00B1", kName, kRel);
    Digits(x + 53, y0, padded_number(record->raw[0x12] & 0xF, 1), 16, kName);
  }
  const std::string target = description(0);
  size_t cut = target.size();
  while (cut != std::string::npos && cut > 0 && twp(utf32(target.substr(0, cut)), kRel) > kRightEdge - x - kTargetX)
    cut = target.rfind(' ', cut - 1);
  if (cut == std::string::npos || cut == 0) cut = target.size();
  TP(x + kTargetX, y0, utf32(target.substr(0, cut)), kName, kRel);
  if (cut < target.size()) TP(x, y0 + kWrapPitch, utf32(trim(target.substr(cut))), kName, kRel);
  for (int line = 1; line <= 2; ++line)
    if (const std::string text_line = description(line); !text_line.empty())
      TPfit(x, y1 + pitch * static_cast<float>(line - 1), utf32(text_line), 470 - x, kName, kRel);
}

// ---- element grid: grid half on the left, stock list + description on the right
// Colour index of an element for the plates / minimap: 0..5 = green, white, black, red, yellow, blue; 7 = multi; -1 none.
// Fixed elements (pgdat flag 0x200) take the member's innate colour.
int MainMenu::Impl::colour_index(int character, int id) const {
  if (id <= 0) return -1;
  const int bit = rules->el_fixed(id) ? gs->innate(character) : rules->el_colour(id);
  if ((bit & 0xFC) == 0xFC) return 7;
  int colour = -1;
  for (int bit_index = 2; bit_index < 8; ++bit_index)
    if (bit & (1 << bit_index)) colour = bit_index - 2;
  return colour;
}

// 184x24 row plate, menutim_14_<tile>_01.
void MainMenu::Impl::plate(int character, int id, float x, float y) {
  const int colour = colour_index(character, id);
  if (colour < 0) return;
  static const int kTile[8] = {9, 12, 10, 11, 13, 8, 0, 38};
  if (Tex* tex = plate_tex(kTile[colour])) Img(tex, PX(x), PY(y), PX(184), PY(24), 0, 0, 1, 1, grey(0x80));
}

// Thin 173x24 frame around an empty slot (eleempty.png), both art modes.
void MainMenu::Impl::empty_box(float x, float y) {
  if (Tex* tex = texs.load("hd/menu/eleempty.png")) Img(tex, PX(x + 4), PY(y), PX(174), PY(25));
}

// 8 levels x 11 columns of 16x8 element tiles at pitch (18, 9).
void MainMenu::Impl::grid_minimap(int character, float px, float py, int sel_row, int hold_id) {
  // menuelem_<n> for colour index 0..5, 7 = multi (8).
  static const int kFile[8] = {3, 6, 4, 5, 7, 2, 8, 8};
  int lo = 99, hi = -99;
  if (hold_id > 0 && rules->elem(hold_id)) {
    const int tol = rules->elem(hold_id)->raw[0x12] & 0xF;
    lo = rules->el_level(hold_id) - 1 - tol, hi = rules->el_level(hold_id) - 1 + tol;
  }
  for (int row = 0; row < GameState::kRows; ++row)
    for (int col = 0; col < GameState::kCols; ++col) {
      if (!gs->cell_exists(character, row, col) || !gs->cell_available(character, row, col)) continue;
      const int id = gs->cell_element(character, row, col);
      // file 1 = empty cell.
      int file = 1;
      if (id > 0) {
        const int colour = colour_index(character, id);
        file = colour < 0 ? 1 : kFile[colour];
      }
      u32 tint = grey(0x30);
      if (hold_id > 0 && row >= lo && row <= hi) tint = ps1_colour(0xA04040);
      if (row == sel_row) tint = grey(0xA0);
      const float x = px + 18.0f * static_cast<float>(row), y = py + 9.0f * static_cast<float>(col);
      char path[48];
      std::snprintf(path, sizeof path, "hd/menu/menuelem/menuelem_%d.png", file);
      if (Tex* tex = texs.load(path)) Img(tex, PX(x), PY(y), PX(16), PY(8), 0, 0, 1, 1, tint);
    }
}

void MainMenu::Impl::build_grid() {
  const int character = cur_char();
  const auto list = grid_list();
  const int cnt = static_cast<int>(list.size());
  const int held = held_element();
  // Widescreen: portrait, member-switch glyphs and the grid panel left; stock list, description and Reorder right.
  group(Left);
  portrait_strip(character, 16.0f, grey(0x40));
  // Member-switch glyphs (L1 / R1) under the grid panel, not while a roster character stands in.
  if (stand_in < 0) {
    btn(0x14, 0x195, 4);
    btn(0xC6, 0x195, 5);
  }
  // ---- left: grid panel (frame 0x38 at (10,27)), level number, minimap, 11-row cell list of the selected level
  Frame(56, 10, 27);
  {
    // Caption "Element Level N" in the grid frame's top gap (cmnmes "Element Level"; ink at (19.7, 27), bn 0.31, bn in every language).
    HT(face_gn, 19.7f, 27.0f, capt("Element Level", "Element Level"), kText, 0.31f);
    // The level number is its own print at (0x66, 0x19) in kb x 0.625, offset by a per-language x
    // {-10, 5, 0x1B, -0x10, 0xB, -10}[L], so it follows the end of the translated caption (FR / IT / ES are longer).
    static const float k42EDE0[6] = {-10, 5, 0x1B, -0x10, 0xB, -10};
    HS(0x91, 0x66 + k42EDE0[lang_index()], 0x19, utf32(std::to_string(cell_row + 1)), kText);
  }
  grid_minimap(character, 50, 45, cell_row, held);
  // The cell strip of the shown row (the old row while a row change slides it out), shifted by the slide and clipped to x 13..231; the
  // names are hidden while it slides and come back one per frame after the row was rebuilt. Names are dimmed when the pending element
  // (or swap source) is not legal in this row, or for fixed occupants.
  int shown_row = cell_row;
  const float slide = static_cast<float>(slide_offset(shown_row));
  const bool sliding = frames >= slide_from && frames - slide_from <= kSlideFrames;
  const bool row_refused = (pick_element || swapping) && held != 0 && !rules->window_ok(held, shown_row);
  sprite_clip_left = PX(13), sprite_clip_right = PX(231);
  for (int i = 0; i < 11; ++i) {
    if (i >= GameState::kCols || !gs->cell_available(character, shown_row, i)) continue;
    const float xb = 22.0f + slide, yi = 152.0f + 24.0f * static_cast<float>(i), yt = 157.0f + 24.0f * static_cast<float>(i);
    const int id = gs->cell_element(character, shown_row, i);
    if (id == 0) {
      empty_box(xb, yi);
      continue;
    }
    plate(character, id, xb, yi);
    if (sliding || (frames > labels_from && frames - labels_from < i)) continue;
    u32 col = i == cell_col && !grid_dim ? ps1_colour(0x7A6AA5) : ps1_colour(0x909090);
    if (row_refused || (swapping && !rules->window_ok(id, swap_row)) || rules->el_fixed(id)) col = ps1_colour(0x404040);
    const std::u32string nm = element_name(id);
    const float rel = fit_rel(nm, 124, 0.9f);
    TP(46.0f + (128.0f - twp(nm, rel)) * 0.5f, yt, nm, col, rel);
    if (id < 0x80) {
      const int level_delta = shown_row - (rules->el_level(id) - 1);
      if (level_delta != 0)
        TP(46.0f + 128.0f + 6.0f, yt, utf32(std::string(level_delta > 0 ? "+" : "-") + std::to_string(std::abs(level_delta))), col, 0.75f);
      if (rules->el_stack(id) && gs->cell_count(character, shown_row, i) > 0)
        TP(xb + 0x98, yt, utf32("x" + std::to_string(gs->cell_count(character, shown_row, i))), col, 0.8f);
    }
  }
  sprite_clip_left = -1e9f, sprite_clip_right = 1e9f;
  // Left-pointing cursor at (205, 157 + 24 i), not while the row slides; while swapping it sits at (211, 151 + 24 i) and a translucent
  // one marks the source cell when it is in the shown row.
  auto left_cursor = [&](float x, float y, int alpha) {
    const int ph = frames % 96;
    const int pulse = 0x90 + (ph < 48 ? ph * 2 : (95 - ph) * 2);
    const int g = std::min(255, (pulse - 0x80) * 2);
    for (int i = 0; i < 12; ++i) {
      const float width_factor = 1.0f - std::fabs(static_cast<float>(i) + 0.5f - 6.0f) / 6.0f;
      rect(PX(x + 23.0f * (1 - width_factor)), PY(y + static_cast<float>(i)), PX(23.0f * width_factor), PY(1) + 0.6f, rgb(255, g, g, alpha));
    }
  };
  if (focus == 1 && !grid_dim && !sliding) {
    if (!swapping) {
      left_cursor(205, 157.0f + 24.0f * static_cast<float>(cell_col), 255);
    } else {
      left_cursor(211, 151.0f + 24.0f * static_cast<float>(cell_col), 255);
      if (cell_row == swap_row) left_cursor(205, 157.0f + 24.0f * static_cast<float>(swap_col), 128);
    }
  }
  // ---- right: stock list panel (frame 0x35 at (253,27))
  group(Right);
  Frame(53, 253, 27);
  // "Elements" + icon in the list frame's top gap.
  notch_caption(253, 27, 3);
  const int bounce = ((frames % 23) * 0x3000) >> 16;
  for (int i = 0; i < kVisibleRows; ++i) {
    const int idx = list_scroll + i;
    if (idx >= cnt) break;
    const float yp = 42.0f + 24.0f * static_cast<float>(i), yt = 47.0f + 24.0f * static_cast<float>(i);
    u32 col = idx == list_cur && !list_dim ? ps1_colour(0x7A6AA5) : ps1_colour(0x909090);
    // Label rows in an empty box: Remove All / Remove, or Auto Allocate / Fill Blanks; the side pointer shows which pair Right / Left
    // switches to.
    if (list[static_cast<size_t>(idx)] < 0) {
      empty_box(275, yp);
      if ((idx == 0 || arrow_side) && (list_dim || pick_cell)) col = ps1_colour(0x404040);
      static const u32 kLabel[2][2] = {{0x801F2A10, 0x801F2A20}, {0x801F2A2C, 0x801F2A3C}};
      static const char* const kFallback[2][2] = {{"Remove All", "Remove"}, {"Auto Allocate", "Fill Blanks"}};
      const std::u32string nm = utf32(menu_str(kLabel[arrow_side][idx], kFallback[arrow_side][idx]));
      const float rel = fit_rel(nm, 124, 0.9f);
      TP(301.0f + (128.0f - twp(nm, rel)) * 0.5f, yt, nm, col, rel);
      if (!list_dim) {
        if (arrow_side) pointer(287.0f - static_cast<float>(bounce), 49.0f + 24.0f * static_cast<float>(i), true);
        else pointer(443.0f + static_cast<float>(bounce), 49.0f + 24.0f * static_cast<float>(i), false);
      }
      continue;
    }
    const auto& stack = gs->slot.elements[static_cast<size_t>(list[static_cast<size_t>(idx)])];
    if (element_dim(character, stack.id)) col = ps1_colour(0x404040);
    plate(character, stack.id, 275, yp);
    const std::u32string nm = element_name(stack.id);
    const float rel = fit_rel(nm, 124, 0.9f);
    TP(301.0f + (128.0f - twp(nm, rel)) * 0.5f, yt, nm, col, rel);
    Digits(453, yt, padded_number(std::min<int>(stack.qty, 99), 2), 16, col);
  }
  const float cursor_y = 47.0f + 24.0f * static_cast<float>(list_cur - list_scroll);
  if (prompt != GridPrompt::None) tri(251, cursor_y, false);
  else if ((focus == 0 && !list_dim) || pick_element) tri(251, cursor_y, true);
  const int total = std::max(cnt, kVisibleRows);
  if (list_scroll > 0) scroll_arrow(485, 35, true);
  if (list_scroll + kVisibleRows < total) scroll_arrow(485, 311, false);
  // Reorder (Triangle) in the gap of the list frame's bottom edge: the glyph at (0x181, 0x137) and the sheet's "Reorder" part at
  // (0x19C, 0x13B), which RDE replaces in both of its art modes by the cmnmes text in style 0x74, 3 PS1 px lower (4 in German and
  // Japanese). The disc's part is the Japanese word, so only the Japanese PS1-resolution classic screen draws it. Dim off the list.
  const bool reorder_dim = focus != 0 || list_dim;
  btn(0x181, 0x137, 3, grey(reorder_dim ? 0x40 : 0x80));
  if (ps1_screen() && jp()) {
    if (Tex* tex = texs.ps1("ps1:sheet104", [&] { return ps1::sheet_frame(*vfs, 0x68); }))
      Img(tex, PX(253 - ps1::kSheetFrameOriginX), PY(27 - ps1::kSheetFrameOriginY), PX(ps1::kSheetFrameW), PY(ps1::kSheetFrameH), 0, 0, 1, 1,
          grey(reorder_dim ? 0x40 : 0x80));
  } else {
    const float drop = lang == import::Lang::De || jp() ? 4.0f : 3.0f;
    HS(0x74, 0x19C, 0x13B + drop, utf32(cm("\xE3\x81\xAA\xE3\x82\x89\xE3\x81\xB3\xE3\x81\x8B\xE3\x81\x88")), reorder_dim ? ps1_colour(0x404040) : kText);
  }
  // ---- description panel: frame 0x36 large at (253,332); text from x 260, lines y = 344, 362, 380, 398
  Frame(0x36, 253, 332, kWhite, nullptr, nullptr, true);
  if (focus == 0 && (prompt != GridPrompt::None || list_cur < 2)) {
    grid_help(character);
  } else {
    const int id = focus == 1 ? gs->cell_element(character, cell_row, cell_col) : list_element(list_cur);
    if (id > 0 && id < 0x80) {
      element_info(id, 260, 344, 384, 19.5f);
      if (rules->el_innate_only(id)) {
        const int colour_mask = colour_index(character, id) >= 0 ? rules->el_colour(id) : 0;
        int ci = 0;
        for (int bit_index = 2; bit_index < 8; ++bit_index)
          if (colour_mask & (1 << bit_index)) ci = bit_index - 2;
        const std::string fmt = menu_str(0x801F2A88, "(Only if innate color %s)");
        const std::string cn = trim(menu_str(0x801F4A30 + 3u * static_cast<u32>(ci), ""));
        std::string caption = fmt;
        if (const size_t marker = caption.find("%s"); marker != std::string::npos) caption.replace(marker, 2, cn);
        TPfit(260, 362, utf32(caption), 210, kName, 0.74f);
      }
    }
  }
  group(Centre);
  message(PX(268), PY(412));
}

// Description panel text of the label rows and the questions: entry k of the menu text block, four lines of 18 px from y 345 at x 260
// (a line's text may run on to the next lines). Entries 0 / 1 = Remove All / Remove, 2 / 3 = Auto Allocate / Fill Blanks, 4 = the Remove All
// Yes / No, 5 / 6 = the member / whole party choice (the member's name leads line 0; the party line is dimmed while a roster
// character stands in), 7 = the sort orders. The choice lines of 4 / 5 / 6 (line 2) sit at x 286, 2.5 px higher. Choice cursors: Yes / No
// (290 + 80 c, 382), member / party (276, 344 + 36 c), sort (268, 365 + 18 c).
void MainMenu::Impl::grid_help(int character) {
  int entry = (arrow_side ? 2 : 0) + list_cur;
  switch (prompt) {
    case GridPrompt::RemoveAll: entry = 4; break;
    case GridPrompt::AutoAllocate: entry = 5; break;
    case GridPrompt::FillBlanks: entry = 6; break;
    case GridPrompt::Sort: entry = 7; break;
    default: break;
  }
  const bool scope = entry == 5 || entry == 6;
  // Leading spaces and runs of two or more advance 4.06 PS1 px each (they line the words up with the choice cursors); single spaces
  // inside a phrase are the font's.
  constexpr float kSpaceAdvance = 4.06f;
  auto spaced_line = [&](float x, float y, const std::string& part, u32 colour) {
    size_t at = part.find_first_not_of(' ');
    float pen = x + kSpaceAdvance * static_cast<float>(at == std::string::npos ? 0 : at);
    while (at != std::string::npos) {
      const size_t gap = part.find("  ", at);
      const std::u32string chunk = utf32(part.substr(at, gap == std::string::npos ? std::string::npos : gap - at));
      const float rel = fit_rel(chunk, 470.0f - pen, 0.98f);
      TP(pen, y, chunk, colour, rel);
      if (gap == std::string::npos) break;
      at = part.find_first_not_of(' ', gap);
      pen += twp(chunk, rel) + kSpaceAdvance * static_cast<float>(at == std::string::npos ? 0 : at - gap);
    }
  };
  for (int line = 0; line < 4; ++line) {
    const std::string* row = lt.menu_text(0x801F46F0u + static_cast<u32>(entry * 4 + line) * 0x1Au, lang);
    if (!row || row->empty()) continue;
    std::string row_text = *row;
    const bool choice_line = entry >= 4 && entry <= 6 && line == 2;
    float x = choice_line ? 286.0f : 260.0f;
    if (scope && line == 0) row_text = import::u32_to_utf8(char_name(character)) + " " + trim(row_text), x = 302;
    const u32 colour = scope && line == 2 && stand_in >= 0 ? ps1_colour(0x404040) : kName;
    size_t start = 0;
    for (int piece = 0; line + piece < 4 && start <= row_text.size(); ++piece) {
      const size_t end = std::min(row_text.find('\n', start), row_text.size());
      const std::string part = row_text.substr(start, end - start);
      spaced_line(x, (choice_line ? 342.5f : 345.0f) + 18.0f * static_cast<float>(line + piece), part, colour);
      start = end + 1;
    }
  }
  switch (prompt) {
    case GridPrompt::RemoveAll: tri(290.0f + 80.0f * static_cast<float>(prompt_cur), 382, true); break;
    case GridPrompt::AutoAllocate:
    case GridPrompt::FillBlanks: tri(276, 344.0f + 36.0f * static_cast<float>(prompt_cur), true); break;
    case GridPrompt::Sort: tri(268, 365.0f + 18.0f * static_cast<float>(prompt_cur), true); break;
    default: break;
  }
}

}  // namespace oc::ui
