# Changelog

All notable changes to the Lazy SF2 Patch, the runtime mods for Strike Fighters 2 (July 2013).
Newest first. Each release is tagged `vX.Y.Z`; its files are on the GitHub Releases page.

## Unreleased

### Developer tools (off unless enabled)

- **Network protocol v2.** The host/client wire protocol changed (theatre check, join velocity, peer messages). It breaks compatibility with earlier builds and with older bots: the host refuses a v1 peer and logs it once.
- **Host: sends its own jet, relays, and puts clients on its wing.** In a session the host sends its own jet to every client at 20 Hz of game time (id 0) and relays each client's States to the other clients. The Welcome gives each client a join position on the host's wing (150 m right per client, 50 m back, at the host's altitude, heading and speed), the host's game time and theatre; the client's avatar holds there until its first State. Each client then hears who else is in the session (the host's callsign from `LAZYSF2_CALLSIGN`, default HOST, and aircraft type), and when someone leaves. A client in another theatre is refused (Bye 5), and the log names both theatre hashes. The host's Byes now carry id 0, the sender, as every Bye does. After a host hitch only the newest State of each client is relayed, and States can no longer crowd Welcomes, PeerInfos, PeerLefts or Byes out of the host's send queue. The client core passes States only from peers the host has announced and not yet reported gone.
- **Host: the avatar is the host player's wingman, and predicted to the present.** A client's avatar is now taken from the host player's own flight (#2 first, same type, friendly), falling back to any friendly and then the nearest aircraft; the join line says which and why, and warns when the client flies another type. The avatar is drawn where the client's jet is now, not an interpolation delay behind, with capped corrections; after 3 s without States it coasts straight on (rates zeroed) instead of hovering, and after 10 s it goes back to the AI until the client's States resume, while the session stays up. The per-second line adds sample age, prediction horizon, snaps, coast steps and peak acceleration, and the host logs once per flight whether the avatars step after the player.
- **Bot: a second player.** `lazysf2-bot` now receives the host's jet, the other clients' relayed States and the peer messages (PeerInfo, PeerLeft, the host's Bye), checks them as the client core does, and logs them. `--script formation` flies on the host's predicted jet at `--offset R,B,U` (default 150 m right, 50 m back, 0 up) and reports how far its States were from that point, judged by the host's own later States. `--lag MS` holds every packet the bot sends and receives for MS, so `--lag 150` gives a 300 ms round trip. `--log-remote` logs one line per second per remote jet (rate, age, horizon, corrections). Ctrl+C now ends a run with a Bye.
- **Bot: `--serve`, a synthetic host.** `lazysf2-bot --serve PORT` runs the mod's own host core and flies a scripted jet (`--script`, default orbit), so the game can be tested as a client on one machine. It welcomes one client onto the jet's wing, sends the jet as id 0 at 20 Hz with its PeerInfo, and logs the client's jet once a second with the same fields as the mod host. `--start X,Y,Z,HEADING` sets where the jet starts; without it the jet starts 2 km ahead of the client's first State and a second Welcome brings the client to its wing (the first Welcome carries no join position). `--pause-at S --pause-for S` freezes its clock to test holds, `--lag MS` delays what it handles and the jet's States, and `--record DIR` writes `botserve_NNN.lzst` (its jet, the client's States, and the new RemoteShown records).
- **Client network core.** A network thread for the client side (connect, Hello, Welcome, clock sync on Pings, checked and queued host messages, Bye). Not wired into the game yet.
- **Host: a Bye sent just before a disconnect now arrives.** The host's refusals and goodbyes (for example Bye 5, wrong theatre) were discarded by the disconnect that followed them.
- **Probe hotkeys for the Phase 4 recon.** Ctrl+Shift+J moves the player's own jet 1 km ahead and turns it 90 degrees right; Ctrl+Shift+K moves it 1 km ahead only. Either then logs the jet's state once a second for 5 s. The probe also logs once per flight whether the player's aircraft steps before or after the other aircraft in a frame.

## 1.0.0 (2026-11-25)

First public release.

### Display and mouse

- **Display Mode** (Options › Graphics): Fullscreen or Borderless. A change applies live when you leave Options.
- **Screen** (Options › Graphics): the monitor the game runs on.
- **Borderless** keeps the game out of exclusive fullscreen and off "always on top", turns off Alt+Enter, and lets the Windows key work.
- **Mouse Pointer** (Options › Controls): "Match Windows cursor" puts the game's pointer exactly on the Windows cursor, so it no longer drifts, including over remote-control tools such as RustDesk. In Fullscreen the cursor stays on the game screen while the game is active.
- **Fixes for stock game problems:** Alt-Tab back into exclusive Fullscreen works; a lost graphics device and display scaling other than 100% are handled; the Display Mode hint text shows in full.

### Tacview

- **Tacview Recording** (Options › Gameplay, default Off) records every flight, single-player included, to `Saved Games\ThirdWire\StrikeFighters2\Tacview\SF2 <date>.zip.acmi`, which Tacview opens directly. A recording holds:
  - aircraft at 10 Hz, with type names so Tacview shows 3D models;
  - missiles, bombs and rockets at 20 Hz, each linked to the aircraft that fired it;
  - ground objects (buildings, sites, AAA and SAM) at 1 Hz, and ground vehicles and ships at 5 Hz, with their type names;
  - flares and chaff at 10 Hz, and tracer rounds at 20 Hz (up to 128 at a time) linked to the shooter;
  - Destroyed events and explosions;
  - the theatre's airfields as named aerodrome markers.

  Recordings are compact: each line holds only the values that changed, and the finished file is zipped (one measured 19.6 MB recording came to about 1.4 MB). While a flight runs the file is a plain `.acmi`; if the game exits before packing finishes, it stays a plain `.acmi` that Tacview opens just the same. A recording is capped at 512 MB.
- **Tacview terrain.** The first recorded flight in each theatre exports that theatre for Tacview: its heightmap, a ground texture built from the theatre's own tiles, and a sharp patch of real ground tiles around each airfield's runways. A theatre whose ini gives a geographic position is placed there (GermanyCE sits over central Germany); the others sit on open Atlantic. The files go to `%APPDATA%\Tacview\Data\Terrain\` and are made once per theatre; delete them to have them made again.
- **Tacview add-on.** The patch writes a small add-on to `%APPDATA%\Tacview\AddOns\LazySF2Patch\`. Enable "Lazy SF2 Patch" in Tacview's Add-ons menu (Tacview 1.9.5 or newer) and it shows only the ground of the recording's theatre, hiding the others. Its "Lazy SF2 Patch: theatre switch" option turns this off.

### Install

- **MSI installer** with a welcome page, the licence and third-party notices, and a finish page. It copies one DLL into the game folder; uninstalling from Installed apps returns the folder to stock. The game's own files are never changed.
- **Works with DXVK**, which is never bundled.
- **Verifiable downloads:** every release file is built by GitHub Actions from the tagged commit, with signed build provenance (`gh attestation verify <file> --repo LazyWorksZA/lazy-sf2-patch`) and SHA-256 hashes in the release notes.

### Developer tools (off unless enabled)

- **Network groundwork** for multiplayer: a sim-step hook, a flight recorder with replay, host mode, a synthetic client (`lazysf2-bot`), host stream recording (`.lzst`, see `docs/stream-format.md`) and an ACMI converter (`lazysf2-acmi`).
- **Unattended test driver** `tools/test/sf2drive.ps1`: starts the game, flies a single mission, takes screenshots.
- **Reverse-engineering tools:** Ghidra scripts, a `.cat` archive lister and extractor, PE inventory, and Process Monitor helpers.
