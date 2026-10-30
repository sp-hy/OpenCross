#pragma once
// Post-battle results screen. After a victory the battle
// stage is gone; over the btend wallpaper the three party portrait strips, one framed row per party member (name, HP,
// the six stats, the element-grid thumbnail, an "UP" marker on every stat that grew) and the right-hand panel with the
// "PowerUp" page (per member: the stat-up labels and the gains, or the star-up message after a boss), the gold line and
// finally the "Spoils" page (dropped items / elements). Confirm advances member -> member -> [heal-item menu when a member
// is hurt and the grids still hold unused healing elements] -> spoils -> exit (black fade, panels slide out).
// The heal menu changes only party HP: the host reads it back with take_final_hp() after done() and writes it over the
// HP that apply_battle_result stored (the HP gain of the growth is added to the current HP here, as are the heals). Both art modes draw the same btlexit-levu / btend PNGs;
// original art only swaps the font files (small_font_*) and the portrait folder (hd/battle/face/1999).
//
//   BattleResults results;  results.init(ctx, vfs, sprites, tables, lang);  results.set_sfx(fn);
//   if (results.open(battle, scene_id)) { per frame: results.update(dt, input); results.draw(frame, vw, vh); until results.done(); }
// open() returns false when there is nothing to show (not a victory, or scene 0x1DD). The caller
// writes the rewards back to the session after done().
#include <array>
#include <functional>
#include <memory>
#include <string>

#include "battle/battle.h"
#include "data/tables.h"
#include "import/text.h"
#include "platform/window.h"
#include "render/text_batch.h"
#include "render/sprite_batch.h"
#include "vfs/vfs.h"

namespace oc::game {

class BattleResults {
 public:
  BattleResults();
  ~BattleResults();
  BattleResults(const BattleResults&) = delete;
  BattleResults& operator=(const BattleResults&) = delete;

  // `tables` must outlive the screen. Fonts, lang tables and the art of both modes are loaded here.
  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, const data::GameTables& tables, import::Lang lang);
  void shutdown();

  // Art / mod switch: a pure rendering swap. `force` = a mod overlay change with the same art mode: every image, the fonts and the
  // portrait cache are re-read through the (re-pointed) VFS.
  void set_art_mode(bool hd, bool force = false);
  // [Video] Filter (global) (Config::filter): true = bilinear. Re-binds every image.
  void set_filter(bool bilinear);
  // Cursor-memory option: the heal menu opens on the last chosen entry.
  void set_cursor_memory(bool on);
  void set_language(import::Lang lang);
  // HD art: "2021" / "1999" = hd/battle/face/<set>/pNN.png (original art: always 1999).
  void set_portrait_set(const std::string& set);
  // Global sound effects (effect00 ids): 0x47 screen open, 0x14 star-up, 1 cursor, 6 cancel, 8 confirm, 0x41 exit.
  void set_sfx(std::function<void(int)> fn);
  // Called once when the screen is done, kFieldReturn ticks after the last confirm (the host stops the victory music there).
  void set_on_exit(std::function<void()> fn);
  // [Video] Widescreen: `canvas_columns` = layer columns per side the wallpaper and fades extend by; [Video] WideHud: `layout_columns` =
  // the columns the portraits / rows (left) and the panel (right) move out by.
  void set_wide_columns(float canvas_columns, float layout_columns);
  // [Debug] BattleAuto / route `fight`: every page confirms itself after kAutoHold ticks.
  void set_auto(bool on);

  bool open(const battle::Battle& finished, int scene_id);
  void update(float dt, const platform::Input& in);
  // Between ctx.begin_frame() and end_frame(); runs its own sprite begin()/end() (`resume` continues an earlier pair).
  void draw(const render::Frame& frame, float vw, float vh, bool resume = false);
  // The batch draw() writes text with (the PS1-resolution pass sets its pixel grid).
  render::TextBatch* text_batch();

  // Drops the screen without writing anything back.
  void abort();
  // open() succeeded and the exit has not finished.
  bool active() const;
  // The exit fade has finished (the host resumes the field).
  bool done() const;
  // Tests / screenshots: skip the fade-in and the settle frames.
  void finish_intro();

  enum class Phase { Idle, FadeIn, PowerUp, Heal, Healed, Spoils, Exit, Done };
  Phase phase() const;

  // After done(): the party HP to write back, per party slot (-1 = no member). Includes the growth's HP gain and the heal menu; 0 for a
  // KO'd member (the write-back raises it to 1). Returns false (and clears) when no results screen ran since the last call.
  bool take_final_hp(std::array<int, battle::kPcSlots>& hp);
  // Test hook: the heal menu's state (route assertions): -1 not offered, else the cursor (0 heal, 1 heal + stock, 2 do not heal).
  int heal_cursor() const;

  // Timing in 60 Hz ticks: fade-in 0x3C, settle 3 + 4 frames, input debounce 0x10, exit fade to black 0x1E.
  static constexpr int kFadeIn = 60, kSettle = 7, kDebounce = 16, kExit = 30, kAutoHold = 180;
  // From the last confirm to the field's return (done(): the field fades in and restores its song, which cuts the victory music).
  static constexpr int kFieldReturn = 43;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oc::game
