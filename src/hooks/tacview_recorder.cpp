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
// process exit. A flight that ends with the table emptying and no further aircraft step is noticed at the next
// step of any aircraft, at the next Options exit, or at process exit.
//
// Game thread cost: at most 10 times per second of game time, on a player step, one pass over the live aircraft
// under probe_with_aircraft's shared lock: a pose read (SEH-guarded) and a name copy per aircraft into the ring. No
// allocation, no I/O, no logging. Everything else (formatting, the rate limit, removals, the file) is the writer
// thread's (acmi/tacview_writer.h, acmi/tacview_core.h). The shell call that finds Saved Games runs once, on the
// game thread, at the first start; it is never made from the module watch.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include "acmi/tacview_core.h"
#include "acmi/tacview_writer.h"
#include "config.h"
#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {

namespace {

constexpr size_t kOffPos = 0x38, kOffAngles = 0x100;  // aircraft object: double[3] position, float[3] angles
constexpr double kSnapshotInterval = 0.1;             // game seconds: at most 10 Hz per aircraft

acmi::TacviewWriter* g_writer = nullptr;  // allocated at the first recording (game thread)
std::wstring g_dir;                       // ...\Tacview, resolved once
bool g_dir_tried = false;
bool g_recording = false;
uint32_t g_epoch = 0;        // sim_player_epoch() at the start
bool g_skip_valid = false;   // a start failed or the writer failed during this player's flight: do not retry it
uint32_t g_skip_epoch = 0;
double g_last_snapshot = 0;
std::atomic<bool> g_registered{false};

void writer_log(const char* line) { LOG("%s", line); }

std::wstring display_path(const std::wstring& p) { return log::log_path(p); }

// Reads the pose of a live aircraft; false instead of a crash if the memory went away. No C++ objects here (SEH).
bool read_pose(const void* obj, double pos[3], float ang[3]) {
    __try {
        const char* b = static_cast<const char*>(obj);
        std::memcpy(pos, b + kOffPos, 3 * sizeof(double));
        std::memcpy(ang, b + kOffAngles, 3 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct SnapCtx {
    double t = 0;
    bool wrote = false;
};

// Under the aircraft table's shared lock: no allocation, no I/O, no logging.
void snapshot_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& c = *static_cast<SnapCtx*>(vctx);
    if (n == 0 || !g_writer->begin_frame(n)) return;
    size_t used = 0;
    for (size_t i = 0; i < n; ++i) {
        acmi::TacviewRec& r = g_writer->slot(used);
        if (!read_pose(list[i].obj, r.pos, r.ang)) continue;
        r.kind = acmi::TacviewRec::kObject;
        r.side = static_cast<uint8_t>(list[i].side);
        r.is_player = list[i].is_player ? 1 : 0;
        r.seq = list[i].seq;
        r.t = c.t;
        size_t k = 0;
        for (; k < acmi::kRecNameChars && list[i].name[k]; ++k) {
            const wchar_t ch = list[i].name[k];
            r.name[k] = (ch >= 0x20 && ch < 0x7f) ? static_cast<char>(ch) : '?';
        }
        r.name[k] = '\0';
        ++used;
    }
    g_writer->commit_frame(used, c.t);
    c.wrote = true;
}

void stop_recording(const char* why) {
    if (!g_recording) return;
    g_recording = false;
    g_writer->close(false, why);  // the thread logs the summary
}

bool start_recording(uint32_t epoch) {
    if (!g_dir_tried) {
        g_dir_tried = true;
        const std::wstring base = config::game_data_dir();  // the shell: game thread, once
        if (!base.empty()) g_dir = base + L"\\Tacview";
    }
    if (g_dir.empty()) {
        LOG("tacview: not recording: the Saved Games folder was not found");
        return false;
    }
    if (!g_writer) g_writer = new (std::nothrow) acmi::TacviewWriter;
    if (!g_writer || !g_writer->ready()) {
        LOG("tacview: not recording: out of memory for the frame ring");
        return false;
    }
    SYSTEMTIME local, utc;
    GetLocalTime(&local);
    GetSystemTime(&utc);
    char iso[40];
    std::snprintf(iso, sizeof(iso), "%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
                  utc.wMinute, utc.wSecond);
    acmi::TacviewCoreConfig cfg;  // reference 0/0, 10 Hz
    if (!g_writer->open(g_dir, local, iso, cfg, writer_log, display_path)) {
        LOG("tacview: not recording: could not start the writer");
        return false;
    }
    g_recording = true;
    g_epoch = epoch;
    g_last_snapshot = -1e300;
    return true;
}

void on_step(void* /*aircraft*/, float /*dt*/, bool is_player) {
    if (g_recording) {
        if (sim_player_epoch() != g_epoch) {
            stop_recording("player lost");
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
        if (!start_recording(ep)) {
            g_skip_valid = true;
            g_skip_epoch = ep;
            return;
        }
    }
    const double t = sim_game_time();
    if (t >= g_last_snapshot && t - g_last_snapshot < kSnapshotInterval - 1e-9) return;
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

void tacview_recorder_menu_moment() {
    if (g_recording && sim_player_epoch() != g_epoch) stop_recording("player lost");
    if (g_recording && !g_writer->accepting()) stop_recording("writer stopped");
}

void tacview_recorder_shutdown() {
    if (!g_writer) return;
    g_recording = false;
    g_writer->close(true, "process exit");
}

}  // namespace lazysf2::hooks
