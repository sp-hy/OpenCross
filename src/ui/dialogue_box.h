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
#include <string>
#include <vector>

#include "import/spritefont.h"
#include "import/text.h"
#include "platform/window.h"
#include "render/camera.h"
#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "ui/typewriter.h"
#include "vfs/vfs.h"

namespace oc::ui {

enum class ArtMode { Hd, Original };

constexpr float kCanvasWidth = 1280.0f;
constexpr float kCanvasHeight = 960.0f;

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
  // Window position chosen by the field engine, PS1 pixels (320x240). Reset by open().
  void set_ps1_window(int x, int y) {
    has_win_ = true;
    win_x_ = x;
    win_y_ = y;
  }
  bool label() const { return label_; }
  bool is_open() const { return open_ && !tw_.closed(); }
  bool finished() const { return !open_ || tw_.closed(); }
  // The {bpage} page being shown (0-based).
  int page_index() const { return static_cast<int>(tw_.page_index()); }

  // Field-window features that need game state.
  // Portrait: RDE face id 0..91 (-1 none), hd/face/<set>/<name>.png at the right end of the box; original art draws the PS1
  // face cdrom/face/<stem>.tim (64 x 64) in the same place, or hd/face/1999 (RDE's original-graphics folder) when the disc has
  // none. `set` = "2021" or "1999" (HD only).
  void set_portrait(int face_id);
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

  void update(float dt, TextPace pace = {});
  void confirm();
  // Between SpriteBatch::begin() and end(). vw/vh: the view size passed to begin().
  // The canvas (1280 x 960 units) maps to the window as RDE draws it over the field (render::overlay_fit: RDE's 4:3 viewport, flatter
  // than wide), so the box covers RDE's screen pixels; `fit` overrides it (the PS1-resolution pass: {0.25, 0, 0} into a (320 + 2e) x 240
  // target).
  void draw(float vw, float vh, const render::CanvasFit* fit = nullptr);

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
  void draw_tiles(const Geo& geo, float zoom, float alpha, bool has_portrait, bool portrait_left);
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
  // Art mode and mod overlay generation (vfs::Vfs::generation) the loaded skin texture belongs to.
  ArtMode skin_mode_ = ArtMode::Hd;
  std::uint32_t skin_generation_ = 0;

  ArtMode mode_ = ArtMode::Hd;
  bool open_ = false;
  bool label_ = false;
  bool has_win_ = false;
  int win_x_ = 0, win_y_ = 0;
  Typewriter tw_;
  render::FaceId face_ = render::kNoFace;
  // Face for {italic} glyphs (kNoFace: the regular face).
  render::FaceId italic_ = render::kNoFace;
  // The open message (kept for swap_faces).
  import::Message flat_;
  bool wrap_ = true;
  float wrap_width_ = 0;
  // JP text: no word wrap, own choice cursor offsets.
  bool jp_ = false;
  float scale_ = 1;
  // Text area in canvas units (largest page).
  float text_w_ = 0, text_h_ = 0;
  float line_h_ = 0;
  // Most lines of any page (the frame has lines + 1 rows).
  int lines_ = 1;
  WindowOpening opening_;
  float blink_ = 0;
  int portrait_ = -1;
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
  int choice_ = 0;
  int chosen_ = -1;
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
  void set_language(import::Lang lang);
  // Re-shows the demo message.
  void set_mode(ArtMode mode);
  // Art swap only: frame skin, portrait and fonts change; an open message keeps its text / typewriter / choice state.
  void set_art_mode(ArtMode mode);

  void update(float dt, TextPace pace = {});
  // Inside SpriteBatch::begin()/end().
  void draw(float vw, float vh, const render::CanvasFit* fit = nullptr);

  // Field integration (FieldWorld).
  bool is_open() const;
  void confirm();
  void move_choice(int delta);
  // MULTI_CHOICE: options are lines first..last of the open message.
  void set_line_choice(int first, int last);
  // -1 until confirmed.
  int chosen() const;
  // The message line the hand is on while a choice list takes the pick (-1 none).
  int picking_line() const;
  // Typewriter position of the open message: page * 0x10000 + revealed glyphs.
  int progress() const;
  // Number entry ({input(n)}) of the open message, started from script variable 0x18 (the digit-entry result) at show(); while it takes
  // input, update() writes its value back to that variable every frame. See DialogueBox::start_digits.
  bool digit_entry() const;
  void move_digit(int step, int shift);
  void set_portrait(int id);
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
  // Top-left of the text area in PS1 pixels (call after show()).
  void set_ps1_window(int x, int y);
  // The shown window opens at full size without the grow (field window flag 0x20; call after show()).
  void skip_grow();
  // The shown message whole and opaque at once: no grow, fade or typing (call after show()).
  void show_at_once();
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
  // Voice events (audio::VoicePlayer): on_page(room, msg, script speaker, {NAME%}-tag speaker, page) when a page is first shown
  // (page 0 right after show()); on_close() once when the window closes. Unset in the demo viewer.
  std::function<void(int room, int msg, int speaker, int tag_speaker, int page)> on_page;
  std::function<void()> on_close;

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
};

}  // namespace oc::ui
