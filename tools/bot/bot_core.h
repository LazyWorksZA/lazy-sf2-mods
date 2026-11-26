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

/// <summary>
/// The lead's heading rate from two of its headings (h0 at t0, h1 at t1), the shorter way round. A State's rates are
/// the game's own (body axes, not Euler rates), so formation_velocity takes this, never rates[1]. 0 if the samples are
/// not 0 &lt; dt &lt;= max_dt apart.
/// </summary>
inline double heading_rate(double h0, double t0, double h1, double t1, double max_dt = 0.25) {
    const double dt = t1 - t0;
    if (!(dt > 0.0) || dt > max_dt) return 0.0;
    return std::remainder(h1 - h0, 2.0 * kPi) / dt;  // the shorter way round
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

/// <summary>
/// The bot's damage story (--damage), t seconds from its start: hit at 3 s (level 1, part 1 shows damage), part 2 lost
/// at 6 s (level 2, a system destroyed), on fire at 8 s (level 3), the pilot ejects at 10 s, destroyed at 12 s.
/// </summary>
struct DamageStep {
    uint8_t level = 0, flags = 0;  // net::kDmg*
    uint64_t lost = 0, shown = 0, dead = 0;
};
inline DamageStep damage_script_at(double t) {
    DamageStep d;
    if (t >= 3) {
        d.level = 1;
        d.shown = 1ull << 1;
    }
    if (t >= 6) {
        d.level = 2;
        d.lost = 1ull << 2;
        d.dead = 1ull << 3;
    }
    if (t >= 8) {
        d.level = 3;
        d.flags |= 0x01;
    }
    if (t >= 10) d.flags |= 0x02;
    if (t >= 12) {
        d.level = 5;
        d.flags |= 0x04;
    }
    return d;
}

/// <summary>Scripted jet controls for the bot's AvatarState (Phase 5): none, `ab` (a 14 s cycle), `guns` (Mil, a 1 s
/// primary-gun burst every 4 s) or `ab+guns` (both).</summary>
enum class JetScript { None, Ab, Guns, AbGuns };
inline bool parse_jet_script(const char* name, JetScript* out) {
    if (!name) return false;
    if (!std::strcmp(name, "none")) *out = JetScript::None;
    else if (!std::strcmp(name, "ab")) *out = JetScript::Ab;
    else if (!std::strcmp(name, "guns")) *out = JetScript::Guns;
    else if (!std::strcmp(name, "ab+guns")) *out = JetScript::AbGuns;
    else return false;
    return true;
}
/// <summary>
/// `ab` at t seconds from its start: idle 4 s, Mil (0.7) 4 s, full afterburner 4 s, then the engines off at idle
/// for 2 s, and again. throttle is input channel 3 (0..1); engines has bit 0 set while running.
/// </summary>
inline void jet_script_at(JetScript s, double t, float* throttle, uint8_t* engines, uint8_t* guns = nullptr) {
    *throttle = 0.0f;
    *engines = 0xff;
    if (guns) *guns = 0;
    if (s == JetScript::None || !(t >= 0)) return;
    if (guns && s != JetScript::Ab && std::fmod(t, 4.0) >= 3.0) *guns = 1;
    if (s == JetScript::Guns) {
        *throttle = 0.7f;
        return;
    }
    const double p = std::fmod(t, 14.0);
    if (p < 4.0) *throttle = 0.0f;
    else if (p < 8.0) *throttle = 0.7f;
    else if (p < 12.0) *throttle = 1.0f;
    else *engines = 0;
}

}  // namespace LazyWorks::SF2Patch::bot
