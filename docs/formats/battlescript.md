# Battle script VM (`battle/event/evt*.pac`)

Enemy AI, scripted fights and battle cut-scenes are bytecode stored in `battle/event/evtNNN.pac` (one file per encounter scene, picked by the file index, see `battle_data.md` section 6, `NNN` is normally the scene id; scenes without logic share the template `evt0000.pac`). This document covers the file layout, the interpreter model, the scheduler that decides when each script slot runs, the variable model, target codes, and every opcode with its length and meaning. The corpus is 440 files, 14183 unique scripts, all of which decode exactly with the length table below; all 47,594 goto / cond-jump targets land on an instruction start. Counts in parentheses are corpus occurrences. Related: `battle_data.md` (scene records).

Confidence column: **C** = behaviour clear, **I** = behaviour known, purpose or name inferred, **U** = no shipped script contains the opcode (its handler is not modelled; the interpreter advances by the table length).

## 1. File layout

All integers little-endian. The `.pac` is a two-part container:

```
0x00 u32 2                        part count
0x04 u32 0x10, u32 end0, u32 eof  part 0 = [0x10, end0) script block; part 1 = [end0, eof) dialogue block
0x10-0x8F  zero
0x90 u32 entity_count             6..11 (6:95 7:101 8:141 9:75 10:21 11:7 files)
0x94 entity[i], 0x40 bytes each:  u16 script_ptr[0..31]   (0 = none)
base = 0x94 + entity_count*0x40   script area
```

* Every pointer, jump target and call target is an offset from `base`. A pointer of 0 means "none"; the code at `base+0` (before the first real pointer) is entity 0's slot-0 script. `call` can enter the middle of an extent.
* In the corpus `entity_count = 5 + monster instances` (18 of 439 scenes have extra entities). Entity 0 is global; entities 1-3 bind to the PC slots (units 0-2) and entities 4.. to monster slots (units 3-8), through opcode 0E.
* Unit ids: 0-2 PC slots, 3-8 enemy slots. Scripts write them as `0xE0+u`; `0xE9` means "self".
* Slots (the 32 pointers per entity; 0-4 are used): 0 init, 1 frequent/idle loop, 2 about to act, 3 killed, 4 attacked. Slot 5 occurs in six files (evt031/032/033/187/188/189) but nothing launches it, so it is dead data.
* Scripts end with `00 halt` (12966), `0D return` (903), `77` (308) or `A6` (6).
* Part 1 holds the dialogue records used by opcode E0 (section 5.3).

## 2. Interpreter model

Each entity has its own context: a frame stack of up to 32 frames (a script program counter, a slot id and a state mark per frame), a call stack (4 return-address slots per frame, indexed by ONE depth counter per entity that every frame shares and halt does not reset; no bounds check: a fifth nested call writes into the next frame's slots), the bound unit (0xFF none, 9 dead/unbindable), a delay counter armed by op 2D, flags recording which of slots 2/3/4 are running, tween state for B0/B1, and a group tag (op 8E, initially 0xFF).

* Initial state: frame 0 = the slot-0 script, bound unit none. An entity other than 0 without a slot-0 pointer starts directly in its slot-1 idle loop.
* **Per tick** (skipped on the first frame and before the battle starts): the scheduler (section 4) may push frames, then each entity in order runs its **top frame** (the highest-index live frame). Each opcode handler returns the length to add to the program counter; `0` means "stay" (a **blocking** op: the dispatcher calls it again in the same tick, each call spending one unit of the budget, so it is in effect retried next tick); `0x7FFF` means "stop this entity for this tick".
* Instruction budget: 0x40 per entity per tick. Ops 59 and 67 set the remaining budget to 0x20; 66 and 2C do not touch it. A loop through 59 therefore ends only at a halt, a block or a stop. If the next opcode is `00` the budget is 1.
* **halt (00)** frees the top frame and then always resets frame 0 to the slot-1 script and ends the tick. Slot 0 therefore runs once and slot 1 re-runs from its start every tick. The frame that runs is chosen again before every instruction (the highest live frame), so a live frame between frame 0 and the freed one resumes next. A pushed frame (slot 2/3/4) suspends everything below it and its `halt` returns to the idle loop. Frames above 0 are pushed only by the scheduler.
* OpenCross (`battle::pac::Vm`) keeps the call depth per frame, clears it when halt resets frame 0, ends the entity on a fifth nested call and treats `0D` at depth 0 as halt. That differs from the shared depth only when a halt, a pushed frame or a return runs at a depth the matching calls did not leave, and a run of every scene through `ocbattle pac-soak` reaches none (639,725 halts, 11,424 calls, none inside a subroutine, none with three frames live, no return at depth 0, no fifth call).
* The 2D delay counter is decremented by 1 per tick for every armed entity, so `2D n` waits `2n` ticks.
* Opcodes with length 0 in the table (03 04 06 0C 7F BE BF C6-DF E5-FF) stall forever; the no-op opcodes of non-zero length (0A 0B 0F B2 B3 B5) skip their length. Stub opcodes (list in 5.4) appear only after a terminator and never execute.
* **Operands.** Most instructions are "S-mode": operand k is the u16 at byte `1+2k`; it is an immediate if bit `7-k` of the instruction's *spec byte* (its last byte) is set, otherwise a variable byte offset (`vars[(u16 & 0xFFFE)/2]`). Op 02 is the exception: its spec byte is byte 5, in front of the jump target. "W-mode" ops have no spec byte: a u16 with bit 15 set is an immediate (low 15 bits), otherwise a variable. Destination operands (`d`) are always raw variable byte offsets. Arithmetic is 16-bit; `div` and comparisons are signed.

## 3. Variables, unit fields, target codes

### 3.1 Variables

`vars` is a u16 array addressed by **byte offset**. Per-entity locals are compiler-allocated from 0x400 up (entity 1: 0x404.., entity 2: 0x418..); this is not enforced. The engine writes these reserved offsets:

| offset | meaning |
|---|---|
| 0x00..0x0E | start as the field variables 0x3D0..0x3DE and are written back to them when the battle ends (not after a monster-party fight). 0x04..0x0E are Pip's colour points, order green, white, black, red, yellow, blue (read by A7; zeroed by the engine at a Pip form change): every element action, a PC's or an enemy's, a technique, item or summon alike, adds 1 to the point of its record colour (byte +5) while character 43 (Pip) is in a party slot, and 1 more when Pip is the caster; one action counts once whatever its targets, and a colour byte that is not a single colour adds nothing |
| 0x10..0x1A | six colour/element slots written by 8D, copied by the engine for a cast |
| 0x20, 0x22, 0x24 | global last attack: type (0 weapon, 1 element) / value / target mask. Init 0xFF, 0xFFFF, 0 |
| 0x26, 0x2C, 0x32 | the same triple per PC (PC slots 0-2). The value is the hit strength 0 / 1 / 2 (weak / strong / fierce) for a weapon attack, not the weapon class; for an element it is the element id |
| 0x38, 0x3A, 0x3C | field-ring colour slots 1-3 (refreshed every tick: 0..5 the colour, 6 all colours; an empty slot leaves its variable unchanged) |
| 0x3E, 0x40, 0x42 | PC character ids |
| 0x44..0x4E | monster ids of units 3-8 |
| 0x50, 0x52 | 0x50: the PC whose command was confirmed last (unit index; enemy turns never change it); 0x52: colour of the last element cast (0-5, 6 = all colours) |
| 0x54/0x56 | battle timer, int32 in frames (op 89 divides by 60) |
| 0x58 | set on PC command confirm: 0 for a weapon attack, else the element's flag (record +4 bit 0x10, "hits its whole range"). evt409/410 (Boxer Boys) read it: set means the "cheaters" dialogue |
| 0x60..0x7E step 2 | the save's Dopplegang pool (16 words), loaded at battle start |
| 0xDE | the value element mode 41 copies into the caster's control word (`battle_data.md` 5) |

evt409 additionally uses 0x82 as its own global (0xE3/0xE4 = the boxer holding the treasure); it is not an engine variable.

### 3.2 Unit fields (ops 75 read_unit / 76 write_unit)

The unit operand is `0xE0..0xE8` (units 0-8) or `0xE9` (own unit). The field word C selects the field: low 9 bits = byte offset inside the block; `0x4000` = 16-bit access (else 8-bit); `0x1000` = the turn-timer block (`+0` countdown, `+2` gauge); `0x2000` = the derived-stat block; otherwise the unit's main stat block. Writes are raw byte/u16 stores with no clamping (HP written before max HP keeps its value).

Words seen in the corpus:
* Main block, 8-bit: `0x01` innate colour byte, `0x18..0x1D` ATK DEF HIT EVD MAG MDEF, `0x2D` model/kind byte (Pip's form), `0x2E` action points, `0x20` source record id, `0x14` gender byte, `0x1BF` home facing byte (home yaw = byte << 6; at battle start the scene record's facing byte of the slot << 3, rewritten by op 85), `0x19B` current facing byte (follows every turn of the model; op 85 writes it too, not modelled by OpenCross). 16-bit (`0x4000 |`): `0x16` max HP, `0xB8` current HP, `0xA0` drop row, `0xA2` gold, `0xA8` / `0xAA` permanent status lo halves, `0xAC` / `0xAE` permanent status hi halves, `0xB0` / `0xB2` inflicted status lo halves, `0xB4` / `0xB6` inflicted hi halves (also readable at `+0x100`: `0x1B0..`), `0x16C` countdown, `0x16E` gauge, `0x19E` stamina.
* Turn-timer block (`0x5000 |`): `0x5000` countdown, `0x5002` gauge, `0x5032` stamina.
* Derived block (`0x6000 |` for 16-bit): `+0 / +1 / +2` hit bases of the weak / medium / heavy hit (a script write replaces the base), `0x6004` weapon power, `0x6006` armour term, `0x6008` EVD total, `0x600A` MAG term, `0x600C` MDEF term, `0x6010` / `0x6012` the lo status-immunity word (low / high half), `0x6014` / `0x6016` the hi one.
* The `0x40B2` bit `0x10` is the charm test of target code `0x25` (status bit `0x100000`).
* `0x6012`: the status-immunity mask. It is rebuilt at every stat refresh as the unit's own resist word OR the armour's OR those of the defence accessories (gear flag 0x40, `battle_data.md` 11.1). A script write of bit n grants immunity to low status bit 16+n until the next refresh (265 read+write pairs in the corpus).
OpenCross: the immunity mask is `Unit::skip_lo`. A word it does not decode reads 0 and is ignored on write (logged as a stub); a gauge write is clamped to -1..gauge maximum and a max HP write to at least 1.

### 3.3 Target codes (op 2B operand B)

The result is a unit bitmask (`1<<u`). "Alive" = in the battle's alive mask, not KO'd, HP != 0 (a frozen unit is alive; ops 57 / 80 count it too); "targetable" additionally requires the unit's untargetable flag (set by op 5B) to be 0.

| code (count) | meaning |
|---|---|
| 0x00 (489) / 0x10 (1416) | random alive targetable PC / enemy-side unit |
| 0x01 (543) / 0x24 (34) | mask of all alive PCs / all alive enemies |
| 0x18 (2671) | random alive unit of either side except self (charm/berserk PC AI, "any other") |
| 0x19 (61) | random alive enemy-side unit except self |
| 0x25 (803) | if self is charmed (status bit 0x100000): as 0x18, else as 0x00 |
| 0x17 / 0x1B | the first enemy (3..8) / PC (0..2) of the start roster whose KO bit is set, else 0. The roster is the starting party/enemy set and is never cleared, so this is not a "last attacker" set. Only evt402 (Solt) uses it: nobody down -> Cure Plus (86) on Serge, else Revive (94) on Serge |
| 0x1C | random alive roster unit (no untargetable test) whose group tag (op 8E) differs from self's |
| 0x1D..0x23 | the same, with group tag = code - 0x1D |
| 0x1A, or any low byte >= 0x50 | the PC whose character id = `(code+0x80)&0xFF`, alive and not untargetable (0x80 -> id 0, Serge); none -> mask 1 |

Pick draws: codes 0x00 / 0x10 / 0x18 / 0x19 / 0x25 draw `rnd(lo, hi)` until a valid unit comes up (0x00 = 0..2, 0x10 = 3..8, 0x18 / charmed 0x25 = 0..8 except self, 0x19 = 3..8 except self). Once every unit of the range has been drawn without success the result is unit 0's bit (mask 1). Retail never returns for the self-excluding 3..8 range with no candidate; OpenCross stops after the range. Codes 0x1C..0x23 draw `rnd(0, 9)` until a roster unit passes (retail loops forever with none; OpenCross returns 0 without drawing). OpenCross draws the same number of RNG values as retail, which matters for matching retail sequences: a 0x25 pick that retries advances the RNG once per retry plus one.

The corpus uses only 0x00 0x01 0x10 0x18 0x19 0x1B 0x1C 0x24 0x25 0x80. Other codes give an undefined result (OpenCross: 0).

**Element pick (ops 55 / 8F).** Cells of the element grid rows 0..gauge (55) or 0..6 (8F) are drawn with `rnd(0, rows*10 - 1)` until a free cell (usable, not yet used, no weapon-tech flag) holds an accepted element: id < 0x7E, element record byte +0x14 == 0xFF, byte +0 & 0x44 == 0, byte +4 & 0x41 == 0 (`battle_data.md` 5), and the element has a target. A free cell that fails is closed. Target: record byte 4 bit 0x10 = every alive unit of 3..8 (55) / 0..2 (8F); otherwise `rnd(0, 8)` (55) / `rnd(0, 2)` (8F) until an alive, targetable unit other than the caster comes up (no side or heal rule); none -> the next cell. Nothing found: element 0, target 0xFFFF. The pick marks its cell used; op 55 also lowers the caster's gauge by the cell's row + 1 (op 8F leaves it).

## 4. Scheduler: when each slot runs

Each tick, before interpreting (skipped on the first frame, while a battle-state value of 4 is set, or before the battle starts). OpenCross launches the slots from the battle rules instead (section 6):

1. **Slot 3** (killed), enemy units 3-8 only: an enemy unit in the alive mask with HP 0 or the KO bit, whose slot-3 flag is clear, pushes slot 3 and sets the flag. An enemy's A6 clears the flag; a PC revive clears it too; no PC launch exists in this overlay.
2. **Slot 4** (attacked): the executor leaves a target list pending only at the end of an action whose first actor is a PC (unit 0-2) and whose first target is an enemy (unit 3-8); the target list is built in ascending unit order from the target mask (an empty mask lists the actor), so a list holding any PC, and every enemy-led action, leaves nothing pending, and while a list is pending the time scheduler does not run. If no actor lock is held and a list is pending, the list is latched and one unit per tick is popped; if that unit's context has neither the slot-4 nor the slot-3 flag set and the unit is alive, push slot 4 and **take the actor lock**.
3. **Slot 2** (about to act) only when no slot-4 list is pending and no scripted death/cutscene is running: a cursor walks units 0-8 (up to 9 per tick, stopping when the lock is taken). An alive unit whose countdown is 0 is handled: an enemy gets countdown 0xFFFF and, if its slot-2 flag is clear, pushes slot 2 and takes the lock. A PC runs its slot-2 script only if it is confused/charmed (status mask `0x20100000`) or a global override is set; otherwise its countdown is reloaded from its action points and the command menu owns the PC. PC slot 2 is the confused/charmed AI (random `0x18` target).
4. Interpret all entities; then advance the B0/B1 tweens.

The actor lock is released by A4 (slot 2), 2A and a PC's A6 (slot 4 / PC death), and by the halt that pops a slot-2 / slot-4 frame, so slot 2 and slot 4 never overlap. Every other entity's slot-1 loop keeps running, except an entity out of the run list.

**Run list.** The per-frame pass walks a run list of entities (one byte per entry, 0xFF = skipped), reading an entry only when that entity's turn in the pass starts. An enemy's A6 writes 0xFF into its own entry, so the entity finishes the running pass (the `00` after A6 pops the slot-3 frame) and is not interpreted again, its slot-1 loop included. An element revive (mode 4) writes the entity back into the run list: the first entity bound to the revived unit, a PC's slot-3 flag cleared as well; the enemy's slot-3 flag was cleared by its A6, so a revived enemy can run slot 3 (and slot 4) again. A bind (`0E`) to a unit that is not alive also writes 0xFF. OpenCross: `pac::Vm` marks the entity dropped at the end of the pass, `Vm::revive` (called through `EnemyBrain::on_revived` by `Battle`'s mode 4) clears it.

**What a waiting script holds.** E0 / E2 / 2D / A4 stop only the entity that executes them (its frame retries the op each tick). While Radius' slot 2 sits in `E2`: (a) the actor lock stays set, so no other unit's slot 2 / 4 can start and the turn waits; (b) every entity's slot-1 loop keeps running; (c) the action executor keeps playing whatever is already queued, so `67 x3, 2D 60, E0 02` queues three attacks, they play while the 120-tick delay runs, and window 2 opens 120 ticks after the queue ops, over the running attack animations; (d) the PC command menu is blocked only by AF or by executor states, not by the dialogue window, but an open window owns the pad. Retail order for evt386's slot-2 body `E0 01, E2, 67 x3, 2D 3C, E0 02, E2, E0 03, E2, E0 04, E2`: window 1 -> button -> attacks start -> 120 ticks -> window 2 -> button -> window 3 -> ...

**Enemy turn (slot 2) shape**, from evt001: `59; 75/19 status test; 02 ... -> 66 (begin action) -> 87 (clear queue) -> 1F coin / 2B select_target / 55 pick_element -> 67 x n -> 59 -> A4 -> 00`.
* **66** blocks until the PC command state is idle (return walk finished), then marks an action in progress.
* **67** only appends to the unit's attack queue (8-byte entries); it does not wait.
* The **action executor** takes the first queued entry of any unit every frame, loads kind / strength / element, plays the sequence, applies damage, pops the entry, and when the queue is empty finalises (countdown, action state cleared).
* **A4** blocks while the action is in progress, so the script resumes only after all queued attacks executed; then it reloads the countdown (`AP + rnd(0, AP>>2)`), clears the slot-2 flag and releases the lock.
* Slot 3 = `65; A6; 00`: 65 enters scripted-death mode, A6 finalises (enemy: remove from alive mask, add to the defeated mask, kill credit, check "all enemies down", clear the slot-3 flag, leave the run list; the lock is not touched). Every PC entity carries this slot 3 too, but slot 3 is launched for enemies only, so the PC branch of A6 never runs. Slot 4 = `2A; 00` in every normal script.

**Radius tutorial timing (retail).** evt386 entity 4 slot 1 writes max HP and current HP (`75/76` words 0x16 / 0xB8): 0x400 each tick while var 0x84 < 2, 0x32 afterwards. Nothing in evt386 writes the countdown, so Radius' countdown is the engine's: AP 12, initial `rnd(1, AP)`, `-= steps` per PC hit (weak 1, strong 2, fierce 3 = stamina spent / 10), reload `AP + rnd(0, AP>>2)` by A4. Radius acts only after the PC has spent its stamina; the PC may hit several times. `66 begin_action` (and 65) waits for the PC to be back to idle, i.e. the return walk must have finished.

## 5. Opcode table

Operands: `d` raw destination var; `A B C` S-mode values (spec byte last); `W` W-mode; `t` u16 jump target (offset from `base`). `len` includes the opcode byte.

### 5.1 Control flow and arithmetic

| op | len | operands | name | semantics | conf |
|---|---|---|---|---|---|
| 00 | 1 | | halt | section 2 (13737) | C |
| 01 | 3 | t | goto | pc = t (17306) | C |
| 02 | 8 | A B spec t | cond_jump | condition = `spec&15`: 0 ==, 1 !=, 2 >, 3 <, 4 >=, 5 <=, 6 A&B != 0, 7 !=, 8 A or B nonzero, 9 bit B of A set, 10 clear (the bit mask comes from a table indexed by `B & 0xFF`, equal to `1 << B` for B below 16; no shipped script uses modes 9 or 10). **True: fall through (+8); false: pc = t.** spec bit 7/6 = A/B immediate (0x40..0x46 dominate; 0x40 and 0x41 most common) | C |
| 05 | 3 | t | call | push pc+3, pc = t | C |
| 0D | 1 | | return | pop; with an empty call stack it acts as halt | C |
| 0E | 3 | W u | bind_unit | if unit `u+0x20` is alive: bind it; else unit = 9, entity dropped from the run list, tick stops. First op of each entity's slot 0 | C |
| 10 | 6 | d B spec | set | d = B (24109) | C |
| 11 / 12 | 3 | d | set1 / set0 | d = 1 / 0 (unused) | C |
| 13 / 14 | 6 | d B spec | add / sub | d += B / d -= B | C |
| 15 / 16 | 6 | d B spec | bit_set / bit_clear | d \|= 1<<B / d &= ~(1<<B) | C |
| 17 / 18 | 3 | d | inc / dec | | C |
| 19 / 1A / 1B | 6 | d B spec | and / or / xor | d op= B | C |
| 1C / 1D | 5 | d W | shl / shr | unused | C |
| 1E | 3 | d | rand15 | d = `rnd(0, 0x7FFF)` (table-driven RNG; unused) | C |
| 1F | 5 | d W | rand_mod | d = `(libc rand() & 0xFFFF) % (B + 1)`. `B` = immediate 1 is the 50% coin (9333). Uses the libc LCG, not the 256-byte table walk of `rnd`. OpenCross: `Host::libc_rand` (`GameRng::libc_rand`, 15-bit LCG); using `rnd` made evt409's `1F 82 01` (which boxer holds the treasure) pick the same boxer in every seed | C |
| 20 / 21 | 6 | A B spec | mul / div | var(A) = A*B / A/B, 16-bit signed | C |
| 59 | 1 | | tick_hint | sets the op budget to 0x20, otherwise a no-op (10767) | C |
| 2D | 3 | W n | delay | first run arms the counter to `2n` and blocks; continues when it reaches 0 | C |
| 77 | 1 | | pad | stub; padding after a terminator (614) | I |

### 5.2 Targets, units, queued actions, data queries

| op | len | operands | name | semantics | conf |
|---|---|---|---|---|---|
| 2B | 6 | d B spec | select_target | d = target mask of code B (3.3) (6042) | C |
| 67 | 8 | A B C spec | queue_attack | A type, B power/element, C target mask (spec 0x80/0xC0/0xE0 = which are immediate). Mask is intersected with the alive mask (OpenCross: with the roster mask, i.e. the PCs, the living enemies and the enemies owing a scripted death); empty -> self (types 1,2,3,6) or unit 0 (types 0,5). OpenCross keeps at most 4 entries per unit; a fifth is dropped. Type **0/5** weapon (B > 2 -> 0; 0 weak, 1 medium, 2 heavy); **1,2,3,6** element/tech id B (6 forces the element trap: the cast first meets the other side's trap as any cast does, then the trap fires whatever it holds, so the cast plays its colour's trap pack and lands nothing, the enemy side's trap is emptied and the trap block hidden; a colourless element casts normally. evt478, the only user, casts Black Hole (36) this way: on the PS1 the enemy trap word 5 -> 0, the trap banner 1 -> 0, the target's HP unchanged, the script counter var 0x80 + 1; OpenCross `EnemyAction::trap_fizzle`, `Battle::catch_in_trap`; type 2 also copies vars 0x10-0x1A as the colour slots); **4** reset own countdown `AP + rnd(0, AP>>2)` and clear the slot-2 flag. Each entry is one executor action whose target list is the units of the mask: an element entry is one cast at all of them (each listed unit hit once, the field ring and the cast-colour history take the colour once; an element hitting its whole side still walks the side); OpenCross runs a weapon entry as one attack per listed unit, and keeps consecutive weapon entries of one plan in one action; every element entry is its own action (`Battle::run_plan`). For PCs it also records the last-attack info. Corpus types 0:8277 1:6116 2:43 3:3 6:1 | C |
| 75 | 8 | d B C spec | read_unit | d = field C of unit B (3.2); spec 0x60 typical (7723) | C |
| 76 | 8 | A B C spec | write_unit | field C of unit B = A, no clamp (1054); HP 0 written here starts the kill script but is no KO: no status bit, no dead flag on the model | C |
| 55 | 5 | d1 d2 | pick_random_element | self: random usable element cell of levels 0..gauge (3.3); mark it used; d1 = element id (0 none), d2 = target mask (all-targets flag -> whole side, else random alive unit != self) or 0xFFFF | I |
| 8F | 7 | d1 d2 B | pick_element_of | as 55 for unit B over all 7 levels (rows 0..6) | I |
| 56 | 5 | d W u | read_flag | d = per-unit off-stage flag of u (see 34) | I |
| 57 | 3 | d | count_enemies | d = alive enemy units (3-8, not off-stage) | C |
| 80 | 3 | d | count_pcs | same for units 0-2 | C |
| 2F | 5 | d W col | count_colour | `col` (low byte) 0..5: d = number of ring slots (of 3) holding that colour; 8..15: the number of empty slots; any other value: the size of the largest group of equal slots, where the empty slots and the all-colour slots each form a group (ring [empty, empty, green] gives 2, an empty ring 3). The shipped scripts pass 0..6 only (6 from an immediate or a variable) | C |
| A7 | 5 | d1 d2 | pip_evolve_check | Pip evolution check. d2 is a variable holding the unit (E0..E8). With light = green + white + blue and dark = black + red + yellow (vars 0x04..0x0E) and the unit's kind byte (field 0x2D): kind 0x2B gives 0x80 (dark >= light and dark >= 100) or 0x81 (light > dark and light >= 125); kind 0x2C 0x82 (dark >= 300) / 0x83 (light >= 200); kind 0x2D 0x84 (dark >= 200) / 0x85 (light >= 300); else 0. d1 = the form element or 0; a nonzero result is also kept as the pending form: while one is pending the next PC turn runs the PC's slot-2 script (Pip casts the form element on himself with 67, element mode 45 changes the form) instead of the command menu | C |
| 8E | 3 | W g | set_group | group tag = g (0, 1, 2 in the corpus) | C |
| 8C | 3 | W m | mark_defeated | defeated mask \|= m | I |
| 8D | 13 | 6 x W | set_slots | copy operands != 0xFF into vars 0x10.., pad with 0xFF | C |
| 83 | 3 | W r | set_result | pending battle result `r - 0x80` (signed), always written, so `r` >= 0x80 clears a pending result; negative = pending: the end test adds 0x80 back (0 / 1 / 2 = Victory / Wiped / Fled, the normal sequences) and ends the battle at its next test, before the wipe / victory tests, without pose or fanfare. evt409 / 410 use 0 after the "presents" element, even when the triggering attack missed (slot 4 runs for every listed target, hit or not). OpenCross: `forced_result()` -> `Outcome::Scripted` | C |
| 84 | 3 | W u | force_quarter_hp | sets the unit's low-HP control flag 0x80000000 (the animation scripts' flag 31). The flag is otherwise recomputed from the HP (HP <= max / 4) when a hit lands on the unit, so it holds until the next hit; a script HP write does not recompute it. Every use writes the HP to 1 just before and then restarts the idle script (`58 1`), which picks the wounded stance at once (scene 450: HP 30000 -> 1 and the flag set in the same frame). OpenCross: event `QuarterHp`, `Visual::low_hp_forced` | C |
| 89 | 7 | d B C | timer_ge | d = (timer/60 >= B + C*3600) | C |
| 96 | 3 | W v | invuln_except_colour | stores the low byte of v as the colour gate (1 green, 2 white, 3 black, 4 red, 5 yellow, 6 blue; 0 = none). Per target of an element or weapon action, after the mode wrote the target's result word: when the actor is a PC, the target an enemy, the gate non-zero, and the action is a weapon hit or an element whose colour (record +5 >> 2 as a colour number, colourless and multi-colour = no match) is not the gate, the result word becomes 0. The statuses and the hit code the mode wrote stay; the number shows "0" and the HP does not change. Only user: `evt463` (Royal Jelly) `96 04 80` = red. OpenCross: `Battle::colour_gated`, `ActionEvent::gated` | C |
| 7C | 1 | | no_drops | disables drops for this battle | C |
| 7B | 3 | W v | reset_menu_cursor | units 0-2: remembered command-menu cursor = v. OpenCross: `Unit::menu_cursor` | I |
| 7D | 2 | b | set_menu_entry100 | b = 1 shows the enemy letter suffix in the target-name label (glyph 100 and the one after it are zeroed when 0). Only the target label honours it. Cleared at battle init. Letters: enemies spawn group by group; in a group of more than one the instances get A, B, C in slot order (a group of one: none), restarting per group. RDE draws "-A" (non-JP) / "・Ａ" (JP). The op sets the flag the target label reads (`menu_entry100()`), `Unit::letter` holds the letters | C |
| 81 | 2 | b | set_end_check | end-of-battle test enabled = b (set to 1 at battle start). 81 0 suspends the victory/defeat check (scripted boss finales). OpenCross: `end_check_enabled()`, consulted by `Battle::check_end` | C |
| 39 | 2 | b | set_ui_enable | b = 0: the per-frame battle HUD (party panels, field ring, command box) is not drawn until b != 0; the Time Devourer's death script (evt472) clears it before its last casts. OpenCross: `ui_enabled()`, `BattleView::hud_script_off_` | C |
| AF | 2 | b | set_menu_lock | while b is nonzero the player command-phase driver does nothing (no menu opens). OpenCross: `menu_locked()` | I |
| AE | 5 | d W | colour_seq_match | d = 1 if the cast-colour history (every element action, PC or enemy; last 7 entries) equals `5 4 1 6 3 2 7` (colour+1: yellow red green blue black white, then a colourless cast: the finale's "Chrono Cross" order), else 0; on a match the battle sound call is muted | C |
| AB | 5 | d W | colour_seq_next | d = the colour (0-5; 6 = sequence complete) still needed next: find the last `5` in the history at p and check the following entries against the pattern; a wrong entry or no start -> 4 (yellow); an empty slot -> that slot's pattern colour - 1; p = 6 -> 3. If W == 0 the result is mapped through `5 3 2 6 1 4 7` (`table[r]-1`, the hint colour in the alternate order) | C |

### 5.3 Turn flow, presentation, engine calls

| op | len | operands | name | semantics | conf |
|---|---|---|---|---|---|
| 66 | 1 | | begin_action | blocks until the PC command state is idle, then marks an action in progress (5286) | C |
| 65 | 1 | | begin_scripted | same wait as 66 (blocks until the PC turn is open again, i.e. the running action has ended, every reaction script included, and the menu state is at most 4); scripted-death mode. PC: sets the scripted flag. Enemy: when a PC stands engaged at the enemies and no approach / return runs, starts the engaged return hop and waits for it; Dopplegang pool credit (the last confirmed PC (var 0x50) is Sprigg or wears accessory 0xFC, and the enemy's `+0xA6` bit 7 is clear: pool bit from record `+0x2B`; OpenCross `Battle::dopple_credit`) | I |
| 87 | 1 | | clear_queue | clears the scripted flag, own queue flags and depth; resets action state (5286) | I |
| A4 | 1 | | end_turn | section 4 (2589) | C |
| A6 | 1 | | end_killed | PC unit: clears slot-4 flag and releases the lock (blocks while scripted); enemy: death finalisation, clears the slot-3 flag and takes the entity out of the run list (section 4). Blocks while the move-wait word is set: an approach script start (S5 entry 2 on the acting list) sets it to 1, the engaged return hop (S5 entry 3) to 2, and any anim script that ends on op 0x3E clears it. OpenCross: the view holds the `Killed` event while `BattleView::move_wait_` is set (section 7) | C |
| 2A | 1 | | end_attacked | if no action is in progress: clears the slot-4 flag and releases the lock; else blocks (2481) | C |
| 2C | 1 | | begin_cutscene | when allowed: UI hidden, then as 66 | I |
| 38 | 1 | | end_cutscene | UI shown, action state cleared, lock released | I |
| 97 | 1 | | begin_sequence2 | as 66 gated by another flag (unused) | U |
| 34 | 2 | b | ghost_self | self off-stage and untargetable = (b != 0); the model's hidden bit follows. b == 0 also sets the unit's bit in the all-target cast mask (seeded at battle start from scene byte +0x1B, `battle_data.md` 6); b != 0 leaves it alone | C |
| 35 | 3 | b e | ghost_entity | as 34 for entity e | C |
| 5B | 4 | f W u | ghost_unit | **not 34 for another unit**: writes only the untargetable flag (f != 0) and the cast mask bit (f == 0 sets, f != 0 clears). The off-stage flag (what count_enemies / read_flag / victory read) and the model's hidden bit stay, so a unit the spawn seed put off-stage stays off-stage under `5B 0` (scene 266's Bomber). OpenCross: `Battle::untargetable`, `Host::reveal` / `Host::conceal` | C |
| 5C | 4 | f W u | set_scripted_death | per-unit flag = (f == 0). When a unit is defeated, if the flag is 0 the standard death presentation runs; A6 clears the flag. So `5C 0` = the script performs the death itself. An owed unit in the alive mask blocks victory, hidden or not. OpenCross: `scripted_death(u)`, `ScriptedBrain::death_owed` | C |
| 3C | 3 | W a | model_call | model call on self (unused) | U |
| 3D | 3 | W a | set_anim | self model pose request = a (event `Pose`; the idle script reads it back; 8 = dead, 1 = stand-by) | I |
| 58 | 3 | W a | play_anim | start animation `a & 0xFF7F` on self; bit 0x80 means "only if no animation stream is currently running". Event `Anim`; `BattleView::script_event` starts the model's S5 entry | C |
| 5A | 4 | e W a | play_anim_of | same for entity e's unit (skipped if unbound); the word as is (>= 0x80 = the external attack table), no gate | C |
| 85 | 5 | W v W u | set_home_facing | the unit's home facing byte (unit byte 0x1BF) and current facing byte (0x19B) = v, and its model turns to yaw v << 6 at once; the menu step after a PC's element command turns every living unit to its home byte (`battle_stage.md` section 6). evt394 (Nikki, slot 4 of the three Cassowaries): once var 0x80 has reached 2, the next slot-4 run sets all three to 0x30 (yaw 3072, facing the party) from their formation yaws 512 / 1024 / 1536, and they keep it at every later facing step. OpenCross: `Unit::home_facing`, the `HomeFacing` event sets the view's home yaw and turns the model | C |
| 86 | 3 | W u | camera_target | camera/acting list = the unit of entity slot `(u + 0x21) & 0xFF` (arg `0xE9` = the script's own unit, `0xE0 + k` = slot `k+1`). The list is read by camera ref 1 / 9, `GetPos` list 0 (effect op 0x74) and list-0 attaches of scripts it starts, until the next writer (command setup, element menu, hit setup). OpenCross: `BattleView::script_actor_` outranks the last action's caster for those refs | C |
| 7A | 1 | | camera_self | camera list = self unit | C |
| 90 | 5 | W u W s | move_to | model of u := position of unit `table[s]-1` with x - 0x200000 (unused) | U |
| 91 | 3 | W u | sync_pos | copies the model position (>> 12) of u (E9 self) into the unit position cache; presentation only (event `SyncPos`) | I |
| A9 | 4 | e a | model_call2 | model call on entity e (unused) | U |
| B0 / B1 | 4 | A | lift / land | a 10-pass height tween of the entity's unit model. B0 stores the step `A / 10` (signed, truncated) in the entity; pass k = 1..10 moves the model y (down) by `k x step` for B0 and by `-k x step` for B1, so B1 after a B0 returns the model to where it was (total 55 steps) and B1 with no B0 before it (step 0, the entity's start value) only waits. The op blocks until the 10th pass is done and continues on the next one (evt399 `B0 -200`: y 0 -> -20, -60, ... -1100 over 10 game frames, the op passed on the 11th; evt417's kill-script B1: 10 frames, y unchanged). B1 ignores its operand. OpenCross: blocks `kLiftTicks` = 20 ticks (two per pass), event `Lift` (the view moves `Visual::y` every second tick) | C |
| E0 | 4 | idx | dialogue | opens dialogue window 4 with record `idx` of the pac's part 1; blocks only while window 4 is still open/closing (state != 0). **It does not wait for the button**; `E2` does. The battle goes on; only the entity (and, holding the lock, the turn) waits (section 4). Record `idx*8` = `{u16 text offset, u16 width in cells, s16 lines (max 4), u16}`; text runs to the next record's offset, game charset (codes 00 mend, 01 LF, 02 bpage, 04 wait n, 06 wend n, 07 item, 09 enemy name, 0E colour, 15 playwork, 1A variable). Frame grows from the centre; width `cells*12 & ~3`, height `lines*16`, text x = `32 + (22 - cells)*6`, rect y 0x2C, text y 0x2E (op 62 overrides x/y, text y + 1). RDE replaces the text with `btel_<lang>.csv[(remapped id << 16) + idx]` (14 battle ids read another battle's rows) and fits the window to it: cells = clamp((widest line in PS1 px + 11) / 12, 1, 26), lines = clamp(most lines of any `{bpage}` page, 1, 5) (`text.md`). The system message plate (window 3, `battle_data.md` +0x13, `rules/battle.md` 2.8) is a separate window: E0 never waits for it and neither window replaces the other. OpenCross: `BattleView::script_pump` + `smsg_*` (section 7) | I |
| E2 | 1 | | wait_dialogue | returns 0 (retry) while the text is not yet fully shown or the window is not fully open, and for page-break states until the page button (button class 5/6) arrives; from `{mend}` state it continues once the button (or `{wend}`) starts the close. So the script resumes when the window starts to close, but the next `E0` blocks until it is fully closed. An open window owns the pad. Only window 4 counts: a system message plate open at the same time neither holds nor releases E2 | I |
| 62 | 5 | W x W y | dialog_pos | dialogue window position x, y (text y = y + 1); applies to the next window only | C |
| C0 / C2 | 5 | u32 tag | effect_start_a / effect_start_b | C0 starts a type-0x10 timeline, C2 a type-0x0C camera shot, by 4-character tag ("face" 109x, "zen0", "ev01") looked up in the resource list (battle.prd, the stage, the loaded packs; newest registration wins). OpenCross: `BattleCamera::play_sequence` / `select` (`ev01` is not in the view's bank) | C |
| C1 / C3 | 5 | | effect_stop_a / _b | stop every pool-A timeline (also clears the screen-copy warp) / tear down type-0x0C shots; operand ignored. OpenCross: C3 is accepted and does nothing | C |
| C4 / C5 | 5 | u32 tag | effect_wait_a / _b | block while pool A / B holds an object with that tag (0 = any object). The battle UI is held the same way while any effect runs. Headless host: `effect_busy()` defaults to false; the op passes and emits `EffectWait`, which holds the script stream while a camera shot of that name still runs (C5; `BattleCamera::shot_running`). Only user: evt386 `C2 ev01 ... E2; C5 ev01`; on the PS1 the shot is over (the camera has not moved since the intro shot ended) by the time the message closes, so C5 passes at once there (closed 24 refreshes after it opened: no hold) | C |

### 5.4 Not executed

* Opcodes with a table length that no shipped script contains (confidence U; OpenCross advances them by their length and `ocbattle pac-soak` lists any that runs): 07 08 09 0A 0B 0F 22-29 2E 30-33 36 37 3A-3C 3E-54 5D-61 63 64 68-74 78 79 7E 82 88 8A 8B 90 92-95 97-A3 A5 A8-AA AC AD B2-BD E1 E4. Their handlers are not modelled because no battle can reach them.
* E3 has table length 6 and its single occurrence is padding after a terminator; 77 is a stub, used as padding. Length 0 (stall): 03 04 06 0C 7F BE BF C6-DF E5-FF. OpenCross drops an entity that reaches a length-0 opcode.

### 5.5 Length table

```
00:1 01:3 02:8 05:3 0D:1 0E:3 10:6 13:6 14:6 15:6 16:6 17:3 18:3 19:6 1A:6 1F:5 20:6 21:6
2A:1 2B:6 2C:1 2D:3 2F:5 34:2 35:3 38:1 39:2 3D:3 55:5 56:5 57:3 58:3 59:1 5A:4 5B:4 5C:4 62:5
65:1 66:1 67:8 75:8 76:8 77:1 7A:1 7C:1 7D:2 80:3 81:2 83:3 84:3 85:5 86:3 87:1 89:7 8C:3 8D:13
8E:3 8F:7 91:3 96:3 A4:1 A6:1 A7:5 AB:5 AE:5 AF:2 B0:4 B1:4 C2:5 C5:5 E0:4 E2:1 E3:6
```

Other opcodes below 0xE5 with a nonzero table length never occur. `python -I tools/py/cc_tables.py bscript` prints the full table and histogram. Opcodes used by the early game (evt000-evt050): 00 01 02 05 0D 0E 10 13 14 15 17 19 1A 1F 21 2A 2B 2C 2D 2F 35 38 3D 55 56 57 58 59 5B 65 66 67 75 76 77 7A 7B 7D 80 87 8E A4 A6 A7 C2 E0 E2.

## 6. OpenCross implementation notes

* Opcode names, lengths and confidence live in `src/battle/pac_opcodes.inc`; the interpreter is `src/battle/pac_vm.cpp` (`Vm`), and `ScriptedBrain` (`src/battle/battle.h`) connects it to the battle rules. One VM context per entity; at most 0x40 ops per entity per tick; "return 0" retries next tick. The slots are launched by the rules, not by the section 4 scheduler: slot 2 by `plan_turn` when a unit's turn comes (after one tick of every entity's idle loop), slot 3 by `on_killed` once a unit is at 0 HP or KO'd, slot 4 by `on_attacked` for every living unit in the post-action target list. The rules run slot 4 at once, but the view holds the run's events (its `Mark`, a = 4, b = the action id) like a Barrier until that action has played, as the PS1 launches slot 4 only once the executor is idle: evt394's op 85 turns the Cassowaries after Serge's hit, not before it. Each run goes on until the pushed frame halts (30000 ticks at most, then it is unwound) and is followed by `Vm::settle`. Between a slot-4 run and its settle, `Battle::claim_ready_turns` takes the turn of every enemy whose countdown is already 0 (countdown 0xFFFF; `next_ready_unit` runs a claimed turn whatever the countdown reads later): retail's scheduler launches that slot 2 on the tick after slot 4 halts, before the idle loops' next pass, so a reaction cutscene that rewrites the countdown does not cancel the turn. evt450 (the Criosphinx) is the case: the first hit starts entity 0's cutscene (var 0x80 2 -> 3, var 0x82 = 1, countdown written 5); when that hit also ran the boss's countdown out, its slot 2 is already running. On the PS1 its fight routine (`call 0x067E`, reached because var 0x82 is already 1) attacks during the cutscene's dialogue; RDE holds that call (it returns without advancing) from the cutscene's `2C` to its `38`, so the boss attacks right after the cutscene. OpenCross runs the cutscene to its end in the settle before the claimed turn starts, which gives RDE's order without a script correction. RDE capture (battlefield 43, Serge / Kid / Guile, boss countdown 1 before one weak hit): hit, var 0x80 = 2 and the boss's countdown 0xFFFF on the same frame, var 0x82 = 1, dialogue 9 open 8 s, var 0x80 = 3 with the countdown 5 and the boss's four queue entries on one frame, its first all-party hit (element 0x15) 2 frames later; OpenCross the same order (`test_battle_scripts`). The battle timer (vars 0x54 / 0x56) is the number of actions times 180 frames. A fifth nested `call` drops the entity.
* Blocking ops in the VM: 66 65 A4 2A 2D 2C B0 B1 C4 C5 (A4 and 2A also wait for the unit's own queue to drain). A6, E0 and E2 only emit events (the view does the waiting, section 7); B0 and B1 block 20 ticks. 67 appends to a per-unit queue consumed by the action executor; A4 completes when the executor is idle. Debug: `OC_PAC_TRACE=1` prints every executed op.
* Tools: `ocbattle pac-disasm <scene>` prints the disassembly (the text of `cc_tables.py bscript dump`). `ocbattle pac-soak [turns] [-v] [--scene N | --from A --to B]` starts every scene's battle in a dummy party, runs every unit's slots 2 / 3 / 4 twice on a second battle, then plays `turns` automatic commands (default 40), and prints scene / battle / command / exception counts, outcomes, executed ops, scripted-turn statistics and the sets of stubbed opcodes, invalid opcodes and undecoded unit fields; `OC_SOAK_LIST=1` lists the per-scene outcomes.
* RNG: `1F` uses libc `rand()` (`Host::libc_rand`); 2B / 55 picks use `rnd(lo, hi)`.
* Engine flags set by ops 81, 39, AF are exposed as `end_check_enabled()`, `ui_enabled()`, `menu_locked()`; `Battle::check_end` honours the first, the battle view the others (39 through its `EngineFlag` event).
* A forced result (op 83) waits for the death casts. A kill script that queues casts (op 67), waits with A4 and then forces its code runs synchronously in OpenCross, so the code is set before its casts have been executed; on the PS1 the A4 holds the script until the action executor has played them. `Battle::check_end` therefore ignores `forced_result()` while a death plan is queued or running and applies it at the end test after the last one, whether the killing blow was an element or a weapon hit (a weapon command's end test runs right after each hit, before the reactions). The case in the data is the Time Devourer (scene 472): PS1 capture (battlefield 29, its HP set to 1 at the first command menu, one weak weapon hit): HP 0 on the hit, element 511 cast by unit 3 26 frames later, the battle overlay gone about 1230 frames after that. OpenCross before the rule: `battle over: scripted (code 3)` right after the hit and no cast; with it, 511 is cast and then the battle ends with code 3.
* Unknown: the exact identity of the two effect pools behind C0-C5 (a resource lookup by tag; they hold face/portrait and 3D effect objects).

## 7. OpenCross: script stream on virtual time

Retail ticks the VM every frame beside the action executor. OpenCross resolves a whole turn synchronously, so `ScriptedBrain` runs each slot to its end inside the turn and the view replays what the script did (`BattleView::script_pump`, `battle_view_script.cpp`).

* **Virtual time.** `Vm::tick()` counts `vtime()`. 2D delays are real (`2n` ticks; the debug `set_skip_delays` remains for tests). Every `Event` carries `vt` (virtual tick) and `seq` (battle-wide emission order). The view releases events in order, waiting the `vt` gap to the previous released event (the script's own elapsed time; E2 and executor waits are not counted, they happen in the view).
* **Settle.** After every slot run (and at battle start) `Vm::settle()` keeps ticking until every entity is back at the start of its idle loop with nothing armed (cap 1500 ticks); a variable an earlier entity already read in that tick and a later one changed keeps it going one more tick (the earlier reader sees the new value on the next tick). A read of a variable the reading entity wrote itself earlier in the same tick does not count: every PC entity's idle loop writes its own unit into one shared global each tick and, for a Pip whose form can still change (kind 0x2B..0x2D), passes that global to A7, so a later PC entity's write would otherwise keep every settle running to the cap and put a 1500-tick gap into the presentation after every action. A slot-1 loop can queue an attack that no slot run drains, and its A4 would wait for it forever: settle moves the entry to the idle list (`Vm::take_idle_attacks`), `Battle::run_idle_plans` plays it as its own action, and the view holds what follows the A4 until that action has finished. This is how idle loops react to what the slot did: evt409's entity 0 slot 1 sees var 0x84 change (the boxer's slot 4 sets 2 / 4) and shows "WROOONG" / "cheaters". Slot-1 `3D` / `58` repeats with identical operands are dropped at the source.
* **Event kinds.** `Queue` (op 67; `QueuedAttack::seq` / `EnemyAction::seq` / `ActionEvent::script_seq` carry its ordinal so a queued attack starts only after its Queue event is released), `Barrier` (2C 65 66 A4 2A passed; `b` = Battle action id; the view holds the stream until the executor has finished the groups queued before it), `Mark` (a slot run starts; carries the run's start `vt`). Every other presentation op emits an event of its own kind (`Dialogue`, `WaitDialogue`, `DialogPos`, `Anim`, `Pose`, `Camera`, `Cutscene`, `EffectStart`, `EffectStop`); the view consumes those in `BattleView::script_event`, while `SetResult`, `NoDrops`, `Defeated`, `Killed`, `Ghost`, `Invuln`, `QuarterHp`, `Shade`, `SyncPos`, `ScriptedDeath`, `MenuCursor`, `EngineFlag` (39 / 7D / 81 / AF) and `SfxMute` are read by the rules.
* **E0 / E2 in the view.** E0 waits while window 4 is open/closing, opens the window and goes on; E2 waits until it has closed. The system message plate has its own state (`BattleView::plate_`) and is drawn beside window 4; E0 / E2 look only at window 4. Attacks released before keep playing under later windows; window 2 opens `vt` gap ticks after the attack was released. 2C / 38 set / clear the HUD-off state in stream order. While `Ui::Intro` is active the stream is held.
* **A6 (`Killed`)** is held while the view's move-wait is set (an approach / return script has started and no script has ended on op 0x3E since).
* **A kill by an engaged PC's weapon hit** (scene 60, a weak hit through the menu kills unit 3; refreshes from the attack start, when the HP is already 0): target reaction start PS1 +30 / OpenCross +29, attack script end PS1 +66, reaction (KO fade) end PS1 +128 / OpenCross +125, the turn reopens and the dying unit's 65 starts the return hop PS1 +130 / OpenCross +125 (OpenCross: the action's settle waits for the reaction script, then the engaged PC with no living target goes home), hop end PS1 +164 / OpenCross +159, A6 PS1 +167 / OpenCross +159. `ocbattleview --weapon` is not this path: it attacks without engaging and starts the return when the attack script ends.
* **Barriers 66 / 65 / 2C** wait while the view's PC is engaged (standing at the target); any queued group of another actor first sends the engaged PC home (`BattleView::tick_action`, `begin_disengage`).
* **Right-pick timeline (evt409 slot 4 tail):** `queue_attack 1 0x17B self (var 0x50 = the PC)`, `A4`, `2C`, `set_result 0`, `ghost_self`, `mark_defeated`. In retail the "presents" pack plays through the executor (the boxer's own model walks to the PC with the chest and opens it); A4 blocks until it ends, then 83, the ghost op and 8C run. `BattleView::ghosted()` therefore treats a `Ghost` set event still queued behind a `Barrier` as not yet executed; it counts once `script_pump` releases it.
* The player's command menu, the end-of-battle test and the results wait for the stream and the window to finish (`script_busy()`), so `set_result 0` in evt409's right-pick path ends the fight only after its attack and cut scene.
* Tests: `test_pac_script_stream` (stream order, vt gap of a 2D, settle reaction of entity 0, op 1F via libc, Ghost after the A4 barrier).
