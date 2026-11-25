// Step order (developer tool, part of the probe): once per player, after 5 s of game time, watches 600 frames and
// logs whether the player's Update runs before or after the other aircraft's in each frame, and the
// sim_step_count() delta seen at each AI step. A frame starts after a gap of more than 2 ms between two aircraft
// steps (the render and present run there). Game thread only. See lazy-sf2-recon: network-recon §8 (R4).
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {

namespace {

constexpr double kStartAfter = 5.0;  // game seconds after the player is first seen
constexpr unsigned kFrames = 600;
constexpr double kGapMs = 2.0;
constexpr int kMaxBefore = 4;  // aircraft pointers listed that stepped before the player

// Game thread only.
bool g_done = false;
uint32_t g_epoch = UINT32_MAX;
double g_epoch_t0 = 0;
bool g_measuring = false;
LARGE_INTEGER g_freq{}, g_last{};
uint64_t g_frame_steps0 = 0;  // sim_step_count() at the start of the frame
int g_frame_calls = 0;        // aircraft steps in this frame so far
int g_player_index = -1;      // index of the player's first step in this frame, -1 = not yet
int g_player_steps = 0;       // player steps in this frame

unsigned g_frames = 0, g_player_first = 0, g_player_missing = 0;
int g_player_steps_max = 0;
uint64_t g_ai_after = 0, g_ai_before = 0;  // AI steps with a step-count delta of >= 1 and of 0
double g_gap_min = 1e9, g_inframe_gap_max = 0;
void* g_before[kMaxBefore] = {};
int g_before_n = 0;

void close_frame() {
    if (g_frame_calls == 0) return;
    ++g_frames;
    if (g_player_index == 0) ++g_player_first;
    if (g_player_index < 0) ++g_player_missing;
    if (g_player_steps > g_player_steps_max) g_player_steps_max = g_player_steps;
}

void report() {
    LOG("step order: %u frames: player first in %u, missing in %u; AI steps after the player %llu, before it %llu "
        "(sim_step_count delta 0); player steps per frame max %d; frame gap min %.2f ms, largest gap inside a frame "
        "%.2f ms",
        g_frames, g_player_first, g_player_missing, static_cast<unsigned long long>(g_ai_after),
        static_cast<unsigned long long>(g_ai_before), g_player_steps_max, g_gap_min, g_inframe_gap_max);
    if (g_before_n > 0) {
        LOG("step order: aircraft that stepped before the player: %p %p %p %p", g_before[0], g_before[1], g_before[2],
            g_before[3]);
    }
}

void on_step(void* aircraft, float, bool is_player) {
    if (g_done && sim_player_epoch() == g_epoch) return;
    if (!sim_player_known()) return;
    if (sim_player_epoch() != g_epoch) {  // a new player: measure once more
        g_epoch = sim_player_epoch();
        g_epoch_t0 = sim_game_time();
        g_done = false;
        g_measuring = false;
        return;
    }
    if (!g_measuring) {
        if (sim_game_time() - g_epoch_t0 < kStartAfter) return;
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_last);
        g_measuring = true;
        g_frames = g_player_first = g_player_missing = 0;
        g_player_steps_max = 0;
        g_ai_after = g_ai_before = 0;
        g_gap_min = 1e9;
        g_inframe_gap_max = 0;
        g_before_n = 0;
        for (void*& p : g_before) p = nullptr;
        g_frame_calls = -1;  // the first frame is partial: not counted
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double gap = static_cast<double>(now.QuadPart - g_last.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
    g_last = now;
    if (gap > kGapMs) {
        if (g_frame_calls > 0) close_frame();
        if (g_frame_calls >= 0 && gap < g_gap_min) g_gap_min = gap;
        if (g_frames >= kFrames) {
            report();
            g_done = true;
            g_measuring = false;
            return;
        }
        g_frame_calls = 0;
        g_player_index = -1;
        g_player_steps = 0;
        g_frame_steps0 = sim_step_count() - (is_player ? 1 : 0);  // the clock has already counted this step
    } else if (g_frame_calls < 0) {
        return;
    } else if (gap > g_inframe_gap_max) {
        g_inframe_gap_max = gap;
    }
    if (is_player) {
        if (g_player_index < 0) g_player_index = g_frame_calls;
        ++g_player_steps;
    } else if (sim_step_count() - g_frame_steps0 >= 1) {
        ++g_ai_after;
    } else {
        ++g_ai_before;
        bool known = false;
        for (int i = 0; i < g_before_n; ++i) known = known || g_before[i] == aircraft;
        if (!known && g_before_n < kMaxBefore) g_before[g_before_n++] = aircraft;
    }
    ++g_frame_calls;
}

}  // namespace

void step_order_on_modules_loaded() {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    if (sim_register_step_callback(on_step)) {
        LOG("step order: on (logs the aircraft step order once per player, %.0f s into the flight)", kStartAfter);
    } else {
        LOG("step order: no free step callback slot");
    }
}

}  // namespace lazysf2::hooks
