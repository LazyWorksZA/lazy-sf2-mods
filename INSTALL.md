# Install and uninstall

LazySF2 is a mod for Strike Fighters 2 (July 2013 patch). The project is not affiliated with ThirdWire, and you need your own copy of the game.

## Players: the installer

1. Run **`LazySF2-win-Setup.exe`**. It installs the small **LazySF2** manager for your Windows account. If the .NET 10 Desktop Runtime is missing, Setup installs it.
2. Setup finds Strike Fighters 2 through its installer's registry entry and copies the mod into the game folder.
   - If the game folder needs administrator rights (for example under `C:\Program Files (x86)`), the manager window asks for them once.
   - If a different `winmm.dll` is already in the game folder, nothing is overwritten.
3. The **LazySF2** window opens. You can reopen it from the Start menu at any time. It shows:
   - the game folder and whether the mod is installed, with **Repair** and **Remove from game** buttons;
   - the **Mouse pointer** choice (see below).

**Uninstall** LazySF2 from Windows' *Installed apps*. That removes exactly the files the mod added to the game folder, and asks for administrator rights if needed.

## In the game

Under **Options → Graphics** there are two new rows below Aspect Ratio:

| Setting | Values | Applies |
|---|---|---|
| **Display Mode** | **Fullscreen**: stock exclusive fullscreen. **Borderless**: a window covering the screen, so Alt-Tab is instant and other monitors stay usable. | Immediately when you leave the page |
| **Screen** | "Screen N (W×H)", numbered as in Windows' Settings → Display | The game moves immediately. Its resolution is set to that screen's native size, which takes effect at the next start. |

Both are stored in your own `Saved Games\ThirdWire\StrikeFighters2\Options.ini` (`DisplayMode`, `DisplayMonitor`). If they're missing or invalid, the game behaves as stock: exclusive fullscreen on the usual screen.

## Mouse pointer

The game normally doesn't use the Windows mouse pointer. It builds its own pointer from raw mouse movement (DirectInput), with its own speed and no Windows acceleration. So it never quite matches where Windows' pointer is and can feel off, and over remote-control tools (RustDesk, Parsec, ...) it wanders by itself.

| Choice | What happens |
|---|---|
| **Match the Windows cursor** (recommended, default for new installs) | The game's pointer sits exactly where Windows' pointer is, at Windows' speed and acceleration. |
| **Stock** | The game's own pointer, as without the mod. |

The choice is stored in the game folder as `lazysf2.ini` (`[Mouse] Mode=Cursor` or `Stock`). Without that file, the mod uses Stock.

## What the mod adds to the game folder

| File | Purpose |
|---|---|
| `winmm.dll` | The mod. It passes all calls through to the real Windows `winmm.dll`. |
| `lazysf2.ini` | The mouse pointer setting. |
| `lazysf2.log` | A log, rewritten each time the game starts. |
| `lazysf2.manifest.txt` | The list of files to remove when uninstalling. |

No game files and no registry keys are changed. Removing these four files returns the game to stock. A leftover `DisplayMode` or `DisplayMonitor` key in `Options.ini` does nothing without the mod.

## How to tell it's working

- **Options → Graphics** shows **Display Mode** and **Screen** below Aspect Ratio.
- In **Borderless**, the game covers the screen without a mode switch, Alt-Tab is instant, and the game stays visible behind other windows.
- `lazysf2.log` in the game folder contains lines like `DisplayMode=1 -> Borderless` and `placed on Screen 1`.

## Developers

Build and install from the repository without the installer:

```powershell
.\build.ps1                                # x86 build: build\Release\winmm.dll
.\scripts\install.ps1 -MouseMode Cursor    # copy into the game folder; Cursor or Stock (writes lazysf2.ini)
.\scripts\uninstall.ps1                    # remove exactly the files listed in lazysf2.manifest.txt
.\installer\build-installer.ps1 -Version 0.1.0   # Velopack installer -> installer\Releases\
```

Start the game with the environment variable `LAZYSF2_DISABLE=1` to run the DLL as a pure pass-through, for comparing with stock behaviour.
