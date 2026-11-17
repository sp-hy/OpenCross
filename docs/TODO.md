# Open issues and known bugs

The working list for the engine: behaviour bugs found by reading the code, fidelity gaps against the original game, open comparisons that need a capture, and code hygiene. Each entry names the file to look at. Remove an entry in the same change that fixes it; keep the sections. This file is temporary working notes, so unlike the other docs it may carry RAM addresses; they leave with their entry, and anything worth keeping moves to `docs/formats` as a fact without the address and to `tools/ghidra/symbols/` as a symbol.

Entries that need a PS1 comparison use `python -X utf8 tools\py\ps1_cast.py force` (`docs/validation.md`, section 3.4) for the reference and `tools\ours_cast34.ps1` for the OpenCross side, which runs `tools\py\fx_diff.py`. Reading a diff: a sequence name that only one side has is not a bug until these are ruled out: a random camera pool or selector op, a different target count (a debug cast hits every enemy of an all-targets element, a forced PS1 entry hits one), our run ending before the sequence starts (raise `-Frames`), the capture holding two casts (`--cast-index`, `--window`), the scene's own opening action running first on our side, and `-Dt` pacing, which caps per-tick emitter fires. Unique techs are cast by their owner: `0xC000 | element` in a grid cell of any `char*.bin` set, or pgdat bytes +0x14..+0x16 for a double or triple tech.

## Start here (P1)

Player-visible, high-reach entries; each is described in full in its section below.

None open.

Priority: P1 = player-visible with wide reach, P2 = visible or behaviour-changing, P3 = small, rare or invisible. Entries with "Decision (owner)" carry the owner's ruling on RDE versus OpenCross behaviour; follow it.

## 1. Bugs (behaviour)


## 2. Fidelity gaps (not modelled)


Disc mode (playing from the player's own PS1 disc images or a CD drive when no RDE install is found; `docs/architecture.md` 3.7, `docs/formats/disc.md`). Run it with an ini holding `[Paths] Source=disc`, `Disc1=` / `Disc2=` the `.cue` files (the owner's JP and USA images are in `Documents\DuckStation\games\`), `SaveDir` under `tmp\` and `AutoSave=0`; RDE itself must stay unchanged (baseline routes, `pac-soak`). Both the Japanese (SLPS) and the USA (SLUS) builds are required. In disc mode every RDE-only file (`hd/`, `ui/`, `font/`, `lang/`, `movie/`, `cdrom2/`) is absent, so each entry below replaces an RDE asset or table with the disc's own.

* **P2 Disc mode: battle screen leftovers.** The disc-mode battle HUD is drawn from `battle/battle.prd` the PS1 way (`src/import/battle_hud.*`, `src/game/battle/battle_view_ps1hud.cpp`; `docs/rules/battle.md` 6.9), checked against PS1 captures of the top menu, target, HIT% box and post-hit screens on both builds. Left: (a) the script window's page cursor has no disc art yet (probably the `mark` sheet); (b) USA enemy letter suffixes show as "-A", the PS1 shows " A"; (c) the USA name glyph "r" differs slightly from the PS1's; (d) element list, Dopplegang list, status plates and damage digits are drawn but not compared with a PS1 capture; (e) the USA battle menu differs (after a weak hit the HIT% box stays open with Element as row 3; JP returns to the command menu): check disc mode follows it. Reference: `ps1_capture.py scene` (`--region usa`). Done when each is fixed and matches a PS1 crop.
* **P1 Disc mode: battle results art.** `src/game/battle/battle_results.cpp:226-248` loads `hd/battle/shapes/btlexit-levu-*`, `hd/battle/btend/up.png` / `star.png`, `battle-gg-*`, `battle_11`, `battle_08_*`, `battle_37.tim`, `hd/menu/menutim/EquipIcon.png`, `hd/battle/face/*/pNN.png` in both modes. Replace with the PS1 sources (`battle/btend.prd`, `menu/playtim/pNN.tim`, ...) per a PS1 capture of the results screen on both builds.
* **P1 Disc mode: menu frames and cursors.** Classic menus take their panel size from `hd/menu/shapes/menu-main-NNN.png` (`src/ui/menu/menu_common.h:86`, `main_menu_draw.cpp:159`, `shop_screen.cpp:621,1171`, `title/slot_list.cpp:281-292`, `title/name_entry.cpp:250,299`); without the PNG `Frame()` draws nothing. Take the extent from the `menutest.prd` sheet frame's part bounds instead. Also replace `hd/menu/menutest_cur.png`, `handle.png`, `eleempty.png`, `menuelem/menuelem_*.png`, `smithy.png`, `shapes/memmenu[2].png` + `memmenu[2]_cur.png` (party screen), `monmenu_cur.png`, `menutim/menutim_02.png` (probably a `menutim.prd` entry) with PS1 art. `ui/keyboard_*` / `ui/gamepad.png` button glyphs in dialogue (code 1B, `ui/dialogue_box.cpp:261,268`) become `menutest.prd` "main" frames 0x7C+. Done when the main menu, shops, name entry, party and monster-pick screens draw frames and cursors in disc mode and match PS1 captures.
* **P2 Disc mode: PS1 layout of text on menus and shops.** The PS1 face is in (`src/import/ps1_font.*`, `TextBatch::add_ps1_font`; `docs/formats/text.md` 7.14); the dialogue box and the battle screen use PS1 metrics and positions, but menus and shops keep RDE positions and window sizes. USA menus use the 4-bit `usfo` sheet (disc record 234, `cdrom/#record/00234`; rects from the `usfo` frames of the USA `menutest.prd`; `text.md` 11.2), not yet decoded or drawn. Text colours 2-6 still use RDE's table. Spritefonts are still read directly in `ui/menu/main_menu.cpp:65`, `app/app_boot.cpp:52`, `ui/pause_screen.cpp:42` (warnings only in disc mode). Done when each screen's text sits where the PS1 puts it (frame diff) on both builds.
* **P2 Disc mode: USA text rules not applied.** `src/import/ps1_text.*` decodes USA text, but Zoah's all-capitals window and the 6-glyph name cut before a `{-y}` / `{-i}` suffix (`text.md` 11) are not done; byte `FF` has no USA glyph (a few records). Menu keys marked "contained" in `tools/py/data/usa_menu_strings.tsv` return the whole line. JP grammar rows are read at character id (RDE's `mesgen.csv` rule; the PS1 may read id + 1): check on the PS1. Where each USA menu message is placed is not traced.
* **P2 Disc mode: remaining RDE-only art.** `src/ui/feature_icons.cpp:36` (`ui/exticon*.png`) and `src/ui/pause_screen.cpp:34,42` (`ui/pause.png`, `ui/ui_font_gnb.spritefont`): owner decision, keep the features and draw their indicators as short PS1-font text. `src/ui/menu/status_model.cpp:114` reads `hd/map/etc/tile.tim` (use the `systim.bin` parchment); `src/ui/shop/shop_screen.cpp:184` loads `hd/window/w01.png` unconditionally. Title labels should come from the disc's `openfont.tim` / `cmptitle.tim` (no `cdrom2` in disc mode): check on screen. JP name entry has no default name in disc mode (New Game needs one typed character).
* **P3 Disc mode: search fills a missing disc from a later place.** `src/import/disc/disc_search.*` stops at the first place holding any Chrono Cross disc, so disc 2 in the exe folder is not found when `Disc1=` names only disc 1; the insert-disc screen asks instead. Merge candidates across places by build and disc number.
* **P3 Disc mode: movie streams are read whole** (`src/movie/decoder.*`, about 92 MB for `ending_s`). Stream them from the VFS for optical drives.



## 3. Open comparisons (need a capture against the PS1 or RDE)

* **P3 Disc mode: unverified PS1 details.** (a) Row 12 of the movie table (short staff roll) uses strip row B scrolled one row per frame (`docs/formats/movies.md` 10): inferred from pacing, not seen on the PS1. (b) The PS1 stops the JP `newop` at the row's end frame 2280 (OpenCross does, hiding 420 black frames): unverified. (c) Whether music keeps playing during the insert-disc screen (`src/ui/disc_prompt.*`, `docs/formats/disc.md` 8). (d) The USA insert-disc text positions come from the USA `field.prg` code, not a USA run. Capture each with `ps1ref.Session` (USA: `--region usa`).



## 4. Code hygiene

## 5. Tests and tooling

* **P2 Disc mode: untested hardware and formats.** CHD: only a zlib v4 CHD (made from JP disc 1) and synthetic v5 files were read; run `ocdisc verify <img.chd> --against ..\data\cdrom.dat` on a `chdman createcd` image (v5, cdlz / cdzl / cdzs / cdfl) of each disc. `.mds`/`.mdf` and `.pbp` were tested only on synthetic images; CloneCD does not parse its track sections. Optical drives: tested on VirtualCloneDrive and a Windows-mounted iso only; not on a real drive, not under Proton, and the two IOCTL raw-read modes never succeeded (fallback path only). `test_disc_text` needs the RDE install and prints SKIP without it, so CI covers none of the disc text path: add synthetic cases to `tests/test_text.cpp`.
* **P3 Disc mode in the dev viewers.** `ocbattleview` and the demo mains (`src/game/battle/demo_main.cpp`, `game/flow_demo_main.cpp`, `ui/menu/demo_main.cpp`, `ui/shop/demo_main.cpp`) open the RDE install and ignore `[Paths] Source=disc`, so a disc-mode battle can only be tested through the real app (`[Debug] Battle=<scene>` with a disc ini). Make them use the same data-source search as the app.
* **P3 USA symbols inferred from neighbours.** `tools/ghidra/symbols/usa/` entries whose description ends in "inferred" may be off (two were found wrong: `menu_save_pack_state`, `save_image_base`; `save_gold` 0x8006AC54 vs gold written at 0x800719A8). `menu_text_set_context` (USA 0x801D68A8) looks up a `mess` message by index: rename. 66 JP symbols have no USA match (listed at the end of each file). About 11 matched function starts scored below 0.5. Fix each when a USA capture uses it. `ps1_capture.py` USA: disc-2 `prep` and `cast enemy` not run.


