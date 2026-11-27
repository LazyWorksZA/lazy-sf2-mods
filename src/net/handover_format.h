#pragma once
// The authority handover (handover task 4): the world owner's snapshot of its world at one game frame, as records,
// deflated and sent in chunks small enough for the client core's queues (80 bytes; a host core HostEvent holds 96).
// No game or Windows dependencies. See lazy-sf2-recon: network-recon §12.6.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::net {

#pragma pack(push, 1)
/// <summary>The snapshot's head: the old owner's mission clocks (ms) at its game time T (ms).</summary>
struct HandoverHead {
    uint32_t epoch;  // the handover's id (the old owner's player epoch and a counter)
    uint32_t t_ms;
    uint32_t clock_ms, clock2_ms;
    uint16_t aircraft, vehicles, flights, dead;
};
/// <summary>A live AI aircraft: pose (WorldObjState, kind/a/b its key), state word +0x5f0, damage level, throttle
/// (input 3, 0..255), fuel (kg / 4), and up to 8 loaded stations' counts.</summary>
struct HandoverAircraft {
    WorldObjState pose;
    uint32_t state;
    uint8_t damage;
    uint8_t throttle;
    uint16_t fuel;
    uint8_t stations;
    uint8_t station[8];
    uint8_t count[8];
};
/// <summary>A live vehicle or ship: pose, current speed (+0x154, 0.05 m/s) and heading (+0x150, 2^-16 turns), damage.</summary>
struct HandoverVehicle {
    WorldObjState pose;
    int16_t speed;
    int16_t heading;
    uint8_t damage;
};
/// <summary>An aircraft or ground object the old owner has destroyed (it dies on the receiver too).</summary>
struct HandoverDead {
    uint8_t kind;
    uint16_t a, b;
};
/// <summary>A flight's current waypoint (its formation AI +0x18).</summary>
struct HandoverFlight {
    uint16_t slot;
    uint8_t waypoint;
};
#pragma pack(pop)
static_assert(sizeof(HandoverHead) == 24);
static_assert(sizeof(HandoverDead) == 5);
static_assert(sizeof(HandoverAircraft) == 60);  // 0:35 pose, 35:4 state, 39:1 damage, 40:1 throttle, 41:2 fuel,
                                                // 43:1 stations, 44:8 station, 52:8 count
static_assert(sizeof(HandoverVehicle) == 40);
static_assert(sizeof(HandoverFlight) == 3);

/// <summary>A whole snapshot, unpacked.</summary>
struct HandoverSnapshot {
    HandoverHead head{};
    std::vector<HandoverAircraft> aircraft;
    std::vector<HandoverVehicle> vehicles;
    std::vector<HandoverFlight> flights;
    std::vector<HandoverDead> dead;
};
/// <summary>The bytes: the head, then the aircraft, vehicles, flights and dead back to back (counts in the head).</summary>
std::vector<uint8_t> serialize(const HandoverSnapshot& s);
/// <summary>The reverse; false if the sizes do not add up.</summary>
bool deserialize(const uint8_t* data, size_t len, HandoverSnapshot* out);

inline constexpr size_t kHandoverChunkData = 80;
inline constexpr uint32_t kHandoverMaxPacked = 64 * 1024, kHandoverMaxRaw = 256 * 1024;

#pragma pack(push, 1)
/// <summary>The old owner's offer: what follows (packed bytes, raw bytes, chunks, CRC-32 of the packed bytes) and its
/// mission clock, so the receiver can tell who loaded first.</summary>
struct HandoverBeginMsg {
    Header h;
    uint32_t epoch;
    uint32_t packed, raw;
    uint16_t chunks;
    uint32_t crc;
    uint32_t clock_ms;
};
struct HandoverChunkMsg {
    Header h;
    uint32_t epoch;
    uint16_t index;
    uint8_t len;
    uint8_t data[kHandoverChunkData];
};
/// <summary>The receiver's answer: taken (it applied the snapshot and owns the world) or refused (it owned it already:
/// it loaded first).</summary>
struct HandoverDoneMsg {
    Header h;
    uint32_t epoch;
    uint8_t taken;
};
#pragma pack(pop)
static_assert(sizeof(HandoverBeginMsg) == 26);
static_assert(sizeof(HandoverChunkMsg) == 91 && sizeof(HandoverChunkMsg) <= 96);
static_assert(sizeof(HandoverDoneMsg) == 9);
size_t encode(const HandoverBeginMsg& m, uint8_t* out, size_t cap);
Status decode(const uint8_t* data, size_t len, HandoverBeginMsg& out);
size_t encode(const HandoverChunkMsg& m, uint8_t* out, size_t cap);
/// <summary>Checks 1 <= len <= kHandoverChunkData.</summary>
Status decode(const uint8_t* data, size_t len, HandoverChunkMsg& out);
size_t encode(const HandoverDoneMsg& m, uint8_t* out, size_t cap);
Status decode(const uint8_t* data, size_t len, HandoverDoneMsg& out);

/// <summary>Deflates the snapshot's bytes; *crc is the CRC-32 of the packed bytes. False if too big.</summary>
bool pack_handover(const std::vector<uint8_t>& raw, std::vector<uint8_t>* packed, uint32_t* crc);
/// <summary>Checks the CRC and the sizes and inflates.</summary>
bool unpack_handover(const std::vector<uint8_t>& packed, uint32_t crc, uint32_t raw_size, std::vector<uint8_t>* raw);

/// <summary>Collects the chunks of one offer (any order, duplicates ignored).</summary>
class HandoverAssembly {
public:
    bool begin(const HandoverBeginMsg& b);
    /// <summary>True once every chunk is in.</summary>
    bool add(const HandoverChunkMsg& c);
    bool active() const { return active_; }
    bool complete() const { return active_ && have_ == count_; }
    const HandoverBeginMsg& offer() const { return offer_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    void reset() { *this = HandoverAssembly{}; }

private:
    bool active_ = false;
    HandoverBeginMsg offer_{};
    std::vector<uint8_t> bytes_, got_;
    size_t have_ = 0, count_ = 0;
};

/// <summary>
/// The host's offers, one at a time: the first acceptable offer of a flight wins (two clients that loaded first:
/// the second is refused), an offer that does not complete within kTimeout is dropped (its client is told), and a
/// disconnect drops its client's offer. The decision is made at the offer: the host loading or advancing meanwhile
/// does not change it. Times are seconds (any monotonic clock).
/// </summary>
class OfferArbiter {
public:
    static constexpr double kTimeout = 15.0;
    enum class Verdict : uint8_t { Take, RefuseOwner, RefuseBusy, RefuseDone };
    Verdict on_begin(int slot, uint32_t offer_clock_ms, uint32_t own_clock_ms, double now);
    /// <summary>The offer of slot completed (applied or failed): no other offer is taken this flight if it applied.</summary>
    void on_complete(int slot, bool applied);
    /// <summary>The slot whose offer timed out (and is dropped), or -1.</summary>
    int on_tick(double now);
    void on_disconnect(int slot);
    void reset() { *this = OfferArbiter{}; }
    int active() const { return slot_; }

private:
    int slot_ = -1;
    double since_ = 0;
    bool done_ = false;
};

/// <summary>Who loaded first owns the world: the receiver takes the offer when the offerer's mission clock is ahead
/// of its own by more than the margin (it has run longer, so it loaded first).</summary>
inline bool take_handover(uint32_t offer_clock_ms, uint32_t own_clock_ms, uint32_t margin_ms = 1000) {
    return offer_clock_ms > own_clock_ms + margin_ms;
}

}  // namespace LazyWorks::SF2Patch::net
