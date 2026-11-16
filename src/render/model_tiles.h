#pragma once
// Room model textures as tiles.
//
// A room model draws from windows of its room's VRAM (import::vram_page_textures: the 256 x 256 texels from its TIM on, through CLUT rows
// clut_y and clut_y + 1) that also hold other models' art, so a page is no unit to dump or replace. The TILES are, per page, the texel
// rects its faces sample (overlapping rects merged, touching ones not) cut through that page's CLUT: dumped and replaced one by one and
// content-named (room_model/tile_<hash8>.png: the same art is ONE file across rooms and models). A page with replaced art is re-composed into
// assets::ModelAsset::mod_textures: enlarged (nearest) to the largest replacement scale, each replacement pasted over its rect (the UVs are
// normalised, so the faces sample the same places).
//
// A room model whose own TIM (the top-left of page 0) is a field character's RDE texture (FieldCharacterArt) IS that character (party members
// placed as room models in scenes): that region draws the character's art - its model/fieldobj/player/<n>/<n>/clut0.png replacement, or in
// the Modded art mode its hd.dat 4x texture - and its tiles are not dumped (the field character's own file holds them).
#include <optional>
#include <string>
#include <vector>

#include "assets/model.h"
#include "core/types.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::render {

struct ModelTile {
  u32 texture = 0;
  u32 x = 0, y = 0, w = 0, h = 0;
  // texture_content_tag of the tile's pixels.
  std::string hash;
};
std::vector<ModelTile> model_tiles(const assets::ModelAsset& asset);

// The field characters' RDE 1x textures (cdrom2 fieldobj/player/<n>/<n>.tim). A room's copy of a character's TIM is the same picture
// converted to PS1 colours on its own (other indices, other CLUT), so it is compared by colour, not bit for bit.
class FieldCharacterArt {
 public:
  explicit FieldCharacterArt(const vfs::Vfs& vfs);
  struct Match {
    // Member stem, e.g. "fieldobj/player/kid/kid".
    std::string stem;
    u32 w = 0, h = 0;
    // Over the texels the model samples that the character's texture covers (opaque): the mean |RGB difference| (0..255; a texel the page
    // leaves transparent counts 255) and the correlation of their luminance (1 = the same picture, whatever the palette conversion did).
    double score = 0, correlation = 0;
  };
  // The character closest (by score) to the top-left of `asset`'s page 0, over the texels its tiles (model_tiles) sample; nullopt when none fits.
  std::optional<Match> closest(const assets::ModelAsset& asset, const std::vector<ModelTile>& tiles) const;
  // closest() when it is the same picture in the same colours (kMinCorrelation, kMaxScore): the model's TIM is that character's texture.
  // A copy the scene recoloured (a darker / greyer grading) is not one: it keeps its own art.
  std::optional<Match> match(const assets::ModelAsset& asset, const std::vector<ModelTile>& tiles) const;
  static constexpr double kMinCorrelation = 0.95, kMaxScore = 40.0;

 private:
  struct Entry {
    Match match;
    import::Image image;
  };
  std::vector<Entry> characters_;
};

// Writes the tiles of a room model (not those inside a field character's region) under the dump root. Returns the files written.
u32 dump_room_model_tiles(const assets::ModelAsset& asset, const FieldCharacterArt& characters, bool forced);
// The live hook for a room model (render::resolve_model_textures): dumps its tiles when [Mods] DumpTextures is on and fills mod_textures from
// the tile replacements and the field character art. `vfs` = the game's (its hd.dat, the Modded flag, the mods).
void resolve_room_model_textures(assets::ModelAsset& asset, const vfs::Vfs& vfs);

// The models of the battle effect packs (summon creatures, tech figures) draw from two cuts of their pack's VRAM and are tiled the same
// way, with no field character rule: <category>/tile_<hash8>.png, category "effect_model". dump_model_tiles writes every tile and returns
// the files written; resolve_model_tiles is the live hook (render::resolve_model_textures), as resolve_room_model_textures.
u32 dump_model_tiles(const assets::ModelAsset& asset, const std::string& category, bool forced);
void resolve_model_tiles(assets::ModelAsset& asset, const std::string& category);

}  // namespace oc::render
