# Install and uninstall

This is a borderless fullscreen mod for Strike Fighters 2 (July 2013 patch). The project is not affiliated with ThirdWire, and you need your own copy of the game.

> Status: **Phase 2 spike.** Borderless mode is always on while the mod is installed. The Options menu setting comes in Phase 4.

## What it adds

These files go in the game folder, next to `StrikeFighters2.exe`:

| File | Purpose |
|---|---|
| `winmm.dll` | The mod. It passes all calls through to the real Windows `winmm.dll`. |
| `lazysf2.ini` | Optional settings, only written if you choose a mouse mode. |
| `lazysf2.log` | A log written on each start. |
| `lazysf2.manifest.txt` | The list of files to remove when uninstalling. |

The mod changes no game files, ini files or registry keys.

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

- The game starts on the desktop and covers the monitor, without a screen-mode switch.
- Alt-Tab switches to other windows instantly, and back again.
- `lazysf2.log` contains `SetFullscreenState(TRUE) suppressed (borderless)`.

To compare with stock behaviour without uninstalling, start the game with the environment variable `LAZYSF2_DISABLE=1`. The DLL then passes calls through and installs no hooks.

## Uninstall

```powershell
.\scripts\uninstall.ps1
```

This removes exactly the files listed in `lazysf2.manifest.txt`, which returns the game folder to stock. To uninstall by hand, delete the files listed above.
