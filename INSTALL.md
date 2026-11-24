# Install and uninstall

LazySF2 is a mod for Strike Fighters 2 (July 2013 patch). The project is not affiliated with ThirdWire, and you need your own copy of the game.

## Players: the installer

1. Run **`LazySF2-win.msi`**. It's a normal installer wizard: a welcome page, the license (ours plus the [third-party notices](THIRD_PARTY_NOTICES.md)), an administrator prompt, and a finish page.
   - If the .NET 10 runtime is missing, the installer adds it.
2. The installer finds Strike Fighters 2 through the game's own registry entry and copies **one file, `winmm.dll`**, into the game folder. If a different `winmm.dll` is already there, it leaves it untouched.
3. That's all. There's no app, window or shortcut. Start the game as usual; the settings are in the game (below).

**Uninstall** LazySF2 from Windows' *Installed apps*. That removes exactly the files the mod added to the game folder.

## Settings, in the game

| Where | Setting | Values | Default |
|---|---|---|---|
| Options → Graphics (below Aspect Ratio) | **Display Mode** | **Fullscreen**: stock exclusive fullscreen. **Borderless**: a window covering the screen, so Alt-Tab is instant and other monitors stay usable. | Fullscreen |
| Options → Graphics | **Screen** | "Screen N (W×H)", numbered as in Windows' Settings → Display. Choosing another screen also sets the game resolution to its native size, which applies at the next start. | the stock choice (primary) |
| Options → Controls (below Keyboard Map) | **Mouse Pointer** | **Match Windows cursor**: the game's pointer sits exactly on the Windows cursor, at Windows' speed and acceleration. **Stock game pointer**: the game builds its own pointer from raw mouse movement, as without the mod. | Match Windows cursor |

Changes take effect when you leave the Options screen, like the game's own settings.

All three are stored in your own `Saved Games\ThirdWire\StrikeFighters2\Options.ini`: `DisplayMode` and `DisplayMonitor` under `[GraphicsOptions]`, and `MouseMode` under `[ControlOptions]`. Without the mod they do nothing.

**Why the mouse option exists:** the stock game doesn't use the Windows pointer. It draws its own, moved by raw mouse movement with its own speed and no Windows acceleration. So it never quite matches where Windows' pointer is and feels off, and over remote-control tools (RustDesk, Parsec, ...) it wanders by itself.

## What the mod adds to the game folder

| File | Purpose |
|---|---|
| `winmm.dll` | The mod. It passes all calls through to the real Windows `winmm.dll`. |
| `lazysf2.log` | A log, rewritten each time the game starts. |
| `lazysf2.manifest.txt` | The list of files to remove when uninstalling. |

No game files and no registry keys are changed. Removing these files returns the game to stock.

## How to tell it's working

- Options → Graphics shows **Display Mode** and **Screen**, and Options → Controls shows **Mouse Pointer**.
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
