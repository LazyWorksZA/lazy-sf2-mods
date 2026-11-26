#pragma once
// Pure logic for remote_avatar.cpp and the host: which AI aircraft becomes a remote player's avatar, and the
// frame-order check. No Windows, no game memory, no allocation, so the tests can use it.
// See the Phase 4 plan, section 2.6, and lazy-sf2-recon: network-recon §8.2 (R2) and §8.4 (R4).
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "hooks/aircraft_side.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>An aircraft we hold. set_bits are the REMOTEOBJECT bits we set (the mask less what was already set).</summary>
struct AvatarHandle {
    void* obj = nullptr;
    unsigned long long seq = 0;
    uint32_t set_bits = 0;
};

/// <summary>One aircraft as the picker sees it: the LiveAircraft fields it ranks on, plus its distance to the placement.</summary>
struct AvatarCandidate {
    const wchar_t* name;    // may be null
    const char* type_name;  // may be null or empty (unknown)
    unsigned long long seq;
    AircraftSide side;
    bool is_player;
    bool taken;   // another remote already holds it (pointer and seq)
    double dist;  // metres from where the avatar will be placed (the player's position if that is unknown)
};

/// <summary>
/// The preference order of plan 2.6; lower is better. Only friendly aircraft are ever taken (owner ruling, Task 11
/// review): an enemy, neutral or unknown-side aircraft is None.
/// </summary>
enum class AvatarTier : uint8_t {
    None = 0,      // never taken: the player, one already held, or not friendly
    Wingman = 1,   // friendly, the wanted type, named with the player's flight callsign
    SameType = 2,  // friendly, the wanted type
    Friendly = 3,
};

/// <summary>
/// Candidates within this many metres of the placement rank before any farther one, whatever their tier: moving an
/// aircraft across the theatre can stall the simulation (a 125 km takeover was once followed by a 44 s host stall).
/// </summary>
inline constexpr double kAvatarNear = 20000.0;

struct AvatarRank {
    AvatarTier tier = AvatarTier::None;
    int flight_no = -1;  // the number after the flight callsign ("DENVER 12" -> 12), -1 if not of the flight
    double dist = 0;
    unsigned long long seq = 0;
    bool nearby = false;  // within kAvatarNear of the placement
};

namespace avatar_logic {

inline double dist3(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

/// <summary>Printable ASCII only ('?' for anything else), NUL-terminated, for log lines and kept fields. src may be null.</summary>
template <typename C>
void to_ascii(char* dst, size_t cap, const C* src) {
    if (!cap) return;
    size_t i = 0;
    for (; src && i + 1 < cap && src[i]; ++i) {
        const auto c = src[i];
        dst[i] = (c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '?';
    }
    dst[i] = '\0';
}

/// <summary>True if one of the n handles holds this aircraft (pointer and construction seq).</summary>
inline bool is_taken(const AvatarHandle* taken, int n, const void* obj, unsigned long long seq) {
    for (int t = 0; t < n; ++t)
        if (taken[t].obj && taken[t].obj == obj && taken[t].seq == seq) return true;
    return false;
}

inline wchar_t up(wchar_t c) { return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c; }
inline char up(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

/// <summary>ASCII case-insensitive equality; false if either is null or empty.</summary>
inline bool same_type(const char* a, const char* b) {
    if (!a || !b || !*a || !*b) return false;
    for (; *a && *b; ++a, ++b)
        if (up(*a) != up(*b)) return false;
    return *a == *b;
}

/// <summary>
/// The flight callsign of the player's name: the name less a trailing number and the spaces before it
/// ("DENVER 11" -> "DENVER", lead 11; "GUNSLINGER" -> itself, lead -1). Returns the callsign length, 0 if none.
/// </summary>
inline size_t flight_base(const wchar_t* player, size_t* base_len, int* lead_no) {
    *base_len = 0;
    *lead_no = -1;
    if (!player) return 0;
    size_t n = 0;
    while (player[n] && n < 64) ++n;
    size_t e = n;
    while (e > 0 && player[e - 1] >= L'0' && player[e - 1] <= L'9') --e;
    if (e < n && n - e <= 4) {
        int v = 0;
        for (size_t i = e; i < n; ++i) v = v * 10 + (player[i] - L'0');
        *lead_no = v;
    } else {
        e = n;  // no number, or too long to be a flight number
    }
    while (e > 0 && player[e - 1] == L' ') --e;
    *base_len = e;
    return e;
}

/// <summary>The flight number of name if it is base, one or more spaces, and 1 to 4 digits (case-insensitive); else -1.</summary>
inline int flight_member_no(const wchar_t* name, const wchar_t* base, size_t base_len) {
    if (!name || !base || base_len == 0) return -1;
    size_t i = 0;
    for (; i < base_len; ++i)
        if (!name[i] || up(name[i]) != up(base[i])) return -1;
    if (name[i] != L' ') return -1;
    while (name[i] == L' ') ++i;
    int v = 0, digits = 0;
    for (; name[i] >= L'0' && name[i] <= L'9'; ++i, ++digits) v = v * 10 + (name[i] - L'0');
    return (digits >= 1 && digits <= 4 && name[i] == 0) ? v : -1;
}

}  // namespace avatar_logic

/// <summary>
/// Ranks candidates one at a time and keeps the best (plan 2.6): friendly only; those within kAvatarNear of the
/// placement first; then Wingman, then SameType, then Friendly. Within a tier: members of the player's flight first, the lowest flight number first (the player's #2
/// before #3), then the nearest, then the earliest constructed.
/// </summary>
class AvatarPicker {
public:
    /// <summary>player_name: the player's aircraft name (null if there is no player); want_type: the remote's aircraft.</summary>
    AvatarPicker(const wchar_t* player_name, const char* want_type) : base_(player_name), want_(want_type) {
        avatar_logic::flight_base(player_name, &base_len_, &lead_no_);
    }

    AvatarRank rank(const AvatarCandidate& c) const {
        AvatarRank r;
        if (c.is_player || c.taken) return r;
        r.dist = std::isfinite(c.dist) ? c.dist : 1e300;
        r.nearby = r.dist <= kAvatarNear;
        r.seq = c.seq;
        int no = avatar_logic::flight_member_no(c.name, base_, base_len_);
        // SF2 names are flight digit then member digit (DENVER 11-14, a second DENVER flight 21-24): with a
        // two-digit lead, only the lead's own tens are its flight.
        if (no == lead_no_ || (lead_no_ >= 10 && lead_no_ <= 99 && no / 10 != lead_no_ / 10)) no = -1;
        r.flight_no = no;
        const bool friendly = c.side == AircraftSide::Friendly;
        const bool type = avatar_logic::same_type(c.type_name, want_);
        if (friendly && type && r.flight_no >= 0) r.tier = AvatarTier::Wingman;
        else if (friendly && type) r.tier = AvatarTier::SameType;
        else if (friendly) r.tier = AvatarTier::Friendly;
        return r;
    }

    /// <summary>True if a ranks before b. Both must have a tier.</summary>
    bool better(const AvatarRank& a, const AvatarRank& b) const {
        if (a.nearby != b.nearby) return a.nearby;
        if (a.tier != b.tier) return a.tier < b.tier;
        const bool ma = a.flight_no >= 0, mb = b.flight_no >= 0;
        if (ma != mb) return ma;
        if (ma && a.flight_no != b.flight_no) return order_no(a.flight_no) < order_no(b.flight_no);
        if (a.dist != b.dist) return a.dist < b.dist;
        return a.seq < b.seq;
    }

    /// <summary>Offers candidate number index; true if it is the best so far.</summary>
    bool offer(size_t index, const AvatarCandidate& c) {
        const AvatarRank r = rank(c);
        if (r.tier == AvatarTier::None) return false;
        if (found_ && !better(r, best_)) return false;
        found_ = true;
        best_ = r;
        index_ = index;
        return true;
    }

    bool found() const { return found_; }
    size_t index() const { return index_; }
    const AvatarRank& best() const { return best_; }

private:
    // With a bare player name ("GUNSLINGER") the lead's number is unknown; an x1 is probably the lead's own slot, so
    // the members after it (x2, x3, ...) rank first.
    int order_no(int no) const { return (lead_no_ < 0 && no % 10 == 1) ? no + 100000 : no; }

    const wchar_t* base_;
    const char* want_;
    size_t base_len_ = 0;
    int lead_no_ = -1;
    bool found_ = false;
    size_t index_ = 0;
    AvatarRank best_;
};

/// <summary>
/// The candidate for one entry of a LiveAircraft-like list (obj, name, seq, is_player, side, type_name), at pos,
/// measured from ref: the player's position, or the placement when there is no player.
/// </summary>
template <typename L>
AvatarCandidate avatar_candidate(const L& a, const double pos[3], const double ref[3], const AvatarHandle* taken,
                                 int n_taken) {
    return AvatarCandidate{a.name,  a.type_name, a.seq, a.side, a.is_player, avatar_logic::is_taken(taken, n_taken, a.obj, a.seq),
                           avatar_logic::dist3(pos, ref)};
}

/// <summary>The whole list at once (the player is the candidate with is_player). Index of the pick, or n if none.</summary>
inline size_t pick_avatar(const AvatarCandidate* list, size_t n, const char* want_type, AvatarRank* rank_out = nullptr) {
    const wchar_t* player = nullptr;
    for (size_t i = 0; i < n; ++i)
        if (list[i].is_player) player = list[i].name;
    AvatarPicker p(player, want_type);
    for (size_t i = 0; i < n; ++i) p.offer(i, list[i]);
    if (rank_out) *rank_out = p.found() ? p.best() : AvatarRank{};
    return p.found() ? p.index() : n;
}

inline const char* tier_name(AvatarTier t) {
    switch (t) {
        case AvatarTier::Wingman: return "the player's wingman";
        case AvatarTier::SameType: return "friendly, same type";
        case AvatarTier::Friendly: return "friendly, other type";
        default: return "none";
    }
}

/// <summary>
/// The frame-order check (R4): does the player step before the avatars in a frame? If an avatar stepped first,
/// sim_game_time() would still be the previous frame's at its step. Fed every aircraft step with a steady
/// clock; a frame starts after a gap of more than kGapMs between two steps (as step_order.cpp). Frames without an
/// avatar step, and the first, partial frame, are not counted. After kFrames frames it gives one verdict.
/// </summary>
class FrameOrderCheck {
public:
    static constexpr double kGapMs = 2.0;
    static constexpr int kFrames = 300;

    void reset() { *this = FrameOrderCheck{}; }

    /// <summary>One aircraft step at t_ms. True once: on the step that completes the check.</summary>
    bool step(double t_ms, bool is_player, bool is_avatar) {
        if (done_) return false;
        if (have_last_ && t_ms - last_ > kGapMs) {
            if (started_ && avatar_seen_) {
                ++frames_;
                if (player_before_) ++player_first_;
            }
            started_ = true;
            player_seen_ = avatar_seen_ = player_before_ = false;
            if (frames_ >= kFrames) {
                done_ = true;
                return true;
            }
        }
        have_last_ = true;
        last_ = t_ms;
        if (!started_) return false;
        if (is_player) player_seen_ = true;
        if (is_avatar && !avatar_seen_) {
            avatar_seen_ = true;
            player_before_ = player_seen_;
        }
        return false;
    }

    bool done() const { return done_; }
    int frames() const { return frames_; }
    int player_first() const { return player_first_; }
    /// <summary>The verdict: in most frames an avatar stepped before the player.</summary>
    bool avatar_first() const { return done_ && (frames_ - player_first_) * 2 > frames_; }

private:
    bool done_ = false, have_last_ = false, started_ = false;
    bool player_seen_ = false, avatar_seen_ = false, player_before_ = false;
    double last_ = 0;
    int frames_ = 0, player_first_ = 0;
};

}  // namespace LazyWorks::SF2Patch::hooks
