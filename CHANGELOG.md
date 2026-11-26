# Changelog

All notable changes to the Lazy SF2 Patch, the runtime mods for Strike Fighters 2 (July 2013).
Newest first. Each release is tagged `vX.Y.Z`; its files are on the GitHub Releases page.

## Unreleased

### Player features

- **Fixed: the cursor stays on the game's screen in Fullscreen with Match Windows cursor.** The Windows cursor is kept inside the game window while the game is active, and released when you switch away.

### Developer tools (off unless enabled)

- **Phase 4: two players.** Client mode, host relay, protocol v2, predicted remote jets, a shared clock with drift, the bot's formation, lag and serve modes, client recordings and the comparison report, and background simulation in sessions. The entries below give the details; the setup steps are in `lazy-sf2-recon` (`network-recon.md`, section 8).
- **Network protocol v2.** The host/client wire protocol changed (theatre check, join velocity, peer messages). It breaks compatibility with earlier builds and with older bots: the host refuses a v1 peer and logs it once.
- **Host: sends its own jet, relays, and puts clients on its wing.** In a session the host sends its own jet to every client at 20 Hz of game time (id 0) and relays each client's States to the other clients. The Welcome gives each client a join position on the host's wing (150 m right per client, 50 m back, at the host's altitude, heading and speed), the host's game time and theatre; the client's avatar holds there until its first State. Each client then hears who else is in the session (the host's callsign from `LAZYSF2_CALLSIGN`, default HOST, and aircraft type), and when someone leaves. A client in another theatre is refused (Bye 5), and the log names both theatre hashes. The host's Byes now carry id 0, the sender, as every Bye does. After a host hitch only the newest State of each client is relayed, and States can no longer crowd Welcomes, PeerInfos, PeerLefts or Byes out of the host's send queue. The client core passes States only from peers the host has announced and not yet reported gone.
- **Host: the avatar is the host player's wingman, and predicted to the present.** A client's avatar is now taken from the host player's own flight (#2 first, same type, friendly), falling back to any friendly and then the nearest aircraft; the join line says which and why, and warns when the client flies another type. The avatar is drawn where the client's jet is now, not an interpolation delay behind, with capped corrections; after 3 s without States it coasts straight on (rates zeroed) instead of hovering, and after 10 s it goes back to the AI until the client's States resume, while the session stays up. The per-second line adds sample age, prediction horizon, snaps, coast steps and peak acceleration, and the host logs once per flight whether the avatars step after the player.
- **Bot: a second player.** `lazysf2-bot` now receives the host's jet, the other clients' relayed States and the peer messages (PeerInfo, PeerLeft, the host's Bye), checks them as the client core does, and logs them. `--script formation` flies on the host's predicted jet at `--offset R,B,U` (default: the bot's slot, 150 m right per client id and 50 m back) and reports how far its States were from that point, judged by the host's own later States. `--lag MS` holds every packet the bot sends and receives for MS, so `--lag 150` gives a 300 ms round trip. `--log-remote` logs one line per second per remote jet (rate, age, horizon, corrections). Ctrl+C now ends a run with a Bye.
- **Bot: `--serve`, a synthetic host.** `lazysf2-bot --serve PORT` runs the mod's own host core and flies a scripted jet (`--script`, default orbit), so the game can be tested as a client on one machine. It welcomes one client onto the jet's wing, sends the jet as id 0 at 20 Hz with its PeerInfo, and logs the client's jet once a second with the same fields as the mod host. `--start X,Y,Z,HEADING` sets where the jet starts; without it the jet starts 2 km ahead of the client's first State and a second Welcome brings the client to its wing (the first Welcome carries no join position); the bot re-sends it about once a second until the client's States show it there, for at most 10 s. `--pause-at S --pause-for S` freezes its clock to test holds, `--lag MS` delays what it handles and the jet's States, and `--record DIR` writes `botserve_NNN.lzst` (its jet, the client's States, and the new RemoteShown records).
- **Client network core.** A network thread for the client side (connect, Hello, Welcome, clock sync on Pings, checked and queued host messages, Bye).
- **The game as a client.** With the probe on, `LAZYSF2_CONNECT=host:port` (a numeric address or a name) makes the game join that host once the player is airborne (above 500 m, faster than 50 m/s), with the callsign from `LAZYSF2_CALLSIGN` (default PLAYER), the player's aircraft type and the theatre. On the Welcome it moves the player's jet onto the host's wing (position and heading; pitch, roll and speed kept), once per session and only when it is more than 50 m away; `LAZYSF2_NET_JOIN=none` turns the move off. A Welcome without a join position (the bot's `--serve` without `--start`) does not move it; the one that follows does. It sends the player's jet at 20 Hz of the host's time, holds it while the host's clock is frozen or silent, and shows the host and every other client the host announces on an AI aircraft of the player's own flight, predicted to the present; a remote that goes quiet coasts after 3 s and goes back to the AI after 10 s. It records the session to `LAZYSF2_PROBE_DIR\client_NNN.lzst` (what it received, its own States as kind 7, and what it showed as kind 9). It logs once a second per remote (the host's fields), and the clock only when it syncs, holds or resets. It leaves with Bye 3 when the player changes, and tries again every 5 s after a failure. `LAZYSF2_HOST` and `LAZYSF2_CONNECT` together turn both off.
- **Recordings on both sides, and the comparison report.** Every session file (`stream_NNN`, `client_NNN`, `botserve_NNN.lzst`) now starts with a SessionInfo record (role, own id, callsign, aircraft) and names the theatre in its header (the host's and the client's). The host also records what it showed of each client (RemoteShown, 10 Hz and every snap) and when an avatar is parked or taken again; the client records the PeerInfos and PeerLefts it gets and the same avatar events; the bot's `--serve` records its jet's States as sent. `lazysf2-acmi` converts all three kinds of file, and `--ghosts` adds each shown track as its own green object ("BOT1 as seen by HOST") next to the real one. `lazysf2-acmi --report VIEWER.lzst TRUTH.lzst [--id N] [--offset R,B,U]` aligns the two files on host time and prints, per 10 s and overall, the median, p95 and max error of the shown jet (along track, across, vertical), the angle error, snaps, coasting time and the shown path's peak acceleration; `--offset` takes the host's jet plus a formation offset as the truth, for the bot's formation runs. See `docs/stream-format.md`.
- **The other player flies in your flight.** On both sides a remote player's avatar is the local player's first free wingman, in slot order (#2, then #3, ...), whatever its type or distance. Only when the flight has no free plane does the picker fall back to another friendly aircraft, one within 20 km of where the avatar is placed first, and it logs that as a fallback with the distance moved. Enemy (or unknown-side) aircraft are never taken: with no friendly free there is no avatar.
- **Refusals are logged once.** A host that refuses before the Welcome (busy, or no free friendly aircraft) is retried quietly every 5 s, and the client names the reason once until a Welcome or a new player. The host logs a client it keeps refusing at most once a minute, with a count.
- **Host: a Bye sent just before a disconnect now arrives.** The host's refusals and goodbyes (for example Bye 5, wrong theatre) were discarded by the disconnect that followed them.
- **The game keeps flying in the background during a session.** While a network session runs (the host has a client, or the client is welcomed) and Display Mode is Borderless, Alt-Tab no longer pauses the game: the flight keeps simulating and drawing, and only the keyboard, mouse and joystick are let go until you come back. This works whatever `[OtherOptions] BackgroundMode` says, and minimised the frames are paced as with BackgroundMode 1. `LAZYSF2_BG_SIM=0` turns this off (BackgroundMode still applies). Fullscreen keeps the stock pause (it logs "Borderless only" once). If the session ends while the game is in the background and BackgroundMode is 0, the stock pause applies at once. Sound follows BackgroundMode: it goes silent in the background unless BackgroundMode is 2.
## 1.1.3 (2026-11-26)

### Tacview

- **Tacview: radar and weapon locks are recorded; Tacview draws lock lines.** When an aircraft (yours or the AI's) tracks or locks another aircraft with its radar, the recording names the target, and clears it when the lock is lost. A guided missile names the aircraft it is guiding to.
- **Tacview: better shapes for ground vehicles, buildings, fuel tanks and bridges.** Ground objects and structures now get more specific Tacview types (truck, jeep, tank, armored vehicle, anti-aircraft, infantry, ship classes, storage and fuel tanks), chosen from the game's category with the name's generic words as a hint. Bridges are written as buildings, because Tacview has no bridge type.

### Fixed

- Airfield names with accented letters (such as ü or ö) show correctly in Tacview.

### Developer tools (off unless enabled)

- **Lock probe hotkeys.** With the probe on: Ctrl+Shift+A places an AI aircraft (an enemy if there is one) 3 km in front of you, at your altitude, heading and speed; Ctrl+Shift+O makes your radar lock it through the avionics' own commands; Ctrl+Shift+I makes that aircraft's radar lock you. Each logs the radar state (`lock probe:` lines).

### Gameplay

- **The game can keep running in the background** (`Options.ini` `[OtherOptions] BackgroundMode`, set by hand, default 0). With **0** the game pauses when its window loses focus, as stock. With **1** it keeps running in any flight (single player included) and in the menus; the keyboard, mouse and joystick are let go until you come back. While the game is minimised, the mod limits it to its normal frame rate so it doesn't use a whole CPU core. With **2** sounds keep playing as well (the menu music still goes quiet in the background). Borderless only: in Fullscreen the game pauses as stock and the log says so once. The value is read when the game starts; a change applies after a restart.

## 1.1.2 (2026-11-26)

### Fixed

- **The installer finds more Strike Fighters 2 installs, and lets you choose the game folder when it can't.** It now also looks in the 64-bit registry, the UAC VirtualStore, Windows' list of installed programs and a folder you chose before. If none works, Start menu > Lazy SF2 Patch lets you pick the folder.

## 1.1.1 (2026-11-26)

### Fixed

- **Rockets show as rockets in Tacview recordings, not as missiles.** Unguided rockets are now written with the Tacview rocket type. Bombs (including laser and TV guided ones) stay bombs, and missiles stay missiles.

### Releases

- **The Tacview add-on is published on its own.** Each release now also has `Lazy-SF2-Patch-<version>-Tacview-AddOn.zip`, for players who install `winmm.dll` by hand. Extract it into `%APPDATA%\Tacview\AddOns\`.

### Developer tools (off unless enabled)

- **Tacview destroy test hotkey.** With the probe on, Ctrl+Shift+X during a recorded single mission destroys the nearest building the recording tracks (its damage is set to 100), so the building's destruction can be checked in Tacview without flying a strike. The recording's end line now also counts all Destroyed events.

## 1.1.0 (2026-11-26)

### Tacview

- **Buildings and bridges in recordings, with their destruction.** Recordings now show the placed buildings and bridges (hangars, bunkers, factories, town buildings) of the target areas the mission uses, as static objects on the ground. Those the game already shows as live objects (guns, SAM sites, radars, mission targets) are not doubled. In single missions only the buildings the game actually draws are recorded, and a building that is destroyed gets Tacview's destroyed event and an explosion.
- **Sharper ground texture.** The theatre's full ground texture is now twice as sharp: 8000 x 8000 pixels (about 125 m a pixel) for GermanyCE instead of 4000 x 4000. An existing smaller texture is replaced once, on the first recorded flight in that theatre; if that fails, the old texture is kept. The file is about four times larger and takes a few seconds longer to make, in the background. The image is built in strips, so it needs less memory than before.
- **The add-on is turned on by the installer.** Tacview turns new add-ons off, so the Lazy SF2 Patch add-on did not run until you enabled it. If Tacview is installed, the installer now puts the add-on in place and turns it on once (close Tacview before installing). If you then turn it off in Tacview, it stays off, also after an upgrade. If Tacview's Add-ons menu shows **Lazy SF2 Patch** off, turn it on there. The add-on's theatre-switch setting from before the rename is kept. Uninstalling removes the add-on file; Tacview's switch for it stays.

### Cockpit

- **Pilot Twist** (Options › Controls, default Off): in the cockpit, looking far to the side or behind (hat or head tracker) turns the view up to 30 degrees further, as if you twist in the seat, and leans you 4 cm toward that side (and 2 cm up). Both amounts can be changed by hand in `Options.ini` (`[ControlOptions] PilotTwistAngle`, 0–45 degrees, and `PilotTwistLean`, 0–6 cm); a menu for them is planned.

### Developer tools (off unless enabled)

- **Automatic probe snapshot.** With the probe on, a world snapshot is taken 10 s into each flight (`snapNNN_auto*` in the probe folder: aircraft dumps, the object table and the theatre). The newest 5 flights are kept. It copies up to 32 aircraft and lists up to 2000 objects in one frame, so it may hitch for a frame.
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
