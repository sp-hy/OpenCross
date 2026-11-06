#include "script/mod_ui.h"

#include <cstdlib>
#include <limits>

namespace oc::script {

std::array<std::array<u8, 3>, ModUi::kPalette> ModUi::default_palette() {
  std::array<std::array<u8, 3>, kPalette> out;
  out.fill({0xFF, 0xFF, 0xFF});
  out[1] = {0xE0, 0x30, 0x30};
  out[2] = {0xFF, 0x80, 0x20};
  out[3] = {0xE0, 0x20, 0x80};
  out[4] = {0xF0, 0xD0, 0x20};
  out[5] = {0x40, 0xD0, 0x40};
  out[6] = {0x40, 0x80, 0xF0};
  out[7] = {0x40, 0xE0, 0xE0};
  out[8] = {0xA0, 0xA0, 0xA0};
  out[9] = {0x00, 0x00, 0x00};
  out[10] = {0x08, 0x10, 0x20};
  out[11] = {0x58, 0x58, 0x58};
  out[12] = {0xC8, 0xC8, 0xC8};
  return out;
}

void ModUi::clear() {
  const u32 kept = serial;
  *this = ModUi{};
  serial = kept;
}

bool ModUi::selectable(int id) const {
  if (id < 0 || id >= kElements) return false;
  const Element& element = elements[static_cast<size_t>(id)];
  return element.kind == kItem && element.visible && !(element.flags & kDisabled);
}

std::pair<int, int> ModUi::centre(int id) const {
  if (id < 0 || id >= kElements) return {0, 0};
  const Element& element = elements[static_cast<size_t>(id)];
  if (element.kind == kLine) return {(element.x + element.w) / 2, (element.y + element.h) / 2};
  if (element.kind == kText) return {element.x, element.y};
  return {element.x + element.w / 2, element.y + element.h / 2};
}

int ModUi::step(int dir) const {
  if (cursor < 0 || cursor >= kElements) return -1;
  const auto [fx, fy] = centre(cursor);
  int best = -1, best_score = std::numeric_limits<int>::max();
  for (int i = 0; i < kElements; ++i) {
    if (i == cursor || !selectable(i)) continue;
    const auto [x, y] = centre(i);
    const bool horizontal = (dir & 1) != 0;
    int along = horizontal ? x - fx : y - fy;
    const int across = horizontal ? y - fy : x - fx;
    if (dir == 0 || dir == 3) along = -along;
    if (along <= 0) continue;
    const int score = along + 2 * std::abs(across);
    if (score < best_score) best_score = score, best = i;
  }
  return best;
}

bool ModUi::set(int id, int field, int value) {
  if (id < 0 || id >= kElements) return false;
  Element& element = elements[static_cast<size_t>(id)];
  const s16 v = static_cast<s16>(value);
  switch (field) {
    case kFieldVisible: element.visible = value != 0; break;
    case kFieldEnabled: element.flags = value != 0 ? static_cast<u8>(element.flags & ~kDisabled) : static_cast<u8>(element.flags | kDisabled); break;
    case kFieldColour: element.colour = static_cast<u8>(value % kPalette); break;
    case kFieldX: element.x = v; break;
    case kFieldY: element.y = v; break;
    case kFieldMsg: element.msg = v; break;
    case kFieldFlags: element.flags = static_cast<u8>(value); break;
    case kFieldW: element.w = v; break;
    case kFieldH: element.h = v; break;
    default: return false;
  }
  element.revision = ++serial;
  return true;
}

}  // namespace oc::script
