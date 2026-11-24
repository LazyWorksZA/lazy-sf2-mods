// Simulation step hook and game clock (developer tool, part of the object probe; see probe_hooks.cpp).
// Inert unless the probe is on: the hook is only installed from probe_on_modules_loaded().
//
// Hooks the aircraft's own Update(dt) (AircraftObject.dll, main vtable slot 9). The game calls it once
// per simulation step for every aircraft, on its main thread, and not while the game is paused. After
// the original returns, the hook adds dt to a game clock when the aircraft is the player's, and then
// calls the registered "after Update" callbacks. Features that need game time (recorder, replay) use
// the clock and the callbacks instead of wall time, so they pause with the game.
//
// Calling convention: __thiscall(void* aircraft, float dt), one 4-byte stack argument, the callee pops
// it ("ret 4"). The repo's __fastcall(self, edx, float dt) pattern matches that exactly.
//
// All state below is touched from the game's main thread only (the hook), except the callback slots and
// the run-active flag, which are atomics.
//
// Finding the player: the player is the earliest-created entry of the probe table. The hook caches its
// pointer and created tick. A step of the cached player costs one pointer compare, plus one tick read;
// once per second, and whenever the probe's destroy generation changed, it re-checks the cache under the
// probe lock (this catches an address reused by another aircraft). Other aircraft take the lock only when no player is cached, or none has stepped for
// 250 ms, and then at most once per 50 ms.
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {

namespace {

constexpr DWORD kAircraftTimestamp = 0x51d220bd, kAircraftImageSize = 0xc2000;
constexpr DWORD kUpdateRva = 0x466c0;  // void __thiscall Update(void* aircraft, float dt)  (ret 4)
constexpr int kMaxCallbacks = 4;
constexpr unsigned kDtStatSteps = 300;
constexpr ULONGLONG kRevalidateMs = 1000;  // re-check the cached player under the lock this often
constexpr ULONGLONG kStaleMs = 250;        // no player step for this long while others update: cache suspect
constexpr ULONGLONG kRetryMs = 50;         // minimum gap between lock-taking refresh attempts

using Update_t = void(__fastcall*)(void* self, void* edx, float dt);
Update_t o_Update = nullptr;

std::atomic<bool> g_hook_tried{false};
std::atomic<SimStepCallback> g_callbacks[kMaxCallbacks];
std::atomic<int> g_callback_count{0};
std::atomic<bool> g_run_active{false};

// Game thread only.
double g_game_time = 0;
uint64_t g_steps = 0;
void* g_player = nullptr;  // cached player aircraft, or null
ULONGLONG g_player_created = 0;
ULONGLONG g_last_player_tick = 0;  // tick of the last player step
ULONGLONG g_last_validate = 0;     // tick of the last refresh or validation
uint32_t g_player_gen = 0;         // g_aircraft_generation when the cache was last refreshed or validated
double g_next_log = 1.0;

// dt statistics over the first kDtStatSteps player steps.
unsigned g_dt_n = 0;
double g_dt_min = 0, g_dt_max = 0, g_dt_sum = 0;

struct PlayerCtx {
    void* obj = nullptr;  // out when refreshing; in when validating
    ULONGLONG created = 0;
    bool validate = false;  // in: only check that obj and created are still listed and earliest
    bool found = false;
};

// Runs under the table's shared lock: reads only, no logging.
void player_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& c = *static_cast<PlayerCtx*>(vctx);
    if (c.validate) {
        bool listed = false, earliest = true;
        for (size_t i = 0; i < n; ++i) {
            if (list[i].obj == c.obj && list[i].created == c.created) listed = true;
            if (list[i].created < c.created) earliest = false;
        }
        c.found = listed && earliest;
        return;
    }
    if (n == 0) return;
    size_t player = 0;
    for (size_t i = 1; i < n; ++i) {
        if (list[i].created < list[player].created) player = i;
    }
    c.obj = list[player].obj;
    c.created = list[player].created;
    c.found = true;
}

// Slow path: the cache is empty or suspect. Takes the probe lock, so callers throttle it by tick.
void refresh_player(ULONGLONG now) {
    g_last_validate = now;
    const uint32_t gen = g_aircraft_generation.load(std::memory_order_acquire);  // before the table read
    PlayerCtx c;
    probe_with_aircraft(player_cb, &c);
    g_player_gen = gen;
    g_player = c.found ? c.obj : nullptr;
    g_player_created = c.found ? c.created : 0;
    g_last_player_tick = now;
}

void report_dt_stats() {
    LOG("sim: dt over the first %u player steps: min %.2f ms, max %.2f ms, mean %.2f ms", kDtStatSteps,
        g_dt_min * 1000.0, g_dt_max * 1000.0, g_dt_sum / kDtStatSteps * 1000.0);
}

void __fastcall h_Update(void* self, void* edx, float dt) {
    o_Update(self, edx, dt);
    bool is_player = false;
    if (self == g_player && self) {
        is_player = true;
        const ULONGLONG now = GetTickCount64();
        g_last_player_tick = now;
        const uint32_t gen = g_aircraft_generation.load(std::memory_order_acquire);
        // An aircraft was destroyed (or an address reused) since the last check: the cached pointer may now
        // belong to a new aircraft, so check at once instead of waiting for the periodic re-check.
        if (gen != g_player_gen || now - g_last_validate >= kRevalidateMs) {
            g_last_validate = now;
            g_player_gen = gen;
            PlayerCtx c;
            c.obj = g_player;
            c.created = g_player_created;
            c.validate = true;
            probe_with_aircraft(player_cb, &c);
            if (!c.found) {  // gone, or no longer the earliest: drop the cache, find it again on a later step
                g_player = nullptr;
                is_player = false;
            }
        }
    } else {
        // Another aircraft: go to the lock only when no player is cached or none stepped lately.
        const ULONGLONG now = GetTickCount64();
        if ((!g_player || now - g_last_player_tick >= kStaleMs) && now - g_last_validate >= kRetryMs) {
            refresh_player(now);
            is_player = self == g_player && self;
        }
    }
    if (is_player) {
        g_game_time += static_cast<double>(dt);
        ++g_steps;
        if (g_dt_n < kDtStatSteps) {
            const double d = static_cast<double>(dt);
            if (g_dt_n == 0 || d < g_dt_min) g_dt_min = d;
            if (g_dt_n == 0 || d > g_dt_max) g_dt_max = d;
            g_dt_sum += d;
            if (++g_dt_n == kDtStatSteps) report_dt_stats();
        }
        if (g_run_active.load(std::memory_order_relaxed) && g_game_time >= g_next_log) {
            LOG("sim: t=%.1fs steps=%llu", g_game_time, static_cast<unsigned long long>(g_steps));
            g_next_log = static_cast<double>(static_cast<long long>(g_game_time)) + 1.0;
        }
    }
    const int n = g_callback_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (SimStepCallback cb = g_callbacks[i].load(std::memory_order_relaxed)) cb(self, dt, is_player);
    }
}

}  // namespace

double sim_game_time() { return g_game_time; }
uint64_t sim_step_count() { return g_steps; }

bool sim_register_step_callback(SimStepCallback cb) {
    if (!cb) return false;
    for (int i = 0; i < kMaxCallbacks; ++i) {
        SimStepCallback expected = nullptr;
        if (g_callbacks[i].compare_exchange_strong(expected, cb) || expected == cb) {
            int cur = g_callback_count.load();
            while (cur < i + 1 && !g_callback_count.compare_exchange_weak(cur, i + 1, std::memory_order_release)) {
            }
            return true;
        }
    }
    return false;
}

void sim_set_run_active(bool active) { g_run_active.store(active, std::memory_order_relaxed); }

void sim_on_modules_loaded() {
    if (g_hook_tried.load()) return;
    HMODULE m = GetModuleHandleW(L"AircraftObject.dll");
    if (!m || g_hook_tried.exchange(true)) return;
    if (!is_expected_build(m, kAircraftTimestamp, kAircraftImageSize, "AircraftObject.dll")) return;
    if (create_and_enable(at_rva(m, kUpdateRva), reinterpret_cast<void*>(&h_Update),
                          reinterpret_cast<void**>(&o_Update), "AircraftObject Update")) {
        LOG("sim: game clock and step callbacks on (aircraft Update hook)");
    }
}

}  // namespace lazysf2::hooks
