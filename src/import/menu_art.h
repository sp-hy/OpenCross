#pragma once
// The menu art of the PS1 discs: the sprite sheets of menu/menutest.prd ("main", the element plates, "curs", "icon", "suji", "roll")
// and of the other menu packs (menu/menuelem.prd "elbi", menu/smithy.prd "face"), over the textures of menu/menutim.prd and of their
// own pack, each frame composed into one picture.
//
// The menus' compass art is not a picture: the meshes "deco" and "dec1" (the faint compass swirls, textured from menutim.prd) and "hari"
// (the dial needle, flat grey triangles) of menutest.prd, all flat (z = 0), are drawn at a screen point, scaled and turned about it. The
// importer hands them over as triangles with their texture pages cut out as pictures; the menus draw them.
//
// A frame is drawn with its origin at a screen point and its parts at their own offsets from it, the last part underneath. The panel
// frames of "main" carry their window lines, caption words and icons in one frame.
//   auto art = import::load_menu_art(menutest, menutim, {menuelem, smithy});
//   import::MenuPicture card = art->compose("main", 7);   // the party card, origin at (-card.x, -card.y)
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <array>

#include "assets/room_effects.h"
#include "core/types.h"

namespace oc::import {

// One composed frame: (x, y) = the top-left of the picture relative to the frame origin; rgba w * h, alpha 0 transparent, else 255.
struct MenuPicture {
  int x = 0, y = 0, w = 0, h = 0;
  std::vector<u8> rgba;
  explicit operator bool() const { return w > 0 && h > 0; }
};

struct MenuArt {
  std::vector<assets::SpriteSheet> sheets;

  // The parts of frame `frame` (1-based) of the named sheet; null when the sheet or frame is missing.
  const std::vector<assets::EffectSprite>* parts(std::string_view sheet, int frame) const;
  // The rectangle the frame's parts cover, relative to its origin (no pixels); empty when the frame is missing.
  MenuPicture extent(std::string_view sheet, int frame) const;
  MenuPicture compose(std::string_view sheet, int frame) const;

  struct Triangle {
    std::array<std::array<s16, 2>, 3> pos{};
    std::array<std::array<u8, 3>, 3> rgb{};
    std::array<std::array<u8, 2>, 3> uv{};
    // Index into Mesh::pages, -1 untextured.
    int page = -1;
  };
  // A flat menu mesh: its triangles (vertex x, y in mesh units, a packet colour per corner) and the 256 x 256 texture pages they sample
  // (alpha 0 transparent, kStpAlpha a texel with the semi-transparency bit, 255 the others).
  struct Mesh {
    std::string name;
    std::vector<Triangle> triangles;
    std::vector<MenuPicture> pages;
  };
  std::vector<Mesh> meshes;
  const Mesh* mesh(std::string_view name) const;
};

// `menutest` = menu/menutest.prd, `menutim` = menu/menutim.prd, `packs` = further menu packs of the same build, each over menutim's
// textures and its own; a sheet name already loaded keeps its first copy. Null when menutest.prd holds no sheet.
std::optional<MenuArt> load_menu_art(ByteSpan menutest, ByteSpan menutim, const std::vector<ByteSpan>& packs = {});

}  // namespace oc::import
