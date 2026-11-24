// Recorder (developer tool, part of the object probe; see probe_hooks.cpp).
// Inert unless the probe is on: recorder_on_modules_loaded() is only called from probe_on_modules_loaded(),
// and the hotkey thread only exists with the probe on.
//
// Ctrl+Shift+R starts and stops a recording of the player's aircraft: on every player step (the "after
// Update" callback from sim_hooks.cpp) one RecSample is appended to a preallocated buffer. A recording
// ends on the hotkey, after 180 s of game time, or when the buffer is full (180 x 120 samples).
// The recorder only reads the aircraft; it never writes to it.
//
// Threads: the hotkey thread only sets g_toggle_request. Everything else runs on the game thread, inside
// the step callback, so there are no locks here.
//
// File format of rec_NNN.bin (little endian):
//   offset 0   char[4]   "LZRC"
//   offset 4   uint32    version (1)
//   offset 8   uint32    count
//   offset 12  RecSample[count], each 68 bytes, packed (see hooks.h):
//       double t; double pos[3]; float vel[3]; float ang[3]; float rate[3]
// t is game time in seconds (sim_game_time() when the step ended), pos is world metres (x east, y north,
// z up), vel is world m/s, ang is pitch/heading/roll in radians, rate is the angular rates the game keeps.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {

namespace {

constexpr double kMaxSeconds = 180.0;
constexpr uint32_t kCapacity = 180 * 120;
constexpr uint32_t kMagic = 0x43525a4c;  // "LZRC" as little-endian bytes
constexpr uint32_t kVersion = 1;

// Offsets in the aircraft object (AircraftObject.dll build checked by the probe before any hook runs).
constexpr size_t kOffPos = 0x38, kOffVel = 0x50, kOffRates = 0xd0, kOffAngles = 0x100;

std::atomic<bool> g_toggle_request{false};

// Game thread only.
RecSample* g_buf = nullptr;  // allocated once, on the first start
uint32_t g_count = 0;        // samples in g_buf
bool g_recording = false;
double g_start_time = 0;

template <typename T>
void read_at(const void* obj, size_t off, T* out, size_t n) {
    std::memcpy(out, static_cast<const char*>(obj) + off, n * sizeof(T));
}

void write_recording() {
    const wchar_t* dir = probe_output_dir();
    if (!dir) return;
    for (unsigned n = 1; n < 1000; ++n) {
        wchar_t path[MAX_PATH];
        if (swprintf_s(path, L"%ls\\rec_%03u.bin", dir, n) < 0) {
            LOG("recorder: output path too long");
            return;
        }
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            LOG("recorder: could not create %ls (error %lu)", path, GetLastError());
            return;
        }
        const uint32_t header[3] = {kMagic, kVersion, g_count};
        DWORD w1 = 0, w2 = 0;
        const DWORD body = static_cast<DWORD>(g_count * sizeof(RecSample));
        const BOOL ok = WriteFile(f, header, sizeof(header), &w1, nullptr) &&
                        WriteFile(f, g_buf, body, &w2, nullptr) && w1 == sizeof(header) && w2 == body;
        const DWORD err = GetLastError();
        CloseHandle(f);
        if (ok) {
            LOG("recorder: wrote %ls (%u samples)", path, g_count);
        } else {
            LOG("recorder: write to %ls failed (error %lu)", path, err);
        }
        return;
    }
    LOG("recorder: no free rec_NNN.bin name in %ls", dir);
}

void stop_recording(const char* why) {
    g_recording = false;
    sim_set_run_active(false);
    const double secs = g_count ? g_buf[g_count - 1].t - g_buf[0].t : 0.0;
    LOG("recorder: %u samples, %.1f s game time (%s)", g_count, secs, why);
    write_recording();
}

void start_recording() {
    if (replay_active()) {
        LOG("recorder: not started, a replay is running");
        return;
    }
    if (!g_buf) {
        g_buf = new (std::nothrow) RecSample[kCapacity];
        if (!g_buf) {
            LOG("recorder: could not allocate the sample buffer");
            return;
        }
    }
    g_count = 0;  // the previous recording is discarded: only the latest is kept
    g_start_time = sim_game_time();
    g_recording = true;
    sim_set_run_active(true);
    LOG("recorder: started (max %.0f s, %u samples)", kMaxSeconds, kCapacity);
}

void on_step(void* aircraft, float /*dt*/, bool is_player) {
    if (!is_player) return;
    if (g_toggle_request.exchange(false, std::memory_order_relaxed)) {
        if (g_recording) {
            stop_recording("hotkey");
            return;
        }
        start_recording();
        if (!g_recording) return;
    }
    if (!g_recording) return;
    const double now = sim_game_time();
    if (now - g_start_time >= kMaxSeconds || g_count >= kCapacity) {
        stop_recording(g_count >= kCapacity ? "buffer full" : "180 s limit");
        return;
    }
    RecSample& s = g_buf[g_count++];
    s.t = now;
    read_at(aircraft, kOffPos, s.pos, 3);
    read_at(aircraft, kOffVel, s.vel, 3);
    read_at(aircraft, kOffAngles, s.ang, 3);
    read_at(aircraft, kOffRates, s.rate, 3);
}

}  // namespace

void recorder_toggle() { g_toggle_request.store(true, std::memory_order_relaxed); }

const RecSample* recorder_latest(uint32_t* count) {
    if (g_recording || !g_buf || g_count == 0) {
        *count = 0;
        return nullptr;
    }
    *count = g_count;
    return g_buf;
}

// Stub: replay.cpp (a later task) takes over this definition and the recorder then refuses to start during a replay.
bool replay_active() { return false; }

void recorder_on_modules_loaded() {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    if (sim_register_step_callback(on_step)) {
        LOG("recorder: on (Ctrl+Shift+R records the player's aircraft, max %.0f s)", kMaxSeconds);
    } else {
        LOG("recorder: no free step callback slot");
    }
}

}  // namespace lazysf2::hooks
