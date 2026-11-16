#pragma once
// Engine-side battle stage (battlefield) asset, produced by import::load_stage().
// Plain float data, no PS1 types.
//
// Space: parts and camera eye/target are in one shared world space, model units, y UP (the importer negates the
// file's +y-down coordinates and reverses face winding). Colours are 1.0 = PS1 0x80 (neutral).
// UVs are normalised to the texture the part's sub-mesh names (origin top-left, v down).
#include <array>
#include <string>
#include <vector>

#include "core/types.h"
#include "import/tim.h"  // import::Image only

namespace oc::assets {

// uv_raw is the packet u, v (texels inside the 256 x 256 texture page), for the op-36 texture-window scroll and the nearest texel pick; uv_scale the size of one
// texel in `uv` units (1 / atlas width, 1 / 256), 0 for untextured corners; centre the centroid of the packet this corner belongs to
// (same space as pos): the PS1 sorts whole packets by their average camera z (ordering table), so the renderer takes the depth of a
// face from here.
struct StageVertex {
  std::array<float, 3> pos{};
  std::array<float, 2> uv{};
  std::array<float, 3> color{1, 1, 1};
  std::array<float, 2> uv_raw{};
  std::array<float, 2> uv_scale{};
  std::array<float, 3> centre{};
};

// texture indexes StageAsset::textures (-1 = untextured, vertex colour only); semi is packet code bit 1: the faces are semi-transparent
// in the data (the drawing object can add the bit too); abr is the tpage blend mode of the faces (0 B/2+F/2, 1 B+F, 2 B-F, 3 B+F/4),
// used when `semi` and the object is opaque; object_texture indexes StageAsset::object_textures after import::bake_object_textures
// (the part's own image; the vertex UVs address it), -1 = not baked (then `texture` and the atlas UVs apply). direct_page is the VRAM
// origin of the texture page of 15-bit direct-colour faces (x halfwords | y rows << 16; such a page can be a copy of the screen, see
// render::StageInstance::screen), -1 for indexed and untextured faces.
struct StageSubMesh {
  int texture = -1;
  u32 first_index = 0, index_count = 0;
  bool semi = false;
  int abr = 0;
  int object_texture = -1;
  int direct_page = -1;
};

// A seam strip of one textured packet (render::build_tile_seams): one of the packet's edges is shared with a
// neighbour whose texels do not continue across it (another tile, the same tile elsewhere, mirrored). The renderer draws the whole packet again (a quad as
// its two triangles, so the band reaches the corners on both ends of the edge), alpha-blended over its own face, with the NEIGHBOUR's texture mirrored
// across the edge (`uv` = the neighbour's UV at each corner's reflection), at alpha 1/2 on the edge fading to 0 one band away (`g` = each corner's
// distance from the edge in bands; 0 on the edge): both faces cross-fade into each other along the seam.
// v: part vertex indices in packet order, (0,1,2) or the quad (0,1,2) (2,1,3); uv: the neighbour's mirrored UV per corner
// (StageVertex::uv convention); g: per corner, the distance from the seam edge in band widths (the fragment fades 1/2 -> 0 over 0..1);
// corners: 3 or 4; index_pos: the packet's first index in StagePart::indices; submesh: StagePart::submeshes index (texture); flat: the
// two faces are within 30 degrees of coplanar (recorded; every seam is drawn).
struct StageSeam {
  std::array<u32, 4> v{};
  std::array<std::array<float, 2>, 4> uv{};
  std::array<float, 4> g{};
  u8 corners = 3;
  u32 index_pos = 0;
  int submesh = -1;
  bool flat = false;
};

// name: drp entry name (not unique within a stage); entry: drp entry index; ground: type 0x12 `ring` mesh, else type 0x02 scenery;
// source_vertices / source_faces: header counts of the packet mesh (before corner splitting); semi_transparent_faces: packets with
// code bit 1 (carried, not interpreted); indices: triangles in the packet's corner order, (0,1,2), quads add (2,1,3) - the GPU draws a
// face only when NCLIP(0,1,2) > 0 on screen (x right, y down), so renderers must cull the other side (render::StagePipeline keeps the
// mapping to the pipeline's front-face rule); submeshes: runs of equal (texture, semi, abr), in face order; emitters: names of the type
// 0x0A entries that name this mesh; seams: render::build_tile_seams (after the bake / every re-compose; any art mode);
// cull_points: the six file-space points of the object screen test (flag bit 28) taken from the whole vertex array's bounds: the
// centre, (max x, 0, 0), (min x, 0, 0), (0, 0, max z), (0, 0, min z), (0, min y, 0).
struct StagePart {
  std::string name;
  u32 entry = 0;
  bool ground = false;
  u32 source_vertices = 0;
  u32 source_faces = 0;
  u32 semi_transparent_faces = 0;
  std::array<std::array<s16, 3>, 6> cull_points{};
  std::vector<StageVertex> vertices;
  std::vector<u32> indices;
  std::vector<StageSubMesh> submeshes;
  std::vector<std::string> emitters;
  std::vector<StageSeam> seams;
};

// One decoded atlas per (colour mode, CLUT row): the stage's whole VRAM image area decoded with that palette. mode: 0 = 4bpp,
// 1 = 8bpp; clut_x / clut_y: halfwords / row of the CLUT in VRAM; image: RGBA8, width = atlas_width_hw * (4 or 2), height = 256.
struct StageTexture {
  int mode = 1;
  int clut_x = 0, clut_y = 0;
  import::Image image;
};

// One TILE of the stage's tile library (render::bake_object_textures): a texel rect the faces sample, cut from the (mode, CLUT) atlases through the CLUT (plain
// RGBA, colour 0 = alpha 0). Identical content (size + RGBA) is ONE tile however many places of the atlases it comes from. The unit of dump / replacement.
// Members: Source = an atlas (StageAsset::textures index) and the top-left texel in it; name = file stem "tile_03_1a2b3c4d"
// (tile_<first-use order>_<content hash>); hash = 8 hex digits, FNV-1a over size + RGBA (render::texture_content_tag); image = RGBA8
// at the original resolution (never changed); users = object name -> triangles of that object sampling the tile.
struct StageTile {
  struct Source {
    int atlas = 0;
    int sx = 0, sy = 0;
  };
  std::string name;
  std::string hash;
  int w = 0, h = 0;
  import::Image image;
  std::vector<Source> sources;
  std::vector<std::pair<std::string, u32>> users;
};

// Per-object texture: the object's tiles shelf-packed into ONE image with a 1-texel edge-extended gutter; the part's UVs address it (normalised).
// Slot: tile = StageAsset::tiles index; dx, dy = top-left of the slot in `image` (gutter included: the texels start at dx + 1, dy + 1);
// w, h = slot size without the gutter (the tile's size at scale 1). name = "obj_03_scenery_nea7" (obj_<part index>_<ground|scenery>
// [_<drp entry name>]); part = StageAsset::parts index; scrolls = an op-36 (UV scroll) emitter uses the part, so whole texture pages
// are in its tiles; scale = image scale against the original texels (> 1 after a tile replacement); image = RGBA8, current (original,
// or composed at `scale` after tile replacements); slots = the current layout; base_uv = per vertex of the part: uv.x, uv.y,
// uv_scale.x, uv_scale.y; composed = the current image / UVs differ from the base (a tile is replaced). windows = the op-36 texture
// windows (off_u, off_v, win_w, win_h) whose art the tiles of a scrolling part hold; atlas_uv = per vertex of the part, the uv and
// uv_scale it had on the raw atlases (only for a bake with render::TileUses: an object drawn through a window that is not in `windows`
// is drawn from the atlases).
struct StageObjectTexture {
  struct Slot {
    int tile = 0;
    int dx = 0, dy = 0;
    int w = 0, h = 0;
  };
  std::string name;
  int part = -1;
  bool scrolls = false;
  float scale = 1;
  import::Image image;
  std::vector<Slot> slots;
  // The pristine state recorded by the bake; render::recompose_object_textures always starts from it (re-runnable, never accumulates).
  std::vector<Slot> base_slots;
  int base_w = 0, base_h = 0;
  std::vector<std::array<float, 4>> base_uv;
  bool composed = false;
  std::vector<std::array<int, 4>> windows;
  std::vector<std::array<float, 4>> atlas_uv;
};

// Where the atlas texels went (render::tile_lookup): lets code that still addresses the raw (mode, CLUT) atlases - the overlays' ground cells / effect polygons
// (StagePrim) - sample the composed, possibly replaced object textures instead.
// Rect: the merged texel rect in an atlas, and its StageAsset::tiles index. Home: the first object texture holding the tile (any is
// fine: the pixels are identical) and its slot. id_map: per atlas, row-major, the rect index covering the texel (-1 = no stage face
// samples it); home: per tile.
struct StageTileMap {
  struct Rect {
    int atlas = 0, sx = 0, sy = 0, w = 0, h = 0;
    int tile = 0;
  };
  struct Home {
    int object = -1, slot = -1;
  };
  std::vector<Rect> rects;
  std::vector<std::vector<int>> id_map;
  std::vector<Home> home;
};

// projection is the GTE projection distance H; roll_deg is the raw roll * 360 / 1024.
struct StageCameraKey {
  std::array<float, 3> eye{}, target{};
  float projection = 512;
  float roll_deg = 0;
  s16 roll_raw = 0;
  u32 w0 = 0;
};

// name is a camera name (zen0, i01, o01, p01, ...); duration is in ticks.
struct StageCamera {
  std::string name;
  u32 flags = 0;
  u16 duration = 0;
  u16 mode = 0;
  u32 w8 = 0;
  std::array<u8, 4> ref{};
  std::vector<StageCameraKey> keys;
};

// offset is within the entry payload; length is in bytes including the 4-byte word; target is the 4-char name at +0xC (camera / entry
// / sound) or empty; data holds the bytes after the word.
struct StageSeqCommand {
  u32 offset = 0;
  u16 op = 0;
  u16 length = 0;
  bool jump = false;
  std::string target;
  Bytes data;
};

// trailing counts the bytes after the 0x21 terminator.
struct StageSequence {
  std::string name;
  std::vector<StageSeqCommand> commands;
  u32 trailing = 0;
  bool trailing_is_pad = true;
};

struct StageLensSprite {
  s16 x = 0, y = 0;
  u8 u = 0, v = 0;
  u16 clut = 0;
  u8 w = 0, h = 0, m = 0, kind = 0;
};

// raw is the whole type 0x15 payload.
struct StageLens {
  std::string name;
  Bytes raw;
  std::vector<std::vector<StageLensSprite>> tables;
};

// A type 0x0A object script of a stage. Static scenery objects (LIFE 0, no velocity) place one instance of a mesh: world position `pos`, rotation `angle`
// (4096 = turn, libgte RotMatrix order Rz * Ry * Rx), scale `scale` (4.12), all in PS1 space (y down); `bias` = OT_BIAS (larger = farther back).
// mesh is the mesh name found in the script (empty if none); sprite the op 05 sprite name as written (mesh, sheet, `sun0` light, `lenz`
// flare, ...); placed = the script has a POS op; color is op 20 (12-bit, 0x800 = neutral; the draw colour is color >> 4); flags is op 06:
// bits 0-2 blend, 17 depth-cue, 26 world-oriented, 28 cull-test; ext is the op 35 extended flag word (bit 0 UV scroll, 4 reversed
// cull, 5 no cull); uv_window is the op 36 texture window: off_u, off_v, win_w, win_h in page texels (win 0 = the whole 256 page);
// sprite_type is the op 05 type: 14 scenery mesh, 19 ring, 3 sheet, 15 built-in, 21 lens.
struct StageEmitter {
  std::string name;
  std::string mesh;
  std::string sprite;
  bool placed = false;
  std::array<int, 3> pos{0, 0, 0};
  std::array<int, 3> angle{0, 0, 0};
  std::array<int, 3> scale{4096, 4096, 4096};
  std::array<int, 3> vel{0, 0, 0};
  std::array<int, 3> color{2048, 2048, 2048};
  int life = 0, bias = 0;
  u32 flags = 0;
  u32 ext = 0;
  std::array<int, 4> uv_window{0, 0, 0, 0};
  int sprite_type = 0;
  int blend() const { return static_cast<int>(flags & 7); }
  bool is_static() const { return life == 0 && vel[0] == 0 && vel[1] == 0 && vel[2] == 0; }
};

struct StageAsset {
  std::string name;
  // Texture dump / replace group "category/name" ("battle_stage/bg_07", "title", "effect_mesh"): names the atlases for
  // render/texture_registry.h. Empty = the caller's pipeline default (the battle stage) or no dump / replace.
  std::string tex_group;
  std::vector<StagePart> parts;
  std::vector<StageTexture> textures;
  // Tile library + baked per-object textures (empty until render::bake_object_textures). `textures` stays: effect polygons (StagePrim), the title's model
  // pages and the ground cells' dynamic draws still address the atlases.
  std::vector<StageTile> tiles;
  StageTileMap tile_map;
  std::vector<StageObjectTexture> object_textures;
  // Set by a bake with render::TileUses (an effect pack's meshes): nothing but the sub-meshes left on their atlas and the objects drawn from
  // the atlases (render::StageInstance::atlas) samples the atlases, so the renderer makes only the former's at creation and the rest when one
  // is first asked for; the opaque runs keep the draw order they had on the atlases.
  bool atlases_on_demand = false;
  // VRAM x (halfwords) of atlas column 0 (a multiple of 64), the atlas width in halfwords (8bpp pixels = 2x, 4bpp = 4x), and the
  // VRAM row of atlas row 0.
  int atlas_x0_hw = 0;
  int atlas_width_hw = 384;
  int atlas_y0 = 256;
  std::vector<StageCamera> cameras;
  std::vector<StageSequence> sequences;
  std::vector<StageLens> lens;
  std::vector<StageEmitter> emitters;
  u32 tim_count = 0;
  // The stock env page (tpage 0x95 = (320, 256) 8 bit, CLUT 0x3BC0 = (0, 239) of the battle VRAM the stage TIMs / ctd filled): 256 x 256 RGBA with STP
  // kept in alpha. Empty when the stage has no image there.
  std::vector<u8> env_rgba;

  // Texture for a (mode, CLUT x, CLUT y) triple or -1.
  int find_texture(int mode, int clut_x, int clut_y) const {
    for (size_t i = 0; i < textures.size(); ++i)
      if (textures[i].mode == mode && textures[i].clut_x == clut_x && textures[i].clut_y == clut_y) return static_cast<int>(i);
    return -1;
  }
};

// Packet words to VRAM. x is in halfwords; y includes the bit-11 +512.
struct StageTpage {
  int x = 0, y = 0, mode = 0, abr = 0;
};
inline StageTpage decode_tpage(u16 tpage) {
  return {(tpage & 15) * 64, ((tpage >> 4) & 1) * 256 + ((tpage >> 11) & 1) * 512, (tpage >> 7) & 3, (tpage >> 5) & 3};
}
inline std::array<int, 2> decode_clut(u16 clut) { return {(clut & 0x3F) * 16, clut >> 6}; }

}  // namespace oc::assets
