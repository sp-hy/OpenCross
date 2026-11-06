#pragma once
// Field script module: the decoded room event bytecode (map/mapbin/<room>.bin subfile 3) and its disassembler.
// The opcode table lives in field_opcodes.inc (one FS_OP row per opcode).
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace oc::script {

// Script pointers per entity.
constexpr int kScriptSlots = 32;
// Key of an extended-page opcode = kExtPage | sub-opcode.
constexpr u16 kExtPage = 0xFE00;
// Extension range FE C0..FE FF: engine features that mods turn on. Every instruction in it is framed as
// FE sub size payload[size], so a reader that does not know the sub-opcode still knows its length (3 + size).
constexpr u8 kExtensionFirst = 0xC0;
constexpr bool is_extension_op(u16 key) { return key >= (kExtPage | kExtensionFirst) && key <= (kExtPage | 0xFF); }

// One opcode table row: key (0xNN, or 0xFExx for the extended page); lengths = total instruction lengths, "/"-separated when the
// layout depends on the first operand byte; spec = operand tokens (b c d w U V J Shh Thh M A4, "sel:MASK{v:tokens;...;default}",
// "C1"); name; conf = 'C' confirmed, 'I' inferred, '?' semantics unknown. Extension rows: spec = the payload tokens (plain tokens
// only), lengths = the instruction length without extra payload bytes.
struct OpInfo {
  u16 key;
  const char* lengths;
  const char* spec;
  const char* name;
  char conf;
};

// nullptr when the opcode is not in the table.
const OpInfo* find_op(u16 key);
// Every row of field_opcodes.inc.
std::span<const OpInfo> op_table();

enum class OperandKind : u8 { Byte, Char, Word, Imm, Var, Jump, Str, Mode };
// `value`: Imm is signed (S/T literals), everything else is unsigned. `text`: Str only (the four ASCII characters).
struct Operand {
  OperandKind kind = OperandKind::Byte;
  s32 value = 0;
  std::string text;
};

enum class Flow : u8 { Next, End, Goto };

// A decoded instruction: `off` is the offset in the code area, `raw` the instruction bytes, `targets` the jump / call targets
// (code offset, is_call), `cond` the IF_JUMP condition (mode & 0xF).
struct Insn {
  u32 off = 0;
  u32 length = 0;
  u16 key = 0;
  const OpInfo* info = nullptr;
  Bytes raw;
  std::vector<Operand> ops;
  std::vector<std::pair<u32, bool>> targets;
  Flow flow = Flow::Next;
  int cond = -1;
};

// Operand tokens chosen by the spec for the instruction at code[off] (after resolving "sel:" / "C1" layouts).
std::vector<std::string_view> resolve_layout(const OpInfo& info, ByteSpan code, u32 off);

// Decodes one instruction. nullopt (and *error) on an unknown opcode or an operand that runs past the end. An extension op
// without a table row decodes as EXTENSION (conf '?', no operands); a row's operands must fit the payload, and payload bytes
// after them are skipped.
std::optional<Insn> decode_insn(ByteSpan code, u32 off, std::string* error = nullptr);

// Pre-script table record: (x, y, z, 0), probably entrance points selected by ROOM_PARAM. `level` is the 4th s16: the
// start-triangle index among the floors under (x, z), see WalkMesh::find_triangle_level.
struct PreScriptPos {
  s16 a = 0, b = 0, c = 0;
  s16 level = 0;
};

// `sign_mask`: per-room variable signedness mask (bit set = unsigned). `code`: the code area, everything from code_base on.
// `code_base`: file offset of the code area (0x84 + 0x40 * entity count). `positions`: pre-script table (empty when the room has none).
struct FieldScriptModule {
  std::array<u32, 32> sign_mask{};
  std::vector<std::array<u16, kScriptSlots>> entities;
  Bytes code;
  u32 code_base = 0;
  std::vector<PreScriptPos> positions;

  size_t entity_count() const { return entities.size(); }
  // Script pointer (code-area offset), 0 when the slot is unused or out of range.
  u16 script_ptr(size_t entity, size_t slot) const {
    return entity < entities.size() && slot < kScriptSlots ? entities[entity][slot] : u16{0};
  }
  u8 code_byte(u32 off) const { return off < code.size() ? code[off] : u8{0}; }

  // Parses the decoded (already LZSS-expanded) subfile. nullopt + *error when the header or entity table is malformed.
  static std::optional<FieldScriptModule> parse(ByteSpan blob, std::string* error = nullptr);
};

// Listing in the format of tools/py/cc_fieldscript.py `disasm`. entity < 0: the whole room (reachable code, unreferenced
// code, .data blocks); entity >= 0: only code reachable from that entity's scripts; with script >= 0 only from that slot.
std::string disassemble(const FieldScriptModule& module, int entity = -1, int script = -1);

}  // namespace oc::script
