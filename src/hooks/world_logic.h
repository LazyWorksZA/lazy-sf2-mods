#pragma once
// Pure decisions of the shared-world tooling (world_sync.cpp), free of Windows headers so the tests can use them.
// See lazy-sf2-recon: network-recon §10.2 (seat) and §10.4 (remote).
#include <cstdint>

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

}  // namespace LazyWorks::SF2Patch::hooks
