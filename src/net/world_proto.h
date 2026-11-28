// Shared-world messages (protocol v3): the mission transfer (T5) and, later, the world stream. Same framing as
// protocol.h (Header, little-endian, packed); types from 20 up, decoded here, not by peek_type(). No game or Windows
// dependencies. See lazy-sf2-recon: network-recon §10.8 and §10.14.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net/protocol.h"

namespace LazyWorks::SF2Patch::net {

enum class WorldType : uint8_t {
    MissionRequest = 20,  // client to host, reliable
    MissionOffer = 21,    // host to client, reliable
    MissionChunk = 22,    // host to client, reliable, variable length
    WorldStates = 23,     // host to client, unreliable (channel 1), 1..kWorldStatesMax object states
    WorldEvents = 24,     // host to client, reliable, 1..kWorldEventsMax events
    AvatarState = 25,     // each player's own jet, reliable, on change: client to host, host to clients (relayed)
    WorldSnapshot = 26,   // host to a client after its Welcome, reliable: the head of a late-join snapshot
    HitClaim = 27,        // a shooter's PC to the host, reliable: one hit of its own gun on another PC's object
    HitApply = 28,        // the host to the PC that owns a player's jet, reliable: damage for that jet
    WeaponLaunch = 29,    // a player's weapon release, reliable: client to host, host to clients (relayed)
    Loadout = 30,         // a player's stores, reliable, every 10 s and on change: relayed like AvatarState
    Consumables = 31,     // a player's fuel, ammunition and countermeasures, unreliable, every 2 s: relayed
    DamageState = 32,     // a player's jet damage, reliable, on change (polled 4 times a second): relayed
    Countermeasure = 33,  // a player's chaff and flare releases, reliable, as they happen: relayed
    MissionClock = 34,    // the world owner's mission clock, unreliable, every 2 s: owner to the others
    SeatState = 35,       // host to a client after its Welcome, reliable: its seat aircraft's current state
    HandoverBegin = 36,   // the world owner's snapshot offer, reliable (net/handover_format.h)
    HandoverChunk = 37,   // ... its chunks, reliable, 80 bytes each
    HandoverDone = 38,    // the receiver's answer, reliable
    LobbyStatus = 39,     // client to host, reliable, on every mission-fetch round: callsign, ready, seat wish, content
    LobbyRoster = 40,     // host to client, reliable, the answer: the pilots, seats, mission summary and Launch count
    LobbyInventory = 41,  // client to host, reliable: its aircraft and ground object types with hashes, in chunks (once a
                          // session, and again while the roster says the host lacks some)
};
inline constexpr uint8_t kWorldTypeFirst = 20, kWorldTypeLast = 41;

/// <summary>The type byte of a v3 packet with a world type, or 0 (not one, or a bad header).</summary>
uint8_t peek_world_type(const uint8_t* data, size_t len);

struct LobbyStatusMsg;
struct LobbyRosterMsg;
size_t encode(const LobbyStatusMsg& m, uint8_t* out, size_t cap);
size_t encode(const LobbyRosterMsg& m, uint8_t* out, size_t cap);
/// <summary>Decoders NUL-terminate the strings.</summary>
Status decode(const uint8_t* data, size_t len, LobbyStatusMsg& out);
Status decode(const uint8_t* data, size_t len, LobbyRosterMsg& out);
struct LobbyInventoryMsg;
size_t encode(const LobbyInventoryMsg& m, uint8_t* out, size_t cap);
/// <summary>Checks counts (first + count <= total <= kMaxInventory, count <= kInventoryPerChunk, kinds 'A'/'G') and
/// NUL-terminates the names.</summary>
Status decode(const uint8_t* data, size_t len, LobbyInventoryMsg& out);

/// <summary>Payload bytes per chunk: a chunk packet (972 bytes) stays under ENet's 1,024-byte limit and one datagram.</summary>
inline constexpr size_t kMissionChunkData = 960;
/// <summary>The largest packed mission (compressed .MSN plus sidecar) the host offers: 274 chunks.</summary>
inline constexpr uint32_t kMissionMaxPacked = 256 * 1024;
inline constexpr uint32_t kMissionMaxRaw = 4 * 1024 * 1024;

inline constexpr uint8_t kOfferOk = 0;       // chunks follow, in order, on the reliable channel
inline constexpr uint8_t kOfferNone = 1;     // the host has no mission yet: ask again later
inline constexpr uint8_t kOfferTooBig = 2;   // the host's mission is over kMissionMaxPacked
inline constexpr uint8_t kOfferSame = 3;     // the client already holds this mission (have_id): no chunks
inline constexpr uint8_t kOfferRefused = 4;  // lobby: the client's game content differs from the host's: no mission

/// <summary>What the host's seat table needs to know of its own flight.</summary>
struct SeatPlan {
    int members = 0;      // the host's flight's member count (0: no seats to give)
    int host_member = 0;  // the member the host flies
};
/// <summary>
/// The owner's seat rule: the first member of the host's flight, in slot order, that is neither the host's nor
/// taken (taken[0..n)). -1 when none is free.
/// </summary>
inline int assign_seat(const SeatPlan& p, const int* taken, size_t n) {
    for (int m = 0; m < p.members; ++m) {
        if (m == p.host_member) continue;
        bool used = false;
        for (size_t i = 0; i < n && !used; ++i) used = taken[i] == m;
        if (!used) return m;
    }
    return -1;
}

#pragma pack(push, 1)
struct MissionRequestMsg {
    Header h;
    uint32_t have_id;  // the mission id the client already holds, 0 for none
    uint32_t tag;      // the client's world tag (random per launch, never 0): the host keys its seat by it
};
struct MissionOfferMsg {
    Header h;
    uint32_t mission_id;    // CRC-32 of the packed bytes
    uint32_t packed_bytes;  // what the chunks carry
    uint32_t raw_bytes;     // unpacked size
    uint16_t chunk_bytes;   // kMissionChunkData
    uint16_t chunk_count;
    uint8_t status;         // kOffer*
    uint8_t seat;           // the member of the host's flight the host assigned this client (kNoSeat: none free)
    uint8_t host_seat;      // the member the host flies
};
/// <summary>The lobby (L4): rows in a roster, and the host's answer to a status.</summary>
inline constexpr int kLobbyRows = 8;
inline constexpr uint8_t kLobbyOk = 0;          // the status was taken (and a seat wish granted)
inline constexpr uint8_t kLobbySeatTaken = 1;   // the wished seat is not free: the pilot keeps the one it had
inline constexpr uint8_t kLobbyContent = 2;     // the client's game content differs from the host's: not in the lobby
inline constexpr uint8_t kLobbyFull = 3;        // no room for another pilot
inline constexpr uint8_t kLobbyRowHost = 1, kLobbyRowReady = 2, kLobbyRowYou = 4;
inline constexpr uint8_t kLobbyNoWish = 0xff;   // want_seat: keep the seat
inline constexpr uint32_t kLaunchFlying = 0x80000000u;  // launch_seq: the host is flying that Launch (late joiners may)
struct LobbyStatusMsg {
    Header h;
    uint32_t tag;           // the client's world tag (as in its mission request)
    char callsign[16];
    uint8_t ready;
    uint8_t want_seat;      // a member of the host's player flight, or kLobbyNoWish
    uint8_t leaving;        // 1: the pilot leaves the session (the host drops its row at once)
    uint32_t content_hash;  // hooks/content_hash: what the host compares with its own
    uint32_t ready_mission; // the mission id Ready was pressed for: Ready counts only while it is the host's mission
};
struct LobbyRow {
    char callsign[16];  // empty: a free seat (the AI flies it)
    uint8_t seat;       // member of the player flight, kNoSeat: none yet
    uint8_t flags;      // kLobbyRow*
};
struct LobbyRosterMsg {
    Header h;
    uint32_t mission_id;  // the host's mission (0: none yet)
    uint32_t launch_seq;  // counts the host's Launches (L7); kLaunchFlying set while the host's flight runs
    uint8_t members;      // seats in the player flight (0: no mission)
    uint8_t count;        // rows used
    uint8_t result;       // kLobby* for the status this answers
    uint8_t your_seat;    // kNoSeat: none
    char summary[3][40];  // the mission in three short lines (ASCII)
    LobbyRow rows[kLobbyRows];
    uint32_t session;     // random per host session: a client still joined when the host hosts again sees it change
    uint32_t content_hash;  // the host's (hooks/content_hash), shown to a client it keeps out
    uint16_t inventory_have;  // entries of this pilot's inventory the host holds (it sends all again while short)
    uint16_t pad;
};
/// <summary>One type of a pilot's inventory: kind 'A' aircraft or 'G' ground object, the content list's hash.</summary>
struct InventoryEntry {
    uint8_t kind;
    uint8_t pad[3];
    uint32_t hash;
    char name[40];
};
inline constexpr int kInventoryPerChunk = 18;
inline constexpr uint16_t kMaxInventory = 4096;
struct LobbyInventoryMsg {
    Header h;
    uint32_t tag;
    uint16_t total;
    uint16_t first;
    uint8_t count;
    uint8_t pad[3];
    InventoryEntry e[kInventoryPerChunk];
};
struct MissionChunkHead {
    Header h;
    uint32_t mission_id;
    uint16_t index;
    uint16_t len;  // payload bytes that follow (1..kMissionChunkData; only the last chunk may be shorter)
};
#pragma pack(pop)
static_assert(sizeof(LobbyStatusMsg) == 35 && sizeof(LobbyRow) == 18 && sizeof(LobbyRosterMsg) == 292);
static_assert(sizeof(InventoryEntry) == 48 && sizeof(LobbyInventoryMsg) == 880);
static_assert(sizeof(MissionRequestMsg) == 12);  // 4:4 have id, 8:4 tag
static_assert(sizeof(MissionOfferMsg) == 23);    // 4:4 id, 8:4 packed, 12:4 raw, 16:2 chunk, 18:2 count, 20:1 status,
                                                 // 21:1 seat, 22:1 host seat
static_assert(sizeof(MissionChunkHead) == 12);   // 4:4 id, 8:2 index, 10:2 len, then the payload
inline constexpr size_t kMissionChunkMax = sizeof(MissionChunkHead) + kMissionChunkData;
static_assert(kMissionChunkMax == 972 && kMissionChunkMax < 1024);

size_t encode(const MissionRequestMsg& m, uint8_t* out, size_t cap);
size_t encode(const MissionOfferMsg& m, uint8_t* out, size_t cap);
/// <summary>Chunk index of packed (n bytes) as one packet; 0 if index is past the end or cap is too small.</summary>
size_t encode_mission_chunk(uint32_t mission_id, uint16_t index, const uint8_t* packed, size_t n, uint8_t* out,
                            size_t cap);
Status decode(const uint8_t* data, size_t len, MissionRequestMsg& out);
Status decode(const uint8_t* data, size_t len, MissionOfferMsg& out);
/// <summary>A chunk's head; *payload points into data. Checks that len matches the head.</summary>
Status decode_mission_chunk(const uint8_t* data, size_t len, MissionChunkHead& head, const uint8_t** payload);

inline size_t mission_chunk_count(size_t packed) { return (packed + kMissionChunkData - 1) / kMissionChunkData; }

/// <summary>
/// The package: "LZWM", uint32 version 1, uint32 sidecar bytes, uint32 .MSN bytes, the sidecar text, the .MSN file,
/// all deflated (miniz). *id is the CRC-32 of the packed bytes, *raw the unpacked size. False if too big.
/// </summary>
bool pack_mission(const std::string& sidecar, const std::vector<uint8_t>& msn, std::vector<uint8_t>* packed,
                  uint32_t* id, uint32_t* raw);
/// <summary>The reverse; checks the CRC, the sizes and the header. *why says what failed.</summary>
bool unpack_mission(const std::vector<uint8_t>& packed, uint32_t id, uint32_t raw, std::string* sidecar,
                    std::vector<uint8_t>* msn, const char** why);

// ---- the world stream (T6) ----

/// <summary>What a stable key names (network-recon §10.3).</summary>
enum class KeyKind : uint8_t { Aircraft = 1, Ground = 2, Placed = 3 };

#pragma pack(push, 1)
/// <summary>
/// One moving object, quantised: position in centimetres (int32, world frame, about 21,000 km either way), velocity in
/// 0.05 m/s (about 1,600 m/s), angles pitch/heading/roll in 2^-16 turns, rates in 0.001 rad/s (32 rad/s).
/// </summary>
struct WorldObjState {
    uint8_t kind;   // KeyKind
    uint16_t a;     // slot index, or area index
    uint16_t b;     // member index, or placed index
    int32_t pos[3];
    int16_t vel[3];
    int16_t ang[3];
    int16_t rates[3];
};
struct WorldStatesHead {
    Header h;
    uint32_t seq;      // per receiver stream, increasing
    uint32_t host_ms;  // host game time in ms when sampled
    uint8_t count;
};
/// <summary>A world event; value depends on the event.</summary>
struct WorldEventRec {
    uint8_t event;  // kEvent*
    uint8_t kind;   // KeyKind
    uint16_t a, b;
    int16_t value;
};
struct WorldEventsHead {
    Header h;
    uint32_t seq;
    uint32_t host_ms;
    uint8_t count;
};
#pragma pack(pop)
static_assert(sizeof(WorldObjState) == 35);   // 0:1 kind, 1:2 a, 3:2 b, 5:12 pos, 17:6 vel, 23:6 ang, 29:6 rates
static_assert(sizeof(WorldStatesHead) == 13);  // 4:4 seq, 8:4 host ms, 12:1 count
static_assert(sizeof(WorldEventRec) == 8);
static_assert(sizeof(WorldEventsHead) == 13);
inline constexpr size_t kWorldStatesMax = 28;  // 13 + 28 * 35 = 993 bytes: under ENet's 1,024 and one datagram
inline constexpr size_t kWorldEventsMax = 64;  // 13 + 64 * 8 = 525 bytes
inline constexpr size_t kWorldStatesBytes = sizeof(WorldStatesHead) + kWorldStatesMax * sizeof(WorldObjState);
inline constexpr size_t kWorldEventsBytes = sizeof(WorldEventsHead) + kWorldEventsMax * sizeof(WorldEventRec);
static_assert(kWorldStatesBytes == 993 && kWorldStatesBytes < 1024 && kWorldEventsBytes == 525);
/// <summary>The largest world message the cores carry.</summary>
inline constexpr size_t kWorldMaxMessage = 1000;  // the cores' world queue entries
static_assert(kWorldStatesBytes <= kWorldMaxMessage && kWorldEventsBytes <= kWorldMaxMessage);

inline constexpr uint8_t kEventDestroyed = 1;     // value: unused
inline constexpr uint8_t kEventAwake = 2;         // the object woke (formation start, scramble)
inline constexpr uint8_t kEventAsleep = 3;        // the object went to sleep (landed)
inline constexpr uint8_t kEventPlacedDamage = 4;  // value: the placed record's damage 0..100 (a delta of its state)
inline constexpr uint8_t kEventWaypoint = 6;      // key (A, slot, kFlightKey): the flight's current waypoint (value)
inline constexpr uint16_t kFlightKey = 0xffff;    // the b of a key that names a whole flight, not a member
inline constexpr uint8_t kEventDamageLevel = 5;   // value: the object's damage level 0..5 (reserved: not sent yet)

/// <summary>An unquantised object state (world metres, m/s, radians).</summary>
struct ObjPose {
    double pos[3];
    float vel[3];
    float ang[3];
    float rates[3];
};
/// <summary>Quantises (clamping to the field's range; not finite gives 0).</summary>
void quantise(const ObjPose& p, WorldObjState* q);
/// <summary>Back to metres, m/s and radians (angles in -pi..pi).</summary>
void dequantise(const WorldObjState& q, ObjPose* p);

/// <summary>Encodes n (1..kWorldStatesMax) states; returns the byte count or 0.</summary>
size_t encode_world_states(uint32_t seq, uint32_t host_ms, const WorldObjState* s, size_t n, uint8_t* out, size_t cap);
/// <summary>Checks the header, the count and the length; *states points into data.</summary>
Status decode_world_states(const uint8_t* data, size_t len, WorldStatesHead& head, const WorldObjState** states);
size_t encode_world_events(uint32_t seq, uint32_t host_ms, const WorldEventRec* e, size_t n, uint8_t* out, size_t cap);
Status decode_world_events(const uint8_t* data, size_t len, WorldEventsHead& head, const WorldEventRec** events);

/// <summary>
/// The host's seats by client tag. A tag holds its seat while one of its connections is open (claim on a request or
/// Hello, release when the last one closes, a refused Hello included). A released seat is kept for that tag while
/// free seats remain, so a client that reconnects or restarts with the same tag gets it back; when none remain, a new
/// client takes the oldest released one. Not thread-safe: HostCore guards it.
/// </summary>
class SeatBook {
public:
    static constexpr size_t kMax = 16;
    void reset(const SeatPlan& p) {
        plan_ = p;
        n_ = 0;
    }
    const SeatPlan& plan() const { return plan_; }
    /// <summary>The tag's seat, now active; -1 for tag 0 or no seat to give.</summary>
    int claim(uint32_t tag) {
        if (tag == 0) return -1;
        const uint64_t now = ++clock_;
        for (size_t i = 0; i < n_; ++i) {
            Entry& e = e_[i];
            if (e.tag != tag) continue;
            if (e.active || !held_by_other(e.seat, i)) {
                e.active = true;
                e.stamp = now;
                return e.seat;
            }
            remove(i);  // its old seat went to someone else
            break;
        }
        int taken[kMax];
        size_t nt = 0;
        for (size_t i = 0; i < n_; ++i) taken[nt++] = e_[i].seat;  // active and kept seats
        int s = assign_seat(plan_, taken, nt);
        if (s < 0) {  // only kept seats are left: the oldest released one goes
            size_t old = kMax;
            for (size_t i = 0; i < n_; ++i)
                if (!e_[i].active && (old == kMax || e_[i].stamp < e_[old].stamp)) old = i;
            if (old == kMax) return -1;
            s = e_[old].seat;
            remove(old);
        }
        if (n_ == kMax) {  // forget the oldest released entry
            size_t old = kMax;
            for (size_t i = 0; i < n_; ++i)
                if (!e_[i].active && (old == kMax || e_[i].stamp < e_[old].stamp)) old = i;
            if (old == kMax) return -1;
            remove(old);
        }
        e_[n_++] = Entry{tag, s, true, now};
        return s;
    }
    /// <summary>The tag's last connection closed: its seat is released (kept for it while seats remain).</summary>
    void release(uint32_t tag) {
        for (size_t i = 0; i < n_; ++i)
            if (e_[i].tag == tag) e_[i].active = false;
    }
    /// <summary>True while the tag holds its seat (diagnostics, tests).</summary>
    bool active(uint32_t tag) const {
        for (size_t i = 0; i < n_; ++i)
            if (e_[i].tag == tag) return e_[i].active;
        return false;
    }

private:
    struct Entry {
        uint32_t tag = 0;
        int seat = -1;
        bool active = false;
        uint64_t stamp = 0;
    };
    bool held_by_other(int seat, size_t self) const {
        for (size_t i = 0; i < n_; ++i)
            if (i != self && e_[i].seat == seat && e_[i].active) return true;
        return false;
    }
    void remove(size_t i) { e_[i] = e_[--n_]; }
    SeatPlan plan_{};
    Entry e_[kMax];
    size_t n_ = 0;
    uint64_t clock_ = 0;
};

// ---- the late-join snapshot (T8) ----

#pragma pack(push, 1)
/// <summary>
/// The head of a snapshot: `parts` WorldEvents messages follow on the same reliable channel, `records` events in all,
/// describing every key whose state differs from the mission file's start (destroyed, awake, asleep, placed damage,
/// flight waypoints) at host time host_ms. The movers' poses come with the stream as usual.
/// </summary>
struct WorldSnapshotMsg {
    Header h;
    uint32_t mission_id;
    uint32_t host_ms;
    uint16_t records;
    uint8_t parts;
};
#pragma pack(pop)
static_assert(sizeof(WorldSnapshotMsg) == 15);  // 4:4 mission id, 8:4 host ms, 12:2 records, 14:1 parts
inline constexpr size_t kSnapshotMaxRecords = 255 * kWorldEventsMax;
size_t encode(const WorldSnapshotMsg& m, uint8_t* out, size_t cap);
Status decode(const uint8_t* data, size_t len, WorldSnapshotMsg& out);

// ---- a player's jet state (T7) ----

#pragma pack(push, 1)
/// <summary>
/// A player's discrete jet state, sent by its owner on change. radar_mode is the detect interface's mode (0 off,
/// 1 search, 2 track, 3 lock); the target is a stable key (KeyKind::Aircraft slot/member; kind 0 = none). config is
/// the object's state word +0x5f0 masked to kAvatarConfigMask (flaps, airbrake, gear, hook, lights, wheel brakes,
/// jettison).
/// throttle is input channel 3 (0..1, 1 = full afterburner) as 0..255; engines has bit i set while engine i runs
/// (the first 8), engine_count says how many of those bits are meaningful (a receiver leaves the avatar's other engines
/// alone); guns has bit 0 for the primary gun trigger (input 12) and bit 1 for the secondary (13).
/// </summary>
struct AvatarStateMsg {
    Header h;
    uint16_t peer;  // the owner: 0 the host, k client k (the host sets it from the connection)
    uint16_t seq;   // per owner, increasing
    uint8_t radar_mode;
    uint8_t target_kind;
    uint16_t target_a, target_b;
    uint16_t config;
    uint8_t throttle;
    uint8_t engines;
    uint8_t guns;
    uint8_t engine_count;  // 0..8
};
#pragma pack(pop)
static_assert(sizeof(AvatarStateMsg) == 20);  // 4:2 peer, 6:2 seq, 8:1 mode, 9:1 target kind, 10:2 a, 12:2 b, 14:2 config,
                                              // 16:1 throttle, 17:1 engines, 18:1 guns, 19:1 engine count
inline constexpr uint16_t kAvatarConfigMask = 0xff48;  // +0x5f0: 0x100/0x200 flaps, 0x400 airbrake, 0x800 gear,
                                                       // 0x1000 hook, 0x2000/0x4000 lights, 0x8000 wheel brakes;
                                                       // 0x8 drop tanks and 0x40 all stores jettisoned (T5)
size_t encode(const AvatarStateMsg& m, uint8_t* out, size_t cap);
Status decode(const uint8_t* data, size_t len, AvatarStateMsg& out);

/// <summary>The detect-interface mode for a radar block (state +0x970, stage +0x974, a target +0x988).</summary>
inline uint8_t radar_mode_of(int state, int stage, bool has_target) {
    if (state < 2 || state > 4) return 0;
    if (has_target && stage >= 3) return 3;
    if (has_target && stage >= 2) return 2;
    return 1;
}
/// <summary>Input channel 3 (0..1) as the wire's 0..255; out of range is clamped, not finite gives 0.</summary>
inline uint8_t throttle_to_wire(float v) {
    if (!(v > 0.0f)) return 0;  // also NaN
    if (v >= 1.0f) return 255;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}
inline float throttle_from_wire(uint8_t q) { return q / 255.0f; }
/// <summary>A throttle change worth sending: at least 2 steps (an axis' noise is not), or onto idle or full.</summary>
inline bool throttle_moved(uint8_t from, uint8_t to) {
    const int d = from > to ? from - to : to - from;
    return d >= 2 || (d == 1 && (to == 0 || to == 255));
}
/// <summary>True when what the receivers see would differ (seq and peer not compared).</summary>
inline bool avatar_state_differs(const AvatarStateMsg& a, const AvatarStateMsg& b) {
    return a.radar_mode != b.radar_mode || a.target_kind != b.target_kind || a.target_a != b.target_a ||
           a.target_b != b.target_b || a.config != b.config || a.engines != b.engines || a.guns != b.guns ||
           a.engine_count != b.engine_count ||
           throttle_moved(a.throttle, b.throttle);
}
/// <summary>Seconds between two sends that only move the throttle: at most 5 a second while it moves.</summary>
inline constexpr double kAvatarThrottleGap = 0.2;
/// <summary>
/// The owner's send rule: a change of anything but the throttle goes at once; a throttle move goes once
/// kAvatarThrottleGap has passed since the last send (since = seconds since then). Nothing goes without a change.
/// </summary>
inline bool avatar_state_due(const AvatarStateMsg& last, const AvatarStateMsg& now, double since) {
    if (!avatar_state_differs(last, now)) return false;
    AvatarStateMsg same = now;
    same.throttle = last.throttle;
    return avatar_state_differs(last, same) || since >= kAvatarThrottleGap;
}
/// <summary>The receiver's RWR call is due: a lock (mode 3) on a target that was not locked by this sender before.</summary>
inline bool avatar_lock_started(const AvatarStateMsg& prev, const AvatarStateMsg& now) {
    return now.radar_mode == 3 && now.target_kind != 0 &&
           (prev.radar_mode != 3 || prev.target_a != now.target_a || prev.target_b != now.target_b ||
            prev.target_kind != now.target_kind);
}

// ---- hits (Phase 5 T3): a shooter's claim, and the damage an owner applies ----

#pragma pack(push, 1)
/// <summary>
/// One hit of a gun round or an unguided store (bomb, rocket) as the game's damage interface took it (network-recon
/// §9.5): slot 4 kinetic (from, to), 5 directed (pos, velocity), 6 blast (pos). Positions are relative to the target as the sender saw it, in centimetres (about 327 m
/// either way); a directed hit's velocity is in 0.05 m/s. HitClaim goes from the shooter's PC to the host (the host
/// sets peer from the connection); HitApply from the host to the PC that owns the target (a player's jet), which
/// applies it to that jet with the shooter's copy as the attacker.
/// </summary>
struct HitMsg {
    Header h;
    uint16_t peer;  // HitClaim: the shooter's owner (set by the host); HitApply: 0
    uint16_t seq;
    uint8_t target_kind;  // KeyKind
    uint16_t target_a, target_b;
    uint8_t shooter_kind;  // KeyKind, 0 = unknown (HitClaim: the host uses the claimer's own jet)
    uint16_t shooter_a, shooter_b;
    uint8_t slot;     // damage interface slot 4, 5 or 6
    uint16_t weapon;  // hit_weapon_to_wire: a gun's data id, or 0x8000 | a store's weapon id
    uint32_t energy;  // joules
    uint32_t radius2;  // directed and blast: radius squared in cm^2
    int16_t p0[3];    // kinetic: from; directed and blast: the position
    int16_t p1[3];    // kinetic: to; directed: velocity (0.05 m/s); blast: 0
    uint8_t warhead;  // directed and blast
    uint8_t flag;     // directed: the last argument
};
#pragma pack(pop)
static_assert(sizeof(HitMsg) == 43);  // 4:2 peer, 6:2 seq, 8:5 target key, 13:5 shooter key, 18:1 slot, 19:2 weapon,
                                      // 21:4 energy, 25:4 r2, 29:6 p0, 35:6 p1, 41:1 warhead, 42:1 flag
size_t encode_hit(const HitMsg& m, WorldType t, uint8_t* out, size_t cap);
/// <summary>t is WorldType::HitClaim or HitApply; checks the slot and the size.</summary>
Status decode_hit(const uint8_t* data, size_t len, WorldType t, HitMsg& out);

/// <summary>The weapon id a gun round's hit carries (BulletObject: gun data id + 0xffff); lower ids are stores.</summary>
inline constexpr uint32_t kGunWeaponBase = 0xffff;
inline bool weapon_is_gun(uint32_t weapon) { return weapon >= kGunWeaponBase && weapon < kGunWeaponBase + 0x8000; }
inline constexpr uint16_t kHitStoreBit = 0x8000;
/// <summary>A damage call's weapon id on the wire: guns as their data id, stores (ids below 0x8000) with bit 15. False
/// for an id that fits neither.</summary>
inline bool hit_weapon_to_wire(uint32_t weapon, uint16_t* out) {
    if (weapon_is_gun(weapon)) {
        *out = static_cast<uint16_t>(weapon - kGunWeaponBase);
        return true;
    }
    if (weapon < kHitStoreBit) {
        *out = static_cast<uint16_t>(kHitStoreBit | weapon);
        return true;
    }
    return false;
}
inline uint32_t hit_weapon_from_wire(uint16_t w) {
    return (w & kHitStoreBit) ? static_cast<uint32_t>(w & ~kHitStoreBit) : kGunWeaponBase + w;
}
inline bool hit_is_store(const HitMsg& m) { return (m.weapon & kHitStoreBit) != 0; }
/// <summary>A relative offset in metres as the wire's centimetres (clamped; not finite gives 0).</summary>
inline int16_t hit_rel_to_wire(double m) {
    if (!(m == m) || m > 1e6 || m < -1e6) return 0;
    const double c = m * 100.0;
    return static_cast<int16_t>(c > 32767.0 ? 32767 : c < -32768.0 ? -32768 : (c < 0 ? c - 0.5 : c + 0.5));
}
inline double hit_rel_from_wire(int16_t c) { return c / 100.0; }


// ---- weapon launches (Phase 5 T4) ----

#pragma pack(push, 1)
/// <summary>
/// A player's weapon release (missile, bomb, rocket), as its PC saw it at MissileObject Release. The shooter and the
/// target are mission keys (target kind 0: none, or a placed target, flag kLaunchPlacedTarget). Position in cm (world),
/// angles as the missile holds them (2^-16 turns), velocities in 0.05 m/s (body: right, forward, up; and world).
/// weapon is the weapon type's name (ASCII, NUL padded): every PC finds its own id for it.
/// </summary>
struct WeaponLaunchMsg {
    Header h;
    uint16_t peer;  // the shooter's owner: 0 the host, k client k (the host sets it from the connection)
    uint16_t seq;   // per owner, increasing: the launch's id
    uint8_t shooter_kind;
    uint16_t shooter_a, shooter_b;
    uint8_t target_kind;
    uint16_t target_a, target_b;
    uint8_t station;  // the shooter's selected station
    uint8_t flags;    // kLaunch*
    uint32_t host_ms;  // the release in host game time
    int32_t pos[3];
    int16_t ang[3];
    int16_t bvel[3];
    int16_t wvel[3];
    char weapon[24];
    uint16_t weapon_id;  // the sender's own id for it: the host ties the sender's store-hit claims to this launch
};
#pragma pack(pop)
static_assert(sizeof(WeaponLaunchMsg) == 80);  // 4:2 peer, 6:2 seq, 8:5 shooter, 13:5 target, 18:1 station, 19:1 flags,
                                               // 20:4 ms, 24:12 pos, 36:6 ang, 42:6 bvel, 48:6 wvel, 54:24 weapon,
                                               // 78:2 weapon id
inline constexpr uint8_t kLaunchGuided = 0x01;         // the weapon has a seeker (GuidanceType != 0)
inline constexpr uint8_t kLaunchPlacedTarget = 0x04;   // aimed at placed scenery: not replicated as a target
size_t encode(const WeaponLaunchMsg& m, uint8_t* out, size_t cap);
Status decode(const uint8_t* data, size_t len, WeaponLaunchMsg& out);

/// <summary>Who flies a replicated launch where (the brief's authority model, network-recon §9.4 and §11.7).</summary>
enum class LaunchRole : uint8_t {
    Visual,      // a copy that does no damage (the shooter's avatar fires it, REMOTEOBJECT)
    LiveNative,  // a guided missile at our own jet: the avatar's own fire path launches it live
    LiveSpawn,   // a guided missile at an object the host owns, on the host: spawned and made live here
};
/// <summary>
/// The role of a launch on this PC. guided: the weapon has a seeker; target_is_us: the target is our own jet;
/// we_are_host and target_host_owned: the host, and the target is one of its objects (its AI, vehicles, ships, or
/// its own jet, not a client's avatar).
/// </summary>
inline LaunchRole launch_role(bool guided, bool target_is_us, bool we_are_host, bool target_host_owned) {
    if (!guided) return LaunchRole::Visual;
    if (target_is_us) return LaunchRole::LiveNative;
    if (we_are_host && target_host_owned) return LaunchRole::LiveSpawn;
    return LaunchRole::Visual;
}
/// <summary>What the receiver does with a launch: spawn a copy with the recipe, and make that copy live.</summary>
struct LaunchPlan {
    bool spawn = false;
    bool live = false;
};
/// <summary>
/// native_fired: the avatar's own fire path took the store (remote event 2). It launches unguided stores and guided
/// missiles at our jet itself; every other case is spawned, and a spawned copy is live whenever its role is.
/// </summary>
inline LaunchPlan launch_plan(LaunchRole role, bool guided, bool native_fired) {
    LaunchPlan p;
    p.spawn = !native_fired || (guided && role != LaunchRole::LiveNative);
    p.live = p.spawn && role != LaunchRole::Visual;
    return p;
}
/// <summary>The shooter's own missile does damage only on the PC that owns its target: false on the host for a
/// guided missile at a client's jet (that client flies the live one). A client's own weapons are already blocked
/// from damaging remote copies.</summary>
inline bool own_launch_stays_live(bool guided, bool we_are_host, bool target_is_client_jet) {
    return !(guided && we_are_host && target_is_client_jet);
}

// ---- stores and consumables (Phase 5 T5) ----

#pragma pack(push, 1)
/// <summary>One loaded station: the sender's weapon id and a 16-bit hash of the weapon's name (weapon_name_hash), so
/// a receiver whose ids differ finds its own; the count and the entry's state (0 empty, 1 untouched, 2 in use).</summary>
struct LoadoutEntry {
    uint8_t station;
    uint16_t weapon;
    uint16_t hash;
    uint8_t count;
    uint8_t state;
};
/// <summary>A player's loadout table (stations with a weapon), in parts of up to kLoadoutPart entries.</summary>
struct LoadoutMsg {
    Header h;
    uint16_t peer;  // the owner (the host sets it from the connection)
    uint16_t seq;
    uint8_t part;   // 0..parts-1
    uint8_t parts;
    uint8_t n;      // entries used in this part
    LoadoutEntry e[12];
};
/// <summary>A player's consumables: internal and external (drop tank) fuel in 4 kg steps, each fixed gun's rounds
/// (system order), chaff, flares.</summary>
struct ConsumablesMsg {
    Header h;
    uint16_t peer;
    uint16_t seq;
    uint16_t fuel;      // internal tanks, kg / 4
    uint16_t fuel_ext;  // external tanks, kg / 4
    uint8_t guns;   // entries used in rounds
    uint16_t rounds[4];
    uint8_t chaff, flares;
};
#pragma pack(pop)
static_assert(sizeof(LoadoutEntry) == 7);
static_assert(sizeof(LoadoutMsg) == 95);      // 4:2 peer, 6:2 seq, 8:1 part, 9:1 parts, 10:1 n, 11:84 entries
static_assert(sizeof(ConsumablesMsg) == 23);  // 4:2 peer, 6:2 seq, 8:2 fuel, 10:2 external fuel, 12:1 guns, 13:8 rounds,
                                              // 21:1 chaff, 22:1 flares
inline constexpr size_t kLoadoutPart = 12;
size_t encode(const LoadoutMsg& m, uint8_t* out, size_t cap);
/// <summary>Checks the size, part < parts and n <= kLoadoutPart.</summary>
Status decode(const uint8_t* data, size_t len, LoadoutMsg& out);
size_t encode(const ConsumablesMsg& m, uint8_t* out, size_t cap);
/// <summary>Checks the size and guns <= 4.</summary>
Status decode(const uint8_t* data, size_t len, ConsumablesMsg& out);

/// <summary>FNV-1a of an ASCII weapon name, folded to 16 bits; never 0.</summary>
inline uint16_t weapon_name_hash(const char* s) {
    uint32_t h = 0x811C9DC5u;
    for (; s && *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
    const uint16_t f = static_cast<uint16_t>(h ^ (h >> 16));
    return f ? f : 1;
}
inline uint16_t fuel_to_wire(float kg) {
    if (!(kg > 0.0f)) return 0;
    const float q = kg / 4.0f + 0.5f;
    return q >= 65535.0f ? 65535 : static_cast<uint16_t>(q);
}
inline float fuel_from_wire(uint16_t q) { return q * 4.0f; }

/// <summary>
/// What the receiver does with one station. Keep: it matches. Rebuild: the game's rearm rebuilds it with the owner's
/// weapon and count (another weapon, a rearm, or stores the avatar kept that the owner used). Empty: the owner has none
/// left and the avatar still has some (the rearm cannot empty a station; noted). GiveUp: this very rebuild was tried
/// and did not take (a weapon the avatar's station cannot carry): it is not retried until the owner's state changes.
/// Skip: the avatar (a fallback airframe) has no such station.
/// </summary>
enum class StationStep : uint8_t { Keep, Rebuild, Empty, GiveUp, Skip };
inline StationStep station_step(int32_t avatar_weapon, int avatar_count, int32_t owner_weapon, int owner_count,
                                bool tried_same, bool has_station = true) {
    if (owner_count <= 0 && avatar_count <= 0) return StationStep::Keep;
    if (avatar_weapon == owner_weapon && avatar_count == owner_count) return StationStep::Keep;
    if (!has_station) return StationStep::Skip;
    if (owner_count <= 0) return StationStep::Empty;
    if (tried_same) return StationStep::GiveUp;
    return StationStep::Rebuild;
}
/// <summary>The factor each avatar tank's fuel is scaled by so that its total (internal or external, each on its own)
/// becomes the owner's (1 when it cannot); each tank is then clamped to the most it has held.</summary>
inline float fuel_scale(float avatar_total, float owner_total) {
    if (!(avatar_total > 1.0f) || !(owner_total >= 0.0f)) return 1.0f;
    const float k = owner_total / avatar_total;
    return k > 10.0f ? 10.0f : k;
}

// ---- damage, death, ejection and countermeasures (Phase 5 T6) ----

#pragma pack(push, 1)
/// <summary>
/// A player's jet as its owner sees it: damage level 0..5, flags (kDmg*), and per component (index < 64) lost and
/// showing damage, per system (in component order, index < 64) destroyed; the last attacker by mission key.
/// </summary>
struct DamageStateMsg {
    Header h;
    uint16_t peer;
    uint16_t seq;
    uint8_t level;
    uint8_t flags;
    uint8_t components;  // how many the bits describe
    uint8_t systems;
    uint64_t lost;
    uint64_t shown;
    uint64_t dead;
    uint8_t attacker_kind;
    uint16_t attacker_a, attacker_b;
    uint32_t attacker_weapon;  // the owner's +0x31c (its id space), for the host's kill record
    uint16_t life;             // the owner's player epoch: a new flight or respawn is a new life
};
/// <summary>Chaff and flares a player released since its last message (its dispensers' counts fell by these).</summary>
struct CountermeasureMsg {
    Header h;
    uint16_t peer;
    uint16_t seq;
    uint8_t chaff, flares;
};
#pragma pack(pop)
static_assert(sizeof(DamageStateMsg) == 47);  // 4:2 peer, 6:2 seq, 8:1 level, 9:1 flags, 10:1 comps, 11:1 systems,
                                              // 12:8 lost, 20:8 shown, 28:8 dead, 36:5 attacker, 41:4 weapon, 45:2 life
static_assert(sizeof(CountermeasureMsg) == 10);
inline constexpr uint8_t kDmgFire = 0x01, kDmgPilotGone = 0x02, kDmgDestroyed = 0x04;
size_t encode(const DamageStateMsg& m, uint8_t* out, size_t cap);
Status decode(const uint8_t* data, size_t len, DamageStateMsg& out);
size_t encode(const CountermeasureMsg& m, uint8_t* out, size_t cap);
Status decode(const uint8_t* data, size_t len, CountermeasureMsg& out);

/// <summary>True when receivers would see a difference (seq, peer and the attacker alone do not count).</summary>
inline bool damage_differs(const DamageStateMsg& a, const DamageStateMsg& b) {
    return a.level != b.level || a.flags != b.flags || a.lost != b.lost || a.shown != b.shown || a.dead != b.dead;
}

/// <summary>What a receiver does to mirror a new damage state on the avatar, in this order: the attacker, parts
/// lost (3), parts showing damage (0xc), systems destroyed (4), the level (8), fire and pilot bits, then the
/// destroy (0xb, never the kill event: only the owner's and the host's records count).</summary>
struct DamagePlan {
    uint64_t lose = 0, show = 0, kill_systems = 0;
    bool set_level = false;
    uint8_t level = 0;
    bool fire_on = false, fire_off = false, pilot_gone = false, destroy = false;
};
inline DamagePlan damage_plan(const DamageStateMsg& was, const DamageStateMsg& now) {
    DamagePlan p;
    p.lose = now.lost & ~was.lost;
    p.show = now.shown & ~was.shown & ~now.lost;
    p.kill_systems = now.dead & ~was.dead;
    const bool destroyed = (now.flags & kDmgDestroyed) || now.level >= 5;
    p.destroy = destroyed && !((was.flags & kDmgDestroyed) || was.level >= 5);
    p.set_level = !destroyed && now.level > was.level;
    p.level = now.level;
    p.fire_on = (now.flags & kDmgFire) && !(was.flags & kDmgFire);
    p.fire_off = !(now.flags & kDmgFire) && (was.flags & kDmgFire);
    p.pilot_gone = (now.flags & kDmgPilotGone) && !(was.flags & kDmgPilotGone);
    return p;
}
/// <summary>
/// A receiver's record of one peer's jet. A life (the owner's player epoch) ends at its destruction: later states of
/// that life (the owner resends every 10 s) are not applied to anything, so a retaken aircraft is never destroyed by
/// them, and no aircraft is taken again for the peer until a new life starts. Within a life, another aircraft (a
/// retake after a park) starts undamaged and gets the whole state again.
/// </summary>
class PeerDamage {
public:
    struct Step {
        bool apply = false;
        DamagePlan plan;
    };
    /// <summary>avatar: the aircraft showing the peer (may be null: only the record is kept); avatar_destroyed: its own
    /// damage level is already 5 (mirrored part losses can destroy it first), so no second destroy is sent.</summary>
    Step on_state(const DamageStateMsg& m, const void* avatar, bool avatar_destroyed) {
        Step s;
        if (!have_ || m.life != life_) {
            have_ = true;
            life_ = m.life;
            dead_ = false;
            avatar_ = nullptr;
            last_ = DamageStateMsg{};
        }
        if (dead_) return s;
        if (avatar != avatar_) {
            avatar_ = avatar;
            last_ = DamageStateMsg{};
        }
        const bool destroyed = (m.flags & kDmgDestroyed) || m.level >= 5;
        if (!avatar) {
            if (destroyed) dead_ = true;
            return s;
        }
        s.plan = damage_plan(last_, m);
        if (s.plan.destroy && avatar_destroyed) s.plan.destroy = false;
        last_ = m;
        if (destroyed) dead_ = true;
        s.apply = true;
        return s;
    }
    bool dead() const { return have_ && dead_; }
    void reset() { *this = PeerDamage{}; }

private:
    bool have_ = false, dead_ = false;
    uint16_t life_ = 0;
    const void* avatar_ = nullptr;
    DamageStateMsg last_{};
};

/// <summary>
/// Chaff and flares to send for a fall in the owner's counts: only while it is alive and its dispensers work, and at
/// most cap at once (a bigger fall, or one at death or a dispenser's destruction, is a reset, not a release).
/// </summary>
inline uint8_t cm_released(int before, int now, bool alive, bool dispenser_ok, int cap = 8) {
    const int d = before - now;
    if (!alive || !dispenser_ok || d <= 0 || d > cap) return 0;
    return static_cast<uint8_t>(d);
}

/// <summary>
/// The game's kill event (mission event 1, an aircraft's slot 30) is let through on the host for every aircraft (the
/// single kill record), and on a client only for its own jet: a REMOTEOBJECT copy's death there (mirrored, or from
/// the world stream) must not make a kill entry or move mission success.
/// </summary>
inline bool kill_event_allowed(bool client_join, bool remote_copy) { return !(client_join && remote_copy); }
/// <summary>What the kill-event hooks do with a call: the game calls the kill event every frame for a dead object
/// until its +0x168 bit 0x80000000 is set, so a call with the bit set is a repeat (nothing to log, nothing to count);
/// otherwise block it (and set the bit) on a joined client for a remote copy, else let the game log it.</summary>
enum class KillHook : uint8_t { Repeat, Block, Log };
inline KillHook kill_hook_action(bool already_logged, bool client_join, bool remote_copy) {
    if (already_logged) return KillHook::Repeat;
    return kill_event_allowed(client_join, remote_copy) ? KillHook::Log : KillHook::Block;
}

/// <summary>Releases since the last count: what fell, clamped to a byte; a rise (a rearm) is none.</summary>
inline uint8_t released(int before, int now) {
    const int d = before - now;
    return static_cast<uint8_t>(d <= 0 ? 0 : d > 255 ? 255 : d);
}

// ---- the mission clock (handover task 1) ----

#pragma pack(push, 1)
/// <summary>The world owner's mission clocks (MSN +0x10, the elapsed mission time the formations follow, and +0x14) in
/// ms, at the owner's game time owner_ms (host time).</summary>
struct MissionClockMsg {
    Header h;
    uint16_t peer;  // the owner
    uint16_t seq;
    uint32_t owner_ms;
    uint32_t clock_ms;
    uint32_t clock2_ms;
};
#pragma pack(pop)
static_assert(sizeof(MissionClockMsg) == 20);  // 4:2 peer, 6:2 seq, 8:4 owner ms, 12:4 clock, 16:4 second clock
size_t encode(const MissionClockMsg& m, uint8_t* out, size_t cap);
Status decode(const uint8_t* data, size_t len, MissionClockMsg& out);
inline uint32_t clock_to_wire(float s) { return s > 0.0f ? static_cast<uint32_t>(static_cast<double>(s) * 1000.0 + 0.5) : 0; }

/// <summary>Corrections smaller than this are left to drift (the clocks run at the same game rate).</summary>
inline constexpr double kClockTolerance = 0.25;
/// <summary>
/// The mission clock a receiver should have now: the owner's, plus the one-way latency (half the round trip, clamped
/// to 0..1 s), the owner's decision "client time = owner time + RTT/2". Only game-time values are compared: a pause on
/// either side just shows as one correction when the clocks run again. set is false within the tolerance.
/// </summary>
struct ClockFix {
    bool set = false;
    double target = 0, target2 = 0;
};
inline ClockFix clock_fix(double local, const MissionClockMsg& m, double latency) {
    ClockFix f;
    if (!(latency > 0)) latency = 0;
    if (latency > 1.0) latency = 1.0;
    f.target = m.clock_ms / 1000.0 + latency;
    f.target2 = m.clock2_ms / 1000.0 + latency;
    const double d = f.target - local;
    f.set = d > kClockTolerance || d < -kClockTolerance;
    return f;
}

// ---- seat takeover in place (handover task 3) ----

#pragma pack(push, 1)
/// <summary>
/// The host's current state of the aircraft in a client's seat, sent after each Welcome: on a late join or a
/// reconnect the client's jet takes it over in place (pose, velocity, fuel, stores) instead of starting where the
/// mission put it. pose.kind/a/b is the seat's key.
/// </summary>
struct SeatStateMsg {
    Header h;
    uint8_t seat;
    uint8_t flags;  // kSeat*
    uint32_t host_ms;
    WorldObjState pose;
    uint16_t fuel, fuel_ext;  // kg / 4
    uint8_t damage;
    uint8_t n;
    LoadoutEntry e[12];
    uint32_t state;  // the seat's +0x5f0 masked to kSeatStateMask (airborne, gear, flaps, lights...)
};
#pragma pack(pop)
static_assert(sizeof(SeatStateMsg) == 139);  // 4:1 seat, 5:1 flags, 6:4 ms, 10:35 pose, 45:4 fuel, 49:1 damage, 50:1 n,
                                             // 51:84 entries, 135:4 state
inline constexpr uint32_t kSeatStateMask = 0x0080ff00u;
inline constexpr uint8_t kSeatAlive = 0x01, kSeatAirborne = 0x02;
size_t encode(const SeatStateMsg& m, uint8_t* out, size_t cap);
/// <summary>Checks the size and n <= 12.</summary>
Status decode(const uint8_t* data, size_t len, SeatStateMsg& out);

/// <summary>Metres between the client's jet and the seat's state below which it is already there (a reconnect).</summary>
inline constexpr double kSeatTakeoverMin = 50.0;
/// <summary>
/// Whether the client's jet takes the seat's state: the seat aircraft lives on the host and the jet is not already
/// there. Joining a world the host owns, only a flying seat is taken over (a parked one is where the mission put ours).
/// A client that loaded first ran the world itself and offered it (handover): when the host took it, the client's own
/// jet is the truth; when the host refused it (it owns the world), the seat is taken over on the ground too.
/// </summary>
inline bool seat_takeover_due(const SeatStateMsg& m, double dist, bool we_owned_the_world = false,
                              bool host_refused_our_world = false) {
    if (we_owned_the_world && !host_refused_our_world) return false;
    return (m.flags & kSeatAlive) && ((m.flags & kSeatAirborne) || host_refused_our_world) && dist >= kSeatTakeoverMin;
}

/// <summary>Reassembles a mission from an offer and its chunks (any order, duplicates ignored).</summary>
class MissionAssembly {
public:
    enum class Add { Ok, Done, Duplicate, Bad };
    /// <summary>Starts over for an offer; false (and empty) for a bad or non-Ok offer.</summary>
    bool begin(const MissionOfferMsg& offer);
    Add add(const MissionChunkHead& head, const uint8_t* payload);
    bool active() const { return active_; }
    bool complete() const { return active_ && have_ == count_; }
    size_t received() const { return have_; }
    size_t count() const { return count_; }
    const MissionOfferMsg& offer() const { return offer_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    bool active_ = false;
    MissionOfferMsg offer_{};
    std::vector<uint8_t> bytes_;
    std::vector<uint8_t> got_;
    size_t have_ = 0, count_ = 0;
};

}  // namespace LazyWorks::SF2Patch::net
