# Achievements

The Radical Dreamers Edition's 37 Steam achievements as the engine evaluates them (`game::Achievements`, `src/game/achievements.*`). Steam itself is `platform::SteamClient`; see `docs/architecture.md` section 10. Steam stores which achievements are unlocked, so nothing here is kept between runs except the save's battle counter (`docs/formats/save.md` section 9).

No achievement reads a story flag. Each is one of the rule kinds below, keyed by the game's own ids. Room ids are the script's numbers (variable 6 the room, variable 8 the room before it); the same number can name different room files on the two discs, and only the number counts.

Nothing is earned unless `[Game] Steam` is on. A session in which the debug tools were used earns nothing. Cheats do not block achievements, as RDE's own boosts do not. The ways the engine differs from RDE here are listed in `docs/architecture.md` section 13.

## 1. Sampling

Outside a battle the room rules run once every 60 ticks, whatever the speed mode. A pair of rooms held for fewer samples than listed earns nothing, and leaving it starts the count again.

## 2. Rooms

| Achievement | Previous room | Room | Samples |
|---|---|---|---|
| `ach_eve1_yumemezame` | 0x1A | 0x1F | 20 |
| `ach_eve2_yumenaka` | 0x164 | 0x166 | 6 |
| `ach_eve5_shikaku` | 0x1D7 | 0xFF | 1 |
| `ach_eve6_chronop` | 0x103 | 0x1DD | 1 |

## 3. Endings

Sampled like the rooms. One value holds the armed ending, none at the start, and each sample compares the rooms with those of the last sample. When the room changed:

1. Arriving at one of these from a beach room (previous room 0x38 or 0x39) arms it:

   | Room | Armed | Room | Armed |
   |---|---|---|---|
   | 0x1CE | `ach_end3_kaihatu` | 0x1F8 | `ach_end7_mabure` |
   | 0x1E8 | `ach_end4_akasia` | 0x1EC | `ach_end8_yamaneko` |
   | 0x1ED | `ach_end5_akasia` | 0x21E | `ach_end9_youtien` |
   | 0x21C | `ach_end10_kuroi` | 0x1CB | `ach_end11_ryujin` |

2. Room 0x216 from anywhere arms `ach_end1_good`. Room 0x1F2 turns an armed `ach_end5_akasia` into `ach_end6_shop`. Room 0 clears the armed ending.
3. Room 9 awards the armed ending and clears it, except `ach_end1_good`, which stays armed.
4. `ach_end1_good` is awarded when the previous room changes to 9 while the room is 9: the ending room's jump to itself.
5. `ach_end2_bad` is arriving at room 9 directly from room 0x38, or from 0x39 with nothing armed.

## 4. Fights

A fight started by a script counts as won with result 0 (victory) or 3 (scripted end).

| Achievement | Rule |
|---|---|
| `ach_eve3_yamaneko` | Battle scene 433 won |
| `ach_eve4_darks` | Battle scene 473 won |
| `ach_eve7_fate` | Battle scene 462 won |
| `ach_eve9_tokiwokurau` | Battle scene 472 won, or element 526 cast (below) |
| `ach_battle1_condition` | 180 main-loop passes after a fight turns to fled (a Run, or a script forcing it). Not every result 2 is a flight |
| `ach_battle8_master` | The save's battle counter is at least 500 at a win. The counter is per save and counts monster-party fights too |

## 5. Element casts

Keyed on the element id, whoever casts it, 60 main-loop passes after the cast starts (element 526: 300). A pass is one tick in a battle and two in the field, so a delay still running when the battle ends takes longer there. Element 152 shares Forever Zero's name and does not count. A cast caught in an element trap does not count: its effect is replaced by the trap's. One award waits at a time: a cast or a flight replaces the one still waiting and restarts the delay.

| Achievement | Element |
|---|---|
| `ach_battle2_sky` | 162 Flying Arrow |
| `ach_battle3_heat` | 165 Hot Shot |
| `ach_battle4_zero` | 158 Forever Zero |
| `ach_battle5_tuki` | 228 Lunairetic |
| `ach_battle6_mixdelta` | 137 Delta Force |
| `ach_battle7_zetgiri` | 136 Z-Slash |

## 6. Items and script bits

Op BB (give) with this script item id, in this room:

| Achievement | Item id | Room |
|---|---|---|
| `ach_eve8_chrinocross` | 0x42D, key item 45 | any |
| `ach_item3_gran` | 0x26 | any |
| `ach_minigame1_esa` | 0x9E | 0x98 |
| `ach_minigame3_roulette` | 0x06 | 0x182 |

`ach_minigame2_gran` is op 71 setting variable bit 0x16C6 in room 0x187; another room sets the same bit, so the room is part of the rule.

## 7. The smith

An item forged with none in the bag before. An equipped copy does not count as held.

| Achievement | Item ids |
|---|---|
| `ach_item1_nijibuki` | 0x25 0x2C 0x2F 0x35 0x3C 0x45 0x4D 0x55 0x5D 0x63 0x6A 0x73 0x7A |
| `ach_item2_nijibougu` | 0x9B 0xA1 0xC7 |

## 8. Save fields

| Achievement | Rule |
|---|---|
| `ach_nakama1_jinkaku` | All 44 recruited bits set, at a sample. Using the Chrono Cross item is not checked |
| `ach_frame1_master` | The window frame word becomes 0x7FFF, at a sample. A loaded save that already has them counts |
| `ach_level1_gold` | The star total steps from 98 to 99. A loaded save already at 99 does not count |
