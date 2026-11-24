# SF2 Network Layer — Claude Code Brief

Nov 24, 2026 · @Jozias van der Westhuizen

This brief covers adding multiplayer back into Strike Fighters 2. That means sessions, replication, recording to Tacview ACMI for debriefs, and the debug tooling to build it all as a solo developer. It follows the borderless fullscreen brief and reuses that brief's recon and hooking work. Nothing here is built until the borderless work is done and the recon below says it's workable.

## Goals

- A few friends can fly single missions together, in dogfight or co-op. That comes first.
- Every session can be recorded and exported to Tacview ACMI for debriefs: what happened, and what could have been done differently.
- Later, release it publicly to the SF2 community. That means server, lobby and client binaries anyone can host.

**Out of scope for v1:** campaigns, joining a mission already in progress, and host migration (if the host leaves, the mission ends).

## Recon additions

Add these to the Phase 0 recon from the borderless brief. Record findings only; build nothing yet.

- **Network layer slot.** Does the exe still call a network-layer interface through `NetworkLayerDLL`? Does any state-serialisation or replication code survive, either dead or reachable?
- **Native replay.** Does SF2 have any flight recording or replay feature, hidden or disabled?
- **Headless mode.** Is there any trace of a dedicated-server or no-render mode? This only decides whether a headless host is a very late option. Don't pursue it beyond that.
- **Controlling objects from outside.** This is the go/no-go test for the whole project. Take one AI aircraft and set its position and velocity every frame from our own code. Does it hold a smooth path, or do SF2's AI and flight model fight back? Then try spawning an object, such as a missile, mid-flight with a given position and velocity.

## Authority model

The host's word is final, but each object has a single owner, chosen to fit what that object needs.

| What | Owned by | Why |
| --- | --- | --- |
| AI aircraft, ground objects, mission state | Host | One source of truth for the world |
| Each player's own jet | That player's machine | Real flight model and inputs live there |
| Damage | Host applies it | Validates claims before changing state |
| Gun hits | Shooter claims, host validates | Instant and short-range, so judge it from what the shooter saw |
| Missiles | Whoever owns the target | Guidance sees the true, lag-free target. AI targets keep missiles on the host |
| Orders to an AI flight | One named player or the host | Avoids conflicting wingman commands |

The host always arbitrates handovers. Clients ask; they never take ownership first-come-first-served. When a player drops, the host immediately takes ownership of their jet and any missiles they own.

## Replication

- **Shared clock.** Clock sync during the handshake, and a mission timestamp on every message. Everything below depends on it.
- **Send state and control inputs.** Receivers predict a remote jet with dead reckoning. When inputs are available, they can run the flight model forward instead.
- **Send only when needed.** The sender runs the same prediction as the receivers and sends an update only when the real path drifts past a threshold.
- **Interpolation delay.** Draw remote objects slightly in the past, between known updates. Blend corrections smoothly over several frames, with no snapping.
- **Interest management.** Update nearby and engaged aircraft often and distant ones rarely. The host's home upload is the bottleneck.
- **Missile launch.** Fast-forward a newly launched missile by the shooter's latency, using our own simple kinematics, then spawn it mid-flight and let SF2 take over. Rewinding the target's history is a refinement for later.

## Single-player features

- **Time compression.** Works like Falcon BMS: the sim speeds up only when every player has chosen the same rate, and drops back as soon as anyone lowers theirs. The players sort it out among themselves.
- **Pause, quick save, mission end.** Decide each one: disable it, give it to the host, or require everyone to agree. Start with disabled.

## Sessions and lobby

- **Master server.** Hosts register and send a heartbeat, and clients fetch the list, then connect directly to the host. The master server never carries gameplay traffic. Make its address configurable, ideally as a list, so the community can run their own. Our own instance runs on a best-effort basis.
- **Direct connect by IP** is always available, so no lobby is ever required.
- **NAT traversal.** For v1 among friends, use Tailscale or ZeroTier. For the public release, add UDP hole punching with a relay fallback.
- **Matching content.** Hash aircraft, weapons, terrain and other relevant data, and refuse to join a session when the hashes differ. Allowing visual-only differences can come later.
- **Version handshake.** Put a protocol version in the handshake from day one, so incompatible builds refuse each other cleanly.
- **Launcher.** Build the lobby browser as a small external launcher, not inside SF2's menus.

## Recording and Tacview

- **Record the host's stream.** Write the host's state and event stream to disk. Replaying it means a client reads the file as if it were a live host, with a free camera. The same recordings are used to debug desyncs.
- **Export to ACMI.** Use Tacview's flat-world coordinates with SF2's own terrain exported as custom terrain. SF2 theatres such as Vietnam are compressed, so mapping onto real latitude and longitude would distort either the paths or the speeds.
- **To verify:** how Tacview supports custom terrain, and which Tacview edition that needs.

## Debug tooling

This gets built first, because it decides how painful everything else is.

- **Synthetic client.** A headless program that connects to a real host and appears to be a second player. It flies scripted paths, including hard break turns, fires weapons, and disconnects on cue. It lets one developer with one copy of the game test multiplayer.
- **Bad-network simulation.** Use clumsy on Windows to inject latency, jitter and packet loss.
- **Logs on every machine.** Each machine logs what it believed happened, with timestamps, so two views can be compared or replayed afterwards.

## Phases

Each phase ends at a checkpoint. Stop, report, and wait for review.

1. **Recon** (above). *Checkpoint:* go or no-go on controlling objects from outside.
2. **Puppet spike.** Drive one AI aircraft from a recorded path inside a single game. *Checkpoint:* it flies smoothly.
3. **Synthetic client and recording.** Host mode, the synthetic client, stream recording, and ACMI export. *Checkpoint:* a bot's flight shows correctly in game and in Tacview.
4. **Two real players.** Replicating player jets, the shared clock, and smoothing, tested under clumsy. *Checkpoint:* two machines fly in formation at 150 ms latency.
5. **Weapons and damage.** Gun claims, missile ownership and launch fast-forward. *Checkpoint:* a shot-down event agrees on both machines and in the recording.
6. **Sessions.** Handshake, content hashing, launcher, master server, time compression. *Checkpoint:* friends join from a list.
7. **Public release.** NAT traversal with relay, packaging, documentation.

## Before a public release

- [ ] Check ThirdWire's EULA for reverse-engineering and distribution terms.
- [ ] Plan for antivirus false positives, since this hooks into a game process. Consider code signing.
- [ ] Ship only our own binaries. Players must own the game.

## Extra tools

These are needed on top of the borderless brief's list.

| Tool | Used for |
| --- | --- |
| clumsy | Simulating bad networks on Windows |
| Wireshark | Inspecting our packets |
| Tacview | Checking ACMI output and custom terrain |
| Tailscale or ZeroTier | v1 connectivity between friends |
| A second PC or VM (optional) | Final two-machine tests; the synthetic client covers most testing |
