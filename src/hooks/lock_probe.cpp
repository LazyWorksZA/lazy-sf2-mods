// Lock probe (developer tool, part of the probe): forces radar locks for the Tacview lock check.
//   Ctrl+Shift+A  places an AI aircraft (the nearest enemy, else the nearest other aircraft) 3 km in front of the player,
//                 with the player's altitude, attitude and velocity.
//   Ctrl+Shift+O  makes the player's radar lock it through the avionics' own commands (power, mode, next target, lock).
//   Ctrl+Shift+I  makes that aircraft's radar track and then lock the player.
// Each logs the radar state before, during and after. Game thread only; the hotkey thread just sets a request.
// See lazy-sf2-recon: recon-notes §27.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

constexpr DWORD kAircraftTimestamp = 0x51d220bd, kAircraftImageSize = 0xc2000;
constexpr DWORD kAvionicsServiceRva = 0xac3cc;  // AircraftObject.dll global: the player's AVN service (or null)
constexpr DWORD kRadarSetStateRva = 0x333b0;    // radar block: __thiscall(state)
// Aircraft object (network-recon §4.1).
constexpr size_t kOffActive = 0x04, kOffId = 0x08, kOffPos = 0x38, kOffVel = 0x50, kOffMatrix = 0xdc, kOffAngles = 0x100,
                 kOffHeading2 = 0x150, kOffWarnings = 0x5f4;
// Radar block and the detect interface (recon-notes §27).
constexpr size_t kOffRadar = 0x970, kOffDetect = 0x24;
constexpr size_t kDetectSetMode = 0x50 / 4, kDetectSetTarget = 0x54 / 4;
// AVN service vtable slots and radar command ids.
constexpr size_t kAvnCommand = 0x10 / 4, kAvnMode = 0x18 / 4, kAvnTargetId = 0x1c / 4;
constexpr int kCmdRadarPower = 0x19, kCmdModeNext = 0x1a, kCmdNextTarget = 0x1e, kCmdLock = 0x20, kCmdBreakLock = 0x22;
constexpr double kAheadMetres = 3000.0;
constexpr double kStepSeconds = 0.5;
constexpr int kMaxAttempts = 24;
constexpr double kWatchSeconds = 10.0;

using Command_t = void(__fastcall*)(void* self, void* edx, int cmd);
using GetInt_t = int(__fastcall*)(void* self, void* edx);
using SetInt_t = void(__fastcall*)(void* self, void* edx, int v);

enum Request : int { kNone = 0, kPlace = 1, kPlayerLock = 2, kAiLock = 3 };
std::atomic<int> g_request{kNone};

// Game thread only.
HMODULE g_ac = nullptr;
bool g_ac_ok = false;
void* g_target = nullptr;  // the placed aircraft (pointer, plus seq to know it is still the same one)
uint64_t g_target_seq = 0;
enum Phase { kIdle, kPower, kMode, kSelect, kLockCmd, kCheck, kAiWatch };
Phase g_phase = kIdle;
double g_next = 0;
double g_until = 0;
int g_attempts = 0;
uint32_t g_epoch = 0;

struct RadarView {
    bool ok = false;
    int state = 0, stage = 0;
    const void* target = nullptr;
    uint32_t warnings = 0;
};

RadarView read_radar(const void* obj) {
    RadarView v;
    __try {
        const char* b = static_cast<const char*>(obj);
        v.state = *reinterpret_cast<const volatile int*>(b + kOffRadar);
        v.stage = *reinterpret_cast<const volatile int*>(b + kOffRadar + 4);
        v.target = *reinterpret_cast<const void* const volatile*>(b + kOffRadar + 0x18);
        v.warnings = *reinterpret_cast<const volatile uint32_t*>(b + kOffWarnings);
        v.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v.ok = false;
    }
    return v;
}

int read_id(const void* obj) {
    __try {
        return *reinterpret_cast<const volatile int*>(static_cast<const char*>(obj) + kOffId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void* avionics() {
    __try {
        return *reinterpret_cast<void* volatile*>(reinterpret_cast<char*>(g_ac) + kAvionicsServiceRva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void** vtable_of(void* obj) { return *static_cast<void***>(obj); }

int avn_int(void* avn, size_t slot) {
    __try {
        return reinterpret_cast<GetInt_t>(vtable_of(avn)[slot])(avn, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

bool avn_command(void* avn, int cmd) {
    __try {
        reinterpret_cast<Command_t>(vtable_of(avn)[kAvnCommand])(avn, nullptr, cmd);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool detect_set(void* obj, size_t slot, int v) {
    __try {
        void* det = static_cast<char*>(obj) + kOffDetect;
        reinterpret_cast<SetInt_t>(vtable_of(det)[slot])(det, nullptr, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool radar_set_state(void* obj, int state) {
    __try {
        reinterpret_cast<SetInt_t>(reinterpret_cast<char*>(g_ac) + kRadarSetStateRva)(static_cast<char*>(obj) + kOffRadar,
                                                                                       nullptr, state);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void avn_module(void* avn, wchar_t* out, DWORD n) {
    out[0] = L'\0';
    HMODULE m = nullptr;
    if (avn && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCWSTR>(vtable_of(avn)[0]), &m))
        GetModuleFileNameW(m, out, n);
}

// The position of a still active aircraft (object +0x04 non-zero); false when finished or unreadable.
bool read_pos_if_active(const void* obj, double pos[3]) {
    __try {
        const char* b = static_cast<const char*>(obj);
        if (*reinterpret_cast<const volatile int*>(b + kOffActive) == 0) return false;
        std::memcpy(pos, b + kOffPos, 3 * sizeof(double));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Pick {
    const void* player = nullptr;
    void* best = nullptr;
    uint64_t best_seq = 0;
    bool best_enemy = false;
    double best_d2 = 0;
    bool target_alive = false;  // g_target is still listed with g_target_seq
    const wchar_t* best_name = nullptr;
    double ppos[3] = {};
};

void pick_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& p = *static_cast<Pick*>(vctx);
    for (size_t i = 0; i < n; ++i)
        if (list[i].obj == g_target && list[i].seq == g_target_seq) p.target_alive = true;
    for (size_t i = 0; i < n; ++i) {
        if (list[i].is_player || list[i].obj == p.player) continue;
        double pos[3];
        if (!read_pos_if_active(list[i].obj, pos)) continue;  // finished (a wreck) or unreadable
        const double dx = pos[0] - p.ppos[0], dy = pos[1] - p.ppos[1], dz = pos[2] - p.ppos[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        const bool enemy = list[i].side == AircraftSide::Enemy;
        if (!p.best || (enemy && !p.best_enemy) || (enemy == p.best_enemy && d2 < p.best_d2)) {
            p.best = list[i].obj;
            p.best_seq = list[i].seq;
            p.best_enemy = enemy;
            p.best_d2 = d2;
            p.best_name = list[i].name;
        }
    }
}

void log_radar(const char* what, void* player) {
    void* avn = avionics();
    const RadarView pr = read_radar(player);
    const int mode = avn ? avn_int(avn, kAvnMode) : -1;
    const int tid = avn ? avn_int(avn, kAvnTargetId) : -1;
    const RadarView tr = g_target ? read_radar(g_target) : RadarView{};
    LOG("lock probe: %s | player avionics %p mode %d target id %d | player radar state %d stage %d target %p, "
        "warnings +0x5f4 %#x | placed %p id %d radar state %d stage %d target %p",
        what, avn, mode, tid, pr.state, pr.stage, pr.target, pr.warnings, g_target, g_target ? read_id(g_target) : -1,
        tr.state, tr.stage, tr.target);
}

void do_place(void* player) {
    Pick p;
    p.player = player;
    std::memcpy(p.ppos, static_cast<const char*>(player) + kOffPos, sizeof(p.ppos));
    probe_with_aircraft(pick_cb, &p);
    if (!p.best) {
        LOG("lock probe: place ignored, no other aircraft is live");
        return;
    }
    double pos[3];
    float vel[3], m[9], ang[3], h2;
    std::memcpy(pos, static_cast<const char*>(player) + kOffPos, sizeof(pos));
    std::memcpy(vel, static_cast<const char*>(player) + kOffVel, sizeof(vel));
    std::memcpy(m, static_cast<const char*>(player) + kOffMatrix, sizeof(m));
    std::memcpy(ang, static_cast<const char*>(player) + kOffAngles, sizeof(ang));
    std::memcpy(&h2, static_cast<const char*>(player) + kOffHeading2, sizeof(h2));
    const double h = static_cast<double>(ang[1]);
    pos[0] += kAheadMetres * std::sin(h);
    pos[1] += kAheadMetres * std::cos(h);
    char* o = static_cast<char*>(p.best);
    std::memcpy(o + kOffPos, pos, sizeof(pos));
    std::memcpy(o + kOffVel, vel, sizeof(vel));
    std::memcpy(o + kOffMatrix, m, sizeof(m));
    std::memcpy(o + kOffAngles, ang, sizeof(ang));
    std::memcpy(o + kOffHeading2, &h2, sizeof(h2));
    g_target = p.best;
    g_target_seq = p.best_seq;
    LOG("lock probe: placed %ls (%s, seq %llu, id %d, was %.0f m away) %.0f m ahead at (%.0f, %.0f, %.0f), heading "
        "%.1f deg, the player's velocity",
        p.best_name ? p.best_name : L"?", p.best_enemy ? "enemy" : "not an enemy",
        static_cast<unsigned long long>(p.best_seq), read_id(p.best), std::sqrt(p.best_d2), kAheadMetres, pos[0],
        pos[1], pos[2], h * 180.0 / 3.14159265358979323846);
}

bool target_alive(void* player) {
    if (!g_target) return false;
    Pick p;
    p.player = player;
    probe_with_aircraft(pick_cb, &p);
    if (!p.target_alive) {
        LOG("lock probe: the placed aircraft is gone");
        g_target = nullptr;
    }
    return p.target_alive;
}

void start_player_lock(void* player, double t) {
    void* avn = avionics();
    wchar_t mod[MAX_PATH];
    avn_module(avn, mod, MAX_PATH);
    log_radar("player lock: before", player);
    if (!avn) {
        LOG("lock probe: player lock ignored, the player has no avionics service (no radar avionics loaded)");
        return;
    }
    LOG("lock probe: player avionics service in %ls", mod[0] ? mod : L"?");
    if (!g_target) LOG("lock probe: no placed aircraft (Ctrl+Shift+A); any locked air target counts");
    g_attempts = 0;
    g_phase = kPower;
    g_next = t;
}

// The player-lock steps: radar on, air search mode, then next target + lock until the placed aircraft is locked.
void step_player_lock(void* player, double t) {
    if (t < g_next) return;
    void* avn = avionics();
    if (!avn) {
        LOG("lock probe: the avionics service went away; stopped");
        g_phase = kIdle;
        return;
    }
    const int mode = avn_int(avn, kAvnMode);
    switch (g_phase) {
        case kPower:
            if (mode == 0) {
                const bool ok = avn_command(avn, kCmdRadarPower);
                LOG("lock probe: radar was off; sent power (0x19)%s", ok ? "" : " - the call faulted");
                g_next = t + 1.0;
            }
            g_phase = kMode;
            g_attempts = 0;
            return;
        case kMode:
            if (mode != 1 && mode != 2 && mode != 3) {
                if (++g_attempts > 8) {
                    log_radar("player lock: no air mode after 8 mode steps; stopped", player);
                    g_phase = kIdle;
                    return;
                }
                avn_command(avn, kCmdModeNext);
                LOG("lock probe: mode %d is not an air mode; sent next mode (0x1a)", mode);
                g_next = t + 0.3;
                return;
            }
            log_radar("player lock: radar on in an air mode", player);
            g_attempts = 0;
            g_phase = kSelect;
            return;
        case kSelect:
            if (++g_attempts > kMaxAttempts) {
                log_radar("player lock: gave up", player);
                g_phase = kIdle;
                return;
            }
            avn_command(avn, kCmdNextTarget);
            g_next = t + kStepSeconds;
            g_phase = kLockCmd;
            return;
        case kLockCmd:
            avn_command(avn, kCmdLock);
            g_next = t + kStepSeconds;
            g_phase = kCheck;
            return;
        case kCheck: {
            const int tid = avn_int(avn, kAvnTargetId);
            const int want = g_target ? read_id(g_target) : -1;
            char what[96];
            std::snprintf(what, sizeof(what), "player lock: attempt %d, next target + lock", g_attempts);
            log_radar(what, player);
            if (tid != -1 && (want == -1 || tid == want)) {
                log_radar("player lock: LOCKED", player);
                g_phase = kIdle;
                return;
            }
            if (tid != -1) avn_command(avn, kCmdBreakLock);  // locked something else: break and try the next
            g_phase = kSelect;
            g_next = t + kStepSeconds;
            return;
        }
        default: return;
    }
}

void start_ai_lock(void* player, double t) {
    if (!g_target) {
        LOG("lock probe: AI lock ignored, no placed aircraft (Ctrl+Shift+A first)");
        return;
    }
    log_radar("AI lock: before", player);
    const int pid = read_id(player);
    const bool a = detect_set(g_target, kDetectSetTarget, pid);
    const bool b = detect_set(g_target, kDetectSetMode, 2);    // track: state 3, stage 2
    const bool c = radar_set_state(g_target, 4);               // single-target track: the stage climbs to 3 (lock)
    LOG("lock probe: AI lock: set target = player id %d%s, mode 2%s, state 4%s", pid, a ? "" : " (faulted)",
        b ? "" : " (faulted)", c ? "" : " (faulted)");
    log_radar("AI lock: after the calls", player);
    g_phase = kAiWatch;
    g_next = t + kStepSeconds;
    g_until = t + kWatchSeconds;
}

void on_step(void* aircraft, float, bool is_player) {
    if (!is_player) return;
    const double t = sim_game_time();
    if (g_phase != kIdle && sim_player_epoch() != g_epoch) {
        g_phase = kIdle;
        g_target = nullptr;
        LOG("lock probe: player lost; stopped");
    }
    if (g_phase == kAiWatch) {
        if (t >= g_next) {
            if (!target_alive(aircraft)) {
                g_phase = kIdle;
            } else {
                log_radar(t >= g_until ? "AI lock: watch done" : "AI lock: watch", aircraft);
                g_next = t + kStepSeconds;
                if (t >= g_until) g_phase = kIdle;
            }
        }
    } else if (g_phase != kIdle) {
        step_player_lock(aircraft, t);
    }
    const int req = g_request.exchange(kNone);
    if (req == kNone) return;
    if (!g_ac_ok) {
        LOG("lock probe: ignored, AircraftObject.dll is not the expected build");
        return;
    }
    if (host_has_clients() || replay_active()) {
        LOG("lock probe: ignored while a host session or replay runs");
        return;
    }
    if (sim_player_epoch() != g_epoch) {
        g_epoch = sim_player_epoch();
        g_target = nullptr;  // a new flight: the last placed aircraft is not ours to touch
    }
    if (g_target && !target_alive(aircraft)) g_target = nullptr;
    switch (req) {
        case kPlace: do_place(aircraft); break;
        case kPlayerLock: start_player_lock(aircraft, t); break;
        case kAiLock: start_ai_lock(aircraft, t); break;
        default: break;
    }
}

}  // namespace

void lock_probe_request(int what) { g_request.store(what); }

void lock_probe_on_modules_loaded() {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    g_ac = GetModuleHandleW(L"AircraftObject.dll");
    g_ac_ok = is_expected_build(g_ac, kAircraftTimestamp, kAircraftImageSize, "AircraftObject.dll");
    if (!g_ac_ok) {
        LOG("lock probe: off (AircraftObject.dll is not the expected build)");
        return;
    }
    if (sim_register_step_callback(on_step))
        LOG("lock probe: on (Ctrl+Shift+A places an AI 3 km ahead, Ctrl+Shift+O locks it with the player's radar, "
            "Ctrl+Shift+I makes it lock the player)");
    else
        LOG("lock probe: no free step callback slot");
}

}  // namespace LazyWorks::SF2Patch::hooks
