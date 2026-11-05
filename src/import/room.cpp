#include "import/room.h"

#include <algorithm>
#include <exception>
#include <map>
#include <set>

#include "core/span_reader.h"
#include "import/container.h"
#include "import/lzss.h"
#include "import/model.h"
#include "import/room_effects.h"
#include "import/script_fixes.h"
#include "import/tim.h"

namespace oc::import {

// The loader uploads a 256-wide rect: palettes are 512 bytes apart in the file, 2048 bytes (one VRAM row) apart in VRAM.
void write_room_cluts(Vram& vram, ByteSpan clut) {
  for (size_t k = 0; k * 512 < clut.size(); ++k)
    vram.write_bytes(kClutVramByteOffset + k * 2048, clut.subspan(k * 512, std::min<size_t>(512, clut.size() - k * 512)));
}

namespace {

enum Sec : size_t {
  kCamera = 0, kClut = 1, kStub = 2, kScript = 3, kTris = 4, kVerts = 5, kConst = 6,
  kLayers = 7, kDialog = 8, kSnd1 = 9, kSnd0 = 10, kModel0 = 11
};

struct Fail {
  std::string msg;
};

// Room cpt: u32 count; u32 off[count]; u32 eof. Subfile i = off[i]..off[i+1] (last: ..eof).
std::vector<ByteSpan> split_cpt(ByteSpan data, const char* what) {
  auto cpt = parse_cpt(data);
  if (!cpt) throw Fail{std::string(what) + ": bad cpt"};
  if (!cpt->has_eof_pointer) throw Fail{std::string(what) + ": cpt without EOF pointer"};
  if (cpt->entries.front().offset != 8 + 4 * cpt->entries.size()) throw Fail{std::string(what) + ": first offset mismatch"};
  std::vector<ByteSpan> out;
  out.reserve(cpt->entries.size());
  for (const auto& entry : cpt->entries) out.push_back(entry.data);
  return out;
}

// sszl subfile -> bytes; checks the decoded length against the header.
Bytes unpack(ByteSpan packed, const std::string& what) {
  if (!is_lzss(packed) || packed.size() < 12) throw Fail{what + ": not sszl"};
  auto decoded = lzss_decode(packed);
  if (!decoded) throw Fail{what + ": sszl decode failed"};
  SpanReader header(packed, 4);
  if (decoded->size() != header.u32_()) throw Fail{what + ": sszl decoded length differs from header"};
  return std::move(*decoded);
}

struct Parsed {
  std::vector<ByteSpan> bin;
  std::vector<Tim> tims;
  Vram vram;
  Bytes layer_data;
  Bytes stub;
};

void parse_files(ByteSpan bin_file, ByteSpan ctd_file, Parsed& parsed) {
  parsed.bin = split_cpt(bin_file, "bin");
  if (parsed.bin.size() < kModel0) throw Fail{"bin: only " + std::to_string(parsed.bin.size()) + " subfiles"};
  std::vector<ByteSpan> ctd = split_cpt(ctd_file, "ctd");
  for (size_t i = 0; i < ctd.size(); ++i) {
    const bool packed = is_lzss(ctd[i]);
    Bytes raw;
    if (packed) raw = unpack(ctd[i], "ctd[" + std::to_string(i) + "]");
    auto tim = parse_tim(packed ? ByteSpan(raw) : ctd[i]);
    if (!tim) throw Fail{"ctd[" + std::to_string(i) + "]: bad TIM"};
    parsed.vram.blit_tim(*tim);
    parsed.tims.push_back(std::move(*tim));
  }
  write_room_cluts(parsed.vram, parsed.bin[kClut]);
}

assets::RoomCamera parse_camera(ByteSpan data) {
  if (data.size() < 40) throw Fail{"camera: subfile 0 too small"};
  SpanReader reader(data);
  assets::RoomCamera camera;
  for (auto& cell : camera.matrix) cell = reader.s16_();
  for (auto& component : camera.trans) component = reader.s16_();
  camera.proj_h = reader.s16_();
  camera.x_max = reader.s16_();
  camera.x_min = reader.s16_();
  camera.y_max = reader.s16_();
  camera.y_min = reader.s16_();
  camera.unknown34 = reader.s16_();
  camera.n_groups = reader.u32_();
  const size_t available = (data.size() - 40) / 4;
  for (size_t i = 0; i < std::min<size_t>(camera.n_groups, available); ++i) camera.group_scale.push_back(reader.u32_());
  return camera;
}

// u32 layer count; u32 first_tile[count + 1]; 12-byte tiles. The last layer runs to the last tile.
std::vector<assets::RoomLayer> parse_layers(ByteSpan data, bool strict_size) {
  SpanReader reader(data);
  const u32 layer_count = reader.u32_();
  if (layer_count == 0 || layer_count > 64) throw Fail{"layers: layer count " + std::to_string(layer_count)};
  std::vector<u32> start(layer_count + 1);
  for (auto& entry : start) entry = reader.u32_();
  const size_t base = 4 + 4ull * (layer_count + 1);
  const u64 tile_count = static_cast<u64>(start[layer_count]) + 1;
  if (base + 12 * tile_count > data.size()) throw Fail{"layers: tile data truncated"};
  if (strict_size && base + 12 * tile_count != data.size()) throw Fail{"layers: decoded size is not exactly the tile data"};
  for (u32 i = 0; i < layer_count; ++i)
    if (start[i] > start[i + 1]) throw Fail{"layers: table not monotonic"};
  std::vector<assets::RoomLayer> layers(layer_count);
  for (u32 li = 0; li < layer_count; ++li) {
    layers[li].index = li;
    const u32 end = li + 1 < layer_count ? start[li + 1] : static_cast<u32>(tile_count);
    for (u32 ti = start[li]; ti < end; ++ti) {
      SpanReader tile_reader(data, base + 12ull * ti);
      assets::RoomTile tile;
      tile.x = tile_reader.s16_();
      tile.y = tile_reader.s16_();
      tile.u = tile_reader.u16_();
      tile.v = tile_reader.u8_();
      tile.clut = tile_reader.u8_();
      tile.group = tile_reader.u8_();
      tile.flags = tile_reader.u8_();
      tile.z = tile_reader.u16_();
      tile.index = ti;
      layers[li].tiles.push_back(tile);
    }
  }
  return layers;
}

// One image per blend kind present in each group (opaque = -1; semi-transparent tiles contribute their STP texels to the
// image of their ABR and their other texels to the opaque one).
void compose_groups(const Vram& vram, u32 clut_rows, assets::RoomLayer& layer) {
  std::map<u8, std::vector<const assets::RoomTile*>> by_group;
  for (const auto& tile : layer.tiles) by_group[tile.group].push_back(&tile);
  for (const auto& [group, tiles] : by_group) {
    int x0 = tiles[0]->x, y0 = tiles[0]->y, x1 = x0, y1 = y0;
    for (const auto* tile : tiles) {
      x0 = std::min<int>(x0, tile->x);
      y0 = std::min<int>(y0, tile->y);
      x1 = std::max<int>(x1, tile->x);
      y1 = std::max<int>(y1, tile->y);
    }
    std::map<int, assets::RoomGroupImage> images;
    auto image_for = [&](int blend) -> assets::RoomGroupImage& {
      auto [it, fresh] = images.try_emplace(blend);
      assets::RoomGroupImage& img = it->second;
      if (fresh) {
        img.blend = blend;
        img.group = group;
        img.x = x0;
        img.y = y0;
        img.width = static_cast<u32>(x1 - x0 + kRoomTileSize);
        img.height = static_cast<u32>(y1 - y0 + kRoomTileSize);
        img.z = tiles[0]->z;
        img.rgba.assign(static_cast<size_t>(img.width) * img.height * 4, 0);
      }
      return img;
    };
    for (const auto* tile : tiles) {
      const TileSource src = tile_source(tile->u, tile->v, tile->clut);
      if ((tile->clut >> 3) >= static_cast<int>(clut_rows)) continue;
      const bool semi = (tile->flags & 0x80) != 0;
      const int abr = (tile->flags >> 3) & 3;
      const std::vector<u16> palette = vram.read_clut(src.clut_x, src.clut_y, 256);
      for (int j = 0; j < kRoomTileSize; ++j)
        for (int i = 0; i < kRoomTileSize; ++i) {
          const u16 colour = vram.texel(src.tex_x + i, src.tex_y + j, 8, palette);
          if (colour == 0) continue;
          assets::RoomGroupImage& img = image_for(semi && (colour & 0x8000) ? abr : -1);
          store_rgba(&img.rgba[((static_cast<size_t>(tile->y - y0 + j)) * img.width + static_cast<size_t>(tile->x - x0 + i)) * 4],
                     bgr555_to_rgba(colour));
        }
    }
    for (auto& [blend, img] : images) layer.groups.push_back(std::move(img));
  }
}

// Triangles: u32 count; 14-byte records (3 x u16 vertex, 3 x s16 adjacent triangle, s16 attribute), optionally 2 bytes of
// padding. Vertices: u32 byte length; 8-byte records x, y, z, w (w always 0).
assets::WalkMesh parse_walk(ByteSpan tris, ByteSpan verts) {
  SpanReader triangle_reader(tris);
  const u32 triangle_count = triangle_reader.u32_();
  if (tris.size() != 4ull + 14ull * triangle_count && tris.size() != 4ull + 14ull * triangle_count + 2)
    throw Fail{"walk: triangle section size " + std::to_string(tris.size()) + " for " + std::to_string(triangle_count)};
  SpanReader vertex_reader(verts);
  const u32 vertex_bytes = vertex_reader.u32_();
  if (vertex_bytes % 8 || 4ull + vertex_bytes != verts.size()) throw Fail{"walk: vertex section size mismatch"};
  assets::WalkMesh mesh;
  for (u32 i = 0; i < triangle_count; ++i) {
    assets::WalkTriangle triangle;
    for (auto& index : triangle.v) index = triangle_reader.u16_();
    for (auto& adjacent : triangle.adj) adjacent = triangle_reader.s16_();
    triangle.attr = triangle_reader.s16_();
    mesh.triangles.push_back(triangle);
  }
  for (u32 i = 0; i < vertex_bytes / 8; ++i) {
    const float x = vertex_reader.s16_(), y = vertex_reader.s16_(), z = vertex_reader.s16_();
    vertex_reader.s16_();
    mesh.vertices.push_back({x, y, z});
  }
  return mesh;
}

// The CLUT-carrying TIM named by a model header is the model's texture. Faces may also select CLUT row + 1 or the page to the
// right, so the model is given the VRAM window it can reach (page_textures).
std::vector<assets::RoomModelHeader> parse_models(const std::vector<ByteSpan>& bin, const std::vector<Tim>& tims, const Vram& vram) {
  std::vector<assets::RoomModelHeader> out;
  for (size_t i = kModel0; i < bin.size(); ++i) {
    if (bin[i].size() < 12) throw Fail{"model " + std::to_string(i - kModel0) + ": header truncated"};
    SpanReader reader(bin[i]);
    assets::RoomModelHeader header;
    header.eof_rel = reader.u32_();
    header.img_x = reader.u16_();
    header.img_y = reader.u16_();
    header.clut_x = reader.u16_();
    header.clut_y = reader.u16_();
    header.size = static_cast<u32>(bin[i].size());
    header.mdl.assign(bin[i].begin() + 12, bin[i].end());
    for (const Tim& tim : tims) {
      if (!tim.has_clut || tim.image.x != header.img_x || tim.image.y != header.img_y || tim.clut.x != header.clut_x ||
          tim.clut.y != header.clut_y)
        continue;
      if (auto img = decode_tim(tim, 0, true)) header.texture = img->indexed ? to_rgba(*img) : std::move(*img);
      break;
    }
    header.page_textures = vram_page_textures(vram, header.img_x, header.img_y, header.clut_x, header.clut_y);
    out.push_back(std::move(header));
  }
  return out;
}

assets::RoomScene build(std::string_view name, ByteSpan bin, ByteSpan ctd, ByteSpan prd, bool compose, bool strict,
                        Parsed& parsed) {
  parse_files(bin, ctd, parsed);
  assets::RoomScene scene;
  scene.name = std::string(name);
  scene.camera = parse_camera(parsed.bin[kCamera]);
  if (parsed.bin[kClut].size() % 512) throw Fail{"clut: size is not a multiple of 512"};
  scene.clut_rows = static_cast<u32>(parsed.bin[kClut].size() / 512);
  parsed.layer_data = unpack(parsed.bin[kLayers], "layers");
  scene.layers = parse_layers(parsed.layer_data, strict);
  if (compose)
    for (auto& layer : scene.layers) compose_groups(parsed.vram, scene.clut_rows, layer);
  scene.walk = parse_walk(parsed.bin[kTris], parsed.bin[kVerts]);
  {
    SpanReader const_reader(parsed.bin[kConst]);
    for (size_t i = 0; i + 4 <= parsed.bin[kConst].size(); i += 4) scene.attr_flags.push_back(const_reader.u32_());
  }
  scene.models = parse_models(parsed.bin, parsed.tims, parsed.vram);
  parsed.stub = unpack(parsed.bin[kStub], "zones");
  for (size_t i = 0; i + 24 <= parsed.stub.size(); i += 24) {
    SpanReader zone_reader(ByteSpan(parsed.stub).subspan(i, 24));
    assets::RoomZone zone;
    for (auto& corner : zone.corner)
      for (auto& coordinate : corner) coordinate = zone_reader.s16_();
    scene.zones.push_back(zone);
  }
  scene.ctd_tims = static_cast<u32>(parsed.tims.size());
  scene.script_bytes = unpack(parsed.bin[kScript], "script");
  apply_script_fixes(name, scene.script_bytes);
  scene.dialogue_bytes = unpack(parsed.bin[kDialog], "dialogue");
  scene.yos1_bytes.assign(parsed.bin[kSnd1].begin(), parsed.bin[kSnd1].end());
  scene.yos0_bytes.assign(parsed.bin[kSnd0].begin(), parsed.bin[kSnd0].end());
  if (!prd.empty()) {
    auto drp = parse_drp(prd);
    if (!drp) throw Fail{"prd: bad drp"};
    for (const auto& entry : drp->entries) scene.prd.push_back({entry.name, entry.type, static_cast<u32>(entry.data.size())});
    scene.effects = load_room_effects(prd, parsed.vram);
  }
  return scene;
}

}  // namespace

std::optional<assets::RoomScene> load_room(std::string_view name, ByteSpan bin, ByteSpan ctd, ByteSpan prd,
                                           std::string* error) {
  try {
    Parsed parsed;
    return build(name, bin, ctd, prd, true, false, parsed);
  } catch (const Fail& failure) {
    if (error) *error = failure.msg;
  } catch (const std::exception& exception) {
    if (error) *error = std::string("malformed data: ") + exception.what();
  }
  return std::nullopt;
}

RoomValidation validate_room(ByteSpan bin, ByteSpan ctd) {
  RoomValidation validation;
  try {
    Parsed parsed;
    const assets::RoomScene scene = build("", bin, ctd, {}, false, true, parsed);
    validation.layers = static_cast<u32>(scene.layers.size());
    const auto& cam = scene.camera;
    if (parsed.bin[kCamera].size() != 40 + 4ull * cam.n_groups)
      validation.errors.push_back("CAM: size " + std::to_string(parsed.bin[kCamera].size()) + " != 40+4*" + std::to_string(cam.n_groups));
    for (size_t i : {kStub, kScript, kDialog}) {
      try {
        unpack(parsed.bin[i], "subfile " + std::to_string(i));
      } catch (const Fail& failure) {
        validation.errors.push_back("SSZL: " + failure.msg);
      }
    }
    if (parsed.bin[kConst].size() != 324 && parsed.bin[kConst].size() != 332)
      validation.errors.push_back("CONST: subfile 6 size " + std::to_string(parsed.bin[kConst].size()));
    for (size_t i : {kSnd1, kSnd0}) {
      const char* magic = i == kSnd1 ? "yos1" : "yos0";
      if (parsed.bin[i].size() < 4 || !std::equal(magic, magic + 4, parsed.bin[i].begin()))
        validation.errors.push_back(std::string("SND: subfile ") + std::to_string(i) + " lacks " + magic);
    }

    // Tiles.
    std::vector<u8> cover(static_cast<size_t>(Vram::kWidth) * Vram::kHeight, 0);
    for (const Tim& tim : parsed.tims)
      for (int row = 0; row < tim.image.h; ++row)
        for (int column = 0; column < tim.image.w; ++column) {
          const size_t x = static_cast<size_t>(tim.image.x + column), y = static_cast<size_t>(tim.image.y + row);
          if (x < Vram::kWidth && y < Vram::kHeight) cover[y * Vram::kWidth + x] = 1;
        }
    u32 bad_flags = 0, bad_clut = 0, bad_align = 0, bad_src = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    int max_group = -1;
    bool first = true;
    for (const auto& layer : scene.layers) {
      if (layer.tiles.empty()) validation.errors.push_back("LAYER: layer " + std::to_string(layer.index) + " is empty");
      for (const auto& tile : layer.tiles) {
        ++validation.tiles;
        if ((tile.flags & 0x60) != 0x20) ++bad_flags;
        if ((tile.clut & 7) || (tile.clut >> 3) >= static_cast<int>(scene.clut_rows)) ++bad_clut;
        if ((tile.u % 8) || (tile.v % 16)) ++bad_align;
        bool ok = tile.u + 8 <= Vram::kWidth;
        if (ok)
          for (int row : {0, 15}) {
            const size_t cell = static_cast<size_t>(kBgTextureRow + tile.v + row) * Vram::kWidth + tile.u;
            if (!cover[cell] || !cover[cell + 7]) ok = false;
          }
        if (!ok) ++bad_src;
        max_group = std::max<int>(max_group, tile.group);
        if (first) {
          bx0 = bx1 = tile.x;
          by0 = by1 = tile.y;
          first = false;
        }
        bx0 = std::min<int>(bx0, tile.x);
        by0 = std::min<int>(by0, tile.y);
        bx1 = std::max<int>(bx1, tile.x);
        by1 = std::max<int>(by1, tile.y);
      }
    }
    auto report_tiles = [&](u32 count, const char* what) {
      if (count) validation.errors.push_back("TILE: " + std::to_string(count) + " x " + what);
    };
    report_tiles(bad_flags, "flags (b9 & 0x60 != 0x20)");
    report_tiles(bad_clut, "clut out of range");
    report_tiles(bad_align, "u/v not 8/16 aligned");
    report_tiles(bad_src, "source not covered by a .ctd TIM");
    if (max_group >= 0 && cam.n_groups != static_cast<u32>(max_group) + 1)
      validation.errors.push_back("CAM: n_groups " + std::to_string(cam.n_groups) + " != max tile group+1 " + std::to_string(max_group + 1));
    validation.bbox_match = !first && bx0 == -cam.x_max && by0 == -cam.y_max && bx1 + kRoomTileSize == 320 - cam.x_min &&
                            by1 + kRoomTileSize == 240 - cam.y_min;
    if (!validation.bbox_match) validation.errors.push_back("CAM: tile bbox differs from viewport + pan limits");

    // Walk mesh.
    const auto& walk = scene.walk;
    const size_t vertex_count = walk.vertices.size(), triangle_count = walk.triangles.size();
    for (size_t i = 0; i < triangle_count; ++i) {
      const auto& triangle = walk.triangles[i];
      const std::string id = "tri " + std::to_string(i);
      if (std::max({triangle.v[0], triangle.v[1], triangle.v[2]}) >= vertex_count)
        validation.errors.push_back("WALK: " + id + " vertex index out of range");
      if (triangle.v[0] == triangle.v[1] || triangle.v[1] == triangle.v[2] || triangle.v[0] == triangle.v[2])
        validation.warnings.push_back("WALK: " + id + " degenerate");
      for (size_t k = 0; k < 3; ++k) {
        const s16 adjacent = triangle.adj[k];
        if (adjacent < 0) continue;
        if (static_cast<size_t>(adjacent) >= triangle_count) {
          validation.errors.push_back("WALK: " + id + " adjacency out of range");
          continue;
        }
        const auto& neighbour = walk.triangles[static_cast<size_t>(adjacent)];
        auto has = [&](u16 index) { return neighbour.v[0] == index || neighbour.v[1] == index || neighbour.v[2] == index; };
        if (!has(triangle.v[k]) || !has(triangle.v[(k + 1) % 3]))
          validation.errors.push_back("WALK: " + id + " adj" + std::to_string(k) + " does not share the edge");
        else if (neighbour.adj[0] != static_cast<s16>(i) && neighbour.adj[1] != static_cast<s16>(i) && neighbour.adj[2] != static_cast<s16>(i))
          validation.warnings.push_back("WALK: " + id + " adj" + std::to_string(k) + " not reciprocal");
      }
    }

    // Models name a CLUT-carrying TIM of the .ctd.
    std::set<std::array<u16, 4>> keys, used;
    for (const Tim& tim : parsed.tims)
      if (tim.has_clut) keys.insert({tim.image.x, tim.image.y, tim.clut.x, tim.clut.y});
    for (size_t k = 0; k < scene.models.size(); ++k) {
      const auto& model = scene.models[k];
      if (model.eof_rel != model.size - 12) validation.errors.push_back("MODEL " + std::to_string(k) + ": eof field != size-12");
      const std::array<u16, 4> key{model.img_x, model.img_y, model.clut_x, model.clut_y};
      if (!keys.count(key)) validation.errors.push_back("MODEL " + std::to_string(k) + ": header matches no CLUT TIM in .ctd");
      used.insert(key);
    }
    validation.extra_clut_tims = keys.size() > used.size();
    for (const Tim& tim : parsed.tims)
      if (!tim.has_clut) {
        if (tim.image.x != 0 || tim.image.y != 256) validation.errors.push_back("CTD: first background TIM not at (0,256)");
        break;
      }
  } catch (const Fail& failure) {
    validation.errors.push_back("LOAD: " + failure.msg);
  } catch (const std::exception& exception) {
    validation.errors.push_back(std::string("LOAD: malformed data: ") + exception.what());
  }
  return validation;
}

}  // namespace oc::import
