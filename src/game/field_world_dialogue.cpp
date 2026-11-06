// FieldWorld: the dialogue windows (placement, sizing, text flags and results) and the menu hand-off.
#include "game/field_world.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <limits>

#include "audio/voice.h"
#include "core/log.h"
#include "data/char_name.h"
#include "game/new_game_plus.h"

namespace oc::game {

void FieldWorld::setup_dialog_window(int ent, std::span<const s32> v5) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size()) || v5.size() < 5) return;
  for (size_t i = 0; i < 5; ++i) ents_[static_cast<size_t>(ent)].win[i] = v5[i];
}
void FieldWorld::set_dialog_face(int face) {
  // The portrait belongs to the entity whose script sets it (entity 9 sets it once in INIT and talks later).
  const int runner = vm_.current_entity();
  if (runner >= 0 && runner < static_cast<int>(ents_.size())) ents_[static_cast<size_t>(runner)].face = face;
  dialogue_face_ = face;
  log::debug("field: ent {} SET_DIALOG_FACE {}", runner, face);
}
// Message windows. The engine keeps up to 4, each owned by the entity whose script opened it (the running
// entity): E0 / E3 fail while that entity's own window is open or none is free, and E2
// waits for the running entity's own window. dem_11's tombstone scene runs three label windows at once (entities 6, 7,
// 8 show the lines of the inscription while a {fset}/{fwait} handshake on variable 0x22 closes them together). Window 0 is dialogue_,
// extra_win_ the other three (created on first use).
ui::DialogueDemo* FieldWorld::window_owned_by(int ent) {
  if (dialogue_.is_open() && dialogue_owner_ == ent) return &dialogue_;
  for (auto& window : extra_win_)
    if (window.box && window.box->is_open() && window.owner == ent) return window.box.get();
  return nullptr;
}

// The confirm button on a window: a confirmed choice line goes into the owning entity's dialogue result byte at once (E2 copies
// it to variable 0x04 once the window is gone), so it does not depend on which window E2 later finds.
void FieldWorld::confirm_window(ui::DialogueDemo& box, int owner) {
  const int before = box.chosen();
  box.confirm();
  const int line = box.chosen();
  if (line < 0 || line == before || owner < 0 || owner >= static_cast<int>(vm_.entity_count())) return;
  vm_.entity(owner).last_choice = static_cast<u8>(line);
}

bool FieldWorld::any_window_open() const {
  if (dialogue_.is_open()) return true;
  for (const auto& window : extra_win_)
    if (window.box && window.box->is_open()) return true;
  return false;
}

bool FieldWorld::modal_window_open() const {
  if (dialogue_.is_open() && !label_) return true;
  for (const auto& window : extra_win_)
    if (window.box && window.box->is_open() && !window.label) return true;
  return false;
}

void FieldWorld::close_windows() {
  dialogue_.close();
  for (auto& window : extra_win_)
    if (window.box) window.box->close();
}

bool FieldWorld::dialogue_open() { return window_owned_by(vm_.current_entity()) != nullptr; }

bool FieldWorld::drop_culled_dialogue() {
  const int runner = vm_.current_entity();
  ui::DialogueDemo* box = window_owned_by(runner);
  if (!box) return false;
  bool kept = box == &dialogue_ && dialogue_kept_;
  for (const auto& window : extra_win_)
    if (window.box.get() == box) kept = window.kept;
  if (kept) return false;
  log::debug("field: tick {} ent {} is off screen at E2: its window closes and the script ends", tick_count_, runner);
  box->close();
  return true;
}

int FieldWorld::other_window_below(const ui::DialogueDemo& self) const {
  int result = -1;
  auto look = [&](const ui::DialogueDemo& box, bool below) {
    if (&box == &self || !box.is_open()) return;
    if (below) result = 1;
    else if (result < 0) result = 0;
  };
  look(dialogue_, dialogue_below_);
  for (const auto& window : extra_win_)
    if (window.box) look(*window.box, window.below);
  return result;
}

bool FieldWorld::dialogue_blocked() {
  if (window_owned_by(vm_.current_entity())) return true;
  if (!dialogue_.is_open()) return false;
  for (const auto& window : extra_win_)
    if (!window.box || !window.box->is_open()) return false;
  return true;
}

void FieldWorld::open_dialogue(int speaker, int message, int flag_byte) {
  // Which window: window 0 when it is free, else the first free extra one (created and wired like window 0 on first use).
  ui::DialogueDemo* box = &dialogue_;
  int win_slot = 0;
  if (dialogue_.is_open()) {
    box = nullptr;
    for (size_t i = 0; i < extra_win_.size() && !box; ++i) {
      ExtraWindow& window = extra_win_[i];
      if (window.box && window.box->is_open()) continue;
      if (!window.box) {
        auto fresh_box = std::make_unique<ui::DialogueDemo>();
        if (!fresh_box->init(*ctx_, *files_, *sprites_, lang_)) continue;
        fresh_box->show_status(false);
        fresh_box->set_portrait_set(portrait_set_);
        fresh_box->set_mode(hd_ ? ui::ArtMode::Hd : ui::ArtMode::Original);
        window.box = std::move(fresh_box);
        install_name_hooks();
      }
      box = window.box.get();
      win_slot = static_cast<int>(i) + 1;
    }
    if (!box) return;
  }
  ui::DialogueDemo& dlg = *box;
  int speaker_id = 0;
  if (speaker >= 0 && speaker < static_cast<int>(ents_.size())) speaker_id = std::max(0, ents_[static_cast<size_t>(speaker)].char_id);
  // Portrait: the speaking entity's face (party members: their character id; others only after
  // SET_DIALOG_FACE; none = no portrait) is the face id. Window flag
  // bit 0 = portrait present, unless the dialog op's flag byte has 0x40.
  const int target = speaker >= 0 && speaker < static_cast<int>(ents_.size()) ? speaker : vm_.current_entity();
  {
    int face = -1;
    if (target >= 0 && target < static_cast<int>(ents_.size())) {
      face = ents_[static_cast<size_t>(target)].face;
      if (face < 0)
        for (int slot = 0; slot < 3 && slot < static_cast<int>(party_chars_.size()); ++slot)
          if (party_ent_[static_cast<size_t>(slot)] == target) face = party_chars_[static_cast<size_t>(slot)];
    }
    // Faces that follow the story: Pip's is his current form (his record's model byte 0x2B..0x30), Kid's is her alternate face while
    // global bit 18 (FE 71) is set, Serge's is Lynx's (0x31) while his model byte is Lynx's body unless global bit 17 (FE 6C) is set.
    if (gs_) {
      const u32 global_flags = vm_.global_flags();
      const int pip_form = gs_->slot.chars[43].model_id & 0xFF;
      if (face == 43 && pip_form >= 0x2B && pip_form <= 0x30) face = pip_form;
      if (face == 1 && (global_flags >> 18 & 1u)) face = 0x5B;
      if (face == 0 && !(global_flags >> 17 & 1u) && (gs_->slot.chars[0].model_id & 0xFF) == 0x31) face = 0x31;
    }
    dialogue_face_ = face;
    portrait_left_ = false;
    if (face >= 0) {
      // The heading is the one scripts set and read (SET_ROTATION / GET_ROTATION), the model yaw is axis 1 (op 28 / AA). The control
      // yaw that turns the pad (op 63) does not enter.
      const int model_yaw = static_cast<s16>(vm_.entity(target).axis[1]);
      portrait_left_ = portrait_on_left(get_facing(target), portrait_yaw_, model_yaw, static_cast<u32>(flags_hi_for(flag_byte)));
    }
  }
  bool lbl = false, kept = false;
  {
    const int runner = vm_.current_entity();
    const int wf = runner >= 0 && runner < static_cast<int>(ents_.size()) ? ents_[static_cast<size_t>(runner)].win[4] : 0;
    lbl = (wf & 0x10) != 0;
    kept = (wf & 0x400) != 0;
  }
  if (win_slot == 0) {
    label_ = lbl;
    dialogue_kept_ = kept;
    dialogue_owner_ = vm_.current_entity();
  } else {
    extra_win_[static_cast<size_t>(win_slot - 1)].label = lbl;
    extra_win_[static_cast<size_t>(win_slot - 1)].kept = kept;
    extra_win_[static_cast<size_t>(win_slot - 1)].owner = vm_.current_entity();
  }
  dlg.set_label(lbl);
  dlg.set_portrait(lbl ? -1 : dialogue_face_);
  last_dialogue_msg_ = message;
  choice_pending_ = false;
  log::debug("field: tick {} dialogue room {} message {} (running ent {}, speaker ent {}, face {}, window {}, portrait {})", tick_count_, room_id_,
             message, vm_.current_entity(), speaker, dialogue_face_, win_slot, dialogue_face_ < 0 || lbl ? "none" : portrait_left_ ? "left" : "right");
  dlg.set_skin_byte(gs_->option(ui::opt::kCustomWindow) ? gs_->skin : 0);
  // The art mode changed while the previous window was open (set_art_mode).
  if (dialogue_mode_pending_) {
    dialogue_mode_pending_ = false;
    dlg.set_mode(hd_ ? ui::ArtMode::Hd : ui::ArtMode::Original);
  }
  // Voice lines (CrossFix layout).
  if (audio::VoicePlayer* vp = audio::VoicePlayer::instance()) {
    dlg.on_page = [vp, &dlg](int room, int msg, int sp, int tag, int page) { vp->field_page(&dlg, room, msg, sp, tag, page); };
    dlg.on_close = [vp, &dlg] { vp->field_close(&dlg); };
  }
  const bool lynx_in_party = std::any_of(party_chars_.begin(), party_chars_.end(), [this](int char_id) { return model_byte(char_id) == data::kLynxBody; });
  dlg.show(room_id_, message, speaker_id, data::line_speaker(speaker_id, lynx_in_party));
  // The portrait panel exists only when the window flags have bit 0; the face is the
  // speaker (name tag of the line / script speaker) unless SET_DIALOG_FACE chose one; (flags & 0x1001) == 1 = left.
  dlg.set_portrait_flags(true, portrait_left_);
  dlg.set_portrait(lbl ? -1 : dialogue_face_);
  // E0 passes no entity: it speaks as the running entity, which also places the window (labels too).
  const bool below = place_dialogue_window(dlg, speaker >= 0 ? speaker : target, message, flag_byte, !lbl && dialogue_face_ >= 0);
  if (win_slot == 0) dialogue_below_ = below;
  else extra_win_[static_cast<size_t>(win_slot - 1)].below = below;
}

// Upper byte of the window flags of the running entity (op 62); the dialog op's own flag byte only replaces the low byte.
int FieldWorld::flags_hi_for(int) const {
  const int runner = vm_.current_entity();
  const int wf = runner >= 0 && runner < static_cast<int>(ents_.size()) ? ents_[static_cast<size_t>(runner)].win[4] : 0;
  return wf & 0xFF00;
}

bool FieldWorld::portrait_on_left(int heading, int portrait_yaw, int model_yaw, u32 window_flags) {
  if (window_flags & 0x800u) return false;
  if (window_flags & 0x1000u) return true;
  const u32 yaw_delta = static_cast<u32>((heading + portrait_yaw + model_yaw) & 0xFFF) - 0x401u;
  return yaw_delta >= 0x800u;
}

// PS1 text width of a dialogue message in pixels: glyphs below 0x75 are 8 wide, the rest 12; the widest line wins. Names, items,
// the speaker's grammar forms (0x12) and button names are measured as the strings the PS1 substitutes (its executable tables and
// the save's names); a numeric field (0x0A / 0x0B) counts as 0x48 / 0x30, a digit entry (0x10) as -0x50.
int FieldWorld::ps1_message_width(std::span<const u8> bytes, int speaker) const {
  const u32 global_flags = vm_.global_flags();
  auto reads_as_lynx = [&](int char_id) { return model_byte(char_id) == data::kLynxBody && !(global_flags & script::gflag::kKeepCustomName); };
  auto var = [&](int arg) { return static_cast<int>(vm_.var(static_cast<u16>(arg * 2 + 0x1A))); };
  int widest = 0, width = 0;
  size_t i = 0;
  while (i < bytes.size()) {
    const u8 code = bytes[i];
    if (code >= 0x20) {
      width += code < 0x75 ? 8 : 12;
      i += code > 0xF7 ? 2 : 1;
      continue;
    }
    const int arg = i + 1 < bytes.size() ? bytes[i + 1] : 0;
    switch (code) {
      case 0:
      case 2:
        width += 12;
        [[fallthrough]];
      case 1:
        widest = std::max(widest, width);
        width = 0;
        i += 1;
        break;
      case 3: case 0x1F: i += 1; break;
      case 4: case 5: case 0xE: case 0xF: case 0x16: case 0x17: i += 2; break;
      case 6: widest = std::max(widest, width); width = 0; i += 2; break;
      case 7:
        width += ps1_string_width(0, arg | (i + 2 < bytes.size() ? bytes[i + 2] << 8 : 0));
        i += 3;
        break;
      case 8: width += ps1_string_width(1, arg); i += 2; break;
      case 9: i += 3; break;
      case 0xA: width += 0x48; i += 2; break;
      case 0xB: width += 0x30; i += 2; break;
      case 0x10: width -= 0x50; i += 2; break;
      case 0x12: {
        const int speaker_char = speaker >= 0 && speaker < static_cast<int>(ents_.size()) ? std::max(0, ents_[static_cast<size_t>(speaker)].char_id) : 0;
        width += ps1_string_width(arg + 1, speaker_char);
        i += 2;
        break;
      }
      case 0x15: {
        int char_id = var(arg);
        char_id = char_id >= 0x2C ? 0 : reads_as_lynx(char_id) ? data::kLynxBody : char_id;
        width += ps1_string_width(1, char_id) + (char_id == 5 ? 0x18 : 0);
        i += 2;
        break;
      }
      case 0x18: {
        const int member = arg < static_cast<int>(party_chars_.size()) ? party_chars_[static_cast<size_t>(arg)] : -1;
        const int char_id = member < 0 ? 0 : member;
        if (reads_as_lynx(char_id)) width += ps1_string_width(1, data::kLynxBody);
        else width += ps1_string_width(1, char_id) + (char_id == 5 ? 0x18 : 0);
        i += 2;
        break;
      }
      case 0x19: width += ps1_string_width(0, var(arg)); i += 2; break;
      case 0x1B: {
        // The button the field configuration puts on that function; with Controller: Default (option 0x100 clear) the function itself.
        const bool custom = gs_ && gs_->option(ui::opt::kControllerEdit);
        const int button = custom ? static_cast<int>(gs_->slot.button_cfg[0] >> ((arg & 7) * 4) & 0xF) : arg & 7;
        width += ps1_string_width(27, button);
        i += 2;
        break;
      }
      default: i += 1; break;
    }
  }
  return std::max(widest, width);
}

// Width of a substituted string: table 0 ids 0x200.. are elements / techs (table 23), 0x400.. key items (24); character names
// (table 1) below 0x2C are the save's names, 0x80 + n is the first n glyphs of Serge's name (Lynx's while his body is Lynx's and
// GF.16 is clear; 0x83 = 2 glyphs of name 0x0D), other ids read as Lynx's. Inside, glyphs count as in a message, a nested name
// (code 8) adds its width, and the first other control code ends it.
int FieldWorld::ps1_string_width(int table, int id) const {
  if (table == 0 && id >= 0x200) {
    table = id < 0x400 ? 23 : 24;
    id -= id < 0x400 ? 0x200 : 0x400;
  }
  std::span<const u8> text;
  size_t glyph_limit = std::numeric_limits<size_t>::max();
  if (table == 1 && (id & 0x80)) {
    const bool lynx = model_byte(0) == data::kLynxBody && !(vm_.global_flags() & script::gflag::kKeepCustomName);
    glyph_limit = id == 0x83 ? 2 : static_cast<size_t>(id - 0x80);
    id = id == 0x83 ? 0xD : lynx ? data::kLynxBody : 0;
  }
  if (table == 1 && id < 0x2C && gs_) {
    const auto& name = gs_->slot.chars[static_cast<size_t>(id)].name;
    text = std::span<const u8>(name.data(), static_cast<size_t>(std::find(name.begin(), name.end(), u8{0}) - name.begin()));
  } else {
    text = exe_strings_.get(table, table == 1 && id >= 0x2C ? data::kLynxBody : id);
  }
  int width = 0;
  size_t glyphs = 0;
  for (size_t i = 0; i < text.size() && glyphs < glyph_limit;) {
    const u8 code = text[i];
    if (code >= 0x20) {
      width += code < 0x75 ? 8 : 12;
      i += code > 0xF7 ? 2 : 1;
      ++glyphs;
    } else if (code == 4 || code == 5 || code == 0xE || code == 0xF || code == 0x16 || code == 0x17) {
      i += 2;
    } else if (code == 8 && i + 1 < text.size()) {
      width += ps1_string_width(1, text[i + 1]);
      i += 2;
    } else {
      break;
    }
  }
  return width;
}

// Where the window goes: PS1 pixels, top-left (x, y) of the text area; the box maps it to the canvas
// (PSX2 = 2 * PS1, canvas x = 2 * PSX2, canvas y = PSX2 * 960 / 432).
bool FieldWorld::place_dialogue_window(ui::DialogueDemo& dlg, int speaker, int message, int flag_byte, bool portrait) {
  const int runner = vm_.current_entity();
  if (runner < 0 || runner >= static_cast<int>(ents_.size())) return false;
  const std::array<int, 5>& win = ents_[static_cast<size_t>(runner)].win;
  const int base = win[4];
  const int lo = flag_byte ? flag_byte : (base & 0xFF);
  const int fl = lo | (base & 0xFF00);
  int lines = 2, width = 0xF0;
  if (const assets::RoomScene* sc = view_.scene(); sc && !sc->dialogue_bytes.empty()) {
    if (auto blk = import::parse_dialogue_block(ByteSpan(sc->dialogue_bytes));
        blk && message >= 0 && message < static_cast<int>(blk->records.size())) {
      const auto& record = blk->records[static_cast<size_t>(message)];
      lines = record.lines;
      width = ps1_message_width(std::span<const u8>(record.bytes.data(), record.bytes.size()), speaker);
    }
  }
  if (lo & 2) {
    lines = 4;
    width = 0xF0;
  }
  if (win[2] != 0) width = win[2] * 12;
  if (win[3] != 0) lines = win[3];
  if (!(fl & 0x40) && portrait) {
    lines = 4;
    if (!(lo & 2)) width += 0x40;
  }
  const int box_height = lines * 13;
  const bool have_actor = speaker >= 0 && speaker < static_cast<int>(ents_.size());
  const float scx = static_cast<float>(view_.scroll_x()), scy = static_cast<float>(view_.scroll_y());
  auto proj = [&](float yoff, float& sx, float& sy) {
    sx = 160.0f;
    sy = 120.0f;
    if (!have_actor) return;
    const ActorPose& pose = ents_[static_cast<size_t>(speaker)].actor.current();
    render::FieldCamera::Projected pr;
    if (view_.camera().project({pose.x, pose.y + yoff, pose.z}, pr)) {
      sx = pr.sx + scx;
      sy = pr.sy + scy;
    }
  };
  float mx, my, fx, fy, tx, ty;
  const float hgt = have_actor ? ent_height(speaker) : 0.0f;
  proj(-hgt / 2.0f, mx, my);
  int x = static_cast<int>(mx) - width / 2;
  int y = static_cast<int>(my) - box_height / 2;
  const int mode = (fl >> 2) & 3;
  bool above = false;
  if (mode == 0) {
    // Auto, by the speaker's engine heading and the other open windows. Heading below 0x800: above when no other window is open or
    // one sits under its speaker, below when the others sit elsewhere. Heading 0x800 and up: above only when one sits under its speaker.
    const bool low_heading = have_actor && (((ents_[static_cast<size_t>(speaker)].actor.facing() - 0x800) & 0xFFF) < 0x800);
    const int others = other_window_below(dlg);
    above = low_heading ? others != 0 : others == 1;
  } else if (mode == 2) {
    above = true;
  }
  if (mode != 3) {
    if (above) {
      proj(-hgt, tx, ty);
      y = static_cast<int>(ty) - 8 - box_height;
    } else {
      proj(0.0f, fx, fy);
      y = static_cast<int>(fy) + 8;
    }
  } else {
    x = 0xA0 - width / 2;
    y = 0x6C - box_height / 2;
  }
  if (win[0] != 0) x = win[0] - width / 2;
  if (win[1] != 0) y = win[1] - box_height / 2;
  if (fl & 2) {
    x = 0xA0 - width / 2;
    y = ((fl >> 2) & 1 ? 0xB7 : 0x41) - box_height / 2;
  }
  y = std::clamp(y, 0x14, std::max(0x14, 0xC4 - box_height));
  x = std::clamp(x, 0x20, std::max(0x20, 0x120 - width));
  dlg.set_ps1_window(x, y);
  // Flag 0x20: the window opens at full size (no grow from the speaker).
  if (fl & 0x20) dlg.skip_grow();
  log::debug("field: dialogue window ps1 ({}, {}) {}x{} px mode {} above {} flags {:#x} op62 {},{},{},{}", x, y, width, box_height, mode, above, fl,
             win[0], win[1], win[2], win[3]);
  return (mode == 0 || mode == 1) && !above;
}

// E1 turns the running entity's own open window into a choice; an entity that owns no open window changes nothing.
void FieldWorld::dialogue_choice(int first, int last) {
  ui::DialogueDemo* box = window_owned_by(vm_.current_entity());
  if (!box) return;
  choice_pending_ = true;
  box->set_line_choice(first, last);
}

// ------------------------------------------------------------------------------------------------ host: screen
// A8: mode 1 = to black, mode 0 = back to the picture; arg = ticks (0 = at once). Drawn as the room change's subtractive fade.

void FieldWorld::request_menu(script::MenuKind kind, int arg) {
  // FE 65: arg = disc - 1; PC mode swaps the index at once (the "Disc N" screen only fades). It is not a screen, so another pending
  // request does not hold it back.
  if (kind == script::MenuKind::ChangeDisc) {
    vm_.set_var(0x42, arg);
    set_disc(arg + 1);
    return;
  }
  if (menu_state_ != MenuState::None) return;
  if (kind == script::MenuKind::Save && gs_) {
    // FE 85: the roster's grids and gear go back to the stock and variable 0x7E = 3, so the
    // save written by the list that follows is a cleared save (summary flags 3).
    ngplus::strip_for_cleared_save(*gs_);
    vm_.set_var(ngplus::kFlagVar, ngplus::kFlagCleared);
    log::info("field: tick {} FE 85 cleared-save prompt: roster stripped, variable 0x7E = 3", tick_count_);
  }
  menu_req_ = {kind, arg};
  menu_state_ = MenuState::Requested;
  log::info("field: tick {} menu request kind {} arg {}", tick_count_, static_cast<int>(kind), arg);
}
bool FieldWorld::menu_active() { return menu_state_ != MenuState::None; }
void FieldWorld::menu_closed(int result) {
  menu_state_ = MenuState::None;
  // SAVE_RESULT: 1 when the menu saved a file.
  vm_.set_var(0x4C, result);
}
// FE 2F (the title room's INIT when ROOM_PARAM != 0, i.e. the title re-entered after a game): the boot state again -
// the dream records from char20.bin, default names, 20 stars, options 2, relief 0xFF, party /
// key items (+ the Time Shifter) / roster reset, frames 0x203, variables 0..0x3CF = 0; the room register = 8
// (FieldVM::room_register) and variable 6 set BEFORE the clear of the live variables 0..0x3CF, so MAP_ID (variable 6) ends 0
// (as after New Game+, which runs the same function: 0 in both variable blocks); DEBUG_MODE (0x34) kept, variable 0x42 = disc - 1
// (game/new_game.h). The VM's 0x3D0.. take the exe table now.

}  // namespace oc::game
