# PS1 disc images: containers, the file table, identification

This document covers how OpenCross reads a Chrono Cross PlayStation disc from an image file or an optical drive: which containers it opens, how sectors are addressed, where the game keeps its files, how a disc is identified as Chrono Cross, by disc number and by build, and how a drive is read (section 7). The code is `src/import/disc/` (library `oc_disc`); `opencross ocdisc` (`docs/tools.md` 1.15) exposes it. Unlike the other importers, `oc_disc` opens the image files itself.

## 1. Sectors

A CD sector is 2352 bytes: 12 bytes of sync (`00`, ten `FF`, `00`), a 4-byte header (minute, second, frame in BCD, mode), then for mode 2 an 8-byte subheader (file, channel, submode, coding, written twice) and the payload: 2048 bytes for form 1 (followed by EDC/ECC), 2324 for form 2 (submode bit `0x20`). Mode 1 puts its 2048 bytes right after the header. An audio sector has no sync and no header.

Sectors are addressed by LBA from the first sector of the first track (LBA 0 = disc time 00:02:00). `SectorSource` (`sector_source.h`) is the image as an array of sectors:

- `read_raw(lba, count, out)`: whole 2352-byte sectors; only when `raw()` is true.
- `read_user(lba, count, out)`: 2048 bytes per sector: the mode-1 payload or the mode-2 form-1 payload (for a form-2 sector, its first 2048 payload bytes, as a drive in 2048-byte mode returns them).
- `read_form2(lba, out)`: the 2324-byte payload of a mode-2 sector.
- `info(lba)`: mode, subheader fields and form.

Every read is thread-safe (one lock per source).

**Raw and cooked.** A raw image stores whole sectors; a cooked image (`.iso`, a `MODE1/2048` cue track) stores only the 2048 user bytes of each sector, so headers, subheaders and form-2 payloads are gone and `raw()` is false. For Chrono Cross this loses nothing: every sector of every file, the movies included, is mode 2 form 1 (the movie sound is stored inside form-1 sectors, not as XA audio), and the only form-2 sectors on the disc are the empty system-area sectors 12 to 15. A cooked image carries all the game's data.

## 2. Containers

The container is recognised by the file's content, not its name (`detect_format`):

| Container | Recognised by | Notes |
|---|---|---|
| CHD | `MComprHD` at offset 0 | CD-type CHD (version 3 to 5, any codec libchdr decodes: `cdlz`, `cdzl`, `cdzs`, `cdfl`, `zlib`, none), read with the vendored libchdr. The disc is 2448-byte frames (2352 sector bytes + 96 subchannel bytes, the latter dropped), eight to a hunk; the track list is in the `CHT2` / `CHTR` metadata strings, the tracks back to back with each padded to four frames. A track's pregap that the file does not store (`PGTYPE` not starting with `V`) reads as zeros and takes up LBAs, as a cue `PREGAP` does. Track types `MODE1_RAW`, `MODE2_RAW`, `AUDIO` give raw sectors, `MODE1` / `MODE2_FORM1` (2048 bytes) give a cooked source, `MODE2` / `MODE2_FORM_MIX` (2336 bytes) get a rebuilt sync and header. Refused: images that need a parent, old `CHCD` metadata, `MODE2_FORM2` tracks, anything whose hunks are not a multiple of 2448 bytes (hard disk, DVD). Hunks are CRC-checked by libchdr; the last eight decoded hunks are cached. |
| PBP | `\0PBP` at offset 0 | See below. |
| Alcohol `.mds` | `MEDIA DESCRIPTOR` at offset 0 | The data file is the one the track block's footer names; `*.mdf` means the descriptor's own name with `.mdf`. Sector sizes 2048, 2352 and 2448 (2352 + 96 bytes of interleaved subchannel). |
| Raw image (`.bin`, `.img`) | sync pattern at offset 0 | Sector stride 2352, or 2448 when the next sync is 2448 bytes on. |
| Cooked image (`.iso`) | `CD001` at offset 0x8001 (the volume descriptor of sector 16) | 2048 bytes per sector. |
| CloneCD `.ccd` | text starting `[CloneCD]` | The sectors are the `.img` of the same name, raw 2352; a `.sub` next to it is noted, not read. |
| Cue sheet | text with `FILE` and `TRACK` lines | `FILE "<name>" BINARY` (quoted or bare names, relative to the sheet, several files per sheet), `TRACK nn <mode>` with mode `MODE2/2352`, `MODE1/2352`, `MODE1/2048`, `MODE2/2048` or `AUDIO`, `INDEX`, `PREGAP` (a gap the file does not store; it reads as zeros). A track starts at its first `INDEX` in its file and runs to the next track's, the last to the end of the file; LBAs run on across files. Other file types (`WAVE`) and modes are refused. |
| `.m3u` playlist | text whose lines (blank lines and `#` comments skipped) all name existing files, relative to the playlist | Groups discs into a set, in line order. A playlist inside a playlist is refused. |

`open_disc_set(path)` returns every disc a file names (one for an image, one per playlist line, one per PBP disc); `open_image(path, &error, index)` returns one of them.

**PBP.** The header is `\0PBP`, a version, then eight section offsets; the last section (offset at 0x24) holds the disc data. It starts with either `PSISOIMG0000` (one disc) or `PSTITLEIMG000000` (several), whose table at +0x200 lists up to five disc offsets relative to the section. A disc's block index sits at +0x4000: 32-byte entries, a u32 offset and a u16 length, ending at the first zero length. Block data starts at +0x100000; each block is 16 raw sectors (37632 bytes), raw-deflated, or stored as is when its length is the full 37632. Encrypted PBPs (store releases) have no such index or do not inflate to a synced first sector, and are refused with "encrypted PBPs are not supported".

## 3. The ISO9660 volume

Sector 16 is the primary volume descriptor (type 1, `CD001`): system id at +8, volume id at +40 (32 bytes, space-padded), volume size in sectors at +80, root directory record at +156. The root directory holds only two files: the boot executable and `SYSTEM.CNF`, whose `BOOT = cdrom:<name>;1` line names the executable. Everything else on the disc is reached through the game's own file table.

| Disc | Volume | Boot file | Executable LBA, size | `SYSTEM.CNF` LBA |
|---|---|---|---|---|
| Japan disc 1 and 2 | `CHRONOCROSS` | `SLPS_023.64` | 38, 632832 | 347 |
| USA disc 1 | `CHRONOCROSS` | `SLUS_010.41` | 38, 606208 | 334 |
| USA disc 2 | `CHRONOCROSS` | `SLUS_010.80` | 38, 606208 | 334 |

The two USA discs carry the same executable under two names; their `SYSTEM.CNF` differ in that name only.

## 4. The file table

The user data of sectors 24 to 35 (12 sectors, 6144 words) is an array of little-endian u32 words, one per file record. Word `i` describes record `i` of the disc's name list (`cdrom.fnd` for disc 1, `cdrom2.fnd` for disc 2; `docs/architecture.md` 3.6):

| Bits | Meaning |
|---|---|
| 0-22 | Start LBA |
| 23 | Empty slot: the record holds no data |
| 24-31 | Unused bytes of the file's last sector, divided by 8 |

The table ends at the first zero word: both discs of both builds have 6060 records (the `.fnd` count). Size of record `i` = (LBA of record `i + 1` - LBA of record `i`) x 2048 - (bits 24-31) x 8; an empty slot has size 0 and carries the LBA of the next file; the last record runs to the end of the image. Sizes are therefore rounded up to 8 bytes (`SYSTEM.CNF`, 68 bytes in the directory, is 72 in the table). Record 0 is the boot executable and record 1 `SYSTEM.CNF`. Records 6035 and up are the movies (`c:\movie\<name>.stv`), stored on the disc like any other file. `FileTable` (`disc_table.h`) decodes the table; `read_record` reads a record's bytes.

Japan discs 1 and 2: every record whose `.fnd` name is a `cdrom.dat` member is byte-identical to it, apart from trailing bytes within the last sector (`ocdisc verify`); `system.cnf` has no `cdrom.dat` member. The USA discs use the same numbering, but 1228 records of disc 1 and 1249 of disc 2 differ from the `cdrom.dat` member of their name, and records 234 (88632 bytes) and 2059 (256 bytes) hold data while their `.fnd` names are empty.

**Trailing bytes.** The table's sizes are the files' sizes rounded up to 8 bytes, and the game reads that many. On Japan disc 1, 2089 of the 5584 named records that have a `cdrom.dat` member are 1 to 7 bytes longer than it (2003 by 4), the rest the same size; the extra bytes are zeros. `cdrom.dat`'s members (and RDE's other archives) have the files' own sizes. So an importer whose format runs to the end of its buffer accepts up to 7 bytes after the end of what it decodes (`import::ends_at`, `kFilePadding`) and ignores them; a buffer shorter than the format needs is still refused. The one such place in the game's files is the last entry of a cpt without an EOF word: the last image object of a `.ltd` Atim (`model.md` section 8), the texture of every battle monster. Formats that carry their own length (a cpt with the EOF word, drp entries, TIM, sszl, AKAO banks and songs, battle scripts, the room files) ignore what follows it anyway, and the battle tables read as whole files (`battle/data`) give the same records. A drp entry is cut at the end of its buffer: the last entry of 7 room `.prd`s (`hes_a03f`, `inj_a01f` ...) declares 2 to 6 bytes more than the `cdrom.dat` member holds, and the disc file holds them, as zeros. `ocdisc audit` (`docs/tools.md` 1.15) runs every file of a disc image through the importers the engine uses for it next to the `cdrom.dat` member of that name.

## 5. The disc-number sector

The user data of sector 37 starts with a u32 disc number, 1 or 2. Its first 34 bytes equal `cdrom.fid` (disc 1) or `cdrom2.fid` (disc 2) of `cdrom.dat`, disc number included, and are the same for the Japanese and USA discs of the same number.

## 6. Identification

`identify(source)` (`disc_ident.h`) decides, in order:

1. The volume descriptor and root directory read, `SYSTEM.CNF` names a boot file and the root holds it. Its SHA-256 is computed.
2. The volume id is `CHRONOCROSS`, the file table decodes with start LBAs that never decrease and stay inside the image, and the disc-number sector holds 1 or 2. Otherwise the disc is not Chrono Cross (the reason is reported).
3. The executable's SHA-256 against the known builds:

| SHA-256 of the executable | Build |
|---|---|
| `441a6ed70fc2e8e9a0e6ac9d30586359dd163f548e7ecff04e7df7adca096f61` | Japan (SLPS-02364 / SLPS-02365), both discs |
| `9a5453bf09d7f5c71a6451d255471374fb2d7cc5f3dfb1ea51223a5c88cdda47` | USA (SLUS-01041 / SLUS-01080), both discs |

4. An unmatched executable on a disc that passed step 2 is an unknown build. Its closest region follows the boot file's prefix: `SLPS`, `SLPM`, `SCPS` = Japan, `SLUS` = USA; any other prefix gives no region.

## 7. Physical drives

A disc in an optical drive is read through `DriveSource` (`drive_source.h`), a `SectorSource` over the platform drive API (`src/platform/optical.h`, which knows sectors and nothing about the game). On Windows, and under Wine / Proton where the drive is mapped to a letter, a drive is any drive letter of type CD-ROM; other systems report none.

**Opening.** The drive must hold a disc (the storage check-verify query answers). The sector count is the larger of the table of contents' lead-out address minus the 150-frame lead-in and the device length / 2048, then reduced to just after the last sector that reads (searching back at most 16 sectors): the two reports can disagree by a sector, and the last file of the disc runs to its end (section 4). Raw reads are tried in this order on sector 16 of the first data track: a SCSI `READ CD` pass-through with sector type "any" and every field selected (sync, headers, user data, EDC/ECC), the CD-ROM raw-read control in XA form-2 track mode, then in mode-2 track mode. The first method whose sector has the sync pattern and a header address equal to its LBA + 150 is kept. When none works the source is cooked: plain 2048-byte reads of the volume device at offset LBA x 2048, which return the form-1 user data of each sector. A drive in this mode refuses form-2 sectors; for Chrono Cross those are only the unused system-area sectors 12 to 15, so a cooked drive loses nothing (section 1). Requests are split into runs of 16 sectors. The system's "insert a disc" dialog is suppressed while a drive is queried.

**Cache.** An optical drive pays for every seek, so reads go through a cache of 16 blocks of 32 aligned sectors (about 1.2 MB raw). A miss reads every missing block of the request in one device request, so a read of one sector also reads the rest of its block; blocks are evicted least recently used first. A failed device read is retried once. When a run still fails (for instance a cooked block holding a form-2 sector), its blocks are kept with every sector marked unread and each requested sector is then read on its own, so the readable neighbours of a sector the drive refuses still read, and only a request for that sector fails.

**Media change.** The drive's media-change count is noted at open. It is checked on every cache miss and by `media_changed()`, one device query, cheap enough to poll. Once the count differs or the disc is gone, the cache is dropped and every later read fails, even of the same disc put back: the caller opens the drive again and identifies what is in it.

**Discovery.** `probe_optical_drives()` lists every drive with its device name, whether it holds a disc, the volume label, the opened source and its identity (section 6); `scan_optical_drives()` keeps the drives holding a Chrono Cross disc. `ocdisc drives` prints the probe.

## 8. Disc change

**The disc header.** The user data of sector 23 is what the game reads to tell its discs apart: u16 disc number (1 or 2), u16 2, then three u16 pairs (sector, byte count): (23, 44) the header itself, (24, 0x60E0) the file table, (37, 34) the disc-number sector; `CHRONOCROSS` (NUL-padded) at +0x20. The four discs differ only in the disc number.

**When the game asks.** The disc number read at boot is the disc in the drive. Script variable 0x42 holds the disc the story needs, minus one. The field compares the two on every frame and runs the disc-change screen before the frame goes on when they differ. Two things set it off: `FE 65 00 n` (variable 0x42 = n, and a disc change is marked pending; `FE 65 01` then waits until it is done), and loading a save whose variable 0x42 names the other disc (the screen comes up before the loaded room is shown, so over a black picture).

**The screen** (the same in both builds apart from the positions below):

- The picture of the last frame stays on screen, frozen. A full-screen subtractive grey quad darkens it: 0, 4, 8, ... 124, one step per 60 Hz frame (32 frames), and the screen holds the last level while it waits.
- From the first frame, the prompt line is drawn over it, unshaded and opaque (texels of colour 0 transparent): Japanese `DISC n に入れ換えて下さい。`, USA `PLEASE INSERT DISC n`. The art is entry 5 of `map/etc/systim.bin` (a cpt of TIMs), a 4bpp 256 x 32 TIM with a white-to-black grey ramp CLUT: rows 0 to 15 the prompt with an empty digit cell, rows 16 to 31 the "wrong disc" line (Japanese `DISCが違います。`, USA `WRONG DISC!`) in u 0 to 127 and the digits 1 and 2 as 12 x 16 cells at u 140 and 156. The needed digit is copied into the prompt's cell before it is drawn.
- Placement in the 320 x 224 field frame, rows 104 to 120 for both lines:

| | Prompt line (255 texels wide) | Digit cell at u | "Wrong disc" line (128 texels wide) |
|---|---|---|---|
| Japan | x 57 | 44 | x 97 |
| USA | x 72 | 160 | x 109 |

- Nothing reads the pad. The drive is watched instead: the check waits until the lid has been opened and closed again and the disc has spun up, then reads the disc header. It is accepted when `CHRONOCROSS` is there and the disc number is the needed one; the build is not compared (the Japanese game takes a USA disc of the right number). Anything else (no disc, an unreadable disc, another game, the other disc) shows the "wrong disc" line instead of the prompt with the quad at 128 for 126 frames, then the prompt for 4 frames, and the screen waits for the next lid cycle.
- Accepted: the game reads the new disc's file table and disc-number sector, the disc in the drive becomes the needed one, and the quad goes back up from 124 to 4 over 31 frames with no text. The frozen picture is put back and play continues on the same frame of the same room: no room is reloaded. A pending `FE 65 01` lets its script go on.

**OpenCross.** `ui::DiscPrompt` (`src/ui/disc_prompt.h`) draws this screen in disc mode with the art of the disc in the drive and the layout of its build, at the same frame counts. A PC has no lid: a source is tried when something changes (a drive reports a media change, an image appears where the sources are searched, a file is picked) or when the player asks for a rescan, and a failure of any kind shows the "wrong disc" line as the PS1 does. Unlike the PS1, a disc of the other build is refused. Two actions the PS1 does not have, "Browse for image..." (a file dialog) and "Rescan" (the configured paths, the exe folder, the drives), are named on one dim line of PS1-font text near the bottom of the picture, well below the prompt, and shown except while the screen fades out. The app keeps the field frozen while `active()` is true.
