# lazy-sf2-mods

These are quality-of-life mods for **Strike Fighters 2**, the merged install with the final July 2013 patch.

- **Borderless fullscreen.** The game runs as a borderless window at desktop resolution, so Alt-Tab is instant and other monitors stay usable.
- **Mouse Cursor mode** (optional). The game's cursor follows the Windows cursor, which fixes menu navigation over remote-control tools such as RustDesk.
- **Later: multiplayer.** The long-term goal is to bring multiplayer back. See `docs/SF2 Network Layer — Claude Code Brief.md`.

> **Status: work in progress.** Borderless mode works and is always on while the mod is installed. An Options-menu setting for it is in progress (Phase 4).

This project is not affiliated with or endorsed by Third Wire Productions. It contains no ThirdWire code or game data, and you need your own copy of the game.

## How it works

The mod is a single `winmm.dll` placed next to `StrikeFighters2.exe`.
- It forwards every `winmm` function to the real Windows DLL.
- It uses [MinHook](https://github.com/TsudaKageyu/minhook) to adjust the game at runtime. The game's files on disk are never changed.
- **Borderless.** SF2 already creates a monitor-sized popup window. It enters exclusive fullscreen only through DXGI (`SetFullscreenState`), and the mod suppresses that. It also removes the window's "always on top" flag and turns off Alt+Enter.
- **Mouse Cursor mode** makes DirectInput's mouse non-exclusive, and sets the game's cursor from the Windows cursor, scaled to the menu's 1024×768 space.
- Everything hooks the DirectX objects the game actually loads, so it also works with DXVK.

The full reverse-engineering notes are in [`docs/recon-notes.md`](docs/recon-notes.md).

## Quick start

Requirements: Windows, and Visual Studio 2026 or 2022 with **Desktop development with C++** (x86 toolset).

```powershell
.\build.ps1                         # x86 build: build\Release\winmm.dll
.\scripts\install.ps1               # copies it into the game folder (found from the registry)
.\scripts\install.ps1 -MouseMode Cursor   # optional: remote-control mouse fix
.\scripts\uninstall.ps1             # removes exactly the files the mod added
```

See [`INSTALL.md`](INSTALL.md) for details and for how to tell the mod is working.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The mod: winmm proxy (`proxy/`), hooks (`hooks/`), config and logging |
| `scripts/` | Install and uninstall for development, using a file manifest |
| `tools/` | The winmm proxy generator, the `.cat` unpacker, Ghidra scripts and Process Monitor helpers |
| `docs/` | Briefs, [recon notes](docs/recon-notes.md) and the [Options-menu trace](docs/option-trace.md) |

## Roadmap (borderless brief)

1. ~~Recon~~
2. ~~Borderless spike~~ (in-flight check still to do at the machine)
3. ~~Trace an existing option~~
4. **Display Mode option in the Options menu**, saved in `Options.ini` (in progress)
5. Hardening: Alt-Tab and focus loss, device lost, multi-monitor, DXVK, and an installer (Velopack)

## Contributing

Contributors, human or AI, should read [`AGENTS.md`](AGENTS.md) first. It lists the ground rules (EULA, no game files in the repo, never patch the exe), the toolchain and its quirks, and the workflow.

## License

[MIT](LICENSE), for this project's own code and documentation only.
