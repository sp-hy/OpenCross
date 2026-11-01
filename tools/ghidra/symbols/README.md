# Symbol lists

Names this project has established, one `name = 0xADDR;  # function|data: description` per line. They come from the project's own reverse engineering (each name paraphrases what the project worked out the code does, including SDK routines it identified); nothing is copied from third-party symbol lists. Lines starting with `#` are comments.

| File | Applies to | Load base | Entries (functions / data) |
| --- | --- | --- | --- |
| `chronocross_1010.txt` | `CHRONOCROSS.exe` v1.0.1.0 (x86 PE, image base `0x400000`) | import default | hook plumbing plus the described functions |
| `ps1_main.txt` | `prog/slps_023.64` (header stripped) | `0x80010000` | 504 / 247 |
| `ps1_field.txt` | `prog/field.prg` | `0x800A9F00` | 348 / 182 |
| `ps1_battle.txt` | `prog/battle.prg` | `0x800A9F00` | 428 / 157 |
| `ps1_btlinit.txt` | `prog/btlinit.prg` | `0x801EE000` | 22 / 7 |
| `ps1_btlexit.txt` | `prog/btlexit.prg` | `0x801EA800` | 35 / 21 |
| `ps1_menu.txt` | `prog/menutest.prg`, `namemenu.prg`, `shopmenu.prg` (and the memmenu/monmenu screens) | `0x801D2800 (0x801EE000 for memmenu, monmenu)` | 197 / 131 |
| `rde_hooks.csv` | catalogue of the RDE exe's hooks (`CHRONOCROSS.exe`) | n/a | 2,034 rows |
| `overlays.csv` | catalogue of the PS1 overlays | n/a | 106 overlays plus the main exe |
| `ps1_effects_<overlay>.txt` (56 files) | `prog/<overlay>.prg`, one file per named effect overlay | `0x801C5E00` | 179 / 23 |

## Which Ghidra program

The PS1 programs are raw bytes from `data\cdrom.dat` (`prog/<name>.prg`; the main exe is `prog/slps_023.64` without its 0x800-byte header). Import each one as its own program: loader **Binary**, language `MIPS:LE:32:default`, base address from the table above (headless: `-loader BinaryLoader -processor MIPS:LE:32:default -loader-baseAddr <base>`). Overlays share address space, so apply a list only to the program it is named for:

- `ps1_field.txt` and `ps1_battle.txt` both start at `0x800A9F00`. An address that both programs use is listed in both files with `(ambiguous)` in the description; check it against the program before trusting it.
- `ps1_menu.txt` covers three overlays that share `0x801D2800`; an entry follows the overlay its description names, and an address that two of them use appears once. Apply it to `menutest`, `namemenu` or `shopmenu` as needed and ignore failed lines.
- `ps1_effects_unknown.txt` holds effect-overlay entries whose overlay is not identified. Treat them as hints, not as names to apply.
- Data entries above the end of an overlay (battle or field state variables) are labels in free RAM; they apply to any program whose memory block covers the address.

## Adding a symbol

Whenever development identifies a function or variable, add it here in the same change:

- Pick the file by program (`ps1_main.txt` below `0x800A9F00`; field, battle, menu, btlinit, btlexit or the effect overlay by load base; `chronocross_1010.txt` for the RDE exe).
- One line, in address order: `name = 0xADDR;  # function: what it does` or `# data: what it holds`. Names are snake_case with the subsystem prefix (`sys_` `btl_` `fld_` `menu_` `snd_` `fx_`), verb-noun for functions, at most about 40 characters, and paraphrase what you worked out. Never copy a name from a third-party symbol list.
- Keep names and addresses unique within the file; replace a `hook_<category>_<pc>` placeholder when you learn what the handler really does (keep the pc in the comment).
- The address lives here only: source comments and docs describe behaviour without addresses.

## Apply

```powershell
& "$env:GHIDRA\support\analyzeHeadless.bat" C:\work\proj ps1 -process battle.prg -noanalysis `
    -scriptPath C:\ghidra_scripts -postScript ApplySymbols.java C:\work\symbols\ps1_battle.txt
```

`ApplySymbols.java` reads the kind from the first word of the comment (`function` creates a missing function, anything else adds a label), prints counts, and skips addresses outside the program. `ghidra_setup.ps1` applies `ps1_main.txt` to the main exe and `chronocross_1010.txt` to the RDE exe automatically; use `-Symbols <file>` for another list. Every line matches `^[A-Za-z_][A-Za-z0-9_]* = 0x[0-9A-Fa-f]{8};`; names and addresses are unique within a file.

## Catalogues

### rde_hooks.csv

One row per hook of the RDE exe: a guest (PS1) PC bound to an x86 handler. Columns:

- `pc` guest address the hook fires at.
- `guard` what the handler checks before acting; `-` means nothing. Forms: `ovl@800A9F00==N` (the word at that guest address, an overlay ID word, equals N: 6 field, 8 battle; `ovl@801D2800==N` likewise for the menu overlays 10, 11, 12); `insn@A==W` or `insn@pc==W` (the guest instruction word at A, or at the hooked PC, equals W; `pc-4` is the word before it; `insn@pc (PC check)` means the handler compares against the PC itself); `mem@A==V` (a guest memory word or byte); `RA(sp+K)==V` (the saved return address in the frame); `map==`, `room==`, `battle==`, `lang!=JP` (game-state conditions); `host flag` (a host-side variable); bare register tests such as `a0<16`. `&&` and `&` join conditions; `;` joins alternatives of the same check.
- `mode` how the handler treats guest execution. `observer`: runs, then the original instruction executes. `replace`: does the work and resumes the guest elsewhere (usually the return address, replacing the function). `skip`: advances the PC past some instructions. `redirect`: jumps the PC to another address. `return-early`: returns from the current function with a value in v0.
- `category` the feature group (native-port, font/text-render, hd-overlay-sprite, hd-texture-vram, hd-background, memory-expansion, and so on; see `docs/rde_internals.md`). `native-port` rows are machine-recompiled game functions, not features.
- `purpose` short plain-language description of what the handler does.
- `assets` host files the handler reads, when any (`-` or empty otherwise).
- `handler` host address of the handler in `CHRONOCROSS.exe` (image base `0x400000`).
- `installer` where the registration is: a host address inside the installer code, or `table@<addr>` for a static (PC, handler) table that an installer walks.
- `handler_symbol` the handler's name in `chronocross_1010.txt`, matched by address; empty when the list has no entry at that address (two rows). A few handlers serve several hooks and share a symbol.

### overlays.csv

`overlay,prg_file,load_base,size,id_word`: every overlay program in `cdrom.dat` plus the main exe, the file name inside the archive, the guest address it loads at, its size in bytes, and its ID word (the first 32-bit word of the file, used by hook guards). `-` means the file has no ID word (its first word is code, so hooks identify it by instruction checks) and applies to the main exe, `btlexit`, `btlinit`, `memmenu`, `memmenu2`, `monmenu` and `playinit`. Overlays that share a load base are alternatives: only one is resident at a time.

`ghidra_setup.ps1 -Ps1Exe <cdrom.dat> -Overlays field,battle,darkfeit` (or `-Overlays all`) imports the listed overlays as separate raw programs at these bases into the same Ghidra project as the main exe, applies the matching `ps1_<overlay>.txt` or `ps1_effects_<overlay>.txt` (menu overlays use `ps1_menu.txt`), and writes each export to `<OutDir>\ps1\<overlay>\`. See `docs/rde_internals.md` for how the overlays and hooks fit together.
