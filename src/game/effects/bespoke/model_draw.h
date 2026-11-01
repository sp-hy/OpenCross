#pragma once
// The overlays' private copy of the model draw, shared by every port that re-skins a model and draws it itself:
// jakotu, fnoise, rakugaki, hensin, ghost2 (and the plane cutters of hensin, the shard builders of fnoise). All are copies of the stock
// model builders with the same skeleton:
//   per part i: F = part flag byte, colour word CW = (part rgb x model rgb) >> 7 per byte, byte 3 = (F & 0xF2) | 4
//   (or F as is), OR'ed with the model flags 0x2 -> 0x02 (semi), 0x100 -> 0x01 (part skipped), 0x20 / 0x40 / 0x80; tpage = (model tpage &
//   0xFF9F) | ((F & 0xC) << 3), the model CLUT; the faces of the 0x24 (GT3) / 0x2C (GT4) sections.
//   per face: RTPT (+ RTPS), NCLIP > 0 (quads: NCLIP(v0,v1,v2) > 0 or NCLIP(v1,v2,v3) < 0), OT index AVSZ3 / AVSZ4 (byte 3 bit 0x10: min / max
//   SZ >> 2) >> the OT shift; packet P1 = corner colours (module) or CW, the face UVs, SetSemiTrans(byte 3 bit 1); with byte 3 & 0x60 the UVs
//   come from the normals (or the module), and unless bit 0x80 the tpage / CLUT become the env page; with 0x40 P1 is forced semi-transparent
//   and a second packet P2 (CW, the face UVs and texture, byte 3 semi) goes into the same OT entry after it - P2 draws first, P1 on top.
#include <functional>
#include <vector>

#include "game/effects/bespoke/bespoke.h"

namespace oc::effects {

// One corner as the builders see it: the skinned WORLD vertex (s16), its normal bytes and the face's texture coordinate.
struct DrawVert {
  int x = 0, y = 0, z = 0;
  int nx = 0, ny = 0, nz = 0;
  // Normalised model texture coordinate (FxSkinFace).
  float u = 0, w = 0;
  // Index of the skinned vertex, -1 = a clip intersection.
  int idx = -1;
};

struct ModelDrawSpec {
  // Model flags as set around the draw (0x2 semi, 0x20 / 0x40 env packets, 0x80 env keeps the texture, 0x100 parts off).
  u16 flags = 0;
  // byte 3 = (F & 0xF2) | 4 (jakotu fnoise rakugaki); false = F as is (hensin ghost2: F bit 0 hides the part).
  bool mask_part_flags = true;
  // Env packet tpage low bits (the page; OR'ed with (byte 3 & 0xC) << 3 = the ABR).
  u16 env_tpage = 0x97;
  u16 env_clut = 0;
  // u, v = n ^ 0x80 (jakotu fnoise) / (n >> 1) + 0x40, (n >> 1) - 0x40 (hensin ghost2) / env_uv.
  enum class EnvUv { Xor80, Half, Custom };
  EnvUv env_uv = EnvUv::Xor80;
  std::function<void(const DrawVert&, u8 uv[2])> env_uv_fn;
  // P1 corner colour; null = the colour word.
  std::function<void(const DrawVert&, u8 rgb[3])> colour;
  // byte 3 bit 0x10 on a triangle: max SZ (hensin ghost2) instead of min (jakotu fnoise rakugaki).
  bool tri_far_z = false;
  // quad OT index = AVSZ3 of v1 v2 v3 (after the 4th vertex's RTPS) instead of AVSZ4 (fnoise).
  bool quad_avsz3 = true;
  // Horizontal world plane y = P (hensin, ghost2): a polygon wholly on side 0 (y <= P) / side 1 (y > P) is drawn unless
  // skip[side]; a cut one is split (Sutherland-Hodgman into two lists, intersections by LoadAverageShort12) and each piece drawn by side.
  bool clip = false;
  int plane = 0;
  bool skip[2] = {false, false};
  // Stock-draw and death-overlay variants:
  // >= 0: every packet semi-transparent with this ABR (the model's tpage word, used as is).
  int force_abr = -1;
  // FxPrim::white for the base packets (the white-CLUT silhouette passes).
  u8 white = 0;
  // false: no NCLIP test (ki_deth, drg_deth draw both sides).
  bool cull = true;
  // OT entry offset (drg_deth AddPrim at idx + 1, ki_deth's white packet).
  int tri_bias = 0, quad_bias = 0;
  // Per polygon before the clip / emit (the corners are copies; the ring buffer keeps the originals): may move corners; false = not drawn.
  // face = index of the face in the model's face list, quad_in_part = running quad count of the part (-1 for a triangle).
  std::function<bool(DrawVert* corners, int count, int face, int quad_in_part, int part)> face_hook;
  // The base packets sample the pack model's textures (TexSrc::PackModel) instead of the unit's own.
  bool pack_model = false;
  // The stock fog builders: the colour word of a plain face takes the depth cue of its last corner, one colour for the face.
  const FxDepthCue* cue = nullptr;
};

// Draws unit `unit`'s skinned model `sk` (FxModels::skin) as the overlay does, appending one FxPrim per packet (TexSrc::Model for the face
// texture, TexSrc::Vram for the env page). `order` continues across calls (the AddPrim order: later = drawn first inside an OT entry).
void model_draw(const FxSkinnedModel& sk, const FxModel& model, const ModelDrawSpec& spec, const ViewParams& view, std::vector<FxPrim>& out, u32& order);

// The plane cutter of hensin: the same part / face walk and clip, drawing nothing;
// for every polygon the plane cuts (>= 2 intersections) `cut` gets the first two intersection corners and the part's tpage word (ABR bits
// cleared) / face texture.
struct PlaneCut {
  DrawVert first, second;
  int tex = -1;
  int part = 0;
};
void model_plane_cuts(const FxSkinnedModel& sk, const ModelDrawSpec& spec, const std::function<void(const PlaneCut&)>& on_cut);

// Lower level, for the overlays whose face loop decides per face (the death dissolves): projection + culling of one polygon as their builders
// do it, and one textured packet. cull: 0 none, 1 the stock rule (quads: NCLIP(v0,v1,v2) > 0 or NCLIP(v1,v2,v3) < 0), 2 the F-B dissolve
// rule (quads: ... or NCLIP(v1,v2,v3) <= 0). False when a corner is behind the camera (GTE FLAG) or the polygon is culled.
struct ProjPoly {
  float x[4] = {}, y[4] = {}, z[4] = {};
  int n = 3;
};
bool project_poly(const ViewParams& view, const DrawVert* corners, int count, int cull, ProjPoly& out);
// One GT3 / GT4 packet: blend = particle blend (0 opaque, ABR + 1), src Model (tex of `unit`) unless vram_tpage >= 0 (TexSrc::Vram with the
// given CLUT; u / w then in texels from `uv`), white = FxPrim::white.
void push_poly(const ProjPoly& poly, const DrawVert* corners, int unit, int tex, const u8 (*rgb)[3], int blend, u8 white, int slot, u32& order,
               std::vector<FxPrim>& out, int vram_tpage = -1, u16 vram_clut = 0, const u8 (*uv)[2] = nullptr);

// Skinned vertex / face helpers for the shard builders.
DrawVert draw_vert(const FxSkinnedModel& sk, const FxSkinFace& face, int corner);
// Colour word of part i (CW): rgb bytes and byte 3.
void model_colour_word(const FxSkinnedModel& sk, const FxModel& model, const ModelDrawSpec& spec, int part, u8 rgb[3], u8& byte3);

}  // namespace oc::effects
