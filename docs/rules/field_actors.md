# Field actor rules

The behaviour rules of the field game mode as `src/game/` implements them: how a tick is ordered, how the player and the party move, how the camera follows, what collides with what, how actors animate and cast shadows, the status effects that run while walking, footsteps, message windows and how a party is placed on room entry. Room data is in `formats/room.md`, the tick order in `architecture.md` 4.6, script ops in `formats/fieldscript.md`, models and animation sequences in `formats/model.md`, text pacing in `formats/text.md`, sound in `formats/audio.md`. Angles are 4096 per turn, distances are world units (Y down), speeds are units per 60 Hz tick unless stated. "Engine heading" is the angle space that scripts and data use (direction `(cos h, -sin h)`); the actor code works in a space rotated by 0x800 from it (`internal = engine + 0x800`) and converts at the script boundary.

## 1. Tick structure

`FieldWorld::update(input, ticks)` runs once per rendered frame and calls `sim_tick()` `ticks` times (`field_world_tick.cpp`). `architecture.md` 4.6 lists the order of the steps, the odd-tick cadence and the 4-tick cull grace; this section adds what it does not say.

- Once per frame, before the ticks: the input snapshot. A confirm press raises `action_pending` (unless a modal window takes it); the flag is cleared after every tick, so with several ticks in a frame only the first sees the press. Select rotates the party once per frame, but not while `select_wait` (48 ticks after a room load, reduced by the frame's tick count) is positive and not when the script's key mask (FE 77) removes it; the rotation is refused with fewer than two members, while the controls are not free, a menu or battle is active, global flag 0x40 or 0x800 is set, or variable 0x36 is not 0. The message windows advance by `ticks / 60` s.
- `tick_count_` is incremented first, so the first tick of a run is odd.
- Early outs: a pending battle in the Hold state counts `kEncounterHoldTicks` = 104 ticks, then becomes Ready and the tick ends. While a room-exit fade of a captured kind (1, 3..6) runs, only `step_fade` and `step_transition` run.
- What 4.6 leaves out of the order: after `vm_.tick()` and `step_watchdog`, every actor's inactive state is set from flag 0x10; `step_motions` refreshes `class_bits` = flags & 0x600000, then runs `step_turn_rates`, then the scripted motions; `begin_tick()` also clears `updating`; the room-entry cut (below) runs after `step_visuals` and before `step_push`.
- Odd ticks alone run the scripted-turn bookkeeping (2.5) and the water wake cadence: `mz01` rings spawn when `wake_count & 3 == 0` while a walk or run clip plays, `& 15` otherwise (`wake_count` counts odd ticks spent in water). Culling (6.7), the shadow easing (7) and the lighting smoothing run every tick.
- Lighting smoothing: each actor's light colour eases toward the zone colour of its triangle (`table[flags & 15]`; 128 on all channels when the room has no table or the actor has flag 0x80000000 in its 0x270 word) by `kStepPerTick` = 8 per channel per tick. The first tick of an actor snaps.
- Room-entry cut: for the first 4 ticks after a load (`entry_cut_`) every running clip cross-fade is finished at the end of the tick, so the first script clip replaces the stand clip with no visible blend.
- Room-entry fade: every entry fades in from black once INIT has run, whatever INIT's own FADE asked for (`begin_entry_fade`): a room change of kind 2 additively from white over 0x20 ticks, every other entry (room change, Continue or Load, battle return, a debug `Room=` start, a route `load`) subtractively from black over 0x30, a captured kind then setting its own length (`formats/fieldscript.md`, FE 02). Room changes, A8 and the return from a menu all use this one fade (slot 0), drawn subtractively toward black (additively toward white for kind 2). The direction of the last screen fade is kept as a flag: a room exit and A8 mode 1 set it, every fade-in clears it, and A8 acts only when it flips the flag, starting from the far end. So a FADE 0 during the entry fade-in does not cut it short, and a FADE 1 while the screen is already going black neither restarts nor speeds it, nor does the exit fade that follows.
- Controls are free (`controls_free()`) when no room change or jump is pending, the fade is not darker than `kLockFade` = 0.3, no modal window is open, the global input-lock flag is clear and no actor has the action-running flag. An actor's input-lock flag raises the global one unless the actor is inactive (`update_input_lock`, at the end of every script tick).

## 2. Player movement

The player is the controlled entity (the party leader unless FE 4C or B2 chose another; 5F only moves the scroll). `step_player` runs after the scripts of the tick and does nothing while a scripted motion is active or the actor is not placed. The pad gives no heading while the party hold is on (global flag 0x10: a formation, 84 / DF, the end of BE's wait and 86 b != 1 set it; the end of the 86 b == 1 regroup and the next room load clear it; a battle keeps it).

### 2.1 Speed

- The raw speed `S` is the entity's 0x10C value (set by the speed ops, `fieldscript.md`). 0 means `kDefaultSpeed` = 0x50.
- The velocity is 16.16, so a step is `S / 16` units. Walk: `S / 16` (5 at the default, `kWalkUnitsPerTick`). Run: `(2 * S + S / 2) / 16` (12.5, `kRunUnitsPerTick`). No acceleration, no analog speed scaling. The cheat "run speed" doubles `S`.
- Scripted walks step `S / 16`, or 5 when `S` is 0.
- Every velocity move ends in the mover's 3D re-make (`room.md` 3.4): the step's length is taken in sixteenths of a unit with each axis truncated and a coarse square root, so a step is slightly shorter than `S / 16` off the axes (2 units at a slight angle: 30 / 16), and a step that rises or drops a whole unit of floor height is shortened further (2 units over a 1-unit rise: 1.79). WALK_TO_XZ aims from the whole-unit position (`fieldscript.md`, 42).
- Tiles with flag 0xC800 (stairs, ladders, forced slide) take the run away. Flag 0x1000 on the tile the actor stands on halves its velocity: the pad step (walk or run, and the step a pushed actor takes from it) as well as the terrain push (5.5).

### 2.2 Walk or run

- Run = (dash held) XOR (option "[button] Pressed" = Walk). Dash is the Cancel action or the pad's east button.
- Gamepad, left stick analog (2.3), option "Left Stick Sensitivity: Analog" on and `hypot(x, y) * 127 > 124`: run = NOT (dash held AND "[button] Pressed" = Walk). A smaller tilt keeps the rule above.
- A sprained party member (status bit 0x10 on any member) forces walk.

### 2.3 Heading

- D-pad index: `up 1 | right 2 | down 4 | left 8`; `kNoHeading` = 0x8000.

| Keys | Heading | Keys | Heading |
|---|---|---|---|
| left | 0x000 | up | 0x400 |
| left + up | 0x200 | up + right | 0x600 |
| right | 0x800 | down | 0xC00 |
| right + down | 0xA00 | down + left | 0xE00 |

  Any other combination, opposite pairs included, is no heading.
- Analog: the left stick counts as analog when either axis is beyond 52/127 of full tilt; the heading is then `round(atan2(y, -x) * 4096 / 2pi) & 0xFFF` (y positive up) and the D-pad is ignored. Inside that dead zone the D-pad table applies.
- Control yaw: `final = (pad + control_yaw) & 0xFFF`. `control_yaw_` is 0 at every room load and is set only by the script global op (ControlYaw). With yaw 0, up moves +Z, right +X, down -Z, left -X; the step is `(-cos h, +sin h) * step` in (x, z).
- The right stick, outside the same dead zone, with the controls free and no move heading, sets the facing to `atan2(ry, -rx)` directly (no control yaw).
- Flu adds `(rsin(phase) << 9) >> 12` (within +-0x200) to a pad heading (8.3).
- The actor takes the heading as its facing at once, unless the move slid along a wall (2.6); the model yaw chases the facing (2.4).

### 2.4 Facing, yaw, turn rate

- Model yaw = facing - `kModelYawBias` (0x400); a model with yaw `h - 0x400` faces heading `h`. The renderer rotates by `yaw + 0x800`.
- Each tick (`turn_step`, first thing in the clip pick) the yaw moves toward `facing - 0x400` by the actor's turn rate, the short way, landing on the target when within one step. An exact half turn counts as the long way down: the yaw turns down only while it is less than half a turn above the target, so a half turn goes up. The rate is the entity's 0x24E value, initially `kTurnPerTick` = 0x80 (180 degrees in 16 ticks); ops A5 / CF set it. A rate of 0 or less never turns the model.
- An inactive actor (flag 0x10) neither turns nor picks a clip; a culled one still does.

### 2.5 Scripted turns (`step_turn_rates`)

Runs on odd ticks; the resulting rate is copied to the actor every tick, except while bits 13 and 14 are both set: a request that waits for its odd tick holds the yaw still (rate 0 for the tick). The engine steps the yaw once per frame, after this bookkeeping, so a turn always starts from the shown facing the request was made at: an exact half turn stays exact, takes `kSlotTurnL` and turns up (2.4). Bits of the entity's 0x270 word: 13 (0x2000) fixed-time turns on, 14 (0x4000) a rotation op asked for a turn, 15 (0x8000) a fixed-time turn is running.

- Bits 13 and 14 set: unless bit 15 is set, the current rate is saved. `diff` = the angular distance between the shown facing and the target. The rate becomes `diff / 0x30`, so any turn lasts about 0x30 ticks. Rate 0: the model snaps to the facing and the saved rate returns (bits 14 and 15 cleared). Otherwise, with `diff > 0x13F`, the entity's 0x4000 flag (0x38 word) clear and the model owning the clip, the turn clip starts and restarts even if it is in force: slot `kSlotTurnR` when `(shown - facing) & 0xFFF < 0x800`, else `kSlotTurnL`. Bit 14 is cleared and bit 15 set.
- Bit 13 set, bit 14 clear, bit 15 set, and the model yaw equal to the facing: the saved rate returns and bits 14 and 15 clear. Bit 13 clear with bit 15 set: the same.
- Walks do not set bit 14 and turn at the plain rate.

### 2.6 Wall probe and slide

`room.md` 3.4 has the three-feeler probe, the slide, the slide facing and the slope rule. Here: the blocking classes are the actor's `class_bits`; a tick that ends inside a solid actor (5.2) puts the actor back on the spot it started the tick on, triangle included.

### 2.7 Off the mesh

With the controls free and a pad heading active, a leader that stands outside the mesh (a scripted walk left him there) is put on the triangle under him, or on the centroid of the triangle of the nearest vertex, before the move.

## 3. Camera follow

- Target actor: the controlled entity (Auto mode) or the entity a script chose (FollowEnt). The point is `(x, y + camera_y_offset, z)` projected by the room camera; `camera_y_offset` is 0 until a script global sets it.
- Target scroll `T = (trunc(160 - sx), trunc(120 - sy))`, clamped to the pan limits unless global flag 0x100 (FE 04) is set.
- Pan limits: the room's `[x_min, x_max]` and `[y_min - 24, y_max]` (24 = `kYMinBias`), or the script's own limits (no bias). In widescreen the x limits move inwards by the number of extra columns on each side; a picture narrower than the view is centred.
- Ease, once per tick in Auto and FollowEnt: `S -= (S - T) / 12` per axis (`kEaseDivisor`) with C truncation, so an error of up to 11 pixels stays. With global flag 0x200 (FE 11) the scroll is set to `T`. A Fixed camera does not move.
- A script tween (set-position, to-actor, follow) moves the scroll linearly over `frames` ticks (rounded each tick, exact at the end; `frames <= 0` jumps at once); `frames = 0x7FFF` means `max(1, distance)` ticks. When it ends the mode becomes Fixed (after set-position), Auto (to-actor) or FollowEnt (follow). Set-position targets are clamped like any other.
- Room entry: the scroll starts at (0, 0), the mode is Auto and the ease runs `kEntryEaseIterations` = 64 times against the player's spot; the view takes the result without interpolation.
- Shake (FE 21 x, y, ticks): `count = max(1, (ticks & 0xFFFF) + 1)`. The 16.16 amplitudes step by `(target - current) / count` for `count` ticks, then equal the target and stay there until the next FE 21. Every tick, on each axis whose integer amplitude `a` is above 0, the scroll gets `((rand15 * a + 1) >> 15) - a / 2` (range `[-a/2, a - a/2]`; `rand15` = bits 16..30 of a congruential generator `x * 1664525 + 1013904223`) and is clamped again to the pan limits (unless FE 04). The offset is not fed back into the eased scroll. The shake state is cleared at room load.
- Sequence shake (room sequence op 2E, `formats/room_effects.md` 5.2): the display itself moves. Every tick, on each axis whose rounded amplitude `a` is above 0, the scroll is lowered by `((rand15 * a) >> 16) + a / 2` (the picture moves right / down by `[a/2, a)` pixels; same generator), after the FE 21 shake and without the pan-limit clamp. On the PS1 the whole display (windows included) moves; OpenCross moves the room picture.
- One scroll per frame: the integer scroll is the only camera state. `RoomView::scroll_at(alpha)` interpolates it from the previous tick to the current one (`room.md` 5.3); layers, actors, shadows and effects take that single value. While a captured-kind exit fade runs, `alpha` is 1.
- Every tick the scroll is published in script variables 0x330 / 0x332.

## 4. Followers

Followers are party slots 1 and 2. "The leader" in the rules below is the controlled entity (party slot 0 unless FE 4C or B2 handed the pad to another entity; 5F does not): ring 0 records its walk, rule 1 measures against it, follower 1 follows it, and it never takes the follower branch itself. A non-party actor whose script runs the follow op and that is not player-controlled (flag 0x1) is treated as a third follower on ring 1. Follower logic runs only on a tick on which the member's own script ran the update-character op (`updating`); a member whose script is busy does not follow. Rule 3 never uses the walk-mesh mover (4.3).

### 4.1 Trail rings

- Two rings of `0x80` entries `{x, y, z, triangle, flags}`; ring 0 is the leader's, ring 1 follower 1's. Both are filled with one spot at room entry and after a spawn settle (ring 0 with the leader's spot, ring 1 with follower 1's, or the leader's when follower 1 is not placed).
- Ring 0 gets the spot the controlled entity stood on before its step, on each tick with a pad heading. A leader moved by a script adds his new spot when he is more than 0.5 units from where his last pad walk ended and from the newest entry, on a tick on which his own script ran the update-character op: a controlled entity that never runs it (a flyer selected with FE 4C so that it fires a guide entity's collision, hid_a05f's Wingapede) adds nothing, and the followers do not chase it through the air. Ring 1 gets follower 1's spot after each of its rule 1 or rule 3 steps, and when a script moves it (more than 0.5 units from the newest entry).
- The rules measure against the leader's spot before this tick's pad step, and distances are floored whole-unit 3D distances (200.9 counts as 200). Follower 2 measures against follower 1's new position.

### 4.2 Rules

`dL` = distance to the leader, `dP` = distance to the member followed (slot 1: the leader; slot 2: slot 1), `base` = the member's raw speed (default 0x50), `ring` = slot - 1. Per tick, in this order:

1. `dL < 80` and no follow wait (op BE) running: back off. Target = `(lx - cos(ar) * 128, lz - sin(ar) * 128)`, the point 128 units from the leader (`l`) on engine angle `ar = angle +- 0x80`, with `angle = -bearing(member - leader)` (0 when the two coincide), `lfe` = the leader's facing minus 0x800, and the sign `+` when `((lfe + angle) & 0xFFF) > 0x7FF + ring`, else `-` (at the exact tie the two followers swing to opposite sides). The step is `base / 16` units along the 3D line to the target taken at the leader's height; facing = the bearing of the move; walk clip.
2. else `dP <= 200`: stand; facing unchanged.
3. else replay the ring. Scan from the newest entry backwards for the first entry nearer than 50 units (100 for the whole scan when the member's tile or the newest entry's tile has flags 0xC800); none: the newest entry. Speed `spd = base - (ring + 1) * 16`; `dP > 230`: `spd += base / 2 + 32`, run clip; `dP >= 351`: `spd = 3 * base`, run clip; else if `dP < 250`: walk clip. The step is the 3D unit vector to the entry times `spd / 16`, the full step even past the entry. Facing = the bearing of the move.

Broken-chain guard: if the chosen entry is not the newest and is already within one step, the newest entry is used.

### 4.3 Writes and flags

- A rule 3 step is a direct position write copying the entry's triangle. Rule 1 is bounded by the mesh (12): a step to a point with no triangle, or whose mesh height differs from the step's height by more than 64 units, is replaced by the probe-and-slide step, and a refused step is standing.
- The walk and run flags are cleared at the start of every follower step, so a member a script froze does not walk on the spot; a member under a scripted motion keeps its flags and plays the walk clip.
- The whole follower pass is skipped while the party hold (global flag 0x10, section 2) is on. The hold is the cutscene rule: a scene starts with BE (wait until the followers stand, then hold) or a formation (84 / DF), so a member stays where it stood or where the scene's own walks put it, even when a script makes another entity the controlled one (5F); the scene ends with the 86 regroup, which walks slots 1 and 2 to the V behind the controlled entity, puts them there exactly, restarts both trails and releases the hold. Controls locked by 2C alone do not stop the followers.

### 4.4 Exit fade

During a room-exit fade only the leader stops: `controls_free()` is false for a pending transition or a fade darker than `kLockFade`, so he gets no heading. Scripts, followers and the camera keep ticking, and the followers settle around him by rule 2. The captured fade kinds freeze the whole tick (1).

## 5. Solid collision and triggers

### 5.1 Defaults

- Radius `kDefaultRadius` = 38 (op 8B overrides), height `kDefaultHeight` = 300, box half extents 38 each (op 8C).
- A box actor has flag 0x200. Its rectangle is oriented by the facing: half extent `w` along the forward axis `(-cos f, sin f)`, `h` along the perpendicular; a margin is added to both.

### 5.2 Player against solids

- Only the controlled entity is tested, against each other non-party actor that has flag 0x100 (solid) and flag 0x10 (inactive) clear. Vertical overlap: `py - 300 <= oy` and `oy - height <= py` (the player's height is the constant 300).
- The test point is the player's position after the mesh move. Circle: hit when `|P' - C|^2 < (r_player + r_other)^2`; if the player already overlapped before the move, only a move that shortens the distance is a hit. Box (margin 0): hit when P' is inside and the old position was not.
- A hit blocks the move unless the actor has the FE 25 flag (0x200000 of its 0x270 word) and its press counter has reached 0x40 (tested before the increment). The counter rises by one (to 0x7FFF) on each tick the player's point lies inside the actor and resets to 0 on any tick it does not, and on any tick the player has no heading.
- Pushable actors (flag 0x2000, FE 06): `fieldscript.md` 8.2. The pending move is the player's pad step of 2.1 from before the probe, slide and slope steps; the "inside another box actor" test looks at box actors (flag 0x200) with flags 0x400050 clear.

### 5.3 Triggers (`step_triggers`)

Candidates are the non-party actors from index 1 with flags 0x400012 clear and `py - 300 <= oy <= py + height`. With `d2` the squared centre distance, `rp` and `ro` the radii:

| Test | Circle actor | Box actor |
|---|---|---|
| touch (ON_COLLISION) | `d2 < (rp + 16 + ro)^2` (`kTouchMargin`) | player point inside the rectangle grown by `kBoxMargin` = 32 |
| talk (ON_ACTION) | `d2 < (rp + 48 + ro)^2` (`kTalkMargin`) and inside the talk cone | the same rectangle; the cone only if the actor has flag 0x1000 |

- Talk cone: `(player facing - bearing(actor - player) + 0x800) & 0xFFF` in `[0x545, 0xABB]` (`kTalkConeMin`, `kTalkConeMax`; about +-61 degrees).
- Talk needs `action_pending`, free controls and the no-action flag clear; touch is skipped for the no-collision flag.
- The actor with the smallest `d2` among the talk candidates gets the button first; if its ON_ACTION cannot start (script busy or locked) the next is tried, and the first that starts consumes the button. An actor that qualified for talk does not also get the touch event that tick.
- A key item used from the field list starts the touch-trigger script of every actor with that flag within `rp + 48 + ro` (box: inside the rectangle), without the cone test; those actors get neither talk nor touch that pass.

### 5.4 Class bits

A party member gets flags 0x400100 when it is bound (player class, solid). SETUP_NPC and D1 set 0x200100 and clear 0x4000 (NPC class, solid). What the class bits block: `room.md` 3.5. Actors never collide with each other except through 5.2.

### 5.5 Terrain push

A placed actor standing on a tile with flag 0x800 is moved every tick by `(cos d, -sin d) * P * 12 * 4096 / 65536`, with `d = (flags & 0x700) << 1` and `P` the push strength (op B5, default 5: 3.75 units per tick); flag 0x1000 halves it. Not pushed: actors with flags 0x50 or with 0x40000 in the 0x270 word, and the controlled entity while the input lock is set. The push goes through the mover, after the walks of the tick. A push strength of 0 disables it.

## 6. Animation playback

### 6.1 Rate

Clip time is in keyframes. It advances by `kKeyframesPerTick` = 0.25 (15 keyframes per second) times the actor's animation rate each tick: the entity's 0x10E value as a signed `v / 4096`, or 1 when it is 0; negative plays backwards. Looping clips wrap. A one-shot clamps at `length - 1` (0 when reversed) and ends the busy state, except under an animation sequence, which reports its own done bit. Rotation channels interpolate with the arc rule of `model.md` 6.3.

### 6.2 Clip slots

Defaults per walk-animation slot: stand 1, walk 3, run 2, fall 0, stairs up 5, idle2 4, stairs down 3, turn left 6, turn right 7; SET_WALK_ANIMATION replaces them per actor. Ids from 0x80 index the loaded extra pack. A model without an animation-sequence table loops ids 1..4 and plays every other clip once, holding the last frame.

### 6.3 Choosing the clip (`pick_clip`, once per tick, skipped while inactive)

1. The yaw takes its turn step.
2. Moving: the walk or run slot; a scripted request is cancelled; the idle timer is re-rolled to `(rand & 0x3FF) + 0x200`.
3. Not moving with a scripted request: that clip.
4. Otherwise the stand slot, with the idle timers of 6.4 when `auto_idle`. Without `auto_idle` and off stair tiles, the stand id becomes the requested id, so changing the stand slot later does not alter a standing actor until it walks or a script asks for a clip.
5. A stair tile (flag 0xC000) with no request forces the stairs-down slot (flag 0x8000) or the stairs-up slot, moving or not; such a tile also turns the actor toward the plane normal's horizontal direction.
6. A new id is submitted only when it differs from the last submitted one, and the stand id is held back while a cross-fade is still running, so short scripted hops keep the walk clip.

Who passes `moving` / `run`: the player's own tick; followers their motion flags; scripted walkers `motion.active && walking && !paused` (never run).

### 6.4 Idle timers

`auto_idle` holds for party members not under a scripted motion, for the player only while the controls are free and for the others while they are `updating`. When it turns on, the generator is seeded with `tick_count_ + 977 * entity index` and both timers are set to `(rand & 0xFF) + 0xFF`. While standing, `idle_timer` counts down; at 0 the idle slot in force plays (idle2 at first) and `idle_alt` counts down; at 0 it is re-rolled to `(rand & 0x3FF) + 0x100` and the idle slot in force is re-rolled, idle2 or stand by `rand & 1`. Moving re-rolls `idle_timer`.

### 6.5 Cross-fade

A clip started without a sequence cross-fades over `kCrossFadeTicks` = 0x20 ticks, linearly (weight gains 0x1000 / fade per tick). A clip started by a sequence op carries its own blend word and may blend from itself: `model.md` 6.1 and 6.2.

### 6.6 Room-model stand clip

SETUP_NPC and D1 on a model without an animation-sequence table set the stand slot and the current id to 0 and start clip 0 at once, looping, with no blend. It stays until the entity walks or a script asks for a clip.

### 6.7 Inactive and culled actors

- Inactive (flag 0x10): no turn, no new clip; the sequence and the clip already playing keep running. It is not drawn only if it is an empty party slot, or has flag 0x20, 0x4000 or 0x8000 (`ent_not_drawn`).
- Culled (`step_cull`, every tick): the entity flag 0x8000 is set (cleared again once the box is on screen), so the actor is not drawn and casts no shadow, and op B3 sees it; the clip is chosen, but the animation sequence, the clip time and the head turn do not run. Exemptions, the box and the screen test: `fieldscript.md` 8.1. The test is skipped for the first 4 ticks after a load; the actor's height offset is added to the box's y; if a box point cannot be projected the actor is kept. `OC_NO_CULL` disables the test.

### 6.8 Head turn

`model.md` 6.4 covers the joint. Each tick, after the clip time moved (`head_step`):

- `d` = the unit vector from the target to attach point 1, in the head joint's frame; `cur` = the current unit direction (initially the model's forward axis). `rate` is the head turn rate (default 0xE00; a head op passes 1..0xFFF, any other value keeps the old one).
- `cur' = normalise(cur * rate / 4096 + d * (1 - rate / 4096))`. Its yaw angle `ax` is clamped to +-0x200; its nod angle `az` to `[max(min(tz, cz), -0x180), min(max(tz, cz), 0x200)]`, `tz` and `cz` being the nod angles of `d` and of the previous `cur` (upper bound tested first).
- When components 0 and 2 of the direction both change by less than 64/4096 the `moving` flag clears, and a head released by `head_reset` returns to the animation. `head_reset` aims the target straight ahead of the face.

### 6.9 Display smoothing

`Actor::end_tick` spreads a change of position or yaw over the ticks since the previous change (1..4 ticks), so a script that steps an actor every second to fourth tick is drawn smoothly; an every-tick mover is unchanged. A jump of more than 150 units or a placement restarts the segment. Rendering interpolates between ticks with the frame's `alpha`.

## 7. Shadow

- Drawn for each loaded actor that is drawn (`ent_not_drawn` false), has strength above 0, has none of the flags 0x10C420 (0x20, 0x400, 0x4000, 0x8000, 0x100000) and whose slot lies in the current draw slab.
- Strength (`shadow_grey`) starts at 0x40 and eases every tick by 2 toward `(R + G + B) / 6` of the actor's entity colour (128 each, so 64), or toward 0 on a triangle with any flag of 0x6E000 (`room.md` 3.5). An actor with one of the flags above is set to 0 at once.
- Geometry: a quad with corners `(+-512, +-512)` in a frame built from the floor normal, scaled by the entity's x and z scale (raw 0x200 gives +-64 units; in general `scale / 8`), centred on the actor's floor point (its y, or the ground under it during a 5B / 5C hop). The normal is the cross product of the two unit edges from vertex 0, not renormalised, `(0, 4096, 0)` off the mesh. The frame is `A = norm(-N.y, N.x, 0)`, `B = norm(A x N)`; it is not rotated by the facing.
- Texture: the 32 x 32 4bpp blob that is TIM 0 of `map/etc/systim.bin` (UVs 0 and 30 of 32), nearest-sampled, bilinear in HD art. Colour = `clamp(strength / 128, 0, 1) * 255` on all channels. If the file is missing or TIM 0 is not that blob, no shadows are drawn.
- Blend: subtractive, depth tested, never depth-written.
- Ordering slot: `floor(max(z, 0) / 16)` of the projected corner 2, plus the actor's layer word (op 2A), plus 0.5 so that a background layer of the same slot is drawn over it; depth = slot / `kDepthRange` (8192).
- `OC_FIELD_SHADOWS=0` draws none.
- **`[Enhancements] Lighting`**: an enhancement, no original art or rule behind it. The same actors, on the same floor point and plane, in the same ordering slot and strength, with the blob replaced by the actor's silhouette as if lit from straight above. `render::ShadowPipeline` flattens the posed model straight down onto the actor's floor height into its own 256 x 256 tile of a 2048 x 2048 mask (up to 48 tiles; an actor past them keeps the blob). The model grows 4 units along its normals; the tile spans the pose's largest joint reach from the actor plus 72 units, 96 to 640 either way. A tile holds a multiplier, `1 - min(strength / 90, 1) x h`, the strength as above and `h` falling from 1 at the floor to 0.45 at height 420, the darkest value kept per texel, then blurred over a disc of 4 units into a 128 x 128 tile. It is multiplied onto the backdrop (depth tested, never written) as a quad with world x, z aligned sides on the floor plane, so it follows a slope and meets the layers as the blob does. The plane is eased per actor, by 0.2 per tick toward the triangle's slope, each axis at most 1, and held off the mesh: the triangles change slope at a step, which the tile's span would show as a jump; corner 2 of the blob still gives the slot. The tiles are drawn once per frame before the field picture (`FieldWorld::prepare_shadows`). The light has no direction: backdrops are painted, and a shadow falling sideways would contradict them.

## 8. Field status effects

`step_status` runs after the player's move. Status bits of a character: poison 0x01, burn 0x08, sprain 0x10, flu 0x20. "The party" is the field party; an effect needs the member to have the bit unless stated.

### 8.1 Counters

Every tick, even with the controls locked: `flu_phase += 0x20 + (rand & 0x1F)`. While the global input lock is set nothing else counts. The counters survive room loads; only the jolt is cleared.

### 8.2 Poison

`poison_ticks` rises each tick; past 0xF0 (every 241 ticks) it resets and every poisoned member loses 4 HP, never below 1. Burn alone does not drain.

### 8.3 Flu

`flu_ticks` rises each tick; past `flu_next + 0x100` (`flu_next` = `rand & 0x1FF`, re-rolled at each reset) it resets and, if any member has flu, script variable 0x40 is set to 1. While a pad heading is active and any member has flu, the heading gets `(rsin(flu_phase) << 9) >> 12` added (within +-0x200).

### 8.4 Damage floor

Walking on a triangle with flag 0x10000 adds to `floor_acc` (`room.md` 3.5: 2, or 5 when running). With a heading on any other triangle it resets to 0; with no heading it is kept; it does not change while the controls are not free. Past 0x80 it drops by 0x80, the jolt starts and every party member loses 2 HP (4 when burning), regardless of other statuses.

### 8.5 HP floor

Every drain sets HP to `max(1, hp - cost)`. Nothing in the field kills a member, so there is no field game over.

### 8.6 Jolt

A hit sets `jolt` = 8. `step_camera` decrements it and publishes `n = 7 .. 0`, one value per tick for 8 ticks. The host pastes the previous finished picture over the new one at 50 % alpha, translated by `(rsin(n * 64) >> 10, rcos(n * 0x39) >> 10)` PS1 pixels (rsin, rcos at 4096 scale) and scaled about (160, 120) by `(((rsin(n * 128) << 4) >> 12) + 0x1010) / 4096`. The paste is clipped to the room picture rectangle (letterbox and widescreen bars untouched); the new picture is not moved. The first frame of a jolt ghosts itself.

### 8.7 Status icons

Shown while the input lock is clear and no room change is pending, with or without a window on screen: one icon per status that any party member has, placed from the right edge (x = 304, 0x130) leftwards in the order flu, burn, sprain, poison, top at y = 8, 16 rows high. PS1 widths 0x19, 0x23, 0x23, 0x12; HD icons are drawn at a quarter of their image size. When the HD set lacks the poison icon the PS1 icons are used.

## 9. Footsteps

The animation sequence of a model raises foot events in model variable 1: bits 0..3 are the foot contacts used by the wake and footprint effects, bits 4 and 5 the footstep sound. `step_wakes` reads and clears the variable each tick and calls `play_footstep` first. `audio.md` covers the effect programs and the masks; this section is the decision and the numbers.

- Conditions: the actor is loaded and on a triangle; flags 0x10C020 are clear; the triangle has no flag 0xC000 (no sound on stairs); the actor is the controlled entity or has the footstep flag (0x1000000 in the 0x270 word).
- Sound id: the actor's own id `array[((feet >> 4) - 1) & 1]` (FE 5C) unless it is 0xFF; then the floor type, from the water register when the triangle has flag 0x60000, else from the dry register, for the walker (a party member's character id, or the entity's FE 40 value). A type of 0xFF or a walker of 0xFFFF plays nothing. `left = feet & 1`, `who = walker & 0x3F`:

| Type | Id |
|---|---|
| 0 | `left + 0x102`; who 0x0C: `left + 0x12E`; who 0x19: `left + 0x1BB` |
| 1 | `left + 0x12A`; the same two exceptions |
| 2 | `left + 0x12C` |
| 3 | `left` |
| 4 | `left + 2` |
| other | 0 |

  Ids from 0x100 are effect programs (`id & 0xFF`), smaller ones room frames.
- Pan: `screen_x = round(projected sx) + scroll_x` (160 when the actor cannot be projected); `pan = clamp(((screen_x * 0x6666) >> 16) + 0x40, 0, 0xFF)`; the sound receives `pan >> 1`.
- Volume: `dist` = the 3D distance from the actor to the listener, where the x term is the actor's own world x (the listener x register is stored but unused) and y, z are relative to the listener registers. 0 beyond `farthest`; 0x7F up to `nearest`; between them `0x7F - min(0x7F, ((dist - nearest) * (0x800000 / (farthest - nearest))) >> 16)` (a zero range counts as 1). The sound volume is `(vol * ((master * 0x10000) >> 7) >> 16) & 0x7F` with the master register.
- Room-load defaults: master 0x7F, nearest and farthest 0x7FFF, listener 0. The floor type registers are not reset by a room load.
- Channels: mask `1 << ((step_voice & 1) + 6)`, so 0x40 and 0x80 alternate per footstep played; a new footstep ends the previous sound of the same mask.

## 10. Message windows

- Count and ownership: window 0 (`dialogue_`) plus three extra windows created on first use, at most 4. Each belongs to the entity whose script opened it. A message takes window 0 when free, else the first free extra one, else it cannot open. An entity cannot open a second window while its own is open (E0 / E3 fail, E2 waits for the owner's window). The choice result is read from the entity's last window, also after it closed.
- Modal: a window is modal unless it is a frameless label (flag 0x10 of the entity's op 62 word). Modal windows take confirm and the direction keys and clear `action_pending`. A label closes itself on its `{wend}`; it takes confirm only while the input is script-locked.
- Opening, pacing and the full-size flag: `text.md` 7.8, `fieldscript.md` 9 (`ui::WindowOpening`: `kWindowGrowTicks` = 16, `kWindowFadeTicks` = 3).
- Number entry: Up / Down step the digit under the cursor, Left / Right move the cursor. A direction held for more than `kRepeatDelay` = 21 ticks (counted by the ticks of each frame) repeats once per rendered frame, not per tick. A fresh press steps once.
- Placement, PS1 pixels, top-left of the text area; flags, size, modes, portrait side and clamps are in `fieldscript.md` 9 (`place_dialogue_window`). Details: the message width counts substituted names as 0x30; `fl` = the dialog op's flag byte if it is non-zero, else the low byte of the entity's op 62 word, OR the high byte of that word; the centre is the speaker's projected point at half its height; mode 1 puts the top at feet + 8, mode 2 the bottom at head - 8; with `fl & 2` the y centre is 0xB7 when bit 2 is set, else 0x41; the portrait side is right if flag 0x800, left if flag 0x1000, otherwise right when `((facing + control_yaw) & 0xFFF) - 0x401` (unsigned) is below 0x800.

## 11. Spawn and entrances

- Order: a party member is placed when its SETUP_PC runs in INIT, the leader first (a follower waits for a placed leader). After INIT the members still unplaced are placed, leader first. Every actor's model yaw then snaps to `facing - 0x400`, the rings are filled, followers settled, the input lock and the camera updated.
- Continue and Load use the same spot: the room parameter is the saved variable 0x3A, and the saved leader position is not read.
- Leader spot: the entrance record picked by the room parameter: `x`, `z`, facing (`to_internal`: the record angle plus 0x800) and a floor level that selects the triangle (`room.md` 3.4). The record is written as it stands, even where the mesh has no floor (a party that walks in from outside). With no record (a parameter past the table, or no table) the spot is the centroid of the triangle of the vertex nearest to the mean of all vertices; a missing record in a room that has a table logs a warning.
- Facing precedence: the record's facing is applied when the member is placed; a rotation the room script does later in INIT overrides it, and the model snaps to the final facing. The previous room's facing is never kept.
- Followers: placed in the party V behind the leader. With `h` the leader's engine heading, slot 1 stands at angle `h + 0x200` and slot 2 at `h - 0x200`, 128 units away: `(x - cos a * 128, z + sin a * 128)`. The spot is accepted when the mesh has a triangle there within 64 units of the leader's height. Half fallback: otherwise the same V at 64 units; if that fails too the follower takes the leader's spot and is spread by settling. The V spot is set without any walk test, so it may lie across a blocked edge. A follower's facing is the leader's.
- Settling (`settle_spawned_followers`, also after the leader is put on a saved position): each follower still on the leader's spot runs rule 1 up to 80 times until it is 80 units from the leader in 3D. Slot 2 takes the other swing side when follower 1 also stood on the leader. The result snaps its facing, takes its height from its triangle without a glide, and the rings are refilled. A follower a script moved in INIT keeps its place.
- A saved position off the mesh is refused with a warning and the entrance spot is kept.
- A member who joins mid-room (7B then 7C): the first entity whose INIT opens with SETUP_PC naming the new character (an `0E FE` that found its slot empty at the load, say) has its INIT re-run and takes the controlled entity's position and facing; then the members of slots 1 and 2 are put in the party V behind the controlled entity (128 units at its heading +- 0x200, its height, no walk-mesh test), facing its way, and both follower trails restart there (`fieldscript.md`, op 7C; `FieldVM::relink_party_entity`, `FieldWorld::place_followers`). A script that places the newcomer itself (alu_a04f's Poshul, set on the spot of the dog she was) does so after the 7C.

## 12. Known deviations

Each is stated in a code comment.

- Follow rule 1 is bounded by the mesh: a step that would leave the mesh or the floor is replaced by the probe-and-slide step, and a blocked one by standing, instead of writing the position into the wall. Rule 3 stays unbounded.
- Followers are settled in a loop before the first frame instead of over the next ticks, so a scene that starts at once never shows the party stacked on one spot.
- A scripted walk blocked for `kStuckTicks` = 240 ticks ends, instead of waiting for ever.
- A leader a scripted move left off the mesh is put back on walkable ground once the player has the controls.
- The broken-chain guard of 4.2.
- Position and yaw smoothing (6.9) and the interpolated single scroll (3) are presentation only and do not change tick state.
