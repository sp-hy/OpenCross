#pragma once
// RDE's corrections to individual rooms' field scripts, kept as data: byte edits applied to the decoded script subfile (subfile 3 of
// map/mapbin/<room>.bin) when a room is imported, so the interpreter runs the corrected bytecode and has no room-specific code.
#include <span>
#include <string>
#include <string_view>

#include "core/types.h"

namespace oc::import {

// One edit. `offset` counts from the start of the code area, like jump targets. `expect` and `bytes` are hex strings (spaces are
// ignored) of the same length; a row with an empty `expect` and `offset` equal to the code area's current size appends `bytes`
// (code reached only through a jump that an earlier row of the same room writes).
struct ScriptFix {
  std::string_view room;
  u32 offset;
  std::string_view expect;
  std::string_view bytes;
};

// Every row, grouped by room.
std::span<const ScriptFix> script_fixes();

// Applies `rows` (normally script_fixes()) for `room` to `script`. Every row of the room is checked first; when one does not match
// (other data, a malformed row), nothing is changed, *error says why and the result is -1. Otherwise returns the number of rows
// applied (0 for a room without corrections).
int apply_script_fixes(std::string_view room, Bytes& script, std::span<const ScriptFix> rows, std::string* error = nullptr);
inline int apply_script_fixes(std::string_view room, Bytes& script, std::string* error = nullptr) {
  return apply_script_fixes(room, script, script_fixes(), error);
}

}  // namespace oc::import
