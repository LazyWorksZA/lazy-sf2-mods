#pragma once

#include "config.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>What a focus loss does: keep the simulation running, or the stock pause (and why).</summary>
enum class BgDecision {
    KeepRunning,
    NoSession,   // single-player: stock pause
    OptionOff,   // LAZYSF2_BG_SIM=0
    Fullscreen,  // Phase 4 supports Borderless only
    Iconic,      // the window is minimised: nothing to present to
};

constexpr BgDecision bg_decision(bool session, bool option, config::DisplayMode mode, bool iconic) {
    if (!session) return BgDecision::NoSession;
    if (!option) return BgDecision::OptionOff;
    if (mode != config::DisplayMode::Borderless) return BgDecision::Fullscreen;
    if (iconic) return BgDecision::Iconic;
    return BgDecision::KeepRunning;
}

/// <summary>True when a focus loss must keep the simulation running instead of the stock pause.</summary>
constexpr bool keep_running(bool session, bool option, config::DisplayMode mode, bool iconic) {
    return bg_decision(session, option, mode, iconic) == BgDecision::KeepRunning;
}

}  // namespace LazyWorks::SF2Patch::hooks
