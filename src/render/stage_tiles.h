#pragma once
// Tile library of a stage-like asset (battle stage, title scene, room effect meshes, the battle effect packs' meshes).
//
// The PS1 scenes sample (colour mode, CLUT) atlases of a VRAM area. bake_object_textures() cuts every texel rect the faces sample (rects that overlap merge;
// neighbours stay separate) out of the atlases - through their own CLUT, so plain RGBA - into TILES, de-duplicated by exact pixel content (size + RGBA incl.
// alpha) within the asset. The tile is the unit that is dumped and replaced. Every object (part) still draws ONE texture: its tiles shelf-packed with a 1-texel
// edge-extended gutter, with the part's UVs remapped to it. At load the tiles' replacements (mods) are applied and the object images are re-composed at the
// largest scale among their tiles (apply_tile_textures / recompose_object_textures).
#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "assets/battle_stage.h"
#include "core/types.h"

namespace oc::render {

// Shelf packing of `sizes` (w, h) rects with `pad` texels of gutter on every side: the slots go left to right on shelves (tallest first) in an image whose
// width is a power of two. `pos[i]` = top-left of rect i's SLOT (its texels start at pos + pad); the image is `w` x `h`.
struct ShelfPacking {
  std::vector<std::array<int, 2>> pos;
  int w = 0, h = 0;
};
ShelfPacking shelf_pack(const std::vector<std::array<int, 2>>& sizes, int pad);

// Builds StageAsset::tiles and ::object_textures from the atlases (see above) and remaps the parts' UVs / sets StageSubMesh::object_texture. A vertex shared by
// two atlases is duplicated at the END of the part's vertex list (the others keep their indices: op-37 morph pairs index two parts' vertices alike). Parts used
// by a UV-scroll emitter (op 35 bit 0) get the op-36 texture window of their page (win 0 = the whole page; clipped to the atlas) so the window wrap keeps working. Pure CPU; call once per asset.
//
// `uses` is for an asset whose parts are drawn by emitters it does not hold (a battle effect pack's meshes: the pack's, the resident and the library emitters name
// them, and the overlays). Every part is baked, whether an emitter of the asset names it or not. A part named in `scroll` gets, per texture page, ONE rect: the
// bounds of its faces' texels and of every listed window, so the same tile serves the part drawn through any of those windows and drawn without a scroll. The
// direct-colour sub-meshes (StageSubMesh::direct_page: they show a copy of the picture, not texels) stay on their atlas. StageObjectTexture::windows and
// ::atlas_uv are filled, so the caller can draw an object through a window the bake was not told of from the raw atlases (StageInstance::atlas), and
// StageAsset::atlases_on_demand is set.
struct TileUses {
  // Mesh name -> the op-36 windows (off_u, off_v, win_w, win_h) its emitters scroll it through.
  std::map<std::string, std::vector<std::array<int, 4>>> scroll;
};
void bake_object_textures(assets::StageAsset& stage, const TileUses* uses = nullptr);
// False when `part` is baked and its tiles do not hold the art of the op-36 texture window `window` (no window, or others, were named for it).
bool tiles_hold_window(const assets::StageAsset& stage, int part, const std::array<int, 4>& window);

// A tile's replacement: fills w / h / rgba (any size) and returns true, or returns false (the tile stays). Called for several tiles at once from worker threads.
using TileReplacer = std::function<bool(const assets::StageTile& tile, u32& w, u32& h, std::vector<u8>& rgba)>;
// Re-composes the object images that contain a replaced tile: scale S = the largest (replacement size / tile size) among the object's tiles (un-replaced tiles
// count 1 and are enlarged nearest), slots are w * S x h * S, UVs follow (uv_scale = texel size at the new scale). Call once, right after the bake.
// Returns the number of replaced tiles.
int recompose_object_textures(assets::StageAsset& stage, const TileReplacer& replace);

// [Enhancements] EdgeBlend: fills StagePart::seams for every baked object (any art mode, modded or not; the strips scale with the
// texture). Two triangles of a part that share an edge (same two positions) but sample different texels across it - another tile, the same tile at
// another place, mirrored - show the upscaler's disagreement as a hard line once each tile was upscaled on its own. For each such pair, both packets get a
// StageSeam (a quad's two triangles count as one packet, its diagonal is no edge): the neighbour's UV mirrored across the edge per corner and each corner's
// distance in bands of kBandTexels original texels (the band at most half of any corner's height); `flat` = the faces are within 30 degrees of coplanar (recorded only). The renderer (StagePipeline) cross-fades the two faces along the seam from these. Untextured
// faces, scrolling (op-36) objects and edges between different parts are not paired. Returns the number of seams.
int build_tile_seams(assets::StageAsset& stage);

// A raw atlas texel (the overlays' packets address mode + CLUT atlases) located in the composed object textures.
struct TileTexel {
  // StageAsset::object_textures index (its texture is atlases + object in StageMesh::sets).
  int object = -1;
  // The merged rect: corners of one packet must share it.
  int rect = -1;
  // The texel's centre in that object image (pixels, at its CURRENT scale: follows recompose).
  float px = 0, py = 0;
  // Pixels per original texel.
  float fx = 1, fy = 1;
};
// False when no stage face samples the texel (a packet may sample anything): the caller falls back to the raw atlas.
bool tile_lookup(const assets::StageAsset& stage, int atlas, int x, int y, TileTexel& out);

// Canonical paths (render/texture_registry.h layout): group = "battle_stage/bg_05" | "title" | "room_mesh" | "effect_mesh". Path-named groups: <group>/<tile name>.png and
// content-named ones (room_mesh, effect_mesh): <group>/tile_<hash8>.png (identical tiles are one file across rooms / packs). No manifest is written (the hash in the name is enough).
std::string tile_path(const std::string& group, const assets::StageTile& tile);
// Writes the tile PNGs of a baked asset under the dump root (forced = regardless of [Mods] DumpTextures). Returns the files written.
u32 dump_tiles(const assets::StageAsset& stage, const std::string& group, bool forced);
// The live hook (after bake_object_textures, before the pipeline sees the asset): dumps when [Mods] DumpTextures is on, applies the active mods' tile
// replacements (stage folder by hash8, then the global tiles/<hash8>.png), re-composes and rebuilds the parts' seam strips (build_tile_seams).
// No-op when texture_registry::active() is false.
// `atlas_files`: a tile with no file of its own is cut from a replacement of the whole atlas it comes from, at that image's scale (the content-named
// <group>/<hash8>.png the effect meshes had before they were tiled; render::stage_group_key).
void apply_tile_textures(assets::StageAsset& stage, const std::string& group, bool atlas_files = false);

}  // namespace oc::render
