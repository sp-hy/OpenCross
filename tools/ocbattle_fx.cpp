// ocbattle fx-audit: plays every battle effect pack headless and records what the generic player does with it.
//
//   ocbattle fx-audit [--csv FILE] [--md FILE] [--eid N] [--ticks N] [-v]
//   ocbattle fx-audit --find NAME   list the packs with an emitter header of that name (no audit; `*` = every at0.prd's emitter names);
//                                   dev env vars OC_FIND_TREE / OC_FIND_HEX / OC_FIND_USERS / OC_FIND_SEQOP select other searches
//
// Every slot of the effect table that holds a pack plus the six cast charge packs ch00..ch05 (battle.prd > sato) is
// run through BattleFxRun in a fixed scene (caster = PC 0, targets = enemies 3 and 4, a camera 5500 units behind the line) until it ends
// or the hard cap (BattleFxRun::kTickCap) is reached, over a stand-in stage ground (the ground overlays bake onto it as in a battle). Per
// pack: emitters started, objects spawned per sprite type, quads / effect-mesh triangles / bespoke primitives drawn per tick, sequence ops
// met that the battle presentation does not carry out, overlay-private ops, the bespoke module the overlay asks for, sprites and mesh
// faces whose texels are all transparent (texture missing in the pack's VRAM), names no registered drp holds, non-finite or runaway
// coordinates, effects that end at once or never. Ranked by use: element records, PC tech grids,
// element shops and the enemy battle scripts (`enemy_refs`, element id -> scenes).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "data/tables.h"
#include "game/battle/battle_fx.h"
#include "game/battle/model_table.h"
#include "game/effects/bespoke/bespoke.h"
#include "import/container.h"
#include "import/room_effects.h"
#include "vfs/vfs.h"

using namespace oc;
using namespace oc::game;

namespace {

struct PackRow {
  // eid < 0: charge pack ch0N (-1 - colour).
  int eid = 0;
  std::string prd, overlay, category;
  // "-" stock host, "module" present, "skipped" (family not run in battle), "missing".
  std::string bespoke_state = "-";
  std::string bespoke_module;
  std::vector<int> elements;
  int pc_refs = 0, shop_refs = 0, enemy_refs = 0;
  int uses = 0;
  bool player = false;
  // static
  size_t seqs = 0, emitters = 0, sheets = 0, parts = 0;
  bool loaded = false, started_seq = true, anmseq = false;
  // dynamic
  int ticks = 0;
  bool capped = false;
  int first_draw = -1, last_draw = -1;
  size_t max_quads = 0, max_tris = 0, max_flats = 0, max_live = 0;
  u64 sum_draw = 0;
  u64 spawned = 0;
  u32 starts = 0, unresolved_sheets = 0;
  // types: sprite type -> objects spawned; unpresented: sequence op -> hits (stock ops the presentation does not carry out);
  // nonvisual: those of them without a picture (rumble, CD / sound queue, music volume).
  std::map<int, u32> types;
  std::map<int, u32> unpresented;
  std::map<int, u32> nonvisual;
  u32 private_ops = 0, private_unhandled = 0;
  std::map<int, u32> private_ignored;
  // Overlay-private ops the pack's sequences hold, run or not: op -> commands.
  std::map<int, u32> private_list;
  u64 empty_sprite_quads = 0, quads_total = 0;
  u64 empty_tex_tris = 0, tex_tris = 0;
  u64 missing_part_inst = 0;
  // Mesh objects naming a PC's weapon by its two-digit character number (drawn from the caster's weapon, BattleView::fx_weapon_mesh_unit).
  u64 weapon_inst = 0;
  // Unresolved op-05 names ("s:" / "m:") that some drp the battle registers holds (ours misses a source: an issue), and those that no
  // such drp holds (the PS1 lookup fails the same way and the object draws nothing: data).
  std::set<std::string> unresolved_registered, unresolved_data;
  u64 nonfinite = 0, runaway = 0;
  // issues: generic-path issue classes.
  std::vector<std::string> issues;
  std::vector<std::string> bespoke_issues;
  // op-05 names that resolve nowhere (type 3 sheets "s:", type 14 meshes "m:", type 20 models "o:").
  std::set<std::string> missing_names;
  // A type-20 object names no loadable type-0x0B model of the pack (BattleView::fx_load_models).
  bool missing_model = false;
};

std::string join_ops(const std::map<int, u32>& ops) {
  std::string text;
  for (const auto& [op, hits] : ops) {
    if (!text.empty()) text += ' ';
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02X:%u", op, hits);
    text += buf;
  }
  return text;
}

std::string category_of(const std::string& prd) {
  // battle/effects/<class>/<name>.prd
  const size_t start = prd.find("effects/");
  if (start == std::string::npos) return "other";
  const size_t end = prd.find('/', start + 8);
  return end == std::string::npos ? "other" : prd.substr(start + 8, end - start - 8);
}

bool sprite_empty(const assets::EffectSprite& sp) {
  for (size_t i = 3; i < sp.rgba.size(); i += 4)
    if (sp.rgba[i]) return false;
  return true;
}

// Per stage part: which textured triangles sample at least one visible texel (a barycentric grid of 15 points, corners included: a ring or
// strip texture is transparent at the face corners and opaque inside).
struct PartTex {
  // Per triangle (indices / 3); tri_page = the sub-mesh's direct-colour page (StageSubMesh::direct_page).
  std::vector<u8> tri_textured, tri_visible;
  std::vector<int> tri_page;
};
std::vector<PartTex> part_textures(const assets::StageAsset& st) {
  std::vector<PartTex> out(st.parts.size());
  for (size_t pi = 0; pi < st.parts.size(); ++pi) {
    const assets::StagePart& part = st.parts[pi];
    PartTex& pt = out[pi];
    const size_t ntri = part.indices.size() / 3;
    pt.tri_textured.assign(ntri, 0);
    pt.tri_visible.assign(ntri, 0);
    pt.tri_page.assign(ntri, -1);
    for (const assets::StageSubMesh& sm : part.submeshes) {
      if (sm.texture < 0 || static_cast<size_t>(sm.texture) >= st.textures.size()) continue;
      const import::Image& im = st.textures[static_cast<size_t>(sm.texture)].image;
      auto alpha_at = [&](float u, float v) -> bool {
        if (!im.width || !im.height || im.indexed) return false;
        const int x = std::clamp(static_cast<int>(u * static_cast<float>(im.width)), 0, static_cast<int>(im.width) - 1);
        const int y = std::clamp(static_cast<int>((1.0f - v) * static_cast<float>(im.height)), 0, static_cast<int>(im.height) - 1);
        return im.pixels[(static_cast<size_t>(y) * im.width + static_cast<size_t>(x)) * 4 + 3] != 0;
      };
      for (u32 k = sm.first_index; k + 2 < sm.first_index + sm.index_count && k + 2 < part.indices.size(); k += 3) {
        const size_t tri = k / 3;
        pt.tri_textured[tri] = 1;
        pt.tri_page[tri] = sm.direct_page;
        const assets::StageVertex& c0 = part.vertices[part.indices[k]];
        const assets::StageVertex& c1 = part.vertices[part.indices[k + 1]];
        const assets::StageVertex& c2 = part.vertices[part.indices[k + 2]];
        constexpr int kSteps = 4;
        bool vis = false;
        for (int i = 0; i <= kSteps && !vis; ++i)
          for (int j = 0; i + j <= kSteps && !vis; ++j) {
            const float w1 = static_cast<float>(i) / kSteps, w2 = static_cast<float>(j) / kSteps, w0 = 1.0f - w1 - w2;
            vis = alpha_at(w0 * c0.uv[0] + w1 * c1.uv[0] + w2 * c2.uv[0], w0 * c0.uv[1] + w1 * c1.uv[1] + w2 * c2.uv[1]);
          }
        pt.tri_visible[tri] = vis ? 1 : 0;
      }
    }
  }
  return out;
}

bool weapon_mesh_name(const std::string& name) {
  return name.size() == 2 && std::isdigit(static_cast<unsigned char>(name[0])) && std::isdigit(static_cast<unsigned char>(name[1]));
}

// (drp type, name) of every entry the battle registers besides the pack, battle.prd and the particle library (which the run already
// searches): the stages battle/bg/*.prd and every unit model's at0.prd and .obj S6 drp, nested drps included.
using RegKey = std::pair<int, std::string>;
const std::set<RegKey>& battle_registered_names(const vfs::Vfs& game_fs) {
  static std::set<RegKey> names;
  static bool built = false;
  if (built) return names;
  built = true;
  std::function<void(ByteSpan)> walk = [&](ByteSpan bytes) {
    const auto drp = import::parse_drp(bytes);
    if (!drp) return;
    for (const auto& entry : drp->entries) {
      names.insert({static_cast<int>(entry.type), entry.name});
      if (entry.type == 1 && import::is_drp(entry.data)) walk(entry.data);
    }
  };
  for (const std::string& member : game_fs.list("cdrom"))
    if (member.rfind("battle/bg/", 0) == 0 && member.size() > 4 && member.compare(member.size() - 4, 4, ".prd") == 0)
      if (const auto file = game_fs.read("cdrom/" + member)) walk(*file);
  std::set<std::string> seen;
  for (const std::string_view sv : kBattleModels) {
    const std::string stem(sv);
    if (stem.empty() || !seen.insert(stem).second) continue;
    if (const auto at0 = game_fs.read("cdrom/" + stem.substr(0, stem.find_last_of('/')) + "/at0.prd")) walk(*at0);
    const auto obj = game_fs.read("cdrom/" + stem + ".obj");
    if (!obj || obj->size() < 32) continue;
    u32 s6 = 0;
    std::memcpy(&s6, obj->data() + 4 + 5 * 4, 4);
    if (s6 && s6 + 12 < obj->size()) walk(ByteSpan(obj->data() + s6, obj->size() - s6));
  }
  return names;
}

FxScene audit_scene() {
  FxScene scene;
  for (int i = 0; i < battle::kUnits; ++i) {
    FxUnit& unit = scene.unit[static_cast<size_t>(i)];
    unit.present = i < 5;
    unit.pc = i < 3;
    unit.x = unit.pc ? -1200.0f + 600.0f * static_cast<float>(i) : 1200.0f - 600.0f * static_cast<float>(i - 3);
    unit.z = unit.pc ? -800.0f : 800.0f;
    unit.yaw = unit.pc ? 0.0f : 3.14159265f;
    unit.height = 1000;
  }
  scene.caster = 0;
  scene.targets = {3, 4};
  return scene;
}

void run_pack(PackRow& row, const std::shared_ptr<const FxPack>& pack, const assets::RoomEffects& lib, int max_ticks, const vfs::Vfs& game_fs) {
  row.loaded = true;
  row.seqs = pack->fx.sequences.size();
  row.emitters = pack->fx.emitters.size();
  row.sheets = pack->fx.sheets.size();
  row.parts = pack->meshes ? pack->meshes->parts.size() : 0;
  // Texel coverage per mesh asset (the pack's own, the resident battle.prd one), cached across packs for the resident one.
  static std::map<const assets::StageAsset*, std::vector<PartTex>> tex_cache;
  auto ptex_of = [&](const assets::StageAsset* asset) -> const std::vector<PartTex>& {
    auto it = tex_cache.find(asset);
    if (it == tex_cache.end()) it = tex_cache.emplace(asset, part_textures(*asset)).first;
    return it->second;
  };
  if (pack->meshes && std::getenv("FXA_TEX")) {
    const assets::StageAsset& st = *pack->meshes;
    const auto& pt = ptex_of(&st);
    std::printf("  atlas x0 %d w %d, %zu textures\n", st.atlas_x0_hw, st.atlas_width_hw, st.textures.size());
    for (const auto& tx : st.textures) {
      size_t vis = 0;
      for (size_t i = 3; i < tx.image.pixels.size(); i += 4) vis += tx.image.pixels[i] != 0;
      std::printf("    tex mode %d clut %d,%d %ux%u visible %zu\n", tx.mode, tx.clut_x, tx.clut_y, tx.image.width, tx.image.height, vis);
    }
    for (size_t i = 0; i < st.parts.size(); ++i) {
      size_t textured = 0, visible = 0;
      for (size_t k = 0; k < pt[i].tri_textured.size(); ++k) textured += pt[i].tri_textured[k], visible += pt[i].tri_visible[k];
      std::printf("    part %s tris %zu textured %zu visible %zu\n", st.parts[i].name.c_str(), st.parts[i].indices.size() / 3, textured, visible);
    }
  }
  {
    effects::EffectBank bank;
    bank.add(&pack->fx);
    if (pack->extra) bank.add(pack->extra.get());
    if (pack->resident)
      for (const auto& resident_pack : pack->resident->packs) bank.add_library(resident_pack.get());
    bank.add_library(&lib);
    for (const auto& em : pack->fx.emitters)
      for (const auto& emitter_op : em.ops)
        if (emitter_op.op == 5)
          if (const auto* sp = std::get_if<assets::FxSprite>(&emitter_op.arg)) {
            size_t part = 0;
            if (sp->type == 3 && !bank.sheet(sp->name)) row.missing_names.insert("s:" + sp->name);
            if (sp->type == 14 && !pack->mesh_asset(sp->name, part)) row.missing_names.insert("m:" + sp->name);
            // Drawn when the pack holds a type-0x0B record of that name whose name decodes as a texture binding.
            if (sp->type == 20) {
              bool found = false;
              if (auto drp = import::parse_drp(pack->prd))
                for (const auto& entry : drp->entries)
                  found = found || (entry.type == 0x0B && entry.name == sp->name && entry.name.size() == 4 &&
                                    std::all_of(entry.name.begin(), entry.name.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; }));
              if (!found) {
                row.missing_names.insert("o:" + sp->name);
                row.missing_model = true;
              }
            }
          }
  }
  // Sheets are drp type 3, meshes type 2; a weapon name is resolved by the caster's weapon.
  const std::set<RegKey>& registered = battle_registered_names(game_fs);
  for (const std::string& name : row.missing_names) {
    const int drp_type = name.rfind("s:", 0) == 0 ? 3 : name.rfind("m:", 0) == 0 ? 2 : 0;
    if (!drp_type || (drp_type == 2 && weapon_mesh_name(name.substr(2)))) continue;
    (registered.count({drp_type, name.substr(2)}) ? row.unresolved_registered : row.unresolved_data).insert(name);
  }
  const FxScene scene = audit_scene();
  effects::ViewParams view;
  view.trans = {0, 400, 5500};
  view.oy = 112;
  // A stage ground as every battle has (cells present, texture words 0): the ground overlays bake their meshes onto it and draw them
  // themselves, as in a battle; what they draw counts as drawn.
  effects::FxGround ground;
  ground.present = true;
  for (effects::FxGroundCell& cell : ground.cells) cell.present = true;
  effects::set_fx_ground(&ground);
  BattleFxRun run(pack, lib, 1);
  BattleFxRun::Callbacks cb;
  cb.camera = [](std::string_view) {};
  run.set_callbacks(cb);
  run.start(scene, view);
  // Stand-in for the anmseq scripts (ops 0x68 / 0x7D).
  row.anmseq = run.start_script_sequences() > 0;
  if (run.sequences_active() == 0) row.started_seq = false;
  FxFrame frame;
  std::vector<effects::EmitterSystem::MeshInstance> mi;
  for (int tick = 0; tick < max_ticks; ++tick) {
    ground.draws.clear();
    ground.prims.clear();
    run.tick(scene);
    ++row.ticks;
    run.collect(view, 1.0f, frame);
    mi.clear();
    run.collect_meshes(view, 1.0f, mi);
    size_t tris = 0;
    for (const auto& instance : mi) {
      bool finite = std::isfinite(instance.centre[0]) && std::isfinite(instance.centre[1]) && std::isfinite(instance.centre[2]);
      for (float value : instance.mat.m) finite = finite && std::isfinite(value);
      if (!finite) {
        ++row.nonfinite;
        continue;
      }
      if (std::fabs(instance.world_pos[0]) > 60000 || std::fabs(instance.world_pos[1]) > 60000 || std::fabs(instance.world_pos[2]) > 60000) ++row.runaway;
      // Types 19 / 20 are counted by type.
      if (instance.sprite_type != 14) continue;
      size_t part = 0;
      const assets::StageAsset* asset = instance.name ? pack->mesh_asset(*instance.name, part) : nullptr;
      if (!asset && instance.name && weapon_mesh_name(*instance.name)) {
        ++row.weapon_inst;
        continue;
      }
      if (!asset) {
        ++row.missing_part_inst;
        if (std::getenv("OC_FX_MISSING")) std::fprintf(stderr, "missing part %s owner %d\n", instance.name ? instance.name->c_str() : "?", instance.owner);
        continue;
      }
      const PartTex& pt = ptex_of(asset)[part];
      tris += pt.tri_textured.size();
      // An op-36 UV-scrolled object samples its texture window at a moving phase: its static UVs say nothing about what shows
      // (maru / haienako 'owan').
      if (instance.uv_scroll) continue;
      // Faces on a page inside a standing frame copy (FxModels::capture, 320 x 224) sample the picture, not their texels.
      const effects::FxModels* models = run.bespoke_models();
      auto on_copy = [&](int page) {
        if (!models || models->capture.x < 0 || page < 0) return false;
        const int dx = (page & 0xFFFF) - models->capture.x, dy = (page >> 16) - models->capture.y;
        return dx >= 0 && dy >= 0 && dx < 320 && dy < 224;
      };
      for (size_t k = 0; k < pt.tri_textured.size(); ++k)
        if (pt.tri_textured[k] && !on_copy(pt.tri_page[k])) {
          ++row.tex_tris;
          if (!pt.tri_visible[k]) ++row.empty_tex_tris;
        }
    }
    for (const auto& quad : frame.quads) {
      bool finite = true;
      for (int k = 0; k < 4; ++k) finite = finite && std::isfinite(quad.x[k]) && std::isfinite(quad.y[k]);
      if (!finite) {
        ++row.nonfinite;
        continue;
      }
      ++row.quads_total;
      if (quad.sprite && sprite_empty(*quad.sprite)) ++row.empty_sprite_quads;
    }
    // Bespoke primitives: flat, textured (pack / model / VRAM sources) and the ground overlays' baked packets.
    const size_t bespoke_prims = frame.flats.size() + frame.textured.size() + ground.prims.size();
    const size_t drawn = frame.quads.size() + tris + bespoke_prims;
    row.max_quads = std::max(row.max_quads, frame.quads.size());
    row.max_tris = std::max(row.max_tris, tris);
    row.max_flats = std::max(row.max_flats, bespoke_prims);
    row.max_live = std::max(row.max_live, run.emitters().live_objects());
    row.sum_draw += drawn;
    if (drawn) {
      if (row.first_draw < 0) row.first_draw = tick;
      row.last_draw = tick;
    }
    if (std::getenv("FXA_TRACE") && tick % 100 == 0) {
      const auto es = run.emitters().stats();
      std::printf("  t=%d seq=%zu live=%u running=%u quads=%zu tris=%zu done=%d:", tick, run.sequences_active(), es.live, es.running, frame.quads.size(), tris,
                  run.done());
      for (const auto& sq : pack->fx.sequences)
        if (run.sequences().running(sq.name)) std::printf(" %s", sq.name.c_str());
      std::printf("\n");
    }
    if (run.done()) break;
  }
  effects::set_fx_ground(nullptr);
  row.capped = run.capped() || !run.done();
  const auto st = run.emitters().stats();
  row.spawned = st.spawned;
  row.starts = st.starts;
  row.unresolved_sheets = st.unresolved_sprites;
  for (size_t k = 0; k < st.type_spawns.size(); ++k)
    if (st.type_spawns[k]) row.types[static_cast<int>(k)] = st.type_spawns[k];
  const auto& ss = run.sequence_stats();
  // op lengths for the 0x2A classification: the battle form is 16 bytes
  std::map<int, int> op_len;
  for (const auto& sq : pack->fx.sequences)
    for (const auto& cmd : sq.cmds) {
      op_len[cmd.op] = std::max(op_len[cmd.op], static_cast<int>(cmd.len));
      if (cmd.op >= 0x100) ++row.private_list[cmd.op];
    }
  for (int op = 0; op < 0x100; ++op)
    if (const u32 hits = ss.op_hits[static_cast<size_t>(op)]; hits) {
      const auto op_class = BattleFxRun::op_class(op, op_len[op]);
      if (op_class == BattleFxRun::OpClass::Missing) row.unpresented[op] = hits;
      else if (op_class == BattleFxRun::OpClass::NonVisual) row.nonvisual[op] = hits;
    }
  row.private_ops = run.private_ops();
  row.private_unhandled = run.private_ops_unhandled();
  row.private_ignored = run.private_ops_ignored();
}

void classify(PackRow& row) {
  auto add = [&](const char* issue) { row.issues.emplace_back(issue); };
  if (!row.loaded) {
    add("no-pack");
    return;
  }
  // A pack without sequences (the stage packs bg_86..90 in effect slots, the script-only packs) has nothing to start.
  if (!row.started_seq && row.seqs) add("no-sequence");
  // Blank: drawable objects were spawned (anything but type-17 lights and type-22 sounds) and nothing was drawn. A pack that spawns none
  // is a model / camera / sound effect (the unit animation scripts, the bespoke dissolves and waves of the unit models), which the
  // headless run does not draw.
  const bool drawable = std::any_of(row.types.begin(), row.types.end(), [](const auto& type_count) { return type_count.first != 17 && type_count.first != 22; });
  if (row.sum_draw == 0 && drawable) add("blank");
  // A script-only pack (kill, bre_roza) lasts as long as the caster's script, which the headless run does not play.
  if (row.ticks < 8 && row.seqs) add("instant");
  if (row.capped) add("never-ends");
  if (!row.unpresented.empty()) add("ops-not-presented");
  const auto registered_kind = [&](const char* prefix) {
    return std::any_of(row.unresolved_registered.begin(), row.unresolved_registered.end(), [&](const std::string& name) { return name.rfind(prefix, 0) == 0; });
  };
  if (row.unresolved_sheets && registered_kind("s:")) add("sheet-unresolved");
  if (row.missing_part_inst && registered_kind("m:")) add("mesh-part-missing");
  // Texels: a sprite whose every texel is transparent, a textured face that samples none at its grid points (holes in a texture
  // are normal: only a majority of empty draws counts).
  if (row.quads_total && row.empty_sprite_quads * 10 >= row.quads_total * 9) add("sprite-texture-missing");
  else if (row.quads_total && row.empty_sprite_quads * 2 >= row.quads_total) add("sprite-texture-partial");
  if (row.tex_tris && row.empty_tex_tris * 10 >= row.tex_tris * 9) add("mesh-texture-missing");
  else if (row.tex_tris && row.empty_tex_tris * 2 >= row.tex_tris) add("mesh-texture-partial");
  // 15 flat / line built-ins, 19 stage ground, 20 models (counted below by name), 21 lens flares, 22 AKAO by pointer (no picture); 17 = a light
  // slot for the lit models (no primitive of its own; EmitterSystem writes effects::LightTable, the view lights
  // the units the sequence flagged). `lit-path` remains for ops 0x65 / 0x66 should they fall out of op_class.
  for (const auto& [type, count] : row.types) {
    if (type != 3 && type != 14 && type != 15 && type != 17 && type != 19 && type != 20 && type != 21 && type != 22) {
      add("sprite-type-not-drawn");
      break;
    }
  }
  if (row.missing_model && row.types.count(20)) add("model-missing");
  if (row.unpresented.count(0x65) || row.unpresented.count(0x66)) add("lit-path");
  if (row.nonfinite) add("non-finite");
  if (row.runaway) add("runaway");
  if (row.bespoke_state == "missing") row.bespoke_issues.emplace_back("module-missing");
  if (row.bespoke_state == "skipped") row.bespoke_issues.emplace_back("module-not-run");
  // A stock host has no private ops: the PS1 forwards them to it and nothing happens (eid 142, hensin.prd under stdtech0).
  if (row.private_unhandled && row.bespoke_state != "-") row.bespoke_issues.emplace_back("private-ops-unhandled");
}

std::string joined(const std::vector<std::string>& items, const char* sep) {
  std::string text;
  for (const auto& item : items) {
    if (!text.empty()) text += sep;
    text += item;
  }
  return text;
}

std::string elem_list(const PackRow& row, size_t maxn) {
  std::string text;
  for (size_t i = 0; i < row.elements.size() && i < maxn; ++i) text += (i ? " " : "") + std::to_string(row.elements[i]);
  if (row.elements.size() > maxn) text += " ...";
  return text;
}

std::string types_str(const PackRow& row) {
  std::string text;
  for (const auto& [type, count] : row.types) text += (text.empty() ? "" : " ") + std::to_string(type) + ":" + std::to_string(count);
  return text;
}

}  // namespace

int cmd_fx_audit(const data::GameTables& tables, const vfs::Vfs& game_fs, const std::vector<std::string>& args, const std::map<int, int>& enemy_refs) {
  std::string csv_path = "battle_effects_audit.csv", md_path;
  int only = 0, max_ticks = BattleFxRun::kTickCap + 2;
  bool verbose = false;
  std::string find_name;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--csv" && i + 1 < args.size()) csv_path = args[++i];
    else if (args[i] == "--md" && i + 1 < args.size()) md_path = args[++i];
    else if (args[i] == "--eid" && i + 1 < args.size()) only = std::atoi(args[++i].c_str());
    else if (args[i] == "--ticks" && i + 1 < args.size()) max_ticks = std::atoi(args[++i].c_str());
    else if (args[i] == "-v") verbose = true;
    else if (args[i] == "--find" && i + 1 < args.size()) find_name = args[++i];
  }
  BattleFxLibrary lib;
  if (!lib.init(game_fs)) {
    std::fprintf(stderr, "fx-audit: no effect slot table\n");
    return 1;
  }
  // ---- usage
  std::map<int, std::vector<int>> eid_elems;
  std::map<int, int> pc_refs, shop_refs;
  for (int id = 1; id < static_cast<int>(tables.elements.size()); ++id)
    if (!tables.elements[static_cast<size_t>(id)].empty()) eid_elems[BattleFxLibrary::element_eid(tables.elements[static_cast<size_t>(id)])].push_back(id);
  if (auto it = tables.char_sets.find("char"); it != tables.char_sets.end())
    for (const auto& rec : it->second.rec)
      for (u16 cell : rec.grid())
        if (cell & 0x3FF) ++pc_refs[cell & 0x3FF];
  for (const auto& row : tables.el_shop)
    for (u16 id : row)
      if (id) ++shop_refs[id & 0x3FF];

  if (!find_name.empty()) {
    // --find also lists the unit models' at0.prd packs (battle/<dir>/<model>/at0.prd).
    static const import::Vram kNoVram;
    std::set<std::string> seen;
    // OC_FIND_TREE=battle/monster/torimeca/at0.prd: the drp tree with the emitter names of every nested drp.
    if (const char* tree = std::getenv("OC_FIND_TREE")) {
      std::function<void(ByteSpan, int)> walk = [&](ByteSpan bytes, int depth) {
        const auto drp = import::parse_drp(bytes);
        if (!drp) return;
        std::string names;
        const assets::RoomEffects fx = import::load_battle_effects(bytes, {}, kNoVram, {});
        for (const auto& em : fx.emitters) names += " " + em.name;
        std::printf("%*sdrp (%zu entries) emitters:%s\n", depth * 2, "", drp->entries.size(), names.c_str());
        for (const auto& entry : drp->entries) {
          std::string has;
          for (const char* nm : {"k000", "rrrr", "dash", "fire", "gd00", "p000"})
            if (std::search(entry.data.begin(), entry.data.end(), nm, nm + 4) != entry.data.end()) has += std::string(" has:") + nm;
          std::printf("%*s- '%s' type %d size %zu%s\n", depth * 2 + 2, "", entry.name.c_str(), static_cast<int>(entry.type), entry.data.size(), has.c_str());
          if (entry.type == 1 && import::is_drp(entry.data)) walk(entry.data, depth + 2);
        }
      };
      if (const auto file = game_fs.read(std::string("cdrom/") + tree)) {
        // A model .obj path walks its S6 DRP (header u32 offsets at +4, entry 5 = S6).
        const std::string ts(tree);
        if (ts.size() > 4 && ts.compare(ts.size() - 4, 4, ".obj") == 0) {
          u32 s6 = 0;
          if (file->size() >= 32) std::memcpy(&s6, file->data() + 4 + 5 * 4, 4);
          if (s6 && s6 < file->size()) walk(ByteSpan(file->data() + s6, file->size() - s6), 0);
          else std::printf("no S6\n");
        } else walk(*file, 0);
      }
      return 0;
    }
    // OC_FIND_HEX=<hex bytes>: byte-search every pack file and at0.prd.
    if (const char* hx = std::getenv("OC_FIND_HEX")) {
      std::vector<u8> pat;
      for (const char* cursor = hx; cursor[0] && cursor[1]; cursor += 2) pat.push_back(static_cast<u8>(std::strtol(std::string(cursor, 2).c_str(), nullptr, 16)));
      auto scan = [&](const std::string& path) {
        const auto file = game_fs.read(path);
        if (file && std::search(file->begin(), file->end(), pat.begin(), pat.end()) != file->end()) std::printf("hex: %s\n", path.c_str());
      };
      for (const auto& slot : lib.slots()) if (!slot.prd.empty()) scan("cdrom/" + slot.prd);
      for (const std::string_view stem : kBattleModels) { const std::string st(stem); if (!st.empty() && seen.insert(st.substr(0, st.find_last_of('/'))).second) scan("cdrom/" + st.substr(0, st.find_last_of('/')) + "/at0.prd"); }
      return 0;
    }
    // OC_FIND_USERS=a,b,c: the enemy ids and scenes whose model stem ends with one of those names.
    if (const char* us = std::getenv("OC_FIND_USERS")) {
      for (const std::string name : {std::string(us)}) {
        std::set<std::string> want;
        for (size_t begin = 0, comma; begin <= name.size(); begin = comma + 1) { comma = name.find(',', begin); if (comma == std::string::npos) comma = name.size(); want.insert(name.substr(begin, comma - begin)); }
        for (size_t en = 0; en < tables.enemies.size(); ++en) {
          const size_t mi = tables.enemies[en].model();
          if (mi >= kBattleModels.size() || kBattleModels[mi].empty()) continue;
          const std::string stem(kBattleModels[mi]);
          const std::string leaf = stem.substr(stem.find_last_of('/') + 1);
          if (!want.count(leaf)) continue;
          std::string sc;
          for (size_t i = 0; i < tables.scenes.size(); ++i)
            for (int k = 0; k < 3; ++k)
              if (tables.scenes[i].enemy_count(k) && tables.scenes[i].enemy_id(k) == en) { sc += " " + std::to_string(i) + (tables.scenes[i].raw[0x1B] ? "(offstage=" + std::to_string(tables.scenes[i].raw[0x1B]) + ")" : ""); break; }
          std::printf("%s enemy %zu %s scenes:%s\n", leaf.c_str(), en, tables.enemy_name(static_cast<int>(en)).c_str(), sc.c_str());
        }
      }
      return 0;
    }
    // OC_FIND_SEQOP=<hex op>: every sequence command of that op in the unit models' at0.prd and S6 DRPs (stem, sequence, offset, bytes).
    if (const char* so = std::getenv("OC_FIND_SEQOP")) {
      const int want = static_cast<int>(std::strtol(so, nullptr, 16));
      std::set<std::string> done;
      auto dump = [&](const std::string& where, ByteSpan bytes) {
        const assets::RoomEffects fx = import::load_battle_effects(bytes, {}, kNoVram, {});
        for (const auto& sq : fx.sequences)
          for (const auto& cmd : sq.cmds)
            if (cmd.op == want) {
              std::string hex;
              for (size_t k = 0; k < cmd.data.size() && k < 12; ++k) { char buf[4]; std::snprintf(buf, sizeof buf, "%02X", cmd.data[k]); hex += buf; }
              std::printf("%s seq '%s' op %02X @%u wait %u data %s\n", where.c_str(), sq.name.c_str(), want, cmd.offset, cmd.wait, hex.c_str());
            }
      };
      std::function<void(const std::string&, ByteSpan)> walk = [&](const std::string& where, ByteSpan bytes) {
        const auto drp = import::parse_drp(bytes);
        if (!drp) return;
        dump(where, bytes);
        for (const auto& entry : drp->entries)
          if (entry.type == 1 && import::is_drp(entry.data)) walk(where, entry.data);
      };
      for (const std::string_view sv : kBattleModels) {
        const std::string stem(sv);
        if (stem.empty() || !done.insert(stem).second) continue;
        if (const auto prd = game_fs.read("cdrom/" + stem.substr(0, stem.find_last_of('/')) + "/at0.prd"); prd && prd->size() >= 0x40 && done.insert(stem.substr(0, stem.find_last_of('/')) + "/at0").second)
          walk(stem.substr(0, stem.find_last_of('/')) + "/at0.prd", *prd);
        const auto obj = game_fs.read("cdrom/" + stem + ".obj");
        if (!obj || obj->size() < 32) continue;
        u32 s6 = 0;
        std::memcpy(&s6, obj->data() + 4 + 5 * 4, 4);
        if (s6 && s6 + 12 < obj->size()) walk(stem + ".obj S6", ByteSpan(obj->data() + s6, obj->size() - s6));
      }
      return 0;
    }
    for (const std::string_view stem : kBattleModels) {
      const std::string st(stem);
      const std::string at0 = "cdrom/" + st.substr(0, st.find_last_of('/')) + "/at0.prd";
      if (st.empty() || !seen.insert(at0).second) continue;
      const auto prd = game_fs.read(at0);
      if (!prd || prd->size() < 0x40) continue;
      const assets::RoomEffects fx = import::load_battle_effects(*prd, {}, kNoVram, {});
      if (find_name == "*") {
        std::string names;
        for (const auto& em : fx.emitters) names += " " + em.name;
        std::printf("at0 %s:%s\n", at0.c_str(), names.c_str());
        continue;
      }
      for (const auto& em : fx.emitters)
        if (em.name == find_name) {
          std::printf("%s: at0 %s\n", find_name.c_str(), at0.c_str());
          break;
        }
    }
  }
  std::vector<PackRow> rows;
  auto fill_usage = [&](PackRow& row) {
    if (auto it = eid_elems.find(row.eid); it != eid_elems.end()) row.elements = it->second;
    for (int id : row.elements) {
      if (auto pc_it = pc_refs.find(id); pc_it != pc_refs.end()) row.pc_refs += pc_it->second;
      if (auto shop_it = shop_refs.find(id); shop_it != shop_refs.end()) row.shop_refs += shop_it->second;
      if (auto enemy_it = enemy_refs.find(id); enemy_it != enemy_refs.end()) row.enemy_refs += enemy_it->second;
    }
    static const std::set<std::string> player_dirs = {"black", "white", "red", "blue", "green", "yellow", "syoukan", "tech"};
    row.player = row.pc_refs > 0 || row.shop_refs > 0 || player_dirs.count(row.category) != 0;
    row.uses = static_cast<int>(row.elements.size()) + row.pc_refs + row.shop_refs + row.enemy_refs;
  };
  static const std::set<std::string> stock = {"stdtech0", "stdtech1", "stdmag0", "magtest"};
  for (const auto& slot : lib.slots()) {
    if (only && slot.eid != only) continue;
    if (slot.prd.size() < 4 || slot.prd.compare(slot.prd.size() - 4, 4, ".prd") != 0) continue;
    PackRow row;
    row.eid = slot.eid;
    row.prd = slot.prd;
    row.overlay = effects::overlay_stem(slot.prg);
    row.category = category_of(slot.prd);
    if (!stock.count(row.overlay) && !row.overlay.empty()) {
      const effects::BespokeInfo* bi = effects::BespokeRegistry::instance().find(row.overlay);
      if (!bi) row.bespoke_state = "missing";
      else {
        row.bespoke_module = bi->family + "/" + bi->name;
        // BespokeInfo::battle: whether the family runs in battle (BattleFxRun::start).
        row.bespoke_state = bi->battle ? "module" : "skipped";
      }
    }
    fill_usage(row);
    auto pack = lib.get(slot.eid);
    if (!find_name.empty()) {
      if (pack)
        for (const auto& em : pack->fx.emitters)
          if (em.name == find_name) {
            std::printf("%s: eid %d %s\n", find_name.c_str(), slot.eid, slot.prd.c_str());
            break;
          }
      continue;
    }
    if (pack) run_pack(row, pack, lib.particle_lib(), max_ticks, game_fs);
    classify(row);
    if (verbose) {
      std::string miss;
      for (const auto& name : row.missing_names) miss += " " + name;
      std::printf("eid %d %s: %s%s%s\n", row.eid, row.prd.c_str(), joined(row.issues, ",").c_str(), miss.empty() ? "" : " | unresolved:", miss.c_str());
    }
    rows.push_back(std::move(row));
  }
  if (!only)
    for (int colour = 0; colour < 6; ++colour) {
      auto pack = lib.charge_pack(colour);
      PackRow row;
      row.eid = -1 - colour;
      row.prd = "battle/battle.prd>sato>ch0" + std::to_string(colour);
      row.overlay = "charge";
      row.category = "charge";
      row.player = true;
      row.uses = 1000;
      if (pack) run_pack(row, pack, lib.particle_lib(), max_ticks, game_fs);
      classify(row);
      rows.push_back(std::move(row));
    }
  std::stable_sort(rows.begin(), rows.end(), [](const PackRow& lhs, const PackRow& rhs) {
    if (lhs.uses != rhs.uses) return lhs.uses > rhs.uses;
    if (lhs.player != rhs.player) return lhs.player;
    return lhs.eid < rhs.eid;
  });

  // ---- CSV
  // --find never rewrites the audit csv.
  if (!csv_path.empty() && find_name.empty()) {
    std::ofstream out(csv_path, std::ios::binary);
    out << "rank,eid,prd,category,overlay,bespoke,bespoke_module,uses,elements,pc_refs,shop_refs,enemy_refs,player,seqs,emitters,sheets,mesh_parts,"
         "ticks,never_ends,first_draw,last_draw,max_quads,max_mesh_tris,max_bespoke_prims,max_live,spawned,emitter_starts,sprite_types,"
         "unresolved_sheets,missing_mesh_parts,weapon_mesh_objects,unresolved_in_data,empty_sprite_quads,quads,empty_tex_tris,tex_tris,non_finite,runaway,ops_not_presented,"
         "ops_non_visual,private_ops,private_unhandled,generic_issues,bespoke_issues,private_list,private_ignored\n";
    int rank = 0;
    for (const PackRow& row : rows) {
      out << ++rank << ',' << row.eid << ',' << row.prd << ',' << row.category << ',' << row.overlay << ',' << row.bespoke_state << ',' << row.bespoke_module << ','
          << row.uses << ',' << elem_list(row, 1000) << ',' << row.pc_refs << ',' << row.shop_refs << ',' << row.enemy_refs << ',' << (row.player ? "Y" : "") << ','
          << row.seqs << ',' << row.emitters << ',' << row.sheets << ',' << row.parts << ',' << row.ticks << ',' << (row.capped ? "Y" : "") << ',' << row.first_draw
          << ',' << row.last_draw << ',' << row.max_quads << ',' << row.max_tris << ',' << row.max_flats << ',' << row.max_live << ',' << row.spawned << ','
          << row.starts << ',' << types_str(row) << ',' << row.unresolved_sheets << ',' << row.missing_part_inst << ',' << row.weapon_inst << ','
          << joined(std::vector<std::string>(row.unresolved_data.begin(), row.unresolved_data.end()), " ") << ',' << row.empty_sprite_quads << ','
          << row.quads_total << ',' << row.empty_tex_tris << ',' << row.tex_tris << ',' << row.nonfinite << ',' << row.runaway << ',' << join_ops(row.unpresented)
          << ',' << join_ops(row.nonvisual) << ',' << row.private_ops << ',' << row.private_unhandled << ',' << joined(row.issues, " ") << ',' << joined(row.bespoke_issues, " ") << ',' << join_ops(row.private_list) << ',' << join_ops(row.private_ignored) << '\n';
    }
  }

  // ---- summary
  std::map<std::string, int> cls, bcls;
  // Per op: packs, hits.
  std::map<int, std::pair<int, u64>> ops, nvops;
  int clean = 0, generic_clean = 0;
  for (const PackRow& row : rows) {
    for (const auto& issue : row.issues) ++cls[issue];
    for (const auto& issue : row.bespoke_issues) ++bcls[issue];
    for (const auto& [op, hits] : row.unpresented) {
      ++ops[op].first;
      ops[op].second += hits;
    }
    for (const auto& [op, hits] : row.nonvisual) {
      ++nvops[op].first;
      nvops[op].second += hits;
    }
    generic_clean += row.issues.empty();
    clean += row.issues.empty() && row.bespoke_issues.empty();
  }
  std::printf("fx-audit: %zu packs (%d clean, %d clean on the generic path)\n", rows.size(), clean, generic_clean);
  std::printf("generic-path issue classes (packs):\n");
  for (const auto& [issue, count] : cls) std::printf("  %-24s %d\n", issue.c_str(), count);
  std::printf("bespoke issue classes (packs):\n");
  for (const auto& [issue, count] : bcls) std::printf("  %-24s %d\n", issue.c_str(), count);
  std::printf("sequence ops not presented (op: packs / hits):");
  for (const auto& [op, ph] : ops) std::printf(" %02X:%d/%llu", op, ph.first, static_cast<unsigned long long>(ph.second));
  std::printf("\nsequence ops without a picture, not carried out (op: packs / hits):");
  for (const auto& [op, ph] : nvops) std::printf(" %02X:%d/%llu", op, ph.first, static_cast<unsigned long long>(ph.second));
  std::printf("\n");

  // ---- markdown status table
  if (!md_path.empty()) {
    std::ofstream out(md_path, std::ios::binary);
    out << "| rank | eid | pack | uses | elements | ticks | max q/t/p | generic issues | ops not presented | bespoke |\n";
    out << "|---|---|---|---|---|---|---|---|---|---|\n";
    int rank = 0;
    for (const PackRow& row : rows) {
      ++rank;
      std::string pack = row.prd;
      if (const size_t pos = pack.find("effects/"); pos != std::string::npos) pack = pack.substr(pos + 8);
      std::string bespoke = row.bespoke_state == "-" ? "" : row.overlay + " " + row.bespoke_state;
      if (!row.bespoke_issues.empty()) bespoke += " (" + joined(row.bespoke_issues, ", ") + ")";
      out << "| " << rank << " | " << row.eid << " | " << pack << " | " << row.uses << " | " << elem_list(row, 6) << " | " << row.ticks << (row.capped ? "+" : "")
          << " | " << row.max_quads << "/" << row.max_tris << "/" << row.max_flats << " | " << joined(row.issues, ", ") << " | " << join_ops(row.unpresented) << " | "
          << bespoke << " |\n";
    }
  }
  return 0;
}
