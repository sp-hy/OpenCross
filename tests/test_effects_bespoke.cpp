// Tests for the bespoke battle-effect overlays (src/game/effects/bespoke): registry, per-family behaviour, determinism, soak.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/effects/bespoke/bespoke.h"
#include "game/effects/soft_raster.h"

using namespace oc;
using namespace oc::effects;

namespace {
int failures = 0;
#define CHECK(condition)                                              \
  do {                                                                \
    if (!(condition)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

std::vector<FxPrim> frame(BespokeEffect& effect) {
  std::vector<FxPrim> prims;
  effect.draw(prims);
  sort_prims(prims);
  return prims;
}

void test_registry() {
  // Every pack's type-0x0B 'act0' Mdl is pseudo slot A; tumaru binds it at "e297".
  CHECK(fx_slot_a_model("act0", 0x0B) && !fx_slot_a_model("act1", 0x0B) && !fx_slot_a_model("act0", 0x1A));
  CHECK(std::string(fx_pack_model_binding("act0", "tumaru")) == "e297");
  auto& registry = BespokeRegistry::instance();
  std::set<std::string> fam[4];
  const char* names[4] = {"band-wipe", "line-ring", "death-dissolve", "mesh-deform"};
  for (const auto* info : registry.all())
    for (int k = 0; k < 4; ++k)
      if (info->family == names[k]) fam[k].insert(info->name);
  CHECK(fam[1].size() == 4);
  CHECK(fam[2].size() == 11);
  CHECK(fam[0].size() >= 9);
  // wipeoff tekken jitanda 2boll saraman left the mesh-deform stand-ins for the ground ports (ground_ports.cpp).
  // Every overlay stays registered while the ports take the stand-ins' places (the mesh-deform family shrinks).
  CHECK(registry.all().size() >= 89);
  for (const char* name : {"wipeoff", "tekken", "jitanda", "2boll", "saraman"}) CHECK(registry.find(name) != nullptr && registry.find(name)->family == "ground");
  // Count the ported families.
  size_t ports = 0;
  for (const auto* info : registry.all())
    ports += info->family == "model-steer" || info->family == "model-deform" || info->family == "emitter-patch" || info->family == "summon" ||
             info->family == "ground" || info->family == "shell";
  CHECK(ports >= 19);
  for (const char* name : {"canon", "jigoku", "malchera", "monfish", "lasdes2", "bossde", "tfleaf", "ripple", "saraman"}) CHECK(registry.find(name) != nullptr);
  CHECK(registry.find("battle/effects/black/44get.prg") != nullptr);
  CHECK(registry.find("get") == registry.find("44get"));
  CHECK(registry.find("nope") == nullptr);
  EffectSlot slot;
  slot.eid = 77;
  slot.prg = "battle/effects/tech/canon.prg";
  CHECK(registry.bind_slots({slot}) == 1);
  CHECK(registry.find_element(77) == registry.find("canon"));
  CHECK(registry.find_element(78) == nullptr);
}

// hebi: the third party slot is off the stage from init; 0x101 starts the model's reload, which holds the effect's end.
void test_third_slot_reload() {
  CHECK(disc_read_ticks(72416) == 15 && disc_read_ticks(54144) == 11 && disc_read_ticks(31492) == 7);
  DummyStage st;
  FxModels ms;
  ms.units.resize(4);
  ms.units[2].pc = true;
  st.ctx.models = &ms;
  auto effect = BespokeRegistry::instance().create("hebi", st.ctx);
  CHECK(effect != nullptr);
  if (!effect) return;
  CHECK(ms.units[2].hidden);
  CHECK(effect->done());
  effect->command(0x101, nullptr, 0);
  for (int i = 0; i < 26; ++i) {
    effect->tick();
    CHECK(ms.units[2].hidden && !ms.units[2].rebuilt && !effect->done());
  }
  effect->tick();
  CHECK(!ms.units[2].hidden && ms.units[2].rebuilt && effect->done());
}

void test_negative_strip() {
  DummyStage st;
  st.ctx.duration = 5;
  auto effect = BespokeRegistry::instance().create("canon", st.ctx);
  // Nothing is drawn until the script op 0x100 (operand = ticks) starts the task.
  CHECK(frame(*effect).empty());
  CHECK(effect->done());
  const int a0[1] = {5};
  effect->command(0x100, a0, 1);
  auto prims = frame(*effect);
  CHECK(prims.size() == 5);
  float cover = 0;
  for (const auto& prim : prims) {
    CHECK(prim.blend == kBlendInvert);
    cover += prim.v[3].x - prim.v[0].x;
  }
  CHECK(std::fabs(cover - 320.0f) < 0.01f);
  SoftCanvas canvas(320, 240);
  canvas.rgba()[0] = 10;
  raster_prims(canvas, prims);
  CHECK(canvas.rgba()[0] == 245);
  int ticks = 0;
  while (!effect->done() && ticks < 100) {
    effect->tick();
    ++ticks;
  }
  // The countdown goes 5 -> 0 -> negative on the sixth tick.
  CHECK(ticks == 6);
  CHECK(frame(*effect).empty());
}

// muteki: the task draws only when the pack has the 'renz' sheet (type 0x15); the dummy stage has none: nothing at all
// lasdes2's scene ops: the song changes, the camera cross-fade words, the tint lerp and the eye-frame copy.
void test_lasdes2() {
  DummyStage st;
  FxModels ms;
  ms.units.resize(4);
  ms.targets = {3};
  st.ctx.models = &ms;
  std::vector<int> songs;
  st.ctx.hooks.play_song = [&songs](int song) { songs.push_back(song); };
  auto effect = BespokeRegistry::instance().create("lasdes2", st.ctx);
  CHECK(effect != nullptr);
  if (!effect) return;
  std::vector<int> words = {0, 0, 0x42};
  effect->command(0x101, words.data(), static_cast<int>(words.size()));
  effect->command(0x103, words.data(), static_cast<int>(words.size()));
  CHECK(songs == std::vector<int>({0x3B, 0x42}));
  words = {4, 0, 's' | ('t' << 8), '0' | ('1' << 8)};
  effect->command(0x104, words.data(), static_cast<int>(words.size()));
  CHECK(ms.cross.shot == "st01" && ms.cross.serial == 1);
  effect->tick();
  CHECK(ms.cross.alpha == 32);
  for (int i = 0; i < 4; ++i) effect->tick();
  CHECK(ms.cross.alpha == 128 && ms.cross.fill == 0);
  words = {2};
  effect->command(0x106, words.data(), static_cast<int>(words.size()));
  effect->tick();
  CHECK(ms.cross.fill == 127 && !ms.cross.promote);
  effect->tick();
  effect->tick();
  CHECK(ms.cross.fill == 255 && ms.cross.promote);
  words = {2, 0, 2 | static_cast<int>(static_cast<s16>(0xFF00)), 128};
  effect->command(0x10D, words.data(), static_cast<int>(words.size()));
  CHECK(ms.tint.mode == 2);
  effect->tick();
  effect->tick();
  CHECK(ms.tint.rgb[0] == 127 && ms.tint.rgb[1] == 64 && ms.tint.rgb[2] == 0);
  effect->tick();
  CHECK(ms.tint.rgb[0] == 255 && ms.tint.rgb[1] == 128);
  words = {1, 0, 2};
  effect->command(0x10C, words.data(), static_cast<int>(words.size()));
  CHECK(ms.vram_moves.size() == 1 && ms.vram_moves[0].x == 352 && ms.vram_moves[0].y == 256 && ms.vram_moves[0].dx == 0x3C4 && ms.vram_moves[0].dy == 0x32);
  CHECK(effect->done());
}

void test_muteki_falloff() {
  DummyStage st;
  auto effect = BespokeRegistry::instance().create("muteki", st.ctx);
  std::vector<int> words = {17, 0, 160, 112, 0, 0, 160, 160, 160, 0};
  effect->command(0x180, words.data(), static_cast<int>(words.size()));
  effect->tick();
  CHECK(frame(*effect).empty());
}

}  // namespace
namespace {
// A host whose skin callback returns two quads of one part at x 900..1100, y -500..0, z 0: face 0 front-facing, face 1 its mirror (back).
void fake_skin(FxModels& ms) {
  ms.skin = [](int unit, FxSkinnedModel& out) {
    out = {};
    out.parts = 1;
    out.shading[0] = {128, 128, 128, 0};
    out.verts = {{900, -500, 0}, {1100, -500, 0}, {900, 0, 0}, {1100, 0, 0}};
    out.normals = {{0, 0, -127}, {0, 0, -127}, {64, 0, -100}, {64, 0, -100}};
    FxSkinFace front;
    front.n = 4;
    for (int k = 0; k < 4; ++k) front.v[k] = static_cast<u32>(k), front.u[k] = k & 1 ? 1.0f : 0.0f, front.w[k] = k & 2 ? 1.0f : 0.0f;
    front.tex = 0;
    out.faces.push_back(front);
    FxSkinFace back = front;
    std::swap(back.v[0], back.v[1]), std::swap(back.v[2], back.v[3]);
    out.faces.push_back(back);
    out.pos = {1000, 0, 0};
    return unit >= 0 || unit == kPseudoUnit;
  };
  ms.attach = [](int, int, Vec3& out) {
    out = {1000, -400, 0};
    return true;
  };
  ms.skin_pack = [&ms](FxSkinnedModel& out) { return ms.skin(0, out); };
}
}  // namespace
namespace {

// The line rings (malchera 5ecc / 6000 / 691c / 6b58 ...): 0x180 hangs 60 points at the attach point, 0x181 sets g / rest; 59 grey LineG2,
// the colour continuous along the chain, every link exactly `rest` long once hung.
void test_ring();
void test_dissolve();
void test_determinism_and_soak() {
  for (const auto* info : BespokeRegistry::instance().all()) {
    std::vector<std::vector<float>> runs[2];
    for (int run = 0; run < 2; ++run) {
      DummyStage st;
      auto effect = BespokeRegistry::instance().create(info->name, st.ctx);
      for (int tick = 0; tick < 300; ++tick) {
        effect->tick();
        if (tick % 25 == 0) {
          std::vector<FxPrim> prims;
          effect->draw(prims);
          std::vector<float> flat;
          for (const auto& prim : prims) {
            CHECK(prim.slot >= 0 && prim.slot <= 0xFFF);
            for (int k = 0; k < 4; ++k) {
              CHECK(std::isfinite(prim.v[k].x) && std::isfinite(prim.v[k].y));
              flat.push_back(prim.v[k].x);
              flat.push_back(prim.v[k].y);
            }
          }
          runs[run].push_back(flat);
        }
      }
    }
    CHECK(runs[0] == runs[1]);
    if (runs[0] != runs[1]) std::printf("  nondeterministic: %s\n", info->name.c_str());
  }
}


// ---- the ports on a host-like model list -----------------------------------------------------------
// Two units: the caster (unit 0, a PC at x -1000) and the target (unit 1, a monster at x +1000), both on the ground (y 0); camera eye at
// (0, -1000, -6000) looking down +z.
struct Host {
  FxModels ms;
  DummyStage st;
  Host() {
    ms.units.resize(2);
    for (int i = 0; i < 2; ++i) {
      FxModel& model = ms.units[static_cast<size_t>(i)];
      model.unit = i;
      model.pc = i == 0;
      model.home = model.pos = {i == 0 ? -1000.0f : 1000.0f, 0, 0};
      model.save_prev();
    }
    ms.casters = {0};
    ms.targets = {1};
    st.ctx.models = &ms;
    st.ctx.view.rot = Mat3::identity();
    st.ctx.view.trans = {0, 1000, 6000};
  }
  FxModel& tgt() { return ms.units[1]; }
  std::unique_ptr<BespokeEffect> make(const char* name) { return BespokeRegistry::instance().create(name, st.ctx); }
  void tick(BespokeEffect& effect, int count) {
    for (int step = 0; step < count; ++step) {
      for (auto& model : ms.units) model.save_prev();
      effect.tick();
    }
  }
};
// Sends command `op` with `words` (the record from +4 on).
void cmd(BespokeEffect& effect, int op, std::initializer_list<int> words) {
  std::vector<int> record(words);
  effect.command(op, record.data(), static_cast<int>(record.size()));
}

void test_actor_scripts() {
  // 41grav: pushed off the caster, flies to the camera eye, falls back home, 'dead' reaction requested
  {
    Host host;
    auto effect = host.make("41grav");
    cmd(*effect, 0x180, {9, 0, 4, 0});
    host.tick(*effect, 10);
    CHECK(host.tgt().steer && host.tgt().pos[0] > 1000.0f);
    CHECK(host.tgt().clip == 5);
    cmd(*effect, 0x181, {10, 0, 160, 0});
    host.tick(*effect, 20);
    // Toward the eye at z -6000, shadow off.
    CHECK(host.tgt().pos[2] < -1000.0f && !(host.tgt().flags & 0x200));
    cmd(*effect, 0x183, {25, 0});
    host.tick(*effect, 30);
    CHECK(std::fabs(host.tgt().pos[1]) < 1e-3f && std::fabs(host.tgt().pos[0] - 1000.0f) < 1e-3f && (host.tgt().flags & 0x200));
    CHECK(host.tgt().script_table == "dead" && host.tgt().script_entry == 0x80 && host.tgt().script_variant == 7);
  }
  // the trap summons' extra pose step: K / 256 keyframes per tick for t = 1..D (tgolem 64, tgfrog 32, tfleaf 4)
  {
    for (auto [name, keyframes] : {std::pair<const char*, int>{"tgolem", 64}, {"tgfrog", 32}, {"tfleaf", 4}, {"tunico", 64}}) {
      Host host;
      auto effect = host.make(name);
      cmd(*effect, 0x100, {10, 0, 1000, 100});
      host.tick(*effect, 15);
      CHECK(std::fabs(host.ms.pack_extra_frames - 10.0f * static_cast<float>(keyframes) / 256.0f) < 1e-4f);
      CHECK(host.ms.pack_model_hidden);
    }
  }
  // lasbos (Portalgheist's 0x167 = lasbos5): op 0x106 spirals the target into the axis while it flies up and tumbles, op 0x107 brings it home facing the caster
  {
    Host host;
    auto& registry = BespokeRegistry::instance();
    CHECK(registry.find("lasbos") != nullptr && registry.find("lasbos")->family == "ground");
    EffectSlot slot;
    slot.eid = 359;
    slot.prg = "prog/lasbos.prg";
    CHECK(registry.bind_slots({slot}) == 1 && registry.find_element(359) == registry.find("lasbos"));
    auto effect = host.make("lasbos");
    // The data of lasbos5 'guru'.
    cmd(*effect, 0x106, {245, 0, 33, 3, 34, 70, 12, 17, 17, 13, 186, 0});
    CHECK(host.tgt().clip == 8 && host.tgt().clip_flags == 13 && host.tgt().steer);
    const float r0 = std::fabs(host.tgt().pos[0]);
    host.tick(*effect, 60);
    const float r60 = std::hypot(host.tgt().pos[0], host.tgt().pos[2]);
    CHECK(r60 < r0 && r60 > 0.0f && host.tgt().pos[1] < -100.0f && host.tgt().rot[0] != 0);
    host.tick(*effect, 190);
    // At the axis, far above the floor.
    CHECK(std::hypot(host.tgt().pos[0], host.tgt().pos[2]) < 1.0f && host.tgt().pos[1] < -5000.0f);
    cmd(*effect, 0x107, {31, 0, 0, 0});
    host.tick(*effect, 40);
    CHECK(host.tgt().pos[0] == 1000.0f && host.tgt().pos[1] == 0.0f && host.tgt().pos[2] == 0.0f);
    CHECK(host.tgt().rot[0] == 0 && host.tgt().rot[2] == 0 && host.tgt().rot[1] == static_cast<short>(-0x400 - ratan2(0, -2000)));
    CHECK(effect->done());
  }
  // lasbos stage swap: 0x101 w = @+8 frees the stage and reads slot w + 1's .prd, 0x102 starts it (lasbos4 357 -> bg_89 = 358)
  {
    Host host;
    std::vector<int> stages;
    host.st.ctx.hooks.set_stage = [&](int stage) { stages.push_back(stage); };
    auto effect = host.make("lasbos");
    cmd(*effect, 0x100, {2, 0, 357, 0});
    CHECK(stages.empty());
    cmd(*effect, 0x101, {0, 0, 357, 0});
    cmd(*effect, 0x102, {2, 0, 0, 0});
    // A second 0x102 re-registers nothing new.
    cmd(*effect, 0x102, {2, 0, 0, 0});
    CHECK(stages.size() == 2 && stages[0] == -1 && stages[1] == kStageSlot + 358);
  }
  // opthedo = element 0x167, the Portalgheist door's summon (the pack `out` sequence runs op 0x101 D 72 v -592 on the cast target): the
  // target is put at the caster and slides v / 16 units a tick along the caster's facing for D + 1 ticks drawn as a fading ghost (custom draw, the stock
  // pass hidden), then it is revealed, released to the host (which hands the placement to the unit) and its own S5 entry 3 starts
  {
    Host host;
    host.tgt().offstage = true;
    // The door is turned toward the pup's spot (yaw 3584 = -512 = 45 degrees; the cast-start facing).
    host.ms.units[0].rot[1] = 3584;
    host.tgt().home = {-1000.0f + 1536.0f, 0, -1536.0f};
    auto effect = host.make("opthedo");
    cmd(*effect, 0x101, {72, 0, -592, 0});
    // At the door; the first drawn frame has colour (alpha) 0.
    CHECK(host.tgt().steer && std::fabs(host.tgt().pos[0] + 1000.0f) < 1.0f && host.tgt().pos[2] == 0.0f && !host.tgt().reveal);
    CHECK(host.tgt().tinted && host.tgt().rgb[0] == 0 && host.tgt().rgb[1] == 0 && host.tgt().rgb[2] == 0);
    host.tick(*effect, 11);
    // Sliding along the facing, ghost-drawn, not yet shown, and fading in.
    CHECK(host.tgt().pos[2] < -200.0f && host.tgt().pos[2] > -400.0f && host.tgt().pos[0] > -800.0f && host.tgt().custom_draw && !host.tgt().reveal && !host.tgt().release);
    CHECK(host.tgt().rgb[0] > 0 && host.tgt().rgb[0] < 20);
    host.tick(*effect, 50);
    CHECK(host.tgt().pos[2] < -1500.0f && !host.tgt().reveal);
    host.tick(*effect, 20);
    CHECK(host.tgt().reveal && host.tgt().release && host.tgt().script_entry == 3 && !host.tgt().custom_draw && !host.tgt().hidden);
    // 73 steps of 37 units along the facing = door + 1.2435 x (home - door) (retail: (-909, -2168) from the door (-2816, -256) toward (-1280, -1792))
    CHECK(std::fabs(host.tgt().pos[0] - (-1000.0f + 73.0f * 37.0f * 0.70710678f)) < 3.0f && std::fabs(host.tgt().pos[2] + 73.0f * 37.0f * 0.70710678f) < 3.0f);
  }
  // 43acce: thrown up by 386 - 5 t per tick and back to the ground
  {
    Host host;
    auto effect = host.make("43acce");
    cmd(*effect, 0x180, {5, 0, 386, 0});
    host.tick(*effect, 40);
    CHECK(host.tgt().pos[1] < -5000.0f);
    host.tick(*effect, 200);
    CHECK(host.tgt().pos[1] == 0.0f);
  }
  // 19helf: x scale 0x1000 -> 0 over D, back to 0x1000 at home
  {
    Host host;
    auto effect = host.make("19helf");
    cmd(*effect, 0x100, {10, 0});
    host.tick(*effect, 12);
    CHECK(host.tgt().scale[0] == 0 && effect->done());
    cmd(*effect, 0x101, {10, 0});
    host.tick(*effect, 12);
    CHECK(host.tgt().scale[0] == 0x1000 && host.tgt().pos[0] == 1000.0f);
  }
  // golem: y scale to 0x200 without a shadow, then back
  {
    Host host;
    auto effect = host.make("golem");
    cmd(*effect, 0x100, {20, 0});
    host.tick(*effect, 22);
    CHECK(host.tgt().scale[1] == 0x200 && !(host.tgt().flags & 0x200));
    cmd(*effect, 0x101, {20, 0});
    host.tick(*effect, 22);
    CHECK(host.tgt().scale[1] == 0x1000 && (host.tgt().flags & 0x200));
  }
  // 45infe: lifted copy, then the collapse toward the saved point; the normal draw hidden while the copy shows
  {
    Host host;
    auto effect = host.make("45infe");
    cmd(*effect, 0x180, {9, 0, -1280, 0});
    host.tick(*effect, 1);
    CHECK(host.tgt().hidden && host.tgt().copies.size() == 1);
    Vec3 lifted{1000, -500, 0};
    if (!host.tgt().copies.empty()) host.tgt().copies[0].deform(lifted);
    CHECK(lifted[1] == -1780.0f);
    cmd(*effect, 0x181, {62, 0, 64, 0});
    host.tick(*effect, 80);
    Vec3 collapsed{1100, -100, 0};
    if (!host.tgt().copies.empty()) host.tgt().copies[0].deform(collapsed);
    // Fully collapsed onto P (+ yoff).
    CHECK(std::fabs(collapsed[0] - 1000.0f) < 2.0f);
    cmd(*effect, 0x182, {7, 0});
    CHECK(!host.tgt().hidden && host.tgt().script_table == "dead");
  }
  // 44get: an additive copy that rises 1760 units over 80 ticks, then sways and fades
  {
    Host host;
    auto effect = host.make("44get");
    cmd(*effect, 0x180, {80, 0, -704, 0});
    host.tick(*effect, 90);
    CHECK(host.tgt().copies.size() == 1 && !host.tgt().hidden);
    if (!host.tgt().copies.empty()) CHECK(host.tgt().copies[0].abr == 1 && host.tgt().copies[0].offset[1] == -1760.0f);
    cmd(*effect, 0x181, {125, 0, 0, 0, 128, 3, 2, 0});
    host.tick(*effect, 60);
    CHECK(host.tgt().copies.size() == 1);
    if (!host.tgt().copies.empty()) CHECK(host.tgt().copies[0].rgb[0] < 128 && host.tgt().copies[0].deform);
  }
  // 46black: the warp pulls a far vertex toward (0, -200, 0)
  {
    Vec3 vertex{2000, 0, 0};
    Host host;
    auto effect = host.make("46black");
    cmd(*effect, 0x180, {12, 0, 4, 5, 112, 0});
    host.tick(*effect, 150);
    CHECK(host.tgt().hidden && host.tgt().copies.size() == 1);
    if (!host.tgt().copies.empty()) host.tgt().copies[0].deform(vertex);
    CHECK(std::hypot(vertex[0], vertex[2]) < 2000.0f);
  }
  // 38super: one task per target, the model hidden and drawn as a copy until the task ends
  {
    Host host;
    auto effect = host.make("38super");
    cmd(*effect, 0x100, {30, 0, 0, 0});
    host.tick(*effect, 5);
    CHECK(host.tgt().hidden && host.tgt().copies.size() == 1);
    host.tick(*effect, 40);
    CHECK(effect->done() && host.tgt().copies.empty());
  }
  // swordbox: the front falls from the head; behind it a corner only ever moves toward C, at the end it is at C
  {
    Host host;
    fake_skin(host.ms);
    host.tgt().height = 500;
    auto effect = host.make("swordbox");
    cmd(*effect, 0x100, {60, 0, 0, 0});
    const auto corner_x = [&](int corner) {
      const std::vector<FxPrim> prims = frame(*effect);
      return prims.empty() ? 1e6f : prims[0].v[corner].x - host.st.ctx.view.ox;
    };
    host.tick(*effect, 1);
    const float head_start = corner_x(0), feet_start = corner_x(2);
    CHECK(host.tgt().hidden && !frame(*effect).empty() && head_start > 0.0f);
    host.tick(*effect, 29);
    CHECK(corner_x(0) < 0.7f * head_start && corner_x(2) == feet_start);
    host.tick(*effect, 28);
    CHECK(std::fabs(corner_x(0)) < 0.1f * head_start && std::fabs(corner_x(2)) < 0.1f * feet_start);
    host.tick(*effect, 4);
    CHECK(frame(*effect).empty() && host.tgt().hidden && host.tgt().release);
  }
  // swordbox: the deformed model keeps the stock draw's env-mapped packet over the textured one
  {
    Host host;
    fake_skin(host.ms);
    host.ms.skin = [stock = host.ms.skin](int unit, FxSkinnedModel& out) {
      const bool ok = stock(unit, out);
      out.shading[0][3] = 0x42;
      return ok;
    };
    host.tgt().height = 500;
    auto effect = host.make("swordbox");
    cmd(*effect, 0x100, {60, 0, 0, 0});
    host.tick(*effect, 1);
    const std::vector<FxPrim> prims = frame(*effect);
    CHECK(std::any_of(prims.begin(), prims.end(), [](const FxPrim& prim) { return prim.src == FxPrim::TexSrc::Vram && prim.blend == 1; }));
    CHECK(std::any_of(prims.begin(), prims.end(), [](const FxPrim& prim) { return prim.src == FxPrim::TexSrc::Model; }));
  }
  // kuzira3: thrown out from the origin, then back home
  {
    Host host;
    auto effect = host.make("kuzira3");
    cmd(*effect, 0x100, {200, 0, -121, 8344, 4096, 11, 21, 17});
    host.tick(*effect, 50);
    CHECK(host.tgt().pos[1] < -1000.0f);
    cmd(*effect, 0x101, {30, 0, 0, 0});
    host.tick(*effect, 40);
    CHECK(host.tgt().pos[0] == 1000.0f && host.tgt().pos[1] == 0.0f && host.tgt().scale[0] == 0x1000);
  }
  // mido2: orbits, rises, returns
  {
    Host host;
    auto effect = host.make("mido2");
    cmd(*effect, 0x100, {352, 0, -32, -4, 1126, 6537, 33, 9, 33});
    host.tick(*effect, 30);
    CHECK(host.tgt().steer);
    cmd(*effect, 0x101, {46, 0, 0, 0});
    host.tick(*effect, 60);
    CHECK(host.tgt().pos[0] == 1000.0f && host.tgt().pos[2] == 0.0f);
  }
  // 42hevi: 0x103 converges on home with the y scale back to 0x1000
  {
    Host host;
    host.tgt().pos = {1200, 300, 50};
    host.tgt().scale[1] = 0x400;
    auto effect = host.make("42hevi");
    cmd(*effect, 0x103, {26, 0, 0, 0});
    host.tick(*effect, 30);
    CHECK(host.tgt().pos[0] == 1000.0f && host.tgt().pos[1] == 0.0f && host.tgt().scale[1] == 0x1000 && effect->done());
  }
  // 11boru: rise, hang, fall, land with the 'dead' reaction
  {
    Host host;
    auto effect = host.make("11boru");
    host.tick(*effect, 128 + 128 + 10);
    CHECK(host.tgt().pos[1] == -8400.0f);
    host.tick(*effect, 127 + 60);
    CHECK(host.tgt().pos[1] == 0.0f && host.tgt().script_table == "dead");
  }
  // theend: drift toward the point after the per-target delay
  {
    Host host;
    auto effect = host.make("theend");
    cmd(*effect, 0x180, {0, 0, 12800, -1140, 0, 0});
    cmd(*effect, 0x181, {120, 0, 0, 0, 0, 0});
    host.tick(*effect, 100);
    CHECK(host.tgt().pos[0] > 1000.0f);
  }
  // No host emitters here: nothing to do, no crash.
  for (const char* name : {"22torune", "icerance", "27earth", "enrou", "hebi", "25upli"}) {
    Host host;
    auto effect = host.make(name);
    host.tick(*effect, 400);
    CHECK(frame(*effect).empty());
  }
  CHECK(ps1_square_root0(4096) == 64);
  CHECK(ps1_square_root0(0) == 0);
}

void test_ring() {
  for (const char* name : {"malchera", "spark", "oomono2", "monfish"}) {
    Host host;
    fake_skin(host.ms);
    auto effect = host.make(name);
    // Nothing is drawn before 0x180.
    CHECK(frame(*effect).empty());
    cmd(*effect, 0x180, {100, 0, 0, 1, 7, 0});
    cmd(*effect, 0x181, {14, 0, 0, 0, 20, 0, 7, 0});
    host.tick(*effect, 30);
    std::vector<FxPrim> raw;
    effect->draw(raw);
    int lines = 0;
    for (const auto& prim : raw) lines += prim.kind == FxPrim::Kind::Line && prim.blend == 2;
    CHECK(lines == 59);
    for (size_t k = 0; k + 1 < raw.size(); ++k) CHECK(raw[k].v[1].r == raw[k + 1].v[0].r);
    host.tick(*effect, 100);
    // Life 100 has passed.
    CHECK(frame(*effect).empty() && effect->done());
  }
}

// The F-B dissolve (darkfeit 6b30): op 0x100 hides the stock pass, the faces fade with the distance to the centre; partial faces are two
// packets (ABR 1 own CLUT + ABR 2 white CLUT), back faces culled.
void test_dissolve() {
  Host host;
  fake_skin(host.ms);
  auto effect = host.make("darkfeit");
  cmd(*effect, 0x100, {240, 0, 4608, 48, -4076, 2, 0, 0});
  host.tick(*effect, 1);
  CHECK(host.tgt().hidden);
  auto f0 = frame(*effect);
  CHECK(!f0.empty());
  for (const auto& prim : f0) CHECK(prim.src == FxPrim::TexSrc::Model && prim.unit == 1);
  host.tick(*effect, 120);
  size_t white = 0, add = 0;
  for (const auto& prim : frame(*effect)) white += prim.white == 1 && prim.blend == 3, add += prim.blend == 2;
  CHECK(white == add);
  host.tick(*effect, 200);
  CHECK(effect->done());
}

void test_model_draw_ports() {
  // jakotu: env packet (VRAM page 0x97 | ABR 1, CLUT 0x3AD0) + base packet per front face; the back face culled; the stock pass hidden
  {
    Host host;
    fake_skin(host.ms);
    auto effect = host.make("jakotu");
    host.tick(*effect, 2);
    auto prims = frame(*effect);
    CHECK(prims.size() == 2);
    size_t env = 0;
    for (const auto& prim : prims) env += prim.src == FxPrim::TexSrc::Vram && prim.tpage == 0xB7 && prim.clut == 0x3AD0 && prim.blend == 2;
    CHECK(env == 1);
    CHECK(host.ms.units[0].hidden && host.ms.units[0].own_shadow);
    CHECK(host.ms.vram_moves.size() == 2 && host.ms.vram_moves[0].scroll);
  }
  // ghost2: the pseudo-actor (not a caster) is hidden at init and drawn by the overlay clipped at y = 0 (the quad lies wholly above, drawn)
  // with its shadow; past D it is shown again
  {
    Host host;
    fake_skin(host.ms);
    auto effect = host.make("ghost2");
    CHECK(!host.ms.units[0].hidden && host.ms.pseudo_hide == 1);
    cmd(*effect, 0x180, {10, 0});
    host.tick(*effect, 1);
    CHECK(host.ms.pseudo_hide == 1 && host.ms.pseudo_own_shadow);
    auto prims = frame(*effect);
    CHECK(prims.size() == 1 && prims[0].kind == FxPrim::Kind::Quad && prims[0].unit == kPseudoUnit);
    host.tick(*effect, 10);
    CHECK(host.ms.pseudo_hide == 0 && !host.ms.pseudo_own_shadow && frame(*effect).empty());
    CHECK(!host.ms.units[0].hidden);
  }
  // hensin: the plane from the head down: s = t H1 / D with H1 = 400 (attach 0xE at y -400); half way the quad is cut and shards spawn
  {
    Host host;
    fake_skin(host.ms);
    auto effect = host.make("hensin");
    cmd(*effect, 0x180, {10, 0});
    // P = 200 - 400 = -200: cut.
    host.tick(*effect, 5);
    auto prims = frame(*effect);
    size_t model = 0, shard = 0;
    for (const auto& prim : prims) (prim.kind == FxPrim::Kind::Tri && prim.blend == 2 ? shard : model) += 1;
    CHECK(model >= 1 && shard >= 1);
    for (const auto& prim : prims)
      if (prim.blend == 0)
        for (int k = 0; k < (prim.kind == FxPrim::Kind::Tri ? 3 : 4); ++k) CHECK(prim.v[k].y >= 0.0f);
  }
  // fnoise 0x181: one shard per face, drawn additive on the env page, fading 2 per tick
  {
    Host host;
    fake_skin(host.ms);
    auto effect = host.make("fnoise");
    cmd(*effect, 0x181, {42, 0, 1120, 16});
    host.tick(*effect, 1);
    auto prims = frame(*effect);
    CHECK(prims.size() == 2 && prims[0].src == FxPrim::TexSrc::Vram && prims[0].clut == 0x3890 && prims[0].v[0].r == 0x62);
    host.tick(*effect, 60);
    CHECK(frame(*effect).empty());
  }
  // rakugaki 0x180: one tile per tick moved (640 + tx, 256 + ty) -> (704 + tx, ...), the model drawn once on the doodle page
  {
    Host host;
    fake_skin(host.ms);
    auto effect = host.make("rakugaki");
    cmd(*effect, 0x180, {472, 0});
    host.tick(*effect, 1);
    CHECK(host.ms.vram_moves.size() == 1 && host.ms.vram_moves[0].w == 4 && host.ms.vram_moves[0].dx - host.ms.vram_moves[0].x == 0x40);
    auto prims = frame(*effect);
    CHECK(prims.size() == 1 && prims[0].src == FxPrim::TexSrc::Vram && prims[0].clut == 0x3B90 && prims[0].v[0].r == 0x80);
  }
  // garadeth: two passes, the second subtractive with the all-white CLUT
  {
    Host host;
    fake_skin(host.ms);
    auto effect = host.make("garadeth");
    cmd(*effect, 0x180, {567, 0});
    host.tick(*effect, 1);
    auto prims = frame(*effect);
    CHECK(prims.size() == 2 && prims[0].blend == 3 && prims[0].white == 2 && prims[1].blend == 2);
  }
  // trap (tgolem): the pack model drawn by the task, pulled toward the jar
  {
    Host host;
    fake_skin(host.ms);
    auto effect = host.make("tgolem");
    cmd(*effect, 0x100, {304, 0, 3332, 6609});
    host.tick(*effect, 1);
    CHECK(host.ms.pack_model_hidden);
    CHECK(!frame(*effect).empty() && frame(*effect)[0].src == FxPrim::TexSrc::PackModel);
  }
  // scene remaps: etrap swaps the lists, seint empties the casters and lists the caster once per opposite-side target
  {
    Host host;
    auto effect = host.make("etrap");
    CHECK(host.ms.caster_override && host.ms.caster_list == std::vector<int>{1} && host.ms.target_list == std::vector<int>{0});
    Host seint_host;
    auto seint_effect = seint_host.make("seint");
    CHECK(seint_host.ms.caster_override && seint_host.ms.caster_list.empty() && seint_host.ms.target_list == std::vector<int>{0});
    seint_host.tgt().hit = 3;
    cmd(*seint_effect, 0x100, {0, 0});
    CHECK(seint_host.ms.target_list == std::vector<int>{1});
  }
  // saraman's 'sara' (FxModels::PackDrive): hidden with SetAnim(0, 0) at init and the effect held; file 0 streamed
  // once t > 0x50; when it is in: 'main' is the overlay's (suppress_main), the hold released, the creature shown and, its 1-frame clip
  // being over, file 0 attached with SetAnim(0x80, 1); the next file only when the animation is over and the stream is in (else the
  // frame stalls: speed 0); 0x183 streams the next file, 0x184 hides the creature
  {
    Host host;
    fake_skin(host.ms);
    auto effect = host.make("saraman");
    const effects::FxModels::PackDrive& drive = host.ms.pack_drive;
    CHECK(host.ms.suppress_main && drive.active && drive.name == "sara" && drive.hidden && drive.clip == 0 && drive.clip_flags == 0);
    CHECK(!effect->done());
    host.ms.pack_drive.clip = -1;
    host.tick(*effect, 0x50);
    CHECK(drive.motion_request == -1 && drive.hidden && frame(*effect).empty());
    host.tick(*effect, 1);
    CHECK(drive.motion_request == 0);
    // The host streams the motion after the tick.
    host.ms.pack_drive.motion_request = -1;
    host.ms.pack_drive.motion_state[0] = 1;
    host.ms.pack_drive.at_end = true;
    host.tick(*effect, 1);
    CHECK(!drive.hidden && drive.motion == 0 && drive.clip == 0x80 && drive.clip_flags == 1 && effect->done());
    CHECK(drive.rot[1] == 0x400 && drive.speed == 64 && drive.pos[0] == 0.0f);
    auto prims = frame(*effect);
    CHECK(prims.size() == 1 && prims[0].src == FxPrim::TexSrc::PackModel);
    host.ms.pack_drive.clip = -1;
    host.ms.pack_drive.at_end = false;
    cmd(*effect, 0x183, {});
    CHECK(drive.motion_request == 1);
    host.ms.pack_drive.motion_request = -1;
    host.tick(*effect, 5);
    CHECK(drive.motion == 0 && drive.clip == -1);
    // Over, file 1 not in yet: the stall.
    host.ms.pack_drive.at_end = true;
    host.tick(*effect, 1);
    CHECK(drive.motion == 0 && drive.speed == 0);
    host.ms.pack_drive.motion_state[1] = 1;
    host.tick(*effect, 1);
    CHECK(drive.motion == 1 && drive.clip == 0x80 && drive.speed == 64);
    cmd(*effect, 0x184, {});
    CHECK(drive.hidden && frame(*effect).empty());
  }
  // 155slot: the three reels pre-scroll 6 lines each at init (2 DR_MOVEs per line); the confirm button stops them left to right
  {
    Host host;
    auto effect = host.make("155slot");
    CHECK(host.ms.vram_moves.size() == 36);
    cmd(*effect, 0x100, {27, 0});
    for (int k = 0; k < 3; ++k) {
      host.ms.pad_edge = 0x20;
      host.tick(*effect, 1);
    }
    host.ms.pad_edge = 0;
    host.tick(*effect, 400);
    CHECK(effect->done());
  }
}

void test_flatten() {
  FxPrim quad_prim;
  quad_prim.kind = FxPrim::Kind::Quad;
  const float xs[4] = {0, 8, 0, 8}, ys[4] = {0, 0, 8, 8};
  for (int i = 0; i < 4; ++i) {
    quad_prim.v[i].x = xs[i];
    quad_prim.v[i].y = ys[i];
    quad_prim.v[i].r = static_cast<u8>(i * 60);
  }
  std::vector<FlatQuad> out;
  flatten_prims({quad_prim}, out, 4);
  CHECK(out.size() == 16);
  out.clear();
  // One cell gives one quad with the four corner colours kept (Gouraud, not the mean).
  flatten_prims({quad_prim}, out, 1);
  CHECK(out.size() == 1);
  for (int i = 0; i < 4; ++i) CHECK(out[0].c[i][0] == static_cast<u8>(i * 60));
  FxPrim tri_prim;
  tri_prim.kind = FxPrim::Kind::Tri;
  for (int i = 0; i < 3; ++i) tri_prim.v[i].r = static_cast<u8>(i * 100);
  out.clear();
  flatten_prims({tri_prim}, out, 1);
  CHECK(out.size() == 1 && out[0].c[0][0] == 0 && out[0].c[1][0] == 100 && out[0].c[2][0] == 200 && out[0].c[3][0] == 200);
  FxPrim line_prim;
  line_prim.kind = FxPrim::Kind::Line;
  line_prim.v[0].r = 10, line_prim.v[1].r = 250, line_prim.v[1].x = 20;
  out.clear();
  flatten_prims({line_prim}, out, 1);
  CHECK(out.size() == 1 && out[0].c[0][0] == 10 && out[0].c[2][0] == 10 && out[0].c[1][0] == 250 && out[0].c[3][0] == 250);
}
}  // namespace

int main() {
  test_registry();
  test_negative_strip();
  test_muteki_falloff();
  test_lasdes2();
  test_ring();
  test_third_slot_reload();
  test_dissolve();
  test_flatten();
  test_actor_scripts();
  test_model_draw_ports();
  test_determinism_and_soak();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("effects_bespoke: ok (%zu overlays)\n", BespokeRegistry::instance().all().size());
  return 0;
}
