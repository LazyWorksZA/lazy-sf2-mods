// Wire protocol shared by the mod (host) and the synthetic client (bot).
// No game or Windows dependencies. All fields are little-endian, packed, and exactly sized.
#pragma once

#include <cstddef>
#include <cstdint>

namespace lazysf2::net {

inline constexpr uint16_t kMagic = 0x5A4C;  // bytes 'L','Z' on the wire (little-endian)
inline constexpr uint8_t kVersion = 1;

// ENet channels.
inline constexpr uint8_t kChannelReliable = 0;    // Hello, Welcome, Bye
inline constexpr uint8_t kChannelUnreliable = 1;  // State, Ping, Pong (unreliable, sequenced)
inline constexpr uint8_t kChannelCount = 2;

enum class MsgType : uint8_t {
    Hello = 1,
    Welcome = 2,
    Ping = 3,
    Pong = 4,
    State = 5,
    Bye = 6,
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
};
struct WelcomeMsg {
    Header h;
    uint16_t client_id;
    double spawn_pos[3];
    float spawn_heading;
    double host_game_time;
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
#pragma pack(pop)

// Wire layout (offset:size), all little-endian. Header: 0:2 magic, 2:1 version, 3:1 type.
static_assert(sizeof(Header) == 4);
static_assert(sizeof(HelloMsg) == 52);    // 4:16 callsign, 20:32 aircraft
static_assert(sizeof(WelcomeMsg) == 42);  // 4:2 id, 6:24 pos, 30:4 heading, 34:8 host time
static_assert(sizeof(PingMsg) == 12);     // 4:8 client send time
static_assert(sizeof(PongMsg) == 20);     // 4:8 client send time, 12:8 host time
static_assert(sizeof(StateMsg) == 78);    // 4:2 id, 6:4 seq, 10:8 time, 18:24 pos, 42:12 vel, 54:12 ang, 66:12 rates
static_assert(sizeof(ByeMsg) == 7);       // 4:2 id, 6:1 reason

// Encoders fill in the header and write the message to out. They return the byte count, or 0 if cap is too small.
size_t encode(const HelloMsg& m, uint8_t* out, size_t cap);
size_t encode(const WelcomeMsg& m, uint8_t* out, size_t cap);
size_t encode(const PingMsg& m, uint8_t* out, size_t cap);
size_t encode(const PongMsg& m, uint8_t* out, size_t cap);
size_t encode(const StateMsg& m, uint8_t* out, size_t cap);
size_t encode(const ByeMsg& m, uint8_t* out, size_t cap);

// Reads the type of a packet after checking length, magic and version.
Status peek_type(const uint8_t* data, size_t len, MsgType& type);

// Decoders require the exact message size, magic, version and type. out is only written on Ok.
// Hello strings are always NUL-terminated after decoding.
Status decode(const uint8_t* data, size_t len, HelloMsg& out);
Status decode(const uint8_t* data, size_t len, WelcomeMsg& out);
Status decode(const uint8_t* data, size_t len, PingMsg& out);
Status decode(const uint8_t* data, size_t len, PongMsg& out);
Status decode(const uint8_t* data, size_t len, StateMsg& out);
Status decode(const uint8_t* data, size_t len, ByeMsg& out);

const char* status_name(Status s);

// Round-trips every message and rejects bad input. Returns true on success; on failure writes a
// description to *why if non-null.
bool protocol_selftest(const char** why = nullptr);

}  // namespace lazysf2::net
