# SF2 recon notes

This is the living findings file for the borderless fullscreen work and the later network layer work.

- **Target:** `StrikeFighters2.exe`, the July 2013 final patch, in a merged install (base game plus Vietnam, Europe, Israel, Exp1, Exp2 and North Atlantic).
- **Addresses:** all addresses are RVAs, that is, offsets from the module base. The exe base is `0x00400000`. Every game DLL is linked at `0x10000000`.
- **Analysis tools:** Ghidra 12.2 run in headless mode with the scripts in `tools/ghidra/`, `pefile` via `tools/recon/pe_inventory.py`, and a Process Monitor capture of one stock startup summarised with `tools/recon/pm_summary.py`.

> EULA note: these notes describe behaviour and give addresses. They do not reproduce game code. See EULA 3(d).

Status: **Phase 0 checkpoint (borderless scope).** Network items are recorded where they came up, but they are not fully followed up.

---

## 1. Corrections to the brief

| Brief said | What recon shows |
|---|---|
| The renderer is probably D3D9-era, and DXVK means `d3d9.dll`. | The main renderer is **D3D10** (`D3D10CreateDevice` with a DXGI swap chain). D3D9 is only a fallback, used when `ForceDX9=TRUE`. With DXVK, the relevant DLLs would be DXVK's `d3d10core.dll`, `d3d11.dll` and `dxgi.dll`, not just `d3d9.dll`. |
| No windowed mode exists, and `WindowMode` does nothing. | It does nothing, but a **windowed code path still exists** in the exe. The exe never reads the `WindowMode` key; it forces the window-mode flag to 0 at startup (§4.1). |
| The exe loads the ini-named subsystem DLLs, including `NetworkLayerDLL`. | The exe loads all the others. **MissionManager.dll** loads the network layer (§6). |
| Use JDK 21 for Ghidra. | Ghidra 12.2 needs **JDK 25**. |

## 2. Module inventory

- There are 72 PE files, and **all of them are 32-bit x86** (machine `0x14c`). The exe and the Core DLLs were built in early July 2013. `DisplayManagerDX10.dll` and `GraphicsEngineDX10.dll` were rebuilt on 2013-07-07.
- **Exe imports:** only system DLLs (KERNEL32, USER32, GDI32, ADVAPI32, SHELL32, ole32, WINMM and WINHTTP). There are no D3D imports; all rendering is in the DLLs.
- **Folders:**
  - `Core\`: 19 subsystems. They all export the same two functions, `InitDLL` and `ExitDLL`.
  - `Menu\`: 23 screens, exporting `InitMenuCode` and `ExitMenuCode`.
  - `Objects\`: 13 object types, exporting `InitObjectDLL` and `ExitObjectDLL`.
  - `Flight\`: 10 modules (avionics, HUD and mission types), exporting `InitDLL` and `ExitDLL`.
- **DirectX imports by module:**
  - `GraphicsEngineDX10.dll`: `d3d10`, `d3d10_1`, `dxgi`, `d3d9`, `d3dx10_43` and `d3dx9_43`.
  - `DisplayManagerDX10.dll`: `dxgi` and `d3d9`. It enumerates adapters and modes.
  - `DirectInput.dll`: `dinput8`.
  - `DirectSound.dll`: `dsound`.
- **The CRT is linked statically everywhere.** There are no MSVC runtime DLL dependencies.

## 3. Startup sequence (Process Monitor, stock run)

| Step | What happens |
|---|---|
| 1 | Reads `StrikeFighters2.ini` (UTF-16 LE), then `Menu\Startup.cat`. |
| 2 | Reads `Saved Games\ThirdWire\StrikeFighters2\Options.ini` and `Version.ini`. |
| 3 | Loads `Core\AddonManager.dll`, which handles DLC and the version check. It checks for each expansion exe in the game folder, including a non-existent `Exp3`. |
| 4 | Loads the Core DLLs in this order: DirectInput, DirectSound, TextUSEnglish, ControlMap, ObjectManager, GraphicsEngineDX10, TerrainEngine, Fader. GraphicsEngine pulls in d3dx10_43, d3dx9_43 and d3d9 at load. |
| 5 | Opens `Objects\ObjectData027.cat` down to `001.cat` (newest first), then `Flight\MissionData` and `MissionText` `007` down to `001`. |
| 6 | Loads the object DLLs, and for each one probes `Saved Games\...\Objects\<name>.ini` first. **The mod folder overrides the game folder.** |
| 7 | Loads MissionManager, then **DisplayManagerDX10**, which pulls in dxgi. |
| 8 | Loads MenuSystem, and then the D3D10 device is created: d3d10_1, d3d10_1core, d3d11, d3d10, d3d10core and D3DCompiler_43 are loaded. |
| 9 | Opens the terrain cats, `Menu\MenuData.cat` and `MenuText.cat`, then probes `Saved Games\...\Menu\MenuSystem.ini`, an override location for the menu. Loads `Menu\MainScreen.dll` and `Flight\InstantAction.dll`. |
| 10 | Makes outbound HTTP (port 80) connections to an AWS-hosted server: one at startup and two at the main menu. These are probably AddonManager's DLC and version checks; AddonManager contains `www.thirdwire.net`, `SF2/VERSION.TXT` and `SF2/NUM_DLC.TXT`. Not confirmed by matching the host name. |

Other things seen in the capture:
- On this machine, the opentrack `NPClient.dll` (TrackIR emulation) is loaded by DirectInput.
- The exe probes for `oculusmedium.exe` and checks the path of the default browser.
- **Registry:** only the standard Direct3D driver keys. The game keeps no ThirdWire registry state at runtime; the install path is under `HKLM\...\WOW6432Node\ThirdWire\Strike Fighters 2`.

## 4. Window and display path (borderless scope)

### 4.1 Window creation: exe, function at `+0x1210`

- The window class is `twWindowClass`, registered at `+0x1180`. The title is "Strike Fighters 2". The HWND is stored in global `+0x49cb4`.
- **The branch depends on `GetWindowMode()`**, through the APP service (see §5): wrapper `+0xf730`, vtable `+0x38`.
  - **Mode 0 (stock):** `CreateWindowExW` with ex-style `WS_EX_TOPMOST` and style `WS_POPUP|WS_VISIBLE`. The rectangle covers **the whole monitor that contains the `[Window] WindowPosX/Y/Width/Height` rectangle**, found with `MonitorFromRect` (nearest monitor) and `GetMonitorInfoW`. If that fails, it falls back to the primary monitor's size from `GetSystemMetrics`. After that it calls APP vtable `+0x58` with 0 (`+0xf850`).
  - **Mode not 0 (dead path):** `AdjustWindowRect` with `WS_OVERLAPPEDWINDOW`, then `CreateWindowExW` with style `WS_POPUP|WS_VISIBLE|WS_OVERLAPPEDWINDOW` at `WindowPosX/Y` and `WindowWidth/Height`.
- **The stock window is already a borderless, monitor-sized popup.** Exclusive fullscreen comes only from DXGI (§4.3).
- **Monitor choice:** `[Window] WindowPosX/Y` in the main ini decides which monitor the stock window covers. This matters for multi-monitor support (Phase 5).
- At `+0x14b0`, the exe reads `[Debug] OutputLevel` and `SkipMenu` and `[Window] WindowPosX/Y`, `WindowWidth` and `WindowHeight`. Between those reads, it calls **`SetWindowMode(0)` with a constant 0** (`+0xf740`, APP vtable `+0x34`). The string `WindowMode` does not occur anywhere in the exe.
- **Other window calls:**
  - `+0x5790` and `+0x5830` call `SetWindowPos(hwnd, HWND_TOPMOST, …, SWP_SHOWWINDOW|SWP_FRAMECHANGED)`.
  - `+0x5800` minimises the window (`SW_SHOWMINNOACTIVE`).
  - `+0x5c80` restores and activates it (`SW_RESTORE`, then `SW_MAXIMIZE` or `SW_SHOWNORMAL`). This is probably the focus-regain path.
  - `+0x5420` calls `EnumDisplayMonitors`.

### 4.2 Display settings block: GraphicsEngineDX10.dll

- The graphics init at `+0x10950` logs "Initializing Graphics Engine in %d x %d x %d mode...". It copies a **block of 15 DWORDs** into globals at `+0x789d8`, and a second block of 8 DWORDs into `+0x789a8`.

| Global | Field | Notes |
|---|---|---|
| `+0x789d8` | adapter / display device | |
| `+0x789dc` | width | Also written at `+0x1ecd0` and at `+0x1f220`, the mode handling. |
| `+0x789e0` | height | |
| `+0x789e8` | **fullscreen flag** | Not 0 means call `_DoFullScreen`. It also runs two cursor helpers (`+0x39300`, `+0x39320`, `+0x393f0`), probably cursor hide and clip. |
| `+0x78a10` | MSAA sample count | Limited to at most 4. |
| `+0x789c0` | a separate flag that affects the MSAA path | Meaning unknown. |

- **Open question:** where the block's fullscreen flag comes from. The exe's `[GraphicsOptions]` handler (`+0x3af0`) reads and writes `DisplayDeviceID`, `DisplayWidth`, `DisplayHeight`, `DisplayDepth`, `AspectRatio`, `AntiAliasing`, `ForceDX9`, `ForceVSyncOff` and `Use32BitZBuffer`. The block is probably filled from these plus the APP window-mode flag, but that hasn't been traced yet. It matters for Phase 3.

### 4.3 Device and swap chain: GraphicsEngineDX10.dll

- **Device init (`+0x28180`):**
  1. `CreateDXGIFactory`, then picks the adapter, then `IDXGIAdapter::GetDesc`.
  2. `D3D10CreateDevice(adapter, HARDWARE, …, SDK 0x1d)`.
  3. `EnumOutputs(0)` and `FindClosestMatchingMode` for width × height.
  4. Builds a `DXGI_SWAP_CHAIN_DESC`. The important fields are **`Windowed = TRUE` (always)**, `OutputWindow` = the exe's HWND (passed in), 2 buffers, and `Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH`.
  5. Calls `IDXGIFactory::CreateSwapChain` (vtable `+0x28`).
  6. **Only if the fullscreen flag is set,** calls `_DoFullScreen`.
- **`_DoFullScreen` (`+0x228d0`):** `ResizeTarget(mode)`, then **`SetFullscreenState(TRUE, NULL)`**, then `ResizeTarget` again with the refresh rate set to 0, then `ResizeBuffers(0, 0, 0, 0, ALLOW_MODE_SWITCH)`.
- **Lost-device handler (`+0x1f9c0`):** releases the render-target view, then calls **`SetFullscreenState(FALSE)`**.
- The back buffer is fetched at `+0x229a0`.
- **No `MakeWindowAssociation` call was seen**, so DXGI's default Alt+Enter handling is presumably still active. To verify.

### 4.4 Implication for the borderless spike (Phase 2)

Borderless means keeping the stock popup window and never entering exclusive mode:

1. **Hook DXGI at the COM layer.** After `CreateDXGIFactory`, patch the factory's `CreateSwapChain`. Then on the swap chain:
   - `SetFullscreenState(TRUE)` becomes a no-op that returns `S_OK`;
   - `ResizeTarget` becomes a no-op;
   - `GetFullscreenState` reports what the game expects.

   Also call `MakeWindowAssociation(hwnd, NO_ALT_ENTER | NO_WINDOW_CHANGES)`.

   This hooks whatever `dxgi.dll` is loaded, so it **works the same with DXVK**.
2. **Force the back buffer to the monitor's resolution,** or require `DisplayWidth/Height` to equal the desktop resolution.
3. **Remove `WS_EX_TOPMOST`** after creation (`SetWindowPos(HWND_NOTOPMOST)`), and neutralise the exe's own `HWND_TOPMOST` calls at `+0x5790` and `+0x5830`, so Alt-Tab works.
4. **An alternative to (1):** clear the fullscreen flag at `+0x789e8` before the device init. It's simpler, but it skips the cursor helpers and needs the Phase 3 trace to do cleanly.
5. **`ForceDX9=TRUE` (the D3D9 path) is out of scope for v1.** In borderless mode, either leave it stock or refuse it.

## 5. Engine architecture: subsystems and services

- **Loader in the exe:**
  - `+0xa1f0`: `GetProcAddress(h, "InitDLL")`, then calls **`InitDLL(HMODULE self, ISystem* sys)`**. `sys` is global `+0x49cc0`.
  - `+0xa220` and `+0xa310`: `ExitDLL()` with no arguments, then `FreeLibrary`.
- **`[System]` DLL names** are read at `+0x1950` (`init_system`, which logs "init_system: lpCmdLine=%s, InitDir=%s, SavedGameDir=%s"). Each name defaults to the stock DLL.
- **Service registry:** `sys` vtable `+0x8` is `Get(const wchar_t* tag, void** out)`, and `+0xc` is `Release(tag, &p)`. **Tags are short UTF-16 strings, not GUIDs.**
  - **Core tags:** `APP`, `DLL`, `MEM`, `DBG`, `FMG`, `AM`, `PM`, `MTD`, `RND`, `INI`, `OPT` (options), `MSN`, `MMG` (MissionManager), `OBJ`, `TXT`, `GFX`, `DSP` (display), `SND`, `TER`, `WLD`, `MNU`, `ENV`, `CTL`, `INP`, `FLT`, `CLK`, `VMG` (view), `HUD`, `AVN`, `MSG`, `VER`, `DLC`, `ADD`.
  - **`NET`** is looked up by FlightEngine and the Aircraft, Missile, Ground, GroundMobile and Naval object DLLs (see §6).
- **Other core globals in the exe:**
  - `+0x49cbc` is the APP service, with:
    - vtable `+0x14`: install directory;
    - `+0x18`: saved-games directory;
    - `+0x34`: `SetWindowMode`;
    - `+0x38`: `GetWindowMode`;
    - `+0x58`: unknown, called with 0 after the fullscreen window is created.
  - `+0x49ccc` is a file or cat service; vtable `+0x8` opens a `.cat`.
- **Ini parser:** custom and UTF-16 aware. It doesn't use `GetPrivateProfile*`, and supports an `include` directive (seen in AddonManager's copy of the parser).

## 6. Network layer (for the network brief; recorded only)

- The exe reads `[System] NetworkLayerDLL` (default empty) and passes it to **MissionManager** through the `MMG` service: vtable `+0x2c` is `SetNetworkLayerDLL(name)`.
- **MissionManager's interface vtable** is at `+0x1b2c4`. Four slots relate to the network layer:
  - slot 11, `+0x2c`: store the name (`this+0x24`);
  - slot 12, `+0x30`: return the handle (`this+0x28`);
  - slot 13, `+0x34`: **load**. If the name is empty it returns 0; otherwise it loads the DLL through the `DLL` service (`+0x8`) and logs "Unable to load NetworkLayer DLL %s!" on failure;
  - slot 14, `+0x38`: unload.
- **Callers of slots 12 and 13 are not found yet.** The `MMG` users are MissionManager, Campaign, InstantAction, SingleMission and 11 menu screens.
- **The `NET` service is looked up by FlightEngine and five object DLLs** (Aircraft, Missile, Ground, GroundMobile and Naval), which are exactly the types that would be replicated. The exe contains the tag too. **This is the strongest lead for surviving replication hooks.** Next step: find what those modules call on `NET`, using `WideTagXrefs.java NET`.
- The expected export of a network-layer DLL is probably `InitDLL` and `ExitDLL` like every other subsystem, with the DLL registering `NET` from `InitDLL`. To confirm.
- No replay or recording strings and no dedicated-server strings were found in the Core DLLs; the search covered replay, playback, dedicated, lobby and multiplayer. Flight, Menu and Objects have not been searched yet.

## 7. Misc

- **`[Debug] SkipMenu`:** read into exe global `+0x492a4`, but no direct read of that global follows. Tested with `SkipMenu=1`: **no visible effect**. It may need a command-line switch, or it may be dead. It's worth tracing later as a quick way into a flight; the CRT start-up calls `GetCommandLineW` at `+0x1a2b6` and passes it to `init_system`.
- `Mods` and `ModsEnabled` (`+0x1950` and `+0x3af0`) and a `mods` path string exist, so there may be built-in mod-folder support. Not traced yet.
- The menu override is probed at `Saved Games\...\Menu\MenuSystem.ini`, and the menu-screen inis are probed in the game root. This is where Phase 4's drop-in will go.

## 8. Injection route (decision for Phase 2)

| Route | Pros | Cons |
|---|---|---|
| **`winmm.dll` proxy in the game folder** (recommended) | It isn't a KnownDLL, and the exe imports only 4 functions from it. Removing the file undoes it. It doesn't touch `d3d9.dll` or `dxgi.dll`, so it's DXVK-safe. It loads before any game DLL. | All winmm exports must be forwarded, because system DLLs such as dsound will bind to the same module. |
| Ini slot (for example `FaderDLL=` pointing to our DLL, which chains to `Fader.dll`) | Uses the engine's own `InitDLL(sys)`, so we get the service registry for free. | Needs a one-line edit to the main ini in the original install, so "remove our files to undo" no longer holds. Loads later than winmm. |
| `NetworkLayerDLL=` slot | Semantically fitting. | When MissionManager calls `Load` is unknown, possibly never. Same ini-edit problem. |

**Recommendation:** use the `winmm.dll` proxy for Phase 2. It can still fetch `ISystem` later by hooking the exe's loader at `+0xa1f0` if we need the services.

## 9. Phase 2 spike results (2026-11-24)

The implementation is in `src/`. It's a `winmm.dll` proxy (193/193 exports forwarded) plus MinHook hooks on:
- `CreateWindowExW` and `SetWindowPos` (removing topmost);
- `CreateDXGIFactory` and `IDXGIFactory::CreateSwapChain`;
- `IDXGISwapChain::SetFullscreenState` and `ResizeTarget` (both suppressed).

**What the log showed on the test machine** (2560×1440, three monitors, system DXGI, no DXVK):
- **The window** was created stock (`WS_EX_TOPMOST`, `WS_POPUP|WS_VISIBLE`, 0,0 2560×1440), and topmost was removed. The live window had style `0x95000000`, ex-style `0`, and covered the monitor exactly.
- **dxgi.dll** loads around 5 s after start. `CreateDXGIFactory` is called 3 times: DisplayManager twice, then GraphicsEngine.
- **The swap chain** was 2560×1440, format 28 (`R8G8B8A8_UNORM`), 60 Hz, MSAA 1, 2 buffers, `Windowed=1`, flags `ALLOW_MODE_SWITCH`. `MakeWindowAssociation(NO_ALT_ENTER|NO_WINDOW_CHANGES)` returned `S_OK`.
- **`_DoFullScreen` runs at startup and again on regaining focus,** both times as `ResizeTarget`, `SetFullscreenState(TRUE)`, `ResizeTarget`. The exe re-asserts `HWND_TOPMOST` on focus changes and at exit, and each time it was rewritten to `NOTOPMOST`.

**User checks:**
- In the menus it looks borderless, and Alt-Tab between the game and the desktop works. In stock mode Alt-Tab from the menu didn't work at all.
- **Flying is not tested yet.**

**Mouse over RustDesk:** the game's cursor moves erratically. **This happens in stock too, so the mod doesn't cause it.** Nothing in the game calls `SetCursorPos` or `ClipCursor`; it reads relative mouse input through DirectInput (`Core\DirectInput.dll`) and draws its own cursor. Remote-control tools inject absolute mouse positions, which DirectInput's relative mouse handles badly. For mod testing, judge mouse behaviour only at the machine.

**Uninstall:** `scripts/uninstall.ps1` removed the 3 added files, and the game folder matched the pre-install snapshot exactly.

## 10. Mouse input (DirectInput.dll) and Cursor mode

The game's input manager keeps a mouse-state block at `this+0x118`, laid out as `int` fields:

| Field | Content |
|---|---|
| `[0]` | `IDirectInputDevice8*` |
| `[4..7]` | cursor range: left, top, right and bottom, inclusive |
| `[8]`, `[9]` | x, y |
| `[10]` | wheel |
| `[0x11]` | movement threshold before a "moved" event is sent (10) |
| `[0x12]` | "position was set" flag |

**Functions** (DirectInput.dll RVAs):
- `+0x5580` is setup. It does `CreateDevice(GUID_SysMouse)`, `SetDataFormat(c_dfDIMouse)`, then **`SetCooperativeLevel(hwnd, EXCLUSIVE|FOREGROUND)`** (flags `0x5`) and a buffer size of 256. The range starts as the primary screen size, and the cursor starts at the centre (`+0x5390`).
- `+0x51b0` is the per-frame read. It calls `GetDeviceData`, then adds the `DIMOFS_X/Y/Z` deltas to `[8]`, `[9]` and `[10]`. It turns the button records into button events, clamps x and y to the range, then sends a "moved" event for each axis through `+0x50f0`. The event carries no coordinate; consumers ask for the position.
- `+0x2840` (manager) is **SetRange**. **The menu system sets the range to (0,0)–(1007,751)**: its 1024×768 virtual space (`MenuSystem.ini` `Width/Height`) minus the 16-pixel cursor image.
- Accessors: GetPos (`+0x4ed0`, raw `[8]`/`[9]`), SetPos (`+0x4ef0`), the centre of the range (`+0x4f20`), Recenter (`+0x5180`), GetAxis(i) (`+0x4f50`) and GetButton(i) (`+0x4f80`).
- The game creates **two** system-mouse devices. Only the first one is read.

**Cursor mode** (`lazysf2.ini` `[Mouse] Mode=Cursor`) is for remote-control tools that inject absolute mouse positions. DirectInput turns those into bogus relative movement, so the stock cursor drifts. That happens in stock too, without the mod.
- The mouse is made **non-exclusive** (`0x5` → `0x6`), and its X/Y records are removed from `GetDeviceData`.
- Before each `+0x51b0` read, `[8]` and `[9]` are set to the Windows cursor's client position scaled to 1024×768. The game's clamp then applies as usual.
- The game window is subclassed so `WM_SETCURSOR` hides the Windows cursor over the client area.
- The read hook is installed only when DirectInput.dll's timestamp is `0x51d21fcc` and its image size is `0x30000`.
- **Checked over RustDesk:** the cursor lines up across the screen, and clicks land on the right items.
- **Not checked yet:** mouse use in flight, where the range may differ.

## 11. Open questions

1. Where does the display block's fullscreen flag (`+0x789e8`) come from? This is the Phase 3 trace.
2. Does `MakeWindowAssociation` get called anywhere, for example in DisplayManagerDX10? Check DisplayManager's vtable calls on the factory.
3. What does APP vtable `+0x58` do after window creation? Possibly cursor confinement.
4. Who calls MissionManager slot 13 (load the network layer), and what is called on the `NET` service?
5. Do `SkipMenu` and `Mods`/`ModsEnabled` have command-line counterparts?
