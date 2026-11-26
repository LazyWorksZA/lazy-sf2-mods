#pragma once
// The host's check of a shooter's hit claim (Phase 5 T3). No game or Windows dependencies. See lazy-sf2-recon:
// network-recon §9.5 and §9.6.
#include <cmath>
#include <cstdint>

#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::net {

enum class HitVerdict : uint8_t { Ok, RateLimited, TooMuchEnergy, TooFar, OffTarget, TargetGone, BadSlot };

inline const char* hit_verdict_name(HitVerdict v) {
    switch (v) {
        case HitVerdict::Ok: return "applied";
        case HitVerdict::RateLimited: return "over the rate limit";
        case HitVerdict::TooMuchEnergy: return "too much energy";
        case HitVerdict::TooFar: return "the shooter too far from the target";
        case HitVerdict::OffTarget: return "the hit too far from the target";
        case HitVerdict::TargetGone: return "no live target";
        case HitVerdict::BadSlot: return "not a damage slot";
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
