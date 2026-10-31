# RDE archives, the cdrom2 override rule, mod overlay and inner containers

The Steam edition ships the game data as ZIP archives in `<GameDir>\data`: `cdrom.dat` (the PS1 file tree), `cdrom2.dat` (HD overrides of PS1 files), `hd.dat` (chip2 backgrounds, HD TIMs, PNG UI, portraits), `lang.dat` (`docs/formats/text.md`), `font.dat` (spritefonts), `ui.dat` (PNG UI), `movie.dat` (Bink 2, `docs/formats/movies.md`). `launcher.dat` and `radicaldreamers.dat` (launcher resources) are not read by the engine. The save archive `save.dat` lives in the Steam save folder, not in `data` (`docs/formats/save.md`).

## 1. Archive format
- The `data\*.dat` files are plain ZIPs: no encryption, members stored (method 0) or deflated (method 8), directory entries included. `movie.dat` is all stored (so a member can be read in place). Member names are relative to the archive and use `/`.
- A logical path is `<archive>/<member>`: the first segment names the archive (`cdrom/map/mapbin/alu_a01f.bin` = member `map/mapbin/alu_a01f.bin` of `cdrom.dat`; `movie/a01op_en.bk2` = member `a01op_en.bk2` of `movie.dat`). Paths are case-insensitive, `\` is read as `/`, a leading `/` is dropped. Callers always ask for `cdrom/...`; `cdrom2` is never addressed directly.
- `Vfs::open` requires `data\cdrom.dat`; archives are mounted lazily and their central directory is indexed by the normalised member name. A read that finds no member returns nothing (callers decide whether that is fatal).
- `save.dat` is a WinZip **AE-2, AES-256** ZIP (`save::zip_open` / `zip_write`):
  - Per member: method 99, general-purpose flag bit 0 (bit 1 set for deflated members), extra field `0x9901` `{u16 7; u16 2 (AE-2); "AE"; u8 3 (AES-256); u16 real method}`; the real method is 8 (deflate) or 0 (stored).
  - Data = `salt[16]`, password verifier `[2]`, ciphertext, HMAC-SHA1 tag `[10]`.
  - Key derivation: PBKDF2-HMAC-SHA1, 1000 iterations, 66 bytes = AES key `[32]`, HMAC key `[32]`, verifier `[2]`. Cipher: AES-256 in CTR mode with a 16-byte little-endian counter starting at 1. The HMAC covers the ciphertext only.
  - AE-2 stores the CRC-32 of the plaintext only for payloads of 20 bytes or more (0 below); the reader checks the CRC when it is non-zero. Fixed password: the RDE save password constant in `src/save/encrypted_zip.h`.
  - Written entries use: version made by 0x033F, needed 20, Unix external attributes `0100666`, fixed DOS timestamp, deflate level 9. A save is written to `<path>.tmp` and renamed over the old file.

## 2. cdrom2 override rule

* **HD art mode**: every `cdrom/<x>` read first tries `cdrom2/<x>`; if present it wins (HD-mode rooms, models, textures).
* **Original art mode**: only a fixed exception list goes to cdrom2: `map/etc/openfont.tim`, `menu/title/cmptitle.tim`, `map/mapbin/ending.ctd` and every member under `map/window/`.
* `Vfs::read_disc` reads `cdrom/...` from `cdrom.dat` only, in both modes (the mod overlay still applies): the PS1 screens that need the original textures use it, because the cdrom2 copies of some PS1 TIMs (e.g. `map/window/w*.tim`) are all transparent.
* `exists` follows the same order as `read`.

OpenCross: implemented in `src/vfs/vfs.cpp` (`Vfs::set_hd_mode`, `Vfs::cdrom2_first`). The art-mode toggle (F6 / options) re-points the VFS and re-reads the art of what is on screen, never the simulation. For a room, the cdrom2 variant differs from the disc only in `.bin` subfiles 11+ (room-model meshes and UVs) and the `.ctd` texture pages (213 / 214 of 566 rooms); script, walk mesh, camera, zones, CLUTs and every `.prd` are byte-identical. Models are rebuilt from the new set together with their meshes, so HD models come with their HD meshes (cdrom2 meshes carry the 4x UVs); the animation clips are the same data in both sets.

## 3. Mod overlay precedence
Mods are folders of loose files under `<GameDir>\mods\<mod>\`, or archives and bare `.dat` files in `<GameDir>\mods` (`docs/formats/mods.md`), resolved once to logical paths and installed as an overlay for the current art mode. Lookup order for `read(path)`:
1. the overlay's file for the exact logical path, loose or a member of a packed mod or a mod's `<archive>.dat`, read in place (first enabled mod in list order wins); an unreadable file falls through;
2. `cdrom2` (only for `cdrom/...` paths and only under the rules of section 2);
3. the shipped archive named by the first path segment.

A mod member named `cdrom/...` also replaces the cdrom2 copy (cdrom2 members of a mod count as `cdrom/`). `list` is always the shipped archive: a mod replaces members and adds none. `loose_file` returns the on-disk path only for loose overlay files (never members of a packed mod), so large movies can be streamed. The overlay carries a generation counter that changes with its content; texture caches key on it.

## 4. Containers inside members
Importers in `src/import/container.*`, `lzss.*`:
- **cpt**: `u32 count; u32 offset[count]; optional u32 eof`. The EOF word is taken as present when the first offset is at least 4 past the table, the word after the table is >= the last offset and within the buffer; otherwise the last entry runs to the end of the buffer. Entries must be monotonic and inside the buffer, `count` 1..0x10000.
- **drp**: `"drp\0"`, `u32 at +8 = 64 * count`, `u32 offset[count]` at +12. Subfile header (12 bytes): `u32 0; char name[4]; u8 type; u24 = 16 * payload length`, payload follows (the C++ parser clamps it to the buffer; the Python importer takes the span to the next subfile).
- **sszl** (LZSS): `"sszl"; u32 decoded length; u32 reserved`; MSB-first bitstream from +12: bit 1 + 8-bit literal, or bit 0 + 12-bit ring position + 4-bit count (copies count + 2 bytes from absolute ring indices). The ring is 4096 bytes, zero-initialised, write pointer starts at 1, every output byte is written to the ring. Decoding stops at the declared length or at the end of the stream.
- **CDMAKE dummy**: a member whose first word is `"It's"` (0x73277449) is a placeholder, not data.
