#include "net/protocol.h"

#include <bit>
#include <cstring>

namespace LazyWorks::SF2Patch::net {

static_assert(std::endian::native == std::endian::little, "the wire format is little-endian and copied raw");

namespace {

template <class T>
size_t encode_impl(const T& in, MsgType t, uint8_t* out, size_t cap) {
    if (!out || cap < sizeof(T)) return 0;
    T m = in;
    m.h.magic = kMagic;
    m.h.version = kVersion;
    m.h.type = static_cast<uint8_t>(t);
    std::memcpy(out, &m, sizeof(T));
    return sizeof(T);
}

Status check_header(const uint8_t* data, size_t len, size_t want, MsgType t) {
    if (!data || len < sizeof(Header)) return Status::Truncated;
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic) return Status::BadMagic;
    if (h.version != kVersion) return Status::BadVersion;
    if (h.type != static_cast<uint8_t>(t)) return Status::BadType;
    if (len < want) return Status::Truncated;
    if (len > want) return Status::Oversized;
    return Status::Ok;
}

template <class T>
Status decode_impl(const uint8_t* data, size_t len, MsgType t, T& out) {
    Status s = check_header(data, len, sizeof(T), t);
    if (s != Status::Ok) return s;
    std::memcpy(&out, data, sizeof(T));
    return Status::Ok;
}

size_t size_of(MsgType t) {
    switch (t) {
        case MsgType::Hello: return sizeof(HelloMsg);
        case MsgType::Welcome: return sizeof(WelcomeMsg);
        case MsgType::Ping: return sizeof(PingMsg);
        case MsgType::Pong: return sizeof(PongMsg);
        case MsgType::State: return sizeof(StateMsg);
        case MsgType::Bye: return sizeof(ByeMsg);
        case MsgType::PeerInfo: return sizeof(PeerInfoMsg);
        case MsgType::PeerLeft: return sizeof(PeerLeftMsg);
        case MsgType::AuthChallenge: return sizeof(AuthChallengeMsg);
        case MsgType::AuthAnswer: return sizeof(AuthAnswerMsg);
        case MsgType::AuthProof: return sizeof(AuthProofMsg);
    }
    return 0;
}

}  // namespace

size_t encode(const HelloMsg& m, uint8_t* o, size_t c) { return encode_impl(m, MsgType::Hello, o, c); }
size_t encode(const WelcomeMsg& m, uint8_t* o, size_t c) { return encode_impl(m, MsgType::Welcome, o, c); }
size_t encode(const PingMsg& m, uint8_t* o, size_t c) { return encode_impl(m, MsgType::Ping, o, c); }
size_t encode(const PongMsg& m, uint8_t* o, size_t c) { return encode_impl(m, MsgType::Pong, o, c); }
size_t encode(const StateMsg& m, uint8_t* o, size_t c) { return encode_impl(m, MsgType::State, o, c); }
size_t encode(const ByeMsg& m, uint8_t* o, size_t c) { return encode_impl(m, MsgType::Bye, o, c); }
size_t encode(const PeerInfoMsg& m, uint8_t* o, size_t c) { return encode_impl(m, MsgType::PeerInfo, o, c); }
size_t encode(const PeerLeftMsg& m, uint8_t* o, size_t c) { return encode_impl(m, MsgType::PeerLeft, o, c); }
size_t encode(const AuthChallengeMsg& m, uint8_t* o, size_t c) { return encode_impl(m, MsgType::AuthChallenge, o, c); }
size_t encode(const AuthAnswerMsg& m, uint8_t* o, size_t c) { return encode_impl(m, MsgType::AuthAnswer, o, c); }
size_t encode(const AuthProofMsg& m, uint8_t* o, size_t c) { return encode_impl(m, MsgType::AuthProof, o, c); }

Status peek_type(const uint8_t* data, size_t len, MsgType& type) {
    if (!data || len < sizeof(Header)) return Status::Truncated;
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic) return Status::BadMagic;
    if (h.version != kVersion) return Status::BadVersion;
    if (h.type < static_cast<uint8_t>(MsgType::Hello) || h.type > static_cast<uint8_t>(MsgType::AuthProof))
        return Status::BadType;
    const MsgType t = static_cast<MsgType>(h.type);
    if (len < size_of(t)) return Status::Truncated;
    if (len > size_of(t)) return Status::Oversized;
    type = t;
    return Status::Ok;
}

Status decode(const uint8_t* d, size_t n, HelloMsg& out) {
    HelloMsg m;
    Status s = decode_impl(d, n, MsgType::Hello, m);
    if (s != Status::Ok) return s;
    m.callsign[sizeof(m.callsign) - 1] = '\0';
    m.aircraft[sizeof(m.aircraft) - 1] = '\0';
    out = m;
    return Status::Ok;
}
Status decode(const uint8_t* d, size_t n, WelcomeMsg& o) { return decode_impl(d, n, MsgType::Welcome, o); }
Status decode(const uint8_t* d, size_t n, PingMsg& o) { return decode_impl(d, n, MsgType::Ping, o); }
Status decode(const uint8_t* d, size_t n, PongMsg& o) { return decode_impl(d, n, MsgType::Pong, o); }
Status decode(const uint8_t* d, size_t n, StateMsg& o) { return decode_impl(d, n, MsgType::State, o); }
Status decode(const uint8_t* d, size_t n, ByeMsg& o) { return decode_impl(d, n, MsgType::Bye, o); }
Status decode(const uint8_t* d, size_t n, PeerInfoMsg& out) {
    PeerInfoMsg m;
    Status s = decode_impl(d, n, MsgType::PeerInfo, m);
    if (s != Status::Ok) return s;
    m.callsign[sizeof(m.callsign) - 1] = '\0';
    m.aircraft[sizeof(m.aircraft) - 1] = '\0';
    out = m;
    return Status::Ok;
}
Status decode(const uint8_t* d, size_t n, PeerLeftMsg& o) { return decode_impl(d, n, MsgType::PeerLeft, o); }
Status decode(const uint8_t* d, size_t n, AuthChallengeMsg& o) { return decode_impl(d, n, MsgType::AuthChallenge, o); }
Status decode(const uint8_t* d, size_t n, AuthAnswerMsg& o) { return decode_impl(d, n, MsgType::AuthAnswer, o); }
Status decode(const uint8_t* d, size_t n, AuthProofMsg& o) { return decode_impl(d, n, MsgType::AuthProof, o); }

const char* status_name(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::Truncated: return "truncated";
        case Status::Oversized: return "oversized";
        case Status::BadMagic: return "bad magic";
        case Status::BadVersion: return "bad version";
        case Status::BadType: return "bad type";
    }
    return "?";
}

namespace {

#define LZ_CHECK(cond, msg)      \
    do {                         \
        if (!(cond)) {           \
            if (why) *why = msg; \
            return false;        \
        }                        \
    } while (0)

bool peek_is(const uint8_t* buf, size_t n, MsgType want) {
    MsgType t{};
    return peek_type(buf, n, t) == Status::Ok && t == want;
}

// Negative cases for one encoded message (n bytes at good) and its decoder.
template <class T>
bool negatives(const uint8_t* good, size_t n, const char** why) {
    uint8_t buf[128];
    T out{};
    for (size_t k = 0; k < n; ++k)
        LZ_CHECK(decode(good, k, out) == Status::Truncated, "prefix not rejected as truncated");
    LZ_CHECK(decode(nullptr, n, out) == Status::Truncated, "null buffer not rejected");

    std::memcpy(buf, good, n);
    buf[n] = 0;
    LZ_CHECK(decode(buf, n + 1, out) == Status::Oversized, "oversized not rejected");

    std::memcpy(buf, good, n);
    buf[0] ^= 0xFF;
    LZ_CHECK(decode(buf, n, out) == Status::BadMagic, "wrong magic (byte 0) accepted");
    std::memcpy(buf, good, n);
    buf[1] ^= 0xFF;
    LZ_CHECK(decode(buf, n, out) == Status::BadMagic, "wrong magic (byte 1) accepted");

    std::memcpy(buf, good, n);
    buf[2] = kVersion + 1;
    LZ_CHECK(decode(buf, n, out) == Status::BadVersion, "wrong version accepted");

    // A type that is valid but is not this decoder's, and one that is invalid.
    std::memcpy(buf, good, n);
    buf[3] = static_cast<uint8_t>(good[3] % 8 + 1);
    LZ_CHECK(decode(buf, n, out) == Status::BadType, "wrong type accepted");
    buf[3] = 0;
    LZ_CHECK(decode(buf, n, out) == Status::BadType, "type 0 accepted");
    return true;
}

}  // namespace

bool protocol_selftest(const char** why) {
    uint8_t buf[128];

    {
        HelloMsg a{};
        set_str(a.callsign, "Maverick");
        set_str(a.aircraft, "F-4E");
        a.theatre_hash = 0xCAFEF00Du;
        size_t n = encode(a, buf, sizeof(buf));
        LZ_CHECK(n == sizeof(HelloMsg), "hello size");
        LZ_CHECK(encode(a, buf, n - 1) == 0, "hello encode into small buffer");
        HelloMsg b{};
        LZ_CHECK(decode(buf, n, b) == Status::Ok, "hello decode");
        LZ_CHECK(std::strcmp(b.callsign, "Maverick") == 0 && std::strcmp(b.aircraft, "F-4E") == 0 &&
                     b.theatre_hash == 0xCAFEF00Du,
                 "hello fields");
        LZ_CHECK(buf[0] == 'L' && buf[1] == 'Z' && buf[2] == kVersion && buf[3] == 1, "header bytes");
        LZ_CHECK(peek_is(buf, n, MsgType::Hello), "peek hello");
        if (!negatives<HelloMsg>(buf, n, why)) return false;
        HelloMsg c{};
        std::memset(c.callsign, 'x', sizeof(c.callsign));
        std::memset(c.aircraft, 'y', sizeof(c.aircraft));
        n = encode(c, buf, sizeof(buf));
        LZ_CHECK(decode(buf, n, b) == Status::Ok, "hello unterminated decode");
        LZ_CHECK(b.callsign[15] == '\0' && b.aircraft[31] == '\0', "hello strings not terminated");
    }
    {
        WelcomeMsg a{};
        a.client_id = 7;
        a.spawn_pos[0] = 1.5;
        a.spawn_pos[1] = -2000.25;
        a.spawn_pos[2] = 3e6;
        a.spawn_heading = 1.25f;
        a.host_game_time = 123.456;
        a.spawn_vel[0] = 1.f;
        a.spawn_vel[1] = -2.5f;
        a.spawn_vel[2] = 300.f;
        a.theatre_hash = 0x12345678u;
        size_t n = encode(a, buf, sizeof(buf));
        LZ_CHECK(n == sizeof(WelcomeMsg), "welcome size");
        WelcomeMsg b{};
        LZ_CHECK(decode(buf, n, b) == Status::Ok, "welcome decode");
        LZ_CHECK(b.client_id == 7 && b.spawn_pos[0] == 1.5 && b.spawn_pos[1] == -2000.25 && b.spawn_pos[2] == 3e6 &&
                     b.spawn_heading == 1.25f && b.host_game_time == 123.456 && b.spawn_vel[0] == 1.f &&
                     b.spawn_vel[1] == -2.5f && b.spawn_vel[2] == 300.f && b.theatre_hash == 0x12345678u,
                 "welcome fields");
        LZ_CHECK(peek_is(buf, n, MsgType::Welcome), "peek welcome");
        if (!negatives<WelcomeMsg>(buf, n, why)) return false;
    }
    {
        PingMsg a{};
        a.client_send_time = 42.125;
        size_t n = encode(a, buf, sizeof(buf));
        LZ_CHECK(n == sizeof(PingMsg), "ping size");
        PingMsg b{};
        LZ_CHECK(decode(buf, n, b) == Status::Ok && b.client_send_time == 42.125, "ping roundtrip");
        LZ_CHECK(peek_is(buf, n, MsgType::Ping), "peek ping");
        if (!negatives<PingMsg>(buf, n, why)) return false;
    }
    {
        PongMsg a{};
        a.client_send_time = 42.125;
        a.host_game_time = 99.5;
        size_t n = encode(a, buf, sizeof(buf));
        LZ_CHECK(n == sizeof(PongMsg), "pong size");
        PongMsg b{};
        LZ_CHECK(decode(buf, n, b) == Status::Ok && b.client_send_time == 42.125 && b.host_game_time == 99.5,
                 "pong roundtrip");
        LZ_CHECK(peek_is(buf, n, MsgType::Pong), "peek pong");
        if (!negatives<PongMsg>(buf, n, why)) return false;
    }
    {
        StateMsg a{};
        a.client_id = 3;
        a.seq = 0xDEADBEEFu;
        a.game_time = 1000.001;
        for (int i = 0; i < 3; ++i) {
            a.pos[i] = 100.0 * (i + 1) + 0.5;
            a.vel[i] = 10.0f * (i + 1);
            a.ang[i] = 0.1f * (i + 1);
            a.rates[i] = -0.2f * (i + 1);
        }
        size_t n = encode(a, buf, sizeof(buf));
        LZ_CHECK(n == sizeof(StateMsg), "state size");
        StateMsg b{};
        LZ_CHECK(decode(buf, n, b) == Status::Ok, "state decode");
        LZ_CHECK(std::memcmp(&a.client_id, &b.client_id, sizeof(StateMsg) - sizeof(Header)) == 0, "state fields");
        if (!negatives<StateMsg>(buf, n, why)) return false;

        MsgType t{};
        LZ_CHECK(peek_is(buf, n, MsgType::State), "peek state");
        LZ_CHECK(peek_type(buf, n - 1, t) == Status::Truncated, "peek truncated");
        uint8_t bad[sizeof(StateMsg)];
        std::memcpy(bad, buf, n);
        bad[3] = 0;
        LZ_CHECK(peek_type(bad, n, t) == Status::BadType, "peek type 0");
        bad[3] = 12;
        LZ_CHECK(peek_type(bad, n, t) == Status::BadType, "peek type 12");
    }
    {
        ByeMsg a{};
        a.client_id = 65535;
        a.reason = 9;
        size_t n = encode(a, buf, sizeof(buf));
        LZ_CHECK(n == sizeof(ByeMsg), "bye size");
        ByeMsg b{};
        LZ_CHECK(decode(buf, n, b) == Status::Ok && b.client_id == 65535 && b.reason == 9, "bye roundtrip");
        LZ_CHECK(peek_is(buf, n, MsgType::Bye), "peek bye");
        if (!negatives<ByeMsg>(buf, n, why)) return false;
    }
    {
        PeerInfoMsg a{};
        a.id = 4;
        set_str(a.callsign, "Goose");
        set_str(a.aircraft, "F-14A");
        size_t n = encode(a, buf, sizeof(buf));
        LZ_CHECK(n == sizeof(PeerInfoMsg), "peerinfo size");
        PeerInfoMsg b{};
        LZ_CHECK(decode(buf, n, b) == Status::Ok && b.id == 4 && std::strcmp(b.callsign, "Goose") == 0 &&
                     std::strcmp(b.aircraft, "F-14A") == 0,
                 "peerinfo roundtrip");
        LZ_CHECK(buf[3] == 7 && peek_is(buf, n, MsgType::PeerInfo), "peek peerinfo");
        if (!negatives<PeerInfoMsg>(buf, n, why)) return false;
        PeerInfoMsg c{};
        std::memset(c.callsign, 'x', sizeof(c.callsign));
        std::memset(c.aircraft, 'y', sizeof(c.aircraft));
        n = encode(c, buf, sizeof(buf));
        LZ_CHECK(decode(buf, n, b) == Status::Ok && b.callsign[15] == '\0' && b.aircraft[31] == '\0',
                 "peerinfo strings not terminated");
    }
    {
        PeerLeftMsg a{};
        a.id = 2;
        a.reason = 3;
        size_t n = encode(a, buf, sizeof(buf));
        LZ_CHECK(n == sizeof(PeerLeftMsg), "peerleft size");
        PeerLeftMsg b{};
        LZ_CHECK(decode(buf, n, b) == Status::Ok && b.id == 2 && b.reason == 3, "peerleft roundtrip");
        LZ_CHECK(buf[3] == 8 && peek_is(buf, n, MsgType::PeerLeft), "peek peerleft");
        if (!negatives<PeerLeftMsg>(buf, n, why)) return false;
        MsgType t{};
        buf[3] = 12;
        LZ_CHECK(peek_type(buf, n, t) == Status::BadType, "peek type 12");
    }
    {
        // A v1 Hello (version byte 1, the old 52-byte size) is refused as a bad version.
        HelloMsg a{};
        size_t n = encode(a, buf, sizeof(buf));
        buf[2] = 1;
        HelloMsg b{};
        MsgType t{};
        LZ_CHECK(decode(buf, 52, b) == Status::BadVersion, "v1 hello accepted");
        LZ_CHECK(peek_type(buf, 52, t) == Status::BadVersion, "v1 hello peeked");
        LZ_CHECK(decode(buf, n, b) == Status::BadVersion, "v1-versioned hello accepted");
    }
    return true;
}

#undef LZ_CHECK

}  // namespace LazyWorks::SF2Patch::net
