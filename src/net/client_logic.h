#pragma once
// The mod client's decisions, without the game: when to connect, when and where to move the own jet on a Welcome,
// when to send a State, and which clock changes to log. Pure logic (no Windows, no game memory, no allocation), so
// net_tests covers it. client.cpp uses it on the game thread. See the Phase 4 plan, sections 2.4 and 2.7.
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "hooks/replay_math.h"
#include "net/clock_sync.h"
#include "net/protocol.h"

namespace LazyWorks::SF2Patch::net::client_logic {

inline constexpr double kRetrySeconds = 5.0;  // wall seconds between connection attempts after a failure
inline constexpr double kAirborneZ = 500.0, kAirborneSpeed = 50.0;  // metres, m/s (plan 2.7)
inline constexpr double kJoinMinMove = 50.0;   // closer than this to the join position: no move
/// <summary>
/// After the jet was moved once in a session, a later join position moves it again only when it is this far from the
/// jet: the host re-sends a join while it waits for the client's States (bot --serve), and those must not yank a
/// pilot who is already on the wing.
/// </summary>
inline constexpr double kJoinMoveAgain = 3000.0;
inline constexpr double kJoinExtrapolateMax = 2.0;  // seconds a join position is carried on with its velocity
inline constexpr double kPeerInfoWait = 5.0;        // wall seconds after the Welcome before a missing host PeerInfo is logged

inline double norm3(const float v[3]) {
    return std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
}
inline double dist3(const double a[3], const double b[3]) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// <summary>In flight for the join rules: above kAirborneZ and faster than kAirborneSpeed.</summary>
inline bool airborne(const double pos[3], const float vel[3]) {
    return pos[2] > kAirborneZ && norm3(vel) > kAirborneSpeed;
}

/// <summary>Whether to start a connection now. blocked: a puppet or replay run, or a refusal that retrying cannot fix.</summary>
constexpr bool connect_due(bool player_known, bool core_idle, bool blocked, double wall, double next_try) {
    return player_known && core_idle && !blocked && wall >= next_try;
}

inline constexpr double kStillTryingSeconds = 30.0;  // wall seconds between "still no answer" lines while retrying

/// <summary>Why a connection does not start now, or nullptr when nothing but the retry timer holds it back.</summary>
constexpr const char* wait_reason(bool player_known, bool busy, bool blocked) {
    if (!player_known) return "player not known yet";
    if (busy) return "blocked: a puppet or replay run is active";
    if (blocked) return "blocked: refused until the player changes";
    return nullptr;
}

enum class JoinAction : uint8_t {
    Move,           // write the jet to the join position
    NoJoin,         // the Welcome has no join position (the first step of a two-step join)
    Off,            // LAZYSF2_NET_JOIN=none
    NotAirborne,    // on the ground or slow: the pilots join up by flying
    Close,          // already within kJoinMinMove
    AlreadyJoined,  // moved earlier in this session, and still within kJoinMoveAgain
};

/// <summary>What to do with a Welcome. dist: from the jet to the (extrapolated) join position.</summary>
constexpr JoinAction join_action(bool has_join, bool move_on, bool is_airborne, bool moved_before, double dist) {
    if (!has_join) return JoinAction::NoJoin;
    if (!move_on) return JoinAction::Off;
    if (dist < kJoinMinMove) return JoinAction::Close;
    if (moved_before && dist < kJoinMoveAgain) return JoinAction::AlreadyJoined;
    if (!is_airborne) return JoinAction::NotAirborne;
    return JoinAction::Move;
}

inline const char* join_action_name(JoinAction a) {
    switch (a) {
        case JoinAction::Move: return "moved";
        case JoinAction::NoJoin: return "no join position yet";
        case JoinAction::Off: return "not moved (LAZYSF2_NET_JOIN=none)";
        case JoinAction::NotAirborne: return "not moved yet (not airborne: below 500 m or slower than 50 m/s)";
        case JoinAction::Close: return "not moved (already there)";
        case JoinAction::AlreadyJoined: return "not moved (already joined in this session)";
    }
    return "?";
}

/// <summary>
/// The join position at host time now: the Welcome's, carried on with its velocity for the time since the Welcome
/// (at most kJoinExtrapolateMax, never backwards).
/// </summary>
inline void join_target(const WelcomeMsg& w, double now, double out[3]) {
    double dt = now - w.host_game_time;
    if (!(dt > 0.0)) dt = 0.0;
    if (dt > kJoinExtrapolateMax) dt = kJoinExtrapolateMax;
    for (int i = 0; i < 3; ++i) out[i] = w.spawn_pos[i] + static_cast<double>(w.spawn_vel[i]) * dt;
}

/// <summary>Turns a world vector about the vertical so its heading (from +y towards +x) grows by delta radians.</summary>
inline void turn_heading(float v[3], double delta) {
    const double c = std::cos(delta), s = std::sin(delta);
    const double x = v[0], y = v[1];
    v[0] = static_cast<float>(x * c + y * s);
    v[1] = static_cast<float>(y * c - x * s);
}

/// <summary>target as a heading in the convention of original: [0, 2 pi) if original is above pi, else (-pi, pi].</summary>
inline float heading_like(float original, double target) {
    using hooks::replay_math::kPi;
    const double w = hooks::replay_math::wrap_pi(target);
    if (static_cast<double>(original) > kPi) return static_cast<float>(w < 0.0 ? w + 2.0 * kPi : w);
    return static_cast<float>(w);
}

/// <summary>States may go out: welcomed, the clock has a Pong, and it is not on hold (the host paused or went quiet).</summary>
constexpr bool may_send(bool welcomed, bool have_pong, bool on_hold) { return welcomed && have_pong && !on_hold; }

/// <summary>
/// Parses LAZYSF2_CONNECT, "host:port": the host a numeric IPv4 address or a name (letters, digits, '.', '-', at most
/// cap - 1 characters), the port 1..65535. False for anything else.
/// </summary>
inline bool parse_connect_spec(const char* spec, char* host, size_t cap, uint16_t* port) {
    if (!spec || !host || !port || cap < 2) return false;
    const char* colon = nullptr;
    for (const char* p = spec; *p; ++p)
        if (*p == ':') colon = p;
    if (!colon || colon == spec) return false;
    const size_t n = static_cast<size_t>(colon - spec);
    if (n + 1 > cap) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = spec[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return false;
    }
    const char* pt = colon + 1;
    if (!*pt) return false;
    unsigned long v = 0;
    for (; *pt; ++pt) {
        if (*pt < '0' || *pt > '9') return false;
        v = v * 10 + static_cast<unsigned long>(*pt - '0');
        if (v > 65535) return false;
    }
    if (v == 0) return false;
    for (size_t i = 0; i < n; ++i) host[i] = spec[i];
    host[n] = '\0';
    *port = static_cast<uint16_t>(v);
    return true;
}

/// <summary>
/// The host's refusals before a Welcome (Bye 1 busy, Bye 2 no avatar): each reason is logged once, until a Welcome or
/// a new player clears it; the retries go on quietly.
/// </summary>
class RefusalOnce {
public:
    /// <summary>True the first time this reason is seen since the last clear().</summary>
    bool first(uint8_t reason) {
        const uint32_t bit = 1u << (reason & 31u);
        if (seen_ & bit) return false;
        seen_ |= bit;
        return true;
    }
    void clear() { seen_ = 0; }

private:
    uint32_t seen_ = 0;
};

/// <summary>
/// Watches the shared clock for the changes the client logs and acts on: the first sync (and the sync after a hold),
/// a hold (the host's clock froze, or no Pong for ClockSync::kStale), and a reset (the ClockSync re-seeded or cleared
/// its window, or the HostClock stepped), on which every remote jet starts again. A reset that came with a Welcome
/// (the Pong count went back) also has kReseed, so the caller can leave it to its Welcome line.
/// The wait for the first Pong after a reset is not a hold.
/// </summary>
class ClockWatch {
public:
    enum Event : unsigned { kNone = 0, kReset = 1, kHold = 2, kSynced = 4, kReseed = 8 };

    void clear() { *this = ClockWatch{}; }

    /// <summary>After each HostClock::read of c at wall. Returns a mask of Events.</summary>
    unsigned update(const ClockSync& c, const HostClock& h, double wall) {
        unsigned ev = kNone;
        if (have_ && (c.resets() != resets_ || h.steps() != steps_)) {
            ev |= kReset;
            if (c.resets() != resets_ && (c.pongs() < pongs_ || c.pongs() == 0)) ev |= kReseed;
        }
        have_ = true;
        resets_ = c.resets();
        steps_ = h.steps();
        pongs_ = c.pongs();
        const bool hold = c.on_hold(wall);
        if (hold && synced_ && !held_) {
            held_ = true;
            ev |= kHold;
        }
        if (c.have_pong() && !hold && (!synced_ || held_)) {
            synced_ = true;
            held_ = false;
            ev |= kSynced;
        }
        return ev;
    }
    bool synced() const { return synced_; }
    bool held() const { return held_; }

private:
    bool have_ = false, synced_ = false, held_ = false;
    int resets_ = 0, steps_ = 0, pongs_ = 0;
};

}  // namespace LazyWorks::SF2Patch::net::client_logic
