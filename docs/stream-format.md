# Session stream recording format (`stream_NNN`, `client_NNN`, `botserve_NNN.lzst`)

Developer tool. With host mode on (`LAZYSF2_HOST`, see `lazy-sf2-recon`, `network-recon.md`), the mod records what the host sees to `LAZYSF2_PROBE_DIR\stream_NNN.lzst`. `NNN` is the first free number from 001, and the file is created with `CREATE_NEW`. The code is `src/net/stream_format.*` (format and reader) and `src/net/stream_record.*` (writer).

The bot's synthetic host (`lazysf2-bot --serve PORT --record DIR`) writes the same format to `DIR\botserve_NNN.lzst`. There the "host" is the bot: kind 2 is its scripted jet at every step, kind 7 the States of that jet it sent (20 Hz, recorded before any `--lag`), kind 1 the client's States, and kind 9 what the bot showed of the client. Its header has no theatre (the bot does not know one). Unlike the mod host's file, it is opened when the bot starts serving (so HostState records come before any client), and kind 1 holds only the States the bot's receiver accepted (not old or repeated ones). Without `--start` the file has two `WelcomeSent` records per client: the first with an all-zero join position (`net::welcome_has_join` is false), the second, after the client's first State, with the join position on the bot's jet; it is re-sent once a second until the client's States show it there (at most 10 s).

The mod as a client (`LAZYSF2_CONNECT`) writes `LAZYSF2_PROBE_DIR\client_NNN.lzst`. It is opened at the first Welcome of a connection (its `start_game_time` is the Welcome's host time, and the header names the client's theatre) and closed when the session ends. There `time` is the client's estimate of the host's game time. Kind 3 holds each Welcome the client received (a re-Welcome gives another record), kind 1 the States that arrived from the host (id 0 and the relayed ids; `time` is the arrival in the client's host-time estimate), kind 4 the host's Bye, kind 6 each PeerLeft (with the same reason codes as below), kind 7 the client's own States as sent, kind 9 what it showed of each remote, kind 10 each PeerInfo, and kind 11 its avatar events.

Every file starts with a SessionInfo record (kind 8) that says whose file it is. Files from earlier builds have none; a reader then treats the file as a host's.

To compare two files, `lazysf2-acmi --report VIEWER.lzst TRUTH.lzst` (see Tools below) lines them up on host time: the `time` and `game_time` fields, which every recorder keeps in the host's clock.

All numbers are little-endian. Structures are packed. A reader is in `src/net/stream_format.h` (`StreamReader`): it works on bytes in memory and never reads past the end.

## When a file exists

- It is created when the first client's Hello is accepted, before its Welcome is queued. The first record is the host's `SessionInfo`, the next that client's `ClientJoined`. If the Welcome cannot be queued, the file holds only `ClientJoined` and `ClientLeft` (reason 3).
- It is closed (flushed) when the last client leaves, when the player changes, or when the mod shuts down.
- It stops at 512 MB. The mod logs this once and records nothing more.
- If a write fails, the mod logs it once and the partial file stays. A file that ends in the middle of a record is therefore valid: a reader keeps the records before it (`StreamStatus::Truncated`).
- On process exit (the player closes the game with a client still connected) the writer thread is already gone. The mod writes the records still in its queue, but the last moments (up to about 200 ms) may be missing. The file is normally readable: it ends cleanly or the reader reports `Truncated`. If the writer thread was killed in the middle of a write, the reader may report `BadLength` at the tail, and the records the dead thread held in its own buffer can leave a gap before the ring records the mod appends.
- The host writes records in host game-time order, because every record is made on the game thread. There are two exceptions. A `RemoteShown` carries the time its position was shown for, which on the host can be the step's time plus the player's `dt` (when the avatars step before the player). A client's kind 1 carries its arrival time. Readers sort by time.

## Header (48 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `LZST` |
| 4 | 2 | version, `1` |
| 6 | 2 | `header_len`: bytes in the whole header (48 now). A reader must skip to this offset, so a later version can add fields |
| 8 | 8 | `start_game_time` (double): host game time when the file was opened |
| 16 | 32 | theatre id (the Tacview theatre id, for example `GermanyCE`), NUL padded, at most 31 characters. Empty when it is not known: in files from earlier builds, from the bot, or when the terrain could not be read |

A reader rejects a wrong magic (`BadMagic`), a version it does not know (`BadVersion`) and a `header_len` below 48 (`BadHeader`).

## Records

After the header, records follow with no padding, up to the end of the file.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `length`: the bytes after this field, that is `kind` + `time` + payload. Valid range 9 to 105 |
| 4 | 1 | `kind` |
| 5 | 8 | `time` (double): host game time when the host made the record |
| 13 | `length - 9` | payload |

A reader stops at a `length` outside 9 to 105 (`BadLength`): nothing after it can be trusted. It must pass kinds it does not know to the caller, which skips them using `length`.

`time` is the host's own clock at the moment of the record. For a received State it is the arrival time as the host game thread saw it. The State's own `game_time` (inside the payload) is the sender's stamp in host time, and is normally a little earlier.

### Kinds

The payloads of kinds 1 to 4, 7 and 10 are the protocol messages **byte for byte as on the wire** (`src/net/protocol.h`, with their own magic, version and type). Decode them with `LazyWorks::SF2Patch::net::decode`. Files recorded by builds before protocol v2 carry v1 payloads (a 52-byte Hello and a 42-byte Welcome, version byte 1), which the current decoders reject with `BadVersion`.

| Kind | Name | Payload | When |
|---|---|---|---|
| 1 | StateReceived | `StateMsg`, 78 bytes, `client_id` 1 to 4 | every State from a welcomed client that the host passed on to the client's avatar (already checked by the network thread) |
| 2 | HostState | `StateMsg`, 78 bytes, `client_id` 0 | the host player's state (position, velocity, angles, rates), once per player step. `seq` counts from 1 in each file. `game_time` equals `time` |
| 3 | WelcomeSent | `WelcomeMsg`, 58 bytes (protocol v2) | the Welcome was queued for a client |
| 4 | ByeReceived | `ByeMsg`, 7 bytes | a client sent Bye |
| 5 | ClientJoined | `ClientJoinedPayload`, 50 bytes | a client was given an avatar; just before its Welcome |
| 6 | ClientLeft | `ClientLeftPayload`, 3 bytes | a client's avatar was released |
| 7 | LocalState | `StateMsg`, 78 bytes, the sender's own id | a State the recorder sent of its own jet (20 Hz of host time). Client and bot serve files |
| 8 | SessionInfo | `SessionInfoPayload`, 51 bytes | always the first record, at `start_game_time`: whose file it is |
| 9 | RemoteShown | `RemoteShownPayload`, 43 bytes | what the receiver showed of a remote jet: 10 Hz of host time per remote, and at every step that snapped. All three recorders |
| 10 | PeerInfoReceived | `PeerInfoMsg`, 54 bytes | a PeerInfo from the host (the host's own, id 0, and each other client's). Client files only |
| 11 | AvatarEvent | `AvatarEventPayload`, 3 bytes | a remote's aircraft was parked, taken again or lost. Host and client files |

`ClientJoinedPayload`: `uint16 client_id`, `char callsign[16]`, `char aircraft[32]`. Both strings are NUL padded and printable ASCII only. The aircraft is the type the client asked for in its Hello. It is not the AI aircraft the host used as the avatar.

`ClientLeftPayload`: `uint16 client_id`, `uint8 reason`.

`SessionInfoPayload`: `uint8 role` (0 host, 1 client, 2 bot serve), `uint16 own_id` (0 for the host and the bot, the Welcome's id for a client), `char callsign[16]`, `char aircraft[32]`. These are the recorder's own; the strings are NUL padded and printable ASCII, and the aircraft is `unknown` when its type could not be read.

`AvatarEventPayload`: `uint16 id` (the remote), `uint8 event`:
- 1 parked: no State for 10 s, so the aircraft went back to the AI. The session stays up.
- 2 taken: an aircraft was taken again after a park or a loss. On a client, also the first take for that remote.
- 3 lost: the client's avatar for that remote was destroyed. On the host this ends the client's session, so the file has a `ClientLeft` instead.

`RemoteShownPayload`: `uint16 id` (0 the host, 1 to 4 a client), `double pos[3]` (shown position), `float ang[3]` (shown pitch, heading, roll), `float age` (the record's `time` minus the newest State's `game_time`, seconds; slightly below 0 when the sender's clock leads), `uint8 out` (the receiver's step result: 1 snap, 2 blend, 3 steady, 4 coast). With 4 the position and angles are the last ones shown: the receiver stopped writing, and the game coasts the aircraft on.

| Reason | Meaning |
|---|---|
| 0 | the client left (Bye or disconnect) |
| 1 | no packets for 5 s |
| 2 | the host ended it: avatar destroyed, the player changed, or the host shut down |
| 3 | the Welcome could not be queued, so the join was undone |

A client that joins twice in one file gives two `ClientJoined` records. `client_id` is a slot (1 to 4) and the host reuses it, so a reader must treat `ClientJoined` as the start of a new aircraft for that id.

### What can be missing

- If the writer falls behind, the mod drops records and counts them. It logs the count (once a second at most while it is rising, and once at the end). A gap in a client's `seq` can mean a drop here, or a lost packet (States are unreliable).
- States the network thread refused (bad format, out of range, wrong phase, over quota) are not recorded. Neither are States that arrive before the Welcome.
- Hello, Ping and Pong are not recorded. The `ClientJoined` record replaces the Hello.

## Tools

`lazysf2-acmi FILE.lzst OUT.acmi [--ghosts]` converts any of the three files for Tacview. The host's jet is object 1 (blue), and each client is 0x100 + id (red). Names come from SessionInfo, ClientJoined and PeerInfoReceived. `--ghosts` adds each RemoteShown track as object 0x200 + id, green, named `<callsign> as seen by <recorder's callsign>`, so the true jet and the shown jet fly side by side. Coasting records are left out.

`lazysf2-acmi --report VIEWER.lzst TRUTH.lzst [--id N] [--offset R,B,U] [--window S]` compares what VIEWER showed of remote N (default: the lowest id it shows) with where that jet was. VIEWER and TRUTH may be the same file.
- **The truth** is interpolated at each RemoteShown's `time`. It is the first of these:
  - with `--offset`: TRUTH's kind 2 (the host's jet) plus R metres right, B back and U up in its heading frame. This is for the bot's formation runs, where the bot records nothing;
  - for id 0, TRUTH's kind 2 (every step);
  - the jet's own kind 7 in TRUTH (as sent, 20 Hz);
  - TRUTH's kind 1 of id N, at the States' own stamps.
- **The figures**, per 10 s of host time and overall:
  - the position error's median, p95 and max, split into along track (+ ahead of the truth), across (+ right of its track) and vertical (+ above). Each part gives the signed median, and the p95 and max of its size;
  - the angle between the shown and the true attitude;
  - snaps, and coasting time (coasting records are not compared);
  - the smoothness figure: the shown path's acceleration (p95 and max over the 10 Hz records, snaps left out), next to the truth's peak at the same times.
- It refuses files that do not overlap in host time. It says how many records fell outside the truth's span, or in a truth gap over 1 s.

## Reading it (outline for the ACMI exporter)

```
StreamReader r(bytes, size);
StreamHeader h;  if (r.read_header(h) != Ok) -> reject the file
StreamRecord rec;
while (r.next(rec) == Ok) switch (rec.kind) { ... }   // End or Truncated: stop, keep what you have
```

Plot the host from kind 2 (id 1 in the ACMI), and each client from kind 1, with the names from kind 5. Use the State's `game_time` for client samples, and `time` for the host. In a client file the host's jet is kind 1 with id 0, the client's own jet is kind 7, and the names come from kinds 8 and 10.
