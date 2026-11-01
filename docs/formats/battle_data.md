# Battle and menu data tables (RDE `cdrom.dat`)

The on-disc tables that drive battles and menus: character and enemy stat records (`char*.bin`, `monster.bin`), the level-growth table, element/tech records (`pgdat.bin`), encounter scenes (`scenedat.bin`, `symtbl.bin`), drop tables, equipment tables and shop stock. The battle script bytecode (`battle/event/evt*.pac`) is in `battlescript.md`. Decoders: `tools/py/cc_tables.py` (commands `identify enemies elements groups drops growth chars keyitems shops gear bscript`); the engine loader is `data::GameTables` (`src/data/tables.cpp`); the battle rules built on it are `src/battle/rules.cpp` and `src/battle/battle.cpp`. `ocbattle tables` prints the table counts, `ocbattle sim <scene>` plays one battle headless and `ocbattle formula <name> <args>` evaluates one formula.

Confidence tags: **V** validated against the data or against names in the language tables, **C** documented elsewhere, not independently re-checked, **I** inferred, **?** unknown. All integers are little-endian. Paths are `cdrom.dat` members.

The PS1 numbers (scene 2534 = groups, 2535 = enemy stats, 0008 = drops) are PS1 file-table positions. In RDE they are parts of bundles (section 1), and the file index does not line up with the PS1 numbering in this range.

## 1. File map (`battle/data/*`, `menu/*`)

`battle/data` has 23 files. Containers: **cpt** = `u32 count, u32 offset[count], u32 end` (offsets from file start; first offset = 8+4*count); **sszl** = LZSS (1.1).

| File | Size | What it is | Records |
|---|---|---|---|
| `player.dat` | 19032 | cpt[6] of sszl: 0 `scenedat`, 1 `monster`, 2 `symtbl`, 3 `btbgdat`, 4 `waa`, 5 `btmes` | |
| `exdat.dat` | 5764 | cpt[7] of sszl: 0 aux A, 1 `el_mask`, 2 `ditem`, 3 aux B, 4 `menu_pg`, 5 `menu_waa`, 6 `lv99para` | |
| `eqp.dat` | 5744 | cpt[1] -> sszl -> identical to `pgdat.bin` (misleading name) | |
| `ditem.dat` | 1606 | sszl -> 2048 B = exdat part 2: drop tables | 256 x 8 B |
| `monster.bin` | 39072 | enemy stats (= player part 1) | 222 x 0xB0 |
| `char.bin` | 8976 | player stats | 51 x 0xB0 |
| `char20.bin` | 8976 | player stats, alternate set (the opening dream) | 51 x 0xB0 |
| `char_w0.bin` | 8976 | player stats preset 0 | 51 x 0xB0 |
| `char_w1..5.bin` | 9384 | presets 1..5 = 51 x 0xB0 + trailing `lv99para` (408 B) | |
| `lv99para.bin` | 408 | level-99 target stats (= exdat part 6) | 51 x 8 |
| `el_mask.bin` | 800 | element-grid growth table (= exdat part 1) | 100 x 8 |
| `pgdat.bin` | 23188 | element/tech battle records | 527 x 0x2C |
| `scenedat.bin` | 14364 | encounter scenes = "enemy groups" (= player part 0) | 513 x 28 |
| `symtbl.bin` | 6144 | battle id x 8 variants -> scene (= player part 2) | 384 x 8 u16 |
| `btbgdat.bin` | 300 | background id -> 3 initial field-ring colours (= player part 3) | 100 x 3 |
| `btmes.dat` | 1612 | battle message table (= player part 5) | 4 B + 201 x 8 |
| `waa.bin` | 7168 | equipment stat records (= player part 4), section 11 | 256 x 0x1C |
| `menu_pg.bin` | 4592 | menu-side element data (= exdat part 4), section 5.1 | 287 x 16 |
| `menu_waa.bin` | 3072 | item info: equip mask, price, kind (= exdat part 5), section 11 | 256 x 12 |
| `testview.dat` | 20 | debug-viewer parameters | |
| `menu/el_shop.bin` | 2048 | shop element stock | 32 x 32 u16 |
| `menu/waa_shop.bin` | 4096 | shop equipment stock | 32 x 64 u16 |
| `menu/trade.bin` | 2048 | trade/forge lists | 32 x 16 pairs |
| `menu/waa_tbl.bin` | 8192 | forge recipes (material counts), section 11 | 256 x 32 u8 |
| `menu/waa_name.dat` | 3246 | u16 offsets (256+1) + JP strings | |
| `menu/waa_spec.dat` | 6092 | u16 offsets (768+1) + JP strings | |
| `menu/keyitem.dat` | 3192 | u16 offsets (195+1) + JP strings | |
| `prog/stdtech0.prg`, `stdtech1.prg`, `stdmag0.prg` | 480 / 396 / 372 | tiny code overlays shared by many tech/element index entries | |

Every plain `battle/data` file that is also a bundle part is byte-identical to the decoded part. `char_w1..5` have records like `char.bin` with different values, then the same `lv99para` bytes. The `*.prd` files under `menu/` and `battle/help.prd` are resource packs and are not parsed here. Row counts of `waa_spec.dat` and `keyitem.dat` match the `waa_spec.txt` and `keyitem.txt` language tables.

`exdat` part 0 is the star-progress table, 100 x 4 B `{u8 battles, u8 pending_tenths, u16 step}` indexed by star level (`GameTables::star_rows`, section 3 growth); `step` is strictly increasing (1,2,4,6,10,15,21,29,...). Aux part (purpose unknown): part 3 is 652 B of 8-byte rows `{u16 id (0x140, 0x142, ...), u16 n, u16 1, u16 v}`.

The battle RNG permutation (`GameTables::rng_table`) is 256 bytes read from the executable member `prog/slps_023.64` at file offset 0x5B584. `rnd(lo, hi)` (lo, hi taken as u16) returns `lo` if `lo == hi`, 0 if `hi == 0`; for `hi < 0x100` it steps an index over the permutation and returns `lo + table[index] % (hi - lo + 1)`, otherwise it uses a 15-bit LCG (`state * 1103515245 + 12345`, bits 16-30) in place of the table; the same LCG is the libc `rand()` of script op 1F. The start index is the first LCG value mod 256.

### 1.1 sszl (LZSS)
Header 12 B: `"sszl"`, `u32 decoded_size`, `u32 aux` (ignored). Bit stream MSB-first: `1` + 8-bit literal; `0` + 12-bit ring position + 4-bit count, copy `count+2` bytes. 4096-byte zero ring, positions absolute. **The ring write pointer starts at 1, not 0.**

## 2. Id spaces

* Language strings: `lang.dat/sysmes.txt`, key `0xCCIIII` = category<<16 | index; TSV columns `Text JP EN FR IT DE ES`. Category 0x00 item names (index = item id 0..255), 0x01 character names, 0x17 element/tech names (index = element id 0..511), 0x18 key items (1..64), 0x19 enemy names (index = enemy id), 0x1A battle strings.
* **Character id** (= `char*.bin` record index = `sysmes` 0x01xx index = save-template order): 0 Serge, 1 Kid, 2 Guile, 3 Norris, 4 Nikki, 5 Viper, 6 Riddel, 7 Karsh, 8 Zoah, 9 Marcy, 10 Korcha, 11 Luccia, 12 Poshul, 13 Razzly, 14 Zappa, 15 Orcha, 16 Radius, 17 Fargo, 18 Macha, 19 Glenn, 20 Leena, 21 Miki, 22 Harle, 23 Janice, 24 Draggy, 25 Starky, 26 Sprigg, 27 Mojo, 28 Turnip, 29 NeoFio, 30 Greco, 31 Skelly, 32 Funguy, 33 Irenes, 34 Mel, 35 Leah, 36 Van, 37 Sneff, 38 Steena, 39 Doc, 40 Grobyc, 41 Pierre, 42 Orlha, 43..48 Pip variants, 49 Lynx, 50 unnamed dummy. Names are the EN `sysmes` rows.
* **Item ids** (equipment, drop and shop fields; `sysmes` cat 0 index): 0x01-0x1F materials (names carry a leading element glyph), 0x20-0x26 swallows, 0x27-0x2C daggers, 0x2D-0x36 rods, 0x37-0x41 swords, 0x42-0x47 guns, 0x48-0x4D picks, 0x4E-0x4F carrots, 0x50-0x58 axes, 0x59-0x5D lures, 0x5F-0x63 boomerangs, 0x65-0x6A shot, 0x6B-0x6C cards, 0x6E-0x73 utensils, 0x75-0x7A gloves, 0x96-0xB1 armour, 0xBA-0xFE accessories. 0x7B-0x7D are blank "natural weapon" placeholders (every enemy uses weapon 0x7D, armour 0xB8).
* **Text item-space**: `id < 0x200` item; `0x200 + n` element n; `0x400 + n` key item n. Shops and drops use the encodings in sections 7-8.
* **Element ids** = `pgdat.bin` record index = `sysmes` 0x17 index. 1-6 blue, 7-12 red, ... (six attacks of rising level per colour), 0x55 Cure, consumable elements with an alpha prefix (0x61 Medicine, 0x62 Ointment, 0x69 Tablet), weapon techs with a beta prefix from 0xA0 (0xA0 Dash & Slash), gamma-prefixed ones from 0x70. Records 512-526 exist in `pgdat` but have no name.
* **Grid cell value** (character and enemy element grid): `0x4000 | element_id` = element; `0xC000 | id` = weapon tech (bit 15); `0x4000` (players) / `0x0000` (enemies) = empty cell. Max id in enemy grids 510. Bit 14 (0x4000) is the cell's lock: the star unlocks clear it in a character record, and a battle unit's grid level cap (`gauge_max`) only counts cells without it. Every element cell of `monster.bin` keeps the bit set.
* **Colour bits** (innate byte, record +0x01 and element +0x05): `04` green, `08` white, `10` black, `20` red, `40` yellow, `80` blue. Serge 0x08 white, Kid 0x20 red, Guile 0x10 black. Bit 0x02 of an innate or armour colour byte marks an absorber: elements whose colour is in the byte's 0xFC mask heal the unit instead of damaging it.

## 3. Stat records: `monster.bin`, `char*.bin` (0xB0 bytes)
Enemies and players share one layout.

| Off | Type | Meaning | Conf |
|---|---|---|---|
| 0x00 / 0x02 | u8 | unknown (0 for 198/222 enemies, else 10/11) | ? |
| 0x01 | u8 | innate colour bit | V |
| 0x03 | u8 | **growth column** 0..7 for players (selects the `el_mask` column); enemies: 2 | V |
| 0x04 / 0x08 | u32 x2 | status weakness masks (lo / hi status word): a set bit doubles the chance that status lands on this unit (enemies; 8 = burn for 10 of them) | C |
| 0x0C / 0x10 | u32 x2 | status immunity masks (lo / hi word), OR-ed with the armour and accessory resist masks (11.1) | C |
| 0x14 | u8 | unknown (0, 1 or 0x70 for enemies; 1 for players) | ? |
| 0x15 | u8 | gender: 00 random, 40 male, 80/A0 female | V |
| 0x16 | u16 | max HP (Radius' record has 0, he reads 65535; set by his script) | V |
| 0x18-0x1D | u8 x6 | ATK, DEF, HIT, EVD%, MAG, MDEF | V |
| 0x1E | u8 | status duration stat (13 typical): a timed status lasts 3 x (stat + modifier) steps (1 x for status bits 19, 38 and 39) | C |
| 0x1F | u8 | stamina recovery rate per step (10 for everyone); accessories with gear flag 0x80 add their +5 byte | C |
| 0x20 | u16 | weapon item id | V |
| 0x22 | u16 | armour item id | V |
| 0x24 | u16 x3 | accessory ids | V |
| 0x2B | u8 | Dopplegang pool slot of this creature (variants share one: records 25 / 62 / 105 -> 10). The pool bit is set in the save when the creature dies in Sprigg's last command (`battle_effects.md`); the pool slot -> model table (220 `{u16 model, s16 val}` rows) is compiled into the engine | C |
| 0x2C | u8 | drop base `x` (150..200 for enemies): at a kill `r = rnd(0, 99)`; `r < x/20` drops the rare slot 1 of the drop row, `r < x/2` the common slot 0, else nothing (section 7) | C |
| 0x2D | u8 | model id (3.1) | C |
| 0x2E | u8 | action points | C |
| 0x2F | u8 | weapon model (0 none, 1..17 = `battle/wepon/*.kmd`, attached when the weapon id is nonzero) | C |
| 0x30 | u16 x8 | per-level cell mask: bit i of level L = column i present | V |
| 0x40 | u16 x48 | grid cells, listed row by row for the set mask bits (sum of popcounts = 48) | V |
| 0xA0 | u16 | drop table id (row of `ditem`) | V |
| 0xA2 | u16 | gold | C |
| 0xA4 / 0xA5 | u8 | HIT stat of the medium / heavy attack, used like +0x1A for that strength's hit base (players 0x32 / 0x14 or 0x0A) | C |
| 0xA6 | u8 | bit 7 set = not doppelgangable (no pool bit); low 7 bits grow with game progress | C |
| 0xA7 | u8 | unknown (0x55 for most bosses; 0xFB/0xBB/0x7A for others) | ? |
| 0xA8 / 0xAC | u32 x2 | permanent status lo / hi words, set on the unit at battle start (an enemy also gets lo 0x400) | C |
| 0xAE-0xAF | | type bits as u16 `(AE<<8)\|AF`: bit0 floating, 1 undead, 2 soft, 3 inorganic, 4 aquatic(?), 13 dragon, 14 human, 15 demon. As bits of the hi word at +0xAC they are 0x01000000 floating, 0x02000000 undead, 0x04000000 soft, 0x08000000 inorganic, 0x10000000 aquatic, 0x00200000 dragon, 0x00400000 human, 0x00800000 demon: the slayer masks of weapons (11.1) test them against the target's hi word | V |

Mask semantics checked on Serge: `03FC 03FC 03FC 03F8 01F0 01E0 01E0 01E0` -> 8+8+8+7+5+4+4+4 = 48 cells. Rows are star levels 1..8 (grid rows 0..7), columns 0..9; **slot id = cell index = row*10 + column** (the numbers that fill `el_mask`; max 0x4F).

Enemy max HP at battle start: New Game+ doubles it (cap 30000; not in scene 0x1DD, the final battle); then the gender byte (+0x15 & 0xC0; 0 rolls male or female with `rnd(1, 2)`) scales it, male x105/100, female x95/100 (at least 1); the result is capped at 32767 (signed 16-bit). Unverified: before the first recruitment enemy HP is halved (not implemented).

`lv99para.bin`: per character `{u16 HP, u8 ATK, DEF, HIT, EVD, MAG, MDEF}` (Serge 850/88/78/95/25/52/43). Growth (`battle::next_pending_hp` / `next_pending_stat` / `minor_growth`): each character holds a **pending delta** per stat. After a star-up battle the next one is rolled: `delta = round(rnd(1, 999) / 1000 * (Lv99 - cur) * 2 / (101 - star))` (HP adds 2), limited so the stat does not pass Lv99. The pending delta is stored in the save record next to a snapshot of the stats at the last star-up. A star-up (boss) battle adds the pending delta to every living party member and every bench character (HP capped at 999), then rolls the next ones. Every other battle fills a progress counter by the star's `step` up to `step x battles` (star row, section 1) and gives each living party member the minor gain `round_half_up(0.5 * ((snapshot + pending x pending_tenths / 10) - cur) * sqrt(progress / (step x battles)))` (not below 0, not above Lv99). The progress counter is not stored in the save.

Presets: `char_w0..5` and `char20` differ from `char.bin` in stats, weapon/armour ids and grid cells for many characters (e.g. Serge weapon 0x20 in char/w0, 0x23 in w3). OpenCross loads all eight sets (`GameTables::char_sets`); battles use `char.bin`, and the opening dream uses `char20.bin` until its end. Unknown: how retail selects `char_w0..5`.

Starting values (char.bin): Serge HP 50 ATK 9 DEF 5 HIT 85 EVD 8 MAG 7 MDEF 7, weapon 0x20 Sea Swallow, armour 0x96.

### 3.1 Model id (`+0x2D`) -> model file
The model table is the disc file table, in triples `{<n>.ltd texture, <n>.obj, weapon .bin}` starting at file-table entry 3194: **model m = entry 3194 + 3m**. The table is shared with the PCs: m 0..51 = `battle/player/*` (m 0 selju2, 1 kid ...), 52..63 empty, 64.. = monsters and re-used player models (64 radius, 70 carshu, 83 zoah, 102 malchera, 115 harley, 116 farga, 137 jakotu, 159 gyada, 161 olha, 211 slash) and `dummy/dummy.dat` placeholders (m 183-185, 186, 191, 202-208, 213, 214, 223 = Vita Unus/Duo/Tres, Polis Police, FATE, the Dragon Tears, Time Devourer, Cytoplasm...). All 222 records resolve to an `.obj` or `dummy.dat`. The mapping is the disc file-table order, not the sorted folder order and not a monster-only order. Spot checks by rendering: Beach Bum 65 `umibouz`, Sand Squirt 66 `ktokage`, Komodo Pup 67 `komodo`, Opah Fish 68 `chouchin`, Mama Komodo 69 `optokage`.

A party member's battle model is entry 3194 + 3 x its kind byte (save record +0x2D), not its character id, and its weapon pack is entry 3195 + 3 x the same byte: Pip's forms are 44..48 with `black.bin`, Serge in Lynx's body is kind 0x31, `sel_yam2/sel_yama` with Serge's `kai.bin`, and kind 0x33, which ryu_b29i writes before the duel, is `yamaneko/yamaneko` with `ply_yama.bin`. The name of a body follows `docs/rules/menus.md` section 14.

The dummy slots are **disc-1-only placeholders**: the disc-2 file index (loaded by the game's disc change when variable 0x42 = 1) fills all 15 of them (183 kuro, 184 rake, 185 ato, 186 robo_g, 191 fate, 202-207 dor_w / dor_bk / dor_b / dor_br / dor_a / dor_ay, 208 tokikui = Time Devourer, 213 / 214 awa, 223 yuka; the `.ltd` is entry - 1, same stem). Scenes 342-344 / 458-462 / 465-471 / 509 are therefore not cut content: with a disc-1 index the group member's model entry is the 24-byte `dummy.dat` and the retail loader hangs on its header; with the disc-2 index the battle starts. OpenCross: generated `src/game/battle/model_table.h` (`kBattleModels[m]`) carries the disc-2 stems.

## 4. Growth table `el_mask.bin`
100 rows (star level 0..99) x 8 columns of u8. Value = slot id (grid cell index, section 3) to unlock when the character reaches that star level, `0xFF` = none (row 0 is all FF); a cell is unlocked only if the character's grid has a populated cell there. Pip's growth column follows his form (kinds 0x2B..0x30 use 3 3 3 4 5 6). Lookup: `slot = el_mask[level*8 + char.growth_column]` where `growth_column = char[0x03]`. Serge (column 0): level 1 -> 0x07, level 2 -> 0x11, level 3 -> 0x1B. Max slot id 0x4F.
A value `v` unlocks bit `v % 10` of the character's unlock mask for grid row `v / 10` (eight u16 masks, one per row; a value with `v / 10 >= 8` is ignored). A new game ORs the masks of stars 0 .. S-1 for a game starting at S stars (20 for the opening's dream state, 0 for the real start) without testing that the cell is populated; a star-up in battle clears the unlocks, re-opens the populated cells of rows 0 and 1, and applies rows 0 .. S for the new star count S, only for populated cells. A cell is open when its grid cell holds an element and its mask bit is set; OpenCross derives this lock from the masks instead of keeping a separate lock bit in the grid word.

## 5. Element / tech records `pgdat.bin` (0x2C bytes; id = record index)
| Off | Type | Meaning | Conf |
|---|---|---|---|
| 0x00 | u8 | flags: `0x80` = the cast neither enters the field ring nor the cast-colour history; `(b & 0x30) == 0x10` = consumable stack (up to 5 per grid cell in the menu); `b & 0x44` and byte 4 & 0x41 gate the enemy element pick (`battlescript.md` 3.3) | C |
| 0x01 | u8 | flags: `0x02` = fixed element, cannot be moved or removed in the menu (every id >= 0x80 is fixed too); 01 for ordinary attack elements | C |
| 0x04 | u8 | flag byte: 0x80, or 0x90 (seen on Deluge, Iceberg, Inferno, Magma Bomb). Bit 0x10 = hits its whole range (every target of the side); bits 0x41 exclude it from the enemy element pick | I |
| 0x03 | u8 | bit `0x80` = innate-colour only: only a character whose innate colour equals the element's colour may place it (menu) | C |
| 0x05 | u8 | colour bit (80 blue, 20 red, 40 yellow, 04 green, 10 black, 08 white); `FC` = all colours | V |
| 0x06 | u8 | element level 1..6 (Aqua Beam 1, Ice Lance 2, Aqua Ball 3, Ice Blast 4, Deluge 5, Iceberg 6) | V |
| 0x07 | u8 | mode, the effect routine (5.2) | C |
| 0x08 | u8 | power (Aqua Beam 12, Ice Lance 15, Aqua Ball 20, Ice Blast 28, Deluge 30, Iceberg 38); modes 3 / 14: stamina given to the target = power x 10 | V |
| 0x0A | u8 | status / death hit chance % (0 Aqua Beam, 20 Aqua Ball, 50 Deluge); mode 3: heal power | V |
| 0x0C | u8 | variance `f` (20 for attacks; 0 UltraNova/BlackHole; 10 SexyWink); modes 3 / 14: % chance that each used grid cell of the target is recharged | V |
| 0x0E | u16 | mode argument (5.2): number of hits (modes 4, 29), field word (mode 6), trapped element id (mode 5), damage-share divisor (mode 35), flags (modes 0, 14, 1) | C |
| 0x10 | u16 | effect id: selects the element's effect pack (`battle_effects.md`; 0x20.. rising within a colour group) | C |
| 0x12 | u8 | placement window as two nibbles `hi lo` (0x77, 0x55, 0x33, 0x66, 0x44, 0x22 ... by level): the element may be placed in grid row `r` (0-based) when `level - hi <= r + 1 <= level + lo` | C |
| 0x13 | u8 | `btmes` row of the system message shown when the action ends (33..59 on 40 status / field records, e.g. Diminish 83 = row 54 "Element damage halved temporarily!"); 0 = none | C |
| 0x14 | u8 x3 | participants of a double / triple tech: character ids (Z-Slash 00 01 1A, X-Strike 00 13), `FF` = none; byte 0x14 == 0xFF is required by the enemy element pick. A PC cast with byte 0x14 != `FF` lists, for k = 0..2, the party slot holding character id `+0x14 + k` (a missing character leaves its entry empty) | C |
| 0x17 | u8 | a monster group tech's caster count (Cross Cut 2, Flame Knock 3), `FF` = none: the acting list is the first `count` units of the script's group list | C |
| 0x18 | u32 | status mask (Fire Pillar 0x08 burn, Aqua Ball 0x20 flu): no bit is a kill; the colour seals are lo bits 0x800 green, 0x8000 white, 0x80000 black, 0x800000 red, 0x8000000 yellow, 0x80000000 blue (a sealed colour's grid cells cannot be cast); byte 0: 01 poison, 08 burn, 10 sprain, 20 flu; byte 1: `01` afraid, `20` fatigued, `40` asleep; byte 2: `02` darkness, `04` stamina drain, `10` confused; byte 3: `04` cursed, `02` dizzy, `20` frozen (bytes as dumped). The hi word: bits 0-5 colour shield (green .. blue), `0x1000` always hits, `0x2000` / `0x8000` weapon absorb / immune, `0x4000` / `0x10000` element absorb / immune, `0x80000` regen, bits 29-31 special (only applied with an accessory 0xF9..0xFB); bits 19-28 are never applied by an element | C |
| 0x1C | u32 | stat-boost mask: `40 00 00 00` Att, `80..` Hit, `00 01` Def, `00 02` Evd, `00 04` MAtt, `00 08` MDef; the byte at +0x1F also differs per level (0x20, 0x40, 0x80) | C |
| 0x20 | u16 | a tech's double-tech partner: the other character's tech that completes it (Dash & Slash 0xA0 <-> 0xD9 Dash & Gash); 0 = none | V |
| 0x22 / 0x24 | u16 | a tech's two triple-tech partners (Luminaire 0xA1: 0xC9 and 0xDE); 0 = none. A record names either a double or a triple partner, never both | V |
| 0x26 | u16 | the double tech the 0x20 pair makes (0xA0 + 0xD9 -> 0x9F X-Strike) | V |
| 0x28 | u16 | the triple tech the 0x22 / 0x24 trio makes (0xA1 + 0xC9 + 0xDE -> 0x89) | V |
| 0x2A | u16 | 0 on techs; 1 / 2 on records 37-48 | ? |
| 0x20-0x2B | | records 511+ hold five `FF 00` words | ? |

527 records, 526 non-empty.

### 5.1 Menu-side element data `menu_pg.bin` (16-byte rows, index = element id, 287 rows)
| Off | Meaning | Conf |
|---|---|---|
| 0x00-0x05 | eligibility bitmask by character id (bit `id & 7` of byte `id >> 3`, ids 0..47): set = the character may place the element in the grid | C |
| 0x08 | s16 buy price in gold; the sell price is `price >> 2` | C |
| 0x0A | bits 0-1: HP healed when used from the menu (0 = 50, 1 = 100, 2 = 200, 3 = 0); bit `0x20` = the shops and the trading post neither buy nor list it | C |
| 0x0B | `0x80` = usable from the menu; `0x40` = it heals HP, else it removes the statuses in the low byte of pgdat +0x18 | C |
| 0x0C / 0x0D | auto-fill score of the element with 0 / 1 copies already placed | C |
| 0x06-0x07, 0x09, 0x0E-0x0F | not decoded | ? |

### 5.2 Element modes (pgdat +0x07)
The effect routine of a cast (`Battle::do_element`); `E` is the u16 at +0x0E. Damage modes use the formulas of section 10; "status roll" is section 10's status roll with the record's +0x18 / +0x1C words at +0x0A %.
* 0: status roll, then MAG damage (`E` bit 15: a landed status cancels the damage; `E` bit 14: a target with hi bit `0x2000000` is knocked out instead). 15: status roll first, MAG damage only when no status landed. 14 (Saints): by the target's slot, whoever casts it: an enemy slot takes mode 0, a party slot the mode 3 heal.
* 1: status roll only; with `E` bit 0 set (the consumables, Purify) the statuses are cured instead: every bit of the two words leaves the target with its timer, no roll, and the cast counts as landed. The Turn elements (Turn Green .. Turn Blue) are mode 1 with one hi bit 0..5 at 100 %: the target's colour status, which replaces any earlier one (the field ring only takes the cast colour as for any element). A unit with a colour status takes element damage x120/100 from the opposite colour and x80/100 from its own (section 10), and its weapon hits are of that colour. 2: the statuses, else darkness or stamina drain (50 / 50) at the same chance. 7: status roll, then every stat of the six bits `(+0x1C & 0xFC0) >> 6` is set to `power` %; with `E` != 0 (Medalsome) this part happens only to a target wearing the hero set (below). Then +0x0C != 0 heals the target by the heal formula (section 10) with +0x0C as its power, whether or not the status landed. 10: gate `rnd(1, 99) <= +0x0A`, then one random status of the lo mask `0x8E8EA939`. 13 (Revenge): gate `rnd(1, 99) <= +0x0A`, else a miss; then the caster's inflicted lo statuses within `0xAA9EED39` take the status roll on the target at +0x0A % and are cleared from the caster with their timers, landed or not. 36: `rnd(0, 99) < +0x0A` forces the colour curse of the target's innate colour (lo 0x901 green, 0xE000 white, 0xE0000 black, 0x900008 red, 0xE000010 yellow, 0xA0000020 blue).
* 3 / 43: heal; mode 3 by the formula of section 10, mode 43 (the consumables) by the flat value +0x0A (0 = "no damage"); both x125/100 when the caster's gear has flag 4 and x75/100 when the target's has flag 8, taken as damage by a target with hi status `0x2000000` (undead). The formula heal (modes 3 and 14) then cures the record's +0x18 / +0x1C statuses as mode 1 does (Holy Healing, Mer-Melody), gives the target `power x 10` stamina (Soothe) and draws one `rnd(0, 99)` per grid cell of the target, a used cell recharging when the roll is below +0x0C. 4: revive a KO'd target to `max HP x pct / 100`, pct = 100 when `E` != 0, 15 for an enemy caster, else `{30, 20, 20, 20, 10, 10, 10, 5, 30, 40, 50, 60, 70, 80, 90, 100}[distance + 8 x (row above the element level)]` with distance = |grid row - (level - 1)|.
* 29: weapon-tech damage: the weapon formula with multiplier `power`, medium strength, `E` hits (min 1), no hit roll, summed into one number. Hit k is dealt by entry k of the participant list (+0x14 / +0x17 above: each participant's ATK, weapon, slayer mask, gear and innate colour for the colour multiplier); when the list's first entry is empty (every single tech) the caster deals every hit. `E` equals the participant count on every double, triple and group tech. 32: ATK / DEF damage through the slot term, then the status roll, wakes the target. 33: fixed damage of `power` % of the target's current HP (`E` == 0) or of the caster's, then the status roll. 35: MAG damage without absorb; the caster recovers damage / `E` (all of it when `E` == 0). 37: mode 0 damage without status, then the field ring is repainted from the colour masks in +0x0A / +0x0C / +0x0E (ring-bit form, bit 0 green .. bit 5 blue). 40: Suicide Bomb: mode 0 damage pass, then the target takes half its current HP. 48: swaps the digits of the target's HP (three random swaps); the caster's HP becomes that number. 50: forced miss, resets the chain. 46 (Piggy Boink): mode 32's damage without the status roll and with power + script variable 0x176 (Van's piggy-bank savings in hundreds of G); a `rnd(0, 15)` is drawn first, which would double the savings term (and clear the word) only when below a 16-bit word of the field's VRAM backup buffer; during a battle that memory holds battle data whose word there reads 0 (RDE runs this element mode unchanged), so the savings term is never doubled. Then the target wakes. 51 (Slap of Cyrus): mode 29 when the caster wears the hero set, else a miss. Hero set: weapon 0x3D with exactly two accessories that are 0xC0 or 0xC1 (party members only), fixed per party unit at battle start (a Dopplegang form keeps it); its wearer's weapon term is doubled and its crit threshold lowered (section 10).
* 5: element trap: the caster's side traps element id `E`; an element of that id cast by the other side fizzles and the trap clears (an enemy element caught by the party's trap is added to the party's element stock after the battle). The HUD's trap block (`Battle::trap_hud`) is runtime state, not table data: it shows the element id each side has trapped, appears when the action that set a trap ends, and hides when a trap springs. 6: `E & 0xFC` = a colour: the whole field ring takes it; otherwise `E & 0xFF` becomes the timed field state (1 = elements weaker, x1/2; 2 = stronger, x3/2). 42: the caster's innate colour becomes the ring colour present 3 times, else twice, else a random ring colour. 47: the ring takes a random colour that some PC lacks. 12: recharges the target's used cells of the rows below the cast row (no effect from rows 0-1). 49: target stamina 70. 31, 34, 38: no effect.
* 30: pilfer: once per enemy; chance = the caster's heavy-attack hit chance, doubled for an opposite-gender target (then the rare chance is 20 instead of 10 when `E` != 0); `rnd(0, 100) >= chance` fails; the loot is slot 2 (common) or slot 3 (rare, `rnd(0, 99) < rare`) of the enemy's drop row, bit 15 = element; it goes into the stock at the end of the battle, on a victory only. 44: scripted-steal setup (no damage).
* The number an element shows is the damage rolled; an overkill is not cut to the target's HP (Big Catch 57 on a 13 HP target), as a weapon hit.
* 39: Big Catch (element 192): once per cast `rnd(0, 16) != 0` draws pool slots with `rnd(0, 255)` until the Dopplegang pool holds one; the hit is mode 0 damage with the slot term replaced by `max(10, (10000 - model) / 200) x 100`; no pool creature = the line breaks (every target misses). 41: the caster's control word := script var 0xDE. 45: Pip's form change (element 0x80 from kind 0x2B to 0x2C, 0x81 from 0x2B to 0x2D, 0x82 / 0x83 from 0x2C to 0x2E / 0x2F, 0x84 / 0x85 from 0x2D to 0x2F / 0x30; `power` = the new kind; battlescript A7). The other modes (8, 9, 11, 16-28) belong to no reachable element (9 to record 356, which no grid, shop, drop or script holds) and do nothing.
After every cast the field ring takes the element's colour (oldest slot leaves) unless +0x00 has `0x80`, the mode is 5, or the mode repainted the ring itself; the cast also enters the cast-colour history read by script ops AE / AB, except with +0x00 `0x80`. +0x13 != 0 gives the system message row shown when the action ends; a newly frozen (lo 0x20000000) target with HP shows row 6.

## 6. Encounter scenes `scenedat.bin` + `symtbl.bin` ("enemy groups")
`scenedat`: 28-byte records. The battle script of scene `i` is the file named by file-index record `2679 + i` (`cdrom.fnd`; the two discs agree on these records), so the name, not the scene number, picks the `.pac`. 60 records name the shared template `evt0000.pac`, and 14 scenes share another scene's script: 4 and 410 -> 409, 5 -> 411, 10 -> 412, 11 -> 406, 52 -> 49, 53 -> 50, 297 -> 296, 300 -> 297, 344 -> 343, 424 -> 414, 483 -> 421, 484 -> 422, 485 -> 478. These are the pairs RDE's `btel` text lookup remaps. A file with a scene's number can exist and still go unused by that scene (`evt297.pac` serves scene 300).

| Off | Meaning |
|---|---|
| 0-2 | enemy id per slot (0xFF = empty); index into `monster.bin` |
| 3-5 | count of that enemy; nonzero = slot active |
| 6-14 | start-position nibbles (x/y): bytes 6-8 are the 3 PCs (always `8D 4B CB`), 9-14 the enemy instances (`0x11` filler); 9 unit slots = 3 PCs + 6 enemies. |
| 0x0F | bit 0 copied to a battle flag (bit 1 set for scene 0x1D8): a monster-party scene (the Grand Slam), whose party slots fight as the picked creatures (`rules/battle.md` 9.3) |
| 0x10 | scripted-death mask: bit k = enemy slot k + 3 starts with its script-death flag set (`battlescript.md` op 5C), i.e. it owes a scripted death |
| 0x11-0x19 | per-slot facing: `00 02 02 02 06 06 06 06 06` |
| 0x1A | low 7 bits: battle background id (index into `btbgdat`; the stage file `battle/bg/bg_NN.prd`); bit 7: star-up (boss) battle, its victory applies the pending stat deltas (3) |
| 0x1B | off-stage enemy mask: the all-target cast mask starts as `~(b << 3)`; bit k set in `b` = enemy slot k + 3 is kept out of all-target casts until a script reveals it with ops 34 / 35 / 5B arg 0 (scenes 84-87 Portalgheist = 3: the two monsters inside the door; 27 scenes non-zero) |

Formation: a unit's start-position byte `b` places it at world `x = (2 * (b & 15) - 15) * 256`, `z = (15 - 2 * (b >> 4)) * 256` (the low nibble is the column, the high nibble the row); its facing byte `f` is a yaw of `f * 45` degrees (`f << 9` in 4096 units per turn), so 2 looks toward -x (party) and 6 toward +x (enemies).

The entity count of the paired `.pac` equals `5 + sum(counts)` for 421 of 439 scenes (the other 18 have extra entities).

`symtbl`: `scene = symtbl[battle_id*8 + variant]` (u16), 384 x 8. `variant` (0..7) selects one of up to eight enemy groups for the id (89 of the 94 used ids differ between variants); a battle id >= 0x200 skips the table (scene = id - 0x200).

Variant roll: every battle started from the field with an id < 0x200 (field op A3) draws one `rnd(0, 2549)` (section 1: a range above 0xFF is one libc `rand()`, `value % 2550`) and looks it up in the 8 bands `{u16 lo, u16 hi}` stored in the battle loader `prog/btlinit.prg` at file offset 0x6AF8; band `i` holds the rolls `lo <= roll < hi` and gives variant `7 - i`. Retail bands: `{0,10} {10,30} {30,70} {70,150} {150,310} {310,630} {630,1270} {1270,2550}`, i.e. variant 0..7 has weight 1280 / 640 / 320 / 160 / 80 / 40 / 20 / 10 out of 2550 (each half the one before). Ids >= 0x200 (op C9, scripted fights) draw nothing and keep their scene; nothing else (story flags, the encounter's field entity, the party) takes part. A battle started outside the field (a debug launch) uses a fixed variant, 0. Example: id 1 = `6 7 8 6 7 9 9 9`, so its Komodo Pup group (scene 6) comes up 1440 times in 2550. OpenCross: `battle::encounter_scene` (`src/battle/rules.cpp`), called by `make_field_battle_args` (`src/app/app_battle.cpp`); the bands load into `GameTables::variant_bands` (empty without the file: variant 0).

`btbgdat`: 100 x 3 u8, the colour index (0..5) of each of the 3 initial field-ring slots for background id `scenedat+0x1A & 0x7F` (or the battlefield the caller names); used as `1 << (value & 0x1F)`, a value >= 6 leaves the slot empty. Applied by `Battle` at battle setup.

## 7. Drop tables `ditem.dat`
256 rows x 4 u16; row = the enemy `drop_id` (+0xA0; same-species enemies repeat ids). Each u16: `0` empty; bit 15 set = **element/consumable** with element id `v & 0x7FFF` (0x8069 Tablet, 0x8061 Medicine, 0x8062 Ointment: low-level enemies drop potions); bit 15 clear = **item id** (materials 1-0x1F, accessories such as 0xC8 Power Glove ...). Typical row `[material, material, element, element]`; strong enemies `[accessory, accessory, material, material]`. Slots 0 / 1 are the kill drops, slot 0 common and slot 1 rare, chosen by the drop roll of stat record +0x2C (section 3); slots 2 / 3 are what the pilfer command steals, slot 2 common and slot 3 rare (5.2, mode 30). Gold is in the stat record.

## 8. Menu tables
* `waa_shop.bin`: 32 rows x 64 u16 item ids (0-terminated; the smith lists the ids whose item-info kind matches the tab: weapon, armour, accessory). Row 0 = Ivory Dagger, Bone Glove, Ivory Vest, Ivory Mail, Ivory Helmet.
* `el_shop.bin`: 32 rows x 32 u16 element ids (0-terminated; 16 non-empty). Row 0 = Tablet, Cure, Fireball, Aqua Beam, Medicine, Antidote.
* `trade.bin`: 32 rows x 16 entries `{u8 goods, u8 colour mask, u16 level sum}`; 31 non-empty. A row ends at the first entry with goods 0; an entry with level sum 0 is a hole that keeps its index. `goods` is the item id given (materials 7..16); `colour mask` (0x80 blue, 0x20 red, 0x40 yellow, 0x04 green, 0x08 white, 0x10 black; 0 = any) restricts which elements pay for it; the trading post gives the goods once for offered elements (those with `menu_pg` flag 0x20, 5.1, are not offered) whose summed `level x count` reaches the level sum, and takes every offered copy.
* Key items: ids 1..64 (`sysmes` 0x18; `0x400 + n` in item-space). `keyitem.txt` has 195 rows = 3 per key item (row `3*n` holds the description; rows +1/+2 are blank in EN). `keyitem.dat` holds the JP text via 195 u16 offsets (first offset 0x188).
* `waa_name.dat` / `waa_spec.dat`: JP name (256) and description (768) tables indexed by waza index; EN text lives in `sysmes.txt` / `waa_spec.txt`.

## 9. Battle script files
The `battle/event/evt*.pac` files (440: 439 + the `evt0000.pac` template) are documented in `battlescript.md`.

## 10. Formulas (`src/battle/rules.cpp`)
All integer arithmetic truncates (C `int`); every random term is one `rnd(lo, hi)` draw (section 1). `Sum(n, p, v)` = the sum over `n + 1` terms of `p x 100 - p x rnd(0, v)`.
* Weapon damage: `raw = (Sum(ATK + 1, wp, wpVar) - Sum(DEF + 1, arm, armVar)) x mult / 20000`, at least `(wp - arm) / 2`; a negative result becomes `rnd(0, 3)`. `mult` = 10 / 25 / 40 for a PC's weak / strong / fierce hit (10 / 12 / 15 for an enemy). `wp` = the weapon term: `ceil(ceil(weapon power x Att% / 100) x (100 + 10 x (hi >> 29)) / 100)`, doubled when the attacker wears the hero set (5.2, mode 51); `arm = ceil(armour term x Def% / 100)`, `arm` x3/4 while the target has lo status `0x4008`. x3/4 when the target's gear guard mask meets the attacker's hi word. Then x `c` / 1000. Critical: `rnd(afraid target ? 5 : 0, fierce ? 15 : 14) >= 15` (`>= 7` for a hero-set attacker), or the weapon's slayer mask meets the target's hi word; it adds the class' `CRIT` percent (`{50, 100, 80, 100, 50, 100, 100, 100, 100, 100, 100, 80, 100, 100, 100, 100}` by class `& 15`). Finally x3/2 when the attack colour is the opposite of the target's, /2 against a guarding PC, x125/100 for the attacker's gear flag 0x04, x75/100 for the target's flag 0x08. Weapon absorb / immune hi bits turn the value into a heal / nothing.
* Weapon techs (mode 29): the same formula with `mult` = the record's power, medium strength, no hit roll, summed over the record's hit count; only the slayer mask makes a critical (the hero set's threshold does not apply). Like every element command the attack colour is the record's (+0x05), not the caster's innate colour or colour status; the caster's innate colour still adds its point, the opposite-colour step compares the record's colour, and the field state (mode 6) does not scale it.
* `c` colour multiplier (per mille): colour points are 1 for the attacker's innate colour and 3 for the attack's colour (a weapon attacks in the colour of the attacker's colour status, hi bits 0..5, else in its innate colour; an element in its own; the opposite-colour step uses the same attack colour); opposite colours (green-yellow, white-black, red-blue) cancel; each field-ring colour adds its count to its own points when the opposite has none, else removes up to that count from the opposite; the target's colour loses `1 + its ring count`. The attack colour's points index `{665, 750, 875, 1000, 1125, 1250, 1375, 1500, 1500, 1500}` (negative = 500, over 9 = 1500). Element mode 6's timed field state halves (1) or raises x3/2 (2) the result.
* Element damage: `raw = (Sum(MAG + 1 + magTerm, power, var) - Sum(MDEF + 1, mdefTerm, mdefVar)) x slot / 400000`, at least `(power - mdefTerm) / 4`, negative = `rnd(0, 3)`; x120/100 when the caster's MAtt% > 100, x80/100 when < 100; x `c` / 1000; then x`(100 + r)` / 100 with `r = rnd(0, 5)` (50 when the element's hi status mask meets the target's hi word); the target's MDef% below / above 100 gives x120/100 / x80/100; a colour shield of the opposite / same colour x120/100 / x80/100; an armour colour boost x150/100; x3/2 for the opposite colour; the gear steps as above. `magTerm = ceil((1 + weapon MAG bonus + offence accessories) x MAtt% / 100)`, `mdefTerm` likewise from the armour (x3/4 while the target has lo status 8), `mdefVar` = the target's armour variance. Element absorb / immune turn the value into a heal / nothing. `slot` = `1000 +/- 125 x n`, `n = |grid row - (level - 1)|`, plus when the element sits above its own level (0 level = 1000).
* Healing (mode 3, mode 14 on a party slot; mode 43 is flat, 5.2): `max(1, Sum(MAG + 1, power, power) / 400)` with `power` = pgdat +0x0A, x `slot` / 1000 x `c` / 1000, at most 999; a target with hi status `0x2000000` takes it (and mode 43's flat heal) as damage.
* Hit %: `base = stat / 3 + min(wpAcc, 99) x 200 / 300` with stat = HIT, +0xA4 or +0xA5 by strength; `chance = base x Hit% / 100 - evade`, `evade = ceil(EVD total x EVD% / 100)` (x125/100 against a hero-set attacker, then halved against a dizzy target), not below 0. A PC's chain counter then pulls it toward 102 / 102 / 103 (weak / strong / fierce) by 30 / 25 / 20 % once per counter point, stopping at 99; every landed hit adds 1 / 3 / 7 to the counter, a miss or another target resets it. Caps: 99; always-hit hi bit 99; darkness halves; a sleeping target is 100. A hit lands unless `chance x 10 < rnd(0, 1000)`.
* Status: lands when `rnd(1, 99) <= chance` (chance doubled by the target's weakness bit); a timed status lasts `3 x (duration stat + modifier)` steps (1 x for status bits 19, 38 and 39).
* Stamina: a weapon hit costs `(strength + 1) x 10` (x500/400 while fatigued), the pool tops out at 70, an element costs 70; recovery per step is `(rate + modifier) x steps` (x300/400 with flu).

Variable mapping: ATK DEF HIT EVD MAG MDEF = stat record +0x18..+0x1D; Att% / Def% / Hit% / EVD% / MAtt% / MDef% = the unit's six temporary stat modifiers (100 at the start; element mode 7 sets them); `power` = pgdat +0x08, `var` = pgdat +0x0C; weapon power / accuracy / variance, armour term and the gear sums = the equipment records (section 11.1).

## 11. Equipment tables
"waa" is **waza/equipment**: materials, weapons, armour and accessories share one id space (section 2). Four files, all indexed by item id 0..255:

| Data | File | Record | Used for |
|---|---|---|---|
| stats | `player.dat` part 4 (= `waa.bin`, 7168 B) | 0x1C | battle setup (5 records per PC: weapon, armour, 3 accessories) and shops |
| info | `exdat.dat` part 5 (= `menu_waa.bin`, 3072 B) | 12 | shops (equip check, price, category) |
| forge | `menu/waa_tbl.bin` (8192 B) | 32 x u8 | units of material id 0..31 needed to make item *i* |
| stock | `menu/waa_shop.bin`, `menu/el_shop.bin` | section 8 | shops |

### 11.1 Stat record (0x1C bytes, `waa.bin`)
| Off | Type | Weapon | Armour | Accessory | Conf |
|---|---|---|---|---|---|
| 0x00 | u8 | material tier 0..9 (0 bone/ivory .. 5 prism, 6+ unique) | 0 | 0 | I |
| 0x01 | u8 | class: low 5 bits index the crit table `CRIT[16]` (1 swallow 2 dagger 3 rod/staff 4 gun 5 pick 6 sword 7 axe 8 glove 9 lure 0xA shot 0xB boomerang 0xC utensil); bit 7 set for rods/boomerangs | 0 | bit 4 set = its +0x10 mask joins the weapon slayer mask | C |
| 0x02 | u8 | attack mode 0x10 + class group (swallow 10, dagger 11, rod 12, gun 13, pick 14, sword 15, axe 16, glove 17, lure 18, shot 19, boomerang 1A, utensil 1B) | 0 | 0 | C |
| 0x03 | u8 | 0 | Plates: colour bit + 0x02 (the 0x02 marks an absorber, 2); materials: element colour bit (Shiny Ember 0x20 red ...) | 0 | I |
| 0x04 | u8 | **wp** weapon power | **DEF term** | value added to the sum selected by its flag | V |
| 0x05 | u8 | MAG bonus (MAG term = 1 + this + sum) | **MDEF** bonus (MDEF term = 1 + this + sum) | flag 0x20: MAG, 0x40: MDEF, 0x80: stamina recovery | V |
| 0x06 | u8 | **accuracy** | **EVD** bonus | flag 0x20: accuracy, 0x40: EVD | V |
| 0x07 | u8 | **variance** (`wpVar`) | armour variance (`armVar`; also the MDEF variance) | 0 | V |
| 0x08 | u16 | gear flags (below) | same | same | V |
| 0x0A | u16 | 0 | 0 | body-slot class (1 ear, 2 purse, 4 eyes, 8 head, 0x10 sigil, 0x20 feet); menu only | I |
| 0x0C | u32 | special lo mask (unused so far) | resist lo mask | resist lo mask (only with flag 0x40): Antitoxinal Cap 0x01 = poison, Poultice 0x08, Plaster 0x10, Antiviral 0x20, Charms/Brooches = colour/element type bits | C |
| 0x10 | u32 | **slayer mask**: crit when `(mask & target hi) != 0`. Stone weapons 0x08000000, guns 0x01000000, Mastermune 0x0FE00000, Einlanzer 0x00020000 | resist hi mask | resist hi mask (flag 0x40) | C |
| 0x14 | u32 | permanent status lo, OR-ed into the unit's status over all five records | same | same | C |
| 0x18 | u32 | permanent status hi (Mastermune, Energizer Suit 0x80000 = regen; Ghetz's Shirt lo 0x02062100 = curse) | same | same | C |

Resist masks of armour and of flag-0x40 accessories are OR-ed together with the unit's own record words (stat record +0x0C / +0x10) into the unit's resist masks; the armour and defence-accessory hi masks also make every weapon and tech hit on the wearer x3/4 when they meet the attacker's hi word (its type bits, 3).

**Gear flags** (u16 at +0x08; `& 0xD` is also kept per unit):
* `0x01` max-HP percent (value = percent: Earrings 5/8/12/15; sequential `hp = hp*(100+n)/100`, +1 when unchanged, cap 999);
* `0x02` gold x125/100 after battle (Profiteer Purse);
* `0x04` outgoing damage x125/100 (Sunglasses); `0x08` incoming damage x75/100 (Moonglasses);
* `0x10` start of battle element-level gauge += value, capped at the gauge limit (Dreamer's Bandana/Sash/Sarong);
* `0x20` offence accessory: its +4/+5/+6 add to wp / MAG term / accuracy (Power Glove, Seals, Rings, Loupes, Honor/Glory);
* `0x40` defence accessory: its +4/+5/+6 add to the armour term / MDEF term / EVD, and its resist masks apply (Pads, Pendants, Shoes, Helmets, Charms, Brooches, Defender);
* `0x80` stamina accessory: +5 is added to the stamina recovery modifier; +4 (signed byte, `>= 0x50` means `-(v & 0x7F)`) is added to the unit's second stamina modifier (Stamina Ring/Belt, Resistance Ring/Belt). OpenCross applies the +5 byte only (`Equip::stamina_mod`).

Derived unit totals (recomputed at battle setup and on every stat refresh): `wp = weapon.+4 + sum(flag-0x20 .+4)`; `armour term = armour.+4 + sum(flag-0x40 .+4)`; `acc = min(99, weapon.+6 + sum(flag-0x20 .+6))`; hit bases = `stat/3 + acc*200/300` for HIT / `+A4` / `+A5`; `EVD = EVD + armour.+6 + sum(0x40 .+6)`; `MAG term = 1 + weapon.+5 + sum(0x20 .+5)`; `MDEF term = 1 + armour.+5 + sum(0x40 .+5)`; weapon class = `weapon.+1 & 0x1F`. OpenCross: `battle::make_equip`.

Enemies use the same records (weapon 0x7D: wp 12, acc 99, var 0, class 0; armour 0xB8: DEF 4, MDEF 4 -> term 5).

Cross-check (EN names): Sea Swallow 0x20 {wp 8, acc 0x59, var 0x14, class 1}; Copper..Spectra Swallow wp 10, 13, 16, 20, 25; Ivory Mail 0x96 {DEF 3, MDEF 1, var 5} is Serge's starting armour; Ivory Vest 0x9C {DEF 2, EVD 3, var 10}; Power Glove 0xC8 flag 0x20, +1 wp; Knee Pad 0xD1 flag 0x40, +1 DEF; Plates 0xA2..0xA7 carry the six colours; Ghetz's Shirt 0xB1 is the curse item.

### 11.2 Item info (12 bytes, `menu_waa.bin`)
| Off | Type | Meaning | Conf |
|---|---|---|---|
| 0x00 | u64 | equip mask, **bit = character id** (51 bits; swallows = Serge only, daggers = Kid, armour/accessories 0x7FFFFFFFFFFFF) | V |
| 0x08 | s16 | price / 10 (buy price = x10; the shops pay `price >> 2` when buying equipment back, no sell price is stored) | C |
| 0x0A | u8 | low nibble kind: 0 weapon, 1 body armour, 2 helmet/accessory, 4 material; high nibble price/rarity class | C |
| 0x0B | u8 | bit `0x80`: the shops do not buy it; bit `0x20`: the smith cannot break it down (0xA0 = event rewards) | C |

### 11.3 Forge `waa_tbl.bin`
Row *i* (32 bytes) = quantity of material *m* (m = material item id 0..31) needed for item *i*; 0 = not forgeable. Example: Ivory Vest {1 Bone, 11 Shiny Ember}, Power Glove {2 Copper, 13 Shiny Leaf}. The shop checks that all counts are owned and consumes them on forging. Forge price = `(price10*10 of the highest material id in 1..6 used, 1000 if none) / 5 * total units`. The smith can also break a piece down (not with item-info bit `0x20`): it returns the recipe's materials, and materials that no longer fit the 99-per-slot bag are paid as gold, `price / 4` each.

### 11.4 Shops
`waa_shop.bin` rows list item ids (section 8); the price of a listed item is `ItemInfo.price`; the smith buys bag gear back at `price / 4` (not materials, not items with item-info bit `0x80`). The element shop sells at the `menu_pg` price (5.1) and buys back at `price >> 2` (not elements with `menu_pg` flag `0x20` or fixed elements). `trade.bin`: section 8. Tooling: `python -I tools/py/cc_tables.py gear` (CSV incl. forge column) and `shops` (with prices); loader `data::GameTables::{gear,item_info,forge}` (`src/data/tables.h`).
