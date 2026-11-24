// Tacview recorder (player-facing): writes every flight's aircraft to Saved Games\ThirdWire\<exe>\Tacview\
// SF2 <date time>.acmi when Options > Other > Tacview Recording is On.
//
// Inert while Off: the aircraft tracker only installs its hooks when the setting (or the probe) asks for them, so
// our sim step callback is never called, no thread exists, no memory is reserved and no file is made. The ring
// (about 1 MB) is allocated when the first recording starts.
//
// One file per flight. A recording starts at the first step of the player's aircraft while the setting is On
// (checked at that moment: tracking may be on for the probe alone), and stops when the player is lost (the sim's
// player epoch changes), when the writer fails or reaches the size cap, when the Options screen is left, or at
// process exit. When the aircraft table empties (the tracker's destructor hook calls tacview_recorder_table_empty(),
// which only sets a flag), the recording is closed at the next step of any aircraft or at the next menu moment (an
// Options page left), whichever comes first; a mission that ends with nothing stepping again is closed there. Before
// a stop for "player lost" or "table empty" one last empty frame is written, so the file gets its removal lines.
// The writer thread of a finished recording is only joined once it has finished: a new start waits (retrying at the
// next player step) while the previous writer is still draining, so the game thread never blocks on it.
//
// Game thread cost: at most 10 times per second of game time (20 while a weapon is in flight), on a player step, one
// pass over the live aircraft under probe_with_aircraft's shared lock and, nested in it, over the live weapons under
// probe_with_weapons': a pose read (SEH-guarded) and a name copy per object into the ring. No allocation, no I/O, no
// logging. The core limits aircraft to 10 Hz and weapons to 20 Hz each. Everything else (formatting, the rate limit, removals, the file) is the writer
// thread's (acmi/tacview_writer.h, acmi/tacview_core.h). The shell call that finds Saved Games runs once, on the
// game thread, at the first start; it is never made from the module watch.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include "acmi/tacview_core.h"
#include "acmi/tacview_writer.h"
#include "config.h"
#include "hooks/hooks.h"
#include "hooks/weapon_class.h"
#include "log.h"

namespace lazysf2::hooks {

namespace {

constexpr size_t kOffPos = 0x38, kOffAngles = 0x100;  // aircraft object: double[3] position, float[3] angles
// Weapon object (MissileObject): the body at +0x20 holds the position (double[3]); the angles (float[3], pitch,
// heading, roll as for aircraft) are at body + 0xc8. The velocity (body + 0x18, float[3]) is not recorded.
constexpr size_t kOffWeaponPos = 0x20, kOffWeaponAngles = 0xe8;
constexpr double kSnapshotInterval = 0.1;        // game seconds between snapshots: 10 Hz (aircraft only)
constexpr double kSnapshotIntervalWeapons = 0.05;  // 20 Hz while a weapon is in flight

static_assert(static_cast<uint8_t>(WeaponClass::Missile) == acmi::TacviewRec::kMissile &&
                  static_cast<uint8_t>(WeaponClass::Bomb) == acmi::TacviewRec::kBomb &&
                  static_cast<uint8_t>(WeaponClass::Rocket) == acmi::TacviewRec::kRocket,
              "weapon classes must match the recorder's");

acmi::TacviewWriter* g_writer = nullptr;  // allocated at the first recording (game thread)
std::wstring g_dir;                       // ...\Tacview, resolved once
bool g_dir_tried = false;
bool g_recording = false;
uint32_t g_epoch = 0;        // sim_player_epoch() at the start
bool g_skip_valid = false;   // a start failed or the writer failed during this player's flight: do not retry it
uint32_t g_skip_epoch = 0;
double g_last_snapshot = 0;
bool g_weapons_live = false;  // the last snapshot held a weapon: snapshot at the faster rate
std::atomic<bool> g_registered{false};
std::atomic<bool> g_table_empty{false};  // set by the tracker when the table empties; cleared at a start

void writer_log(const char* line) { LOG("%s", line); }

std::wstring display_path(const std::wstring& p) { return log::log_path(p); }

// Reads the pose of a live aircraft; false instead of a crash if the memory went away. No C++ objects here (SEH).
bool read_pose(const void* obj, size_t off_pos, size_t off_ang, double pos[3], float ang[3]) {
    __try {
        const char* b = static_cast<const char*>(obj);
        std::memcpy(pos, b + off_pos, 3 * sizeof(double));
        std::memcpy(ang, b + off_ang, 3 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct SnapCtx {
    double t = 0;
    bool wrote = false;
    const LiveAircraft* air = nullptr;  // valid inside the aircraft callback, where the weapon callback runs
    size_t n_air = 0;
};

void copy_name(char* dst, const wchar_t* src) {
    size_t k = 0;
    for (; k < acmi::kRecNameChars && src[k]; ++k) {
        const wchar_t ch = src[k];
        dst[k] = (ch >= 0x20 && ch < 0x7f) ? static_cast<char>(ch) : '?';
    }
    dst[k] = '\0';
}

// Under both tables' shared locks (the weapons' nested in the aircraft's): no allocation, no I/O, no logging.
// Writes the whole frame: the aircraft first (a weapon's parent must be known to the core before the weapon), then
// the weapons.
void weapon_cb(const LiveWeapon* wl, size_t nw, void* vctx) {
    auto& c = *static_cast<SnapCtx*>(vctx);
    const LiveAircraft* list = c.air;
    const size_t n = c.n_air;
    if (!g_writer->begin_frame(n + nw)) return;
    size_t used = 0;
    for (size_t i = 0; i < n; ++i) {
        acmi::TacviewRec& r = g_writer->slot(used);
        r.obj_class = acmi::TacviewRec::kAircraft;
        r.parent_seq = 0;
        if (!read_pose(list[i].obj, kOffPos, kOffAngles, r.pos, r.ang)) {
            // Keep the object in the frame with a non-finite pose: the core counts it as seen (no remove and re-add)
            // and skips the transform.
            r.kind = acmi::TacviewRec::kObject;
            r.seq = list[i].seq;
            r.t = c.t;
            r.pos[0] = r.pos[1] = r.pos[2] = std::nan("");
            ++used;
            continue;
        }
        r.kind = acmi::TacviewRec::kObject;
        r.side = static_cast<uint8_t>(list[i].side);
        r.is_player = list[i].is_player ? 1 : 0;
        r.seq = list[i].seq;
        r.t = c.t;
        copy_name(r.name, list[i].name);
        ++used;
    }
    for (size_t i = 0; i < nw; ++i) {
        acmi::TacviewRec& r = g_writer->slot(used);
        r.obj_class = wl[i].weapon_class;
        r.kind = acmi::TacviewRec::kObject;
        r.is_player = 0;
        r.seq = wl[i].seq;
        r.parent_seq = wl[i].parent_seq;
        r.t = c.t;
        if (!read_pose(wl[i].obj, kOffWeaponPos, kOffWeaponAngles, r.pos, r.ang)) {
            r.pos[0] = r.pos[1] = r.pos[2] = std::nan("");  // seen, but no transform (see above)
            r.side = 0;
            r.name[0] = '\0';
            ++used;
            continue;
        }
        r.side = static_cast<uint8_t>(wl[i].side);
        size_t k = 0;
        for (; k < acmi::kRecNameChars && wl[i].name[k]; ++k) r.name[k] = wl[i].name[k];
        r.name[k] = '\0';
        ++used;
    }
    g_weapons_live = nw != 0;
    g_writer->commit_frame(used, c.t);
    c.wrote = true;
}

// Under the aircraft table's shared lock.
void snapshot_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& c = *static_cast<SnapCtx*>(vctx);
    if (n == 0) return;
    c.air = list;
    c.n_air = n;
    probe_with_weapons(weapon_cb, &c);
}

// One last frame with no objects, so the file gets a removal line for every object (the player's -1 included).
// An empty frame is right in both cases: the table is empty, or it holds a new flight's aircraft.
void stop_recording(const char* why, bool final_frame = false) {
    if (!g_recording) return;
    g_recording = false;
    if (final_frame && g_writer->accepting() && g_writer->begin_frame(0)) g_writer->commit_frame(0, sim_game_time());
    g_writer->close(false, why);  // the thread logs the summary
}

enum class StartResult { Started, Failed, Retry };

StartResult start_recording(uint32_t epoch) {
    if (!g_dir_tried) {
        g_dir_tried = true;
        const std::wstring base = config::game_data_dir();  // the shell: game thread, once
        if (!base.empty()) g_dir = base + L"\\Tacview";
    }
    if (g_dir.empty()) {
        LOG("tacview: not recording: the Saved Games folder was not found");
        return StartResult::Failed;
    }
    if (!g_writer) g_writer = new (std::nothrow) acmi::TacviewWriter;
    if (!g_writer || !g_writer->ready()) {
        LOG("tacview: not recording: out of memory for the frame ring");
        return StartResult::Failed;
    }
    if (!g_writer->idle()) return StartResult::Retry;  // the previous recording is still being written out
    SYSTEMTIME local, utc;
    GetLocalTime(&local);
    GetSystemTime(&utc);
    char iso[40];
    std::snprintf(iso, sizeof(iso), "%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
                  utc.wMinute, utc.wSecond);
    acmi::TacviewCoreConfig cfg;  // reference 0/0, aircraft 10 Hz, weapons 20 Hz
    if (!g_writer->open(g_dir, local, iso, cfg, writer_log, display_path)) {
        LOG("tacview: not recording: could not start the writer");
        return StartResult::Failed;
    }
    g_table_empty.store(false, std::memory_order_relaxed);
    g_recording = true;
    g_epoch = epoch;
    g_last_snapshot = -1e300;
    g_weapons_live = false;
    weapon_track_new_flight();
    return StartResult::Started;
}

void on_step(void* /*aircraft*/, float /*dt*/, bool is_player) {
    if (g_recording) {
        if (sim_player_epoch() != g_epoch) {
            stop_recording("player lost", true);
        } else if (g_table_empty.exchange(false, std::memory_order_acq_rel)) {
            stop_recording("table empty", true);
        } else if (!g_writer->accepting()) {
            g_skip_valid = true;  // the writer already logged why (write failure, size cap, no file)
            g_skip_epoch = g_epoch;
            stop_recording("writer stopped");
        }
    }
    if (!is_player) return;
    if (!g_recording) {
        if (config::tacview_recording_peek() != 1) return;
        const uint32_t ep = sim_player_epoch();
        if (g_skip_valid && g_skip_epoch == ep) return;
        const StartResult sr = start_recording(ep);
        if (sr == StartResult::Retry) return;  // not marked: tried again at the next player step
        if (sr == StartResult::Failed) {
            g_skip_valid = true;
            g_skip_epoch = ep;
            return;
        }
    }
    const double t = sim_game_time();
    const double interval = g_weapons_live ? kSnapshotIntervalWeapons : kSnapshotInterval;
    if (t >= g_last_snapshot && t - g_last_snapshot < interval - 1e-9) return;
    g_last_snapshot = t;
    SnapCtx c;
    c.t = t;
    probe_with_aircraft(snapshot_cb, &c);
}

}  // namespace

void tacview_recorder_on_modules_loaded() {
    if (g_registered.load(std::memory_order_relaxed) || g_registered.exchange(true)) return;
    if (!sim_register_step_callback(on_step)) LOG("tacview: no free step callback slot; the recorder is off");
}

void tacview_recorder_table_empty() { g_table_empty.store(true, std::memory_order_release); }

namespace {
void count_cb(const LiveAircraft*, size_t n, void* ctx) { *static_cast<size_t*>(ctx) = n; }
}  // namespace

void tacview_recorder_menu_moment() {
    if (!g_recording) return;
    if (sim_player_epoch() != g_epoch) {
        stop_recording("player lost", true);
        return;
    }
    size_t live = 1;
    probe_with_aircraft(count_cb, &live);
    if (live == 0 || g_table_empty.exchange(false, std::memory_order_acq_rel)) {
        stop_recording("table empty", true);
        return;
    }
    if (!g_writer->accepting()) stop_recording("writer stopped");
}

void tacview_recorder_shutdown() {
    if (!g_writer) return;
    g_recording = false;
    g_writer->close(true, "process exit");
}

}  // namespace lazysf2::hooks
