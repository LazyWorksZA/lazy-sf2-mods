#pragma once
// Pure decisions of the shared-world tooling (world_sync.cpp), free of Windows headers so the tests can use them.
// See lazy-sf2-recon: network-recon §10.2 (seat) and §10.4 (remote).
#include <cstdint>
#include <cstdio>
#include <string>

namespace LazyWorks::SF2Patch::hooks {

/// <summary>The most members a mission slot holds (slot record +0x24..+0xe4).</summary>
constexpr int kWorldMaxMembers = 48;

struct SeatDecision {
    bool apply = false;  // write the member index before the start call
    int member = 0;      // the player's member index (0 = the lead, stock)
    const char* why = "";
};

/// <summary>
/// The player's seat in the player flight. requested is the member index (1 = "x-2"); members is the flight record's
/// member count (+0xe8). Stock (no change) for a request below 1 or a flight that is too small.
/// </summary>
inline SeatDecision decide_seat(int requested, int members) {
    SeatDecision d;
    if (requested < 1) {
        d.why = "seat 0 is the stock seat";
        return d;
    }
    if (members < 1 || members > kWorldMaxMembers) {
        d.why = "the player flight's member count is not plausible";
        return d;
    }
    if (requested >= members) {
        d.why = "the player flight has too few members";
        return d;
    }
    d.apply = true;
    d.member = requested;
    d.why = "ok";
    return d;
}

/// <summary>Parses LAZYSF2_WORLD_SEAT: a member index 1..47; -1 for anything else (empty, junk, 0, too big).</summary>
inline int parse_seat(const wchar_t* s) {
    if (!s || !*s) return -1;
    int v = 0;
    for (; *s; ++s) {
        if (*s < L'0' || *s > L'9') return -1;
        v = v * 10 + (*s - L'0');
        if (v >= kWorldMaxMembers) return -1;
    }
    return v >= 1 ? v : -1;
}

/// <summary>
/// Whose choice a flight's current waypoint is on a client. The owner's decision (2026-11-26): for now every flight
/// follows the host (the game ties #2's active waypoint to its lead, a remote copy on the client); later the
/// player's own flight may pick its own, which needs that tie undone (network-recon §10.17).
/// </summary>
enum class WaypointPolicy : uint8_t { FollowHost, OwnFlightChooses };
inline WaypointPolicy waypoint_policy() { return WaypointPolicy::FollowHost; }
/// <summary>The single decision point: does a host waypoint change apply to this flight on the client?</summary>
inline bool apply_host_waypoint(WaypointPolicy p, bool players_flight) {
    return p == WaypointPolicy::FollowHost || !players_flight;
}

enum class RemoteKind : uint8_t { Aircraft, Ground, Placed };

/// <summary>What one marking pass did, for its log line.</summary>
struct RemoteCounts {
    int aircraft = 0, ground = 0, placed = 0;  // made REMOTEOBJECT now
    int player = 0;      // the player's object, left alone
    int already = 0;     // already REMOTEOBJECT
    int unresolved = 0;  // an id that names no aircraft or ground object
    int faulted = 0;
    void count(RemoteKind k) { (k == RemoteKind::Aircraft ? aircraft : k == RemoteKind::Ground ? ground : placed)++; }
};

/// <summary>A mission object is made remote unless it is the player's, or its id names no object.</summary>
inline bool should_mark_remote(uint32_t id, uint32_t player_id, bool resolved) {
    return resolved && id != 0xffffffffu && id != player_id;
}

/// <summary>Local damage is dropped only while the remote world is on, for objects with the REMOTEOBJECT bit.</summary>
inline bool block_damage(bool remote_on, uint32_t flags, uint32_t remote_mask) {
    return remote_on && remote_mask != 0 && (flags & remote_mask) != 0;
}

/// <summary>Log the 1st, 10th, 100th, ... occurrence (n counts from 1).</summary>
inline bool log_milestone(uint64_t n) {
    if (n == 0) return false;
    while (n % 10 == 0) n /= 10;
    return n == 1;
}

inline std::string format_remote_counts(const RemoteCounts& c) {
    char b[200];
    const int n = std::snprintf(b, sizeof(b),
                                "marked %d aircraft, %d ground-group members, %d placed objects remote; player %d, "
                                "already %d, unresolved %d, faulted %d",
                                c.aircraft, c.ground, c.placed, c.player, c.already, c.unresolved, c.faulted);
    return std::string(b, n > 0 ? static_cast<size_t>(n) : 0);
}

}  // namespace LazyWorks::SF2Patch::hooks
