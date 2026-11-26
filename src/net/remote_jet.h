#pragma once
// The receiver for one remote jet: it keeps the States it is sent and says what to show at each step. Pure logic: no
// game memory, no Windows, no allocation, no globals, so the tests replay it deterministically.
// See the Phase 4 plan, section 2.5.
#include <cstdint>

#include "hooks/replay_math.h"
#include "net/protocol.h"
#include "net/receiver_math.h"

namespace LazyWorks::SF2Patch::net {

namespace replay_math = hooks::replay_math;

/// <summary>Thresholds for RemoteJet (plan section 2.5). Times in seconds, distances in metres, angles in radians.</summary>
struct JetParams {
    /// <summary>Predict renders at the present; Interpolate renders a delay behind (the Phase 2/3 behaviour).</summary>
    enum class Mode { Predict, Interpolate } mode = Mode::Predict;
    double second_order = 0.5;  // prediction horizon with the acceleration term; beyond it velocity only, angles held
    double coast_after = 3.0;   // newest State older than this: Coast (do not write)
    double gap_snap = 3.0;      // a gap between States longer than this snaps on resume
    double small_pos = 10.0, small_ang = 10.0 * replay_math::kPi / 180.0;  // a jump below this: the short correction
    double snap_pos = 300.0, snap_ang = 90.0 * replay_math::kPi / 180.0;   // an offset or jump above either: snap
    double blend_short = replay_math::kBlendSeconds, blend_long = 0.5;    // correction times, small and large
    double blend_accel = 2.5 * 9.80665;  // the short position correction never adds more acceleration than this
    int accel_samples = 3;      // the acceleration is the velocity slope over this many intervals ...
    double accel_span = 0.5;    // ... spanning at most this long
    double delay_initial = 0.1, delay_min = 0.05, delay_max = 1.0;
    double delay_interval_factor = 1.0;  // Interpolate: delay target = age p90 + this x the median interval
    double max_step = 0.25;              // the longest step the delay ramp counts
};

/// <summary>One stats window (see RemoteJet::take_stats). Times in seconds.</summary>
struct JetStats {
    double rate_hz, age_p50, age_p90, delay, horizon_max, offset_max;
    uint32_t snaps, coasting_steps, rx, dup_or_old;
    double accel_g_max;  // the largest per-step acceleration of the shown path, in g
};

/// <summary>
/// Receiver for one remote jet. push() takes checked States as they arrive; shown() is called once per step and
/// gives the state to write. Single-thread-owned (the game thread).
///
///  - Predict: the newest State moved on by its age; position second order (the acceleration is the velocity slope
///    over the last accel_samples States), angles by the angle rate of the two newest. Beyond `second_order` it goes on
///    with the velocity reached there, angles held, so the path stays continuous.
///  - Interpolate: the same at now - delay, and between two States by sample_at. The delay ramps (5 % of the elapsed
///    time) to the arrival-age p90 + delay_interval_factor x the median interval.
///  - Corrections. A new State moves that raw path; the shown path does not jump with it. Position and velocity follow
///    the raw path as a critically damped spring that feeds the raw acceleration forward (with no offset it is the
///    raw path exactly). It settles in about blend_short, its own acceleration capped at blend_accel so catching up
///    after a manoeuvre starts is spread out instead of showing as a g spike. A State that moves the raw path by
///    small_pos or more gets blend_long, uncapped, for blend_long. Angle offsets decay by blend_weight (blend_long from
///    small_ang). The first State, a resume after Coast or a gap over gap_snap, and anything over the snap limits snap.
/// </summary>
class RemoteJet {
public:
    using Kin = replay_math::Kin;
    using Samp = replay_math::Samp;
    static constexpr int kRing = 32;  // enough for a 1 s delay at 20 Hz
    static constexpr int kPending = 16;
    static constexpr int kAgeWindow = 32;
    static constexpr double kG = 9.80665;
    static constexpr double kSubStep = 1.0 / 120.0;  // the spring's integration step (stable for blend_short >= 0.05)
    static constexpr double kMaxTrackGap = 1.0;      // steps further apart than this restart the spring on the raw path

    enum class Out { None, Snap, Blend, Steady, Coast };

    explicit RemoteJet(const JetParams& p = {}) : p_(p), delay_(p.delay_initial) {}
    void reset() { *this = RemoteJet(p_); }

    const JetParams& params() const { return p_; }
    double delay() const { return delay_; }
    bool coasting() const { return coasting_; }

    /// <summary>A checked State; now is the receiver's host time at arrival. False if old or duplicate (seq and time).</summary>
    bool push(const StateMsg& s, double now);

    /// <summary>Once per step, now non-decreasing. Coast means: do not write this step. out is untouched on None and Coast.</summary>
    Out shown(double now, Kin* out);

    /// <summary>The stats since the last call (ages over the last kAgeWindow States); then starts a new window.</summary>
    JetStats take_stats();

    /// <summary>Nearest-rank percentile (q in [0,1]) of up to kAgeWindow values.</summary>
    static double percentile(const double* v, int n, double q);

private:
    // A critically damped double pole at -w is within about 5 % of its target after w * t = 4.7.
    static constexpr double kSettle = 4.7;

    void touch(double now) {
        if (!have_now_) win_start_ = last_now_ = now;
        have_now_ = true;
        if (now > last_now_) last_now_ = now;
    }
    double render_time(double now) const { return p_.mode == JetParams::Mode::Interpolate ? now - delay_ : now; }

    void drain_one();

    // The acceleration (velocity slope over the newest accel_samples intervals within accel_span) and angle rate.
    void fit();

    // The raw path at render time for `now`, and its acceleration ff. Predict (and Interpolate past the newest
    // State): second order up to `second_order`, then on with the velocity reached there.
    void raw_at(double now, Kin* out, double ff[3]) const;

    // Moves the spring from track_t_ to now. False if the steps were too far apart (the caller snaps).
    bool track_to(double now);

    void angles_at(double now, double a[3]) const;
    // Adds the decaying angle offset. True while it is still applied.
    bool apply_angles(double now, double a[3]) const;

    void track_accel(double now, const double p[3]);

    JetParams p_;
    Samp ring_[kRing] = {};
    int n_ = 0;
    Samp pending_[kPending] = {};
    int pn_ = 0;
    bool have_seq_ = false;
    uint32_t last_seq_ = 0;
    double newest_t_ = 0;
    double acc_[3] = {}, rate_[3] = {};
    double iv_[kIntervalWindow] = {};
    int iv_n_ = 0, iv_pos_ = 0;
    double age_[kAgeWindow] = {};
    int age_n_ = 0, age_pos_ = 0;
    double delay_;
    bool have_last_ = false, coasting_ = false;
    double last_step_ = 0;
    double ps_[3] = {}, vs_[3] = {}, track_t_ = 0;  // the shown position and velocity (the spring)
    double spring_w_ = 0, spring_a_ = 0, large_until_ = 0;
    double off_a_[3] = {}, off_t_ = 0, off_dur_a_ = 0;
    double hp_[2][3] = {}, ht_[2] = {};
    int hist_n_ = 0;
    bool have_now_ = false;
    double win_start_ = 0, last_now_ = 0;
    JetStats st_ = {};
};

}  // namespace LazyWorks::SF2Patch::net
