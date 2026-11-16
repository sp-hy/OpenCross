<img width="1869" height="842" alt="image" src="https://github.com/user-attachments/assets/13d34927-a389-4678-a28f-09a58754792c" />

# OpenCross

OpenCross is a native PC engine for Chrono Cross, built in C++20 on SDL3 and its GPU API (Vulkan or Direct3D 12). Drop it into your **Chrono Cross: The Radical Dreamers Edition** folder on Steam and it plays the game from your own files and saves, started from the game's own launcher or on its own.

OpenCross is a fan project, not affiliated with Square Enix, and ships no game data. A legitimately owned copy of the Radical Dreamers Edition is required.

## Status

OpenCross is **in development**. Exploring the world, talking to characters, the story scripts, battles, menus, music and saving all work. Some features are still being polished, and there may be some bugs. If you find one, please [raise an issue](https://github.com/sp-hy/OpenCross/issues).

## Features

- **Both art styles**: switch between the remastered HD art and the original PlayStation look at any time.
- **Widescreen**: the field, world map, battles and menus fill wide and ultrawide screens; movies stay in 4:3.
- **High frame rates**: supports rendering at any frame rate.
- **HD models in fields**: the characters walk the field with their detailed Status-screen and battle models.
- **Retro look**: optionally draw the game at the PlayStation's resolution, with its wobbly vertices, or keep the original field models.
- **Shaders**: optional character styles (realistic, toon, cel, matcap), outlines, rim light and a cast glow; full-screen CRT and composite-video filters and colour grades; ambient occlusion and depth of field in battles. Mods can add matcaps and colour-grade LUTs.
- **Higher-quality options**: render scaling, smoother effects and reflections, lighting and shadow improvements.
- **Remastered or original music**: play the Radical Dreamers Edition soundtrack or the original PlayStation one.
- **Speed modes**: the Radical Dreamers Edition's fast and slow modes (F2 / F1, or the triggers), with an adjustable speed.
- **Quality of life**: movement that follows the camera, the 2021 or 1999 character portraits, all six languages, and settings you can navigate fully with a controller.
- **Mods**: replace any of the game's original files, plus textures and voice lines, from the game's `mods` folder, with a priority order and conflict view. Mods can also add new rooms and scripts and turn on engine features.
- **Optional extras**: a Swap command in battle that trades the acting character with a reserve member, and party screen tweaks, from the Mods tab's **Engine features** page. Off by default.
- **Cheats**: the Radical Dreamers Edition boosts plus extras such as no random encounters or free element use; all off by default.
- **Updates**: check for and install new releases from the in-game settings.
- **Steam**: Steam play and achievements.
- **OpenCross achievements**: 173 achievements of its own for story, places, bosses, recruits, elements and battles, listed in the **Achievements** tab. They only watch the game and never change it.
- **Multiplayer** (optional, off by default): see other players' parties in your room, chat with them, and challenge them to battles between your parties, on the public server or your own.
- **Game guide** (optional): read a GameFAQs guide in an overlay over the game (`` ` `` or LB + RB), kept on your PC after the first download.
- **Launcher button**: launch OpenCross from the official game launcher.

## Requirements

- **OS**: Windows 10 or 11, 64-bit, or Linux and Steam Deck through Proton.
- **GPU**: a graphics card with Vulkan or Direct3D 12 support. OpenCross picks one automatically, but can be changed in the settings.
- **Game**: Chrono Cross: The Radical Dreamers Edition on Steam, installed.
- **Controller** (optional): any controller SDL supports, such as Xbox, PlayStation or Switch pads, including rumble.

## Installing

1. Download `opencross.exe` and `XINPUT9_1_0.dll` from the [releases page](https://github.com/sp-hy/OpenCross/releases).
2. Put both in your Radical Dreamers Edition folder, for example `C:\Program Files (x86)\Steam\steamapps\common\CHRONO CROSS  THE RADICAL DREAMERS EDITION`.
3. Start the game from Steam and press the **OpenCross** button in its launcher, or run `opencross.exe` directly.

`XINPUT9_1_0.dll` only adds the launcher button and changes no game file. It is optional: without it, run `opencross.exe` yourself. Delete it to remove the button. **Update** in the settings installs new releases of both files.

On Steam Deck or Linux, Proton ignores the DLL unless you add this to the game's **Launch Options** in Steam:

```
WINEDLLOVERRIDES="xinput9_1_0=n,b" %command%
```

OpenCross finds your Steam saves on its own and writes its settings to `opencross.ini` next to the executable. Its own achievements are kept in `achievements.txt` beside it.

## Mods

Put mods in a `mods` folder inside your Radical Dreamers Edition folder. A mod can be a folder, a `.zip`, `.7z` or `.rar` file, or a single `.dat` file:

```
CHRONO CROSS  THE RADICAL DREAMERS EDITION\
  mods\
    My Texture Pack\
    My Downloaded Mod.7z
    lang.dat
```

Archives work without unpacking, but **large mods should be extracted to a folder for better performance**. The Mods tab warns you about 7z and RAR mods that may be slow. Password-protected and multi-part archives must be extracted, and so must any `.dat` file inside an archive.

What a mod can replace:

- **Game archives**: a ZIP named after a game archive, such as `lang.dat`. It only needs the files it changes.
- **Textures**: PNG files under a `textures` folder, at any resolution. **Dump all textures** in the Mods tab shows the names to use, and **Model Viewer** previews character and monster textures.
- **Movies**: a `.bk2`, `.mp4` or `.mkv` named after a game movie, such as `a01op_en.mp4`. Use HEVC, H.264 or Bink 2 video up to 1440p, with AAC, Opus or Bink audio, and keep the original's length.
- **Voice lines**: MP3 files in a `voices` folder. CrossFix voice packs work as they are.
- **Any other game file**: matched by file name, wherever it sits in the mod. Files that match more than one game file are skipped and counted in the Mods tab.
- **New files**: a file that matches no game file is added when it sits below a folder named after a game archive, such as `cdrom\map\mapbin\newroom.bin`. Anything else that matches nothing (readme files, alternates) is skipped and counted.

Mods that change game data, such as stats, text or scripts, show **Won't change until restart** in the Mods tab: changes to them apply the next time the game starts.

Manage mods in the **Mods** tab of the settings window (Esc): turn them on or off, change the order, and press **Rescan** after adding one. When two mods replace the same file, the higher one wins. **Load on** picks which art style a mod applies to: Modded (default), SD, HD or All. Game data always applies.

A `mod.ini` at a mod's root names the mod and can turn on engine features such as Party Swap, or give new rooms their ids. The Mods tab's **Engine features** page shows what each mod turns on, and lets you turn on the extras that work without a mod.

## Building from source

OpenCross builds on Windows with Visual Studio 2026 (C++ tools), CMake, Ninja and [vcpkg](https://learn.microsoft.com/vcpkg/get_started/get-started), set up as its guide describes (`VCPKG_ROOT` set, or `vcpkg` on `PATH`; `-VcpkgRoot C:\path\to\vcpkg` also works). From PowerShell:

```
.\build.ps1 release
```

The executable is written to `build\win-x64\bin\Release\opencross.exe`. Use `.\build.ps1 debug` for a debug build and add `-Test` to run the unit tests. `.\build.ps1 -Launcher` builds the launcher button, `build\launcher-x86\bin\XINPUT9_1_0.dll`.

The multiplayer server is a Cloudflare Worker in `server/cloudflare` (see its README). To run your own locally: `cd server/cloudflare; npm install; npx wrangler dev`, then pick **Local / other** in the Multiplayer tab.

## Credits and licence

OpenCross is licensed under the GPL-3.0-or-later (see [LICENSE](LICENSE)). The libraries it uses and the community references that made it possible are listed in [CREDITS.md](CREDITS.md).

Chrono Cross © 1999 Square (Square Enix). The Radical Dreamers Edition © 2022 Square Enix / D4 Enterprise.
