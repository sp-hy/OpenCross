# Text: message blocks, JP glyphs, lang.dat tables, spritefonts

This document covers how dialogue and UI text is stored and shown: the PS1 message block inside a room `.bin`, the control-code byte stream, the JP 1-bit glyph font, RDE's `lang.dat` string tables and their lookup rules, the markup tokens, the `font.dat` / `ui.dat` spritefonts and which face draws what, choice lines, window sizing, and translation coverage. Tools: `tools/py/cc_text.py` (stdlib only, run with `python -I`; commands in section 9) and `octext` (`tools/octext.cpp`: `msg <room> <idx> [lang]`, `find <text> [lang]`, `coverage`). Companion data: `tools/py/data/jp_glyphs.tsv` (generated glyph table, also embedded in the engine through `import/text.h`) and `tools/py/data/text_room_ids.csv` (lang.dat room id -> map file). Code: `src/import/text.*`, `src/ui/` (dialogue box, typewriter, fonts), `src/render/text_batch.*`.

## 1. Message block (subfile 008 of a room `.bin`)

```
u16 firstTextPtr = 8 * recordCount
record[i] (8 bytes): u16 textPtr, u16 w1 (box width in chars), u16 w2 (lines), u16 w3 (width px, 12*w1)
text area: messages back to back, message i = [textPtr_i, textPtr_{i+1}), last one ends at the block end
```

* Every block has **one more record than lang.dat has rows** (57,265 records vs 56,699 rows over the 566 room ids): the last record of each room is a sentinel with no translation row. Scripts never reference it.
* A message ends with `00 01` (END, NL) or a timed close `06 xx 01`. The trailing `01` belongs to the message.
* Messages are requested by script ops `E0 w b` (w = message index), `E3 c w b`, `E4 c w b` (c = speaking entity).
* The record header's `lines` and `px` fields are not used by the PC window layout (section 7.7).

## 2. Byte stream and control codes

Bytes `< 0x20` are control codes followed by the argument bytes in the table. Bytes `0x20..0xF7` are single-byte glyph codes. Bytes `0xF8..0xFE` are **page prefixes**: the next byte (any value, including 00/01) completes a 2-byte glyph. (`FF` appears only as an argument, e.g. `05 FF` = instant speed.) The table gives the RDE markup that `jp.csv` uses for the same code (`\n` below is the 2 characters backslash + n, how lang.dat writes a newline):

| code | args | name | RDE markup | meaning |
|---|---|---|---|---|
| 00 | 0 | END | `{mend}` | end of message |
| 01 | 0 | NL | `\n` | line break |
| 02 | 0 | PAGE | `{bpage}` | wait for confirm, clear window, next page |
| 03 | 0 | WAIT | `{bwait}` | wait for confirm inside a page |
| 04 | 1 | DELAY | `{wait(n)}` | pause n frames |
| 05 | 1 | SPEED | `{speed(n)}` | typewriter speed (255 = instant) |
| 06 | 1 | AUTOCLOSE | `{wend(n)}` | window closes by itself after n frames |
| 07 | 2 | ITEM | (no 1:1 markup; the `{itemwork}` family) | item name, u16 little-endian item id |
| 08 | 1 | NAME | `{SERGE%}` .. `{PIP%}` (id = speaker id) | fixed character name |
| 0A, 0B | 1 | NUM | `{num(n)}` | decimal value of script variable n |
| 0E | 1 | COLOR | `{color(n)}` | text colour (0 white, 1 blue, ...) |
| 10 | 1 | DIGITS | `{input(n)}` | numeric entry field, n placeholder glyphs follow; result in script variable 0x18 (story rooms jac_b08i, jac_b42i, tel_b02f, tel_b23e, tel_a19i) |
| 12 | 1 | ACCENT | per-speaker grammar form: `{呼び名}` `{一人称}` ... | auto-accent / dialect lookup |
| 15 | 1 | SCRNAME | `{playwork(n)}` | name of the character in script variable n |
| 16 | 1 | FWAIT | `{fwait(n)}` | wait for a field event |
| 17 | 1 | FSET | `{fset(n)}` | set a field event flag |
| 18 | 1 | PARTYNAME | `{PARTYn%}` | name of party slot n |
| 19 | 1 | SCRSTR | `{itemwork(n)}` | item name from script variable n |
| 1B | 1 | BUTTON | `{key(n)}` | controller button glyph |
| 1C | 1 | CASE | (no markup) | capitalisation mode 0/1/2 |
| 1E | 1 | SUFFIX | (no markup) | name suffix |
| 1F | 0 | ACCENTPH | (no markup) | accent placeholder |
| 09 0C 0D 0F 11 13 14 1A 1D | 0 | unused | | no-ops on the PS1 |

The markup column follows `jp.csv` (RDE's re-authored JP text): 55,337 of 56,699 messages decode to byte-identical markup (section 4). Message text commonly begins with `05 FF` or the 3-glyph indent (`75 75 75` = U+3000 x3, used to leave room for a cursor).

## 3. Glyph lookup (JP build)

The PS1 renderer reads one glyph per step:

```
b = *p
if b < 0xF8:  idx = b - 0x20;                       p += 1;  advance = (b <= 0x74) ? 8 : 12 px
else:         idx = (b - 0xF7)*256 + (p[1] - 0x20); p += 2;  advance = 12 px
bitmap = FONT + idx * 22
```

* The font table lives in the PS1 executable `prog/slps_023.64`, at file offset `0x46F0C`. 2016 index slots are addressable; the glyph set on disc uses 1,595 codes. Indices 216..255 are never produced (single bytes stop at 0xF7, two-byte glyphs start at 256).
* A glyph is **11 rows of one u16 little-endian**, bit 15 = leftmost pixel, **11 pixels wide**, 1 bit per pixel, 22 bytes.
* **Single-byte order**: `20` space, `21 ( 22 ) 23 - 24 , 25 + 26 . 27 /`, `28..31` = `0..9`, `32..4B` = `A..Z`, `4C..65` = `a..z`, then `= ! " $ % & * : ? - [ ] 。 ? Ⅰ`, `75` = U+3000 (blank/indent), then hiragana in gojuon order from `76` (`77` い, `78` う, `7A` お), voiced and small kana, `「」ー～…？！・`, full-width digits `０-９`, full-width capitals `Ａ-Ｚ`, `／、`, up to `F5`. `0x7F` = こ, `0x85` = た, `0x8E` = の.
* **Two-byte glyphs**: prefix `F8` starts with full-width lower-case Latin then katakana (`F82E` ナ, `F85E` プ); `F9..FE` are kanji in a frequency-then-reading order (`F90D` 解, `F96B` 帰, `FB0C` 女, `FC86` 道, `FDBB` 上, then rare kanji). The order is not JIS.
* **The code-to-Unicode map is derived from data, not a published table.** `cc_text.py glyphs` aligns `jp.csv` with the disc bytes message by message; where unit counts and control-code positions agree (56,545 of 56,699 messages) each glyph unit pairs with one character, and a majority vote per glyph code resolves **1,593 of 1,595 codes** (1,434 with share >= 0.9 and >= 3 votes; 159 single-use or rare codes are flagged `low`; 54 codes have runner-up votes from RDE edits). Unresolved: `F98E` (used once) and `FF FF` (65 uses, all in one debug string "キッド前＃＃ループ" in rooms 458-464, where `jp.csv` shows two `＃`: `FF xx` is two placeholder glyphs, not a prefix: the importer reads every `FF` in the text stream as a single placeholder glyph, U+FF03 `＃`, so `FF FF` decodes as `＃＃`).
* `jp_glyphs.tsv` columns: `bytes<TAB>bitmap_index<TAB>char<TAB>votes<TAB>share<TAB>runner-up<TAB>conf`. The engine converts a PS1 glyph to a Unicode code point with it (needed to use RDE spritefonts, which are keyed by Unicode). Regenerate with `python -I tools/py/cc_text.py glyphs --write`. `render alu_a01f 15 out.png` draws room 27 message 15 from the exe bitmaps through this lookup and reads correctly; `fontsheet` prints the table as a grid. The name code `08 00` draws nothing in that PNG.

## 4. Round trip

`cc_text.py glyphs` decodes every message with the table into the `jp.csv` notation and compares: **55,337 of 56,699 messages are byte-identical**. The rest are RDE edits (246 rows), a different `{mend}`/`{wend}` terminator spelling, `{itemwork}` vs code 07, and grammar-form tokens. `room <name>` prints the decode as `JP*:`.

## 5. lang.dat keying per context

`data/lang.dat` is a ZIP (45 members; `cc_text.py langs`). Language index 0..5 = en fr it de es jp. There is no cross-language fallback.

| Context | Key | Table |
|---|---|---|
| Field dialogue | `(room id, message index)`; room id = map id (+1000 for `fin_*` maps) | `<lang>.csv`, `Ex_<lang>.csv` (FR IT DE ES), `mess_wend.csv`, `mesgen_en.csv` / `mesgen.csv` |
| Battle event messages | `(battle id << 16) | msg` (with an id remap list) | `btel_<lang>.csv` |
| Battle system messages | byte id (0-3 Pilfer, 5 run-away drop, 6 Frozen, 7 / 8 wipe, 33-59 = status / field rows) | `btmes.txt` |
| Menus, shops | the address of the JP string in the PS1 menu overlay (e.g. 「はい」) | `menutest.txt`, `shopmenu.txt` |
| Names, items, techs | `(category << 16) | index` | `sysmes.txt` |
| Descriptions | line index / char index | `waa_spec`, `keyitem`, `ome`, `savetbl`, `memmenu`, `btlexit`, `error`, `ui_*`, `cmnmes` (key = English string) |

`.txt` tables are UTF-16LE with BOM (`btmes` is UTF-8), TSV with `"` quoting; the header row names the language columns (`# Text JP EN FR IT DE ES`; `cmnmes` is `EN FR IT DE ES JP`), the column is chosen by header name, and the `Text` column is documentation. `{66}` / `{99}` become curly quotes and a literal `\n` becomes LF. `cc_text.py sysmes [lang] [--table T] [--cat N]` dumps any of them.

sysmes categories (2,683 rows): 00 items/equipment (256), 01 default character names (50; key `0x10000+id`), 17 elements/techs (512), 18 key items (65), 19 enemy names (256), 1A battle strings/descriptions (1,536), 1B misc (8). `{itemwork(n)}` maps ids: `id > 0x400` -> `0x180000 | (id-0x400)`, `id > 0x200` -> `0x170000 | (id-0x200)`, else `id`.

### 5.1 Field lookup `resolve(lang, room, msg)`

1. Rows of `<lang>.csv` are keyed by `(room, msg)`. A row is loaded only if no earlier row in the file has a larger room id (RDE stops reading at the first greater room; this matters only for the unsorted `Ex_es.csv` blocks of rooms 374 and 474, which RDE never loads).
2. `{wend(` rule: if `mess_wend.csv` has `(room,msg)` and the text contains `{wend(`, cut there and append the override (non-JP only).
3. FR/IT/DE/ES with an `Ex_<lang>.csv` row use variant `v[speaker]`. The speaker is the first `{NAME%}` tag in the leading tag run (`{SERGE%}` = 0 ... `{PIP%}` = 43, `{LYNX%}` = 49; ids 44..48 reuse 43), else the script's speaker; default 0 -> 1. `{PARTYn%}` and `{playwork}` need live party state; `cc_text.py` ignores them.
4. Substitute every `mesgen` key with `value[speaker]` (EN: dialect tokens; JP: grammar forms; empty fields collapse).
5. `\n` -> LF, delete `{n/a}`, non-JP: normalise spaces and quotes. A missing row shows "MESSAGE NOT FOUND" and an empty window.

OpenCross: `LangTables` in `src/import/text.h` implements this.

**Examples.** Room 27 (`alu_a01f`) message 15 (Arni elder), EN: key `(27,15)`, three pages (`{bpage}`), `{wait(20)}` matches JP `04 14`; `{SERGE%}` is a `mesgen_en.csv` key whose value is the bare token `SERGE%`, which the EN bare-token table renders as the name. Room 22 (`opn_a01i`) message 1, EN speaker 1 (Kid): `{PARTY2%}:\n{Cap}what is◆ it,\nSERGE%?...`; FR uses `Ex_fr.csv (22,1)` column 1. Battle: `btel_en.csv` key `386<<16|1` (Radius tutorial). Menu: `menutest.txt` key for 「はい」 -> "Yes". `sysmes` `0x000001` -> "δBone" (δ = element-colour glyph).

## 6. RDE markup tokens

157 tokens exist. The engine must support, for field dialogue:

* flow: `{bpage}` `{bpage}\n` `{bwait}` `{mend}` `{wend(n)}` `{wait(n)}` `{speed(n)}`, LF.
* style: `{color(0..4,GRAY)}` (0 white, 1 blue, ...), `{italic}` `{italic_end}`, `{66}` `{99}` `{♪}` `{♪♪}` (curly quotes, notes).
* names: `{SERGE%}`..`{PIP%}` (id = token index), `{SERGE%1}` / `{SERGE%2}` (JP: first 1 / 2 characters), `{RAZ%}`, `{PARTY0%}`..`{PARTY2%}`, `{playwork(0)}`, `{itemwork(0..3)}`, `{num(0..3)}`, `{HIDE}` (suppress next name), `{-y}` `{-i}` name suffix, `{Cap}` `{CAP}` `{CapL}` capitalisation.
* EN bare tokens (51): `SERGE%`..`PIP%`, `Sir SERGE%`, `Mister S_%`, `Herr SERGE%`, `The-SERGE%`, `◆` (marker, prints nothing), `''` (apostrophe).
* field sync: `{fset(n)}` `{fwait(n)}`, `{key(0..3,8,up,down,right,left)}` (button icon), `{input(n)}` (number entry, variable 0x18), `{ERROR}` (placeholder).
* battle only: `{wmode W H}`, `{size W H}` (window size), `{playwork(0)}` `{itemwork(0)}`.
* key-item descriptions (`keyitem`): `{ButtonFast}` `{ButtonSlow}` (the Time Shifter row, every language), the key cap or pad glyph bound to the fast / slow speed action (`rules/menus.md` section 7). The row's lines are separated by `\n`.
* An unknown `{...}` is drawn as plain characters.

Placeholders: `{PARTYn%}` = party slot n -> character id; `{playwork(n)}` / `{itemwork(n)}` / `{num(n)}` read script variable `0x1A + 2n`; `{NAME%}` = fixed id. A save name record that still holds the JP default (kana) is "unset" outside JP. Names come from the custom-name map (name entry) or `sysmes[0x10000+id]`, truncated to 15 UTF-16 units; Serge becomes "Lynx" while in Lynx's body.

Name entry: a Latin keyboard of 14x10 (`A-Z a-z`, accented rows, digits/punctuation) or the JP kana grid; reserved names クロノ / くろの / chrono / crono; names are stored host-side in `ext_save.dat` (44 x 32-byte UTF-8, `docs/formats/save.md`), while the PS1 save block keeps the default name.

## 7. Spritefonts and what draws text

### 7.1 File format (`data/font.dat`, `data/ui.dat`)

DirectXTK MakeSpriteFont binary, little-endian:

```
char[8] "DXTKfont"
u32     glyphCount
glyph[glyphCount]  (32 bytes, sorted by character)
    u32 character (UTF-16 code unit);  i32 left, top, right, bottom (subrect in texels);  f32 xOffset, yOffset, xAdvance
f32     lineSpacing
u32     defaultCharacter                (0 = none; the loader sets '#' for most faces)
u32     texWidth, texHeight, dxgiFormat (115 = B4G4R4A4_UNORM), stride (bytes/row), rows
u8      pixels[stride * rows]
```

All 20 files parse; header + `stride*rows` equals the file size for 18 of 20. The exception is both `ch_100` files (headline face), which have 1,638,400 trailing bytes after the pixels (unexplained; ignore).

Faces: `kb` 8,184/8,185 glyphs U+0000..U+FFE5 (large, 16384x5940), `kt` 7,773/7,774 (JP message windows), `bn` 217 (default non-JP), `gn` / `gn_italic` 222 (non-JP message windows), `ik` 290, `ci` 191 (numbers), `cur` 1 (U+25B6), `ch` 191 (headline), `ui_font_gnb(_small)` 191. Line spacing: gn 111.1068, bn 114.2513, kb/kt 186.6667, ik 93.3333. `small_` vs `large_` prefix follows the graphics mode; small draws every glyph 1 texel narrower. A missing glyph prints the face's default `#`; `ci`, `cur` and `ch` have none and RDE throws, so the engine must fall back.

### 7.2 RDE never draws the PS1 glyphs

In every language and art mode, RDE suppresses the PS1 glyph rasteriser and text primitives; every field, menu, shop, battle and name-entry string is drawn from spritefonts. The 1-bit JP font of section 3 is only what the PS1 itself drew; it is not part of RDE's classic look and is not drawn for any language.

**Original (classic) art swaps only the font files**: `data\font\<large|small>_font_<face>.spritefont` for the nine faces (kb kt gn bn ci cur ch ik gn_italic), same slots, offsets, scales, layout and colours; `ui_font_gnb_small.spritefont` for the PC UI object. With the small set every glyph's source rectangle is one texel narrower (`right -= 1`); advance and measuring use the full rectangle.

**The small_ files are pixel fonts** (the PS1 English release's look): one font pixel = 70/16 = 4.375 texels (runs of 4 / 5 texels in the masks); R = fill, G = fill plus a one-pixel outline ring, B = G plus a one-pixel drop shadow down-right (nested masks, as in the large files). At the usual 0.5 scale on the 1280 x 960 canvas a font pixel is 2.19 canvas units. Metrics differ from the large files (gn `A` small 61 x 83 at (11.5, 11.75) advance -20.5; large 64 x 87 at (11, 13) advance -24). `ch_100` and `cur_70` are byte-identical in both sets.

Field message windows use non-JP `gn_70` (slot 12; `gn_italic_70` slot 13 for italic glyphs) and JP `kt_70` (slot 11), scale 0.5, normal colour minus 0.085, outline = shadow = 0.13, in both art modes. OpenCross: `ui::font::FontSet` (`src/ui/ps1_font.h`) loads the slot faces for the current mode (14 slots; aliased slots share one face; `prefetch` reads a face file on a worker thread, the JP faces being large); `render::TextBatch` draws small_ faces with `SpriteFontOptions::narrow` / `pixel_pitch`.

### 7.3 Sampling the small_ pixel fonts

* **RDE** draws every glyph as a float quad (source rectangle at pen + offset * scale, no rounding) with a linear, clamped sampler, into the back buffer at the window's client size through the 4:3 game viewport. A font pixel is `4.375 * scale` canvas units, i.e. `35/16 * (viewport height / 960)` screen pixels at scale 0.5; no 4:3 height makes that whole, and the cells are stored as uneven 4 / 5-texel runs, so RDE's own output is uneven at every window size (at 1280 x 960 a font pixel comes out 2 or 2.5 px wide with half-intensity columns).
* **Grid analysis**: in gn / gn_italic / bn / ci / ik the R / G / B masks are binary (0 / 15) and every run is a whole number of 4.375-texel cells within 0.35 cell, on one grid per face: `(yOffset / 4.375) mod 1` = 0.57 +- 0.06 for every glyph, so all glyphs share the baseline row (gn: 21.6 cells below the pen line). Exceptions with smooth glyphs: Greek alpha..delta and U+2640 / U+2642 in bn, and all of `ch_100` / `cur_70`.
* **OpenCross rendering.** `TextBatch` with `pixel_pitch = size / 16` decodes each grid glyph to one texel per font pixel (the centre texel of each cell of each run) and draws it with nearest sampling at **n = round(4.375 * scale * px-per-canvas-unit)** whole screen pixels per font pixel (at least 1; below 0.75 px the source rectangle is drawn as RDE does). Layout keeps RDE's: pen advance, measuring, alignment and shrink-to-fit use the full float metrics; each glyph is centred on its RDE box on x and hung from the face baseline, which is snapped once per pen line. The narrow rule does not apply to decoded glyphs. Smooth glyphs keep the linear draw with the narrow rule.
* **Size rule.** n = round(pitch) is used only while within 10% of RDE's pitch (`kPixelSizeTolerance`, `src/render/text_batch.cpp`); otherwise the glyph is drawn RDE's way (exact float size, linear source rectangle, narrow rule). The choice is per draw (scale x window size). At 1280 x 960 every main-menu scale draws like RDE; 0.5 text (dialogue, n = 2) keeps the whole-pixel decode.

### 7.4 Pixel-font text in a PS1-resolution menu target (`[Video] Ps1Res`)

With Ps1Res, menus are drawn into a target of the PS1 overlay's display size (main menu 512 x 448, title / save list 640 x 448, shop / name entry 512 x 432, memmenu 640 x 216, key-item list 320 x 216). The text keeps RDE's small_ faces, slots, scales and positions; `ui::LowRes` sets `TextBatch::set_pixel_grid(W / 1280, H / 960)` (target pixels per canvas unit). Nearly every menu font pixel is then smaller than a target pixel (list labels 0.35 -> 0.61 x 0.71 target px; card/equip 0.3125 -> 0.55 x 0.64; numbers 0.275 -> 0.48 x 0.56; key-item list 0.55 x 0.49; memmenu labels 1.09 x 0.49; title menu 1.31 x 1.23), so nearest sampling would drop strokes and one target pixel per font pixel would make glyphs 1.6x too wide.

Choice: every glyph is drawn at RDE's exact size and pen position through the **box filter** of `SpriteBatch` (`SpriteFilter::Box`: 4 x 4 point taps over the target pixel's footprint in the decoded one-texel-per-font-pixel atlas, averaged), an area-sampled rendering at PS1 size, then nearest-upscaled with the rest of the screen. Backdrop and fill passes are filtered separately and composited, as in RDE. Only the baseline is snapped (once per line); x stays exact. Where a font pixel is within 10% of a whole number of target pixels on both axes, the glyph is placed on whole target pixels instead. Smooth glyphs take the box filter on their source rectangle. With Ps1Res off, text uses RDE's window-resolution draw of 7.3.

Menu glyphs the PS1 drew from its own sheet but OpenCross still draws from RDE font faces in classic + Ps1Res: the main-menu bar star, the element prefix glyphs alpha / beta / gamma / delta, and the Lv / HP marks. Button glyphs (L1 / R1 / O / X) are the exception: `MainMenu::Impl::btn` draws the disc's own glyph, frame `0x7C + slot` of the menu sheet in `menutest.prd`.

### 7.5 Which face draws a JP string; glyph coverage

* **Coverage** (the small_ files match): `bn_70` has 217 glyphs = ASCII + Latin-1 + Œœ, αβγδ, ’“”„, ←↑→↓, ★☆♀♂♪♫・ and **no kana or kanji**. `ik_70` has 290 = `#%x`, all hiragana / katakana, the 64 kanji of the menu labels (上下中久先具分初力化取右向命器回売天字定属左御復性抗撃攻数方期正武毒決法率確耐腕英解買身転軽造避邪鍛防風魔), full-width `＋０-９：Ａ-Ｚａ-ｚ`, and **no ASCII letters or digits**. `kb_70` / `kt_70` have the full set. A string drawn with a face lacking a glyph shows `#`.
* **RDE's slot rule.** Every string list picks its slot from the *effective* language `L' = flags bit 16 ? 5 : bit 17 ? 0 : L`. Default slot: `L' == 5 ? 0 (kb) : 4 (bn)`. The "slotL" styles (menutest 2 / 3 / 7 and 0x64-0x67, shop 7, memmenu 0x82 / 0x83, btlexit 9) use `9 (ik)` in JP else `4 (bn)` (menutest 0x64-0x67 test the global L, the others L'). Fixed slots stay fixed (menutest 0x84 = bn, 0x78 / 0x90 / 0x91 = kb, title 0x80 = ci). Bit 16 is set by RDE's caption tables for untranslated JP text and cleared when a cmnmes lookup of the JP key hits; bit 17 (the frame captions) stays set, so those captions are bn in every language (their JP cmnmes column is Latin: "Weapon", "Armor", "Elements", ...).
* So in JP, menu labels and list entries are ik or kb, never bn; the screen titles from the exe table (Latin in JP too) and the frame captions are bn. The smith panel title uses the keys "Weapon" / "Armor" / "Accessories".
* **Layout audit (debug).** `OC_TEXT_AUDIT=1` makes `render::TextBatch` record every string's glyph boxes and log `text-audit:` lines for glyphs missing from the face, strings whose glyphs overlap another's (inner 70% of each box), glyphs crossing a registered frame edge, and off-screen text. `ocmenudemo --shots` / `ocshopdemo --shots` report per shot; the game logs each new issue once (`=2` adds a sample frame every 300).

### 7.6 Choice lines (in-window MULTI_CHOICE)

* **Marker.** A choice option is a message line that starts with three spaces (`　　　` U+3000 in the csv; non-JP text has U+0020 after normalisation). `E1 0xFL` names the option lines F..L (`docs/formats/fieldscript.md`). The PS1 has **no per-line condition code**: option lists that depend on the party are separate messages and the script picks one (tel_b02f painter: message 37 = `{PARTY0%}`, `{PARTY1%}`, No thanks; message 38 adds `{PARTY2%}`; the script reads party slot 2 and `== #255` selects 37).
* **Result** = the cursor's line number in the message (F..L), stored on confirm in the entity and copied to script variable 0x04 by `E2`. The cursor starts at F and wraps between F and L.
* **Indent width (RDE).** The `E1` hook turns on choice mode; the layout then flags a line whose glyphs 0..2 are spaces and, in non-JP text, lays the first space out with base width 70 instead of 20: advance = 2 * w0 * 0.45 canvas units at glyph scale 0.5, so the three-space indent is 63 + 18 + 18 = 99 canvas units and the 70 x 70 hand (drawn at the line's first glyph) sits in front of the name. Flagged lines are never word-wrapped. Only the indent is widened; other spaces keep w0 = 20.
* **OpenCross.** `DialogueBox::set_line_choice` (called by `E1` after `E0` opened the window) lays the open message out again with `LayoutMetrics::choice_space_first / choice_space` (`ui::layout_pages`; page, revealed glyphs and waits kept). The result is `line_first + cursor`. Routes `painter`, `choice_names`.
* **Auto-close messages.** A choice list inside a `{wend(n)}` message (the Viper Manor guards' password list, jac_b22i message 5, `E1 03`, ends `{wend(254)}`) is up while the typewriter sits in `Closing`, not `WaitClose`. The hand, the highlight and the confirm all work in `Closing`; the timer still closes the window and the result is then unwritten. Route `y2_password`.
* **Line mapping between translated text and the disc message.** The `E1` rows `F..L` and the stored result are line numbers of the **disc (PS1) page**, but translated text is laid out differently (the EN text of tel_b04f message 95 has one question line where the disc has two). RDE highlights and draws the hand on `max(0, translated first choice line - disc first choice line + row)`, with `row = F + option index`. The translated first choice line is the first line of the page whose first three glyphs are spaces (`DialogueBox::first_choice_line`); the disc first choice line is the newline count (page-relative, a newline at entry 0 not counted) at which the page's third U+3000 glyph of the disc stream appears (`DialogueBox::guest_choice_lines`, read from the room's message block; the last translated page uses the last disc page, since page counts need not match). The pick written to variable 0x04 stays the **row** (`F + index`). `occ dialogue-choices` counts 165 message x language cases that need the remap (5 in EN: tel_b04f 95, gal_a10i 0, tel_b16i 2, hid_a04f 13, wld_b00 8). Routes `termina_drift(_orig)`, `choice_remap`; test `test_text` (`test_choice_lines`).
* **Text-data defects identical in RDE.** A translated list with fewer option lines than the `E1` range (es tel_b02f 20: two of three options; es tel_b24e 90: the first option merged into the question line; fin_29 19) leaves the last option without a line (no hand, never highlighted, still pickable by index). jac_b01f 39 has an empty fourth option line in every language and on the disc. The hand is also drawn in a frameless (label) window.

### 7.7 Window size of short messages

There is no 3-line minimum. The frame's row count is `rows = lines + 1`, where `lines` is the tallest page's line count (it starts at 1): a one-line message has 2 rows, a two-line message 3 rows; only a portrait forces `rows >= 5`. Width = the widest line of any page, `cols` = tiles with `x <= XRp + 70`, plus 1. The PS1 header's `lines` / `px` fields are not used. Example: `!{wend(60)}` (dem_02 message 0) gets a wide, one-text-line frame of 2 tile rows. With skin 8 and 2 rows: bottom corner at `last + 0.15 TH - 0.17 TW` (no portrait), corner sheets T0 / R0 v0 = 0.004, B0 / R24 v0 = 0.43. Sizes on the 1280 x 960 canvas (TW 61.75, TH 66.5, no portrait, default skin): one text line = 286 high, three lines = 409; width = W + 281.

OpenCross: `DialogueBox::layout` starts `lines_` at 1 and the frame uses it directly; `DialogueBox::frame_size(skin, W, lines)` is the pure helper; `test_text` `test_frame_rows`. `occ dialogue-sizes` reports the blast radius: of 12,373 EN room messages, 1,386 are 1-line and 3,706 are 2-line.

### 7.8 Typewriter pacing and window opening

OpenCross: `ui::layout_pages` splits a flattened message into pages (`{bpage}` boundaries) with a per-page event list (`Delay`, `Speed`, `Wait`, `FWait`, `AutoClose`, `FSet`, each firing before glyph `at`); `ui::Typewriter` reveals it in 1/60 s ticks.

* **Speed.** A glyph accumulator gains `speed / 64` per tick (default 64 = 1 glyph per tick; `{speed(0x80)}` = 2 per tick). `{speed(255)}` reveals the whole page at once. The speed is set at the start of a message to the default and persists across its pages.
* **Waits.** `{wait(n)}` pauses n frames and `{wend(n)}` closes the window n frames after the last glyph, both counted at 30 Hz script frames. `{bwait}` waits for confirm inside a page; `{bpage}` waits for confirm and turns the page; the last page waits for confirm to close. `{fset(n)}` writes n to script variable 0x22 when the reveal reaches it; `{fwait(n)}` holds the reveal until that variable equals n.
* **Confirm.** While revealing, confirm fast-forwards to the next wait (delays are skipped, `{bwait}` still stops); in a wait state it continues; in the auto-close countdown it closes at once.
* **Opening.** A field message window grows from nothing to full size in 16 ticks (scale k / 16 on the k-th tick); text, its waits and a number entry start on the tick after it is full and confirm presses before that are dropped. Window and text are not drawn while it grows and fade in over the next 3 ticks. Closing has no animation.

### 7.9 Window skins

Field message windows use the PS1 skin sheets `map/window/wN.tim` (8bpp 128 x 96 TIM: frame pieces at the top, the 16 x 16 page-leaf / hand frames at v 80, u = frame * 16; colour 0x0000 transparent); HD art draws the 4x redraws `hd/window/wNN.png` with the same normalised UVs. The option byte of the Custom Window Frame setting picks the sheet: byte 0 -> w4, 1..3 -> w1..w3, 4..14 -> w5..w15; menus use w17. A window is a 7 x 4 grid of fill pieces plus corner, top / bottom and left / right edge pieces and the page leaf (close triangle, bottom right, its u offset steps with a phase 0..4); `ui::build_skin_window` (`window_frame.*`) returns the quads in canvas units (1280 x 960) and `DialogueBox::draw_tiles` / the key-item list use the same piece model.

### 7.10 World name

In HD art the world-map rooms show the world name as `map/HomeAnother/{home,another}_<lang>.png` from hd.dat, stretched over (20, 7)..(330, 207) of the 1280 x 960 canvas. It is shown while the map has a world name (script variable 0x50) and the World Names option (variable 0x52) is on, over any window, fading in by 0.0625 and out by 0.065 of full opacity per 60 Hz frame. Home World maps are 0x8F, 0xFA, 0xFC, 0xFE, 0x102, 0x1BB, 0x213; every other map is Another World. The PS1 draws the same name as a screen-fixed layer group of the world-map rooms. OpenCross: `ui::WorldName`.

### 7.11 The PS1 field glyph path (reference only; RDE does not draw it)

The PS1 expands each glyph's 11 rows into one bit-plane of a 16 x 12 4bpp cell uploaded to VRAM. A glyph counter picks the plane (`& 3`), so **four consecutive glyphs share one cell**, and each glyph sprite uses a CLUT chosen by that counter. The four CLUTs (VRAM x 0x380 / 0x390 / 0x3A0 / 0x3B0, y 0xF8) are bit-plane selectors: entry i = 0xFFFF if bit k of i is set, else 0x0000. The colour is the sprite's RGB (0x80 = 1.0); there is no outline or shadow plane. The memmenu name uses plane 0 only (6 cells at VRAM (0x3C0 + 3 i, 0xF3)) and fakes weight by drawing the strip three times (x 66 / 67 / 69, colours 0x64 / 0x64 / 0x40).

## 8. Coverage of translations over the 566 rooms

References are `E0 / E3 / E4` operands found by the script decoder (reachable instructions). 566 room ids, 57,265 message records, 54,281 distinct reachable (room, message) references, none dangling:

| lang | reachable refs with real text | `{ERROR}` placeholder | missing row |
|---|---|---|---|
| en | 12,308 | 41,973 | 0 |
| fr | 12,307 | 41,974 | 0 |
| it | 12,308 | 41,973 | 0 |
| de | 12,307 | 41,974 | 0 |
| es | 12,302 | 41,979 | 0 |
| jp | 54,281 | 0 | 0 |

Dead-code-only references: 185 (59 translated, 124 `{ERROR}`, 2 out of range: `wld_a00` / `wld_b00` message 62042, a malformed operand in unreachable code). 2,801 messages are never referenced (the 566 sentinels plus unused). Every (room, msg) the game can show **has a row in all six files**. The `{ERROR}` rows are the debug/dummy text of 68 rooms entirely, and about 50 debug-menu messages in each of 491 other rooms; only 7 rooms have no `{ERROR}` among reachable references. The cost is untranslated debug text, which the engine can treat as JP-only or hidden. Per-room counts: `coverage <lang> --rooms`.

## 9. Tool usage and open questions

`python -I tools/py/cc_text.py` commands: `langs` | `room <name|id> [lang]` | `coverage [lang] [--rooms --all --errors]` | `sysmes [lang] [--table T --cat N]` | `font` | `glyphs [--write]` | `render <room> <msg> <png> [--scale N]` | `fontsheet <png> [--first N --count N]`. The room argument is a map-file stem or the numeric id (ids 0 / 9 share `ending`, 1 / 8 `opening`, `fin_19` appears twice).

Unknown: the markup spelling of code `07` (ITEM; the importer's own debug dump writes `{item(n)}`); glyph `F98E`; the 159 low-confidence glyphs (mostly single-use rare kanji / symbols) deserve an eyeball check against the bitmap sheet; the speaker detection needs live `{PARTYn%}` / `{playwork}` state that the tool lacks; the kana / kanji ordering inside the font table is tied to data only; the trailing bytes of `ch_100`; the policy for debug-room `{ERROR}` text.

## 10. UI art sources

Three groups of UI art are decoded or cut by the engine itself. All disc reads use `Vfs::read_disc` (the disc image, never the HD override copies).

### 10.1 PS1 menu art (`src/ui/menu/ps1_art.*`)

Used by Original-art menus, shops, name entry and save/load with `[Video] Ps1ResMenus`; callers sample it nearest.

* **`map/etc/systim.bin`** holds several TIMs back to back. The loader scans it at 4-byte steps for the TIM magic `10 00 00 00` and takes the one that is 4bpp with a CLUT and sits at VRAM (896, 96) with 16 x 64 halfwords: the **parchment tile**, 64 x 64 texels, CLUT row 0, colour 0x0000 transparent. Menus tile it 8 x 7. The file's other TIMs are not used by this code.
* **`menu/menutim.prd`** is a DRP; each entry is one TIM found by its four-character name (`menutim(fs, "wind")`, `mpla`, `icon`, `p1ic`..`pfic`, the colour sheets `mnbl` `mngr` `mnku` `mnre` `mnwh` `mnye` `mnni`). The **`wind` sheet** holds the window frame pieces (texel rects): 8 x 8 corners TL (19,1), TR (37,1), BL (19,10), BR (37,10); top edge (28,1) and bottom edge (28,10) as double lines (a one-column slice at x 31 is stretched along the edge); left edge (15,10), 3 px wide; right edge = the 3 right columns of TR (42,4). Lines are 3 px (dark, light, dark) and the interior is transparent. `frame_pieces` emits those pieces for any rectangle and can cut a gap in the top edge (caption tab) or the bottom edge.
* **Sprite sheet frames** (`sheet_frame`): frame n (1-based) of the `main` type-3 entry of `menu/menutest.prd`. A frame is a part list (`s16 x, y; u8 u, v; u16 CLUT; u8 w, h; u16 flags`, 12 bytes; texture page in the low nibble of flags, page half in bit 4, bit 9 / 10 = mirrored in x / y), drawn last part first from whichever `menutim.prd` 4bpp TIM holds that CLUT position and those texels, into a 512 x 512 image with the frame origin at (192, 192).
* **Faces** `face/<name>.tim`: one 64 x 64 TIM per character id 0..49 (name table `ps1::face_name`: `seljyu`, `kid`, `gill`, ... `yama`), decoded with CLUT row 0. Retail portrait strips are `menu/playtim/pNN.tim` (8bpp, 128 x 224). Key-item pictures are `menu/keyitem/<name>.tim` (8bpp 256 x 256, 255 x 255 drawn; ids 54..60 have none); save-box icons are `menutim.prd` `p1ic`..`pfic` (16 x 16, own CLUT).
* **Font fallback.** The 1-bit PS1 font (section 3) has no ASCII punctuation. `bitmap_text` replaces every character it lacks by the first form it does have: `"` -> curly quote then full-width `＂`; `,` -> `，` then `、`; `'` -> `＇` then `’`; any other printable ASCII -> its full-width form; curly quotes and dashes and `…` -> their full-width / JP counterpart (`⋯`, `ー`). Spaces and newlines pass; a character with no substitute is kept and the caller's HD face draws it.

### 10.2 Play-time feature icons (`src/ui/feature_icons.*`)

* **Sheet:** `ui/exticon_<lang>.png` from `ui.dat` (`ui/exticon.png` for JP and as fallback), 256 x 256, a 4 x 4 grid of 64 px cells (drawn 53 x 53 with a 0.001 UV inset). Row 0: column 0 encounters off, 1 auto battle, 2 battle boost. Row 1: the AUTO SAVE strip across the full width. Row 2: column 0 fast, column 1 slow. Row 3 is unused by this code.
* **Placement** (1280 x 960 canvas): the column starts at x 20, its first icon's bottom edge at y 877, and grows upwards 63 units per active icon in the order auto battle, battle boost, encounters off, slow, fast. When the window leaves more than 75 canvas units of black beside the 4:3 picture, the column and strip move 93 units outward into the bar.
* **AUTO SAVE strip:** 212 x 53 at x 83 beside the first slot, 80 % opaque. A 120-count starts at each completed auto-save and runs down once per 60 Hz frame; with no toggle on it runs down twice per frame (about 2 s with icons, 1 s without).
* **When shown:** icons draw over the finished frame (under the pause screen) while the game is in play, i.e. not during the boot logo, movies, the title scene, or its slot list / name entry (flow states other than Opening and Field); they follow the Cheats toggles (auto battle, battle boost, no encounters) and the slow / fast modes (slow and fast hidden while the speed is locked), and hide with the free-camera overlay hiding.

### 10.3 User guide screen (`src/ui/keymap_screen.*`)

* **Trigger:** every New Game shows it between the title menu and name entry (the game is paused and the frame under it is black). Dismiss: any key except F1..F12 (or Space), or the pad going from some button or stick (>= half way) held to none held; L3, R3 and Start do not count. A latched press during a fade is used when the counter reaches the next wait.
* **Art:** two 1620 x 1215-space pages `hd/userguide/steampad_<lang>.png` ("Controller Controls") and `steamkey_<lang>.png` ("Keyboard Controls"), suffixes en fr it du es jp; glyphs from `ui/gamepad_assign.png` (14 x 4 cells of 96 px; glyph rows are the odd rows) and `ui/keyboard_<l>.png` (16 x 7 cells of 48 px, suffix e f i g s j). Both art modes use the same pages; if either page is missing the screen is skipped.
* **Timing** (counter steps at 60 per second): page 1 fades in over 15 steps and waits at 15; 15..30 cross-fades page 2 over page 1; waits at 30; 30..45 fades page 2 to black; 45 closes.
* **Key caps.** The cell of a virtual key is `platform::keyboard_cell(vk)` (column `cell & 15`, row `cell >> 4`; 0 = no glyph). Each action has two keys (`platform/keymap.h`: set 0 arrows / Enter / Backspace / F / I / H / J / F1 / F2, set 1 W S A D / X / Left Shift / R / Tab / Q / E, none for the speed modes). Move rows draw four caps 52 units apart from x 110 (set 0 at y 247.5, set 1 at y 301.5); two-key rows draw set 0's cap then set 1's 52 units to the right; any unbound key shifts its row half a step (26) right. Fixed caps: Space (pause), K (change lead), F3 (no random battles), F4 (battle boosts), F5 (auto-battle), Esc (ESC menu). The controller page is a fixed picture of 20 glyph cells at fixed positions.

Not determined from this code: the contents of `systim.bin` other than the parchment TIM, and the full virtual-key to cell table (`keyboard_cell` in `src/platform/keymap.cpp`).
