#pragma once
// The receiver for one remote jet: it keeps the States it is sent and says what to show at each step. Pure logic: no
// game memory, no Windows, no allocation, no globals, so the tests replay it deterministically.
// See the Phase 4 plan, section 2.5 (and the Task 5 report for where this differs from it).
#include <cstdint>

#include "hooks/replay_math.h"
#include "net/protocol.h"
#include "net/receiver_math.h"

namespace LazyWorks::SF2Patch::net {

namespace replay_math = hooks::replay_math;

/// <summary>
/// Attitude helpers. A quaternion maps body axes (x right, y forward, z up) to the world (x east, y north, z up), with
/// the Euler convention of replay_math::build_matrix: (pitch, heading, roll).
/// </summary>
namespace jet_math {
struct Quat {
    double w, x, y, z;
};
Quat q_mul(const Quat& a, const Quat& b);
Quat q_conj(const Quat& a);
Quat q_from_euler(const double e[3]);
/// <summary>Pitch in [-pi/2, pi/2], heading and roll in (-pi, pi]. Straight up or down: roll 0.</summary>
void q_to_euler(const Quat& q, double e[3]);
/// <summary>Rotation vector (axis x angle, the shorter way round) of a unit quaternion, and back.</summary>
void q_log(const Quat& q, double r[3]);
Quat q_exp(const double r[3]);
/// <summary>The angle of the rotation between two attitudes, in [0, pi].</summary>
double q_angle(const Quat& a, const Quat& b);
/// <summary>The same for two Euler triples.</summary>
double rotation_distance(const double ea[3], const double eb[3]);
}  // namespace jet_math

/// <summary>Thresholds for RemoteJet (plan section 2.5). Times in seconds, distances in metres, angles in radians.</summary>
struct JetParams {
    /// <summary>Predict renders at the present; Interpolate renders a delay behind (the Phase 2/3 behaviour).</summary>
    enum class Mode { Predict, Interpolate } mode = Mode::Predict;
    double second_order = 0.5;  // prediction horizon with the acceleration and rotation; beyond it velocity only
    double rot_lag = 0.05;      // the attitude is predicted for the horizon less about this much (see raw_at)
    double coast_after = 3.0;   // newest State older than this: Coast (do not write). Its age includes the latency,
    double gap_snap = 3.0;      // so a gap over gap_snap usually coasts (and snaps on resume) before this applies
    double restart_back = 0.5;  // a State this much older than the newest is a source restart, not a late packet
    double small_pos = 10.0;  // a jump below this: the short position correction
    double snap_pos = 300.0, snap_ang = 90.0 * replay_math::kPi / 180.0;   // an offset or jump above either: snap
    double blend_short = replay_math::kBlendSeconds, blend_long = 0.5;    // correction times, small and large
    double blend_ang = 0.1;  // attitude corrections of any size (below snap_ang) decay over this
    double blend_accel = 2.0 * 9.80665;        // the short position correction adds at most this acceleration ...
    double blend_accel_large = 6.0 * 9.80665;  // ... and the large one at most this
    int accel_samples = 3;      // the acceleration is the velocity slope over this many intervals ...
    double accel_span = 0.5;    // ... spanning at most this long
    double rate_span = 0.2;     // the turn rate comes from the two newest States only if at most this far apart
    double delay_initial = 0.1, delay_min = 0.05, delay_max = 1.0;
    double delay_interval_factor = 1.0;  // Interpolate: delay target = age p90 + this x the median interval
    double max_step = 0.25;              // the longest step the delay ramp counts
};

/// <summary>One stats window (see RemoteJet::take_stats). Times in seconds.</summary>
struct JetStats {
    double rate_hz, age_p50, age_p90, delay, horizon_max, offset_max;
    uint32_t snaps, coasting_steps, rx, dup_or_old;
    double accel_g_max;  // the largest per-step acceleration of the shown path, in g
    uint32_t restarts;   // the source restarted (sequence or time went back): the samples were dropped
    uint32_t resyncs;    // the receiver's clock stepped back, or steps were over kMaxTrackGap apart
    uint32_t resync_snaps;  // step-gap resyncs that returned Snap: every Snap is counted in snaps or here
};

/// <summary>
/// Receiver for one remote jet. push() takes checked States as they arrive; shown() is called once per step and
/// gives the state to write. Single-thread-owned (the game thread).
///
///  - Predict: the newest State moved on by its age. Position is second order (the acceleration is the velocity slope
///    over the last accel_samples States). Attitude turns at the angular velocity between the two newest States (a
///    rotation, so the Euler flip through the vertical is harmless; held from the last pair at most rate_span apart),
///    over the horizon less rot_lag, so a roll that stops overshoots less. Between States the attitude then turns at
///    close to the full rate only at the design latency (about 150 ms); at LAN ages it lags and is pulled forward at
///    each State. Beyond `second_order` the position goes on with the velocity reached there, the
///    attitude held.
///  - Interpolate: the same at now - delay; between two States, position by sample_at, attitude by slerp. The delay
///    ramps (5 % of the elapsed time) to the arrival-age p90 + delay_interval_factor x the median interval.
///  - Corrections. A new State moves that raw path; the shown path does not jump with it. Position and velocity follow
///    the raw path as a critically damped tracker with the raw acceleration fed forward (with no offset it is the raw
///    path exactly), settling in about blend_short with its own acceleration capped at blend_accel. A State that moves
///    the raw path by small_pos or more gets blend_long and blend_accel_large for blend_long. Attitude offsets (a
///    rotation) decay by blend_weight over blend_ang. The first State, a resume after Coast or a gap
///    over gap_snap, and an offset over the snap limits snap (position and attitude separately).
///  - Recovery. A State whose sequence number goes back by kSeqRestart or more, whose time is restart_back older
///    than the newest, or whose sequence is not newer while its time is, is a source restart: the samples are dropped
///    (counted in restarts) and the next State is one correction from what is shown (blended, or a snap if it is
///    over the limits). A receiver `now` that goes back re-anchors without a jump (counted in resyncs). The 0.5 s
///    restart_back rule relies on ENet's unreliable-sequenced delivery, which drops late States: an unsequenced
///    channel or a relay that can deliver a State over 0.5 s late would trip it.
///  - The caller (Task 7) calls reset() on a Welcome, a mission change, and a HostClock reset or step.
/// </summary>
class RemoteJet {
public:
    using Kin = replay_math::Kin;
    using Samp = replay_math::Samp;
    using Quat = jet_math::Quat;
    static constexpr int kRing = 32;  // enough for a 1 s delay at 20 Hz
    static constexpr int kPending = 16;
    static constexpr int kAgeWindow = 32;
    static constexpr double kG = 9.80665;
    static constexpr double kSubStep = 1.0 / 120.0;  // the tracker's integration step (stable for blend_short >= 0.05)
    static constexpr double kMaxTrackGap = 1.0;      // steps further apart than this re-anchor on the raw path
    static constexpr int32_t kSeqRestart = 1024;

    enum class Out { None, Snap, Blend, Steady, Coast };

    explicit RemoteJet(const JetParams& p = {}) : p_(p), delay_(p.delay_initial) {}
    void reset() { *this = RemoteJet(p_); }

    const JetParams& params() const { return p_; }
    double delay() const { return delay_; }
    bool coasting() const { return coasting_; }

    /// <summary>
    /// A checked State. now is the receiver's host time when it arrived: ideally the network thread's receive time,
    /// carried through the queue (a drain time adds the queue wait to the measured age and the Interpolate delay).
    /// False if old or duplicate (sequence and time), or if now is not finite.
    /// </summary>
    bool push(const StateMsg& s, double now);

    /// <summary>
    /// Once per step. Coast means: do not write this step. out is untouched on None and Coast. out->w is the newest
    /// State's rates as sent (not predicted). A non-finite now returns None.
    /// </summary>
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
    void restart();
    // The acceleration (velocity slope over the newest accel_samples intervals within accel_span) and turn rate
    // (from the two newest States if within rate_span, else held).
    void fit();
    // The raw path at render time for `now`: state, acceleration ff and attitude q. Requires n_ > 0.
    void raw_at(double now, Kin* out, double ff[3], Quat* q) const;
    // Moves the tracker from track_t_ to now.
    void track_to(double now);
    // The shown attitude: q_raw turned by the decaying offset. True while the offset is still applied.
    bool shown_attitude(double now, const Quat& q_raw, Quat* q) const;
    void track_accel(double now, const double p[3]);

    JetParams p_;
    Samp ring_[kRing] = {};
    Quat qring_[kRing] = {};
    int n_ = 0;
    Samp pending_[kPending] = {};
    Quat qpend_[kPending] = {};
    int pn_ = 0;
    bool have_seq_ = false;
    uint32_t last_seq_ = 0;
    double newest_t_ = 0;
    double acc_[3] = {}, omega_[3] = {};  // omega: world-frame angular velocity, rad/s
    double iv_[kIntervalWindow] = {};
    int iv_n_ = 0, iv_pos_ = 0;
    double age_[kAgeWindow] = {};
    int age_n_ = 0, age_pos_ = 0;
    double delay_;
    bool have_last_ = false, coasting_ = false, have_shown_ = false;
    double last_step_ = 0, step_len_ = 1.0 / 60.0;  // the latest step length, assumed when the clock steps back
    double ps_[3] = {}, vs_[3] = {}, track_t_ = 0;  // the shown position and velocity (the tracker)
    Quat q_last_ = {1, 0, 0, 0};                   // the attitude shown at the last step
    double spring_w_ = 0, spring_a_ = 0, large_until_ = 0;
    double off_r_[3] = {}, off_t_ = 0, off_dur_a_ = 0;  // attitude offset (world rotation vector), its start and time
    double hp_[2][3] = {}, ht_[2] = {};
    int hist_n_ = 0;
    bool have_now_ = false;
    double win_start_ = 0, last_now_ = 0;
    JetStats st_ = {};
};

}  // namespace LazyWorks::SF2Patch::net
