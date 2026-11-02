#include "ui/title/slot_list.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "core/log.h"
#include "game/new_game_plus.h"
#include "import/tim.h"
#include "render/texture.h"
#include "ui/menu/ps1_art.h"
#include "ui/menu/save_layout.h"

namespace oc::ui {

namespace {
using render::pack_rgba;
}  // namespace

std::u32string format_playtime(u32 frames) {
  const std::string text = save_layout::playtime(frames);
  return std::u32string(text.begin(), text.end());
}

std::vector<SlotEntry> SlotList::collect(const save::SaveDat& dat, int card_no) {
  std::vector<SlotEntry> out;
  const save::MemCard* card = dat.card(card_no);
  if (!card) return out;
  for (const save::SlotRef& ref : save::list_slots(*card)) {
    try {
      SlotEntry entry;
      entry.card = card_no;
      entry.slot = ref.slot;
      entry.block = ref.block;
      entry.data = save::read_slot(*card, ref.block);
      out.push_back(std::move(entry));
    } catch (const std::exception& ex) {
      log::warn("slot list: block {}: {}", ref.block, ex.what());
    }
  }
  // Box order = the card's directory order, not the CHRONOnn number: box k shows the k-th file, with the icon of its number.
  std::sort(out.begin(), out.end(), [](const SlotEntry& lhs, const SlotEntry& rhs) { return lhs.block < rhs.block; });
  return out;
}

std::vector<std::optional<SlotEntry>> SlotList::collect(const save::OcSaves& page) {
  std::vector<std::optional<SlotEntry>> out(save::kOcFiles + 1);
  for (int number = 0; number <= save::kOcFiles; ++number)
    if (const save::OcSaves::File* file = page.file(number)) {
      SlotEntry entry;
      entry.card = 0;
      entry.slot = number;
      entry.data = file->data;
      out[static_cast<size_t>(number)] = std::move(entry);
    }
  return out;
}

bool SlotList::any(const save::SaveDat* dat, const save::OcSaves* page) {
  return (page && !page->empty()) || (dat && (!collect(*dat, 1).empty() || !collect(*dat, 2).empty()));
}

void SlotList::begin(const save::SaveDat* dat, const save::OcSaves* page, import::Lang lang, ArtMode mode, bool plus) {
  for (int card = 0; card < 2; ++card) cards_[static_cast<size_t>(card)] = dat ? collect(*dat, card + 1) : std::vector<SlotEntry>{};
  rows_ = page ? collect(*page) : std::vector<std::optional<SlotEntry>>(save::kOcFiles + 1);
  const u32 language = static_cast<u32>(lang);
  for (auto& entries : cards_)
    for (SlotEntry& entry : entries) entry.ext_names = dat->peek_ext(entry.card, entry.slot, language).names;
  for (auto& row : rows_)
    if (row) row->ext_names = page->peek_ext(row->slot, language).names;
  entries_.clear();
  phase_ = Phase::Card;
  card_ = page && !page->empty() ? 0 : 1;
  phase_time_ = 0;
  plus_ = plus;
  lang_ = lang;
  mode_ = mode;
  sel_ = top_ = 0;
  picked_ = cancelled_ = false;
}

void SlotList::update(const Nav& nav, float dt) {
  if (picked_ || cancelled_) return;
  clock_ += dt;
  phase_time_ += dt;
  // Choice: up / down step OpenCross / Slot 1 / Slot 2 (wrap); confirm opens it (an empty one shows "No save data in ...").
  if (phase_ == Phase::Card) {
    if (nav.up) card_ = (card_ + 2) % 3, play(1);
    if (nav.down) card_ = (card_ + 1) % 3, play(1);
    if (nav.confirm) {
      play(8);
      phase_time_ = 0;
      sel_ = top_ = 0;
      if (card_ == 0) {
        const auto first = std::find_if(rows_.begin(), rows_.end(), [](const auto& row) { return row.has_value(); });
        const bool any_file = first != rows_.end();
        // The cursor starts on the first file, which is the auto-save when it exists (as the card grid's first box).
        sel_ = any_file ? static_cast<int>(first - rows_.begin()) : 0;
        top_ = std::clamp(sel_ - save_layout::kPageRows / 2, 0, save::kOcFiles + 1 - save_layout::kPageRows);
        phase_ = any_file ? Phase::Files : Phase::NoData;
      } else {
        entries_ = cards_[static_cast<size_t>(card_ - 1)];
        phase_ = entries_.empty() ? Phase::NoData : Phase::Files;
      }
    } else if (nav.cancel) {
      cancelled_ = true, play(6);
    }
    return;
  }
  // The message closes on confirm or after 120 frames.
  if (phase_ == Phase::NoData) {
    if (nav.confirm || phase_time_ > 120.0f / 60.0f) phase_ = Phase::Card, phase_time_ = 0;
    return;
  }
  auto refused = [&](const SlotEntry& entry) { return plus_ && game::ngplus::refusal(entry.data) != game::ngplus::Refusal::None; };
  // OpenCross page: up / down one row (wrap), left / right one screen; the list scrolls with the cursor.
  if (card_ == 0) {
    constexpr int kCount = save::kOcFiles + 1;
    const int before = sel_;
    if (nav.up) sel_ = (sel_ + kCount - 1) % kCount;
    if (nav.down) sel_ = (sel_ + 1) % kCount;
    if (nav.left) sel_ = std::max(0, sel_ - save_layout::kPageRows);
    if (nav.right) sel_ = std::min(kCount - 1, sel_ + save_layout::kPageRows);
    if (sel_ != before) play(1);
    top_ = std::clamp(top_, std::max(0, sel_ - save_layout::kPageRows + 1), sel_);
    if (nav.confirm) {
      const auto& row = rows_[static_cast<size_t>(sel_)];
      if (!row || refused(*row)) play(0x18);
      else picked_ = true, play(8);
    }
    if (nav.cancel) phase_ = Phase::Card, phase_time_ = 0, play(6);
    return;
  }
  const int cnt = static_cast<int>(entries_.size());
  if (cnt > 0) {
    // Left / right step through the files; up / down jump between the two rows of the 8 + 7 strip (box i = entry i).
    if (nav.left) sel_ = (sel_ + cnt - 1) % cnt, play(1);
    if (nav.right) sel_ = (sel_ + 1) % cnt, play(1);
    if (nav.up || nav.down) {
      const int want = sel_ < 8 ? sel_ + 8 : sel_ - 8;
      if (want >= 0 && want < cnt) sel_ = want, play(1);
    }
    if (nav.confirm) {
      // Continue+ (variant 2): a slot with summary flags != 0 is refused with sfx 0x18.
      if (refused(entries_[static_cast<size_t>(sel_)])) play(0x18);
      else picked_ = true, play(8);
    }
  } else if (nav.confirm) {
    play(0x18);
  }
  if (nav.cancel) phase_ = Phase::Card, phase_time_ = 0, play(6);
}

// Continue file select: the same screen as the in-game Load grid (layout: menu/save_layout.h). It draws no parchment; its backdrop
// is the static title-menu picture (bg0 / bg1), not the 3D title scene. Slot grid frame 82 at PS1 (66,84) with 15 boxes of
// 32 x 32 at (74 + 34 c, 98 + 34 r): one fixed face icon per box (menufaceicon/<set>/icon_NN.bmp; PS1: the file number); detail
// frame 84 at (84,230) with the three party faces (88 + 64 i, 242), name, star + count, Time, gold, location; message frame 86 at
// (138,320).
void SlotList::draw(DrawList& dl, UiKit& kit) const {
  namespace sl = save_layout;
  // Both art modes draw this screen; original art swaps the font files (small_font_*), the face folder hd/face/1999 and
  // hd/menufaceicon/1999.
  const bool classic = mode_ == ArtMode::Original;
  kit.set_classic(classic);
  const char* const face_set = classic ? "1999" : "2021";
  const int li = std::min(static_cast<int>(lang_), 5);
  const render::TexRef white_set = kit.white();
  const render::FaceId gnf = kit.face(mode_), kbf = kit.face_kb();
  // Labels: bn_70 (slot 4) for non-JP, kb_70 (slot 0) for JP; numbers kb_70. Placed by their ink box.
  const render::FaceId bnf = lang_ == import::Lang::Jp && kbf != render::kNoFace ? kbf : kit.face_bn();
  const bool ink = kit.has_bn() && kbf != render::kNoFace;
  auto canvas_x = [](float x) { return 2.0f * x; };
  auto canvas_y = [](float y) { return y * 960.0f / 448.0f; };
  const u32 white = pack_rgba(240, 240, 240), text_c = pack_rgba(206, 197, 197);
  // Fallback text with its top-left at PS1 (x, y) when the bn / kb fonts are missing (gn_70).
  auto txt = [&](float x, float y, const std::u32string& str, u32 col, float rel = 1.0f) {
    dl.text(gnf, str, canvas_x(x), canvas_y(y), col, 0.42f * rel);
  };
  // Text by ink box: x = ink left (or right edge with `right`), y = cap top, PS1 px; bn cap top 25.6 s below the line top, left
  // bearing 25.4 s; kb 32.3 s / 22.5 s (as main_menu.cpp HT).
  auto ink_text = [&](render::FaceId face_id, float x, float y, const std::u32string& str, u32 col, float sc, bool right = false) {
    if (str.empty()) return;
    const bool kb = face_id == kbf;
    const float px = right ? canvas_x(x) - dl.measure(face_id, str, sc) - 12.0f * sc : canvas_x(x) - (kb ? 22.5f : 25.4f) * sc;
    dl.text(face_id, str, px, canvas_y(y) - (kb ? 32.3f : 25.6f) * sc, col, sc);
  };
  auto label = [&](float x, float y, const std::u32string& str, float sc, float maxw = 0) {
    if (maxw > 0)
      while (sc > 0.18f && dl.measure(bnf, str, sc) > canvas_x(maxw)) sc -= 0.01f;
    ink_text(bnf, x, y, str, text_c, sc);
  };
  auto number = [&](float xr, float y, const std::string& str, float sc) {
    ink_text(kbf, xr, y, std::u32string(str.begin(), str.end()), text_c, sc, true);
  };
  // A mod / art switch re-points the VFS (Vfs::generation): drop the cached pictures, face list and text tables so the next draw reads the
  // new files; the old images are kept in stale_ to be refreshed in place when the new one has the same size.
  if (const std::uint32_t generation = kit.vfs().generation(); generation != vfs_gen_) {
    for (const auto& [key, cached] : pics_) stale_[key] = cached;
    pics_.clear();
    face_files_.clear();
    faces_listed_ = 0;
    tables_.reset();
    vfs_gen_ = generation;
  }
  auto store = [&](const std::string& key, u32 iw, u32 ih, const void* px, bool linear) -> NamePic {
    NamePic stored;
    if (iw && ih && px) {
      stored.w = static_cast<float>(iw), stored.h = static_cast<float>(ih);
      const auto it = stale_.find(key);
      if (it != stale_.end() && it->second.set && it->second.w == stored.w && it->second.h == stored.h &&
          kit.update_rgba(it->second.set, px))
        stored.set = it->second.set;
      else
        stored.set = kit.upload_rgba(iw, ih, px, linear);
    }
    pics_[key] = stored;
    return stored;
  };
  auto pic = [&](const std::string& path) -> NamePic {
    if (auto it = pics_.find(path); it != pics_.end()) return it->second;
    render::DecodedImage decoded;
    if (auto raw = kit.vfs().read(path); raw && render::decode_image(*raw, decoded))
      return store(path, decoded.width, decoded.height, decoded.rgba.data(), true);
    return store(path, 0, 0, nullptr, true);
  };
  auto ps1pic = [&](const std::string& key, auto&& make) -> NamePic {
    if (auto it = pics_.find(key); it != pics_.end()) return it->second;
    const ps1::Rgba im = make();
    return store(key, im.w, im.h, im ? im.px.data() : nullptr, false);
  };
  // Backdrop = the static title-menu picture: bg0 (640 x 256) over bg1 (640 x 192) =
  // one 640 x 448 screen, opaque, the same art as TitleScreen's menu phase (both art modes; no 3D title scene behind the list).
  {
    auto tim_pic = [&](const std::string& path) -> NamePic {
      if (auto it = pics_.find(path); it != pics_.end()) return it->second;
      if (auto raw = kit.vfs().read(path))
        if (auto tim = import::parse_tim(*raw))
          if (auto img = import::decode_tim(*tim)) {
            const import::Image rgba = import::to_rgba(*img);
            return store(path, rgba.width, rgba.height, rgba.pixels.data(), false);
          }
      return store(path, 0, 0, nullptr, false);
    };
    dl.rect(white_set, 0, 0, kCanvasW, kCanvasHeight, pack_rgba(0, 0, 0));
    const float ps1_scale = kCanvasHeight / 448.0f;
    const NamePic b0 = tim_pic("cdrom/menu/title/bg0.tim"), b1 = tim_pic("cdrom/menu/title/bg1.tim");
    if (b0.set) dl.image(b0.set, 0, 0, kCanvasW, 256 * ps1_scale);
    if (b1.set) dl.image(b1.set, 0, 256 * ps1_scale, kCanvasW, 192 * ps1_scale);
  }
  // Face avatar: hd/face/<2021 | 1999>/NN_name.png by art mode; cdrom/face/<name>.tim only when the PNG is missing.
  // Classic art + [Video] Ps1ResMenus: the list at 640 x 448 with the PS1 disc art where the PS1 has it: faces
  // cdrom/face/<stem>.tim, box icons menutim.prd p0ic..pfic.
  const bool ps1_art = classic && ps1_menus();
  kit.request_lowres(dl, kMenuWide);
  auto face_pic = [&](int id) -> NamePic {
    if (ps1_art) {
      const NamePic disc_face = ps1pic("ps1:face" + std::to_string(id), [&] { return ps1::face(kit.vfs(), id); });
      if (disc_face.set) return disc_face;
    }
    if (faces_listed_ != (classic ? 2 : 1)) {
      faces_listed_ = classic ? 2 : 1;
      face_files_.clear();
      const std::string pre = std::string("face/") + face_set + "/";
      for (const std::string& file : kit.vfs().list("hd"))
        if (file.rfind(pre, 0) == 0 && file.size() > 13 && std::isdigit(static_cast<unsigned char>(file[10])) &&
            std::isdigit(static_cast<unsigned char>(file[11])))
          face_files_[(file[10] - '0') * 10 + (file[11] - '0')] = "hd/" + file;
    }
    if (auto it = face_files_.find(id); it != face_files_.end()) return pic(it->second);
    return ps1pic("ps1:face" + std::to_string(id), [&] { return ps1::face(kit.vfs(), id); });
  };
  // Frames: the RDE shape (transparent interior) in both modes; the PS1 sheet pieces
  // (ps1::frame_pieces) only when the PNG is missing. `tab_x` / `tab_w` (PS1 px from the frame's left) cut that fallback's caption gap.
  auto frame = [&](int shape, float x, float y, float tab_x = 0, float tab_w = 0) {
    char path[80];
    std::snprintf(path, sizeof path, "hd/menu/shapes/menu-main-%03d%s.png", shape,
                  std::array<const char*, 6>{"_en", "_fr", "_it", "_de", "_es", "_jp"}[static_cast<size_t>(li)]);
    // Classic + Ps1Res uses the disc's "wind" pieces instead.
    NamePic shape_pic = ps1_art ? NamePic{} : kit.vfs().exists(path) ? pic(path) : NamePic{};
    if (!shape_pic.set && !ps1_art) {
      std::snprintf(path, sizeof path, "hd/menu/shapes/menu-main-%03d.png", shape);
      shape_pic = pic(path);
    }
    float fw = shape_pic.w / 4.0f, fh = shape_pic.h / 4.0f;
    // For the PS1 panel size the RDE shape still gives the extent.
    if (ps1_art) {
      std::snprintf(path, sizeof path, "hd/menu/shapes/menu-main-%03d.png", shape);
      const NamePic sz = pic(path);
      fw = sz.w / 4.0f, fh = sz.h / 4.0f;
    }
    if (shape_pic.set) {
      dl.image(shape_pic.set, canvas_x(x), canvas_y(y), canvas_x(fw), canvas_y(fh));
    } else {
      const NamePic wd = ps1pic("ps1:wind", [&] { return ps1::menutim(kit.vfs(), "wind"); });
      if (wd.set)
        ps1::frame_pieces(
            [&](float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh) {
              dl.image(wd.set, dx, dy, dw, dh, 0xFFFFFFFFu, sx / wd.w, sy / wd.h, (sx + sw) / wd.w, (sy + sh) / wd.h);
            },
            canvas_x(x), canvas_y(y), canvas_x(fw), canvas_y(fh), canvas_x(1), canvas_y(1), canvas_x(tab_x), canvas_x(tab_w));
    }
  };
  if (!tables_) tables_ = std::make_unique<import::LangTables>(), tables_->init(&kit.vfs());
  auto menu_str = [&](u32 addr, const char* fb) -> std::u32string {
    if (const std::string* text = tables_->menu_text(addr, lang_); text && !text->empty()) return import::utf8_to_u32(*text);
    return import::utf8_to_u32(fb);
  };
  // A cmnmes.txt caption keyed by its JP / internal name.
  auto cmn = [&](const char* key, const char* fb) -> std::u32string {
    if (const import::TextTable* table = tables_->table("cmnmes"))
      if (const std::string* caption = table->get(key, lang_); caption && !caption->empty()) return import::utf8_to_u32(*caption);
    return import::utf8_to_u32(fb);
  };

  // Pointer: menutest_cur.png (23 x 11) pulsing like the menu's; the PS1 red triangle when the sprite is missing.
  auto pointer = [&](float cx, float cy) {
    const int ph = static_cast<int>(clock_ * 60.0f) % 96;
    const int pulse = 0x90 + (ph < 48 ? ph * 2 : (95 - ph) * 2);
    const NamePic cursor = ps1_art ? NamePic{} : pic("hd/menu/menutest_cur.png");
    if (cursor.set) {
      const int red = std::min(255, (pulse >> 1) * 255 / 128), rest = std::min(255, ((pulse - 0x80) >> 1) * 255 / 128);
      dl.image(cursor.set, canvas_x(cx), canvas_y(cy), canvas_x(23), canvas_y(11), pack_rgba(static_cast<u8>(red), static_cast<u8>(rest), static_cast<u8>(rest)));
      return;
    }
    const int green = std::min(255, (pulse - 0x80) * 2);
    for (int i = 0; i < 12; ++i) {
      const float width_factor = 1.0f - std::fabs(static_cast<float>(i) + 0.5f - 6.0f) / 6.0f;
      dl.rect(white_set, canvas_x(cx), canvas_y(cy + static_cast<float>(i)), canvas_x(23.0f * width_factor), canvas_y(1) + 0.6f,
              pack_rgba(255, static_cast<u8>(green), static_cast<u8>(green)));
    }
  };
  // Save choice (no grid): message frame 86 with "Please choose a slot." and the lines OpenCross / Slot 1 / Slot 2 (save_layout
  // choice_y), the pointer beside the current one; an empty choice's "No save data in ..." replaces them until it closes.
  if (phase_ != Phase::Files) {
    frame(86, sl::kMsgX, sl::kMsgY);
    auto line = [&](float x, float y, const std::u32string& str) {
      if (ink) label(x + 1.5f, y + 2.5f, str, sl::kMsgScale, sl::kMsgWidth);
      else txt(x, y, str, white, 1.0f);
    };
    if (phase_ == Phase::NoData) {
      if (card_ == 0) line(146, 336, U"No save data in OpenCross.");
      else line(146, 336, menu_str(card_ == 1 ? 0x801F3181 : 0x801F3182, card_ == 1 ? "No save data in Slot 1." : "No save data in Slot 2."));
      return;
    }
    line(146, 336, menu_str(0x801F2CA4, "Please choose a slot."));
    line(sl::kChoiceX, sl::choice_y(0), U"OpenCross");
    line(sl::kChoiceX, sl::choice_y(1), menu_str(0x801F2CDC, "Slot 1"));
    line(sl::kChoiceX, sl::choice_y(2), menu_str(0x801F2CF8, "Slot 2"));
    pointer(sl::kChoiceCursorX, sl::choice_y(card_) + 1.0f);
    return;
  }

  // Location caption of a save: the savetbl row of its scenario key, split at the first newline.
  auto location_lines = [&](const save::SaveSlot& data) -> std::pair<std::u32string, std::u32string> {
    const int scenario = data.progression();
    std::string row;
    if (const import::TextTable* table = tables_->table("savetbl"))
      if (const std::string* found = table->get(static_cast<u64>(sl::location_key(scenario)), lang_)) row = *found;
    for (size_t pos; (pos = row.find('\r')) != std::string::npos;) row.erase(pos, 1);
    const size_t nl = row.find('\n');
    if (nl == std::string::npos) return {import::utf8_to_u32(row), {}};
    return {import::utf8_to_u32(row.substr(0, nl)), import::utf8_to_u32(row.substr(nl + 1))};
  };
  const SlotEntry* cur = nullptr;
  if (card_ == 0) {
    // ---- OpenCross page: frame 64 with the caption in its tab, kPageRows rows from top_ (save_layout.h)
    cur = rows_[static_cast<size_t>(sel_)] ? &*rows_[static_cast<size_t>(sel_)] : nullptr;
    frame(64, sl::kPageX, sl::kPageY, 8, 53);
    const float cap_x = sl::kPageX + (sl::kSlotCapX - sl::kGridX);
    float page_cap_scale = sl::kSlotCapScale;
    while (page_cap_scale > 0.18f && dl.measure(kit.face_bn(), U"OpenCross", page_cap_scale) > canvas_x(sl::kPageCapMaxW)) page_cap_scale -= 0.01f;
    if (ink) ink_text(kit.face_bn(), cap_x, sl::caption_y(sl::kPageY, page_cap_scale), U"OpenCross", text_c, page_cap_scale);
    else txt(cap_x, sl::kPageY - 4.0f, U"OpenCross", white, 0.55f);
    const u32 sel_c = pack_rgba(192, 120, 0), dim_c = pack_rgba(140, 132, 132);
    const std::u32string empty_text = menu_str(0x801F2E78, "EMPTY");
    // Leader (auto-save caption on Auto) and first location line of every file; one row scale for the whole page.
    std::vector<std::pair<std::u32string, std::u32string>> labels(rows_.size());
    float row_scale = sl::kRowScale;
    const float name_w = sl::kRowLocX - sl::kRowNameX - 4.0f;
    for (size_t file_no = 0; file_no < rows_.size(); ++file_no) {
      if (!rows_[file_no]) continue;
      const save::SaveSlot& data = rows_[file_no]->data;
      const auto party = data.party();
      std::u32string name = file_no == 0 ? import::utf8_to_u32(sl::auto_save_caption(li))
                                        : party[0] < save::kNumChars && name_of_ ? name_of_(data, rows_[file_no]->ext_names, party[0]) : std::u32string();
      std::replace(name.begin(), name.end(), U'\n', U' ');
      labels[file_no] = {std::move(name), sl::row_location(location_lines(data).first)};
      while (ink && row_scale > 0.18f && (dl.measure(bnf, labels[file_no].first, row_scale) > canvas_x(name_w) ||
                                          dl.measure(bnf, labels[file_no].second, row_scale) > canvas_x(sl::kRowLocW)))
        row_scale -= 0.01f;
    }
    auto row_text = [&](render::FaceId face_id, float x, float y, const std::u32string& str, u32 col, bool right = false) {
      if (!ink) {
        txt(right ? x - dl.measure(gnf, str, 0.6f) / 2.0f : x, y - 2.0f, str, col, 0.6f);
        return;
      }
      ink_text(face_id, x, y, str, col, row_scale, right);
    };
    for (int i = 0; i < sl::kPageRows; ++i) {
      const int row_no = top_ + i;
      if (row_no > save::kOcFiles) break;
      const float y = sl::row_y(i);
      const auto& row = rows_[static_cast<size_t>(row_no)];
      const u32 col = row_no == sel_ ? sel_c : text_c;
      if (row_no == sel_) pointer(sl::kRowCursorX, y - 1.5f);
      row_text(bnf, sl::kRowNumX, y, import::utf8_to_u32(sl::page_number(row_no)), col);
      if (!row) {
        row_text(bnf, sl::kRowNameX, y, empty_text, row_no == sel_ ? sel_c : dim_c);
        continue;
      }
      const save::SaveSlot& data = row->data;
      row_text(bnf, sl::kRowNameX, y, labels[static_cast<size_t>(row_no)].first, col);
      row_text(bnf, sl::kRowLocX, y, labels[static_cast<size_t>(row_no)].second, col);
      const std::string time = sl::playtime(data.playtime_frames());
      row_text(ink ? kbf : gnf, sl::kRowTimeRight, y, std::u32string(time.begin(), time.end()), col, true);
      const u32 star_c = (data.summary_flags() & 1) ? sl::cleared_star_colour(col) : col;
      const std::string stars = std::to_string(std::min<int>(data.stars_total(), 99));
      row_text(ink ? kbf : gnf, sl::kRowStarX, y, U"\u2605", star_c);
      row_text(ink ? kbf : gnf, sl::kRowStarsX, y, std::u32string(stars.begin(), stars.end()), star_c);
    }
    // Scroll marks: small triangles at the top / bottom right while rows lie above / below.
    auto mark = [&](float y, bool up) {
      for (int i = 0; i < 5; ++i) {
        const float half = up ? static_cast<float>(i) : static_cast<float>(4 - i);
        dl.rect(white_set, canvas_x(sl::kRowMarkX - half), canvas_y(y + static_cast<float>(i)), canvas_x(2.0f * half + 1.0f), canvas_y(1) + 0.6f, text_c);
      }
    };
    if (top_ > 0) mark(sl::kRowY0 - 2.0f, true);
    if (top_ + sl::kPageRows <= save::kOcFiles) mark(sl::row_y(sl::kPageRows - 1) + 10.0f, false);
  } else {
  // Indexed by BOX: the k-th file in directory order.
  std::array<const SlotEntry*, 15> by_slot{};
  for (size_t i = 0; i < entries_.size() && i < by_slot.size(); ++i) by_slot[i] = &entries_[i];
  cur = entries_.empty() ? nullptr : &entries_[static_cast<size_t>(sel_)];

  // ---- slot grid
  frame(82 + card_ - 1, sl::kGridX, sl::kGridY, 8, 40);
  {
    // Caption tab (memory-card slot), bn font in every language.
    const std::u32string caption = cmn("slot", "Slot") + (card_ == 1 ? U" 1" : U" 2");
    if (ink) ink_text(kit.face_bn(), sl::kSlotCapX, sl::kSlotCapY, caption, text_c, sl::kSlotCapScale);
    else txt(sl::kSlotCapX, sl::kGridY - 4.0f, caption, white, 0.55f);
  }
  for (int box = 0; box < 15; ++box) {
    const float x = sl::box_x(box), y = sl::box_y(box);
    if (by_slot[static_cast<size_t>(box)]) {
      int max_n = 0;
      for (const SlotEntry& entry : entries_) max_n = std::max(max_n, entry.slot + 1);
      // The PS1 draws file_no + 1; fn is the face icon (save_layout.h).
      const int file_no = by_slot[static_cast<size_t>(box)]->slot;
      const int fn = sl::box_icon(box, file_no, cur ? sel_ : -1, max_n);
      const NamePic p1 = ps1_art ? ps1pic("ps1:icon" + std::to_string(file_no), [&] { return ps1::slot_icon(kit.vfs(), file_no); }) : NamePic{};
      const NamePic ic = p1.set ? p1 : pic(sl::slot_icon_path(face_set, fn));
      // PS1 icon: texels (0, 0)..(15, 15) of the 16 x 16 icon on the 32 x 32 box.
      if (p1.set) {
        dl.image(ic.set, canvas_x(x), canvas_y(y), canvas_x(sl::kBox), canvas_y(sl::kBox), 0xFFFFFFFFu, 0, 0, ps1::kSlotIconUv, ps1::kSlotIconUv);
      } else if (ic.set) {
        dl.image(ic.set, canvas_x(x), canvas_y(y), canvas_x(sl::kBox), canvas_y(sl::kBox));
      } else {
        // Icon missing: the PS1's file number.
        const std::u32string number_text = import::utf8_to_u32(std::to_string(file_no + 1));
        dl.text(gnf, number_text, canvas_x(x + sl::kBox * 0.5f) - dl.measure(gnf, number_text, 0.42f) * 0.5f, canvas_y(y + 8.0f), white, 0.42f);
      }
    }
    // Cursor: white outline.
    if (cur && sel_ == box) {
      dl.rect(white_set, canvas_x(x - 1), canvas_y(y - 1), canvas_x(sl::kBox + 1), 2, white);
      dl.rect(white_set, canvas_x(x - 1), canvas_y(y + sl::kBox), canvas_x(sl::kBox + 1), 2, white);
      dl.rect(white_set, canvas_x(x - 1), canvas_y(y - 1), 2, canvas_y(sl::kBox + 1), white);
      dl.rect(white_set, canvas_x(x + sl::kBox), canvas_y(y - 1), 2, canvas_y(sl::kBox + 1), white);
    }
  }
  }
  // ---- detail frame + caption
  frame(84, sl::kDetailX, sl::kDetailY, 30, 34);
  if (cur || card_ == 0) {
    // Caption: the box FILE's number + 1 (used boxes only; page: the row's number), bn font in every language.
    const std::u32string caption =
        cmn("File#", "File#") + import::utf8_to_u32(card_ == 0 ? sl::page_caption(sel_) : sl::file_number(cur->slot));
    float cap_scale = sl::kFileCapScale;
    auto fits = [&](const std::u32string& str) { return dl.measure(kit.face_bn(), str, cap_scale) <= canvas_x(sl::kFileCapMaxW); };
    // Page: one size for every number (the widest caption's).
    while (cap_scale > 0.18f && !(fits(caption) && (card_ != 0 || (fits(cmn("File#", "File#") + import::utf8_to_u32(sl::kPageCaptionWidest[0])) &&
                                                                     fits(cmn("File#", "File#") + import::utf8_to_u32(sl::kPageCaptionWidest[1]))))))
      cap_scale -= 0.01f;
    const float cap_y = card_ == 0 ? sl::caption_y(sl::kDetailY, cap_scale) : sl::kFileCapY;
    if (ink) ink_text(kit.face_bn(), sl::kFileCapX, cap_y, caption, text_c, cap_scale);
    else txt(sl::kFileCapX, sl::kDetailY - 4.0f, caption, white, 0.5f);
  }
  // ---- message frame
  frame(86, sl::kMsgX, sl::kMsgY);
  {
    const bool none = card_ != 0 && entries_.empty();
    const std::u32string message = menu_str(none ? 0x801F2F1C : 0x801F2E08, none ? "No Data" : "Please select a file.");
    if (ink) label(sl::kMsgInkX, sl::kMsgInkY, message, sl::kMsgScale, sl::kMsgWidth);
    else txt(146, 336, message, white, 1.0f);
  }
  // Continue+ (variant 2): a refused slot shows no details, but its menutest message in the DETAIL frame: message 7 / 8 (flag bit 0,
  // "This game data has been completed.") or 5 / 6 (bit 1, "... continue the game in a strong state.") at PS1 (0x5C, 0xF6) /
  // (0x11C, 0xF6), as an empty file's messages 2 / 3; menutest.txt carries each sentence on the first half (the second is empty).
  // The bottom frame keeps "Please select a file." (messages 0 / 1).
  const game::ngplus::Refusal refused =
      plus_ && cur ? game::ngplus::refusal(cur->data) : game::ngplus::Refusal::None;
  if (refused != game::ngplus::Refusal::None) {
    const bool done = refused == game::ngplus::Refusal::Completed;
    const std::u32string message = menu_str(done ? 0x801F2F74 : 0x801F2F38, done ? "This game data has been completed."
                                                                                  : "This data lets you continue the game in a strong state.");
    if (ink) label(92.0f + (sl::kMsgInkX - 146.0f), 246.0f + (sl::kMsgInkY - 336.0f), message, sl::kMsgScale, 440.0f);
    else txt(92, 246, message, white, 1.0f);
  }
  if (!cur || refused != game::ngplus::Refusal::None) return;
  const save::SaveSlot& save_data = cur->data;
  const auto party = save_data.party();
  for (size_t i = 0; i < 3 && i < party.size(); ++i) {
    if (party[i] >= save::kNumChars) continue;
    const NamePic fp = face_pic(party[i]);
    const float fx = sl::kFaceX + sl::kFaceStep * static_cast<float>(i);
    if (fp.set) dl.image(fp.set, canvas_x(fx), canvas_y(sl::kFaceY), canvas_x(sl::kFace), canvas_y(sl::kFace));
    else txt(fx + 2.0f, 262, name_of_ ? name_of_(save_data, cur->ext_names, party[i]) : std::u32string(), white, 0.6f);
  }
  const std::u32string leader = !party.empty() && party[0] < save::kNumChars && name_of_ ? name_of_(save_data, cur->ext_names, party[0]) : std::u32string();
  // Card 1's CHRONO00 and the page's file 0 are the auto-saves.
  const bool auto_file = cur->slot == 0 && cur->card <= 1;
  const std::u32string name = auto_file ? import::utf8_to_u32(sl::auto_save_caption(li)) : leader;
  const std::string stars = std::to_string(std::min<int>(save_data.stars_total(), 99)), tb = sl::playtime(save_data.playtime_frames());
  std::string gold;
  {
    const u32 gold_amount = save_data.gold();
    char buffer[24];
    if (gold_amount >= 1000000)
      std::snprintf(buffer, sizeof buffer, "%u,%03u,%03u", gold_amount / 1000000, gold_amount / 1000 % 1000, gold_amount % 1000);
    else if (gold_amount >= 1000) std::snprintf(buffer, sizeof buffer, "%u,%03u", gold_amount / 1000, gold_amount % 1000);
    else std::snprintf(buffer, sizeof buffer, "%u", gold_amount);
    gold = buffer;
  }
  std::u32string pl = cmn("PLAY", "Time");
  // A cleared save (summary flag bit 0) prints its star and count in yellow (0x108080 instead of 0x808080).
  const u32 star_c = (save_data.summary_flags() & 1) ? sl::cleared_star_colour(text_c) : text_c;
  const auto [l1, l2] = location_lines(save_data);
  if (ink) {
    {
      // Name / auto-save caption (save_layout.h name_fit): shrink to 97, two lines for the auto-save outside EN / JP.
      const sl::NameFit nf = sl::name_fit(dl.measure(bnf, name, 0.35f), auto_file, li);
      size_t line_start = 0;
      for (int line = 0;; ++line) {
        const size_t line_end = name.find(U'\n', line_start);
        label(sl::kNameX, sl::kNameY + (nf.dy + nf.pitch * static_cast<float>(line)) * (448.0f / 960.0f),
              name.substr(line_start, line_end == std::u32string::npos ? line_end : line_end - line_start), sl::kNameScale * nf.mul);
        if (line_end == std::u32string::npos) break;
        line_start = line_end + 1;
      }
    }
    {
      // The star is the kb_70 glyph "★" (slot 0, mul 0.625, H3), pen at list (kStarX, kStarY).
      const font::SlotInfo& k0 = font::slot(font::kKb);
      const float sc = k0.scale * 0.625f;
      dl.text(kbf, U"★", canvas_x(sl::kStarX) + k0.off_x * sc, canvas_y(sl::kStarY) + k0.off_y * sc, star_c, sc);
    }
    ink_text(kbf, sl::kStarsX, sl::kStarsY, std::u32string(stars.begin(), stars.end()), star_c, sl::kNumScale);
    label(sl::kTimeLabelX, sl::kTimeLabelY, pl, sl::kSmallScale);
    number(sl::kTimeRight, sl::kTimeY, tb, sl::kNumScale);
    number(sl::kGoldRight, sl::kGoldY, gold, sl::kNumScale);
    label(sl::kGoldUnitX, sl::kGoldY, U"G", sl::kGoldUnitScale);
    label(sl::kLocX, sl::kLocY1, l1, sl::kLocScale, 250);
    label(sl::kLocX, sl::kLocY2, l2, sl::kLocScale, 250);
    return;
  }
  txt(284, 246, name, white, 0.85f);
  txt(388, 248, U"★", (save_data.summary_flags() & 1) ? sl::cleared_star_colour(white) : white, 0.9f);
  txt(402, 249, import::utf8_to_u32(stars), (save_data.summary_flags() & 1) ? sl::cleared_star_colour(white) : white, 0.9f);
  txt(437, 243, pl, white, 0.62f);
  txt(469, 244, import::utf8_to_u32(tb), white, 0.9f);
  txt(532.0f - 8.0f * static_cast<float>(gold.size()), 258, import::utf8_to_u32(gold), white, 0.9f);
  txt(532, 256, U"G", white, 0.9f);
  txt(284, 278, l1, white, 0.7f);
  txt(284, 294, l2, white, 0.7f);
}

}  // namespace oc::ui
