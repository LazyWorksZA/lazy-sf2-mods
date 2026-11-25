#pragma once
// The synthetic client's pure logic: the scripted flights and the clock sync. No sockets and no wall clock (callers
// pass times in), so the tests can drive both deterministically. Header only.
//
// Angle conventions (the same as the game's and protocol.h's): heading in (-pi, pi], measured from north (+y) toward
// east (+x); pitch positive nose-up; roll positive right-wing-down. Angles and rates are in the order
// (pitch, heading, roll). z is altitude in metres.
#include <cmath>
#include <cstdint>
#include <cstring>

#include "net/protocol.h"

namespace lazysf2::bot {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kSpeed = 220.0;                   // m/s along the path
inline constexpr double kG = 9.80665;                     // m/s^2
inline constexpr double kRollRate = 180.0 * kPi / 180.0;  // roll-in and roll-out, rad/s (180 deg/s)
inline constexpr double kPitchRate = 20.0 * kPi / 180.0;  // rad/s, how fast the nose moves in `climb`
inline constexpr double kBreakPeriod = 8.0;               // seconds per `breaks` turn
inline constexpr double kClimbPitch = 30.0 * kPi / 180.0;
inline constexpr double kClimbTime = 10.0;  // seconds
inline constexpr double kStep = 0.005;      // integration sub-step, seconds

enum class Script { Straight, Orbit, Breaks, Climb };

inline bool parse_script(const char* name, Script* out) {
    if (!name) return false;
    if (!std::strcmp(name, "straight")) *out = Script::Straight;
    else if (!std::strcmp(name, "orbit")) *out = Script::Orbit;
    else if (!std::strcmp(name, "breaks")) *out = Script::Breaks;
    else if (!std::strcmp(name, "climb")) *out = Script::Climb;
    else return false;
    return true;
}

inline double wrap_pi(double a) {  // into (-pi, pi]
    a = std::fmod(a, 2.0 * kPi);
    if (a > kPi) a -= 2.0 * kPi;
    else if (a <= -kPi) a += 2.0 * kPi;
    return a;
}

/// <summary>Bank of a coordinated level turn at load factor n: n = 1/cos(bank).</summary>
inline double bank_for_load(double n) { return std::acos(1.0 / n); }
/// <summary>Turn rate of a coordinated turn: g * tan(bank) / V. Right turns (bank &gt; 0) are positive (heading increases).</summary>
inline double turn_rate(double bank) { return kG * std::tan(bank) / kSpeed; }

/// <summary>
/// One scripted flight. Position is integrated from the velocity in small sub-steps (midpoint heading), so the
/// reported velocity is the derivative of the reported path.
/// </summary>
class Flight {
public:
    struct Sample {
        double pos[3];
        float vel[3], ang[3], rates[3];  // ang and rates: pitch, heading, roll
    };

    void start(Script s, const double spawn[3], double heading, double t0) {
        script_ = s;
        for (int i = 0; i < 3; ++i) pos_[i] = spawn[i];
        heading_ = wrap_pi(heading);
        pitch_ = bank_ = 0.0;
        pitch_rate_ = roll_rate_ = 0.0;
        t0_ = t_ = t0;
    }
    double time() const { return t_; }

    /// <summary>Advances to host game time t (never backwards).</summary>
    void advance_to(double t) {
        while (t - t_ > 1e-9) {  // ignore a remainder below a nanosecond (a rate from it would be rounding noise)
            const double dt = (t - t_) < kStep ? (t - t_) : kStep;
            step(dt);
            t_ += dt;
        }
    }

    Sample sample() const {
        Sample s{};
        const double cp = std::cos(pitch_);
        for (int i = 0; i < 3; ++i) s.pos[i] = pos_[i];
        s.vel[0] = static_cast<float>(kSpeed * cp * std::sin(heading_));
        s.vel[1] = static_cast<float>(kSpeed * cp * std::cos(heading_));
        s.vel[2] = static_cast<float>(kSpeed * std::sin(pitch_));
        s.ang[0] = static_cast<float>(pitch_);
        s.ang[1] = static_cast<float>(heading_);
        s.ang[2] = static_cast<float>(bank_);
        s.rates[0] = static_cast<float>(pitch_rate_);
        s.rates[1] = static_cast<float>(turn_rate(bank_));
        s.rates[2] = static_cast<float>(roll_rate_);
        return s;
    }

private:
    static double slew(double cur, double target, double max_step) {
        const double d = target - cur;
        return d > max_step ? cur + max_step : (d < -max_step ? cur - max_step : target);
    }
    void targets(double tau, double* bank, double* pitch) const {
        *bank = 0.0;
        *pitch = 0.0;
        switch (script_) {
            case Script::Straight: break;
            case Script::Orbit: *bank = bank_for_load(3.0); break;
            case Script::Breaks: {
                const long seg = static_cast<long>(std::floor(tau / kBreakPeriod));
                *bank = ((seg % 2) == 0 ? 1.0 : -1.0) * bank_for_load(6.0);  // right first, then alternating
                break;
            }
            case Script::Climb: *pitch = tau < kClimbTime ? kClimbPitch : 0.0; break;
        }
    }
    void step(double dt) {
        double tb, tp;
        targets(t_ - t0_, &tb, &tp);
        const double nb = slew(bank_, tb, kRollRate * dt);
        const double np = slew(pitch_, tp, kPitchRate * dt);
        roll_rate_ = (nb - bank_) / dt;
        pitch_rate_ = (np - pitch_) / dt;
        // Midpoint integration over the step, with the attitude at the midpoint.
        const double mb = 0.5 * (bank_ + nb), mp = 0.5 * (pitch_ + np);
        const double hr = turn_rate(mb);
        const double hm = heading_ + 0.5 * hr * dt;
        const double cp = std::cos(mp);
        pos_[0] += kSpeed * cp * std::sin(hm) * dt;
        pos_[1] += kSpeed * cp * std::cos(hm) * dt;
        pos_[2] += kSpeed * std::sin(mp) * dt;
        heading_ = wrap_pi(heading_ + hr * dt);
        bank_ = nb;
        pitch_ = np;
    }

    Script script_ = Script::Straight;
    double pos_[3] = {};
    double heading_ = 0, pitch_ = 0, bank_ = 0, pitch_rate_ = 0, roll_rate_ = 0;
    double t0_ = 0, t_ = 0;
};

// The bot's copy of the host's game clock. All times are seconds; wall is any steady clock.
//
//  - Seeded by the Welcome (host time, minus half the connect RTT). Pongs received before the Welcome are ignored.
//  - Each Pong gives a sample offset = host_game_time - (send + receive) / 2, with its RTT. The offset in use is that
//    of the lowest-RTT sample among the last 16 Pongs (windowed, so a lucky early sample cannot stick forever).
//  - The host's game time freezes while it is paused or in a menu. A Pong whose host time did not advance while wall
//    time did (kFreezeWall) puts the clock on hold: now() stays at the last host time and callers send no States. The
//    first advance after that clears the window (every sample is stale by the length of the pause) and resumes. A
//    Pong whose offset differs from the one in use by more than kJump (plus half its RTT) does the same, which covers
//    short pauses and a host that restarted its clock. No Pong for kStale also puts callers on hold.
class ClockSync {
public:
    static constexpr int kWindow = 16;
    static constexpr double kFreezeWall = 0.1;  // wall seconds with no host advance that mean "frozen"
    static constexpr double kFreezeAdvance = 0.001;
    static constexpr double kJump = 0.2;
    static constexpr double kStale = 3.0;
    static constexpr double kMaxRtt = 2.0;

    bool welcomed() const { return welcomed_; }
    bool have_pong() const { return pongs_ > 0; }
    bool frozen() const { return frozen_; }
    double offset() const { return offset_; }
    double best_rtt() const { return best_rtt_; }
    int pongs() const { return pongs_; }
    int resets() const { return resets_; }

    void on_welcome(double wall_rx, double host_time, double half_rtt) {
        welcomed_ = true;
        offset_ = host_time - (wall_rx - half_rtt);
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
        const double off = host_time - 0.5 * (wall_send + wall_rx);
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
            ++resets_;
        }
        frozen_ = false;
        ++pongs_;
        rtt_[head_] = rtt;
        off_[head_] = off;
        head_ = (head_ + 1) % kWindow;
        if (n_ < kWindow) ++n_;
        int best = 0;
        for (int i = 1; i < n_; ++i)
            if (rtt_[(head_ + kWindow - 1 - i) % kWindow] < rtt_[(head_ + kWindow - 1 - best) % kWindow]) best = i;
        const int idx = (head_ + kWindow - 1 - best) % kWindow;
        offset_ = off_[idx];
        best_rtt_ = rtt_[idx];
        return true;
    }

    /// <summary>
    /// True while States must not be sent: the host's clock is frozen, or nothing has come for kStale: no Pong since
    /// the last one, or, if none ever arrived, none since the Welcome (the Welcome's clock goes stale, too).
    /// </summary>
    bool on_hold(double wall) const { return frozen_ || (welcomed_ && wall - last_pong_wall_ > kStale); }
    /// <summary>The host's game time as of wall. Held at the last known host time while frozen.</summary>
    double now(double wall) const { return frozen_ ? held_ : wall + offset_; }

private:
    bool welcomed_ = false, frozen_ = false;
    double offset_ = 0, held_ = 0, ref_wall_ = 0, ref_host_ = 0, last_pong_wall_ = 0, best_rtt_ = 0;
    double rtt_[kWindow] = {}, off_[kWindow] = {};
    int n_ = 0, head_ = 0, pongs_ = 0, resets_ = 0;
};

}  // namespace lazysf2::bot
