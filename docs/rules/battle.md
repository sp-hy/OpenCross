# Battle rules

The lifecycle and turn rules of a fight: how a battle is built, what a command costs, who acts next, how statuses and the field ring change, when the fight ends and what it leaves behind. This document is the behaviour of `battle::Battle` and of the host code around it (field hand-off, session write-back, results screen). The formulas themselves (damage, hit chance, heal, status roll, stamina, colour multiplier values) are in `formats/battle_data.md` section 10, growth in its section 3, drop rows in its section 7 and the element modes in its section 5.2; the class API and the PAC VM are in `architecture.md` section 5; the script ops are in `formats/battlescript.md`. They are referenced here, not repeated. Unit numbers: 0 to 2 are the party slots, 3 to 8 the enemy slots. Colour indices: 0 green, 1 white, 2 black, 3 red, 4 yellow, 5 blue, 6 none (in the field ring 6 is the all-colour slot and -1 an empty one).

## 1. Battle start

### 1.1 From the field to `Battle::start`

* `make_field_battle_args` resolves the scene with `battle::encounter_scene`: an id of 0x200 or more (field op C9) is the scene `id - 0x200` and draws nothing; otherwise (op A3, a random encounter) one `rnd(0, 2549)` picks the variant from the 8 bands of `battle_data.md` section 6 (variant 0 half the time, each next one half as often, variant 7 10 in 2550) and the scene is `symtbl[id * 8 + variant]`. The roll uses a generator of its own, seeded from the battle seed (`OC_BATTLE_SEED`, set per fight by `[Debug] Seed`), so a seeded run repeats its enemy groups.
* Battle id and scene: only two tests read the field's battle id: the Relief Charm below (0x3DD) and the boss intro (an id above 0x1FF, `StartOptions::battle_id`). Every later test of the fight (scene 0x17F countdowns, 0x1D7 / 0x1D8 / 0x1DD music, results screen and New Game+ HP, mode 49's 0x1D8, the event-text table) compares the scene number.
* The party is the field party (`world.party_chars()`). Relief Charm: unless the battle id is 0x3DD, when the save's relief member is set and not already in the party, every party slot holding character 0 (Serge) takes the relief member instead.
* `battle_setup` adds: the New Game+ flag, the growth state (star from the save, progress kept by the host for the session, counter from the save) and the bench (every recruited character with non-zero max HP outside the party).
* `Battle::start` fails (returns false, the host reports the fight as fled) for a scene id outside the table, a party of 0 or more than 3, or a scene that yields no enemy.

### 1.2 Party

* Party slot i (the field party order) takes unit i. Only the placement order follows the character id (the lowest id is placed when the load ends, the others with the party intro); the seat, the HUD panel and the unit slot stay the party slot.
* Per member (`init_pc`): stats from the save record; HP `min(saved HP, max HP)`; countdown 8 and AP 8; stamina 70; statuses `lo = saved persistent bits (mask 0x39) | gear permanent statuses`, `hi` = gear permanent statuses; status skip masks = armour mask OR defence-accessory masks (`Equip::resist_*`); gender from record word +0x14; stamina rate = record rate + gear stamina modifier, floor 0.
* Gear HP% (gear flag 0x01, once per accessory): `maxHP = maxHP x (pct + 100) / 100`, plus 1 when that changed nothing, cap 999. Current HP is then the new max HP when the saved HP was at the old max, else the saved HP (never above max). HP 0 starts KO'd.
* Gear flags are the OR of `flags & 0xF` over weapon, armour and accessories: 0x01 HP%, 0x02 gold x125/100, 0x04 deals x125/100, 0x08 takes x75/100. Accessory flags 0x10 start gauge (`power`), 0x20 offence (adds weapon power, accuracy and MAG), 0x40 defence (adds armour term, MDEF term, evade, status and guard masks), 0x80 stamina rate (`mag`). When no gear table is loaded, or neither the weapon nor the armour id resolves, the flat defaults of `Equip` apply (weapon 16, accuracy 60, variance 20, armour 4, armour variance 20, MDEF gear 4) and the accessories add nothing; their ids are still kept (`Equip::acc`), so the Dopplegang accessory 0xFC and the special-status accessories 0xF9 to 0xFB keep working.
* Pip: a kind byte 0x2B to 0x30 rewrites the cells holding element 0x11F / 0x121 / 0x123 to the form's first tech and 0x120 / 0x122 / 0x124 to its second (section 9), and sets the colour his 18 techs take to his innate colour. Any member whose grid holds element 0x7E (Chrono Cross) turns the colour-history row on for the HUD (`colour_row_shown`).
* Starting gauge: `-1 + gear start-gauge`, clamped to `[-1, gauge_max]`. `gauge_max` is the deepest grid row (cell / 10) that holds an unlocked cell with an element; -1 when there is none. Usable cells: unlocked, element id non-zero and inside the element table.
* The relief member is only a different character in the slot; the rules see no difference.

### 1.3 Enemies

Scene record (28 bytes, `battle_data.md` section 6): groups 0 to 2 in order; a group is skipped when its enemy id is 0xFF, its count is 0 or the id is outside `monster.bin`; each copy takes the next free enemy unit (up to 6). A group with count above 1 numbers its copies 1, 2, 3 ... (name suffix A, B, C ...); a group of one has no suffix. The scene's boss bit (byte 0x1A bit 7) makes it a star-up fight; the low 7 bits are the stage background; byte 0x1B seeds the all-target cast mask (section 3.3).

Per enemy (`init_enemy`, `load_monster_record`), in slot order:

| Source | Becomes |
|---|---|
| record +0xA8 / +0xAC | permanent status words `perm_lo` (OR 0x400 at spawn) / `perm_hi`; `hi` also carries the monster type bits 0x00200000 to 0x10000000 that weapon slayer masks and the element exploit test read |
| record +0x04 / +0x08 | weakness masks (the status chance of those bits is doubled) |
| record +0x0C / +0x10 | status skip masks, OR the masks of the record gear (armour and defence accessories) |
| ATK DEF HIT EVD MAG MDEF, medium and heavy hit bytes, innate colour byte, duration, stamina rate, grid | the same fields of the unit; usable cells = every cell with a valid element id |
| record gear | `Equip` (weapon, armour, accessories) as for a PC; flat weapon power 22 when no gear table is loaded |
| gold, drop row, drop base, model | `gold`, `drop_row`, `drop_base`, `kind` |

* Max HP: `min(record, 32767)`, then the New Game+ and gender scaling of `battle_data.md` section 3 (a record gender of 0 costs one RNG draw, `rnd(1, 2)`), final cap 32767. HP starts full.
* Action period: `ap = record AP; ap += rnd(0, ap >> 2)`. First countdown: `rnd(lo, ap)` with `lo = 1` for scene ids above 0x17F, else 0 (after the AP bonus, so the draw uses the larger value).
* RNG order per enemy: gender (if rolled), AP bonus, countdown.
* Enemies start with stamina 70 and gauge -1. Their `gauge_max` is the deepest grid row holding an element (`& 0x7FFF` non-zero) in a cell without the lock bit 0x4000, -1 when there is none; every stock monster cell carries the lock bit, so an enemy gauge stays at -1 (landed hits are clamped to it).
* Field ring: the three `btbgdat` colours of the stage's row: the battlefield operand when it names a row of the table, else the scene's background (the same choice the stage loader makes); a colour value of 6 or more leaves its slot empty (-1). Slot 0 is the oldest.

### 1.4 Before the first menu

After the units exist `brain_.begin_battle` runs the scripts' init slot, then `settle()` (section 2.6) runs. Time does not advance while a PC can open a menu: only an enemy whose seeded countdown is 0 acts, one turn each in round-robin order from the first enemy slot, each followed by its reactions. Then the loop stops at the first point where a PC can open a menu.

## 2. The turn system

### 2.1 Clocks

* Each enemy has a countdown (0xFFFF = "is taking its turn now", never decremented). A PC has stamina (0 to 70, can be negative after a cast) and a countdown that matters only for confusion and Pip's form change (section 2.6).
* "Steps" are the unit of time: a weapon hit of strength s (0 weak, 1 medium, 2 heavy) is `s + 1` steps; a cast, a defend and an enemy cast are 1 step.

### 2.2 Commands (`apply`)

`apply` runs `validate` (section 3) and does nothing when it fails. Then, for the acting PC: its guard flag is cleared and every PC's "defended" flag is cleared. Per kind:

| Command | Cost and effect | After each unit of the command |
|---|---|---|
| Attack, 1 to 4 chained strengths (`kMaxChain` = 4) | each hit costs stamina (`battle_data.md` section 10; weak 10 / 12, medium 20 / 25, heavy 30 / 37 when fatigued); the whole chain must fit the stamina at the time of the command, strengths must be 0 to 2; the cost is paid per hit, also on a miss | other PCs recover (2.3), enemies count down `s + 1`, `post_action(s + 1, s + 1)`, `check_end`; the chain stops when the battle ends or the actor is down |
| Element (a grid cell) | stamina `- 70`; gauge `- (row + 1)`, floor -1; the cell is marked used (section 4). A double / triple tech (section 4.1): every caster pays 70 and gives up its cell; caster entry k's gauge becomes party slot k's gauge `- (its own cell row + 1)`, clamped to `[-1, slot k's gauge_max]` | other PCs recover 1 step (not the casters, not the target if it is a PC), enemies count down 1, `post_action(7, 1)` |
| Defend | stamina `+ 10`, cap 70; guard on; "defended" flag on | other PCs recover 1 step, enemies count down 1, `post_action(1, 1)` |
| Run | the fight ends at once as Fled (2.7, 7): no roll, no cost, no check of the scene | none |

Notes:

* Automatic target. An Attack uses the given target only when it is a living, targetable enemy; otherwise (target -1, a party unit, a dead or off-stage enemy) it takes the living, targetable enemy with the lowest current HP (first on ties), and re-picks the same way inside a chain when the target dies. An Element is re-targeted only when its target is -1 or an empty slot: a self-only element picks the caster, a party-only element the living PC with the lowest HP fraction (compared by cross-multiplication, the caster first), any other the enemy as for an Attack.
* Chain counter: the PC's hit chance bonus (`battle_data.md` section 10) survives from one Attack command to the next only against the same target; a miss, a retarget, any other command, a confused turn or an enemy turn resets it.
* Guard: from a Defend until the PC's next applied command or its KO. It halves weapon damage taken (`battle_data.md` section 10; element damage is not halved).
* "Defended" flag: closes that PC's own command menu. It is cleared when any PC command is applied, by every time advance (2.5) and on KO.
* After the command: reactions (slot-4 and kill scripts, section 7), then `settle()` when the fight is still on.

### 2.3 Stamina recovery of the others (`step_after_pc_action`)

For every living PC except the actor, the other casters of a double / triple tech and (for a cast) a PC target: stamina rises by the recovery step of `battle_data.md` section 10 (modifier 0), capped at 70. The number of steps is the hit's steps for a weapon hit and 1 for a cast or defend. The same call lowers the countdown of every living enemy that is not 0xFFFF by that number of steps, floor 0.

### 2.4 Upkeep per action (`post_action`)

Run once per hit of a chain, once per cast or defend, and once per enemy turn, scripted counter-attack plan or death cast (a plan: once for the whole plan, with the values of its last executed entry). `actions` is counted. For every living unit, in unit order:

1. Poison (`lo` 0x1): HP `- poisonStep`. At 0 or below a PC is knocked out (`kill`); an enemy stays at 1.
2. Regen (`hi` 0x80000), if still alive: HP `+ poisonStep`, cap max HP.
3. Every timed status timer `- timerSteps`; at 0 or below the bit is cleared (and, for hi bits 6 to 11, the matching percent modifier returns to 100) and the timer removed.

`poisonStep` / `timerSteps`: weapon hit `s + 1` / `s + 1`; cast 7 / 1; defend 1 / 1; enemy weapon `s + 1` / `s + 1`; enemy cast 7 / 1; a wait or a confused turn 1 / 1. A status gets its timer when it lands (section 6) and a repeated landing does not refresh it.

### 2.5 Time advance (`time_advance`)

Runs only when nobody can act (2.6).

1. `step` = the smallest positive countdown among living, non-hidden enemies that are not 0xFFFF; 255 when there is none.
2. Every living unit: countdown `- step`, floor 0 (not for 0xFFFF).
3. Every living PC: `stamina = min(70, stamina + step x rate)` (no flu factor); the "defended" flag is cleared.

The cheat state is applied first (section 10).

### 2.6 The scheduler (`settle`)

Loop (at most 20000 iterations; exceeding it ends the fight as Stalled):

1. `check_end` (section 7).
2. Each living PC whose countdown is 0 (all three are checked in slot order) takes a scripted turn: its countdown is set to its AP (8) and it acts only if Pip's form element is pending (the unit's script slot 2 casts it, other PCs pass) or the PC is confused (a scripted plan if the script has one, else a weak weapon attack on a random living unit other than itself, then `post_action(1, 1)`). Then the reactions run and the loop restarts.
3. Otherwise the next enemy with countdown 0, scanning the six enemy slots round-robin from the one after the last that acted, takes its turn: countdown 0xFFFF, then either the scripted plan (the script's slot 2; several queued attacks are one turn) or `EnemyBrain::choose`. A turn sets `countdown = ap + rnd(0, ap >> 2)` and runs `post_action`. An enemy element is cast from the grid row `level - 1` (0 when it has no level). A weapon aimed at a dead target retargets to the first living PC; an absent or out-of-range target becomes unit 0. The fallback brain (`RandomBrain`) picks a random living PC, then with 40 percent a random usable mode-0 element of its grid, else a weapon strength 0 / 1 / 2 with 60 / 30 / 10 percent. Then reactions run and the loop restarts.
4. Otherwise, if any PC `can_act`, the loop returns: the host picks which PC. While a form change is pending nobody can act, so time runs on until a PC's countdown reaches 0 and step 2 casts the form element.
5. Otherwise `time_advance` and repeat.

`can_open_menu`: a PC, alive, stamina 10 or more, none of the `lo` bits 0x20304080 (KO 0x80, asleep 0x4000, confused 0x100000, 0x200000, defeated / frozen 0x20000000), and not defended. `can_act` also requires the fight to be on and no pending form change.

### 2.7 Flee

Only `Command::Run` ends a fight as Fled; `validate` refuses it only when the battle is over or the actor cannot act (section 3.2). A scripted code 2 (section 7) ends it the same way. A Run while pilfered loot is pending (`Rewards::stolen` not empty) drops it: the list is cleared and the Run event carries system message row 5 ('Dropped the stolen goods!'). The view then holds the escape 80 ticks longer and opens that window at (32, 144) 40 ticks into the hold; the battle loop ends with the hold, so the window closes then.

## 3. Targeting and validation

### 3.1 Target codes

The element record byte +4 (`target_spec`):

| Bit | Meaning |
|---|---|
| 0x40 | the party (units 0 to 2) can be aimed at |
| 0x80 | the enemies (units 3 to 8) can be aimed at |
| neither 0x40 nor 0x80 | enemies only |
| 0x10 | the cast covers the whole range at once (3.3) |
| 0x04 | self only: the cursor cannot leave the caster |
| 0x01 | only knocked-out units qualify (Revive) |

### 3.2 `validate`

Refused, with the reason string, when: the battle is over; the actor is not a party slot or `can_act` is false; an Attack chain is empty, longer than 4, has a strength outside 0 to 2 or does not fit the stamina; an Element cell is not usable (section 4.3); the target number is 9 or more; the target is an enemy that `untargetable` reports (hidden / off-stage units, script ops 34, 35 and 5B) and the command is an Attack or Element. For an Element with an explicit target, additionally: the target slot is empty; the target's side is not allowed by the target bits; a self-only element aimed at another unit; a KO-only element aimed at a unit that is not KO'd, or any other element aimed at a unit that is not alive.

### 3.3 All-target casts

`cast_element` (record byte +4 bit 0x10) hits every qualifying unit of the target's side, or all nine units when both side bits are set (colour fields, Chrono Cross, the seal-all elements). A unit qualifies when it is present and alive (KO'd for a KO-only element or mode 4), and, for an enemy slot, bit `unit` of `cast_mask` is set; the party side is never masked. `cast_mask` starts as `~(scene byte 0x1B << 3) & 0x1FF`; script ops 34, 35 and 5B (argument 0) set a unit's bit. The cast stops when the fight ends. If nobody qualified, the cast is made once on the chosen target. An enemy's scripted element (op 67) aimed at several units is likewise one cast: each unit of its target mask that is alive (KO'd for a KO-only element) takes it once. The ring takes the cast colour once, after the last target (section 5).

### 3.4 The "defended" flag

See 2.2. It is a command-menu gate only; it does not affect targeting.

## 4. Element grid and gauge

* The grid is 8 rows x 10 columns, cell = `row x 10 + column`, 80 cells. A cell word's low 14 bits are the element id; the top two bits are kept when a cell is emptied (`& 0xC000`).
* Gauge: starts as in 1.2. A landed weapon hit (the roll succeeded, whatever the damage code) by any unit adds `s + 1` (weak 1, medium 2, heavy 3), clamped to `[-1, gauge_max]`. A cast subtracts `row + 1`, floor -1 (not with the free-elements cheat).
* An element is usable (`element_usable`) when all hold: the cell is not colour-sealed; the PC can open a menu and has stamina above 0; the cell is usable (unlocked, valid id) and not yet used this battle; `gauge >= row`.
* Seals: the PC's `lo` bit `0x800 << (4 x colour)` seals every cell whose element has that single colour (green 0x800, white 0x8000, black 0x80000, red 0x800000, yellow 0x8000000, blue 0x80000000); rainbow and colourless elements are never sealed. Sealed cells stay on the menu and refuse the confirm.
* Used cells stay used for the rest of the battle. Recharge: mode 12 (row 2 or higher) clears the used bit of the target's cells 0 to `10 x row - 1`; mode 3 (and mode 14 on a party slot) clears each used cell of the target with chance `variance` percent.
* Slot term: the per-mille factor of `battle_data.md` section 10 for the cell's row and the element's level; it scales damage and healing.
* Double / triple techs (`combo_at`, `cell_element`): see 4.1.
* Consumable stacks (element record flag byte bit 0x10): the stack size is the save's count (at least 1). Each cast decrements it; the cell is marked used and emptied (`usable` false) only when the last copy goes; with more than one copy the cell stays usable. The count is written back to the party record. The free-elements cheat bypasses the stack, the used bit and the gauge.
* Element traps (mode 5): the caster's side stores the element id of record word +0xE (a different id replaces it). Before any cast the other side's stored id is compared: on a match the cast fizzles (a miss event, no effect, no ring change), the trap is cleared, and the cast still costs its stamina, gauge and used bit. A fizzled PC cast also destroys the cell (`& 0xC000` in the battle grid and the party record, `Rewards::lost_cells`, written to the save by the host whatever the outcome). A fizzled enemy cast adds the element id to `Rewards::trapped`, which the host adds to the element stock whatever the outcome. `trap_hud()` reports the HUD block.

### 4.1 Double and triple techs

A tech cell of the PC whose menu is open turns into a double or triple tech while its partners can join; the menu then shows that tech (name, colour, description, target ring, the minimap plate) in the cell's place, and the confirm casts it. The substitution is worked out from the current state each time; only the seal flags and the menu tables described below are stored.

* Partner test, for party slot P and partner tech T: the first open cell of P whose word is exactly T (no lock or flag bits); P qualifies when that cell is unused and its seal flag is clear (the flag below, not the live status word), `gauge >= its row`, stamina is 10 or more and none of the `lo` bits 0x203000C0 is set (KO 0x80, 0x40, confused 0x100000, 0x200000, defeated / frozen 0x20000000). Asleep does not keep a partner out.
* The other two party slots are tried in slot order (the menu PC 0: slots 1, 2; PC 1: 0, 2; PC 2: 0, 1). Double (record +0x22 == 0, +0x20 != 0): the first of them that qualifies for +0x20 makes it the +0x26 tech. Triple (+0x22 != 0): each of the two slots must qualify for +0x22 or +0x24 (when both qualify the +0x24 cell is the one given up); the result is +0x28.
* The cell's own state is not tested beyond the usual element rules (section 4): a used tech still shows the double tech, greyed. The menu PC's own cells are sealed by the colour of the tech they show (the double tech's colour).
* The cast: the menu PC is the actor (its stats for every formula without a participant list); the casters are the record's character bytes +0x14..+0x16 matched against the party (`ActionEvent::group`, entry k with clip variant k, so X-Strike lists Serge first whoever chose it); every caster's cell is marked used and every caster pays 70 stamina (it may go negative). Each caster entry k then takes party slot k's gauge minus its own cell's `row + 1`, clamped to `[-1, slot k's gauge_max]`, entries in order; a caster that does not sit in slot k therefore takes another PC's gauge (Serge and Glenn's X-Strike with Kid in slot 1: Glenn's gauge becomes Kid's `- 3`).

* Seal flags and the menu build. Each PC cell carries a seal flag, and only the flag decides whether a partner joins. It is judged by the element the cell shows (`seal_flag`, `combo_word`): the double / triple tech where the last menu build put one into that cell, else the cell's own element; rainbow and empty cells keep their flag, and a cell is sealed when its colour's bit is in the PC's `lo`. The flag is judged again (a) for every PC at battle start, (b) for each PC that is the target of an action (any cast or weapon hit, once the action has run), (c) for the menu PC's own cells, all cleared first, when its element menu is built. It is not judged when a seal wears off or when only the partner's status changes, so a partner keeps a stale flag until one of the three happens.
* The element menu build (`open_element_menu`, run when the Element row of a PC's command menu or the HIT% menu is confirmed, and by an Element command first): every PC's menu table starts from its own cells; each of the menu PC's cells that offers a double / triple tech takes its result word, and so does the cell each partner gives up. The partners' tables keep those words (and so their flags are judged by the result's colour) until the next build. Consequences: a seal that lands on a partner after a menu build that showed the double tech is judged by the double tech's colour, so a seal of the partner's own colour leaves the flag clear and the partner still joins; a seal of the result's colour sets it. A partner's own menu build recomputes its flags, clearing a flag whose seal has run out.
* Confused PCs act by script or with a weak weapon attack, and the game's auto-battle only chooses attacks; neither ever picks a grid cell, so no combo substitution is involved. `auto_command` (tools and tests) is an OpenCross driver, not game behaviour: it casts through `apply`, which builds the menu first.

## 5. Field ring and colour

* The ring has three slots, oldest first. `ring_push` drops slot 0, shifts, and puts the new colour in slot 2.
* Colour multiplier (`colour_multiplier`): inputs are the attacker's innate colour, the attack colour (a weapon attacks in the colour of the attacker's colour status, hi bits 0..5 set by the Turn elements, else in its innate colour; every element command, weapon techs included, in the record's colour), the ring, and the target's colour. The values are in `battle_data.md` section 10. A ring slot of 6 (all colours, the Chrono Cross) and an empty slot count as no colour. Mode 6's field state (`battle_data.md` section 5.2) scales the multiplier of every element cast; no timer is modelled, so the state lasts for the rest of the battle once set (it is only set while it is 0).
* Timing: during a cast the ring is not changed, so every target of the cast sees the ring as it stood before. When the cast has finished (once, after the last target of an all-target cast) the ring takes the new state:
  * a repaint replaces all three slots: mode 6 with a colour in record word +0xE `& 0xFC` (all three become it), mode 37 (slots from record bytes +0xA, +0xC, +0xE, each byte `<< 2` read as a colour mask), mode 47 (a random colour that some PC lacks);
  * otherwise the cast's colour is pushed: the effective element colour (below) `& 0xFC` as a single bit gives 0 to 5, all six bits (0xFC) gives 6, anything else (a colourless element) gives -1 (an empty slot, which still shifts the oldest colour out). A missed or no-damage cast pushes too.
* Sources: element casts of PCs and enemies, including scripted casts. Weapon attacks never push.
* No-shift cases: an element whose record byte 0 has bit 0x80 (death casts, form changes), an element trap (mode 5), a fizzled cast.
* Effective element colour (`element_colour`): the record byte +5, except Pip's 18 techs, which take Pip's innate colour mask (`& 0xFC`) with the record's low 2 bits, and Vary Innate (element 336), which keeps its last caster's innate mask from its first cast on.
* Innate absorb: the colour mask of the innate byte is bits 2 to 7 (`& 0xFC`); innate bit 1 makes the unit absorb elements of its own colour. The armour's colour byte works the same way: bits 2 to 7 are a colour mask and bit 1 set means elements of that colour are absorbed. A target absorbs an element when it has the absorb hi status or `(absorbMask & element colour byte) != 0`; it is immune with the immune hi status (`battle_data.md` section 10 for the result codes).
* Armour colour boost (factor in `battle_data.md` section 10): with bit 1 of the armour colour byte set, elements of one colour are raised, chosen by the byte's colour bit:

| Armour colour bit | Element colour raised |
|---|---|
| 0x04 green | black |
| 0x08 white | white |
| 0x10 black | green |
| 0x20 red, 0x40 yellow | none |
| 0x80 blue | green |

* Colour shields (`hi` bits 0 to 5, one per colour; factors in `battle_data.md` section 10): when several bits are set the highest colour index is the shield; it compares against the element's colour, opposite or same.
* The colour history (`seq_colour`) is separate from the ring: 0 to 5 for a coloured element, 0 for an element without a colour mask, 6 for the all-colour element, -1 for none (record byte 0 bit 0x80). `ActionEvent` carries the first seven entries for the HUD.

## 6. Status application

### 6.1 `apply_status_bits(target, lo, hi, chance, colourBit)`

1. A target with the immune hi status (`kHiImmune`) takes nothing.
2. If `hi` has any of bits 29 to 31: when one of the target's accessories is 0xF9 to 0xFB ("special"), the element's colour (`colourBit >> 2`) joins `hi` as shield bits, and a permanent bit of 29 to 31 that the new mask repeats first makes the target's permanent bits 0 to 5 and 29 to 31 leave both its permanent and live words; without such an accessory, `hi` is cut to bits 6 to 28.
3. If `hi` (with the colour joined in step 2) has any shield bit (0 to 5), the target's inflicted shields are replaced: its `hi` bits 0 to 5 return to the permanent ones.
4. For set 0 (`lo`) then set 1 (`hi`), bit 0 to 31: skipped when the bit is not in the mask or the target is not alive; skipped when the target's skip mask has the bit; otherwise the bit lands without a roll if "special" and it is a hi bit below 6 or above 28, else when the status roll (`battle_data.md` section 10; the weakness mask is the doubling bit) lands. A roll that passes counts as landed even when the bit was already set; a new bit is set and, when it is timed, its timer starts. Each bit rolls separately.

Element statuses use the record's `lo` word, `hi` word `& 0xE007FFFF`, chance byte +0xA and the element colour (`apply_status`).

### 6.2 Skip masks

PC: the OR of the armour's and the defence accessories' (flag 0x40) masks. Enemy: record +0x0C / +0x10 OR the masks of the gear on its record. Weakness masks exist for enemies only. A mask bit means the status never lands.

### 6.3 Timed statuses

A bit is timed when it is in `kTimedLo` (0xBBBFFB00) or `kTimedHi` (0xE003FFFF). Its timer starts at the status duration of `battle_data.md` section 10 (status index = bit, plus 32 for hi), from the unit's duration byte with no modifier. Timers count down in `post_action` (2.4). A weapon hit wakes a sleeping target (clears `lo` 0x4000 and its timer); element mode 32 also clears confusion (and its timer).

### 6.4 `force_status_bits`

Used by the colour curse (mode 36): `lo` bits only, no roll and no weakness doubling; skipped when the target is not alive, already has the bit or has it in its skip mask; timed bits get their timer.

### 6.5 HP application order

1. Each element mode orders its parts: mode 0 rolls the status first, a landed status cancels the damage when record word +0xE bit 15 is set, and with bit 14 a target with hi 0x2000000 is knocked out outright; modes 32 and 33 damage first, then roll the status; mode 15 rolls the status first and deals damage only when nothing landed.
2. `damage_unit`: a KO'd target is untouched (HP stays 0, no healing). Code 3 heals up to max HP. Code 2 or a negative value does nothing. Otherwise HP `- value`; a target with `lo` 0x40000 also loses 10 stamina; HP at or below 0 becomes 1 for an invincible unit and for a self-inflicted hit, else `kill`.
3. `kill`: HP 0, KO bit (0x80), guard and "defended" cleared, and for an enemy its kill script starts (section 7).
4. A unit is alive when it is present and its `lo` has neither KO (0x80) nor defeated / frozen (0x20000000). A status that sets one of those bits removes the unit from play without touching its HP.

### 6.6 Status plates on the HUD

The menu shows a unit's statuses as a column of name plates (`status_plates.h`, `BattleView::build_status_plates`): one plate per `lo` bit of mask 0x22166139, in bit order, each a 3 x 6 colour triangle (bottom row of `battle_34.png`) at (x, y + 1) and the status name (cmnmes, style 2, colour 0x757575) at (x + 4, y). Plate n sits at y = 50 + 12 (n % 9), column n / 9.

| `lo` bit | Name | Triangle |
|---|---|---|
| 0x00000001 | Poison | green |
| 0x00000008 | Burns | red |
| 0x00000010 | Sprain | yellow |
| 0x00000020 | Flu | blue |
| 0x00000100 | Afraid | green |
| 0x00002000 | Fatigued | white |
| 0x00004000 | Asleep | white |
| 0x00020000 | Darkness | black |
| 0x00040000 | Cursed | black |
| 0x00100000 | Confused | red |
| 0x02000000 | Dizzy | yellow |
| 0x20000000 | Frozen | blue |

* Top menu: the active PC's plates on the right, columns x 248 and 210 (the original's 268 and 230, moved 20 px left for the wider translated names).
* Target selection: the cursor unit's plates on the left, columns x 16 and 54. An element's target ring shows them only while it holds one unit; under the Dopplegang creature list they are not shown. The chain menu and the element list show none.
* DYNAMIC target camera: the target cursor is not drawn unless the target mode is the whole range (an element whose record byte 4 has bit 0x10; mode 1 = a single-target Attack or element), and then element targeting shows no plates either; Attack targeting still shows them. The NORMAL camera draws both.
* Each plate is one frame of the battle menu sprite sheet: the 3 x 6 triangle part at (0, +1) (cells u 212 / 215 / 218 / 221 / 224 / 227 = red / green / blue / yellow / white / black) and the name glyphs from (+4, 0); frame = 7 + the status's rank among the bits of 0x33377339. Captured on the PS1 (all twelve statuses in both columns): every triangle colour, the y rows and the left columns match; the right column is drawn at 268 there.

## 7. Battle end

### 7.1 `check_end`

Run before every scheduler step, after each hit of a chain, after each plan entry and after reactions. In this order:

1. The cheat state is applied (section 10). Nothing happens if the outcome is already set.
2. Script op 81 can switch the whole test off.
3. Forced code (script op 83, non-negative): 0 ends as Victory (rewards are paid), 1 as Wiped, 2 as Fled, any other as Scripted with that code.
4. Wipe: no PC alive (a frozen PC counts as down) ends as Wiped. This test comes before the victory test, so a mutual KO is a defeat.
5. Victory: for each enemy slot that is present, the fight continues if the unit is alive or still owes a scripted death (`death_pending`, 7.2), and (it owes a death, or it is not hidden). Hidden (off-stage) living enemies therefore do not block the win. When no enemy blocks, the fight ends as Victory.

`finish` sets the outcome once. The result code is the forced code, else the `Outcome` value: Victory 0, Wiped 1, Fled 2, Scripted 3 and up (the script's code), Stalled 9. `Outcome::Stalled` is set only by the scheduler's 20000-turn guard.

### 7.2 Scripted deaths

A killed enemy runs its kill script (slot 3) once, when its HP reaches 0 by damage or by a script write. Death casts it queues (op 67) are executed by the dying unit after the killing action, each op 67 as its own action (its targets share one: the Time Devourer's 341, then 526). A result the kill script forces (op 83) takes effect only once all its casts have run (the script sets it after its end-turn op A4). The unit blocks the victory test (`death_pending`) until the cast has run; the presentation additionally holds the unit's death while the script flag of op 5C is set (cleared by the script's A6). After each action the target list is walked and each living target runs its "attacked" script (slot 4); the attacks it queues are executed like any action (counter-attacks), looping up to 64 rounds, with death casts run before, between and after the rounds. A death-cast action is an ordinary `post_action` / `check_end` action.

### 7.3 What `finish` records

The party records: `hp = min(unit HP, record max HP)`, raised to 1 if 0; persistent status = `lo & 0x39` (bit 0 poison, bits 3 and 4 the burn and sprain statuses, bit 5 flu). Rewards (section 8) are computed first, for Victory only.

### 7.4 Host mapping (`finish_field_battle`)

| Outcome | Code to the field |
|---|---|
| Victory | 0 |
| Wiped | 1 |
| Scripted | the script's code |
| Fled, Stalled | 2 |

* The battle counter that RDE keeps goes up by one (cap 0xFFFF) for code 0 or 3.
* `apply_battle_result` runs for every outcome (section 8.3), the Dopplegang mask goes back to the world, the results screen's HP is copied (8.5), then `world.end_battle(code)`.
* Losable fights: the encounter's flag 2 ("losable"). A wipe (code 1) without it loads the game-over room (map 9); the title is the fallback if that load fails. With flag 2 the field simply continues with result 1 (`architecture.md` sections 5.6 and 12.3).
* A battle that cannot start counts as fled (code 2).

## 8. Rewards and after-battle state

### 8.1 Gold and drops (`compute_rewards`, Victory only)

* Only enemies in the defeated mask pay: those that are not alive, plus those a script marked defeated (op 8C). A living, unmarked enemy (a forced win) pays nothing.
* Gold: the sum of the paying enemies' record gold; `x 125 / 100` (truncating) when the start option says so or any PC in the fight has gear flag 0x02 (Profiteer Purse). The host adds it to the save's gold, cap 9,999,999.
* Drops: no roll at all after script op 7C (the gold is still paid). Per paying enemy: `r = rnd(0, 99)`; `r < dropBase / 20` gives drop row slot 1 (rare), else `r < dropBase / 2` slot 0 (common), else nothing (`battle_data.md` section 7 for the rows; the row is the record's drop row `& 0x3FFF`, as for the pilfer). The word's bit 15 makes it an element (`id & 0x7FFF`, `Rewards::elements`, added to the stock with that id), otherwise a non-zero item id (`Rewards::items`). The roll is consumed even when the row is outside the table.
* The pilfer (mode 30) loot is separate: `Rewards::stolen`, added to the stock on a victory only (a flight or a defeat loses it).

### 8.2 Growth (Victory, star below 99 and inside the star table)

Row `{battles, pendingTenths, step}` of the star table (`battle_data.md` sections 1 and 3 for the table and the formulas); `cap = (short)(step x battles)`.

* Normal fight: `progress = min(progress + step, cap)` (the instant-growth cheat sets it to `cap`). For every party member that is alive: for each of the 7 stats, `gain = minor_growth(snap, pending, stat, pendingTenths, progress, cap)`, limited to the room below the Lv 99 value; the stat is raised and the gain recorded in `Rewards::gains`. Max HP is capped at 999 (the core raises the stat, not the current HP). Knocked-out members get nothing.
* Boss fight (scene boss bit): `star_up`. Every living party member and every bench member (the bench regardless of state) gains its whole pending delta, limited to the Lv 99 room. Then `progress = 0`, `star + 1`, `counter + 1`. Then everyone (the whole party including the knocked-out, and the bench, merged and rolled in character id order with the battle RNG) takes `snap = stat` and new pending deltas (`next_pending_hp`, `next_pending_stat`), and their grid unlocks are recomputed: the populated cells of rows 0 and 1 plus the `el_mask` unlocks up to the new star.

### 8.3 What the host writes (`apply_battle_result`)

For every outcome, per party member: the 7 stats (max HP clamped 1 to 999, the others 0 to 127), current HP `clamp(member HP, 1, max HP)`, persistent status `lo & 0x39`, the growth snapshot and pending words, Pip's form change (section 9), and the consumable stacks (a stack that ran out empties its grid cell, `& 0xC000`). Then the trap results (section 4): trapped elements and lost cells are written. On a victory only, the stolen loot (8.1): stolen words go to the element stock (bit 15, `id & 0xFF`, like a trapped element) or the item stock. The growth progress goes back to the host's session counter. Stamina is not carried; every battle starts at 70.

For Victory only: gold, dropped items and elements (an element drop uses `id & 0x7F`), and after a boss fight the star byte (cap 99), the counter byte (cap 255), the party's new grid unlocks, and the bench's new stats, growth words and grid unlocks.

### 8.4 Results screen (`BattleResults`)

* It opens for Victory, Fled or Scripted outcomes unless the result code is 1, 3 or 4 or the scene is 0x1DD. A flight or a scripted code of 5 and up shows it without rewards; the scene 0x1D8 plays muted unless the code is 2.
* Pages in order: the star-up page (boss fights only), then one "PowerUp" page per party member that has any stat gain (a blank PowerUp page when nobody gained; the first member's gains are applied when the screen opens; showing a member adds its HP gain to its displayed HP, cap 999), the heal menu when offered, the "Healed" page listing the healers used (after a heal choice), "Spoils" (items and elements), then the exit.
* Exit: the last confirm plays the exit sound 0x41 only (no sound 8); the screen fades to black over 30 ticks while the panels slide out and stays black. The victory music plays on at full volume until the field returns, 43 ticks after the confirm: the field restores its song (which cuts the victory song, no fade); the field's fade-in from black starts 8 ticks later, the screen staying black until then (`BattleResults::kFieldReturn`, `kFieldFadeDelay`).
* Victory music (`BattleView::tick_finish`): the battle theme plays on after the win; 43 ticks after it the theme fades to silence over 16 ticks, and 20 ticks after the fade began the victory song (0x16, 0x17 after a boss) starts. Battle flag 0x10 and the scenes 0x1D7 / 0x1D8 / 0x1DD play no victory song.
* Heal menu rule: offered when at least one present, living member has HP below max HP and the healer list is not empty. Healers are the healing elements (menu-side byte +B bit 0x40, byte +A bit 0x40 clear = not a consumable) found in the open cells (unlocked, unused, id below 0x80) of rows 0 to the gauge of every living member, merged by id (the count is the number of cells), ordered by tier (menu-side byte +A & 3, tier 2 first, tier 3 excluded; equal tiers in discovery order, each member's cells scanned from the highest down), single-target before all-party (byte +A bit 0x80) when fewer than two members are hurt and after otherwise. Tier amounts: 80, 160, 320 (the fourth value, 16, is never used). The menu has three entries: heal, heal with stock (identical, the stock list is empty), do not heal (and cancel does the same). Healing applies each healer in order, up to its count of uses, until nobody is hurt: a single-target element heals the first hurt living member, an all-party element every hurt one, capped at max HP. No cell is marked used; only HP changes.
* Confirm debounce is 16 ticks, fade-in 60, settle 7.

### 8.5 `take_final_hp`

After the screen is done it returns, per party slot, the displayed HP `clamp(HP, 0, 999)` (HP after the fight, plus the growth's HP gain, plus the heals) or -1 for an empty slot; it returns false when no results screen ran since the last call. The host writes `clamp(HP, 1, max HP)` over the value `apply_battle_result` stored, so a knocked-out member ends at 1.

## 9. Pip's evolution and Dopplegang

### 9.1 Pip's form change

* Script op A7 reads Pip's kind byte and the six colour-point script variables (green, white, black, red, yellow, blue) and stores the form element 0x80 to 0x85 (or 0) in a script variable; a non-zero result also sets `evo_pending` (`battlescript.md` for the thresholds).
* While `evo_pending` is non-zero no PC gets a command menu; every PC whose countdown reaches 0 takes its script slot 2 turn: Pip casts the pending element on himself, the others pass.
* Mode 45 of that element calls `Battle::evolve(unit, form)` with the record's power as the new kind (0x2B to 0x30 only):
  * the innate colour mask becomes the colour with the most points (the last of equal ones; all zero gives blue), keeping innate bits 0 and 1;
  * the six colour-point script variables are zeroed and `evo_pending` cleared;
  * the unit's kind byte, AP and countdown (8) are set; the grid cells (row 2, 4, 6; column 7) take the form's techs from `kPipTechs`, keeping the cell's top two bits, the third cell only for a second form (kind above 0x2D), in the unit and in the party record; Pip's techs take the new colour;
  * the party record gets the kind, `evolved = true` and the growth column of the form (`kPipGrowthCol`: 3, 3, 3, 4, 5, 6).
* The event is silent (no number), result code 1, and carries the new kind for the presentation. An element with record byte 0 bit 0x80 leaves the ring unchanged (section 5).
* The host (`apply_battle_result`, any outcome): writes the kind into the record's model id (the growth column follows it) and the 2 or 3 tech cells (`& 0x1FF`).

### 9.2 Dopplegang

* The pool is 256 bits in 16 words (`BattleOptions::dopple_mask`, the save's 16 words, script variables 0x60 to 0x7F). Bit `n` of word `w` is pool slot `w x 16 + n`. `dopple_pool` lists the set slots ascending.
* Credit (`dopple_credit`), called by script op 65 on an enemy (the start of its kill script) with the last PC that commanded (script variable 0x50): it sets the enemy's pool slot (monster record byte +0x2B) when that PC is character 26 (Sprigg) or wears accessory 0xFC, and the enemy's record byte +0xA6 bit 0x80 is clear. It reports whether the bit was new.
* `transform(unit, form)` (the unit swap the pick menu requests): `form >= 0` makes the PC take monster record `form`: stats, innate colour, gear, status words (with 0x400400 OR-ed into the permanent `lo`), the monster's grid (its cells lose the lock bit, `& 0x3FFF`) and its name; HP and max HP stay, stamina stays, AP and countdown become 8, gauge -1, and `gauge_max` becomes the deepest row of that grid holding an element (-1 for none); inflicted statuses and timers are gone; the character's unit is backed up the first time. `form < 0` restores the backup with the current HP and max HP, countdown 8 and the current stamina. It fails (returns false) for an unknown form, a non-PC unit, or `form < 0` on a unit that has no form.
* Big Catch (mode 39) draws creatures from the same pool (`battle_data.md` section 5.2).
* The host persists only the mask: `finish_field_battle` writes `dopple_mask()` back to the world after every fight (not after a monster-party fight, 9.3). A creature form is never written to the party record; the PC's HP at the end is its unit HP.

### 9.3 Monster-party scenes (the Grand Slam)

* A scene whose record byte 0x0F has bit 0 set (`battle_data.md` section 6) is a monster-party fight when the host passes picks (`BattleOptions::monster_party`, the Grand Slam picker's three pool slots, `rules/menus.md`); without any pick (dev tools) the party fights as usual.
* Party slot i becomes monster record `pick[i]` (`init_party_monster`; a slot without a pick stays empty) under the slot's character id: the whole record (stats, innate colour, status words with 0x400400 OR-ed into the permanent `lo`, skip masks, grid, gear, name, record gender word). Max HP = the record's (NG+ doubles it outside scene 0x1DD, cap 30000), then max and current HP are capped at 999; AP and the first countdown 8, stamina 70; gauge `-1 + gear start-gauge` clamped to `[-1, gauge_max]`, `gauge_max` the deepest row holding an element in an unlocked cell.
* In such a fight every monster record's grid (the picks' and the enemies') loses the flag bits (0xC000) of the cells in rows 1 to 7 outside column 8, so their elements can be cast; row 0 and column 8 keep theirs.
* The end writes nothing back: no rewards (gold, drops, growth), no party HP or status, no results screen, no Dopplegang pool.

## 10. Cheats as the rules see them

`Battle::Cheats` is pushed by the host every update (`set_cheats`); `set_invincible(party, enemies)` is the separate debug switch. All are off by default.

| Flag | What it bypasses |
|---|---|
| `invincible` (debug party / enemies; for PCs also God mode and Battle boost) | `kill` leaves HP at least 1 (no KO, no kill script); damage never takes HP below 1 |
| `god_mode` | every damaging hit on a PC becomes value 0, code 1 (heals untouched); each time advance and each `check_end` revives KO'd PCs, refills HP, clears the negative `lo` statuses (mask 0xAA9EE939, frozen included, so a frozen PC is freed) and their timers |
| `boost` | everything God mode does, plus gauge to `gauge_max` and stamina 70 on every living PC |
| `max_stamina` | living PCs' stamina 70 at each time advance and `check_end` |
| `free_elements` | a cast does not mark the cell used, does not spend a consumable stack and does not spend gauge; stamina is still paid |
| `always_hit` | the hit chance of a PC weapon attack is 100 (also the chance figure the pilfer reads) |
| `always_loot` | a PC's pilfer skips the chance roll and takes the rare word when it is non-zero; kill drops take row slot 1 when non-zero, else slot 0 (the drop roll is still drawn) |
| `instant_growth` | a normal victory sets `progress` to `cap` (the whole star's growth at once); boss fights are unchanged |
