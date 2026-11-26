#pragma once
// The bot as a synthetic host (lazysf2-bot --serve): the real HostCore and HostPeers, with a game-thread loop that
// flies a scripted jet on wall time as its host time, so the game can be tested as a client on one machine.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "bot_core.h"

namespace LazyWorks::SF2Patch::bot {

struct ServeConfig {
    uint16_t port = 7777;  // 0: any free port (see port_out)
    std::string bind = "127.0.0.1";  // numeric IPv4
    std::string callsign = "BOTHOST";
    std::string aircraft = "F-4E";
    Script script = Script::Orbit;  // not Formation
    /// <summary>
    /// Received events are handled, and the jet's States sent, this long late. The Pongs come from HostCore's network
    /// thread and are not delayed, so the client's measured round trip does not include it.
    /// </summary>
    double lag_ms = 0.0;
    bool has_start = false;  // false: start 2 km ahead of the client's first State
    double start[3] = {};
    double start_heading = 0.0;  // radians, from +y towards +x
    std::wstring record_dir;     // non-empty: record botserve_NNN.lzst there
    double pause_at = 0.0, pause_for = 0.0;  // host time freezes pause_for seconds, pause_at seconds after the jet starts
    double duration = 0.0;                   // wall seconds to serve; 0 until cancelled
    uint32_t theatre_hash = 0;               // 0 unknown: never refuses a client's theatre
    std::function<void(const char*)> log;    // may be empty; also called from HostCore's and the recorder's threads
    const std::atomic<bool>* cancel = nullptr;
    std::atomic<uint16_t>* port_out = nullptr;  // set to the bound port once listening
};

struct ServeResult {
    int code = 0;  // 0 ok, 1 bad arguments, 2 cannot listen
    uint32_t clients = 0;        // clients welcomed
    uint32_t welcomes = 0;       // Welcomes sent (a second one carries the join position when --start is omitted)
    uint32_t refused = 0;        // a second client (Bye 1), or another theatre (Bye 5)
    uint32_t jet_states = 0;     // States of its own jet (id 0) handed to the core
    uint32_t client_states = 0;  // client States handled
    uint32_t paused_steps = 0;
    bool join_confirmed = false;  // the client's States showed it at the join point after the join Welcome
    bool join_gave_up = false;    // ... or not within 10 s of re-sending it
    bool jet_started = false;
    double jet_start[3] = {};
    double jet_start_heading = 0, jet_start_time = 0;  // host time
    double host_time_end = 0;
};

/// <summary>
/// Serves until cfg.duration or cfg.cancel. Without a start position the first Welcome carries no join position
/// (net::welcome_has_join is false); once the client's first State is in, the jet starts 2 km ahead of it, and a
/// second Welcome puts the client on the jet's wing.
/// </summary>
ServeResult run_serve(const ServeConfig& cfg);

}  // namespace LazyWorks::SF2Patch::bot
