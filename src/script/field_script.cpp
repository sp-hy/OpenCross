#include "script/field_script.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "core/span_reader.h"

namespace oc::script {

namespace {

constexpr OpInfo kOps[] = {
#define FS_OP(key, lens, spec, name, conf) {key, lens, spec, name, conf},
#include "script/field_opcodes.inc"
#undef FS_OP
};

std::vector<std::string_view> split_ws(std::string_view text) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') ++i;
    size_t j = i;
    while (j < text.size() && text[j] != ' ') ++j;
    if (j > i) out.push_back(text.substr(i, j - i));
    i = j;
  }
  return out;
}

int token_width(std::string_view token) {
  if (token == "b" || token == "c" || token == "d" || token == "M") return 1;
  if (token == "w" || token == "U" || token == "V" || token == "J") return 2;
  if (token == "A4") return 4;
  if (!token.empty() && (token[0] == 'S' || token[0] == 'T')) return 2;
  return 0;
}

u16 rd16(ByteSpan code, size_t pos) { return pos + 1 < code.size() ? static_cast<u16>(code[pos] | (code[pos + 1] << 8)) : u16{0}; }

}  // namespace

std::span<const OpInfo> op_table() { return kOps; }

const OpInfo* find_op(u16 key) {
  static const std::map<u16, const OpInfo*> index = [] {
    std::map<u16, const OpInfo*> by_key;
    for (const auto& op : kOps) by_key[op.key] = &op;
    return by_key;
  }();
  auto it = index.find(key);
  return it == index.end() ? nullptr : it->second;
}

std::vector<std::string_view> resolve_layout(const OpInfo& info, ByteSpan code, u32 off) {
  const std::string_view spec = info.spec;
  const u32 nprefix = info.key > 0xFF ? 2 : 1;
  const u8 b1 = off + nprefix < code.size() ? code[off + nprefix] : u8{0};
  if (spec == "C1") return b1 == 0 ? split_ws("b c U") : split_ws("b");
  if (spec.starts_with("sel:")) {
    const int mask = std::stoi(std::string(spec.substr(4, 2)), nullptr, 16);
    const std::string_view body = spec.substr(spec.find('{') + 1, spec.rfind('}') - spec.find('{') - 1);
    std::vector<std::string_view> parts;
    size_t i = 0;
    for (;;) {
      const size_t j = body.find(';', i);
      parts.push_back(body.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i));
      if (j == std::string_view::npos) break;
      i = j + 1;
    }
    for (size_t k = 0; k + 1 < parts.size(); ++k) {
      const size_t colon = parts[k].find(':');
      const int value = std::stoi(std::string(parts[k].substr(0, colon)), nullptr, 16);
      if ((b1 & mask) == value) return split_ws(parts[k].substr(colon + 1));
    }
    return split_ws(parts.back());
  }
  return split_ws(spec);
}

std::optional<Insn> decode_insn(ByteSpan code, u32 off, std::string* error) {
  auto fail = [&](const std::string& message) -> std::optional<Insn> {
    if (error) *error = message;
    return std::nullopt;
  };
  const size_t size = code.size();
  if (off >= size) return fail(std::format("past end @{:04X}", off));
  const u8 op = code[off];
  u32 nprefix = 1;
  u16 key = op;
  if (op == 0xFE) {
    if (off + 1 >= size) return fail(std::format("truncated FE @{:04X}", off));
    key = static_cast<u16>(kExtPage | code[off + 1]);
    nprefix = 2;
  }
  const OpInfo* info = find_op(key);
  if (!info) {
    return fail(op == 0xFE ? std::format("unknown opcode FE {:02X} @{:04X}", key & 0xFF, off)
                           : std::format("unknown opcode {:02X} @{:04X}", op, off));
  }
  const auto toks = resolve_layout(*info, code, off);
  u32 pos = off + nprefix;
  std::vector<u32> offs;
  u32 moff = 0;
  bool has_mode = false;
  for (auto token : toks) {
    offs.push_back(pos);
    if (token == "M") {
      moff = pos;
      has_mode = true;
    }
    pos += static_cast<u32>(token_width(token));
  }
  if (pos > size) return fail(std::format("operands run past end @{:04X}", off));
  Insn ins;
  ins.off = off;
  ins.length = pos - off;
  ins.key = key;
  ins.info = info;
  if (ins.length > ins.raw.size()) return fail("instruction too long");
  std::copy(code.begin() + off, code.begin() + pos, ins.raw.begin());
  const u8 mode = has_mode ? code[moff] : u8{0};
  for (size_t i = 0; i < toks.size(); ++i) {
    const auto token = toks[i];
    const u32 operand_pos = offs[i];
    Operand operand;
    if (token == "b" || token == "d") {
      operand = {OperandKind::Byte, code[operand_pos], {}};
    } else if (token == "c") {
      operand = {OperandKind::Char, code[operand_pos], {}};
    } else if (token == "w") {
      operand = {OperandKind::Word, rd16(code, operand_pos), {}};
    } else if (token == "U") {
      const u16 value = rd16(code, operand_pos);
      operand = value & 0x8000 ? Operand{OperandKind::Imm, value & 0x7FFF, {}} : Operand{OperandKind::Var, value, {}};
    } else if (token == "V") {
      operand = {OperandKind::Var, rd16(code, operand_pos), {}};
    } else if (token == "J") {
      const u16 value = rd16(code, operand_pos);
      operand = {OperandKind::Jump, value, {}};
      ins.targets.emplace_back(value, key == 0x05 || key == 0x57 || key == 0x58);
    } else if (token == "A4") {
      operand = {OperandKind::Str, 0, std::string(reinterpret_cast<const char*>(&code[operand_pos]), 4)};
    } else if (token == "M") {
      operand = {OperandKind::Mode, code[operand_pos], {}};
    } else {
      // S / T with a mode bit.
      const int mask = std::stoi(std::string(token.substr(1)), nullptr, 16);
      const u16 value = rd16(code, operand_pos);
      if (mode & mask) {
        operand = {OperandKind::Imm, static_cast<s16>(value), {}};
      } else {
        operand = {OperandKind::Var, value, {}};
      }
    }
    ins.ops.push_back(std::move(operand));
  }
  if (key == 0x02) ins.cond = mode & 0xF;
  if (key == 0x00 || key == 0x03 || key == 0x04 || key == 0x0D || key == 0x3B) {
    ins.flow = Flow::End;
  } else if (key == 0x01 || key == 0xD0) {
    ins.flow = Flow::Goto;
  }
  return ins;
}

std::optional<FieldScriptModule> FieldScriptModule::parse(ByteSpan blob, std::string* error) {
  auto fail = [&](const char* message) -> std::optional<FieldScriptModule> {
    if (error) *error = message;
    return std::nullopt;
  };
  constexpr size_t kHeader = 0x80;
  if (blob.size() < kHeader + 4) return fail("script too short");
  SpanReader reader(blob);
  FieldScriptModule module;
  for (auto& word : module.sign_mask) word = reader.u32_();
  const u32 count = reader.u32_();
  if (count > 0x100 || kHeader + 4 + static_cast<size_t>(count) * 0x40 > blob.size()) return fail("entity table past end");
  module.entities.resize(count);
  for (auto& entity : module.entities)
    for (auto& ptr : entity) ptr = reader.u16_();
  module.code_base = static_cast<u32>(kHeader + 4 + count * 0x40);
  module.code.assign(blob.begin() + module.code_base, blob.end());
  // Pre-script table: 0xFF then n * 8-byte records, ending where the first real script starts.
  u32 first = 0xFFFFFFFF;
  for (const auto& entity : module.entities)
    for (u16 ptr : entity)
      if (ptr) first = std::min<u32>(first, ptr);
  if (!module.code.empty() && module.code[0] == 0xFF && first != 0xFFFFFFFF && first > 1 && (first - 1) % 8 == 0 &&
      first <= module.code.size()) {
    for (u32 offset = 1; offset + 8 <= first; offset += 8)
      module.positions.push_back({static_cast<s16>(rd16(module.code, offset)), static_cast<s16>(rd16(module.code, offset + 2)),
                                  static_cast<s16>(rd16(module.code, offset + 4)), static_cast<s16>(rd16(module.code, offset + 6))});
  }
  return module;
}

// ------------------------------------------------------------------ disassembler (port of cc_fieldscript.py)
namespace {

const char* kScriptNames[] = {"INIT", "UPDATE", "ON_ACTION", "ON_COLLISION"};
const char* kCharNames[] = {"Serge", "Kid",    "Guile",  "Norris", "Nikki",  "Viper",   "Riddel", "Karsh", "Zoah",
                            "Marcy", "Korcha", "Luccia", "Poshul", "Razzly", "Zappa",   "Orcha",  "Radius", "Fargo",
                            "Macha", "Glenn",  "Leena",  "Miki",   "Harle",  "Janice",  "Draggy", "Starky", "Sprigg",
                            "Mojo",  "Turnip", "NeoFio", "Greco",  "Skelly", "Funguy",  "Irenes", "Mel",    "Leah",
                            "Van",   "Sneff",  "Steena", "Doc",    "Grobyc", "Pierre",  "Orlha",  "Pip"};

std::string var_name(u32 value) {
  value &= 0xFFFF;
  switch (value) {
    case 0x02: return "$ROOM_PARAM";
    case 0x04: return "$MULTI_CHOICE_RESULT";
    case 0x06: return "$MAP_ID";
    case 0x08: return "$PREVIOUS_MAP_ID";
    case 0x0C: return "$PARTY_MEMBER_0";
    case 0x0E: return "$PARTY_MEMBER_1";
    case 0x10: return "$PARTY_MEMBER_2";
    case 0x18: return "$DIGIT_ENTRY_RESULT";
    case 0x2A: return "$USED_KEY_ITEM";
    case 0x32: return "$BATTLE_RESULT";
    case 0x34: return "$DEBUG_MODE";
    case 0x44: return "$CURRENT_FRAME";
    case 0x4C: return "$SAVE_RESULT";
    default: break;
  }
  if (value >= 0x400) return std::format("$room[{}]", (value - 0x400) / 2);
  return std::format("${:04X}", value);
}

std::string char_name(u32 id) {
  if (id == 0xFF) return "LEADER";
  if (id == 0xFE) return "PC1";
  if (id == 0xFD) return "PC2";
  if (id == 0xFC) return "PC3";
  if (id == 0xFB) return "THIS";
  if (id >= 0x80) {
    const u32 i = id - 0x80;
    return i < std::size(kCharNames) ? kCharNames[i] : std::format("char{:02X}", id);
  }
  return std::format("ent{}", id);
}

std::string fmt_operand(const Operand& operand) {
  switch (operand.kind) {
    case OperandKind::Imm: return std::format("#{}", operand.value);
    case OperandKind::Var: return var_name(static_cast<u32>(operand.value));
    case OperandKind::Jump: return std::format("L{:04X}", operand.value);
    case OperandKind::Char: return char_name(static_cast<u32>(operand.value));
    case OperandKind::Byte: return std::format("0x{:02X}", operand.value);
    case OperandKind::Word: return std::format("0x{:04X}", operand.value);
    case OperandKind::Str: return "\"" + operand.text + "\"";
    case OperandKind::Mode: return std::format("m={:02X}", operand.value);
  }
  return {};
}

std::string fmt_insn(const Insn& ins) {
  std::vector<std::string> parts;
  for (const auto& operand : ins.ops)
    if (operand.kind != OperandKind::Mode) parts.push_back(fmt_operand(operand));
  std::string txt;
  if (ins.key == 0x02 && parts.size() >= 3) {
    static const char* kCond[] = {"==", "!=", ">", "<", ">=", "<=", "&", "!=7", "|", "&9", "~"};
    const std::string cond = ins.cond >= 0 && ins.cond <= 10 ? kCond[ins.cond] : std::format("cond{:X}", ins.cond);
    txt = std::format("IF_JUMP   {} {} {} -> {}", parts[0], cond, parts[1], parts[2]);
  } else {
    std::string args;
    for (size_t i = 0; i < parts.size(); ++i) args += (i ? ", " : "") + parts[i];
    txt = std::format("{:<9} {}", ins.info->name, args);
  }
  while (!txt.empty() && txt.back() == ' ') txt.pop_back();
  return txt;
}

std::string hexbytes(const u8* bytes, size_t size) {
  std::string text;
  for (size_t i = 0; i < size; ++i) text += std::format("{}{:02X}", i ? " " : "", bytes[i]);
  return text;
}

struct Listing {
  std::map<u32, Insn> insns, dead;
  std::map<u32, std::vector<std::pair<int, int>>> entries;
  std::set<u32> subs, labels;
  std::vector<std::pair<u32, std::string>> errors;
};

std::vector<u8> covered(const FieldScriptModule& module, const Listing& listing, bool with_dead) {
  std::vector<u8> cov(module.code.size(), 0);
  auto mark = [&](const std::map<u32, Insn>& source) {
    for (const auto& [offset, insn] : source)
      for (u32 k = offset; k < offset + insn.length && k < cov.size(); ++k) cov[k] = 1;
  };
  mark(listing.insns);
  if (with_dead) mark(listing.dead);
  return cov;
}

Listing explore(const FieldScriptModule& module) {
  Listing listing;
  const ByteSpan code(module.code);
  for (size_t entity = 0; entity < module.entities.size(); ++entity)
    for (size_t k = 0; k < kScriptSlots; ++k) {
      const u16 ptr = module.entities[entity][k];
      if (ptr || k == 0) listing.entries[ptr].emplace_back(static_cast<int>(entity), static_cast<int>(k));
    }
  std::vector<u32> work;
  for (const auto& [ptr, slots] : listing.entries) work.push_back(ptr);
  std::set<u32> seen;
  while (!work.empty()) {
    const u32 start = work.back();
    work.pop_back();
    if (!seen.insert(start).second) continue;
    u32 off = start;
    for (;;) {
      if (listing.insns.count(off)) break;
      if (off >= code.size()) {
        listing.errors.emplace_back(start, "runs off end of code");
        break;
      }
      std::string err;
      auto ins = decode_insn(code, off, &err);
      if (!ins) {
        listing.errors.emplace_back(off, err);
        break;
      }
      const Insn& insn = listing.insns.emplace(off, std::move(*ins)).first->second;
      for (const auto& [target, is_call] : insn.targets) {
        if (target >= code.size()) {
          listing.errors.emplace_back(off, std::format("jump target {:04X} out of range", target));
        } else {
          work.push_back(target);
          (is_call ? listing.subs : listing.labels).insert(target);
        }
      }
      if (insn.flow != Flow::Next) break;
      off += insn.length;
    }
  }
  // Unreferenced code: clean runs of instructions in the gaps that end in a terminator.
  auto cov = covered(module, listing, false);
  const size_t size = code.size();
  size_t i = 0;
  while (i < size) {
    if (cov[i]) {
      ++i;
      continue;
    }
    std::vector<Insn> run;
    size_t j = i;
    bool ok = false;
    while (j < size && !cov[j]) {
      auto ins = decode_insn(code, static_cast<u32>(j));
      if (!ins) break;
      bool clash = false;
      for (u32 k = ins->off; k < ins->off + ins->length && k < size; ++k) clash |= cov[k] != 0;
      if (clash) break;
      j += ins->length;
      const Flow flow = ins->flow;
      run.push_back(std::move(*ins));
      if (flow != Flow::Next) {
        ok = true;
        break;
      }
    }
    if (ok) {
      for (auto& ins : run) {
        for (u32 k = ins.off; k < ins.off + ins.length; ++k) cov[k] = 1;
        listing.dead.emplace(ins.off, std::move(ins));
      }
      i = j;
    } else {
      while (i < size && !cov[i]) ++i;
    }
  }
  return listing;
}

std::set<u32> reach(const Listing& listing, std::vector<u32> stack) {
  std::set<u32> seen;
  while (!stack.empty()) {
    u32 offset = stack.back();
    stack.pop_back();
    for (;;) {
      auto it = listing.insns.find(offset);
      if (it == listing.insns.end() || seen.count(offset)) break;
      seen.insert(offset);
      for (const auto& target : it->second.targets) stack.push_back(target.first);
      if (it->second.flow != Flow::Next) break;
      offset += it->second.length;
    }
  }
  return seen;
}

}  // namespace

std::string disassemble(const FieldScriptModule& module, int entity, int script) {
  const Listing listing = explore(module);
  std::string out;
  out += std::format("; script entities={}  code_base=0x{:X}  code_len=0x{:X}  insns={}  unreferenced={}  errors={}\n",
                     module.entities.size(), module.code_base, module.code.size(), listing.insns.size(), listing.dead.size(), listing.errors.size());
  std::optional<std::set<u32>> keep;
  if (entity >= 0 && static_cast<size_t>(entity) < module.entities.size()) {
    std::vector<u32> starts;
    for (int k = 0; k < kScriptSlots; ++k)
      if ((script < 0 || k == script) && module.entities[entity][k]) starts.push_back(module.entities[entity][k]);
    keep = reach(listing, starts);
  }
  std::map<u32, const Insn*> all;
  for (const auto& [offset, insn] : listing.insns) all[offset] = &insn;
  for (const auto& [offset, insn] : listing.dead) all[offset] = &insn;
  std::optional<u32> last_end;
  u32 off = 0;
  const u32 size = static_cast<u32>(module.code.size());
  while (off < size) {
    auto it = all.find(off);
    if (it == all.end()) {
      u32 j = off;
      while (j < size && !all.count(j)) ++j;
      if (!keep) {
        if (last_end) out += "\n";
        for (u32 k = off; k < std::min(j, off + 4096); k += 16)
          out += std::format("  {:04X}: .data {}\n", k, hexbytes(&module.code[k], std::min<u32>(16, j - k)));
      }
      off = j;
      last_end.reset();
      continue;
    }
    const Insn& ins = *it->second;
    if (keep && !keep->count(off)) {
      off += ins.length;
      continue;
    }
    if (last_end && off != *last_end) out += "\n";
    if (auto entry = listing.entries.find(off); entry != listing.entries.end())
      for (const auto& [ent, k] : entry->second)
        if ((entity < 0 || ent == entity) && (script < 0 || k == script))
          out += std::format("; ---- entity {} script {} ({})  ptr={:04X}  file={:X}\n", ent, k,
                             k < 4 ? std::string(kScriptNames[k]) : std::format("script{}", k), off, module.code_base + off);
    if (listing.subs.count(off))
      out += std::format("sub_{:04X}:\n", off);
    else if (listing.labels.count(off))
      out += std::format("L{:04X}:\n", off);
    const bool dead = listing.dead.count(off) != 0;
    out += std::format("  {:04X}: {:<24} {}{}\n", off, hexbytes(ins.raw.data(), ins.length), fmt_insn(ins),
                       dead ? "  ; unreferenced" : "");
    last_end = off + ins.length;
    off += ins.length;
  }
  for (const auto& [offset, msg] : listing.errors) out += std::format("; ERROR @{:04X} {}\n", offset, msg);
  return out;
}

}  // namespace oc::script
