#pragma once
// Pure identity logic for the world walker (world_walk.cpp), free of Windows headers so the tests can use it.
// Slots can be reused in place (pooled groups) or hold a new object (other groups), and an id never says which: this
// decides, from what the walker sees each walk, when a slot holds a new instance. See lazy-sf2-recon: recon-notes §20.4.
#include <cmath>
#include <cstdint>

namespace LazyWorks::SF2Patch::hooks {

/// <summary>What the walker remembers about one slot between walks.</summary>
struct SlotMem {
    uintptr_t ptr = 0;
    bool live = false;  // recorded at the last walk
    uint64_t seq = 0;
    uint8_t cls = 0;    // TacviewRec class while live
    float life = 0;     // countermeasures: remaining life at the last walk
    double pos[3] = {};  // bullets: position, velocity and game time at the last walk
    float vel[3] = {};
    double t = 0;
};

/// <summary>The damage level of a destroyed ground object, vehicle or ship.</summary>
constexpr int kDamageDestroyed = 5;

struct GroundStep {
    bool present = false;     // record the object this walk, as seq
    bool is_new = false;      // a new instance: seq is fresh
    bool destroyed = false;   // it just went inactive at damage kDamageDestroyed: write the event for dead_seq
    uint64_t dead_seq = 0;
};

/// <summary>
/// Ground objects, vehicles and ships (not pooled). live = the slot is in use and the object is active. damage is read
/// only when it matters: pass -1 unless the slot was live and holds the same pointer.
/// A seq stays while the same pointer stays live; a new pointer, or a slot live again after being inactive, gets a new one.
/// </summary>
inline GroundStep ground_step(SlotMem& m, uintptr_t ptr, bool live, int damage, uint64_t& next_seq) {
    GroundStep r;
    if (live) {
        r.is_new = !(m.live && m.ptr == ptr);
        if (r.is_new) m.seq = next_seq++;
        m.ptr = ptr;
        m.live = true;
        r.present = true;
        return r;
    }
    if (m.live) {
        if (ptr == m.ptr && damage == kDamageDestroyed) {
            r.destroyed = true;
            r.dead_seq = m.seq;
        }
        m.live = false;
    }
    m.ptr = ptr;
    return r;
}

struct CounterMeasureStep {
    bool present = false;
    bool is_new = false;
};

/// <summary>
/// Flares and chaff (pooled, reused in place). A new instance when the slot was not live at the last walk, or when its
/// remaining life went up (the same object was started again between two walks).
/// </summary>
inline CounterMeasureStep counter_measure_step(SlotMem& m, bool live, float life, uint64_t& next_seq) {
    CounterMeasureStep r;
    if (!live) {
        m.live = false;
        return r;
    }
    r.is_new = !m.live || life > m.life;
    if (r.is_new) m.seq = next_seq++;
    m.live = true;
    m.life = life;
    r.present = true;
    return r;
}

/// <summary>A tracer that is further than this from where its last position and velocity put it is a new round.</summary>
constexpr double kBulletJumpMetres = 300.0;

struct BulletStep {
    bool present = false;
    bool is_new = false;
};

/// <summary>
/// Tracer rounds (pooled, and a round lives a few seconds, so the slot is reused within a frame or two). A new round
/// when the slot was not live at the last walk, or when the position is more than kBulletJumpMetres from last position
/// plus last velocity times dt. Not finite counts as a jump.
/// </summary>
inline BulletStep bullet_step(SlotMem& m, bool live, const double pos[3], const float vel[3], double t,
                              uint64_t& next_seq) {
    BulletStep r;
    if (!live) {
        m.live = false;
        return r;
    }
    bool fresh = !m.live;
    if (!fresh) {
        const double dt = t - m.t;
        double d2 = 0;
        for (int k = 0; k < 3; ++k) {
            const double e = pos[k] - (m.pos[k] + static_cast<double>(m.vel[k]) * dt);
            d2 += e * e;
        }
        fresh = !(d2 <= kBulletJumpMetres * kBulletJumpMetres);
    }
    if (fresh) m.seq = next_seq++;
    for (int k = 0; k < 3; ++k) {
        m.pos[k] = pos[k];
        m.vel[k] = vel[k];
    }
    m.t = t;
    m.live = true;
    r.present = true;
    r.is_new = fresh;
    return r;
}

/// <summary>Pitch and heading (radians, the aircraft convention: x east, y north) of a velocity; roll is 0.</summary>
inline void angles_from_velocity(const float vel[3], float ang[3]) {
    const double h = std::sqrt(static_cast<double>(vel[0]) * vel[0] + static_cast<double>(vel[1]) * vel[1]);
    ang[0] = static_cast<float>(std::atan2(static_cast<double>(vel[2]), h));
    ang[1] = static_cast<float>(std::atan2(static_cast<double>(vel[0]), static_cast<double>(vel[1])));
    ang[2] = 0.0f;
}

}  // namespace LazyWorks::SF2Patch::hooks
