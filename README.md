# lazy-sf2-mods

These are quality-of-life mods for **Strike Fighters 2**, the merged install with the final July 2013 patch.

- **Display Mode and Screen** in the game's own **Options → Graphics**: Borderless or stock exclusive Fullscreen, on the monitor of your choice. Both apply immediately.
- **Fixes:** Alt-Tab back into Fullscreen works again (a stock bug), display scaling other than 100% no longer breaks the picture, the Windows key works in Borderless, and lock/unlock recovers.
- **Mouse pointer that matches Windows.** The game normally builds its own pointer from raw mouse movement, so it never quite lines up with Windows' pointer and feels off (and wanders over remote desktop). The mod can make the game's pointer sit exactly on Windows' pointer. This is the default for new installs.
- **Installer:** a Velopack `Setup.exe` with a small manager window, a clean uninstall, and an administrator prompt only when the game folder needs it.
- **Later: multiplayer.** The long-term goal is to bring multiplayer back. See `docs/SF2 Network Layer — Claude Code Brief.md`.

> **Status:** the borderless brief's phases 1–5 are done. Auto-update and a public release are still to come.

This project is not affiliated with or endorsed by Third Wire Productions. It contains no ThirdWire code or game data, and you need your own copy of the game.

## How it works

The mod is a single `winmm.dll` placed next to `StrikeFighters2.exe`.
- It forwards every `winmm` function to the real Windows DLL.
- It uses [MinHook](https://github.com/TsudaKageyu/minhook) to adjust the game at runtime. The game's files on disk are never changed.
- **Borderless.** SF2 already creates a monitor-sized popup window. It enters exclusive fullscreen only through DXGI (`SetFullscreenState`), and the mod suppresses that. It also removes the window's "always on top" flag and turns off Alt+Enter.
- **Matching the Windows cursor** ("Cursor" mode) makes DirectInput's mouse non-exclusive, and sets the game's pointer from the Windows cursor, scaled to the menu's 1024×768 space.
- **The Options rows** are added in memory as the game reads its menu files; no game files are shipped or changed.
- Everything hooks the DirectX objects the game actually loads, so it also works with DXVK.

The full reverse-engineering notes are in [`docs/recon-notes.md`](docs/recon-notes.md).

## Quick start

**Players:** run `LazySF2-win-Setup.exe`. See [`INSTALL.md`](INSTALL.md).

**Developers:** you need Windows, Visual Studio 2026 or 2022 with **Desktop development with C++** (x86 toolset), and for the installer the .NET 10 SDK plus `vpk`.

```powershell
.\build.ps1                                # x86 build: build\Release\winmm.dll
.\scripts\install.ps1 -MouseMode Cursor    # copy into the game folder (found from the registry)
.\scripts\uninstall.ps1                    # remove exactly the files the mod added
.\installer\build-installer.ps1 -Version 0.1.0   # Velopack Setup.exe -> installer\Releases\
```

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The mod: winmm proxy (`proxy/`), hooks (`hooks/`), config and logging |
| `installer/` | The LazySF2 manager app (C#, Velopack hooks and status window) and `build-installer.ps1` |
| `scripts/` | Install and uninstall for development, using a file manifest |
| `tools/` | The winmm proxy generator, the `.cat` unpacker, Ghidra scripts, Process Monitor helpers, and the unattended test driver |
| `docs/` | Briefs, [recon notes](docs/recon-notes.md) and the [Options-menu trace](docs/option-trace.md) |

## Roadmap (borderless brief)

1. ~~Recon~~
2. ~~Borderless spike~~
3. ~~Trace an existing option~~
4. ~~Display Mode option in the Options menu~~ (applied live)
5. ~~Hardening~~: Alt-Tab, device lost, DPI, Windows key, multi-monitor (Screen option), DXVK check, Velopack installer

Next: auto-update and a public release, then the network brief.

## Contributing

Contributors, human or AI, should read [`AGENTS.md`](AGENTS.md) first. It lists the ground rules (EULA, no game files in the repo, never patch the exe), the toolchain and its quirks, and the workflow.

## License

[MIT](LICENSE), for this project's own code and documentation only.
