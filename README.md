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
- **Steam**: turn on **Steam Connection** in the General settings to show as playing the Radical Dreamers Edition and unlock its achievements, using the Steam client you already have. It's off by default, and Steam isn't required to play.
- **Launcher button**: an optional `XINPUT9_1_0.dll` adds an OpenCross button to the Radical Dreamers Edition launcher, so you can start from Steam's Play button. It changes no game files.

## Requirements

- **OS**: Windows 10 or 11, 64-bit.
- **GPU**: a graphics card with Vulkan or Direct3D 12 support. OpenCross picks one automatically, but can be changed in the settings.
- **Game**: Chrono Cross: The Radical Dreamers Edition on Steam, installed.
- **Controller** (optional): any controller SDL supports, such as Xbox, PlayStation or Switch pads, including rumble.

## Installing

1. Download `opencross.exe` from the [releases page](https://github.com/sp-hy/OpenCross/releases).
2. Put it in your Radical Dreamers Edition folder, for example `C:\Program Files (x86)\Steam\steamapps\common\CHRONO CROSS  THE RADICAL DREAMERS EDITION`.
3. Run it.

To start OpenCross from the game's own launcher, also put `XINPUT9_1_0.dll` from the same release page in that folder, or use **Update** in the settings, which installs it for you. To remove the button, delete the file.

OpenCross finds your Steam saves on its own and writes its settings to `opencross.ini` next to the executable.

## Mods

Mods live in a `mods` folder inside your Radical Dreamers Edition folder, one sub-folder per mod:

```
CHRONO CROSS  THE RADICAL DREAMERS EDITION\
  mods\
    My Texture Pack\
    My Voice Pack\
```

When the game starts (or when you press **Rescan** in the Mods tab), OpenCross looks through every file in each mod folder, including all sub-folders, and works out what each one replaces:

- **Whole archives**: a file named after one of the game's `data` archives (for example `movie.dat`) replaces that archive. Like the originals, it must be a ZIP file.
- **Textures**: PNG images anywhere under a `textures` folder (for example `textures\battle_stage\...`) replace the game's textures, at any resolution. To find the right names, use **Dump all textures** in the Mods tab. It writes every texture to a `dump\textures` folder in the game folder, with the same names and layout a mod uses. To check a character or monster texture mod without playing to it, press **Model Viewer** in the Mods tab: it shows each model with and without your mods, with its animations.
- **Movies**: a `.bk2`, `.mp4` or `.mkv` named after one of the game's movies (for example `a01op_en.mp4` for `a01op_en.bk2`) replaces it; an `.mp4` / `.mkv` wins over any `.bk2`. Video: HEVC (8 or 10-bit), H.264 or Bink 2, at any constant frame rate (30 and 60 fps tested) up to 1440p. Audio: AAC, Opus or Bink audio (other audio codecs play silent). Tag the colour space (BT.709 or BT.601); untagged files are read as BT.709 from 720 lines up. Keep the original's length and timing, since some scenes play only part of a movie.
- **Voice lines**: MP3 files in a `voices` folder at the top of the mod (or a mod folder that is itself called `voices`) play over the dialogue. CrossFix voice packs work as they are.
- **Any other game file**: every other file is matched against the files inside the game's archives by name (upper or lower case doesn't matter). If only one game file has that name, it is replaced, wherever the mod file sits, so wrapper folders like `My Mod\files\...` don't get in the way. If several game files share the name, the one whose folder path matches the end of the mod file's path wins; if that still leaves more than one, the file is skipped as **ambiguous**. Files that match nothing are skipped as **unmatched**. The Mods tab shows both counts.

Mods are managed in the **Mods** tab of the settings window (Esc). There you can turn each mod on or off and change the order. When two mods replace the same file, the one higher in the list wins, and the tab shows these conflicts. Each mod also has a **Load on** setting that picks which art style it applies to: Modded (the default, HD art with mods), SD (the original art), HD, or All. Changes take effect straight away.

## Building from source

OpenCross builds on Windows with Visual Studio 2026 (C++ tools), CMake, Ninja and vcpkg. From PowerShell:

```
.\build.ps1 release
```

The executable is written to `build\win-x64\bin\Release\opencross.exe`. Use `.\build.ps1 debug` for a debug build and add `-Test` to run the unit tests.

## Credits and licence

OpenCross is licensed under the GPL-3.0-or-later (see [LICENSE](LICENSE)). The libraries it uses and the community references that made it possible are listed in [CREDITS.md](CREDITS.md).

Chrono Cross © 1999 Square (Square Enix). The Radical Dreamers Edition © 2022 Square Enix / D4 Enterprise.
