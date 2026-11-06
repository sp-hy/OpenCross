#pragma once
// Room .prd effects importer: timinfo sheets (0x03), emitters (0x0A), sequences (0x10).
// Nothing throws across the API; per-entry failures are collected in RoomEffects::errors and the entry is skipped.
#include <string>
#include <string_view>
#include <vector>

#include "assets/room_effects.h"
#include "import/tim.h"

namespace oc::import {

// Parses a room .prd (drp). The type-0x04 TIMs are uploaded onto a copy of `base` (the optional shared library
// `lib` = effect/particle.prd is uploaded first, room TIMs last) and the sprite images are cut from that.
assets::RoomEffects load_room_effects(ByteSpan prd, const Vram& base, ByteSpan lib = {});

// Battle effect pack (battle/effects/*/NAME.prd + optional NAME.ctd): the ctd rows are uploaded to the VRAM copy first,
// sequences accept the ~100 battle ops (lenient framing), camera/AKAO/curve entries are kept as blobs.
assets::RoomEffects load_battle_effects(ByteSpan prd, ByteSpan ctd, const Vram& base, ByteSpan lib = {});

// RDE's 4x page of one sheet: `hd` is a TIM four times the size of the sheet's texture rect (its header position is not used), placed at
// the sheet's texture position, its CLUT (when it has one) at the sheet's CLUT position. Fills EffectSprite::hd_rgba of every sprite whose
// texels lie inside that rect (same depth as the TIM): the 4x texels through the CLUT the sprite names, read from `vram` with the page's
// CLUT written in. Returns the number of sprites filled.
size_t cut_hd_sheet(assets::SpriteSheet& sheet, const Vram& vram, const Tim& hd);

// Payload of the nested type-0x01 pack `name` of a drp (a drp itself whose entries the engine links into the same name
// registry as the outer file's), empty when absent. effect/particle.prd > 'fenc' holds the field encounter effect 'enct';
// load it with load_battle_effects (its sequences use battle ops) and its meshes with load_stage.
Bytes nested_pack(ByteSpan prd, std::string_view name);

// Op metadata. `known` is false for ops absent from the spec table (those make a stream fail).
const char* effect_op_name(u16 op);
const char* sequence_op_name(u16 op);

struct EffectsValidation {
  std::vector<std::string> errors;    // stream failures (incl. SPRITE cache != 0, emitter without SPRITE)
  std::vector<std::string> warnings;  // timinfo / TIM position mismatches
  u32 emitters = 0, sheets = 0, sequences = 0, frames = 0;
  u32 sprites = 0, sprites_inside = 0;
  u32 pair_ok = 0, pair_mismatch = 0, unpaired = 0;
  bool ok() const { return errors.empty(); }
};

EffectsValidation validate_room_effects(const assets::RoomEffects& fx);

}  // namespace oc::import
