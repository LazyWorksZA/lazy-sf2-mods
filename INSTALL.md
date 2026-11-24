# Install and uninstall

This is a borderless fullscreen mod for Strike Fighters 2 (July 2013 patch). The project is not affiliated with ThirdWire, and you need your own copy of the game.

> Status: **Phase 4.** You choose the display mode in the game, under **Options → Graphics → Display Mode** (Fullscreen or Borderless). The change **applies as soon as you leave the Graphics page**. It's saved as `DisplayMode` in `[GraphicsOptions]` of `Saved Games\ThirdWire\StrikeFighters2\Options.ini`, and is used again at the next start. If the key is missing or invalid, the game runs in stock exclusive fullscreen.

## What it adds

These files go in the game folder, next to `StrikeFighters2.exe`:

| File | Purpose |
|---|---|
| `winmm.dll` | The mod. It passes all calls through to the real Windows `winmm.dll`. |
| `lazysf2.ini` | Optional settings, only written if you choose a mouse mode. |
| `lazysf2.log` | A log written on each start. |
| `lazysf2.manifest.txt` | The list of files to remove when uninstalling. |

The mod changes no game files and no registry keys. The Options menu stores your Display Mode choice in your own `Options.ini`.

## Build

You need Visual Studio 2026 (or 2022) with the **Desktop development with C++** workload and its x86 toolset.

```powershell
.\build.ps1          # produces build\Release\winmm.dll
```

## Install

```powershell
.\scripts\install.ps1                   # reads the game folder from the registry
.\scripts\install.ps1 -GameDir 'D:\SF2' # or pass it yourself
```

The script refuses to overwrite a `winmm.dll` that it did not install.

### Playing over remote control (RustDesk, Parsec, ...)

Remote-control tools send absolute mouse positions, which the game's DirectInput mouse misreads, so the menu cursor drifts. That happens without the mod too. Cursor mode makes the game's cursor follow the Windows cursor instead:

```powershell
.\scripts\install.ps1 -MouseMode Cursor   # writes lazysf2.ini: [Mouse] Mode=Cursor
.\scripts\install.ps1 -MouseMode Stock    # back to stock mouse handling
```

## How to tell it's working

- **Options → Graphics** has a **Display Mode** list below Aspect Ratio.
- With **Borderless** chosen (live, or at the next start):
  - the game starts on the desktop and covers the monitor, without a screen-mode switch;
  - Alt-Tab switches to other windows instantly, and back again;
  - `lazysf2.log` contains `DisplayMode=1 -> Borderless` and `SetFullscreenState(TRUE) suppressed (borderless)`.
- With **Fullscreen**, the game behaves exactly like stock.

The mod's only change to `Options.ini` is the `DisplayMode` key, which the game itself writes when you leave the Graphics page. A leftover `DisplayMode` key does nothing once the mod is uninstalled.

To compare with stock behaviour without uninstalling, start the game with the environment variable `LAZYSF2_DISABLE=1`. The DLL then passes calls through and installs no hooks.

## Uninstall

```powershell
.\scripts\uninstall.ps1
```

This removes exactly the files listed in `lazysf2.manifest.txt`, which returns the game folder to stock. To uninstall by hand, delete the files listed above.
