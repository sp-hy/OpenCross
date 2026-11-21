#pragma once
// Room importer: <room>.bin + <room>.ctd (+ .prd listed only) -> assets::RoomScene.
// Nothing here throws across the API; failures come back as std::nullopt plus a message.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/room_scene.h"
#include "core/types.h"
#include "import/tim.h"

namespace oc::import {

// Room geometry constants.
constexpr int kRoomTileSize = 16;
// VRAM row of background texture space (row = 256 + tile v).
constexpr int kBgTextureRow = 256;
// .bin subfile 1 lands here = VRAM row 224, x 0.
constexpr size_t kClutVramByteOffset = 0x70000;
constexpr int kClutVramRow = 224;

// Puts the .bin CLUT subfile (k palettes of 512 bytes) into VRAM: palette k at (0, 224 + k).
void write_room_cluts(Vram& vram, ByteSpan clut_subfile);

// Where a tile's 16x16 8bpp source lives in VRAM. Texel (i, j) = byte (i & 1) of halfword (u + i / 2) in row y + j,
// and its colour is VRAM(0 + index, clut_y).
struct TileSource {
  int tex_x;   // in 8bpp texel units (= 2 * u)
  int tex_y;   // VRAM row of the first texel row
  int clut_x;  // VRAM position of the 256-entry palette
  int clut_y;
};
constexpr TileSource tile_source(u16 u, u8 v, u8 clut) {
  return {2 * static_cast<int>(u), kBgTextureRow + static_cast<int>(v), 0, kClutVramRow + (static_cast<int>(clut) >> 3)};
}

// `prd` may be empty. On failure returns nullopt and, if `error` is non-null, a message.
std::optional<assets::RoomScene> load_room(std::string_view name, ByteSpan bin, ByteSpan ctd, ByteSpan prd = {},
                                           std::string* error = nullptr);
// Only the room's models (RoomScene::models, their texture pages cut from the room's VRAM): the .bin / .ctd read without the layers,
// script or effects, for a model shown in another room.
std::optional<std::vector<assets::RoomModelHeader>> load_room_models(ByteSpan bin, ByteSpan ctd, std::string* error = nullptr);
// A fingerprint of what a room's built art comes from: the .ctd and every .bin subfile but the ones a script, walk mesh or zone edit
// changes (zones, script, walk triangles / vertices, walk attributes, dialogue). Equal keys: the backgrounds, palettes, camera and room
// models are the same (an editor's reload of an edited room keeps its built art). nullopt: not a room .bin.
std::optional<u64> room_art_key(ByteSpan bin, ByteSpan ctd);
// The other half: what load_room reads from the subfiles room_art_key leaves out (walk mesh, walk attributes, zones, script, dialogue),
// written over `scene`. With the art key unchanged, `scene` then equals a fresh load_room, without composing the backgrounds and models
// again (most of a room import: an editor's reload and a controlled run's restart skip it). On failure `scene` is untouched.
bool reload_room_logic(std::string_view name, ByteSpan bin, assets::RoomScene& scene, std::string* error = nullptr);

struct RoomValidation {
  std::vector<std::string> errors;    // format violations: a room with errors counts as a failure
  std::vector<std::string> warnings;  // known data anomalies (walk-mesh adjacency quirks)
  u32 layers = 0, tiles = 0;
  bool bbox_match = false;            // tile bbox == (-x_max,-y_max)..(320-x_min,240-y_min)
  bool extra_clut_tims = false;       // .ctd has CLUT TIMs no model header references
  bool ok() const { return errors.empty(); }
};

// Runs the room format checks (subfile sizes and magics, tile flags / CLUT / alignment / texture coverage, camera
// bbox, walk-mesh indices and adjacency, model headers against the .ctd CLUT TIMs). Never throws.
RoomValidation validate_room(ByteSpan bin, ByteSpan ctd);

}  // namespace oc::import
