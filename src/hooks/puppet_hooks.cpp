// Puppet mode (developer tool, part of the object probe; see probe_hooks.cpp). Inert unless the probe
// is on: the hotkey that starts a run lives in the probe's hotkey thread, which only exists then.
//
// A run takes the AI aircraft nearest to the player, freezes a level path from its start position
// and velocity, and every Present writes position, velocity and orientation back onto that path.
// The distance it finds the object from where we last put it ("drift") shows how much the game
// moves it between our writes. This is the multiplayer go/no-go test: can we hold an aircraft?
//
// All run state is owned by the thread that calls puppet_run_frame() (the Present thread). The
// hotkey thread only touches g_puppet_state. Every access to an aircraft object happens inside
// probe_with_aircraft(), i.e. under the table's shared lock.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>

#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {

// 0 idle, 1 start requested, 2 running, 3 stop requested (hotkey).
std::atomic<int> g_puppet_state{0};

namespace {

enum State { kIdle = 0, kStartRequested = 1, kRunning = 2, kStopRequested = 3 };

constexpr size_t kPosOffset = 0x38, kVelOffset = 0x50, kRotOffset = 0xdc;
constexpr double kMaxSeconds = 30.0;

struct Vec3d {
    double x, y, z;
};

// The run. Present thread only.
struct Run {
    void* target = nullptr;
    unsigned long long created = 0;  // identifies the object, in case its address is reused
    double last_alt = 0;
    Vec3d p0{}, v{};
    float r0[9] = {};
    LARGE_INTEGER start{};
    double next_log = 1.0;
    unsigned frames = 0;
    double drift_max = 0, drift_sum = 0;
};
Run g_run;
LARGE_INTEGER g_freq{};
bool g_thread_logged = false;

void read_pos(const void* obj, Vec3d* p) { memcpy(p, static_cast<const char*>(obj) + kPosOffset, sizeof(*p)); }

// --- Start (callback runs under the shared lock) --------------------------------------------------

struct StartCtx {
    int result = 0;  // 0 ok, 1 fewer than 2 aircraft, 2 target too slow, 3 too steep
    void* player = nullptr;
    wchar_t name[64] = {};
    double distance = 0, speed = 0;
    Run run;
};

void start_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& c = *static_cast<StartCtx*>(vctx);
    if (n < 2) {
        c.result = 1;
        return;
    }
    size_t player = 0;
    for (size_t i = 1; i < n; ++i) {
        if (list[i].created < list[player].created) player = i;
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
    const char* obj = static_cast<const char*>(list[best].obj);
    float v0[3];
    memcpy(v0, obj + kVelOffset, sizeof(v0));
    const double speed = std::sqrt(double(v0[0]) * v0[0] + double(v0[1]) * v0[1] + double(v0[2]) * v0[2]);
    const double horiz = std::sqrt(double(v0[0]) * v0[0] + double(v0[1]) * v0[1]);
    c.speed = speed;
    if (speed < 1.0) {
        c.result = 2;
        return;
    }
    if (horiz < 0.1 * speed) {
        c.result = 3;
        return;
    }
    Run& r = c.run;
    r.target = list[best].obj;
    r.created = list[best].created;
    read_pos(obj, &r.p0);
    r.v = {v0[0] * speed / horiz, v0[1] * speed / horiz, 0.0};  // level, same speed
    memcpy(r.r0, obj + kRotOffset, sizeof(r.r0));
    c.player = list[player].obj;
    wcsncpy_s(c.name, list[best].name, _TRUNCATE);
    c.distance = best_d;
}

void start_run() {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    StartCtx c;
    probe_with_aircraft(start_cb, &c);
    if (c.result == 1) {
        LOG("puppet: need at least 2 aircraft");
        g_puppet_state.store(kIdle);
        return;
    }
    if (c.result == 3) {
        LOG("puppet: target is climbing or diving too steeply for a level path");
        g_puppet_state.store(kIdle);
        return;
    }
    if (c.result == 2) {
        LOG("puppet: target speed %.1f m/s is below 1 m/s; not starting", c.speed);
        g_puppet_state.store(kIdle);
        return;
    }
    g_run = c.run;
    QueryPerformanceCounter(&g_run.start);
    LOG("puppet: start target %p \"%ls\" at (%.0f, %.0f, %.0f) speed %.1f m/s, player %p, distance %.0f m",
        g_run.target, c.name, g_run.p0.x, g_run.p0.y, g_run.p0.z, c.speed, c.player, c.distance);
    // A stop request that arrived while starting wins and is handled next frame.
    int expected = kStartRequested;
    g_puppet_state.compare_exchange_strong(expected, kRunning);
}

// --- One frame (callback runs under the shared lock) ----------------------------------------------

struct FrameCtx {
    double t = 0;
    bool destroyed = false;
    double drift = 0;
    double alt = 0;
};

void frame_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& c = *static_cast<FrameCtx*>(vctx);
    bool live = false;
    for (size_t i = 0; i < n; ++i) {
        if (list[i].obj == g_run.target && list[i].created == g_run.created) {
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
    const float v[3] = {static_cast<float>(g_run.v.x), static_cast<float>(g_run.v.y), static_cast<float>(g_run.v.z)};
    memcpy(obj + kPosOffset, &want, sizeof(want));
    memcpy(obj + kVelOffset, v, sizeof(v));
    memcpy(obj + kRotOffset, g_run.r0, sizeof(g_run.r0));
}

void stop_run(const char* reason, double t) {
    if (g_run.frames > 0) {  // the last, partial second
        LOG("puppet: t=%.0fs frames=%u drift max %.2f m mean %.2f m (alt %.0f m)", t, g_run.frames, g_run.drift_max,
            g_run.drift_sum / g_run.frames, g_run.last_alt);
    }
    g_puppet_state.store(kIdle);
    g_run.target = nullptr;
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
        stop_run("30 s limit", t);
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
    ++g_run.frames;
    g_run.drift_sum += c.drift;
    if (c.drift > g_run.drift_max) g_run.drift_max = c.drift;
    if (t >= g_run.next_log) {
        LOG("puppet: t=%.0fs frames=%u drift max %.2f m mean %.2f m (alt %.0f m)", t, g_run.frames, g_run.drift_max,
            g_run.drift_sum / g_run.frames, c.alt);
        g_run.next_log = std::floor(t) + 1.0;
        g_run.frames = 0;
        g_run.drift_max = g_run.drift_sum = 0;
    }
}

}  // namespace lazysf2::hooks
