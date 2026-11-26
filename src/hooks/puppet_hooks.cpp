// Puppet mode (developer tool, part of the probe): holds the nearest AI aircraft on a level path to test whether
// the game lets us drive it. Run state belongs to the Present thread; aircraft access is under probe_with_aircraft().
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

// 0 idle, 1 start requested, 2 running, 3 stop requested (hotkey).
std::atomic<int> g_puppet_state{0};

namespace {

enum State { kIdle = 0, kStartRequested = 1, kRunning = 2, kStopRequested = 3 };

constexpr size_t kPosOffset = 0x38, kVelOffset = 0x50, kRotOffset = 0xdc;
constexpr size_t kAngleOffset = 0x100;  // pitch, heading, roll in radians: the real attitude state
constexpr size_t kFlagsOffset = 0xc, kRatesOffset = 0xd0;  // object flag word; angular rates float[3]
constexpr DWORD kRemoteMaskRva = 0xb7b28;  // AircraftObject.dll global: the REMOTEOBJECT flag mask
constexpr double kMaxSeconds = 120.0;
// true: set the game's REMOTEOBJECT flag once and only measure (the game skips AI and coasts the
// aircraft). false: the old mode, writing position, velocity and attitude every frame.
constexpr bool kCoast = true;

struct Vec3d {
    double x, y, z;
};

// The run. Present thread only.
struct Run {
    void* target = nullptr;
    unsigned long long seq = 0;  // identifies the object, in case its address is reused
    double last_alt = 0;
    uint32_t mask = 0, orig_flags = 0;  // REMOTEOBJECT mask, and the flag word before we set it
    bool cleared_logged = false;
    bool first_frame_done = false;  // frame 1 is the teleport; its drift is not a measurement
    Vec3d p0{}, v{};
    float r0[9] = {};
    float a0[3] = {};  // pitch, heading, roll
    LARGE_INTEGER start{};
    double next_log = 1.0;
    unsigned frames = 0;
    double drift_max = 0, drift_sum = 0;
    double turn_max = 0, turn_sum = 0;  // degrees
    double angle_max = 0;               // degrees
};
Run g_run;
LARGE_INTEGER g_freq{};
bool g_thread_logged = false;

void read_pos(const void* obj, Vec3d* p) { memcpy(p, static_cast<const char*>(obj) + kPosOffset, sizeof(*p)); }

struct StartCtx {
    int result = 0;  // 0 ok, 1 fewer than 2 aircraft, 2 player too slow, 3 no player
    void* player = nullptr;
    wchar_t name[64] = {};
    double distance = 0, speed = 0, heading_deg = 0;
    Run run;
    uint32_t mask = 0;  // in: REMOTEOBJECT mask
};

void start_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& c = *static_cast<StartCtx*>(vctx);
    if (n < 2) {
        c.result = 1;
        return;
    }
    size_t player = n;
    for (size_t i = 0; i < n; ++i) {
        if (list[i].is_player) player = i;
    }
    if (player == n) {
        c.result = 3;
        return;
    }
    Vec3d pp;
    read_pos(list[player].obj, &pp);
    size_t best = n;
    double best_d = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i == player) continue;
        Vec3d p;
        read_pos(list[i].obj, &p);
        const double d = std::sqrt((p.x - pp.x) * (p.x - pp.x) + (p.y - pp.y) * (p.y - pp.y) +
                                   (p.z - pp.z) * (p.z - pp.z));
        if (best == n || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    const char* pobj = static_cast<const char*>(list[player].obj);
    float pv[3], hp_f;
    memcpy(pv, pobj + kVelOffset, sizeof(pv));
    memcpy(&hp_f, pobj + kAngleOffset + 4, sizeof(hp_f));  // heading, radians from north toward east
    const double speed = std::sqrt(double(pv[0]) * pv[0] + double(pv[1]) * pv[1] + double(pv[2]) * pv[2]);
    c.speed = speed;
    if (speed < 1.0) {
        c.result = 2;
        return;
    }
    // Place the puppet 150 m ahead of the player, 20 m to the right and 10 m up, flying level on
    // the player's heading at the player's speed.
    const double hp = hp_f, sh = std::sin(hp), ch = std::cos(hp);
    Run& r = c.run;
    r.target = list[best].obj;
    r.seq = list[best].seq;
    r.p0 = {pp.x + 150.0 * sh + 20.0 * ch, pp.y + 150.0 * ch - 20.0 * sh, pp.z + 10.0};
    r.v = {speed * sh, speed * ch, 0.0};
    const float rows[9] = {float(ch), float(-sh), 0.f, float(sh), float(ch), 0.f, 0.f, 0.f, 1.f};
    memcpy(r.r0, rows, sizeof(rows));
    r.a0[0] = 0.f;
    r.a0[1] = hp_f;
    r.a0[2] = 0.f;
    c.heading_deg = hp * 180.0 / 3.14159265358979323846;
    c.player = list[player].obj;
    if (kCoast) {
        char* obj = static_cast<char*>(list[best].obj);
        r.mask = c.mask;
        memcpy(&r.orig_flags, obj + kFlagsOffset, sizeof(uint32_t));
        const float zero[3] = {};
        memcpy(obj + kRatesOffset, zero, sizeof(zero));
        const uint32_t flags = r.orig_flags | c.mask;
        memcpy(obj + kFlagsOffset, &flags, sizeof(flags));
        const float v[3] = {float(r.v.x), float(r.v.y), float(r.v.z)};
        memcpy(obj + kPosOffset, &r.p0, sizeof(r.p0));
        memcpy(obj + kVelOffset, v, sizeof(v));
        memcpy(obj + kRotOffset, r.r0, sizeof(r.r0));
        memcpy(obj + kAngleOffset, r.a0, sizeof(r.a0));
    }
    wcsncpy_s(c.name, list[best].name, _TRUNCATE);
    c.distance = best_d;
}

void start_run() {
    if (net_session_active()) {
        LOG("puppet: not started, a network session is running");
        g_puppet_state.store(kIdle);
        return;
    }
    if (replay_active()) {
        LOG("puppet: not started, a replay is running");
        g_puppet_state.store(kIdle);
        return;
    }
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    StartCtx c;
    if (kCoast) {
        const HMODULE m = GetModuleHandleW(L"AircraftObject.dll");  // the probe checked its build stamp
        if (m) memcpy(&c.mask, reinterpret_cast<const char*>(m) + kRemoteMaskRva, sizeof(c.mask));
        if (!c.mask) {
            LOG("puppet: REMOTEOBJECT mask not set");
            g_puppet_state.store(kIdle);
            return;
        }
    }
    probe_with_aircraft(start_cb, &c);
    if (c.result == 1) {
        LOG("puppet: need at least 2 aircraft");
        g_puppet_state.store(kIdle);
        return;
    }
    if (c.result == 3) {
        LOG("puppet: the player is unknown (destroyed, or not seen); not starting");
        g_puppet_state.store(kIdle);
        return;
    }
    if (c.result == 2) {
        LOG("puppet: player speed %.1f m/s is below 1 m/s; not starting", c.speed);
        g_puppet_state.store(kIdle);
        return;
    }
    g_run = c.run;
    QueryPerformanceCounter(&g_run.start);
    LOG("puppet: start target %p \"%ls\" placed 150 m ahead of player %p at (%.0f, %.0f, %.0f) heading %.1f deg speed "
        "%.1f m/s (was %.0f m away)%s",
        g_run.target, c.name, c.player, g_run.p0.x, g_run.p0.y, g_run.p0.z, c.heading_deg, c.speed, c.distance,
        kCoast ? " (REMOTEOBJECT coast)" : "");
    // A stop request that arrived while starting wins and is handled next frame.
    int expected = kStartRequested;
    g_puppet_state.compare_exchange_strong(expected, kRunning);
}

struct FrameCtx {
    double t = 0;
    bool destroyed = false;
    double drift = 0;
    double turn = 0;  // degrees between the current and the held orientation
    double angle = 0;  // largest per-component angle difference, degrees
    double alt = 0;
    bool cleared = false;  // the game cleared REMOTEOBJECT since the last frame
};

void frame_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& c = *static_cast<FrameCtx*>(vctx);
    bool live = false;
    for (size_t i = 0; i < n; ++i) {
        if (list[i].obj == g_run.target && list[i].seq == g_run.seq) {
            live = true;
            break;
        }
    }
    if (!live) {
        c.destroyed = true;
        return;
    }
    char* obj = static_cast<char*>(g_run.target);
    Vec3d cur;
    read_pos(obj, &cur);
    const Vec3d want{g_run.p0.x + g_run.v.x * c.t, g_run.p0.y + g_run.v.y * c.t, g_run.p0.z + g_run.v.z * c.t};
    c.drift = std::sqrt((cur.x - want.x) * (cur.x - want.x) + (cur.y - want.y) * (cur.y - want.y) +
                        (cur.z - want.z) * (cur.z - want.z));
    c.alt = cur.z;
    g_run.last_alt = cur.z;
    float rc[9];
    memcpy(rc, obj + kRotOffset, sizeof(rc));
    double trace = 0;  // trace(R0^T * R) = sum of element-wise products
    for (int i = 0; i < 9; ++i) trace += double(g_run.r0[i]) * rc[i];
    c.turn = std::acos(std::fmin(1.0, std::fmax(-1.0, (trace - 1.0) / 2.0))) * 180.0 / 3.14159265358979323846;
    float ac[3];
    memcpy(ac, obj + kAngleOffset, sizeof(ac));
    for (int i = 0; i < 3; ++i) {
        double d = std::fmod((double(ac[i]) - g_run.a0[i]) * 180.0 / 3.14159265358979323846, 360.0);
        if (d > 180.0) d -= 360.0;
        if (d < -180.0) d += 360.0;
        if (std::fabs(d) > c.angle) c.angle = std::fabs(d);
    }
    if (kCoast) {
        uint32_t flags;
        memcpy(&flags, obj + kFlagsOffset, sizeof(flags));
        if ((flags & g_run.mask) != g_run.mask) {
            c.cleared = true;
            flags |= g_run.mask;
            memcpy(obj + kFlagsOffset, &flags, sizeof(flags));
        }
        return;  // coast: measure only
    }
    const float v[3] = {static_cast<float>(g_run.v.x), static_cast<float>(g_run.v.y), static_cast<float>(g_run.v.z)};
    memcpy(obj + kPosOffset, &want, sizeof(want));
    memcpy(obj + kVelOffset, v, sizeof(v));
    memcpy(obj + kRotOffset, g_run.r0, sizeof(g_run.r0));
    memcpy(obj + kAngleOffset, g_run.a0, sizeof(g_run.a0));
}

void log_stats(double t, double alt) {
    LOG("puppet: t=%.0fs frames=%u drift max %.2f m mean %.2f m, turn max %.3f deg mean %.3f deg, angles max %.3f deg (alt %.0f m)", t,
        g_run.frames, g_run.drift_max, g_run.drift_sum / g_run.frames, g_run.turn_max,
        g_run.turn_sum / g_run.frames, g_run.angle_max, alt);
}

void stop_cb(const LiveAircraft* list, size_t n, void*) {
    for (size_t i = 0; i < n; ++i) {
        if (list[i].obj == g_run.target && list[i].seq == g_run.seq) {
            char* obj = static_cast<char*>(g_run.target);
            uint32_t flags;
            memcpy(&flags, obj + kFlagsOffset, sizeof(flags));
            flags &= ~(g_run.mask & ~g_run.orig_flags);  // clear only what we set
            memcpy(obj + kFlagsOffset, &flags, sizeof(flags));
            return;
        }
    }
}

void stop_run(const char* reason, double t) {
    if (kCoast && g_run.target) probe_with_aircraft(stop_cb, nullptr);  // hand the aircraft back to the AI
    if (g_run.frames > 0) log_stats(t, g_run.last_alt);  // the last, partial second
    g_puppet_state.store(kIdle);
    g_run = Run{};  // a later "hotkey before start" must not see this run's numbers
    LOG("puppet: stop (%s) after %.1f s", reason, t);
}

}  // namespace

void puppet_toggle() {
    int s = g_puppet_state.load();
    if (s == kIdle) {
        g_puppet_state.compare_exchange_strong(s, kStartRequested);
    } else if (s == kRunning || s == kStartRequested) {
        g_puppet_state.compare_exchange_strong(s, kStopRequested);
    }
}

void puppet_run_frame() {
    if (!g_thread_logged) {
        g_thread_logged = true;
        LOG("puppet: Present thread %lu", GetCurrentThreadId());
    }
    const int s = g_puppet_state.load();
    if (s == kStartRequested) {
        start_run();
        return;
    }
    if (s == kIdle) return;
    // kRunning or kStopRequested
    if (!g_run.target) {  // stop arrived before the start was processed: nothing was touched
        stop_run("hotkey before start", 0.0);
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double t = static_cast<double>(now.QuadPart - g_run.start.QuadPart) / static_cast<double>(g_freq.QuadPart);
    if (s == kStopRequested) {
        stop_run("hotkey", t);
        return;
    }
    if (t >= kMaxSeconds) {
        stop_run("120 s limit", t);
        return;
    }
    FrameCtx c;
    c.t = t;
    probe_with_aircraft(frame_cb, &c);
    if (c.destroyed) {
        LOG("puppet: target destroyed");
        stop_run("target destroyed", t);
        return;
    }
    if (c.cleared && !g_run.cleared_logged) {
        g_run.cleared_logged = true;
        LOG("puppet: game cleared REMOTEOBJECT at t=%.1f s", t);
    }
    if (!g_run.first_frame_done) {
        g_run.first_frame_done = true;
        return;
    }
    ++g_run.frames;
    g_run.drift_sum += c.drift;
    if (c.drift > g_run.drift_max) g_run.drift_max = c.drift;
    g_run.turn_sum += c.turn;
    if (c.turn > g_run.turn_max) g_run.turn_max = c.turn;
    if (c.angle > g_run.angle_max) g_run.angle_max = c.angle;
    if (t >= g_run.next_log) {
        log_stats(t, c.alt);
        g_run.next_log = std::floor(t) + 1.0;
        g_run.frames = 0;
        g_run.drift_max = g_run.drift_sum = g_run.turn_max = g_run.turn_sum = g_run.angle_max = 0;
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
