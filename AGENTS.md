# AGENTS.md

These are instructions for AI coding agents, and for humans, working on this repository. Read this file before changing anything.

This is the repo's **only** instructions file. Don't add a `CLAUDE.md` or tool-specific copies. Claude Code reads `AGENTS.md` natively, but only when no `CLAUDE.md` exists. Put new conventions here.

## What this is

This repo holds runtime mods for Strike Fighters 2, the July 2013 final patch: a 32-bit x86 D3D10 game with a D3D9 fallback. The mod is a `winmm.dll` proxy that hooks the game with MinHook. The briefs in `docs/` set the scope and the phases. `docs/recon-notes.md` is the living findings file; read the section that concerns you before starting.

## Ground rules (non-negotiable)

1. **Never put game files in the repo.** That includes ThirdWire binaries, `.cat` contents, extracted inis, STR files, textures, the EULA text and decompiled game code.
   - `.gitignore` covers the common cases, but check `git status` before every commit.
   - Notes describe behaviour and give RVAs. Paraphrase instead of pasting decompiled code (EULA 3(d)).
2. **Never patch the game on disk.** No byte patches to the exe or DLLs, and no edits to game ini or cat files that you leave behind.
   - The mod may only add its own files to the game folder, and must list them in `lazysf2.manifest.txt` (see `scripts/install.ps1`). Running `scripts/uninstall.ps1` must return the folder to stock.
   - Temporary experiments on a game file must be restored from `G:\SF2-backup`, then hash-checked.
3. **Test against the original install. Don't run a copy of the game.** EULA 3(b) allows copies only as backups. The backups in `G:\SF2-backup\` exist only to restore files.
4. **Stay compatible with DXVK.** Don't assume we own `d3d9.dll`, `dxgi.dll` or the `d3d10*.dll` files. Hook the COM objects the game actually gets, rather than replacing system DLLs.
5. **Stop at each phase checkpoint** listed in the brief, report, and wait for review. If something contradicts the brief, stop and say so; don't work around it.
6. **Write findings down as you go** in `docs/recon-notes.md`: addresses as RVAs (offsets from the module base), function roles, file formats and open questions.
7. **Privacy.** Process Monitor captures and logs contain the machine's hostname and user paths. Keep them in `G:\SF2-recon`, and keep them out of commits.

## Fixed-address hooks

The July 2013 binaries never change, so hooking by RVA is acceptable. But **every RVA hook must first check the module's PE `TimeDateStamp` and `SizeOfImage`**, and skip with a log line if they don't match. See `is_expected_build` in `src/hooks/mouse_hooks.cpp`.

The game DLLs are linked at `0x10000000`, and the exe at `0x00400000`.

## Build and install

```powershell
.\build.ps1                    # enters the VS dev shell (x86) and runs the CMake preset; output build\Release\winmm.dll
.\scripts\install.ps1 [-MouseMode Cursor|Stock]
.\scripts\uninstall.ps1
```

- **Toolchain:** VS 2026 (v18), generator `"Visual Studio 18 2026" -A Win32`, triplet `x86-windows-static`, with a static CRT (`CMAKE_MSVC_RUNTIME_LIBRARY`).
- **vcpkg:** use the copy bundled with VS. It requires `builtin-baseline` in `vcpkg.json`.
- **MSVC `C1083 Cannot open compiler generated file: ''`** is the long-path limit. Keep build directories short.
- **Runtime log:** `lazysf2.log` in the game folder. Use it to check that hooks were installed and what they did.
- **`LAZYSF2_DISABLE=1`** runs the mod as a pure pass-through proxy.

## Reverse-engineering tools

| Tool | Use |
|---|---|
| Ghidra 12.2 (`G:\DevTools\ghidra_12.2_PUBLIC`, needs JDK 25) | Analysed project at `G:\GhidraProjects\SF2`: the exe at the root, then `/Core`, `/Flight`, `/Menu`, `/Objects`. |
| `tools/ghidra/Invoke-Ghidra.ps1` | Runs a script from `tools/ghidra/` read-only, for example `-Folder /Core -Program MenuSystem.dll -Script StringXrefs.java -ScriptArgs foo`. Only one run at a time, because the project locks. |
| `tools/ghidra/*.java` | Cross-references from strings, wide tags, imports and addresses; vtable dump; decompile everything. Write output to `G:\SF2-recon\decomp`. |
| `tools/catx.py` | Lists and extracts `.cat` archives. It refuses to extract into the repo. |
| `tools/recon/` | Inventory of PE imports and exports, a Process Monitor filter generator, and a capture summariser. |
| `tools/gen_winmm_proxy.py` | Regenerates `src/proxy/winmm.def` and `winmm_exports.inc` from the system `winmm.dll`. |

**Gotcha:** Ghidra's `.bat` breaks on `(x86)` in a path, so import the game through the junction `G:\SF2-game`.

## Code conventions

- C++20, `/W4 /permissive-`, everything in namespace `lazysf2`. The build must have no warnings.
- Each feature is its own `src/hooks/*.cpp`. Features that need modules loaded later register with the module watch (`module_watch.cpp`) instead of hooking `LoadLibrary` again.
- Settings go in `src/config.*`. Defaults must give stock behaviour, unless the brief says a feature is always on.
- Log every hook install and every behaviour change once, with `LOG(...)`. Don't log every frame.

## Workflow

- **GitHub flow, without pull requests.** Do each piece of work on its own branch off `master`, and commit there. **Never commit to `master` directly.** Merge into `master` and push it **only when the user explicitly says so**. Don't open pull requests.
- Delete branches after they are merged.
- End commit messages with the `Co-Authored-By` lines given in your session instructions, if any.
- Testing needs the live game. Get the user to drive it, ask specific questions, and read `lazysf2.log` yourself.
- The user often works over RustDesk. Judge mouse behaviour at the machine, or with Cursor mode on.
