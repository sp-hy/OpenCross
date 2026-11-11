// Tests for the room .prd effects importer on a hand-built drp (TIM + timinfo + emitter + sequence) and for the effect player: stepper /
// header / overlay pacing, ramps and oscillators, easing and curves, blend arithmetic, OT slots, the sequence player, nested packs, the
// rotation order, quad-strip smoothing and the lens flare flash.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/effects/emitter_system.h"
#include "game/effects/fx_math.h"
#include "game/effects/object_heap.h"
#include "game/effects/sequence_player.h"
#include "game/effects/soft_raster.h"
#include "game/effects/strip_smooth.h"
#include "import/battle_stage.h"
#include "import/room_effects.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

void p16(Bytes& bytes, u32 value) {
  bytes.push_back(static_cast<u8>(value));
  bytes.push_back(static_cast<u8>(value >> 8));
}
void p32(Bytes& bytes, u32 value) {
  p16(bytes, value & 0xFFFF);
  p16(bytes, value >> 16);
}
void pname(Bytes& bytes, const char* name) {
  for (int i = 0; i < 4; ++i) bytes.push_back(name[i] ? static_cast<u8>(name[i]) : u8{0});
}

// 4bpp TIM: CLUT of 16 entries at (cx,cy) with entry 1 = red with STP; image iw halfwords x ih rows at (ix,iy), all texels = index 1.
Bytes make_tim(u16 cx, u16 cy, u16 ix, u16 iy, u16 iw, u16 ih) {
  Bytes bytes;
  p32(bytes, 0x10);
  p32(bytes, 8);
  p32(bytes, 12 + 32);
  p16(bytes, cx); p16(bytes, cy); p16(bytes, 16); p16(bytes, 1);
  for (int i = 0; i < 16; ++i) p16(bytes, i == 1 ? 0x801F : 0);
  p32(bytes, 12 + 2u * iw * ih);
  p16(bytes, ix); p16(bytes, iy); p16(bytes, iw); p16(bytes, ih);
  for (int i = 0; i < iw * ih; ++i) p16(bytes, 0x1111);
  return bytes;
}

// timinfo with one table at +0xE (after the single offset) holding one sprite: x=-4 y=-8 u=2 v=1 clut=(cy<<6)|(cx>>4) w=4 h=2, flags = page 15
// with mirror X (bit 9).
Bytes make_timinfo(u16 count_flags, u16 tx, u16 ty, u16 cx, u16 cy) {
  Bytes bytes;
  p16(bytes, 0); p16(bytes, count_flags); p16(bytes, tx); p16(bytes, ty); p16(bytes, cx); p16(bytes, cy);
  p16(bytes, 0xE);
  p16(bytes, 1); p16(bytes, 0);
  p16(bytes, 0xFFFC); p16(bytes, 0xFFF8);
  bytes.push_back(2); bytes.push_back(1);
  p16(bytes, static_cast<u32>((cy << 6) | (cx >> 4)));
  bytes.push_back(4); bytes.push_back(2);
  p16(bytes, 15 | (1u << 9));
  return bytes;
}

void cmd(Bytes& bytes, u32 op, u32 len) { p16(bytes, op | (len << 9)); }

// Emitter header (link, start 3, duration 0, frame, period 60, countdown, repeat 0; flags start-stopped + jitter 5; pointer) followed by
// the ops 01, 03 (OT bias -1), 05 (sprite "sprt" type 3 frame 1), 06 (flags, blend 2), 08 (position 10 -10 0), 09 (3 x amplitude /
// frequency / phase + one pad s16), 35 (raw words kept) and 0F life 90; `bad` replaces the 0F by the unknown op 7F.
Bytes make_emitter(bool bad) {
  Bytes bytes;
  p32(bytes, 0); p16(bytes, 3); p16(bytes, 0); p16(bytes, 0); p16(bytes, 60); p16(bytes, 0); p16(bytes, 0);
  p32(bytes, 4 | (5u << 3)); p32(bytes, 0);
  cmd(bytes, 1, 2);
  cmd(bytes, 3, 4); p16(bytes, 0xFFFF);
  cmd(bytes, 5, 16); p16(bytes, 1); p32(bytes, 0); pname(bytes, "sprt"); p16(bytes, 3); p16(bytes, 0);
  cmd(bytes, 6, 8); p16(bytes, 0); p32(bytes, 0x00800002);
  cmd(bytes, 8, 8); p16(bytes, 10); p16(bytes, 0xFFF6); p16(bytes, 0);
  cmd(bytes, 9, 26);
  for (int axis = 0; axis < 3; ++axis) { p16(bytes, 100 + axis); p16(bytes, 7); p16(bytes, 9); p16(bytes, 0); }
  cmd(bytes, 0x35, 8); p16(bytes, 0); p16(bytes, 0x1234); p16(bytes, 0x5678);
  cmd(bytes, bad ? 0x7F : 0x0F, bad ? 4 : 4); p16(bytes, 90);
  p16(bytes, 0);
  return bytes;
}

// A 0x14-byte sequence header, a 22 (start emitter "sprt" after a wait of 30) and a 21 (end).
Bytes make_sequence() {
  Bytes bytes;
  p32(bytes, 1); p32(bytes, 0); p32(bytes, 0); p32(bytes, 0); p32(bytes, 0);
  cmd(bytes, 0x22, 16); p16(bytes, 30); p16(bytes, 0); p16(bytes, 0); p16(bytes, 0); p16(bytes, 0); pname(bytes, "sprt");
  cmd(bytes, 0x21, 8); p16(bytes, 0); p32(bytes, 0);
  return bytes;
}

struct Ent {
  const char* name;
  u8 type;
  Bytes data;
};

// drp: "drp\0", u32 0, u32 64*count, u32 offsets[count]; subfile = u32 0, name[4], u8 type, u24 16*len, payload.
Bytes make_drp(const std::vector<Ent>& ents) {
  Bytes bytes = {'d', 'r', 'p', 0};
  p32(bytes, 0);
  p32(bytes, static_cast<u32>(ents.size()) * 64);
  Bytes body;
  const u32 base = 12 + 4 * static_cast<u32>(ents.size());
  for (const auto& entry : ents) {
    p32(bytes, base + static_cast<u32>(body.size()));
    p32(body, 0);
    pname(body, entry.name);
    body.push_back(entry.type);
    const u32 len = static_cast<u32>(entry.data.size()) * 16;
    body.push_back(static_cast<u8>(len)); body.push_back(static_cast<u8>(len >> 8)); body.push_back(static_cast<u8>(len >> 16));
    body.insert(body.end(), entry.data.begin(), entry.data.end());
  }
  bytes.insert(bytes.end(), body.begin(), body.end());
  return bytes;
}

// A well-formed drp loads one sheet (its sprite relocated by the TIM's page: u + ((962 & 63) << 2) = 10, v unchanged), one emitter with all
// eight ops decoded and one sequence, and validates clean.
void test_good() {
  const Bytes drp = make_drp({{"sprt", 4, make_tim(0, 200, 962, 0, 4, 4)},
                              {"sprt", 3, make_timinfo(0x8001, 962, 0, 0, 200)},
                              {"smok", 10, make_emitter(false)},
                              {"own", 16, make_sequence()}});
  import::Vram vram;
  const auto fx = import::load_room_effects(drp, vram);
  for (const auto& error : fx.errors) std::printf("error: %s\n", error.c_str());
  CHECK(fx.errors.empty());
  CHECK(fx.tims == 1 && fx.sheets.size() == 1 && fx.emitters.size() == 1 && fx.sequences.size() == 1);
  if (fx.sheets.size() != 1 || fx.emitters.size() != 1 || fx.sequences.size() != 1) return;

  const auto& sheet = fx.sheets[0];
  CHECK(sheet.reloc_pending && sheet.has_tim && sheet.tim_match);
  CHECK(sheet.frames.size() == 1 && sheet.frames[0].size() == 1);
  if (sheet.frames.size() != 1 || sheet.frames[0].size() != 1) return;
  const auto& sprite = sheet.frames[0][0];
  CHECK(sprite.x == -4 && sprite.y == -8 && sprite.w == 4 && sprite.h == 2);
  CHECK(sprite.u == 10 && sprite.v == 1);
  CHECK(sprite.page_x() == 960 && sprite.page_y() == 0 && sprite.depth() == 0 && sprite.mirror_x() && !sprite.mirror_y());
  CHECK(sprite.clut_x() == 0 && sprite.clut_y() == 200);
  CHECK(sprite.rgba.size() == 4u * 2 * 4 && sprite.rgba[0] == 255 && sprite.rgba[1] == 0 && sprite.rgba[3] == 255);
  CHECK(sheet.sprites_total == 1 && sheet.sprites_inside == 1);

  const auto& emitter = fx.emitters[0];
  CHECK(emitter.start == 3 && emitter.duration == 0 && emitter.period == 60 && emitter.repeat == 0);
  CHECK(emitter.start_stopped() && emitter.jitter() == 5);
  CHECK(emitter.ops.size() == 8);
  if (emitter.ops.size() != 8) return;
  CHECK(emitter.ops[0].op == 1 && emitter.ops[1].op == 3);
  CHECK(std::get<assets::FxValue>(emitter.ops[1].arg).value == -1);
  const auto& sprite_op = std::get<assets::FxSprite>(emitter.ops[2].arg);
  CHECK(sprite_op.name == "sprt" && sprite_op.type == 3 && sprite_op.frame0 == 1 && sprite_op.cache == 0);
  CHECK(std::get<assets::FxFlags>(emitter.ops[3].arg).blend() == 2);
  const auto& pos = std::get<assets::FxXyz>(emitter.ops[4].arg);
  CHECK(pos.x == 10 && pos.y == -10 && pos.z == 0);
  const auto& osc = std::get<assets::FxOsc>(emitter.ops[5].arg);
  CHECK(osc.count == 3 && osc.axis[1].amp == 101 && osc.axis[2].freq == 7);
  CHECK(emitter.ops[6].op == 0x35 && emitter.ops[6].len == 8 && std::get<assets::FxExt35>(emitter.ops[6].arg).ext == (0x1234u | (0x5678u << 16)));
  CHECK(std::get<assets::FxValue>(emitter.ops[7].arg).value == 90);

  const auto& sequence = fx.sequences[0];
  CHECK(sequence.flags == 1 && sequence.cmds.size() == 2 && sequence.cmds[0].op == 0x22 && sequence.cmds[0].wait == 30 &&
        sequence.cmds[0].name == "sprt");
  CHECK(sequence.cmds[1].op == 0x21);

  const auto validation = import::validate_room_effects(fx);
  CHECK(validation.ok() && validation.pair_ok == 1 && validation.sprites == 1);
}

// An unknown op 0x7F skips the emitter (one error) while the sequence still loads; a timinfo whose TIM does not match is a warning only
// and its sprite keeps u = 2 (the reloc flag clear); garbage is one error.
void test_bad() {
  const Bytes drp = make_drp({{"smok", 10, make_emitter(true)}, {"own", 16, make_sequence()}});
  const auto fx = import::load_room_effects(drp, import::Vram{});
  CHECK(fx.errors.size() == 1);
  CHECK(fx.emitters.empty() && fx.sequences.size() == 1);
  CHECK(!import::validate_room_effects(fx).ok());
  const Bytes drp2 = make_drp({{"sprt", 4, make_tim(0, 200, 960, 0, 4, 4)}, {"sprt", 3, make_timinfo(0x0001, 962, 0, 0, 200)}});
  const auto fx2 = import::load_room_effects(drp2, import::Vram{});
  const auto validation2 = import::validate_room_effects(fx2);
  CHECK(validation2.ok() && validation2.pair_mismatch == 1 && validation2.warnings.size() == 1);
  CHECK(fx2.sheets.size() == 1);
  if (fx2.sheets.size() != 1 || fx2.sheets[0].frames.empty() || fx2.sheets[0].frames[0].empty()) return;
  CHECK(fx2.sheets[0].frames[0][0].u == 2);
  CHECK(import::load_room_effects(Bytes{1, 2, 3}, import::Vram{}).errors.size() == 1);
}

// ---- effect player ------------------------------------------------------------------------------------------------------------
using assets::EffectOp;

EffectOp mk(u16 op, decltype(EffectOp::arg) arg) {
  EffectOp result;
  result.op = op;
  result.arg = std::move(arg);
  return result;
}
EffectOp xyz(u16 op, int x, int y, int z) {
  return mk(op, assets::FxXyz{static_cast<s16>(x), static_cast<s16>(y), static_cast<s16>(z)});
}
EffectOp val(u16 op, int value) { return mk(op, assets::FxValue{value}); }

// A one-frame 8x8 white sprite sheet named "wht1" centred on the origin.
assets::RoomEffects make_pack() {
  assets::RoomEffects fx;
  assets::SpriteSheet sheet;
  sheet.name = "wht1";
  assets::EffectSprite sprite;
  sprite.x = -4;
  sprite.y = -4;
  sprite.w = 8;
  sprite.h = 8;
  sprite.rgba.assign(8 * 8 * 4, 255);
  sheet.frames.push_back({sprite});
  fx.sheets.push_back(sheet);
  return fx;
}
assets::Emitter make_emitter(const char* name, s16 start, s16 duration, u16 period, std::vector<EffectOp> ops) {
  assets::Emitter emitter;
  emitter.name = name;
  emitter.start = start;
  emitter.duration = duration;
  emitter.period = period;
  emitter.ops.push_back(mk(1, assets::FxNone{}));
  emitter.ops.push_back(mk(5, assets::FxSprite{1, 0, "wht1", 3}));
  for (auto& op : ops) emitter.ops.push_back(std::move(op));
  return emitter;
}

// A dying object with a death child runs its last tick first: life 3 means three velocity steps before the child takes the position.
void test_death_child_position() {
  assets::RoomEffects fx = make_pack();
  auto parent = make_emitter("par", 0, 0, 0, {xyz(0x08, 0, 0, 0), xyz(0x0A, 128, 0, 0), val(0x0F, 3), mk(0x33, assets::FxNamed{0, "chd"})});
  parent.flags = 4;
  fx.emitters.push_back(parent);
  auto child = make_emitter("chd", 0, 0, 0, {xyz(0x08, 0, 0, 0), val(0x0F, 100)});
  child.flags = 4;
  fx.emitters.push_back(child);
  effects::EffectBank bank;
  bank.add(&fx);
  effects::EmitterSystem sys(bank);
  sys.register_all();
  CHECK(sys.start("par"));
  for (int tick = 0; tick < 10; ++tick) sys.tick();
  const float one_step = static_cast<float>((128 << 6) << 3) / 65536.0f;
  bool found_child = false;
  effects::EmitterSystem::ObjectView view;
  for (size_t i = 0; i < sys.live_objects(); ++i)
    if (sys.object_view(i, view) && view.owner == 1) found_child = std::fabs(view.pos[0] - 3.0f * one_step) < 1e-3f;
  CHECK(found_child);
}

// Frame stepping (2 ticks per frame, the field): an emitter firing every 10 ticks with life 12 never has its object and the successor
// alive together after a step, while a step per tick shows both on the tick the successor spawns. Life, colour and position advance by
// the frame's 2 ticks per step and stay put between steps.
void test_frame_stepping() {
  assets::RoomEffects fx = make_pack();
  fx.emitters.push_back(make_emitter("lmp", 0, 0, 9, {xyz(0x08, 0, 0, 0), xyz(0x0A, 128, 0, 0), xyz(0x20, 0x800, 0x800, 0x800),
                                                       xyz(0x22, static_cast<u16>(-29), 0, 0), val(0x0F, 12)}));
  effects::EffectBank bank;
  bank.add(&fx);
  int most_per_tick = 0;
  {
    effects::EmitterSystem sys(bank);
    sys.register_all();
    for (int tick = 0; tick < 60; ++tick) {
      sys.tick();
      most_per_tick = std::max(most_per_tick, static_cast<int>(sys.live_objects()));
    }
  }
  CHECK(most_per_tick == 2);
  effects::EmitterSystem sys(bank);
  sys.set_frame_ticks(2);
  sys.register_all();
  int most_per_frame = 0;
  bool steps_ok = true;
  effects::EmitterSystem::ObjectView view, before;
  const float one_step = static_cast<float>((128 << 6) << 3) / 65536.0f;
  for (int tick = 1; tick <= 60; ++tick) {
    const bool had = sys.live_objects() == 1 && sys.object_view(0, before);
    sys.tick();
    if (tick % 2 == 1 && had && sys.object_view(0, view)) steps_ok = steps_ok && view.life == before.life && view.color[0] == before.color[0];
    if (tick % 2 != 0) continue;
    most_per_frame = std::max(most_per_frame, static_cast<int>(sys.live_objects()));
    if (had && sys.object_view(0, view) && view.life < before.life)
      steps_ok = steps_ok && view.life == before.life - 2 && view.color[0] == before.color[0] - 2 * 29 &&
                 std::fabs(view.pos[0] - before.pos[0] - 2.0f * one_step) < 1e-3f;
  }
  CHECK(most_per_frame == 1);
  CHECK(steps_ok);
}

// mark_mesh_objects marks the mesh objects alive at the call: what the emitter spawns later stays unmarked, and the mark can be taken off.
void test_mark_mesh_objects() {
  assets::RoomEffects fx = make_pack();
  assets::Emitter near_emitter = make_emitter("near", 0, 0, 1, {xyz(0x08, 0, 0, 0), val(0x0F, 1000)});
  near_emitter.ops[1] = mk(5, assets::FxSprite{1, 0, "mesh", 14});
  fx.emitters.push_back(near_emitter);
  effects::EffectBank bank;
  bank.add(&fx);
  effects::EmitterSystem sys(bank);
  sys.register_all();
  CHECK(sys.start("near"));
  for (int tick = 0; tick < 3; ++tick) sys.tick();
  const effects::ViewParams view;
  auto marked_of = [&](std::size_t& total) {
    std::vector<effects::EmitterSystem::MeshInstance> meshes;
    sys.collect_meshes(view, 1.0f, meshes);
    total = meshes.size();
    return static_cast<std::size_t>(std::count_if(meshes.begin(), meshes.end(), [](const auto& mesh) { return mesh.marked; }));
  };
  std::size_t alive = 0;
  CHECK(marked_of(alive) == 0);
  CHECK(alive > 0);
  sys.mark_mesh_objects("near", true);
  const std::size_t at_mark = alive;
  for (int tick = 0; tick < 3; ++tick) sys.tick();
  CHECK(marked_of(alive) == at_mark);
  CHECK(alive > at_mark);
  sys.mark_mesh_objects("near", false);
  CHECK(marked_of(alive) == 0);
}

// Stepper maths of three ops, VEL (0A), ACC (0C) and LIFE (0F), checked against the PS1 integer recurrence: pos 0, vel = 128 << 6 (1 unit
// per tick), acc = 4 << 3 = 32 per tick, life 10. Period 100 would fire on tick 101; start() sets countdown = 1 so the object spawns on the
// second tick and is updated once in that tick; it dies when its life reaches 0.
void test_stepper_ops() {
  assets::RoomEffects fx = make_pack();
  fx.emitters.push_back(make_emitter("mov", 0, 0, 100, {xyz(0x08, 0, 0, 0), xyz(0x0A, 128, 0, 0), xyz(0x0C, 4, 0, 0), val(0x0F, 10)}));
  effects::EffectBank bank;
  bank.add(&fx);
  effects::EmitterSystem sys(bank);
  sys.register_all();
  CHECK(sys.start("mov"));
  sys.tick();
  CHECK(sys.live_objects() == 0);
  sys.tick();
  CHECK(sys.live_objects() == 1);
  s32 pos = 0, vel = 128 << 6;
  const s32 acc = 4 << 3;
  pos += vel << 3;
  vel += acc;
  effects::EmitterSystem::ObjectView view;
  CHECK(sys.object_view(0, view));
  CHECK(view.life == 9);
  CHECK(std::fabs(view.pos[0] - static_cast<float>(pos) / 65536.0f) < 1e-4f);
  for (int tick = 0; tick < 5; ++tick) {
    sys.tick();
    pos += vel << 3;
    vel += acc;
  }
  CHECK(sys.object_view(0, view));
  CHECK(std::fabs(view.pos[0] - static_cast<float>(pos) / 65536.0f) < 1e-4f);
  CHECK(std::fabs(view.vel[0] - static_cast<float>(vel) / 65536.0f) < 1e-4f);
  CHECK(view.life == 4);
  for (int tick = 0; tick < 4; ++tick) sys.tick();
  CHECK(sys.live_objects() == 0);
  CHECK(sys.stats().spawned >= 1 && sys.stats().died >= 1);
}

// A header that waits (bit 2) and is never started keeps the frame word the file stores (unico's periodic child pou0 stores 116) and the
// oscillator ops read it as their time: an x-only oscillation of amplitude 256 / frequency 6 at frame 116 gives sin(2 pi 696 / 4096) = 0.88.
// A script start would zero it (sin(0) = 0); fire() spawns the periodic child without a start.
void test_stored_header_frame() {
  assets::RoomEffects fx = make_pack();
  assets::FxOsc osc;
  osc.axis[0] = {256, 6, 0};
  assets::Emitter emitter = make_emitter("osc", 0, 0, 100, {xyz(0x08, 0, 0, 0), xyz(0x12, 128, 128, 128), mk(0x14, osc), val(0x0F, 50)});
  emitter.flags = 4;
  emitter.frame0 = 116;
  fx.emitters.push_back(emitter);
  effects::EffectBank bank;
  bank.add(&fx);
  effects::EmitterSystem sys(bank);
  sys.register_all();
  CHECK(sys.fire("osc"));
  effects::EmitterSystem::ObjectView view;
  CHECK(sys.object_view(0, view));
  CHECK(view.scale[2] == 128 && view.scale[1] == 128);
  const int want = 128 + ((effects::rsin(116 * 6) * 256 * 16) >> 16);
  CHECK(view.scale[0] == want && want > 300);
}

// Overlay pacing: with a window of 3 ticks an overlay's per-call fire sites (two sites of "kem" per tick) fire once per window; a one-shot
// site that first appears in the middle of a window still fires; outside begin/end every fire passes.
void test_overlay_pacing() {
  assets::RoomEffects fx = make_pack();
  fx.emitters.push_back(make_emitter("kem", 0, 0, 100, {xyz(0x08, 0, 0, 0), val(0x0F, 1000)}));
  fx.emitters.push_back(make_emitter("one", 0, 0, 100, {xyz(0x08, 0, 0, 0), val(0x0F, 1000)}));
  effects::EffectBank bank;
  bank.add(&fx);
  effects::EmitterSystem sys(bank);
  sys.register_all();
  for (int tick = 0; tick < 6; ++tick) {
    sys.begin_overlay_tick(tick % 3 == 0);
    sys.fire("kem");
    sys.fire("kem");
    if (tick == 1) sys.fire("one");
    sys.end_overlay_tick();
  }
  CHECK(sys.live_objects() == 2 * 2 + 1);
  sys.fire("kem");
  CHECK(sys.live_objects() == 6);
}

// Header stepper: a steady emitter (period 2) fires every period + 1 = 3 ticks (ticks 3, 6, 9); a burst (duration 4, period 1) fires once in
// frames 1..3 and stops after its duration; stop() and kill() end the header and its objects.
void test_header_stepper() {
  assets::RoomEffects fx = make_pack();
  fx.emitters.push_back(make_emitter("std", 0, 0, 2, {xyz(0x08, 0, 0, 0), val(0x0F, 1000)}));
  fx.emitters.push_back(make_emitter("bst", 0, 4, 1, {xyz(0x08, 0, 0, 0), val(0x0F, 1000)}));
  effects::EffectBank bank;
  bank.add(&fx);
  effects::EmitterSystem sys(bank);
  sys.register_all();
  CHECK(sys.header_running(0) && sys.header_running(1));
  int spawns = 0;
  size_t last = 0;
  for (int tick = 1; tick <= 9; ++tick) {
    sys.tick();
    const size_t live = sys.live_objects();
    if (live > last) spawns += static_cast<int>(live - last);
    last = live;
  }
  CHECK(spawns == 4);
  CHECK(!sys.header_running(1));
  CHECK(sys.header_running(0));
  CHECK(sys.stop("std") && !sys.header_running(0));
  CHECK(sys.kill("0000"));
  sys.tick();
  CHECK(sys.live_objects() == 0);
}

// Start-stopped emitters (flags 4) wait for start(); the death child (op 33) spawns where the parent died (position 20), not at its own
// op-08 position (99).
void test_start_and_death_child() {
  assets::RoomEffects fx = make_pack();
  auto parent = make_emitter("par", 0, 0, 0, {xyz(0x08, 20, 0, 0), val(0x0F, 3), mk(0x33, assets::FxNamed{0, "chd"})});
  parent.flags = 4;
  fx.emitters.push_back(parent);
  auto child = make_emitter("chd", 0, 0, 0, {xyz(0x08, 99, 0, 0), val(0x0F, 50)});
  child.flags = 4;
  fx.emitters.push_back(child);
  effects::EffectBank bank;
  bank.add(&fx);
  effects::EmitterSystem sys(bank);
  sys.register_all();
  CHECK(!sys.header_running(0));
  for (int tick = 0; tick < 5; ++tick) sys.tick();
  CHECK(sys.live_objects() == 0);
  CHECK(sys.start("par"));
  sys.tick();
  sys.tick();
  CHECK(sys.live_objects() == 1);
  for (int tick = 0; tick < 6; ++tick) sys.tick();
  bool found_child = false;
  effects::EmitterSystem::ObjectView view;
  for (size_t i = 0; i < sys.live_objects(); ++i)
    if (sys.object_view(i, view) && view.owner == 1) found_child = std::fabs(view.pos[0] - 20.0f) < 1.0f;
  CHECK(found_child);
}

// Ramps: scale rate (15) and colour rate (22) accumulate per tick; the oscillator (A = 100, F = 16, P = 0) is A * sin(...) with frame 0 at
// spawn so the position stays within the amplitude; the trig tables return 4096 at a quarter turn.
void test_ramps_and_osc() {
  assets::RoomEffects fx = make_pack();
  assets::FxOsc osc;
  osc.axis[0] = {100, 16, 0};
  fx.emitters.push_back(make_emitter("rmp", 0, 0, 1000,
                                     {xyz(0x08, 0, 0, 0), mk(0x09, osc), xyz(0x12, 0x1000, 0x1000, 0x1000), xyz(0x15, 8, 0, 0),
                                      xyz(0x20, 0x400, 0x400, 0x400), xyz(0x22, 0x10, 0, 0), val(0x0F, 100)}));
  effects::EffectBank bank;
  bank.add(&fx);
  effects::EmitterSystem sys(bank);
  sys.register_all();
  sys.start("rmp");
  sys.tick();
  sys.tick();
  effects::EmitterSystem::ObjectView view;
  CHECK(sys.live_objects() == 1 && sys.object_view(0, view));
  const int scale0 = view.scale[0], colour0 = view.color[0];
  for (int tick = 0; tick < 10; ++tick) sys.tick();
  CHECK(sys.object_view(0, view));
  CHECK(view.scale[0] - scale0 == 8 * 10);
  CHECK(view.color[0] - colour0 == 0x10 * 10);
  CHECK(std::fabs(view.pos[0]) <= 100.5f);
  CHECK(effects::rsin(0) == 0 && effects::rsin(1024) == 4096 && effects::rsin(3072) == -4096);
  CHECK(effects::rcos(0) == 4096 && effects::rcos(2048) == -4096);
}

// Easing: mode 0 is reflected at bit 12 (a triangle wave), modes 1..4 map 0 -> 0 and 4096 -> 4096 with mode 1 monotonic; the path curve
// sampler interpolates between keys, clamps at the last key and extrapolates before the first.
void test_easing_and_curve() {
  using effects::ease;
  CHECK(ease(1000, 0) == 1000 && ease(4096, 0) == 4096);
  CHECK(ease(0x1000 + 0x400, 0) == 0x1000 - 0x400);
  CHECK(ease(0, 1) == 0 && ease(4096, 1) == 4096 && ease(2048, 1) == effects::rsin(512));
  CHECK(ease(0, 2) == 0 && ease(4096, 2) == 4096);
  CHECK(ease(0, 3) == 0 && ease(4096, 3) == 4096);
  CHECK(ease(0, 4) == 0 && ease(4096, 4) == 4096 && ease(2048, 4) == 0x800 - effects::rcos(1024) / 2);
  int prev = -1;
  bool mono = true;
  for (int time = 0; time <= 4096; time += 64) {
    const int eased = ease(time, 1);
    mono = mono && eased >= prev;
    prev = eased;
  }
  CHECK(mono);
  effects::Curve curve;
  curve.keys = {{0, 0, 0, 0}, {100, 0, -50, 2048}, {100, 40, -50, 4096}};
  curve.valid = true;
  auto at_1024 = curve.sample(1024);
  CHECK(std::fabs(at_1024[0] - 50.0f) < 1e-3f && std::fabs(at_1024[2] + 25.0f) < 1e-3f);
  auto at_3072 = curve.sample(3072);
  CHECK(std::fabs(at_3072[1] - 20.0f) < 1e-3f);
  CHECK(curve.sample(9999)[1] == 40.0f);
  CHECK(curve.sample(-1024)[0] < 0.0f);
}

// Blend arithmetic of the GPU modes and the colour modulation; a rasterised 8x8 white sprite at 1:1 over a grey backdrop, additive with
// modulation 0x40 (255 * 0x40 / 0x80 = 127), and the subtractive pass that brings it back.
void test_blend_arithmetic() {
  using effects::blend_channel;
  using effects::modulate;
  CHECK(modulate(200, 0x80) == 200 && modulate(200, 0x40) == 100 && modulate(200, 0xFF) == 255);
  CHECK(blend_channel(0, 50, 77) == 77);
  CHECK(blend_channel(1, 100, 60) == 80);
  CHECK(blend_channel(2, 100, 60) == 160 && blend_channel(2, 200, 100) == 255);
  CHECK(blend_channel(3, 100, 60) == 40 && blend_channel(3, 30, 60) == 0);
  CHECK(blend_channel(4, 100, 60) == 115 && blend_channel(4, 250, 100) == 255);
  assets::EffectSprite sprite;
  sprite.w = 8;
  sprite.h = 8;
  sprite.rgba.assign(8 * 8 * 4, 255);
  effects::DrawQuad quad;
  quad.sprite = &sprite;
  const float xs[4] = {4, 12, 4, 12}, ys[4] = {4, 4, 12, 12};
  for (int i = 0; i < 4; ++i) {
    quad.x[i] = xs[i];
    quad.y[i] = ys[i];
  }
  quad.blend = 2;
  quad.r = quad.g = quad.b = 0x40;
  effects::SoftCanvas canvas(16, 16);
  Bytes grey(16 * 16 * 4, 0);
  for (size_t i = 0; i < grey.size(); i += 4) grey[i] = grey[i + 1] = grey[i + 2] = 100, grey[i + 3] = 255;
  canvas.blit(grey.data(), 16, 16, 0, 0);
  CHECK(canvas.draw(quad) == 64);
  auto px = [&](int x, int y) { return canvas.rgba()[(static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4]; };
  CHECK(px(8, 8) == 100 + 127 && px(2, 2) == 100 && px(13, 13) == 100);
  quad.blend = 3;
  canvas.draw(quad);
  CHECK(px(8, 8) == 100);
}

// OT slot rule: slot = (z >> 4) + bias (z 2400 -> 150, bias 1), or the absolute value with flag bit 23; the farther slot is drawn first; an
// 8x8 sprite (x8 units = 64) at z = 2400 with H = 512 projects 64 * 512 / 2400 px wide, centred at (160, 120).
void test_ot_slot() {
  assets::RoomEffects fx = make_pack();
  auto flagged = [&](const char* name, int bias, u32 flags) {
    fx.emitters.push_back(make_emitter(name, 0, 0, 1000,
                                       {mk(0x06, assets::FxFlags{flags}), val(0x03, bias), xyz(0x08, 0, 0, 800),
                                        xyz(0x12, 0x1000, 0x1000, 0x1000), val(0x0F, 100)}));
  };
  flagged("nrm", 1, 2);
  flagged("fix", 2500, 2 | (1u << 23));
  effects::EffectBank bank;
  bank.add(&fx);
  effects::EmitterSystem sys(bank);
  sys.register_all();
  sys.start("nrm");
  sys.start("fix");
  for (int tick = 0; tick < 3; ++tick) sys.tick();
  effects::ViewParams view;
  view.trans = {0, 0, 1600};
  std::vector<effects::DrawQuad> quads;
  sys.collect_quads(view, 1.0f, quads);
  CHECK(quads.size() == 2);
  if (quads.size() != 2) return;
  int normal = -1, fixed = -1;
  for (const auto& quad : quads) (quad.slot == 2500 ? fixed : normal) = quad.slot;
  CHECK(normal == (800 + 1600) / 16 + 1);
  CHECK(fixed == 2500);
  effects::sort_draw_order(quads);
  CHECK(quads[0].slot == 2500 && quads[1].slot == 151);
  const float w = quads[1].x[1] - quads[1].x[0];
  CHECK(std::fabs(w - 64.0f * 512.0f / 2400.0f) < 0.05f);
  CHECK(std::fabs((quads[1].x[0] + quads[1].x[1]) * 0.5f - 160.0f) < 0.05f);
}

// Sequence player: the camera / shake hooks and START_EMITTER run in the first tick (waits only follow), the 4-tick shake ramp and the
// 10-tick ambient fade (ticks at +4 of the command, rgb target at +8) end exactly on their targets, END comes after 5 + 3 ticks of waits
// and the task is gone afterwards; an unknown sequence does not start.
void test_sequence_player() {
  assets::RoomEffects fx = make_pack();
  auto emitter = make_emitter("fx01", 0, 0, 0, {xyz(0x08, 0, 0, 0), val(0x0F, 50)});
  emitter.flags = 4;
  fx.emitters.push_back(emitter);
  assets::Sequence seq;
  seq.name = "main";
  auto cmd = [](u16 op, u16 len, u16 wait, const char* name, std::vector<u8> data) {
    assets::SeqCmd command;
    command.op = op;
    command.len = len;
    command.wait = wait;
    command.name = name;
    command.data = std::move(data);
    command.data.resize(len - 4u, 0);
    return command;
  };
  std::vector<u8> fade(12, 0);
  fade[0] = 10;
  fade[4] = 100, fade[5] = 50, fade[6] = 0;
  std::vector<u8> shake(8, 0);
  shake[0] = 4;
  shake[4] = 6;
  shake[5] = 2;
  seq.cmds = {cmd(0x65, 16, 0, "", fade),      cmd(0x2E, 12, 0, "", shake), cmd(0x23, 16, 0, "cam1", {}),
              cmd(0x22, 16, 5, "fx01", {}),    cmd(0x20, 8, 3, "", {}),     cmd(0x21, 8, 0, "", {})};
  fx.sequences.push_back(seq);
  effects::EffectBank bank;
  bank.add(&fx);
  effects::EmitterSystem sys(bank);
  sys.register_all();
  effects::SequencePlayer player(bank, sys);
  std::string cam;
  int shake_ax = -1;
  effects::SequenceHooks hooks;
  hooks.camera = [&](std::string_view name, const assets::EffectBlob*) { cam = std::string(name); };
  hooks.shake = [&](int ax, int, int) { shake_ax = ax; };
  player.set_hooks(hooks);
  CHECK(player.start("main"));
  CHECK(cam == "cam1" && shake_ax == 6);
  CHECK(player.active() == 1);
  CHECK(player.running("main") && !player.running("enct"));
  CHECK(sys.header_running(0));
  int r, g, b;
  player.ambient_rgb(r, g, b);
  CHECK(r == 0);
  int ax, ay;
  for (int tick = 0; tick < 4; ++tick) player.tick();
  player.shake_amplitude(ax, ay);
  CHECK(ax == 6 && ay == 2);
  for (int tick = 0; tick < 6; ++tick) player.tick();
  player.ambient_rgb(r, g, b);
  CHECK(r == 100 && g == 50 && b == 0);
  CHECK(player.active() == 0);
  CHECK(!player.running("main"));
  CHECK(!player.start("nope"));
}

// The name "0000": start (field B6) looks it up as a name and starts nothing; kill (field 9C / E7) stops every header and frees every
// object. The repeating 'loop' sequence restarts at its end; stop(name) leaves it alone, stop_all (field D7 "0000") unlinks it as well
// and ends the shake.
void test_every_name() {
  assets::RoomEffects fx = make_pack();
  auto waiting = make_emitter("wai1", 0, 0, 0, {xyz(0x08, 0, 0, 0), val(0x0F, 500)});
  waiting.flags = 4;
  fx.emitters.push_back(waiting);
  fx.emitters.push_back(make_emitter("run1", 0, 0, 2, {xyz(0x08, 0, 0, 0), val(0x0F, 500)}));
  auto cmd = [](u16 op, u16 len, u16 wait, const char* name, std::vector<u8> data) {
    assets::SeqCmd command;
    command.op = op;
    command.len = len;
    command.wait = wait;
    command.name = name;
    command.data = std::move(data);
    command.data.resize(len - 4u, 0);
    return command;
  };
  assets::Sequence loop;
  loop.name = "loop";
  loop.cmds = {cmd(0x20, 8, 30, "", {}), cmd(0x21, 8, 0, "", {})};
  fx.sequences.push_back(loop);
  assets::Sequence other;
  other.name = "shk1";
  std::vector<u8> shake(8, 0);
  shake[4] = 6;
  other.cmds = {cmd(0x2E, 12, 100, "", shake), cmd(0x21, 8, 0, "", {})};
  fx.sequences.push_back(other);
  effects::EffectBank bank;
  bank.add(&fx);
  effects::EmitterSystem sys(bank);
  sys.register_all();
  CHECK(!sys.start("0000"));
  CHECK(!sys.header_running(0) && sys.header_running(1));
  for (int tick = 0; tick < 4; ++tick) sys.tick();
  CHECK(sys.live_objects() > 0);
  CHECK(sys.kill("0000"));
  sys.tick();
  CHECK(!sys.header_running(1) && sys.live_objects() == 0);
  effects::SequencePlayer player(bank, sys);
  CHECK(player.start("loop") && player.start("shk1"));
  for (int tick = 0; tick < 40; ++tick) player.tick();
  CHECK(player.running("loop") && player.running("shk1"));
  player.stop("shk1");
  CHECK(player.running("loop") && !player.running("shk1"));
  int ax = 0, ay = 0;
  player.shake_amplitude(ax, ay);
  CHECK(ax == 6);
  player.stop_all();
  player.shake_amplitude(ax, ay);
  CHECK(player.active() == 0 && ax == 0);
}

// Nested type-0x01 pack (effect/particle.prd > fenc): import::nested_pack returns the inner drp by name, nothing for an entry of another
// type or an unknown name, and the inner drp loads as a battle effect pack.
void test_nested_pack() {
  const Bytes inner = make_drp({{"enct", 0x10, make_sequence()}});
  const Bytes outer = make_drp({{"sprt", 0x03, make_timinfo(0x8001, 0x3C0, 0x100, 0x100, 0x1F0)}, {"fenc", 0x01, inner}});
  const Bytes got = import::nested_pack(outer, "fenc");
  CHECK(got.size() >= inner.size() && std::equal(inner.begin(), inner.end(), got.begin()));
  CHECK(import::nested_pack(outer, "sprt").empty());
  CHECK(import::nested_pack(outer, "none").empty());
  import::Vram empty;
  const assets::RoomEffects fx = import::load_battle_effects(got, {}, empty);
  CHECK(fx.find_sequence("enct") != nullptr);
}

// A closed strip of `segments` quads between the radii 80 (colour 0) and 100 (colour 255); the first `open_gap` quads are left out.
std::vector<effects::SmoothQuad> make_ring(int segments, int open_gap) {
  std::vector<effects::SmoothQuad> quads;
  const float kPi = 3.14159265f;
  auto point = [&](int index, float radius, float colour) {
    effects::SmoothVert vert{};
    vert.p[0] = radius * std::cos(2 * kPi * static_cast<float>(index) / static_cast<float>(segments));
    vert.p[1] = radius * std::sin(2 * kPi * static_cast<float>(index) / static_cast<float>(segments));
    vert.c[0] = vert.c[1] = vert.c[2] = colour;
    return vert;
  };
  for (int i = 0; i < segments; ++i) {
    if (i < open_gap) continue;
    effects::SmoothQuad quad;
    quad.tag = i;
    quad.v[0] = point(i, 80, 0), quad.v[1] = point(i, 100, 255), quad.v[2] = point(i + 1, 100, 255), quad.v[3] = point(i + 1, 80, 0);
    quads.push_back(quad);
  }
  return quads;
}

void test_strip_smooth() {
  const effects::SmoothProject proj = [](const float point[3], float xy[2]) {
    xy[0] = point[0] * 4.0f, xy[1] = point[1] * 4.0f;
    return true;
  };
  auto ring = make_ring(32, 0);
  const auto orig = ring;
  CHECK(effects::smooth_quad_rings(ring, 6.0f, proj));
  CHECK(ring.size() > orig.size() * 3);
  double worst = 0;
  for (const auto& quad : ring)
    for (int corner = 0; corner < 4; ++corner) {
      const double radius = std::hypot(quad.v[corner].p[0], quad.v[corner].p[1]);
      // Colours lerp along the loop only, so each rail keeps its own.
      const bool outer = quad.v[corner].c[0] > 127.5f ? true : quad.v[corner].c[0] > 0.5f;
      (void)outer;
      const double ref = (corner == 0 || corner == 3) ? 80.0 : 100.0;
      worst = std::max(worst, std::fabs(radius - ref) / ref);
      CHECK(quad.v[corner].c[0] == (corner == 0 || corner == 3 ? 0.0f : 255.0f));
    }
  CHECK(worst < 0.005);
  // every original vertex is still a vertex
  for (const auto& orig_quad : orig)
    for (int corner = 0; corner < 4; ++corner) {
      bool found = false;
      for (const auto& quad : ring)
        for (int other = 0; other < 4; ++other)
          found |= std::fabs(quad.v[other].p[0] - orig_quad.v[corner].p[0]) < 1e-3f && std::fabs(quad.v[other].p[1] - orig_quad.v[corner].p[1]) < 1e-3f;
      CHECK(found);
    }
  // two bands sharing the middle rail (the contact ring: 3 radii, the middle quads share three edges): both bands refine, rails agree
  {
    std::vector<effects::SmoothQuad> two;
    const float kPi = 3.14159265f;
    auto pt = [&](int i, float radius) {
      effects::SmoothVert vert{};
      vert.p[0] = radius * std::cos(2 * kPi * static_cast<float>(i) / 16.0f);
      vert.p[1] = radius * std::sin(2 * kPi * static_cast<float>(i) / 16.0f);
      vert.c[0] = radius;
      return vert;
    };
    for (int band = 0; band < 2; ++band)
      for (int i = 0; i < 16; ++i) {
        const float r0 = band ? 100.0f : 80.0f, r1 = band ? 120.0f : 100.0f;
        effects::SmoothQuad quad;
        quad.v[0] = pt(i, r0), quad.v[1] = pt(i, r1), quad.v[2] = pt(i + 1, r1), quad.v[3] = pt(i + 1, r0);
        two.push_back(quad);
      }
    CHECK(effects::smooth_quad_rings(two, 6.0f, proj));
    CHECK(two.size() >= 32 * 4);
    double worst_rail = 0;
    for (const auto& quad : two)
      for (int corner = 0; corner < 4; ++corner) {
        const double radius = std::hypot(quad.v[corner].p[0], quad.v[corner].p[1]);
        // Rails at 80 / 100 / 120.
        const double ref = std::round(radius / 20.0) * 20.0;
        worst_rail = std::max(worst_rail, std::fabs(radius - ref) / ref);
      }
    CHECK(worst_rail < 0.01);
    // watertight: every refined vertex on the shared middle rail (r = 100) occurs for both bands at the same place
    int mid_hits = 0, mid_exact = 0;
    for (const auto& quad : two)
      for (int corner = 0; corner < 4; ++corner) {
        if (std::fabs(std::hypot(quad.v[corner].p[0], quad.v[corner].p[1]) - 100.0) > 1.0) continue;
        ++mid_hits;
        int same = 0;
        for (const auto& other_quad : two)
          for (int other = 0; other < 4; ++other) same += std::fabs(other_quad.v[other].p[0] - quad.v[corner].p[0]) < 1e-4f && std::fabs(other_quad.v[other].p[1] - quad.v[corner].p[1]) < 1e-4f;
        // Two quads (one per band) each use it as the end of one sub-quad and the start of the next (2 + 2).
        mid_exact += same >= 4;
      }
    CHECK(mid_hits > 0 && mid_exact == mid_hits);
  }
  // the contact ring's real layout: radii 1342 / 2104 / 2348, 16 segments, two bands as triangle pairs (in0 in1 out0)(out0 in1 out1); the shared
  // middle rail must come out identical for both bands (no cracks)
  {
    const float kPi = 3.14159265f;
    auto pt = [&](int i, float radius) {
      effects::SmoothVert vert{};
      vert.p[0] = std::round(radius * std::cos(2 * kPi * static_cast<float>(i) / 16.0f));
      vert.p[1] = -std::round(radius * std::sin(2 * kPi * static_cast<float>(i) / 16.0f));
      vert.c[2] = radius / 10.0f;
      return vert;
    };
    std::vector<effects::SmoothVert> tris;
    const float rr[3] = {1342, 2104, 2348};
    for (int band = 0; band < 2; ++band)
      for (int i = 0; i < 16; ++i) {
        const auto in0 = pt(i, rr[band]), in1 = pt(i + 1, rr[band]), o0 = pt(i, rr[band + 1]), o1 = pt(i + 1, rr[band + 1]);
        for (const auto& vert : {in0, in1, o0, o0, in1, o1}) tris.push_back(vert);
      }
    const effects::SmoothProject big = [](const float point[3], float xy[2]) {
      xy[0] = point[0] * 0.2f, xy[1] = point[1] * 0.2f;
      return true;
    };
    CHECK(effects::smooth_triangle_rings(tris, 6.0f, big));
    // collect the vertices near the middle rail per band: a vertex of band 0 triangles lies at r <= 2104 + small, band 1 at r >= 2104 - small
    std::vector<std::pair<float, float>> a0, a1;
    for (size_t i = 0; i < tris.size(); i += 3) {
      float rmax = 0, rmin = 1e9f;
      for (int corner = 0; corner < 3; ++corner) {
        const float radius = std::hypot(tris[i + corner].p[0], tris[i + corner].p[1]);
        rmax = std::max(rmax, radius), rmin = std::min(rmin, radius);
      }
      for (int corner = 0; corner < 3; ++corner) {
        const float radius = std::hypot(tris[i + corner].p[0], tris[i + corner].p[1]);
        if (std::fabs(radius - 2104.0f) > 30.0f) continue;
        (rmax < 2200 ? a0 : a1).push_back({tris[i + corner].p[0], tris[i + corner].p[1]});
      }
    }
    int unmatched = 0;
    for (const auto& point : a0) {
      bool matched = false;
      for (const auto& other : a1) matched |= std::fabs(point.first - other.first) < 1e-4f && std::fabs(point.second - other.second) < 1e-4f;
      unmatched += !matched;
    }
    CHECK(!a0.empty() && !a1.empty() && unmatched == 0);
  }
  // a ring whose inner rail is shared with a plain fan (the disc enc0) is left alone: refining it would open a crack against the fan's chords
  {
    std::vector<effects::SmoothVert> tris;
    for (const auto& quad : make_ring(16, 0))
      for (int i : {0, 1, 2, 0, 2, 3}) tris.push_back(quad.v[i]);
    auto ring_pt = [&](int i) {
      effects::SmoothVert vert{};
      vert.p[0] = 80.0f * std::cos(2 * 3.14159265f * static_cast<float>(i) / 16.0f);
      vert.p[1] = 80.0f * std::sin(2 * 3.14159265f * static_cast<float>(i) / 16.0f);
      return vert;
    };
    for (int i = 0; i < 16; ++i) {
      effects::SmoothVert centre{};
      tris.push_back(centre), tris.push_back(ring_pt(i)), tris.push_back(ring_pt(i + 1));
    }
    const size_t count_before = tris.size();
    CHECK(!effects::smooth_triangle_rings(tris, 6.0f, proj));
    CHECK(tris.size() == count_before);
  }
  // an open strip (one quad missing) and a lone quad are left alone
  auto open = make_ring(32, 1);
  const size_t n_open = open.size();
  CHECK(!effects::smooth_quad_rings(open, 6.0f, proj));
  CHECK(open.size() == n_open);
  auto lone = make_ring(32, 31);
  CHECK(!effects::smooth_quad_rings(lone, 6.0f, proj));
  CHECK(lone.size() == 1);
  // already short enough on screen: unchanged
  auto small = make_ring(32, 0);
  CHECK(!effects::smooth_quad_rings(small, 1000.0f, proj));
  // triangle list: the same ring as 64 triangles, plus a stray triangle
  std::vector<effects::SmoothVert> tv;
  for (const auto& quad : make_ring(32, 0)) {
    for (int i : {0, 1, 2, 0, 2, 3}) tv.push_back(quad.v[i]);
  }
  for (int i = 0; i < 3; ++i) {
    effects::SmoothVert vert{};
    vert.p[0] = 500.0f + static_cast<float>(i * 7);
    vert.p[1] = static_cast<float>(i * i * 3);
    tv.push_back(vert);
  }
  CHECK(effects::smooth_triangle_rings(tv, 6.0f, proj));
  CHECK(tv.size() % 3 == 0 && tv.size() > 64 * 3 * 3);
  // The stray triangle comes first, untouched.
  CHECK(tv[0].p[0] == 500.0f);
  std::vector<effects::SmoothVert> tri_only(tv.begin(), tv.begin() + 3);
  CHECK(!effects::smooth_triangle_rings(tri_only, 6.0f, proj));
}

void test_rot_order() {
  const int ax = 300, ay = 700, az = 1500;
  const float sx = effects::rsin(ax) / 4096.0f, cx = effects::rcos(ax) / 4096.0f;
  const float sy = effects::rsin(ay) / 4096.0f, cy = effects::rcos(ay) / 4096.0f;
  const effects::Mat3 zyx = effects::Mat3::from_angles(ax, ay, az);
  CHECK(std::fabs(zyx.m[2] - sy) < 1e-5f && std::fabs(zyx.m[5] + cy * sx) < 1e-5f && std::fabs(zyx.m[8] - cy * cx) < 1e-5f);
  const effects::Mat3 xyz_order = effects::Mat3::from_angles(ax, ay, az, true);
  CHECK(std::fabs(xyz_order.m[6] + sy) < 1e-5f && std::fabs(xyz_order.m[7] - cy * sx) < 1e-5f && std::fabs(xyz_order.m[8] - cy * cx) < 1e-5f);
  for (int facing : {0, 0x200, 0x400, 0x900}) {
    const effects::Mat3 ring = effects::Mat3::from_angles(1024, 0, -facing).scaled_columns(1.5f, 1.5f, 0.0f);
    for (const effects::Vec3 corner : {effects::Vec3{-128, -128, 0}, effects::Vec3{120, -128, 0}, effects::Vec3{-128, 120, 0}})
      CHECK(std::fabs(ring.apply(corner)[1]) < 1e-3f);
  }
  // tel_b11i nm01 / nm02 (sea waves seen through the window): angle (-1024, 0, 1024), world quad -> flat on the sea (both sprite axes
  // horizontal), the sprite's x axis along world -z (the crest), moving along -x. Rz*Rx stood them upright.
  const effects::Mat3 wave = effects::Mat3::from_angles(-1024, 0, 1024);
  CHECK(std::fabs(wave.m[3]) < 1e-3f && std::fabs(wave.m[4]) < 1e-3f && std::fabs(wave.m[6] + 1.0f) < 1e-3f);
}
// Lens flare flash: channel = d / (0x25400 / c), d = 0x25400 - (dx^2 + dy^2), dx = 320 - 2 sx, dy = 224 - 2 sy.
void test_lens_flash() {
  // The light at the centre gives the colour byte itself (the default colour 0x800 >> 4).
  CHECK(effects::lens_flash_channel(0x80, 160, 112) == 0x80);
  CHECK(effects::lens_flash_channel(0xFF, 160, 112) == 0xFF);
  // The corner: d = 0.
  CHECK(effects::lens_flash_channel(0x80, 0, 0) == 0);
  // The middle of the top edge.
  CHECK(effects::lens_flash_channel(0x80, 160, 0) == (0x25400 - 224 * 224) / (0x25400 / 0x80));
  // Widescreen columns beyond the corners clamp to 0.
  CHECK(effects::lens_flash_channel(0x80, -100, 112) == 0);
  CHECK(effects::lens_flash_channel(0, 160, 112) == 0);
  // A 41 % white turn_white flash (c = 0x80) is c * d / 0x25400, so 105 / 255 needs d / 0x25400 = 0.82.
  CHECK(effects::lens_flash_channel(0x80, 160, 112) * 100 / 255 == 50);
  // The wash is gated on the light in the PS1 frame, the ghosts on the light in OUR frame. A wide / high-FOV picture (extra = 100
  // columns per side): a sun at ours x = -40 (PS1 frame x = -10, outside it) still shows its ghosts but no wash.
  CHECK(effects::lens_ghosts_visible(-40, 112, 100));
  // At 4:3 the PS1 test applies.
  CHECK(!effects::lens_ghosts_visible(-40, 112, 0));
  // Outside the PS1 frame there is no wash (lens_flash_channel alone would give > 0 there); inside it is unchanged.
  CHECK(effects::lens_wash_channel(0x80, -10, 112) == 0);
  CHECK(effects::lens_flash_channel(0x80, -10, 112) > 0);
  CHECK(effects::lens_wash_channel(0x80, 160, 112) == 0x80);
  CHECK(effects::lens_wash_channel(0x80, 0x141, 112) == 0 && effects::lens_wash_channel(0x80, 160, -1) == 0);
  CHECK(effects::lens_ghosts_visible(0x140 + 100, 224, 100) && !effects::lens_ghosts_visible(0x140 + 101, 112, 100) && !effects::lens_ghosts_visible(160, 225, 100));
  // A screen-locked (camera-relative) overlay is widened only when it spans the whole 320 px picture, and only in widescreen.
  CHECK(effects::screen_lock_widen(-40, 360, 80) == (320.0f + 160.0f) / 320.0f);
  CHECK(effects::screen_lock_widen(-40, 360, 0) == 1.0f);
  CHECK(effects::screen_lock_widen(10, 360, 80) == 1.0f && effects::screen_lock_widen(-40, 300, 80) == 1.0f);
}
}  // namespace

// Flag bit 28: the six bound points of a mesh and the screen test (dropped only when every point is in a screen corner).
void test_object_screen_test() {
  const auto points = import::mesh_cull_points({{-100, -200, -50}, {300, 40, 70}, {0, 0, 0}});
  CHECK(points[0] == (std::array<s16, 3>{100, -80, 10}));
  CHECK(points[1] == (std::array<s16, 3>{300, 0, 0}) && points[2] == (std::array<s16, 3>{-100, 0, 0}));
  CHECK(points[3] == (std::array<s16, 3>{0, 0, 70}) && points[4] == (std::array<s16, 3>{0, 0, -50}) && points[5] == (std::array<s16, 3>{0, -200, 0}));
  effects::ViewParams view;
  view.h = 256.0f, view.ox = 160.0f, view.oy = 112.0f;
  effects::EmitterSystem::MeshInstance mi;
  mi.sprite_type = 14;
  mi.flags = 1u << 28;
  mi.centre = {0, 0, 1000};
  CHECK(!mi.off_screen(points, view));
  mi.centre = {5000, 5000, 1000};
  CHECK(mi.off_screen(points, view));
  mi.centre = {5000, 0, 1000};
  CHECK(!mi.off_screen(points, view));
  mi.centre = {0, 5000, 1000};
  CHECK(!mi.off_screen(points, view));
  mi.centre = {5000, 5000, 1000};
  mi.flags = 0;
  CHECK(!mi.off_screen(points, view));
  // Other types (here the ground) project their origin only: a screen x past 1023, a depth up to h / 2 or behind the eye drops them.
  mi.flags = 1u << 28;
  mi.sprite_type = 19;
  CHECK(mi.off_screen(points, view) && mi.off_screen(view));
  mi.centre = {5000, 0, 1000};
  CHECK(mi.off_screen(view));
  mi.centre = {3000, 0, 1000};
  CHECK(!mi.off_screen(view) && !mi.off_screen(points, view));
  mi.centre = {0, 0, 128};
  CHECK(mi.off_screen(view));
  mi.centre = {0, 0, 129};
  CHECK(!mi.off_screen(view));
  mi.centre = {0, 0, -10};
  CHECK(mi.off_screen(view));
  mi.centre = {0, 0, 70000};
  CHECK(mi.off_screen(view));
  mi.flags = 0;
  CHECK(!mi.off_screen(view));
  mi.flags = 1u << 28;
  mi.sprite_type = 14;
  CHECK(!mi.off_screen(view));
}

// The object arena: blocks cut from the top downwards, an exact refit takes the freed block and restarts the search at the bottom
// block, freed neighbours merge, flag-10 spawns land at the top, and a full arena refuses.
void test_object_heap() {
  effects::ObjectHeap heap(0x4000);
  CHECK(heap.alloc(0x94) == 0x3F54);
  CHECK(heap.alloc(0x94) == 0x3EB4);
  CHECK(heap.alloc(0x91) == 0x3E14);
  heap.free(0x3F54);
  CHECK(heap.alloc(0x94) == 0x3F54);
  CHECK(heap.alloc(0x94) == 0x3D74);
  heap.free(0x3EB4);
  heap.free(0x3E14);
  CHECK(heap.alloc(0xFC) == 0x3E4C);
  CHECK(heap.alloc(0x94) == 0x3CD4);
  heap.free(0x3D74);
  heap.search_from_top();
  CHECK(heap.alloc(0x10) == 0x3E30);
  heap.reset();
  int count = 0;
  while (heap.alloc(0x94) >= 0) ++count;
  CHECK(count == 102);
}

int main() {
  test_lens_flash();
  test_good();
  test_bad();
  test_stepper_ops();
  test_stored_header_frame();
  test_header_stepper();
  test_overlay_pacing();
  test_start_and_death_child();
  test_ramps_and_osc();
  test_easing_and_curve();
  test_blend_arithmetic();
  test_ot_slot();
  test_sequence_player();
  test_every_name();
  test_nested_pack();
  test_rot_order();
  test_strip_smooth();
  test_object_screen_test();
  test_mark_mesh_objects();
  test_death_child_position();
  test_frame_stepping();
  test_object_heap();
  std::printf(failures ? "%d failure(s)\n" : "ok\n", failures);
  return failures ? 1 : 0;
}
