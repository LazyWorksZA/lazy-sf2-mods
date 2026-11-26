# AGENTS.md

These are instructions for AI coding agents, and for humans, working on this repository. Read this file before changing anything.

This is the repo's **only** instructions file. Don't add a `CLAUDE.md` or tool-specific copies. Claude Code reads `AGENTS.md` natively, but only when no `CLAUDE.md` exists. Put new conventions here.

## What this is

This repo holds runtime mods for Strike Fighters 2, the July 2013 final patch: a 32-bit x86 D3D10 game with a D3D9 fallback. The mod is a `winmm.dll` proxy that hooks the game with MinHook. The briefs in `docs/` set the scope and the phases. The reverse-engineering notes live in a separate private repo, `lazy-sf2-recon` (check it out next to this one, at `../lazy-sf2-recon`). Its `recon-notes.md` is the living findings file; read the section that concerns you before starting.

## Machine-specific locations

These locations differ per machine, so this repo doesn't name them: the game install, the game backup, the tools folder (Ghidra, x64dbg, DXVK and the like), the recon scratch folder (logs, captures, decompiles, extracted data), the Ghidra project, and the junction to the game used for Ghidra imports. Keep them in your local agent memory, and ask the user for any you don't have.

## Ground rules (non-negotiable)

1. **Never put game files in the repo.** That includes ThirdWire binaries, `.cat` contents, extracted inis, STR files, textures, the EULA text and decompiled game code.
   - `.gitignore` covers the common cases, but check `git status` before every commit.
   - Notes describe behaviour and give RVAs. Paraphrase instead of pasting decompiled code (EULA 3(d)).
2. **Never patch the game on disk.** No byte patches to the exe or DLLs, and no edits to game ini or cat files that you leave behind.
   - The mod may only add its own files to the game folder, and must list them in `lazysf2.manifest.txt` (see `scripts/install.ps1`). Running `scripts/uninstall.ps1` must return the folder to stock.
   - Temporary experiments on a game file must be restored from the game backup, then hash-checked.
3. **Test against the original install. Don't run a copy of the game.** EULA 3(b) allows copies only as backups. The game backup exists only to restore files.
4. **Stay compatible with DXVK, but don't vendor it.** Don't assume we own `d3d9.dll`, `dxgi.dll` or the `d3d10*.dll` files. Hook the COM objects the game actually gets, rather than replacing system DLLs.
   - DXVK isn't part of this project: never commit it, ship it, or install it with the mod. We only make sure the mod works when a player has installed it.
   - For compatibility tests, keep it outside the repo (in the tools folder), add it to the game folder only for the test, and remove it afterwards (see `lazy-sf2-recon`, `recon-notes.md` §15).
5. **Stop at each phase checkpoint** listed in the brief, report, and wait for review. If something contradicts the brief, stop and say so; don't work around it.
6. **Write findings down as you go** in the `lazy-sf2-recon` repo, never in this one: addresses as RVAs (offsets from the module base), function roles, file formats and open questions.
7. **Third-party notices.** Every third-party component we *distribute* (statically linked libraries such as MinHook; shipped assemblies and binaries such as Velopack's) must be listed in `THIRD_PARTY_NOTICES.md` with its full license text, taken from the package itself rather than from memory. The installer ships that file with `LICENSE`. When you add, upgrade or remove a dependency, update the file in the same change.
8. **Privacy.** Process Monitor captures and logs contain the machine's hostname and user paths. Keep them in the recon scratch folder, and keep them out of commits.

## Fixed-address hooks

The July 2013 binaries never change, so hooking by RVA is acceptable. But **every RVA hook must first check the module's PE `TimeDateStamp` and `SizeOfImage`**, and skip with a log line if they don't match. See `is_expected_build` in `src/hooks/hook_util.h`.

The game DLLs are linked at `0x10000000`, and the exe at `0x00400000`.

## Build and install

```powershell
.\build.ps1                    # enters the VS dev shell (x86) and runs the CMake preset; output build\Release\winmm.dll
.\scripts\install.ps1 [-MouseMode Cursor|Stock]
.\scripts\uninstall.ps1
dotnet test installer\LazyWorks.SF2Patch.Manager.Tests   # xUnit tests for the installer helper
```

The test project is only for development: it is not published or shipped.

- **Toolchain:** VS 2026 (v18), generator `"Visual Studio 18 2026" -A Win32`, triplet `x86-windows-static`, with a static CRT (`CMAKE_MSVC_RUNTIME_LIBRARY`).
- **vcpkg:** use the copy bundled with VS. It requires `builtin-baseline` in `vcpkg.json`.
- **MSVC `C1083 Cannot open compiler generated file: ''`** is the long-path limit. Keep build directories short.
- **Developer tools** (`lazysf2-bot`, `lazysf2-acmi`, `lazysf2-net-tests`) build into `build\Release\tools\`, not next to `winmm.dll`, so they never load the proxy as their own winmm. The installer copies only `build\Release\winmm.dll`.
- **Runtime log:** `lazysf2.log` in the game folder. Use it to check that hooks were installed and what they did.
- **`LAZYSF2_DISABLE=1`** runs the mod as a pure pass-through proxy.
- **`LAZYSF2_PROBE_DIR=<folder>`** (an existing folder outside the game folder) turns on the developer probe and its Ctrl+Shift hotkeys, polled by one thread in `src/hooks/probe_hooks.cpp` while the game is in front: D snapshot, P puppet, M missile, R recorder, T/Y replay and its rate, J/K player jump, and for Pilot Twist screenshots **W** (twist on/off for this session, without changing the setting) and **H** (head hold: off, 120 degrees left, 160 left, 120 right, 160 right, off; pitch 0). The hold feeds the same inputs real head tracking does: inside the view update it answers the view's head-tracker axis reads (input-manager axes 3 = yaw, 4 = pitch, through a ReadAxis detour hooked on the first hold) with the held angles, and also writes the head target angles for the hat path, so the game clamps to the seat and smooths it like real head movement; the twist applies on top when it is on. While a hold is active, a `pilot twist hold:` line once a second shows the target, the smoothed yaw and the twist added. Smoothing stops while the game is paused, so let it settle unpaused before taking shots. Positive view yaw is to the left. Each press is logged once. Without the variable none of this runs.
- **Installer:** `installer\build-installer.ps1 -Version x.y.z [-Clean]` builds the mod, publishes the headless C# helper (`installer/LazyWorks.SF2Patch.Manager`, framework-dependent .NET 10, no WinForms), and runs `vpk pack --msi`.
  - The player-facing artefact is `LazySF2Patch-win.msi`, published as `Lazy-SF2-Patch-<version>.msi` by the release workflow: a wizard (welcome, license with third-party notices, finish), per machine, elevated, with no shortcuts. Velopack's `Setup.exe` is built as well.
  - It needs the .NET 10 SDK and `vpk`, installed with `dotnet tool install -g vpk --version 1.2.161`. Keep that version in step with the `Velopack` package version in the csproj.
  - The output in `installer\Releases` and `installer\publish*` is ignored by git.
  - The helper's install, update and uninstall hooks use the same manifest rules as `scripts/install.ps1`. Hooks have 15–30 s and no UI. The MSI runs them elevated. The per-user `Setup.exe` falls back to a UAC prompt on the next normal start, and its uninstall falls back to an elevated `cmd /c del`.
  - Velopack's MSI doesn't start the helper after installing and has no "launch" checkbox. The finish page tells players how to start the game.
- **Settings** live in the player's `Options.ini`, written by the game's own Options menu (rows the mod adds): `[GraphicsOptions] DisplayMode/DisplayMonitor`, `[ControlOptions] MouseMode/PilotTwist` (plus `PilotTwistAngle`, degrees 0–45, default 30, and `PilotTwistLean`, cm 0–6, default 4, up = half of it: hand-edited until the planned "Head Tracking…" popup; missing or invalid values use the defaults and are logged) and `[OtherOptions] TacviewRecording` (the key keeps its old section, but the row is on the Gameplay page). They take effect when the player leaves the Options screen. The mod reads them once per process: if you edit `Options.ini` by hand during a session, the change applies after a restart, and only the Options UI applies live. There is no mod settings file.
- **Tacview terrain:** with Tacview Recording on, the first recorded flight in each theatre per game session also writes that theatre's heightmap (`LazySF2Patch-<THEATRE>.raw` and `.xml`) to `%APPDATA%\Tacview\Data\Terrain\Custom\LazySF2Patch\`, and a ground texture composed from the theatre's tiles (`LazySF2Patch-<THEATRE>.jpg` at 16 px a tile, 8000x8000 for a 500-tile theatre, composed and JPEG-encoded in bands of 16 tile rows through a streaming WIC writer so the whole image is never held; and `.xml`, plus one sharp `LazySF2Patch-<THEATRE>-AFnnn.jpg` patch per airfield, listed in that xml) to `%APPDATA%\Tacview\Data\Terrain\Textures\LazySF2Patch\`. File names use dashes because Tacview cannot load a texture whose name has dots; the Tacview ids stay dotted (`LazySF2Patch.<THEATRE>`, patches `LazySF2Patch.<THEATRE>.AFnnn`), and the recording names the theatre in `Theater=` and `LazySF2PatchTheater=` (no `MapId`). The mod also writes its Tacview add-on, embedded from `tacview/addon/LazySF2Patch/main.lua`, to `%APPDATA%\Tacview\AddOns\LazySF2Patch\main.lua` when it differs. The installer helper embeds the same file (`TacviewAddOn.cs`): its install and update hooks (first, before the game-folder step) do nothing unless Tacview is present (`%APPDATA%\Tacview` or `HKCU\Software\Raia Software Inc.\Tacview`); otherwise they write it for the user running the hook and set Tacview's per-user switch `HKCU\Software\Raia Software Inc.\Tacview\Local\AddOns\LazySF2Patch\Enabled=1` once, together with the marker `EnabledByInstaller=1` in the same key; while the marker exists, Enabled is never touched again (Tacview itself writes 0 for a new add-on, so a 0 without the marker is not the player's choice). They carry `TheatreSwitch` over from the pre-rename `AddOns\LazySF2` key and delete that key and the old files. Uninstall removes only the add-on file (and `.tmp`) and its folder if empty: Tacview's `LazySF2Patch` key stays, so whichever order an upgrade runs the hooks in, the key and the player's choice are kept. While a Tacview process runs, the marker is not written, so the next hook sets Enabled again. Folders that are reparse points are refused (installer) or skipped with a log line (the mod's cleanup). **The mod never touches the registry.** The files are made once and kept, so delete them to have them made again; the one exception is a ground texture smaller than the current size (the earlier 4000x4000 ones), replaced once (if that fails, the old one is kept). Once per session the export also deletes the files of the pre-rename export (exact `LazySF2-*` names in `Textures\LazySF2`, `Custom\LazySF2`, and `AddOns\LazySF2\main.lua`), and those folders if empty. The mod reads the tile images and inis through the exe's own file manager (`src/hooks/game_files.cpp`), which also sees cat and DLC content. These files are outside the game folder and not in `lazysf2.manifest.txt`: uninstall leaves them.

## Reverse-engineering tools

| Tool | Use |
|---|---|
| Ghidra 12.2 (in the tools folder, needs JDK 25) | The analysed Ghidra project: the exe at the root, then `/Core`, `/Flight`, `/Menu`, `/Objects`. |
| `tools/ghidra/Invoke-Ghidra.ps1` | Runs a script from `tools/ghidra/` read-only, for example `-Folder /Core -Program MenuSystem.dll -Script StringXrefs.java -ScriptArgs foo`. Only one run at a time, because the project locks. |
| `tools/ghidra/*.java` | Cross-references from strings, wide tags, imports and addresses; vtable dump; decompile everything. Write output to `decomp` in the recon scratch folder. |
| `tools/catx.py` | Lists and extracts `.cat` archives. It refuses to extract into the repo. |
| `tools/recon/` | Inventory of PE imports and exports, a Process Monitor filter generator, and a capture summariser. |
| `tools/test/sf2drive.ps1` | Unattended game checks: start (optionally with DXVK logs and HUD), focus, screenshot, switch away and back, stop, and `run -Seconds N -Shots DIR`, a full Single Mission flight (Strike by default, `-MissionType`) from launch to quit that prints the new `lazysf2.log` lines. It sends input to the desktop, so use it only with the user's OK. |
| `tools/gen_winmm_proxy.py` | Regenerates `src/proxy/winmm.def` and `winmm_exports.inc` from the system `winmm.dll`. |

**Gotcha:** Ghidra's `.bat` breaks on `(x86)` in a path, so import the game through a junction whose path has no parentheses.

## Code conventions

- C++20, `/W4 /permissive-`, everything in namespace `LazyWorks::SF2Patch` (C# uses `LazyWorks.SF2Patch.*`). The build must have no warnings.
- Each feature is its own `src/hooks/*.cpp`. Features that need modules loaded later register with the module watch (`module_watch.cpp`) instead of hooking `LoadLibrary` again.
- Settings go in `src/config.*`. Defaults must give stock behaviour, unless the brief says a feature is always on.
- Log every hook install and every behaviour change once, with `LOG(...)`. Don't log every frame.
- **Comments stay minimal.** Use `///` XML doc comments (`<summary>`, and `<param>`/`<returns>` only when they add something) on functions and types whose purpose isn't obvious. Don't restate the code. Keep short "why" comments for threading, lifetime and other non-obvious constraints. How the game works inside belongs in `lazy-sf2-recon`; point to it (`// See lazy-sf2-recon: recon-notes §4.3.`) instead of explaining it in the code.

## Workflow

- **GitHub flow, without pull requests.** Do each piece of work on its own branch off `master`, and commit there. **Never commit to `master` directly.** Merge into `master` and push it **only when the user explicitly says so**. Don't open pull requests.
- Delete branches after they are merged.
- Each branch adds its user-visible changes to `CHANGELOG.md` under **Unreleased**, in the same branch.
- End commit messages with the `Co-Authored-By` lines given in your session instructions, if any.
- Testing needs the live game. Get the user to drive it, ask specific questions, and read `lazysf2.log` yourself.
- The user often works over RustDesk. Judge mouse behaviour at the machine, or with Cursor mode on.
