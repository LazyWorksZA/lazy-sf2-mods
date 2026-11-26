#pragma once
// Pure decisions for the focus feature (no game calls, unit tested). See focus_hooks.cpp.
#include <cstdint>

#include "config.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>What a focus loss does: keep the game running, or the stock pause (and why).</summary>
enum class BgDecision {
    KeepRunning,
    NoReason,    // BackgroundMode Pause and no other reason: stock pause
    Fullscreen,  // a reason exists, but only Borderless keeps running (owner parked Fullscreen)
};

/// <summary>
/// The decision on a focus loss. `player_option` is BackgroundMode != Pause; `other_reason` is any further reason to
/// keep running (none on master; the network session joins here, see focus_keep_running_reason()).
/// </summary>
constexpr BgDecision bg_decision(bool player_option, bool other_reason, config::DisplayMode mode) {
    if (!player_option && !other_reason) return BgDecision::NoReason;
    if (mode != config::DisplayMode::Borderless) return BgDecision::Fullscreen;
    return BgDecision::KeepRunning;
}

constexpr bool keep_running(bool player_option, bool other_reason, config::DisplayMode mode) {
    return bg_decision(player_option, other_reason, mode) == BgDecision::KeepRunning;
}

/// <summary>True when new secondary sound buffers get DSBCAPS_GLOBALFOCUS: mode 2, never the primary or the stream.</summary>
constexpr bool wants_global_focus(config::BackgroundMode mode, uint32_t flags) {
    constexpr uint32_t kPrimary = 0x1, kPositionNotify = 0x100;  // DSBCAPS_PRIMARYBUFFER, DSBCAPS_CTRLPOSITIONNOTIFY
    return mode == config::BackgroundMode::KeepRunningWithSound && (flags & (kPrimary | kPositionNotify)) == 0;
}

// Frame limiter while the kept-running window is minimised (Present no longer waits for vsync). Microseconds.
inline constexpr int64_t kPaceMinPeriodUs = 4167;     // 240 Hz
inline constexpr int64_t kPaceMaxPeriodUs = 33333;    // 30 Hz
inline constexpr int64_t kPaceDefaultPeriodUs = 16667;
inline constexpr int64_t kPaceMaxSampleUs = 100000;   // longer gaps (loading, menus switching) are not frames

/// <summary>Folds one measured frame interval into the period estimate (1/8 weight), clamped to 30..240 Hz.</summary>
constexpr int64_t pace_update_period(int64_t period_us, int64_t interval_us) {
    if (interval_us <= 0 || interval_us > kPaceMaxSampleUs) return period_us;
    int64_t p = period_us + (interval_us - period_us) / 8;
    if (p < kPaceMinPeriodUs) p = kPaceMinPeriodUs;
    if (p > kPaceMaxPeriodUs) p = kPaceMaxPeriodUs;
    return p;
}

/// <summary>One paced frame: the new deadline and how long to wait for it.</summary>
struct PaceStep {
    int64_t deadline_us;
    int64_t wait_us;
};

/// <summary>
/// The next frame is due one period after the last deadline. A frame that is late by less than a period does not
/// wait (it catches up); later than that, the schedule restarts from now. `last_deadline_us` 0 = first paced frame.
/// </summary>
constexpr PaceStep pace_step(int64_t now_us, int64_t last_deadline_us, int64_t period_us) {
    int64_t deadline = last_deadline_us == 0 ? now_us : last_deadline_us + period_us;
    if (deadline < now_us - period_us) deadline = now_us;
    return {deadline, deadline > now_us ? deadline - now_us : 0};
}

}  // namespace LazyWorks::SF2Patch::hooks
