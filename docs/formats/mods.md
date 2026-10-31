# Mods (loose replacement files) and the Modded art mode

RDE itself has no mod loader; mods for it are folders of loose files that replace members of the `data\*.dat` archives. This document describes how OpenCross loads them (`src/vfs/mods.*`, `src/vfs/vfs.*`): the mod folder layout, archive and single-file mods, how each loose file is matched to an archive member, art modes and priority, the voice-line layout, and the texture dump / replacement system. Everything here is OpenCross behaviour, not an RDE feature.

## 1. Folder rules

```
<GameDir>\mods\<mod name>\<replacement files, any depth>
<GameDir>\mods\<mod name>.zip | .7z | .rar
<GameDir>\mods\<archive>.dat
```

* A missing `<GameDir>\mods` is created at boot (empty; a warning is logged when the folder is read-only).
* Every sub-folder of `mods` is one mod, and so is every `.zip`, `.7z`, `.rar` and `.dat` file directly in `mods`. Its **folder or file name** (extension included, so `Pack` and `Pack.zip` are two mods) is the name shown in the Esc overlay's *Mods* tab and the `[Mods]` ini key. Other files directly in `mods` are ignored.
* A bare `.dat` is a mod of that one file, resolved like a file at a mod root: `mods\lang.dat` overlays `data\lang.dat` (section 2, rule 1).
* An archive reads like a folder (section 1.1).
* Everything below a mod folder is scanned recursively; each file is resolved on its own (section 2). Layouts seen in the wild:
  * partial path: `Chrono Revision\face\2021\00_seljyu.png` -> `hd/face/2021/00_seljyu.png`;
  * extra wrapper folders: `HD Boat\HD Boat Plus\fieldobj\player\cl_bort\cl_bort.tim` -> `cdrom/fieldobj/player/cl_bort/cl_bort.tim`;
  * flat files at the mod root: `Zeightgeist HD Backgrounds\alu_a01f.chip2` -> `hd/map/mapbin/alu_a01f.chip2`;
  * archive overlay: `Snooty AI Upscale Videos\movie.dat` -> its members replace those of `data\movie.dat`;
  * loose movie: `a01op_de.mp4` (or `.mkv`, any folder) named after a shipped `a01op_de.bk2` -> replaces that movie (section 2, rule 3).

### 1.1 Archive mods

A ZIP, 7z or RAR (4 or 5, solid included; the extension selects the file, the format is recognised by content) is read live and never unpacked: its files resolve exactly as those of a folder would, and nothing is written to disk (`src/vfs/mod_archive.*`). A file of a packed mod is an overlay file with a `member` (`OverlayFile`), and every consumer takes bytes through `Vfs::read_mod_file`, so textures, game files, voices and movies work the same as loose files. The list is built at the scan whether the mod is enabled or not.

* **ZIP** (stored or deflated members). Each member is read on its own through miniz.
* **Everything else** (7z, RAR, a ZIP with another method) can only be walked from the start. An archive of up to 256 MiB unpacked is decoded into memory at its first read and served from there. A larger one is walked for each read: fast when its members are stored separately, slow when it is solid, because every read decodes from the start of the data. Such an archive shows *May run slowly: extract it to a folder* under its name in the Mods tab (`Mod::may_be_slow`; whether it is really solid cannot be told in advance), and a read that takes over 0.5 s logs a warning once. The memory goes when the overlay stops using the archive.
* **Scan cost.** Listing a ZIP or 7z reads its directory only. A solid RAR has no directory: listing it decodes the whole archive, at every scan.
* **Wrapper folder.** When every member sits in one folder, as from zipping a mod folder, the mod starts inside it (repeatedly), unless the folder is called `voices` or `textures`. This puts `Pack\voices\...` at the top of the mod, where the voice lookup expects it. An archive whose stem is `voices` reads like a folder called `voices`, with no wrapper removed.
* **Limits.** A password-protected, damaged or multi-part archive shows *cannot read* in the *Issues* column. An `<archive>.dat` inside a packed mod is unmatched: unpack the mod, or put the `.dat` directly in `mods`. A movie inside an archive is read whole into memory when it starts, not streamed.
* A file held open by the game cannot be replaced or deleted until *Rescan* or a mod switch stops using it. A replaced archive (new size or time) is picked up at the next *Rescan*.

## 2. Resolution (once per scan, case-insensitive, component-wise)

1. **Archive overlay.** A file named `<archive>.dat`, where `data\<archive>.dat` is one of the archives OpenCross reads (`cdrom cdrom2 hd lang font ui movie`) and the file starts with the ZIP signature `PK\3\4`, is a set of member replacements, not a new archive. Each of its members is resolved by rule 2 as the path `<archive>/<member>` (`cdrom2.dat` members as `cdrom/<member>`) and read in place from the ZIP, with nothing unpacked; every member it does not hold still comes from the shipped archive, so the file may hold one member or all of them. The members behave like loose files: they count in the mod's files and conflicts, the first active mod in the list wins each one, several mods' copies of the same `.dat` combine, and a `cdrom.dat` member wins over the `cdrom2` copy. A member that cannot be read falls back to the game's copy. A `.dat` not matching this rule is resolved as an ordinary file by rule 2.
2. **Member.** The candidates are all shipped members with the same file name. Members are named by the path the game reads, `<archive>/<member>`, with `cdrom2` members counted as `cdrom/` (callers always ask for `cdrom/...`; the mod file then wins over both the cdrom2 HD override and the disc file). For each candidate, count how many trailing path components it shares with the mod file's path (inside the mod folder). The candidates with the longest shared run win:
   * exactly one: **matched**. Leading folders of the mod path that do not occur (wrappers such as `HD Boat Plus`) are ignored; a bare file name works when the name is unique (`alu_a01f.chip2`);
   * several: **ambiguous**, not loaded (for example `00_seljyu.png` alone fits both `hd/face/1999/` and `hd/face/2021/`; add the folder);
   * none: **unmatched**, not loaded (for example alternates such as `02_gill alt1.png`, readme files).
3. **Movie.** An `.mp4` or `.mkv` file whose stem equals a shipped `.bk2` member's stem is a movie replacement, kept as `movie/<name>.mp4|.mkv` whatever its folder (H.264 / HEVC, or any other codec the bundled decoder opens). The movie player looks for `movie/<name>_<lang>` then `movie/<name>`, trying a loose `.mp4`, a loose `.mkv`, then the archive's `.bk2`, so a loose file wins over the shipped Bink movie and over a `.bk2` in a mod's `movie.dat`. Rule 3 does not apply inside a `movie.dat`: an `.mp4` there is unmatched.
4. A mod can only *replace* existing files; it cannot add new ones.

The startup log lists per mod `mods: '<name>' (enabled, load on modded): N matched, D game data, A archive(s), T texture(s), V voice(s), U unmatched, B ambiguous`, followed by up to 12 unmatched / ambiguous paths each (a `.dat` member as `<rel>/<member>`); `mods: K active mod(s), F file(s), T texture(s)` is logged each time the overlay is built.

## 3. Load on, priority, overlay

**Game data.** A member under `lang/`, `cdrom/battle/data/`, `cdrom/battle/event/`, `cdrom/battle/mes/` or `cdrom/prog/`, one of `cdrom/menu/el_shop.bin`, `waa_shop.bin`, `trade.bin`, `waa_tbl.bin` and `keyitem.dat`, or the base sound banks `cdrom/sound/effect/common/effect00.dat` and `cdrom/sound/wave/wave0000.dat`, is **game data** (`vfs::is_game_data`, `Mod::data`). Everything else follows *Load on* and swaps live: art and the rest of the sound, which takes effect the next time it is read, so a song already playing keeps playing. That includes room files: their scripts and art share one file, and `cdrom2` holds art versions of them. Game data ignores *Load on*: it is active in every art mode. Much of it is read once at boot, so the overlay keeps what the first `ModManager::apply` installed: the enabled mods and order at launch, until the next launch. A switch never changes it, which keeps tables, menus and scripts from different mods apart. The Mods tab shows *Won't change until restart* under a mod with game data, and a mod with neither art member files nor textures shows `All`, fixed, as its Load on. Priority and conflicts work as for art, in any art mode.

* Each mod has **Load on** = `Modded` (default) | `SD` | `HD` | `All` (ini: `modded|sd|hd|all`; `original` reads as `sd`, anything unknown as `modded`): the art modes in which its files are active. Art mode `Modded` = HD art (cdrom2 override on, RDE's HD assets) plus OpenCross's own improvements (`Vfs::enhanced()`) plus the mods set to Modded or All. `HD` and `SD` (original art) load only the mods set to that mode or All, so with the defaults HD and SD stay stock.
* **Priority** = list order (top first). For each target the first enabled, active mod in the list wins (loose files and `.dat` members alike). The Mods tab shows per mod the conflicts with other enabled mods that share an art mode, and which mod wins each.
* Lookup order of `Vfs::read` / `read_disc` / `exists`: overlay file (loose, or a member of a mod's `.dat`) -> (cdrom: cdrom2 when the HD rule says so, `docs/formats/rde_archives.md`) -> shipped archive. `list` always lists the shipped archive.
* Overlay changes are a pure art swap. `apply_art_mode` rebuilds the overlay; when its content changed (HD <-> Modded, a Mods tab edit), field room art / models and battle unit models are rebuilt even if HD stays HD (`set_art_mode(hd, force)`), and the path-keyed texture caches of the dialogue portraits, main menu, shops and party screen use `Vfs::cache_key` (path + overlay generation), so the next draw loads the mod file. A movie already playing keeps its file. A mod's loose movie (`movie/<name>.mp4|.mkv`) is not read into memory: `Vfs::loose_file(path)` returns the overlay's file on disk and the movie decoder streams it through a file-backed reader; archive members, those of a packed mod or a mod's `.dat` included, are read whole (`docs/formats/movies.md`).

## 4. Settings (`opencross.ini`)

```
; comments are whole lines only (; or #); the reader does not strip trailing comments
[Video]
; ArtMode: hd | original | modded
ArtMode=modded

[Mods]
; one line per mod, in priority order (top = wins): <folder or file name>=<enabled 1|0>,<modded|sd|hd|all>
Chrono Revision=1,modded
HD Boat=1,all
Zeightgeist HD Backgrounds=0,modded
```

Mods missing from `[Mods]` are appended in name order with `1,modded`; entries whose mod is gone are dropped on the next save. F6 cycles SD -> HD -> Modded; the Esc overlay's art-mode setting has the same three values; the route command `art modded` selects it. The `Dump*` category keys (section 7) share the `[Mods]` section with the mods; they are not mod names, and the obsolete `DumpTextures` / `ReplaceTextures` keys are ignored and dropped.

**Mods tab (Esc overlay).** Per mod: enable checkbox, up / down priority arrows, name (`(active)` when it loads in the current art mode), *Load on* combo, file / game data / archive / unmatched / ambiguous counts, a *Conflicts* list (target -> winning mod), and the unmatched / ambiguous paths. *Rescan* re-reads the `mods` folder (keeping settings by name). *Model Viewer* starts `opencross modelview` as a separate process (`docs/tools.md` 1.12): the models with and without the enabled mods. Every change is saved to the ini and re-applied like an art switch.

## 5. Voices (field layout + battle lines)

Voice lines are MP3 files in a mod folder's `voices` folder, played when a dialogue page is shown. The field layout is described in 5.1, so existing voice packs in that layout work unchanged. The battle layout (5.2) is OpenCross's own.

### 5.1 Field dialogue

```
<GameDir>\mods\<pack>\voices\<scene>\<dialog>-<page>-<character>.mp3        e.g.  voices\123\5-0-serge.mp3
<GameDir>\mods\voices\<scene>\<dialog>-<page>-<character>.mp3                shared location (the folder `voices` is itself a "mod")
```

| field | meaning | OpenCross value |
|---|---|---|
| `scene` | the **lang.dat room id** (row index of `tools/py/data/text_room_ids.csv`, 0..565; the field map id, +1000 in the `fin_` variants) | `FieldWorld::room_id_` |
| `dialog` | the **message index inside the room** (the E0 / E3 / E4 word operand = message block record index) | the `message` argument of `FieldWorld::open_dialogue` |
| `page` | 0-based `{bpage}` page; a line is requested when a page is shown (page 0 when the window opens) | `DialogueBox::page_index()` |
| `character` | lowercase name of the **speaker character id** (the id the script gives the window; E3 / E4 = the speaking entity's character). Names come from a 83-entry table: 0 `serge`, 1 `kid`, 2 `guile` ... 19 `glenn` ... 43-48 `pip`, 49 `lynx` ... 82 `orcha`; speaker 0 / none is named `npc` | `ents_[speaker].char_id` of the E3 / E4 entity (`voice_char_name`) |

Reading chosen so that existing packs work unchanged:

* Speaker id 0 is `npc` in existing packs, yet their examples name `serge`. OpenCross tries **in order**: `npc` for speaker id 0 or none, else the table name; then `serge` for id 0; then the character named by the line's own `{NAME%}` tag. The first existing file wins.
* A speaker id past the table (>= 83) is written as the number.
* **E0 lines (no speaker) use the previous speaker.** After the names above, OpenCross tries the **previous speaker of the same room**: `VoicePlayer` keeps the last window speaker id > 0 per lang.dat room (`FieldVoiceNames`, reset when the room id changes; a room's first E0 line has none). Test: `test_voice`.
* **`fin_` rooms.** `scene` is the lang.dat room id, which for the ending rooms is 1000 + the map id (`fin_06` = 1459 ... `fin_14` = 1544; see the csv). `FieldWorld::room_id_` (`import::room_id_from_name`) already is that id for every `fin_*` room, so `voices\1459\...` resolves as is. `fin_19` is listed under two ids (501 = its slot in the disc-1 list, 1501 = the ending id); it always reads text and voices as scene 501 + 1000 = 1501, so its voice scene candidates are `1501` first, then `501` (`field_voice_scenes`).
* Existing packs cover only the base text (no per-speaker variants: EN / JP); OpenCross plays for every language.

### 5.2 Battle dialogue (OpenCross)

```
<GameDir>\mods\<pack>\voices\battle\<battle id>\<msg>-<page>-<unit>.mp3        e.g.  voices\battle\7\2-0-serge.mp3
```

* `battle id` = the **scene id** of the battle (`evt<id>.pac`; `BattleView::scene_id_`). RDE reads `btel_<lang>.csv` rows through a remap (`btel_battle_id` in `battle_view_script.cpp`, for example 4 -> 0x199); the voice name uses the **un-remapped** scene id, so a pack follows the battle, not the table row.
* `msg` = the message index of the script op E0 (`ScriptMsgReq::msg`, the key of the btel table). `page` = 0-based `{bpage}` page.
* `unit` = the name of the unit whose entity script issued the E0 op (`ScriptMsgReq::unit`; a scene-level issuer falls back to the first PC), lowercase, with spaces and the characters `\ / : * ? " < > |` removed (`Dead Ahead` -> `deadahead`). That entity is not necessarily the speaker (evt386's message 0 is issued by Serge's entity slot but is Radius' line; the PS1 battle window has no speaker), so the lookup tries the unit form first and then the speaker-less `<msg>-<page>.mp3` (`voices\battle\386\0-0.mp3`). Packs should use the speaker-less form unless the unit matters.
* Played when the window shows a page (page 0 when it opens), stopped when the window closes or a new line starts.

### 5.3 Lookup, formats, settings

* The player searches the **enabled mods in priority order** ("Load on" is ignored: voices are not art): for each, `<mod>\voices\<scene>\...` and, for a mod folder literally called `voices`, `<mod>\<scene>\...`; a packed mod's lines are found by name in its list and decoded from memory. The first existing file wins. Files below `voices` are not archive members: the Mods tab counts a pack's `voices\**\*.mp3` (and every `.mp3` of a mod folder called `voices`) as voice files (`Mod::voices`, shown as `N voice` in the Files column), not as "unmatched". Other files below `voices`, and `.mp3` files elsewhere (the player looks only at `<mod>\voices`), stay "unmatched".
* **MP3 only** (any bit rate / sample rate, mono or stereo; decoded to 48 kHz stereo) through Windows Media Foundation, on a worker thread, the whole line before it starts.
* A page plays when it is first shown; a new page or line stops the previous one; closing the window stops it. A missing file is logged once per path at debug level (`voice: not found voices/<path>`).
* `opencross.ini`: `[Audio] Voices=1` (default on) and `[Audio] VoiceVolume=100` (0..100, the voice bus gain; Master applies too). Both are rows of the Esc General tab, Audio section. The voice bus is described in `docs/formats/audio.md`.

## 6. Textures: what can be replaced

Loose-file mods replace archive *members* (sections 1-2). Many textures are not members: they are decoded out of containers and VRAM images (a battle stage's atlases come from `battle/bg/bg_NN.prd` + `.ctd` by colour mode and CLUT row; the title backdrop from `title.ltd`; effect sprite sheets from the effect packs). `src/render/texture_registry.*` names each of these with a **canonical path** and offers two hooks that every upload site calls: `dump(key, image)` and `replace(key, image)`.

Replacement is always on (a texture mod is switched by its own row in the list) and nothing is dumped while playing. An *active* mod (enabled, Load on matches the art mode, priority = list order, like member files) that contains `textures\<canonical path>` has that PNG decoded (stb_image, any size) and used instead of the original. The Mods tab shows "Textures this session: N dumped, M replaced" and the dump button (section 7).

### 6.1 Folder layout (canonical paths; lower case, `/`)

Two naming rules. **Path-named**: the texture is cut from one member file, so its member / group id is in the path. **Content-named**: the texture is cut from a per-scene VRAM page and repeats across scenes, so it is named by an 8-hex-digit FNV-1a tag (`hash8`) of its size + RGBA pixels: identical images are ONE file (bulk dumps stay small) and a replacement applies everywhere the image is used.

| Category (first folder) | Source | Path | Naming |
|---|---|---|---|
| `battle_stage` | the tile library of `battle/bg/bg_NN.prd` + `.ctd` (section 6.2): one PNG per distinct texel rect the stage's faces sample | `battle_stage/bg_NN/tile_<NN>_<hash8>.png` | path + content hash |
| `title` | the title scene (`title.prd` + `title.ltd`), same tile library | `title/tile_<NN>_<hash8>.png` | path + content hash |
| `tiles` | **global tile replacements** only (never dumped): `tiles/<hash8>.png` replaces that tile in EVERY stage / title / room mesh that uses it | `tiles/<hash8>.png` | content |
| `effects` | effect sprite sheets (frames stacked vertically in one atlas, width = widest sprite): battle packs, the particle library, battle.prd's resident packs and every room's `.prd` sheets | `effects/<sheet name>/<tag>.png` | content (per sheet name) |
| `effect_mesh` | the battle effect packs' mesh atlases | `effect_mesh/<tag>.png` | content |
| `room_mesh` | a room's `.prd` mesh effects: the same tile library, per room; identical tiles are ONE file across rooms | `room_mesh/tile_<hash8>.png` | content |
| `room_model` | room models and the title scene's models, as tiles (`src/render/model_tiles.h`): per VRAM page window and CLUT row, the texel rects the model's faces sample (overlapping rects merged), cut through that CLUT. A room model whose own TIM is a field character's RDE texture (375 room models: party members in scenes; matched by the colours of the sampled texels: luminance correlation >= 0.95 and mean RGB difference <= 40; scene-recoloured copies keep their own tiles) is not tiled; it draws the character's `model/fieldobj/player/<n>/<n>/clut0.png` replacement, or in the Modded art mode its hd.dat 4x texture | `room_model/tile_<hash8>.png` | content |
| `model` | battle player / monster models (`battle/player\|monster/<n>/<n>.obj` + `.ltd` / `.ltc`) and field character models (`fieldobj/player/<n>/<n>.obj` + `.tim`): one texture per CLUT row, then the overlay strips | `model/<member stem>/clut<row>.png`, `model/<member stem>/overlay<k>.png` (the battle players' eye strip: blink frames are cut from the replacement, any size) | path |
| `model` (status screen) | `menu/player/<n>.ome` in HD art: its only texture is the hd.dat TIM | `model/menu/player/<n>/clut0.png` | path |
| `model` (weapons) | battle weapons (`battle/wepon/<pack>.bin`, `monster/` included): one TIM per pack part (part = the gear's tier byte & 0xF for a PC, the item byte & 0x7F for a monster); PCs', monsters' and the status menu's weapons all load it | `model/battle/wepon/<pack>/part<n>.png` | path |

Notes:

* Models with an hd.dat 4x image dump THAT image as `clut0`, and a `clut0` replacement is drawn in every art mode its mod loads on, in place of the 4x image: field characters (`hd/fieldobj/player/<n>/<n>.tim`, 512 x 512) and status screen models (`hd/menu/player/<n>.tim`, 1024 x 1024, left half eyes open, right half shut; keep both halves). Battle players have no hd.dat image: they dump and draw their RDE battle texture (cdrom2 `.ltc` / `.ltd`, 128 x 256) in every art mode. The status-screen TIM's left half is that page at 4x for 41 of them, but it is deliberately NOT used in battle because the art looks off on the battle models.
* `model/...` and `room_model/...` files are plain cut-outs (alpha 0 / 255). The PS1 semi-transparency bit (STP) of each texel lives in the decoded texture's alpha in the game (`kStpAlpha` in `src/import/tim.h`) and is NOT in the file: a replacement's alpha is coverage only (< 128 = transparent), and each covered texel takes the STP of the original (`render::model_restore_stp`: the original's flag when all its texels share it, else the flag of the original texel under it). Room-model names still hash the decoded pixels, STP included.
* **Not covered:** whole `.tim` / `.png` / `.chip2` members (replace them by member path), room layers (composed per frame from `chip2` / PS1 tile sheets: replace the `.chip2`), the actor shadow blob (`systim.bin`), fonts and UI sheets, bespoke-overlay mesh atlases, the encounter-effect meshes, battle effect models cut from the pack VRAM, and the status-menu model in the original art mode (HD art is covered).
* `model/...` is dumped from RDE's HD file set (cdrom2 first), the art the game shows by default, so a path never holds two versions. The room dump imports every room's `.bin` / `.ctd` / `.prd` headlessly (`import::load_room`) with the HD file set only (cdrom2 holds every room; its `.ctd` pages and models differ from the disc's): it holds what the Modded art mode draws.

### 6.2 Tiles (battle stages, title, room meshes)

The PS1 scenes sample (colour mode, CLUT) atlases of a VRAM area: one stage is about 7 images that are all the squares of the scene glued together with the same art repeated, which is hopeless to repaint. The importer therefore cuts them into **tiles** (`render::bake_object_textures`, `src/render/stage_tiles.*`, pure CPU, run by the loaders and by the headless dump), and the tile is what you dump and replace:

1. **Collect.** Every face's texel rect (its corners' u / v bounds, in the atlas of its colour mode + CLUT) is collected stage-wide. Rects that **overlap** are merged; rects that merely touch are NOT (neighbouring squares stay separate tiles). A part drawn by a UV-scroll emitter (op 36) contributes its **texture window** (off_u, off_v, win_w, win_h of the op; the GPU wraps every texel of the mesh inside it, so it is the only art the mesh can show, e.g. bg_00's sea shimmer `nea7`); a window of 0 means the whole 256 x 256 page. The window's tile is the image the scroll slides over.
2. **Cut.** Each rect is cut from its atlas **already decoded through its own CLUT**: a plain RGBA PNG, no palette, colour 0 = alpha 0.
3. **De-duplicate.** Tiles whose decoded pixels are *exactly* equal (size + RGBA incl. alpha, FNV-1a `hash8`) are ONE tile. Only exact duplicates merge: the same art through another CLUT, or shifted / cropped by one pixel, is a different tile.
4. **Name.** `tile_<NN>_<hash8>.png`: NN = first-use order in the stage (objects in drp order) for browsing, `hash8` = the content hash, so an identical tile has the same hash in every stage that uses it. No manifest is written. Only meshes the stage's emitters actually spawn contribute tiles. Room meshes use `room_mesh/tile_<hash8>.png` only.
5. **Draw.** Every object (part) still samples ONE texture: its tiles shelf-packed (power-of-two width) with a 1-texel edge-extended gutter, UVs remapped (`StageSubMesh::object_texture`), one descriptor set per object. A vertex used from two atlases is duplicated at the end of the part's vertex list. A morph (op-37 pair) is drawn only when base and target have the same vertex count, so a duplicate appended to one side would silently turn the morph off; in the shipped data the bake duplicates no vertex in any part of any stage or the title (no face shares a vertex between two atlases), and `test_texture_registry` fails if a data or bake change breaks that. The raw atlases stay for the effect polygons.
6. **Transparency is unchanged.** Colour 0 stays alpha 0 (cut out; the bilinear bleed runs on the object image), everything else is opaque; semi-transparency stays per packet / sub-mesh (`StageSubMesh::semi` + the tpage ABR).

**Replacement** (at load, `apply_tile_textures`, any size). The first match wins in this order: `mods/<mod>/textures/battle_stage/bg_05/tile_*_<hash8>.png` (matched by the hash only; the order number may differ or be dropped, and the file may be renamed freely as long as it ends with `<hash8>.png` after the `tile_` prefix), then the global `mods/<mod>/textures/tiles/<hash8>.png`, which replaces the tile in every stage / title / room mesh. An object that contains a replaced tile is **re-composed at the largest scale among its tiles** (replacement size / tile size; un-replaced tiles count 1x and are enlarged nearest-neighbour), and UVs follow (normalised; `uv_scale` = texel size at the new scale). Nothing replaced = the original look. A re-composed part also gets seam strips (`StagePart::seams`, `render::build_tile_seams`): tiles upscaled one by one disagree along shared mesh edges, so with `[Enhancements] EdgeBlend` the stage pipeline cross-fades the two faces along each such edge on screen.

**Threaded loading.** A battle stage's decode, tile bake, tile replacements (PNG decode, several tiles at once through `parallel_for`), transparent-texel bleed and texture upload run on a worker thread (`StageScene::prepare`, `prepare_stage_textures`); the staged battle load reports `Wait` until it finishes, so a large texture mod does not stall frames. Uploads off the render thread go through one persistent 32 MiB staging buffer (`GpuContext::band_staging`).

**Overlay cells and effect polygons** (`effects::FxGroundPrim` / `StagePrim`, drawn by `StagePipeline::draw_prims`) address the raw atlases with PS1 packet texels. Each corner texel is located through `StageAsset::tile_map` (`render::tile_lookup`: atlas texel -> merged rect -> tile -> the first object texture holding it -> composed texel at its current scale) and the packet is drawn from that object texture, so a spell's cracks and patches show the replaced tiles exactly like the normal ground. A packet whose corners do not lie in one rect that stage faces sample keeps the raw atlas (logged once per power of two: "effect polygon(s) sample texels no stage face uses").

Names in code: `render::tile_path` / `dump_tiles` (stage tiles), `render::stage_group_key` (un-baked stage-asset atlases: the effect meshes), `render::raw_atlas_key` (`--raw`), `render::model_texture_key` (`ModelAsset::source` = the member stem, or `@room_model`), `effects::effect_sheet_key`.

### 6.3 Replacement rules

* The PNG may have **any size**. Tiles, effect-mesh atlases and effect sheets are sampled with normalised UVs, so a 2x / 4x upscale of the dumped file lines up with no other change (keep the aspect ratio and layout; for effect sheets the frames are stacked, so scale the whole sheet uniformly). Different proportions are simply stretched.
* Transparent texels (alpha 0) get the colour of their opaque neighbours at upload (the bilinear-fringe bleed that the originals get).
* The mod file is found by its path below a `textures` folder whose next component is a category (`battle_stage`, `title`, `tiles`, `effects`, `effect_mesh`, `room_mesh`, `room_model`, `model`); any wrapper folder above `textures` is ignored, the match is case-insensitive. Other files below a `textures` folder are resolved as archive members. Textures count in the Mods tab's *Files* column (`N + M texture`, voice files `+ V voice`) and take part in conflicts (the first enabled mod wins a path, like members).

### 6.4 Live rebuild

`apply_art_mode` rebuilds the overlay (a Mods tab toggle, priority or Load-on edit, or an art-mode switch that changes the overlay, changes `Vfs::generation()`), and field room art / models and battle unit models follow immediately (`set_art_mode(hd, force)`). Models (`model`, `room_model`) follow that forced rebuild (`MeshPipeline::create_model` consults the registry).

**Tiles follow the same trigger without a reload.** The scenes that draw baked tiles (the running battle's `StageScene`, the `TitleBackdrop`, the field's `RoomMeshes`) poll `texture_registry::generation()` once per frame. When it changed they re-run `apply_tile_textures`, which recomposes every object image from the **pristine tiles** (`StageObjectTexture::base_slots` / `base_uv`; never from the previously composed image, so nothing accumulates and switching a mod off restores the original image, slots and UVs bit for bit), then re-uploads (`StagePipeline::reload_textures`, or `RoomMeshes::refresh_tiles`). The old GPU textures, sets and buffer are retired by idling the device first: a one-frame hitch at the toggle, no scene reload.

## 7. Dumping textures

**Dump all textures** (button in the Mods tab, the main way) runs the full headless dump (`src/game/battle/texture_dump.*`, the same code as `occ dump-textures all`) on a **worker thread** into `<GameDir>\dump\textures\`. It never touches Vulkan (only decoded images), runs at below-normal priority, and reads the shipped archives through its own VFS (no mods applied, so it always dumps originals). The tab shows a Cancel button, a progress bar and "<phase> n / total, F files" (phases: battle stages, title backdrop, effect packs, rooms, models, weapons); a one-line summary (files per phase, skipped items, seconds) is shown when it ends and written to the log ("texture dump done: ...").

**Category checkboxes** next to the button (`[Mods] DumpStages` / `DumpTitle` / `DumpRooms` / `DumpModels` / `DumpWeapons` / `DumpEffects`; defaults 1 / 1 / 1 / 1 / 1 / **0**): *Battle stages* (3D battle scenes), *Title* (the title scene), *Rooms (3D meshes + models)* (`room_mesh`, `room_model`), *Models* (battle / field unit models, `model`), *Weapons* (every part of every `battle/wepon/*.bin` pack), *Effects (sheets + meshes)* (`effects`, `effect_mesh`; large, hence off by default). The button writes only the checked ones (disabled when none is). Changing a box saves the ini but rebuilds nothing. Room effect sheets belong to Effects, so with only *Effects* checked the room pass still loads every room but writes just the sheets.

**Dev tool (no game):**

```
opencross occ [-g <GameDir>] dump-textures [stages|title|effects|rooms|models|weapons|all] [--raw] <outdir>
```

The same job, blocking, into `<outdir>`; the names are the checkboxes (none named = all); it prints progress every 5 s, then per-phase file counts and the time. `--raw` additionally writes the raw (mode, CLUT) atlases of stages / title / room meshes under `raw/...` (debugging; `raw` is not a replaceable category).

**Modder's workflow.** Tick *Battle stages* / *Title* / *Rooms* and press **Dump all textures**. Open `dump\textures\battle_stage\bg_05\`, find the tile by eye, paint it in any editor (alpha 0 = see-through; keep the layout; any scale factor, same aspect) and save it as `mods\<mod>\textures\battle_stage\bg_05\tile_<NN>_<hash8>.png` (or `textures\tiles\<hash8>.png` to change it everywhere). Enable the mod (Load on Modded / All) and toggle it in the Mods tab: a running battle, the title or the room picks it up at once (6.4). Copy a dumped tree, or part of it, into `mods\<mod name>\textures\...`.
