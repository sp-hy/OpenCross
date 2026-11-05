// Battle presentation logic without rendering: the animation-sequence VM (battle_seq.h) on hand-built scripts and the camera
// shots / timelines (battle_camera.h) on a hand-built drp.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/anim_arc.h"
#include "game/battle/battle_camera.h"
#include "game/battle/battle_numbers.h"
#include "game/battle/battle_seq.h"
#include "game/battle/cast_header.h"
#include "game/battle/status_plates.h"

using namespace oc;
using namespace oc::game;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

bool near(float lhs, float rhs, float eps = 1e-3f) { return std::fabs(lhs - rhs) <= eps; }

void p16(Bytes& out, int value) {
  out.push_back(static_cast<u8>(value & 0xFF));
  out.push_back(static_cast<u8>((value >> 8) & 0xFF));
}
void p32(Bytes& out, u32 value) {
  p16(out, static_cast<int>(value & 0xFFFF));
  p16(out, static_cast<int>(value >> 16));
}

// One-entry table: variant 0 = `v0`, variant 7 = `v7` (words).
Bytes make_block(const std::vector<int>& v0, const std::vector<int>& v7) {
  Bytes block;
  const u32 size = 12 + 16 + 2 * static_cast<u32>(v0.size() + v7.size());
  p32(block, 1);
  p32(block, 12);
  p32(block, size);
  p16(block, 16);
  for (int i = 0; i < 6; ++i) p16(block, 0);
  p16(block, 16 + 2 * static_cast<int>(v0.size()));
  for (int w : v0) p16(block, w);
  for (int w : v7) p16(block, w);
  return block;
}

struct Run {
  // (tick, event) pairs.
  std::vector<std::pair<int, bseq::Event>> ev;
  int done_tick = -1;
};

Run run(const bseq::Table& table, int entry, int variant, const bseq::Ctx& ctx, int ticks = 400) {
  Run result;
  bseq::Vm vm;
  if (!vm.start(&table, entry, variant)) return result;
  std::vector<bseq::Event> out;
  for (int k = 0; k < ticks && vm.running(); ++k) {
    out.clear();
    vm.tick(ctx, out);
    for (const auto& event : out) {
      result.ev.emplace_back(k, event);
      if (event.kind == bseq::Event::Done) result.done_tick = k;
    }
  }
  return result;
}

void test_vm_basic() {
  // SetAnim 128 (blend 4); wait 16; React all; wait 8; end
  bseq::Table table;
  CHECK(table.parse(make_block({0x15, 128, 4, 0x09, 16, 0x5A, 0x09, 8, 0x00}, {0x15, 5, 4, 0x41, 0x00})));
  CHECK(table.entries() == 1);
  // A 12 byte table header + 16 byte entry header = word 14.
  CHECK(table.start(0, 0) == 14);
  CHECK(table.start(0, 7) > 14);
  CHECK(table.start(0, 3) < 0);
  bseq::Ctx ctx;
  Run result = run(table, 0, 0, ctx);
  CHECK(result.ev.size() == 3);
  CHECK(result.ev[0].first == 0 && result.ev[0].second.kind == bseq::Event::SetAnim && result.ev[0].second.a == 128 && result.ev[0].second.b == 4);
  CHECK(result.ev[1].first == 16 && result.ev[1].second.kind == bseq::Event::React && result.ev[1].second.a == -1);
  CHECK(result.done_tick == 24);
  // reaction script: SetAnim 5, CallDamage
  result = run(table, 0, 7, ctx);
  CHECK(result.ev.size() == 3 && result.ev[1].second.kind == bseq::Event::Damage && result.ev[1].first == 0);
}

// GetOption / GetBeta / GetNumberTarget write the running unit's control-block words into their output variables.
void test_vm_unit_queries() {
  bseq::Table table;
  CHECK(table.parse(make_block({0x69, 1, 1, 0xA9, 0, 2, 0xA1, 3, 4, 0x09, 4, 0x00}, {0x00})));
  bseq::Ctx ctx;
  ctx.option = 0x4070;
  ctx.result = 0x20;
  ctx.target_count = 3;
  ctx.target_index = 2;
  bseq::Vm vm;
  CHECK(vm.start(&table, 0, 0));
  std::vector<bseq::Event> out;
  vm.tick(ctx, out);
  CHECK(vm.var(1) == 0x4070);
  CHECK(vm.var(2) == 0x20);
  CHECK(vm.var(3) == 3);
  CHECK(vm.var(4) == 2);
  // The Done event names the op that ended the script (op 0x3E clears the view's move-wait).
  bseq::Table end_table;
  CHECK(end_table.parse(make_block({0x3E}, {0x00})));
  Run result = run(end_table, 0, 0, ctx);
  CHECK(!result.ev.empty() && result.ev.back().second.kind == bseq::Event::Done && result.ev.back().second.a == 0x3E);
}

void test_vm_branch() {
  // GetAction(var1, var2); if var1 == 2 jump to the heavy branch (SetAnim 130) else SetAnim 128; end
  //  0: 43 1 2        GetAction
  //  3: 40 3 2        var3 = 2
  //  6: 6 1 3 0 8     if var1 == var3 fall through else pc += 8 + 2
  // 11: 15 130 0      heavy
  // 14: 7 3           pc = 14 + 3 + 2 = 19
  // 16: (skipped)
  // 16: 15 128 0      light  (reached by the false branch: 6 + 8 + 2 = 16)
  // 19: 0
  std::vector<int> v0 = {0x43, 1, 2, 0x40, 3, 2, 0x06, 1, 3, 0, 8, 0x15, 130, 0, 0x07, 3, 0x15, 128, 0, 0x00};
  bseq::Table table;
  CHECK(table.parse(make_block(v0, {0x00})));
  bseq::Ctx ctx;
  ctx.action_a = 2;
  Run result = run(table, 0, 0, ctx);
  CHECK(!result.ev.empty() && result.ev[0].second.kind == bseq::Event::SetAnim && result.ev[0].second.a == 130);
  ctx.action_a = 0;
  result = run(table, 0, 0, ctx);
  CHECK(!result.ev.empty() && result.ev[0].second.kind == bseq::Event::SetAnim && result.ev[0].second.a == 128);
}

void test_vm_waits() {
  // SetAnim 3 (clip of 9 frames -> (9-1)*4 = 32 ticks); wait until animation end minus 4; MoveS with ticks from the context; wait for the move.
  std::vector<int> v0 = {0x15, 3, 0, 0x42, 4, 0x0C, 0, 64, 0x4D, 4, 0x00};
  bseq::Table table;
  CHECK(table.parse(make_block(v0, {0x00})));
  bseq::Ctx ctx;
  ctx.clip_frames = [](int) { return 9; };
  // A move at the given speed takes 640 / speed ticks (10 here).
  ctx.move_ticks = [](int, int speed) { return 640 / speed; };
  Run result = run(table, 0, 0, ctx);
  int move_tick = -1;
  for (auto& [tick, event] : result.ev)
    if (event.kind == bseq::Event::MoveSpeed) {
      move_tick = tick;
      CHECK(event.c == 10);
    }
  // 32 - 4.
  CHECK(move_tick == 28);
  // MoveS lasts 10 ticks, the wait ends 4 before.
  CHECK(result.done_tick == 28 + 6);
}

// The run-away script (S5 entry 7 of every PC): GetStandPos; if x > 0 { yaw = -1024 (= 3072, faces +x), x = 5000 }
// else { yaw = 1024 (faces -x), x = -5000 }; RotateT over 16 ticks; run clip; PosMoveT over 80 ticks. The party turns its back on the enemies
// and leaves by its own side.
void test_vm_run_away() {
  //  0: 40 3 0        var3 = 0 (rx)
  //  3: 40 4 0        var4 = 0 (rz)
  //  6: 6F 0          GetStandPos -> var0, var1, var2
  //  8: 40 15 0       var15 = 0
  // 11: 06 0 15 2 11  if var0 > var15 fall through else pc = 11 + 11 + 2 = 24
  // 16: 40 5 -1024    var5 = ry
  // 19: 40 0 5000
  // 22: 07 6          pc = 22 + 6 + 2 = 30
  // 24: 40 5 1024
  // 27: 40 0 -5000
  // 30: 11 3 16       RotateT (vars 3, 5, 4) over 16 ticks
  // 33: 15 3 8        run clip
  // 36: 71 0 80       PosMoveT (var0, var1) over 80 ticks
  // 39: 00
  std::vector<int> v0 = {0x40, 3, 0, 0x40, 4, 0, 0x6F, 0, 0x40, 15, 0, 0x06, 0, 15, 2, 11, 0x40, 5, -1024, 0x40, 0, 5000, 0x07, 6,
                         0x40, 5, 1024, 0x40, 0, -5000, 0x11, 3, 16, 0x15, 3, 8, 0x71, 0, 80, 0x00};
  bseq::Table table;
  CHECK(table.parse(make_block(v0, {0x00})));
  for (int side = 0; side < 2; ++side) {
    bseq::Ctx ctx;
    ctx.stand_x = side == 0 ? 2816 : -1280;
    ctx.stand_z = -256;
    Run result = run(table, 0, 0, ctx);
    int yaw = -1, mx = 0, mz = 0, mt = 0;
    for (auto& [tick, event] : result.ev) {
      if (event.kind == bseq::Event::Rotate) {
        yaw = event.a;
        CHECK(event.b == 16);
      }
      if (event.kind == bseq::Event::MovePos) {
        mx = event.a;
        mz = event.b;
        mt = event.d;
      }
    }
    CHECK(yaw == (side == 0 ? 3072 : 1024));
    CHECK(mx == (side == 0 ? 5000 : -5000) && mz == -256 && mt == 80);
  }
}

// Op 6E JumpT (S5 entry 3 = the 16-tick return hop): the target comes from vars [v] x, [v+1] z, [v+2] y (PS1 y down); the event carries
// all three and the apex position (after n / 2 ticks, y = y0 + vy h + g h^2 / 2 with g = 0x9ccc >> 12) goes to the out vars.
void test_vm_jump() {
  // 0: 6F 0 (GetStandPos -> vars 0, 1, 2) ; 2: 40 2 -100 (target y = -100: 100 units above the floor) ; 5: 6E 0 3 16 ; 9: 00
  std::vector<int> v0 = {0x6F, 0, 0x40, 2, -100, 0x6E, 0, 3, 16, 0x00};
  bseq::Table table;
  CHECK(table.parse(make_block(v0, {0x00})));
  bseq::Ctx ctx;
  ctx.stand_x = 2816;
  ctx.stand_z = -256;
  ctx.pos_x = 2000;
  ctx.pos_z = 500;
  ctx.pos_y = 0;
  Run result = run(table, 0, 0, ctx);
  bool seen = false;
  for (auto& [tick, event] : result.ev)
    if (event.kind == bseq::Event::Jump) {
      seen = true;
      CHECK(event.a == 2816 && event.b == -256 && event.c == -100 && event.d == 16);
    }
  CHECK(seen);
  // the apex out vars are private to the VM; re-run through a script that stores [3] into the position of a MovePos
  std::vector<int> v1 = {0x6F, 0, 0x6E, 0, 3, 16, 0x71, 3, 1, 0x00};
  bseq::Table t1;
  CHECK(t1.parse(make_block(v1, {0x00})));
  Run r1 = run(t1, 0, 0, ctx);
  for (auto& [tick, event] : r1.ev)
    if (event.kind == bseq::Event::MovePos) {
      // x, z halfway (2000 -> 2816, 500 -> -256 over 16 ticks, 8 ticks).
      CHECK(event.a == 2408 && event.b == 122);
      // y = vy h + g h^2 / 2 with vy = -g * 8: -32 g = -313.6 (negative = up).
      CHECK(event.c == -314);
    }
}

// Ops 7F / 80 JumpUpT / JumpDownT (Hair-Cutter's leap to y -2048, every reaction lift of a pack's variant 7) and 81 GetELPos:
// the target from vars [v] x, [v+1] z, [v+2] y, duration = word 2; the event is the same Jump with tag 1 / 2.
void test_vm_jump_up_down() {
  // y -= 768 ; JumpUp 24 ; y += 768 ; JumpDown 12.
  std::vector<int> v0 = {0x6F, 0, 0x40, 3, 768, 0x29, 2, 3, 0x7F, 0, 24, 0x28, 2, 3, 0x80, 0, 12, 0x00};
  bseq::Table table;
  CHECK(table.parse(make_block(v0, {0x00})));
  bseq::Ctx ctx;
  ctx.stand_x = 100;
  ctx.stand_z = 200;
  Run result = run(table, 0, 0, ctx);
  int jumps = 0;
  for (auto& [tick, event] : result.ev)
    if (event.kind == bseq::Event::Jump) {
      if (jumps == 0) CHECK(event.tag == 1 && event.a == 100 && event.b == 200 && event.c == -768 && event.d == 24);
      if (jumps == 1) CHECK(event.tag == 2 && event.c == 0 && event.d == 12);
      ++jumps;
    }
  CHECK(jumps == 2);
  // GetELPos target 0 attach 16 -> vars 6..8, then PosMoveT to them.
  std::vector<int> v1 = {0x81, 1, 0, 16, 6, 0x71, 6, 1, 0x00};
  bseq::Table t1;
  CHECK(t1.parse(make_block(v1, {0x00})));
  bseq::Ctx c1;
  c1.get_attach = [](int list, int idx, int point, int& x, int& z, int& y) {
    x = 10 * point + list;
    z = 20 + idx;
    y = -300;
    return true;
  };
  Run r1 = run(t1, 0, 0, c1);
  bool seen = false;
  for (auto& [tick, event] : r1.ev)
    if (event.kind == bseq::Event::MovePos) {
      seen = true;
      CHECK(event.a == 161 && event.b == 20 && event.c == -300);
    }
  CHECK(seen);
}

void test_tags() {
  CHECK(bseq::tag_str(bseq::tag_of(0x735F, 0x3030)) == "_s00");
  CHECK(bseq::tag_str(bseq::tag_of(29793, 12907)) == "atk2");
}

// ----------------------------------------------------------------------------------------------------------- camera
Bytes entry(const char* name, int type, const Bytes& payload) {
  Bytes drp_entry = {0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) drp_entry.push_back(static_cast<u8>(name[i]));
  drp_entry.push_back(static_cast<u8>(type));
  const u32 len = static_cast<u32>(payload.size()) * 16;
  drp_entry.push_back(static_cast<u8>(len & 0xFF));
  drp_entry.push_back(static_cast<u8>((len >> 8) & 0xFF));
  drp_entry.push_back(static_cast<u8>((len >> 16) & 0xFF));
  drp_entry.insert(drp_entry.end(), payload.begin(), payload.end());
  return drp_entry;
}

Bytes make_camera(int duration, int mode, int w8_hi) {
  Bytes camera;
  p32(camera, 0);
  p16(camera, duration);
  p16(camera, mode);
  p32(camera, static_cast<u32>(w8_hi) << 16);
  // The ref word, then the key count.
  p32(camera, 0);
  p32(camera, 2);
  auto key = [&](int ex, int ey, int ez, int h, int tx, int ty, int tz) {
    p32(camera, 0);
    for (int word : {ex, ey, ez, h, tx, ty, tz, 0}) p16(camera, word);
  };
  key(0, 0, 1000, 512, 0, 0, 0);
  key(1000, -200, 1000, 400, 0, 0, 0);
  return camera;
}

void put_cmd(Bytes& out, int op, int delay, const char* name = nullptr, int len = 8) {
  p32(out, static_cast<u32>(op) | (static_cast<u32>(len) << 9) | (static_cast<u32>(delay) << 16));
  if (len > 4) {
    p32(out, 0);
    if (len > 8) {
      p32(out, 0);
      for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(name ? name[i] : 0));
    }
  }
}

Bytes make_drp(const std::vector<Bytes>& entries) {
  Bytes drp = {'d', 'r', 'p', 0, 0, 0, 0, 0};
  p16(drp, static_cast<int>(entries.size()) * 64);
  p16(drp, 0);
  u32 off = 12 + 4 * static_cast<u32>(entries.size());
  for (const Bytes& drp_entry : entries) {
    p32(drp, off);
    off += static_cast<u32>(drp_entry.size());
  }
  for (const Bytes& drp_entry : entries) drp.insert(drp.end(), drp_entry.begin(), drp_entry.end());
  return drp;
}

void test_camera() {
  CHECK(near(BattleCamera::ease(0, 0.3f), 0.3f));
  CHECK(near(BattleCamera::ease(1, 1.0f), 1.0f) && near(BattleCamera::ease(1, 0.0f), 0.0f));
  CHECK(near(BattleCamera::ease(4, 0.5f), 0.5f) && BattleCamera::ease(4, 0.25f) < 0.25f);
  CHECK(near(BattleCamera::ease(3, 0.5f), 0.5f));
  CHECK(near(BattleCamera::ease(2, 1.0f), 1.0f));

  Bytes seq(0x14, 0);
  put_cmd(seq, 0x20, 0);
  put_cmd(seq, 0x23, 5, "cam1", 16);
  // A choice group: cam2 and cam3 (delay 0, then 12).
  put_cmd(seq, 0x23, 0, "cam2", 16);
  put_cmd(seq, 0x23, 12, "cam3", 16);
  put_cmd(seq, 0x21, 0);
  CamBank bank;
  bank.add_drp(make_drp({entry("cam1", 0x0C, make_camera(100, 0, 0x100)), entry("cam2", 0x0C, make_camera(50, 4, 0x100)),
                         entry("cam3", 0x0C, make_camera(50, 4, 0x100)), entry("t1  ", 0x10, seq)}));
  CHECK(bank.cameras() == 3);
  CHECK(bank.sequence("t1  ") != nullptr);
  CHECK(bank.camera("cam1")->keys.size() == 2);
  // File y -200 = up 200.
  CHECK(near(bank.camera("cam1")->keys[1].eye[1], 200.0f));

  BattleCamera cam;
  cam.set_bank(&bank);
  CHECK(cam.select("cam1"));
  CamEvents ev;
  cam.tick(ev);
  // Duration 100: the whole step (0x40000 / 100) >> 6 = 40 of 4096 per tick.
  CHECK(BattleCamera::progress_step(*bank.camera("cam1")) == 40);
  CHECK(near(cam.pose(1.0f).eye[0], 1000.0f * (40.0f / 4096.0f), 0.2f));
  for (int i = 0; i < 101; ++i) cam.tick(ev);
  CHECK(cam.task_running());
  cam.tick(ev);
  CHECK(near(cam.pose(1.0f).eye[0], 1000.0f, 1.0f) && near(cam.pose(1.0f).h, 400.0f, 0.5f));
  CHECK(!cam.task_running());

  // timeline: cam1 at tick 0 (delay 5), then the group {cam2, cam3} after the delay 5; the sequence ends 12 ticks later
  CHECK(cam.play_sequence("t1  "));
  CHECK(cam.sequence_running());
  cam.tick(ev);
  CHECK(cam.shot_name() == "cam1");
  for (int i = 0; i < 3; ++i) cam.tick(ev);
  CHECK(cam.shot_name() == "cam1" && cam.sequence_running());
  // The 5th call: the delay of 5 counts the tick the command ran in.
  cam.tick(ev);
  CHECK(cam.shot_name() == "cam2" || cam.shot_name() == "cam3");
  for (int i = 0; i < 30; ++i) cam.tick(ev);
  CHECK(!cam.sequence_running());

  // A newer shot pushes the running one back, which takes over again at its own progress when the newer one ends.
  BattleCamera pushed;
  pushed.set_bank(&bank);
  CHECK(pushed.select("cam1"));
  for (int i = 0; i < 10; ++i) pushed.tick(ev);
  CHECK(pushed.select("cam2"));
  for (int i = 0; i < 55; ++i) pushed.tick(ev);
  CHECK(pushed.task_running() && pushed.pose(1.0f).eye[0] < 900.0f);
  // After retire_tasks (an effect pack starts) the next select unlinks it: the pose holds the newer shot's last key.
  BattleCamera retired;
  retired.set_bank(&bank);
  CHECK(retired.select("cam1"));
  for (int i = 0; i < 10; ++i) retired.tick(ev);
  retired.retire_tasks();
  CHECK(retired.select("cam2"));
  for (int i = 0; i < 55; ++i) retired.tick(ev);
  CHECK(!retired.task_running() && near(retired.pose(1.0f).eye[0], 1000.0f, 1.0f));
}

// The ticks (1-based) on which a timeline played its 0x4C sound, and the first tick after which it no longer runs.
struct TimelineTrace {
  std::vector<int> sound_ticks;
  int end_tick = 0;
};
TimelineTrace trace_timeline(BattleCamera& cam, const std::string& name, int ticks) {
  TimelineTrace trace;
  CHECK(cam.play_sequence(name));
  for (int tick = 1; tick <= ticks && cam.sequence_running(); ++tick) {
    CamEvents ev;
    cam.tick(ev);
    if (!ev.sounds.empty()) trace.sound_ticks.push_back(tick);
    if (!cam.sequence_running()) trace.end_tick = tick;
  }
  return trace;
}

void test_camera_timeline_flow() {
  // Per-target loop (header flag 1): 0x4E, sound (delay 1), 0x4D (delay 5), end.
  Bytes loop(0x14, 0);
  loop[0] = 1;
  put_cmd(loop, 0x20, 0);
  put_cmd(loop, 0x4E, 0);
  put_cmd(loop, 0x4C, 1, "snd1", 16);
  put_cmd(loop, 0x4D, 5);
  put_cmd(loop, 0x21, 0);
  // Per-target restart at the end (header flag 1, no loop marker).
  Bytes restart(0x14, 0);
  restart[0] = 1;
  put_cmd(restart, 0x4C, 0, "snd1", 16);
  put_cmd(restart, 0x21, 0);
  // Op 0x50 (delay 2) stalls while the actor is busy; op 0x67 (delay 9) yields one tick.
  Bytes stall(0x14, 0);
  put_cmd(stall, 0x50, 2);
  put_cmd(stall, 0x4C, 0, "snd1", 16);
  put_cmd(stall, 0x21, 0);
  Bytes yield(0x14, 0);
  put_cmd(yield, 0x67, 9);
  put_cmd(yield, 0x4C, 0, "snd1", 16);
  put_cmd(yield, 0x21, 0);
  // Op 0x2F starts the child timeline, which steps from the next tick on.
  Bytes parent(0x14, 0);
  put_cmd(parent, 0x2F, 0, "chld", 16);
  put_cmd(parent, 0x21, 0);
  Bytes child(0x14, 0);
  put_cmd(child, 0x4C, 0, "snd1", 16);
  put_cmd(child, 0x21, 0);
  CamBank bank;
  bank.add_drp(make_drp({entry("loop", 0x10, loop), entry("rest", 0x10, restart), entry("stal", 0x10, stall), entry("yiel", 0x10, yield),
                         entry("prnt", 0x10, parent), entry("chld", 0x10, child)}));
  CHECK(bank.sequence("loop") && bank.sequence("loop")->flags == 1u);

  BattleCamera cam;
  cam.set_bank(&bank);
  // No target list: one pass.
  TimelineTrace trace = trace_timeline(cam, "loop", 40);
  CHECK(trace.sound_ticks == std::vector<int>{1});
  // Three targets: the jump back runs the body on the next tick; the end is dropped one step after the 0x21.
  cam.set_target_hook([](int index) { return index >= 0 && index < 3; });
  trace = trace_timeline(cam, "loop", 40);
  CHECK((trace.sound_ticks == std::vector<int>{1, 3, 5}) && trace.end_tick == 11);
  cam.set_target_hook([](int index) { return index >= 0 && index < 2; });
  trace = trace_timeline(cam, "rest", 40);
  CHECK((trace.sound_ticks == std::vector<int>{1, 3}) && trace.end_tick == 4);

  int busy_ticks = 3;
  cam.set_actor_busy_hook([&busy_ticks] { return busy_ticks-- > 0; });
  trace = trace_timeline(cam, "stal", 40);
  CHECK(trace.sound_ticks == std::vector<int>{6});
  trace = trace_timeline(cam, "yiel", 40);
  CHECK(trace.sound_ticks == std::vector<int>{2});
  trace = trace_timeline(cam, "prnt", 40);
  CHECK((trace.sound_ticks == std::vector<int>{2}) && trace.end_tick == 2);

  // Two-anchor shot whose second anchor has no frame: the origin stands in (midpoint (500, 0, 0), local +X toward the origin, x / z scaled by
  // 1000 / 2048), so the key eye (0, 0, 1000) lands at (500, 0, -488.3).
  Bytes anchored = make_camera(100, 0, 0x100);
  anchored[12] = 1;
  anchored[14] = 2;
  CamBank shots;
  shots.add_drp(make_drp({entry("anc1", 0x0C, anchored)}));
  BattleCamera framed;
  framed.set_bank(&shots);
  framed.set_anchor([](int type, int) {
    CamFrame frame;
    if (type == 1) {
      frame.valid = true;
      frame.x = 1000.0f;
    }
    return frame;
  });
  CamPose pose;
  CHECK(framed.key_pose("anc1", pose));
  CHECK(near(pose.eye[0], 500.0f, 0.5f) && near(pose.eye[1], 0.0f, 0.5f) && near(pose.eye[2], -1000.0f * 1000.0f / 2048.0f, 0.5f));
}

void test_vm_weapon_show() {
  // 0x7A n / 0x79 n: hide / show the attached weapon object n; n outside 0..1 = both.
  // Wanda In's caster script: SetAnim 0x80, wait 40, 7A 0 (the wand leaves the hand), ..., 79 0, SetAnim 0x81 (the catch).
  bseq::Table table;
  CHECK(table.parse(make_block({0x15, 128, 8, 0x09, 2, 0x7A, 0, 0x09, 2, 0x79, 0, 0x7A, 5, 0x7E, 0x00}, {0x00})));
  bseq::Ctx ctx;
  Run result = run(table, 0, 0, ctx);
  int hide0 = 0, show0 = 0, hide_all = 0, other = 0;
  for (const auto& [tick, event] : result.ev) {
    if (event.kind != bseq::Event::WeaponShow) continue;
    if (event.a == 0 && event.b == 0 && tick == 2) ++hide0;
    else if (event.a == 1 && event.b == 0 && tick == 4) ++show0;
    else if (event.a == 0 && event.b == -1 && tick == 4) ++hide_all;
    else ++other;
  }
  CHECK(hide0 == 1 && show0 == 1 && hide_all == 1 && other == 0);
  // 0x7E (a global cleared) is a 1-word no-op.
  CHECK(result.done_tick == 4);
}

// Ops that are length-correct but otherwise had no effect, each exercised through its handler.
void test_vm_round35_ops() {
  using Ev = bseq::Event;
  // 7E; v3 = 0, v4 = 0, v5 = -1024 (GetRot layout: x, z, yaw); 62 3 (SetRot, yaw 3072); 21 / 22 (shadow on / off); 82 2 (pacing k); 83 (scale ramp over 2 ticks);
  // 9C (one-tick yield); 68 "ab" twice (the second is the same name: not started again), 7E, 68 "ab" (started again); 7D "ab" always starts.
  const int ab1 = 0x6261, ab2 = 0x3030;
  bseq::Table table;
  CHECK(table.parse(make_block({0x7E, 0x40, 3, 0, 0x40, 4, 0, 0x40, 5, -1024, 0x62, 3, 0x21, 0x22, 0x82, 2, 0x83, 2048, 4096, 1536, 2, 0x9C, 0x68, ab1, ab2,
                                0x68, ab1, ab2, 0x7E, 0x68, ab1, ab2, 0x7D, ab1, ab2, 0x00},
                               {0x00})));
  bseq::Ctx ctx;
  Run result = run(table, 0, 0, ctx);
  int rot = 0, sh_on = 0, sh_off = 0, slow = 0, scale = 0, seq = 0, seq_tick1 = 0;
  for (const auto& [tick, event] : result.ev) {
    if (event.kind == Ev::SetRot && event.a == 3072 && tick == 0) ++rot;
    if (event.kind == Ev::Shadow && event.a == 1) ++sh_on;
    if (event.kind == Ev::Shadow && event.a == 0) ++sh_off;
    if (event.kind == Ev::SlowK && event.a == 2) ++slow;
    if (event.kind == Ev::ModelScale && event.off[0] == 2048 && event.off[1] == 4096 && event.off[2] == 1536 && event.d == 2) ++scale;
    if (event.kind == Ev::Sequence) {
      ++seq;
      if (tick == 1) ++seq_tick1;
    }
  }
  CHECK(rot == 1 && sh_on == 1 && sh_off == 1 && slow == 1 && scale == 1);
  // 68 (new), [68 same: skipped], 7E, 68 (again), 7D: three starts, all after the 9C yield (tick 1).
  CHECK(seq == 3 && seq_tick1 == 3);
  CHECK(result.done_tick == 1);
  // 52 ticks mode from(rgb) to(rgb); 53 likewise; 8F part sx sy sz ticks; 90 part; 91 a b ca cb; 92 a b; 6A / 6B elem; 6C elem vars ticks; 6D elem vars; A0 part ticks mode from to
  bseq::Table fade_table;
  CHECK(fade_table.parse(make_block({0x40, 6, 100, 0x40, 7, 0, 0x40, 8, 200, 0x52, 4, 2, static_cast<int>(static_cast<s16>(0xBFBF)), 0xBF, 0, 0, 0x53, 6, 1, 0x1234, 0x56, 0x4321, 0x65, 0x8F, 3, 8192, 4096, 4096, 5, 0x90, 3, 0x91, 4, 5, 6, 7, 0x92, 4, 5, 0x6A, 18, 0x6C, 18, 6, 8, 0x6D, 21, 6, 0x6B, 18,
                            0xA0, 2, 10, 1, 0x8080, 0x80, 0x6386, 0x8C, 0x00},
                           {0x00})));
  result = run(fade_table, 0, 0, ctx);
  bool fade = false, mfade = false, pscale = false, pstop = false, blink = false, bstop = false, mot_off = false, mot_on = false, erot = false, erate = false, pcol = false;
  for (const auto& [tick, event] : result.ev) {
    fade = fade || (event.kind == Ev::ScreenFade && event.a == 4 && event.b == 2 && event.off[0] == 0xBF && event.off[1] == 0xBF && event.off[2] == 0xBF && event.ang[0] == 0 && event.ang[2] == 0);
    mfade = mfade || (event.kind == Ev::ModelFade && event.a == 6 && event.b == 1 && event.off[0] == 0x34 && event.off[1] == 0x12 && event.off[2] == 0x56 && event.ang[0] == 0x21 && event.ang[1] == 0x43 && event.ang[2] == 0x65);
    pscale = pscale || (event.kind == Ev::PartScale && event.a == 3 && event.off[0] == 8192 && event.off[1] == 4096 && event.off[2] == 4096 && event.d == 5);
    pstop = pstop || (event.kind == Ev::PartScaleStop && event.a == 3);
    blink = blink || (event.kind == Ev::PartBlink && event.a == 4 && event.b == 5 && event.c == 6 && event.d == 7);
    bstop = bstop || (event.kind == Ev::PartBlinkStop && event.a == 4 && event.b == 5);
    mot_off = mot_off || (event.kind == Ev::ElemMotion && event.a == 18 && event.b == 0);
    mot_on = mot_on || (event.kind == Ev::ElemMotion && event.a == 18 && event.b == 1);
    // 6C reads vars [6] = x rot, [7] = z rot, [8] = yaw and sends them as the element's (x, y, z) = (100, 200 & 0xFFF, 0)
    erot = erot || (event.kind == Ev::ElemRot && event.a == 18 && event.d == 8 && event.off[0] == 100 && event.off[1] == 200 && event.off[2] == 0);
    erate = erate || (event.kind == Ev::ElemRotRate && event.a == 21 && event.off[0] == 100 && event.off[1] == 200 && event.off[2] == 0);
    pcol = pcol || (event.kind == Ev::PartColor && event.a == 2 && event.d == 10 && event.b == 1 && event.off[0] == 0x80 && event.off[1] == 0x80 && event.off[2] == 0x80 && event.ang[0] == 0x86 && event.ang[1] == 0x63 && event.ang[2] == 0x8C);
  }
  CHECK(fade && mfade && pscale && pstop && blink && bstop && mot_off && mot_on && erot && erate && pcol);
  // 8B list idx point out: the dive spot 2048 out from the target's attach point on the caster's side and 3547 below it (y +)
  bseq::Table dive_table;
  CHECK(dive_table.parse(make_block({0x8B, 1, 0, 16, 0, 0x00}, {0x00})));
  ctx.pos_x = 0;
  ctx.pos_z = 0;
  ctx.get_attach = [](int, int, int, int& x, int& z, int& y) {
    x = 4000, z = 0, y = -500;
    return true;
  };
  bseq::Vm vm;
  CHECK(vm.start(&dive_table, 0, 0));
  std::vector<bseq::Event> out;
  vm.tick(ctx, out);
  CHECK(vm.var(0) == 4000 - 2048 && vm.var(1) == 0 && vm.var(2) == -500 + 3547);
  // 8C list idx point ticks / 8D ticks: element 0's z rotation walks to atan2(-h, -dy) of the own attach 9 -> target attach vector
  bseq::Table tilt;
  CHECK(tilt.parse(make_block({0x8C, 1, 0, 16, 40, 0x8D, 12, 0x00}, {0x00})));
  ctx.get_attach_self = [](int, int& x, int& z, int& y) {
    x = 0, z = 0, y = 0;
    return true;
  };
  // Level: -0x400; target 4000 above (PS1 y down: -4000): -0x200; 4000 below: -0x600.
  for (int rise : {0, 4000, -4000}) {
    ctx.get_attach = [rise](int, int, int, int& x, int& z, int& y) {
      x = 4000, z = 0, y = -rise;
      return true;
    };
    bseq::Vm vt;
    CHECK(vt.start(&tilt, 0, 0));
    std::vector<bseq::Event> to;
    for (int i = 0; i < 4; ++i) vt.tick(ctx, to);
    bool got = false, back = false;
    const int want = rise == 0 ? -1024 : (rise > 0 ? -512 : -1536);
    for (const auto& event : to) {
      got = got || (event.kind == Ev::ElemTilt && event.a == 0 && event.d == 40 && event.off[2] == want);
      back = back || (event.kind == Ev::ElemTiltBack && event.a == 0 && event.d == 12);
    }
    CHECK(got && back);
  }
}

}  // namespace

// Pairs 180..270 deg apart interpolate the long way (the robot leg 0 -> -182), > 270 take the short arc.
void test_retail_arc() {
  float from = 0, to = -182;
  retail_arc(from, to);
  CHECK(from == 0 && to == -182);
  from = 10;
  // 340 apart: wraps.
  to = 350;
  retail_arc(from, to);
  CHECK(to == -10);
  from = 350;
  to = 10;
  retail_arc(from, to);
  CHECK(to == 370);
  from = 0;
  // Exactly 0xC00: not wrapped.
  to = 270;
  retail_arc(from, to);
  CHECK(to == 270);
  from = -100;
  // A turn or more apart: 500 reduced to 140, no further shift.
  to = 500;
  retail_arc(from, to);
  CHECK(to == 140);
}

// Party eye blink ops 72 / 73 / 4F and the light-position op 5D, plus the entry-8 weapon hide prefix scan.
void test_vm_eye_light() {
  bseq::Table table;
  // 72 (BlinkOn), 73 (BlinkOff), 4F 2 (ChgEye 2), 5D slot 1 at (100, -200, 300), 5C-sized neighbours stay untouched, end
  CHECK(table.parse(make_block({0x72, 0x73, 0x4F, 2, 0x5D, 1, 100, -200, 300, 0x00}, {0x00})));
  bseq::Ctx ctx;
  Run result = run(table, 0, 0, ctx);
  CHECK(result.ev.size() == 5);
  if (result.ev.size() == 5) {
    CHECK(result.ev[0].second.kind == bseq::Event::Blink && result.ev[0].second.a == 1);
    CHECK(result.ev[1].second.kind == bseq::Event::Blink && result.ev[1].second.a == 0);
    CHECK(result.ev[2].second.kind == bseq::Event::Eye && result.ev[2].second.a == 2);
    const bseq::Event& light = result.ev[3].second;
    CHECK(light.kind == bseq::Event::LightAbs && light.a == 1 && light.off[0] == 100 && light.off[1] == -200 && light.off[2] == 300);
    CHECK(result.ev[4].second.kind == bseq::Event::Done);
  }
  // All five ops run in the first tick: 72 / 73 / 5D are 1 / 1 / 5 words, 4F 2 words.
  CHECK(result.done_tick == 0);
}

// A table of 9 entries; entry 8 = `e8` (words after its header), the others just end.
Bytes make_nine(const std::vector<int>& e8) {
  Bytes table;
  const u32 count = 9, hdr = 4 + 4 * (count + 1);
  // Each entry is a header + one end word.
  const u32 each = 16 + 2;
  std::vector<u32> off;
  u32 at = hdr;
  for (u32 i = 0; i < count; ++i) {
    off.push_back(at);
    at += i == 8 ? 16 + 2 * static_cast<u32>(e8.size()) : each;
  }
  p32(table, count);
  for (u32 entry_offset : off) p32(table, entry_offset);
  p32(table, at);
  for (u32 i = 0; i < count; ++i) {
    p16(table, 16);
    for (int k = 0; k < 7; ++k) p16(table, 0);
    if (i == 8) for (int w : e8) p16(table, w);
    else p16(table, 0);
  }
  return table;
}

void test_initial_weapon_hide() {
  bseq::Table table;
  // The first wait stops the prefix: slot 0 stays shown.
  CHECK(table.parse(make_nine({0x7A, 1, 0x30, 4, 0, 0x09, 4, 0x7A, 0, 0x00})));
  CHECK(table.initial_weapon_hide() == 2);
  bseq::Table both_hidden;
  // Harle / Lutianna: both hidden.
  CHECK(both_hidden.parse(make_nine({0x7A, 1, 0x7A, 0, 0x00})));
  CHECK(both_hidden.initial_weapon_hide() == 3);
  bseq::Table reshown;
  // Setters are skipped; 7A 5 hides both, 79 1 shows slot 1 again.
  CHECK(reshown.parse(make_nine({0x30, 4, 0, 0x7A, 5, 0x79, 1, 0x00})));
  CHECK(reshown.initial_weapon_hide() == 1);
  bseq::Table glenn;
  // Glenn (second form): nothing hidden before the wait.
  CHECK(glenn.parse(make_nine({0x09, 4, 0x7A, 1, 0x00})));
  CHECK(glenn.initial_weapon_hide() == 0);
  bseq::Table few;
  CHECK(few.parse(make_block({0x00}, {0x00})));
  // Fewer than 9 entries hide nothing.
  CHECK(few.initial_weapon_hide() == 0);
}

// The chosen plate stays on its row while a8 counts 1..12 from the list pulse index, then floats up 0x16 per tick to y 0x16 and rests.
void test_cast_header_float() {
  bseq_hdr::FloatHeader header;
  header.reset(0);
  // Slot 9: row 195.
  const int row = 0x3C + 15 * 9;
  int ticks = 0;
  while (header.state == 0) { CHECK(header.plate_y(row) == row); header.step(row); ++ticks; }
  CHECK(ticks == 13 && header.state == 1 && header.a8 == 12 && header.y == row && header.fold_frame() == 0);
  int path[16], path_len = 0;
  while (header.state == 1) { header.step(row); path[path_len++] = header.y; }
  // The path is 173 151 129 107 85 63 41 -> 19 < 22 -> 22.
  CHECK(path[0] == 195 - 22 && path[path_len - 1] == 22 && path_len == 8);
  CHECK(header.state == 2);
  header.step(row);
  CHECK(header.state == 3 && header.plate_y(row) == 22);
  // Back to the list and out again: restarts from the pulse index.
  header.reset(3);
  int t2 = 0;
  while (header.state == 0) { header.step(row); ++t2; }
  CHECK(t2 == 10);
  bseq_hdr::FloatHeader clamped;
  clamped.reset(0);
  clamped.step(60);
  CHECK(clamped.fold_frame() == 4);
  for (int i = 0; i < 12; ++i) clamped.step(60);
  CHECK(clamped.state == 1 && clamped.y == 60);
  clamped.step(60);
  CHECK(clamped.y == 38);
  clamped.step(60);
  // 16 < 22 -> clamped.
  CHECK(clamped.y == 22 && clamped.state == 2);
}

// Damage numbers (battle_numbers.h) against retail captures: the op-0x38 pre-run start, the flight, the dam1 rest; and the anchor /
// overlap rules.
void test_damage_numbers() {
  const bnum::Flight& flight = bnum::flight_for_code(0);
  // Frozen (freeze count 4): position (192, 288.9, 1356.4), scale 16384, colour 128.
  bnum::State state = bnum::digit_state(flight, 192, 0, 4, 1);
  CHECK(near(state.x, 192.0f) && near(state.y, 288.9f, 0.1f) && near(state.z, 1356.4f, 0.1f) && near(state.scale[0], 16384.0f) && near(state.col[0], 128.0f));
  CHECK(state.lighten && state.visible);
  // Life 23, y 263.8, z 1434.6, colour 208.
  state = bnum::digit_state(flight, 192, 0, 4, 5);
  CHECK(near(state.y, 263.8f, 0.1f) && near(state.z, 1434.6f, 0.1f) && near(state.col[0], 208.0f));
  // y -18.8, z 735.6, scale 5632.
  state = bnum::digit_state(flight, 192, 0, 4, 25);
  CHECK(near(state.y, -18.8f, 0.1f) && near(state.z, 735.6f, 0.1f) && near(state.scale[0], 5632.0f));
  // dam1: (192, -24.1, 0), scale 4096, colour 1968, opaque.
  state = bnum::digit_state(flight, 192, 0, 4, 28);
  CHECK(near(state.y, -24.1f, 0.1f) && near(state.z, 0.0f) && near(state.scale[0], 4096.0f) && near(state.col[0], 1968.0f) && !state.lighten);
  // dam2 starts after dam1's 34 ticks.
  CHECK(bnum::digit_state(flight, 192, 0, 4, 62).lighten && !bnum::digit_state(flight, 192, 0, 4, 61).lighten);
  // The digit ends at 4 + 24 + 34 + 26.
  CHECK(!bnum::digit_state(flight, 192, 0, 4, 88).visible);
  CHECK(bnum::digit_ticks(flight, 4) == 88);
  // The gaiv flight ends green (g 128 + 55 x 34).
  CHECK(near(bnum::digit_state(bnum::flight_for_code(3), 0, 0, 4, 39).col[1], 1998.0f));
  // The anchor: x / y = v * H / |z| with |z| >= H / 4, clamped to +-130 / -90..40.
  auto anchor = bnum::anchor_px(100, -50, 1000, 320);
  CHECK(anchor[0] == 32 && anchor[1] == -16);
  anchor = bnum::anchor_px(5000, 5000, 1000, 320);
  CHECK(anchor[0] == 130 && anchor[1] == 40);
  // z is raised to 80.
  anchor = bnum::anchor_px(-10, -10, 10, 320);
  CHECK(anchor[0] == -40 && anchor[1] == -40);
  bnum::Overlap ov;
  CHECK(ov.add(0, 0, 2) == 0 && ov.add(5, 3, 2) == 16 && ov.add(100, 0, 1) == 0 && ov.add(2, 1, 3) == 32);
  for (int i = 0; i < 64; ++i) ov.tick();
  CHECK(!ov.rects.empty());
  ov.tick();
  CHECK(ov.rects.empty());
}

// Status plates: the shown statuses in the fixed order, nine per column, the hidden bits skipped.
void test_status_plates() {
  CHECK(bstat::place(0, false).empty());
  // KO (0x80), 0x2 and 0x200 have no plate.
  CHECK(bstat::place(0x282u, true).empty());
  // Frozen + poison: poison first, at the top of the left column.
  auto placed = bstat::place(0x20000001u, false);
  CHECK(placed.size() == 2 && placed[0].plate == 0 && placed[0].x == 16 && placed[0].y == 50);
  CHECK(placed[1].plate == 11 && placed[1].x == 16 && placed[1].y == 62);
  // Every plate: the tenth starts the second column; the right column is 248 then 210.
  placed = bstat::place(0xFFFFFFFFu, true);
  CHECK(placed.size() == 12 && placed[8].x == 248 && placed[8].y == 146 && placed[9].x == 210 && placed[9].y == 50 && placed[11].y == 74);
  CHECK(bstat::kPlates[static_cast<size_t>(placed[9].plate)].bit == 0x100000u);
}

int main() {
  test_status_plates();
  test_damage_numbers();
  test_cast_header_float();
  test_vm_eye_light();
  test_initial_weapon_hide();
  test_retail_arc();
  test_vm_basic();
  test_vm_weapon_show();
  test_vm_round35_ops();
  test_vm_branch();
  test_vm_unit_queries();
  test_vm_waits();
  test_vm_run_away();
  test_vm_jump();
  test_vm_jump_up_down();
  test_tags();
  test_camera();
  test_camera_timeline_flow();
  if (failures == 0) std::printf("battle_anim: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
