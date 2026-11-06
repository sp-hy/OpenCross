#pragma once
// Save / Load screen layout shared by the in-game menu (menu/main_menu_save.cpp) and the title Continue list (title/slot_list.cpp):
// on the 640-wide display (canvas X = 2.0 x, Y = 960/448 y). Frame and sprite positions follow the PS1 layout;
// the HD ink positions (x = ink left / right edge, y = cap top) and the
// bn_70 / kb_70 scales are measured from the HD frames. All values are PS1 px of the 640 mode.
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace oc::ui::save_layout {

// Frames: slot grid 0x52, detail 0x54, message 0x56, Save / Load description 0x55 (state 0, and the saving line while saving), progress
// 0x51 (while saving).
constexpr float kGridX = 66, kGridY = 84;
constexpr float kDetailX = 84, kDetailY = 230;
constexpr float kMsgX = 138, kMsgY = 320;
constexpr float kTopMsgX = 88, kTopMsgY = 266;
constexpr float kProgressX = 216, kProgressY = 234, kProgressCapDx = 11;
// 15 boxes of 32 x 32, 8 per row; then the party faces of the detail panel.
constexpr float kBoxX = 74, kBoxY = 98, kBoxStep = 34, kBox = 32;
constexpr float kFaceX = 88, kFaceY = 242, kFaceStep = 64, kFace = 64;
inline float box_x(int box) { return kBoxX + kBoxStep * static_cast<float>(box % 8); }
inline float box_y(int box) { return kBoxY + kBoxStep * static_cast<float>(box / 8); }

// HD captions in the frames' top gaps: "Slot 1" (cmnmes "slot") and "File# N" (cmnmes "File#", N = box index + 1).
constexpr float kSlotCapX = 76, kSlotCapY = 84.6f, kSlotCapScale = 0.25f;
constexpr float kFileCapX = 95, kFileCapY = 228.7f, kFileCapScale = 0.31f;
// Detail panel: leader name / AUTO SAVE, star + count, Time + value, gold + G, two location lines.
constexpr float kNameX = 285, kNameY = 250.2f, kNameScale = 0.33f;
// Star "★": list position (0x54 + 0x130 - 2, 0xE6 + 0x14 + 1), kb_70 at
// 0.5 * 0.625.
constexpr float kStarX = 0x54 + 0x130 - 2, kStarY = 0xE6 + 0x14 + 1;
constexpr float kStarsX = 406, kStarsY = 251.5f, kNumScale = 0.30f;
constexpr float kTimeLabelX = 439, kTimeLabelY = 245.6f, kSmallScale = 0.25f;
constexpr float kTimeRight = 540, kTimeY = 245.6f;
constexpr float kGoldRight = 531, kGoldY = 259.8f, kGoldUnitX = 533, kGoldUnitScale = 0.26f;
constexpr float kLocX = 285, kLocY1 = 275.3f, kLocY2 = 291.8f, kLocScale = 0.28f;
// Message box text ("Please select a file." ...) and the state-0 description ("Save a File" / "Load a File").
constexpr float kMsgInkX = 147.5f, kMsgInkY = 338.5f, kMsgScale = 0.31f, kMsgWidth = 400;
constexpr float kTopMsgInkX = 97.5f, kTopMsgInkY = 284.5f;
// Save / Load list: labels at the top right with leader lines (horizontal run, then a short diagonal) toward the dial
// (menutim_02 at the same canvas size as the main list's) whose needle aims at the end of the selected entry's line.
constexpr float kListX[2] = {387.5f, 375.0f}, kListY[2] = {74.1f, 101.5f}, kListScale = 0.35f;
constexpr float kLeadDy = 12.8f, kLeadBend[2] = {431.0f, 420.0f}, kLeadDx = 7.5f, kLeadDrop = 6.9f;
constexpr float kDialX = 420, kDialY = 64.5f, kDialW = 160, kDialH = 145.9f;
constexpr float kPivotX = 500, kPivotY = 137.2f;

// OpenCross save page (an OpenCross layout, not the PS1's): list frame 64 (416 x 210) at (112, 12), caption "OpenCross" in its tab
// (the grid caption's offset), 10 rows of 18 px with the first row's cap top at 34; pointer at x 118 (1.5 px above the cap top);
// columns (ink): number 146 (clear of the 23 px pointer), leader 182, location 248 (fit 128), playtime right 452, star 466, count 485. Scroll marks at x 508.
// The in-game star sprite is drawn at row y + kRowStarDy: its centre then matches the digits' centre.
// The detail and message frames stay as on the grid page.
constexpr float kPageX = 112, kPageY = 12;
constexpr int kPageRows = 10;
// kRowScale is the largest row size: every row of the page shares one scale, the largest at which each file's leader and location fit
// their columns. The caption is shrunk once to the tab gap (frame 64: no top border from x + 8 to x + 61).
constexpr float kRowY0 = 34, kRowStep = 18, kRowScale = 0.29f, kPageCapMaxW = 49;
constexpr float kRowCursorX = 118, kRowNumX = 146, kRowNameX = 182, kRowLocX = 248, kRowLocW = 128, kRowTimeRight = 452;
constexpr float kRowStarX = 466, kRowStarsX = 485, kRowMarkX = 508, kRowStarDy = -0.6f;
inline float row_y(int row) { return kRowY0 + kRowStep * static_cast<float>(row); }
// Card choice with the page on top: "Please choose a slot." stays at (146, 336); the three lines at (298, 356 / 376 / 396), the pointer at
// (266, line + 1).
constexpr float kChoiceX = 298, kChoiceY0 = 356, kChoiceStep = 20, kChoiceCursorX = 266;
inline float choice_y(int line) { return kChoiceY0 + kChoiceStep * static_cast<float>(line); }
// Location caption of a save: savetbl row = the number of scenario start values <= the save's scenario (u16 head 0x40), at least 1.
inline int location_key(int scenario) {
  static constexpr int kStart[30] = {0, 8, 16, 24, 26, 28, 30, 36, 52, 64, 72, 80, 88, 92, 100, 108, 112, 116, 124, 128, 140, 144, 156, 164, 172, 184, 195, 202, 209, 999};
  int key = 0;
  for (int start : kStart)
    if (scenario >= start) ++key;
  return key < 1 ? 1 : key;
}
// Page file caption: "File# 1" .. "File# 100" / "File# Auto" (number 0 = the auto-save), one size for every number: the scale at
// which the widest (page_caption_widest) fits the detail frame's tab gap (frame 84: no top border from x + 8 to x + 61, ink from
// kFileCapX up to kFileCapMaxW wide).
constexpr float kFileCapMaxW = 47;
inline std::string page_caption(int number) { return number == 0 ? " Auto" : " " + std::to_string(number); }
inline const char* const kPageCaptionWidest[2] = {" 100", " Auto"};
// The page's captions ("OpenCross", "File# N") are centred on their frame's top border at whatever scale they fit: the double line's centre
// lies 3.85 px below the frame's y (frames 64 and 84), the bn cap height is 24 px per unit scale (both measured on the HD frames).
constexpr float kBorderMid = 3.85f, kCapPerScale = 24.0f;
inline float caption_y(float frame_y, float scale) { return frame_y + kBorderMid - kCapPerScale * scale / 2.0f; }
// Row location: the first savetbl line without its trailing dash and blanks (the quote that follows it is not shown in the row).
inline std::u32string row_location(std::u32string line) {
  while (!line.empty() && (line.back() == U' ' || line.back() == U'-' || line.back() == U'\u2013' || line.back() == U'\u2014' || line.back() == U'\u3000'))
    line.pop_back();
  return line;
}
// Row number: "Auto" / "001".
inline std::string page_number(int number) {
  if (number == 0) return "Auto";
  char buffer[8];
  std::snprintf(buffer, sizeof buffer, "%03d", number);
  return buffer;
}

// Detail-panel name: the leader's name, or the auto-save box (slot 0) caption - two lines in FR / IT / DE / ES.
// w = trunc(Measure(L == 5 ? kb : bn, text, 0.35) / 2) (sx = 2 on the 640 screen, the widest line); w > 97: scale *= 97 / w and
// Y += k - k * 97 / w canvas px (k 28, JP 32); then line by line, pitch 16 * sy, minus 5 canvas px for the auto-save caption, which
// also adds a per-language dy of {5, -5, -5, -5, -5, 0} canvas px.
inline const char* auto_save_caption(int lang) {
  static const char* const kCaptions[6] = {"AUTO SAVE", "SAUVEGARDE\nAUTO", "SALVATAGGIO\nAUTOMATICO", "AUTOM.\nSPEICHERN", "GUARDADO\nAUTOM\xC3\x81TICO",
                                           "\xE3\x82\xAA\xE3\x83\xBC\xE3\x83\x88\xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96"};
  return kCaptions[lang < 0 || lang > 5 ? 0 : lang];
}
struct NameFit {
  float mul = 1;    // scale factor (shrink to 97)
  float dy = 0;     // canvas px added to the first line
  float pitch = 0;  // canvas px between lines
};
// `w_canvas` = the widest line measured at 0.35 in the language's default face (canvas px).
inline NameFit name_fit(float w_canvas, bool auto_save, int lang) {
  NameFit fit;
  const float w = static_cast<float>(static_cast<int>(w_canvas / 2.0f));
  if (w > 97.0f) {
    const float font_size = lang == 5 ? 32.0f : 28.0f;
    fit.mul = 97.0f / w;
    fit.dy = font_size - font_size * fit.mul;
  }
  static const float kDy[6] = {5, -5, -5, -5, -5, 0};
  // Relative to English: kNameY is the calibration of the English caption.
  if (auto_save) fit.dy += kDy[lang < 0 || lang > 5 ? 0 : lang] - kDy[0];
  fit.pitch = 16.0f * 960.0f / 448.0f - (auto_save ? 5.0f : 0.0f);
  return fit;
}

// Slot icon of `box` (0-based): hd/menufaceicon/<set>/icon_{box:02}.bmp (32 x 32 at 1x, 128 x 128 8-bit BMP). One fixed face per box,
// not the save's party.
inline std::string slot_icon_path(std::string_view set, int box) {
  char path[80];
  std::snprintf(path, sizeof path, "hd/menufaceicon/%.*s/icon_%02d.bmp", static_cast<int>(set.size()), set.data(), box);
  return path;
}
// The face icon drawn in a grid box (n = file number + 1): the file's own
// icon when the box is the one its number belongs to (box == file number); a file sitting in another box (a card whose directory is not
// in number order) shows the CURSOR's icon unless cursor + 1 exceeds the largest n drawn (`max_n` = largest file number + 1), then its
// own. E.g. a lone CHRONO01 in box 0 shows Serge (cursor 0) / Kid (cursor 1). Returns the icon number (0 = Serge).
inline int box_icon(int box, int file_no, int cursor, int max_n) {
  if (box == file_no || cursor < 0 || cursor + 1 > max_n) return file_no;
  return cursor;
}
// The lowest file number (0..14) not in `used`, -1 when every number is taken. With card 1's 0 counted as used it is the number a new
// save writes, and the saving screen draws it on the cursor box, over the box's own icon, for a new save and an overwrite alike.
inline int saving_number(const std::array<bool, 15>& used) {
  for (int number = 0; number < 15; ++number)
    if (!used[static_cast<size_t>(number)]) return number;
  return -1;
}
// The icon of that overlay: the box rule above with the drawn number, which counts toward `max_n` as well.
inline int saving_icon(int cursor, int number, int max_n) {
  return box_icon(cursor, number, cursor, std::max(max_n, number + 1));
}
// "File# N" number field: N = the box file's number + 1 in a 3-wide field ("File#  1" = CHRONO00, "File# 12").
inline std::string file_number(int box) {
  char buffer[8];
  std::snprintf(buffer, sizeof buffer, "%3d", box + 1);
  return buffer;
}
// Playtime as "HHH:MM:SS" ("%03d:%02d:%02d" of frames / 60).
inline std::string playtime(unsigned frames) {
  const unsigned seconds = frames / 60;
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%03u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
  return buffer;
}

// A cleared save prints the star and the star count with the blue channel at 0x10 / 0x80 of the normal one.
// `colour` = the list's normal text colour, packed R | G << 8 | B << 16 | A << 24.
inline unsigned cleared_star_colour(unsigned colour) { return (colour & 0xFF00FFFFu) | ((((colour >> 16) & 0xFFu) / 8u) << 16); }

}  // namespace oc::ui::save_layout
