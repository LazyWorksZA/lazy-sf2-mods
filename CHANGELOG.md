# Changelog

All notable changes to LazySF2, the runtime mods for Strike Fighters 2 (July 2013).
Newest first. Each release is tagged `vX.Y.Z`; its files are on the GitHub Releases page.

## Unreleased

## 1.1.1 (2026-11-25)

### Player features

- Fixed: in Fullscreen with Mouse Pointer set to "Match Windows cursor", the cursor could leave the game screen on a second monitor and click other windows. It now stays inside the game while the game is active, is free after Alt-Tab, and follows a live Screen change. Borderless is unchanged.

## 1.1.0 (2026-11-25)

### Player features

- **Smaller Tacview recordings.** Each aircraft, weapon and ground-object line now holds only the position and attitude values that changed since the last one, and lines that change nothing are left out. A finished recording is also packed into `SF2 <date>.zip.acmi`, which Tacview opens directly. Together this made one measured 19.6 MB recording about 14 times smaller (about 1.4 MB). While a flight runs, the recording stays a plain `.acmi`. The packing runs in the background after the flight ends. If the game exits or crashes before it finishes (or the write fails), the recording stays a plain `.acmi` that Tacview opens as before.

### Developer tools (off unless enabled)

- **`lazysf2-bot` gives up on a silent host.** After 4 s with nothing from the host it prints one line and exits: code 3 when the connection is not established within 4 s or the Welcome has not come 15 s after connecting (the host may be loading), code 4 when the host goes silent or closes the connection later. Code 2 now means only a bad address or a refused connection.
- The loopback tests no longer fail on a slow or busy machine.

## 1.0.0 (2026-11-25)

### Player features

- **Tacview Recording** (Options › Gameplay, default Off). With it on, every flight, single-player included, is recorded to `Saved Games\ThirdWire\StrikeFighters2\Tacview\SF2 <date>.acmi`. A recording holds:
  - aircraft at 10 Hz, with type names so Tacview shows 3D models;
  - missiles, bombs and rockets at 20 Hz, each linked to the aircraft that fired it;
  - ground objects (buildings, sites, AAA and SAM) at 1 Hz, and ground vehicles and ships at 5 Hz, with their type names;
  - flares and chaff at 10 Hz;
  - tracer rounds at 20 Hz, up to 128 at a time, linked to the shooter;
  - a Destroyed event when an aircraft or a ground object, vehicle or ship is destroyed;
  - explosions for detonations and kills;
  - the theatre's airfields as aerodrome markers with their names.
  
  The file is flushed to disk every 200 ms or 32 KB and capped at 512 MB.
- **Tacview terrain.** With Tacview Recording on, the first flight in each theatre also exports that theatre's heightmap to `%APPDATA%\Tacview\Data\Terrain\Custom\LazySF2\`. The recording names the theatre (`Theater=`), so Tacview shows the SF2 terrain under the flight. A theatre with geographic coordinates in its ini is placed there (GermanyCE sits over central Germany); the others stay on open Atlantic, with game distances kept to within about 0.3%. Recordings of GermanyCE, IsraelME and VietnamSEA made before this version no longer line up with the terrain, and with a position the flat mapping stretches east-west by up to about 10% at the theatre's north and south edges. It also builds a ground texture from the theatre's own tiles, with the runways painted in, plus a sharp patch per airfield: the real ground tiles around its runways at up to 512 px per tile (at most 2048 px a side). They go in `%APPDATA%\Tacview\Data\Terrain\Textures\LazySF2\`. The files are made once per theatre and kept: delete them to have them made again.
- **LazySF2 Tacview add-on.** The mod also writes a small add-on to `%APPDATA%\Tacview\AddOns\LazySF2\main.lua`. Enable "LazySF2" in Tacview's Add-ons menu: it then shows only the ground of the recording's theatre (with its airfield patches) and hides the other LazySF2 grounds. Its "LazySF2 theatre switch" option turns this off.
- Fixed: the ground texture did not load in Tacview because of the dots in its file name.
- **Tacview Recording row on Options › Gameplay.** It sits below the stock rows; the Options tabs and pages stay stock. The setting is still stored as `[OtherOptions] TacviewRecording`.
- **Mouse Pointer** (Options › Controls). In Cursor mode the game's pointer follows the Windows cursor, which fixes pointer drift with remote-control tools such as RustDesk.
- **Screen** (Options › Graphics) picks the monitor the game runs on.
- **Display Mode** (Options › Graphics) chooses Fullscreen or Borderless. The change applies live when you leave Options.
- **Borderless:**
  - The game stays out of exclusive fullscreen.
  - The window isn't "always on top".
  - Alt+Enter is off.
  - The Windows key works.
- **Fixes:**
  - The stock Alt-Tab bug in exclusive Fullscreen.
  - A lost graphics device is handled.
  - DPI scaling is handled.
  - The full-length Display Mode hint text shows.
  - Tacview recordings no longer keep detonated weapons and destroyed aircraft frozen in place until the flight ends.
  - A Tacview recording now closes as soon as its flight ends, instead of staying open (and locked) until the next flight starts.
- **Installer:** an MSI wizard (Velopack) with a small headless helper, using the same file-manifest rules as the dev scripts. Uninstalling returns the game folder to stock.
- **Compatibility:** checked to work with DXVK, which is never bundled.

### Developer tools (off unless enabled)

- **Network groundwork:**
  - Phase 1: recon and an object probe.
  - Phase 2: a sim-step hook, a recorder, and replay with network-style smoothing.
  - Phase 3: host mode, a synthetic client (`lazysf2-bot`), host stream recording (`.lzst`, see `docs/stream-format.md`) and an ACMI converter (`lazysf2-acmi`).
- **Unattended test driver** `tools/test/sf2drive.ps1`. Click and key actions are on branch `sf2-autofly`, untested.
- **Reverse-engineering tools:**
  - Ghidra scripts;
  - a `.cat` archive lister and extractor;
  - PE inventory;
  - Process Monitor helpers.

### Project

- The reverse-engineering notes have moved to a separate private repository, together with their history. This repo's history was rewritten to remove them.
- Code comments are trimmed to short `///` XML doc comments.
- **CI and releases on GitHub Actions.** Every push is built and tested. A version tag (`vX.Y.Z`) builds the release on GitHub's runner, records signed build provenance for each file (check it with `gh attestation verify`), and publishes the MSI, `Setup.exe`, `winmm.dll` and their SHA-256 hashes.
- Security hardening:
  - the elevated helper uses a manifest allow-list and refuses reparse points;
  - hooks remove stale entries safely;
  - logs hide the profile path.
