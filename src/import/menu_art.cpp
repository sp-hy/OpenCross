#include "import/menu_art.h"

#include <algorithm>
#include <cstring>

#include "import/battle_stage.h"
#include "import/container.h"
#include "import/room_effects.h"
#include "import/tim.h"

namespace oc::import {

namespace {

// The texel (u, v) of a face's texture page through its CLUT; alpha 0 for colour 0, kStpAlpha with the semi-transparency bit.
u32 page_texel(const Vram& vram, u16 tpage, u16 clut, int u, int v) {
  const int page_x = (tpage & 0xF) * 64, row = ((tpage & 0x10) ? 256 : 0) + (v & 255), depth = (tpage >> 7) & 3;
  unsigned index;
  if (depth == 0) index = (vram.at(page_x + (u >> 2), row) >> ((u & 3) * 4)) & 15u;
  else if (depth == 1) index = (vram.at(page_x + (u >> 1), row) >> ((u & 1) * 8)) & 255u;
  else return bgr555_to_rgba_stp(vram.at(page_x + u, row));
  return bgr555_to_rgba_stp(vram.at((clut & 0x3F) * 16 + static_cast<int>(index), clut >> 6));
}

// A packet mesh as triangles (quads split as (0, 1, 2), (2, 1, 3)), each textured face's (tpage, CLUT) cut once into a page picture.
MenuArt::Mesh resolve_mesh(std::string name, const StageMeshSrc& src, const Vram& vram) {
  MenuArt::Mesh mesh;
  mesh.name = std::move(name);
  std::vector<u32> page_keys;
  for (const StageFace& face : src.faces) {
    int page = -1;
    if (face.textured) {
      const u32 key = static_cast<u32>(face.tpage) << 16 | face.clut;
      const auto found = std::find(page_keys.begin(), page_keys.end(), key);
      page = static_cast<int>(found - page_keys.begin());
      if (found == page_keys.end()) {
        page_keys.push_back(key);
        MenuPicture picture;
        picture.w = picture.h = 256;
        picture.rgba.resize(256 * 256 * 4);
        for (int v = 0; v < 256; ++v)
          for (int u = 0; u < 256; ++u) store_rgba(&picture.rgba[(static_cast<size_t>(v) * 256 + static_cast<size_t>(u)) * 4], page_texel(vram, face.tpage, face.clut, u, v));
        mesh.pages.push_back(std::move(picture));
      }
    }
    const int corners[2][3] = {{0, 1, 2}, {2, 1, 3}};
    for (int half = 0; half < (face.n == 4 ? 2 : 1); ++half) {
      MenuArt::Triangle triangle;
      triangle.page = page;
      for (int k = 0; k < 3; ++k) {
        const size_t corner = static_cast<size_t>(corners[half][k]);
        const auto& vert = src.verts[std::min<size_t>(face.idx[corner], src.verts.size() - 1)];
        triangle.pos[static_cast<size_t>(k)] = {vert[0], vert[1]};
        triangle.rgb[static_cast<size_t>(k)] = face.rgb[corner];
        triangle.uv[static_cast<size_t>(k)] = face.uv[corner];
      }
      mesh.triangles.push_back(triangle);
    }
  }
  return mesh;
}

}  // namespace

const MenuArt::Mesh* MenuArt::mesh(std::string_view name) const {
  for (const Mesh& entry : meshes)
    if (entry.name == name) return &entry;
  return nullptr;
}

const std::vector<assets::EffectSprite>* MenuArt::parts(std::string_view sheet, int frame) const {
  for (const assets::SpriteSheet& entry : sheets) {
    if (entry.name != sheet) continue;
    if (frame < 1 || static_cast<size_t>(frame) > entry.frames.size()) return nullptr;
    return &entry.frames[static_cast<size_t>(frame - 1)];
  }
  return nullptr;
}

MenuPicture MenuArt::extent(std::string_view sheet, int frame) const {
  MenuPicture out;
  const std::vector<assets::EffectSprite>* list = parts(sheet, frame);
  if (!list || list->empty()) return out;
  int x0 = (*list)[0].x, y0 = (*list)[0].y, x1 = x0, y1 = y0;
  for (const assets::EffectSprite& part : *list) {
    x0 = std::min<int>(x0, part.x), y0 = std::min<int>(y0, part.y);
    x1 = std::max(x1, part.x + part.w), y1 = std::max(y1, part.y + part.h);
  }
  out.x = x0, out.y = y0, out.w = x1 - x0, out.h = y1 - y0;
  return out;
}

MenuPicture MenuArt::compose(std::string_view sheet, int frame) const {
  MenuPicture out = extent(sheet, frame);
  if (!out) return out;
  const std::vector<assets::EffectSprite>* list = parts(sheet, frame);
  const int x0 = out.x, y0 = out.y;
  out.rgba.assign(static_cast<size_t>(out.w) * static_cast<size_t>(out.h) * 4, 0);
  for (auto it = list->rbegin(); it != list->rend(); ++it) {
    const assets::EffectSprite& part = *it;
    // Flag bit 11 draws the part as a plain rectangle, which cannot be mirrored.
    const bool quad = (part.flags & 0x800) == 0;
    for (int j = 0; j < part.h; ++j)
      for (int i = 0; i < part.w; ++i) {
        const int src_x = quad && part.mirror_x() ? part.w - 1 - i : i, src_y = quad && part.mirror_y() ? part.h - 1 - j : j;
        const u8* texel = &part.rgba[(static_cast<size_t>(src_y) * part.w + static_cast<size_t>(src_x)) * 4];
        if (texel[3] == 0) continue;
        std::memcpy(&out.rgba[(static_cast<size_t>(part.y - y0 + j) * static_cast<size_t>(out.w) + static_cast<size_t>(part.x - x0 + i)) * 4], texel, 4);
      }
  }
  return out;
}

std::optional<MenuArt> load_menu_art(ByteSpan menutest, ByteSpan menutim, const std::vector<ByteSpan>& packs) {
  Vram vram;
  if (auto drp = parse_drp(menutim))
    for (const DrpEntry& entry : drp->entries)
      if (entry.type == 4)
        if (auto tim = parse_tim(entry.data)) vram.blit_tim(*tim);
  MenuArt art;
  art.sheets = load_room_effects(menutest, vram).sheets;
  if (art.sheets.empty()) return std::nullopt;
  if (auto drp = parse_drp(menutest))
    for (const DrpEntry& entry : drp->entries)
      if (entry.type == 2)
        if (auto mesh = parse_stage_mesh(entry.data); mesh && !mesh->verts.empty()) art.meshes.push_back(resolve_mesh(entry.name, *mesh, vram));
  for (ByteSpan pack : packs)
    for (assets::SpriteSheet& sheet : load_room_effects(pack, vram).sheets) art.sheets.push_back(std::move(sheet));
  return art;
}

}  // namespace oc::import
