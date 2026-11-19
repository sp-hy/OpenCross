# OpenCross achievements

OpenCross's own achievement set: 173 achievements that the engine keeps for the player, apart from the 37 Steam achievements of `achievements.md`. They are an addition, not part of the original game, so they never change play: they only watch it. Nothing here touches Steam, the save file or the Steam rules.

Ids in the tables are the game's own: room ids are the script's numbers (variable 6), the same as `achievements.md`; scenes are battle scene numbers (`ocbattle scenes`); elements are element ids (`ocbattle elements`); characters are the character ids of `formats/battle_data.md`; item ids are script item ids (`formats/save.md`: below 0x200 an item, 0x401 and up a key-item bit). Where a place has a room on each world, both ids count.

## 1. Switch, gates and storage

* **Switch.** `[Game] Achievements` (bool). The player's ini reads a missing key as 1, a `--config` ini as 0 (`formats/config.md` section 3). Off: nothing is evaluated, no popup, the Achievements page still lists the stored unlocks. It is set on the Achievements' Options page of the OpenCross menu row (section 10) or in the ini.
* **Gates.** The same as Steam's: a session in which the debug tools were used earns nothing and moves no counter or profile set, and cheats do not block anything. A versus battle (`versus.md`) counts nothing on either side. `[Game] Steam` plays no part.
* **Profile.** Unlocks belong to the player, not to a save: one file, `achievements.txt`, next to the player's ini (beside `opencross.exe`, or in the `--profile` folder). It is never written in the save folder. A `--config` run keeps its unlocks in memory and writes nothing.
* **File.** Text, UTF-8, LF line endings, read with the `Ini` rules of `formats/config.md` section 1:

  ```
  [Unlocked]
  oc_boss_hydra=2026-11-16T14:02Z
  [Counters]
  battles_won=212
  steals=31
  forges=7
  [Seen]
  oc_story_three_routes=5
  ```

  `[Unlocked]` maps an achievement id to the UTC time it was earned. `[Counters]` holds the profile counters of section 2. `[Seen]` holds, per profile-set achievement (section 2), a bitmask of the members seen so far, bit i = the i-th member as listed in its table. The file is rewritten whole after every change. An id this build does not know is kept and written back as it was, so an older build does not drop a newer build's unlocks. A file that cannot be parsed is renamed to `achievements.bad.txt` and a new one started.
* **Once.** An unlocked achievement is never evaluated again and never re-locked.

## 2. Rule kinds

| Kind | Earned when |
|---|---|
| `room` | A sample finds the room in the listed ids. |
| `room_pair` | A sample finds the previous room (variable 8) in the first list and the room in the second. |
| `recruit` | A sample finds the character's recruited bit (`formats/save.md`, 0x590) set. |
| `recruits` | A sample finds at least N recruited bits set. |
| `body` | A sample finds the listed model byte (record +0x2D) on a party member, or on Pip's record while he is recruited. |
| `save` | A sample finds gold at or above the threshold, or a script variable equal to the listed value. |
| `var_bits` | A sample finds a listed bit of a script variable set; with `profile`, each bit is a member. |
| `fight` | A battle with one of the listed scenes ends won: result 0 (victory) or 3 (scripted end), as in `achievements.md` section 4. |
| `cast` | An element in the listed ids is cast by a party member and runs (a cast caught in an element trap does not count). |
| `item` | A script gives (op BB) the listed item id, in any room. |
| `count` | A profile counter reaches N. |
| `profile` | Every member of the listed set has been seen, across any saves on the profile; the members are tracked in `[Seen]`. A member is a room, a won scene, a model byte, a bit of a variable, a summon's colour, a double or triple tech, or an ending, as the row says. |
| `set` | Every listed achievement is unlocked. |
| `event` | The battle event named in the row happens (section 3). |

**Sampling.** Outside a battle the `room`, `room_pair`, `recruit`, `recruits`, `body`, `save` and `var_bits` rules run once every 60 ticks, whatever the speed mode, like the Steam room rules; one sample is enough. The other kinds are evaluated when their event happens. A `set` is checked after every unlock.

**Counters.**

| Counter | Adds 1 when |
|---|---|
| `battles_won` | A battle ends won (result 0 or 3), any scene. This is the profile's own count, not the save's battle counter. |
| `steals` | A party member's steal takes an item (not an element). |
| `forges` | The smith forges an item. |

## 3. Battle events

| Event | Happens when |
|---|---|
| `big_hit` | One hit by a party member takes 9,999 HP or more from one enemy. |
| `last_standing` | A battle whose scene has the boss flag ends won with exactly one party member above 0 HP. |
| `one_colour` | After a party member's cast, all three field-effect colours are the same colour. |
| `trap_catch` | An element trap the party set catches an enemy's element: the trap holds that element. A scripted fizzle that springs the trap whatever it holds is no catch unless the trap held the element. |
| `level_eight` | A party member casts an element whose table level is 8 (element 126, the Chrono Cross, excluded). |

## 4. Story and places

| Id | Title | Description | Rule |
|---|---|---|---|
| `oc_story_cross` | Wrong Side of the Tide | Cross into Another World for the first time | room 28 |
| `oc_story_manor` | Uninvited Guests | Reach Viper Manor | room 194, 197 |
| `oc_story_three_routes` | Every Door Tried | Recruit each of the three Viper Manor guides (Guile, Nikki, Pierre), across any saves | profile: `var_bits` variable 0x1C2 bits 0 (Nikki), 1 (Pierre), 2 (Guile), which Termina's scripts set when that guide joins and the manor's routes test |
| `oc_story_fort` | Top of the Fort | Reach Fort Dragonia | room 311, 312 |
| `oc_story_swap` | Not Yourself | Go through the body swap | body 0x31 |
| `oc_story_vortex` | Out of the Vortex | Leave the Temporal Vortex | room_pair 372, 374 to 236 |
| `oc_story_humour` | Bitter Medicine | Get the Hydra Humour | item 0x415 |
| `oc_story_dead_sea` | Frozen Sea | Reach the Dead Sea Ruins | room 411 |
| `oc_story_tear` | Dragon's Tear | Get the Dragon Tear | item 0x418 |
| `oc_story_tower` | Skyward | Reach Terra Tower | room 506 |
| `oc_place_guldove` | Island of Blades | Reach Guldove | room 203, 204 |
| `oc_place_marbule` | Demi-human Haven | Reach Marbule | room 397, 409 |
| `oc_place_hermit` | Hermit's Door | Find the Hermit's Hideaway | room 279, 280 |
| `oc_place_damned` | Damned Shores | Set foot on the Isle of the Damned | room 420, 421 |
| `oc_place_navel` | Navel Gazing | Enter Gaea's Navel | room 450 |
| `oc_place_pyre` | Hot Feet | Climb Mount Pyre | room 297, 298, 307, 308 |
| `oc_place_triangle` | Lost Triangle | Explore the El Nido Triangle | room 443, 444 |
| `oc_place_cruise` | All Aboard | Board the cruise ship | room 382 |
| `oc_place_pirate` | Ship Shape | Board the pirate ship | room 262 |
| `oc_place_termina` | Port of Call | Reach Termina | room 77, 118 |
| `oc_place_cape` | Cliff Edge | Reach Cape Howl | room 58, 59 |
| `oc_place_forest` | Deep Woods | Enter the Shadow Forest | room 122, 123 |
| `oc_place_fossil` | Dig Site | Enter Fossil Valley | room 66, 70 |
| `oc_place_lizard` | Rock Climber | Climb Lizard Rock | room 50, 51, 54, 55 |
| `oc_place_marsh` | Wet Boots | Wade into the Hydra Marshes | room 230, 231 |
| `oc_place_marsh_sky` | High Ground | Reach the sky above the Hydra Marshes | room 238, 239 |
| `oc_place_water_isle` | Blue Isle | Reach Water Dragon Isle | room 285, 286 |
| `oc_place_earth_isle` | Sand in Your Shoes | Reach Earth Dragon Isle | room 412, 413 |
| `oc_place_sky_isle` | Cloud Walker | Reach Sky Dragon Isle | room 433, 434 |
| `oc_place_opassa` | Shell Collector | Walk Opassa Beach | room 56, 57 |
| `oc_place_fates` | Past, Present, Future | Visit all three Fates | profile: room 474, 475, 476 |
| `oc_place_all` | Grand Tour | Earn every place achievement above | set: every `oc_place_*` above |

## 5. Bosses

| Id | Title | Description | Rule |
|---|---|---|---|
| `oc_boss_komodo` | Lizard Queen | Beat Mama Komodo | fight 389 |
| `oc_boss_ambush` | Dragoon Ambush | Beat Karsh, Solt and Peppor | fight 390 |
| `oc_boss_solt` | Comic Relief | Beat Solt and Peppor | fight 393, 396, 402, 474, 478, 485 |
| `oc_boss_ketchop` | Saucy | Beat Solt, Peppor and Ketchop | fight 402 |
| `oc_boss_moaman` | Moaning Monarch | Beat King Moaman | fight 399 |
| `oc_boss_bulb` | Lights Out | Beat the Neo-N-Bulbs | fight 406 |
| `oc_boss_marcy` | Wired | Beat Marcy | fight 407 |
| `oc_boss_lynx` | Cat Fight | Beat Lynx in Viper Manor | fight 408 |
| `oc_boss_dwarves` | Dwarf Trouble | Beat the Dwarf and his band | fight 414, 424 |
| `oc_boss_tank` | Tank Buster | Beat the Hi-Ho Tank | fight 425 |
| `oc_boss_hydra` | Many-Headed | Beat the Hydra | fight 415 |
| `oc_boss_dead_head` | Head Count | Beat the Dead Head | fight 423 |
| `oc_boss_acacia` | Acacia's Finest | Beat Karsh, Zoah and Marcy together | fight 427 |
| `oc_boss_sun` | Eclipse | Beat the Sun of a Gun | fight 428 |
| `oc_boss_gloop` | Goo Gone | Beat the Giant Gloop | fight 429 |
| `oc_boss_taurusoid` | Bull Run | Beat the Taurusoid | fight 430 |
| `oc_boss_bunyip` | Double Trouble | Beat the Bunyips | fight 431 |
| `oc_boss_viper` | Fallen General | Beat General Viper | fight 432 |
| `oc_boss_radius` | Sparring Partner | Beat Radius | fight 434 |
| `oc_boss_garai` | Hollow Knight | Beat Garai | fight 438 |
| `oc_boss_highwayman` | Toll Road | Beat the Highwayman | fight 439 |
| `oc_boss_miguel` | Guardian of the Gate | Beat Miguel | fight 440 |
| `oc_boss_roachester` | Pest Control | Beat the Roachester | fight 441 |
| `oc_boss_cook` | Burnt Offering | Beat Hell's Cook | fight 442 |
| `oc_boss_grobyc` | Scrap Metal | Beat Grobyc | fight 445 |
| `oc_boss_sage` | Mind Over Matter | Beat the Sage | fight 447 |
| `oc_boss_fire_dragon` | Fire Scale | Beat the Fire Dragon | fight 426, 453 |
| `oc_boss_earth_dragon` | Earth Scale | Beat the Earth Dragon | fight 451 |
| `oc_boss_water_dragon` | Water Scale | Beat the Water Dragon | fight 452 |
| `oc_boss_green_dragon` | Green Scale | Beat the Green Dragon | fight 454 |
| `oc_boss_black_dragon` | Black Scale | Beat the Black Dragon | fight 455 |
| `oc_boss_sky_dragon` | Sky Scale | Beat the Sky Dragon | fight 457 |
| `oc_boss_dragons` | Dragon Slayer | Beat all six dragons | set: the six `oc_boss_*_dragon` |
| `oc_boss_dinos` | Fossil Record | Beat the Tyrano and the Pterodact | fight 476 |
| `oc_boss_vita` | Vital Signs | Beat Vita Unus, Vita Duo and Vita Tres | profile: fight 458, 459, 460 |
| `oc_boss_police` | Law Breaker | Beat the Polis Police | fight 461 |
| `oc_boss_jelly` | Sticky End | Beat the Royal Jelly | fight 463 |
| `oc_boss_luxator` | Dimmer Switch | Beat the Luxator | fight 465 |
| `oc_boss_gravitor` | Heavy Going | Beat the Gravitor | fight 466 |
| `oc_boss_aquator` | Low Tide | Beat the Aquator | fight 467 |
| `oc_boss_pyrotor` | Fire Drill | Beat the Pyrotor | fight 468 |
| `oc_boss_anemotor` | Wind Down | Beat the Anemotor | fight 469 |
| `oc_boss_terrator` | Rock Bottom | Beat the Terrator | fight 470 |
| `oc_boss_pillars` | Six Pillars | Beat all six elemental guardians | set: `oc_boss_luxator` .. `oc_boss_terrator` |
| `oc_boss_dragon_god` | Planet's Fury | Beat the Dragon God | fight 471 |
| `oc_boss_polly` | Parrot Fashion | Beat Polly | fight 483 |
| `oc_boss_fargo` | Captain's Duel | Beat Fargo | fight 484 |
| `oc_boss_dragoon` | Dress Code | Beat the Dragoon | fight 486 |

## 6. Recruits

Each row is a `recruit` rule on the character id; its description is `Recruit <name>`. Some recruits exclude others within one playthrough; since unlocks are kept per profile, those are earned over several.

| Id | Title | Character (id) |
|---|---|---|
| `oc_join_kid` | Thief's Honour | Kid (1) |
| `oc_join_guile` | Masked Magician | Guile (2) |
| `oc_join_norris` | Change of Heart | Norris (3) |
| `oc_join_nikki` | Encore | Nikki (4) |
| `oc_join_viper` | General's Return | Viper (5) |
| `oc_join_riddel` | Lady of the Manor | Riddel (6) |
| `oc_join_karsh` | Axe to Grind | Karsh (7) |
| `oc_join_zoah` | Strongman | Zoah (8) |
| `oc_join_marcy` | Little Terror | Marcy (9) |
| `oc_join_korcha` | Ferryman | Korcha (10) |
| `oc_join_luccia` | Lab Coat | Luccia (11) |
| `oc_join_poshul` | Good Dog | Poshul (12) |
| `oc_join_razzly` | Fairy Tale | Razzly (13) |
| `oc_join_zappa` | Old Hammer | Zappa (14) |
| `oc_join_orcha` | Head Chef | Orcha (15) |
| `oc_join_radius` | Village Elder | Radius (16) |
| `oc_join_fargo` | Captain's Share | Fargo (17) |
| `oc_join_macha` | Mother Hen | Macha (18) |
| `oc_join_glenn` | Twin Blades | Glenn (19) |
| `oc_join_leena` | Childhood Friend | Leena (20) |
| `oc_join_miki` | Stage Dancer | Miki (21) |
| `oc_join_harle` | Jester's Mask | Harle (22) |
| `oc_join_janice` | Beast Tamer | Janice (23) |
| `oc_join_draggy` | Hatchling | Draggy (24) |
| `oc_join_starky` | Close Encounter | Starky (25) |
| `oc_join_sprigg` | Shapeshifter | Sprigg (26) |
| `oc_join_mojo` | Doll's House | Mojo (27) |
| `oc_join_turnip` | Root Vegetable | Turnip (28) |
| `oc_join_neofio` | In Bloom | NeoFio (29) |
| `oc_join_greco` | Tag Team | Greco (30) |
| `oc_join_skelly` | Bare Bones | Skelly (31) |
| `oc_join_funguy` | Spore Sport | Funguy (32) |
| `oc_join_irenes` | Mermaid's Song | Irenes (33) |
| `oc_join_mel` | Pickpocket | Mel (34) |
| `oc_join_leah` | Cave Girl | Leah (35) |
| `oc_join_van` | Painter's Son | Van (36) |
| `oc_join_sneff` | Magic Show | Sneff (37) |
| `oc_join_steena` | Shrine Maiden | Steena (38) |
| `oc_join_doc` | House Call | Doc (39) |
| `oc_join_grobyc` | Hired Steel | Grobyc (40) |
| `oc_join_pierre` | Self-Styled Hero | Pierre (41) |
| `oc_join_orlha` | Barkeep | Orlha (42) |
| `oc_join_pip` | Strange Pet | Pip (43) |

| Id | Title | Description | Rule |
|---|---|---|---|
| `oc_join_10` | Company | Have 10 recruits in one save | recruits 10 |
| `oc_join_20` | Growing Crowd | Have 20 recruits in one save | recruits 20 |
| `oc_join_22` | Half the Cast | Have 22 recruits in one save | recruits 22 |
| `oc_join_30` | Full Table | Have 30 recruits in one save | recruits 30 |
| `oc_join_40` | Packed House | Have 40 recruits in one save | recruits 40 |
| `oc_join_pip_forms` | Light and Shadow | See Pip in every form, across any saves | profile: Pip's model byte 44..48 at a sample (five forms, at most two in one playthrough) |

## 7. Elements and techs

| Id | Title | Description | Rule |
|---|---|---|---|
| `oc_cast_frog_prince` | Royal Croak | Summon the Frog Prince | cast 37 |
| `oc_cast_blue_whale` | Whale Song | Summon the Blue Whale | cast 38 |
| `oc_cast_red_wolf` | Red Howl | Summon the Red Wolf | cast 39 |
| `oc_cast_salamander` | Fire Walker | Summon the Salamander | cast 40 |
| `oc_cast_sonja` | Green Spirit | Summon Sonja | cast 41 |
| `oc_cast_genie` | Bottle Opener | Summon the Genie | cast 42 |
| `oc_cast_golem` | Clay Giant | Summon the Golem | cast 43 |
| `oc_cast_thunda_snake` | Live Wire | Summon the Thunda Snake | cast 44 |
| `oc_cast_unicorn` | One Horn | Summon the Unicorn | cast 45 |
| `oc_cast_saints` | Halo | Summon the Saints | cast 46 |
| `oc_cast_mothership` | Abduction | Summon the Mothership | cast 47 |
| `oc_cast_grim_reaper` | Harvest | Summon the Grim Reaper | cast 48 |
| `oc_cast_spectrum` | Full Spectrum | Cast a summon of every colour | profile: one of each colour among 37..48 |
| `oc_cast_summoner` | Summoner | Cast all twelve summons | set: the twelve summon rows above |
| `oc_cast_level_eight` | Level Eight | Cast a level 8 element | event `level_eight` |
| `oc_tech_draggy_rider` | Saddle Up | Use Draggy Rider | cast 138 |
| `oc_tech_vital_force` | Second Wind | Use Vital Force | cast 139 |
| `oc_tech_tossed_salad` | Side Dish | Use Tossed Salad | cast 140 |
| `oc_tech_flamenco` | Dance Partners | Use Flamenco | cast 141 |
| `oc_tech_double_take` | Look Again | Use Double-take | cast 142 |
| `oc_tech_pitch_black` | Midnight | Use Pitch-black | cast 143 |
| `oc_tech_sword_storm` | Blade Rain | Use Sword Storm | cast 144 |
| `oc_tech_dragon_spike` | Spiked | Use Dragon Spike | cast 145 |
| `oc_tech_x_strike` | Crossed Swords | Use X-Strike | cast 159 |
| `oc_tech_all` | Three as One | Use every double and triple tech | profile: every element whose record names two or more casters (`formats/battle_data.md`, +0x14; element 0 is empty and skipped): 136..145 and 159 |
| `oc_cast_trap` | Caught Red-Handed | Catch an enemy's element in a trap | event `trap_catch` |
| `oc_cast_one_colour` | One Colour Sky | Turn the whole field effect a single colour | event `one_colour` |

## 8. Battle

| Id | Title | Description | Rule |
|---|---|---|---|
| `oc_fight_first` | First Blood | Win a battle | count `battles_won` 1 |
| `oc_fight_100` | Seasoned | Win 100 battles | count `battles_won` 100 |
| `oc_fight_300` | Veteran | Win 300 battles | count `battles_won` 300 |
| `oc_fight_1000` | Old Soldier | Win 1,000 battles | count `battles_won` 1000 |
| `oc_fight_steal_10` | Light Fingers | Steal 10 items | count `steals` 10 |
| `oc_fight_steal_50` | Master Thief | Steal 50 items | count `steals` 50 |
| `oc_fight_big_hit` | Heavy Hitter | Deal 9,999 damage in one hit | event `big_hit` |
| `oc_fight_last_standing` | Last One Standing | Win a boss fight with only one party member still up | event `last_standing` |

## 9. Items and endgame

| Id | Title | Description | Rule |
|---|---|---|---|
| `oc_item_forge` | Hammer Time | Forge your first item | count `forges` 1 |
| `oc_item_forge_10` | Smith's Regular | Forge 10 items | count `forges` 10 |
| `oc_item_forge_50` | Master Smith | Forge 50 items | count `forges` 50 |
| `oc_item_time_egg` | Egg of Ages | Get the Time Egg | item 0x435 |
| `oc_item_gold` | Nest Egg | Hold 100,000 G | save: gold 100,000 |
| `oc_item_gold_max` | Millionaire | Hold 1,000,000 G | save: gold 1,000,000 |
| `oc_end_ngplus` | Second Lap | Start a New Game+ | save: variable 0x7E = 2 |
| `oc_end_ngplus_done` | Lap of Honour | Finish a New Game+ | an ending awarded (`achievements.md` section 3) while variable 0x7E = 2 |
| `oc_end_all` | Every Ending | See all eleven endings, across any saves | profile: the eleven endings of `achievements.md` section 3, as that state machine awards them, whatever `[Game] Steam` |

## 10. Achievements page and popup

* **Page.** The OpenCross row of the main menu (`[Game] OpenCrossMenu`, `rules/menus.md` section 17.10) has an Achievements entry in its group list that opens the Achievements page: the unlocked count (`8 / 173`), a category filter (All, Story and places, Bosses, Recruits, Elements and techs, Battle, Items and endgame) on Square, and one row per achievement in table order with its title, bright once unlocked; the cursor's row shows its description and either the local date it was earned or, while locked, its progress: `count` rows show the counter (`212 / 300`), `profile` rows the members seen (`2 / 3`). The story rows from `oc_story_swap` to `oc_story_tower`, the bosses from `oc_boss_dragons` on, and `oc_end_*` are spoilers: locked, they show `???` for the title and description unless Show spoilers is on.
* **Settings.** Triangle on the page opens its Options page (`rules/menus.md` section 17.9): Achievements (`[Game] Achievements`), Hide notices (`[Game] HideAchievementNotices`), both stored in the ini at once, and Show spoilers (this session only, off at launch). With the OpenCross row off the page is not shown; the set is earned and noticed as the two keys say, and they are changed in the ini.
* **Popup.** Unless `[Game] HideAchievementNotices` is on (the player's ini reads a missing key as 1), an unlock shows a panel in the top-right corner of the game view for 240 ticks: `Achievement unlocked` and the title. A panel shows every unlock waiting when it opens: with more than one, the heading is `N achievements unlocked` and the first three titles are listed, then `and N more`; unlocks that come while a panel is up wait for the next panel. Loading a save that unlocks many at once thus shows one panel. No sound. It draws over the field and battle, and waits while a movie plays or the overlay is open.
* **Language.** Titles and descriptions are English in every language; the page's own labels are in the six languages.
