#pragma once
// The client's estimate of the host's game clock, and the adapter the game thread reads it through. Pure logic: no
// sockets and no wall clock (callers pass times in), so tests drive it deterministically. Header only.
// See the Phase 4 plan, section 2.4.
#include <cmath>

namespace LazyWorks::SF2Patch::net {

/// <summary>
/// The local copy of the host's game clock. All times are seconds; wall is any steady clock.
///
///  - Seeded by the Welcome (host time, minus half the connect RTT). Pongs received before the Welcome are ignored.
///  - Each Pong gives a sample offset = host_game_time - (send + receive) / 2, with its RTT. Samples are kept in a
///    window of the last kWindow Pongs. offset() is that of the lowest-RTT sample in the window (so a lucky early
///    sample cannot stick forever).
///  - Drift: a line of offset against wall time is fitted by least squares through the lowest-RTT half of the window.
///    Its slope is skew() (clamped to +-kMaxSkew); now() follows that line. The fit needs kFitMinSamples samples in
///    that half spanning kFitMinSpan, and a slope over kSkewSignificance standard errors; otherwise the skew is 0. now() follows the line through the mean of that half.
///  - The host's game time freezes while it is paused or in a menu. A Pong whose host time did not advance while wall
///    time did (kFreezeWall) puts the clock on hold: now() stays at the last host time and callers send no States. The
///    first advance after that clears the window (every sample is stale by the length of the pause) and resumes. A
///    Pong whose offset differs from the one in use by more than kJump (plus half its RTT) does the same, which covers
///    short pauses and a host that restarted its clock. No Pong for kStale also puts callers on hold.
/// </summary>
class ClockSync {
    // Single-thread-owned and unsynchronised: it is a small copyable value, so another thread must read a copy
    // (a snapshot taken under a lock), never the live object.
public:
    static constexpr int kWindow = 32;
    static constexpr double kFreezeWall = 0.1;  // wall seconds with no host advance that mean "frozen"
    static constexpr double kFreezeAdvance = 0.001;
    static constexpr double kJump = 0.2;
    static constexpr double kStale = 3.0;
    static constexpr double kMaxRtt = 2.0;
    static constexpr double kMaxSkew = 0.02;
    static constexpr int kFitMinSamples = 8;
    static constexpr double kSkewSignificance = 3.0;  // slope must exceed this many standard errors
    static constexpr double kFitMinSpan = 3.0;  // wall seconds the fitted samples must cover

    bool welcomed() const { return welcomed_; }
    bool have_pong() const { return pongs_ > 0; }
    bool frozen() const { return frozen_; }
    /// <summary>Offset of the lowest-RTT sample in the window (not the fitted line).</summary>
    double offset() const { return offset_; }
    /// <summary>Fitted drift of host time against wall time, in seconds per second; 0 until the fit has enough samples.</summary>
    double skew() const { return skew_; }
    double best_rtt() const { return best_rtt_; }
    int pongs() const { return pongs_; }
    int resets() const { return resets_; }

    void on_welcome(double wall_rx, double host_time, double half_rtt) {
        if (welcomed_) ++resets_;  // a re-Welcome re-seeds the clock: readers must step, not slew
        welcomed_ = true;
        offset_ = host_time - (wall_rx - half_rtt);
        line_off_ = offset_;
        line_wall_ = wall_rx;
        skew_ = 0.0;
        ref_wall_ = wall_rx;
        ref_host_ = host_time;
        last_pong_wall_ = wall_rx;
        held_ = host_time;
        frozen_ = false;
        n_ = 0;
        pongs_ = 0;
        best_rtt_ = half_rtt * 2.0;
    }

    /// <summary>Returns false if the Pong was ignored.</summary>
    bool on_pong(double wall_send, double wall_rx, double host_time) {
        if (!welcomed_) return false;
        const double rtt = wall_rx - wall_send;
        if (!(rtt >= 0.0) || rtt > kMaxRtt || !(host_time == host_time)) return false;
        last_pong_wall_ = wall_rx;
        const double mid = 0.5 * (wall_send + wall_rx);
        const double off = host_time - mid;
        const double wall_elapsed = wall_rx - ref_wall_;
        const double advance = host_time - ref_host_;
        if (advance < kFreezeAdvance && wall_elapsed >= kFreezeWall && advance > -kJump) {
            frozen_ = true;  // host time did not move while ours did: hold
            held_ = host_time;
            ref_wall_ = wall_rx;
            ref_host_ = host_time;
            return true;
        }
        ref_wall_ = wall_rx;
        ref_host_ = host_time;
        bool reset = frozen_;
        if (!reset && pongs_ > 0 && std::fabs(off - offset_) > kJump + 0.5 * rtt) reset = true;
        if (reset) {
            n_ = 0;
            skew_ = 0.0;
            ++resets_;
        }
        frozen_ = false;
        ++pongs_;
        t_[head_] = mid;
        rtt_[head_] = rtt;
        off_[head_] = off;
        head_ = (head_ + 1) % kWindow;
        if (n_ < kWindow) ++n_;
        refit();
        return true;
    }

    /// <summary>
    /// True while States must not be sent: the host's clock is frozen, or nothing has come for kStale: no Pong since
    /// the last one, or, if none ever arrived, none since the Welcome (the Welcome's clock goes stale, too).
    /// </summary>
    bool on_hold(double wall) const { return frozen_ || (welcomed_ && wall - last_pong_wall_ > kStale); }
    /// <summary>The host's game time as of wall: the fitted line, held at the last known host time while frozen.</summary>
    double now(double wall) const { return frozen_ ? held_ : wall + line_off_ + skew_ * (wall - line_wall_); }

private:
    int slot(int age) const { return (head_ + kWindow - 1 - age) % kWindow; }  // age 0 is the newest sample

    void refit() {
        int order[kWindow];  // sample ages, lowest RTT first (insertion sort; n is at most 32)
        for (int i = 0; i < n_; ++i) {
            int j = i;
            while (j > 0 && rtt_[slot(order[j - 1])] > rtt_[slot(i)]) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = i;
        }
        const int best = slot(order[0]);
        offset_ = off_[best];
        best_rtt_ = rtt_[best];
        const int m = n_ / 2 > 0 ? n_ / 2 : 1;
        double mt = 0, mo = 0, tmin = t_[best], tmax = tmin;
        for (int i = 0; i < m; ++i) {
            const int s = slot(order[i]);
            mt += t_[s];
            mo += off_[s];
            if (t_[s] < tmin) tmin = t_[s];
            if (t_[s] > tmax) tmax = t_[s];
        }
        mt /= m;
        mo /= m;
        line_off_ = mo;  // the mean of the lowest-RTT half: less jittery than the single best sample
        line_wall_ = mt;
        skew_ = 0.0;
        if (m < kFitMinSamples || tmax - tmin < kFitMinSpan) return;
        double num = 0, den = 0;
        for (int i = 0; i < m; ++i) {
            const int s = slot(order[i]);
            num += (t_[s] - mt) * (off_[s] - mo);
            den += (t_[s] - mt) * (t_[s] - mt);
        }
        if (!(den > 0.0)) return;
        const double slope = num / den;
        double ss = 0;
        for (int i = 0; i < m; ++i) {
            const int s = slot(order[i]);
            const double r = off_[s] - mo - slope * (t_[s] - mt);
            ss += r * r;
        }
        // Jitter alone gives slopes of a few tenths of a percent: trust the slope only when it stands clear of its
        // own standard error (zero is then the best guess for a host that keeps pace with wall time).
        if (std::fabs(slope) * std::sqrt(den) < kSkewSignificance * std::sqrt(ss / (m - 2))) return;
        skew_ = slope;
        if (skew_ > kMaxSkew) skew_ = kMaxSkew;
        else if (skew_ < -kMaxSkew) skew_ = -kMaxSkew;
    }

    bool welcomed_ = false, frozen_ = false;
    double offset_ = 0, skew_ = 0, line_off_ = 0, line_wall_ = 0;
    double held_ = 0, ref_wall_ = 0, ref_host_ = 0, last_pong_wall_ = 0, best_rtt_ = 0;
    double t_[kWindow] = {}, rtt_[kWindow] = {}, off_[kWindow] = {};
    int n_ = 0, head_ = 0, pongs_ = 0, resets_ = 0;
};

/// <summary>
/// The game thread's reader of the shared clock. Its output never goes backwards, except at a reset (a window
/// reset in the ClockSync, which includes the resume from a freeze), where it steps to the estimate. Otherwise it
/// runs at the estimate's rate and closes any gap to the estimate by at most kMaxSlew of the elapsed time.
/// While the ClockSync is frozen, the output holds (held() is true).
/// </summary>
class HostClock {
public:
    static constexpr double kMaxSlew = 0.05;

    double read(double wall, const ClockSync& c) {
        const double target = c.now(wall);
        held_ = c.frozen();
        if (!started_ || c.resets() != resets_) {
            started_ = true;
            resets_ = c.resets();
            out_ = target;
            last_wall_ = wall;
            return out_;
        }
        const double dt = wall - last_wall_;
        last_wall_ = wall;
        if (c.frozen()) {
            if (target > out_) out_ = target;
            return out_;
        }
        if (!(dt > 0.0)) return out_;
        out_ += dt * (1.0 + c.skew());  // the estimate moves at this rate, so the gap does not grow on its own
        double gap = target - out_;
        if (std::fabs(gap) > ClockSync::kJump) {  // slewing would take too long: treat as a reset
            ++steps_;
            out_ = target;
            return out_;
        }
        const double lim = kMaxSlew * dt;
        out_ += gap > lim ? lim : (gap < -lim ? -lim : gap);
        return out_;
    }

    /// <summary>True if the last read found the host's clock frozen.</summary>
    bool held() const { return held_; }
    /// <summary>Steps taken by the gap rule (not counting the ClockSync's own resets); readers watch it with resets().</summary>
    int steps() const { return steps_; }

private:
    bool started_ = false, held_ = false;
    int resets_ = 0, steps_ = 0;
    double out_ = 0, last_wall_ = 0;
};

}  // namespace LazyWorks::SF2Patch::net
