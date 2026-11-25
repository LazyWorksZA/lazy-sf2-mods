// Recorder (developer tool, part of the probe): Ctrl+Shift+R records the player's aircraft each player step for up
// to 180 s. Game thread only; the hotkey thread just sets a request. File format: see hooks.h (RecSample).
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

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
uint32_t g_start_epoch = 0;  // sim_player_epoch() at the start
double g_start_time = 0;

template <typename T>
void read_at(const void* obj, size_t off, T* out, size_t n) {
    std::memcpy(out, static_cast<const char*>(obj) + off, n * sizeof(T));
}

void write_recording() {
    const wchar_t* dir = probe_output_dir();
    if (!dir) return;
    for (unsigned n = 1; n < 1000; ++n) {
        wchar_t name[16];
        swprintf_s(name, L"rec_%03u.bin", n);  // at most 11 characters: cannot truncate
        const std::wstring path = std::wstring(dir) + L"\\" + name;
        if (path.size() >= MAX_PATH) {  // CreateFileW without the \?\ prefix is limited to MAX_PATH
            LOG("recorder: output path too long (%zu characters, limit %d)", path.size(), static_cast<int>(MAX_PATH) - 1);
            return;
        }
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            LOG("recorder: could not create %ls (error %lu)", path.c_str(), GetLastError());
            return;
        }
        const uint32_t header[3] = {kMagic, kVersion, g_count};
        const DWORD body = static_cast<DWORD>(g_count * sizeof(RecSample));
        DWORD w1 = 0, w2 = 0;
        bool ok = false;
        if (!WriteFile(f, header, sizeof(header), &w1, nullptr)) {
            LOG("recorder: writing the header to %ls failed (error %lu)", path.c_str(), GetLastError());
        } else if (w1 != sizeof(header)) {
            LOG("recorder: short write of the header to %ls: %lu of %zu bytes", path.c_str(), w1, sizeof(header));
        } else if (!WriteFile(f, g_buf, body, &w2, nullptr)) {
            LOG("recorder: writing the samples to %ls failed (error %lu)", path.c_str(), GetLastError());
        } else if (w2 != body) {
            LOG("recorder: short write of the samples to %ls: %lu of %lu bytes", path.c_str(), w2, body);
        } else {
            ok = true;
        }
        CloseHandle(f);
        if (ok) {
            LOG("recorder: wrote %ls (%u samples)", path.c_str(), g_count);
        } else {
            DeleteFileW(path.c_str());  // never leave a partial recording behind
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
    g_start_epoch = sim_player_epoch();
    g_recording = true;
    sim_set_run_active(true);
    LOG("recorder: started (max %.0f s, %u samples)", kMaxSeconds, kCapacity);
}

void on_step(void* aircraft, float /*dt*/, bool is_player) {
    // The destroyed player never steps again, so any aircraft's step is where we notice.
    if (g_recording && sim_player_epoch() != g_start_epoch) {
        stop_recording("player lost");
        // A hotkey press that arrived in the same moment was most likely meant to stop it: it is spent, and must not
        // be answered later with "not started, player unknown".
        if (g_toggle_request.exchange(false, std::memory_order_relaxed))
            LOG("recorder: hotkey ignored, the recording had just ended (player lost)");
        return;
    }
    if (!is_player) {
        if (!sim_player_known() && g_toggle_request.exchange(false, std::memory_order_relaxed)) {
            LOG("recorder: not started, player unknown");
        }
        return;
    }
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

void recorder_on_modules_loaded() {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    if (sim_register_step_callback(on_step)) {
        LOG("recorder: on (Ctrl+Shift+R records the player's aircraft, max %.0f s)", kMaxSeconds);
    } else {
        LOG("recorder: no free step callback slot");
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
