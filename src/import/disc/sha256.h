#pragma once
// SHA-256 (FIPS 180-4) of a byte span, as 64 lower-case hex digits.
#include <string>

#include "core/types.h"

namespace oc::import::disc {

std::string sha256_hex(ByteSpan bytes);

}  // namespace oc::import::disc
