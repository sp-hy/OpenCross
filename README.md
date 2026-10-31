<img width="1869" height="842" alt="image" src="https://github.com/user-attachments/assets/13d34927-a389-4678-a28f-09a58754792c" />

# OpenCross

OpenCross is a native PC engine for Chrono Cross, built in C++20 on SDL3 and its GPU API (Vulkan or Direct3D 12). Drop a single executable into your **Chrono Cross: The Radical Dreamers Edition** folder on Steam and it plays the game from your own files and saves.

OpenCross is a fan project, not affiliated with Square Enix, and ships no game data. A legitimately owned copy of the Radical Dreamers Edition is required.

## Status

OpenCross is **in development**. Exploring the world, talking to characters, the story scripts, battles, menus, music and saving all work. Some features are still being polished, and there may be some bugs. If you find one, please [raise an issue](https://github.com/sp-hy/OpenCross/issues).

## Features

- **Both art styles**: switch between the remastered HD art and the original PlayStation look at any time.
- **Widescreen**: the field, world map, battles and menus fill wide and ultrawide screens; movies stay in 4:3.
- **High frame rates**: supports rendering at any frame rate.
- **Retro look**: optionally draw the game at the PlayStation's resolution, with its wobbly vertices, and a CRT filter.
- **Higher-quality options**: render scaling, smoother effects and reflections, lighting and shadow improvements.
- **Remastered or original music**: play the Radical Dreamers Edition soundtrack or the original PlayStation one.
- **Mods**: replace any of the game's original files, plus textures and voice lines, from the game's `mods` folder, with a priority order and conflict view.
- **Cheats**: the Radical Dreamers Edition boosts plus extras such as no random encounters or free element use; all off by default.
- **Updates**: check for and install new releases from the in-game settings.
- **Steam**: Steam play and achievements.
- **Launcher button**: launch OpenCross from the official game launcher.

## Requirements

- **OS**: Windows 10 or 11, 64-bit, or Linux and Steam Deck through Proton.
- **GPU**: a graphics card with Vulkan or Direct3D 12 support. OpenCross picks one automatically, but can be changed in the settings.
- **Game**: Chrono Cross: The Radical Dreamers Edition on Steam, installed.
- **Controller** (optional): any controller SDL supports, such as Xbox, PlayStation or Switch pads, including rumble.

## Installing

1. Download `opencross.exe` from the [releases page](https://github.com/sp-hy/OpenCross/releases).
2. Put it in your Radical Dreamers Edition folder, for example `C:\Program Files (x86)\Steam\steamapps\common\CHRONO CROSS  THE RADICAL DREAMERS EDITION`.
3. Run it.

To start OpenCross from the game's own launcher, also put `XINPUT9_1_0.dll` from the same release page in that folder, or use **Update** in the settings, which installs it for you. To remove the button, delete the file.

On Steam Deck or Linux, Proton ignores that file unless you add this to the game's **Launch Options** in Steam:

```
WINEDLLOVERRIDES="xinput9_1_0=n,b" %command%
```

OpenCross finds your Steam saves on its own and writes its settings to `opencross.ini` next to the executable.

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
- **Any other game file**: matched by file name, wherever it sits in the mod. Files that match nothing, or more than one game file, are skipped and counted in the Mods tab.

Mods that change game data, such as stats, text or scripts, show **Won't change until restart** in the Mods tab: changes to them apply the next time the game starts.

Manage mods in the **Mods** tab of the settings window (Esc): turn them on or off, change the order, and press **Rescan** after adding one. When two mods replace the same file, the higher one wins. **Load on** picks which art style a mod applies to: Modded (default), SD, HD or All. Game data always applies.

## Building from source

OpenCross builds on Windows with Visual Studio 2026 (C++ tools), CMake, Ninja and vcpkg. From PowerShell:

```
.\build.ps1 release
```

The executable is written to `build\win-x64\bin\Release\opencross.exe`. Use `.\build.ps1 debug` for a debug build and add `-Test` to run the unit tests.

## Credits and licence

OpenCross is licensed under the GPL-3.0-or-later (see [LICENSE](LICENSE)). The libraries it uses and the community references that made it possible are listed in [CREDITS.md](CREDITS.md).

Chrono Cross © 1999 Square (Square Enix). The Radical Dreamers Edition © 2022 Square Enix / D4 Enterprise.
