#pragma once
// Pure helpers for the host's per-client receiver and for checking received numbers. No game memory, no
// globals, no allocation; the static_asserts at the end are the self-check (they fail the build).
//
// The interpolation, extrapolation and blend math itself is in hooks/replay_math.h.
#include <cstddef>
#include <limits>

#include "net/protocol.h"

namespace lazysf2::net {

/// <summary>Limits for numbers that arrive from the network. Anything else is dropped.</summary>
inline constexpr double kMaxPosition = 1.0e7;   // metres, per axis
inline constexpr double kMaxVelocity = 2000.0;  // m/s, per axis
inline constexpr double kMaxAngle = 10.0;       // radians
inline constexpr double kMaxRate = 50.0;        // rad/s
inline constexpr double kMaxClockSkew = 3600.0; // seconds either side of the host's game time

/// <summary>Interpolation delay bounds and ramp.</summary>
inline constexpr double kDelayMin = 0.05, kDelayMax = 0.6;
inline constexpr double kDelayFactor = 1.5;         // delay = 1.5 * median packet interval
inline constexpr double kDelayRampFraction = 0.05;  // the delay moves by at most 5% of the elapsed game time
inline constexpr int kIntervalWindow = 16;

constexpr bool finite(double x) { return x == x && x - x == 0.0; }  // false for NaN and infinities
constexpr double abs_(double x) { return x < 0 ? -x : x; }
constexpr bool within(double x, double limit) { return finite(x) && abs_(x) < limit; }

/// <summary>
/// True if every number in a State is finite and in range, and game_time is within an hour of the host's.
/// host_time must itself be finite (the game's clock is).
/// </summary>
constexpr bool state_in_range(const StateMsg& s, double host_time) {
    if (!within(s.game_time - host_time, kMaxClockSkew)) return false;  // also rejects NaN game_time
    for (int i = 0; i < 3; ++i) {
        if (!within(s.pos[i], kMaxPosition)) return false;
        if (!within(s.vel[i], kMaxVelocity)) return false;
        if (!within(s.ang[i], kMaxAngle)) return false;
        if (!within(s.rates[i], kMaxRate)) return false;
    }
    return true;
}

/// <summary>Median of up to kIntervalWindow values (insertion sort of a copy; n is tiny). Returns 0 for n &lt;= 0.</summary>
constexpr double median(const double* v, int n) {
    if (n <= 0) return 0.0;
    if (n > kIntervalWindow) n = kIntervalWindow;
    double t[kIntervalWindow] = {};
    for (int i = 0; i < n; ++i) {
        int j = i;
        while (j > 0 && t[j - 1] > v[i]) {
            t[j] = t[j - 1];
            --j;
        }
        t[j] = v[i];
    }
    return (n % 2) ? t[n / 2] : 0.5 * (t[n / 2 - 1] + t[n / 2]);
}

constexpr double clamp_(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

/// <summary>The delay we want for a median packet interval.</summary>
constexpr double delay_target(double median_interval) {
    return clamp_(kDelayFactor * median_interval, kDelayMin, kDelayMax);
}

/// <summary>Moves the delay towards the target by at most 5% of the elapsed game time (never steps).</summary>
constexpr double ramp_delay(double current, double target, double elapsed) {
    const double step = kDelayRampFraction * (elapsed > 0 ? elapsed : 0.0);
    const double d = target - current;
    return d > step ? current + step : (d < -step ? current - step : target);
}

constexpr bool near_(double a, double b) { return abs_(a - b) < 1e-12; }

constexpr StateMsg good_state() {
    StateMsg s{};
    s.game_time = 100.0;
    s.pos[0] = 1000.0;
    s.vel[0] = 220.0;
    s.ang[1] = 1.0;
    s.rates[2] = 0.5;
    return s;
}
constexpr bool check_state() {
    if (!state_in_range(good_state(), 100.0)) return false;
    StateMsg s = good_state();
    s.pos[2] = 1.0e7;
    if (state_in_range(s, 100.0)) return false;
    s = good_state();
    s.vel[1] = -2000.0;
    if (state_in_range(s, 100.0)) return false;
    s = good_state();
    s.ang[0] = 10.0;
    if (state_in_range(s, 100.0)) return false;
    s = good_state();
    s.rates[1] = 50.0;
    if (state_in_range(s, 100.0)) return false;
    s = good_state();
    s.game_time = 100.0 + 3601.0;
    if (state_in_range(s, 100.0)) return false;
    s = good_state();
    s.game_time = 100.0 - 3599.0;
    if (!state_in_range(s, 100.0)) return false;
    s = good_state();
    s.pos[0] = std::numeric_limits<double>::infinity();
    if (state_in_range(s, 100.0)) return false;
    s = good_state();
    s.game_time = std::numeric_limits<double>::quiet_NaN();
    if (state_in_range(s, 100.0)) return false;
    s = good_state();
    s.ang[2] = std::numeric_limits<float>::quiet_NaN();
    return !state_in_range(s, 100.0);
}
static_assert(check_state());

constexpr bool check_delay() {
    const double v[5] = {0.5, 0.01, 0.02, 0.03, 0.9};
    if (!near_(median(v, 5), 0.03)) return false;
    if (!near_(median(v, 4), 0.025)) return false;  // 0.01 0.02 0.03 0.5
    if (median(v, 0) != 0.0) return false;
    if (!near_(delay_target(0.016), 0.05)) return false;   // 24 ms clamps up to 50 ms
    if (!near_(delay_target(0.2), 0.3)) return false;
    if (!near_(delay_target(5.0), 0.6)) return false;
    // 5% of 1 s elapsed is 50 ms: a target 400 ms away takes 8 s, never one jump.
    if (!near_(ramp_delay(0.1, 0.6, 1.0), 0.15)) return false;
    if (!near_(ramp_delay(0.6, 0.05, 0.5), 0.575)) return false;
    if (!near_(ramp_delay(0.1, 0.11, 1.0), 0.11)) return false;  // closer than one step: lands on it
    return near_(ramp_delay(0.1, 0.6, -1.0), 0.1);                // negative elapsed time: no move
}
static_assert(check_delay());

}  // namespace lazysf2::net
