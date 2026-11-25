# SF2 Quality-of-Life Follow-up — Claude Code Brief

Nov 24, 2026

## Goal and context

Ship a series of small quality-of-life improvements to SF2. Each one is released to CombatAce on its own. They come before the multiplayer work and the dynamic campaign link, for two reasons: each win builds goodwill, and each one teaches us more of the renderer and the mission flow.

**Already shipped from the borderless brief**

- Borderless fullscreen, mouse capture mode and display selection, all as native in-game Options menu items.
- Absolute Windows cursor in menus; DirectInput deltas still drive the mouse stick in flight.
- Alt-Tab, device reset and DXVK compatibility.
- An installer that applies and updates the mod.

**Priority order for this brief**

1. Smart scaling for distant aircraft.
2. Z-fighting and depth precision investigation.
3. Per-mission start position, with EOR starts.
4. Clickable cockpits (stretch goal).

Use the same phase-and-checkpoint pattern as before: stop after each item, report, and wait for review.

## 1. Smart scaling

Distant aircraft stay invisible until you zoom in. That suggests the engine culls objects by projected screen size, not only by distance. The fix is smart scaling, as in DCS and BMS: past a set distance, keep each aircraft at least a few pixels wide, then fade the extra scaling out as it gets closer.

**Steps**

1. Confirm how culling works. Check the LOD distance entries in the aircraft data files first; modders often tune these. Then find the cull test in code, and check whether it uses distance, screen size, or both.
2. Hook the cull and LOD selection so distant aircraft are never culled inside a configurable range.
3. Scale distant aircraft up to a minimum on-screen size, and blend the scale back to 1.0 as they close in.
4. Add Options menu controls (on/off and strength), following the pattern from the borderless work.

**Constraints**

- Cap the boost, so an aircraft at 40 nm never looks larger or easier to spot than it should.
- Scale aircraft only at first. Ground objects can come later if it's worth it.
- In multiplayer this changes who spots whom, so the setting must be one the host can enforce for everyone. Design it as a single value that the network layer can later sync.

*Checkpoint:* side-by-side screenshots at 5, 10 and 20 nm, with the feature on and off.

## 2. Z-fighting and depth precision

There's visible Z-fighting in some scenes. The working theory: the depth buffer, not the 32-bit world coordinates, causes it. A huge view distance with a near plane close to the eye spreads depth precision too thin. This item is investigation first; fix only once the cause is confirmed.

**Steps**

1. Capture a frame in RenderDoc (SF2 is DX10, so captures should work). Record the near and far planes, the depth format, and the draw order.
2. Check whether the cockpit is a separate pass. Look for a `ClearDepthStencilView` between the world draws and the cockpit draws.
3. Depending on what the capture shows, try fixes cheapest first:
   - Push the world pass's near plane out, if the cockpit has its own pass.
   - Split the world into near and far depth ranges.
   - Reversed-Z with a floating-point depth buffer.
4. Run the float jitter test. Fly to the far corner of the largest theatre and hold close formation with a wingman. If the cockpit is a separate pass, it won't show jitter, so watch the wingman instead. Note whether SF2 appears to render relative to the camera.

**Constraints**

- Any depth change must not break the cockpit, HUD, or effects such as smoke and clouds.
- Test with and without DXVK.

*Checkpoint:* a short findings note with the capture details, the confirmed cause, and before/after screenshots of a Z-fighting location.

## 3. Per-mission start position and EOR starts

The start position is currently a global game setting, and the runway start puts you on the threshold. Start position is a decision made per flight, so it belongs on the mission screen. This item also adds an end-of-runway (EOR) start: parked in the arming area just before the hold-short line, where aircraft wait for clearance.

**Steps**

1. Trace the existing global start setting, from the Options menu through Options.ini to where mission start reads it. Reuse the method from the borderless option trace.
2. Add a start-position control to the mission screen. The global setting stays as the default, and the mission-screen choice overrides it for that flight only.
3. Find how the runway start chooses its spawn point and heading, and whether airfield data defines any parking, taxi or hold-short points.
4. Add EOR spawn spots per airfield. If the airfield data has nothing usable, define them in a mod-side data file, starting with one airfield.
5. Spawn the player, and any AI flight members, in the EOR spots, with engines running and weapons armed.

**Constraints**

- Each aircraft needs its own EOR spot, so flight members never spawn inside each other. Plan for several human players per airfield later.
- Keep the stock start options working unchanged.
- Leave room for multiplayer: later, each player may pick their own start, and the host assigns the take-off order.

*Checkpoint:* an EOR start at one airfield with a four-ship flight, then a normal take-off.

## 4. Clickable cockpits (stretch)

The mouse hook already gives a real Windows cursor, which is half of clickable cockpits. In flight the mouse is the stick, so this needs a toggle key that switches between flying and clicking. Do a feasibility note first, not an implementation.

**Two routes to assess**

- **Click zones (simple).** Define clickable regions per cockpit, and turn a click into a keypress SF2 already understands, such as gear or flaps. The open question is how zones follow the view as the camera moves.
- **Ray picking (proper).** Find the 3D cockpit mesh and camera, cast a ray from the cursor, and act on the switch it hits. Much more work, but it scales to every cockpit.

*Checkpoint:* a recommendation, a rough effort estimate for each route, and a prototype on one cockpit if the simple route holds up.

## Ground rules

- **Never modify the original install or the exe on disk.** Patch in memory from the DLL. Removing our files must return the game to stock.
- **Every new option lives in the game's own menus**, saved to Options.ini or mission data, never as a hand-edited ini.
- **Design with multiplayer in mind.** A setting that changes what players can see or do (smart scaling, start rules) must be a single value the host can later sync and enforce.
- **Keep public and private apart.** The public repo holds only our own release code. Recon notes, extracted data and any quoted disassembly stay in the private repo. Check code comments for pasted disassembly before each push. Hook offsets in code are fine.
- **Ship each item on its own** through the installer, with release notes that mention possible antivirus false positives.
- **Stop at each checkpoint** and report anything that contradicts this brief.

## Open questions

- [ ] Smart scaling: what default strength and range feel right? Pick by eye after the first screenshots.
- [ ] EOR starts: which airfield to prototype on, and in which theatre?
- [ ] EOR spots: are they defined in our own mod data file, or added to the airfield data so other modders can extend them?
- [ ] Should the QoL items share one DLL and installer with the borderless mod, or ship as separate modules?
- [ ] Network layer side note, not part of this brief: is the 64-object limit specific to the network layer, or global? Check it during multiplayer recon.
