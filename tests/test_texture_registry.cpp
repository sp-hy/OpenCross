// Texture registry: key -> canonical path, dump -> mod folder -> replace round trip through the mods overlay.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "assets/battle_stage.h"
#include "game/battle/model_table.h"
#include "game/title_backdrop.h"
#include "import/battle_stage.h"
#include "import/room_effects.h"
#include "platform/paths.h"
#include "render/model_tiles.h"
#include "render/stage_tiles.h"
#include "render/texture_registry.h"
#include "vfs/mods.h"
#include "vfs/vfs.h"

using namespace oc;
namespace fs = std::filesystem;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)
}  // namespace

// ---- does the bake duplicate a vertex of an op-37 morph part? (needs the game install; SKIP without one)
// A baked part may append duplicates of vertices shared by faces of two atlases; a morph (render::StagePipeline) pairs vertex i of the base
// part with vertex i of the target and is only drawn when both have the same count. So per (base, target): the appended vertices of both
// must copy the same source vertices, in the same order.
struct MorphRow {
  std::string stage, base, target;
  size_t base_n = 0, base_dups = 0, tgt_dups = 0;
  bool same_count = true, same_sources = true;
};

// Index of the first original vertex equal to `vertex` in position / colour / packet texel / centre (the bake changes only uv and uv_scale).
static size_t source_of(const std::vector<assets::StageVertex>& orig, const assets::StageVertex& vertex) {
  for (size_t i = 0; i < orig.size(); ++i)
    if (orig[i].pos == vertex.pos && orig[i].color == vertex.color && orig[i].uv_raw == vertex.uv_raw && orig[i].centre == vertex.centre) return i;
  return orig.size();
}

static const assets::StagePart* scenery_part(const assets::StageAsset& asset, const std::string& name) {
  for (const assets::StagePart& part : asset.parts)
    // StageScene::parts_ keeps the first one of a name.
    if (!part.ground && part.name == name) return &part;
  return nullptr;
}

// `raw` = the same drp loaded without atlases (vertex lists as imported), `baked` = the asset after bake_object_textures with its atlases.
static void scan_one(const std::string& label, ByteSpan prd, ByteSpan lib, const assets::StageAsset& baked, std::vector<MorphRow>& rows, int& morph_ops, size_t& dup_parts, size_t& baked_morph_bases) {
  import::Vram empty;
  const assets::RoomEffects fx = import::load_battle_effects(prd, {}, empty, lib);
  auto raw = import::load_stage(prd, {});
  if (!raw) return;
  for (size_t i = 0; i < raw->parts.size() && i < baked.parts.size(); ++i)
    // The control: parts that do get duplicates (any part, morph or not).
    if (baked.parts[i].vertices.size() != raw->parts[i].vertices.size()) ++dup_parts;
  for (const assets::Emitter& em : fx.emitters) {
    std::string base, target;
    for (const assets::EffectOp& op : em.ops) {
      if (op.op == 0x05)
        if (auto* sprite = std::get_if<assets::FxSprite>(&op.arg)) base = sprite->name;
      if (op.op == 0x37)
        if (auto* morph = std::get_if<assets::FxMorph>(&op.arg)) target = morph->name;
    }
    if (target.empty() || base.empty()) continue;
    ++morph_ops;
    const assets::StagePart* ba = scenery_part(baked, base);
    const assets::StagePart* ta = scenery_part(baked, target);
    const assets::StagePart* br = scenery_part(*raw, base);
    const assets::StagePart* tr = scenery_part(*raw, target);
    if (!ba || !ta || !br || !tr) continue;
    MorphRow row;
    row.stage = label;
    for (const assets::StageSubMesh& sm : ba->submeshes) baked_morph_bases += sm.object_texture >= 0 ? 1 : 0;
    row.base = base;
    row.target = target;
    row.base_n = br->vertices.size();
    row.base_dups = ba->vertices.size() - br->vertices.size();
    row.tgt_dups = ta->vertices.size() - tr->vertices.size();
    row.same_count = ba->vertices.size() == ta->vertices.size();
    // A differing count means the pair is not a morph at all (different packet split).
    if (br->vertices.size() != tr->vertices.size()) row.same_count = false;
    if (row.same_count)
      for (size_t k = br->vertices.size(); k < ba->vertices.size(); ++k)
        if (source_of(br->vertices, ba->vertices[k]) != source_of(tr->vertices, ta->vertices[k])) row.same_sources = false;
    rows.push_back(row);
  }
}

static void scan_morph_bake(vfs::Vfs& files) {
  std::vector<MorphRow> rows;
  int morph_ops = 0, stages = 0;
  size_t dup_parts = 0, baked_bases = 0;
  std::set<std::string> seen;
  std::vector<std::string> names;
  for (const std::string& file_name : files.list("cdrom")) {
    std::string lower = file_name;
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.starts_with("battle/bg/bg_") && lower.ends_with(".prd") && seen.insert(lower).second) names.push_back(file_name);
  }
  std::sort(names.begin(), names.end());
  for (const std::string& file_name : names) {
    std::string lower = file_name;
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const auto prd = files.read("cdrom/" + file_name);
    const auto ctd = files.read("cdrom/" + file_name.substr(0, file_name.size() - 4) + ".ctd");
    if (!prd) continue;
    auto st = import::load_stage(*prd, ctd ? ByteSpan(*ctd) : ByteSpan{});
    if (!st) continue;
    render::bake_object_textures(*st);
    ++stages;
    scan_one(lower.substr(10, lower.size() - 14), *prd, {}, *st, rows, morph_ops, dup_parts, baked_bases);
  }
  int title_morphs = 0;
  if (auto prd = files.read("cdrom/title.prd"))
    if (auto st = game::load_title_stage(files)) {
      const auto lib = files.read("cdrom/effect/particle.prd");
      scan_one("title", *prd, lib ? ByteSpan(*lib) : ByteSpan{}, *st, rows, title_morphs, dup_parts, baked_bases);
      ++stages;
    }
  size_t with_dups = 0, broken = 0;
  for (const MorphRow& morph_row : rows) {
    with_dups += (morph_row.base_dups || morph_row.tgt_dups) ? 1 : 0;
    const bool bad = !morph_row.same_count || !morph_row.same_sources;
    broken += bad ? 1 : 0;
    if (morph_row.base_dups || morph_row.tgt_dups || bad)
      std::printf("  morph %s: %s -> %s  base %zu vertices, +%zu dup(s) base / +%zu dup(s) target%s%s\n", morph_row.stage.c_str(), morph_row.base.c_str(), morph_row.target.c_str(), morph_row.base_n, morph_row.base_dups, morph_row.tgt_dups,
                  morph_row.same_count ? "" : "  COUNT MISMATCH (morph disabled)", morph_row.same_sources ? "" : "  SOURCE MISMATCH (tear)");
  }
  std::printf("morph bake scan: %d stage(s) + title, %d op-37 emitter(s) (%d in the title), %zu resolved pair(s), %zu with duplicated vertices, %zu broken (control: %zu part(s) of all stages get duplicates, %zu morph-base sub-mesh(es) baked)\n", stages, morph_ops + title_morphs,
              title_morphs, rows.size(), with_dups, broken, dup_parts, baked_bases);
  CHECK(broken == 0);
}

int main() {
  using namespace render;
  CHECK(texture_path(stage_texture_key("battle_stage", "bg_07", 3, 1, 320, 480)) == "battle_stage/bg_07/03_m1_c320x480.png");
  CHECK(texture_path(stage_texture_key("title", "", 0, 0, 0, 225)) == "title/00_m0_c0x225.png");
  CHECK(texture_path({"effects", "Ice 0", -1, "1A2B3C4D"}) == "effects/ice_0/1a2b3c4d.png");
  // No ".." escapes.
  CHECK(texture_path({"effect_mesh", "../eid_5", 12, ""}) == "effect_mesh/eid_5/12.png");
  const std::vector<u8> a(4 * 4 * 4, 7), b(4 * 4 * 4, 8);
  // Path-named vs content-named groups, model keys.
  CHECK(is_content_named("room_mesh") && is_content_named("effect_mesh") && is_content_named("room_model") && !is_content_named("battle_stage"));
  CHECK(texture_path(stage_group_key("battle_stage/bg_07", 2, 0, 5, 6, 4, 4, a.data())) == "battle_stage/bg_07/02_m0_c5x6.png");
  CHECK(texture_path(stage_group_key("room_mesh", 2, 0, 5, 6, 4, 4, a.data())) == "room_mesh/" + texture_content_tag(4, 4, a.data()) + ".png");
  CHECK(stage_group_key("room_mesh", 0, 0, 0, 0, 4, 4, a.data()).name.empty());
  CHECK(texture_path(model_texture_key("battle/player/selju/selju", 1, 3, 4, 4, a.data())) == "model/battle/player/selju/selju/clut1.png");
  CHECK(texture_path(model_texture_key("battle/player/selju/selju", 4, 3, 4, 4, a.data())) == "model/battle/player/selju/selju/overlay2.png");
  CHECK(texture_path(model_texture_key("@room_model", 0, 2, 4, 4, a.data())) == "room_model/" + texture_content_tag(4, 4, a.data()) + ".png");
  CHECK(texture_path(model_texture_key(oc::game::weapon_texture_source("battle/wepon/sword.bin", 3), 0, 1, 4, 4, a.data())) == "model/battle/wepon/sword/part3.png");
  CHECK(oc::game::weapon_texture_source("battle/wepon//monster/adk_yari.bin", 0) == "battle/wepon/monster/adk_yari/part0");
  {
    // Room model tiles: per page, the texel rects the faces sample; overlapping ones merge, a separate one stays; an all-transparent rect is no tile.
    oc::assets::ModelAsset room;
    oc::import::Image page;
    page.width = page.height = 64;
    page.pixels.assign(64 * 64 * 4, 200);
    for (u32 y = 40; y < 64; ++y)
      for (u32 x = 40; x < 64; ++x) page.pixels[(size_t(y) * 64 + x) * 4 + 3] = 0;
    room.textures = {page};
    oc::assets::Mesh mesh;
    mesh.materials.push_back({0});
    auto texel = [](u32 x, u32 y) { return std::array<float, 2>{(x + 0.5f) / 64, (y + 0.5f) / 64}; };
    const std::array<std::array<u32, 2>, 12> corners = {{{0, 0}, {9, 0}, {0, 9}, {5, 5}, {14, 5}, {5, 14}, {30, 30}, {33, 30}, {30, 31}, {50, 50}, {55, 50}, {50, 55}}};
    for (const auto& corner : corners) {
      mesh.indices.push_back(static_cast<u32>(mesh.vertices.size()));
      mesh.vertices.push_back({0, texel(corner[0], corner[1])});
    }
    mesh.submeshes.push_back({0, 0, 12});
    room.meshes.push_back(mesh);
    const auto tiles = model_tiles(room);
    CHECK(tiles.size() == 2);
    CHECK(tiles.size() == 2 && tiles[0].x == 0 && tiles[0].y == 0 && tiles[0].w == 15 && tiles[0].h == 15);
    CHECK(tiles.size() == 2 && tiles[1].x == 30 && tiles[1].y == 30 && tiles[1].w == 4 && tiles[1].h == 2 && tiles[1].hash.size() == 8);
  }
  {
    // Model STP alpha: the file is 0 / 255; a replacement (2x here) gets the original's flag back, per texel when the original mixes them.
    const std::vector<u8> original = {9, 9, 9, 0xBF, 9, 9, 9, 0xFF, 0, 0, 0, 0, 9, 9, 9, 0xBF};
    const std::vector<u8> file = model_file_pixels(2, 2, original.data());
    CHECK(file[3] == 0xFF && file[7] == 0xFF && file[11] == 0 && file[15] == 0xFF);
    std::vector<u8> repl(4 * 4 * 4, 200);
    repl[3] = 0x40;
    model_restore_stp(2, 2, original.data(), 4, 4, repl);
    // (0,0) under coverage 0x40 -> transparent; (1,0) over texel 0 -> STP; (2,0) over texel 1 -> plain; (0,2) over the transparent texel -> the common flag (STP).
    CHECK(repl[3] == 0 && repl[7] == 0xBF && repl[11] == 0xFF && repl[(2 * 4 + 0) * 4 + 3] == 0xBF);
    const std::vector<u8> all_stp = {9, 9, 9, 0xBF, 0, 0, 0, 0, 0, 0, 0, 0, 9, 9, 9, 0xBF};
    std::vector<u8> full(4 * 4 * 4, 0xFF);
    model_restore_stp(2, 2, all_stp.data(), 4, 4, full);
    CHECK(std::all_of(full.begin(), full.end(), [&, i = 0](u8 v) mutable { return (i++ % 4 != 3) || v == 0xBF; }));
  }
  CHECK(texture_content_tag(4, 4, a.data()) == texture_content_tag(4, 4, a.data()));
  CHECK(texture_content_tag(4, 4, a.data()) != texture_content_tag(4, 4, b.data()));
  CHECK(texture_content_tag(4, 4, a.data()).size() == 8);

  const fs::path tmp = fs::temp_directory_path() / "oc_test_texreg";
  fs::remove_all(tmp);
  fs::create_directories(tmp / "mods" / "m1" / "Wrapper");

  // Dump: a 2x2 image.
  const TextureKey key = stage_texture_key("battle_stage", "bg_99", 0, 1, 256, 230);
  std::vector<u8> px = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 9, 9, 9, 128};
  texture_registry::configure({true, true});
  texture_registry::set_dump_root(tmp / "dump");
  texture_registry::reset_stats();
  CHECK(texture_registry::dump(key, 2, 2, px.data()));
  // Dumped once per path per session.
  CHECK(!texture_registry::dump(key, 2, 2, px.data()));
  CHECK(texture_registry::stats().dumped == 1);
  // The "Dump all textures" job writes regardless of the DumpTextures flag.
  texture_registry::configure({false, true});
  CHECK(!texture_registry::dump(stage_texture_key("title", "", 1, 1, 0, 0), 2, 2, px.data()));
  CHECK(texture_registry::dump_forced(stage_texture_key("title", "", 1, 1, 0, 0), 2, 2, px.data()));
  CHECK(fs::is_regular_file(tmp / "dump" / "title" / "01_m1_c0x0.png"));
  // The dump folder deleted between two dump jobs of one session: the second job writes the file again.
  fs::remove_all(tmp / "dump" / "title");
  CHECK(!texture_registry::dump_forced(stage_texture_key("title", "", 1, 1, 0, 0), 2, 2, px.data()));
  texture_registry::begin_dump();
  CHECK(texture_registry::dump_forced(stage_texture_key("title", "", 1, 1, 0, 0), 2, 2, px.data()));
  CHECK(fs::is_regular_file(tmp / "dump" / "title" / "01_m1_c0x0.png"));
  texture_registry::configure({true, true});
  const fs::path dumped = tmp / "dump" / "battle_stage" / "bg_99" / "00_m1_c256x230.png";
  CHECK(fs::is_regular_file(dumped));

  // The mod: the dumped tree copied below <mod>\<wrapper>\textures\ (any wrapper folder, any case).
  const fs::path dst = tmp / "mods" / "m1" / "Wrapper" / "Textures" / "Battle_Stage" / "bg_99";
  fs::create_directories(dst);
  fs::copy_file(dumped, dst / "00_m1_c256x230.png");
  fs::create_directories(tmp / "mods" / "m1" / "Wrapper" / "textures" / "room_mesh");
  // A content-named category.
  fs::copy_file(dumped, tmp / "mods" / "m1" / "Wrapper" / "textures" / "room_mesh" / "0123abcd.png");
  // A tile replacement: matched by its hash only (the order number is free), 2x2.
  fs::copy_file(dumped, dst / "tile_01_cafef00d.png");
  texture_registry::configure({true, true});
  const std::vector<u8> one = {1, 2, 3, 255};
  CHECK(texture_registry::dump_forced({"scratch", "", -1, "one"}, 1, 1, one.data()));
  fs::create_directories(tmp / "mods" / "m1" / "Wrapper" / "textures" / "tiles");
  // The global tile, 1x1.
  fs::copy_file(tmp / "dump" / "scratch" / "one.png", tmp / "mods" / "m1" / "Wrapper" / "textures" / "tiles" / "cafef00d.png");
  fs::create_directories(tmp / "mods" / "m1" / "Wrapper" / "textures" / "unknown_cat");
  // Not a texture category -> unmatched.
  fs::copy_file(dumped, tmp / "mods" / "m1" / "Wrapper" / "textures" / "unknown_cat" / "x.png");

  // Voice packs: <mod>/voices/**.mp3 and every .mp3 of a mod folder called "voices" count as voice files (not unmatched); an mp3
  // outside a top-level voices folder, or a non-mp3 below it, is an ordinary (here unmatched) file.
  fs::create_directories(tmp / "mods" / "m1" / "voices" / "123");
  std::ofstream(tmp / "mods" / "m1" / "voices" / "123" / "5-0-serge.mp3") << "x";
  std::ofstream(tmp / "mods" / "m1" / "voices" / "123" / "6-0-kid.MP3") << "x";
  std::ofstream(tmp / "mods" / "m1" / "voices" / "readme.txt") << "x";
  std::ofstream(tmp / "mods" / "m1" / "Wrapper" / "stray.mp3") << "x";
  fs::create_directories(tmp / "mods" / "voices" / "55");
  std::ofstream(tmp / "mods" / "voices" / "55" / "1-0-npc.mp3") << "x";

  vfs::Vfs files;
  vfs::ModManager mods;
  mods.scan(files, tmp / "mods", {});
  CHECK(mods.mods().size() == 2);
  CHECK(mods.mods()[1].name == "voices" && mods.mods()[1].voices.size() == 1 && mods.mods()[1].unmatched.empty());
  CHECK(mods.mods()[0].voices.size() == 2);
  CHECK(mods.mods()[0].textures.size() == 4);
  {
    std::vector<std::string> targets;
    for (const auto& texture : mods.mods()[0].textures) targets.push_back(texture.target);
    std::sort(targets.begin(), targets.end());
    CHECK(targets == (std::vector<std::string>{"battle_stage/bg_99/00_m1_c256x230.png", "battle_stage/bg_99/tile_01_cafef00d.png", "room_mesh/0123abcd.png", "tiles/cafef00d.png"}));
  }
  // Unmatched: unknown_cat/x.png, voices/readme.txt, Wrapper/stray.mp3.
  CHECK(mods.mods()[0].unmatched.size() == 3);

  texture_registry::bind(&files);
  u32 w = 2, h = 2;
  std::vector<u8> out = {1, 2, 3, 4};
  CHECK(!texture_registry::active() || texture_registry::settings().dump);
  // The overlay is not applied yet.
  CHECK(!texture_registry::replace(key, w, h, out));
  mods.apply(files, vfs::ModArt::Modded);
  CHECK(files.has_textures());
  CHECK(texture_registry::replace(key, w, h, out));
  CHECK(w == 2 && h == 2 && out == px);
  CHECK(texture_registry::stats().replaced == 1);
  u32 rw = 0, rh = 0;
  std::vector<u8> rpx;
  CHECK(texture_registry::replace({"room_mesh", "", -1, "0123abcd"}, rw, rh, rpx) && rw == 2 && rh == 2);

  // ReplaceTextures off.
  texture_registry::configure({false, false});
  w = h = 1;
  out = {1, 2, 3, 4};
  CHECK(!texture_registry::replace(key, w, h, out));
  texture_registry::configure({false, true});

  // ---- tile replacement lookup (stage folder by hash, then the global tiles/ folder) ----------------------------------------------------------------
  {
    u32 tw = 0, th = 0;
    std::vector<u8> trgba;
    texture_registry::configure({false, true});
    // tile_01_cafef00d.png matches with any order number, the stage folder first.
    CHECK(texture_registry::replace_tile("battle_stage/bg_99", "cafef00d", tw, th, trgba) && tw == 2 && th == 2);
    // Another stage falls back to the global tiles/cafef00d.png.
    CHECK(texture_registry::replace_tile("battle_stage/bg_05", "cafef00d", tw, th, trgba) && tw == 1 && th == 1);
    CHECK(!texture_registry::replace_tile("battle_stage/bg_05", "deadbeef", tw, th, trgba));
    texture_registry::configure({false, false});
    // With ReplaceTextures off nothing is replaced.
    CHECK(!texture_registry::replace_tile("battle_stage/bg_99", "cafef00d", tw, th, trgba));
    texture_registry::configure({false, true});
  }

  // Load on SD: a Modded mod is not active in the SD art mode.
  mods.apply(files, vfs::ModArt::Sd);
  CHECK(!files.has_textures());
  CHECK(!texture_registry::replace(key, w, h, out));
  mods.mods()[0].load_on = vfs::ModLoadOn::All;
  mods.apply(files, vfs::ModArt::Sd);
  CHECK(files.has_textures());
  mods.mods()[0].enabled = false;
  mods.apply(files, vfs::ModArt::Sd);
  CHECK(!files.has_textures());
  // Only Modded art enhances RDE's art; switching HD <-> Modded with the same mods still counts as an overlay change.
  CHECK(!files.enhanced());
  mods.apply(files, vfs::ModArt::Hd);
  const std::uint32_t hd_generation = files.generation();
  CHECK(!files.enhanced());
  mods.apply(files, vfs::ModArt::Modded);
  CHECK(files.enhanced() && files.generation() != hd_generation);
  mods.apply(files, vfs::ModArt::Sd);

  // ---- tile library + per-object textures (render::bake_object_textures) ----------------------------------------------
  {
    CHECK(texture_path(raw_atlas_key("battle_stage/bg_22", 2, 1, 256, 230)) == "raw/battle_stage/bg_22/02_m1_c256x230.png");
    CHECK(texture_path(raw_atlas_key("title", 0, 0, 0, 225)) == "raw/title/00_m0_c0x225.png");

    // Shelf packer: no overlaps, inside the image, power-of-two width.
    {
      std::vector<std::array<int, 2>> sizes = {{16, 16}, {40, 8}, {5, 30}, {256, 256}, {1, 1}, {64, 64}, {64, 64}};
      const ShelfPacking pk = shelf_pack(sizes, 1);
      CHECK(pk.w > 0 && (pk.w & (pk.w - 1)) == 0 && pk.h > 0);
      bool ok = true;
      for (size_t i = 0; i < sizes.size(); ++i) {
        const int x0 = pk.pos[i][0], y0 = pk.pos[i][1], x1 = x0 + sizes[i][0] + 2, y1 = y0 + sizes[i][1] + 2;
        ok = ok && x0 >= 0 && y0 >= 0 && x1 <= pk.w && y1 <= pk.h;
        for (size_t j = i + 1; j < sizes.size(); ++j) {
          const int a0 = pk.pos[j][0], b0 = pk.pos[j][1], a1 = a0 + sizes[j][0] + 2, b1 = b0 + sizes[j][1] + 2;
          ok = ok && (x1 <= a0 || a1 <= x0 || y1 <= b0 || b1 <= y0);
        }
      }
      CHECK(ok);
    }

    // A synthetic stage: one 128 x 256 atlas whose texel (x, y) is (x & 31, y & 31, 7, 255) - periodic, so equal-looking rects 64 texels apart are identical.
    assets::StageAsset st;
    assets::StageTexture at;
    at.mode = 1;
    at.image.width = 128;
    at.image.height = 256;
    at.image.pixels.resize(128 * 256 * 4);
    for (u32 y = 0; y < 256; ++y)
      for (u32 x = 0; x < 128; ++x) {
        u8* texel = &at.image.pixels[(y * 128 + x) * 4];
        texel[0] = static_cast<u8>(x & 31), texel[1] = static_cast<u8>(y & 31), texel[2] = 7, texel[3] = 255;
      }
    st.textures.push_back(at);
    auto corner = [&](assets::StagePart& pt, int u, int v) {
      assets::StageVertex vx;
      vx.uv = {(static_cast<float>(u) + 0.5f) / 128.0f, 1.0f - (static_cast<float>(v) + 0.5f) / 256.0f};
      vx.uv_raw = {static_cast<float>(u), static_cast<float>(v)};
      vx.uv_scale = {1.0f / 128.0f, 1.0f / 256.0f};
      pt.vertices.push_back(vx);
      return static_cast<u32>(pt.vertices.size() - 1);
    };
    // A quad as two triangles like the importer's (0,1,2) (2,1,3).
    auto quad = [&](assets::StagePart& pt, int u0, int v0, int u1, int v1) {
      const u32 c0 = corner(pt, u0, v0), c1 = corner(pt, u1, v0), c2 = corner(pt, u0, v1), c3 = corner(pt, u1, v1);
      for (u32 i : {c0, c1, c2, c2, c1, c3}) pt.indices.push_back(i);
    };
    assets::StagePart p0, p1;
    p0.name = "nea7";
    // Tile A; a quad adjacent to A that stays its own tile (no merging of neighbours); a far-away quad.
    quad(p0, 8, 8, 23, 23);
    quad(p0, 24, 8, 39, 23);
    quad(p0, 100, 200, 110, 210);
    assets::StageSubMesh sm;
    sm.texture = 0;
    sm.first_index = 0;
    sm.index_count = static_cast<u32>(p0.indices.size());
    p0.submeshes.push_back(sm);
    p1.name = "ring";
    p1.ground = true;
    // A scenery mesh an emitter spawns: meshes no emitter names are skipped by the bake (not used by the level).
    p0.emitters.push_back("nea0");
    // The same pixels as tile A, 64 texels away -> ONE tile with two sources.
    quad(p1, 72, 72, 87, 87);
    sm.index_count = static_cast<u32>(p1.indices.size());
    p1.submeshes.push_back(sm);
    st.parts = {p0, p1};
    const assets::StageAsset before = st;
    bake_object_textures(st);
    CHECK(st.object_textures.size() == 2);
    CHECK(st.object_textures[0].name == "obj_00_scenery_nea7" && st.object_textures[1].name == "obj_01_ground_ring");
    // Tiles: A (also the ring's), B, the far one; the identical rect is de-duplicated.
    CHECK(st.tiles.size() == 3);
    CHECK(st.object_textures[0].slots.size() == 3 && st.object_textures[1].slots.size() == 1 && st.object_textures[1].slots[0].tile == st.object_textures[0].slots[0].tile);
    CHECK(st.tiles[0].sources.size() == 2 && st.tiles[0].users.size() == 2 && st.tiles[0].users[0].first == "obj_00_scenery_nea7" && st.tiles[0].users[1].first == "obj_01_ground_ring");
    // A quad = 2 triangles.
    CHECK(st.tiles[0].users[0].second == 2 && st.tiles[0].users[1].second == 2);
    CHECK(st.tiles[0].w == 16 && st.tiles[0].hash == texture_content_tag(16, 16, st.tiles[0].image.pixels.data()));
    CHECK(st.tiles[0].name == "tile_00_" + st.tiles[0].hash && st.tiles[1].name.starts_with("tile_01_") && st.tiles[2].name.starts_with("tile_02_"));
    CHECK(tile_path("battle_stage/bg_05", st.tiles[1]) == "battle_stage/bg_05/" + st.tiles[1].name + ".png");
    CHECK(tile_path("title", st.tiles[1]) == "title/" + st.tiles[1].name + ".png");
    // Room meshes are content-named: no order number.
    CHECK(tile_path("room_mesh", st.tiles[1]) == "room_mesh/tile_" + st.tiles[1].hash + ".png");
    CHECK(st.parts[0].submeshes[0].object_texture == 0 && st.parts[1].submeshes[0].object_texture == 1);
    // Atlases and indices are untouched (no shared-vertex duplicates).
    CHECK(st.textures.size() == 1 && st.parts[0].indices == before.parts[0].indices);
    // A remapped UV lands on the same texel: the object image at the new UV == the atlas at the old UV (nearest), alpha included.
    bool same = true;
    for (size_t pi = 0; pi < st.parts.size(); ++pi) {
      const assets::StageObjectTexture& ot = st.object_textures[pi];
      for (size_t vi = 0; vi < st.parts[pi].vertices.size(); ++vi) {
        const assets::StageVertex& nv = st.parts[pi].vertices[vi];
        const assets::StageVertex& ov = before.parts[pi].vertices[vi];
        const int ox = static_cast<int>(std::floor(ov.uv[0] * 128.0f)), oy = static_cast<int>(std::floor((1.0f - ov.uv[1]) * 256.0f));
        const int nx = static_cast<int>(std::floor(nv.uv[0] * static_cast<float>(ot.image.width)));
        const int ny = static_cast<int>(std::floor((1.0f - nv.uv[1]) * static_cast<float>(ot.image.height)));
        for (size_t k = 0; k < 4; ++k)
          same = same && ot.image.pixels[(static_cast<size_t>(ny) * ot.image.width + static_cast<size_t>(nx)) * 4 + k] ==
                             at.image.pixels[(static_cast<size_t>(oy) * 128 + static_cast<size_t>(ox)) * 4 + k];
        same = same && nv.uv_scale[0] == 1.0f / static_cast<float>(ot.image.width) && nv.uv_raw == ov.uv_raw && nv.pos == ov.pos;
      }
    }
    CHECK(same);
    // Gutter: the texel left of a slot's content repeats its edge column (edge extend), the corner repeats the corner.
    {
      const assets::StageObjectTexture& ot = st.object_textures[1];
      const auto& slot = ot.slots[0];
      auto texel = [&](int x, int y, size_t k) { return ot.image.pixels[(static_cast<size_t>(y) * ot.image.width + static_cast<size_t>(x)) * 4 + k]; };
      CHECK(slot.w == 16 && slot.h == 16 && texel(slot.dx, slot.dy + 5, 0) == texel(slot.dx + 1, slot.dy + 5, 0) && texel(slot.dx, slot.dy, 1) == texel(slot.dx + 1, slot.dy + 1, 1));
      CHECK(texel(slot.dx + slot.w + 1, slot.dy + slot.h + 1, 0) == texel(slot.dx + slot.w, slot.dy + slot.h, 0));
    }
    // Scale composition: tile A replaced by a 2x image (nearest upscale of A with blue + 1): object 0 (A, B, far) is composed at 2x, B and the far tile enlarged
    // nearest, every UV lands on the same texel (replacement texel / upscaled original); the ring (only A) is 2x as well; no replacement -> nothing changes.
    {
      assets::StageAsset rc = st;
      const assets::StageTile& tileA = rc.tiles[0];
      std::vector<u8> big(32 * 32 * 4);
      for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
          std::memcpy(&big[(static_cast<size_t>(y) * 32 + static_cast<size_t>(x)) * 4], &tileA.image.pixels[(static_cast<size_t>(y / 2) * 16 + static_cast<size_t>(x / 2)) * 4], 4);
          big[(static_cast<size_t>(y) * 32 + static_cast<size_t>(x)) * 4 + 2] = 8;
        }
      const std::string hashA = tileA.hash;
      CHECK(recompose_object_textures(rc, [&](const assets::StageTile&, u32&, u32&, std::vector<u8>&) { return false; }) == 0 && rc.object_textures[0].image.pixels == st.object_textures[0].image.pixels);
      const int recomposed = recompose_object_textures(rc, [&](const assets::StageTile& tile, u32& w, u32& h, std::vector<u8>& rgba) {
        if (tile.hash != hashA) return false;
        w = h = 32;
        rgba = big;
        return true;
      });
      CHECK(recomposed == 1);
      CHECK(rc.object_textures[0].scale == 2.0f && rc.object_textures[1].scale == 2.0f);
      bool ok = true;
      for (size_t pi = 0; pi < rc.parts.size(); ++pi) {
        const assets::StageObjectTexture& ot = rc.object_textures[pi];
        for (size_t vi = 0; vi < rc.parts[pi].vertices.size(); ++vi) {
          const assets::StageVertex& nv = rc.parts[pi].vertices[vi];
          const assets::StageVertex& ov = before.parts[pi].vertices[vi];
          const int ox = static_cast<int>(std::floor(ov.uv[0] * 128.0f)), oy = static_cast<int>(std::floor((1.0f - ov.uv[1]) * 256.0f));
          // the texel centre of an original texel is the corner of its 2x2 block: sample a quarter pixel before it (inside the block's first pixel)
          const int nx = static_cast<int>(std::floor(nv.uv[0] * static_cast<float>(ot.image.width) - 0.25f));
          const int ny = static_cast<int>(std::floor((1.0f - nv.uv[1]) * static_cast<float>(ot.image.height) - 0.25f));
          const bool in_a = (ox >= 8 && ox <= 23 && oy >= 8 && oy <= 23) || (ox >= 72 && ox <= 87 && oy >= 72 && oy <= 87);
          for (size_t k = 0; k < 3; ++k) {
            const u8 want = (k == 2 && in_a) ? 8 : at.image.pixels[(static_cast<size_t>(oy) * 128 + static_cast<size_t>(ox)) * 4 + k];
            ok = ok && ot.image.pixels[(static_cast<size_t>(ny) * ot.image.width + static_cast<size_t>(nx)) * 4 + k] == want;
          }
          ok = ok && std::fabs(nv.uv_scale[0] * static_cast<float>(ot.image.width) - 2.0f) < 1e-4f;
        }
      }
      CHECK(ok);
      // Packets that still address the raw atlas (tile_lookup): a texel inside a used rect maps to the composed texel holding the same RGBA (here at the 2x scale of
      // the re-composed object); one no stage face samples maps to "raw".
      {
        TileTexel tt;
        CHECK(tile_lookup(rc, 0, 10, 10, tt) && tt.object >= 0 && tt.fx == 2.0f && tt.fy == 2.0f);
        const import::Image& oim = rc.object_textures[static_cast<size_t>(tt.object)].image;
        const int cx = static_cast<int>(std::floor(tt.px - 0.25f)), cy = static_cast<int>(std::floor(tt.py - 0.25f));
        const u8* got = &oim.pixels[(static_cast<size_t>(cy) * oim.width + static_cast<size_t>(cx)) * 4];
        // Tile A is the replaced one (blue 8).
        CHECK(got[0] == at.image.pixels[(10 * 128 + 10) * 4] && got[1] == at.image.pixels[(10 * 128 + 10) * 4 + 1] && got[2] == 8);
        // Tile B (24..39): un-replaced, enlarged nearest: the same texel colour.
        TileTexel tb;
        CHECK(tile_lookup(rc, 0, 30, 12, tb));
        const import::Image& bim = rc.object_textures[static_cast<size_t>(tb.object)].image;
        const u8* gb = &bim.pixels[(static_cast<size_t>(std::floor(tb.py - 0.25f)) * bim.width + static_cast<size_t>(std::floor(tb.px - 0.25f))) * 4];
        CHECK(gb[0] == at.image.pixels[(12 * 128 + 30) * 4] && gb[1] == at.image.pixels[(12 * 128 + 30) * 4 + 1] && gb[2] == 7);
        // Lookups fail for an unused texel, a texel outside the atlas, and a missing atlas.
        CHECK(!tile_lookup(rc, 0, 50, 50, tt) && !tile_lookup(rc, 0, 200, 5, tt) && !tile_lookup(rc, 1, 10, 10, tt));
        // The unreplaced original: scale 1, the same texel.
        TileTexel base;
        CHECK(tile_lookup(st, 0, 10, 10, base) && base.fx == 1.0f);
        const import::Image& sim = st.object_textures[static_cast<size_t>(base.object)].image;
        const u8* gs = &sim.pixels[(static_cast<size_t>(std::floor(base.py)) * sim.width + static_cast<size_t>(std::floor(base.px))) * 4];
        CHECK(std::memcmp(gs, &at.image.pixels[(10 * 128 + 10) * 4], 4) == 0);
      }
      // Re-runnable: the same replacement again gives the same images (no accumulated scale); then NO replacement restores the original object images, slots and
      // UVs bit for bit (the recompose starts from the pristine tiles / base layout, never from the previously replaced image).
      assets::StageAsset again = rc;
      CHECK(recompose_object_textures(again, [&](const assets::StageTile& tile, u32& w, u32& h, std::vector<u8>& rgba) {
              if (tile.hash != hashA) return false;
              w = h = 32;
              rgba = big;
              return true;
            }) == 1);
      CHECK(again.object_textures[0].scale == 2.0f && again.object_textures[0].image.pixels == rc.object_textures[0].image.pixels && again.parts[0].vertices[0].uv == rc.parts[0].vertices[0].uv);
      CHECK(recompose_object_textures(again, [&](const assets::StageTile&, u32&, u32&, std::vector<u8>&) { return false; }) == 0);
      bool restored = true;
      for (size_t pi = 0; pi < again.object_textures.size(); ++pi) {
        restored = restored && again.object_textures[pi].image.pixels == st.object_textures[pi].image.pixels && again.object_textures[pi].image.width == st.object_textures[pi].image.width &&
                   again.object_textures[pi].image.height == st.object_textures[pi].image.height && again.object_textures[pi].scale == 1.0f;
        for (size_t vi = 0; vi < again.parts[pi].vertices.size(); ++vi)
          restored = restored && again.parts[pi].vertices[vi].uv == st.parts[pi].vertices[vi].uv && again.parts[pi].vertices[vi].uv_scale == st.parts[pi].vertices[vi].uv_scale;
      }
      CHECK(restored);
    }
    // EdgeBlend (render::build_tile_seams): a ground strip of two squares A | B (tiles 16 x 16) and a
    // wall C standing on B's far edge, replaced by 4x images. A | B is a flat seam (both quads get a strip), B | C a crease (not flat); the strip of A's quad
    // mirrors B's texture across the edge: its edge corners take B's edge UVs and its far corners land on B's far corners (a square pair), its band is
    // 4 texels; the same strips with the original (1x) art.
    {
      assets::StageAsset sb;
      {
        // Texel (x, y, 7): not periodic, so the three rects below are three tiles (the shared atlas repeats every 32).
        assets::StageTexture at2 = at;
        for (u32 y = 0; y < 256; ++y)
          for (u32 x = 0; x < 128; ++x) at2.image.pixels[(y * 128 + x) * 4] = static_cast<u8>(x), at2.image.pixels[(y * 128 + x) * 4 + 1] = static_cast<u8>(y);
        sb.textures.push_back(at2);
      }
      assets::StagePart ground;
      ground.name = "ring";
      ground.ground = true;
      auto vc = [&](int u, int v, float x, float y, float z) {
        assets::StageVertex vx;
        vx.pos = {x, y, z};
        vx.uv = {(static_cast<float>(u) + 0.5f) / 128.0f, 1.0f - (static_cast<float>(v) + 0.5f) / 256.0f};
        vx.uv_raw = {static_cast<float>(u), static_cast<float>(v)};
        vx.uv_scale = {1.0f / 128.0f, 1.0f / 256.0f};
        ground.vertices.push_back(vx);
        return static_cast<u32>(ground.vertices.size() - 1);
      };
      // a quad like the importer's: corners (u0,v0) (u1,v0) (u0,v1) (u1,v1) at positions p00 p10 p01 p11, triangles (0,1,2) (2,1,3)
      auto vq = [&](int u0, int v0, std::array<float, 3> p00, std::array<float, 3> p10, std::array<float, 3> p01, std::array<float, 3> p11) {
        const u32 c0 = vc(u0, v0, p00[0], p00[1], p00[2]), c1 = vc(u0 + 15, v0, p10[0], p10[1], p10[2]), c2 = vc(u0, v0 + 15, p01[0], p01[1], p01[2]),
                  c3 = vc(u0 + 15, v0 + 15, p11[0], p11[1], p11[2]);
        for (u32 i : {c0, c1, c2, c2, c1, c3}) ground.indices.push_back(i);
      };
      // A: floor x 0..16 (vertices 0..3). B: floor x 16..32 (4..7), shares the edge x = 16 with A. C: wall at x = 32 (8..11), a 90 degree
      // crease with B.
      vq(8, 8, {0, 0, 0}, {16, 0, 0}, {0, 0, 16}, {16, 0, 16});
      vq(24, 8, {16, 0, 0}, {32, 0, 0}, {16, 0, 16}, {32, 0, 16});
      vq(40, 8, {32, 0, 0}, {32, 16, 0}, {32, 0, 16}, {32, 16, 16});
      assets::StageSubMesh gsm;
      gsm.texture = 0;
      gsm.index_count = static_cast<u32>(ground.indices.size());
      ground.submeshes.push_back(gsm);
      sb.parts = {ground};
      bake_object_textures(sb);
      CHECK(sb.object_textures.size() == 1 && sb.tiles.size() == 3);
      // The original art gets its strips too (any art mode); the same 4 at 4x below.
      CHECK(build_tile_seams(sb) == 4);
      auto replacer = [&](const assets::StageTile&, u32& w, u32& h, std::vector<u8>& rgba) {
        w = h = 64;
        rgba.assign(64 * 64 * 4, 128);
        return true;
      };
      CHECK(recompose_object_textures(sb, replacer) == 3 && sb.object_textures[0].scale == 4.0f);
      CHECK(build_tile_seams(sb) == 4);
      const auto& seams = sb.parts[0].seams;
      int flat = 0;
      for (const auto& se : seams) flat += se.flat ? 1 : 0;
      CHECK(seams.size() == 4 && flat == 2);
      // A's quad (indices 0..5) and B's quad (6..11) each get one strip for the edge x = 16, covering the whole quad in packet order
      const assets::StageSeam* sa = nullptr;
      const assets::StageSeam* sbm = nullptr;
      for (const auto& se : seams) {
        if (se.index_pos == 0 && se.flat) sa = &se;
        if (se.index_pos == 6 && se.flat) sbm = &se;
      }
      CHECK(sa && sbm);
      if (sa && sbm) {
        auto same_uv = [](const std::array<float, 2>& lhs, const std::array<float, 2>& rhs) { return std::fabs(lhs[0] - rhs[0]) < 1e-5f && std::fabs(lhs[1] - rhs[1]) < 1e-5f; };
        const auto& verts = sb.parts[0].vertices;
        CHECK(sa->corners == 4 && sa->submesh == 0 && (sa->v == std::array<u32, 4>{0, 1, 2, 3}));
        // band = 4 texels of 16 / 15 units (the quad spans texel centres 8..23): 4.2667; the far corners 0 / 2 are 16 units away = 3.75 bands, the edge corners 1 / 3 at 0
        CHECK(std::fabs(sa->g[0] - 3.75f) < 1e-4f && sa->g[1] == 0.0f && std::fabs(sa->g[2] - 3.75f) < 1e-4f && sa->g[3] == 0.0f);
        // mirrored across x = 16 onto B: A's corners 0 (0,0,0) / 1 (16,0,0) / 2 (0,0,16) / 3 (16,0,16) land on B's 5 (32,0,0) / 4 / 7 (32,0,16) / 6
        CHECK(same_uv(sa->uv[0], verts[5].uv) && same_uv(sa->uv[1], verts[4].uv) && same_uv(sa->uv[2], verts[7].uv) && same_uv(sa->uv[3], verts[6].uv));
        // and B's strip the other way round: its corners 4 / 5 / 6 / 7 land on A's 1 / 0 / 3 / 2
        CHECK(sbm->corners == 4 && (sbm->v == std::array<u32, 4>{4, 5, 6, 7}));
        CHECK(same_uv(sbm->uv[0], verts[1].uv) && same_uv(sbm->uv[1], verts[0].uv) && same_uv(sbm->uv[2], verts[3].uv) && same_uv(sbm->uv[3], verts[2].uv));
        CHECK(sbm->g[0] == 0.0f && std::fabs(sbm->g[1] - 3.75f) < 1e-4f && sbm->g[2] == 0.0f && std::fabs(sbm->g[3] - 3.75f) < 1e-4f);
      }
      // the crease strips are recorded as not flat; a re-run from the pristine state rebuilds the same four
      int crease = 0;
      for (const auto& se : seams) crease += (!se.flat && (se.index_pos == 6 || se.index_pos == 12)) ? 1 : 0;
      CHECK(crease == 2);
      CHECK(recompose_object_textures(sb, [&](const assets::StageTile&, u32&, u32&, std::vector<u8>&) { return false; }) == 0 && build_tile_seams(sb) == 4 && std::fabs(sb.parts[0].seams[0].g[0] - 3.75f) < 1e-4f);
    }
    // A scroll emitter (op 35 bit 0) on a part gives it whole pages (here clipped to the 128-wide atlas): the page-origin relation holds.
    assets::StageAsset sc = before;
    assets::StageEmitter em;
    em.name = "e";
    em.mesh = "ring";
    em.ext = 1;
    sc.emitters.push_back(em);
    bake_object_textures(sc);
    CHECK(sc.object_textures[1].scrolls && !sc.object_textures[0].scrolls);
    {
      const auto& slots = sc.object_textures[1].slots;
      CHECK(slots.size() == 1 && sc.tiles[static_cast<size_t>(slots[0].tile)].w == 128 && sc.tiles[static_cast<size_t>(slots[0].tile)].h == 256);
      const assets::StageVertex& vertex = sc.parts[1].vertices[0];
      const float page_u = vertex.uv[0] - vertex.uv_raw[0] * vertex.uv_scale[0], page_v = (1.0f - vertex.uv[1]) - vertex.uv_raw[1] * vertex.uv_scale[1];
      CHECK(std::fabs(page_u * static_cast<float>(sc.object_textures[1].image.width) - (static_cast<float>(slots[0].dx + 1) + 0.5f)) < 1e-3f);
      CHECK(std::fabs(page_v * static_cast<float>(sc.object_textures[1].image.height) - (static_cast<float>(slots[0].dy + 1) + 0.5f)) < 1e-3f);
    }
    // A vertex shared by two atlases is duplicated at the end (the first user keeps its index).
    assets::StageAsset sh = before;
    sh.textures.push_back(at);
    {
      assets::StagePart& gp = sh.parts[1];
      assets::StageSubMesh s2 = gp.submeshes[0];
      // One more triangle on the first quad's corners, drawn from atlas 1.
      s2.texture = 1;
      s2.first_index = static_cast<u32>(gp.indices.size());
      s2.index_count = 3;
      for (u32 i : {0u, 1u, 2u}) gp.indices.push_back(i);
      gp.submeshes.push_back(s2);
    }
    bake_object_textures(sh);
    CHECK(sh.parts[1].vertices.size() == before.parts[1].vertices.size() + 3 && sh.parts[1].submeshes[1].object_texture == sh.parts[1].submeshes[0].object_texture);
    CHECK(sh.parts[1].indices[6] >= before.parts[1].vertices.size() && sh.parts[1].indices[0] == 0);
    // The second atlas holds the same pixels: identical tiles are shared across atlases too.
    CHECK(sh.tiles.size() == 3);
  }

  {  // the real stages + the title (SKIP without the game install)
    const std::string gd = std::getenv("OPENCROSS_GAMEDIR") ? std::getenv("OPENCROSS_GAMEDIR") : "";
    const auto dir = platform::find_game_dir(gd);
    vfs::Vfs game_files;
    if (!dir.empty() && game_files.open(dir)) scan_morph_bake(game_files);
    else std::printf("morph bake scan: SKIP (no game install)\n");
  }

  fs::remove_all(tmp);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("texture_registry: ok\n");
  return 0;
}
