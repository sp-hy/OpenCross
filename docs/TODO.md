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

* **P3 Disc mode: battle screen leftovers.** The battle HUD and results screen are the PS1's on both builds (`docs/rules/battle.md` 6.9, 8.4.1), checked against PS1 crops. Left: (a) the script window's page cursor (`mark` sheet) is not compared with a PS1 scripted battle message; (b) the USA name glyph "r" has one lit texel at (4,1) in our `osak` frame that the PS1 does not show; (c) the swap/bench list triangles (an OpenCross feature) were seen on screen in JP only. Done when (a) matches a PS1 crop and (b) is explained or fixed.
* **P2 Disc mode: menu text leftovers.** Menus, shops, name entry, the party screen and the monster picker draw the PS1's art and text on both builds (`docs/formats/text.md` 7.15, 10.1, 11.6), and with a BIOS file next to the exe or the disc images JP text and name entry use the console font. Left: (a) JP settings are drawn on a 640-wide display where the PS1 uses 512, and some strings are wrong ("×ボタンON" shows "×ON", "バイブレーション機能" loses "機能", "左スティック傾斜角度" wraps); (b) JP name entry: the right kana block's row at y 194 (ッャュョヴ) is empty; (c) key items: the underlines start one row low (78 against 62); (d) the load screen's "ファイルを選択してください" wraps to two lines, the PS1 draws one; (e) the in-game save screen's detail lines are not measured, and a cleared save's yellow star is not tinted; (f) USA trading goods with no colour restriction: the word the USA overlay draws is unknown; (g) JP full-width needs/levels in shops keep our spacing, not the PS1's 16-px cells; (h) name entry's swirls blend at quarter alpha (the title canvas has no additive mode); (i) the PS1's "..." in the USA name prompt is small dots, ours the sheet period. Reference: `ps1_capture.py` with the border crop (the picture is the raw frame buffer). Done when each matches a PS1 crop on both builds.
* **P3 Disc mode: search fills a missing disc from a later place.** `src/import/disc/disc_search.*` stops at the first place holding any Chrono Cross disc, so disc 2 in the exe folder is not found when `Disc1=` names only disc 1; the insert-disc screen asks instead. Merge candidates across places by build and disc number.
* **P3 Disc mode: movie streams are read whole** (`src/movie/decoder.*`, about 92 MB for `ending_s`). Stream them from the VFS for optical drives.



## 3. Open comparisons (need a capture against the PS1 or RDE)

* **P3 Disc mode: unverified PS1 details.** (a) Row 12 of the movie table (short staff roll) uses strip row B scrolled one row per frame (`docs/formats/movies.md` 10): inferred from pacing, not seen on the PS1. (b) The PS1 stops the JP `newop` at the row's end frame 2280 (OpenCross does, hiding 420 black frames): unverified. (c) Whether music keeps playing during the insert-disc screen (`src/ui/disc_prompt.*`, `docs/formats/disc.md` 8). (d) The USA insert-disc text positions come from the USA `field.prg` code, not a USA run. Capture each with `ps1ref.Session` (USA: `--region usa`).



## 4. Code hygiene

## 5. Tests and tooling

* **P2 Disc mode: untested hardware and formats.** CHD: only a zlib v4 CHD (made from JP disc 1) and synthetic v5 files were read; run `ocdisc verify <img.chd> --against ..\data\cdrom.dat` on a `chdman createcd` image (v5, cdlz / cdzl / cdzs / cdfl) of each disc. `.mds`/`.mdf` and `.pbp` were tested only on synthetic images; CloneCD does not parse its track sections. Optical drives: tested on VirtualCloneDrive and a Windows-mounted iso only; not on a real drive, not under Proton, and the two IOCTL raw-read modes never succeeded (fallback path only). `test_disc_text` needs the RDE install and prints SKIP without it, so CI covers none of the disc text path: add synthetic cases to `tests/test_text.cpp`.
* **P3 Disc mode in the dev viewers.** `ocbattleview` and the demo mains (`src/game/battle/demo_main.cpp`, `game/flow_demo_main.cpp`, `ui/menu/demo_main.cpp`, `ui/shop/demo_main.cpp`) open the RDE install and ignore `[Paths] Source=disc`, so a disc-mode battle can only be tested through the real app (`[Debug] Battle=<scene>` with a disc ini). Make them use the same data-source search as the app.
* **P3 USA symbols inferred from neighbours.** `tools/ghidra/symbols/usa/` entries whose description ends in "inferred" may be off (two were found wrong: `menu_save_pack_state`, `save_image_base`; `save_gold` 0x8006AC54 vs gold written at 0x800719A8). `menu_text_set_context` (USA 0x801D68A8) looks up a `mess` message by index: rename. 66 JP symbols have no USA match (listed at the end of each file). About 11 matched function starts scored below 0.5. Fix each when a USA capture uses it. `ps1_capture.py` USA: disc-2 `prep` and `cast enemy` not run. Name entry: USA `menu_name_kbd_page` and the `confirm` / `reject_name` addresses look like labels inside the input function, and the JP list names the same mid-function points (`keyboard_page_prev` and others) as functions.


