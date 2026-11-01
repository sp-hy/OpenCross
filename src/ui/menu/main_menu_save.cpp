#include "ui/menu/main_menu_impl.h"

#include <algorithm>
#include <cstdio>
#include <map>

#include "core/log.h"
#include "ui/menu/hand.h"
#include "ui/menu/menu_common.h"
#include "ui/menu/ps1_art.h"
#include "ui/menu/save_layout.h"
#include "ui/ps1_font.h"

namespace oc::ui {

namespace {

SavePrev preview_of(const save::SaveSlot& data, int slot, int block) {
  SavePrev preview;
  preview.used = true;
  preview.slot = slot;
  preview.block = block;
  preview.party = data.party();
  preview.frames = data.playtime_frames();
  preview.gold = data.gold();
  preview.stars = data.stars_total();
  preview.scenario = data.progression();
  preview.flags = data.summary_flags();
  return preview;
}

}  // namespace

// Fills the save-grid boxes from the chosen card in the card's directory (block) order, or the page rows by file number.
void MainMenu::Impl::load_previews() {
  prev.fill({});
  page_prev.fill({});
  if (save_card == 0) {
    for (int number = 0; page && number <= save::kOcFiles; ++number)
      if (const save::OcSaves::File* file = page->file(number)) page_prev[static_cast<size_t>(number)] = preview_of(file->data, number, 0);
    return;
  }
  if (!save_dat) return;
  const save::MemCard* card = save_dat->card(save_card);
  if (!card) return;
  std::vector<save::SlotRef> refs = save::list_slots(*card);
  std::sort(refs.begin(), refs.end(), [](const save::SlotRef& lhs, const save::SlotRef& rhs) { return lhs.block < rhs.block; });
  size_t box = 0;
  for (const save::SlotRef& ref : refs) {
    if (box >= prev.size()) break;
    try {
      prev[box++] = preview_of(save::read_slot(*card, ref.block), ref.slot, ref.block);
    } catch (const std::exception& error) {
      log::warn("menu: save slot {} unreadable: {}", ref.slot, error.what());
    }
  }
}

// The Save / Load dial sits at the top right of the 640-wide screen; its needle aims at the end of the entry's leader line. The hand
// texture is resampled in the 2.5 x 2.143 canvas ratio, so the aim is computed in canvas px with that ratio.
int MainMenu::Impl::save_list_angle(int sel) {
  namespace sl = save_layout;
  const int entry = std::clamp(sel, 0, 1);
  const float end_x = sl::kLeadBend[entry] + sl::kLeadDx, end_y = sl::kListY[entry] + sl::kLeadDy + sl::kLeadDrop;
  return hand_angle_toward((end_x - sl::kPivotX) * 2.0f, (end_y - sl::kPivotY) * (960.0f / 448.0f), 2.5f, 960.0f / 448.0f);
}

// Save / Load: phase 0 = the Save a File / Load a File choice (Save is greyed and unselectable when saving is blocked); phase 2 = the
// save choice (OpenCross / Slot 1 / Slot 2; starts on OpenCross when the page holds a file); phase 3 = "No save data in ..." (Load on
// an empty choice; confirm or 120 frames back to phase 0); phase 1 = the card's 2 x 8 box grid (card 1's box 0 is the auto-save, never
// a manual save target; up / down swap rows, boxes 7 and 8 stay) or the page's list (file 0 is its auto-save, never a manual save
// target; up / down one row, left / right one screen), with the overwrite / load confirmation on a used file. Cancel on the choice or
// the list returns to phase 0. The save-list-only mode (save_only) has no phase 0: Cancel on the choice closes the menu, on the list it
// returns to the choice.
void MainMenu::Impl::update_save(const Nav& nav) {
  if (save_phase == 0) {
    if ((nav.up || nav.down) && can_save) top_cur = 1 - top_cur, sfx(1);
    if (nav.cancel) {
      back_to_main(5);
      sfx(6);
    } else if (nav.ok) {
      if (top_cur == 0 && !can_save) {
        sfx(0x18);
        return;
      }
      load_mode = top_cur == 1;
      save_phase = 2;
      save_card = page && !page->empty() ? 0 : 1;
      save_confirm = false;
      msg.clear();
      sfx(8);
    }
    return;
  }
  if (save_phase == 2) {
    if (nav.up) save_card = (save_card + 2) % 3, sfx(1);
    if (nav.down) save_card = (save_card + 1) % 3, sfx(1);
    if (nav.cancel) {
      if (save_only) closed = true;
      else save_phase = 0;
      sfx(6);
    } else if (nav.ok) {
      sfx(8);
      load_previews();
      const bool any = save_card == 0 ? std::any_of(page_prev.begin(), page_prev.end(), [](const SavePrev& row) { return row.used; })
                                      : std::any_of(prev.begin(), prev.end(), [](const SavePrev& box) { return box.used; });
      if (load_mode && !any) {
        save_phase = 3;
        card_msg_start = frames;
        return;
      }
      save_phase = 1;
      if (save_card == 0) {
        // As the card grid: Load starts on the first used row (the auto-save when it exists), Save on the first savable row (file 1).
        page_cur = 1;
        if (load_mode) {
          page_cur = 0;
          for (int row = 0; row <= save::kOcFiles; ++row)
            if (page_prev[static_cast<size_t>(row)].used) {
              page_cur = row;
              break;
            }
        }
        page_top = std::clamp(page_cur - save_layout::kPageRows / 2, 0, save::kOcFiles + 1 - save_layout::kPageRows);
        return;
      }
      save_cur = save_card == 1 ? 0 : -1;
      if (load_mode) {
        load_cur = 0;
        for (int box = 0; box < 15; ++box)
          if (prev[static_cast<size_t>(box)].used) {
            load_cur = box;
            break;
          }
      }
    }
    return;
  }
  if (save_phase == 3) {
    if (nav.ok || frames - card_msg_start > 120) save_phase = 0;
    return;
  }
  if (save_confirm) {
    if (nav.up || nav.down || nav.left || nav.right) confirm_cur = 1 - confirm_cur, sfx(1);
    if (nav.cancel) save_confirm = false, sfx(6);
    else if (nav.ok) {
      save_confirm = false;
      if (confirm_cur == 0) {
        if (load_mode) do_load();
        else do_save();
      } else {
        sfx(6);
      }
    }
    return;
  }
  if (nav.cancel) {
    save_phase = save_only ? 2 : 0;
    msg.clear();
    sfx(6);
    return;
  }
  bool used = false;
  if (save_card == 0) {
    constexpr int kCount = save::kOcFiles + 1;
    const int before = page_cur;
    if (nav.up) page_cur = (page_cur + kCount - 1) % kCount;
    if (nav.down) page_cur = (page_cur + 1) % kCount;
    if (nav.left) page_cur = std::max(0, page_cur - save_layout::kPageRows);
    if (nav.right) page_cur = std::min(kCount - 1, page_cur + save_layout::kPageRows);
    if (page_cur != before) sfx(1);
    page_top = std::clamp(page_top, std::max(0, page_cur - save_layout::kPageRows + 1), page_cur);
    if (!nav.ok) return;
    if (!load_mode && page_cur == 0) {
      sfx(0x18);
      return;
    }
    used = page_prev[static_cast<size_t>(page_cur)].used;
  } else {
    const int first_box = load_mode || save_card == 2 ? 0 : 1;
    int box = load_mode ? load_cur : save_cur + 1;
    if (nav.left) box = box - 1 >= first_box ? box - 1 : 14, sfx(1);
    if (nav.right) box = box + 1 <= 14 ? box + 1 : first_box, sfx(1);
    if (nav.up || nav.down) {
      if (box < 7) box += 8;
      else if (box >= 9) box -= 8;
      if (box < first_box) box = first_box;
      sfx(1);
    }
    if (load_mode) load_cur = box;
    else save_cur = box - 1;
    if (!nav.ok) return;
    used = prev[static_cast<size_t>(box)].used;
  }
  if (load_mode && !used) {
    sfx(0x18);
  } else if (used) {
    save_confirm = true;
    confirm_cur = 1;
    sfx(8);
  } else {
    do_save();
  }
}

// Load: the host picks the request up (take_load_request) after the menu closed and starts the slot like Continue does.
void MainMenu::Impl::do_load() {
  const SavePrev& file = save_card == 0 ? page_prev[static_cast<size_t>(std::clamp(page_cur, 0, save::kOcFiles))]
                                        : prev[static_cast<size_t>(std::clamp(load_cur, 0, 14))];
  if (!file.used || (save_card == 0 ? !page : !save_dat)) {
    sfx(0x18);
    return;
  }
  load_card = save_card;
  load_slot = file.slot;
  load_block = file.block;
  load_pending = true;
  closed = true;
  sfx(8);
}

// Page: the cursor row's file (never the auto-save). Card: a used box overwrites its own file; an empty box makes a new file with the
// lowest unused number, 0 being card 1's auto-save. The language, battle counter and custom names go with it (ext_save.dat for the
// cards, the file itself for the page); a no-op without a host.
void MainMenu::Impl::do_save() {
  if (save_card == 0) {
    if (!page || page_cur < 1 || page_cur > save::kOcFiles) {
      sfx(0x18);
      return;
    }
    try {
      page->write(page_cur, gs->slot);
      saved = true;
      load_previews();
      sfx(5);
      say("Saved to OpenCross file " + save_layout::page_number(page_cur) + ".", 240);
    } catch (const std::exception& error) {
      log::warn("menu: OpenCross save {}: {}", page_cur, error.what());
      sfx(0x18);
      say("Failed to save to OpenCross. Please try again.");
    }
    return;
  }
  const SavePrev& box = prev[static_cast<size_t>(std::clamp(save_cur + 1, 0, 14))];
  const bool auto_card = save_card == 1;
  const u32 failed = auto_card ? 0x801F3115 : 0x801F3116;
  int slot_no = box.used ? box.slot : -1;
  if (slot_no < 0) {
    std::array<bool, 15> taken{};
    taken[0] = auto_card;
    for (const SavePrev& other : prev)
      if (other.used && other.slot >= 0 && other.slot < 15) taken[static_cast<size_t>(other.slot)] = true;
    for (int number = 0; number < 15 && slot_no < 0; ++number)
      if (!taken[static_cast<size_t>(number)]) slot_no = number;
  }
  if (slot_no < 0 || (slot_no == 0 && auto_card)) {
    sfx(0x18);
    return;
  }
  save::MemCard* card = save_dat ? save_dat->card(save_card) : nullptr;
  if (!card) {
    sfx(0x18);
    say(menu_str(failed, ""));
    return;
  }
  try {
    const int block = save::write_slot(*card, slot_no, gs->slot);
    if (!block) {
      sfx(0x18);
      say(menu_str(failed, ""));
      return;
    }
    save_dat->write_ext(save_card, slot_no);
    if (!save_out.empty()) save_dat->save(save_out);
    saved = true;
    load_previews();
    sfx(5);
    say(menu_str(auto_card ? 0x801F2ECD : 0x801F2ECE, ""), 240);
  } catch (const std::exception&) {
    sfx(0x18);
    say(menu_str(failed, ""));
  }
}

// Save / Load list of the save screen: "Save" / "Load" (cmnmes セーブ / ロード) at the top right with leader lines, the dial and the
// needle at the 640-wide display's spots (save_layout). Save is dimmed while saving is blocked. Both art modes.
void MainMenu::Impl::save_dial() {
  namespace sl = save_layout;
  if (Tex* deco = texs.load("hd/menu/menutim/menutim_02.png")) Img(deco, PX(sl::kDialX), PY(sl::kDialY), PX(sl::kDialW), PY(sl::kDialH), 0, 0, 1, 1, rgb(172, 172, 182));
  Hand(sl::kPivotX, sl::kPivotY, hand_angle);
  const u32 line_colour = rgb(200, 190, 190);
  const render::FaceId list_face = slot_face(mt_style(7, lang_index()).slot);
  for (int entry = 0; entry < 2; ++entry) {
    const std::u32string label = capt(entry == 0 ? "\xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96" : "\xE3\x83\xAD\xE3\x83\xBC\xE3\x83\x89", entry == 0 ? "Save" : "Load");
    const u32 colour = (entry == 0 && !can_save) ? ps1_colour(0x404040) : entry == top_cur ? kSel : kText;
    HT(list_face, sl::kListX[entry], sl::kListY[entry], label, colour, sl::kListScale);
    const float y = sl::kListY[entry] + sl::kLeadDy, start_x = sl::kListX[entry] - 2.8f, bend_x = sl::kLeadBend[entry];
    rect(PX(start_x), PY(y) - 1.1f, PX(bend_x - start_x), 2.2f, line_colour);
    Seg(PX(bend_x), PY(y), PX(bend_x + sl::kLeadDx), PY(y + sl::kLeadDrop), 2.2f, line_colour);
  }
  if (save_locked)
    HT(list_face, sl::kListX[1] - 40.0f, sl::kListY[1] + 26.0f, U"Debug tools used this session", rgb(230, 170, 90), sl::kListScale * 0.62f);
}

const Tex* MainMenu::Impl::face_tex(int character) {
  if (character < 0 || character > 49) return nullptr;
  // Classic art + Ps1ResMenus: cdrom/face/<stem>.tim (64 x 64; hd/face/1999 is its 4x redraw).
  if (ps1_screen())
    if (Tex* disc_face = texs.ps1("ps1:face" + std::to_string(character), [&] { return ps1::face(*vfs, character); })) return disc_face;
  const std::string set = face_set();
  auto [it, fresh] = face_files.try_emplace(set);
  if (fresh) {
    const std::string pre = "face/" + set + "/";
    for (const std::string& name : vfs->list("hd")) {
      if (name.rfind(pre, 0) != 0) continue;
      const std::string file = name.substr(pre.size());
      if (file.size() > 3 && std::isdigit(static_cast<unsigned char>(file[0])) && std::isdigit(static_cast<unsigned char>(file[1])) && file[2] == '_')
        it->second[(file[0] - '0') * 10 + (file[1] - '0')] = "hd/" + name;
    }
  }
  const auto found = it->second.find(character);
  return found == it->second.end() ? nullptr : texs.load(found->second);
}

// Face avatar: hd/face/<1999|2021>/NN_name.png in both art modes (folder by art mode); 64 x 64 art drawn at (x, y) size w x h canvas px.
void MainMenu::Impl::avatar(int character, float x, float y, float w, float h) {
  if (const Tex* tex = face_tex(character)) Img(tex, x, y, w, h);
}

// Location caption of a save (savetbl row save_layout::location_key), two lines split at the first newline.
std::pair<std::string, std::string> MainMenu::Impl::location_lines(u32 value) {
  std::string row = table_row("savetbl", save_layout::location_key(static_cast<int>(value)));
  std::string first_line = row, second_line;
  for (size_t pos; (pos = row.find('\r')) != std::string::npos;) row.erase(pos, 1);
  if (const size_t nl = row.find('\n'); nl != std::string::npos) first_line = row.substr(0, nl), second_line = row.substr(nl + 1);
  else first_line = row;
  return {trim(first_line), trim(second_line)};
}

// Label at ink (x, y) shrunk until it fits `maxw` PS1 px.
void MainMenu::Impl::HLfit(float x, float y, const std::u32string& str, float maxw, float sc) {
  const render::FaceId face_id = glyphs_ok(str) || face_kb == render::kNoFace ? face_gn : face_kb;
  text->prepare(face_id, str);
  while (sc > 0.18f && text->measure(face_id, str, sc) > PX(maxw)) sc -= 0.01f;
  HT(face_id, x, y, str, kText, sc);
}

// Slot icon of file number `file`: hd/menufaceicon/<set>/icon_NN.bmp.
const Tex* MainMenu::Impl::slot_icon(int file) {
  return texs.load(save_layout::slot_icon_path(face_set(), file));
}

// Save / Load screen (states 0 / 0x10 / 0x20 on the 640-wide display; layout: save_layout.h). No
// portrait strip, no title and no bottom bar here: the parchment, the Save / Load list with its dial, the slot grid (one fixed face icon
// per box, both art modes) or the OpenCross page's list, the detail frame of the cursor file and the message frame.
void MainMenu::Impl::build_save() {
  namespace sl = save_layout;
  const bool on_page = save_card == 0;
  // Box index; box 0 is the auto-save entry. Page: the row = the file number.
  const int cursor = save_phase == 0 ? -1 : on_page ? page_cur : load_mode ? load_cur : save_cur + 1;
  // Widescreen: list + dial right, slot grid left; the wide detail / message frames stay centred. The page's list takes the dial's place.
  if (!(on_page && save_phase == 1) && !save_only) {
    group(Right);
    save_dial();
  }
  group(Centre);
  // State 0: only the list + dial and frame 0x55 with the description of the highlighted choice. The save choice and its "No save
  // data" message keep them and add frame 0x56: "Please choose a slot." and the lines OpenCross / Slot 1 / Slot 2 (save_layout
  // choice_y) with the pointer beside the current one. The save-list-only mode draws neither the list nor frame 0x55.
  if (save_phase != 1) {
    if (!save_only) {
      Frame(85, sl::kTopMsgX, sl::kTopMsgY);
      const std::u32string description = utf32(menu_str(top_cur == 0 ? 0x801F2BE0 : 0x801F2C18, top_cur == 0 ? "Save a File" : "Load a File"));
      HL(sl::kTopMsgInkX, sl::kTopMsgInkY, description, kText, sl::kMsgScale);
    }
    if (save_phase == 0) return;
    Frame(86, sl::kMsgX, sl::kMsgY);
    auto line = [&](float x, float y, const std::string& str) { HLfit(x + 1.5f, y + 2.5f, utf32(str), sl::kMsgWidth, sl::kMsgScale); };
    if (save_phase == 3) {
      if (on_page) line(146, 336, "No save data in OpenCross.");
      else line(146, 336, menu_str(save_card == 1 ? 0x801F3181 : 0x801F3182, save_card == 1 ? "No save data in Slot 1." : "No save data in Slot 2."));
      return;
    }
    line(146, 336, menu_str(0x801F2CA4, "Please choose a slot."));
    line(sl::kChoiceX, sl::choice_y(0), "OpenCross");
    line(sl::kChoiceX, sl::choice_y(1), menu_str(0x801F2CDC, "Slot 1"));
    line(sl::kChoiceX, sl::choice_y(2), menu_str(0x801F2CF8, "Slot 2"));
    tri(sl::kChoiceCursorX, sl::choice_y(save_card) + 1.0f, true);
    return;
  }
  const SavePrev cp = cursor < 0 ? SavePrev{} : on_page ? page_prev[static_cast<size_t>(cursor)] : prev[static_cast<size_t>(cursor)];
  if (on_page) {
    // ---- OpenCross page: frame 64 with the caption in its tab, kPageRows rows from page_top (save_layout.h)
    Frame(64, sl::kPageX, sl::kPageY);
    {
      text->prepare(face_gn, U"OpenCross");
      float scale = sl::kSlotCapScale;
      while (scale > 0.18f && text->measure(face_gn, U"OpenCross", scale) > PX(sl::kPageCapMaxW)) scale -= 0.01f;
      HT(face_gn, sl::kPageX + (sl::kSlotCapX - sl::kGridX), sl::caption_y(sl::kPageY, scale), U"OpenCross", kText, scale);
    }
    const u32 dim = rgb(140, 132, 132);
    const std::u32string empty_text = utf32(menu_str(0x801F2E78, "EMPTY"));
    auto face_of = [&](const std::u32string& str) { return glyphs_ok(str) || face_kb == render::kNoFace ? face_gn : face_kb; };
    // Leader (auto-save caption on Auto) and first location line of every file; one row scale for the whole page.
    std::vector<std::pair<std::u32string, std::u32string>> labels(page_prev.size());
    float row_scale = sl::kRowScale;
    const float name_w = sl::kRowLocX - sl::kRowNameX - 4.0f;
    for (size_t number = 0; number < page_prev.size(); ++number) {
      const SavePrev& row = page_prev[number];
      if (!row.used) continue;
      std::u32string name = number == 0 ? utf32(sl::auto_save_caption(lang_index()))
                                         : row.party[0] < save::kNumChars ? default_char_name(row.party[0]) : std::u32string();
      std::replace(name.begin(), name.end(), U'\n', U' ');
      labels[number] = {std::move(name), sl::row_location(utf32(location_lines(row.scenario).first))};
      for (const std::u32string* str : {&labels[number].first, &labels[number].second}) {
        const render::FaceId face_id = face_of(*str);
        text->prepare(face_id, *str);
        const float width = str == &labels[number].first ? name_w : sl::kRowLocW;
        while (row_scale > 0.18f && text->measure(face_id, *str, row_scale) > PX(width)) row_scale -= 0.01f;
      }
    }
    auto label = [&](float x, float y, const std::u32string& str, u32 colour) { HT(face_of(str), x, y, str, colour, row_scale); };
    for (int i = 0; i < sl::kPageRows; ++i) {
      const int number = page_top + i;
      if (number > save::kOcFiles) break;
      const float y = sl::row_y(i);
      const SavePrev& row = page_prev[static_cast<size_t>(number)];
      const u32 colour = number == page_cur ? kSel : kText;
      if (number == page_cur) tri(sl::kRowCursorX, y - 1.5f, true);
      HL(sl::kRowNumX, y, utf32(sl::page_number(number)), colour, row_scale);
      if (!row.used) {
        HL(sl::kRowNameX, y, empty_text, number == page_cur ? kSel : dim, row_scale);
        continue;
      }
      label(sl::kRowNameX, y, labels[static_cast<size_t>(number)].first, colour);
      label(sl::kRowLocX, y, labels[static_cast<size_t>(number)].second, colour);
      HN(sl::kRowTimeRight, y, sl::playtime(row.frames), colour, row_scale);
      const u32 star_c = (row.flags & 1) ? sl::cleared_star_colour(colour) : colour;
      StarGlyph(sl::kRowStarX, y + sl::kRowStarDy, 0.55f, 0.0f, star_c);
      HT(face_kb != render::kNoFace ? face_kb : face_gn, sl::kRowStarsX, y, utf32(std::to_string(std::min(row.stars, 99))), star_c, row_scale);
    }
    if (page_top > 0) scroll_arrow(sl::kRowMarkX - 4.5f, sl::kRowY0 - 3.0f, true);
    if (page_top + sl::kPageRows <= save::kOcFiles) scroll_arrow(sl::kRowMarkX - 4.5f, sl::row_y(sl::kPageRows - 1) + 10.0f, false);
    Frame(84, sl::kDetailX, sl::kDetailY);
    {
      // One size for every number: the widest caption's.
      const std::u32string caption = capt("File#", "File#") + utf32(sl::page_caption(page_cur));
      float scale = sl::kFileCapScale;
      for (const char* widest : sl::kPageCaptionWidest) {
        const std::u32string str = capt("File#", "File#") + utf32(widest);
        text->prepare(face_gn, str);
        while (scale > 0.18f && text->measure(face_gn, str, scale) > PX(sl::kFileCapMaxW)) scale -= 0.01f;
      }
      text->prepare(face_gn, caption);
      HT(face_gn, sl::kFileCapX, sl::caption_y(sl::kDetailY, scale), caption, kText, scale);
    }
  } else {
    group(Left);
    Frame(82 + save_card - 1, sl::kGridX, sl::kGridY);
    // Caption tab: cmnmes "slot" in bn.
    HT(face_gn, sl::kSlotCapX, sl::kSlotCapY, capt("slot", "Slot") + (save_card == 1 ? U" 1" : U" 2"), kText, sl::kSlotCapScale);
    group(Centre);
    Frame(84, sl::kDetailX, sl::kDetailY);
    // Caption tab (bn): "File#" + the number + 1 of the cursor box's FILE, parsed from the CHRONOnn name; an
    // empty box shows no number; "File#  1" = CHRONO00, the auto-save.
    if (cursor >= 0)
      HT(face_gn, sl::kFileCapX, sl::kFileCapY, capt("File#", "File#") + (cp.used ? utf32(sl::file_number(cp.slot)) : std::u32string()), kText,
         sl::kFileCapScale);
    group(Left);
    // Boxes: no fill (the parchment shows); icon only when the box holds a file.
    for (int box = 0; box < 15; ++box) {
      const float x = sl::box_x(box), y = sl::box_y(box);
      if (prev[static_cast<size_t>(box)].used) {
        // Classic art + Ps1ResMenus: the PS1 box picture menutim.prd p<box>ic, texels 0..15 on the 32 x 32 box.
        int max_n = 0;
        for (const SavePrev& other : prev)
          if (other.used) max_n = std::max(max_n, other.slot + 1);
        // The PS1 draws this number + 1.
        const int file_no = prev[static_cast<size_t>(box)].slot;
        // Face icon choice: see save_layout.h.
        const int fn = sl::box_icon(box, file_no, cursor, max_n);
        Tex* p1 = ps1_screen() ? texs.ps1("ps1:icon" + std::to_string(file_no), [&] { return ps1::slot_icon(*vfs, file_no); }) : nullptr;
        if (p1) Img(p1, PX(x), PY(y), PX(sl::kBox), PY(sl::kBox), 0, 0, ps1::kSlotIconUv, ps1::kSlotIconUv);
        else if (const Tex* ic = slot_icon(fn)) Img(ic, PX(x), PY(y), PX(sl::kBox), PY(sl::kBox));
      }
      if (box == cursor) box_outline(PX(x - 1), PY(y - 1), PX(sl::kBox + 1), PY(sl::kBox + 1), 2, kWhite);
    }
    group(Centre);
  }
  // message box (frame 0x56): "Please select a file." / the overwrite question / the result
  Frame(86, sl::kMsgX, sl::kMsgY);
  auto msg_line = [&](const std::u32string& str) { HLfit(sl::kMsgInkX, sl::kMsgInkY, str, sl::kMsgWidth, sl::kMsgScale); };
  if (save_confirm) {
    msg_line(utf32(load_mode ? menu_str(0x801F27D0, "Alright to proceed?") : menu_str(0x801F2EE0, "")));
    // Yes / No: frame 0x57 at (0x192, 0x150), cursor at (0x198, 0x160 + 0x16 row).
    Frame(87, 402, 336);
    HL(434, 356, utf32(menu_str(0x801F2BD0, "Yes")), kText, sl::kMsgScale);
    HL(434, 378, utf32(menu_str(0x801F2BD8, "No")), kText, sl::kMsgScale);
    tri(408, 352.0f + 22.0f * static_cast<float>(confirm_cur), true);
  } else if (!msg.empty()) {
    msg_line(utf32(msg));
  } else {
    msg_line(utf32(menu_str(0x801F2E08, "Please select a file.")));
  }
  // Detail panel of the cursor box: faces, name / AUTO SAVE, star + stars, Time, gold, location
  if (!cp.used) {
    HL(sl::kFaceX + 7.0f, sl::kNameY, utf32(menu_str(0x801F2E78, "EMPTY")), kText, sl::kNameScale);
    return;
  }
  for (int i = 0; i < 3; ++i)
    if (cp.party[static_cast<size_t>(i)] < save::kNumChars)
      avatar(cp.party[static_cast<size_t>(i)], PX(sl::kFaceX + sl::kFaceStep * static_cast<float>(i)), PY(sl::kFaceY), PX(sl::kFace), PY(sl::kFace));
  const std::u32string leader = cp.party[0] < save::kNumChars ? default_char_name(cp.party[0]) : std::u32string();
  // The auto-save FILES (card 1's CHRONO00, the page's file 0) show their caption wherever their box is.
  const bool auto_file = cp.slot == 0 && save_card <= 1;
  const std::u32string name = auto_file ? utf32(sl::auto_save_caption(lang_index())) : leader;
  const std::string stars = std::to_string(std::min(cp.stars, 99)), tb = sl::playtime(cp.frames);
  std::string gold = grouped_gold(cp.gold);
  gold.erase(0, gold.find_first_not_of(' '));
  const auto ln = location_lines(cp.scenario);
  {
    // Name / auto-save caption: style 0x83 / 0xB7, shrink to 97, two lines for the auto-save outside EN / JP (save_layout.h name_fit).
    const int ds = jp() ? font::kKb : font::kBn;
    fonts.prepare(ds, name);
    const sl::NameFit nf = sl::name_fit(fonts.measure(ds, name, 0.35f), auto_file, lang_index());
    size_t line_start = 0;
    for (int line = 0;; ++line) {
      const size_t line_end = name.find(U'\n', line_start);
      HL(sl::kNameX, sl::kNameY + (nf.dy + nf.pitch * static_cast<float>(line)) * (448.0f / 960.0f),
         name.substr(line_start, line_end == std::u32string::npos ? line_end : line_end - line_start), kText, sl::kNameScale * nf.mul);
      if (line_end == std::u32string::npos) break;
      line_start = line_end + 1;
    }
  }
  // Detail frame at (0x54, 0xE6); frame 6 at (+0x130, +0x14). Star print: x - 2, style 0x10004 (slot 0, mul 0.625,
  // mode 4), y + 1 (instead of + 2).
  // The star sprite and count use colour 0x8080 | (flag bit 0 ? 0x100000 : 0x800000): a cleared save's stars are yellow.
  const u32 star_c = (cp.flags & 1) ? sl::cleared_star_colour(kText) : kText;
  StarGlyph(sl::kStarX, sl::kStarY, 0.625f, 0.0f, star_c);
  HT(face_kb != render::kNoFace ? face_kb : face_gn, sl::kStarsX, sl::kStarsY, utf32(stars), star_c, sl::kNumScale);
  HL(sl::kTimeLabelX, sl::kTimeLabelY, utf32(cm("PLAY")), kText, sl::kSmallScale);
  HN(sl::kTimeRight, sl::kTimeY, tb, kText, sl::kNumScale);
  HN(sl::kGoldRight, sl::kGoldY, gold, kText, sl::kNumScale);
  HL(sl::kGoldUnitX, sl::kGoldY, U"G", kText, sl::kGoldUnitScale);
  if (!ln.first.empty()) HLfit(sl::kLocX, sl::kLocY1, utf32(ln.first), 250, sl::kLocScale);
  if (!ln.second.empty()) HLfit(sl::kLocX, sl::kLocY2, utf32(ln.second), 250, sl::kLocScale);
}

}  // namespace oc::ui
