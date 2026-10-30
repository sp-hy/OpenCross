#pragma once
// Internals of the main menu shared by its screen files: the text colours, the in-executable screen strings, the per-frame input and
// draw-command types, and the full MainMenu::Impl declaration. The setup and input live in main_menu.cpp, the drawing primitives in
// main_menu_draw.cpp, and each screen (main list / status, elements, equip / key items, settings, save) in its own main_menu_*.cpp.
#include <array>
#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "platform/keymap.h"
#include "ui/lowres.h"
#include "ui/menu/main_menu.h"
#include "ui/menu/menu_common.h"
#include "ui/ps1_font.h"

namespace oc::ui {

using namespace menudraw;

namespace mainmenu {

const u32 kText = rgb(206, 197, 197), kName = rgb(164, 164, 164), kSel = rgb(192, 120, 0), kGold = rgb(255, 214, 102);

// Screen strings kept in the executable rather than in lang.dat (main list, Use / Allocate picker, screen titles), columns EN FR IT DE ES JP.
struct Str6 {
  const char* by_lang[6];
};

// The menutest text styles: the font slot, the scale multiplier and the canvas-px nudges (dx / dy are added after scaling) of a list
// entry with `flags` (bits 0-7 = style; bit 16 = force JP, bit 17 = force non-JP) for the global language (0 en, 1 fr, 2 it, 3 de, 4 es,
// 5 jp). The effective language picks the default slot (kb for JP, else bn) and the slot of styles 2 / 3 / 7 (ik for JP); the per-language
// styles 0x64-0x67 use the global language's slot. Only the styles the menu screens use are listed; others get the default slot at 0.7.
struct MtStyle {
  int slot;
  float mul, dx, dy;
};
MtStyle mt_style(u32 flags, int lang_index);

struct Nav {
  bool up = false, down = false, left = false, right = false, ok = false, cancel = false, tri = false, sq = false, l1 = false,
       r1 = false;
  // Held this update (no repeat): the four directions and L2 / R2 (keyboard SlowSpeed / FastSpeed, pad triggers past half way).
  bool held_up = false, held_down = false, held_left = false, held_right = false, l2 = false, r2 = false;
};

struct Cmd {
  enum Kind : u8 { Rect, Img, Text } kind = Rect;
  render::TexRef set = {};
  float x = 0, y = 0, w = 0, h = 0, u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  u32 color = kWhite;
  std::u32string text;
  render::FaceId face = render::kNoFace;
  float scale = 1;
};

// One box of the save / load grid. The boxes follow the card's DIRECTORY order (one box per block of every CHRONOnn file) and a box shows
// the file number of its entry, so box k is not file k on a card whose files are not in number order. `slot` = the CHRONOnn number of the
// box's file, `block` its memory-card block, `scenario` the story progression the location caption is keyed by, `flags` the summary's two
// flag bits (bit 0 = a cleared save).
struct SavePrev {
  bool used = false;
  int slot = -1;
  std::array<u8, 3> party{};
  u32 frames = 0, gold = 0;
  int stars = 0;
  int block = 0;
  u32 scenario = 0;
  u8 flags = 0;
};

enum class GridPrompt { None, RemoveAll, AutoAllocate, FillBlanks, Sort };

}  // namespace mainmenu

using namespace mainmenu;

// ================================================================================================================
struct MainMenu::Impl {
  // ---- services
  render::GpuContext* ctx = nullptr;
  const vfs::Vfs* vfs = nullptr;
  render::SpriteBatch* sprites = nullptr;
  render::TextBatch* text = nullptr;
  const data::GameTables* tables = nullptr;
  const MenuRules* rules = nullptr;
  import::Lang lang = import::Lang::En;
  import::LangTables lt;
  // Menu text faces of the current art mode: labels = slot 4 bn_70 (`face_gn`, `gn` is its glyph table), numbers / JP = slot 0 kb_70,
  // JP labels = slot 9 ik_70; original art swaps only the font files (large_ -> small_).
  font::FontSet fonts;
  std::shared_ptr<const import::SpriteFont> gn;
  render::FaceId face_gn = render::kNoFace, face_kb = render::kNoFace, face_ik = render::kNoFace;
  // Art mode the faces above belong to.
  int fonts_mode = -1;
  TexCache texs;
  Tex* cursor_tex = nullptr;
  // [Video] Ps1ResMenus: the screen drawn at the PS1 display size and scaled up.
  LowRes lowres;
  bool ready = false;
  std::function<void(int)> sfx_fn;
  save::SaveDat* save_dat = nullptr;
  save::OcSaves* page = nullptr;
  std::filesystem::path save_out;

  // ---- state
  GameState* gs = nullptr;
  ArtMode mode = ArtMode::Hd;
  std::string portrait_set = "2021";
  MenuScreen screen = MenuScreen::Main;
  bool closed = true, can_save = true;
  // Debug tab tools were used: the save screen shows a warning (saving itself stays allowed).
  bool save_locked = false;
  int frames = 0;
  // Opening a screen from the main list slides the cards (and, for character screens, the portraits) away while it fades to black (state 1),
  // switches at the darkest point and fades in (state 2); going back reverses the slide. The fade-out ends before the slide does. In ticks.
  static constexpr int kSlideAway = 18, kOutFade = 10, kInFade = 5, kExitOut = 8;
  int xfade_state = 0, xfade = 0, xfade_len = kOutFade, xfade_cursor = 0;
  int slide_t = 0;
  bool leaving = false, portraits_away = false, xfade_pick = false;
  MenuScreen xfade_target = MenuScreen::Main;
  // Last-used input device: false = gamepad glyphs, true = keyboard glyphs.
  bool kb_mode = false;
  // pick_for: 0 status, 1 grid, 2 equip.
  int main_cur = 0, mem_cur = 0, sub = 0, pick_cur = 0, pick_for = 0;
  int member = 0;
  int choice_cur = 0;
  // Status: the profile panel (shown first) or the equipment frames.
  bool profile = true;
  // Roster list of all 44 characters, opened by moving left past the first member while picking: it slides in, runs, slides out.
  // roster_ask = the Triangle "remove all" prompt (Grid / Equip picks), roster_no = its cursor (0 Yes, 1 No). A character picked
  // from it stands in for party slot 0 (`stand_in`) until the screen it opened is left.
  enum class Roster { Off, In, On, Out };
  static constexpr int kRosterRows = 17, kRosterSlide = 20;
  Roster roster = Roster::Off;
  int roster_anim = 0, roster_top = 0, roster_row = 0, roster_no = 0;
  bool roster_ask = false;
  int stand_in = -1;
  // elements: use
  int use_cur = 0, use_scroll = 0, target_cur = 0;
  bool use_target = false;
  // elements: grid. focus 0 = the stock list, 1 = the grid row of `cell_row`. List rows 0 / 1 are Remove All / Remove, or with
  // `arrow_side` (Right on the list) Auto Allocate / Fill Blanks. An element (or Remove) picked in the list first waits for a cell
  // (`pick_element`); a cell picked first waits for an element or Remove from the list (`pick_cell`), or, taken back onto the grid, for a
  // cell to swap with (`swapping`, from swap_row / swap_col). `prompt` is the question in the description panel, `prompt_cur` its
  // choice. list_dim / grid_dim: that half has lost its highlight.
  int focus = 0, list_cur = 0, list_scroll = 0, cell_row = 0, cell_col = 0, swap_row = -1, swap_col = -1, prompt_cur = 0;
  bool arrow_side = false, pick_element = false, pick_cell = false, swapping = false, list_dim = false, grid_dim = false;
  GridPrompt prompt = GridPrompt::None;
  // Grid timing in menu frames (`frames`): input waits until grid_unlock; the cell names of the shown row reappear one per frame from
  // labels_from; a row change slides the old row out and the new one in from slide_from (slide_dir +1 = out to the left, slide_row =
  // the row sliding out).
  int grid_unlock = 0, labels_from = -100, slide_from = -100, slide_dir = 0, slide_row = 0;
  // equip
  int eslot = 0, eq_cur = 0, eq_scroll = 0;
  bool eq_list = false;
  // key items / settings / save
  int ki_cur = 0, ki_scroll = 0;
  int set_cur = 0;
  bool skin_pick = false;
  // save_cur: save grid cursor, slot - 1 (slots 1..14); top_cur: Save / Load choice (0 save, 1 load);
  // save_phase: 0 = Save / Load choice, 1 = slot grid / page list, 2 = save choice, 3 = "No save data" message (since frame
  // card_msg_start); save_card: the choice (0 = the OpenCross page, 1 = Slot 1, 2 = Slot 2); load_mode: the grid is the Load tab;
  // load_cur: load grid cursor, slot 0..14 (0 = the auto-save); page_cur / page_top: the page's cursor row (= file number, 0 = its
  // auto-save) and first visible row.
  int save_cur = 0;
  int top_cur = 0;
  int save_phase = 0;
  int save_card = 1;
  int page_cur = 1, page_top = 0;
  int card_msg_start = 0;
  bool load_mode = false;
  // Opened by open_save_list: the save choice is the first screen and its Cancel closes the menu.
  bool save_only = false;
  int load_cur = 0;
  bool save_confirm = false;
  bool load_pending = false;
  // A file was written since open().
  bool saved = false;
  // load_card: 0 = the OpenCross page, 1 / 2 = the memory cards.
  int load_card = 1, load_slot = 0, load_block = 0;
  int confirm_cur = 1;
  // dial needle: current angle, the settled selection, the 0..10 easing step, the list the angle belongs to
  int hand_angle = 0, hand_settled = 0, hand_step = 0, hand_list = -1;
  std::array<SavePrev, 15> prev{};
  std::array<SavePrev, save::kOcFiles + 1> page_prev{};
  // transient message
  std::string msg;
  int msg_frames = 0;
  DirectionRepeat<4> repeat;

  // ---- drawing
  float zoom = 1, ox = 0, oy = 0;
  // T / TP text scale at rel 1 (bn_70).
  static constexpr float base = 0.36f;
  std::vector<Cmd> cmds;
  // Button glyphs / key caps of the screen being built: drawn after everything else, over the frame borders.
  std::vector<Cmd> key_caps;
  // Frames / dots handed to the text layout audit (render::TextBatch::audit_obstacle), canvas units.
  struct AuditBox {
    float x, y, w, h;
    std::string label;
  };
  std::vector<AuditBox> audit_boxes;
  std::map<int, std::u32string> name_cache;

  // Portrait folder of the faces, slot icons and playtim portraits: original art = 1999, HD art = the [Game] Portraits choice.
  std::string face_set() const { return mode == ArtMode::Hd ? portrait_set : std::string("1999"); }

  // Defined in main_menu.cpp.
  bool init(render::GpuContext& context, const vfs::Vfs& files, render::SpriteBatch& sprite_batch, render::TextBatch& text_batch,
            const data::GameTables& game_tables, import::Lang language);
  void select_fonts();
  void shutdown();
  std::string menu_str(u32 addr, const char* fallback);
  std::u32string char_name(int id);
  std::u32string item_name(int id);
  std::u32string element_name(int id);
  std::u32string key_item_name(int id);
  std::string table_row(std::string_view table, int key);

  // ===================================================== party helpers
  std::vector<int> party() const {
    std::vector<int> members = gs->party();
    if (stand_in >= 0) {
      if (members.empty()) members.push_back(stand_in);
      else members[0] = stand_in;
    }
    return members;
  }
  // Status-screen model and its camera: turning by itself until Triangle hands the camera to the pad (model_cam_manual), then square cycles
  // the poses 0..2. Entering the screen and L1 / R1 start it again.
  MenuModelView* model_view = nullptr;
  bool model_wanted = false;
  MenuCamera model_cam;
  bool model_cam_manual = false;
  int model_pose = 0;
  u32 model_session = 0;
  // cmds index where the parchment ends: the Status model (shadow, body, weapons, effects) is drawn there, under the frames, text, compass
  // swirls, key caps and legend (retail: the PS1 frame holds only the parchment and the 3D model; RDE draws the UI over it).
  size_t model_layer = 0;
  // 60 Hz menu frames spent on the Status screen; the model logic (camera, pose, blink, effects) ticks on every second one.
  int model_frames = 0;
  std::chrono::steady_clock::time_point model_clock{};

  // Defined in main_menu.cpp.
  int cur_char() const;
  void cycle_member(int step);
  void sfx(int id);
  void say(const std::string& message_text, int duration_frames = 150);
  Nav poll(const platform::Input& in);
  void open(GameState& state, bool allow_save);
  void enter(MenuScreen target);
  // to_pick: land on the member pick again (cursor on the member just used) instead of the plain list.
  void back_to_main(int cursor, bool to_pick = false);

  // Defined in main_menu_save.cpp.
  void load_previews();

  // Defined in main_menu.cpp.
  void update(const platform::Input& in, int ticks);
  static int table_angle(int list, int sel);

  // Defined in main_menu_save.cpp.
  static int save_list_angle(int sel);

  // Defined in main_menu.cpp.
  void hand_tick(int list, int sel, int target);

  static int wrap(int value, int count) { return count <= 0 ? 0 : ((value % count) + count) % count; }

  // Defined in main_menu.cpp.
  void scroll_to(int cur, int& scroll, int rows);
  int party_pick_start() const { return std::min(member, std::max(0, static_cast<int>(party().size()) - 1)); }

  // Defined in main_menu_main.cpp.
  void update_main(const Nav& nav);
  void update_roster(const Nav& nav);
  void update_status(const Nav& nav);
  // Menu models run to 0x32 (the weapon packs to 0x31).
  static constexpr int kLastMenuModel = 0x32, kLastWeaponModel = 0x31;
  int model_number(int character, int last) const;
  void restart_model();
  void step_model_camera(const Nav& nav, int ticks);
  void update_choice(const Nav& nav);

  // Defined in main_menu_elements.cpp.
  void update_use(const Nav& nav);
  std::vector<int> grid_list() const;
  int list_element(int index) const;
  int held_element() const;
  int deepest_open_row(int character) const;
  bool element_dim(int character, int id) const;
  void grid_reset();
  void lock_grid(int ticks);
  int slide_offset(int& shown_row) const;
  void to_grid(int row, int lock_ticks, bool rebuild_labels);
  void to_list();
  void move_cell_row(int step);
  void move_cell_col(int step);
  void list_changed();
  void apply_to_cell();
  void update_grid_prompt(const Nav& nav);
  void update_grid_list(const Nav& nav);
  void update_grid_cells(const Nav& nav);
  void update_grid(const Nav& nav);

  // Defined in main_menu_equip.cpp.
  void update_equip(const Nav& nav);
  void update_keyitems(const Nav& nav);

  // Defined in main_menu_settings.cpp.
  void update_settings(const Nav& nav, const platform::Input& in);

  // Defined in main_menu_save.cpp.
  void update_save(const Nav& nav);
  void do_load();
  void do_save();

  // ===================================================== draw primitives (canvas units)
  // Drawing DSL (all positions are emitted into `cmds` and drawn by draw_view): rect / box_outline / Img = canvas rectangles and sprites;
  // T = canvas-unit text; PX / PY map PS1 px to the canvas; Frame = a panel shape at PS1 (x, y); TP / TPfit = text with its top-left at
  // PS1 (x, y), optionally shrunk to a width; HT / HL / HN / HS = ink-box text (bn label / kb number / menutest style) at PS1 px;
  // Digits = a right-aligned number field. Widescreen groups: with [Video] Widescreen the menus use a widened canvas (x from -canvas_s
  // to 1280 + canvas_s, the window's width) and every primitive adds the current group's shift (Left -wide_s, Right +wide_s, Centre
  // none); wide_s ([Video] WideHud) is at most canvas_s, both are 0 at 4:3.
  enum Group { Left = -1, Right = 1, Centre = 2 };
  float wide_s = 0.0f, canvas_s = 0.0f, gdx = 0.0f;
  // Horizontal clip of sprites and rectangles (canvas x, before the group shift); text is not clipped.
  float sprite_clip_left = -1e9f, sprite_clip_right = 1e9f;
  // Set by set_wide_columns.
  float wide_cols = 0.0f, canvas_cols = 0.0f;
  void group(Group which) { gdx = which == Left ? -wide_s : which == Right ? wide_s : 0.0f; }

  // Defined in main_menu_draw.cpp.
  void emit(Cmd&& cmd);
  void rect(float x, float y, float w, float h, u32 colour);
  void box_outline(float x, float y, float w, float h, float thickness, u32 colour);
  void Img(const Tex* tex, float x, float y, float w, float h, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1, u32 colour = kWhite);
  void pick_face(const std::u32string& str, float rel, render::FaceId& face, float& scale) const;
  float tw(const std::u32string& str, float rel = 1);
  void T(float x, float y, const std::u32string& str, u32 colour = kText, float rel = 1);
  void T(float x, float y, std::string_view str, u32 colour = kText, float rel = 1) { T(x, y, utf32(str), colour, rel); }

  // Defined in main_menu_draw.cpp.
  void cursor(float x, float y);

  // ---- PS1 pixel space: the menu overlay draws on a 512 x 448 display mapped onto the 1280 x 960 canvas (canvas X = 2.5 x, canvas
  // Y = y * 960 / 448); the Save / Load screen runs on the 640-wide mode (X = 2.0 x).
  float xscale = 2.5f;
  float PX(float x) const { return x * xscale; }
  static float PY(float y) { return y * (960.0f / 448.0f); }

  // Defined in main_menu_draw.cpp.
  static u32 grey(int ps1_level);
  void Frame(int frame, float x, float y, u32 colour = kWhite, float* out_w = nullptr, float* out_h = nullptr, bool large = false);
  void TP(float x, float y, const std::u32string& str, u32 colour = kText, float rel = 1);
  void TP(float x, float y, std::string_view str, u32 colour = kText, float rel = 1) { TP(x, y, utf32(str), colour, rel); }
  // Text width in PS1 px.
  float twp(const std::u32string& str, float rel = 1) { return tw(str, rel) / xscale; }

  // Defined in main_menu_draw.cpp.
  void Digits(float x, float y, const std::string& number, float advance, u32 colour = kText);
  u32 ps1_colour(u32 bgr, int a = 255) const;
  void message(float x, float y);
  const Tex* portrait(int character);
  void build();

  const char* S6(const Str6& strings) const { return strings.by_lang[static_cast<int>(lang)]; }
  bool jp() const { return lang == import::Lang::Jp; }
  int lang_index() const { return std::clamp(static_cast<int>(lang), 0, 5); }

  // Defined in main_menu_draw.cpp.
  render::FaceId slot_face(int slot) const;
  void HS(u32 flags, float x, float y, const std::u32string& str, u32 colour = kText, float key_dx = 0, float key_dy = 0, bool right = false);
  // Per-language offset table entry relative to English.
  static float dl(const float (&table)[6], int lang_idx) { return table[lang_idx] - table[0]; }

  // Defined in main_menu_draw.cpp.
  std::string cm(std::string_view key);
  void background();

  // ---- menu text: labels = bn_70 (face_gn), numbers = kb_70, both placed by their INK box: x = left edge (or right edge with
  // `right`), y = cap top, in PS1 px. Same in both art modes.
  bool hd_ink() const { return face_gn != render::kNoFace && face_kb != render::kNoFace; }

  // Defined in main_menu_draw.cpp.
  bool glyphs_ok(const std::u32string& str) const;
  void HT(render::FaceId face, float x, float y, const std::u32string& str, u32 colour, float scale, bool right = false);
  void HL(float x, float y, const std::u32string& str, u32 colour = kText, float scale = 0.3125f, bool right = false);
  void HN(float right_x, float y, const std::string& number, u32 colour = kText, float scale = 0.275f);
  void Disc(float cx, float cy, float radius, u32 colour);
  void StarGlyph(float x, float y, float mul, float dy, u32 colour = kText);
  void Seg(float x0, float y0, float x1, float y1, float thickness, u32 colour);
  void leader_raw(float start_x, float y, float bend_x, float end_x, u32 colour);
  void leader(int entry, float x_ink, float cap_y, u32 colour);

  // The dial needle (handle.png turned by `angle`, 4096 = 2 pi; menu/hand.h) centred on the pivot at PS1 (pivot_x, pivot_y).
  NeedleTexture needle_tex;

  // Defined in main_menu_draw.cpp.
  void Hand(float pivot_x, float pivot_y, int angle);

  // Defined in main_menu_main.cpp.
  void clock_dial();

  // Defined in main_menu_save.cpp.
  void save_dial();

  // Defined in main_menu_main.cpp.
  float list_maxw(int list_id, int lang_idx);
  static float list_dx(int list_id, int lang_idx, int entry);
  void list_and_bar(int list_id, int sel, bool bar = true);

  // Defined in main_menu_draw.cpp.
  void bar_and_title(int title_index, bool bar = true, float dx = 0);
  std::string stars_logged;

  // Defined in main_menu_main.cpp.
  void mini_grid(int character, float x, float y);

  // Defined in main_menu_draw.cpp.
  std::u32string capt(std::string_view key, const char* fallback);
  // Caption colour (light grey-blue, like the sheet art).
  static constexpr u32 kCap = 0xFFD8D8E8u;

  // Defined in main_menu_equip.cpp.
  void CapR(u32 flags, float right_x, float y, std::string_view key, const char* fallback, float key_dx, float key_dy, float maxw);

  // Defined in main_menu_draw.cpp.
  void notch_caption(float frame_x, float frame_y, int kind);

  // Defined in main_menu_main.cpp.
  void card_hd(float card_x, float card_y, int character, bool stats, bool stamina, int group_id = 7);
  void member_card(int character, float x, float y);
  void build_main();
  void build_roster();
  void status_panel(int character, float panel_x, float panel_y);
  std::vector<std::u32string> profile_lines(int character);
  void build_status();
  void camera_legend();

  // Defined in main_menu_elements.cpp.
  void build_use();
  void element_row_tint(int id, float x, float y);
  Tex* plate_tex(int tile);
  void element_info(int id, float x, float y0, float y1, float pitch);
  int colour_index(int character, int id) const;
  void plate(int character, int id, float x, float y);
  void empty_box(float x, float y);
  void grid_minimap(int character, float px, float py, int sel_row, int hold_id);
  void build_grid();
  void grid_help(int character);

  // Defined in main_menu_draw.cpp.
  void tri(float x, float y, bool active);
  void scroll_arrow(float x, float y, bool up);
  void pointer(float x, float y, bool left);
  void btn(float x, float y, int slot_index, u32 colour = kWhite);
  void action_glyph(platform::Action action, float x, float y);
  void key_item_line(const std::string& description, float x, float y, float maxw);
  void portrait_strip(int character, float x, u32 col, float dy = 0.0f);
  float fit_rel(const std::u32string& str, float maxw, float rel);
  void TPfit(float x, float y, const std::u32string& str, float maxw, u32 colour = kText, float rel = 0.9f);

  // Defined in main_menu_equip.cpp.
  void header_block(int character, float x, float y, int hmode);
  void build_equip();
  void build_keyitems();

  // Defined in main_menu_settings.cpp.
  void build_settings();
  void skin_preview();

  // ---- save (states 0x10 / 0x12): slot grid frame 0x52 at (66,84), detail panel 0x54 at (84,230), message box 0x56
  // at (138,320). 2 rows x 8 boxes of 32x32 at (74 + 34 c, 98 + 34 r); slot 0 is the auto-save entry (shown, not selectable).
  // Per folder (1999 / 2021): character id -> hd/face/<set>/NN_name.png.
  std::map<std::string, std::map<int, std::string>> face_files;

  // Defined in main_menu_save.cpp.
  const Tex* face_tex(int character);
  void avatar(int character, float x, float y, float w, float h);
  std::pair<std::string, std::string> location_lines(u32 value);
  void HLfit(float x, float y, const std::u32string& str, float maxw, float sc);
  const Tex* slot_icon(int file);
  void build_save();

  bool pad_present = false;

  // ===================================================== draw
  // [Video] Ps1Res (any art mode): the whole screen into a target of menutest's display size
  // (512 x 448; Save / Load and Settings on the 640 x 448 mode), scaled up nearest.
  // lowres_on() = that pass; ps1_screen() = the PS1 disc textures, classic art only (HD keeps its own art, point-sampled).
  // Widescreen: the canvas grows by canvas_s = 4 * screen_extra() canvas units per side; the target grows with it
  // (512 + 3.2 e / 640 + 4 e texels wide) and is shown in the window rectangle of the widened 240-row frame.
  bool lowres_on() const { return ps1_menus(); }
  bool ps1_screen() const { return mode == ArtMode::Original && ps1_menus(); }

  // Defined in main_menu_draw.cpp.
  void draw(const render::Frame& frame, float vw, float vh, bool resume);
  void draw_view(const render::Frame& frame, float vw, float vh, bool resume, bool in_pass = false);
};

}  // namespace oc::ui
