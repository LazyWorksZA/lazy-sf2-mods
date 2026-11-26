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
};
inline constexpr uint8_t kWorldTypeFirst = 20, kWorldTypeLast = 22;

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

#pragma pack(push, 1)
struct MissionRequestMsg {
    Header h;
    uint32_t have_id;  // the mission id the client already holds, 0 for none
};
struct MissionOfferMsg {
    Header h;
    uint32_t mission_id;    // CRC-32 of the packed bytes
    uint32_t packed_bytes;  // what the chunks carry
    uint32_t raw_bytes;     // unpacked size
    uint16_t chunk_bytes;   // kMissionChunkData
    uint16_t chunk_count;
    uint8_t status;         // kOffer*
    uint8_t seat;           // the member index the host suggests for this client (0 = none)
};
struct MissionChunkHead {
    Header h;
    uint32_t mission_id;
    uint16_t index;
    uint16_t len;  // payload bytes that follow (1..kMissionChunkData; only the last chunk may be shorter)
};
#pragma pack(pop)
static_assert(sizeof(MissionRequestMsg) == 8);   // 4:4 have id
static_assert(sizeof(MissionOfferMsg) == 22);    // 4:4 id, 8:4 packed, 12:4 raw, 16:2 chunk, 18:2 count, 20:1 status, 21:1 seat
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
