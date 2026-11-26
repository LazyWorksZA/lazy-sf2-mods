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
constexpr float kMaxTwist = 30.0f * kDeg;   // extra view yaw at full strength (also how far past the seat limit)
constexpr float kYawCap = 180.0f * kDeg;    // the twisted view never turns past this
constexpr float kLeanSide = 0.04f;          // metres toward the side the pilot looks, at full strength
constexpr float kLeanUp = 0.015f;           // metres up, at full strength
constexpr float kCapSide = 0.06f;           // our lean plus the game's tracker head offset stays within these
constexpr float kCapUp = 0.03f;
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
/// Lean offset in body axes for a weight. native is the game's own tracker head offset this frame, in body axes (zero
/// without a tracker). The lean only ever shrinks so that the sum stays within kCapSide sideways and kCapUp up; the
/// native offset itself is left alone.
/// </summary>
inline Vec3 lean_body(float weight, const Vec3& native) {
    if (!(std::fabs(weight) <= 1.0f) || !(std::fabs(native.x) < 1e3) || !(std::fabs(native.z) < 1e3)) return {0, 0, 0};
    const float want_side = weight * kLeanSide;
    const float side_room = static_cast<float>(want_side >= 0 ? kCapSide - native.x : kCapSide + native.x);
    const float side_mag = clampf(std::fabs(want_side), 0.0f, side_room > 0.0f ? side_room : 0.0f);
    const float up_room = static_cast<float>(kCapUp - native.z);
    const float up = clampf(std::fabs(weight) * kLeanUp, 0.0f, up_room > 0.0f ? up_room : 0.0f);
    return {want_side >= 0 ? side_mag : -side_mag, 0.0, up};
}

/// <summary>Rotates a body-axes vector into world axes with the aircraft orientation (rows = body axes).</summary>
inline Vec3 body_to_world(const float m[9], const Vec3& b) {
    return {b.x * m[0] + b.y * m[3] + b.z * m[6], b.x * m[1] + b.y * m[4] + b.z * m[7],
            b.x * m[2] + b.y * m[5] + b.z * m[8]};
}

/// <summary>Rotates a world vector into body axes (the transpose of body_to_world).</summary>
inline Vec3 world_to_body(const float m[9], const Vec3& w) {
    return {w.x * m[0] + w.y * m[1] + w.z * m[2], w.x * m[3] + w.y * m[4] + w.z * m[5],
            w.x * m[6] + w.y * m[7] + w.z * m[8]};
}

/// <summary>
/// The game's tracker head offset in camera axes (x side, z up), as +0x2ec0 computes it: tracker axes 0 and 1
/// (+-16383) scaled by the seat's movement limits (lo/hi as the seat returns them, both positive).
/// </summary>
constexpr Vec3 native_camera_offset(int axis_x, int axis_z, const float lo[3], const float hi[3]) {
    const float kx = -static_cast<float>(axis_x) / 16383.0f, kz = static_cast<float>(axis_z) / 16383.0f;
    return {kx * (kx < 0 ? lo[0] : hi[0]), 0.0, kz * (kz < 0 ? lo[2] : hi[2])};
}

// Developer head hold (probe hotkey): a fixed head yaw instead of the hat or tracker, for screenshots. Steps: 0 off,
// then 120 and 160 degrees left, then 120 and 160 degrees right. Positive view yaw is to the left (seen live), so
// right is negative.
constexpr int kHoldSteps = 5;

/// <summary>The step after step; anything out of range goes back to 0 (off).</summary>
constexpr int next_hold(int step) { return step >= 0 && step < kHoldSteps - 1 ? step + 1 : 0; }

/// <summary>The held head yaw in radians for a step (0 for off or out of range; check hold_active first).</summary>
constexpr float hold_yaw(int step) {
    constexpr float yaws[kHoldSteps] = {0.0f, 120.0f * kDeg, 160.0f * kDeg, -120.0f * kDeg, -160.0f * kDeg};
    return step >= 0 && step < kHoldSteps ? yaws[step] : 0.0f;
}

constexpr bool hold_active(int step) { return step > 0 && step < kHoldSteps; }

/// <summary>
/// A head angle as the head tracker axis value the inside view reads (input-manager axis 3 = yaw, 4 = pitch; +-16383
/// is +-180 degrees), rounded and clamped to that range. NaN gives 0.
/// </summary>
inline int tracker_axis_from_angle(float rad) {
    if (!(rad == rad)) return 0;
    const float v = rad * 16383.0f / kPi;
    return v >= 16383.0f ? 16383 : (v <= -16383.0f ? -16383 : static_cast<int>(std::lround(v)));
}

/// <summary>A short name for the log.</summary>
constexpr const char* hold_name(int step) {
    constexpr const char* names[kHoldSteps] = {"off", "120 left", "160 left", "120 right", "160 right"};
    return step >= 0 && step < kHoldSteps ? names[step] : "off";
}

}  // namespace LazyWorks::SF2Patch::hooks::pilot_twist
