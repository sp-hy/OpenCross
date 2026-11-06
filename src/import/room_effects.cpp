#include "import/room_effects.h"

#include <cstdio>
#include <exception>
#include <optional>

#include "core/span_reader.h"
#include "import/container.h"
#include "import/lzss.h"

namespace oc::import {

namespace {

struct Fail {
  std::string msg;
};

enum class ArgKind { None, S16, U8, U16, Xyz, Osc3, Osc1, Flags, Sprite, Named, Blob, Ext35, Uv, Morph, Basis, Drag, RandDir, Path, Model, Raw };

struct OpSpec {
  u16 op;
  const char* name;
  ArgKind kind;
  u8 len;
  // Alternative length (0 = none).
  u8 len2;
};

constexpr OpSpec kOps[] = {
    {0x01, "BEGIN", ArgKind::None, 2, 0},
    {0x03, "OT_BIAS", ArgKind::S16, 4, 0},
    {0x04, "ANIM_PERIOD", ArgKind::U8, 4, 0},
    {0x05, "SPRITE", ArgKind::Sprite, 16, 0},
    {0x06, "FLAGS", ArgKind::Flags, 8, 0},
    {0x08, "POS", ArgKind::Xyz, 8, 0},
    {0x09, "POS_ADD_OSC", ArgKind::Osc3, 26, 0},
    {0x0A, "VEL", ArgKind::Xyz, 8, 0},
    {0x0B, "VEL_ADD_RAND", ArgKind::Xyz, 8, 0},
    {0x0C, "ACC", ArgKind::Xyz, 8, 0},
    {0x0D, "ACC_ADD_OSC", ArgKind::Osc3, 26, 0},
    {0x0E, "ACC_ADD_RAND", ArgKind::Xyz, 8, 0},
    {0x0F, "LIFE", ArgKind::U16, 4, 0},
    {0x10, "LIFE_ADD", ArgKind::U16, 4, 0},
    {0x11, "LIFE_ADD_OSC", ArgKind::Osc1, 10, 0},
    {0x12, "SCALE", ArgKind::Xyz, 8, 0},
    {0x13, "SCALE_ADD_RAND", ArgKind::Xyz, 8, 0},
    {0x14, "SCALE_ADD_OSC", ArgKind::Osc3, 26, 0},
    {0x15, "SCALE_RATE", ArgKind::Xyz, 8, 0},
    {0x16, "SCALE_RATE_ADD_OSC", ArgKind::Osc3, 26, 0},
    {0x17, "NOP17", ArgKind::Xyz, 8, 0},
    {0x18, "SCALE_RATE_ADD_RAND", ArgKind::Xyz, 8, 0},
    {0x19, "ANGLE", ArgKind::Xyz, 8, 0},
    {0x1A, "ANGLE_ADD_RAND", ArgKind::Xyz, 8, 0},
    {0x1B, "ANGLE_RATE", ArgKind::Xyz, 8, 0},
    {0x1C, "ANGLE_ADD_OSC", ArgKind::Osc3, 26, 0},
    {0x1D, "NOP1D", ArgKind::Xyz, 8, 0},
    {0x1E, "ANGLE_RATE_ADD_RAND", ArgKind::Xyz, 8, 0},
    {0x1F, "ANGLE_RATE_ADD_OSC", ArgKind::Osc3, 26, 0},
    {0x20, "COLOR", ArgKind::Xyz, 8, 0},
    {0x21, "COLOR_ADD_OSC", ArgKind::Osc3, 26, 0},
    {0x22, "COLOR_RATE", ArgKind::Xyz, 8, 0},
    {0x23, "COLOR_RATE_ADD_OSC", ArgKind::Osc3, 26, 0},
    {0x24, "NOP24", ArgKind::Xyz, 8, 0},
    {0x25, "COLOR_ADD_RAND", ArgKind::Xyz, 8, 0},
    {0x26, "COLOR_RATE_ADD_RAND", ArgKind::Xyz, 8, 0},
    {0x27, "POS_ADD_BLOB", ArgKind::Blob, 22, 26},
    {0x2F, "VEL_ADD_OSC", ArgKind::Osc3, 26, 0},
    {0x30, "ANCHOR", ArgKind::Raw, 12, 0},
    {0x31, "BOUNCE", ArgKind::U8, 4, 0},
    {0x32, "FLOOR_Y", ArgKind::U16, 4, 0},
    {0x33, "ON_DEATH_SPAWN", ArgKind::Named, 12, 0},
    {0x34, "VEL_ADD_AWAY", ArgKind::U16, 4, 0},
    {0x35, "EXT35", ArgKind::Ext35, 8, 0},
    {0x36, "EXT36", ArgKind::Uv, 14, 0},
    {0x37, "EXT37", ArgKind::Morph, 16, 0},
    {0x38, "INTEGRATE", ArgKind::None, 4, 0},
    {0x39, "ORIENT_BASIS", ArgKind::Basis, 10, 0},
    {0x3A, "DRAG", ArgKind::Drag, 6, 0},
    {0x3B, "FACE_PARENT_YAW", ArgKind::None, 4, 0},
    {0x3C, "PERIODIC_SPAWN", ArgKind::Named, 12, 0},
    {0x3D, "PATH_POS", ArgKind::Path, 20, 0},
    {0x3E, "EXT3E", ArgKind::Path, 24, 0},
    {0x3F, "EXT3F", ArgKind::None, 4, 0},
    {0x40, "EXT40", ArgKind::None, 4, 0},
    {0x41, "VEL_RANDOM_DIR", ArgKind::RandDir, 14, 0},
    {0x42, "AIM_AT_BASIS", ArgKind::None, 4, 0},
    {0x43, "SCALE_ACC", ArgKind::Xyz, 8, 0},
    {0x44, "ANGLE_ACC", ArgKind::Xyz, 8, 0},
    {0x45, "SPAWN_WITH", ArgKind::Named, 12, 0},
    {0x46, "FACE_PARENT", ArgKind::None, 4, 0},
    {0x48, "OP48", ArgKind::Raw, 12, 0},
    {0x49, "OP49", ArgKind::Model, 10, 0},
};

const OpSpec* find_op(u16 op) {
  for (const auto& spec : kOps)
    if (spec.op == op) return &spec;
  return nullptr;
}

struct SeqSpec {
  u16 op;
  const char* name;
  u8 len;
};
constexpr SeqSpec kSeqOps[] = {{0x20, "MARK", 8},  {0x21, "END", 8},        {0x22, "START_EMITTER", 16},
                               {0x23, "SOUND_GROUP", 16}, {0x28, "HOLD", 8}, {0x29, "RAMP", 12},
                               {0x2E, "OP2E", 12}, {0x2F, "CHILD_SEQ", 16}, {0x3E, "BRACKET_BEGIN", 8},
                               {0x3F, "BRACKET_END", 8}, {0x65, "OP65", 16}};

const SeqSpec* find_seq_op(u16 op) {
  for (const auto& spec : kSeqOps)
    if (spec.op == op) return &spec;
  return nullptr;
}

std::string hex(u32 value) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "0x%X", value);
  return buffer;
}

std::string name4(const SpanReader& reader, size_t offset) {
  std::string name;
  for (size_t i = 0; i < 4; ++i) {
    const u8 byte = reader.at<u8>(offset + i);
    if (!byte) break;
    name.push_back(static_cast<char>(byte));
  }
  return name;
}

s16 s16_at(const SpanReader& reader, size_t offset) { return static_cast<s16>(reader.at<u16>(offset)); }

// A name is kept only when every byte is printable (0x7F included: it occurs in real names); control bytes mean the slot
// holds an op-specific operand instead.
void drop_unprintable(std::string& name) {
  for (char ch : name)
    if (ch < 32) {
      name.clear();
      return;
    }
}

// Emitter payload: 0x18-byte header, then commands of u16 word (op = word & 0x1FF, length = (word >> 9) & 0x3F, bit 15 = jump
// flag) plus operands, terminated by op 0.
assets::Emitter parse_emitter(const std::string& name, ByteSpan payload) {
  if (payload.size() < 0x1A) throw Fail{"short 0x0A payload"};
  SpanReader reader(payload);
  assets::Emitter emitter;
  emitter.name = name;
  emitter.payload_size = static_cast<u32>(payload.size());
  emitter.start = s16_at(reader, 4);
  emitter.duration = s16_at(reader, 6);
  emitter.period = reader.at<u16>(0xA);
  emitter.repeat = s16_at(reader, 0xE);
  emitter.frame0 = reader.at<u16>(8);
  emitter.count0 = s16_at(reader, 0xC);
  emitter.flags = reader.at<u32>(0x10);
  size_t pos = 0x18;
  for (;;) {
    if (pos + 2 > payload.size()) throw Fail{"stream runs off the payload at " + hex(static_cast<u32>(pos))};
    const u16 word = reader.at<u16>(pos);
    const u16 op = word & 0x1FF;
    const u16 len = (word >> 9) & 0x3F;
    if (word & 0x8000) throw Fail{"jump-flagged command at " + hex(static_cast<u32>(pos))};
    if (op == 0) break;
    const OpSpec* spec = find_op(op);
    if (!spec) throw Fail{"unknown op " + hex(op) + " at " + hex(static_cast<u32>(pos))};
    if (len != spec->len && len != spec->len2) throw Fail{"op " + hex(op) + " length " + std::to_string(len)};
    if (pos + len > payload.size()) throw Fail{"command overruns payload at " + hex(static_cast<u32>(pos))};
    assets::EffectOp command;
    command.op = op;
    command.len = len;
    command.offset = static_cast<u32>(pos);
    switch (spec->kind) {
      case ArgKind::None: command.arg = assets::FxNone{}; break;
      case ArgKind::S16: command.arg = assets::FxValue{s16_at(reader, pos + 2)}; break;
      case ArgKind::U8: command.arg = assets::FxValue{reader.at<u8>(pos + 2)}; break;
      case ArgKind::U16: command.arg = assets::FxValue{reader.at<u16>(pos + 2)}; break;
      case ArgKind::Xyz: command.arg = assets::FxXyz{s16_at(reader, pos + 2), s16_at(reader, pos + 4), s16_at(reader, pos + 6)}; break;
      case ArgKind::Osc3:
      case ArgKind::Osc1: {
        assets::FxOsc osc;
        osc.count = spec->kind == ArgKind::Osc1 ? 1 : 3;
        for (size_t axis = 0; axis < osc.count; ++axis) {
          const size_t axis_offset = pos + 2 + 8 * axis;
          osc.axis[axis] = {s16_at(reader, axis_offset), s16_at(reader, axis_offset + 2), s16_at(reader, axis_offset + 4)};
        }
        command.arg = osc;
        break;
      }
      case ArgKind::Flags: command.arg = assets::FxFlags{reader.at<u32>(pos + 4)}; break;
      case ArgKind::Sprite:
        command.arg = assets::FxSprite{reader.at<u16>(pos + 2), reader.at<u32>(pos + 4), name4(reader, pos + 8), s16_at(reader, pos + 12)};
        break;
      case ArgKind::Named: command.arg = assets::FxNamed{reader.at<u32>(pos + 4), name4(reader, pos + 8)}; break;
      case ArgKind::Blob:
        command.arg = assets::FxBlob{s16_at(reader, pos + 2), s16_at(reader, pos + 4), s16_at(reader, pos + 6), s16_at(reader, pos + 8),
                                     s16_at(reader, pos + 10), s16_at(reader, pos + 12), s16_at(reader, pos + 14)};
        break;
      case ArgKind::Ext35: command.arg = assets::FxExt35{reader.at<u32>(pos + 4)}; break;
      case ArgKind::Uv:
        command.arg = assets::FxUvScroll{reader.at<u8>(pos + 2), reader.at<u8>(pos + 4), s16_at(reader, pos + 6), s16_at(reader, pos + 8),
                                         s16_at(reader, pos + 10), s16_at(reader, pos + 12)};
        break;
      case ArgKind::Morph:
        command.arg = assets::FxMorph{reader.at<u16>(pos + 2), s16_at(reader, pos + 4), reader.at<u32>(pos + 8), name4(reader, pos + 12)};
        break;
      case ArgKind::Basis:
        command.arg = assets::FxBasis{reader.at<u8>(pos + 2), reader.at<u8>(pos + 3), reader.at<u8>(pos + 4), reader.at<u8>(pos + 5),
                                      reader.at<u16>(pos + 6)};
        break;
      case ArgKind::Drag: command.arg = assets::FxDrag{reader.at<u16>(pos + 2), reader.at<u16>(pos + 4)}; break;
      case ArgKind::RandDir: {
        assets::FxRandDir rand_dir;
        for (size_t k = 0; k < 3; ++k) {
          rand_dir.base[k] = reader.at<u16>(pos + 2 + 2 * k);
          rand_dir.spread[k] = s16_at(reader, pos + 8 + 2 * k);
        }
        command.arg = rand_dir;
        break;
      }
      case ArgKind::Path: command.arg = assets::FxPath{s16_at(reader, pos + 2), reader.at<u32>(pos + 4), name4(reader, pos + 8)}; break;
      case ArgKind::Model:
        command.arg = assets::FxModelBody{reader.at<u8>(pos + 2), reader.at<u8>(pos + 3), reader.at<u8>(pos + 4), reader.at<u8>(pos + 5),
                                          reader.at<u16>(pos + 6), s16_at(reader, pos + 8)};
        break;
      case ArgKind::Raw: {
        const auto raw = reader.slice(pos + 2, len - 2u);
        command.arg = assets::FxRaw{Bytes(raw.begin(), raw.end())};
        break;
      }
    }
    emitter.ops.push_back(std::move(command));
    pos += len;
  }
  return emitter;
}

// Sequence payload: u32 flags, 0x14-byte header, then commands (same word framing as emitters) up to END (0x21). Commands of
// 16+ bytes carry a 4-char name at +12; the emitter stop / kill ops 0x55 and 0x5A carry theirs at +8 (+12 is padding). Lenient
// mode (battle packs) accepts any op with a length of at least 4 and a missing END.
assets::Sequence parse_sequence(const std::string& name, ByteSpan payload, bool lenient) {
  if (payload.size() < 0x14) throw Fail{"short sequence"};
  SpanReader reader(payload);
  assets::Sequence sequence;
  sequence.name = name;
  sequence.flags = reader.at<u32>(0);
  size_t pos = 0x14;
  for (;;) {
    if (pos + 4 > payload.size()) {
      if (lenient && !sequence.cmds.empty()) break;
      throw Fail{"sequence runs off the payload"};
    }
    const u16 word = reader.at<u16>(pos);
    const u16 op = word & 0x1FF;
    const u16 len = (word >> 9) & 0x3F;
    const SeqSpec* spec = find_seq_op(op);
    if (lenient ? len < 4 : (!spec || spec->len != len))
      throw Fail{"sequence op " + hex(op) + " length " + std::to_string(len) + " at " + hex(static_cast<u32>(pos))};
    if (pos + len > payload.size()) throw Fail{"sequence command overruns payload"};
    assets::SeqCmd command;
    command.op = op;
    command.len = len;
    command.offset = static_cast<u32>(pos);
    command.wait = reader.at<u16>(pos + 2);
    if (len >= 16) command.name = name4(reader, pos + 12);
    if (lenient) drop_unprintable(command.name);
    if ((op == 0x55 || op == 0x5A) && len >= 12) {
      command.name = name4(reader, pos + 8);
      drop_unprintable(command.name);
    }
    const auto raw = reader.slice(pos + 4, len - 4u);
    command.data.assign(raw.begin(), raw.end());
    sequence.cmds.push_back(std::move(command));
    pos += len;
    if (op == 0x21) break;
  }
  return sequence;
}

// Timinfo payload: u16 at +2 = frame count (bit 15: relocation pending), tex x/y and CLUT x/y at +4..+10, then u16 frame
// offsets; each frame is a u16 sprite count and 12-byte sprites. A pending relocation adds the texture position to every
// sprite's u (x 4 or x 2 texels per halfword) and v once.
assets::SpriteSheet parse_timinfo(const std::string& name, ByteSpan payload) {
  if (payload.size() < 0x10) throw Fail{"short timinfo"};
  SpanReader reader(payload);
  assets::SpriteSheet sheet;
  sheet.name = name;
  const u16 count_word = reader.at<u16>(2);
  sheet.tex_x = reader.at<u16>(4);
  sheet.tex_y = reader.at<u16>(6);
  sheet.clut_x = reader.at<u16>(8);
  sheet.clut_y = reader.at<u16>(10);
  sheet.reloc_pending = (count_word & 0x8000) != 0;
  const size_t frame_count = count_word & 0x7FFF;
  if (0xC + 2 * frame_count > payload.size()) throw Fail{"timinfo offsets overrun"};
  for (size_t i = 0; i < frame_count; ++i) {
    const size_t frame_offset = reader.at<u16>(0xC + 2 * i);
    if (frame_offset + 4 > payload.size()) throw Fail{"timinfo table offset out of range"};
    const size_t sprite_count = reader.at<u16>(frame_offset);
    if (frame_offset + 4 + 12 * sprite_count > payload.size()) throw Fail{"timinfo table overruns payload"};
    std::vector<assets::EffectSprite> frame;
    for (size_t j = 0; j < sprite_count; ++j) {
      const size_t record = frame_offset + 4 + 12 * j;
      assets::EffectSprite sprite;
      sprite.x = s16_at(reader, record);
      sprite.y = s16_at(reader, record + 2);
      sprite.u = reader.at<u8>(record + 4);
      sprite.v = reader.at<u8>(record + 5);
      sprite.clut = reader.at<u16>(record + 6);
      sprite.w = reader.at<u8>(record + 8);
      sprite.h = reader.at<u8>(record + 9);
      sprite.flags = reader.at<u16>(record + 10);
      if (sheet.reloc_pending) {
        const unsigned add = (sprite.depth() == 0) ? (sheet.tex_x & 0x3Fu) << 2 : (sheet.tex_x & 0x3Fu) << 1;
        sprite.u = static_cast<u8>(sprite.u + add);
        sprite.v = static_cast<u8>(sprite.v + (sheet.tex_y & 0xFFu));
      }
      frame.push_back(std::move(sprite));
    }
    sheet.frames.push_back(std::move(frame));
  }
  return sheet;
}

void cut_sprite(const Vram& vram, assets::EffectSprite& sprite) {
  const bool four_bit = sprite.depth() == 0;
  const int colours = four_bit ? 16 : 256;
  std::vector<u32> palette(static_cast<size_t>(colours));
  for (int i = 0; i < colours; ++i) palette[static_cast<size_t>(i)] = bgr555_to_rgba(vram.at(sprite.clut_x() + i, sprite.clut_y()));
  sprite.rgba.assign(static_cast<size_t>(sprite.w) * sprite.h * 4, 0);
  for (int j = 0; j < sprite.h; ++j) {
    const int row = (sprite.page_y() + sprite.v + j) & 511;
    for (int i = 0; i < sprite.w; ++i) {
      const int tx = sprite.u + i;
      unsigned index;
      if (four_bit) index = (vram.at(sprite.page_x() + (tx >> 2), row) >> ((tx & 3) * 4)) & 15u;
      else index = (vram.at(sprite.page_x() + (tx >> 1), row) >> ((tx & 1) * 8)) & 255u;
      store_rgba(&sprite.rgba[(static_cast<size_t>(j) * sprite.w + static_cast<size_t>(i)) * 4], palette[index]);
    }
  }
}

std::optional<Tim> tim_of(ByteSpan data) {
  if (is_lzss(data)) {
    auto decoded = lzss_decode(data);
    if (!decoded) return std::nullopt;
    return parse_tim(*decoded);
  }
  return parse_tim(data);
}

}  // namespace

const char* effect_op_name(u16 op) {
  const OpSpec* spec = find_op(op);
  return spec ? spec->name : "?";
}
const char* sequence_op_name(u16 op) {
  const SeqSpec* spec = find_seq_op(op);
  return spec ? spec->name : "?";
}

namespace {

// "ctd" sections (battle egfx): 1 header sector (u32 type 0x1200 / 0x1201, u16 x, y, x offset, y offset, u16 width in
// halfwords, u32 data sector count at +24, u16 row counts per data sector from +28) + N data sectors of raw VRAM rows.
// Stops silently at the first malformed section.
void apply_ctd(Vram& vram, ByteSpan ctd) {
  SpanReader reader(ctd);
  size_t offset = 0;
  while (offset + 2048 <= ctd.size()) {
    const u32 type = reader.at<u32>(offset);
    const int x = reader.at<u16>(offset + 4) + reader.at<u16>(offset + 8), y = reader.at<u16>(offset + 6) + reader.at<u16>(offset + 10);
    const u32 width_hw = reader.at<u16>(offset + 12), sector_count = reader.at<u32>(offset + 24);
    if ((type & 0xFFFEu) != 0x1200 || width_hw == 0 || offset + 2048ull * (1ull + sector_count) > ctd.size()) return;
    std::vector<u16> data;
    int rows_total = 0;
    for (u32 i = 0; i < sector_count && 28 + 2ull * i + 2 <= 2048; ++i) {
      const u32 rows = reader.at<u16>(offset + 28 + 2ull * i);
      const size_t sector_offset = offset + 2048ull * (1 + i);
      if (static_cast<size_t>(rows) * width_hw * 2 > 2048) return;
      for (size_t k = 0; k < static_cast<size_t>(rows) * width_hw; ++k) data.push_back(reader.at<u16>(sector_offset + 2 * k));
      rows_total += static_cast<int>(rows);
    }
    vram.blit(x, y, static_cast<int>(width_hw), rows_total, data);
    offset += 2048ull * (1ull + sector_count);
  }
}

assets::RoomEffects load_pack(ByteSpan prd, const Vram& base, ByteSpan lib, ByteSpan ctd, bool battle) {
  assets::RoomEffects fx;
  auto drp = parse_drp(prd);
  if (!drp) {
    fx.errors.push_back("prd: bad drp");
    return fx;
  }
  Vram vram = base;
  if (!ctd.empty()) {
    try {
      apply_ctd(vram, ctd);
    } catch (const std::exception&) {
      fx.errors.push_back("ctd: malformed");
    }
  }
  if (!lib.empty()) {
    if (auto lib_drp = parse_drp(lib))
      for (const auto& entry : lib_drp->entries)
        if (entry.type == 4)
          if (auto tim = tim_of(entry.data)) vram.blit_tim(*tim);
  }
  struct TimInfo {
    std::string name;
    Tim tim;
  };
  std::vector<TimInfo> tims;
  for (const auto& entry : drp->entries) {
    if (entry.type != 4) continue;
    auto tim = tim_of(entry.data);
    if (!tim) {
      fx.errors.push_back(entry.name + "/0x04: bad TIM");
      continue;
    }
    vram.blit_tim(*tim);
    tims.push_back({entry.name, std::move(*tim)});
    ++fx.tims;
  }
  for (const auto& entry : drp->entries) {
    try {
      switch (entry.type) {
        case 0x02: ++fx.meshes; break;
        case 0x03:
        case 0x15: {
          // Timinfo sheet; 0x15 is the battle stage lens flare `lenz`, registered like a type 3 sheet.
          assets::SpriteSheet sheet = parse_timinfo(entry.name, entry.data);
          for (const auto& tim : tims) {
            if (tim.name != entry.name) continue;
            sheet.has_tim = true;
            sheet.tim_x = tim.tim.image.x;
            sheet.tim_y = tim.tim.image.y;
            sheet.tim_w = tim.tim.image.w;
            sheet.tim_h = tim.tim.image.h;
            sheet.tim_match = sheet.tim_x == sheet.tex_x && sheet.tim_y == sheet.tex_y &&
                              (!tim.tim.has_clut || (tim.tim.clut.x == sheet.clut_x && tim.tim.clut.y == sheet.clut_y));
            break;
          }
          for (auto& frame : sheet.frames)
            for (auto& sprite : frame) {
              cut_sprite(vram, sprite);
              if (!sheet.has_tim) continue;
              const int texels_per_halfword = sprite.depth() == 0 ? 4 : 2;
              const int x0 = sprite.page_x() * texels_per_halfword + sprite.u - sheet.tim_x * texels_per_halfword;
              const int y0 = sprite.page_y() + sprite.v - sheet.tim_y;
              ++sheet.sprites_total;
              if (x0 >= 0 && x0 + sprite.w <= sheet.tim_w * texels_per_halfword && y0 >= 0 && y0 + sprite.h <= sheet.tim_h)
                ++sheet.sprites_inside;
            }
          fx.sheets.push_back(std::move(sheet));
          break;
        }
        case 0x0A: fx.emitters.push_back(parse_emitter(entry.name, entry.data)); break;
        case 0x0C:
        case 0x16:
        case 0x18:
        case 0x24: fx.blobs.push_back({entry.name, entry.type, Bytes(entry.data.begin(), entry.data.end())}); break;
        case 0x10: fx.sequences.push_back(parse_sequence(entry.name, entry.data, battle)); break;
        default: break;
      }
    } catch (const Fail& failure) {
      fx.errors.push_back(entry.name + "/" + hex(entry.type) + ": " + failure.msg);
    } catch (const std::exception& exception) {
      fx.errors.push_back(entry.name + "/" + hex(entry.type) + ": " + exception.what());
    }
  }
  return fx;
}

}  // namespace

assets::RoomEffects load_room_effects(ByteSpan prd, const Vram& base, ByteSpan lib) {
  return load_pack(prd, base, lib, {}, false);
}

assets::RoomEffects load_battle_effects(ByteSpan prd, ByteSpan ctd, const Vram& base, ByteSpan lib) {
  return load_pack(prd, base, lib, ctd, true);
}

size_t cut_hd_sheet(assets::SpriteSheet& sheet, const Vram& vram, const Tim& hd) {
  constexpr int kScale = 4;
  const TimBlock& image = hd.image;
  if ((hd.bpp != 4 && hd.bpp != 8) || image.w % kScale || image.h % kScale || image.data.size() < static_cast<size_t>(image.w) * image.h) return 0;
  Vram with_clut = vram;
  if (hd.has_clut) with_clut.blit(sheet.clut_x, sheet.clut_y, hd.clut.w, hd.clut.h, hd.clut.data);
  const int per_halfword = hd.bpp == 4 ? 4 : 2, rect_w = image.w / kScale * per_halfword, rect_h = image.h / kScale;
  size_t filled = 0;
  for (auto& frame : sheet.frames)
    for (assets::EffectSprite& sprite : frame) {
      if ((sprite.depth() == 0) != (hd.bpp == 4)) continue;
      const int col0 = (sprite.page_x() - sheet.tex_x) * per_halfword + sprite.u, row0 = sprite.page_y() + sprite.v - sheet.tex_y;
      if (col0 < 0 || row0 < 0 || col0 + sprite.w > rect_w || row0 + sprite.h > rect_h) continue;
      const int colours = hd.bpp == 4 ? 16 : 256;
      std::vector<u32> palette(static_cast<size_t>(colours));
      for (int i = 0; i < colours; ++i) palette[static_cast<size_t>(i)] = bgr555_to_rgba(with_clut.at(sprite.clut_x() + i, sprite.clut_y()));
      const int w = sprite.w * kScale, h = sprite.h * kScale;
      sprite.hd_rgba.assign(static_cast<size_t>(w) * h * 4, 0);
      for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
          const int col = col0 * kScale + i;
          const u16 halfword = image.data[static_cast<size_t>(row0 * kScale + j) * image.w + static_cast<size_t>(col / per_halfword)];
          const unsigned index = hd.bpp == 4 ? (halfword >> ((col & 3) * 4)) & 15u : (halfword >> ((col & 1) * 8)) & 255u;
          store_rgba(&sprite.hd_rgba[(static_cast<size_t>(j) * w + static_cast<size_t>(i)) * 4], palette[index]);
        }
      ++filled;
    }
  return filled;
}

Bytes nested_pack(ByteSpan prd, std::string_view name) {
  if (auto drp = parse_drp(prd))
    for (const auto& entry : drp->entries)
      if (entry.type == 0x01 && entry.name == name && is_drp(entry.data)) return Bytes(entry.data.begin(), entry.data.end());
  return {};
}

EffectsValidation validate_room_effects(const assets::RoomEffects& fx) {
  EffectsValidation validation;
  validation.errors = fx.errors;
  validation.emitters = static_cast<u32>(fx.emitters.size());
  validation.sheets = static_cast<u32>(fx.sheets.size());
  validation.sequences = static_cast<u32>(fx.sequences.size());
  for (const auto& emitter : fx.emitters) {
    bool has_sprite = false;
    for (const auto& command : emitter.ops) {
      if (command.op != 5) continue;
      has_sprite = true;
      if (std::get<assets::FxSprite>(command.arg).cache != 0) validation.errors.push_back(emitter.name + "/0x0A: SPRITE cache not zero");
    }
    if (!has_sprite) validation.errors.push_back(emitter.name + "/0x0A: no SPRITE command");
  }
  for (const auto& sheet : fx.sheets) {
    validation.frames += static_cast<u32>(sheet.frames.size());
    validation.sprites += sheet.sprites_total;
    validation.sprites_inside += sheet.sprites_inside;
    if (!sheet.has_tim) {
      ++validation.unpaired;
    } else if (sheet.tim_match) {
      ++validation.pair_ok;
    } else {
      ++validation.pair_mismatch;
      validation.warnings.push_back("timinfo " + sheet.name + " tex/clut != TIM (" + std::to_string(sheet.tex_x) + "," +
                                    std::to_string(sheet.tex_y) + ") vs (" + std::to_string(sheet.tim_x) + "," +
                                    std::to_string(sheet.tim_y) + ")");
    }
  }
  return validation;
}

}  // namespace oc::import
