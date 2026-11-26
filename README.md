# Lazy SF2 Patch

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

**Players:** download `Lazy-SF2-Patch-<version>.msi` from the [Releases](../../releases) page and run it. See [`INSTALL.md`](INSTALL.md).

Every release file is built by GitHub Actions from the tagged commit, never on a developer's machine. To confirm that a download came from this repository's code:

```
gh attestation verify <downloaded file> --repo LazyWorksZA/lazy-sf2-patch
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
| `installer/` | The headless Lazy SF2 Patch helper (C#, Velopack install/uninstall hooks), installer texts, and `build-installer.ps1` |
| `scripts/` | Install and uninstall for development, using a file manifest |
| `tools/` | The winmm proxy generator, the `.cat` unpacker, Ghidra scripts, Process Monitor helpers, and the unattended test driver |
| `tacview/` | The Lazy SF2 Patch Tacview add-on (Lua), built into the DLL |
| `docs/` | The briefs (borderless, network, quality of life) and the [host stream format](docs/stream-format.md) |
| `.github/` | CI (build and tests on every push) and the release workflow |

## Roadmap

**Done (v1.0.0)**

- Borderless brief: recon, borderless, the Display Mode and Screen options, mouse pointer, Alt-Tab, device lost, DPI, Windows key, DXVK check, MSI installer.
- Tacview Recording: aircraft, weapons, ground objects, ships, countermeasures, tracers and explosions; each theatre's terrain, ground texture and airfields; the Lazy SF2 Patch Tacview add-on.
- Releases built on GitHub Actions with build provenance.

**Now: multiplayer** (the [network brief](docs/SF2%20Network%20Layer%20%E2%80%94%20Claude%20Code%20Brief.md))

Multiplayer comes in this order:

1. **Co-op single missions** (in progress). Now: two players flying together. Then: both games running the same mission world, then weapons and damage, then sessions with our own multiplayer screens (encrypted, compressed traffic), then a public release.
2. Co-op campaigns.
3. PvP dogfights.

**Quality of life** (the [QoL brief](docs/SF2%20Quality-of-Life%20Follow-up%20%E2%80%94%20Claude%20Code%20Brief.md); each item ships on its own)

1. Smart scaling for distant aircraft (investigating).
2. Z-fighting and depth precision (investigating).
3. Per-mission start position, with end-of-runway starts.
4. Clickable cockpits (feasibility first).

Smaller items:

- Pilot Twist: the cockpit view twists further and leans a little when looking far to the side or behind (built, in testing).
- A "Head Tracking…" button in Options that opens a settings popup (twist and lean amounts), replacing the single on/off row.
- Device-aware bindings: controls stay bound to the right device when a HOTAS or stick is unplugged at start-up.
- Ramp starts, using the airfields' parking spots.
- Esc in flight pauses first instead of ending the flight.
- Rebind controls while flying (needs our own in-flight pause screen, like the Esc item).
- Voice commands to wingmen through a FoxVox plugin.
- Tougher AI air combat: enemy pilots that are harder to out-turn and out-fly (needs recon on the AI skill settings and manoeuvre logic).
- Rear-view mirror performance: the mirror re-renders the whole world every frame; make it optional or update it less often.
- Tacview: buildings and bridges in recordings, with destruction (in testing); non-ASCII aerodrome names show as "?"; check the IcelandNA theatre.

**Developer tools**

- Automatic probe world snapshot a few seconds into each flight (in progress).
- Render probe: fix its aircraft LOD/cull readings.

**Later**

- Auto-update, and Authenticode signing of the installer.
- Keep the game simulating in the background in exclusive Fullscreen during a network session (Phase 4 covers Borderless).
- A headless dedicated server (needs its own licensed install; revisit after Phase 6).

## Contributing

Contributors, human or AI, should read [`AGENTS.md`](AGENTS.md) first. It lists the ground rules (EULA, no game files in the repo, never patch the exe), the toolchain and its quirks, and the workflow.

## License

[MIT](LICENSE), for this project's own code and documentation only. Third-party components we distribute (MinHook, ENet, Velopack) are listed with their licenses in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
