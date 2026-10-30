# Save format: `save.dat`, PS1 memory cards, the Chrono Cross save block

This document covers the Steam edition's `save.dat` container (an AES-encrypted ZIP), the two PS1 memory-card images inside it, the 0x2000-byte Chrono Cross save block (header, raw state, bit-packed party/inventory stream), the side file `ext_save.dat`, and how OpenCross reads, writes, creates and auto-saves them. Tools: `tools/py/cc_save.py` (`info slots dump checksum ext`; read-only, stdlib only, pure-Python AES-256), `tools/py/save_check.py <save.dat> [slot]`, and `occ save-info | save-slots | save-roundtrip | save-blank` (`tools/occ_save.cpp`). Code: `src/save/` (`cc_save.*`, `memcard.*`, `encrypted_zip.*`, `crypto.*`, `ext_save.*`, `oc_saves.*`).

Offsets are byte offsets into the 0x2000 block unless stated. All integers are little-endian. Fields marked "unknown" were never decoded; keep them verbatim.

## 1. Container: `save.dat`

A ZIP archive. Every member is deflate (method 8) wrapped in **WinZip AES-256, AE-2**:

| field | value |
|---|---|
| local/central method | 99, extra field `0x9901` = {version 2, vendor "AE", strength 3, real method 8} |
| password | `376c671d-da29-44c7-a158-4a18faa5fed0` (ASCII) |
| member body | salt(16) + password verifier(2) + AES-CTR ciphertext + HMAC-SHA1(10) |
| KDF | PBKDF2-HMAC-SHA1, 1000 iterations, 66 bytes out = AES key(32) + HMAC key(32) + verifier(2) |
| cipher | AES-256-CTR, 16-byte **little-endian** counter starting at **1**, keystream = AES-ECB(counter) |
| HMAC | HMAC-SHA1(hmac key, ciphertext) truncated to 10 bytes |
| CRC-32 | the real CRC-32 for members >= 20 bytes, 0 for the 16-byte `achievement.dat` (libzip's AE-2 small-file rule); integrity comes from the HMAC |
| entry fields | GP flags 3, version made by 0x33F, version needed 20, external attributes 0x81B60000 (Unix 0100666) on every member; OpenCross writes the same |

Members of a real file: `memcard1.mcr` (131072 bytes), `memcard2.mcr` (131072), `rd_save.dat` (1024, all 0x60, owned by the launcher exe, keep verbatim), `achievement.dat` (16), `ext_save.dat` (42548 = 0xA634). Order is creation order; look members up by name. A `save.dat.<8 hex>` sibling is a dead libzip temp file: ignore it.

## 2. Memory card image (`memcardN.mcr`, 0x20000 bytes)

A standard PS1 raw card. Card 1 = `bu00`, card 2 = `bu10`.

| offset | size | content |
|---|---|---|
| 0x0000 | 0x80 | frame 0: `"MC"`, zeros, byte 0x7F = XOR of bytes 0..0x7E |
| 0x0080 * n | 0x80 | directory frame n = 1..15 describes block n (bytes at 0x2000 * n) |
| 0x0980.. | 20 x 0x80 | broken-sector list (0xFFFFFFFF), frames 16..35 |
| 0x2000 * n | 0x2000 | block n = 1..15 |

Directory frame: `u32 state` (0xA0 free, 0x51 first block of a file, 0x52 middle, 0x53 last, 0xA1/0xA2/0xA3 deleted), `u32 size` (file bytes, 0x2000 per block), `u16 next` (0-based index of the file's next directory frame, 0xFFFF = none), `char name[20]` at +0x0A (NUL padded), byte +0x7F = XOR of bytes 0..0x7E.

All Chrono Cross saves are single-block files (`size` 0x2000, `next` 0xFFFF). File names are `BISLPSP02364CHRONO00` .. `BISLPSP02364CHRONO14` ("BI" = Japan region prefix, `SLPSP02364` the product code, `CHRONOnn` the slot number). The RDE build is the Japanese one; the US disc used `BASLUSP01041USCHROnn`. Slot 00 is the auto-save; 01..14 are manual saves. Always resolve a slot by name, not by directory position (in practice directory entry n+1 holds CHRONOnn).

## 3. The 0x2000 Chrono Cross block

```
0x0000-0x007F  PS1 save header frame      "SC", 0x11 (icon flag: 1 frame), 0x01 (blocks), title[64] at +4
0x0060-0x007F  icon palette (16 x BGR555)
0x0080-0x00FF  icon frame (16x16, 4bpp)
0x0100-0x05AB  raw game state            (section 4)
0x05AC-0x1F20  bit-packed section stream (0x1975 = 6517 bytes, section 5) -> decompresses to the 0xE1F4 image (section 6)
0x1F21-0x1FF7  raw tail (0xD7 bytes; non-zero, content unknown; keep verbatim)
0x1FF8-0x1FFB  u32 save version, 0x106 in every RDE-written block
0x1FFC-0x1FFF  u32 checksum
```

**Checksum.** `u32 at 0x1FFC == (sum of bytes 0x0000..0x1FF7) mod 2^32` when the version is > 0x102. RDE's loader sums only `0x180..0x1FF7` when the version is <= 0x102 (older PS1 saves) and adds the header frame `0x000..0x17F` when it is > 0x102. A block with version 0 is summed without the header, fails the stored all-bytes sum, and RDE reports "data corrupted". So any block RDE must load needs the version stamped and the checksum recomputed after every edit, anywhere in the block.

**Version gates** after the check: `> 0x100` loads the 44-bit character bitmap of steps 46-47; `> 0x101` the per-character rec+0xA8 bytes (step 48); `>= 0x104` the 16-bit obtained-frames mask (else 0xFF); `>= 0x105` the two innate-colour bytes (else 8); `>= 0x106` the two New Game+ words (steps 52-53, else 0). OpenCross always writes 0x106 and all 54 steps.

**Header frame fields** (`encode_block` stamps all of these on every block it writes):

| offset | value |
|---|---|
| 0x00 | `53 43 11 01` = "SC", icon flag 0x11, 1 block |
| 0x04 | title, Shift-JIS `クロノ・クロス　` + fullwidth (slot + 1), NUL padded to 0x24 (`クロノ・クロス　０１` .. `１５`) |
| 0x24 | u32 = 210, the bit count of the summary stream (the reader ignores it) |
| 0x28.. | summary stream (bit-packed LSB first, 210 bits): name 12 bytes, party 3 bytes (0x34), play time u32 (0x37), gold u32 (0x3B), stars u8 (0x3F), location u16 (0x40; the key into the Continue-screen location text table), 2 flag bits (0x42) |
| 0x50 | `01 00 43 52 44 30` = 01 00 "CRD0" marker |
| 0x60 / 0x80 | icon palette / frame: present in manual saves (different per slot), absent (zeros) in RDE's own auto-save |

The 0xD7 tail bytes before the version are uninitialised pack-buffer leftovers in manual saves (identical across slots of one session, not read by the loader). RDE's auto-save zeroes the block first, so zeros are right.

## 4. Raw state (0x0000-0x05AB)

The first table is the **summary/preview** that the load menu shows without decoding the rest (it lives inside the title area's zero padding).

| off | type | field |
|---|---|---|
| 0x28 | char[12], Shift-JIS | leader name (preview): "セルジュ"; the auto-save has "オートセーブ" |
| 0x34..0x36 | u8 x3 | party character ids (preview; same as 0x59C) |
| 0x37 | u32 | play time in 1/60 s frames, preview copy (unaligned; may be 1 frame ahead of 0x588) |
| 0x3B | u32 | gold (preview) |
| 0x3F | u8 | stars (preview) |
| 0x40 | u16 | story progression value (0, 8, 0x10 .. 0x38 in a real file); also the key of the location text. Equals script variable 0 (the whole u16; the last location threshold is 999) |
| 0x42 | u8 | bits 0-1 = script variable 0x7E & 3: 0 normal, 2 a New Game+ / Continue+ game, 3 a cleared save written by the ending. The Continue+ list refuses slots with either bit |

The full state:

| off | type | field |
|---|---|---|
| 0x180..0x187 | 64 bits | key items; bit i = key item i + 1 (Tele-Porter = bit 0). RDE sets bit 61 (Time Shifter) on every new game and every load; OpenCross too. New Game+ sets bits 60 (Relief Charm) and 61; Continue+ also bit 52 (Time Egg) |
| 0x188..0x587 | 0x200 x u16 | script variables 0x000..0x3FE, variable v at `0x188 + 2v`. Variable 0x7E is the New Game+ state (also read by the enemy-HP doubling) |
| 0x18E | u16 | location (room/area) id |
| 0x194/0x196/0x198 | u8 | party ids again (hidden copy) |
| 0x1C2 | u8 | unknown (0x1E, 4, 0 seen) |
| 0x1CC | u8 | custom window frame |
| 0x488 / 0x48A | u16 | field position X / Z |
| 0x55C..0x566 | u16 x6 | Pip colour stats: green, white, black, red, yellow, blue |
| 0x568..0x587 | u16 x16 | the Dopplegang pool: 256 bits, bit n of word w = creature slot w * 16 + n. Set when Sprigg (or a Forget-me-not Pot wearer's last command) kills the creature |
| 0x588 | u32 | play time in frames (/60 = seconds) |
| 0x58C | u32 | gold |
| 0x590..0x595 | 44 bits | recruited characters, bit i = character id i (Serge = bit 0 ... Pip = byte 0x595 bit 3) |
| 0x598 | u32 | growth progress counter; meaning unknown, 0 after every new game; not written by OpenCross |
| 0x59C..0x59E | u8 x3 | current party / formation, character ids (0 Serge, 1 Kid, 2 Guile, 10 Korcha) |
| 0x59F | u8 | Relief Charm member (party slot 3; 0xFF none) |
| 0x5A0 | u8 | option bits (low byte of the options word): 0 cursor memory, 1 accuracy selection (auto), 2 sound mono, 3 window frame custom. A new game starts with 0x0002 |
| 0x5A1 | u8 | options high byte: bit 0 controller edit, bit 1 "[button] Pressed" = walk, bit 2 vibration, bit 3 left stick analog |
| 0x5A4 / 0x5A5 | u8 | stars total / current |

Character ids (0..43), the engine order used everywhere: Serge, Kid, Guile, Norris, Nikki, Viper, Riddel, Karsh, Zoah, Marcy, Korcha, Luccia, Poshul, Razzly, Zappa, Orcha, Radius, Fargo, Macha, Glenn, Leena, Miki, Harle, Janice, Draggy, Starky, Sprigg, Mojo, Turnip, NeoFio, Greco, Skelly, Funguy, Irenes, Mel, Leah, Van, Sneff, Steena, Doc, Grobyc, Pierre, Orlha, Pip. Other event/story flags sit in 0x100..0x5AB and the tail without names. The raw head is not a plain copy of game memory at a fixed base.

**Not saved:** the field-monster defeated cache (per-room kill masks), so a loaded game always respawns field monsters (OpenCross keeps the cache in session memory only); the field script's room-state tables (ops 75-78 flag table, the pair table).

## 5. Bit-packed section stream

Input: bytes 0x05AC..0x1F20 (6517 bytes = 52136 bits, consumed exactly by the 54 steps below; there is no padding or end marker). Bits are read **LSB first**: bit k of the stream is `(byte[k>>3] >> (k&7)) & 1`; an n-bit value is assembled little-endian (first bit read = value bit 0). Output: a zeroed 0xE1F4-byte image (section 6). The step table is `STEPS` in `cc_save.py`; the codec is in `src/save/cc_save.cpp`.

Modes (per step; stored width: 32 bits -> u32, more than 8 -> u16, else u8, little-endian):

| mode | meaning |
|---|---|
| 0 | `count` values of `length` bits; value i stored at `offset + i * stride` |
| 1 | for char c < `count`, j < 0x30: a 3-bit value into a nibble at `offset + c*0x18 + (j>>1)`, low nibble for even j |
| 2 | for char c < `count`, 0x30 values of `length` bits stored as u16 at `offset + c*0xCC + 2k` |
| 3 | for char c < `count`, 8 values of `length` bits stored as u16 at `offset + c*0x10 + 2k` |
| 4 | names: for char c < `count`, 6 symbols of 11 bits each (section 7); record `offset + c*0x10`, NUL after the last symbol |

Step table (`bits` = value width; `count` = number of values read; `stride` = bytes between values; image offsets in hex; 44 = 0x2C characters):

| # | mode | bits | count | stride | image off | stream bit | field |
|---|---|---|---|---|---|---|---|
| 0 | 0 | 9 | 384 | 2 | D13C | 0 | item ids (u16) |
| 1 | 0 | 7 | 384 | 1 | D43C | 3456 | item quantities (u8, 0-99) |
| 2 | 0 | 9 | 192 | 2 | D5BC | 6144 | element ids (u16) |
| 3 | 0 | 7 | 192 | 1 | D73C | 7872 | element quantities |
| 4 | 1 | 3 | 2112 | 48 | DD98 | 9216 | equipped-element quantity nibbles (0x18 B/char) |
| 5 | 0 | 8 | 44 | 204 | AAD4 | 15552 | status bits (rec+0xB0) |
| 6 | 0 | 8 | 44 | 204 | AA26 | 15904 | rec+0x02 (unknown) |
| 7 | 2 | 9 | 2112 | 2 | AA64 | 16256 | grid cells, 48 x u16 per char (rec+0x40) |
| 8-10 | 0 | 7 | 44 | 204 | AA3C, AAE4, AAE5 | 35264.. | ATK, ATK extra A, B |
| 11-13 | 0 | 7 | 44 | 204 | AA3E, AAE6, AAE7 | 36188.. | HIT, extra A, B |
| 14-16 | 0 | 7 | 44 | 204 | AA3D, AAE8, AAE9 | 37112.. | DEF, extra A, B |
| 17-19 | 0 | 7 | 44 | 204 | AA3F, AAEA, AAEB | 38036.. | EVD, extra A, B |
| 20-22 | 0 | 7 | 44 | 204 | AA40, AAEC, AAED | 38960.. | MAG, extra A, B |
| 23 | 0 | 7 | 44 | 204 | AA41 | 39884 | MDEF |
| 24 | 3 | 10 | 352 | 0 | DAD8 | 40192 | unlocked-element-level masks, 8 x u16 per char (0x10 B/char) |
| 25 | 0 | 10 | 44 | 204 | AADC | 43712 | current HP (u16) |
| 26 | 0 | 10 | 44 | 204 | AA3A | 44152 | max HP (u16) |
| 27 | 0 | 10 | 44 | 204 | AAE0 | 44592 | max-HP extra (u16) |
| 28-32 | 0 | 9 | 44 | 204 | AA44, AA46, AA48, AA4A, AA4C | 45032.. | weapon, armour, accessory 1-3 (u16 item ids) |
| 33 | 0 | 9 | 44 | 204 | AA51 | 47012 | model / character id (u16 slot, < 0x200) = record +0x2D: the field and battle model. Pip (43) holds his form 0x2B..0x30 here, written by the battle at a form change |
| 34 | 0 | 10 | 44 | 204 | AAE2 | 47408 | rec+0xBE (u16, unknown) |
| 35-36 | 0 | 7 | 44 | 204 | AAEE, AAEF | 47848.. | MDEF extra A, B |
| 37 | 4 | 11 | 264 | 0 | D80C | 48464 | character names |
| 38-41 | 0 | 32 | 1 | 1 | E1B8, E1BC, E1C0, E1C4 | 51368.. | party lock bitmap (bit c = character c cannot leave its party-screen slot; new game = 1, Serge) and party-screen exclusion bitmap (script ops FE 5D / FE 60). Real slot 0: lock 1, excluded 2 (Kid) |
| 42-45 | 0 | 32 | 1 | 1 | 0000, 0004, 0008, 000C | 51496.. | button config: field, battle, field analog, battle analog (nibble per button, section 6) |
| 46-47 | 0 | 32 | 1 | 1 | E1E0, E1E4 | 51624.. | 44-bit character bitmap tested by field ops E5 / E6; zeroed at a new game, no op sets it |
| 48 | 0 | 8 | 44 | 204 | AACC | 51688 | rec+0xA8 (unknown) |
| 49 | 0 | 16 | 1 | 1 | E1E8 | 52040 | obtained Frames bitfield (15 named frames, bit 15 unused) |
| 50 | 0 | 8 | 1 | 1 | AA25 | 52056 | innate colour bit of character 0 (value 8 = Serge) |
| 51 | 0 | 8 | 1 | 1 | CC69 | 52064 | innate colour bit of character 43 (Pip) |
| 52-53 | 0 | 32 | 1 | 1 | E1EC, E1F0 | 52072.. | New Game+ carry-over bitmap (cleared-save bitmap, OR cleared recruited, OR the E5/E6 bitmap), test-and-cleared by field op E8; zeroed by a new game; loaded only from version >= 0x106 |

The 7-bit stat fields hold 0..127; larger values cannot be stored. New-game values: the nibbles of step 4 are 5 in every cell; the "extra A, B" pairs of steps 8-22 / 35-36 and steps 27 / 34 are the growth snapshot and pending values (+0xBC max HP, +0xBE pending HP, +0xC0/C1 ATK, C2/C3 HIT, C4/C5 DEF, C6/C7 EVD, C8/C9 MAG, CA/CB MDEF); step 24's masks start at {0x40, 0x40, 0x40, 0, ...} plus the star unlocks. The button-config defaults are 0x76543210 / 0x76543210 / 0x3210 / 0x3210; image offsets 0x0000.. are only the placement used by this layout. Re-packing a decoded image reproduces the original 6517 bytes bit-exactly for all real slots.

## 6. Decompressed image map (0xE1F4 bytes)

The image mirrors the game's working memory; only the stepped fields are non-zero. Character records start at 0xAA24.

| image off | size | content |
|---|---|---|
| 0x0000..0x000F | 4 x u32 | button config. Each nibble is one button action index: field = {confirm/talk, cancel/run, key-items, menu, (=cancel), (=confirm), unused, unused} (default `76543210`), battle similarly; analog configs hold 4 nibbles (`00003210`) |
| 0xAA24 + 0xCC * i | 0xCC | **character record i** (i < 44) |
| 0xD13C | 384 x u16 | inventory item ids (0 = empty); quantities at 0xD43C (384 x u8) |
| 0xD5BC | 192 x u16 | element inventory ids; quantities at 0xD73C |
| 0xD80C + 0x10 * i | 16 | name i (section 7) |
| 0xDAD8 + 0x10 * i | 8 x u16 | per star-level cell mask of unlocked grid levels (Serge `480 480 192 64 0 0 0 0` at slot 4 = 3 levels open) |
| 0xDD98 + 0x18 * i | 48 x 4 bit | quantity left of the element equipped in grid cell k (5 seen) |
| 0xE1B8..0xE1F0 | | the bitmaps of steps 38-41 and 46-53; 0xE1E8 u16 = obtained-frame bits (0x203 = 3 frames in a real slot) |

**Character record** (0xCC bytes). The first 0xB0 bytes are the `char*.bin` stat record of `docs/formats/battle_data.md` with the same offsets (max HP +0x16, ATK +0x18 .. MDEF +0x1D, equipment +0x20.., model id +0x2D, grid cells +0x40). Not saved (rebuilt from character data at load): +0x00, +0x03, +0x04, +0x14..+0x15, level masks +0x30..+0x3F, +0x2B..+0x2C, +0x2E..+0x2F, +0xA0..+0xAF except +0xA8. The extension 0xB0..0xCB holds the dynamic state:

| rec off | size | field |
|---|---|---|
| +0x01 | u8 | innate colour bit; only characters 0 and 43 are saved |
| +0x02 | u8 | unknown, saved for all |
| +0x16 | u16 | max HP |
| +0x18..+0x1D | u8 x6 | ATK, DEF, HIT, EVD, MAG, MDEF |
| +0x20 / +0x22 / +0x24 / +0x26 / +0x28 | u16 | weapon, armour, accessory 1..3 |
| +0x2D | u8 | character/model id (Serge 0 / Lynx 0x31 in slot 0; Pip evolutions 0x2B..0x30) |
| +0x40 | 48 x u16 | element grid: element id placed in cell k (0 = empty) |
| +0xA8 | u8 | unknown (a status word that is ORed with +0xB0 by every status test; gear/innate related) |
| +0xB0 | u8 | status condition bits: 0 poison, 3 burns, 4 sprain, 5 flu (others unknown); set by script op FE 3D, cleared by FE 3E |
| +0xB8 | u16 | current HP |
| +0xBC | u16 | "max HP related" (equals max HP in real files; likely the HP the star-up bonus accumulates on) |
| +0xBE | u16 | unknown (0 seen) |
| +0xC0..+0xCB | u8 x12 | pairs A,B per stat in the order ATK, HIT, DEF, EVD, MAG, MDEF (Serge `13 00 / 55 00 / 0e 01 / 08 00 / 0d 01 / 09 00`); likely the fractional/bonus accumulators of star-up and level-up growth |

## 7. Name encoding

In the image a name is up to 6 symbols in a 16-byte record. A longer name (the RDE Latin keyboard allows 12 characters) does not fit: RDE keeps the full name only in `ext_save.dat` (section 9) and leaves the record as it was; OpenCross stores the first 6 encodable symbols in the record and the full name in `ext_save.dat` (cards) or the page file (section 13), and a load prefers the full name. In the stream each symbol is 11 bits: if the byte is >= 0xF8 the symbol is `((byte - 0xF7) << 8) | next_byte`, else the byte itself. Decoding: if `(v & 0x700) != 0` emit `((v & 0x700) >> 8) + 0xF7` then `v & 0xFF`. In short, **bytes 0xF8..0xFE are a 2-byte-code prefix** (the same as the message font, `docs/formats/text.md`). JP font table:

- single bytes: 0x75 space, 0x76.. hiragana in gojuon order (あいうえお かきくけこ ... ん, then voiced が..ぽ, small ぁぃぅぇぉっゃゅょ), 0xC6 。, 0xC7 「, 0xC8 」, 0xC9 #, 0xCA ー, 0xCB ~, 0xCC ⋯, 0xCD ?, 0xCE !, 0xCF •, 0xD0-0xD9 digits, 0xDA-0xF3 A-Z, 0xF5 、;
- `F8 00`.. = a-z then katakana ア.. ヴ (F8 1A ア, F8 20 キ, F8 27 セ, F8 42 ル ...), a few kanji at F8 6C/6D 蛇骨, F8 73-77 星子夫改良, F8 8E/8F 龍種.

Examples: Serge `F827 F842 F84E F868` = セルジュ, Kid `F820 F866 F856` = キッド, Pip's default ツマル. The US build uses a different single-byte font (0x20 space, 0x28-0x31 digits, 0x32-0x4B A-Z, 0x4C-0x65 a-z). The preview name (block 0x28) and the directory title are **standard Shift-JIS** (`83 5A 83 8B 83 57 83 85` = セルジュ), not the game font. `ext_save.dat` custom names are UTF-8.

## 8. Writing a block (export rule, OpenCross -> RDE)

1. Start from a real 0x2000 block of the target card (keeps header frame, icon, raw head, tail).
2. Edit raw fields in place (section 4) and/or the 0xE1F4 image (section 6).
3. `packed = compress(image)` (exactly the 54 steps, same bit order); write it to bytes 0x05AC..0x1F20 (always 0x1975 bytes).
4. Keep 0x1F21..0x1FFB unchanged unless you know the field; set `u32 at 0x1FFC = sum(block[0:0x1FF8]) & 0xFFFFFFFF`.
5. Put the block at `0x2000 * n` and make the directory frame valid: state 0x51, size 0x2000, next 0xFFFF, name `BISLPSP02364CHRONOnn`, XOR byte at +0x7F (frame 0 untouched). A new slot uses the first free (0xA0) frame. The card stays exactly 0x20000 bytes.
6. Keep the preview in sync (0x28 name as Shift-JIS, 0x34..0x36 party, 0x37 time, 0x3B gold, 0x3F stars, 0x40 progression, 0x588/0x58C/0x59C/0x5A4) so the load menu matches; the header title carries the slot number.
7. Rewrite `save.dat`: replace only `memcardN.mcr` (deflate level 9 + AES-256 AE-2, fresh random 16-byte salt, same password), keep `rd_save.dat`, `achievement.dat`, `ext_save.dat`; write to `save.dat.<hex>` and rename over it. Do this only when the game is not running.
8. `ext_save.dat`: the record for slot `(card-1)*15 + n` carries language, battle counter, custom names and the form byte (section 9). A block imported without a record leaves it default (zero record, valid flag 0).

Values wider than the field width (item counts above 127, stats above 127, ids above 511) cannot be stored: clamp them. RDE loads OpenCross-written saves: a fresh new-game auto-save appears in the Continue list with portraits, time and location text, and loads into the right room with the right party, with no "data corrupted".

## 9. `ext_save.dat`, `achievement.dat`, `rd_save.dat`

`ext_save.dat` (0xA634 bytes): `"EXT\0"`, u32 version 0x104, 30 slot records of 0x588 bytes at `8 + s*0x588`, then 30 x {u8 form byte, u8 valid} at 0xA5F8. Slot `s = 15*(card-1) + slot number`. Record: u32 language (0 en, 1 fr, 2 it, 3 de, 4 es, 5 jp), u32 battle counter, 44 x 32-byte UTF-8 custom names (NUL padded, at most 31 bytes). A real file usually populates only slot 0 (language 0, counter 0, form/valid = {3, 1}).

`achievement.dat` (16 bytes): `41 43 48 00` ("ACH\0"), u32 version 0x100, then a 38-bit little-endian bitfield at +8..+0xC (achievement i = byte i/8, bit i%8), rest zero. RDE rewrites it on every new unlock and never reads it back: at start-up the bits are rebuilt from Steam, which is the source of truth, so keep the member verbatim. What earns each achievement is in `docs/rules/achievements.md`. OpenCross leaves the unlocks to Steam (`platform::SteamClient`) and preserves the member unchanged. `rd_save.dat`: 1024 bytes of 0x60, unused by the game; keep.

### 9.1 How RDE uses `ext_save.dat`

1. **Read** (before every save and load): start from an empty buffer (`"EXT\0"`, version 0x104). The member is accepted only when it is 0xA634 bytes with version 0x104 (copied whole) or 0xA5F8 bytes with version 0x103 (copied, version bumped to 0x104, pair table zero). A size below 4 or a wrong magic is skipped; any other size/version leaves the defaults. A damaged file therefore silently becomes an empty one and the next write replaces it.
2. **Write** (Save menu and auto-save): read as above, then for slot `s`: set `language` to the game language, `battles` to the battle counter, the pair to `(form byte, 1)`; clear the 0x580 name bytes and copy each character's custom name (UTF-8, at most 31 bytes + NUL) from the host name map; rewrite the whole archive (temp + rename). Other slots' records are untouched.
3. **Load**: the battle counter becomes `record.battles` (always, 0 for an empty record). The form byte is `pair.form` when it is 3..6, else it is derived from the story byte (0x2B..0x30 -> 3..6). A pair with valid == 0 is set valid after the load; for a story byte in 0x2C..0x30 a stat sync runs first (it rewrites the character's HP/stat fields from tables when the story byte is 0x2B..0x30; it also runs when the story byte changes between 0x2B and 0x30). The custom-name map is **cleared**, then **only if `record.language` equals the current language** every non-empty cell is inserted as that character's name. A language mismatch therefore loads no custom names at all (defaults from sysmes), and the block's PS1 name bytes stay the default kana names.
4. The battle counter is raised (cap 0xFFFF) when a battle ends with result 0 or 3 (win); an achievement fires at 500.

OpenCross: `ExtFile` (`src/save/ext_save.*`) parses by rule 1 and offers `slot(i)` / `set_slot(i, ExtSlot)` with UTF-8 <-> UTF-32 and the 31-byte cut at a code point. `SaveDat::ext()`, `set_ext()`, `write_ext(card, n)` and `read_ext(card, n, language)` use `ExtHost {language, battles, names*}`, set once at boot (`SaveDat::set_ext_host`); without a host (tools, tests) writing does nothing. `write_ext` runs after `write_slot` in the Save menu (for the chosen card, 1 or 2) and in the auto-save (card 1, slot 0). The pair written is `(0, 1)`: form 0 (so RDE derives it from its own story byte) and valid 1, as every RDE save does. Reading: `GameFlow::tick_fade` calls `read_ext(entry.card, entry.slot, language)` (card 1 or 2; an OpenCross-page entry reads its own file's record instead, section 13) before `FlowMachine::on_load`; the ext names override the record names (RDE-saved slots hold default kana names that `usable_name` drops outside JP), and `read_ext` restores the battle counter. Custom names that the non-JP font cannot show (kana) are dropped on load. The counter rises by one in `App::finish_field_battle` for result 0 / 3 (cap 0xFFFF); OpenCross has no achievements. `save_check.py` ends with an `ext_save.dat` block (size/version/magic verdict, one line per populated slot, and `PROBLEM:` lines for a language above 5, a counter above 0xFFFF, a pair outside {0, 3..6} x {0, 1}, names without a NUL, bytes after it or invalid UTF-8).

## 10. What OpenCross writes into a slot

`FieldWorld::capture_state` + `write_slot`: script variables 0x000..0x3FF at `0x188 + 2*id`; location word 0x18E = the script room id of the disc in use; **head 0x40 = the scenario flag (variable 0, u16)**; preview stars 0x3F; preview leader name 0x28 (Shift-JIS); party at 0x59C and 0x34; recruited bits; key items 0x180; header title `クロノ・クロス　` + fullwidth (slot + 1). Script item ids split as: ids < 0x200 search the 0x180 item slots, 0x200.. the 0xC0 element slots (id - 0x200), 0x400.. key-item bit (id - 0x401).

**Continue.** Loading a slot restores the party/inventory block, the script variables and the room/entrance, so scripts resume exactly as a retail Continue. The entrance (room parameter) is **script variable 0x3A**: room init sets 0x3A to the entrance and save events overwrite it (for example `gal_b09i` saves inside its wake-up event with 0x3A = 4 and Continue resumes that event). The room is variable 6. The slot codec is bit-exact (round-trip tested).

Route check: `tests/routes/run_save.ps1` (scratch copy with `occ save-blank`, `cc_save.py dump`, `tests/routes/check_save_dump.py`).

## 11. RDE auto-save (card 1 slot 0, `BISLPSP02364CHRONO00`)

Evaluated once per field tick:

1. **Load latch.** After a save is loaded, the next evaluation only records "last location = current, last allowed = 1", clears the latch and returns.
2. **Trigger.** "Saving allowed" is true (OpenCross: `FieldWorld::can_save()` = script variable 0x3C == 1; using the debug tools does not change it) **and** (last allowed was false **or** the location differs from the last location). So it also fires when "allowed" turns on while the location is unchanged (after a cutscene).
3. **Skips** (map id = the field map, entrance = the room parameter of the last room load; only maps 0xFA-0xFD): 0xFA entrances 0x13-0x16; 0xFB 0x13-0x17; 0xFC 0x10-0x15, 0x18, 0x19, 0x1A, 0x1D; 0xFD 0x10-0x15, 0x17-0x1A, 0x1D. A skipped call still records last location / allowed.
4. **Write.** The block is built exactly like a manual save (header, state, version 0x106, checksum) and goes to card 1 file `BISLPSP02364CHRONO00` (existing entry reused, else the first free directory frame). The frame title is the normal `クロノ・クロス　０１`; the **summary leader-name field (0x28, 12 bytes) is "オートセーブ"** (`83 49 81 5B 83 67 83 5A 81 5B 83 75`), which is what the slot list shows; the icon flag stays 0x11. Then `ext_save.dat` slot 0 is stored.
5. The manual save menu refuses slot 0.

OpenCross: `game::Autosave` (`src/game/autosave.*`): `update(Field)` implements rules 1-3 and `write(SaveDat&, SaveSlot)` rule 4 through `write_slot(card 1, 0)`. `main.cpp` calls it after `world.update` while the field runs, then `FieldWorld::capture_state()` + `SaveDat::save(save_path)` (the same file and temp + rename path as the Save menu). `[Game] AutoSave=1` (default on; RDE has no switch) turns it off. A completed write shows the "AUTO SAVE" strip. Targets are chosen as in section 13. The play-time counter is the tick count of the whole main loop (field, battle, results, menus; not movies or the title scene), added at the top of `advance` in `main.cpp`; the auto-save runs before that increment, so an auto-save on tick 1 shows 0:00:00. Test: `test_autosave`.

## 12. Where `save.dat` lives and how it is created

**RDE.**
1. **Folder.** `<Documents>\My Games\CHRONO CROSS  THE RADICAL DREAMERS EDITION\Steam\<id64>\`, where `<id64>` is the logged-in Steam user's 64-bit id. The folder is created at first boot; there is no other source of the id and no fallback folder. The file is `<folder>\save.dat`.
2. **At boot** RDE reads `memcard1.mcr` then `memcard2.mcr`. When a member is missing or not 0x20000 bytes, it formats a card and writes it (libzip create/overwrite, deflate level 9, AES-256 with the fixed password, temp + rename). So the **first boot creates `save.dat` with exactly two members, `memcard1.mcr` then `memcard2.mcr`**, both formatted empty. A save.dat that libzip cannot open is not replaced.
3. **Formatted card**, byte for byte: all zero except frame 0 = `"MC"` with byte 0x7F = 0x0E (the XOR); directory frames 1..15 = u32 0xA0, next 0xFFFF at +8, XOR 0xA0 at +0x7F; frames 16..35 = u32 0xFFFFFFFF, next 0xFFFF, XOR 0. A real never-used `memcard2.mcr` is this image (CRC-32 0x7B2751FD, 207-byte deflate stream).
4. Later members are appended in creation order: `ext_save.dat` on the first save (manual or auto), `achievement.dat` on the first achievement, `rd_save.dat` only by the launcher exe.

**OpenCross** (`platform::find_save_dir` in `src/platform/paths.cpp`, boot block in `src/app/app_boot.cpp`, `SaveDat::create_empty()` / `ensure_cards()`):

| case | folder | at boot |
|---|---|---|
| `[Paths] SaveDir` set | that folder (created when missing; relative = to the working dir) | as below |
| an RDE `Steam\<id>` folder has save.dat | the one with the newest save.dat (never another user's folder while one has a save) | loaded; a missing or wrong-size card member is formatted and the file saved (RDE rule 2) |
| no save.dat in any `Steam\<id>` | the current Steam user, found without the Steam API: `HKCU\Software\Valve\Steam\ActiveProcess\ActiveUser` (account id, 0 when Steam is not running), else the `MostRecent "1"` entry of `<SteamPath>\config\loginusers.vdf`; id64 = 76561197960265728 + account id; failing that, the newest existing `Steam\<id>` folder | folder created, save.dat created by `create_empty()` (two formatted cards, the same deflate + AE-2 entries as RDE) |
| no Steam user / no Documents | `saves\` next to opencross.exe | same |

`[Paths] SaveFile` renames the file. The log says which rule picked the folder (`save dir: <path> (<rule>)`). Continue stays greyed until a slot exists. The first save appends `ext_save.dat`; OpenCross never writes `achievement.dat` or `rd_save.dat`. A save.dat that does not open is left untouched and saving stays off (as RDE). Checks: `test_save` (`test_create_empty` compares the card, member order and every ZIP entry field with the real file; `loginusers.vdf` parsing) and route `tests/routes/run_save_fresh.ps1`.

## 13. OpenCross save page (`OpenCross\*.ocsav`)

OpenCross only; RDE never reads it. A third save target next to the two memory cards, listed first on the save choice ("OpenCross" / "Slot 1" / "Slot 2").

* **Folder:** `<save dir>\OpenCross\` for `save.dat`; for another `[Paths] SaveFile`, `<save dir>\<file stem>_OpenCross\`. Created on the first write. Files: `auto.ocsav` (the page's auto-save, number 0) and `001.ocsav` .. `100.ocsav`.
* **File** (9616 bytes): `"OCSV"`, u32 version 1, u32 language index, u32 battle counter, 44 x 32-byte UTF-8 custom names (the ext_save record without the pair table), then the 0x2000 slot block exactly as `encode_block` writes it to a card (version 0x106, checksum; the title is not stamped). A load takes the names only when the language matches, and always the battle counter (the ext_save rule). Written as `<name>.tmp` + rename; a file with a bad size, magic, version or checksum is skipped at boot with a warning.
* **Auto-save** (`game::Autosave::targets`): on every trigger the state goes to card 1 slot 0 when either memory card holds a save, and to `auto.ocsav` when the page holds a file; with no save anywhere, to the page only. Its leader-name field is the katakana auto-save caption, as on the card.
* **Choice and slots.** Continue and Save first ask for the target: OpenCross, Slot 1 (card 1) or Slot 2 (card 2); the choice starts on OpenCross when the page holds a file, else on Slot 1. A card shows its 15 CHRONOnn slots (slot 00 = the RDE auto-save, not writable from the menu); the page shows its auto-save plus a scrolling list of 100 files. An empty target shows "No save data". The ext_save record of a card slot is `(card-1)*15 + n` (section 9); a page file carries its own record.
* Code: `save::OcSaves` (`src/save/oc_saves.*`), `ui::SlotList`, `MainMenu`; test `test_autosave` (`test_page`, `test_targets`).

## 14. New Game+ and Continue+

State transforms in `game::ngplus` (`src/game/new_game_plus.*`) over the slot image; variable 0x7E carries the state (section 4, head 0x42).

* **Cleared save.** The ending's save prompt first returns, for every recruited character (or one in the E5/E6 bitmap), the elements of its grid (stacks with their copies; a cell's count is the equipped nibble when the element table flags it as counted, else 1) and its five gear words (ids < 0x200 items, 0x200.. elements) to the stock, then sets variable 0x7E = 3. Loading such a save re-enters the ending room, whose title list offers *New Game +* and *Continue +*. The Continue+ list refuses a slot whose head 0x42 bit 0 is set ("completed") or bit 1 is set (already a + game).
* **Carry-over** (`carry_over`, onto the fresh new game or onto the loaded save): per character record (all 44) the dynamic state of section 6 (rec +0x02, +0xA8, max / current HP, HP extra and pending, the six stats, the growth bonus bytes, unlocked-level masks and the names of characters neither recruited nor in the E5/E6 bitmap of the target) while the target keeps its innate colour bit, gear, model byte and grid; status byte cleared. Items and elements are added into the target's stock (cap 99 per stack), except items 0x26, 0x3F, 0x40, 0xC0, 0xC1, 0x3D, 0xD6, 0xEC, 0xDA, 0xFD, 0x58 (the unique weapons, medals and keepsakes) and elements 0x25..0x30 (the 12 summons) and from 0x7E up. Gold is added (cap 9,999,999). Stars total / current are copied (preview stars = total) and the star unlocks of stars 0..total-1 are ORed into the masks. Obtained-frames bits are ORed. The carry-over bitmaps (steps 52-53) become `cleared carry-over bitmaps | cleared recruited | cleared E5/E6 bitmap (steps 46-47)` (the second word masked to 12 bits). Variables 0x3E0..0x3FF (the Dopplegang pool) are ORed. Variable 0x7E = 2 (head 0x42 bits 0-1 = 2). Key items gain Relief Charm and Time Shifter; Continue+ also gains the Time Egg (bit 52).
* **New Game+ start.** A normal new game plus the carry-over: Serge alone recruited and in the party, gold 100 + carried, play time 0, frames 0x203, the debug-mode variable 0x34 kept. The ending room's script then names Serge and builds the dream party. FE 3F (Serge's room at the real start) resets the party to {Serge, -, -}; with variable 0x7E != 0 it keeps the carried state and ensures Relief Charm + Time Shifter, with 0x7E == 0 it builds a fresh game.

### 14.1 New game state

`game::newgame` (`src/game/new_game.*`) builds the two states of a new game as transforms on the slot image.

* **Opening (title, field op FE 2F).** Records from `char20.bin` (the dream set; `dream_records` set), default names, every record at full HP with growth bytes 0, unlock masks `{0x40, 0x40, 0x40, 0, 0, 0, 0, 0}`, every grid cell's equipped nibble 5, items and elements emptied, gold 100, 20 stars (total, current and preview), then the star unlocks of stars 0..19. Options word 2 (Accuracy Selection = Auto; the other three bytes 0), the default pad tables, play time 0, party `{Serge, -, -}`, relief member 0xFF, only Serge recruited, key items = Time Shifter only, frames 0x203, variables 0..0x3CF = 0 and variable 0x42 = 0. Variables 0x3D0..0x3FE are re-seeded: 0x3D2 = 0x3F, and 0x3E0, 0x3E2, 0x3E6, 0x3E8, 0x3F0 = 0x40A, 0x404, 0x2060, 0x80, 0x2000 (0x3E0..0x3FE are the 256-bit Dopplegang pool, so these are the creatures it starts with); the rest are 0.
* **Real start (FE 3F, normal branch).** The same, but records from `char.bin` with names kept, 0 stars and no star unlocks. A New Game+ skips this and keeps the carried state (section 14).
* **Growth snapshot.** Per record, with `star` = the total star count: `hp_extra` := max HP and `bonus[2k]` := the current stat (attack, hit, defence, evade, magic, magic defence); pending gains are `bonus[2k+1]` and the HP pending word, each `growth_roll(gap, star)` with gap = the `lv99para.bin` value minus the current value. `growth_roll` draws `rnd(1, 999)` and returns `round(2 * roll * gap / ((101 - star) * 1000))`, a fraction of .5 or more rounding up; the HP roll gets +2 and is clamped to 0..999. One RNG serves all records in order; per record the rolls go HP, attack, hit, defence, evade, magic, magic defence.
* **Key items.** The Time Shifter (key item 62) is granted by every new game (`reset_party`) and by every load (`ensure_time_shifter`). The Relief Charm (key item 61) is granted only by the New Game+ branch of FE 3F and by the carry-over; a normal new game does not have it.
* **Pip on Continue+.** When Pip (record 43) is in a form (model byte 0x2B..0x30), `rde_playinit_end` re-rolls his seven pending growth values against the `char.bin` / `lv99para.bin` record of that form: with stars < 99, pending = `rand() % ((target - snapshot) / (99 - stars) + 1)`, 0 when the snapshot is not below the target; with 99 stars all 0. The unlock masks of the cleared save are then ORed into Pip's.
