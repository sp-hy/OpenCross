# Text: message blocks, JP glyphs, lang.dat tables, spritefonts

This document covers how dialogue and UI text is stored and shown: the PS1 message block inside a room `.bin`, the control-code byte stream, the JP 1-bit glyph font, RDE's `lang.dat` string tables and their lookup rules, the markup tokens, the `font.dat` / `ui.dat` spritefonts and which face draws what, choice lines, window sizing, translation coverage, and the USA build's own text (section 11). Tools: `tools/py/cc_text.py` (stdlib only, run with `python -I`; commands in section 9) and `octext` (`tools/octext.cpp`: `msg <room> <idx> [lang]`, `find <text> [lang]`, `coverage`). Companion data: `tools/py/data/jp_glyphs.tsv` (generated glyph table, also embedded in the engine through `import/text.h`) and `tools/py/data/text_room_ids.csv` (lang.dat room id -> map file); USA build: `usa_glyphs.tsv`, `usa_tables.tsv`, `usa_menu_strings.tsv` in the same folder (section 11). Code: `src/import/text.*`, `src/ui/` (dialogue box, typewriter, fonts), `src/render/text_batch.*`.

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
* **The code-to-Unicode map is derived from data, not a published table.** `cc_text.py glyphs` aligns `jp.csv` with the disc bytes message by message; where unit counts and control-code positions agree (56,545 of 56,699 messages) each glyph unit pairs with one character, and a majority vote per glyph code resolves **1,593 of 1,595 codes** (1,434 with share >= 0.9 and >= 3 votes; 159 single-use or rare codes are flagged `low`; 54 codes have runner-up votes from RDE edits). 30 more codes that no room message uses (`銀 銅 拳 ☆ → ‥ Ｌ ｖ`, ...) are resolved the same way from the other text sources of section 12 against their lang.dat tables (glyph units paired one to one where both sides have as many), so 1,623 codes are known; still unknown: `FDEB` / `FDEC` (the triangle and square button names of the executable's button table, which RDE leaves empty). Unresolved: `F98E` (used once) and `FF FF` (65 uses, all in one debug string "キッド前＃＃ループ" in rooms 458-464, where `jp.csv` shows two `＃`: `FF xx` is two placeholder glyphs, not a prefix: the importer reads every `FF` in the text stream as a single placeholder glyph, U+FF03 `＃`, so `FF FF` decodes as `＃＃`).
* `jp_glyphs.tsv` columns: `bytes<TAB>bitmap_index<TAB>char<TAB>votes<TAB>share<TAB>runner-up<TAB>conf`. The engine converts a PS1 glyph to a Unicode code point with it (needed to use RDE spritefonts, which are keyed by Unicode). Regenerate with `python -I tools/py/cc_text.py glyphs --write`. `render alu_a01f 15 out.png` draws room 27 message 15 from the exe bitmaps through this lookup and reads correctly; `fontsheet` prints the table as a grid. The name code `08 00` draws nothing in that PNG.

## 4. Round trip

`cc_text.py glyphs` decodes every message with the table into the `jp.csv` notation and compares: **55,337 of 56,699 messages are byte-identical**. The rest are RDE edits (246 rows), a different `{mend}`/`{wend}` terminator spelling, `{itemwork}` vs code 07, and grammar-form tokens. `room <name>` prints the decode as `JP*:`.

## 5. lang.dat keying per context

`data/lang.dat` is a ZIP (45 members; `cc_text.py langs`). Language index 0..5 = en fr it de es jp. There is no cross-language fallback.

| Context | Key | Table |
|---|---|---|
| Field dialogue | `(room id, message index)`; room id = map id (+1000 for `fin_*` maps); a room a mod adds (not shipped, so not in `text_room_ids.csv`) uses its own map id (`FieldWorld::load_room`) | `<lang>.csv`, `Ex_<lang>.csv` (FR IT DE ES), `mess_wend.csv`, `mesgen_en.csv` / `mesgen.csv` |
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
4. Substitute every `mesgen` key with `value[speaker]` (EN: dialect tokens; JP: grammar forms; empty fields collapse), key by key in file order. Every occurrence is replaced, however many there are: each search starts again at the beginning of the text. A long EN line can carry 30 or more `{-}` (word space) keys.
5. `\n` -> LF, delete `{n/a}`, non-JP: normalise spaces and quotes. A missing row shows "MESSAGE NOT FOUND" and an empty window.

OpenCross: `LangTables` in `src/import/text.h` implements this. Without lang.dat it reads the PS1 files instead (section 12).

**Examples.** Room 27 (`alu_a01f`) message 15 (Arni elder), EN: key `(27,15)`, three pages (`{bpage}`), `{wait(20)}` matches JP `04 14`; `{SERGE%}` is a `mesgen_en.csv` key whose value is the bare token `SERGE%`, which the EN bare-token table renders as the name. Room 22 (`opn_a01i`) message 1, EN speaker 1 (Kid): `{PARTY2%}:\n{Cap}what is◆ it,\nSERGE%?...`; FR uses `Ex_fr.csv (22,1)` column 1. Battle: `btel_en.csv` key `386<<16|1` (Radius tutorial). Menu: `menutest.txt` key for 「はい」 -> "Yes". `sysmes` `0x000001` -> "δBone" (δ = element-colour glyph).

## 6. RDE markup tokens

157 tokens exist. The engine must support, for field dialogue:

* flow: `{bpage}` `{bpage}\n` `{bwait}` `{mend}` `{wend(n)}` `{wait(n)}` `{speed(n)}`, LF.
* A malformed auto-close ends the text: nothing after it is shown, and its leading digits are the close delay. `mess_wend.csv` row (186, 1), Lynx in jac_b36e, is `{wend(160})` in every language; RDE shows the text up to the token and drops the `)`, and in a test `{wend(30}Z\nQ` showed neither the `Z`, the line break nor the `Q`.
* style: `{color(0..4,GRAY)}` (0 white, 1 blue, ...), `{italic}` `{italic_end}`, `{66}` `{99}` `{♪}` `{♪♪}` (curly quotes, notes).
* names: `{SERGE%}`..`{PIP%}` (id = token index), `{SERGE%1}` / `{SERGE%2}` (JP: first 1 / 2 characters), `{RAZ%}`, `{PARTY0%}`..`{PARTY2%}`, `{playwork(0)}`, `{itemwork(0..3)}`, `{num(0..3)}`, `{HIDE}` (suppress next name), `{-y}` `{-i}` name suffix, `{Cap}` `{CAP}` `{CapL}` capitalisation.
* EN bare tokens (51): `SERGE%`..`PIP%`, `Sir SERGE%`, `Mister S_%`, `Herr SERGE%`, `The-SERGE%`, `◆` (marker, prints nothing), `''` (apostrophe).
* field sync: `{fset(n)}` `{fwait(n)}`, `{key(0..3,8,up,down,right,left)}` (button glyph, section 7.6.1), `{input(n)}` (number entry, variable 0x18), `{ERROR}` (placeholder).
* battle only: `{wmode W H}`, `{size W H}` (window size), `{playwork(0)}` `{itemwork(0)}`.
* key-item descriptions (`keyitem`): `{ButtonFast}` `{ButtonSlow}` (the Time Shifter row, every language), the key cap or pad glyph bound to the fast / slow speed action (`rules/menus.md` section 7). The row's lines are separated by `\n`.
* An unknown `{...}` is drawn as plain characters.

Placeholders: `{PARTYn%}` = party slot n -> character id; `{playwork(n)}` / `{itemwork(n)}` / `{num(n)}` read script variable `0x1A + 2n`; `{NAME%}` = fixed id. A save name record that still holds the JP default (kana) is "unset" outside JP. Names come from the custom-name map (name entry) or `sysmes[0x10000+id]`, truncated to 15 UTF-16 units; which of them a screen shows, and when Serge reads as Lynx (`sysmes[0x10031]`), is `docs/rules/menus.md` section 14.

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

In every language and art mode, RDE suppresses the PS1 glyph rasteriser and text primitives; every field, menu, shop, battle and name-entry string is drawn from spritefonts. The 1-bit JP font of section 3 is only what the PS1 itself drew; it is not part of RDE's classic look and is not drawn for any language. Played from the PS1 discs (no `font.dat`), OpenCross draws the PS1's own font instead (7.14).

**Original (classic) art swaps only the font files**: `data\font\<large|small>_font_<face>.spritefont` for the nine faces (kb kt gn bn ci cur ch ik gn_italic), same slots, offsets, scales, layout and colours; `ui_font_gnb_small.spritefont` for the PC UI object. With the small set every glyph's source rectangle is one texel narrower (`right -= 1`); advance and measuring use the full rectangle.

**The small_ files are pixel fonts** (the PS1 English release's look): one font pixel = 70/16 = 4.375 texels (runs of 4 / 5 texels in the masks); R = fill, G = fill plus a one-pixel outline ring, B = G plus a one-pixel drop shadow down-right (nested masks, as in the large files). At the usual 0.5 scale on the 1280 x 960 canvas a font pixel is 2.19 canvas units. Metrics differ from the large files (gn `A` small 61 x 83 at (11.5, 11.75) advance -20.5; large 64 x 87 at (11, 13) advance -24). `ch_100` and `cur_70` are byte-identical in both sets.

Field message windows use non-JP `gn_70` (slot 12; `gn_italic_70` slot 13 for italic glyphs) and JP `kt_70` (slot 11), scale 0.5, normal colour minus 0.085, outline = shadow = 0.13, in both art modes. OpenCross: `ui::font::FontSet` (`src/ui/ps1_font.h`) loads the slot faces for the current mode (14 slots; aliased slots share one face; `prefetch` reads a face file on a worker thread, the JP faces being large); `render::TextBatch` draws small_ faces with `SpriteFontOptions::narrow` / `pixel_pitch`.

### 7.3 Sampling the small_ pixel fonts

* **RDE** draws every glyph as a float quad (source rectangle at pen + offset * scale, no rounding) with a linear, clamped sampler, into the back buffer at the window's client size through the 4:3 game viewport. A font pixel is `4.375 * scale` canvas units, i.e. `35/16 * (viewport height / 960)` screen pixels at scale 0.5; no 4:3 height makes that whole, and the cells are stored as uneven 4 / 5-texel runs, so RDE's own output is uneven at every window size (at 1280 x 960 a font pixel comes out 2 or 2.5 px wide with half-intensity columns).
* **Grid analysis**: in gn / gn_italic / bn / ci / ik the R / G / B masks are binary (0 / 15) and every run is a whole number of 4.375-texel cells within 0.35 cell, on one grid per face: `(yOffset / 4.375) mod 1` = 0.57 +- 0.06 for every glyph, so all glyphs share the baseline row (gn: 21.6 cells below the pen line). Exceptions with smooth glyphs: Greek alpha..delta and U+2640 / U+2642 in bn, and all of `ch_100` / `cur_70`.
* **OpenCross rendering.** `TextBatch` with `pixel_pitch = size / 16` decodes each grid glyph to one texel per font pixel (the centre texel of each cell of each run) and draws it with nearest sampling at **n = round(4.375 * scale * px-per-canvas-unit)** whole screen pixels per font pixel (at least 1; below 0.75 px the source rectangle is drawn as RDE does). Layout keeps RDE's: pen advance, measuring, alignment and shrink-to-fit use the full float metrics; each glyph is centred on its RDE box on x and hung from the face baseline, which is snapped once per pen line. The narrow rule does not apply to decoded glyphs. Smooth glyphs keep the linear draw with the narrow rule. In original art the small kb file (numbers on most screens, all Japanese text) is laid out with the large kb file's metrics (pen advance, x offset, measure), its own glyphs drawn from the large glyph's box left; the other small files keep their own layout. OpenCross: `render::SpriteFontOptions::layout`, set by `ui::font::FontSet` from `ui::font::load_metrics` (metrics only). A run drawn with `TextStyle::exact` (text that RDE scales down to fit a width, such as a long Spoils item name) takes RDE's exact scaled size instead of whole pixels.
* **Size rule.** n = round(pitch) is used only while within 10% of RDE's pitch (`kPixelSizeTolerance`, `src/render/text_batch.cpp`); otherwise the glyph is drawn RDE's way (exact float size, linear source rectangle, narrow rule). The choice is per draw (scale x window size). At 1280 x 960 every main-menu scale draws like RDE; 0.5 text (dialogue, n = 2) keeps the whole-pixel decode.

### 7.3.1 Where the message canvas lands on screen

RDE draws its 1280 x 960 canvas into the game viewport: the 4:3 fit of the window (h = min(window height, window width * 3 / 4), centred) less a band of 8/960 h at the top and at the bottom. Over the field and the world map (both a 216-line display) the canvas is also scaled by 928/960 about its top, so canvas 960 is display line 216 of the 224 shown. Screen x = (W - 1280 sx) / 2 + x sx with sx = h / 960; screen y = (H - h) / 2 + 8 sx + y sy with sy = sx * 944/960 * 928/960 (about 0.95 sx: the canvas is drawn flatter than it is wide). Measured with opa_a02f message 0 in the fixed window at 1280 x 800, 1280 x 720, 1024 x 768, 960 x 600, 900 x 900, 1920 x 1080, 2560 x 1080 and 1600 x 1200: glyph pairs on one capture give sy / sx = 0.950..0.952, and the frame corners of OpenCross and RDE coincide within 0.1 px at every size. RDE has no widescreen mode; OpenCross's widescreen leaves the message canvas on the centred 4:3 viewport. The HD world name and end card (7.10), the battle results screen (`rules/battle.md` 8.4), the shops and the party screen land on the same canvas. The main menu draws the same 4:3 viewport less the bands, but its 960 canvas rows fill the whole band-less height (no 216-line squash): sy = sx * 944/960 (`render::menu_fit`, `MainMenu::Impl::draw_view`). Its list rows (item and key item names, the Customize rows) are drawn at the 0.35 text scale, and a line too wide for its room is shrunk to exactly fit it. OpenCross: `render::overlay_fit`, used by `DialogueBox::draw`, `ui::WorldName` and `BattleResults::draw` (the view's y unit is sy / sx pixels, `SpriteBatch::set_view`, and the text snaps through `TextBatch::set_pixel_grid(1, sy / sx)`). The field picture keeps its own 236-row zoom (`formats/room.md`), and the menus, title and other canvases keep `render::canvas_fit`.

Further screen comparisons with the same captures (OpenCross - RDE, px):

* **Portrait.** With the portrait on the left (window flags 0x1000) and on the right (0x800), message 0 with Poshul's face at 1280 x 800, 1920 x 1080, 900 x 900 and 2560 x 1080: the frame's top corners match to 0.1 px, the bottom corners (where the corner search was reliable) to 0.8 px, and the text ink box to 1 px. The face itself was not located separately.
* **Text.** EN: the ink box's left, top and right edges match within 1 px at all eight sizes; its lowest row ends 3 to 5 px higher in OpenCross (the box takes in the page leaf and the descenders; not resolved). JP (message 0, fixed window): the frame is 2.2 px (1280 x 800) / 3 px (1920 x 1080) lower in OpenCross and the text 14 / 19 px lower: RDE puts the JP text origin 18 canvas units above the EN one (glyph y 155 against 173.3 for the same window) and the JP frame top 2.8 units above, which OpenCross does not model yet.
* **Choice indent.** RDE's glyph table with a choice list (`E1`) and the lines `   Yes` / `   No`: the first space advances 62, the option text starts 98 units in (y 250 against a text origin of 152), as in section 7.6. The hand was not compared on screen.
* **Page leaf, world-map labels.** Not compared on screen. On the world map RDE's display is also 216 lines, so the label windows take the same canvas placement.

### 7.4 Pixel-font text in a PS1-resolution menu target (`[Video] Ps1ResUi`)

With Ps1ResUi, menus are drawn into a target of the PS1 overlay's display size (main menu 512 x 448, title / save list 640 x 448, shop / name entry 512 x 432, memmenu 640 x 216, key-item list 320 x 216). The text keeps RDE's small_ faces, slots, scales and positions; `ui::LowRes` sets `TextBatch::set_pixel_grid(W / 1280, H / 960)` (target pixels per canvas unit). Nearly every menu font pixel is then smaller than a target pixel (list labels 0.35 -> 0.61 x 0.71 target px; card/equip 0.3125 -> 0.55 x 0.64; numbers 0.275 -> 0.48 x 0.56; key-item list 0.55 x 0.49; memmenu labels 1.09 x 0.49; title menu 1.31 x 1.23), so nearest sampling would drop strokes and one target pixel per font pixel would make glyphs 1.6x too wide.

Choice: every glyph is drawn at RDE's exact size and pen position through the **box filter** of `SpriteBatch` (`SpriteFilter::Box`: 4 x 4 point taps over the target pixel's footprint in the decoded one-texel-per-font-pixel atlas, averaged), an area-sampled rendering at PS1 size, then nearest-upscaled with the rest of the screen. Backdrop and fill passes are filtered separately and composited, as in RDE. Only the baseline is snapped (once per line); x stays exact. Where a font pixel is within 10% of a whole number of target pixels on both axes, the glyph is placed on whole target pixels instead. Smooth glyphs take the box filter on their source rectangle. With Ps1ResUi off, text uses RDE's window-resolution draw of 7.3.

Menu glyphs the PS1 drew from its own sheet but OpenCross still draws from RDE font faces in classic + Ps1ResUi: the main-menu bar star, the element prefix glyphs alpha / beta / gamma / delta, and the Lv / HP marks. Button glyphs (L1 / R1 / O / X) are the exception: `MainMenu::Impl::btn` draws the disc's own glyph, frame `0x7C + slot` of the menu sheet in `menutest.prd`.

### 7.5 Which face draws a JP string; glyph coverage

* **Coverage** (the small_ files match): `bn_70` has 217 glyphs = ASCII + Latin-1 + Œœ, αβγδ, ’“”„, ←↑→↓, ★☆♀♂♪♫・ and **no kana or kanji**. `ik_70` has 290 = `#%x`, all hiragana / katakana, the 64 kanji of the menu labels (上下中久先具分初力化取右向命器回売天字定属左御復性抗撃攻数方期正武毒決法率確耐腕英解買身転軽造避邪鍛防風魔), full-width `＋０-９：Ａ-Ｚａ-ｚ`, and **no ASCII letters or digits**. `kb_70` / `kt_70` have the full set. A string drawn with a face lacking a glyph shows `#`.
* **RDE's slot rule.** Every string list picks its slot from the *effective* language `L' = flags bit 16 ? 5 : bit 17 ? 0 : L`. Default slot: `L' == 5 ? 0 (kb) : 4 (bn)`. The "slotL" styles (menutest 2 / 3 / 7 and 0x64-0x67, shop 7, memmenu 0x82 / 0x83, btlexit 9) use `9 (ik)` in JP else `4 (bn)` (menutest 0x64-0x67 test the global L, the others L'). Fixed slots stay fixed (menutest 0x84 = bn, 0x78 / 0x90 / 0x91 = kb, title 0x80 = ci). Bit 16 is set by RDE's caption tables for untranslated JP text and cleared when a cmnmes lookup of the JP key hits; bit 17 (the frame captions) stays set, so those captions are bn in every language (their JP cmnmes column is Latin: "Weapon", "Armor", "Elements", ...).
* So in JP, menu labels and list entries are ik or kb, never bn; the screen titles from the exe table (Latin in JP too) and the frame captions are bn. The smith panel title uses the keys "Weapon" / "Armor" / "Accessories".
* **Layout audit (debug).** `OC_TEXT_AUDIT=1` makes `render::TextBatch` record every string's glyph boxes and log `text-audit:` lines for glyphs missing from the face, strings whose glyphs overlap another's (inner 70% of each box), glyphs crossing a registered frame edge, and off-screen text. `ocmenudemo --shots` / `ocshopdemo --shots` report per shot; the game logs each new issue once (`=2` adds a sample frame every 300).

### 7.6 Choice lines (in-window MULTI_CHOICE)

* **Marker.** A choice option is a message line that starts with three spaces (`　　　` U+3000 in the csv; non-JP text has U+0020 after normalisation). `E1 0xFL` names the option lines F..L (`docs/formats/fieldscript.md`). The PS1 has **no per-line condition code**: option lists that depend on the party are separate messages and the script picks one (tel_b02f painter: message 37 = `{PARTY0%}`, `{PARTY1%}`, No thanks; message 38 adds `{PARTY2%}`; the script reads party slot 2 and `== #255` selects 37).
* **Result** = the cursor's line number in the message (F..L), stored on confirm in the entity and copied to script variable 0x04 by `E2`. The cursor starts at F and wraps between F and L.
* **Indent width (RDE).** The `E1` hook turns on choice mode; the layout then flags a line whose glyphs 0..2 are spaces and, in non-JP text, lays the first space out with base width 70 instead of 20: advance = 2 * w0 * 0.45 canvas units at glyph scale 0.5, so the three-space indent counts 63 + 18 + 18 = 99 canvas units of line width; on the pen grid (7.13) the first space moves the pen 62 units, so the name starts 98 units in, and the 70 x 70 hand (drawn at the line's first glyph) sits in front of the name. Flagged lines are never word-wrapped. Only the indent is widened; other spaces keep w0 = 20.
* **OpenCross.** `DialogueBox::set_line_choice` (called by `E1` after `E0` opened the window) lays the open message out again with `LayoutMetrics::choice_space_first / choice_space` (`ui::layout_pages`; page, revealed glyphs and waits kept). The result is `line_first + cursor`. Routes `painter`, `choice_names`.
* **Auto-close messages.** A choice list inside a `{wend(n)}` message (the Viper Manor guards' password list, jac_b22i message 5, `E1 03`, ends `{wend(254)}`) is up while the typewriter sits in `Closing`, not `WaitClose`. The hand, the highlight and the confirm all work in `Closing`; the timer still closes the window and the result is then unwritten. Route `y2_password`.
* **Line mapping between translated text and the disc message.** The `E1` rows `F..L` and the stored result are line numbers of the **disc (PS1) page**, but translated text is laid out differently (the EN text of tel_b04f message 95 has one question line where the disc has two). RDE highlights and draws the hand on `max(0, translated first choice line - disc first choice line + row)`, with `row = F + option index`. The translated first choice line is the first line of the page whose first three glyphs are spaces (`DialogueBox::first_choice_line`); the disc first choice line is the newline count (page-relative, a newline at entry 0 not counted) at which the page's third U+3000 glyph of the disc stream appears (`DialogueBox::guest_choice_lines`, read from the room's message block; the last translated page uses the last disc page, since page counts need not match). The pick written to variable 0x04 stays the **row** (`F + index`). `occ dialogue-choices` counts 165 message x language cases that need the remap (5 in EN: tel_b04f 95, gal_a10i 0, tel_b16i 2, hid_a04f 13, wld_b00 8). Routes `termina_drift(_orig)`, `choice_remap`; test `test_text` (`test_choice_lines`).
* **Text-data defects identical in RDE.** A translated list with fewer option lines than the `E1` range (es tel_b02f 20: two of three options; es tel_b24e 90: the first option merged into the question line; fin_29 19) leaves the last option without a line (no hand, never highlighted, still pickable by index). jac_b01f 39 has an empty fourth option line in every language and on the disc. The hand is also drawn in a frameless (label) window.

#### 7.6.1 Button glyphs (`{key(n)}`, PS1 control code 1B)

The argument names a pad function, not a picture: 0 confirm (the PS1's circle), 1 cancel, 2 the key-item button (square), 3 the menu button (triangle), 8 Start, `up` / `down` / `right` / `left` the d-pad (0x10..0x13 in the PS1 byte stream). RDE lays the token out as one placeholder glyph, a no-break space (a plain space in JP), which counts w = 40 when a space or no-break space follows and 55 otherwise (layout width 0.9 w canvas units at glyph scale 0.5, placed on the pen grid of 7.13; in JP the placeholder is an ordinary glyph, 49 wide), and draws the bound button over it: with the keyboard the key cap of the function's first key (`ui/keyboard_<e f i g s j>.png`, the action's key, Space for Start), with a pad the pad glyph (`ui/gamepad.png` cells 8 A, 9 B, 0xB X, 0xA Y, 13 Start, 0..3 d-pad up / left / right / down). The picture is a 45 x 45 canvas-unit square at the placeholder's pen x, 5 units below the top of its line (opa_a02f message 4 against RDE at 960 x 600: a 28 x 28 px cap at the text origin). OpenCross: `ui::button_placeholder`, `LayoutGlyph::button`, `DialogueBox::draw_button`; the field shows key caps after the last key press and pad glyphs after the last pad press (`FieldWorld::key_caps_`).

### 7.7 Window size of short messages

There is no 3-line minimum. The frame's row count is `rows = lines + 1`, where `lines` is the tallest page's line count (it starts at 1): a one-line message has 2 rows, a two-line message 3 rows; only a portrait forces `rows >= 5`. The PS1 header's `lines` / `px` fields are not used. Example: `!{wend(60)}` (dem_02 message 0) gets a wide, one-text-line frame of 2 tile rows. With skin 8 and 2 rows: bottom corner at `last + 0.15 TH - 0.17 TW` (no portrait), corner sheets T0 / R0 v0 = 0.004, B0 / R24 v0 = 0.43.

**Width.** `W` = the widest line of any page as the layout counts it: the sum of its glyphs' layout widths (7.13), not the distance to the last glyph's pen. Nothing else enters: no table width field, no padding, the button glyph only as its own 0.9 w. Placement from the text origin `x` (the first glyph's pen, canvas units): the frame base line `b = x + 10` (`x + 23.5 - 270` with the portrait on the left; JP `x - 6`, `x - 276`), its left edge `X0 = b - 40` (JP `b - 25`, `b - 5` with the portrait on the left), the top-left corner piece at `X0 - 1.28 TW`, and the right corner piece at `b + W + 36` (`b + W - 4` when the message has three or more text lines), `+ 253` with the portrait on the right (the face's room), `+ 233` with it on the left; the fill tiles run to the right corner. Sizes on the 1280 x 960 canvas (TW 61.75, TH 66.5, no portrait, default skin): one text line = 286 high, three lines = 409; width = W + 321.3 (W + 281.3 from three lines on). RDE's frame records for opa_a02f with no portrait: right corner `b + W + 36` for one and two lines (messages 3 and 4, EN and JP), `b + W - 4` for three and four lines (message 0, EN and JP, fixed window and placed window). RDE (opa_a02f, HD art, 1280 x 800 window): `{key(0)} to jump.` W = 225, frame corners 380.04 apart; a one-line page of 687.15 (message 3) and Poshul's three-line message with the portrait on either side (W = 571.5) match these rules to a unit, as do the JP messages (W = 294, 882, 539).

OpenCross: `DialogueBox::frame_geo_for`; debug `[Debug] ShowFlags` opens the `ShowMessage` window with given window flags (2 = the fixed window).

OpenCross: `DialogueBox::layout` starts `lines_` at 1 and the frame uses it directly; `DialogueBox::frame_size(skin, W, lines)` is the pure helper; `test_text` `test_frame_rows`. `occ dialogue-sizes` reports the blast radius: of 12,373 EN room messages, 1,274 are 1-line and 3,746 are 2-line.

### 7.8 Typewriter pacing and window opening

OpenCross: `ui::layout_pages` splits a flattened message into pages (`{bpage}` boundaries) with a per-page event list (`Delay`, `Speed`, `Wait`, `FWait`, `AutoClose`, `FSet`, each firing before glyph `at`); `ui::Typewriter` reveals it in 1/60 s ticks.

* **Speed.** A glyph accumulator gains `speed / 64` per tick (default 64 = 1 glyph per tick; `{speed(0x80)}` = 2 per tick). `{speed(255)}` reveals the whole page at once. The speed is set at the start of a message to the default and persists across its pages.
* **Waits.** `{wait(n)}` pauses n frames and `{wend(n)}` closes the window n frames after the last glyph, both counted at 30 Hz script frames. `{bwait}` waits for confirm inside a page; `{bpage}` waits for confirm and turns the page; the last page waits for confirm to close. `{fset(n)}` writes n to script variable 0x22 when the reveal reaches it; `{fwait(n)}` holds the reveal until that variable equals n.
* **Confirm.** While revealing, confirm fast-forwards to the next wait (delays are skipped, `{bwait}` still stops); in a wait state it continues; in the auto-close countdown it closes at once.
* **Opening.** A field message window grows from nothing to full size in 16 ticks (scale k / 16 on the k-th tick); text, its waits and a number entry start on the tick after it is full and confirm presses before that are dropped. Window and text are not drawn while it grows and fade in over the next 3 ticks. Closing has no animation.

### 7.9 Window skins

Field message windows use the PS1 skin sheets `map/window/wN.tim` (8bpp 128 x 96 TIM: frame pieces at the top, the 16 x 16 page-leaf / hand frames at v 80, u = frame * 16; colour 0x0000 transparent); HD art draws the 4x redraws `hd/window/wNN.png` with the same normalised UVs. The option byte of the Custom Window Frame setting picks the sheet: byte 0 -> w4, 1..3 -> w1..w3, 4..14 -> w5..w15; menus use w17. A window is a 7 x 4 grid of fill pieces plus corner, top / bottom and left / right edge pieces and the page leaf (close triangle, bottom right, its u offset steps with a phase 0..4); `ui::build_skin_window` (`window_frame.*`) returns the quads in canvas units (1280 x 960) and `DialogueBox::draw_tiles` / the key-item list use the same piece model.

### 7.10 World name

In HD art the world-map rooms show the world name as `map/HomeAnother/{home,another}_<lang>.png` from hd.dat, stretched over (20, 7)..(330, 207) of the 1280 x 960 canvas, placed as over the field (7.3.1, flatter than wide). It is shown while the map has a world name (script variable 0x50) and the World Names option (variable 0x52) is on, over any window, fading in by 0.0625 and out by 0.065 of full opacity per 60 Hz frame. Home World maps are 0x8F, 0xFA, 0xFC, 0xFE, 0x102, 0x1BB, 0x213; every other map is Another World. The PS1 draws the same name as a screen-fixed layer group of the world-map rooms. OpenCross: `ui::WorldName`.

**End card.** In HD art RDE also draws `map/Fin/fin.png` (a "Fin" signature) over (975, 830)..(1130, 930) of the same canvas (placed as in 7.3.1) on the ending room's THE END screen (map 9, entrance 2, reached after the `final` movie, and the screen the other endings jump to). In widescreen the card stays on the centred 4:3 canvas, over the place of the room's own "Fin" tiles (screen-fixed groups drawn at their 4:3 position); only the world name moves out to the window's left edge. It fades in from the room's load by 0.0125 of full opacity per 60 Hz frame (80 frames, about 1.3 s) and stays; the first fade to black over 64 ticks (the screen's key press, `FADE 1,#64`) fades it out by 0.015625 per frame (64 frames), and the fade back for the save prompt does not bring it again. RDE takes it down only once it has reached full opacity; the room's script reads the key only about 440 frames after the load, so the card is always full by then and OpenCross does not test it. The room's own fades do not dim it (RDE capture: its luma falls linearly with the card's alpha while the screen fades). While the card is up RDE also hides background tile groups 0 and 1 of the room (the PS1's own "Fin" picture), so only the card shows; it hides them only on the screen the ending's script leads to (a save loaded straight into entrance 2, which the game never writes, shows both). Classic art and the PS1 have no card and keep the room's picture. RDE steps both fades, and the world name's, once per presented 60 Hz frame of its own loop, not per game tick: its fast and slow modes leave them unchanged (RDE capture: 79 steps of 0.0125 in 1.31 s with either mode). OpenCross keeps the card as data, `import::room_cards` (`src/import/room_cards.*`: room, entrance, picture, rectangle, hidden groups, fade rates and the length of the fade to black that takes it down); `FieldWorld::room_card` is the row of the loaded room entrance until that fade starts, and `WorldName::draw_room_card` draws it. The world name and card fades count the app's 60 Hz frames (`App::screen_clock`: real time, one frame per route step), so `FastMode` / `SlowMode` do not change them and a route sees them per route tick at any `ScriptSpeed`.

### 7.11 The PS1 field glyph path (RDE does not draw it; disc mode does, 7.14)

The PS1 expands each glyph's 11 rows into one bit-plane of a 16 x 12 4bpp cell uploaded to VRAM. A glyph counter picks the plane (`& 3`), so **four consecutive glyphs share one cell**, and each glyph sprite uses a CLUT chosen by that counter. The four CLUTs (VRAM x 0x380 / 0x390 / 0x3A0 / 0x3B0, y 0xF8) are bit-plane selectors: entry i = 0xFFFF if bit k of i is set, else 0x0000. The colour is the sprite's RGB (0x80 = 1.0, so default text is pure white); there is no outline or shadow plane in the cell, but the screen shows every glyph over a pure black copy of itself one pixel down and right (7.14). The USA build runs the same path with its own glyph table and pen advance (7.14). The memmenu name uses plane 0 only (6 cells at VRAM (0x3C0 + 3 i, 0xF3)) and fakes weight by drawing the strip three times (x 66 / 67 / 69, colours 0x64 / 0x64 / 0x40).

### 7.12 Word wrap

Translated text wraps by the width of the disc message it replaces (`ui::wrap_limit`, `ui::layout_pages`). Widths are in layout units at glyph scale 0.5.

* **Disc line width.** The widest line of the disc message in characters (`ui::disc_line_chars`): every glyph counts 1, a typed-name or item token (07, 08, 15, 18) counts 6, other control codes count nothing, and a confirm wait (03) ends a line as a newline, a page turn or the end does. It need not equal the record header's width field.
* **Glyph width.** A glyph counts `0.45 x (measure - 10)` (`ui::wrap_glyph_width`), where `measure` is the sprite font's box for that one character, `max(0, xOffset) + width`, read at 0.8 for an italic glyph; a blank (a space with an empty box) counts 18. The same width is the glyph's pen advance (7.13), not the DirectXTK advance.
* **Limit.** `min(62 x width, 925)`; 1000 when there is no disc message. It is raised to the message's widest word (spaces and newlines end a word, a page turn does not), so a word is never cut. It does not depend on the window or the aspect ratio.
* **Walk.** A page is walked in reading order. A space that is not the page's first entry is remembered as the break. Before each entry the width reached so far is compared with the limit: when it is past the limit and a break is remembered, the space becomes a newline and the walk continues from there, so the last glyph of a page may overhang and a space that crosses the limit breaks before the word in front of it. A line that has a choice indent (7.6) never breaks. Typed names take part as ordinary glyphs.
* **Listed messages.** Some translations are longer than their disc lines and are shown as authored: room 387 (`zel_a13f`) per language, en 6 10 11 13 14 16, fr 8 12 13 16 17 26 28 32 39, it 16, de 6 10 11 13, es 6 10 11 16 17 27 34 40 48, and in English room 414 messages 19 21 23 and room 442 message 33. Rooms 1490 messages 1 3 and 1492 messages 0 1 are measured with a width of 30.

### 7.13 Glyph placement (pen grid)

Message windows do not place glyphs at the sprite font's advance. The layout gives every glyph a width and keeps two sums per line: the line width, the plain sum of the widths (the frame and word wrap use it), and the pen, an integer in the 640-unit grid of the PS1 screen (2 canvas units): each glyph sits at the pen, then `pen = trunc(pen + width / 2)`. The fraction of a grid step is dropped at every glyph, so glyphs drift left of the summed width along a line (`{key(0)} to jump.`: glyphs at 0 36 54 70 94 112 124 146 180 204, line width 225). The draw puts each glyph at its pen; the button picture of 7.6.1 is drawn over its placeholder at the same x.

* **Widths, non-JP.** `0.45 x (measure - 10)` (7.12); a blank 18 (italic 14.4); a button glyph 36 before a space or no-break space, else 49.5; the first space of a choice indent 63 (7.6). The face is the window face of 7.2 at glyph scale 0.5.
* **Widths, JP.** A Latin letter, `-`, `[` or `]` counts `0.45 x measure` (no padding taken off); a half-width digit (`0`-`9`, the superscripts one, two and three; the digits of `{num(n)}` and of a number entry) counts 28 and advances 14; every other glyph (kana, kanji, full-width digits, spaces, the button placeholder) 49, so it advances 48 (`ui::jp_glyph_width`). Character names are inserted into the text and laid out like any other text: a katakana name advances 48 per glyph (RDE, opa_a02f message 0 with the JP default names: ポシュル at 616 664 712 760); RDE's `コ1３枚` sits at 952 1000 1028 1076.
* **Number entry, JP text.** The JP csv writes an entry of n = 1..4 digits as `{input(n)}0123456789` followed by the n placeholder digits; the digit row belongs to the token and is not shown (`import::parse_markup`).
* **OpenCross.** `LayoutMetrics::glyph_width` and `pen_grid` (`ui::layout_pages`): `DialogueBox::layout` sets the grid to 2 for every language; the page width is the widest line width. Test `test_text` `test_pen_grid`.

### 7.14 The PS1 font in disc mode

Played from the PS1 discs there is no `font.dat`: every font slot of 7.2 draws the PS1's own 1-bit font, the one the field message windows draw (7.11), in both builds.

* **Glyphs.** A glyph is 11 rows of one u16 little-endian, bit 15 = leftmost pixel, 22 bytes, in the main executable `prog/slps_023.64` (each build's own copy of that record). Japanese build: file offset `0x46F0C`, indexed as in section 3. USA build: file offset `0x46FD0`, 191 glyphs, glyph i = the single-byte code `0x20 + i`. The USA glyphs are up to 11 pixels wide (`A` is 5); the atlas cell is 17 x 12 (the glyph's 16 x 11 and a blank column and row, so a sample at a cell's edge never reads the next glyph), and a font pixel always covers a whole number of target pixels, at least one, so no glyph column or row is dropped at a small scale.
* **Pen advance.** Japanese: 8 pixels for single bytes up to `0x74`, 12 for the rest (section 3). USA: the u8 at index `code - 0x20` of the 256-byte width table, disc record 2059 (no name in the index; the game copies it into memory unchanged): 2 to 13 pixels, the glyph's ink plus its spacing (`A` 7, `I` 3, `W` 9), 0 from index 191 on (section 11.2). A window mode can draw letters as capitals: it maps `a`-`z` to `A`-`Z` and the accented lower-case letters to their capitals before the lookup.
* **USA glyph order** and code points: section 11.2 (`tools/py/data/usa_glyphs.tsv`). OpenCross keeps it as a data table (`import::Ps1Font::usa_char`, U+E000 + the code for a symbol with no Unicode form); a character several codes draw is drawn with the first of them, except the comma, which the text writes as `0xDB`.
* **Lines.** 13 pixels apart in both builds; a new line returns the pen to the window's text origin. The PS1 does not word-wrap: the disc text carries its own line breaks.
* **Look.** White glyph pixels (sprite colour 0x80) over a pure black copy one pixel down and right (7.11). Field capture, Japanese build (`tel_b11i`, Pancliff, three text lines): glyph tops at y 52 / 65 / 78 / 91 of the 320 x 240 picture; the emulator shows the picture about 0.93 as wide, and at that scale 68 of the 73 white pixels of the name line are the executable's glyphs at the 12-pixel advance, 511 of the 546 pure black pixels of the text area sit one pixel down and right of a glyph pixel. USA build (room 22, the opening dream, message 1 "Kid: / Oi! / C'mon Serge, / Leena!" with portrait): glyph tops at y 31 / 44 / 57 / 70, text origin x 142; the emulator shows that picture 0.94 as wide, and at that scale 304 of the 325 white pixels of the text are those of the executable's glyphs placed at the width-table advance, every glyph pixel over a black one down and right.
* **Battle.** The battle HUD's words, digits and names are sprite frames of its own sheets, not this font (`rules/battle.md` 6.9); the battle text drawn as window text (element names on their plates, descriptions, battle messages, the Dopplegang and swap lists, script windows) uses this font at one font pixel per PS1 pixel, each line's glyph cells from the PS1 point the HUD gives, centred where the layout centres (tech titles, target names), no shrink to a width.
* **Menus.** The USA menus draw an anti-aliased sheet (section 11.2, menu sheet); OpenCross does not decode it yet and draws the 1-bit font there too.
* **OpenCross.** `import::Ps1Font` (`load_ps1_font` picks the USA build when record 2059 is the 256-byte table) decodes the font; `render::TextBatch::add_ps1_font` draws it nearest-sampled, one font pixel = 8 x scale view units on x (the message scale 0.5 = 4 canvas units = one pixel of the 320-wide field screen) and `pixel_aspect` times that on y, the shadow first; `TextBatch::advance` / `line_height` / `measure` give the PS1 metrics. `ui::font::FontSet` hands the PS1 face to every slot when the backend has no `font` archive. `DialogueBox` lays a PS1-face message out at the PS1 advance, with no pen grid, no word wrap and 13-pixel lines; field text pixels are 960 / 216 / 4 as high as wide (the field's 216 lines over the 960-unit canvas). Menus and other screens keep their RDE layout positions with the PS1 face. Dev view: `octext ps1font` (`docs/tools.md` 1.2). Test `test_ps1_font`.

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

`python -I tools/py/cc_text.py` commands: `langs` | `room <name|id> [lang]` | `coverage [lang] [--rooms --all --errors]` | `sysmes [lang] [--table T --cat N]` | `font` | `glyphs [--write]` | `render <room> <msg> <png> [--scale N]` | `fontsheet <png> [--first N --count N]`; on a USA disc image (`--disc <cue|bin|iso> [--disc2 ...]` before the command, default the USA cue sheets in the DuckStation games folders; section 11): `usa-glyphs [--write]` | `usa-room <name> [--en]` | `usa-tables [--cat N --verbose --write]` | `usa-menu [--verbose --write]` | `usa-compare` | `usa-fontsheet <png>`. The room argument is a map-file stem or the numeric id (ids 0 / 9 share `ending`, 1 / 8 `opening`, `fin_19` appears twice).

Unknown: the markup spelling of code `07` (ITEM; the importer's own debug dump writes `{item(n)}`); glyph `F98E`; the 159 low-confidence glyphs (mostly single-use rare kanji / symbols) deserve an eyeball check against the bitmap sheet; the speaker detection needs live `{PARTYn%}` / `{playwork}` state that the tool lacks; the kana / kanji ordering inside the font table is tied to data only; the trailing bytes of `ch_100`; the policy for debug-room `{ERROR}` text.

## 10. UI art sources

Three groups of UI art are decoded or cut by the engine itself. All disc reads use `Vfs::read_disc` (the disc image, never the HD override copies).

### 10.1 PS1 menu art (`src/ui/menu/ps1_art.*`)

Used by Original-art menus, shops, name entry and save/load with `[Video] Ps1ResUi`; callers sample it nearest.

* **`map/etc/systim.bin`** holds several TIMs back to back. The loader scans it at 4-byte steps for the TIM magic `10 00 00 00` and takes the one that is 4bpp with a CLUT and sits at VRAM (896, 96) with 16 x 64 halfwords: the **parchment tile**, 64 x 64 texels, CLUT row 0, colour 0x0000 transparent. Menus tile it 8 x 7. The file's other TIMs are not used by this code.
* **`menu/menutim.prd`** is a DRP; each entry is one TIM found by its four-character name (`menutim(fs, "wind")`, `mpla`, `icon`, `p1ic`..`pfic`, the colour sheets `mnbl` `mngr` `mnku` `mnre` `mnwh` `mnye` `mnni`). The **`wind` sheet** holds the window frame pieces (texel rects): 8 x 8 corners TL (19,1), TR (37,1), BL (19,10), BR (37,10); top edge (28,1) and bottom edge (28,10) as double lines (a one-column slice at x 31 is stretched along the edge); left edge (15,10), 3 px wide; right edge = the 3 right columns of TR (42,4). Lines are 3 px (dark, light, dark) and the interior is transparent. `frame_pieces` emits those pieces for any rectangle and can cut a gap in the top edge (caption tab) or the bottom edge.
* **Sprite sheet frames** (`sheet_frame`): frame n (1-based) of the `main` type-3 entry of `menu/menutest.prd`. A frame is a part list (`s16 x, y; u8 u, v; u16 CLUT; u8 w, h; u16 flags`, 12 bytes; texture page in the low nibble of flags, page half in bit 4, bit 9 / 10 = mirrored in x / y), drawn last part first from whichever `menutim.prd` 4bpp TIM holds that CLUT position and those texels, into a 512 x 512 image with the frame origin at (192, 192).
* **Faces** `face/<name>.tim`: one 64 x 64 TIM per character id 0..49 (name table `ps1::face_name`: `seljyu`, `kid`, `gill`, ... `yama`), decoded with CLUT row 0. Retail portrait strips are `menu/playtim/pNN.tim` (8bpp, 128 x 224). Key-item pictures are `menu/keyitem/<name>.tim` (8bpp 256 x 256, 255 x 255 drawn; ids 54..60 have none); save-box icons are `menutim.prd` `p1ic`..`pfic` (16 x 16, own CLUT).
* **Font fallback.** The 1-bit PS1 font (section 3) has no ASCII punctuation. `bitmap_text` replaces every character it lacks by the first form it does have: `"` -> curly quote then full-width `＂`; `,` -> `，` then `、`; `'` -> `＇` then `’`; any other printable ASCII -> its full-width form; curly quotes and dashes and `…` -> their full-width / JP counterpart (`⋯`, `ー`). Spaces and newlines pass; a character with no substitute is kept and the caller's HD face draws it.

### 10.2 Play-time feature icons (`src/ui/feature_icons.*`)

* **Sheet:** `ui/exticon_<lang>.png` from `ui.dat` (`ui/exticon.png` for JP and as fallback), 256 x 256, a 4 x 4 grid of 64 px cells (drawn 53 x 53 with a 0.001 UV inset). Row 0: column 0 encounters off, 1 auto battle, 2 battle boost. Row 1: the AUTO SAVE strip across the full width. Row 2: column 0 fast, column 1 slow. Row 3 is unused by this code.
* **Placement** (1280 x 960 canvas): the column starts at x 20, its first icon's bottom edge at y 877, and grows upwards 63 units per active icon in the order auto battle, battle boost, encounters off, slow, fast. When the window leaves more than 75 canvas units of black beside the 4:3 picture, the column and strip move 93 units outward into the bar.
* **AUTO SAVE strip:** 212 x 53 at x 83 beside the first slot, 80 % opaque. A 120-count starts at each completed auto-save and runs down once per 60 Hz frame; with no toggle on it runs down twice per frame (about 2 s with icons, 1 s without).
* **When shown:** icons draw over the finished frame (under the pause screen) on every screen from the first one on: the boot logo, movies, the title scene, its slot list and name entry, the field and battles; they follow the Cheats toggles (auto battle, battle boost, no encounters) and the slow / fast modes (slow and fast hidden while the speed is locked), and hide with the free-camera overlay hiding. With `[Enhancements] HideFeatureIcons` (default off; the Esc settings' General tab, "Hide cheat and speed icons") no toggle icon is drawn while the toggles stay in force; the AUTO SAVE strip still shows, counting down as with no toggle on.

### 10.3 User guide screen (`src/ui/keymap_screen.*`)

* **Trigger:** every New Game shows it between the title menu and name entry (the game is paused and the frame under it is black). Dismiss: any key except F1..F12 (or Space), or the pad going from some button or stick (>= half way) held to none held; L3, R3 and Start do not count. A latched press during a fade is used when the counter reaches the next wait.
* **Art:** two 1620 x 1215-space pages `hd/userguide/steampad_<lang>.png` ("Controller Controls") and `steamkey_<lang>.png` ("Keyboard Controls"), suffixes en fr it du es jp; glyphs from `ui/gamepad_assign.png` (14 x 4 cells of 96 px; glyph rows are the odd rows) and `ui/keyboard_<l>.png` (16 x 7 cells of 48 px, suffix e f i g s j). Both art modes use the same pages; if either page is missing the screen is skipped.
* **Timing** (counter steps at 60 per second): page 1 fades in over 15 steps and waits at 15; 15..30 cross-fades page 2 over page 1; waits at 30; 30..45 fades page 2 to black; 45 closes.
* **Key caps.** The cell of a virtual key is `platform::keyboard_cell(vk)` (column `cell & 15`, row `cell >> 4`; 0 = no glyph). Each action has two keys (`platform/keymap.h`: set 0 arrows / Enter / Backspace / F / I / H / J / F1 / F2, set 1 W S A D / X / Left Shift / R / Tab / Q / E, none for the speed modes). Move rows draw four caps 52 units apart from x 110 (set 0 at y 247.5, set 1 at y 301.5); two-key rows draw set 0's cap then set 1's 52 units to the right; any unbound key shifts its row half a step (26) right. Fixed caps: Space (pause), K (change lead), F3 (no random battles), F4 (battle boosts), F5 (auto-battle), Esc (ESC menu). The controller page is a fixed picture of 20 glyph cells at fixed positions.

Not determined from this code: the contents of `systim.bin` other than the parchment TIM, and the full virtual-key to cell table (`keyboard_cell` in `src/platform/keymap.cpp`).

## 11. USA build text (PS1 discs)

The USA discs carry English text in the same containers as the JP build: the message block of section 1 in subfile 8 of every room `.bin`, part 1 of every battle `.pac`, and string tables in the executable. RDE has none of it; its English (`en.csv`, `sysmes.txt`, ...) is keyed by the JP files. Tool: `cc_text.py usa-*` (section 9) reads a USA disc image. Data: `tools/py/data/usa_glyphs.tsv` (glyph table), `usa_tables.tsv` (string-table map), `usa_menu_strings.tsv` (menu keys).

### 11.1 Message stream

The block layout, record header and terminators are those of section 1. Message counts differ from the JP block in 118 of 563 rooms (the USA field scripts were edited): most rooms end with three extra debug messages, a few are renumbered (`opa_a04f`, `you_b01f`, `des_a06f`, `inj_b03i`, ...). USA text therefore goes with USA scripts; a USA message index is not an `en.csv` key.

* **Glyphs are single bytes.** Every byte `>= 0x20` is one glyph; there are no page prefixes. Codes `0x20..0xDE` have glyphs (11.2).
* **Control codes** keep the codes and argument lengths of section 2 (`07` 2 bytes; `04 05 06 08 0A 0B 0E 10 12 15 16 17 18 19 1B 1C 1E` 1 byte; the rest none). Changed or new meanings:

| code | name | USA behaviour | RDE markup |
|---|---|---|---|
| 08 n | NAME | `n < 0x2C` name of character n (the Lynx-body rule of section 6 applies); `0x80 + k` the first k glyphs of Serge's name; `0x83` the first 3 glyphs of character 13's name ("Raz"; the JP build takes 2) | `{NAME%}`, `{SERGE%k}`, `{RAZ%}` |
| 0A n, 0B n | NUM | value of script variable `0x1A + 2n` in decimal, up to 5 digits, leading zeros dropped. `0B` uses the digits `0x28..0x31` and minus `0x23`; `0A` uses the second digit set `0xD0..0xD9` and minus `0xF6` (no glyph) | `{num(n)}` |
| 0E n | COLOR | colour table of 9 entries (r, g, b at 0x80 = 1.0): 0..7 as JP (0 white, 1 blue, ...), 8 grey (70, 70, 70) | `{color(n)}`, `{color(GRAY)}` for 8 |
| 10 n | DIGITS | as JP; the message carries the row `0123456789` (`0x28..0x31`) after the code, then n placeholder digits | `{input(n)}`; `en.csv` drops the digit row |
| 12 n | DIALECT | inserts the speaker's form of a word or sound (11.3): string table `n + 6`, row `column(speaker) + 1`, where `column` is the dialect column of the character id of the entity that owns the window; for `n = 0x20` the row is `character id + 1` (what the speaker calls Serge). `n` runs 1..0x30 | the `mesgen_en.csv` key of 11.3 |
| 1C n | CASE | `n = 0`: the next glyph drawn is upper case; `1`: every following glyph of the message is upper case; `2`: as 0, only when the speaker is character 35 (Leah). The mode waits through control codes, so it reaches the first letter of a following name or dialect string | `{Cap}`, `{CAP}`, `{CapL}` |
| 1E n | SUFFIX | the next name (codes 08, 15, 18) is cut to 6 glyphs and gets `y` (n = 0) or `i` (n = 1) appended, as a capital when the name's last glyph is a capital; nothing is added when the name already ends in `I`, `Y`, `i` or `y` | `{-y}`, `{-i}` |
| 1F | (none) | no-op, no argument; dialect strings use it as an empty form | `◆` |

* **Upper case** maps `a`-`z` to `A`-`Z` and `ä ë ï ö ü` to `Ä Ë Ï Ö Ü`; other accented letters stay small. A window whose speaker is character 8 (Zoah) draws its whole message in capitals.
* `09 0C 0D 0F 11 13 14 1A 1D` do not occur in field text.
* **Battle packs.** Part 1 of the `.pac` has the record layout of section 1 and the same codes; the USA packs use only `00 01 02 04 06 0E 15`.

### 11.2 Glyphs

**Field font.** 191 glyphs in the executable (`SLUS_010.41`, the record `prog/slps_023.64` of the USA index), file offset `0x46FD0`: glyph `code - 0x20`, 11 rows of one u16 little-endian, bit 15 = leftmost pixel, up to 11 pixels wide. The pen advances by the u8 at index `code - 0x20` of disc record 2059 (256 bytes, no name in the index): 2..13 pixels (`A` 7, `i` 3, `M` 9), 0 from index 191 on. Lines are 13 pixels apart. The glyphs take the four-planes-per-cell path of 7.11.

Order (`usa_glyphs.tsv` has every code): `0x20` space, `0x21..0x65` the JP single-byte Latin order (`( ) - , + . /`, digits, `A`-`Z`, `a`-`z`), `0x66..0x74` `= ! " $ % & * : ? – [ ] ｡ ' #`, `0x75` a 12-pixel blank (the choice indent, U+3000 in `en.csv`), `0x76..0x7F` `← ↑ ↓ →`, larger `＋ （ ） ．`, `“ ”`, `0x80..0x83` the item-kind prefixes `α β γ δ` (drawn as four boxed icons), `0x84..0xA3` `À Ä Ç È Ë Ò Ö Ù Ü à â á ä ç è ê é ë ì î í ï ù û ú ü ò ó ô ö Ñ ñ` (the capitals carry a one-pixel accent), `0xA4 ♪`, `0xA5 ♫` (`{♪}`, `{♪♪}`), `0xA6 Ï`, `0xA7 ¡`, `0xA8..0xAB` alternate `M W m w`, `0xAC` a boxed `E`, `0xAD..0xB3` 2-pixel blanks, `0xB4..0xDE` symbols at their JP positions: `『 』 × & ☆ □ △ ※ < > ~ … ? ! ・ 「 」 # − / 、 ― ‥ ＿ ○ ’ : -`, digits `0`-`9` (`0xD0..0xD9`), `% , ; < >`. The text writes its commas as `0xDB` (12,269 uses against 11,561 of `0x24`); `0xBE 0xC0 0xC1 0xDD 0xDE` also appear as plain `~ ? ! < >`. Aligning 8,819 USA messages with `en.csv` (`usa-glyphs`) votes for 99 codes and agrees with this table on every one (`0xDC ;` reads `.` in RDE's six edited rows).

**Menu sheet.** The menus draw an anti-aliased 4bpp sheet: entry `usfo` of disc record 234 (a DRP of menu art, no name in the index) is a TIM of 256 x 160 texels at VRAM (960, 0) with a 16-entry grey-ramp CLUT. The glyph rectangles are the type-3 sprite sheet `usfo` of `menu/menutest.prd` (layout of 10.1: `u16` frame count at +2 with bit 15 set, frame offsets at +0xC): frame n is code `0x20 + n`, one part each (`x, y` offset, `u, v, w, h` on the sheet). Advance `x + w + 1`; a code without a frame (space, `0x84`, `0xAD..0xB5`, `0xC0` and up) advances 4, except `0xDB`, drawn with the comma's frame. Lines are 18 pixels apart; a tab moves to x 128. The sheet's order matches the field font up to `0x83`, with `0x6F` drawn as `_`; from `0x84` it differs: `0x84` none, `0x85..0xAC` `À Ä Ç È Ë Ò Ö Ù Ü à á â ä ç è é ê ë ì í î ï ù ú û ü ò ó ô ö Ñ ñ Ì Í Î Ï Œ œ ¡ ¿`, `0xB6..0xBF` `× & ★ □ △ ※ < > ~ ○`. `usa_glyphs.tsv` gives the frame rectangle, offset and advance per code.

**Menu text** is not in the overlays: the 178 messages of entry `mess` (type 7, u16 offset table) of `menu/menutest.prd` are ASCII. LF or CR starts a line, TAB moves to x 128; ASCII maps to sheet codes (`0`-`9` to `0x28..`, `A`-`Z` to `0x32..`, `a`-`z` to `0x4C..`, `! " $ % & ' ( ) * + , - . / : = ? [ ] _` to their codes of the field order, `0xA1` to `0x72`); Shift-JIS pairs `81 67` / `81 68` are `“ ”`, `81 9B` is `○`, `81 7E` and `82 77` are `×`; every other byte (space included) draws nothing and advances 4. Entry `stbl` (u32 offsets) holds the 28 save-location titles (a leading story-progress byte, `0x0D` between the two lines), the USA form of RDE's `savetbl`. Entry `gaij` and the menu labels drawn as art are not text.

### 11.3 Executable string tables

The executable holds one container at file offset `0x4803C` (the JP build's is at `0x508E0`): u32 count, u32 offsets (from the container start), then the tables; a table is a u16 offset list (first offset = 2 x entries, the last offset only ends the previous string) of NUL-terminated strings with control codes. The USA build has 55 tables, the JP build 28:

| USA table | entries | contents | RDE | JP table |
|---|---|---|---|---|
| 0 | 256 | items, equipment (entry 0 blank; equipment names start with an item-kind prefix) | `sysmes` 0x00 | 0 |
| 1 | 65 | default character names | `sysmes` 0x01 | 1 |
| 2 | 1536 | battle strings: three entries per element (target, two description lines) | `sysmes` 0x1A (the two lines joined with `\n` in the first) | 26 |
| 3 | 8 | button names (`○ button` ...) | `sysmes` 0x1B (empty in EN) | 27 |
| 4 | 512 | elements and techs | `sysmes` 0x17 | 23 |
| 5 | 65 | key items | `sysmes` 0x18 | 24 |
| 6 | 256 | enemy names | `sysmes` 0x19 | 25 |
| 7..54 | 23 (table 38: 52) | dialect forms for `12 n`, n = table - 6 | `mesgen_en.csv` | none (JP tables 2..22 are the JP grammar forms, 66 rows by character id + 1, read by `12 n` as table `n + 1`) |

A field text item id (`07`, `19`) reads table 0, ids `0x200..0x3FF` table 4 and `0x400..` table 5; names (`08`, `15`, `18`) read the save's name records for ids below `0x2C` and table 1 row `0x31` (Lynx) for any other id.

**Dialect tables.** Row 0 of each is the code itself, rows 1..21 the forms of the 21 dialect columns (row 22 is `X`). The character id -> column map is a byte table of the field overlay `prog/field.prg`, file offset `0x3EA84` (the colour table of 11.1 sits at `0x3EA48`): ids 0..49 -> `0 8 1 1 0 1 1 0 1 0 7 13 2 0 17 7 0 8 7 1 0 0 11 0 9 4 6 18 14 5 1 0 0 12 7 16 0 10 1 1 15 0 0 3 3 3 3 3 3 0` (column 0 = plain English). Table 38 (`12 20`, what Serge is called) is read by character id instead: `{SERGE%}` for most, `{-i}{SERGE%}poo` (Poshul), `{-y}{SERGE%}` (Razzly), `Sir {SERGE%}`, `Mister {SERGE%1}`, `Herr {SERGE%}`, `The-{SERGE%}` and so on. RDE's `mesgen_en.csv` is these tables expanded per character id: key order `{-} {a} {e} {i} {o} {u} {g'} {'re} {h'} {l} {rr} {rw} {rwr} {f} {thd} {thf} {thff} {thvv} {sth} {stz} {w} {-umeth} {-a-} {-an-} {-the-} {-I-} {-my-} {-me-} {-you-} {-your-} {yes}` for `n = 1..0x1F`, `{SERGE}` for `0x20`, then `{n't} {n't-you} {can't} {can't-you} {won't} {won't-you} {aren't} {aren't-you} {'ve} {'ll} {'ld} {'d} {t-you} {'s} {ss} {wha}` for `0x21..0x30`; per key 47..50 of the 50 character ids give the same string (`usa_tables.tsv`). Battle text has no dialect codes.

### 11.4 Against RDE's English

RDE's English is a light edit of the USA script, not a new translation (`cc_text.py usa-compare`):

* **Field.** Of the 12,827 USA messages whose `en.csv` row (same room id, same message index) is real text (27,535 more rows are `{ERROR}`: debug text RDE did not translate, present in English on the USA discs), 6,613 (51.6 %) have identical markup; 8,457 (65.9 %) once RDE's systematic changes are undone (no `{Cap}` before a name, no number-entry digit row, line breaks moved, curly quotes, waits and case marks); 9,920 (77.3 %) differ in punctuation or markup only; 803 more match another message of the same room (renumbered rooms); 2,104 (16.4 %) have edited wording or no counterpart.
* **Edits** most often remove or add ellipses and commas, drop quotation marks, write `{SERGE%}` where the disc has the dialect name `{SERGE}`, capitalise titles (`General`, `Grandma`, `Captain`), fix run-together words (`DaggyDwarf`) and reword lines.
* **Battle** (`btel_en.csv`, 255 rows that have text): 116 identical, 152 (59.6 %) after the same normalisation, 103 edited (the Criosphinx riddles are rewritten).
* **Tables** against `sysmes`: character names 50 / 50, key items 62 / 65, items 182 (+ 51 spacing only) / 256, enemies 169 (+ 36) / 256, elements and techs 113 (+ 56) / 512, battle strings 48 (+ 517) / 1536. The USA names are length-limited (`AquaBeam`, `Acacia PVT`); RDE writes them out (`Aqua Beam`, `Acacia Private`).
* **Menus.** RDE rewrote most menu text (memory-card wording became slot wording, shorter option lines): of the non-empty English keys, 91 of 143 `menutest.txt` and 62 of 86 `shopmenu.txt` texts are found in the USA `mess` messages, whole, as one line or tab field, or inside one (`usa_menu_strings.tsv`; the rest exist only in RDE or are drawn as art).

### 11.5 Open questions

* The pixel layout of a USA menu screen (where each `mess` message and table string is drawn) was not traced; only the text and the sheet metrics are known.
* `0xAC` (boxed `E`), the alternate `M W m w` and the 2-pixel blanks `0xAD..0xB3` never occur in the text or tables; their use is unknown.
* `0x84` has a field glyph (`À`) but no menu frame, so an `À` in a menu string would draw nothing.
* The `gaij` entry of `menutest.prd` and the leading byte of each `stbl` title were not decoded.

## 12. Text without lang.dat (PS1 discs)

A session on the PS1 discs has no lang.dat. Every table and lookup of section 5 then comes from the disc's own files, in the shape lang.dat gives it, in the disc's language (Japanese or English) whatever language is asked for. The build is recognised by the executable's string-table container (JP: file offset `0x508E0`, 28 tables; USA: `0x4803C`, 55 tables, section 11.3). OpenCross: `import::Ps1Text` (`src/import/ps1_text.*`), used by `LangTables` when the install has no `lang` archive; one row of `ps1_text_builds()` per build, so another build is a data addition. Test `test_disc_text` compares every source with lang.dat on the installed game (cdrom.dat holds the JP discs' files byte for byte) and reads the player's JP and USA disc images when present.

| lang.dat | PS1 source | shape |
|---|---|---|
| `<lang>.csv` field rows | message block of the room `.bin` (section 1), record = message index | decoded to the `jp.csv` notation (section 2): name code `08` with `0x81` / `0x82` = `{SERGE%1}` / `{SERGE%2}`, `0x83` = `{RAZZLY%2}` (USA `%3`); grammar code `12 n` resolved for the speaker (below); `1F` dropped; USA: `1C 0/1/2` = `{Cap}` `{CAP}` `{CapL}`, `1E 0/1` = `{-y}` `{-i}`, `0E 8` = `{color(GRAY)}`. Then the steps of 5.1 that apply: speaker detection, `\n` to LF, non-JP normalisation. A mod's row for the same key still wins. |
| `btel_<lang>.csv` | part 1 of the battle scene's `.pac` (record `2679 + battle id` of the disc file numbering), record = message | as the field text; the newline after the closing control is dropped |
| `sysmes` | the executable's string tables: category 00 items, 01 names, 17 elements / techs, 18 key items, 19 enemies, 1A battle strings, 1B button names (JP tables 0, 1, 23, 24, 25, 26, 27; USA 0, 1, 4, 5, 6, 2, 3) | the table's last offset only ends it; a row of blanks is empty; category 1A is (target, line 1, line 2) triples and the second line is joined to the first with LF (RDE's layout) |
| `btmes` | `battle/data/btmes.dat` (message-block layout) | leading blanks (the PS1 centres the line) and the closing newline dropped, and the message-end codes at the end of a record (the last record runs to the end of the file, so it takes in the zeros a file read from a PS1 disc ends with, `disc.md` section 4); code `07 00 00` (the stolen item) = `{itemwork(0)}` |
| `keyitem`, `waa_spec` | `menu/keyitem.dat`, `menu/waa_spec.dat` (u16 offset tables, three lines per entry) | the entry's lines joined with LF in its first row, trailing blank lines dropped |
| `menutest`, `shopmenu` | JP: the Shift-JIS strings of `prog/menutest.prg` / `prog/shopmenu.prg`, loaded at `0x801D2800`, keyed by guest address; USA: a line or tab field of a `mess` message of `menu/menutest.prd` (section 11.2), by `usa_menu_strings.tsv` | JP rows that are not the one string at their key are put together from overlay strings (`kMenuPartsJp`, `tools/py/ps1_text_tables.py`): consecutive strings that are the lines of one text, or 1 to 3 strings near the key per line (`敵・` + `単体`, `どの` + `武器を` / `鍛造しますか？`); the PS1's own line breaks are kept |
| `savetbl` | entry `stbl` of `menu/menutest.prd` | JP: 36-byte records, a NUL-padded title and its story-progress value, row n = record n - 1; USA: titles that start with their progress value, each taking the row of the JP title with the same value; a CR splits the two lines |
| `ome` | JP: part 0 of `menu/player/<name>.ome` (u32 part count, u32 part offsets), Shift-JIS lines ending in CR LF; the row -> file order is `kOmeFilesJp` | none for USA |
| `cmnmes`, `memmenu`, `btlexit`, `error`, `ui_pausemenu`, `ui_configmenu` | none: RDE's own screens and captions | absent; every user keeps its built-in text (the pause screen and RDE-only features draw their English fallback) |

**Grammar forms (`12 n`).** JP: table `n + 1`, row = the speaker's character id (the row RDE's `mesgen.csv` uses; row 0 is the form's name). USA: table `n + 6`, row 1 + the speaker's dialect column from the byte table at offset `0x3EA84` of `prog/field.prg`; `12 20` (what the speaker calls Serge) by character id + 1 (section 11.3). The speaker is the one of 5.1 (the first name tag of the message, else the script's speaker).

**Shift-JIS.** The JP menu files use Shift-JIS, not the glyph codes of section 3: ASCII, half-width katakana (`A1..DF`) and two-byte characters. The engine embeds the two-byte characters the menu files use (`kSjisChars`, generated from the standard code page); a string with any other code is not text.

**Against lang.dat (JP).** Field 56,516 of 56,699 rows equal after the same steps (the rest: RDE text edits such as room 27 message 15 `海の男` -> `海の民`, `{color(4)}` -> `{color(2)}` item colours, spacing around `{key(n)}`); battle 249 of 256 (with RDE's `{wmode}` / `{size}` prefix and the newline after `{bpage}` ignored; the rest RDE edits and the half-width blank padding of evt462); sysmes 2,637 of 2,683 (RDE edits, `<Missing>` placeholders for dummy rows); btmes 60 of 60; keyitem 178 of 195; waa_spec 575 of 768 (RDE keeps a blank third line); menutest 100 of 202 and shopmenu 76 of 161 (RDE rewrote the memory-card wording, re-split lines and has rows for strings the PS1 builds in code; 18 / 17 keys have no PS1 string); savetbl 26 of 30; ome 0 of 51 (RDE writes the digits and colons half-width). USA disc 1: 35,363 room messages decode with no unknown glyph except `FF`, which the USA font lacks (three debug and tutorial records); no grammar code is left after resolution.
