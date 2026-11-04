#pragma once
// Texture registry: every texture that is NOT a whole file in the archives (battle stage atlases, the title backdrop's atlases, effect sprite
// sheets, effect mesh atlases) is named by a TextureKey -> canonical relative path. Two hooks are called at the upload sites:
//   dump(key, image)     Options::dump on (off in the game; the dump button / tool use dump_forced): writes <GameDir>\dump\textures\<path> (once per path per session);
//   replace(key, image)  Options::replace on (always, in the game): an active mod's textures\<path> (PNG, any size) substitutes the image.
// Replacement files come from the mods overlay (vfs::Overlay::textures).
#include <filesystem>
#include <string>
#include <vector>

#include "core/types.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::render {

struct TextureKey {
  // battle_stage | title | effects | effect_mesh (vfs::kTextureCategories).
  std::string category;
  // bg_07, title, the sheet name, ... ('/' allowed: sub-folders).
  std::string name;
  // >= 0: "<index:02>_" file name prefix (atlas index within the group).
  int index = -1;
  // File stem tail ("m1_c320x480", a content tag); may be empty.
  std::string variant;
};

// Canonical path "battle_stage/bg_07/00_m1_c320x480.png": lower case, '/' separators, characters outside [a-z0-9._-/] -> '_', no "..".
std::string texture_path(const TextureKey& key);
// Key of one stage-asset atlas (colour mode 0 = 4bpp / 1 = 8bpp, CLUT position in VRAM halfwords / row).
TextureKey stage_texture_key(const std::string& category, const std::string& name, int index, int mode, int clut_x, int clut_y);
// Categories whose textures are cut from per-scene VRAM pages and repeat across scenes are named by content (effects/<sheet>/<tag>, effect_mesh/<tag>,
// room_mesh/<tag>, room_model/<tag>): identical images are ONE file, so a replacement applies everywhere and bulk dumps stay small.
bool is_content_named(const std::string& category);
// Key of one stage-asset atlas of `group` ("category/name", name may be empty): path-named (<idx>_m<mode>_c<x>x<y>) or content-named per category.
// Content-named keys hash the pixels: call only when texture_registry::active().
TextureKey stage_group_key(const std::string& group, int index, int mode, int clut_x, int clut_y, u32 w, u32 h, const u8* rgba);
// Key of a RAW (mode, CLUT) atlas of a stage / title asset, for debugging only (`occ dump-textures --raw`): raw/<category>/<name>/<idx>_m<mode>_c<x>x<y>.png.
// Not a replaceable category.
TextureKey raw_atlas_key(const std::string& group, int index, int mode, int clut_x, int clut_y);
// Key of one model texture (assets::ModelAsset::source): "<member stem>" -> model/<stem>/clut<row>.png | overlay<k>.png; a weapon pack part
// "battle/wepon/<pack>/part<n>" -> model/battle/wepon/<pack>/part<n>.png; "@<category>" (room models, VRAM-page textures) -> content-named
// <category>/<tag>.png.
TextureKey model_texture_key(const std::string& source, size_t index, size_t main_count, u32 w, u32 h, const u8* rgba);
// Model textures keep the PS1 STP bit in alpha (import/tim.h kStpAlpha); the files do not show it. model_file_pixels: the image as dumped, every
// non-transparent texel opaque (alpha 0 / 255 only). model_restore_stp: a replacement (any size, alpha = coverage, < 128 transparent) gets the STP of
// the original: the original's own flag when every texel shares it, else the flag of the original texel it covers.
std::vector<u8> model_file_pixels(u32 w, u32 h, const u8* rgba);
void model_restore_stp(u32 original_w, u32 original_h, const u8* original, u32 w, u32 h, std::vector<u8>& rgba);
// Short content tag (8 hex digits, FNV-1a over size + pixels): names effect sheets that share a name but differ in content.
std::string texture_content_tag(u32 w, u32 h, const u8* rgba);

namespace texture_registry {

struct Settings {
  // Per-visit dumping (off in the game: the dump button / tool dump everything).
  bool dump = false;
  // Mods' textures replace (always on in the game; a texture mod is switched by its own row).
  bool replace = true;
};
void configure(const Settings& new_settings);
Settings settings();
// Where dump() writes (default <GameDir>\dump\textures, set by bind()); the dev tool points it at its output folder.
void set_dump_root(const std::filesystem::path& root);
// The VFS whose mod overlay supplies the replacement files (null = none). Also sets the default dump root from the game folder.
void bind(const vfs::Vfs* vfs);
// The bound VFS (null = none).
const vfs::Vfs* bound_vfs();

// True when dump() can write or replace() can find something: dumping is on, or replacing is on and a mod provides textures. The upload
// sites that would pay for computing a key (hashing) test this first.
bool active();
// Changes whenever the mod overlay content changes (a Mods tab toggle / priority / Load-on edit, an art-mode switch that changes the overlay): the scenes that
// compose tile replacements poll it and re-apply them live (apply_tile_textures, StagePipeline::reload_textures). 0 without a bound Vfs.
unsigned generation();
// Writes a small text file (the tiles.json manifests) under the dump root at the canonical relative path; once per path per session; not counted in stats().
bool dump_text(const std::string& rel_path, const std::string& text, bool forced);
// Tile replacement: the active mods' PNG for the tile with content hash `hash` (8 hex): <dir>/tile_*<hash>.png (any NN, the first
// by path), else the global tiles/<hash>.png. `dir` = "battle_stage/bg_05" | "title" | "room_mesh". Fills w / h / rgba (any size) and returns true when found.
bool replace_tile(const std::string& dir, const std::string& hash, u32& w, u32& h, std::vector<u8>& rgba);
// As dump(), but regardless of the dump flag (the "Dump all textures" job and the dev tool); still once per path per session.
bool dump_forced(const TextureKey& key, u32 w, u32 h, const u8* rgba);
// Writes the image under the dump root when dumping is on. Returns true when a file was written (once per path per session).
bool dump(const TextureKey& key, u32 w, u32 h, const u8* rgba);
// Replaces the image when an active mod provides textures\<path>. `rgba` is w*h*4 and is resized with w / h. Returns true if replaced.
bool replace(const TextureKey& key, u32& w, u32& h, std::vector<u8>& rgba);

struct Stats {
  // Unique files written this session.
  u32 dumped = 0;
  // Unique textures replaced this session.
  u32 replaced = 0;
  u32 failed = 0;
};
Stats stats();
void reset_stats();
// A full dump starts (the "Dump all textures" job, `occ dump-textures`): forgets which files this session wrote, so every one is written again
// (the dump folder may have been deleted since). The per-file "once per session" rule then holds within the job.
void begin_dump();

}  // namespace texture_registry
}  // namespace oc::render
