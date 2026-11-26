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

#include "net/clock_sync.h"
#include "net/protocol.h"

namespace LazyWorks::SF2Patch::bot {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kSpeed = 220.0;                   // m/s along the path
inline constexpr double kG = 9.80665;                     // m/s^2
inline constexpr double kRollRate = 180.0 * kPi / 180.0;  // roll-in and roll-out, rad/s (180 deg/s)
inline constexpr double kPitchRate = 20.0 * kPi / 180.0;  // rad/s, how fast the nose moves in `climb`
inline constexpr double kBreakPeriod = 8.0;               // seconds per `breaks` turn
inline constexpr double kClimbPitch = 30.0 * kPi / 180.0;
inline constexpr double kClimbTime = 10.0;  // seconds
inline constexpr double kStep = 0.005;      // integration sub-step, seconds

/// <summary>Formation flies on the host's predicted jet (bot_client.cpp); Flight flies it as Straight.</summary>
enum class Script { Straight, Orbit, Breaks, Climb, Formation };

inline bool parse_script(const char* name, Script* out) {
    if (!name) return false;
    if (!std::strcmp(name, "straight")) *out = Script::Straight;
    else if (!std::strcmp(name, "orbit")) *out = Script::Orbit;
    else if (!std::strcmp(name, "breaks")) *out = Script::Breaks;
    else if (!std::strcmp(name, "climb")) *out = Script::Climb;
    else if (!std::strcmp(name, "formation")) *out = Script::Formation;
    else return false;
    return true;
}

inline const char* script_name(Script s) {
    switch (s) {
        case Script::Straight: return "straight";
        case Script::Orbit: return "orbit";
        case Script::Breaks: return "breaks";
        case Script::Climb: return "climb";
        case Script::Formation: return "formation";
    }
    return "?";
}

/// <summary>
/// The formation point of a lead at lead[3] with heading h (from +y towards +x): off = (right, back, up) in the lead's
/// heading frame. Right is (cos h, -sin h), ahead (sin h, cos h): the same frame as net::join_position.
/// </summary>
inline void formation_point(const double lead[3], double h, const double off[3], double out[3]) {
    const double sh = std::sin(h), ch = std::cos(h);
    out[0] = lead[0] + off[0] * ch - off[1] * sh;
    out[1] = lead[1] - off[0] * sh - off[1] * ch;
    out[2] = lead[2] + off[2];
}

/// <summary>
/// The velocity of that point: the lead's, plus the offset turning with the lead's heading rate (the outside of a
/// turn is faster). The derivative of formation_point along a turn.
/// </summary>
inline void formation_velocity(const double v[3], double h, double heading_rate, const double off[3], double out[3]) {
    const double sh = std::sin(h), ch = std::cos(h);
    out[0] = v[0] + heading_rate * (-off[0] * sh - off[1] * ch);
    out[1] = v[1] + heading_rate * (-off[0] * ch + off[1] * sh);
    out[2] = v[2];
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
            case Script::Straight:
            case Script::Formation: break;
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

using net::ClockSync;  // the shared clock estimate lives in net/clock_sync.h

}  // namespace LazyWorks::SF2Patch::bot
