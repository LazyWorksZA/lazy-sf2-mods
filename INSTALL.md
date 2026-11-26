# Install and uninstall

The Lazy SF2 Patch is a mod for Strike Fighters 2 (July 2013 patch). The project is not affiliated with ThirdWire, and you need your own copy of the game.

## Players: the installer

1. Run **`Lazy-SF2-Patch-<version>.msi`** from the Releases page. It's a normal installer wizard: a welcome page, the license (ours plus the [third-party notices](THIRD_PARTY_NOTICES.md)), an administrator prompt, and a finish page.
   - If the .NET 10 runtime is missing, the installer adds it.
2. The installer finds Strike Fighters 2 through the game's own registry entry and copies **one file, `winmm.dll`**, into the game folder. If a different `winmm.dll` is already there, it leaves it untouched.
   - If Tacview is installed, it also puts the mod's Tacview add-on in place for your Windows account and turns it on in Tacview (see [Tacview terrain](#tacview-terrain)). Close Tacview before installing.
3. That's all. There's no app, window or shortcut. Start the game as usual; the settings are in the game (below).

**Uninstall** the Lazy SF2 Patch from Windows' *Installed apps*. That removes exactly the files the mod added to the game folder, and the Tacview add-on. Tacview's on/off switch for the add-on stays, so a later install or upgrade keeps your choice.

## Settings, in the game

| Where | Setting | Values | Default |
|---|---|---|---|
| Options → Graphics (below Aspect Ratio) | **Display Mode** | **Fullscreen**: stock exclusive fullscreen. **Borderless**: a window covering the screen, so Alt-Tab is instant and other monitors stay usable. | Fullscreen |
| Options → Graphics | **Screen** | "Screen N (W×H)", numbered as in Windows' Settings → Display. Choosing another screen also sets the game resolution to its native size, which applies at the next start. | the stock choice (primary) |
| Options → Controls (below Keyboard Map) | **Mouse Pointer** | **Match Windows cursor**: the game's pointer sits exactly on the Windows cursor, at Windows' speed and acceleration. **Stock game pointer**: the game builds its own pointer from raw mouse movement, as without the mod. | Match Windows cursor |
| Options → Controls (below Mouse Pointer) | **Pilot Twist** | **On**: in the cockpit, looking far to the side or behind (hat or head tracker) turns the view up to 20 degrees further and leans you a little toward that side. **Off**: stock view. | Off |
| Options → Gameplay (bottom row) | **Tacview Recording** | **On**: every flight is recorded to a Tacview file (`SF2 <date>.zip.acmi`) in `Saved Games\ThirdWire\StrikeFighters2\Tacview`. **Off**: no recording. | Off |

Changes take effect when you leave the Options screen, like the game's own settings.

All five are stored in your own `Saved Games\ThirdWire\StrikeFighters2\Options.ini`: `DisplayMode` and `DisplayMonitor` under `[GraphicsOptions]`, `MouseMode` and `PilotTwist` under `[ControlOptions]`, and `TacviewRecording` under `[OtherOptions]`. Without the mod they do nothing.

**Why the mouse option exists:** the stock game doesn't use the Windows pointer. It draws its own, moved by raw mouse movement with its own speed and no Windows acceleration. So it never quite matches where Windows' pointer is and feels off, and over remote-control tools (RustDesk, Parsec, ...) it wanders by itself.

## What the mod adds to the game folder

| File | Purpose |
|---|---|
| `winmm.dll` | The mod. It passes all calls through to the real Windows `winmm.dll`. |
| `lazysf2.log` | A log, rewritten each time the game starts. |
| `lazysf2.manifest.txt` | The list of files to remove when uninstalling. |

The mod changes no game files and no registry keys. Removing these files returns the game to stock. (The installer also sets Tacview's on/off switch for the mod's add-on once; see [Tacview terrain](#tacview-terrain).)

## Tacview terrain

With **Tacview Recording** on, the first flight you record in each theatre also writes that theatre's heightmap to `%APPDATA%\Tacview\Data\Terrain\Custom\LazySF2Patch\` and a ground texture (made from the theatre's own ground tiles, with the runways painted in) to `%APPDATA%\Tacview\Data\Terrain\Textures\LazySF2Patch\` (8000 × 8000 pixels, about 125 m a pixel, for GermanyCE; a smaller texture from an earlier version is replaced once). Every recording also marks the theatre's airfields by name. A theatre whose ini gives a real-world position, such as GermanyCE, is placed there. The others are not to real scale, so each one sits as its own square over open ocean. Each airfield also gets a sharp patch of ground around its runways.

If Tacview is installed, the installer puts a Tacview add-on in `%APPDATA%\Tacview\AddOns\LazySF2Patch\main.lua` and turns it on in Tacview (1.9.5 or newer) once; close Tacview before installing. Otherwise the mod writes the add-on on your first recorded flight. The mod keeps the file up to date. If you turn it off in Tacview, it stays off, also after an upgrade. If Tacview's Add-ons menu shows **Lazy SF2 Patch** off, turn it on there. It shows only the ground of the theatre you recorded, and it hides the other Lazy SF2 Patch grounds and all of them for other recordings. Without the add-on, every Lazy SF2 Patch theatre that has a real-world position is always visible at that place.

In Tacview, set the terrain layer set to **Auto** (or **Real World**). Recordings say they come from Strike Fighters 2, and Auto then shows the Real World layers, where this terrain lives. If Tacview is set to another simulator's layers, such as BMS, the texture is hidden. Leave Tacview's Advanced Options "Auto Terrain Switch" as it is: our recordings do not write `MapId`, so it does not touch our terrain. Tacview reads new terrain files and add-ons only when it starts, so restart it after your first flight in a new theatre.

The terrain files are outside the game folder, so uninstalling the mod leaves them (it removes only the add-on). You can delete the `LazySF2Patch` folders in `Data\Terrain\Custom` and `Data\Terrain\Textures` yourself. Files of versions before the LazySF2Patch rename (the `LazySF2` folders) are deleted by the mod.

Theatres with a real-world position moved, so recordings of GermanyCE, IsraelME and VietnamSEA made before this version no longer line up with the terrain. The flat mapping also stretches east-west by up to about 10% at the theatre's north and south edges.

## How to tell it's working

- Options → Graphics shows **Display Mode** and **Screen**, Options → Controls shows **Mouse Pointer**, and Options → Gameplay shows **Tacview Recording**.
- In **Borderless**, the game covers the screen without a mode switch, Alt-Tab is instant, and the game stays visible behind other windows.
- `lazysf2.log` in the game folder contains lines like `DisplayMode=1 -> Borderless` and `MouseMode=1 -> match the Windows cursor`.

## Developers

```powershell
.\build.ps1                                       # x86 build: build\Release\winmm.dll
.\scripts\install.ps1                             # copy into the game folder (found from the registry)
.\scripts\uninstall.ps1                           # remove exactly the files in lazysf2.manifest.txt
.\installer\build-installer.ps1 -Version 0.1.0 -Clean   # MSI + Setup.exe -> installer\Releases\
```

Start the game with the environment variable `LAZYSF2_DISABLE=1` to run the DLL as a pure pass-through, for comparing with stock behaviour.
