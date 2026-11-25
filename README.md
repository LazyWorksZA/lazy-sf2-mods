# lazy-sf2-mods

These are quality-of-life mods for **Strike Fighters 2**, the merged install with the final July 2013 patch.

- **Display Mode and Screen** in the game's own **Options → Graphics**: Borderless or stock exclusive Fullscreen, on the monitor of your choice.
- **Mouse Pointer** in **Options → Controls**. The game normally builds its own pointer from raw mouse movement, so it never quite lines up with Windows' pointer and feels off (and wanders over remote desktop). "Match Windows cursor", the default, puts the game's pointer exactly on Windows' pointer.
- **Tacview Recording** in **Options → Gameplay** (default Off). Every flight, single-player included, is recorded to a compact Tacview file (`.zip.acmi`, which Tacview opens directly): aircraft, weapons, ground units, ships, flares, chaff, tracers and explosions. Each theatre's terrain, ground texture and airfields are exported for Tacview too, and a small Tacview add-on shows the right theatre for each recording. See [`INSTALL.md`](INSTALL.md).
- **Fixes:** Alt-Tab back into Fullscreen works again (a stock bug), display scaling other than 100% no longer breaks the picture, the Windows key works in Borderless, and lock/unlock recovers.
- **Installer:** a normal MSI wizard (license and third-party notices included) that copies one DLL into the game folder. No app, window or shortcut is left behind. Uninstall it from *Installed apps*.
- **Later: multiplayer.** The long-term goal is to bring multiplayer back. See `docs/SF2 Network Layer — Claude Code Brief.md`.

> **Status:** the borderless brief is done, and Tacview recording is in. Multiplayer is in progress (developer-only so far).

This project is not affiliated with or endorsed by Third Wire Productions. It contains no ThirdWire code or game data, and you need your own copy of the game.

## How it works

The mod is a single `winmm.dll` placed next to `StrikeFighters2.exe`.
- It forwards every `winmm` function to the real Windows DLL.
- It uses [MinHook](https://github.com/TsudaKageyu/minhook) to adjust the game at runtime. The game's files on disk are never changed.
- **Borderless.** SF2 already creates a monitor-sized popup window. It enters exclusive fullscreen only through DXGI (`SetFullscreenState`), and the mod suppresses that. It also removes the window's "always on top" flag and turns off Alt+Enter.
- **Matching the Windows cursor** ("Cursor" mode) makes DirectInput's mouse non-exclusive, and sets the game's pointer from the Windows cursor, scaled to the menu's 1024×768 space.
- **The Options rows** are added in memory as the game reads its menu files; no game files are shipped or changed.
- Everything hooks the DirectX objects the game actually loads, so it also works with DXVK.

The reverse-engineering notes are kept in a separate private repository.

## Quick start

**Players:** download `LazySF2-win.msi` from the [Releases](../../releases) page and run it. See [`INSTALL.md`](INSTALL.md).

Every release file is built by GitHub Actions from the tagged commit, never on a developer's machine. To confirm that a download came from this repository's code:

```powershell
gh attestation verify LazySF2-win.msi --repo LazyWorksZA/lazy-sf2-mods
```

The release notes also list each file's SHA-256.

**Developers:** you need Windows, Visual Studio 2026 with **Desktop development with C++** (x86 toolset), and for the installer the .NET 10 SDK plus `vpk`.

```powershell
.\build.ps1                                       # x86 build: build\Release\winmm.dll
.\scripts\install.ps1                             # copy into the game folder (found from the registry)
.\scripts\uninstall.ps1                           # remove exactly the files the mod added
.\installer\build-installer.ps1 -Version 0.1.0 -Clean   # MSI + Setup.exe -> installer\Releases\
```

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The mod: winmm proxy (`proxy/`), hooks (`hooks/`), config and logging |
| `installer/` | The headless LazySF2 helper (C#, Velopack install/uninstall hooks), installer texts, and `build-installer.ps1` |
| `scripts/` | Install and uninstall for development, using a file manifest |
| `tools/` | The winmm proxy generator, the `.cat` unpacker, Ghidra scripts, Process Monitor helpers, and the unattended test driver |
| `tacview/` | The LazySF2 Tacview add-on (Lua), built into the DLL |
| `docs/` | The briefs (borderless, network, quality of life) and the [host stream format](docs/stream-format.md) |
| `.github/` | CI (build and tests on every push) and the release workflow |

## Roadmap

**Done (v1.0.0)**

- Borderless brief: recon, borderless, the Display Mode and Screen options, mouse pointer, Alt-Tab, device lost, DPI, Windows key, DXVK check, MSI installer.
- Tacview Recording: aircraft, weapons, ground objects, ships, countermeasures, tracers and explosions; each theatre's terrain, ground texture and airfields; the LazySF2 Tacview add-on.
- Releases built on GitHub Actions with build provenance.

**Now: multiplayer** (the [network brief](docs/SF2%20Network%20Layer%20%E2%80%94%20Claude%20Code%20Brief.md))

1. ~~Recon~~
2. ~~Puppet spike~~: drive one AI aircraft from a recorded path.
3. ~~Synthetic client and recording~~: host mode, a bot client, stream recording, ACMI export (developer-only).
4. **Two real players** (next): replicated player jets, a shared clock, smoothing; the game keeps running (and optionally keeps its sound) without focus; the cursor stays on the game screen in Fullscreen.
5. Weapons and damage.
6. Sessions: handshake, content hashing, our own multiplayer screens, master server. Spawns use a theatre's dogfight file when it has one, otherwise a runway or air start.
7. Public multiplayer release: NAT traversal with relay.

**Quality of life** (the [QoL brief](docs/SF2%20Quality-of-Life%20Follow-up%20%E2%80%94%20Claude%20Code%20Brief.md); each item ships on its own)

1. Smart scaling for distant aircraft (investigating).
2. Z-fighting and depth precision (investigating).
3. Per-mission start position, with end-of-runway starts.
4. Clickable cockpits (feasibility first).

Smaller items: automatic pilot lean when looking far to the side or behind (the view now only turns in place); Esc in flight pauses first instead of ending the flight; Tacview: move old recordings to a theatre's current position, non-ASCII aerodrome names.

**Later**

- Auto-update, and Authenticode signing of the installer.

## Contributing

Contributors, human or AI, should read [`AGENTS.md`](AGENTS.md) first. It lists the ground rules (EULA, no game files in the repo, never patch the exe), the toolchain and its quirks, and the workflow.

## License

[MIT](LICENSE), for this project's own code and documentation only. Third-party components we distribute (MinHook, ENet, Velopack) are listed with their licenses in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
