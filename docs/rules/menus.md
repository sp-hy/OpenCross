# Menu rules

Behaviour of the menus, shops, name entry, save and load screens, party editors and the pause screen: the rules, filters, formulas and limits the code enforces. Screen inventory and the flow machine: `architecture.md` section 7. Menu tables, the sell price and the forge fee: `formats/battle_data.md` sections 5.1, 11.3, 11.4. Save container, slot choice, New Game+: `formats/save.md`. Fonts, window skins, UI art: `formats/text.md`. Field script variables: `formats/fieldscript.md` section 10.8. Player walk / run and analog movement: `rules/field_actors.md` section 2. None of that is repeated here.

Code: `src/ui/menu/` (main menu, `GameState`), `src/ui/shop/`, `src/ui/title/`, `src/ui/party_screen.*`, `src/ui/pause_screen.*`, `src/game/game_flow.*`, `src/game/party_editor.h`, `src/app/app_sim.cpp`.

Conventions. "sfx N" is the menu sound id the code plays (1 cursor, 5 saved or sold, 6 cancel, 8 confirm, 0xC grid row change, 0x17 grid change, 0x18 refused, 0x1B traded, 0x48 equipment change, 0xBF forged or broken down). A refusal is only sfx 0x18 unless a message is named. Item stacks hold at most 99 (a further add succeeds silently without raising the count); the element stock has 192 slots, the gear and material stock 384. `party()` is the menu party: up to 3 valid ids in slot order, with a roster stand-in (section 1) replacing slot 0. Offsets such as "+0x12" are record byte offsets from `formats/battle_data.md`.

## 1. Main menu graph and input

Source: `main_menu.cpp`, `main_menu_main.cpp`.

Screens: Main (list, member pick, roster), Status, ElementChoice (Use / Allocate), ElementUse, ElementGrid, Equip, KeyItems, Settings, Save (Save / Load).

| List row | Opens | Exit cursor on return |
|---|---|---|
| 0 Status | member pick, then Status | 0 |
| 1 Elements | ElementChoice | 1 (from the grid: the Allocate pick) |
| 2 Equip. | member pick, then Equip | 2, on the member pick |
| 3 Items | KeyItems | 3 |
| 4 Customize | Settings | 4 |
| 5 Save/Load | Save (Load is always available; Save is gated inside, section 10) | 5 |

* **List.** Up / Down wrap over 6 rows (sfx 1). OK opens (sfx 8). Cancel closes the menu (the other exit is a confirmed Load, section 10): it stores the row in the cursor memory and sets closed (sfx 6).
* **Cursor memory.** On `open()` the list starts on the remembered row when options bit 0x01 is set, else on row 0. The remembered row is written only by Cancel on the list; the exit cursors above are not stored.
* **ElementChoice.** Two rows, Use and Allocate; Up / Left and Down / Right both toggle (wrap). Use opens ElementUse. Allocate returns to the member pick with the target set to ElementGrid; during that pick the Use / Allocate list (and its needle) stays on screen instead of the main list. Cancel returns to the list on row 1.
* **Member pick.** Shows the party (at most 3 portraits). The pick cursor starts on the current member clamped to the party size. Left / Up step back and Right / Down forward, wrapping. Left on the first member opens the roster instead of wrapping. OK opens the target screen with that member; Cancel returns to the list, from the Allocate pick to the Use / Allocate list on Allocate. Leaving Equip or the grid returns to the pick with the cursor on the member the screen showed last (`land_on_main`); leaving Status returns to the list.
* **Roster.** Lists all 44 characters (`save::kNumChars`), 17 rows visible (kRosterRows), scrolling one row at a time without wrap. It slides in and out over 20 ticks (kRosterSlide); input waits while it slides. Only available characters (recruited and not in the exclusion bitmap) show a row. OK on an unavailable row is refused (the confirm sfx 8 is requested first, then 0x18); OK on an available one opens the target screen with that character standing in for party slot 0 (`stand_in`). Right or Cancel slides it out and puts the pick cursor on member 0. On the Elements-grid and Equip picks, Triangle on an available row asks "proceed?" (Left / Right; the cursor is on Yes when the menu opens and keeps its last position afterwards): Yes empties the removable cells of that character's grid (sfx 0x17) or unequips all three accessories (sfx 0x48); No leaves it unchanged. Leaving a screen that was opened from the roster returns to the roster on the same row, not to the list.
* **Member switch.** L1 / R1 cycle through `party()` (wrap, sfx 1) on Status, Equip and the Elements grid. On Status and Equip a stand-in takes slot 0 of the cycle; on the grid L1 / R1 are ignored while a stand-in is active.
* **Input lockouts.** The first 4 ticks after `open()` ignore all input. Directions repeat after 350 ms every 70 ms (`DirectionRepeat`); a stick counts as a direction past 0.6, L2 / R2 past 0.5 of travel. The grid and the roster have their own locks (section 5). A message (`say`) clears after 150 updates, a save result after 240.
* **Screen transitions** (`MainMenu::Impl::Transition`, counts in 60 Hz ticks). The menu takes no input during one.
  * *Open*: every screen opened from the list or the Use / Allocate list (not ElementChoice itself, which switches at once). The screen fades linearly to black over 30 ticks (kOpenFade). One tick after the fade starts the member cards move right at 8.5 PS1 px per tick; on the way to Status, Equip or the grid the other portraits drop 16 rows per tick and the chosen one moves linearly to the first slot (x 16), arriving with the end of the fade. Items, Customize, Save/Load and Use move the cards only.
  * *Back*: a screen left for the list or the member pick fades linearly to black over 8 ticks (kBackFade) and stays black; there is no reverse motion.
  * Both last 32 ticks (kTransitionTicks); tick 33 cuts to the new screen at full brightness, with no fade-in.
* **Entering a screen** resets its cursors: Status shows the profile; ElementUse list and target at 0; the grid screen `grid_reset`; Equip slot 0 on the slot column; KeyItems 0; Settings row 0 with the skin picker closed; Save starts on Load when saving is blocked, else on Save.
* **What closes the menu.** Cancel on the list; a confirmed Load (section 10). After Cancel the host fades the field in over 0x10 ticks and a menu opened by a script sets script variable 0x4C = 1 when a file was written since `open()`, else 0; a Load does neither.
* **Opening.** The Menu key (pad North) opens it when the field is idle, no menu or battle is active and global flag bit 12 (menu key disallowed) is clear. The field fades to black over 10 frames, the screen stays black for 40 more frames, then the menu cuts in at full brightness (`GameFlow::begin_menu_open_fade`; the game stands still meanwhile). A script MainMenu request opens it without the fade. A script Save request (field op FE 85, the ending's save prompt; save points open the main menu) opens the save-list-only mode of section 10, always with saving allowed.

## 2. Settings screen and the options word

Source: `main_menu_settings.cpp`; options word and defaults: `game_state.h` (`opt::`), `game/new_game.h`.

The options word is the u16 at head bytes 0x5A0..0x5A1 (`formats/save.md` section 4). Up / Down wrap over 8 rows; Left clears the row's bit, Right sets it, OK toggles it (sfx 1 / 1 / 8); Cancel returns to the list on row 4.

| Row | Bit | Clear / set | Effect in code |
|---|---|---|---|
| Sound | 0x04 | Stereo / Monaural | `Mixer::set_mono` every frame |
| Window Frame | 0x08 | Default / Custom | dialogue and key-item list use the skin byte only when set, else skin 0 |
| Cursor Position | 0x01 | Default / Memorized | main list start row, battle command cursor, results cursor |
| Accuracy Selection | 0x02 | Manual / Auto | battle menu option |
| Controller | 0x100 | Default / Custom | toggles only (not modelled: nothing reads the bit) |
| [button] Pressed | 0x200 | Run / Walk | inverts the run button (`rules/field_actors.md` section 2.2) |
| Vibration Function | 0x400 | Off / On | rumble enable; also set by the title page and the pause screen |
| Left Stick Sensitivity | 0x800 | Digital / Analog | Analog makes a full stick tilt run (`rules/field_actors.md` section 2.2) |

* **Pad rows.** Rows 6 and 7 are greyed without a connected pad; Left / Right / OK on them play sfx 0x18 and change nothing. Rows 0..5 are always enabled.
* **Skin picker.** OK on Window Frame while it is Custom opens the picker instead of toggling. Left / Right step the skin byte 0..14 (wrap) to the next skin whose bit is set in the owned-frames mask (`slot.frames & 0x7FFF`); an empty mask makes the step a no-op. OK or Cancel closes the picker. The skin byte lives on `GameState::skin`, is not part of the slot image, and is set by the script (clamped 0..14).
* **Defaults.** A new game or New Game+ starts from the word in `formats/save.md` section 14.1, then sets bit 0x400 from the title vibration page (section 13). A loaded slot keeps its word.

## 3. Status screen

Source: `main_menu_main.cpp`, `game_state.cpp`.

OK toggles the profile panel and the equipment panel; Cancel returns to the list on row 0; L1 / R1 change member and return to the profile with the model camera reset.

* **Card values.** Name; HP as current / maximum, each shown as `min(value, 999)`; the current HP uses the warning colour when `cur_hp < max_hp / 8` (integer division). STR, ACC, MAG, RES, AGI, M.RES are the record fields `atk, hit, mag, def, evd, mdef`, each `min(value, 99)`. The record holds the character's own value: neither equipment nor a growth lookup is applied on this screen (the equip screen, section 4, adds gear). INNATE shows the colour disc of `innate(character)`: the record colour bit for characters 0 and 43, else the `char` record's innate byte (the dream record set while the dream state is active).
* **Stamina recovery** (Status and Elements-use panels). `min(rate + sum over accessory slots 2..4 of gear.mag where gear flag 0x80 (stamina) is set, 99)`; `rate` is byte +0x1F of the character record.
* **Ailment badges.** Four badges for record status bits 0x01, 0x10, 0x20, 0x08.
* **Grid minimap.** One tile per unlocked grid cell.
* **Profile text.** Table row keyed by the character's model byte when it is at most 0x31, else by the character id; at most 7 lines, trailing blank lines dropped. The 3D model is the menu model with the same key (model byte up to 0x32).
* **Pad camera.** Triangle toggles between the turning model and the pad camera; in pad mode Square steps the pose 0, 1, 2. The model logic ticks every second 60 Hz frame. Auto turn +8 per tick. Pad camera: Left / Right turn 16, Up / Down move 32 (distance clamped between the model's nearest distance and 0x960); with L2 held the directions pan by 8 (limit 0xAA); with R2 held Up / Down tilt by 16 (limit -0x200..0).

## 4. Equip screen

Source: `main_menu_equip.cpp`; rules `GameState::equip_candidates`, `can_equip`, `equip`, `stats_with`.

Slots 0 weapon, 1 armour, 2..4 accessories. The slot column (Up / Down wrap over 5): OK opens the candidate list, Cancel returns to the member pick (list row 2, cursor on the member shown), Triangle unequips all three accessories at once (sfx 0x48, no prompt). L1 / R1 change member and close the list. The list: Up / Down without wrap, 11 rows visible (kVisibleRows), Cancel back to the slot column.

* **Candidate filter.** The list holds, in bag stock order, every stack with quantity > 0 whose item kind (low nibble of the item-info category) equals `min(slot, 2)` (0 weapon, 1 armour, 2 accessory). An accessory slot that is occupied gets a "Remove" row first (always selectable); weapon and armour have none.
* **Selectable.** A row is selectable when the equippable-by mask has the character's bit (ids below 64) and, for accessories, `gear.slot_mask & (OR of the slot masks of the other two equipped accessories) == 0` (the accessory conflict mask). Other rows stay listed in grey; OK on one plays sfx 0x18. Quantity shows as `min(qty, 99)`.
* **Preview.** For the highlighted selectable row the screen shows the new values in a raised or lowered colour next to the current ones. The Remove row previews with item 0.
* **Stat formulas** (`stats_with`; `ch` = record, `o` / `d` = sums over the equipped accessories with gear flag 0x20 (offence) / 0x40 (defence) of gear bytes +4 / +5 / +6, `wp` / `ar` = the weapon / armour gear records):

| Stat | Formula | Display cap |
|---|---|---|
| HP | `max_hp` | (not shown) |
| ATK | `ch.atk + o4 + wp[4]` | 999 |
| M.ATT (mag) | `ch.mag + o5 + wp[5]` | 999 |
| HIT% | `(2 * (ch.hit + o6 / 3)) / 3 + wp[6] / 3` | 99 |
| DEF | `ch.def + d4 + ar[4]` | 999 |
| M.DEF | `ch.mdef + d5 + ar[5]` | 999 |
| EVD% | `ch.evd + d6 + ar[6]` | 99 |

  All divisions are integer. A slot with no item contributes 0.
* **Equipping.** `equip` re-checks `can_equip` (including a bag count > 0 for the item), takes one copy from the bag, returns the replaced item to the bag, and writes the id into the character's weapon, armour or accessory word. The record stats are not rewritten; gear is added at display time. Success plays sfx 0x48 and closes the list. A replaced item at 99 held goes back without raising the count (it is lost, as in the original). If the bag has neither a stack of the replaced item nor a free slot, the change is refused (sfx 0x18) and nothing moves; with fewer than 256 item ids in 384 slots this cannot happen in play.

## 5. Elements screen (grid editor)

Source: `main_menu_elements.cpp`; rules `game_state.cpp` (`can_place`, `can_remove`, `swap`, `auto_fill`, `sort_elements`), `MenuRules`.

**Geometry.** 8 rows (the element level, row r holds level r + 1) by 10 columns. A character has a cell where its record level mask (+0x30, one u16 per row) has the bit; cells are stored compactly in row-major order, at most 48. A cell is usable only when its bit is set in the character's unlocked mask for that row. L1 / R1 re-enter the screen with the next member (`grid_reset`).

**List.** Two label rows (0 and 1) followed by every occupied stock slot (id nonzero, quantity > 0) in stock order, 192 rows at most (so 190 elements). Rows 0 / 1 read Remove All / Remove; Right on the list (ignored while a cell is picked) shows the second pair, Auto Allocate / Fill Blanks; Right again moves to the grid; Left from the second pair returns to the first; Left from the first pair moves to the grid. 11 rows visible, Up / Down without wrap, Triangle opens Reorder.

**Focus model.** Focus is the list (0) or the grid (1). One pick is held at a time:
* An element or Remove picked on the list waits for a cell (`pick_element`). The cursor goes to the grid on the element's level row clamped into the character's unlocked rows (first open row .. deepest open row; sfx 0xC when the row changes); if the member may not hold the element, or no unlocked row lies within its reach, the pick is not taken (sfx 1 only). Reach is the low nibble of record byte +0x12. Remove and the row past the end of the list hold no element: they go to the grid on the current row and accept any cell.
* A cell picked on the grid first waits for a list row (`pick_cell`). On the list, OK with Remove or an element applies it to that cell (the label rows Remove All, Auto Allocate and Fill Blanks are refused); Left takes the picked cell back to the grid as a swap source (`swapping`); Cancel drops the pick.
* Swap: OK on a second cell swaps the two (`swap`); Cancel drops the swap and stays on the grid.
* Grid keys: Up / Down move along the row to the next unlocked cell (wrap); Left / Right change to the next lower / higher row that has an unlocked cell, wrapping. Wrapping past the bottom or top row moves to the list instead, unless an element pick or a swap is pending. A row change plays sfx 0xC. While an element is held (list pick, swap source or picked cell), a move onto a row outside its placement window is ignored. Cancel on the grid drops a swap, or a list pick (back to the list), else returns to the list. Cancel on the list returns to the main list on row 1 (to the roster if a stand-in is active).
* Applying (`apply_to_cell`, sfx 0x17 first): Remove empties the cell (on an empty cell it changes nothing and still hands the focus back); an element replaces the occupant, which goes back to the stock. An element that fails the window, eligibility or stock check plays sfx 0x18 and keeps the pick; a fixed occupant never gets this far, OK on a fixed cell is refused on the grid. When the element came from the list the focus returns to the list, when the cell came first it returns to the grid.

**Placement legality** (`can_place`, first failing check wins): no such cell; cell locked; occupant fixed; id is not an element (1..0x7F with a record) or the character may not hold it; outside the placement window; no copy in stock (the cell's own occupant does not count); the occupant has neither a stack nor a free slot to go back to (Full; unreachable with fewer than 128 element ids in 192 slots).
* **Order.** Placing first takes the new copies, then returns the occupant. Refilling a stack cell with its own element therefore leaves in the cell what the stock held (at most 5) and returns the old count; replacing an element with itself at 99 in stock stays at 99. A return to a stack at 99 is accepted without raising the count.
* **May hold.** The element's menu eligibility bit for the character (ids 0..47, `menu_pg` bytes 0..5; ids past the table are open to all), and for innate-only elements (record byte +3 bit 0x80) the element colour must equal the character's innate colour exactly.
* **Fixed.** Id >= 0x80 (techs and special cells), or element record byte +1 bit 0x02. Fixed cells cannot be picked, swapped, replaced or removed.
* **Placement window.** The element of level L fits row r (0-based) when `L - hi <= r + 1 <= L + lo`, `hi` and `lo` being the high and low nibble of record byte +0x12.
* **Stacks.** An element whose record byte +0 has `(b & 0x30) == 0x10` is a consumable stack: placing takes up to 5 copies (kStackMax) from the bag, as many as exist, and stores the count in the cell; removing or replacing returns the stored count (a cell holding 0 returns none). Other elements use one copy.
* **Swap rules.** Both cells exist, are unlocked and are different; neither holds a fixed element; each element must satisfy the window of the other cell's row. An empty cell may take part. Counts travel with the ids. (Picking the same cell twice is treated as success, no change.)
* **Remove** (`can_remove`) fails on an empty cell, a fixed cell (checked before locked), a locked cell, and (Full) when the element has neither a stack nor a free slot in the stock.

**List dimming.** A listed element is dimmed when the character may not hold it; when a cell is picked and its row is outside the element's window (not while swapping); or when the element's reach (level row +- low nibble) misses every row between the first and deepest unlocked row of the character.

**Prompts** (question in the description panel; Cancel dismisses, sfx 6):
* Remove All: Yes / No (Left / Right), default No; Yes removes every element with 0 < id < 0x80 from unlocked cells of the member (fixed ones stay), sfx 0x17.
* Auto Allocate / Fill Blanks: choice "this member" (default) or "whole party" (Up / Down). The whole party is refused (sfx 0x18) while a stand-in is active. Auto Allocate clears the member(s) first, Fill Blanks only fills empty cells.
* Reorder (Triangle on the list): three orders, cursor starts on the first (Up / Down wrap); the stock is repacked from slot 0 with quantities kept.

**Reorder orders** (stable sort by group, then key):

| Order | Groups |
|---|---|
| Innate first | any id: group 0 if the element colour has the member's innate colour (key id); group 2 if it has the opposite colour (key stock slot); everything else group 1, ordered by the exchange sort below |
| Attack first | ids 1..0x30 group 0 by descending id; the rest group 1 by id |
| Heal first | 0x80 group 0; 0x55..0x6B group 1 by id; 0x31..0x54 group 2 by id; 1..0x30 group 3 by descending id; 0x6C..0x7D group 4 by descending id; rest group 5 by id |

The opposite colour pairs are bits 0x04 <-> 0x40, 0x08 <-> 0x10, 0x20 <-> 0x80; the member's innate colour is the innate bit masked with 0xFC (a member with no single innate bit has no opposite group).

Innate first, group 1: the 192 stock slots, with the group 0 and group 2 entries and the empty slots set to 0, go through an exchange sort: for each position i in turn, every later position j (from i) is swapped into i when its id is lower than the id at i or the id at i is 0. A 0 is lower than any id, so a later hole displaces an entry and is displaced again by the next entry; the ids come out ascending only where no hole lies between them. Group 1 takes the nonzero ids in the resulting order.

**Auto-allocate algorithm.**
1. Buckets: stock slots (first 190 occupied) with ids below 0x80, bucketed by element level (bucket = level - 1), in stock order.
2. Two passes, exact level first (`fallback` false), then fallback; each pass walks rows 7 down to 0, and each row for every member in party order (one member for "this member"), so members share the stock in that order.
3. For each empty unlocked cell, left to right: sort the row's bucket by score, descending (exchange sort, an entry moves forward only for a strictly higher score), then take the first stock slot that passes: id nonzero, member may hold it, element fits the cell row's window, stock quantity >= 1 (>= 5 for a stack), and fewer than 2 copies of it already placed on that member. The fallback pass also searches the buckets of rows +1, -1, +2, -2 ... around the cell row.
4. Score of an element: empty slot -200; two or more copies already placed -100; otherwise the `menu_pg` bonus for 0 copies (byte 0x0C) or 1 copy (byte 0x0D) placed, then -10 if the element colour intersects the opposite colour, else +10 if it intersects the member's innate colour (members whose innate colour has no single opposite get neither).

**Timing locks** (menu frames during which grid input is ignored): after opening, Remove All, Auto Allocate, Fill Blanks and Reorder 27 (kLockOpen); list to grid 18 (kLockToGrid); cancelled cell pick 9; list-side fill 17; grid-side fill 15; swap 10; column move 9; grid to list 4; row change 40 (kLockRow: a 30-frame slide, 15 PS1 px per frame out then in); a row change started from the list adds the 3-frame hand-over (kHandOver).

## 6. Use element from the menu

Source: `main_menu_elements.cpp` (`update_use`), `game_state.cpp` (`use_element`).

* **List filter.** Stock slots with quantity > 0 and id > 0 whose `menu_pg` byte 0x0B has bit 0x80 (usable). Up / Down without wrap, 11 rows visible, Cancel returns to the main list on row 1.
* **Target roster.** OK on a row opens the target choice over `party()` (Up / Down wrap, at most 3); Cancel returns to the list. The target must be available (recruited and not excluded, section 1): any other target is refused (sfx 0x18) with nothing consumed.
* **Heal** (`formats/battle_data.md` 5.1): heals the listed amount, capped at max HP. Refused when `cur_hp >= max_hp`: sfx 0x18, nothing consumed. Heal size 3 heals 0 and still consumes a copy.
* **Cure:** the mask is the low byte of the status field of the element record (+0x18). Refused when `(status & mask) == 0`; otherwise those bits are cleared from the character's status byte.
* **Result.** On success one copy is consumed (sfx 8). When the last copy goes, the focus returns to the list with cursor and scroll reset; otherwise the target choice stays up. Nothing checks whether the target is at 0 HP.

## 7. Key items

Source: `main_menu_equip.cpp` (menu list), `keyitem_popup.*` and `game/field_world_party.cpp` (field list and use).

* **Menu list** (Items row). The owned ids in ascending order (ids 1..64 from the 64-bit key-item bitmap), 15 rows visible, Up / Down without wrap, Cancel returns to the main list on row 3. It is read-only: it shows the picture `menu/keyitem/NN` and the three description rows of the key-item table; nothing can be used here.
* **Description rows** (`key_item_line`). Row `r` (0..2) starts at PS1 (246, 334 + 24 r); a row may hold several lines split at `\n` (a `\r` is dropped), 24 rows apart. Text scale 0.95, shrunk in 0.04 steps (down to 0.4) until the line fits 228 PS1 px. `{ButtonFast}` / `{ButtonSlow}` (the Time Shifter row, `formats/text.md` section 6) stand for the key cap of the fast / slow speed action (the pad glyph with a pad): a 50 x 50 canvas-unit cap drawn 65 canvas units right of the token and 16 above the line's top, over the run of spaces that follows; the text after the token resumes 59 canvas units further on. Any other `{..}` is drawn as written.
* **Field list** (Item key / pad West, no menu, battle or script active, field idle). A separate panel of 64 slots packed in the display order of a 64-entry id table read from the executable (ascending order when the table is missing), 2 columns by 6 rows, scroll 0..26 row pairs. Input starts on the 15th active frame. Left at column 0 and Right at column 1 continue on the previous / next row; Up / Down scroll at the edges; no wrap. Names come from the key-item system-message table of the text language.
* **Refused rows.** An empty slot, or a disabled row: ids 1 (Tele-Porter), 61 (Relief Charm) and 63 (Smith Spirit) are enabled only while script variable 0x46 == 1 and variable 0x3C == 1. Refusal is sfx 0x18; Cancel (or the list button) closes with sfx 6.
* **Using an item** closes the list (sfx 8) and writes variable 0x2A (USED_KEY_ITEM) = 0x400 + id; the room script reacts. Ids other than 1, 61, 63 only set the "key item shown" flag. Id 1 requests the party screen, 61 the Relief Charm screen (argument 1), 63 the smith shop with the shop id taken from variable 0x4A. A request while another menu is open is dropped.

## 8. Shops

Source: `shop_logic.cpp` (rules), `shop_screen.cpp` (flow).

Script requests: ElementShop opens the element shop, TradingPost the trading post, WeaponShop the blacksmith (forge, break down, sell); the Smith Spirit key item opens the blacksmith too. The argument is the shop row id. Sounds: 1 cursor, 8 confirm, 6 cancel, 0x18 denied, 5 bought or sold, 0xBF forged or broken down, 0x1B traded. Gold gains from selling and breaking down are clamped to 9999999 (kMaxGold) without a refusal.

**Flow.** Top list (Element: Buy, Sell; Smith: Forge, Break down, Sell; Up / Down wrap; Cancel closes the shop). Smith goes through a category list (weapon, armour, accessory = item kind 0, 1, 2). Then the item list (wrap, 11 rows visible, Cancel one level up), then quantity (Buy, Sell element, Sell gear) or a Yes / No confirm (Forge, Break down; default No). An empty list on entering is refused (sfx 0x18). Quantity: Right / Left +-1, Up / Down +-10, clamped to 1..max without wrap; OK commits at once (no confirm). After a change the list is rebuilt and the flow falls back one level when the list is empty.

**Element shop** (price: `formats/battle_data.md` 5.1, clamped >= 0):
* Buy list: the shop row (`el_shop`, ids up to the first 0). The Cheats option "every element in shops" appends every other sellable element with price > 0, by id.
* Buy max = `min(99 - owned, gold / price)`. OK on a row is refused if the price exceeds gold or the max is below 1. Buying checks owned + qty <= 99 (Full), `price * qty <= gold` (NoGold) and a free stock slot (else rolled back, Full). Cost = `price * qty`. A price of 0 cannot be bought.
* Sellable: the `menu_pg` no-shop flag clear (5.1), id > 0, not fixed. Sell list: bag elements in slot order, then elements that are only placed in some roster member's removable grid cells.
* Sell max = bag copies + removable placed copies over the whole roster. Selling takes bag copies first, then empties placed cells (a stack cell returns its copies to the bag first) and takes from the bag. Proceeds are `(price * qty) >> 2`, computed on the total, not per unit.

**Blacksmith.**
* Forge list: the shop's `waa_shop` row filtered to the category; the Cheats option widens it to every craftable, non-dummy (price > 0) piece of that category.
* Recipe: the nonzero counts for materials 1..31 in the forge table. Forge needs every count in the bag (else "short of materials", one message per category) and gold >= cost (else "not enough gold"); no recipe is silent. Cost: `formats/battle_data.md` 11.3. There is no check of the count held: it takes the materials, adds the piece, then deducts gold, so at 99 held the materials and gold are spent and the count stays 99. If the bag has neither a stack of the piece nor a free slot the forge is refused (Full, sfx 0x18) before anything is taken; with fewer than 256 item ids in 384 slots this cannot happen in play. The Cheats option "no crafting cost" skips the material and gold checks and takes nothing.
* Break down list: bag or worn gear of the category without item-info bit 0x20 that has a recipe. Refused (message) when only worn copies exist (bag count 0). It removes one bag copy and returns each recipe material one unit at a time; a unit that no longer fits (the material at 99 or no free slot) is paid as gold (11.3), summed per material as `price * over / 4` in integer arithmetic and shown in a second message line when the total is above 0.
* Sell list: bag gear of the category without item-info bit 0x80. Max = bag count (worn copies are never sold). Unit price: 11.4; proceeds = unit * qty. Materials (kind 4) are refused.

**Trading post** (own three-stage loop over the shop's `trade` row; `battle_data.md` section 8 for the row layout).
* Stage 0: greeting; OK enters the goods list, Cancel closes.
* Stage 1: the goods list: the row's entries up to the first goods id 0; an entry with level sum 0 is a hole (no goods, not counted). Up clamps at 0, Down at count - 1, no wrap. Names are prepared one row per frame and input waits until the list is ready. OK enters the offer, Cancel returns to the greeting.
* Offer list: every element stock slot with a nonzero id and without the `menu_pg` no-shop flag (192 rows at most, at least 11 rows), cursor row 0..10, scrolling by one; Down never moves onto an empty row. Rows the entry's colour mask refuses are dimmed. Cancel returns to the goods list (sfx 6) and the offer is cleared on the next visit.
* A press of OK (auto-repeating while held; sfx 8 on every press and repeat) adds one copy of the row's element to the offer when the entry's colour mask is 0 or shares a bit with the element's colour, the offered level sum is still below the entry's level sum, and the offered count differs from the stock count of that element. Level sum = the sum of element level * offered copies.
* When the sum reaches the required level sum the "Make the exchange?" question opens (default Yes; Up / Down toggle). OK is accepted only more than 30 frames after the question opened. Yes takes every offered copy from the stock and adds one unit of the goods item (sfx 0x1B); No (sfx 8) and Cancel (sfx 6) discard the offer. A single copy may overshoot the required sum. The offer list is not rebuilt after a trade.
* The goods are added with the normal stack add: at 99 held the elements are still taken and the goods are lost. With neither a stack of the goods nor a free slot (unreachable in play) Yes is refused with sfx 0x18 and nothing is taken.
* The goods cursor indexes the row's entries, holes included, while Down clamps at the count of entries without holes: a hole in the middle would make the last entries unreachable and let the cursor rest on a hole (goods 0). No row of the game's `trade` table has a hole before its end.
* **Layout check (JP, goods list, 512 x 432 shop units).** Against the PS1 screen the list panel (20..251 x, bottom 357..359), its header rule (y 64..65), the info panel (315..497 x, y 70..195), the "in stock" row, the colour box, the cursor arrow and the clock / gold block agree within 1 unit; the list panel's top rule sits 2 units lower than the PS1's (RDE shape art). The item description (style 0x8F) starts 9 units higher than the PS1's (y 85 against 94); that offset is RDE's per-language baseline of the style: RDE's Japanese layout puts the description's first line within 1 canvas pixel of ours.

## 9. Name entry

Source: `ui/title/name_entry.cpp`, `name_codec.cpp`.

* **Keyboard.** Non-JP languages: a 10 x 14 Latin grid (upper and lower case, accented letters, `ß`, `#`, digits, `+-*@<>()[]:;,.!?` and inverted marks); a blank cell inserts a space. JP: three 10 x 10 pages (katakana, hiragana, alphanumeric) selected by buttons; blank is U+3000. Nothing else can be typed (no text input from the keyboard).
* **Length.** `max_name_length`: 12 on the Latin keyboard; 6 in JP, 4 for Viper (character id 5). The buffer is always that many cells, blank padded.
* **Navigation.** Up / Down / Left / Right move without wrap inside the grid; leaving the grid at an edge goes to the button column, Left from the buttons returns to the last grid column. The button column wraps. The Latin layout starts in the grid on cell (0, 0); the JP layout starts on the button column at the active page. L1 / R1 move the text cursor.
* **Entering.** OK in the grid writes the cell at the cursor and advances unless on the last cell (which is overwritten repeatedly). Cancel is Backspace: it blanks the cell at the cursor, then moves left. There is no way to leave the screen without a valid name. Pad Start / Space accepts the name from anywhere, as the Confirm button.
* **Buttons.** Latin: Change Name (back to the grid), Confirm, Default. JP: the three page buttons, Confirm, Default. Default restores the character's default name for the language (system-message table), padded to the length.
* **Rejection.** Trailing U+0020 and U+3000 are trimmed. The name is rejected, with a 0.45 s flash and sfx 0x18 and the screen stays, when it is empty or equals クロノ, くろの, "chrono" or "crono" (Latin letters case-insensitive). The reserved list applies to every character. Only trailing blanks are trimmed: leading blanks stay and count as part of the name.
* **Initial text.** The existing custom name of the character if there is one (rename), else the default name.
* **Storage.** The typed name is kept whole (UTF-32 in the flow's custom names, saved to `ext_save.dat` / the page file). The save record gets only what the game font can encode, at most 6 symbols (`pack_record`); characters the font lacks are skipped. On load, an `ext_save.dat` name replaces the record's when the languages match. A name counts as unset when empty, all blanks, containing an undecodable code, or (outside JP) containing a code point at or above U+3000.

## 10. Save and load screens

Source: `main_menu_save.cpp`; title side `ui/title/slot_list.*`.

**In-game flow** (`save_phase`):

| Phase | Screen |
|---|---|
| 0 | Save / Load choice. Up / Down toggle only when saving is allowed; OK on Save when blocked is refused. Cancel returns to the main list on row 5. |
| 2 | Target choice: OpenCross, Slot 1, Slot 2 (Up / Down wrap). Starts on OpenCross when the page holds a file, else on Slot 1. Cancel returns to phase 0 (save-list-only mode: closes the menu). |
| 3 | "No save data" (Load on an empty target); confirm or 120 frames returns to phase 0. |
| 1 | The file grid or list. Cancel returns to phase 0 (save-list-only mode: to phase 2). |

* **Save-list-only mode** (`MainMenu::open_save_list`, the script Save request). The menu opens on the Save screen in phase 2 in Save mode; the Save / Load list, its dial and the description frame are not drawn and there is no phase 0. Everything else (targets, grid, page, overwrite confirm, `saved`) is as in the normal flow.
* **Save allowed.** The menu opens with saving allowed when script variable 0x3C == 1, and always in the save-list-only mode. Having used the debug tools only shows a warning line (a toast for the script request); it never blocks a save.
* **Card grid.** 15 boxes (8 + 7), one per file in the card's directory order (not by file number); each box shows its file's number, whatever that number is (the game itself only writes 00..14). Left / Right step with wrap (Save on card 1 wraps within boxes 1..14); Up / Down jump 8 boxes (boxes 7 and 8 stay). Box 0 of card 1 is the auto-save: loadable, never a manual save target. Load starts on the first used box, Save on box 1 of card 1 (box 0 on card 2).
* **Page list.** 101 rows (0 = the page's auto-save, 1..100), 10 visible (kPageRows). Up / Down wrap one row, Left / Right move 10 rows without wrap. Row 0 is not a save target. Load starts on the first used row, Save on row 1.
* **Overwrite confirm.** OK on a used box or row opens Yes / No with the cursor on No (`confirm_cur` 1); any direction toggles; Cancel dismisses. The same prompt guards a load. OK on an empty box saves at once; OK on an empty box in Load is refused (sfx 0x18).
* **Where a save goes.** A used box overwrites its own file. An empty box creates a new file with the lowest unused number from 0..14 (0 is reserved on card 1). With no free number nothing is written (sfx 0x18 only). The card save also writes the `ext_save.dat` record (language, battle counter, custom names; `formats/save.md` section 9) and rewrites the whole `save.dat`. A page save writes the numbered file with the same extras. A full card or write failure shows the failure message and plays sfx 0x18; success plays sfx 5 and shows the saved message. The result sets `saved`, which scripted menus return as variable 0x4C = 1.
* **Load.** A confirmed load sets the load request and closes the menu; the host takes the request (card, slot, block, data) and starts the slot like Continue (fade 0.5 s, then section 13 flow).
* **Location caption.** Keyed by script variable 0 (head byte 0x40, u16): the key is the number of the 30 thresholds `{0, 8, 16, 24, 26, 28, 30, 36, 52, 64, 72, 80, 88, 92, 100, 108, 112, 116, 124, 128, 140, 144, 156, 164, 172, 184, 195, 202, 209, 999}` that the value has reached, at least 1; the caption has two lines.
* **Corrupted data.** The card read path decodes the block without testing the checksum or the save version (not modelled; `formats/save.md` section 3). A block that fails structurally (wrong size, packed stream overrun) is left out of the grid with a warning; a failing file on the title list is likewise skipped. Page files that fail validation are skipped at boot (`formats/save.md` section 13). Directory entries are accepted with a Japanese or US game name and a two-digit number.
* **Title Continue lists.** The choice (OpenCross / Slot 1 / Slot 2) and the page list behave as above. The card grid holds one box per readable file in directory order with no number limit and no auto-save rule; Left / Right wrap over the files, Up / Down jump 8 boxes when that box exists. A file is picked, not confirmed: OK starts the load (fade), Cancel on the grid or page list returns to the choice, Cancel on the choice leaves to the title with the cursor on Continue. An empty target shows "No save data" (120 frames or confirm). Continue+ is section 13.

## 11. Party editors

Source: `game/party_editor.h`, `ui/party_screen.cpp`; flow fallback `game_flow.cpp`.

**Grid** (shared): the recruited characters that are not in the exclusion bitmap (words 2..3 of the lock block, which holds two lock words then two exclusion words), ascending ids, at most 44, 4 columns, 5 visible rows, scroll 0..6 rows. Left / Right wrap over the columns, Up / Down move the row and scroll at the visible edge, sfx 1 on every press including the ends. A scroll takes 17 frames of 2 px; input is ignored meanwhile.

**Party editor** (script party screen / Tele-Porter, 3 slots). Members are packed on entry (empties and duplicates dropped). A member that is not in the grid (excluded) keeps its slot.
* Slot mode: Left / Right wrap over 3 slots. OK on an empty slot, or on a slot whose member is not locked, enters pick mode (sfx 8); a locked member is refused (0x18). Cancel leaves (sfx 6) unless a slot is empty while the number of filled slots differs from the grid size (sfx 0x18).
* Pick mode: OK on a character who is not in the party puts it into the slot (it replaces the slot's member); on a character of another slot swaps the two and moves the slot cursor to that other slot (both sfx 8, the grid stays open; the swap does not test the lock bit, so a locked member can change slot but never leaves the party); on the slot's own character or an empty cell is refused. Cancel returns to slot mode.
* Lock bitmap: bit c of the two lock words means character c's slot cannot be opened for a replacement; the party-screen path passes it, the flow fallback (used when the party screen is unavailable) passes none and lists every recruited character, excluded ones included.
* Leaving compacts the party (empties dropped, order kept). The host writes it to the state (0xFF for empty) and rebinds the party entities and models; the party screen first runs a 0x40-frame closing fade, the fallback closes at once.

**Relief editor** (Relief Charm). Grid only. OK on a character other than the current relief member sets it (sfx 8, the screen stays and the cursor stays); on the current relief member or an empty cell sfx 0x18; Cancel always leaves (sfx 6). The relief member cannot be cleared here. The stored member is head byte 0x59F; on entry it is cleared when it is 0x2C or above or not recruited. The cursor starts on cell 0 and the shown character starts as the relief member, else character 0.

**Grand Slam monster picker** (field FE 75 #round + FE 61, zel_a17i). Source: `ui/monster_pick.*` (rules), `ui/monster_pick_screen.*` (screen), `ui/menu/picker_model.*` (creature models); opening and closing: `app/app_sim.cpp`.
* **List.** The Dopplegang pool slots whose bits are set in variables 0x3E0..0x3FE (word w bit n = slot w * 16 + n), ascending, at most 130 (checked after each word, so the last word may add up to two more); 3 per row, 42 rows, 6 visible, 13 px per row. A pool slot is also the monster record and the name index (system text category 0x19).
* **Round table.** 9 list positions (3 per round, 0xFF none), kept for the session. Opening round r clears its entries from r * 3 on, so earlier rounds' creatures stay taken. The three picks start on the first three free list positions (searched among positions 0..128). A position is taken while it is in the round table or one of the current picks; taken names are red, the cursor's name orange, the rest white.
* **Slot mode.** Left / Right wrap over the three picks (sfx 1); confirm opens the list (sfx 8); cancel ends the picker (sfx 8): the picks go into the round table and their pool slots become the battle's party (`rules/battle.md`, monster-party scenes).
* **List mode.** One 60 Hz frame runs two input passes (Left / Right only in the first). Left / Right move over the columns, wrapping column 0 <-> 2 into the previous / next row; Up / Down move the row and at the visible edge scroll the list one row in 13 steps of 1 px, two per frame, ignoring input meanwhile. After a scroll up the step counter rests at -1 (the list 1 px lower) until the next scroll down. Confirm on a free entry makes it the slot's pick (the old pick is free again, sfx 8); on a taken or empty entry sfx 0x18. Cancel returns to slot mode (sfx 8).
* **Layout (320 x 240).** The three picks' creature models sit in the pick boxes (drawn at the table scale; a scale of 0 draws nothing). Pick names start at x 64 + 96 * slot - 6 * n + nudge, y 100, where n is the Japanese name's glyph count (at most 8); the list window (16, 128, 288 x 78) clips its rows, names at x 28 + 96 * column, y 128 + 13 * row + 2. The cursor (5 frames of 8 frames each, 8 x 8 px, 4 px below its point) sits at x 14 + 96 * slot, y 98 in slot mode and at x 16 + 96 * column, y 128 + 13 * row - 2 in the list.
* **Text (RDE, both art modes).** Names are left-aligned in the PC font: the JP font slot (kb) in Japanese, the menu font slot (bn) otherwise, at the slot's scale; colours white 0x757575, cursor orange 0x285085, taken red 0x000075. Outside Japanese a pick name moves by a nudge chosen by its pool slot (-12: 0x02 0x12 0x18 0x1A 0x2F 0x35 0x36 0x38 0x41 0x6C 0x7C 0x8E 0xB2 0xD7 0xD8 0xD9; +6: 0x03 0x20 0x27 0x31 0x4D 0x56 0x5C 0x5E 0x70 0x8F; -6: 0x0A 0x0B 0x0E 0x13 0x14 0x1F 0x29 0x2A 0x3D 0x47 0x5D 0x78 0x79 0x88 0x99 0xAC 0xB5; -18: 0x0D 0x17 0x24 0x46 0x51 0x71 0x89 0x8A 0x8D 0x95 0xA4 0xA5 0xA6 0xB7 0xD6; -24: 0x7E), and a few long names (per language, by pool slot) are drawn at a smaller scale and up to 2 px lower (`kStylesEn..Es`, `kStyleScale` in `monster_pick_screen.cpp`).
* **Backdrop.** The room's background group 1 (the picker's frames) at scroll (0, 0) with group 0 hidden, over the field system texture pack's fifth image tiled 64 x 64 across 640 x 220; closing restores group 0 and the scroll. HD draws the cursor from `hd/menu/monmenu_cur.png`.

## 12. Pause screen

Source: `ui/pause_screen.*`; opening and closing: `app/app_sim.cpp`.

* **Rows.** PAUSE bar; "Vibration Function: On / Off" always; "World Names: On / Off" only when the row condition holds.
* **World Names condition.** The field world is up, script variable 0 (scenario) >= 0x34 and variable 0x50 (map has a world name) is nonzero. The value starts from variable 0x52 != 0 and is written back to variable 0x52 (1 / 0) when the screen closes, only if the row was shown.
* **Input.** Left / Right (or OK) flip Vibration (sfx 1); Up / Down flip World Names when its row is shown (sfx 1). Start (Space / pad Start), Cancel or pad East close it (sfx 6). Vibration is written to options bit 0x400 every frame; switching it on gives a test rumble pulse.
* **Opening.** In the field: Start when `FieldWorld::pause_allowed()` and the menu is closed. Over a running battle: Start at any time except while the battle transition is active. Over a movie: Start. Never over the title scene (logo, attract movies) or the title's own screens (menu, slot list, name entry). The keyboard's Space opens it on the press; the pad's Start opens it on release of a press shorter than 500 ms, a longer press opens the settings window instead.
* **Where pause is refused** (`pause_allowed`): during room init, a room transition or a jump; when the room's pause flag is clear (global flag kPauseAllowed, set at every room load and cleared by script) or the low byte of variable 0x36 is nonzero; on maps 8 and 9; while a menu, movie or battle hand-off is in progress. Open windows, choices, running cutscenes and the input lock do not refuse it.
* **What it freezes.** The pause screen takes all input: the field, battle, results and movie simulation stand still. The play-time counter is advanced before the pause check.
* **Tint.** A counter 0..479 advances by wall time at 60 per second and is kept across openings. `ramp = counter > 0xEF ? 480 - counter : counter`; `t = ramp / 240 * 0.53`; the picture is multiplied by `(1 - t, 0.47, 0.47 + t)`.

## 13. Title screen flow

Source: `game/game_flow.*`, `ui/title/title_screen.cpp`, `nav.h`.

* **Phases.** Fade in, "Press any button" prompt, menu, fade out (0.5 s each). With the title room behind (scene mode) the screen starts at the prompt. Any input (Confirm, Cancel, shoulder buttons, any key except Esc and F1..F12, any pad button except the stick clicks) leaves the prompt through black into the menu. Navigation repeats after 0.4 s every 0.08 s; the left stick past half way counts as the d-pad.
* **Press.** The press plays the chime: global sounds 59, 61 and 62 started together, each on its own field sound slot (0, 1, 2; full volume, centre pan), so none cuts another. The picture holds 22 frames, then a subtractive fade (the picture minus a rising level) reaches black over 32 frames. The music fades out: the fade starts 14 frames after the press, takes 20 frames to reach silence and the song is stopped when it ends. This is the title room's song in scene mode and the title's own song otherwise.
* **Rows.** New Game, Continue, Settings, Quit (Up / Down wrap). Settings reports at once and opens the options window. Quit fades and quits.
* **Continue enabled rule.** The row is enabled when the OpenCross page holds a file or either card holds a readable slot (`SlotList::any`, re-evaluated each time the title is entered). Disabled: sfx 0x18.
* **Back to the prompt.** Cancel on the menu (sfx 6) fades to black and returns to the prompt, which fades in again. The music starts again from its beginning: in scene mode the title room's song (the scene keeps running), otherwise the title's own song. The original's menu has no way back to the prompt; OpenCross keeps one.
* **Vibration page.** New Game opens a page with On and Off (cursor on On; Up / Down toggle). OK stores the choice, reports New Game and fades out; Cancel returns to the row list on row 0. The choice becomes options bit 0x400 of the new game state; Continue never touches the options word.
* **New Game.** After the fade: the user guide (below), then name entry for character 0 (Serge) with the language's default name. A validated name starts a fresh game (Serge, Kid and a random third member from a 14-entry table; gold and stars per `new_game.h`; location room 22), plays movie row 2 and loads `opn_a01i` with room param 0.
* **User guide.** Two pages (controller, then keyboard) of baked images. Shown at every New Game, and on a script name entry (opcode FE 4A) when script variable 6 (MAP_ID) is 0 or 8. Skipped when the page images are missing. A 45-step counter at 60 steps per second runs the fade-in of page 1, a wait, the fade-in of page 2 over it, a wait and the fade-out of page 2 (15 steps each). Each wait ends on any key press except F1..F12, or on the frame the pad goes from some button or stick held to none (stick clicks and Start excluded).
* **Continue.** Slot list (section 10); a pick fades the screen out for 0.5 s and then restores the state: the state is replaced by the slot, the Time Shifter key item is granted, record names that are usable and `ext_save.dat` names (same language only) become the custom names, the location word selects the room (a word of 0 with no room name logs a warning and loads no room; a nonzero id is looked up in the disc index and returns to the title when no room is found), the saved leader position (script variables 0x12, 0x14) is restored and the room parameter is variable 0x3A. Cancel on the choice returns to the title menu with the cursor on Continue.
* **New Game+ variant.** The ending room's script (FE 84 + FE 61) shows variant 2 over the cleared game: rows "New Game +" and "Continue +" only, straight into the menu with no prompt and no cancel. New Game+ goes through the vibration page, then replaces the state in place (carry-over per `formats/save.md` section 14) and the ending script goes on. Continue+ opens the slot list in plus mode: a slot whose two summary flag bits are set (bit 0 completed, bit 1 already New Game+) is refused with sfx 0x18; a pick carries the cleared game into the loaded slot after the fade; the custom names stay those of the cleared game. Cancel on the Continue+ choice returns to the variant 2 list on Continue +.
