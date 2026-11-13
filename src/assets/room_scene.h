#pragma once
// Engine-side description of one field room. Plain data: no PS1 types, no VRAM.
// Produced by import::load_room(); consumed by the game/render layers.
#include <array>
#include <string>
#include <vector>

#include "assets/room_effects.h"
#include "core/types.h"
#include "import/tim.h"  // import::Image (plain struct)

namespace oc::assets {

// Camera / scroll block (.bin subfile 0). matrix is 3x3, 4.12 fixed (4096 = 1.0), row-major; trans is the translation and proj_h the
// projection distance H (both inferred); x_max..y_min are the pan limits; group_scale is the parallax factor of each tile group
// (4096 = 1.0).
struct RoomCamera {
  std::array<s16, 9> matrix{};
  std::array<s16, 3> trans{};
  s16 proj_h = 0;
  s16 x_max = 0, x_min = 0;
  s16 y_max = 0, y_min = 0;
  s16 unknown34 = 0;
  u32 n_groups = 0;
  std::vector<u32> group_scale;
};

// One background tile record (12 bytes in the file), kept for depth / animation work. x, y: screen position of the top-left corner
// (16x16 tiles, 1x); u: VRAM halfword column of the 8bpp source; v: source row within texture space (VRAM row 256 + v); clut: 8 *
// palette row; flags: bit7 semi-transparent, bits3-4 ABR; z: larger = farther; index: index within the whole room's tile array.
struct RoomTile {
  s16 x = 0, y = 0;
  u16 u = 0;
  u8 v = 0;
  u8 clut = 0;
  u8 group = 0;
  u8 flags = 0;
  u16 z = 0;
  u32 index = 0;
};

// All tiles of one (layer, group) composed into RGBA8 images, one per blend kind: `blend` -1 = opaque (tiles without bit 7 and the
// non-STP texels of semi-transparent tiles), 0..3 = the STP texels of the semi-transparent tiles with that ABR (texture A / texture B
// split). A semi-transparent tile gives its group both kinds, so one of them may be empty in the 1x art and filled in the HD art.
// All images of a group share the bounding box of the group's tiles within the layer. x, y: top-left in room (screen)
// coordinates; rgba: width*height*4, alpha 0 where the texel colour is 0.
struct RoomGroupImage {
  s32 blend = -1;
  u32 group = 0;
  s32 x = 0, y = 0;
  u32 width = 0, height = 0;
  u16 z = 0;
  std::vector<u8> rgba;
};

// groups are in ascending group number.
struct RoomLayer {
  u32 index = 0;
  std::vector<RoomTile> tiles;
  std::vector<RoomGroupImage> groups;
};

// adj is the neighbour across edge (v[k], v[(k+1)%3]), or -1.
struct WalkTriangle {
  std::array<u16, 3> v{};
  std::array<s16, 3> adj{};
  s16 attr = 0;
};

// vertices are in world units, Y down.
struct WalkMesh {
  std::vector<std::array<float, 3>> vertices;
  std::vector<WalkTriangle> triangles;
};

// size is the whole subfile; mdl is the Mdl pack after the 12-byte header (offsets relative to it); texture is the RGBA8 of the .ctd
// TIM named by (img, clut), empty when no TIM matches.
struct RoomModelHeader {
  u32 eof_rel = 0;
  u16 img_x = 0, img_y = 0, clut_x = 0, clut_y = 0;
  u32 size = 0;
  std::vector<u8> mdl;
  import::Image texture;
  // What the model's faces can sample: the 256-texel window of its page + the page to the right, from the room's
  // composed VRAM, coloured with CLUT row clut_y ([0]) and clut_y + 1 ([1]). Input of import::load_room_model.
  std::vector<import::Image> page_textures;
};

// Trigger zone (subfile 2): an XZ quad of four (x, y, z) corners.
struct RoomZone {
  std::array<std::array<s16, 3>, 4> corner{};
};

struct RoomPrdEntry {
  std::string name;
  u8 type = 0;
  u32 size = 0;
};

// attr_flags: subfile 6, walk-mesh attr -> flag word (little-endian u32 entries); models: room models (subfiles 11+), the index is the
// SETUP_NPC model number; zones: trigger zones (subfile 2), the index is the zone operand of the collision ops; prd: listed only
// (decoded payloads are in `effects`); effects: .prd sprite sheets / emitters / sequences (empty when the room has no .prd);
// script_bytes: subfile 3 decoded, the field script module; yos1_bytes / yos0_bytes: subfiles 9 / 10 raw, room SFX frames /
// instrument banks; dialogue_bytes: subfile 8 decoded, the dialogue block.
struct RoomScene {
  std::string name;
  RoomCamera camera;
  u32 clut_rows = 0;
  std::vector<RoomLayer> layers;
  WalkMesh walk;
  std::vector<u32> attr_flags;
  std::vector<RoomModelHeader> models;
  std::vector<RoomZone> zones;
  std::vector<RoomPrdEntry> prd;
  RoomEffects effects;
  u32 ctd_tims = 0;
  std::vector<u8> script_bytes;
  std::vector<u8> yos1_bytes, yos0_bytes;
  std::vector<u8> dialogue_bytes;

  size_t group_image_count() const {
    size_t count = 0;
    for (const auto& layer : layers) count += layer.groups.size();
    return count;
  }
};

}  // namespace oc::assets
