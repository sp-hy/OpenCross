#include "game/effects/sequence_player.h"

#include <cstdlib>
#include <algorithm>
#include <cstdio>

#include "core/log.h"
#include "platform/rumble.h"

namespace oc::effects {

namespace {
// Random camera pools met in this process (dev: OC_CAM_PICK 'name@k' addresses the k-th, in trace order, across the packs' players).
int g_pools_seen = 0;

constexpr int kMaxTasks = 24;
constexpr int kMaxStepsPerTick = 512;
// Op 0x6A reads bttim.ltd; op 0x42 waits for that read.
constexpr int kStockTextureReadTicks = disc_read_ticks(31492);

// off counts from the command start (the stored data begins at +4).
u16 u16_at(const assets::SeqCmd& cmd, size_t off) {
  if (off < 4 || off - 4 + 2 > cmd.data.size()) return 0;
  return static_cast<u16>(cmd.data[off - 4] | (cmd.data[off - 3] << 8));
}
u32 u32_at(const assets::SeqCmd& cmd, size_t off) { return u16_at(cmd, off) | (static_cast<u32>(u16_at(cmd, off + 2)) << 16); }
// Op 0x4C operands: volume & 0x7F, pan & 0xFF, mask & 0xFFFFFF.
void akao_operands(const assets::SeqCmd& cmd, int& volume, int& pan8, int& mask) {
  volume = u16_at(cmd, 16) & 0x7F;
  pan8 = u16_at(cmd, 18) & 0xFF;
  mask = static_cast<int>(u32_at(cmd, 20) & 0xFFFFFF);
}
int u8_at(const assets::SeqCmd& cmd, size_t off) { return (off >= 4 && off - 4 < cmd.data.size()) ? cmd.data[off - 4] : 0; }
// Op 0x51: plays a sound effect (s16 @+8 = effect00 id, u32 @+0xC = channel mask, u8 @+0xB = pan, u8 @+0xA = volume). The field's
// encounter sequence `enct` uses it twice (id 0x24 at tick 0, 0x23 at tick 1, mask 1, pan 0x80, volume 0x7F; the second cuts the first).
void play_sfx_op(audio::Sfx* sfx, const assets::SeqCmd& cmd) {
  if (!sfx) return;
  sfx->play_field(0x100 + (u16_at(cmd, 8) & 0xFF), u8_at(cmd, 10) & 0x7F, u8_at(cmd, 11) >> 1, u32_at(cmd, 12) & 0xFFFFFF);
}

struct Tween {
  float cur[3]{};
  float target[3]{};
  int remaining = 0;
  void set(const float goal[3], int ticks) {
    for (int i = 0; i < 3; ++i) target[i] = goal[i];
    remaining = ticks;
    if (ticks <= 0) {
      for (int i = 0; i < 3; ++i) cur[i] = goal[i];
      remaining = 0;
    }
  }
  void step() {
    if (remaining <= 0) return;
    for (int i = 0; i < 3; ++i) cur[i] += (target[i] - cur[i]) / static_cast<float>(remaining);
    // Ends exactly on the target.
    if (--remaining == 0)
      for (int i = 0; i < 3; ++i) cur[i] = target[i];
  }
};

struct Task {
  const assets::Sequence* seq = nullptr;
  size_t pc = 0, loop_pc = 0;
  int wait = 0;
  bool done = false;
  bool is_loop = false;
  // cursor: the target its per-target loop serves. inherited: the target of the parent when it started this one (0 = none).
  // caster: the acting-list index a CLONE of the charge sequence serves (0 = the original).
  int cursor = 0;
  int inherited = 0;
  int caster = 0;
  // Controller rumble: level (big motor << 7), per-tick delta, ramp ticks left, small-motor ticks left.
  s16 vib_level = 0, vib_delta = 0;
  u16 vib_ramp = 0, vib_small = 0;
  int target() const { return inherited ? inherited : cursor; }
};

}  // namespace

struct SequencePlayer::Impl {
  const EffectBank& bank;
  EmitterSystem& em;
  Rng rng;
  SequenceHooks hooks;
  audio::Sfx* sfx = nullptr;
  std::vector<Task> tasks;
  SequenceStats st;
  Tween ambient, shake;
  // Targets of the task whose command is being dispatched.
  int disp_cursor = 0, disp_inherited = 0, disp_caster = 0;
  // Ticks since this player was created (OC_FX_SEQ trace only).
  long long tick_n = 0;
  // Ticks left of the op 0x6A read; a stop asked for inside the pass.
  int read_left = 0;
  bool stop_pending = false;

  Impl(const EffectBank& effect_bank, EmitterSystem& emitters, u32 seed) : bank(effect_bank), em(emitters) { rng.state = seed ? seed : 1; }

  bool start(std::string_view name, int inherited = 0, int caster = 0) {
    const assets::Sequence* sequence = bank.sequence(name);
    if (!sequence || sequence->cmds.empty()) return false;
    if (tasks.size() >= static_cast<size_t>(kMaxTasks)) return true;
    Task task;
    task.seq = sequence;
    task.is_loop = sequence->name == "loop";
    task.inherited = inherited;
    task.caster = caster;
    tasks.push_back(task);
    ++st.started;
    run(tasks.size() - 1);
    return true;
  }

  // Op 0x28: small motor on for u16 +4 ticks. Case 0x29: finish the ramp in progress (level += delta * ticks left, floor 0), then
  // either (u16 +4 == 0) store s16 +8 << 7 as the delta with no ramp - the level itself is NOT set, a quirk of the original: the 13 such ops in
  // the corpus are all level 0 resets that change nothing - or ramp the level to s16 +8 << 7 over +4 ticks (integer division as the original).
  void vib_op(Task& task, const assets::SeqCmd& cmd) {
    const char* seq = task.seq ? task.seq->name.c_str() : "?";
    if (cmd.op == 0x28) {
      task.vib_small = u16_at(cmd, 4);
      log::debug("rumble: big {} small 1 frames {} ({} op 28)", task.vib_level >> 7, task.vib_small, seq);
      return;
    }
    if (task.vib_ramp != 0) {
      task.vib_level = static_cast<s16>(task.vib_level + task.vib_delta * static_cast<s16>(task.vib_ramp));
      if (task.vib_level < 0) task.vib_level = 0;
    }
    const s16 target = static_cast<s16>(u16_at(cmd, 8));
    const u16 ticks = u16_at(cmd, 4);
    if (ticks == 0) {
      task.vib_ramp = 0;
      task.vib_delta = static_cast<s16>(target << 7);
    } else {
      task.vib_delta = static_cast<s16>((target * 0x80 - task.vib_level) / static_cast<s16>(ticks));
      task.vib_ramp = ticks;
    }
    log::debug("rumble: big {} small 0 frames {} ({} op 29, from {})", target, ticks, seq, task.vib_level >> 7);
  }

  // The per-tick rumble step: the task adds big = level >> 7 (0..255) and, while its small timer runs, small = 1 to the pad, then
  // the ramp and timer advance one tick (a ramp that overshoots to below zero ends at once).
  void vib_step(Task& task, platform::Rumble& pad) {
    const int big = task.vib_level >> 7;
    if (big > 0 || task.vib_small > 0) pad.contribute(this, task.vib_small > 0 ? 1 : 0, big);
    if (task.vib_ramp != 0) {
      task.vib_level = static_cast<s16>(task.vib_level + task.vib_delta);
      --task.vib_ramp;
      if (task.vib_level < 0) task.vib_level = 0;
    }
    if (task.vib_small != 0) --task.vib_small;
  }

  // A non-camera command met inside a camera pool (0x23 run): the spawn / sound / ambient ops run at once, the rest goes to the host.
  void exec_other(const assets::SeqCmd& cmd, int target, size_t ti) {
    ++st.commands;
    ++st.op_hits[cmd.op & 0x1FF];
    switch (cmd.op) {
      case 0x20: case 0x21: case 0x42: case 0x67: break;
      case 0x28: case 0x29: vib_op(tasks[ti], cmd); break;
      case 0x22: em.start_for_actor(cmd.name, tasks[ti].caster, target); break;
      case 0x2F: start(std::string(cmd.name), target); break;
      case 0x55: em.stop_expire(cmd.name); break;
      case 0x5A: em.kill(cmd.name); break;
      case 0x4C:
        if (hooks.akao) {
          int vol, pan8, mask;
          akao_operands(cmd, vol, pan8, mask);
          hooks.akao(cmd.name, bank.blob(0x16, cmd.name), vol, pan8, mask);
        }
        break;
      case 0x51:
        play_sfx_op(sfx, cmd);
        break;
      case 0x65: {
        const int r = u8_at(cmd, 8), g = u8_at(cmd, 9), b = u8_at(cmd, 10), ticks = u16_at(cmd, 4);
        const float tg[3] = {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)};
        ambient.set(tg, ticks);
        if (hooks.ambient) hooks.ambient(r, g, b, ticks);
        break;
      }
      default:
        ++st.other_ops;
        if (hooks.other) hooks.other(cmd);
        break;
    }
  }

  // Runs the commands of one task while its accumulated wait is not positive (a negative wait = commands overdue).
  // A negative command wait is signed and carries into the following commands.
  void run(size_t ti) {
    for (int guard = 0; guard < kMaxStepsPerTick; ++guard) {
      if (ti >= tasks.size() || tasks[ti].done || tasks[ti].wait > 0) return;
      Task& task = tasks[ti];
      if (task.pc >= task.seq->cmds.size()) {
        task.done = true;
        return;
      }
      const assets::SeqCmd& cmd = task.seq->cmds[task.pc];
      disp_cursor = task.cursor;
      disp_inherited = task.inherited;
      disp_caster = task.caster;
      ++st.commands;
      ++st.op_hits[cmd.op & 0x1FF];
      // Dev trace: one line per executed command.
      if (static const bool trace = std::getenv("OC_FX_SEQ") != nullptr; trace)
        std::fprintf(stderr, "fxseq: t%lld %s #%zu op %02X wait %d %s\n", tick_n, task.caster > 0 ? "sado" : std::string(task.seq->name).c_str(), task.pc, cmd.op, static_cast<int>(static_cast<s16>(cmd.wait)), cmd.name.c_str());
      int wait = static_cast<s16>(cmd.wait);
      bool advance = true;
      switch (cmd.op) {
        case 0x20: break;
        case 0x21:
          if (task.is_loop) {
            task.pc = task.loop_pc;
            // Never spin inside one tick.
            task.wait = std::max(wait, 1);
            return;
          }
          // At the end a sequence with header flag bit0 serves the next target (cursor + 1) from its loop label (the
          // start unless an op 0x4E moved it) while has_target finds one.
          if ((task.seq->flags & 1) && hooks.has_target && hooks.has_target(task.cursor + 1)) {
            ++task.cursor;
            task.pc = task.loop_pc;
            task.wait = 0;
            // Continues next tick (the task's time is reset).
            return;
          }
          task.done = true;
          return;
        case 0x22: em.start_for_actor(cmd.name, task.caster, task.target()); break;
        case 0x2F: {
          // Copy: start() may reallocate `tasks`.
          const std::string child_name = cmd.name;
          start(child_name, task.target());
          break;
        }
        case 0x23: {
          // From this op to the first one with a non-zero wait (inclusive) every 0x23 joins a pool and every other
          // op runs at once; one camera of the pool is picked with rand() % n (the charge packs put an emitter between their four shots).
          // `task` may dangle after a child start (vector growth): index by `sq` / `ti` below.
          const assets::Sequence* sq = task.seq;
          const size_t first = task.pc;
          size_t last = first;
          size_t pool_size = 0;
          for (;; ++last) {
            const assets::SeqCmd& pooled = sq->cmds[last];
            if (pooled.op == 0x23) ++pool_size;
            else exec_other(pooled, tasks[ti].target(), ti);
            if (pooled.wait != 0 || last + 1 >= sq->cmds.size()) break;
          }
          size_t pick = static_cast<size_t>(rng.next()) % std::max<size_t>(pool_size, 1);
          // Dev: force the member of a random camera pool; a comma list covers several pools.
          // 'name' matches in every pool; 'name@k' only in the k-th pool of the run (for pools sharing a member name).
          if (const char* want = std::getenv("OC_CAM_PICK")) {
            const std::string list = std::string(",") + want + ",";
            const std::string at = "@" + std::to_string(g_pools_seen) + ",";
            for (size_t i = first, k = 0; i <= last; ++i)
              if (sq->cmds[i].op == 0x23) {
                const std::string nm = std::string(sq->cmds[i].name);
                if (list.find("," + nm + ",") != std::string::npos || list.find("," + nm + at) != std::string::npos) pick = k;
                ++k;
              }
          }
          ++g_pools_seen;
          // Dev trace: the members of the pool.
          if (static const bool trace = std::getenv("OC_FX_SEQ") != nullptr; trace) {
            std::string names;
            for (size_t i = first; i <= last; ++i)
              if (sq->cmds[i].op == 0x23) names += (names.empty() ? "" : ",") + std::string(sq->cmds[i].name);
            std::fprintf(stderr, "fxseq: t%lld %s pool %s\n", tick_n, std::string(sq->name).c_str(), names.c_str());
          }
          for (size_t i = first; i <= last; ++i) {
            if (sq->cmds[i].op != 0x23) continue;
            if (pick-- == 0) {
              if (hooks.camera) hooks.camera(sq->cmds[i].name, bank.blob(0x0C, sq->cmds[i].name));
              break;
            }
          }
          wait = static_cast<s16>(sq->cmds[last].wait);
          tasks[ti].pc = last;
          break;
        }
        case 0x28: case 0x29: vib_op(tasks[ti], cmd); break;
        case 0x2E:
        case 0x2A:
          if (cmd.op == 0x2A && cmd.len >= 16) {
            // Battle colour matrix (type-0x11 entry by name).
            ++st.other_ops;
            if (hooks.other) hooks.other(cmd);
            break;
          }
          {
            const int ax = u8_at(cmd, 8), ay = u8_at(cmd, 9), ticks = static_cast<s16>(u16_at(cmd, 4));
            const float tg[3] = {static_cast<float>(ax), static_cast<float>(ay), 0};
            shake.set(tg, ticks);
            if (hooks.shake) hooks.shake(ax, ay, ticks);
          }
          break;
        case 0x65: {
          const int r = u8_at(cmd, 8), g = u8_at(cmd, 9), b = u8_at(cmd, 10), ticks = u16_at(cmd, 4);
          const float tg[3] = {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)};
          ambient.set(tg, ticks);
          if (hooks.ambient) hooks.ambient(r, g, b, ticks);
          break;
        }
        case 0x4C:
          if (hooks.akao) {
            int vol, pan8, mask;
            akao_operands(cmd, vol, pan8, mask);
            hooks.akao(cmd.name, bank.blob(0x16, cmd.name), vol, pan8, mask);
          }
          break;
        case 0x51:
          play_sfx_op(sfx, cmd);
          break;
        case 0x55: em.stop_expire(cmd.name); break;
        case 0x5A: em.kill(cmd.name); break;
        // Wait for CD idle: the data is resident, so nothing to do.
        case 0x42:
          if (read_left > 0) {
            task.wait = 1;
            return;
          }
          break;
        // Loop label: the next command becomes the loop start, the target cursor resets to 0.
        case 0x4E: task.loop_pc = task.pc + 1; task.cursor = 0; break;
        case 0x4D:
          // Only sequences with header flag bit0 loop over the targets; when the next one exists the cursor advances and the pc returns to the label.
          // A served loop returns at once with the wait reset (the op's own wait is NOT taken - only the
          // last target's pass waits it out). The next tick overshoots that zero wait by one, which the pass's first wait pays back.
          if (hooks.has_target) {
            if ((task.seq->flags & 1) && hooks.has_target(task.cursor + 1)) {
              ++task.cursor;
              task.pc = task.loop_pc;
              task.wait = -1;
              // Continues next tick.
              return;
            }
          } else if (hooks.next_target && hooks.next_target()) {
            task.pc = task.loop_pc;
            task.wait = -1;
            return;
          }
          break;
        case 0x6A:
          read_left = kStockTextureReadTicks;
          ++st.other_ops;
          if (hooks.other) hooks.other(cmd);
          break;
        case 0x50:
          if (hooks.actor_busy && hooks.actor_busy()) {
            // Stall: re-test next tick.
            task.wait = 1;
            return;
          }
          break;
        case 0x7A:
          if (hooks.wait_cond && hooks.wait_cond()) {
            task.wait = 1;
            return;
          }
          break;
        // Yield.
        case 0x67: wait = std::max(wait, 1); break;
        default:
          ++st.other_ops;
          if (hooks.other) hooks.other(cmd);
          break;
      }
      // Refetch: a child start may have grown the vector.
      Task& refetched = tasks[ti];
      if (advance) ++refetched.pc;
      refetched.wait += wait;
      if (refetched.wait > 0) return;
    }
    // Runaway loop of zero-wait commands: continue next tick.
    if (ti < tasks.size()) tasks[ti].wait = 1;
  }

  void tick() {
    ++tick_n;
    ambient.step();
    shake.step();
    if (read_left > 0) --read_left;
    platform::Rumble& pad = platform::Rumble::instance();
    for (size_t i = 0; i < tasks.size(); ++i) {
      if (tasks[i].done) continue;
      vib_step(tasks[i], pad);
      if (tasks[i].wait > 0) --tasks[i].wait;
      run(i);
      if (stop_pending) break;
    }
    if (stop_pending) {
      stop_pending = false;
      tasks.clear();
      shake = Tween{};
    }
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [](const Task& task) { return task.done; }), tasks.end());
  }
};

SequencePlayer::SequencePlayer(const EffectBank& bank, EmitterSystem& emitters, u32 seed)
    : impl_(std::make_unique<Impl>(bank, emitters, seed)) {}
SequencePlayer::~SequencePlayer() = default;

void SequencePlayer::set_hooks(SequenceHooks hooks) { impl_->hooks = std::move(hooks); }
void SequencePlayer::set_sfx(audio::Sfx* sfx) { impl_->sfx = sfx; }
bool SequencePlayer::start(std::string_view name) { return impl_->start(name); }
bool SequencePlayer::start(std::string_view name, int inherited) { return impl_->start(name, inherited); }
bool SequencePlayer::start_caster_clone(std::string_view name, int caster) { return impl_->start(name, 0, caster); }
bool SequencePlayer::start_main(bool caster_is_target) {
  if (caster_is_target && impl_->start("own")) return true;
  return impl_->start("main");
}
void SequencePlayer::stop(std::string_view name) {
  auto& tasks = impl_->tasks;
  tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [&](const Task& task) { return task.seq->name == name; }), tasks.end());
}
void SequencePlayer::stop_all() {
  impl_->tasks.clear();
  impl_->shake = Tween{};
}
void SequencePlayer::stop_all_after_pass() { impl_->stop_pending = true; }
void SequencePlayer::tick() { impl_->tick(); }
size_t SequencePlayer::active() const { return impl_->tasks.size(); }
size_t SequencePlayer::active_non_loop() const {
  return static_cast<size_t>(std::count_if(impl_->tasks.begin(), impl_->tasks.end(), [](const Task& task) { return !task.is_loop; }));
}
bool SequencePlayer::running(std::string_view name) const {
  return std::any_of(impl_->tasks.begin(), impl_->tasks.end(), [&](const Task& task) { return !task.done && task.seq && task.seq->name == name; });
}
const SequenceStats& SequencePlayer::stats() const { return impl_->st; }
void SequencePlayer::ambient_rgb(int& r, int& g, int& b) const {
  r = static_cast<int>(impl_->ambient.cur[0] + 0.5f);
  g = static_cast<int>(impl_->ambient.cur[1] + 0.5f);
  b = static_cast<int>(impl_->ambient.cur[2] + 0.5f);
}
void SequencePlayer::shake_amplitude(int& ax, int& ay) const {
  ax = static_cast<int>(impl_->shake.cur[0] + 0.5f);
  ay = static_cast<int>(impl_->shake.cur[1] + 0.5f);
}
int SequencePlayer::dispatch_cursor() const { return impl_->disp_cursor; }
int SequencePlayer::dispatch_inherited() const { return impl_->disp_inherited; }
int SequencePlayer::dispatch_caster() const { return impl_->disp_caster; }
int SequencePlayer::rumble_level() const {
  int big = 0;
  for (const Task& task : impl_->tasks) big += task.vib_level >> 7;
  return std::min(big, 255);
}
void SequencePlayer::shake_offset(int& dx, int& dy) {
  int ax = 0, ay = 0;
  shake_amplitude(ax, ay);
  dx = ax ? impl_->rng.next() % (2 * ax + 1) - ax : 0;
  dy = ay ? impl_->rng.next() % (2 * ay + 1) - ay : 0;
}

}  // namespace oc::effects
