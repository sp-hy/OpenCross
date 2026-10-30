# RDE launcher and the OpenCross button

`CHRONOCROSS_LAUNCHER.exe` is a 32-bit .NET Framework 4 WinForms program; `CHRONOCROSS_LAUNCHER.dll` is its native helper (Steam, music, pad, resolution list). The proxy in `launcher/` adds an OpenCross button without changing any game file. It is the only 32-bit code in the project and has no link to the engine.

## 1. Launcher files

Paths are relative to the game folder, which the launcher assumes is its working directory. Every file is looked up loose under `data\launcher\` first, then as an entry of the ZIP `data\launcher.dat` (entry names drop the `data\launcher\` prefix).

| File | Content |
|---|---|
| `Resources\Common\ui.csv` | The menu table, section 2 |
| `Resources\<lang>\Image\BASE0.png` | The 1280 x 720 background, 24-bit RGB. `<lang>` is `EN FR IT DE ES JP` |
| `Resources\<lang>\Image\GAMEnn_MOUSEOFF.png`, `_MOUSEON.png` | One button, 280 x 54, 32-bit RGBA, text baked in |
| `Resources\<lang>\Image\GAME02_CANTPLAY.png` | Radical Dreamers while locked |
| `Resources\<lang>\languagelist.csv` | Settings window text |
| `Resources\Common\Sound\*.wav` | Menu music and sound effects |

## 2. `ui.csv`

One picture per line, CRLF, ending in a line break. Columns, in order:

| Column | Meaning |
|---|---|
| 0 | Picture name, and the file's stem. The first 6 characters tie a button's `_MOUSEON` and `_MOUSEOFF` pictures together |
| 1, 2, 3, 4 | x, y, width, height in the 1280 x 720 picture |
| 5 to 8 | Unused, 0 |
| 9 | Action: `GAME` (run column 10 and wait), `OPTION`, `CREDIT`, `EXIT`, `NON` (nothing), `BASE` (the background) |
| 10 | The program for `GAME`, relative to the working directory |

Rows are drawn first on top, and menu focus (Up / Down, Tab, the pad) follows the order of the `_MOUSEON` rows, starting on the first. A picture whose PNG has alpha is composited over the matching rectangle of `BASE0.png` when it loads; a picture without alpha is taken as the background and receives the music title. The table and pictures are read again whenever the launcher returns from a game or the credits, and the number of rows must not change in between.

`BASE0.png` has each button's soft drop shadow baked in at its original place: 5 rows up and left of the button, 4 rows down and right. The bottom button stands on plain white, so there the picture is the shadow.

## 3. The proxy

`XINPUT9_1_0.dll` sits beside the launcher. The helper and both game exes import that name, and it is not a system-protected DLL, so the copy in the game folder is the one loaded. It forwards the four XInput exports to the system DLL and does nothing else unless the process has `CHRONOCROSS_LAUNCHER.dll` loaded. There:

1. While the helper is loading, the proxy redirects the helper's `_Initialize@4` export to itself. Loader rules forbid running managed code at that point, so this only edits the export table.
2. The launcher's first call to that export comes from its window constructor, on the UI thread, before the window loads. The proxy runs its embedded managed assembly in the launcher's default domain, then calls the real function.
3. The assembly (`launcher/LauncherPatch.cs`) redirects the launcher's one loose-file reader to its own. Every read of the menu's files then passes through it, and the launcher's own code does the rest: drawing, focus, clicks, and starting the program.

The patch serves three things and passes everything else through:

- `ui.csv`: a new button `GAME00` as the first row of each state, action `GAME`, program `opencross.exe`. The six buttons start at y 272, 58 apart (4 pixels between them); originally five started at y 288, 62 apart.
- `GAME00_MOUSEOFF.png` / `_MOUSEON.png`: the Exit plate with its text painted out, and OPENCROSS assembled from the letters of the English Chrono Cross and Credits buttons (no button has a P, so it is built from R's stem and the top of its bowl, mirrored to close a taller bowl), redrawn with a shadow. The art is the same in every language.
- `BASE0.png`: the shadow mask read from the bottom button's white strip removes the five baked shadows (divide) and draws six (multiply).

Under Proton the launcher runs on Wine Mono, and Wine prefers its own `xinput9_1_0` unless the game's launch options say `WINEDLLOVERRIDES="xinput9_1_0=n,b" %command%`. Mono's `_Assembly` COM interface declares the `ModuleResolve` event last, where the CLR's declares it before `GetType_4`, so every method from there on is in a different slot, `CreateInstance` included. The patch is therefore an attribute on its own assembly, and the proxy starts it with `GetCustomAttributes`, which is in the same slot on both.

Nothing is written to disk. With `opencross.exe` missing from the game folder, or when the art cannot be built, the launcher stays as it shipped. Removing `XINPUT9_1_0.dll` undoes everything.

## 4. Build and delivery

`.\build.ps1 -Launcher` writes `build\launcher-x86\bin\XINPUT9_1_0.dll`: the x86 MSVC toolset for the native part, the .NET Framework's own `csc.exe` for the managed part, which is embedded as a resource, so one file ships. Each release carries it as an asset beside `opencross.exe`. The updater installs it with the exe, and offers it alone when the installed copy is missing or differs from the release's digest; see `docs/architecture.md` section 10. It is only placed where `CHRONOCROSS_LAUNCHER.exe` is beside `opencross.exe`.
