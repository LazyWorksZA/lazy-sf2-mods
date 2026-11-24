// Replay with network-style smoothing (developer tool, part of the object probe; see probe_hooks.cpp).
// Inert unless the probe is on: replay_on_modules_loaded() is only called from probe_on_modules_loaded(),
// and the hotkeys live in the probe's hotkey thread, which only exists then.
//
// Ctrl+Shift+T replays the latest recording (recorder.cpp) onto the AI aircraft nearest to the player,
// as if it arrived over a network: a simulated sender plays the recording at game time, and one sample
// per feed period (60, 10, 5 or 2 Hz; Ctrl+Shift+Y cycles) counts as "received". The receiver keeps the
// last 8 received samples and, on every Update of the target, shows the state at render time
// t_rec - delay: Hermite interpolation between received samples, dead reckoning past the newest, and a
// correction offset that decays over 200 ms whenever a new sample (or a rate change) makes the shown
// state jump. The target is flagged REMOTEOBJECT (no AI, no flight model), so we are the only writer.
// The math is in replay_math.h (pure functions, compile-time self-check). Metrics compare what we wrote
// with the recording itself at the same render time, once per second of game time.
//
// Threads: the hotkey thread only sets g_toggle_request and g_rate_idx (atomics). Everything else runs on
// the game thread, inside the "after Update" callback of sim_hooks.cpp. Nothing is logged under the
// probe's lock, and nothing is allocated per step.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "hooks/hooks.h"
#include "hooks/replay_math.h"
#include "log.h"

namespace lazysf2::hooks {

namespace {

using namespace replay_math;

constexpr DWORD kRemoteMaskRva = 0xb7b28;  // AircraftObject.dll global: the REMOTEOBJECT flag mask
constexpr size_t kOffFlags = 0xc, kOffPos = 0x38, kOffVel = 0x50, kOffRates = 0xd0, kOffMatrix = 0xdc,
                 kOffAngles = 0x100;
constexpr int kRates[4] = {60, 10, 5, 2};
constexpr int kRingSize = 8;
constexpr double kMaxCatchUp = 1.0;  // after a stall longer than this, skip the feed boundaries we missed
constexpr double kG = 9.80665;

std::atomic<bool> g_toggle_request{false};
std::atomic<int> g_rate_idx{0};
std::atomic<bool> g_active{false};  // true from a successful start to the stop

// Placement of the recording into the world: rotate about the vertical by d, then move the first position
// to the anchor.
struct Placement {
    double c = 1, s = 0;  // cos d, sin d
    double d = 0;         // heading change, radians
    double first[3] = {};
    double anchor[3] = {};
    double t0 = 0;        // recording time of the first sample
};

Samp place(const RecSample& r, const Placement& pl) {
    Samp o;
    o.t = r.t - pl.t0;
    const double rel[3] = {r.pos[0] - pl.first[0], r.pos[1] - pl.first[1], r.pos[2] - pl.first[2]};
    o.k.p[0] = pl.anchor[0] + rel[0] * pl.c + rel[1] * pl.s;
    o.k.p[1] = pl.anchor[1] - rel[0] * pl.s + rel[1] * pl.c;
    o.k.p[2] = pl.anchor[2] + rel[2];
    o.k.v[0] = r.vel[0] * pl.c + r.vel[1] * pl.s;
    o.k.v[1] = -r.vel[0] * pl.s + r.vel[1] * pl.c;
    o.k.v[2] = r.vel[2];
    o.k.a[0] = r.ang[0];
    o.k.a[1] = r.ang[1] + pl.d;
    o.k.a[2] = r.ang[2];
    for (int i = 0; i < 3; ++i) o.k.w[i] = r.rate[i];  // body-axis rates (probably): written, not integrated
    return o;
}

// Rotation matrix rows (right, forward, up) from pitch, heading, roll. Verified from live dumps: pitch nose-up
// positive, roll right-wing-down positive, heading in (-pi, pi]. Heading is from north toward east.
void build_matrix(const double a[3], float m[9]) {
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

template <typename T>
void read_at(const void* obj, size_t off, T* out, size_t n) {
    std::memcpy(out, static_cast<const char*>(obj) + off, n * sizeof(T));
}
template <typename T>
void write_at(void* obj, size_t off, const T* in, size_t n) {
    std::memcpy(static_cast<char*>(obj) + off, in, n * sizeof(T));
}

// Largest distance between two kinematic states' positions, and their largest angle component difference.
double dist(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
double max_angle_diff_deg(const double a[3], const double b[3]) {
    double m = 0;
    for (int i = 0; i < 3; ++i) m = std::fmax(m, std::fabs(angle_diff(a[i], b[i])));
    return m * 180.0 / kPi;
}

// Acceleration implied by three positions at three times, in g. For equal steps this is
// |p[n] - 2 p[n-1] + p[n-2]| / dt^2; with unequal steps it is the change of the two mean velocities over the
// mean step, so a jittery step length doesn't read as acceleration.
double accel_g(const double p[3], double t, const double p1[3], double t1, const double p2[3], double t2) {
    const double dn = t - t1, dp = t1 - t2;
    if (!(dn > 1e-6 && dp > 1e-6)) return -1;
    double s = 0;
    for (int i = 0; i < 3; ++i) {
        const double dv = (p[i] - p1[i]) / dn - (p1[i] - p2[i]) / dp;
        s += dv * dv;
    }
    return std::sqrt(s) / (0.5 * (dn + dp)) / kG;
}

// The run. Game thread only.
struct Run {
    bool active = false;
    void* target = nullptr;
    unsigned long long target_created = 0;
    void* player = nullptr;
    unsigned long long player_created = 0;
    uint32_t gen = 0;  // g_aircraft_generation when both aircraft were last confirmed live
    uint32_t set_bits = 0;  // REMOTEOBJECT bits that we set (mask & ~original flags)

    const RecSample* rec = nullptr;
    uint32_t count = 0;
    Placement pl;
    double t_start = 0;  // game time at the start

    // Sender and receiver.
    int rate = 0;        // feed rate in force, Hz
    double delay = 0;    // interpolation delay in force, seconds
    bool have_boundary = false;
    double last_boundary = 0;  // recording time of the last feed boundary processed
    uint32_t send_idx = 0;     // latest recording sample at or before the last boundary
    int64_t last_sent = -1;    // index of the last recording sample pushed to the receiver
    Samp ring[kRingSize];      // received samples, oldest first
    int ring_n = 0;
    uint32_t truth_idx = 0;    // search hint into the recording

    // Correction offset: shown = raw + offset * blend_weight(now - off_t).
    double off_p[3] = {}, off_a[3] = {};
    double off_t = 0;

    // History of what we wrote and of the truth (game-clock times), for the acceleration figures.
    double wp[2][3] = {}, wt[2] = {};
    double tp[2][3] = {}, tt[2] = {};
    int hist = 0;

    // Diagnostics (what the game actually holds), per one-second window.
    double last_written[3] = {};  // position we wrote on the previous target step
    bool have_written = false;
    double player_pos[3] = {};    // from the player's callback
    bool have_player_pos = false;
    double actual[3] = {};        // the target's position as read before our latest write
    unsigned diag_cb = 0, diag_writes = 0;
    double diag_moved = 0;
    bool diag_cleared = false;
    double diag_next_log = 1.0;   // recording time

    // Metrics for the current one-second window.
    double next_log = 1.0;  // recording time
    unsigned n = 0;
    double err_max = 0, err_sum = 0, ang_max = 0, acc_max = 0, truth_acc_max = 0;
};
Run g_run;

// --- Liveness ------------------------------------------------------------------------------------

struct LiveCtx {
    void* target;
    unsigned long long target_created;
    void* player;
    unsigned long long player_created;
    bool target_live = false, player_live = false;
};

void live_cb(const LiveAircraft* list, size_t n, void* vctx) {  // under the shared lock: no logging
    auto& c = *static_cast<LiveCtx*>(vctx);
    for (size_t i = 0; i < n; ++i) {
        if (list[i].obj == c.target && list[i].created == c.target_created) c.target_live = true;
        if (list[i].obj == c.player && list[i].created == c.player_created) c.player_live = true;
    }
}

void restore_flags() {  // target must be live
    uint32_t flags;
    read_at(g_run.target, kOffFlags, &flags, 1);
    flags &= ~g_run.set_bits;  // clear only what we set
    write_at(g_run.target, kOffFlags, &flags, 1);
}

void flush_metrics(double t_rec);

void stop_run(const char* reason, bool target_live) {
    const double t_rec = sim_game_time() - g_run.t_start;
    if (target_live) restore_flags();
    if (g_run.n > 0) flush_metrics(t_rec);  // the last, partial second
    g_active.store(false);
    sim_set_run_active(false);
    g_run = Run{};
    LOG("replay: stop (%s) after %.1f s%s", reason, t_rec, target_live ? "" : " (target gone, flags not restored)");
}

// True if both aircraft are still live. Only goes to the probe table when an aircraft was destroyed since the
// last check (a cached pointer is trusted while the generation is unchanged). Stops the run if not.
bool still_live() {
    const uint32_t gen = g_aircraft_generation.load(std::memory_order_acquire);  // before the table read
    if (gen == g_run.gen) return true;
    LiveCtx c{g_run.target, g_run.target_created, g_run.player, g_run.player_created};
    probe_with_aircraft(live_cb, &c);
    if (!c.target_live) {
        stop_run("target destroyed", false);
        return false;
    }
    if (!c.player_live) {
        stop_run("player destroyed", true);
        return false;
    }
    g_run.gen = gen;
    return true;
}

// --- Start ---------------------------------------------------------------------------------------

struct StartCtx {
    void* player;       // in
    uint32_t mask;      // in: REMOTEOBJECT mask
    const double* pp;   // in: player position
    int result = 0;     // 0 ok, 1 fewer than 2 aircraft
    void* target = nullptr;
    unsigned long long created = 0, player_created = 0;
    wchar_t name[64] = {};
    double distance = 0;
    uint32_t orig_flags = 0;
    double target_pos[3] = {};  // before placement
};

void start_cb(const LiveAircraft* list, size_t n, void* vctx) {  // under the shared lock: no logging
    auto& c = *static_cast<StartCtx*>(vctx);
    size_t best = n;
    double best_d = 0;
    bool player_listed = false;
    for (size_t i = 0; i < n; ++i) {
        if (list[i].obj == c.player) {
            player_listed = true;
            c.player_created = list[i].created;
            continue;
        }
        double p[3];
        read_at(list[i].obj, kOffPos, p, 3);
        const double d = dist(p, c.pp);
        if (best == n || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    if (!player_listed || best == n) {
        c.result = 1;
        return;
    }
    c.target = list[best].obj;
    c.created = list[best].created;
    c.distance = best_d;
    read_at(c.target, kOffPos, c.target_pos, 3);
    wcsncpy_s(c.name, list[best].name, _TRUNCATE);
    // REMOTEOBJECT on, rates zero: the aircraft now coasts, and we are the only writer.
    char* obj = static_cast<char*>(c.target);
    read_at(obj, kOffFlags, &c.orig_flags, 1);
    const float zero[3] = {};
    write_at(obj, kOffRates, zero, 3);
    const uint32_t flags = c.orig_flags | c.mask;
    write_at(obj, kOffFlags, &flags, 1);
}

void try_start(void* player) {
    if (g_puppet_state.load() != 0) {
        LOG("replay: not started, a puppet run is active");
        return;
    }
    uint32_t count = 0;
    const RecSample* rec = recorder_latest(&count);
    if (!rec || count < 2) {
        LOG("replay: not started, no finished recording (none made yet, one is being recorded, or fewer than 2 "
            "samples)");
        return;
    }
    if (rec[count - 1].t <= rec[0].t) {
        LOG("replay: not started, the recording has no duration");
        return;
    }
    uint32_t mask = 0;
    if (const HMODULE m = GetModuleHandleW(L"AircraftObject.dll")) {  // the probe checked its build stamp
        std::memcpy(&mask, reinterpret_cast<const char*>(m) + kRemoteMaskRva, sizeof(mask));
    }
    if (!mask) {
        LOG("replay: not started, REMOTEOBJECT mask not set");
        return;
    }
    double pp[3];
    float pang[3];
    read_at(player, kOffPos, pp, 3);
    read_at(player, kOffAngles, pang, 3);
    const uint32_t gen = g_aircraft_generation.load(std::memory_order_acquire);  // before the table read
    StartCtx c;
    c.player = player;
    c.mask = mask;
    c.pp = pp;
    probe_with_aircraft(start_cb, &c);
    if (c.result != 0) {
        LOG("replay: not started, need the player and at least one other aircraft");
        return;
    }

    Run r;
    r.active = true;
    r.target = c.target;
    r.target_created = c.created;
    r.player = player;
    r.player_created = c.player_created;
    r.gen = gen;
    r.set_bits = mask & ~c.orig_flags;
    r.rec = rec;
    r.count = count;
    // Placement: rotate by (player heading now - recording's first heading), first position to 150 m ahead,
    // 20 m right and 10 m above the player (f and r as in puppet mode).
    const double hp = pang[1], sh = std::sin(hp), ch = std::cos(hp);
    r.pl.d = angle_diff(hp, rec[0].ang[1]);
    r.pl.c = std::cos(r.pl.d);
    r.pl.s = std::sin(r.pl.d);
    r.pl.t0 = rec[0].t;
    for (int i = 0; i < 3; ++i) r.pl.first[i] = rec[0].pos[i];
    r.pl.anchor[0] = pp[0] + 150.0 * sh + 20.0 * ch;
    r.pl.anchor[1] = pp[1] + 150.0 * ch - 20.0 * sh;
    r.pl.anchor[2] = pp[2] + 10.0;
    r.t_start = sim_game_time();
    r.diag_next_log = 1.0;
    g_run = r;
    g_active.store(true);
    sim_set_run_active(true);
    LOG("replay: diag start player %p at (%.1f, %.1f, %.1f) heading %.1f deg; target was at (%.1f, %.1f, %.1f); "
        "first position (%.1f, %.1f, %.1f)",
        player, pp[0], pp[1], pp[2], hp * 180.0 / kPi, c.target_pos[0], c.target_pos[1], c.target_pos[2],
        r.pl.anchor[0], r.pl.anchor[1], r.pl.anchor[2]);
    LOG("replay: start target %p \"%ls\" (was %.0f m from the player), %u samples, %.1f s, rotated %.1f deg, "
        "first position at (%.0f, %.0f, %.0f), feed %d Hz",
        c.target, c.name, c.distance, count, rec[count - 1].t - rec[0].t, r.pl.d * 180.0 / kPi, r.pl.anchor[0],
        r.pl.anchor[1], r.pl.anchor[2], kRates[g_rate_idx.load(std::memory_order_relaxed)]);
}

// Once per second: what the game actually holds against what we wrote. Not under any lock.
void diag_log(double t_rec, const double actual[3], bool flag_ok) {
    const bool cleared = g_run.diag_cleared || !flag_ok;
    const double dp = g_run.have_player_pos ? dist(g_run.player_pos, actual) : -1.0;
    LOG("replay: diag t=%.0fs cb %u writes %u, actual (%.1f, %.1f, %.1f) vs written (%.1f, %.1f, %.1f) moved %.1f m, "
        "flag %s, player dist %.0f m",
        t_rec, g_run.diag_cb, g_run.diag_writes, actual[0], actual[1], actual[2], g_run.last_written[0],
        g_run.last_written[1], g_run.last_written[2], g_run.diag_moved, cleared ? "CLEARED" : "set", dp);
    g_run.diag_cb = g_run.diag_writes = 0;
    g_run.diag_moved = 0;
    g_run.diag_cleared = false;
    g_run.diag_next_log = std::floor(t_rec) + 1.0;
}

// --- The receiver --------------------------------------------------------------------------------

void flush_metrics(double t_rec) {
    LOG("replay: t=%.0fs rate %d Hz delay %.0f ms err max %.2f m mean %.2f m ang %.2f deg accel %.1f g (truth %.1f g)",
        t_rec, g_run.rate, g_run.delay * 1000.0, g_run.err_max, g_run.err_sum / g_run.n, g_run.ang_max, g_run.acc_max,
        g_run.truth_acc_max);
    g_run.n = 0;
    g_run.err_max = g_run.err_sum = g_run.ang_max = g_run.acc_max = g_run.truth_acc_max = 0;
    g_run.next_log = std::floor(t_rec) + 1.0;
}

double rec_time(uint32_t i) { return g_run.rec[i].t - g_run.pl.t0; }

void push_received(const Samp& s) {
    if (g_run.ring_n == kRingSize) {
        std::memmove(&g_run.ring[0], &g_run.ring[1], sizeof(Samp) * (kRingSize - 1));
        --g_run.ring_n;
    }
    g_run.ring[g_run.ring_n++] = s;
}

// The recording at recording time t, placed (the ground truth).
void truth_at(double t, Kin* out) {
    uint32_t& i = g_run.truth_idx;
    while (i + 1 < g_run.count && rec_time(i + 1) <= t) ++i;
    while (i > 0 && rec_time(i) > t) --i;
    Samp pair[2] = {place(g_run.rec[i], g_run.pl), {}};
    const int n = i + 1 < g_run.count ? 2 : 1;
    if (n == 2) pair[1] = place(g_run.rec[i + 1], g_run.pl);
    sample_at(pair, n, t, out);
}

void receive_step() {
    const double now = sim_game_time();
    const double t_rec = now - g_run.t_start;
    // Diagnostics: the target as the game left it after its own Update, before we write.
    double actual[3];
    uint32_t actual_flags;
    read_at(g_run.target, kOffPos, actual, 3);
    read_at(g_run.target, kOffFlags, &actual_flags, 1);
    const bool flag_ok = (actual_flags & g_run.set_bits) == g_run.set_bits;
    ++g_run.diag_cb;
    if (!flag_ok) g_run.diag_cleared = true;
    if (g_run.have_written) g_run.diag_moved = std::fmax(g_run.diag_moved, dist(actual, g_run.last_written));
    const int rate = kRates[g_rate_idx.load(std::memory_order_relaxed)];
    const double period = 1.0 / rate, delay = feed_delay(rate);
    // Angles past the newest sample extrapolate with a rate estimated from the two newest received samples
    // (the recorded +0xd0 values are probably body-axis rates); those are still interpolated and written as is.

    // The state we show now, under the old feed and delay, so that a change can be turned into an offset.
    bool have_before = false;
    Kin before{};
    auto capture_before = [&] {
        if (!have_before && g_run.ring_n > 0) have_before = sample_at(g_run.ring, g_run.ring_n, t_rec - g_run.delay, &before);
    };
    if (rate != g_run.rate) {
        if (g_run.rate != 0) capture_before();
        g_run.rate = rate;
        g_run.delay = delay;
    }

    // The sender: one sample per feed period, the latest one at or before the period boundary.
    double b = g_run.have_boundary ? g_run.last_boundary + period : 0.0;
    if (t_rec - b > kMaxCatchUp) b = t_rec;  // a long stall: skip the boundaries we missed
    while (t_rec >= b) {
        while (g_run.send_idx + 1 < g_run.count && rec_time(g_run.send_idx + 1) <= b) ++g_run.send_idx;
        if (static_cast<int64_t>(g_run.send_idx) > g_run.last_sent) {
            capture_before();
            push_received(place(g_run.rec[g_run.send_idx], g_run.pl));
            g_run.last_sent = g_run.send_idx;
        }
        g_run.have_boundary = true;
        g_run.last_boundary = b;
        b += period;
    }

    const double t_render = t_rec - delay;
    Kin raw;
    if (!sample_at(g_run.ring, g_run.ring_n, t_render, &raw)) return;

    // Correction blending: carry what we showed before the change minus what we would show now, and let it
    // decay, so the jump never reaches the screen. Re-base only when the change really moved the computed
    // state; otherwise the running decay must not restart.
    const double w_now = blend_weight(now - g_run.off_t, kBlendSeconds);
    bool jumped = false;
    if (have_before) {
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(before.p[i] - raw.p[i]) >= 1e-6 || std::fabs(angle_diff(before.a[i], raw.a[i])) >= 1e-9) {
                jumped = true;
            }
        }
    }
    if (jumped) {
        for (int i = 0; i < 3; ++i) {
            g_run.off_p[i] = g_run.off_p[i] * w_now + (before.p[i] - raw.p[i]);
            g_run.off_a[i] = wrap_pi(g_run.off_a[i] * w_now + angle_diff(before.a[i], raw.a[i]));
        }
        g_run.off_t = now;
    }
    const double w = blend_weight(now - g_run.off_t, kBlendSeconds);
    Kin shown = raw;
    for (int i = 0; i < 3; ++i) {
        shown.p[i] += g_run.off_p[i] * w;
        shown.a[i] = wrap_pi(shown.a[i] + g_run.off_a[i] * w);
    }

    char* obj = static_cast<char*>(g_run.target);
    const float v[3] = {float(shown.v[0]), float(shown.v[1]), float(shown.v[2])};
    const float a[3] = {float(shown.a[0]), float(shown.a[1]), float(shown.a[2])};
    const float rates[3] = {float(shown.w[0]), float(shown.w[1]), float(shown.w[2])};
    float m[9];
    build_matrix(shown.a, m);
    write_at(obj, kOffPos, shown.p, 3);
    write_at(obj, kOffVel, v, 3);
    write_at(obj, kOffRates, rates, 3);
    write_at(obj, kOffMatrix, m, 9);
    write_at(obj, kOffAngles, a, 3);
    ++g_run.diag_writes;
    std::memcpy(g_run.last_written, shown.p, sizeof(g_run.last_written));
    g_run.have_written = true;
    if (t_rec >= g_run.diag_next_log) diag_log(t_rec, actual, flag_ok);

    // Metrics against the ground truth at the same render time.
    Kin truth;
    truth_at(t_render, &truth);
    if (t_render >= 0) {  // before that we show the first sample dead-reckoned backwards; nothing to compare
        const double e = dist(shown.p, truth.p);
        ++g_run.n;
        g_run.err_sum += e;
        g_run.err_max = std::fmax(g_run.err_max, e);
        g_run.ang_max = std::fmax(g_run.ang_max, max_angle_diff_deg(shown.a, truth.a));
        if (g_run.hist >= 2) {
            g_run.acc_max = std::fmax(g_run.acc_max, accel_g(shown.p, now, g_run.wp[0], g_run.wt[0], g_run.wp[1], g_run.wt[1]));
            g_run.truth_acc_max =
                std::fmax(g_run.truth_acc_max, accel_g(truth.p, now, g_run.tp[0], g_run.tt[0], g_run.tp[1], g_run.tt[1]));
        }
    }
    if (t_render >= 0) {  // the acceleration history starts with the first real sample time
        std::memcpy(g_run.wp[1], g_run.wp[0], sizeof(g_run.wp[0]));
        std::memcpy(g_run.tp[1], g_run.tp[0], sizeof(g_run.tp[0]));
        std::memcpy(g_run.wp[0], shown.p, sizeof(g_run.wp[0]));
        std::memcpy(g_run.tp[0], truth.p, sizeof(g_run.tp[0]));
        g_run.wt[1] = g_run.wt[0];
        g_run.tt[1] = g_run.tt[0];
        g_run.wt[0] = g_run.tt[0] = now;
        if (g_run.hist < 2) ++g_run.hist;
    }
    if (g_run.n > 0 && t_rec >= g_run.next_log) flush_metrics(t_rec);

    if (t_render >= rec_time(g_run.count - 1)) stop_run("end of recording", true);
}

// --- The step callback ---------------------------------------------------------------------------

void on_step(void* aircraft, float /*dt*/, bool is_player) {
    if (g_toggle_request.load(std::memory_order_relaxed) &&
        (is_player || (g_run.active && aircraft == g_run.target))) {
        g_toggle_request.store(false, std::memory_order_relaxed);
        if (g_run.active) {
            if (still_live()) stop_run("hotkey", true);
        } else {
            try_start(aircraft);
        }
        if (is_player) return;
    }
    if (!g_run.active) return;
    if (is_player) {
        read_at(aircraft, kOffPos, g_run.player_pos, 3);
        g_run.have_player_pos = true;
    }
    if (aircraft == g_run.target || is_player) {
        if (!still_live()) return;
        if (aircraft == g_run.target) receive_step();
    }
}

bool self_check() {
    // Placement: a recording that flew north, rotated by 90 deg, flies east and ends at heading 90 deg.
    RecSample r{};
    r.pos[1] = 100.0;
    r.vel[1] = 100.0;
    Placement pl;
    pl.d = kPi / 2;
    pl.c = std::cos(pl.d);
    pl.s = std::sin(pl.d);
    const Samp s = place(r, pl);
    bool ok = approx(s.k.p[0], 100.0, 1e-9) && approx(s.k.p[1], 0.0, 1e-9) && approx(s.k.v[0], 100.0, 1e-9) &&
              approx(s.k.a[1], kPi / 2, 1e-12);
    // Level, heading east: right = south (0,-1,0)... rows are right, forward, up.
    const double a[3] = {0, kPi / 2, 0};
    float m[9];
    build_matrix(a, m);
    const float want[9] = {0, -1, 0, 1, 0, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) ok = ok && std::fabs(m[i] - want[i]) < 1e-6f;
    // Pitch 0.3, roll 0.4, heading 0: forward.z = sin P, right.z = -sin R cos P.
    const double b[3] = {0.3, 0.0, 0.4};
    build_matrix(b, m);
    ok = ok && std::fabs(m[5] - std::sin(0.3)) < 1e-6 && std::fabs(m[2] + std::sin(0.4) * std::cos(0.3)) < 1e-6;
    return ok;
}

}  // namespace

bool replay_active() { return g_active.load(std::memory_order_relaxed); }

void replay_toggle() { g_toggle_request.store(true, std::memory_order_relaxed); }

void replay_cycle_rate() {
    const int idx = (g_rate_idx.load(std::memory_order_relaxed) + 1) % 4;  // the hotkey thread is the only writer
    g_rate_idx.store(idx, std::memory_order_relaxed);
    LOG("replay: feed rate %d Hz (interpolation delay %.0f ms)", kRates[idx], feed_delay(kRates[idx]) * 1000.0);
}

void replay_on_modules_loaded() {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    if (!self_check()) LOG("replay: SELF-CHECK FAILED (placement or matrix)");
    if (sim_register_step_callback(on_step)) {
        LOG("replay: on (Ctrl+Shift+T starts and stops a replay of the latest recording, Ctrl+Shift+Y cycles the "
            "feed rate 60/10/5/2 Hz, now %d Hz)", kRates[0]);
    } else {
        LOG("replay: no free step callback slot");
    }
}

}  // namespace lazysf2::hooks
