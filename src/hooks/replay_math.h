#pragma once
// Pure math for the replay (replay.cpp): interpolation, dead reckoning, angle wrap, blend weight and
// the feed delay. No game memory, no globals, no allocation, so each function can be read and checked
// on its own. The static_asserts at the end are the self-check: they fail the build, not the game.
//
// Conventions: positions in metres (x east, y north, z up), time in seconds, angles in radians
// (pitch, heading, roll). Heading runs from north toward east.
#include <cmath>
#include <cstddef>

namespace LazyWorks::SF2Patch::hooks::replay_math {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBlendSeconds = 0.2;   // a correction offset decays to zero over this much game time
constexpr double kMaxExtrapolate = 1.0;  // dead reckoning never runs longer than this
constexpr double kMinDelay = 0.05, kMaxDelay = 0.6;

constexpr double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
constexpr double absd(double x) { return x < 0 ? -x : x; }

/// <summary>Wraps an angle into (-pi, pi].</summary>
constexpr double wrap_pi(double a) {
    if (!(absd(a) < 1e6)) return a;  // inf or NaN (or absurdly large): never loop on it
    while (a > kPi) a -= 2.0 * kPi;
    while (a <= -kPi) a += 2.0 * kPi;
    return a;
}
/// <summary>Shortest signed difference to - from, in (-pi, pi].</summary>
constexpr double angle_diff(double to, double from) { return wrap_pi(to - from); }

constexpr double lerp(double a, double b, double s) { return a + (b - a) * s; }
/// <summary>Angle interpolation along the shortest way round.</summary>
constexpr double lerp_angle(double a, double b, double s) { return a + angle_diff(b, a) * s; }

/// <summary>
/// Cubic Hermite position between (p0, slope m0) and (p1, slope m1), s in [0,1], interval length h seconds.
/// The slopes are velocities (units per second).
/// </summary>
constexpr double hermite(double p0, double m0, double p1, double m1, double h, double s) {
    const double s2 = s * s, s3 = s2 * s;
    const double h00 = 2.0 * s3 - 3.0 * s2 + 1.0;
    const double h10 = s3 - 2.0 * s2 + s;
    const double h01 = -2.0 * s3 + 3.0 * s2;
    const double h11 = s3 - s2;
    return h00 * p0 + h10 * h * m0 + h01 * p1 + h11 * h * m1;
}

/// <summary>
/// Weight of a correction offset, `elapsed` seconds after it was set: 1 at 0, 0 at `duration`, and flat at
/// both ends (so neither the offset nor its rate jumps). 1 - smoothstep(elapsed / duration).
/// </summary>
constexpr double blend_weight(double elapsed, double duration) {
    const double s = clampd(elapsed / duration, 0.0, 1.0);
    return 1.0 - s * s * (3.0 - 2.0 * s);
}

/// <summary>Interpolation delay for a feed rate: one and a half periods, kept within [50 ms, 600 ms].</summary>
constexpr double feed_delay(int rate_hz) { return clampd(1.5 / static_cast<double>(rate_hz), kMinDelay, kMaxDelay); }

struct Kin {          // kinematic state, all double for the math
    double p[3];      // position
    double v[3];      // velocity
    double a[3];      // angles: pitch, heading, roll
    double w[3];      // angular rates
};
struct Samp {         // a state at a time (seconds, on the replay's own timeline)
    double t;
    Kin k;
};

/// <summary>
/// State at time t between a and b (a.t &lt;= t &lt;= b.t): position by cubic Hermite with the two velocities,
/// velocity and rates linearly, angles component-wise along the shortest way.
/// </summary>
constexpr void interpolate(const Samp& a, const Samp& b, double t, Kin* out) {
    const double h = b.t - a.t;
    if (!(h > 1e-9)) {
        *out = a.k;
        return;
    }
    const double s = clampd((t - a.t) / h, 0.0, 1.0);
    for (int i = 0; i < 3; ++i) {
        out->p[i] = hermite(a.k.p[i], a.k.v[i], b.k.p[i], b.k.v[i], h, s);
        out->v[i] = lerp(a.k.v[i], b.k.v[i], s);
        out->a[i] = lerp_angle(a.k.a[i], b.k.a[i], s);
        out->w[i] = lerp(a.k.w[i], b.k.w[i], s);
    }
}

/// <summary>
/// Angle rate (radians per second) between two states, component-wise along the shortest way; 0 if they share
/// a time. Used instead of the recorded rates when extrapolating angles (they are not rates of pitch/heading/roll).
/// </summary>
constexpr void angle_rate(const Samp& a, const Samp& b, double out[3]) {
    const double h = b.t - a.t;
    for (int i = 0; i < 3; ++i) out[i] = h > 1e-9 ? angle_diff(b.k.a[i], a.k.a[i]) / h : 0.0;
}

/// <summary>
/// Dead reckoning from a state: position + velocity * dt, angles + rate * dt, dt in [-1 s, 1 s]. A negative dt
/// runs backwards (used before the oldest sample). Velocity and the recorded rates are held.
/// </summary>
constexpr void extrapolate(const Samp& s, double t, const double rate[3], Kin* out) {
    const double dt = clampd(t - s.t, -kMaxExtrapolate, kMaxExtrapolate);
    for (int i = 0; i < 3; ++i) {
        out->p[i] = s.k.p[i] + s.k.v[i] * dt;
        out->v[i] = s.k.v[i];
        out->a[i] = s.k.a[i] + rate[i] * dt;
        out->w[i] = s.k.w[i];
    }
}

/// <summary>
/// State at render time t from n samples ordered by time (oldest first).
/// Between two: interpolates. After the newest: dead reckoning forward from it, with the angle rate taken from
/// the two newest samples. Before the oldest: the same, backwards from the oldest, with the rate from the two
/// oldest. Returns false if n == 0.
/// </summary>
constexpr bool sample_at(const Samp* s, int n, double t, Kin* out) {
    if (n <= 0) return false;
    int i = n - 1;
    while (i >= 0 && s[i].t > t) --i;
    double rate[3] = {0.0, 0.0, 0.0};
    if (i < 0) {
        if (n >= 2) angle_rate(s[0], s[1], rate);
        extrapolate(s[0], t, rate, out);
    } else if (i == n - 1) {
        if (n >= 2) angle_rate(s[n - 2], s[n - 1], rate);
        extrapolate(s[i], t, rate, out);
    } else {
        interpolate(s[i], s[i + 1], t, out);
    }
    return true;
}

/// <summary>
/// Rotation matrix rows (right, forward, up) from pitch, heading, roll: pitch nose-up positive, roll
/// right-wing-down positive, heading from north toward east in (-pi, pi].
/// </summary>
inline void build_matrix(const double a[3], float m[9]) {
    const double cp = std::cos(a[0]), sp = std::sin(a[0]), ch = std::cos(a[1]), sh = std::sin(a[1]);
    const double cr = std::cos(a[2]), sr = std::sin(a[2]);
    const double f[3] = {sh * cp, ch * cp, sp};
    const double r0[3] = {ch, -sh, 0.0};
    const double u0[3] = {-sh * sp, -ch * sp, cp};  // r0 x f
    for (int i = 0; i < 3; ++i) {
        m[i] = static_cast<float>(r0[i] * cr - u0[i] * sr);
        m[3 + i] = static_cast<float>(f[i]);
        m[6 + i] = static_cast<float>(u0[i] * cr + r0[i] * sr);
    }
}

constexpr bool approx(double a, double b, double eps = 1e-9) { return absd(a - b) <= eps; }

// Hermite returns the endpoints at s = 0 and s = 1, and follows straight-line motion exactly.
static_assert(approx(hermite(3.0, 7.0, 11.0, -2.0, 0.5, 0.0), 3.0));
static_assert(approx(hermite(3.0, 7.0, 11.0, -2.0, 0.5, 1.0), 11.0));
static_assert(approx(hermite(0.0, 10.0, 10.0, 10.0, 1.0, 0.5), 5.0));
static_assert(approx(hermite(0.0, 10.0, 10.0, 10.0, 1.0, 0.25), 2.5));
// Slope scaling: 20 m/s over a 0.5 s interval is 10 m, so the path is a straight line (fails without "* h").
static_assert(approx(hermite(0.0, 20.0, 10.0, 20.0, 0.5, 0.25), 2.5));
// Shortest angle difference: 350 deg from 0 is -10 deg, 10 from 350 is +20, and 180 stays +180.
static_assert(approx(angle_diff(350.0 * kPi / 180.0, 0.0), -10.0 * kPi / 180.0));
static_assert(approx(angle_diff(10.0 * kPi / 180.0, 350.0 * kPi / 180.0), 20.0 * kPi / 180.0));
static_assert(approx(angle_diff(kPi, 0.0), kPi));
static_assert(approx(wrap_pi(3.0 * kPi), kPi));
static_assert(wrap_pi(1e300) == 1e300);  // absurd input is returned, not looped on
static_assert(approx(lerp_angle(350.0 * kPi / 180.0, 10.0 * kPi / 180.0, 0.5), 360.0 * kPi / 180.0));
// Blend weight: 1 at the start, 0 at 200 ms and after, 0.5 half way.
static_assert(approx(blend_weight(0.0, kBlendSeconds), 1.0));
static_assert(approx(blend_weight(0.1, kBlendSeconds), 0.5));
static_assert(approx(blend_weight(0.2, kBlendSeconds), 0.0));
static_assert(approx(blend_weight(5.0, kBlendSeconds), 0.0));
// Delay: 1.5 periods within [50 ms, 600 ms].
static_assert(approx(feed_delay(60), 0.05));
static_assert(approx(feed_delay(10), 0.15));
static_assert(approx(feed_delay(5), 0.3));
static_assert(approx(feed_delay(2), 0.6));
/// <summary>Extrapolation is capped at 1 s; sample_at interpolates, extrapolates and holds.</summary>
constexpr bool check_sample_at() {
    Samp s[2] = {};
    s[0].t = 0.0;
    s[0].k.v[0] = 10.0;
    s[1].t = 1.0;
    s[1].k.p[0] = 10.0;
    s[1].k.v[0] = 10.0;
    s[1].k.a[1] = 1.0;  // heading turns 1 rad in 1 s: the angle rate from the two samples is 1 rad/s
    Kin k = {};
    if (!sample_at(s, 2, 0.5, &k) || !approx(k.p[0], 5.0)) return false;   // between the two
    if (!sample_at(s, 2, 3.0, &k) || !approx(k.p[0], 20.0)) return false;  // 1 s of dead reckoning, not 2 s
    if (!approx(k.a[1], 2.0)) return false;  // heading 1 rad + 1 s at 1 rad/s
    if (!sample_at(s, 2, -0.5, &k) || !approx(k.p[0], -5.0)) return false;  // before the oldest: backwards
    if (!approx(k.a[1], -0.5)) return false;
    if (!sample_at(s, 2, -5.0, &k) || !approx(k.p[0], -10.0)) return false;  // ... for at most 1 s
    return !sample_at(s, 0, 0.0, &k);
}
static_assert(check_sample_at());

}  // namespace LazyWorks::SF2Patch::hooks::replay_math
