// Extension range (FE C0..FE FF): ops that turn on engine features for mods. Each instruction is FE sub size payload[size];
// a handler reads its operands from the start of the payload and the VM always steps over the whole instruction, so a payload
// may also carry operands that a later version of the op adds.
#include "script/field_vm.h"

#include <cstdlib>

#include "core/log.h"

namespace oc::script {

namespace {

// Feature ids of GET_FEATURE_LEVEL.
namespace feature {
constexpr s32 kExtensionRange = 0;
}  // namespace feature

// The level of a feature this build implements, 0 when it has none.
s32 feature_level(s32 id) {
  switch (id) {
    case feature::kExtensionRange: return 1;
    default: return 0;
  }
}

u32 payload_size(const OpInfo& info) { return static_cast<u32>(std::atoi(info.lengths)) - 3; }

}  // namespace

void FieldVM::exec_extension(u8 sub) {
  const u16 key = static_cast<u16>(kExtPage | sub);
  // `base_` is the sub-opcode byte: byte 1 is the payload size, the payload starts at byte 2.
  const u32 size = byte_at(1);
  const u32 length = 2 + size;
  const OpInfo* info = find_op(key);
  if (!info) {
    note_unknown(key);
    advance(length);
    return;
  }
  if (size < payload_size(*info)) {
    ++stats_.unknown_ops[key];
    if (logged_.insert(key).second)
      log::warn("field script: extension op FE {:02X} ({}) has a {}-byte payload, needs {} (skipped)", sub, info->name, size,
                payload_size(*info));
    advance(length);
    return;
  }
  switch (sub) {
    case 0xC0: write_var(word_at(2), feature_level(operand_u(4))); break;
    default:
      ++stats_.unimplemented;
      if (logged_.insert(key).second) log::warn("field script: extension op FE {:02X} ({}) has no handler", sub, info->name);
      break;
  }
  advance(length);
}

}  // namespace oc::script
