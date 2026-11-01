// FieldWorld: the dialogue windows (placement, sizing, text flags and results) and the menu hand-off.
#include "game/field_world.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>

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

ui::DialogueDemo& FieldWorld::window_of(int ent) {
  const auto it = owner_window_.find(ent);
  if (it != owner_window_.end() && it->second > 0 && extra_win_[static_cast<size_t>(it->second - 1)].box)
    return *extra_win_[static_cast<size_t>(it->second - 1)].box;
  return dialogue_;
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
      // bit 12 of the window flags: ((facing + camera yaw + entity yaw offset) & 0xFFF) - 0x401 < 0x800 (unsigned); clear = portrait left.
      const u32 yaw_delta = static_cast<u32>((ents_[static_cast<size_t>(target)].actor.facing() + control_yaw_) & 0xFFF) - 0x401u;
      const u32 fl = static_cast<u32>(flags_hi_for(flag_byte));
      portrait_left_ = !((fl & 0x800u) != 0 || ((fl & 0x1000u) == 0 && yaw_delta < 0x800u));
    }
  }
  bool lbl = false;
  {
    const int runner = vm_.current_entity();
    const int wf = runner >= 0 && runner < static_cast<int>(ents_.size()) ? ents_[static_cast<size_t>(runner)].win[4] : 0;
    lbl = (wf & 0x10) != 0;
  }
  if (win_slot == 0) {
    label_ = lbl;
    dialogue_owner_ = vm_.current_entity();
  } else {
    extra_win_[static_cast<size_t>(win_slot - 1)].label = lbl;
    extra_win_[static_cast<size_t>(win_slot - 1)].owner = vm_.current_entity();
  }
  owner_window_[vm_.current_entity()] = win_slot;
  dlg.set_label(lbl);
  dlg.set_portrait(lbl ? -1 : dialogue_face_);
  last_dialogue_msg_ = message;
  choice_pending_ = false;
  log::debug("field: tick {} dialogue room {} message {} (running ent {}, speaker ent {}, face {}, window {})", tick_count_, room_id_, message,
             vm_.current_entity(), speaker, dialogue_face_, win_slot);
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
  place_dialogue_window(dlg, speaker >= 0 ? speaker : target, message, flag_byte, !lbl && dialogue_face_ >= 0);
}

// Upper byte of the window flags of the running entity (op 62); the dialog op's own flag byte only replaces the low byte.
int FieldWorld::flags_hi_for(int) const {
  const int runner = vm_.current_entity();
  const int wf = runner >= 0 && runner < static_cast<int>(ents_.size()) ? ents_[static_cast<size_t>(runner)].win[4] : 0;
  return wf & 0xFF00;
}

namespace {
// PS1 text width of a dialogue message in pixels: glyphs below 0x75 are 8 wide, the rest 12; the widest line wins.
// Names / items substituted at run time (controls 7, 8, 0x12, 0x15, 0x18, 0x19, 0x1B) are estimated at 6 narrow glyphs.
int ps1_message_width(std::span<const u8> bytes) {
  int widest = 0, width = 0;
  size_t i = 0;
  while (i < bytes.size()) {
    const u8 code = bytes[i];
    if (code >= 0x20) {
      width += code < 0x75 ? 8 : 12;
      i += code > 0xF7 ? 2 : 1;
      continue;
    }
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
      case 6: widest = std::max(widest, width); width = 0; i += 2; break;
      case 4: case 5: case 0xE: case 0xF: case 0x16: case 0x17: i += 2; break;
      case 7: width += 0x30; i += 3; break;
      case 8: case 0x12: case 0x15: case 0x18: case 0x19: case 0x1B: width += 0x30; i += 2; break;
      case 9: i += 3; break;
      case 0xA: width += 0x48; i += 2; break;
      case 0xB: width += 0x30; i += 2; break;
      case 0x10: width -= 0x50; i += 2; break;
      default: i += 1; break;
    }
  }
  return std::max(widest, width);
}
}  // namespace

// Where the window goes: PS1 pixels, top-left (x, y) of the text area; the box maps it to the canvas
// (PSX2 = 2 * PS1, canvas x = 2 * PSX2, canvas y = PSX2 * 960 / 432).
void FieldWorld::place_dialogue_window(ui::DialogueDemo& dlg, int speaker, int message, int flag_byte, bool portrait) {
  const int runner = vm_.current_entity();
  if (runner < 0 || runner >= static_cast<int>(ents_.size())) return;
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
      width = ps1_message_width(std::span<const u8>(record.bytes.data(), record.bytes.size()));
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
    // Auto: above the speaker when its engine heading is below 0x800, else below (no other window is open).
    above = have_actor && (((ents_[static_cast<size_t>(speaker)].actor.facing() - 0x800) & 0xFFF) < 0x800);
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
}

void FieldWorld::dialogue_choice(int first, int last) {
  choice_pending_ = true;
  (window_of(vm_.current_entity())).set_line_choice(first, last);
}
// The entity's last window, also after it closed (E2 reads the pick then); -1: no choice confirmed.
int FieldWorld::dialogue_result() {
  return window_of(vm_.current_entity()).chosen();
}

// ------------------------------------------------------------------------------------------------ host: screen
// A8: mode 1 = to black, mode 0 = back to the picture; arg = ticks (0 = at once). Drawn as the room change's subtractive fade.

void FieldWorld::request_menu(script::MenuKind kind, int arg) {
  if (menu_state_ != MenuState::None) return;
  // FE 65: arg = disc - 1; PC mode swaps the index at once (the "Disc N" screen only fades).
  if (kind == script::MenuKind::ChangeDisc) {
    vm_.set_var(0x42, arg);
    set_disc(arg + 1);
    return;
  }
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
