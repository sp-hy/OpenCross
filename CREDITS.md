# Credits

This file lists (A) the third-party libraries and build tools OpenCross uses, (B) the references used for file formats, game data and hardware behaviour, and (C) the playtesters.

## A. Libraries and build tools

| Library | Author | URL | Licence | Use |
|---|---|---|---|---|
| SDL3 | Sam Lantinga et al. | https://github.com/libsdl-org/SDL | zlib | window, input, audio device, SDL_GPU renderer |
| Dear ImGui | Omar Cornut | https://github.com/ocornut/imgui | MIT | in-game options overlay and debug panels (SDL3 + SDL_GPU backends) |
| miniz | Rich Geldreich | https://github.com/richgel999/miniz | MIT | ZIP members of the `.dat` archives and `save.dat`, updater packages |
| libarchive | Tim Kientzle et al. | https://github.com/libarchive/libarchive | BSD-2-Clause | reading 7z and RAR mods |
| stb_image / stb_image_write | Sean Barrett | https://github.com/nothings/stb | public domain / MIT | PNG / JPG texture loading, screenshot and texture dumps |
| clHCA (vgmstream) | bnnm, kode54 et al. (original decoder: nyaga; VGAudio: Thealexbarney) | https://github.com/vgmstream/vgmstream (`src/coding/libs/clhca.c`, `clhca.h`, `clhca_data.h`; vendored unmodified in `third_party/clhca/`, licence text `COPYING.vgmstream`) | ISC | HCA decoding of the remastered BGM (`.sab`) |
| librempeg | Paul B Mahol (Bink 2 decoder: Kostya Shishkov, Paul B Mahol) | https://github.com/librempeg/librempeg | GPL-3.0-or-later | Bink 2 movie playback: commit 25d1ead99068ffb1f11df19cf0707854603d621e, built static by `scripts/build-librempeg.ps1` into `third_party/librempeg` (bink demuxer + bink2 / binkaudio decoders only) |
| shaderc (glslc) | Google | https://github.com/google/shaderc | Apache-2.0 | build tool: GLSL to SPIR-V shader compilation |
| SDL_shadercross | SDL project contributors | https://github.com/libsdl-org/SDL_shadercross | zlib | build tool: SPIR-V to DXIL / MSL shader conversion |

## B. References

| Reference | Author | URL | Use |
|---|---|---|---|
| roomviewer-rde (after Magil's Room Viewer) | stoofin, Magil | https://github.com/stoofin/roomviewer-rde | room `.bin/.ctd` section container, palettes, TIM textures |
| cc_fd | Orlha | https://github.com/Orlha/cc_fd | field-script header/entity table, operand conventions |
| chrono-cross (battlefield viewer) | tasior | https://github.com/tasior/chrono-cross | DRP container, battle mesh header and face-packet layout |
| innate-randomizer / heckrandomizer | zhadn | https://github.com/zhadn/innate-randomizer | innate element encoding, player folder → character map |
| game-tools-collection (Chrono Cross save template) | RyudoSynbios | https://github.com/RyudoSynbios/game-tools-collection | save block layout, checksum, bit-packed sections |
| Chrono Cross Model Viewer | UTunnels | https://storage.googleapis.com/utunnels1.appspot.com/ccmodelviewer/index.html | model/animation facts (bones, 12-bit angles, 2-bone skinning) |
| Chrono Compendium — Drp, Cpt, Tbt, Lzss, Rgfx, Egfx, Eins, Mseq | Purple Cat (Alfador), utunnels, wiki contributors | https://www.chronocompendium.com/Term/Modification.html | containers, LZSS, room subfiles, background and effect data |
| Chrono Compendium — Mesh, Mdl, Anim, Atim, TIM Textures | utunnels, Magil, FaustWolf, ZeaLitY, Grobycftw | https://www.chronocompendium.com/Term/Mesh.html | mesh packets, character model, skeleton, animation, battle textures |
| Chrono Compendium — Fieldscript Notes / Research, Script.cpt | yaz0r, ZeaLitY, Grobycftw, wiki contributors | https://www.chronocompendium.com/Term/Chrono_Cross_Fieldscript_Notes.html | field-script opcodes, variable encoding, dialogue control codes |
| Chrono Compendium — Battlescript, Enemy Stats, Game Mechanics | F-man, saiferoth, Magil, glennxserge, utunnels | https://www.chronocompendium.com/Term/Chrono_Cross_Enemy_Stats.html | battle script ops, enemy records, damage / hit / growth formulas |
| vgmstream (sqex_sead) | bnnm et al. | https://github.com/vgmstream/vgmstream | SEAD `.sab` container, HCA key table |
| FF7 / FF9 AKAO wiki pages | ff7-mods, ffrtt | https://ff7-mods.github.io/ff7-flat-wiki/FF7/PSX/Sound/AKAO_sequence.html , https://wiki.ffrtt.ru/index.php/FF9/Sound/AKAO_sequence | AKAO sequence opcode families |
| psx-spx | nocash, N. Noble (ed.) | https://psx-spx.consoledev.net | PS1 GPU / GTE / SPU behaviour (packets, CLUTs, ADPCM, Gaussian interpolation, noise) |

## C. Playtesters

| Name | Where |
|---|---|
| BathoryDormin | Nexus Mods |

Chrono Cross © 1999 Square (Square Enix). The Radical Dreamers Edition © 2022 Square Enix / D4 Enterprise. OpenCross is a fan project; it requires a legitimately owned copy of the Radical Dreamers Edition.
