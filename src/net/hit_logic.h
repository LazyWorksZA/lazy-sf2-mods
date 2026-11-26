#pragma once
// The host's check of a shooter's hit claim (Phase 5 T3). No game or Windows dependencies. See lazy-sf2-recon:
// network-recon §9.5 and §9.6.
#include <cmath>
#include <cstdint>

#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::net {

enum class HitVerdict : uint8_t { Ok, RateLimited, TooMuchEnergy, TooFar, OffTarget, TargetGone, BadSlot, NoShot,
                                  NoShooter, Replayed };

inline const char* hit_verdict_name(HitVerdict v) {
    switch (v) {
        case HitVerdict::Ok: return "applied";
        case HitVerdict::RateLimited: return "over the rate limit";
        case HitVerdict::TooMuchEnergy: return "too much energy";
        case HitVerdict::TooFar: return "the shooter too far from the target";
        case HitVerdict::OffTarget: return "the hit too far from the target";
        case HitVerdict::TargetGone: return "no live target";
        case HitVerdict::BadSlot: return "not a damage slot";
        case HitVerdict::NoShot: return "no recent shot of that weapon by the claimer";
        case HitVerdict::NoShooter: return "the claimer has no avatar here";
        case HitVerdict::Replayed: return "an old or repeated claim";
    }
    return "?";
}

/// <summary>What a hit may plausibly be: gun rounds (the defaults), and unguided stores (store_limits()).</summary>
struct HitLimits {
    double energy_max = 2.0e6;  // joules: a 30 mm round (0.4 kg) at 1,100 m/s is about 0.25 MJ, HE adds its blast
    double range_max = 5000.0;  // metres from the shooter's jet to the target, with lag
    double offset_max = 150.0;  // metres from the target to any position in the hit (the target's size and lag)
    double rate = 60.0;         // claims a second per shooter, on average ...
    double burst = 60.0;        // ... and at once
};

/// <summary>A bomb or rocket: a big warhead (a 2,000 lb bomb is about 1.7 GJ), a shooter far from the impact, a blast
/// centre up to the wire's 327 m from the target.</summary>
inline HitLimits store_limits() {
    HitLimits l;
    l.energy_max = 5.0e9;
    l.range_max = 40000.0;
    l.offset_max = 330.0;
    return l;
}

/// <summary>A token bucket per shooter: rate a second, burst at once. now is any monotonic seconds.</summary>
class HitRate {
public:
    bool take(double now, const HitLimits& l) {
        if (!started_) {
            started_ = true;
            tokens_ = l.burst;
        } else if (now > last_) {
            tokens_ += (now - last_) * l.rate;
            if (tokens_ > l.burst) tokens_ = l.burst;
        }
        last_ = now;
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }
    void reset() { started_ = false; }

private:
    bool started_ = false;
    double tokens_ = 0, last_ = 0;
};

/// <summary>
/// The claimer's recent shots, as the host saw them: its guns (AvatarState) and its store launches (WeaponLaunch, by
/// the claimer's own weapon id). A gun claim needs the trigger on now or released at most kGunGrace ago; a store
/// claim takes one of kStoreBudget hits from a launch of that weapon at most kStoreWindow ago. now in seconds.
/// </summary>
class ShotLedger {
public:
    static constexpr double kGunGrace = 1.0, kStoreWindow = 90.0;
    static constexpr int kStoreBudget = 64;  // objects in the blasts of one bomb or cluster, at most
    static constexpr int kLaunches = 16;
    void guns(bool on, double now) {
        if (on_ && !on) off_at_ = now;
        on_ = on;
        seen_ = true;
    }
    bool allow_gun(double now) const { return on_ || (seen_ && now - off_at_ <= kGunGrace && now >= off_at_); }
    void launch(uint16_t weapon_id, double now) {
        Launch& l = l_[next_++ % kLaunches];
        l = Launch{weapon_id, now, kStoreBudget};
    }
    bool take_store(uint16_t weapon_id, double now) {
        for (Launch& l : l_)
            if (l.budget > 0 && l.id == weapon_id && now >= l.t && now - l.t <= kStoreWindow) {
                --l.budget;
                return true;
            }
        return false;
    }
    /// <summary>Claims come in the claimer's seq order (reliable channel): an old or repeated one is refused.</summary>
    bool fresh(uint16_t seq) {
        if (have_seq_ && static_cast<int16_t>(seq - last_seq_) <= 0) return false;
        have_seq_ = true;
        last_seq_ = seq;
        return true;
    }
    void reset() { *this = ShotLedger{}; }

private:
    struct Launch {
        uint16_t id = 0;
        double t = 0;
        int budget = 0;
    };
    Launch l_[kLaunches];
    unsigned next_ = 0;
    bool on_ = false, seen_ = false, have_seq_ = false;
    double off_at_ = 0;
    uint16_t last_seq_ = 0;
};

/// <summary>The shot check before the plausibility checks: replay, then the claimer's matching shot.</summary>
inline HitVerdict check_hit_shot(const HitMsg& m, ShotLedger& ledger, double now) {
    if (!ledger.fresh(m.seq)) return HitVerdict::Replayed;
    if (m.weapon & kHitStoreBit) {
        if (!ledger.take_store(static_cast<uint16_t>(m.weapon & ~kHitStoreBit), now)) return HitVerdict::NoShot;
    } else if (!ledger.allow_gun(now)) {
        return HitVerdict::NoShot;
    }
    return HitVerdict::Ok;
}

inline double hit_offset_m(const int16_t p[3]) {
    const double x = hit_rel_from_wire(p[0]), y = hit_rel_from_wire(p[1]), z = hit_rel_from_wire(p[2]);
    return std::sqrt(x * x + y * y + z * z);
}

/// <summary>
/// The host's verdict on a claim. shooter_dist is metres between the host's copies of the shooter's jet and the
/// target; target_alive that the target exists here and is not destroyed. The rate bucket is charged only for claims
/// that pass the other checks.
/// </summary>
inline HitVerdict check_hit_claim(const HitMsg& m, double shooter_dist, bool target_alive, HitRate& rate, double now,
                                  const HitLimits& l = HitLimits{}) {
    if (!target_alive) return HitVerdict::TargetGone;
    if (m.slot < 4 || m.slot > 6) return HitVerdict::BadSlot;
    if (!(m.energy <= l.energy_max)) return HitVerdict::TooMuchEnergy;
    if (!(shooter_dist <= l.range_max)) return HitVerdict::TooFar;
    if (hit_offset_m(m.p0) > l.offset_max || (m.slot == 4 && hit_offset_m(m.p1) > l.offset_max))
        return HitVerdict::OffTarget;
    if (!rate.take(now, l)) return HitVerdict::RateLimited;
    return HitVerdict::Ok;
}

}  // namespace LazyWorks::SF2Patch::net
