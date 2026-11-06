#pragma once
// Engine-side data model of a room's .prd effect payloads. Plain data, no simulation:
// sprite sheets (type 0x03, cut from VRAM), emitters (0x0A, decoded command stream) and sequences (0x10).
// Produced by import::load_room_effects(); consumed by the (future) effect simulation / renderer.
#include <array>
#include <string>
#include <variant>
#include <vector>

#include "core/types.h"

namespace oc::assets {

// One 12-byte sprite record. u/v are AFTER the one-time relocation against the paired TIM.
// x, y: quad offset from the object origin, 1/8 world unit; u, v: page-relative texel of the top-left corner; clut: (clut_y << 6) |
// (clut_x >> 4); flags: GPU tpage word + orientation; rgba: w*h*4 cut from VRAM, NOT mirrored (alpha 0 where the palette entry is 0).
// page_x() is in halfwords; depth() is 0 = 4bpp, otherwise 8bpp.
struct EffectSprite {
  s16 x = 0, y = 0;
  u8 u = 0, v = 0;
  u16 clut = 0;
  u8 w = 0, h = 0;
  u16 flags = 0;
  std::vector<u8> rgba;
  // RDE's HD image of the same texels (4x: w * 4 x h * 4 RGBA, unmirrored like `rgba`), empty when no HD page covers them.
  std::vector<u8> hd_rgba;

  int page_x() const { return (flags & 0xF) * 64; }
  int page_y() const { return (flags & 0x10) ? 256 : 0; }
  int abr() const { return (flags >> 5) & 3; }
  int depth() const { return (flags >> 7) & 3; }
  int orient() const { return (flags >> 9) & 3; }
  bool mirror_x() const { return orient() & 1; }
  bool mirror_y() const { return orient() & 2; }
  int clut_x() const { return (clut & 63) * 16; }
  int clut_y() const { return clut >> 6; }
};

// reloc_pending is the file's flag (the relocation has been applied to `frames`); frames[k-1] is frame k of the animation. tim_*:
// pairing with the same-named type-0x04 TIM of the room (by name): tim_match = the tex/clut fields equal the TIM image / CLUT
// position; tim_x..tim_h = the TIM image rect in halfwords; sprites_inside = sprite rects wholly inside the TIM image (when paired).
struct SpriteSheet {
  std::string name;
  u16 tex_x = 0, tex_y = 0, clut_x = 0, clut_y = 0;
  bool reloc_pending = false;
  std::vector<std::vector<EffectSprite>> frames;
  bool has_tim = false;
  bool tim_match = false;
  s32 tim_x = 0, tim_y = 0, tim_w = 0, tim_h = 0;
  u32 sprites_total = 0, sprites_inside = 0;
};

struct OscTerm {
  s16 amp = 0, freq = 0, phase = 0;
};

// Decoded command arguments, by op: FxValue = ops 03 04 0F 10 31 32 34; FxXyz = 3 x s16 at +2 (unsigned ops reinterpret the bits);
// FxOsc = ops 09 0D 11(count 1) 14 16 1C 1F 21 23 2F; FxFlags = op 06; FxSprite = op 05; FxNamed = ops 33 3C 45; FxBlob = op 27;
// FxExt35 = op 35, the extended-object flag word; FxUvScroll = op 36; FxMorph = op 37; FxBasis = op 39 (actor selector, joint,
// 2nd point); FxDrag = op 3A; FxRandDir = op 41; FxPath = ops 3D / 3E (ease mode + type-0x18 curve name); FxModelBody = op 49;
// FxRaw = everything else (the bytes after the command word).
struct FxNone {};
struct FxValue { s32 value = 0; };
struct FxXyz { s16 x = 0, y = 0, z = 0; };
struct FxOsc { std::array<OscTerm, 3> axis{}; u8 count = 3; };
struct FxFlags {
  u32 flags = 0;
  int blend() const { return static_cast<int>(flags & 7); }
};
struct FxSprite { u16 frame0 = 0; u32 cache = 0; std::string name; s16 type = 0; };
struct FxNamed { u32 cache = 0; std::string name; };
struct FxBlob { s16 span = 0, base = 0, sx = 0, sy = 0, sz = 0, base_grow = 0, span_grow = 0; };
struct FxExt35 { u32 ext = 0; };
struct FxUvScroll { u8 off_u = 0, off_v = 0; s16 win_w = 0, win_h = 0, rate_u = 0, rate_v = 0; };
struct FxMorph { u16 mode = 0; s16 rate = 0; u32 cache = 0; std::string name; };
struct FxBasis { u8 a = 0, b = 0, c = 0, d = 0; u16 flags = 0; };
struct FxDrag { u16 base = 0, rand = 0; };
struct FxRandDir { std::array<u16, 3> base{}; std::array<s16, 3> spread{}; };
struct FxPath { s16 mode = 0; u32 cache = 0; std::string name; };
struct FxModelBody { u8 show = 0, f200 = 0, f100 = 0, clone = 0; u16 rate = 0; s16 n = 0; };
struct FxRaw { Bytes data; };

// len is in bytes including the command word; offset is the payload offset of the command.
struct EffectOp {
  u16 op = 0;
  u16 len = 0;
  u32 offset = 0;
  std::variant<FxNone, FxValue, FxXyz, FxOsc, FxFlags, FxSprite, FxNamed, FxBlob, FxExt35, FxUvScroll, FxMorph, FxBasis,
               FxDrag, FxRandDir, FxPath, FxModelBody, FxRaw>
      arg;
};

// frame0 is the header's frame counter as stored (file u16 @+8; kept unless bit 10 clears it); a header nothing ever starts (a periodic
// child, op 0x2D) keeps it for good, and the oscillator ops read it as their time. count0 is the stored countdown (file s16 @+0xC).
// ops excludes the closing zero word; the leading op 0x01 is kept. start_stopped() is flag bit 2: the emitter waits for a script /
// sequence start.
struct Emitter {
  std::string name;
  u32 payload_size = 0;
  s16 start = 0, duration = 0;
  u16 period = 0;
  s16 repeat = 0;
  u16 frame0 = 0;
  s16 count0 = 0;
  u32 flags = 0;
  std::vector<EffectOp> ops;

  bool start_stopped() const { return flags & 4; }
  bool big_object() const { return flags & 2; }
  bool huge_object() const { return flags & (1u << 9); }
  bool restart_on_start() const { return flags & (1u << 10); }
  u32 jitter() const { return (flags >> 3) & 0x3F; }
};

// wait is in ticks after the command; name is the 4 chars at +12 for len >= 16 commands; data is the bytes after the 4-byte (word,
// wait) prefix.
struct SeqCmd {
  u16 op = 0;
  u16 len = 0;
  u32 offset = 0;
  u16 wait = 0;
  std::string name;
  Bytes data;
};

// flags bit 0 is chain/repeat; cmds run up to and including op 0x21.
struct Sequence {
  std::string name;
  u32 flags = 0;
  std::vector<SeqCmd> cmds;
};

// Opaque named entries kept for the effect player: type 0x0C camera track, 0x16 AKAO sound sequence, 0x18 path curve.
struct EffectBlob {
  std::string name;
  u8 type = 0;
  Bytes data;
};

// blobs hold types 0x0C / 0x16 / 0x18 (battle eins files; rooms have none of 0x0C/0x16); errors lists stream / entry parse failures
// (the entry is skipped).
struct RoomEffects {
  std::vector<SpriteSheet> sheets;
  std::vector<Emitter> emitters;
  std::vector<Sequence> sequences;
  std::vector<EffectBlob> blobs;
  u32 tims = 0, meshes = 0;
  std::vector<std::string> errors;

  const SpriteSheet* find_sheet(std::string_view name) const {
    for (const auto& sheet : sheets)
      if (sheet.name == name) return &sheet;
    return nullptr;
  }
  const Emitter* find_emitter(std::string_view name) const {
    for (const auto& emitter : emitters)
      if (emitter.name == name) return &emitter;
    return nullptr;
  }
  const EffectBlob* find_blob(u8 type, std::string_view name) const {
    for (const auto& blob : blobs)
      if (blob.type == type && blob.name == name) return &blob;
    return nullptr;
  }
  const Sequence* find_sequence(std::string_view name) const {
    for (const auto& sequence : sequences)
      if (sequence.name == name) return &sequence;
    return nullptr;
  }
};

}  // namespace oc::assets
