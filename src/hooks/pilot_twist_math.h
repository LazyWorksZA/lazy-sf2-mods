#pragma once
// Pure math for Pilot Twist (pilot_twist.cpp): the effect curve, its filter, the twisted view yaw and the lean
// offset. No game memory, no globals, no allocation. Unit tested in tests/net_tests.cpp.
//
// Conventions (ThirdWire): body axes x right, y forward, z up; a 3x3 orientation is three rows, the body x, y and z
// axes in world coordinates. View yaw is seat-relative, in radians, positive to the right.
// See lazy-sf2-recon: recon-notes §25.
#include <cmath>

namespace LazyWorks::SF2Patch::hooks::pilot_twist {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

// Tunables: one place, one line each.
constexpr float kStartYaw = 60.0f * kDeg;   // head yaw where the effect starts
constexpr float kFullYaw = 150.0f * kDeg;   // head yaw where it is at full strength
constexpr float kMaxTwist = 15.0f * kDeg;   // extra view yaw at full strength (also how far past the seat limit)
constexpr float kYawCap = 180.0f * kDeg;    // the twisted view never turns past this
constexpr float kLeanSide = 0.06f;          // metres toward the side the pilot looks, at full strength
constexpr float kLeanUp = 0.03f;            // metres up, at full strength
constexpr float kCapSide = 0.10f;           // the lean plus the tracker's own head movement stays within these
constexpr float kCapUp = 0.06f;
constexpr float kFilterSeconds = 0.12f;     // low-pass time constant
constexpr float kSnapAfterSeconds = 0.5f;   // a longer gap (view not updated) jumps straight to the target

struct Vec3 {
    double x, y, z;
};

constexpr float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

/// <summary>Wraps an angle into (-pi, pi]. NaN and absurd values come back unchanged.</summary>
inline float wrap_pi(float a) {
    if (!(std::fabs(a) < 1e4f)) return a;
    while (a > kPi) a -= 2.0f * kPi;
    while (a <= -kPi) a += 2.0f * kPi;
    return a;
}

constexpr float smoothstep01(float x) {
    const float t = clampf(x, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// <summary>
/// Signed effect strength in [-1, 1] for a (continuous) head yaw: 0 up to kStartYaw, 1 from kFullYaw, sign of the
/// yaw. Not wrapped: a yaw past +-180 degrees keeps its side (see continuous_yaw).
/// </summary>
inline float target_weight(float yaw) {
    if (!(std::fabs(yaw) < 1e4f)) return 0.0f;
    const float s = smoothstep01((std::fabs(yaw) - kStartYaw) / (kFullYaw - kStartYaw));
    return yaw < 0 ? -s : s;
}

/// <summary>
/// The head yaw unwrapped against last frame's (prev), so turning past +-180 degrees keeps the side the pilot turned
/// to instead of flipping. Re-anchored to the wrapped yaw whenever the head is back inside kStartYaw.
/// </summary>
inline float continuous_yaw(float prev, float yaw) {
    const float y = wrap_pi(yaw);
    if (!(std::fabs(y) <= kPi)) return 0.0f;
    if (!(std::fabs(prev) < 4.0f * kPi) || std::fabs(y) < kStartYaw) return y;
    return prev + wrap_pi(y - prev);
}

/// <summary>One first-order low-pass step over dt seconds. A gap of kSnapAfterSeconds or more jumps to the target.</summary>
inline float filter_step(float current, float target, float dt) {
    if (!(dt > 0.0f)) return current;
    if (dt >= kSnapAfterSeconds || !(std::fabs(current) <= 1.0f)) return target;
    return current + (target - current) * (1.0f - std::exp(-dt / kFilterSeconds));
}

/// <summary>
/// Extra view yaw for this frame's camera: weight * kMaxTwist, keeping yaw + extra within the seat limits widened by
/// kMaxTwist (when the view limits yaw) and within +-kYawCap. Never pulls back a yaw the game already has past them.
/// </summary>
inline float twist_delta(float yaw, float weight, float min_yaw, float max_yaw, bool limited) {
    if (!(std::fabs(yaw) < 1e4f) || !(std::fabs(weight) <= 1.0f)) return 0.0f;
    float lo = -kYawCap, hi = kYawCap;
    if (limited && min_yaw <= max_yaw) {
        lo = clampf(min_yaw - kMaxTwist, -kYawCap, kYawCap);
        hi = clampf(max_yaw + kMaxTwist, -kYawCap, kYawCap);
    }
    const float t = clampf(yaw + weight * kMaxTwist, yaw < lo ? yaw : lo, yaw > hi ? yaw : hi);
    return t - yaw;
}

/// <summary>Per-view filter state, owned by the hook.</summary>
struct TwistState {
    float weight = 0.0f;  // filtered signed strength
    float yaw = 0.0f;     // continuous head yaw
    bool have = false;    // false: the next frame starts at its target
};

/// <summary>
/// Advances the state with this frame's head yaw (the game's smoothed view yaw) and returns the yaw to build this
/// frame's camera with. The head yaw itself must be put back afterwards (restore_yaw).
/// </summary>
inline float camera_yaw(TwistState& s, float head, float dt, float min_yaw, float max_yaw, bool limited) {
    s.yaw = continuous_yaw(s.have ? s.yaw : 1e9f, head);
    const float target = target_weight(s.yaw);
    s.weight = s.have ? filter_step(s.weight, target, dt) : target;
    s.have = true;
    return head + twist_delta(s.yaw, s.weight, min_yaw, max_yaw, limited);
}

/// <summary>
/// The yaw to leave in the view after the camera is built: the head yaw, unless the game changed the value we wrote
/// (then its write stands).
/// </summary>
constexpr float restore_yaw(float current, float written, float head) { return current == written ? head : current; }

/// <summary>
/// Lean offset in body axes for a weight. native_bound is the largest head movement the tracker itself may add
/// (0 without one); the sum stays within kCapSide sideways and kCapUp up.
/// </summary>
inline Vec3 lean_body(float weight, float native_bound) {
    if (!(std::fabs(weight) <= 1.0f)) return {0, 0, 0};
    const float b = native_bound > 0.0f ? native_bound : 0.0f;
    const float side_cap = kCapSide - b > 0.0f ? kCapSide - b : 0.0f;
    const float up_cap = kCapUp - b > 0.0f ? kCapUp - b : 0.0f;
    const float side = clampf(weight * kLeanSide, -side_cap, side_cap);
    const float up = clampf(std::fabs(weight) * kLeanUp, 0.0f, up_cap);
    return {side, 0.0, up};
}

/// <summary>Rotates a body-axes vector into world axes with the aircraft orientation (rows = body axes).</summary>
inline Vec3 body_to_world(const float m[9], const Vec3& b) {
    return {b.x * m[0] + b.y * m[3] + b.z * m[6], b.x * m[1] + b.y * m[4] + b.z * m[7],
            b.x * m[2] + b.y * m[5] + b.z * m[8]};
}

}  // namespace LazyWorks::SF2Patch::hooks::pilot_twist
