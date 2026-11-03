# Open issues and known bugs

The working list for the engine: behaviour bugs found by reading the code, fidelity gaps against the original game, open comparisons that need a capture, and code hygiene. Each entry names the file to look at. Remove an entry in the same change that fixes it; keep the sections. This file is temporary working notes, so unlike the other docs it may carry RAM addresses; they leave with their entry, and anything worth keeping moves to `docs/formats` as a fact without the address and to `tools/ghidra/symbols/` as a symbol.

Entries that need a PS1 comparison use `python -X utf8 tools\py\ps1_cast.py force` (`docs/validation.md`, section 3.4) for the reference and `tools\ours_cast34.ps1` for the OpenCross side, which runs `tools\py\fx_diff.py`. Reading a diff: a sequence name that only one side has is not a bug until these are ruled out: a random camera pool or selector op, a different target count (a debug cast hits every enemy of an all-targets element, a forced PS1 entry hits one), our run ending before the sequence starts (raise `-Frames`), the capture holding two casts (`--cast-index`, `--window`), the scene's own opening action running first on our side, and `-Dt` pacing, which caps per-tick emitter fires. Unique techs are cast by their owner: `0xC000 | element` in a grid cell of any `char*.bin` set, or pgdat bytes +0x14..+0x16 for a double or triple tech.

## 1. Bugs (behaviour)

## 2. Fidelity gaps (not modelled)

- **Four effect packs fail `fx-audit`** (`tools/ocbattle_fx.cpp`, mesh and model loading in `src/game/effects/`). `opencross ocbattle fx-audit` prints `542 packs (538 clean ...)`; a pack counts as clean when its `generic_issues` and `bespoke_issues` columns are empty. The four that are not, from `battle_effects_audit.csv`: eid 3 `tech/yamanek0.prd` and eid 142 `tech/hensin.prd` (`mesh-texture-missing`), eid 204 `syoukan/tunico.prd` (`mesh-texture-partial`), eid 206 `syoukan/tfleaf.prd` (`model-missing`). The same four were already flagged before the dialogue and effect-timeline batch, so they are not a regression of it. Reproduce with `opencross ocbattle fx-audit --eid <N> -v`, then cast the effect in `ocbattleview` and against the PS1 (`ps1_cast.py force`) to see whether the texture or model really is missing on screen or whether the audit misses an indirect source (a texture or model reached through another pack or the caster's model). Done when each pack is either fixed or shown to look the same as the PS1, with the audit taught that case, and `fx-audit` reports 542 clean.

## 3. Open comparisons (need a capture against the PS1 or RDE)

## 4. Code hygiene

## 5. Tests and tooling
