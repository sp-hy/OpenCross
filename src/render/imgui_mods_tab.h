#pragma once
// Esc overlay tab "Mods": the mod folders of <GameDir>\mods in priority order, each with an enable checkbox, up / down priority,
// a "Load on" combo (Modded / SD / HD / All), file counts and the conflicts (which mod wins), plus Rescan.
#include <functional>
#include <string>
#include <vector>

#include "platform/config.h"
#include "vfs/mods.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::render {

// [Mods] ini settings <-> the manager's prefs.
std::vector<vfs::ModPref> mod_prefs(const platform::Config& cfg);
void store_mod_prefs(platform::Config& cfg, const vfs::ModManager& mods);
// The overlay mode for an art mode (Original = SD, Hd = HD, Modded = HD + mods).
vfs::ModArt mod_art(platform::ArtMode mode);
// Starts `opencross modelview` as a separate process (Mods and Debug tabs); the game keeps running.
void open_model_viewer();

// The "Dump all textures" button runs game::TextureDumpJob (a worker thread); render cannot see the game library, so the app (app/app_boot.cpp) installs
// these callbacks. Without hooks the button is not shown.
struct TextureDumpHooks {
  // Argument is a bitmask: 1 stages, 2 title, 4 effects, 8 rooms, 16 models (game::DumpSet). Returns false when already running.
  std::function<bool(unsigned)> start;
  std::function<void()> cancel;
  std::function<bool()> running;
  // "<phase> n / total, F files".
  std::function<std::string()> status;
  // Progress of the current phase, 0..1.
  std::function<float()> fraction;
  // The last finished run's one-line summary ("" before).
  std::function<std::string()> summary;
};
void set_texture_dump_hooks(TextureDumpHooks hooks);

// Draws the tab body (call between BeginTabItem / EndTabItem). Returns true when the mod set changed: cfg.mods has been updated,
// the caller saves the ini and re-applies the art mode (ModManager::apply + the art swap).
// `settings_dirty` (optional) is set when only a setting changed (the dump category checkboxes): the ini needs saving, no art re-apply.
bool draw_mods_tab(platform::Config& cfg, vfs::ModManager& mods, const vfs::Vfs& files, bool* settings_dirty = nullptr);

}  // namespace oc::render
