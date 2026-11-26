#pragma once
// Pure parts of the world stream (T6): how often each moving object is sent, what changed for the events, and how
// records are cut into messages. No game or Windows dependencies. See lazy-sf2-recon: network-recon §10.4 and §10.15.
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::net {

/// <summary>Seconds between states of an object this far (metres) from the nearest player: 10 Hz, 2 Hz or 1 Hz.</summary>
inline double world_send_interval(double dist_m) {
    if (!(dist_m < 80000.0)) return 1.0;  // NaN too
    return dist_m < 20000.0 ? 0.1 : 0.5;
}

/// <summary>A stable key as one number (kind, a, b), for maps.</summary>
inline uint64_t world_key(uint8_t kind, uint16_t a, uint16_t b) {
    return (static_cast<uint64_t>(kind) << 32) | (static_cast<uint64_t>(a) << 16) | b;
}

/// <summary>Per object: when it was last sent. Due when the interval for its distance has passed.</summary>
class WorldSendClock {
public:
    bool due(uint64_t key, double now, double dist_m) {
        auto it = last_.find(key);
        if (it != last_.end() && now - it->second < world_send_interval(dist_m) - 1e-6 && now >= it->second) return false;
        last_[key] = now;
        return true;
    }
    void clear() { last_.clear(); }
    size_t size() const { return last_.size(); }

private:
    std::unordered_map<uint64_t, double> last_;
};

/// <summary>What the host saw of an object at one walk.</summary>
struct WorldSeen {
    bool alive = true;   // resolvable and not at damage level 5
    bool awake = false;  // AWAKE bit
    bool asleep = false; // ASLEEP bit
    int16_t level = 0;   // damage level (+0x16c), or a placed record's damage 0..100
};

/// <summary>
/// The events between walks: the first sighting of a key only remembers it (the client starts from the same mission
/// file, so the start state is shared); later changes give Destroyed (once), Awake/Asleep on the bit turning on, and
/// the new damage when it changes.
/// </summary>
class WorldEventTracker {
public:
    /// <summary>Appends to out what changed for this key since the last call.</summary>
    void observe(uint8_t kind, uint16_t a, uint16_t b, const WorldSeen& s, std::vector<WorldEventRec>* out) {
        const uint64_t k = world_key(kind, a, b);
        auto it = prev_.find(k);
        if (it == prev_.end()) {
            prev_.emplace(k, s);
            return;
        }
        WorldSeen& p = it->second;
        auto ev = [&](uint8_t e, int16_t v) { out->push_back(WorldEventRec{e, kind, a, b, v}); };
        if (!p.alive) return;  // nothing after a destruction
        if (s.level != p.level) ev(kind == static_cast<uint8_t>(KeyKind::Placed) ? kEventPlacedDamage : kEventDamageLevel, s.level);
        if (s.awake && !p.awake) ev(kEventAwake, 0);
        if (s.asleep && !p.asleep) ev(kEventAsleep, 0);
        if (!s.alive) ev(kEventDestroyed, 0);
        p = s;
    }
    void clear() { prev_.clear(); }

private:
    std::unordered_map<uint64_t, WorldSeen> prev_;
};

/// <summary>Cuts n records into messages of at most per_msg; calls emit(first, count) for each.</summary>
template <class Rec, class Emit>
size_t world_batches(const Rec* r, size_t n, size_t per_msg, Emit&& emit) {
    size_t msgs = 0;
    for (size_t i = 0; i < n; i += per_msg) {
        emit(r + i, n - i < per_msg ? n - i : per_msg);
        ++msgs;
    }
    return msgs;
}

/// <summary>Bytes on the wire for n states sent in full messages (UDP/ENet headers not included).</summary>
inline size_t world_states_bytes(size_t n) {
    const size_t msgs = (n + kWorldStatesMax - 1) / kWorldStatesMax;
    return msgs * sizeof(WorldStatesHead) + n * sizeof(WorldObjState);
}

}  // namespace LazyWorks::SF2Patch::net
