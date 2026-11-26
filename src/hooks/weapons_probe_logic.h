#pragma once
// Pure logic for the weapons probe (weapons_probe.cpp), kept free of Windows headers so the tests can use it.
// See lazy-sf2-recon: network-recon §9.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hooks/aircraft_side.h"

namespace LazyWorks::SF2Patch::hooks::wprobe {

/// <summary>A weapon's start state: position, angles (pitch, heading, roll; the aircraft convention) and velocity.</summary>
struct LaunchPose {
    double pos[3];
    float angles[3];
    float vel[3];
    double range;  // from the shooter to the target
};

/// <summary>
/// Puts a weapon offset metres out from the shooter on the line of sight to the target, flying along it at speed
/// m/s. False if the two are less than 1 m apart.
/// </summary>
inline bool launch_pose(const double from[3], const double to[3], double offset, double speed, LaunchPose* out) {
    const double d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const double r = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(r > 1.0)) return false;
    const double u[3] = {d[0] / r, d[1] / r, d[2] / r};
    for (int i = 0; i < 3; ++i) {
        out->pos[i] = from[i] + u[i] * offset;
        out->vel[i] = static_cast<float>(u[i] * speed);
    }
    const double z = u[2] > 1.0 ? 1.0 : (u[2] < -1.0 ? -1.0 : u[2]);
    out->angles[0] = static_cast<float>(std::asin(z));             // pitch
    out->angles[1] = static_cast<float>(std::atan2(u[0], u[1]));  // heading, from north (+y) toward east (+x)
    out->angles[2] = 0.f;
    out->range = r;
    return true;
}

/// <summary>One aircraft as the probe's target and shooter choice sees it.</summary>
struct Candidate {
    double pos[3];
    bool is_player;
    bool alive;  // active and below the destroyed damage level
    AircraftSide side;
    const wchar_t* name;  // may be null
};

inline double distance(const double a[3], const double b[3]) {
    const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

/// <summary>
/// The nearest live AI aircraft to ref, other than index exclude (-1 for none); with only_side set, only aircraft
/// of that side. -1 if there is none.
/// </summary>
inline int nearest_ai(const Candidate* c, size_t n, const double ref[3], int exclude, bool only_side = false,
                      AircraftSide side = AircraftSide::Unknown) {
    int best = -1;
    double best_d = 0;
    for (size_t i = 0; i < n; ++i) {
        if (static_cast<int>(i) == exclude || c[i].is_player || !c[i].alive) continue;
        if (only_side && c[i].side != side) continue;
        const double d = distance(c[i].pos, ref);
        if (best < 0 || d < best_d) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    return best;
}

/// <summary>True if name is the player's callsign followed by a space (the player is "GUNSLINGER", its flight "GUNSLINGER 2").</summary>
inline bool in_player_flight(const wchar_t* name, const wchar_t* player_name) {
    if (!name || !player_name || !player_name[0]) return false;
    size_t i = 0;
    for (; player_name[i]; ++i) {
        if (name[i] != player_name[i]) return false;
    }
    return name[i] == L' ';
}

/// <summary>
/// The player's wingman: the nearest live AI aircraft of the player's flight, else the nearest of the player's
/// side (when the side is known), else -1.
/// </summary>
inline int pick_wingman(const Candidate* c, size_t n, int player) {
    if (player < 0 || static_cast<size_t>(player) >= n) return -1;
    int best = -1;
    double best_d = 0;
    for (size_t i = 0; i < n; ++i) {
        if (static_cast<int>(i) == player || c[i].is_player || !c[i].alive) continue;
        if (!in_player_flight(c[i].name, c[player].name)) continue;
        const double d = distance(c[i].pos, c[player].pos);
        if (best < 0 || d < best_d) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    if (best >= 0 || c[player].side == AircraftSide::Unknown) return best;
    return nearest_ai(c, n, c[player].pos, player, true, c[player].side);
}

/// <summary>A weapon type the probe may fire: its id, WeaponDataType and GuidanceType.</summary>
struct WeaponInfo {
    int32_t id;
    int32_t data_type;
    int16_t guidance;
};

/// <summary>GuidanceType 10 IR, 11 command/beam, 12 semi-active radar, 13 active radar.</summary>
inline bool is_air_to_air(const WeaponInfo& w) {
    return (w.data_type == 1 || w.data_type == 7) && w.guidance >= 10 && w.guidance <= 13;
}

/// <summary>
/// The default weapon among those created at mission load (first created first, so the player's own stores lead):
/// the first air-to-air missile, else the first other guided weapon, else the first type 1. -1 if none.
/// </summary>
inline int pick_default_weapon(const WeaponInfo* w, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (is_air_to_air(w[i])) return static_cast<int>(i);
    }
    for (size_t i = 0; i < n; ++i) {
        if ((w[i].data_type == 1 || w[i].data_type == 7) && w[i].guidance != 0) return static_cast<int>(i);
    }
    for (size_t i = 0; i < n; ++i) {
        if (w[i].data_type == 1) return static_cast<int>(i);
    }
    return -1;
}

/// <summary>Case-insensitive ASCII match of a game UTF-16 name (at most max characters) against a name.</summary>
inline bool name_matches(const wchar_t* game, size_t max, const char* name) {
    size_t i = 0;
    for (; i < max && name[i]; ++i) {
        wchar_t a = game[i];
        char b = name[i];
        if (a >= L'a' && a <= L'z') a = static_cast<wchar_t>(a - L'a' + L'A');
        if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
        if (a != static_cast<wchar_t>(static_cast<unsigned char>(b))) return false;
    }
    return name[i] == '\0' && (i == max || game[i] == L'\0');
}

/// <summary>J per kg of explosive: the missile's damaging end scales the warhead mass by 42000 and 1000 (MissileObject +0x2f738, +0x2f730).</summary>
constexpr double kExplosiveJoulesPerKg = 42000.0 * 1000.0;

// The WLD queue records (network-recon §9.5).
constexpr size_t kHitRecordSize = 0x50;
constexpr size_t kExplosionRecordSize = 0x58;

/// <summary>A WLD+0x44 hit on one object. Kind 1 is a kinetic segment from..to (the target applies damage slot 4).</summary>
inline void build_hit_record(unsigned char* r, uint32_t target, int32_t kind, float energy, float radius2,
                             const double from[3], const double to[3], uint32_t shooter, int32_t weapon) {
    std::memset(r, 0, kHitRecordSize);
    std::memcpy(r + 0x00, &target, 4);
    std::memcpy(r + 0x04, &kind, 4);
    std::memcpy(r + 0x08, &energy, 4);
    std::memcpy(r + 0x0c, &radius2, 4);
    std::memcpy(r + 0x18, from, 24);
    std::memcpy(r + 0x30, to, 24);
    std::memcpy(r + 0x48, &shooter, 4);
    std::memcpy(r + 0x4c, &weapon, 4);
}

/// <summary>A WLD+0x40 area blast at pos with no delay, no effect and no sound (as the missile's own damaging end queues it).</summary>
inline void build_explosion_record(unsigned char* r, const double pos[3], float energy, float radius, int32_t weapon,
                                   uint32_t shooter, int32_t warhead_type) {
    std::memset(r, 0, kExplosionRecordSize);
    const int32_t none = -1;
    std::memcpy(r + 0x00, pos, 24);
    std::memcpy(r + 0x24, &energy, 4);
    std::memcpy(r + 0x28, &radius, 4);
    std::memcpy(r + 0x34, &weapon, 4);
    std::memcpy(r + 0x38, &shooter, 4);
    std::memcpy(r + 0x3c, &none, 4);  // no object hit directly
    std::memcpy(r + 0x40, &none, 4);  // effect
    std::memcpy(r + 0x44, &none, 4);  // sound
    std::memcpy(r + 0x48, &warhead_type, 4);
    std::memcpy(r + 0x54, &none, 4);  // grid cell, filled in by WLD
}

/// <summary>
/// Rounds a gun station fired in one update. The loop lowers the ammunition (+0x4e) and raises the burst count
/// (+0x4a) once per round; the burst count is reset to 0 before the loop when a burst ends. The larger of the two
/// counts is used, so unlimited ammunition (a count that does not fall) still counts.
/// </summary>
inline unsigned rounds_fired(int16_t ammo_before, int16_t ammo_after, int16_t burst_before, int16_t burst_after) {
    const unsigned by_ammo = ammo_before > ammo_after ? static_cast<unsigned>(ammo_before - ammo_after) : 0u;
    const int burst = burst_after >= burst_before ? burst_after - burst_before : burst_after;
    const unsigned by_burst = burst > 0 ? static_cast<unsigned>(burst) : 0u;
    return by_ammo > by_burst ? by_ammo : by_burst;
}

/// <summary>A one-second reporting window on a clock that only moves forward.</summary>
struct SecondWindow {
    double start = -1;
    /// <summary>True when t closes the open window (at least 1 s after its start); the next one starts at t.</summary>
    bool tick(double t) {
        if (start < 0 || t < start) {
            start = t;
            return false;
        }
        if (t - start < 1.0) return false;
        start = t;
        return true;
    }
};

}  // namespace LazyWorks::SF2Patch::hooks::wprobe
