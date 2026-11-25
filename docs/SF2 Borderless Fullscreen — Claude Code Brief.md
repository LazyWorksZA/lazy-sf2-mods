# SF2 Borderless Fullscreen — Claude Code Brief

Nov 24, 2026 · @Jozias van der Westhuizen

## Goal and scope

Add a borderless fullscreen display mode to Strike Fighters 2 (final July 2013 patch). The player picks it in the game's own Options menu, and the choice is saved in Options.ini alongside the existing graphics settings. In v1 the new mode takes effect on the next launch.

This is also the first step towards adding multiplayer back into SF2. The injection, hooking and reverse-engineering tooling built here should be reusable for that later.

**In scope**

- A display mode setting with at least two values: exclusive fullscreen (current behaviour) and borderless fullscreen at desktop resolution.
- A control for that setting in the in-game Options menu.
- Saving and loading the setting through Options.ini.
- Clean Alt-Tab behaviour in borderless mode.

**Out of scope for v1**

- Switching mode live, without a restart.
- A bordered, resizable windowed mode.
- Multiplayer. The only multiplayer-related task here is to record any findings about the `NetworkLayerDLL` slot (see below).

## What we already know

These are community reports, not verified findings. Phase 0 confirms or corrects each one.

- **No windowed mode exists.** Forum regulars say SF2 doesn't support one. Setting `windowmode = TRUE` in the main ini reportedly does nothing. ([CombatAce](https://combatace.com/forums/topic/70145-windowed-mode-or-multimonitor), [CombatAce](https://combatace.com/forums/topic/53150-strike-fighter-2-wont-run-in-windowed-mode))
- **Subsystems load from DLLs named in the main ini.** The main ini has entries such as `MessageSystemDLL`, `SpeechSystemDLL`, `FaderDLL`, and an empty `NetworkLayerDLL=`. This may be a cleaner entry point than proxy-DLL injection. ([CombatAce](https://combatace.com/forums/topic/53150-strike-fighter-2-wont-run-in-windowed-mode))
- **Menus are data-driven.** The main ini points to `MenuSystem.ini`, `MenuData.cat` and `MenuText.cat`, and modders edit menu screens regularly.
- **User options live in** `Saved Games\ThirdWire\StrikeFighters2\Options.ini`, under `[GraphicsOptions]` (`DisplayDeviceID`, `DisplayWidth`, `DisplayHeight`, `DisplayDepth`, `AspectRatio`). Mod folders may have their own copy. ([CombatAce](https://combatace.com/forums/topic/70145-windowed-mode-or-multimonitor))
- **Many players run DXVK** (x86 DLLs dropped into the game folder) to fix crashes and missing textures. That suggests a 32-bit D3D9-era renderer, but confirm this from the imports. ([CombatAce](https://combatace.com/forums/topic/96953-usmc%C2%A0usa-nato-and-israel-m109a2-howitzer))
- **Multiplayer was dropped because** SF2 moved to DirectX 10, which didn't support the DirectPlay library that SFP1 and the Wings Over series used. ([CombatAce](https://combatace.com/topic/89699-anyone-interested-in-an-sfp1wox-series-dogfight-server))
- **The binary is stable.** July 2013 is the final patch, so addresses and hooks won't move under us.

## Approach

Work in five phases. Each phase ends at a checkpoint: stop there, report back, and wait for review before starting the next.

1. **Recon.** Inventory the executable and DLLs: architecture, imports, exports, and which D3D version is actually used. Use Process Monitor to record which ini, cat and DLL files are read at startup, and in what order. Work out how the ini-named subsystem DLLs are loaded (LoadLibrary path, expected exports, call sequence). Capture what `NetworkLayerDLL` would need to export. Also find out whether the exe still calls through that slot into a network-layer interface, and whether code that serialises or replicates game state survives behind it (dead or reachable). Record what you find, but build nothing for multiplayer. *Checkpoint:* a recon notes file.
2. **Borderless spike.** Prove borderless can be forced at all, with no menu work. Pick the injection route from the recon: an ini subsystem slot if one is usable, otherwise a proxy DLL. Intercept window creation and device creation. Create the device windowed at desktop resolution, in a borderless popup window. *Checkpoint:* the game starts borderless and is flyable.
3. **Trace an existing option.** Pick one existing graphics setting in the Options menu. Trace it end to end: the menu data entry, then the code that handles the control, then the Options.ini key, then where the engine reads it at startup. Document the menu file format as far as that path needs. *Checkpoint:* a written trace that the next phase can copy.
4. **Menu option.** Add a display mode control by following the traced path. Persist it as a new key in `[GraphicsOptions]`. The DLL reads the key at startup and applies the matching mode. If the key is missing or invalid, fall back to stock exclusive fullscreen. *Checkpoint:* the mode is chosen in-game and takes effect after a restart.
5. **Hardening.** Alt-Tab and focus loss, D3D device lost/reset, multi-monitor setups (stay on the configured display), and running with and without DXVK. Write install and uninstall notes.

## Ground rules

- **Never modify the original install.** Work on a full copy of the game folder and a separate mod folder. Never patch the exe on disk. Every change must be undone by removing the files we added.
- **No game files in the repo.** Commit only our own source, notes and tooling. Don't commit or redistribute ThirdWire binaries, cat contents or extracted assets. Short excerpts of disassembly inside notes are fine.
- **Stay compatible with DXVK.** Don't assume we own `d3d9.dll`. If we need a proxy, either chain to whatever d3d9.dll is present or hook at a different layer.
- **Write findings down as you go.** Keep a living recon notes file with addresses (relative to module base), function roles, file formats and open questions. The multiplayer work will start from these notes.
- **Stop at each checkpoint.** If something contradicts this brief, stop and report it instead of working around it.

## Deliverables and acceptance

**Deliverables**

- A git repo with the DLL source and a one-command x86 build.
- `recon-notes.md`: the living findings file (now in the private `lazy-sf2-recon` repo).
- `option-trace.md`: the Phase 3 end-to-end trace (also in `lazy-sf2-recon`).
- The menu data changes, packaged as a normal SF2 mod-folder drop-in.
- `INSTALL.md`: install, uninstall, and how to tell it's working.

**Done when**

- [ ] Display mode can be selected in the Options menu, and the choice survives a restart.
- [ ] Borderless runs at desktop resolution on the configured display, with no border or title bar.
- [ ] 10 Alt-Tab round trips during a mission, with no crash, black screen or lost input.
- [ ] A 30-minute mission in borderless mode is stable.
- [ ] Exclusive fullscreen still behaves exactly as stock.
- [ ] All of the above pass with DXVK installed and without it.
- [ ] Removing our files returns the game to stock.

## Open questions

Settle these with me before Phase 0 starts.

- [ ] Which install is the target: stock SF2, or a merged install (SF2V, SF2E, SF2I, SF2NA)? Which mod folder?
- [ ] Is DXVK in the test install right now?
- [ ] Language and hooking library: C++ with MinHook or Detours, or something else?
- [ ] Windows version on the test machine, and is there a second monitor for multi-monitor testing?
- [ ] Has ThirdWire's EULA been checked for reverse-engineering terms before anything is published?

## Tools to install

Install these on the Windows machine where SF2 is installed. Claude Code needs to run there, because every phase needs the live game.

| Tool | Used for | Needed |
| --- | --- | --- |
| Git | Repo and history | Required |
| Visual Studio 2022 Build Tools (Desktop C++, x86 toolset, Windows SDK) | Building the 32-bit DLL; includes `dumpbin` for imports and exports | Required |
| CMake | One-command build | Required |
| vcpkg | Pulling in the hooking library | Required |
| MinHook or Microsoft Detours (via vcpkg) | Function hooking | Required (choose one) |
| Ghidra + JDK 21 | Static analysis; `analyzeHeadless` lets Claude Code script it | Required |
| x64dbg (use x32dbg) | Live debugging of the 32-bit process | Required |
| Sysinternals Process Monitor | Recording which files and registry keys are read at startup | Required |
| Python 3 + `pefile` | Scripted PE inspection and small helper tools | Required |
| A ThirdWire `.cat` unpacker (community tool, or one Claude Code writes) | Reading `MenuData.cat` and `MenuText.cat` | Required for Phase 3 |
| Dependencies (lucasg) | Viewing the DLL load tree in a GUI | Optional |
| API Monitor (rohitab) | Watching Win32 and D3D calls without writing hooks | Optional |
| apitrace | Tracing D3D9 calls (RenderDoc doesn't support D3D9) | Optional |
| ImHex | Hex editing and pattern-based format notes | Optional |
| GhidraMCP | Lets Claude Code query Ghidra directly | Optional |

DXVK is only needed for the compatibility tests in Phase 5.
