// Wire protocol shared by the mod (host) and the synthetic client (bot).
// No game or Windows dependencies. All fields are little-endian, packed, and exactly sized.
#pragma once

#include <cstddef>
#include <cstdint>

namespace LazyWorks::SF2Patch::net {

inline constexpr uint16_t kMagic = 0x5A4C;  // bytes 'L','Z' on the wire (little-endian)
inline constexpr uint8_t kVersion = 2;  // v2 (Phase 4) is a clean break from v1: a v1 peer is refused with BadVersion

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
};
struct WelcomeMsg {
    Header h;
    uint16_t client_id;
    double spawn_pos[3];  // the client's join position
    float spawn_heading;
    double host_game_time;
    float spawn_vel[3];
    uint32_t theatre_hash;
};
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
    uint16_t client_id;
    uint8_t reason;
};
struct PeerInfoMsg {
    Header h;
    uint16_t id;  // 0 = the host, 1..kMaxClients = a client
    char callsign[16];
    char aircraft[32];
};
struct PeerLeftMsg {
    Header h;
    uint16_t id;
    uint8_t reason;
};
#pragma pack(pop)

// Wire layout (offset:size), all little-endian. Header: 0:2 magic, 2:1 version, 3:1 type.
static_assert(sizeof(Header) == 4);
static_assert(sizeof(HelloMsg) == 56);    // 4:16 callsign, 20:32 aircraft, 52:4 theatre hash
static_assert(sizeof(WelcomeMsg) == 58);  // 4:2 id, 6:24 pos, 30:4 heading, 34:8 host time, 42:12 vel, 54:4 theatre hash
static_assert(sizeof(PingMsg) == 12);     // 4:8 client send time
static_assert(sizeof(PongMsg) == 20);     // 4:8 client send time, 12:8 host time
static_assert(sizeof(StateMsg) == 78);    // 4:2 id, 6:4 seq, 10:8 time, 18:24 pos, 42:12 vel, 54:12 ang, 66:12 rates
static_assert(sizeof(ByeMsg) == 7);       // 4:2 id, 6:1 reason
static_assert(sizeof(PeerInfoMsg) == 54); // 4:2 id, 6:16 callsign, 22:32 aircraft
static_assert(sizeof(PeerLeftMsg) == 7);  // 4:2 id, 6:1 reason

/// <summary>Bye reasons added in v2.</summary>
inline constexpr uint8_t kByeWrongTheatre = 5;
inline constexpr uint8_t kByeWrongVersion = 6;

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
