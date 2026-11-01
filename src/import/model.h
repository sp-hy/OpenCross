#pragma once
// Model importer: Mdl .obj (+ .ltd/.ltc or .tim textures, external anim packs) -> assets::ModelAsset.
// Nothing throws across the API; failures come back as std::nullopt plus a message.
#include <optional>
#include <string>
#include <vector>

#include "assets/model.h"
#include "core/types.h"

namespace oc::import {

// Decodes an RDE HD .tim (hd.dat: standard TIM layout, 4x the 1x pixel size, own CLUT) to RGBA. nullopt if malformed.
std::optional<Image> load_hd_tim(ByteSpan bytes);

// `hd_tim` (optional, last argument of load_model / load_field_model) is the matching hd.dat TIM; when its size is an
// integer multiple of the 1x main texture it fills ModelAsset::hd_textures[0] / hd_scale, otherwise it is ignored.
// Battle player/monster model. `tex` is an .ltd or .ltc (may be empty), `at0_prd` the type-0x19 holder
// (at0.prd, may be empty; its clips are appended with source "at0").
std::optional<assets::ModelAsset> load_model(ByteSpan obj, ByteSpan tex, ByteSpan at0_prd, std::string* err = nullptr,
                                          ByteSpan hd_tim = {});

// Field model: fieldobj/player/<n>/<n>.obj + <n>.tim (+ fieldobj/ex/<n>.bin whose entry 0 is an anim pack).
std::optional<assets::ModelAsset> load_field_model(ByteSpan obj, ByteSpan tim, ByteSpan ex_bin, std::string* err = nullptr,
                                                ByteSpan hd_tim = {});

// Room model: the Mdl pack that follows the 12-byte header of a .bin subfile 11+, with the RGBA texture of
// the .ctd TIM its header names (assets::RoomModelHeader::mdl / ::texture). Own animations only (S4), no ex pack.
std::optional<assets::ModelAsset> load_room_model(ByteSpan mdl, const Image& texture, std::string* err = nullptr);
// Room model textured as the engine draws it: `clut_rows` = assets::RoomModelHeader::page_textures, the model's
// texture page plus the page to its right (256 texels wide) coloured with CLUT row clut_y ([0]) and clut_y + 1 ([1]), so faces carrying
// the CLUT / page bits (the Hydra Marshes hydra and 56 other room models) sample the neighbouring palette / page.
std::optional<assets::ModelAsset> load_room_model(ByteSpan mdl, const std::vector<Image>& clut_rows, std::string* err = nullptr);

// The page_textures of a room model: `rows` RGBA images of the 8bpp VRAM window at (img_x halfwords, img_y), 256 texels wide and
// min(256, 512 - img_y) rows, coloured with CLUT rows clut_y, clut_y + 1, ... read at (clut_x, clut_y + k).
std::vector<Image> vram_page_textures(const Vram& vram, u16 img_x, u16 img_y, u16 clut_x, u16 clut_y, u32 rows = 2);

// The anim pack of a fieldobj/ex/*.bin (LOAD_ANIMATION C1) decoded against the skeleton of any field Mdl: a character
// .obj or a room model's Mdl pack (an NPC such as dem_02's Zoah). The engine hangs the pack on the entity's display model whatever
// the model came from; the clips are relative to that model's rest pose. Clip source "ex", index k = SET_ANIMATION 0x80 + k.
// Fails when the pack's joint count differs from the model's.
std::optional<std::vector<assets::AnimationClip>> load_ex_clips(ByteSpan obj, ByteSpan ex_bin, std::string* err = nullptr);
// An animation pack (a cpt of animations, e.g. the summon motion streams `battle/effects/syoukan/st_0N.mtc`
// attached to the 'sara' model as the external clip table) decoded against the skeleton of a Mdl of any
// class, the type-0x0B record of an effect drp included. Clip source `source`, index k = pack entry k (the engine plays entry 0 as SetAnim
// 0x80 after the attach). A pack entry for another joint count keeps its index as an empty clip (no keyframes): one pack can serve two
// skeletons in different index ranges. Fails when no entry fits.
std::optional<std::vector<assets::AnimationClip>> load_mdl_clips(ByteSpan mdl, ByteSpan pack, const char* source, std::string* err = nullptr);

// Section `index` (0 = S1 mesh, 1 = S2 skeleton, ... 5 = S6) of a 6-section Mdl, up to the next non-zero section / the EOF word.
// Empty when the header is not a 6-section Mdl or the section is absent.
ByteSpan mdl_section(ByteSpan mdl, int index);
// `mdl` with its S1 (mesh: vertices, faces, UVs) replaced by the S1 of `donor`; the header offsets after S1 and the EOF word move by the size
// difference, every other byte stays. nullopt unless both are 6-section Mdls with byte-identical S2 (skeleton). Gives an effect
// pack's copy of a battle model given the model's cdrom2 (HD art) mesh.
std::optional<Bytes> mdl_with_mesh_of(ByteSpan mdl, ByteSpan donor);

// The main texture of a battle model's .ltd / .ltc (first atim, first image) as raw 8 bpp indices plus CLUT row 0. nullopt if malformed.
struct AtimMain {
  u32 width = 0, height = 0;  // pixels (8 bpp: width / 2 halfwords)
  std::vector<u8> indices;    // width * height
  std::vector<u16> clut;      // CLUT row 0 (clut width entries)
};
std::optional<AtimMain> load_atim_main(ByteSpan tex);

// Animation-sequence tables: `table` = Mdl S5 or the second entry of a fieldobj/ex/*.bin. Never fails: malformed
// entries come back empty. ex_anim_seq() takes the whole ex .bin.
assets::AnimSeqTable parse_anim_seq_table(ByteSpan table);
assets::AnimSeqTable ex_anim_seq(ByteSpan ex_bin);

// Rigid mesh (.kmd or a weapon-pack mesh). UVs are divided by tex_w/tex_h pixels.
std::optional<assets::Mesh> load_kmd(ByteSpan data, std::string* err = nullptr, float tex_w = 256, float tex_h = 256);
std::optional<assets::WeaponPack> load_weapon_pack(ByteSpan data, std::string* err = nullptr);

struct AnimPackInfo {
  u32 clips = 0;
  u32 joints = 0;  // inferred from frame 0 unless given
  u32 frames = 0;  // total over all clips
};
// Anim pack = cpt of animations. Joint count is inferred when `joints` is 0.
std::optional<AnimPackInfo> inspect_anim_pack(ByteSpan pack, std::string* err = nullptr, u32 joints = 0);
// The anim pack inside at0.prd (DRP entry type 0x19) / inside fieldobj/ex/*.bin (entry 0). Empty span if absent.
ByteSpan at0_pack(ByteSpan prd);
ByteSpan ex_pack(ByteSpan ex_bin);

struct ModelValidation {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  u32 joints = 0, constructs = 0, vertices = 0, faces = 0, clips = 0;
  bool ok() const { return errors.empty(); }
};
// Loads the model and runs the validation checks (faces inside their construct, UVs inside the texture,
// weights, anim frame sizes, external pack joint count). Never throws. `ext_anims` is at0.prd (battle) or the
// ex .bin (field), may be empty.
ModelValidation validate_model(ByteSpan obj, ByteSpan tex, ByteSpan ext_anims, bool field);

}  // namespace oc::import
