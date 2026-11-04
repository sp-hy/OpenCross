# Movies: which FMV a field script plays (op 7E / D4), file names, play range

This document covers how the field-script movie ops (`7E U` PLAY_MOVIE, `D4 U x` PLAY_MOVIE_SKIPPABLE; `7F` WAIT_MOVIE only waits for the end) choose a movie, how the movie number maps to a file in the remastered `movie.dat` archive (Bink 2, `.bk2`), which rows play only part of a file, and how OpenCross plays, ends and pauses a movie. `ocmovie table` lists everything below from the game data.

## 1. The operand is a table row, not a movie number
`7E U` and `D4 U x` read **row = low byte of U** (the high byte is the wipe variant, 0 or 1 in the data) and index a table in `field.prg`: disc 1 holds rows 0..23, disc 2 holds the following 13 rows (the same array continues). Row layout (0x18 bytes):

| off | field | meaning |
|---|---|---|
| +0 | start | frame offset used by some rows (section 4) |
| +4 | 1 | |
| +8 | end | frame count (STR frames = Bink frames, 15 fps) |
| +0xC | mode | 0, or 3 / 4 for the two ending_s rows (disc 2 rows 11 / 12; variant flag) |
| +0x10 | **movie number** | |
| +0x14 | volume | 0x7F full, 0x6E on row 0, 0x50 / 0 on a few rows |

The operand is not the movie number. Rows 1..15 happen to be the identity, but row 0 is number 19 (the opening `newop`) and rows 16..23 map as 16 -> number 0 `a07b_ite`, 17 -> 16 `b17opkid`, 18 -> 17, 19 -> 18, 20..23 -> numbers 7 / 5 / 15 / 16 again. The title room (`opening.bin`) plays `D4 #0` on disc 1 = the opening FMV `a01op` (the logo scene appears when it ends or is skipped) and `D4 #1` on disc 2 = `titleb`. OpenCross: all rows are in `src/game/disc_index.cpp` (`DiscIndex::movie_row`).

## 2. Number to file
The movie's file index is **6035 + number** in the `.fnd` of the disc in the drive (`cdrom.fnd` disc 1, `cdrom2.fnd` disc 2; the record is `c:\movie\<name>.stv`). The disc is variable 0x42 (disc - 1) / the DiscCheck (`FieldWorld::disc()`). Disc 1 rows address the 20 disc-1 records (numbers 0..19), disc 2 rows the 11 disc-2 records. A record that is not `c:\movie\...` plays nothing.

Name to archive member: `movie/<name>.bk2`, except the three per-language movies: `newop` -> `a01op_<lang>`, `b28end` -> `b28end_<lang>`, `final` -> `final_<lang>`, suffix `en fr it de es jp`. `movie.dat` holds 46 members (stored, not compressed): the 6 languages of those three, `ending_s`, `title`, `titleb` and the other `a..` / `b..` movies. OpenCross: for each stem, `<name>_<lang>` is tried before `<name>`, and at each stem a mod's loose `.mp4` / `.mkv` before the `.bk2` (`movie::resolve_member`); the language comes from the config, so a mod may localise any movie.

## 3. Rows
Disc 1: 0 a01op (newop), 1 title, 2 a02koryu, 3 a03kidfb, 4 a04opstn, 5 a05kid, 6 a06jkotu, 7 a07a_jka, 8 a08hanyo, 9 a09ko_fb, 10 a10hyofb, 11 a11koryu, 12 a12kidfb, 13 a13hyo, 14 a14seisn, 15 a15kid, 16 a07b_ite, 17 b17opkid, 18 b18sikai, 19 b19selya, 20 a07a_jka (part), 21 a05kid (part), 22 a15kid (part), 23 b17opkid (part). Disc 2: 0 final, 1 titleb, 2 b28end, 3 final (silent), 4 b24hoshi, 5 b20kojin, 6 b22fujyo, 7 b23_7ryu, 8 b08hanyo, 9 b25mezam, 10 b26saise, 11 ending_s, 12 ending_s (part).

Rooms that request a movie (all rooms scanned with `tools/py/cc_fieldscript.py disasm`): bak_a17e, chi_b03d, crn_b13i, crn_b16i, des_a06f, des_b06f, ending, gal_b06i, jac_b04f, jac_b20i, jac_b44f, kji_b06i, mis_b02e, opa_a04f, opa_b04f, opening, opn_a05i, ryu_a29i, ryu_b28i, ryu_b29i, sui_a09e, ten_b07f, you_b06i, you_b10e. Examples: opening `D4` 0 / 1 / 2, opn_a05i `7E 3`, you_b06i `7E 10` -> a10hyofb, jac_b20i `7E 7` / `D4 16`, ryu_b29i `7E 13..17`, you_b10e `7E 21 / 22`, `D4 23`, gal_b06i `7E 20`, bak_a17e `7E 18`, ending `7E 0 / 11` and `D4 2` (disc 2). A session that visits none of them logs no movie request after the title FMV.

## 4. Play range
Table frame numbers are 1-based at 15 fps: playback starts at `max(start, 1)` and plays to `end` (clamped to the file). Only some rows give a range:
- disc 1 row 20: start = 1 + 0x131 = 306 (not the row's own start field), end = row end (489);
- rows 21 and 23: start = 1 + row start = 1 (the beginning), end = row end (90 / 258, shorter than the file);
- row 22: start = 1 + 0x118 = 281, end = row end - 10 (417);
- disc 2 row 12: start 1, end = whole file (0). Every other row plays the whole file.

OpenCross: `DiscIndex::movie_range` returns the range; `movie::Player::open(..., start, end)` converts it to 0-based decoder frames at the file's own rate (`(start - 1) * fps / 15`; `end` is exclusive, scaled the same way), decodes and drops the frames and audio before `start` (clock and audio start there) and stops at `end` (audio is cut at the same time).

**Volume.** RDE starts every row's Bink sound at `vol / 127` of its movie volume; OpenCross scales the movie's sound by the row's `vol / 127` on top of the movie bus (`Mixer::movie_start(level)`): 110 for the opening, 80 for `final` and the two `ending_s` rows, **0** for disc 2 rows 2 (`b28end`) and 3 (`final`), which play without sound.

**The music keeps playing.** Neither the PS1 nor RDE stops the field's song for a movie: the PS1 movie loop suspends the music only while the CD stream runs dry (a stall, not the movie), and RDE replaces the stream routines with Bink and never touches the music. A script that wants silence stops the song itself (`88 FF` or an `FE 1D` fade before the `7E`). The true ending relies on it: opa_b04f (entrance 2) starts song 7, fades it in over 90 frames and plays disc 2 row 2 (`b28end`, whose Bink track is digital silence) under it, then fades the song out after `7F`. OpenCross leaves the BGM alone while a movie plays (`App::start_movie`); only the pause screen holds it (`Mixer::hold`).

**The staff roll (disc 2 rows 11 / 12, modes 3 / 4).** The row's mode 3 or 4 switches the movie loop to the staff-roll layout: the movie is shown in a panel that changes place across the roll, and a credits strip scrolls beside it, one new row per movie frame, kept in a 272-row ring. On the PS1 the rows come from the STR's own data sectors (0x1B0 bytes each = 144 pixels of 24-bit colour). RDE has no STR: its rows are the PNGs `hd/staff/staff24us.png` (English) and `hd/staff/staff24.png` (Japanese) in `hd.dat`, 144 x 8950 RGB, row k = the strip row of movie frame k (8950 = the frame count of `ending_s`), shown 1.5 times as wide as stored (216 PS1 pixels). RDE (and the PS1) therefore show white names and lilac section headings on black with the movie in the panel. OpenCross plays `ending_s` full screen and draws neither the panel layout nor the strip; where the panel stands in each frame is not yet known (the PS1 takes it from the stream, RDE from its own data).

## 5. Decoder and file access (OpenCross)
Bink 2 is decoded with librempeg (built locally by `scripts\build-librempeg.ps1`, commit pinned in CREDITS.md); audio is pulled from the same stream. YUV->RGBA runs on the decoder worker thread; the frame texture is created at the movie's own size.

- **Streams.** The first video and first audio stream are used; a movie without audio plays against the wall clock. Frame time = decode index / frame rate (`avg_frame_rate`, else `r_frame_rate`, else 15), not the container timestamps. Video decodes with automatic codec threading.
- **Mod codecs.** Besides Bink (`.bk2`), mods may supply HEVC or H.264 in `.mp4` / `.mkv` (any codec librempeg decodes; the file name is the container hint). Accepted pixel formats: YUV 4:2:0 / 4:2:2 / 4:4:4 8-bit, full-range 4:2:0 (YUVJ), 4:2:0 with alpha (alpha plane copied to RGBA alpha), 4:2:0 10-bit (shifted to 8); anything else logs an error and drops the frame.
- **Colour.** Bink is always full-range BT.601. Other codecs follow the stream tags: JPEG range = full, else limited; BT.709 / BT.2020 NCL = Rec. 709, untagged = Rec. 709 from 720 lines up, else Rec. 601.
- **Audio.** Float samples (packed or planar) only; mono is duplicated to stereo, channels above two are ignored, and the stream is resampled by linear interpolation to 48 kHz stereo float (`Decoder::kAudioRate`).
- **Pacing.** The player keeps up to 8 decoded frames and about 1 s of audio ahead. The clock starts once the first frame and 0.25 s of audio are ready; it then follows the mixer's consumed movie sample count (never going backwards) and switches to the wall clock once the audio is drained. The movie ends when the decoder is drained, the queue is empty, one frame period has passed and the audio has played out. A movie that cannot be opened is skipped and the script continues.

**Mod movies stream from disk.** A `movie/<name>.bk2` supplied by a mod is a loose file (often hundreds of MB, e.g. 1280x960 or 1920x1440 upscales). `Player::open` asks `Vfs::loose_file(member)`; when the overlay resolves the member to a file on disk, `Decoder::open_file` reads it through a buffered, seekable file AVIO (no whole-file read, no start stall, no RAM copy). Archive members (`movie.dat`) go through `Vfs::read` + `Decoder::open` (memory AVIO). The member name is resolved with `exists()` (overlay first), localized `_<lang>` before plain, so `a01op_en.bk2` etc. are found.

`FieldWorld::play_movie` logs a warning when a second movie is requested while the first is still requested or playing.

While the STR plays (wipe variant 1) the retail loop runs the field's frame driver once per movie frame with the field picture off: the logic half runs (entity scripts, movement and triggers, the screen fade, camera scroll), the picture half does not (animation requests, background layers, models and shadows, effects, dialogue windows); the movie's own room sequence (`mo1?` / `mo2?`, `room_effects.md`) runs alongside. Every one of the 48 `7E` / `D4` ops in the scripts is followed directly by `7F WAIT_MOVIE`, so the calling script stands still; only other entities' scripts and a running fade advance. OpenCross ticks only the room sequences during playback (`FieldWorld::movie_tick`).

## 6. Movie end: hold the last frame and cross-fade into the scene
Retail: the movie loop of op 7E / D4 (wipe variant 1 is the real STR playback) fades during the last 8 frames, then rebuilds the field display and the script continues in the same frame: no black gap, no load between the movie and the next scene. RDE replaces the routine with Bink, so the fade is OpenCross's own: `movie::Player::close(true)` keeps the last decoded frame (`held()`); `draw_held()` draws it over the finished frame and `fade_held(dt, 0.5)` lowers its alpha over **0.5 s** (30 ticks, the title's own fade length). The fade starts when the field has run **4 ticks** after the movie (or the room changed or was reloaded), so a map jump right behind the movie fades over the new room. `App::end_movie` (`src/app/app_sim.cpp`) does this for every FMV (title opening, 7E / D4 from scripts, skipped or finished). Route: `title_fade`.

## 7. The title script (`opening.bin`) and the New Game movie
The second operand of every `D4` here is `#520` (0x208), the wipe / flag word; the first is the table row.
- UPDATE script (runs on entering the room): `OPFE1D #20,#0` (music fade, 20 frames), `WAIT 20`, `PLAY_SONG #255` (stop), then by `$ROOM_PARAM`: 0 -> `D4 #1` (row 1 `title`) unless var 0x42 == 1 (disc 2) where `D4 #0` plays; param 1 -> `D4 #1` only when var 0x42 != 0; param 2 / 3 -> the same `D4 #1` / `D4 #0` pair after `SET_0 $3E`. Row 1 `title` is the title loop / logo FMV played when the title room is re-entered (after game over / reset); row 0 (`a01op`) is the cold-boot opening.
- After the logo scene: `OPFE1D #20,#0`, `FADE 1,#32`, `WAIT 32`, layers 0 / 1 off, `PLAY_SONG #255`, `WAIT_SONG`, then the menu / name entry (`OPEN_MENU`, `RENAME_CHARACTER #0`, `OPEN_MENU`, `WAIT 24` each).
- **New Game tail**: `ADD_TO_PARTY #0`, `#1`, `RAND_MOD $4,#14` -> `ARRAY8_GET 0x310` -> `ADD_TO_PARTY` (third member), each followed by `COMMIT_PARTY_MODEL`; `MAP_JUMP_DC #22,#0` (sets the pending jump; the room changes when the script ends); then `D4 #2,#520` (`a02koryu`, skippable) + `WAIT_MOVIE` + `STOP_EXEC`. The fly-in to Fort Dragonia plays over the title room with no further fade or music op (music was already stopped), and `opn_a01i` loads when it ends.
- OpenCross: `FlowMachine::on_name` returns `FlowAction{movie_row = 2, load_room}`; `App::apply_flow_action` requests the row (`FieldWorld::play_movie(2, 0, true)`), starts it at once and keeps the room load (`deferred_flow`) until the movie ends or is skipped (a missing file loads the room immediately). Route `newgame`.

## 8. Pause over an FMV (OpenCross; not retail PS1 behaviour)
The PS1 has no pause during an STR movie. RDE's pause path calls Bink pause, but whether Start reaches it inside the movie wait loop is unknown, so this is an OpenCross feature:
- Start / P while a movie plays (any movie, skippable or not) opens the RDE pause screen over the held frame (`App::pause_modal`, `src/app/app_sim.cpp`): `movie::Player::set_paused(true)` makes `update()` do nothing (the wall clock stops, no frame is picked, the on-screen frame stays and `draw()` shows it through the CRT ramp filter), and `Mixer::hold(true)` stops the movie PCM ring and its clock (`movie_frames_played`, the audio clock the player follows) and silences the music and voice buses. The decoder thread fills its ready queue and waits. Resuming (Start / P / B / X) continues frame and sound exactly where they stopped.
- Skip while paused is not possible: the pause screen is modal and swallows every button. Resume first, then skip with confirm (skippable movies only). Esc still opens the settings window, which pauses the host as before.

## 9. Verification
Tests: `tests/test_flow.cpp` (`DiscIndex` movie checks incl. ranges), `test_movie` (`test_mixer_hold`, `test_player_pause_flag`, the wall-clock Player check). Routes: `tests/routes/movie.txt` (title room `D4 #0` -> `movie/a01op_en.bk2`, skip with confirm), `title.txt` / `title_orig.txt` / `newgame.txt` (boot FMV = a01op), `movie_opn.txt` (opn_a05i `7E #3` -> `a03kidfb`), `movie_flow.txt` (title -> New Game -> name -> opn_a01i, then `load opn_a05i 1` requests and starts `a03kidfb`).
