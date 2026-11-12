// Every element's effect pack (pgdat +0x10 -> slot files) loaded and run headlessly through BattleFxRun for 600 ticks.
// Needs the game install (OPENCROSS_GAMEDIR or a parent of the exe); without it the test reports SKIP and passes.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <set>
#include <string>
#include <vector>

#include "data/tables.h"
#include "game/battle/battle_fx.h"
#include "game/battle/battle_model_cache.h"
#include "game/battle/battle_seq.h"
#include "game/effects/bespoke/bespoke.h"
#include "import/battle_stage.h"
#include "import/container.h"
#include "import/room_effects.h"
#include "platform/paths.h"
#include "vfs/vfs.h"

using namespace oc;
using namespace oc::game;

// The bg_00 sun (`far` emitter, op 05 sprite `lenz`, type 21) carries colour 1422 / 2108 / 2446 = flash
// bytes 88 / 131 / 152, and our wash at the screen centre is exactly those bytes; outside the PS1 frame the wash is 0 and the ghosts still show
// inside the (wider) picture.
static int check_lenz(vfs::Vfs& files) {
  int bad = 0;
  auto prd = files.read("cdrom/battle/bg/bg_00.prd");
  if (!prd) return 0;
  const Bytes ctd = files.read("cdrom/battle/bg/bg_00.ctd").value_or(Bytes{});
  std::string err;
  auto st = import::load_stage(*prd, ctd, &err);
  if (!st) { std::printf("FAIL lenz: bg_00 %s\n", err.c_str()); return 1; }
  int lens_count = 0;
  for (const auto& emitter : st->emitters) {
    if (emitter.sprite_type != 21) continue;
    ++lens_count;
    const int want[3] = {1422, 2108, 2446}, bytes[3] = {88, 131, 152};
    for (int i = 0; i < 3; ++i) {
      if (emitter.color[static_cast<size_t>(i)] != want[i]) { std::printf("FAIL lenz colour %d = %d\n", i, emitter.color[static_cast<size_t>(i)]); ++bad; }
      const int flash = (emitter.color[static_cast<size_t>(i)] >> 4) & 0xFF;
      if (flash != bytes[i] || effects::lens_wash_channel(flash, 160, 112) != bytes[i] || effects::lens_wash_channel(flash, 400, 112) != 0) { std::printf("FAIL lenz flash %d\n", i); ++bad; }
    }
  }
  if (lens_count != 1) { std::printf("FAIL lenz: %d type-21 emitters in bg_00\n", lens_count); ++bad; }
  return bad;
}

// The idle (S5 entry 1) and the element end hook's entry 11 of every battle player model, run for a dead PC: with the idle-pose word 1 the
// pick plays clip 8 with blend 16 and the dead branch shuts the eyes; with the word 8 the poison step writes it plays the same clip through
// the word, with no eye-shut op.
static int check_ko_pose_picks(vfs::Vfs& files) {
  int bad = 0, models = 0;
  for (const std::string& path : files.list("cdrom")) {
    if (!path.starts_with("battle/player/") || !path.ends_with(".obj")) continue;
    const auto obj = files.read("cdrom/" + path);
    bseq::Table table;
    if (!obj || !bseq::load_model_table(*obj, table)) continue;
    ++models;
    for (const int entry : {1, 11})
      for (const int pose : {1, 8}) {
        bseq::Ctx ctx;
        ctx.dead = true;
        ctx.pose = pose;
        bseq::Vm vm;
        bool clip8 = false, shut = false;
        if (vm.start(&table, entry, 0)) {
          std::vector<bseq::Event> out;
          for (int k = 0; k < 200 && vm.running(); ++k) {
            out.clear();
            vm.tick(ctx, out);
            for (const bseq::Event& event : out) {
              if (event.kind == bseq::Event::SetAnim && event.a == 8 && (event.b & 0x1FFF) == 16) clip8 = true;
              if (event.kind == bseq::Event::Eye && event.a == 2) shut = true;
            }
          }
        }
        if (!clip8 || shut != (pose == 1)) {
          std::printf("FAIL ko pose: %s entry %d pose word %d: clip 8 %d, eyes shut %d\n", path.c_str(), entry, pose, clip8, shut);
          ++bad;
        }
      }
  }
  if (models < 40) {
    std::printf("FAIL ko pose: only %d player models\n", models);
    ++bad;
  }
  return bad;
}

// The weapon-throw emitters of the units' own at0.prd (a boomerang's flight, the thrown weapons of Harle, Luccia, Sneff and Doc) draw a
// type-14 mesh sprite named after the thrower's character number, which resolves to that PC's weapon mesh.
static int check_thrown_weapon_meshes(vfs::Vfs& files) {
  int bad = 0;
  if (weapon_mesh_character("34") != 34 || weapon_mesh_character("02") != 2 || weapon_mesh_character("maru") != -1 || weapon_mesh_character("3") != -1 ||
      weapon_mesh_character("3a") != -1) {
    std::printf("FAIL thrown weapon: weapon_mesh_character\n");
    ++bad;
  }
  struct Thrower {
    const char* folder;
    int character, emitters;
  };
  const Thrower throwers[] = {{"lutianna", 11, 2}, {"harley", 22, 2}, {"mell", 34, 8}, {"banc", 36, 8}, {"sunef", 37, 2}, {"doc", 39, 2}};
  static const import::Vram kEmpty;
  for (const Thrower& thrower : throwers) {
    auto prd = files.read(std::string("cdrom/battle/player/") + thrower.folder + "/at0.prd");
    if (!prd) {
      std::printf("FAIL thrown weapon: no at0.prd for %s\n", thrower.folder);
      ++bad;
      continue;
    }
    const assets::RoomEffects fx = import::load_battle_effects(*prd, {}, kEmpty, {});
    int found = 0;
    for (const assets::Emitter& emitter : fx.emitters)
      for (const assets::EffectOp& op : emitter.ops)
        if (const auto* sprite = std::get_if<assets::FxSprite>(&op.arg); sprite && op.op == 0x05 && sprite->type == 14 && weapon_mesh_character(sprite->name) >= 0) {
          if (weapon_mesh_character(sprite->name) != thrower.character) {
            std::printf("FAIL thrown weapon: %s emitter %s draws '%s'\n", thrower.folder, emitter.name.c_str(), sprite->name.c_str());
            ++bad;
          }
          ++found;
        }
    if (found != thrower.emitters) {
      std::printf("FAIL thrown weapon: %s has %d weapon-mesh emitters, expected %d\n", thrower.folder, found, thrower.emitters);
      ++bad;
    }
  }
  return bad;
}

// 3voice op 0x100: the operand's tag starts on the named acting-list entry; a zero tag starts nothing.
static int check_3voice_start() {
  effects::FxContext ctx;
  std::string started;
  int caster = -1;
  ctx.hooks.start_for_caster = [&](std::string_view name, int entry) {
    started = name;
    caster = entry;
  };
  auto overlay = effects::BespokeRegistry::instance().create("3voice", ctx);
  if (!overlay) { std::printf("FAIL 3voice: no overlay\n"); return 1; }
  const int none[5] = {0, 0, 0, 0, 2};
  overlay->command(0x100, none, 5);
  if (!started.empty()) { std::printf("FAIL 3voice: a zero tag started %s\n", started.c_str()); return 1; }
  const int shut[5] = {0, 0, 'h' << 8 | 's', 't' << 8 | 'u', 2};
  overlay->command(0x100, shut, 5);
  if (started != "shut" || caster != 2) { std::printf("FAIL 3voice: started '%s' on %d\n", started.c_str(), caster); return 1; }
  return 0;
}

int main() {
  if (check_3voice_start()) return 1;
  std::string gd = std::getenv("OPENCROSS_GAMEDIR") ? std::getenv("OPENCROSS_GAMEDIR") : "";
  const auto dir = platform::find_game_dir(gd);
  vfs::Vfs files;
  if (dir.empty() || !files.open(dir)) {
    std::printf("SKIP: no game install\n");
    return 0;
  }
  data::GameTables tables;
  std::string err;
  if (!tables.load(files, &err)) {
    std::printf("FAIL tables: %s\n", err.c_str());
    return 1;
  }
  {
    // Every character set takes its Lv 99 targets from lv99para.bin (Serge 850 / 88 / 78 / 95 / 25 / 52 / 43), char_w1..5 included.
    const data::Lv99& serge = tables.chars("char").lv99[0];
    if (serge.hp != 850 || serge.atk != 88 || serge.def != 78 || serge.hit != 95 || serge.evd != 25 || serge.mag != 52 || serge.mdef != 43) {
      std::printf("FAIL lv99: Serge's char row is not lv99para.bin\n");
      return 1;
    }
    for (const auto& [set_name, set] : tables.char_sets)
      for (size_t i = 0; i < set.lv99.size(); ++i) {
        const data::Lv99& row = set.lv99[i];
        const data::Lv99& want = tables.chars("char").lv99[i];
        if (row.hp != want.hp || row.atk != want.atk || row.def != want.def || row.hit != want.hit || row.evd != want.evd || row.mag != want.mag ||
            row.mdef != want.mdef) {
          std::printf("FAIL lv99: set %s character %zu differs from lv99para.bin\n", set_name.c_str(), i);
          return 1;
        }
      }
    if (tables.char_sets.count("char_w1") == 0) {
      std::printf("FAIL lv99: char_w1 not loaded\n");
      return 1;
    }
  }
  if (check_lenz(files)) return 1;
  if (check_thrown_weapon_meshes(files)) return 1;
  if (check_ko_pose_picks(files)) return 1;
  {
    // A party member's model follows its kind byte: Serge in Lynx's body is the sel_yam2 model.
    battle::Unit body;
    body.pc = true;
    body.src_id = 0;
    body.kind = 0x31;
    if (ModelCache::unit_model_stem(files, &tables, body).find("sel_yam2") == std::string::npos) {
      std::printf("FAIL kind byte: Serge in Lynx's body has the wrong model\n");
      return 1;
    }
    body.kind = 0;
    if (ModelCache::unit_model_stem(files, &tables, body).find("selju") == std::string::npos) {
      std::printf("FAIL kind byte: Serge's own body has the wrong model\n");
      return 1;
    }
    // Glenn with the Einlanzer pair (kind 0x32): the gren2 model, and the scripts read model number 0x32, not his character id.
    body.src_id = 0x13;
    body.kind = 0x32;
    if (ModelCache::unit_model_stem(files, &tables, body).find("gren2") == std::string::npos || ModelCache::model_number(tables, body) != 0x32) {
      std::printf("FAIL kind byte: Glenn with two swords has the wrong model or model number\n");
      return 1;
    }
    // Dash & Gash's caster script plays the pack's two-sword clip 0x81 for model number 0x32 and the one-sword clip 0x80 for Glenn's own.
    const auto hardhit = files.read("cdrom/battle/effects/tech/hardhit.prd");
    const auto drp = hardhit ? import::parse_drp(*hardhit) : std::nullopt;
    bseq::Table main_table;
    if (drp)
      for (const auto& entry : drp->entries)
        if (entry.type == 0x1A && entry.name == "main") main_table.parse(entry.data);
    for (const int model : {0x13, 0x32}) {
      bseq::Ctx ctx;
      ctx.model_num = model;
      ctx.clip_frames = [](int) { return 8; };
      bseq::Vm vm;
      int pack_clip = -1;
      std::vector<bseq::Event> out;
      if (vm.start(&main_table, 0, 0))
        for (int k = 0; k < 400 && vm.running() && pack_clip < 0; ++k) {
          out.clear();
          vm.tick(ctx, out);
          for (const auto& event : out)
            if (event.kind == bseq::Event::SetAnim && event.a >= 0x80 && pack_clip < 0) pack_clip = event.a;
        }
      if (pack_clip != (model == 0x32 ? 0x81 : 0x80)) {
        std::printf("FAIL Dash & Gash: model %#x plays pack clip %#x\n", model, pack_clip);
        return 1;
      }
    }
  }
  BattleFxLibrary lib;
  const std::set<u32> actor_tags = actor_script_tags(files);
  if (!lib.init(files)) {
    std::printf("FAIL: slot table\n");
    return 1;
  }
  {
    // A caught cast plays the trap pack of its colour: an etrap pack for an element, the colour's trap summon for a summon.
    static const char* const kTrapSummons[6] = {"t53mido1", "tunico", "t59ufo", "tfleaf", "tgolem", "tgfrog"};
    int summons = 0, bad = 0;
    for (const data::ElemRec& rec : tables.elements) {
      const int colour = data::colour_index_of_bit(rec.colour_bit() & 0xFC);
      if (rec.empty() || colour < 0) continue;
      const bool summon = (rec.flags0() & 0x40) != 0;
      summons += summon;
      const int eid = BattleFxLibrary::caught_eid(rec);
      const auto pack = lib.get(eid);
      const bool right_pack = pack && (summon ? pack->slot.prd == std::string("battle/effects/syoukan/") + kTrapSummons[colour] + ".prd" : pack->overlay == "etrap");
      // The jar scene: `test` and its emitters, top-level or in the nested drp `tubo`, register with the pack.
      const bool jar = !summon || (pack && pack->fx.find_sequence("test") && pack->fx.find_emitter("tb"));
      if (eid != (summon ? 203 : 197) + colour || !right_pack || !jar) {
        std::printf("caught eid %d: overlay %s prd %s\n", eid, pack ? pack->overlay.c_str() : "none", pack ? pack->slot.prd.c_str() : "");
        ++bad;
      }
    }
    if (bad || summons != 12 || BattleFxLibrary::caught_eid(tables.elements[39]) != 206) {
      std::printf("FAIL caught casts: %d wrong trap packs, %d summons\n", bad, summons);
      return 1;
    }
  }
  FxScene scene;
  for (int i = 0; i < battle::kUnits; ++i) {
    FxUnit& unit = scene.unit[static_cast<size_t>(i)];
    unit.present = i < 3 || i < 5;
    unit.pc = i < 3;
    unit.x = unit.pc ? -1200.0f + 600.0f * static_cast<float>(i) : 1200.0f - 600.0f * static_cast<float>(i - 3);
    unit.z = 0;
    unit.yaw = unit.pc ? 1.5708f : -1.5708f;
    unit.height = 1000;
  }
  scene.caster = 0;
  scene.targets = {3, 4};
  effects::ViewParams view;
  view.trans = {0, 400, 5500};

  if (std::getenv("BFX_NAMES")) {
    for (int id = 1; id < static_cast<int>(tables.elements.size()); ++id) {
      const data::ElemRec& element = tables.elements[static_cast<size_t>(id)];
      if (!element.empty())
        std::printf("%d=%s eid %d%s\n", id, tables.element_name(id).c_str(), BattleFxLibrary::element_eid(element), (element.flags0() & 0x40) ? " (consumable)" : "");
      if (!element.empty())
        if (const auto* sl = lib.slot(BattleFxLibrary::element_eid(element))) std::printf("    %s / %s\n", sl->prd.c_str(), sl->prg.c_str());
    }
    return 0;
  }
  // BFX_DIAG=<element id> prints what the pack holds; BFX_EID gives the effect id itself.
  if (const char* diag = std::getenv("BFX_DIAG")) {
    const int id = std::atoi(diag);
    const int eid = std::getenv("BFX_EID") ? std::atoi(std::getenv("BFX_EID")) : BattleFxLibrary::element_eid(tables.elements[static_cast<size_t>(id)]);
    auto pack = lib.get(eid);
    std::printf("element %d eid %d: %s\n", id, eid, pack ? pack->slot.prd.c_str() : "(no pack)");
    if (const auto* sl = lib.slot(eid)) std::printf("  slot ctd '%s' prd '%s' prg '%s'\n", sl->ctd.c_str(), sl->prd.c_str(), sl->prg.c_str());
    if (pack) {
      if (auto drp = import::parse_drp(pack->prd)) {
        std::printf("  entries:");
        for (const auto& entry : drp->entries) std::printf(" %s/%02X(%zu)", entry.name.c_str(), entry.type, entry.data.size());
        std::printf("\n");
      }
      std::printf("  sequences:");
      for (const auto& sequence : pack->fx.sequences) std::printf(" %s(%zu)", sequence.name.c_str(), sequence.cmds.size());
      // BFX_SEQ: every command of every sequence (op, wait, data words).
      if (std::getenv("BFX_SEQ"))
        for (const auto& sequence : pack->fx.sequences) {
          std::printf("\n  seq %s:", sequence.name.c_str());
          for (const auto& cmd : sequence.cmds) {
            std::printf("\n    %03X w%d '%s' len %d:", cmd.op, cmd.wait, cmd.name.c_str(), cmd.len);
            for (size_t i = 0; i + 1 < cmd.data.size(); i += 2) std::printf(" %d", static_cast<short>(cmd.data[i] | (cmd.data[i + 1] << 8)));
          }
        }
      // BFX_EMIT: the ops of the emitters named (exact name match).
      if (const char* en = std::getenv("BFX_EMIT"))
        for (const auto& em : pack->fx.emitters)
          if (em.name == en) {
            std::printf("\n  emitter %s flags %08X start %d dur %d period %u repeat %d:", em.name.c_str(), em.flags, em.start, em.duration, em.period, em.repeat);
            for (const auto& effect_op : em.ops) std::printf(" %02X", effect_op.op);
          }
      std::printf("\n  emitters %zu, sheets %zu, blobs %zu, errors %zu\n", pack->fx.emitters.size(), pack->fx.sheets.size(), pack->fx.blobs.size(),
                  pack->fx.errors.size());
      for (const auto& em : pack->fx.emitters)
        for (const auto& effect_op : em.ops)
          if (effect_op.op == 5) {
            const auto& sprite = std::get<assets::FxSprite>(effect_op.arg);
            std::printf("    emitter %s sprite type %d name '%s' frame0 %d\n", em.name.c_str(), sprite.type, sprite.name.c_str(), sprite.frame0);
          }
      for (const auto& em : pack->fx.emitters)
        for (const auto& effect_op : em.ops)
          if (effect_op.op == 0x39) {
            const auto& basis = std::get<assets::FxBasis>(effect_op.arg);
            std::printf("    emitter %s basis a=%d b=%d c=%d d=%d flags=%x\n", em.name.c_str(), basis.a, basis.b, basis.c, basis.d, basis.flags);
          }
      for (const auto& error : pack->fx.errors) std::printf("    error: %s\n", error.c_str());
      for (const auto& sh : pack->fx.sheets) {
        size_t sp = 0, vis = 0;
        for (const auto& fr : sh.frames)
          for (const auto& sprite : fr) {
            ++sp;
            for (size_t i = 3; i < sprite.rgba.size(); i += 4)
              if (sprite.rgba[i]) {
                ++vis;
                break;
              }
          }
        std::printf("    sheet %s: %zu sprites, %zu with visible texels\n", sh.name.c_str(), sp, vis);
      }
      if (const auto* sl = lib.slot(eid)) {
        Bytes ctd;
        if (!sl->ctd.empty()) ctd = files.read("cdrom/" + sl->ctd).value_or(Bytes{});
        std::string serr;
        auto st = import::load_stage(pack->prd, ctd, &serr);
        if (!st) std::printf("  load_stage failed: %s\n", serr.c_str());
        else {
          std::printf("  stage: %zu parts, %zu textures\n", st->parts.size(), st->textures.size());
          for (const auto& pt : st->parts) std::printf("    part %s verts %zu tris %zu subs %zu\n", pt.name.c_str(), pt.vertices.size(), pt.indices.size() / 3, pt.submeshes.size());
        }
      }
      if (std::getenv("BFX_RUN")) {
        BattleFxRun run(pack, lib.particle_lib(), 1);
        run.start(scene, view);
        for (int tick = 0; tick < 200; ++tick) {
          run.tick(scene);
          if (tick % 10 == 0) {
            const auto st = run.emitters().stats();
            FxFrame frame;
            run.collect(view, 0.5f, frame);
            std::printf("  t=%d seq=%zu live=%u running=%u spawned=%llu dropped=%llu unresolved=%u mesh=%u quads=%zu done=%d\n", tick,
                        run.sequences_active(), st.live, st.running, static_cast<unsigned long long>(st.spawned),
                        static_cast<unsigned long long>(st.dropped), st.unresolved_sprites, st.mesh_objects, frame.quads.size(), run.done());
          }
        }
      }
    }
    return 0;
  }
  // 155slot (eid 150) draws its three reel meshes itself as TexSrc::Vram primitives (the scrolling VRAM window), and the stock
  // draw of those emitters is off.
  if (auto pack = lib.get(150)) {
    BattleFxRun run(pack, lib.particle_lib(), 1);
    run.start(scene, view);
    size_t vram = 0;
    for (int tick = 0; tick < 400; ++tick) {
      run.tick(scene);
      if (tick > 20 && tick % 20 == 0) {
        FxFrame frame;
        run.collect(view, 0.5f, frame);
        for (const auto& prim : frame.textured) vram += prim.src == effects::FxPrim::TexSrc::Vram;
      }
    }
    if (vram == 0) {
      std::printf("FAIL: 155slot drew no Vram reel primitives\n");
      return 1;
    }
  }
  // Luminaire (eid 223): every sprite of its 'renz' sheet takes its 4x image from RDE's page, no other sheet does.
  if (auto pack = lib.get(223)) {
    size_t renz = 0, filled = 0, others = 0;
    for (const auto& sheet : pack->fx.sheets)
      for (const auto& frame : sheet.frames)
        for (const auto& sprite : frame) {
          const bool hd = sprite.hd_rgba.size() == static_cast<size_t>(sprite.w) * sprite.h * 16 * 4;
          if (sheet.name == "renz") ++renz, filled += hd;
          else others += !sprite.hd_rgba.empty();
        }
    if (renz == 0 || filled != renz || others != 0 || pack->hd_sprites != renz) {
      std::printf("FAIL: shine renz HD page: %zu sprites, %zu with HD, %zu others, hd_sprites %zu\n", renz, filled, others, pack->hd_sprites);
      return 1;
    }
    if (pack->hd_texels) {
      std::printf("FAIL: Luminaire has HD texels\n");
      return 1;
    }
  }
  // Canon Cannon (eid 462): RDE's 4x face expressions over the 48 x 96 halfword rect at (0x2C0, 0x100) the anim ops 0xA2 - 0xA5 copy from.
  if (auto pack = lib.get(462)) {
    const auto& texels = pack->hd_texels;
    if (!texels || texels->x != 0x2C0 || texels->y != 0x100 || !texels->indices.indexed || texels->indices.width != 48 * 2 * 4 ||
        texels->indices.height != 96 * 4) {
      std::printf("FAIL: Canon Cannon HD face texels missing or misplaced\n");
      return 1;
    }
  }
  // The lit model path: the light matrix from the slot table, and 9boll (eid 38: op 0x66, the 0x65 ambient ramp
  // (0x50, 0x3C, 0x3E) over 35 ticks, type-17 lights 'ligt' in slots) writing effects::LightTable through the emitter system.
  int lit_failures = 0;
  {
    effects::LightTable lights;
    lights.slot[0].pos[0] = 1000;
    lights.slot[0].intensity = 4096;
    lights.slot[0].rgb[0] = 0x80;
    lights.slot[1].pos[2] = -500;
    lights.slot[1].intensity = 4096;
    // k = 4096 - 500 * 4096 / 4096 = 3596.
    lights.slot[1].falloff = 4096;
    lights.slot[2].pos[1] = 100;
    lights.slot[2].intensity = 100;
    // k = 100 - (100 * 0x7FFF >> 12 = 799) < 0 -> 0.
    lights.slot[2].falloff = 0x7FFF;
    const effects::LightMatrices battle_matrices = effects::light_matrices(lights, {0, 0, 0}, false);
    auto near = [](float lhs, float rhs) { return std::fabs(lhs - rhs) < 1e-3f; };
    if (!near(battle_matrices.dir[0][0], 1.0f) || !near(battle_matrices.dir[0][1], 0.0f) || !near(battle_matrices.rgb[0][0], 0.5f) || !near(battle_matrices.rgb[0][1], 0.0f)) ++lit_failures;
    if (!near(battle_matrices.dir[1][2], -3596.0f / 4096.0f) || !near(battle_matrices.dir[1][0], 0.0f)) ++lit_failures;
    if (!near(battle_matrices.dir[2][1], 0.0f)) ++lit_failures;
    // Field: falloff << 3 -> k = 4096 - 4000 = 96.
    const effects::LightMatrices field_matrices = effects::light_matrices(lights, {0, 0, 0}, true);
    if (!near(field_matrices.dir[1][2], -96.0f / 4096.0f)) ++lit_failures;
    if (lit_failures) std::printf("FAIL: light_matrices\n");
  }
  if (auto pack = lib.get(38)) {
    effects::LightTable lights;
    BattleFxRun run(pack, lib.particle_lib(), 1);
    int amb_r = -1, amb_g = -1, amb_b = -1, amb_ticks = -1, amb_calls = 0, zero_ops = 0;
    BattleFxRun::Callbacks cb;
    cb.ambient = [&](int r, int g, int b, int ticks) {
      if (amb_calls++ == 0) amb_r = r, amb_g = g, amb_b = b, amb_ticks = ticks;
    };
    cb.other = [&](const assets::SeqCmd& cmd) { zero_ops += cmd.op == 0x66; };
    run.set_callbacks(cb);
    run.set_lights(&lights);
    run.start(scene, view);
    int lit_tick = -1, slots_used = 0;
    for (int tk = 0; tk < 400 && !run.done(); ++tk) {
      run.tick(scene);
      int used = 0;
      for (const auto& light : lights.slot) used += light.intensity != 0;
      if (used && lit_tick < 0) lit_tick = tk;
      slots_used = std::max(slots_used, used);
    }
    const bool ok = amb_calls >= 1 && amb_r == 0x50 && amb_g == 0x3C && amb_b == 0x3E && amb_ticks == 35 && zero_ops == 1 && lit_tick >= 0 &&
                    slots_used >= 2 && BattleFxRun::presented(0x65, 16) && BattleFxRun::presented(0x66, 8);
    if (!ok) {
      std::printf("FAIL: 9boll lit path: ambient calls %d first (%d %d %d over %d) op66 %d first light tick %d slots %d\n", amb_calls, amb_r,
                  amb_g, amb_b, amb_ticks, zero_ops, lit_tick, slots_used);
      ++lit_failures;
    }
  } else {
    std::printf("FAIL: 9boll (eid 38) missing\n");
    ++lit_failures;
  }
  // The six cast charge packs (battle.prd > sato > ch00..ch05): each loads, draws, and hands over with op 0x41, whose waits sum to 108 ticks.
  int charge_ok = 0;
  for (int colour = 0; colour < 6; ++colour) {
    auto pack = lib.charge_pack(colour);
    if (!pack) {
      std::printf("FAIL: charge pack %d missing\n", colour);
      continue;
    }
    BattleFxRun run(pack, lib.particle_lib(), 1);
    int ready_tick = -1, cams = 0;
    size_t drawn = 0;
    BattleFxRun::Callbacks cb;
    cb.camera = [&](std::string_view) { ++cams; };
    cb.ready = [&] { ready_tick = run.ticks(); };
    run.set_callbacks(cb);
    run.start(scene, view);
    for (int tick = 0; tick < 140; ++tick) {
      run.tick(scene);
      FxFrame frame;
      run.collect(view, 0.5f, frame);
      std::vector<effects::EmitterSystem::MeshInstance> mi;
      run.collect_meshes(view, 0.5f, mi);
      drawn += frame.quads.size() + mi.size();
    }
    const auto st = run.emitters().stats();
    // The start's own step pays one tick of the waits, so op 0x41 fires in sequence tick 107 (ticks() counts completed ticks).
    const bool ok = ready_tick == 106 && cams >= 1 && drawn > 0 && st.unresolved_sprites == 0;
    if (std::getenv("BFX_CHARGE") || !ok)
      std::printf("charge %s: sheets %zu (+%zu shared) emitters %zu ready at tick %d cameras %d drawn %zu spawned %llu unresolved %u mesh %u%s\n",
                  pack->start_seq.c_str(), pack->fx.sheets.size(), pack->extra ? pack->extra->sheets.size() : size_t{0}, pack->fx.emitters.size(), ready_tick,
                  cams, drawn, static_cast<unsigned long long>(st.spawned), st.unresolved_sprites, st.mesh_objects, ok ? "" : "  FAIL");
    charge_ok += ok;
  }
  int with_sheets = 0, with_meshes = 0, failures = lit_failures, ran = 0, nopack = 0, bespoke = 0, damage_ops = 0, consumable = 0;
  if (charge_ok != 6) ++failures;
  std::vector<int> blank, nopack_list;
  std::vector<std::string> caster_react, react_no_number;
  std::set<int> seen_eids;
  const int element_count = static_cast<int>(tables.elements.size());
  for (int id = 1; id < element_count; ++id) {
    const data::ElemRec& rec = tables.elements[static_cast<size_t>(id)];
    if (rec.empty()) continue;
    const int eid = BattleFxLibrary::element_eid(rec);
    // The view uses the generic path for consumables; the pack is still run below.
    if (rec.flags0() & 0x40) ++consumable;
    auto pack = lib.get(eid);
    if (!pack) {
      ++nopack;
      nopack_list.push_back(id);
      continue;
    }
    try {
      BattleFxRun run(pack, lib.particle_lib(), 1);
      int cams = 0, snds = 0, dmg = 0;
      BattleFxRun::Callbacks cb;
      cb.camera = [&](std::string_view) { ++cams; };
      cb.sound = [&](std::string_view, int, int) { ++snds; };
      cb.damage = [&](int) { ++dmg; };
      run.set_callbacks(cb);
      run.start(scene, view);
      // The sequences the actors' anmseq scripts start in battle (ops 0x68 / 0x7D).
      run.start_script_sequences(actor_tags);
      size_t drawn = 0;
      FxFrame frame;
      for (int tick = 0; tick < 600; ++tick) {
        run.tick(scene);
        if (tick % 10 == 0) {
          run.collect(view, 0.5f, frame);
          std::vector<effects::EmitterSystem::MeshInstance> mi;
          run.collect_meshes(view, 0.5f, mi);
          // Count the mesh instances whose mesh exists in the pack.
          size_t placed = 0;
          for (const auto& instance : mi)
            if (pack->meshes)
              for (const auto& part : pack->meshes->parts)
                if (part.name == *instance.name) {
                  ++placed;
                  break;
                }
          drawn += frame.quads.size() + frame.flats.size() + placed;
          for (const auto& quad : frame.quads)
            if (quad.slot < 0 || quad.slot > 0xFFF) throw std::runtime_error("slot out of range");
        }
        if (run.done()) break;
      }
      ++ran;
      with_sheets += !pack->fx.sheets.empty();
      with_meshes += pack->meshes != nullptr;
      bespoke += pack->bespoke != nullptr;
      damage_ops += pack->has_damage_op;
      // The hit comes from a sequence-started caster script's React (no generic hit at the pack start).
      if (pack->caster_script_reacts && !pack->has_damage_op && !pack->has_react_op && seen_eids.count(eid) == 0)
        caster_react.push_back(std::to_string(id) + "/" + pack->overlay);
      // The reaction ends without CallDamage and nothing else calls it -> the React lands the hit without a number (Toxic Burst 387).
      if (pack->reaction_lands_no_number && seen_eids.count(eid) == 0) react_no_number.push_back(std::to_string(id) + "/" + pack->overlay);
      if (seen_eids.insert(eid).second && drawn == 0) blank.push_back(id);
    } catch (const std::exception& ex) {
      std::printf("FAIL element %d eid %d (%s): %s\n", id, eid, pack->slot.prd.c_str(), ex.what());
      ++failures;
    }
  }
  std::printf("battle fx: %d elements ran (%d with bespoke module, %d with a damage op), %d without a pack, %d consumable, %d failures\n", ran,
              bespoke, damage_ops, nopack, consumable, failures);
  std::printf("packs with sprite sheets: %d, packs with effect meshes: %d\n", with_sheets, with_meshes);
  std::printf("no pack:");
  for (int id : nopack_list) std::printf(" %d", id);
  std::printf("\nnothing drawn in 600 ticks (per distinct eid):");
  for (int id : blank) std::printf(" %d", id);
  std::printf("\nhit from a sequence-started caster script's React (element/overlay, per distinct eid):");
  for (const std::string& entry : caster_react) std::printf(" %s", entry.c_str());
  std::printf("\nreaction ends without CallDamage = React lands without a number (element/overlay):");
  for (const std::string& entry : react_no_number) std::printf(" %s", entry.c_str());
  std::printf("\n");
  // Potpourri's Toxic Burst (387, jibaku.prd): variant 7 = Done only; the retail fight shows no number.
  if (std::none_of(react_no_number.begin(), react_no_number.end(), [](const std::string& entry) { return entry.rfind("387/", 0) == 0; })) {
    std::printf("FAIL: element 387's reaction should land no number\n");
    ++failures;
  }
  // Bomber's Suicide Bomb (348, bombom.prd): `main` op 0x45 -> entry 0, t290 React(-1).
  if (std::none_of(caster_react.begin(), caster_react.end(), [](const std::string& entry) { return entry.rfind("348/", 0) == 0; })) {
    std::printf("FAIL: element 348 does not take its hit from the caster script's React\n");
    ++failures;
  }
  // Script-only packs (a type-0x1A 'main', no 'main' sequence, no emitter: kill, comebigi, konsin, bre_roza) play: the host runs the table.
  for (const int eid : {307, 493, 452, 144}) {
    const auto pack = lib.get(eid);
    if (!pack || !pack->tables.count("main")) {
      std::printf("FAIL: script-only pack eid %d not played\n", eid);
      ++failures;
    }
  }
  // Rumble and the music fade are carried out; the continuation hold ops and the bttim reload have nothing to do.
  for (const int op : {0x28, 0x29, 0x54})
    if (!BattleFxRun::presented(op, 12)) {
      std::printf("FAIL: op %02X not carried out\n", op);
      ++failures;
    }
  // lasdes3 (eid 537, 'main' 2240 ticks) ends by its sequences, not by the safety cap.
  if (const auto pack = lib.get(537)) {
    BattleFxRun run(pack, lib.particle_lib(), 1);
    run.start(scene, view);
    int ticks = 0;
    while (!run.done() && ticks < BattleFxRun::kTickCap + 2) {
      run.tick(scene);
      ++ticks;
    }
    if (run.capped() || ticks < 2240) {
      std::printf("FAIL: lasdes3 ended at tick %d (capped %d)\n", ticks, run.capped());
      ++failures;
    }
  } else {
    std::printf("FAIL: lasdes3 (eid 537) missing\n");
    ++failures;
  }
  if (ran < 400) {
    std::printf("FAIL: only %d packs ran\n", ran);
    ++failures;
  }
  return failures == 0 ? 0 : 1;
}
