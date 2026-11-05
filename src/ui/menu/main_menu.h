#pragma once
// The in-game main menu (menutest overlay): main list, Status, Elements (use / grid editor), Equip, Key Items,
// Settings and Save. Drawing is done on a fixed 1280x960 canvas fitted into the view;
// both art modes draw the same screens (same sprites, layout and text slots); original mode only swaps
// the small_font_* files, the 1999 face / portrait / slot-icon folders and the HD status-model texture.
//
//   MainMenu m;  m.init(ctx, vfs, sprites, text, tables, lang);  m.set_rules(&rules);
//   m.open(state, can_save);  per frame: m.update(input); m.draw(frame, vw, vh [, resume]);  until m.closed().
// draw() runs its own SpriteBatch begin()/end() pair (resume = continue the frame's quad buffer after an earlier pair).
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "data/tables.h"
#include "import/text.h"
#include "platform/window.h"
#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "save/cc_save.h"
#include "save/oc_saves.h"
#include "ui/dialogue_box.h"  // ArtMode
#include "ui/menu/game_state.h"
#include "ui/menu/menu_model.h"
#include "vfs/vfs.h"

namespace oc::ui {

enum class MenuScreen { Main, Status, ElementChoice, ElementUse, ElementGrid, Equip, KeyItems, Settings, Save };
const char* menu_screen_name(MenuScreen screen);

class MainMenu {
 public:
  MainMenu();
  ~MainMenu();
  MainMenu(const MainMenu&) = delete;
  MainMenu& operator=(const MainMenu&) = delete;

  // Loads the menu fonts (ui::font::FontSet, both art modes) and the lang tables. `tables` must outlive the menu.
  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text,
            const data::GameTables& tables, import::Lang lang);
  void shutdown();

  // Status screen 3D model (optional; without it the member portrait strip is drawn instead). Must outlive the menu.
  void set_model_view(MenuModelView* view);
  // Must outlive the menu; needed before open().
  void set_rules(const MenuRules* rules);
  // The typed names by character id; must outlive the menu.
  void set_custom_names(const std::map<int, std::u32string>* names);
  // A character's name in a saved game: its data, its ext_save.dat names, the character id.
  using SavedNameLookup = std::function<std::u32string(const save::SaveSlot&, const std::map<int, std::u32string>&, int)>;
  void set_saved_name_lookup(SavedNameLookup lookup);
  void set_mode(ArtMode mode);
  ArtMode mode() const;
  void set_language(import::Lang lang);
  // "2021" or "1999" (HD art; original art always uses the 1999 folder).
  void set_portrait_set(const std::string& set);
  // Save screen target: slots are read from / written to card 1 of `dat`; `out` is where save() writes it.
  // `page`: the OpenCross save page (null = the memory cards only).
  void set_save_target(save::SaveDat* dat, std::filesystem::path out, save::OcSaves* page = nullptr);
  // Sound hook: 1 cursor, 8 confirm, 6 cancel, 0x18 denied, 0x17 placed, 0x48 equipped.
  void set_sfx(std::function<void(int)> fn);
  // A second sound played with the first on its own sound mask (id, mask), so the two do not cut each other.
  void set_sfx_layer(std::function<void(int, u32)> fn);
  // Debug tab tools were used this session: the save screen shows "Debug tools used this session"; saving stays allowed.
  void set_save_locked(bool on);
  // [Video] Widescreen: `canvas_columns` = layer columns per side the backdrop (parchment) extends by, the window's own; [Video]
  // WideHud ("Wide Menus/HUD"): `layout_columns` = the columns per side the left / right groups spread out by (0 = the 4:3 layout).
  void set_wide_columns(float canvas_columns, float layout_columns);
  bool save_locked() const;

  void open(GameState& state, bool can_save);
  // The ending's save prompt (field op FE 85): the Save screen without the Save / Load list, starting on the save choice; Cancel on
  // the choice closes the menu, Cancel on a file list returns to the choice.
  void open_save_list(GameState& state);
  void close();
  bool closed() const;
  // A file was written since open(); the script reads it as variable 0x4C (SAVE_RESULT) for the ending's save prompt.
  bool saved() const;
  MenuScreen screen() const;
  // The Status model's pad camera is on: L2 / R2 (F1 / F2, the triggers) belong to it, not to the slow / fast toggles.
  bool camera_takes_triggers() const;
  // The Controller assignment page of Customize is open: the pad is read as it is, without the assignment.
  bool pad_page_open() const;

  // Save/Load > Load: the menu closes and this reports the chosen slot once (card block + decoded data); the host starts it like Continue
  // (GameFlow::load_slot). Load is always available, Save only when `can_save` of open() is set.
  struct LoadRequest {
    // card: memory card 1 / 2, 0 = the OpenCross page; slot: CHRONOnn (0 = auto-save on card 1) or the page's file number;
    // block: memory-card block.
    int card = 1;
    int slot = 0;
    int block = 0;
    save::SaveSlot data;
  };
  std::optional<LoadRequest> take_load_request();

  // ticks = 60 Hz sim ticks this frame (the animations advance by them).
  void update(const platform::Input& in, int ticks = 1);
  void draw(const render::Frame& frame, float vw, float vh, bool resume = false);

  // Test / screenshot hook: jumps straight to a screen (member 0, cursors at the start). `variant` 1 = a sub-state
  // (member pick on Main, element picked over the grid, equip candidate list, equipment view on Status); grid: 2 = the Auto Allocate
  // question, 3 = the Remove All question, 4 = Reorder.
  // Main: 2 = the roster (Status pick), 3 = the roster's remove-all prompt (Equip pick). ElementUse: 1 = the party target, 2 = the roster. Save: 0 = Save/Load choice,
  // 1 = save grid, 2 = load grid, 3 = load confirm, 4 = choice with saving blocked.
  // Settings: 1 = the skin picker, 2 / 3 = the Controller assignment page (field / battle).
  void debug_show(MenuScreen screen, int variant = 0);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oc::ui
