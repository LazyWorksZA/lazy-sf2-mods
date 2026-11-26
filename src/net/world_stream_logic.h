#pragma once
// Pure parts of the world stream (T6): how often each moving object is sent, what changed for the events, and how
// records are cut into messages. No game or Windows dependencies. See lazy-sf2-recon: network-recon §10.4 and §10.15.
#include <cstddef>
#include <algorithm>
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
    int16_t waypoint = -1;  // a flight key: its current waypoint index; -1 = not a flight / unknown
};

/// <summary>
/// The events between walks: the first sighting of a key only remembers it (the client starts from the same mission
/// file, so the start state is shared); later changes give Destroyed (once), Awake/Asleep on the bit turning on, and
/// a placed record's new damage when it changes (objects' damage levels are not sent).
/// </summary>
class WorldEventTracker {
public:
    /// <summary>Appends to out what changed for this key since the last call.</summary>
    void observe(uint8_t kind, uint16_t a, uint16_t b, const WorldSeen& s, std::vector<WorldEventRec>* out) {
        const uint64_t k = world_key(kind, a, b);
        auto it = prev_.find(k);
        if (it == prev_.end()) {
            prev_.emplace(k, s);
            first_.emplace(k, s);  // the mission's start state, as the client's copy also begins
            return;
        }
        WorldSeen& p = it->second;
        auto ev = [&](uint8_t e, int16_t v) { out->push_back(WorldEventRec{e, kind, a, b, v}); };
        if (!p.alive) return;  // nothing after a destruction
        // Objects' damage levels are not sent (the client cannot apply them yet); placed records' damage is.
        if (kind == static_cast<uint8_t>(KeyKind::Placed) && s.level != p.level) ev(kEventPlacedDamage, s.level);
        if (s.waypoint >= 0 && s.waypoint != p.waypoint) ev(kEventWaypoint, s.waypoint);
        if (s.awake && !p.awake) ev(kEventAwake, 0);
        if (s.asleep && !p.asleep) ev(kEventAsleep, 0);
        if (!s.alive) ev(kEventDestroyed, 0);
        p = s;
    }
    void clear() {
        prev_.clear();
        first_.clear();
    }
    /// <summary>
    /// The late-join snapshot: per key, sorted, what differs from the mission's start (the first sighting; the
    /// tracker runs from the host's mission start): destroyed; else woke, went to sleep, a placed record's changed
    /// damage; and every flight's waypoint. Applying them to a fresh copy of the mission brings it to the host's
    /// state, as far as events go.
    /// </summary>
    void snapshot(std::vector<WorldEventRec>* out) const {
        std::vector<uint64_t> keys;
        keys.reserve(prev_.size());
        for (const auto& kv : prev_) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        for (uint64_t k : keys) {
            const WorldSeen& s = prev_.at(k);
            const WorldSeen& f = first_.at(k);
            const uint8_t kind = static_cast<uint8_t>(k >> 32);
            const uint16_t a = static_cast<uint16_t>(k >> 16), b = static_cast<uint16_t>(k);
            auto ev = [&](uint8_t e, int16_t v) { out->push_back(WorldEventRec{e, kind, a, b, v}); };
            if (!s.alive) {
                if (f.alive) ev(kEventDestroyed, 0);
                continue;
            }
            if (s.awake && !f.awake) ev(kEventAwake, 0);
            if (s.asleep && !f.asleep) ev(kEventAsleep, 0);
            if (kind == static_cast<uint8_t>(KeyKind::Placed) && s.level != f.level) ev(kEventPlacedDamage, s.level);
            if (s.waypoint >= 0) ev(kEventWaypoint, s.waypoint);
        }
    }
    size_t size() const { return prev_.size(); }

    /// <summary>Every flight key's current waypoint (the periodic resend that keeps the clients from drifting).</summary>
    void waypoints(std::vector<WorldEventRec>* out) const {
        for (const auto& kv : prev_)
            if (kv.second.waypoint >= 0)
                out->push_back(WorldEventRec{kEventWaypoint, static_cast<uint8_t>(kv.first >> 32),
                                             static_cast<uint16_t>(kv.first >> 16), static_cast<uint16_t>(kv.first),
                                             kv.second.waypoint});
    }

private:
    std::unordered_map<uint64_t, WorldSeen> prev_, first_;
};

/// <summary>
/// The latest AvatarState of each peer as the host knows it (its own as peer 0, each client's as received), so that a
/// joiner gets everyone's current lock and configuration right after its snapshot.
/// </summary>
class AvatarStateCache {
public:
    static constexpr int kPeers = 5;
    void set(const AvatarStateMsg& m) {
        if (m.peer < kPeers) {
            have_[m.peer] = true;
            last_[m.peer] = m;
        }
    }
    void forget(int peer) {
        if (peer >= 0 && peer < kPeers) have_[peer] = false;
    }
    void clear() {
        for (bool& h : have_) h = false;
    }
    /// <summary>The states a joiner (peer id) should get: everyone's but its own.</summary>
    size_t for_joiner(int joiner, AvatarStateMsg* out) const {
        size_t n = 0;
        for (int p = 0; p < kPeers; ++p)
            if (have_[p] && p != joiner) out[n++] = last_[p];
        return n;
    }

private:
    bool have_[kPeers] = {};
    AvatarStateMsg last_[kPeers] = {};
};

/// <summary>The most parts a snapshot sends: well inside the client's 256-entry events ring, shared with live traffic.</summary>
inline constexpr size_t kSnapshotMaxParts = 64;

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
