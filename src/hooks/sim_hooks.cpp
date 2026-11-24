// Simulation step hook and game clock. Part of the aircraft tracker (aircraft_track.cpp), which installs and
// removes the hook (sim_install / sim_remove) only while a client needs tracking: the probe, or the Tacview
// Recording setting. Nothing is hooked otherwise.
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
// Finding the player: the tracker marks it (LiveAircraft::is_player: the first aircraft constructed since the
// table was last empty). The hook caches its pointer and construction seq. When the player is destroyed it is
// unknown (sim_player_known() is false) until the table empties and a new one is constructed; the hook never falls
// back to another aircraft. A step of the cached player costs one pointer compare, plus one tick read;
// once per second, and whenever the tracker's destroy generation changed, it re-checks the cache under the
// probe lock (this catches an address reused by another aircraft). The step of another aircraft takes the lock only
// when no player is cached and none was lost (or the destroy generation changed), when the destroy generation changed
// while a player is cached, or when the cached player has not stepped for 250 ms; and then at most once per 50 ms.
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {

namespace {

constexpr DWORD kUpdateRva = 0x466c0;  // void __thiscall Update(void* aircraft, float dt)  (ret 4)
constexpr int kMaxCallbacks = 8;
constexpr unsigned kDtStatSteps = 300;
constexpr ULONGLONG kRevalidateMs = 1000;  // re-check the cached player under the lock this often
constexpr ULONGLONG kStaleMs = 250;        // no player step for this long while others update: cache suspect
constexpr ULONGLONG kRetryMs = 50;         // minimum gap between lock-taking refresh attempts

using Update_t = void(__fastcall*)(void* self, void* edx, float dt);
Update_t o_Update = nullptr;

std::atomic<SimStepCallback> g_callbacks[kMaxCallbacks];
std::atomic<int> g_callback_count{0};
std::atomic<bool> g_run_active{false};

// Game thread only.
double g_game_time = 0;
uint64_t g_steps = 0;
void* g_player = nullptr;  // cached player aircraft, or null
uint64_t g_player_seq = 0;  // construction seq of the cached player; 0 = none cached
bool g_player_lost = false;  // a player was cached and has been destroyed; cleared when a new one is found
uint32_t g_player_epoch = 0;  // bumped on every loss and every change of player
ULONGLONG g_last_player_tick = 0;  // tick of the last player step
ULONGLONG g_last_validate = 0;     // tick of the last refresh or validation
uint32_t g_player_gen = 0;         // g_aircraft_generation when the cache was last refreshed or validated
double g_next_log = 1.0;

// dt statistics over the first kDtStatSteps player steps.
unsigned g_dt_n = 0;
double g_dt_min = 0, g_dt_max = 0, g_dt_sum = 0;

struct PlayerCtx {
    void* obj = nullptr;  // out when refreshing; in when validating
    uint64_t seq = 0;
    bool validate = false;  // in: only check that obj and seq are still listed and still the player
    bool found = false;
};

// Runs under the table's shared lock: reads only, no logging.
void player_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& c = *static_cast<PlayerCtx*>(vctx);
    if (c.validate) {
        for (size_t i = 0; i < n; ++i) {
            if (list[i].obj == c.obj && list[i].seq == c.seq && list[i].is_player) c.found = true;
        }
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        if (list[i].is_player) {
            c.obj = list[i].obj;
            c.seq = list[i].seq;
            c.found = true;
            return;
        }
    }
}

// Applies the result of a table lookup to the cache. Outside the lock, so it may log.
void apply_player(const PlayerCtx& c) {
    if (c.found) {
        if (g_player_seq != 0 && c.seq != g_player_seq) {
            // The table emptied and refilled while no step noticed (mission end, then a new load): a loss, then a
            // new player.
            ++g_player_epoch;
            LOG("sim: player lost (destroyed); a new player was chosen");
        }
        g_player = c.obj;
        g_player_seq = c.seq;
        g_player_lost = false;  // the tracker logs "player chosen"
        return;
    }
    const bool had = g_player_seq != 0;
    g_player = nullptr;
    g_player_seq = 0;
    if (had) {
        g_player_lost = true;
        ++g_player_epoch;
        LOG("sim: player lost (destroyed); unknown until the aircraft table empties and a new one is constructed");
    }
}

// Slow path: the cache is empty or suspect. Takes the table lock, so callers throttle it by tick.
void refresh_player(ULONGLONG now) {
    g_last_validate = now;
    const uint32_t gen = g_aircraft_generation.load(std::memory_order_acquire);  // before the table read
    PlayerCtx c;
    probe_with_aircraft(player_cb, &c);
    g_player_gen = gen;
    apply_player(c);
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
            c.seq = g_player_seq;
            c.validate = true;
            probe_with_aircraft(player_cb, &c);
            if (!c.found) {  // destroyed (or its address was reused): the player is now unknown
                apply_player(c);
                is_player = false;
            }
        }
    } else {
        // Another aircraft: go to the lock only when no player is cached, none stepped lately, or an aircraft was
        // destroyed since the last check (the cached player may be the one that went: it never steps again).
        const ULONGLONG now = GetTickCount64();
        const bool changed = g_aircraft_generation.load(std::memory_order_acquire) != g_player_gen;
        // After a loss, look again only when the generation changed: a new player needs the table to empty and
        // refill, and both bump it.
        const bool need = g_player ? (changed || now - g_last_player_tick >= kStaleMs) : (!g_player_lost || changed);
        if (need && now - g_last_validate >= kRetryMs) {
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

bool sim_player_known() { return g_player != nullptr; }
uint32_t sim_player_epoch() { return g_player_epoch; }
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

bool sim_install(HMODULE aircraft_dll) {
    const bool ok = create_and_enable(at_rva(aircraft_dll, kUpdateRva), reinterpret_cast<void*>(&h_Update),
                                      reinterpret_cast<void**>(&o_Update), "AircraftObject Update");
    if (ok) LOG("sim: game clock and step callbacks on (aircraft Update hook)");
    return ok;
}

void sim_remove(HMODULE aircraft_dll) {
    MH_STATUS s = MH_RemoveHook(at_rva(aircraft_dll, kUpdateRva));
    (void)s;  // MH_ERROR_NOT_CREATED is expected
}

}  // namespace lazysf2::hooks
