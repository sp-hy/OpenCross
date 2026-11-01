#pragma once
// Generic effect player, part 2: the type-0x10 sequence task.
// Each command carries a wait (ticks after it); commands with wait 0 run in the same tick. The ops that the effect system itself
// can execute (emitter start / stop / kill, child sequences, flow control, ambient colour fade, shake / rumble ramps, AKAO / SFX)
// are handled here; the camera record play and everything that needs the battle scene (actor show/hide, fades, flashes, tints) is
// delivered through SequenceHooks.
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "audio/sfx.h"
#include "game/effects/emitter_system.h"

namespace oc::effects {

struct SequenceHooks {
  // Op 0x23: camera track `name` (type-0x0C blob, may be null when the pack lacks it).
  std::function<void(std::string_view name, const assets::EffectBlob* record)> camera;
  // Op 0x4C: the AKAO sound sequence `name` (type-0x16 blob) is looked up in the resource list and started with
  // volume = s16 at +16 & 0x7F, pan = s16 at +18 & 0xFF (0x80 = centre), mask = u32 at +20 & 0xFFFFFF.
  std::function<void(std::string_view name, const assets::EffectBlob* sound, int volume, int pan8, int mask)> akao;
  // Notifications for the ramps (the current values are pulled from the player every tick).
  // shake (op 2E / 2A(12)): target amplitude (bytes), ramp ticks.
  std::function<void(int amp_x, int amp_y, int ticks)> shake;
  // rumble (op 28 kind 0: small motor on; op 29 kind 1: level ramp).
  std::function<void(int kind, int level, int ticks)> rumble;
  // ambient (op 65): back colour fade target.
  std::function<void(int r, int g, int b, int ticks)> ambient;
  // Every other op (actor visibility, brightness, fades, flashes, tint ramps, colour matrix, overlay-private ops...).
  std::function<void(const assets::SeqCmd&)> other;
  // Flow conditions (default: false = not blocking).
  // actor_busy (op 50): stall while true.
  std::function<bool()> actor_busy;
  // next_target (op 4D): restart the loop body for the next target when true (used when has_target is unset).
  std::function<bool()> next_target;
  // True when the action has a k-th target. Drives the per-task target loop of sequences with header flag bit0 (op 4D and the
  // end of the sequence restart the task for the next target).
  std::function<bool(int target)> has_target;
  // wait_cond (op 7A): stall while true.
  std::function<bool()> wait_cond;
};

struct SequenceStats {
  u64 commands = 0, other_ops = 0, started = 0;
  // Commands executed per op (audit).
  std::array<u32, 0x200> op_hits{};
};

class SequencePlayer {
 public:
  SequencePlayer(const EffectBank& bank, EmitterSystem& emitters, u32 seed = 1);
  ~SequencePlayer();
  SequencePlayer(const SequencePlayer&) = delete;
  SequencePlayer& operator=(const SequencePlayer&) = delete;

  void set_hooks(SequenceHooks hooks);
  // Sound-effect sink for op 51 SFX_ID.
  void set_sfx(audio::Sfx* sfx);

  // D3 PLAY_EFFECT: starts a task (first commands run immediately). False if the name is unknown.
  bool start(std::string_view name);
  // Same with the target index the started task serves (op 0x2F's `inherited`; 0 = none).
  bool start(std::string_view name, int inherited);
  // A clone of the entry bound to acting-list caster `caster` (> 0): the sequence task is copied and
  // its caster operands (op 0x45 s16@10 := caster, 0x46 / 0x57 / 0x59 / 0x47 / 0x6F / 0x70 / 0x58 targets := 0) and its op-0x22 emitters (selector 1 -> 9 + caster).
  bool start_caster_clone(std::string_view name, int caster);
  // Auto-start of the stock hosts: 'own' when caster == target, else 'main'.
  bool start_main(bool caster_is_target = false);
  // D7 STOP_EFFECT: unlinks the tasks of `name` only (emitters they started keep running).
  void stop(std::string_view name);
  // D7 "0000": unlinks every task, 'loop' included, and ends the screen shake.
  void stop_all();
  // The same for an op running inside tick(): the tasks go when the running task's pass ends.
  void stop_all_after_pass();

  // One 60 Hz tick.
  void tick();
  size_t active() const;
  // Tasks other than the repeating 'loop' sequence.
  size_t active_non_loop() const;
  // A task of sequence `name` is still linked (e.g. the test for the encounter sequence 'enct').
  bool running(std::string_view name) const;
  const SequenceStats& stats() const;

  // Current tween values (pulled by the renderer each frame).
  void ambient_rgb(int& r, int& g, int& b) const;
  void shake_amplitude(int& ax, int& ay) const;
  // Rumble level, 0..255.
  int rumble_level() const;
  // Target indices of the task whose command is being dispatched (valid inside the hooks): `cursor` = its own per-target loop index,
  // `inherited` = the index its parent served when it started it (the started copy's target operands are rewritten).
  // dispatch_caster() is > 0 inside a clone of the charge sequence (see start_caster_clone).
  int dispatch_cursor() const;
  int dispatch_inherited() const;
  int dispatch_caster() const;
  // Random screen offset within the current shake amplitude.
  void shake_offset(int& dx, int& dy);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oc::effects
