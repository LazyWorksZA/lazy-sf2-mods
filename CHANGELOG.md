# Changelog

All notable changes to LazySF2, the runtime mods for Strike Fighters 2 (July 2013).
Newest first. There is no tagged release yet; the installer is at 0.1.0.

## Unreleased

### Player features

- **Tacview Recording** (Options › Gameplay, default Off). With it on, every flight, single-player included, is recorded to `Saved Games\ThirdWire\StrikeFighters2\Tacview\SF2 <date>.acmi`. A recording holds:
  - aircraft at 10 Hz, with type names so Tacview shows 3D models;
  - missiles, bombs and rockets at 20 Hz, each linked to the aircraft that fired it;
  - ground objects (buildings, sites, AAA and SAM) at 1 Hz, and ground vehicles and ships at 5 Hz, with their type names;
  - flares and chaff at 10 Hz;
  - tracer rounds at 20 Hz, up to 128 at a time, linked to the shooter;
  - a Destroyed event when an aircraft or a ground object, vehicle or ship is destroyed;
  - explosions for detonations and kills.
  
  The file is flushed to disk every 200 ms or 32 KB and capped at 512 MB.
- **Tacview terrain.** With Tacview Recording on, the first flight in each theatre also exports that theatre's heightmap to `%APPDATA%\Tacview\Data\Terrain\Custom\LazySF2\`. The recording names the theatre (`Theater=`), so Tacview shows the SF2 terrain under the flight. The theatre is placed as its own map on an equatorial patch of open Atlantic, with game distances kept to within about 0.3%. Elevation only for now; a ground texture comes later.
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
- Security hardening:
  - the elevated helper uses a manifest allow-list and refuses reparse points;
  - hooks remove stale entries safely;
  - logs hide the profile path.
