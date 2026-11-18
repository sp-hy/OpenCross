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

* **P3 Disc mode: battle leftovers.** The disc-mode battle HUD, results screen, command box (PS1 box (28, 160), cursor at (34, row y), the OpenCross fifth row on the same 10-px pitch), stamina digits and script page cursor match PS1 captures on both builds (`docs/rules/battle.md` 6.9). Left: the command cursor's two-frame switch (ours every 16 ticks) is unverified; unscaled PS1 shots 16 frames apart all showed frame 1 while an earlier run showed frame 2. Done when a frame-by-frame PS1 capture of the menu (`ps1ref.Session`, one shot per frame) settles the period and phase.
* **P2 Disc mode: menu and text leftovers.** Menus, shops, name entry, dialogue, the title and the load screens match PS1 captures on both builds (`docs/formats/text.md` 7.14, 7.15, 10.1, 11.6; `docs/rules/menus.md`). Left, each to be checked against a PS1 capture (border crop, `docs/validation.md` 2.3) on both builds: (a) the text-table generator (`tools/py/ps1_text_tables.py`) now keeps a row on one line when RDE's row is one line; check the JP shop rows it joined (ありがとう / ございました！ at shopmenu 0x801DF4A4 and 0x801DF744, 必要なＬｖ分選択 / 済みです, あとＬｖ / 分必要, 分解後の素材数 / ／所持数) against the PS1, which may break them; (b) the composed menutest row 0x801F4BD2 in `src/import/text_tables.inc` still reads only ＯＮ (the settings screen no longer uses it); (c) the USA save screen lacks messages 46 (EMPTY BLOCK) and 49 (the overwrite prompt), and the in-game save opens without the PS1 dial, with OpenCross's slot choice first; (d) the Customize screen's run/walk button glyph follows OpenCross's button mapping (×) where the USA PS1 shows ○; (e) the pad rows are not greyed without an analog pad or rumble, as `rules/menus.md` specifies; (f) the element shop and the smithy were not re-checked after the round-4 shop and font changes; (g) JP menu message lines (slot-choice prompt, load caption) measured about 0.8x the PS1 size in a run without a BIOS: re-check with a BIOS copy next to the exe (`docs/validation.md` 3.8); (h) dialogue: the page leaf's animation timing still follows RDE's, and the number-entry cursor digit's orange is not compared; (i) the title's Settings / Quit labels use the PS1 menu font (the title art has no S, g, s, Q glyphs), an OpenCross-only look.

## 3. Open comparisons (need a capture against the PS1 or RDE)

## 4. Code hygiene

## 5. Tests and tooling

* **P2 Disc mode: untested hardware and formats.** CHD: only a zlib v4 CHD (made from JP disc 1) and synthetic v5 files were read; run `ocdisc verify` on a `chdman createcd` image (v5, cdlz / cdzl / cdzs / cdfl) of each disc (`chdman` is not on the dev machine). `.mds`/`.mdf` and `.pbp` were tested only on synthetic images; CloneCD does not parse its track sections. Optical drives: VirtualCloneDrive with the USA and JP `.cue` images reads raw (SCSI READ CD), files match the image, boot, movies, disc swaps, a wrong-build refusal and an eject mid-movie work; not tested on a physical drive or under Proton, and the two IOCTL raw-read modes were never reached.
* **P3 USA symbols left.** All "inferred" USA entries are resolved. Still unmatched in `usa/ps1_battle.txt`: `btl_unit_record_backup`, `btl_grid_growth_table`, `btl_queued_element_id`. The swap group (party-swap state) is confirmed on JP by a run but only by code match on USA: a forced Dopplegang cast on the USA build resets the game to the title when the swap starts (probably because the forced cast skips the creature pick). Done when each is matched or removed and the USA swap is seen in a run with a real Dopplegang pick.


