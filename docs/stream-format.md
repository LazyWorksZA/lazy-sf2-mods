# Host stream recording format (`stream_NNN.lzst`)

Developer tool. With host mode on (`LAZYSF2_HOST`, see `docs/network-recon.md`), the mod records what the host sees to `LAZYSF2_PROBE_DIR\stream_NNN.lzst`. `NNN` is the first free number from 001, and the file is created with `CREATE_NEW`. The code is `src/net/stream_format.*` (format and reader) and `src/net/stream_record.*` (writer).

All numbers are little-endian. Structures are packed. A reader is in `src/net/stream_format.h` (`StreamReader`): it works on bytes in memory and never reads past the end.

## When a file exists

- It is created when the first client is welcomed. The first record is that client's `ClientJoined`.
- It is closed (flushed) when the last client leaves, when the player changes, or when the mod shuts down.
- It stops at 512 MB. The mod logs this once and records nothing more.
- If a write fails, the mod logs it once and the partial file stays. A file that ends in the middle of a record is therefore valid: a reader keeps the records before it (`StreamStatus::Truncated`).
- The host writes records in host game-time order, because every record is made on the game thread.

## Header (48 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `LZST` |
| 4 | 2 | version, `1` |
| 6 | 2 | `header_len`: bytes in the whole header (48 now). A reader must skip to this offset, so a later version can add fields |
| 8 | 8 | `start_game_time` (double): host game time when the file was opened |
| 16 | 32 | theatre name, NUL padded. **Empty in this version**: the mod has no cheap, safe way to read it |

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

The payloads of kinds 1 to 4 are the protocol messages **byte for byte as on the wire** (`src/net/protocol.h`, with their own magic, version and type). Decode them with `lazysf2::net::decode`.

| Kind | Name | Payload | When |
|---|---|---|---|
| 1 | StateReceived | `StateMsg`, 78 bytes, `client_id` 1 to 4 | every State from a welcomed client that the host passed on to the client's avatar (already checked by the network thread) |
| 2 | HostState | `StateMsg`, 78 bytes, `client_id` 0 | the host player's state (position, velocity, angles, rates), once per player step. `seq` counts from 1 in each file. `game_time` equals `time` |
| 3 | WelcomeSent | `WelcomeMsg`, 42 bytes | the Welcome was queued for a client |
| 4 | ByeReceived | `ByeMsg`, 7 bytes | a client sent Bye |
| 5 | ClientJoined | `ClientJoinedPayload`, 50 bytes | a client was given an avatar; just before its Welcome |
| 6 | ClientLeft | `ClientLeftPayload`, 3 bytes | a client's avatar was released |

`ClientJoinedPayload`: `uint16 client_id`, `char callsign[16]`, `char aircraft[32]`. Both strings are NUL padded and printable ASCII only. The aircraft is the type the client asked for in its Hello. It is not the AI aircraft the host used as the avatar.

`ClientLeftPayload`: `uint16 client_id`, `uint8 reason`.

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

## Reading it (outline for the ACMI exporter)

```
StreamReader r(bytes, size);
StreamHeader h;  if (r.read_header(h) != Ok) -> reject the file
StreamRecord rec;
while (r.next(rec) == Ok) switch (rec.kind) { ... }   // End or Truncated: stop, keep what you have
```

Plot the host from kind 2 (id 1 in the ACMI), and each client from kind 1, with the names from kind 5. Use the State's `game_time` for client samples, and `time` for the host.
