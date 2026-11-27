---
name: live-test
description: Live-test a Lazy SF2 Patch build against the real Strike Fighters 2 install - build, install, drive the game, read lazysf2.log, check Tacview output and network host/client runs. Use when a change needs checking in the running game.
---

# Live test

Rules first (see AGENTS.md): test on the original install only, never a copy; never patch game files; keep captures and logs in the recon scratch folder, not the repo. Get the game install and scratch paths from local agent memory, or ask the owner.

## Before you start

1. **Refuse if the game is running** (`Get-Process StrikeFighters2`). It may be the owner's session. `sf2drive.ps1 start/run` also refuses without `-Force`; do not use `-Force` on a game you did not start.
2. **Synthetic input needs the owner's OK**, and never while the owner is using the machine. `sf2drive.ps1` and `tvshot.ps1` send keys, clicks and mouse-wheel to the desktop. Check for remote-desktop sessions (`query user`, RustDesk or RDP processes) and idle time first. If in doubt, ask.
3. Write multi-line scripts to a file first (in the scratch folder), then run the file. Avoid long inline one-liners with paths.

## Build and install

```powershell
.\build.ps1                       # output build\Release\winmm.dll, no warnings allowed
.\scripts\install.ps1             # copies winmm.dll, writes lazysf2.manifest.txt
```

Run `.\scripts\uninstall.ps1` when done unless the owner wants the mod left in place.

## Drive the game (`tools/test/sf2drive.ps1`)

- `start`, `focus`, `stop`: launch, bring to front, close.
- `run -Seconds N -Shots <dir>`: full Single Mission flight (Strike by default, `-MissionType`), prints the new `lazysf2.log` lines.
- `fly -Seconds N`: main menu to cockpit and back.
- `shot -Out <png>`, `key -Key ctrl+shift+t`, `hold -Key numpad4 -Ms 2000 -Out <png>`, `click`, `away`.
- Read the `-small.png` copy (1280 px wide) of each shot, not the full-size PNG.

## Read the log from a mark

Record the byte length of `lazysf2.log` before the test, then read only what follows (the log persists across runs). `run` does this itself. Check that each hook logged its install line and that no `ERR` lines appear.

## Probe and dev hotkeys

Set `LAZYSF2_PROBE_DIR` (to a scratch folder) before starting the game; the probe is inert without it. Hotkeys (for example Ctrl+Shift+D dump, Ctrl+Shift+J/K move the jet) are listed in `src/hooks/probe_hooks.cpp` and in CHANGELOG. Output goes to that folder.

## Tacview

Turn on Tacview Recording in Options > Gameplay, fly, quit. The `.acmi` lands in `Saved Games\ThirdWire\StrikeFighters2\Tacview\`. Then:

```powershell
.\tools\test\tvshot.ps1 -Recording <file.acmi> -Out <scratch>\tv [-Zoom 8,8,8 -ZoomOut 20 -Keep]
```

It exits 2 if Tacview is already running. It prints add-on lines from the newest Tacview trace log. Read the `-small.png` shots.

## Network runs (host and client)

Developer tools, off unless enabled. Bots build to `build\Release\tools\` (not next to `winmm.dll`).

- Host: start the game with `LAZYSF2_HOST=port` or `address:port` (also needs `LAZYSF2_PROBE_DIR` and, from protocol v5, `LAZYSF2_PASSWORD`). Or use the Multiplayer screen's Host with a session password.
- Client in a second game: `LAZYSF2_CONNECT=address:port` with the same `LAZYSF2_PASSWORD`, where the build supports it.
- `lazysf2-bot` needs `--password TEXT` (or `LAZYSF2_PASSWORD` in its environment) in every mode from v5.
- Synthetic peer: `build\Release\tools\lazysf2-bot.exe --host 127.0.0.1:7777 --script straight|orbit|breaks|climb [--duration S --loss P --jitter MS]`. Use `--serve`, `--script formation` and `--lag` where `lazysf2-bot` usage lists them.
- Unit tests: `build\Release\tools\lazysf2-net-tests.exe` (loopback tests are sometimes flaky; rerun once before calling a failure).

## Report

State what you ran, the log lines that prove each hook, the screenshots read, and anything you could not check. Leave the game stopped and the folder as you found it.
