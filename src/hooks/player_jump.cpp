// Player jump (developer tool, part of the probe): Ctrl+Shift+J moves the player's own aircraft 1 km ahead and turns
// it 90 degrees right (position, angles, matrix and world velocity); Ctrl+Shift+K moves it 1 km ahead only. Then it
// logs the player's state for 5 s of game time, to show whether the flight model keeps the written state.
// Game thread only; the hotkey thread just sets a request. See lazy-sf2-recon: network-recon §8 (R3).
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "hooks/hooks.h"
#include "hooks/replay_math.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

using replay_math::kPi;
using replay_math::wrap_pi;

// Offsets in the aircraft object (network-recon §4.1, §4.2).
constexpr size_t kOffPos = 0x38, kOffVel = 0x50, kOffBodyVel = 0xb8, kOffRates = 0xd0, kOffMatrix = 0xdc,
                 kOffAngles = 0x100, kOffHeading2 = 0x150, kOffSpeed = 0x624;
constexpr double kAhead = 1000.0;
constexpr double kWatchSeconds = 5.0;

enum Request : int { kNone = 0, kFull = 1, kPositionOnly = 2 };
std::atomic<int> g_request{kNone};

// Game thread only.
bool g_watching = false;
bool g_full = false;
uint32_t g_epoch = 0;
double g_t0 = 0;
double g_next_log = 0;
bool g_logged_first = false;
double g_pos0[3] = {};
float g_vel0[3] = {};

template <typename T>
void read_at(const void* obj, size_t off, T* out, size_t n) {
    std::memcpy(out, static_cast<const char*>(obj) + off, n * sizeof(T));
}
template <typename T>
void write_at(void* obj, size_t off, const T* in, size_t n) {
    std::memcpy(static_cast<char*>(obj) + off, in, n * sizeof(T));
}

constexpr double deg(double r) { return r * 180.0 / kPi; }

/// <summary>Turns a world vector 90 degrees right (heading from north toward east grows by 90).</summary>
void turn_right(float v[3]) {
    const float x = v[0];
    v[0] = v[1];
    v[1] = -x;
}

/// <summary>heading + 90 degrees, kept in the convention of the original value: (-pi, pi], or [0, 2 pi) above pi.</summary>
float turned_heading(float h) {
    const double t = static_cast<double>(h) + kPi / 2.0;
    if (h > kPi) return static_cast<float>(t >= 2.0 * kPi ? t - 2.0 * kPi : t);
    return static_cast<float>(wrap_pi(t));
}

void log_state(const char* what, void* obj, double t) {
    double pos[3];
    float vel[3], bv[3], rates[3], ang[3], h2, speed;
    read_at(obj, kOffPos, pos, 3);
    read_at(obj, kOffVel, vel, 3);
    read_at(obj, kOffBodyVel, bv, 3);
    read_at(obj, kOffRates, rates, 3);
    read_at(obj, kOffAngles, ang, 3);
    read_at(obj, kOffHeading2, &h2, 1);
    read_at(obj, kOffSpeed, &speed, 1);
    // Where a straight line from the written state would be now: a large miss means the move was undone.
    const double dt = t - g_t0;
    double miss = 0;
    for (int i = 0; i < 3; ++i) {
        const double d = pos[i] - (g_pos0[i] + static_cast<double>(g_vel0[i]) * dt);
        miss += d * d;
    }
    const double v = std::sqrt(double(vel[0]) * vel[0] + double(vel[1]) * vel[1] + double(vel[2]) * vel[2]);
    const double vel_heading = deg(std::atan2(double(vel[0]), double(vel[1])));
    const double written_heading = deg(std::atan2(double(g_vel0[0]), double(g_vel0[1])));
    LOG("jump: %s t+%.2fs pos (%.0f, %.0f, %.0f) off-line %.1f m | vel %.1f m/s heading %.1f (written %.1f) | "
        "angles p %.1f h %.1f r %.1f, +0x150 %.1f | body V (%.1f, %.1f, %.1f) rates (%.3f, %.3f, %.3f) +0x624 %.1f",
        what, dt, pos[0], pos[1], pos[2], std::sqrt(miss), v, vel_heading, written_heading, deg(ang[0]),
        deg(ang[1]), deg(ang[2]), deg(h2), bv[0], bv[1], bv[2], rates[0], rates[1], rates[2], speed);
}

void do_jump(void* obj, bool full) {
    double pos[3];
    float vel[3], m[9], ang[3], h2;
    read_at(obj, kOffPos, pos, 3);
    read_at(obj, kOffVel, vel, 3);
    read_at(obj, kOffMatrix, m, 9);
    read_at(obj, kOffAngles, ang, 3);
    read_at(obj, kOffHeading2, &h2, 1);
    g_t0 = sim_game_time();
    std::memcpy(g_pos0, pos, sizeof(pos));
    std::memcpy(g_vel0, vel, sizeof(vel));
    log_state("before", obj, g_t0);

    const double h = static_cast<double>(ang[1]);
    pos[0] += kAhead * std::sin(h);
    pos[1] += kAhead * std::cos(h);
    write_at(obj, kOffPos, pos, 3);
    if (full) {
        turn_right(vel);
        for (int row = 0; row < 3; ++row) turn_right(m + 3 * row);  // rows right, forward, up: world vectors
        const bool h2_is_copy = std::fabs(wrap_pi(static_cast<double>(h2) - h)) < 1e-3;
        ang[1] = turned_heading(ang[1]);
        write_at(obj, kOffVel, vel, 3);
        write_at(obj, kOffMatrix, m, 9);
        write_at(obj, kOffAngles, ang, 3);
        if (h2_is_copy) {
            h2 = turned_heading(h2);
            write_at(obj, kOffHeading2, &h2, 1);
        }
        LOG("jump: wrote position +%.0f m ahead, heading +90 (angles, matrix, world velocity%s); body V and rates "
            "left as they were",
            kAhead, h2_is_copy ? ", +0x150" : "; +0x150 differs from the heading, left alone");
    } else {
        LOG("jump: wrote position +%.0f m ahead only", kAhead);
    }
    std::memcpy(g_pos0, pos, sizeof(pos));
    std::memcpy(g_vel0, vel, sizeof(vel));
    g_full = full;
    g_epoch = sim_player_epoch();
    g_watching = true;
    g_logged_first = false;
    g_next_log = g_t0 + 1.0;
}

void on_step(void* aircraft, float, bool is_player) {
    if (!is_player) return;
    if (g_watching) {
        if (sim_player_epoch() != g_epoch) {
            g_watching = false;
            LOG("jump: player lost; watch stopped");
            return;
        }
        const double t = sim_game_time();
        if (!g_logged_first) {
            g_logged_first = true;
            log_state("next step", aircraft, t);
        } else if (t >= g_next_log) {
            log_state("watch", aircraft, t);
            g_next_log += 1.0;
            if (t - g_t0 >= kWatchSeconds) {
                g_watching = false;
                LOG("jump: watch done (%s)", g_full ? "full" : "position only");
            }
        }
    }
    const int req = g_request.exchange(kNone);
    if (req == kNone) return;
    if (g_watching) {
        LOG("jump: ignored, the last jump is still being watched");
        return;
    }
    if (host_has_clients() || replay_active()) {
        LOG("jump: ignored while a host session or replay runs");
        return;
    }
    do_jump(aircraft, req == kFull);
}

}  // namespace

void player_jump_request(bool full) { g_request.store(full ? kFull : kPositionOnly); }

void player_jump_on_modules_loaded() {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    if (sim_register_step_callback(on_step)) {
        LOG("jump: on (Ctrl+Shift+J moves the player 1 km ahead and turns it 90 degrees; Ctrl+Shift+K moves it only)");
    } else {
        LOG("jump: no free step callback slot");
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
