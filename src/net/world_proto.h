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
};
inline constexpr uint8_t kWorldTypeFirst = 20, kWorldTypeLast = 24;

/// <summary>The type byte of a v3 packet with a world type, or 0 (not one, or a bad header).</summary>
uint8_t peek_world_type(const uint8_t* data, size_t len);

/// <summary>Payload bytes per chunk: a chunk packet (972 bytes) stays under ENet's 1,024-byte limit and one datagram.</summary>
inline constexpr size_t kMissionChunkData = 960;
/// <summary>The largest packed mission (compressed .MSN plus sidecar) the host offers: 274 chunks.</summary>
inline constexpr uint32_t kMissionMaxPacked = 256 * 1024;
inline constexpr uint32_t kMissionMaxRaw = 4 * 1024 * 1024;

inline constexpr uint8_t kOfferOk = 0;       // chunks follow, in order, on the reliable channel
inline constexpr uint8_t kOfferNone = 1;     // the host has no mission yet: ask again later
inline constexpr uint8_t kOfferTooBig = 2;   // the host's mission is over kMissionMaxPacked

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
struct MissionChunkHead {
    Header h;
    uint32_t mission_id;
    uint16_t index;
    uint16_t len;  // payload bytes that follow (1..kMissionChunkData; only the last chunk may be shorter)
};
#pragma pack(pop)
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
