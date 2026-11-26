// Wire protocol shared by the mod (host) and the synthetic client (bot).
// No game or Windows dependencies. All fields are little-endian, packed, and exactly sized.
#pragma once

#include <cstddef>
#include <cstdint>

namespace LazyWorks::SF2Patch::net {

inline constexpr uint16_t kMagic = 0x5A4C;  // bytes 'L','Z' on the wire (little-endian)
// v3 adds the shared-world messages (world_proto.h). Each version is a clean break: an older peer is refused with
// BadVersion.
inline constexpr uint8_t kVersion = 3;

/// <summary>ENet channels.</summary>
inline constexpr uint8_t kChannelReliable = 0;    // Hello, Welcome, PeerInfo, PeerLeft, Bye
inline constexpr uint8_t kChannelUnreliable = 1;  // State, Ping, Pong (unreliable, sequenced)
inline constexpr uint8_t kChannelCount = 2;

enum class MsgType : uint8_t {
    Hello = 1,
    Welcome = 2,
    Ping = 3,
    Pong = 4,
    State = 5,
    Bye = 6,
    PeerInfo = 7,  // host to client only
    PeerLeft = 8,  // host to client only
};

enum class Status {
    Ok = 0,
    Truncated,   // fewer bytes than the message needs
    Oversized,   // more bytes than the message needs
    BadMagic,
    BadVersion,
    BadType,     // unknown type, or not the type this decoder handles
};

#pragma pack(push, 1)
struct Header {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
};
struct HelloMsg {
    Header h;
    char callsign[16];
    char aircraft[32];
    uint32_t theatre_hash;  // FNV-1a of the theatre id, 0 = unknown
    uint32_t world_tag;     // shared world: the tag the client's MissionRequest carried (its seat key), 0 = none
};
struct WelcomeMsg {
    Header h;
    uint16_t client_id;
    double spawn_pos[3];  // the client's join position
    float spawn_heading;
    double host_game_time;
    float spawn_vel[3];
    uint32_t theatre_hash;
    uint32_t mission_id;  // shared world: the host's mission (world_proto.h), 0 = none
    uint8_t seat;         // shared world: the member of the host's flight the client flies, kNoSeat = none
    uint8_t host_seat;    // ... and the one the host flies
};
inline constexpr uint8_t kNoSeat = 0xff;
struct PingMsg {
    Header h;
    double client_send_time;
};
struct PongMsg {
    Header h;
    double client_send_time;
    double host_game_time;
};
struct StateMsg {
    Header h;
    uint16_t client_id;
    uint32_t seq;
    double game_time;  // in host time
    double pos[3];
    float vel[3];
    float ang[3];
    float rates[3];
};
struct ByeMsg {
    Header h;
    uint16_t client_id;  // the sender, as in State: 0 from the host (always, also before a Welcome), its own id from a client
    uint8_t reason;
};
struct PeerInfoMsg {
    Header h;
    uint16_t id;  // 0 = the host, 1..kMaxClients = a client
    char callsign[16];
    char aircraft[32];
    uint8_t seat;  // shared world: that peer's member of the host's flight, kNoSeat = none
};
struct PeerLeftMsg {
    Header h;
    uint16_t id;
    uint8_t reason;
};
#pragma pack(pop)

// Wire layout (offset:size), all little-endian. Header: 0:2 magic, 2:1 version, 3:1 type.
static_assert(sizeof(Header) == 4);
static_assert(sizeof(HelloMsg) == 60);    // 4:16 callsign, 20:32 aircraft, 52:4 theatre hash, 56:4 world tag
static_assert(sizeof(WelcomeMsg) == 64);  // 4:2 id, 6:24 pos, 30:4 heading, 34:8 host time, 42:12 vel, 54:4 theatre hash,
                                          // 58:4 mission id, 62:1 seat, 63:1 host seat
static_assert(sizeof(PingMsg) == 12);     // 4:8 client send time
static_assert(sizeof(PongMsg) == 20);     // 4:8 client send time, 12:8 host time
static_assert(sizeof(StateMsg) == 78);    // 4:2 id, 6:4 seq, 10:8 time, 18:24 pos, 42:12 vel, 54:12 ang, 66:12 rates
static_assert(sizeof(ByeMsg) == 7);       // 4:2 id, 6:1 reason
static_assert(sizeof(PeerInfoMsg) == 55); // 4:2 id, 6:16 callsign, 22:32 aircraft, 54:1 seat
static_assert(sizeof(PeerLeftMsg) == 7);  // 4:2 id, 6:1 reason

/// <summary>
/// The two-step join (a Phase 4 protocol rule). A host that does not yet know where its own jet should be relative to
/// the client (the bot's --serve without --start places its jet from the client's first State) sends a first Welcome
/// with no join position: spawn_pos, spawn_heading and spawn_vel all zero. Once the client's States arrive, the host
/// sends the same client a second Welcome (same id) that carries the join position, no sooner than kReWelcomeMinGap
/// after the client could have received the first. The client:
///  - must not move its jet on a Welcome for which welcome_has_join() is false;
///  - moves on a later Welcome that has one (once per join position);
///  - treats every Welcome as re-seeding the shared clock (a ClockSync reset), so its remote jets are reset.
/// A real host whose own jet is stationary would also send a zero velocity: Phase 4 joins are air starts only (the
/// client moves only while airborne, plan 2.7). Use an explicit marker when the protocol next changes.
/// </summary>
inline bool welcome_has_join(const WelcomeMsg& w) {
    return w.spawn_vel[0] != 0.0f || w.spawn_vel[1] != 0.0f || w.spawn_vel[2] != 0.0f;
}

/// <summary>
/// A client drops a re-Welcome that arrives sooner than this after the previous one (seconds; ClientCore). A host that
/// re-Welcomes leaves at least this, plus a margin for delivery, between them.
/// </summary>
inline constexpr double kReWelcomeMinGap = 1.0;

/// <summary>PeerLeft reasons (the same codes as the recording's LeaveReason).</summary>
inline constexpr uint8_t kPeerLeftLeft = 0;     // Bye or disconnect
inline constexpr uint8_t kPeerLeftTimeout = 1;  // no packets for 5 s
inline constexpr uint8_t kPeerLeftRemoved = 2;  // the host ended it (avatar lost, the host's flight ended)

/// <summary>Bye reasons added in v2.</summary>
inline constexpr uint8_t kByeWrongTheatre = 5;
inline constexpr uint8_t kByeWrongVersion = 6;
/// <summary>Bye reasons added in v3 (shared world).</summary>
inline constexpr uint8_t kByeWorldBehind = 7;   // host: this client's world events could not be queued
inline constexpr uint8_t kByeWrongMission = 8;  // client: it flies another mission than the host's

/// <summary>A Bye reason in words, for log lines (the host's reasons; any number is safe).</summary>
inline const char* bye_reason_name(uint8_t reason) {
    switch (reason) {
        case 0: return "no reason given";
        case 1: return "the host is busy (a puppet or replay run, or no room)";
        case 2: return "the host has no free friendly AI aircraft for our avatar";
        case 3: return "the host's session ended";
        case 4: return "the host lost our avatar";
        case kByeWrongTheatre: return "another theatre";
        case kByeWrongVersion: return "another protocol version";
        case kByeWorldBehind: return "the client could not keep up with the world events";
        case kByeWrongMission: return "another mission than the host's";
        default: return "unknown reason";
    }
}

/// <summary>The largest message. HostCore's queues carry at most this many bytes per message.</summary>
inline constexpr size_t kMaxMessage = 96;
static_assert(sizeof(HelloMsg) <= kMaxMessage && sizeof(WelcomeMsg) <= kMaxMessage && sizeof(StateMsg) <= kMaxMessage &&
              sizeof(PeerInfoMsg) <= kMaxMessage && sizeof(PeerLeftMsg) <= kMaxMessage && sizeof(ByeMsg) <= kMaxMessage);

/// <summary>Encoders fill in the header and write the message to out. They return the byte count, or 0 if cap is too small.</summary>
size_t encode(const HelloMsg& m, uint8_t* out, size_t cap);
size_t encode(const WelcomeMsg& m, uint8_t* out, size_t cap);
size_t encode(const PingMsg& m, uint8_t* out, size_t cap);
size_t encode(const PongMsg& m, uint8_t* out, size_t cap);
size_t encode(const StateMsg& m, uint8_t* out, size_t cap);
size_t encode(const ByeMsg& m, uint8_t* out, size_t cap);
size_t encode(const PeerInfoMsg& m, uint8_t* out, size_t cap);
size_t encode(const PeerLeftMsg& m, uint8_t* out, size_t cap);

/// <summary>Reads the type of a packet after checking length, magic and version.</summary>
Status peek_type(const uint8_t* data, size_t len, MsgType& type);

/// <summary>
/// Decoders require the exact message size, magic, version and type. out is only written on Ok.
/// Hello and PeerInfo strings are always NUL-terminated after decoding.
/// </summary>
Status decode(const uint8_t* data, size_t len, HelloMsg& out);
Status decode(const uint8_t* data, size_t len, WelcomeMsg& out);
Status decode(const uint8_t* data, size_t len, PingMsg& out);
Status decode(const uint8_t* data, size_t len, PongMsg& out);
Status decode(const uint8_t* data, size_t len, StateMsg& out);
Status decode(const uint8_t* data, size_t len, ByeMsg& out);
Status decode(const uint8_t* data, size_t len, PeerInfoMsg& out);
Status decode(const uint8_t* data, size_t len, PeerLeftMsg& out);

/// <summary>Copies at most N-1 bytes of src into a fixed field and always NUL-terminates it (zero-fills the rest).</summary>
template <size_t N>
void set_str(char (&dst)[N], const char* src) {
    size_t i = 0;
    if (src)
        for (; i + 1 < N && src[i]; ++i) dst[i] = src[i];
    for (; i < N; ++i) dst[i] = '\0';
}

const char* status_name(Status s);

/// <summary>
/// Round-trips every message and rejects bad input. Returns true on success; on failure writes a
/// description to *why if non-null.
/// </summary>
bool protocol_selftest(const char** why = nullptr);

}  // namespace LazyWorks::SF2Patch::net
