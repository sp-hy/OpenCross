#pragma once
// Dialogue window, the field message window model in both art modes: HD draws the skin
// hd/window/wNN.png and the large_ fonts; original (classic) art draws the same pieces from the PS1 skin cdrom map/window/wN.tim
// (the 1/4-size original of the same sheet: wNN.png is a 4x redraw of wN.tim with the same layout, so the UVs are shared) with
// the small_ fonts (only the font files differ between the modes). Typewriter,
// portrait, page / choice cursor. DialogueDemo bundles everything the debug view needs.
//
// Layout is in a 960-unit-high canvas (RDE's 1280x960); draw() scales it to the SpriteBatch view it is given.
#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "import/spritefont.h"
#include "import/text.h"
#include "platform/window.h"
#include "render/camera.h"
#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "save/dialogue_log.h"
#include "ui/typewriter.h"
#include "vfs/vfs.h"

namespace oc::ui {

enum class ArtMode { Hd, Original };

constexpr float kCanvasWidth = 1280.0f;
constexpr float kCanvasHeight = 960.0f;

// Dialogue face ids 0..kDialogueFaces - 1 and the portrait picture of each: hd/face/<2021|1999>/<name>.png ("" outside the range). Some
// ids share a picture.
constexpr int kDialogueFaces = 92;
const char* dialogue_face_art(int face_id);

class DialogueBox {
 public:
  // `skin` is the member below hd/window/ without extension ("w01"); RDE's default skin is texture w01.
  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text,
            const char* skin = "w04");
  void destroy();

  void set_mode(ArtMode mode);
  ArtMode mode() const { return mode_; }

  // Lays out `flat` (import::flatten output) and opens the window (WindowOpening: it grows first, then the typewriter starts). Call
  // outside SpriteBatch begin()/end(). `wrap_width` is the wrap limit at glyph scale 0.5 (ui::wrap_limit).
  void open(const import::Message& flat, render::FaceId face, float text_scale, bool wrap, float wrap_width,
            render::FaceId italic = render::kNoFace);
  // A window opened without the grow (field window flag 0x20): full size at once. Call right after open().
  void skip_grow() { opening_.start(false); }
  // The shown message whole and opaque now: no grow, no fade, no typing (a label window whose text changes).
  void show_at_once() {
    opening_.finish();
    tw_.reveal_now();
  }
  // Art swap of an open window: lays the same message out again with other faces (the small_ / large_ metrics differ) and keeps
  // the page, the revealed glyph count, pending waits, the choice cursor and the window position. Call outside begin()/end().
  void swap_faces(render::FaceId face, render::FaceId italic = render::kNoFace);
  void close() { open_ = false; }
  // Field "label" window (world-map location names): no frame / portrait; at the PS1 window position (set_ps1_window) when the field engine gave one.
  void set_label(bool on) { label_ = on; }
  // Window position chosen by the field engine, PS1 pixels (320x240): the text area's top-left. `width` / `lines` are the PS1's size of
  // the text area (pixels, the page leaf's 12 and a portrait's 64 included; 0 = taken from the layout), which the window of the PS1
  // font is built around. Reset by open().
  void set_ps1_window(int x, int y, int width = 0, int lines = 0) {
    has_win_ = true;
    win_x_ = x;
    win_y_ = y;
    win_w_ = width;
    win_lines_ = lines;
  }
  bool label() const { return label_; }
  // The widest line of the open message's pages, canvas units (4 per PS1 pixel across), and the height of its tallest page.
  float text_width() const { return text_w_; }
  float text_height() const { return text_h_; }
  bool is_open() const { return open_ && !tw_.closed(); }
  bool finished() const { return !open_ || tw_.closed(); }
  // The {bpage} page being shown (0-based).
  int page_index() const { return static_cast<int>(tw_.page_index()); }

  // Field-window features that need game state.
  // Portrait: RDE face id 0..91 (-1 none), hd/face/<set>/<name>.png at the right end of the box; original art draws the PS1
  // face cdrom/face/<stem>.tim (64 x 64) in the same place, or hd/face/1999 (RDE's original-graphics folder) when the disc has
  // none. `set` = "2021" or "1999" (HD only).
  // `name`: a face by name (hd/face/<set>/<name>.png, a character mods add) shown instead when the file exists.
  void set_portrait(int face_id, std::string name = {});
  // The portrait shows only when the window flags have bit 0 set (`on`); `left` = the portrait sits left of the text.
  void set_portrait_flags(bool on, bool left) {
    portrait_on_ = on;
    portrait_left_ = left;
  }
  // Window skin (RDE config byte 0..14 -> texture hd/window/wNN.png via the engine's table; default byte 0 = w04).
  void set_skin_byte(int config_byte);
  int skin_index() const { return skin_idx_; }
  void set_portrait_set(std::string set);
  const std::string& portrait_set() const { return portrait_set_; }
  // Button glyphs ({key(n)}): the key caps of the keyboard bindings (ui/keyboard_<e f i g s j>.png by `lang_index`) when `keyboard`, else
  // the pad glyphs (ui/gamepad.png).
  void set_button_style(bool keyboard, int lang_index) {
    key_caps_ = keyboard;
    key_cap_lang_ = lang_index;
  }
  // In-window choice (field MULTI_CHOICE): lines first..last of the message are the options; a cursor is drawn at the
  // selected line once the message is fully shown, and confirm() stores the pick in chosen(). Reset by open().
  void set_line_choice(int first, int last);
  bool line_choice() const { return line_count_ > 0; }
  void move_choice(int delta);
  int selected_choice() const { return choice_; }
  // -1 until a choice was confirmed; in-window: the message line number (first..last).
  int chosen() const { return chosen_; }
  // The confirmed option as the window showed it: the glyphs of the highlighted line, without the indent (empty until confirmed).
  const std::u32string& chosen_text() const { return chosen_text_; }
  // The portrait the window shows (RDE face id), -1 none.
  int shown_portrait() const { return portrait_on_ && !label_ ? portrait_ : -1; }
  // Hand cursor of an in-window choice at message line `line` of `page`, relative to the text origin (canvas units): the 70 x 70 quad at
  // the first glyph of the line (offsets by language and skin). valid = false when the line has no glyph on the page.
  struct ChoiceCursor {
    bool valid = false;
    float x = 0, y = 0, w = 70, h = 70;
  };
  static ChoiceCursor choice_cursor(const TextPage& page, float line_h, int line, bool jp, int skin_idx);
  // Choice line mapping: the script's E1 rows are PS1 line numbers of the guest
  // (disc) message page, but the translated text is laid out differently (the EN text merges or splits lines), so the line that is
  // highlighted and carries the hand is  max(0, translated first choice line - guest first choice line + row)  with row = F + index.
  // first_choice_line: the first line of the page whose first three glyphs are spaces (-1 none). guest_choice_lines: per page of the
  // PS1 message, the newline count when the page's third U+3000 glyph is met (-1 none).
  static int first_choice_line(const TextPage& page, float line_h);
  static std::vector<int> guest_choice_lines(const import::Message& ps1);
  static int map_choice_line(int translated_first, int guest_first, int row);
  void set_guest_choice_lines(std::vector<int> lines) { guest_lines_ = std::move(lines); }
  // Number entry ({input(n)}): n digit glyphs of the message, leftmost first. start_digits fills them with the last n decimal digits
  // of `value` and puts the cursor on the rightmost one (no-op for a message without an entry). The entry takes input once its first
  // digit is shown; move_digit adds `step` to the cursor digit (wrapping 9 <-> 0) and moves the cursor by `shift` (wrapping at both
  // ends). The cursor digit is drawn in colour 6 (orange). digit_value = the digits read as a decimal number.
  void start_digits(int value);
  bool digit_entry() const;
  void move_digit(int step, int shift);
  int digit_value() const;
  int digit_cursor() const { return digit_cursor_; }
  // The choice hand at glyph `glyph` of the shown page (-1 none), placed as the in-window choice places it at a line's first glyph: a
  // cursor for a window whose options are not whole lines. Reset by open().
  void set_glyph_cursor(int glyph) { glyph_cursor_ = glyph; }
  // The face that lays out and draws the button symbols (circle, cross, triangle, square), set before open() (kNoFace: the message
  // face); a message face without them borrows the JP one.
  void set_symbol_face(render::FaceId face) { symbol_face_ = face; }
  // choice_line() while the list is up and takes the pick, else -1.
  int picking_line() const;
  // The translated line the hand / highlight sits on now (line_first + cursor, mapped).
  int choice_line() const;

  // Outer size (canvas units) of the frame for a message whose widest line is `text_width` and tallest page `lines` lines (no portrait):
  // corner T0 top-left to the B0 / R0 corner pieces' far edges. rows = lines + 1: no minimum height.
  struct FrameSize {
    float w = 0, h = 0;
    int rows = 0, cols = 0;
  };
  static FrameSize frame_size(int skin_idx, float text_width, int lines);
  // The window a box of the canvas gets (draw_frame). The box is the window's inside: the fill covers it and the frame goes around it.
  // left / top / right / bottom = the fill as drawn: the box's left and top; its right unless the box is narrower than the two corner
  // pieces allow; the bottom the nearest one a whole number of fill rows gives (at least 2 rows, a one-line message's window).
  // outer_w / outer_h = the frame's outer size, as frame_size gives it for a message.
  struct FrameBox {
    float left = 0, top = 0, right = 0, bottom = 0;
    int rows = 0;
    float outer_w = 0, outer_h = 0;
  };
  static FrameBox frame_box(int skin_idx, float left, float top, float right, float bottom);
  // Where the loaded skin's frame round a box of the canvas (draw_frame's box) leaves the inside clear: the frame pieces as draw_frame
  // places them, their pixels (alpha at least half) on a one-unit grid of the canvas, the fill not counted. Corner pieces and ornaments
  // count, so skins of any border width and shape give their own edges. Without a loaded skin the box itself is clear.
  class FrameClear {
   public:
    // The inner edges of the top and bottom sides: below the lowest frame pixel of the upper half and above the highest of the lower
    // half, over the middle half of the box's width.
    float top() const { return top_; }
    float bottom() const { return bottom_; }
    // The inner edges of the left and right sides over the rows y0..y1: right of the rightmost frame pixel of the left half, left of the
    // leftmost of the right half.
    float left(float y0, float y1) const;
    float right(float y0, float y1) const;

   private:
    friend class DialogueBox;
    float box_left_ = 0, box_top_ = 0, box_right_ = 0, box_bottom_ = 0, top_ = 0, bottom_ = 0, rows_from_ = 0;
    // Per canvas row from rows_from_: the side's inner edge, or the box's edge where the frame has no pixel on that row.
    std::vector<float> left_edge_, right_edge_;
  };
  const FrameClear& frame_clear(float left, float top, float right, float bottom);

  void update(float dt, TextPace pace = {});
  void confirm();
  // Auto-advance: the open framed window waits for the confirm button and nothing else (no choice list, number entry or glyph
  // cursor), and has waited its reading pause (auto_advance_seconds of the glyphs read, times `scale`).
  bool auto_advance_due(float scale) const;
  // Between SpriteBatch::begin() and end(). vw/vh: the view size passed to begin().
  // The canvas (1280 x 960 units) maps to the window as RDE draws it over the field (render::overlay_fit: RDE's 4:3 viewport, flatter
  // than wide), so the box covers RDE's screen pixels; `fit` overrides it (the PS1-resolution pass: {0.25, 0, 0} into a (320 + 2e) x 240
  // target).
  void draw(float vw, float vh, const render::CanvasFit* fit = nullptr);
  // The loaded skin's fill over a box of the canvas and its frame around it (frame_box; x 0 = the 1280-wide canvas's left edge), whether
  // or not a message is open. Same pass and view as draw(). Nothing when no skin is loaded.
  void draw_frame(float left, float top, float right, float bottom, float alpha, float vw, float vh, const render::CanvasFit* fit = nullptr);
  // The portrait of RDE face `face_id` (the art set_portrait loads for the current mode and set) as a `size` square at canvas (x, y), in
  // the same pass and view as draw_frame. Load it first with set_portrait outside SpriteBatch begin() / end(); an unloaded face draws
  // nothing. The portrait set_portrait chose last stays the window's.
  void draw_portrait(int face_id, float x, float y, float size, float alpha, float vw, float vh);
  // The button glyph of {key(n)} (set_button_style) as a `size` square at canvas (x, y), in the same pass and view as draw_frame.
  void draw_key(int key, float x, float y, float size, float vw, float vh);

  const Typewriter& typewriter() const { return tw_; }
  // {fset(n)} / {fwait(n)}: the field event register (script variable 0x22), see Typewriter::event_get.
  void set_field_events(std::function<int()> get, std::function<void(int)> set) {
    tw_.event_get = std::move(get);
    tw_.event_set = std::move(set);
  }

 private:
  struct Geo {
    int skin = 0, rows = 0, cols = 0;
    float X0 = 0, Y0 = 0, T0x = 0, T0y = 0, R0x = 0, B0y = 0, last = 0;
  };
  Geo frame_geo(float x, float y, float text_width, int lines, bool has_portrait, bool portrait_left) const;
  static Geo frame_geo_for(int skin_idx, bool jp, float x, float y, float text_width, int lines, bool has_portrait, bool portrait_left);
  static Geo frame_geo_box(int skin_idx, float left, float top, float right, float bottom);
  void draw_tiles(const Geo& geo, float zoom, float alpha, bool has_portrait, bool portrait_left);
  // The frame pieces of a window (edges, then corners), in draw order: canvas rectangle and sheet UV rectangle each.
  struct Piece {
    float x, y, w, h, u, v, du, dv;
  };
  static void frame_pieces(const Geo& geo, const std::function<void(const Piece&)>& out);
  // The window of a message in the PS1 font (disc mode), laid out as the PS1 lays its field window.
  void draw_ps1(float vw, float vh, const render::CanvasFit* fit, float alpha);
  // Glyph passes of the shown page from the text origin (tx, ty) in canvas units.
  void draw_text(float tx, float ty, float zoom, float alpha, bool choosing, int hl_line);
  // Window y of canvas y 0 for the draw in progress (render::canvas_fit oy).
  float yo_ = 0;
  bool load_skin(int index);
  bool skin_current(int index) const;
  std::vector<TextPage> layout(render::FaceId face, render::FaceId italic, float text_scale);
  void rect(float x, float y, float w, float h, u32 color);

  render::GpuContext* ctx_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::TextBatch* text_ = nullptr;
  render::Texture white_, skin_;
  render::Sampler linear_, nearest_;
  render::TexRef white_set_ = {};
  render::TexRef skin_set_ = {};
  float skin_w_ = 0, skin_h_ = 0;
  // The loaded sheet's alpha (skin_w_ x skin_h_), for frame_clear.
  std::vector<u8> skin_alpha_;
  // Art mode and mod overlay generation (vfs::Vfs::generation) the loaded skin texture belongs to.
  ArtMode skin_mode_ = ArtMode::Hd;
  std::uint32_t skin_generation_ = 0;
  // The last frame_clear and what it was measured for (the skin load it measured, counted by skin_loads_, and the box).
  FrameClear clear_;
  std::uint32_t skin_loads_ = 0, clear_loads_ = 0;
  float clear_box_[4] = {};

  ArtMode mode_ = ArtMode::Hd;
  bool open_ = false;
  bool label_ = false;
  bool has_win_ = false;
  int win_x_ = 0, win_y_ = 0, win_w_ = 0, win_lines_ = 0;
  Typewriter tw_;
  render::FaceId face_ = render::kNoFace;
  // Face for {italic} glyphs (kNoFace: the regular face).
  render::FaceId italic_ = render::kNoFace;
  render::FaceId symbol_face_ = render::kNoFace;
  render::FaceId glyph_face(char32_t ch, bool italic) const;
  // The open message (kept for swap_faces).
  import::Message flat_;
  bool wrap_ = true;
  float wrap_width_ = 0;
  // JP text: no word wrap, own choice cursor offsets.
  bool jp_ = false;
  // The message face is the PS1 font (disc mode): PS1 pen advance and line height, no word wrap.
  bool ps1_ = false;
  float scale_ = 1;
  // Text area in canvas units (largest page).
  float text_w_ = 0, text_h_ = 0;
  float line_h_ = 0;
  // Most lines of any page (the frame has lines + 1 rows).
  int lines_ = 1;
  WindowOpening opening_;
  float blink_ = 0;
  int portrait_ = -1;
  std::string portrait_name_;
  bool portrait_on_ = true, portrait_left_ = false;
  // Texture index (config byte 0 -> 3 = w04).
  int skin_idx_ = 3;
  const vfs::Vfs* vfs_ = nullptr;
  std::string portrait_set_ = "2021";
  struct Face {
    render::Texture tex;
    render::TexRef set = {};
  };
  // Keyed by "<set>/<name>", loaded on demand.
  std::map<std::string, std::unique_ptr<Face>> faces_;
  Face* face_now_ = nullptr;
  void refresh_face();
  // Button glyph sheets by member name, loaded on demand.
  std::map<std::string, std::unique_ptr<Face>> button_sheets_;
  bool key_caps_ = false;
  int key_cap_lang_ = 0;
  const render::TexRef* button_sheet(const std::string& member);
  void draw_button(int key, float x, float y, float zoom, float alpha);
  void draw_button_quad(int key, float x, float y, float size, u32 colour);
  int choice_ = 0;
  int chosen_ = -1;
  std::u32string chosen_text_;
  // The glyphs of line `line` of the shown page, without leading and trailing blanks.
  std::u32string page_line_text(int line) const;
  int glyph_cursor_ = -1;
  // In-window choice lines.
  int line_first_ = 0, line_count_ = 0;
  // PS1 first choice line of each page of the open message (-1 none).
  std::vector<int> guest_lines_;
  // E1 seen for this message: choice-indent layout.
  bool choice_mode_ = false;
  // Number entry: one value 0..9 per digit glyph (empty: the message has none) and the cursor digit.
  std::vector<int> digits_;
  int digit_cursor_ = 0;
};

// Self-contained dialogue viewer: loads fonts and tables, shows (room, message) pairs. Wire-up in the app (app/app.h):
//   init once after the SpriteBatch exists; show(); per frame: update(dt), confirm() / move_choice() from the host, and draw() inside
//   SpriteBatch::begin()/end().
class DialogueDemo {
 public:
  DialogueDemo();
  ~DialogueDemo();
  DialogueDemo(const DialogueDemo&) = delete;
  DialogueDemo& operator=(const DialogueDemo&) = delete;

  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites,
            import::Lang lang = import::Lang::En);
  void shutdown();

  // speaker: lang.dat speaker id of the E3/E4 caller (0 = unknown).
  // `line_speaker` (default: `speaker`) picks the line's generated words; `speaker` stays the window's owner for the voice lines.
  void show(int room_id, int msg_index, int speaker = 0, int line_speaker = -1);
  void show(std::string_view room_name, int msg_index);
  // Shows text that is not a room message (a panel the engine builds): lines break only at its newlines, the button symbols are drawn
  // with the JP face outside JP, no portrait, no voice.
  void show_text(const import::Message& flat);
  // DialogueBox::set_glyph_cursor of the shown text.
  void set_glyph_cursor(int glyph);
  void set_language(import::Lang lang);
  // Re-shows the demo message.
  void set_mode(ArtMode mode);
  // Art swap only: frame skin, portrait and fonts change; an open message keeps its text / typewriter / choice state.
  void set_art_mode(ArtMode mode);

  void update(float dt, TextPace pace = {});
  // Inside SpriteBatch::begin()/end().
  void draw(float vw, float vh, const render::CanvasFit* fit = nullptr);
  // The window skin's frame and fill alone around a box of the canvas (DialogueBox::draw_frame).
  void draw_frame(float left, float top, float right, float bottom, float alpha, float vw, float vh, const render::CanvasFit* fit = nullptr);

  // Field integration (FieldWorld).
  bool is_open() const;
  void confirm();
  // DialogueBox::auto_advance_due of the shown message.
  bool auto_advance_due(float scale) const;
  void move_choice(int delta);
  // MULTI_CHOICE: options are lines first..last of the open message.
  void set_line_choice(int first, int last);
  // -1 until confirmed.
  int chosen() const;
  // The message line the hand is on while a choice list takes the pick (-1 none).
  int picking_line() const;
  // Typewriter position of the open message: page * 0x10000 + revealed glyphs.
  int progress() const;
  // The page being shown is fully typed and waits for the player (the typewriter's waiting_for_input).
  bool page_typed() const;
  // Number entry ({input(n)}) of the open message, started from script variable 0x18 (the digit-entry result) at show(); while it takes
  // input, update() writes its value back to that variable every frame. See DialogueBox::start_digits.
  bool digit_entry() const;
  void move_digit(int step, int shift);
  void set_portrait(int id, std::string name = {});
  void set_portrait_flags(bool on, bool left);
  void set_skin_byte(int config_byte);
  // Button glyphs as key caps (`keyboard`, the device the player used last) or pad glyphs; DialogueBox::set_button_style.
  void set_key_caps(bool keyboard);
  // Speaker id resolved for the open message (name tag or script speaker).
  int speaker() const;
  // Plain UTF-8 text of the last shown message (newlines / pages as spaces).
  const std::string& text() const;
  // "2021" or "1999".
  void set_portrait_set(const std::string& set);
  // Frameless label window (field SETUP_DIALOG_WINDOW flag 0x10).
  void set_label(bool on);
  // Top-left of the text area in PS1 pixels and its PS1 size (call after show(); DialogueBox::set_ps1_window).
  void set_ps1_window(int x, int y, int width = 0, int lines = 0);
  // The shown window opens at full size without the grow (field window flag 0x20; call after show()).
  void skip_grow();
  // The shown message whole and opaque at once: no grow, fade or typing (call after show()).
  void show_at_once();
  // DialogueBox::text_width / text_height of the shown text.
  float text_width() const;
  float text_height() const;
  void close();
  // The demo status line at the top left (default on).
  void show_status(bool on);
  import::Lang language() const;

  // Name overrides (the typed character names): return an empty string to keep the default sysmes name.
  std::function<std::u32string(int slot)> party_name;
  std::function<std::u32string(int speaker)> speaker_name;
  // Script state for {playwork(n)} / {itemwork(n)} / {num(n)}: they read global script variable 0x1A + 2n;
  // char_name maps a character id to its display name (typed name or the language default).
  // {fset(n)} writes variable 0x22 through script_set (with script_var: {fwait(n)} reads it).
  std::function<int(int var)> script_var;
  std::function<void(int var, int value)> script_set;
  std::function<std::u32string(int char_id)> char_name;
  // {itemwork(n)} of a script item id that is a mod's own id (a key item past 64, an item from data::kScriptItemBase, an element from
  // kScriptElementBase) -> the record it names, through the running room's owner; unset = the id less the range's base.
  std::function<std::optional<int>(int item_id)> script_record;
  // Voice events (audio::VoicePlayer): on_page(room, msg, script speaker, {NAME%}-tag speaker, page) when a page is first shown
  // (page 0 right after show()); on_close() once when the window closes. Unset in the demo viewer.
  std::function<void(int room, int msg, int speaker, int tag_speaker, int page)> on_page;
  std::function<void()> on_close;
  // Dialogue log ([Game] OpenCrossMenu): set before show(), it receives the room message once its framed window has closed (by the
  // player, the script or a room change): the portrait, the speaker the text names (else the portrait's character), the text as
  // shown, the option picked and the voice files noted for its pages. Unset: nothing is gathered.
  std::function<void(save::DialogueEntry)> on_logged;
  // The voice file (path below a voices folder) a page of the shown message played; empty = none.
  void note_voice(std::string rel);

  import::LangTables& tables() { return tables_; }
  int room_id() const { return room_; }
  int msg_index() const { return msg_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  import::LangTables tables_;
  int room_ = 27, msg_ = 15;
  // Page last reported to on_page (-1 = none shown).
  int voice_page_ = -1;
  // The `speaker` argument of the last show().
  int show_speaker_ = 0;
  void voice_sync();
  // The shown message's log entry, waiting for its window to close (on_logged).
  bool log_pending_ = false;
  save::DialogueEntry log_entry_;
  void log_sync();
};

}  // namespace oc::ui
