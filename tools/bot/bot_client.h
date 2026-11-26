#pragma once
// The synthetic client: connects to a host, syncs the clock, flies a script and sends States, and receives the host's
// jet, the relayed States of the other clients and the peer messages. Used by lazysf2-bot.exe and by the loopback tests.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "bot_core.h"

namespace LazyWorks::SF2Patch::bot {

struct BotConfig {
    std::string host = "127.0.0.1:7777";  // numeric address or name, and port
    std::string callsign = "BOT1";
    std::string aircraft = "F-4E";
    Script script = Script::Straight;
    double rate = 30.0;      // States per second
    double duration = 30.0;  // wall seconds of flying
    double loss_pct = 0.0;   // percent of States dropped before sending
    double jitter_ms = 0.0;  // each State is delayed by a random 0..jitter_ms before sending
    /// <summary>
    /// Every packet sent is held this long before it goes to ENet, and every packet received is held this long
    /// before it is handled: a fixed delay each way (the Pings' round trip grows by twice this).
    /// </summary>
    double lag_ms = 0.0;
    double offset[3] = {150.0, 50.0, 0.0};  // formation: metres right, back and up in the host's heading frame ...
    bool offset_set = false;  // ... if set; otherwise the Welcome's slot: 150 m right per client id, 50 m back
    bool log_remote = false;                // one line per second per remote id
    bool drop = false;                      // exit without Bye
    uint32_t rng_seed = 12345;
    std::function<void(const char*)> log;     // may be empty
    const std::atomic<bool>* cancel = nullptr;  // set to end the run early (with a Bye)
};

struct BotResult {
    int code = 0;  // 0 ok, 1 bad arguments, 2 cannot connect (bad address, refused), 3 no connection within 4 s or no Welcome within 15 s of connecting, 4 the host closed the connection (with or without a Bye) or went silent for 4 s
    uint16_t client_id = 0;
    uint32_t generated = 0;  // States produced (seq numbers used)
    uint32_t sent = 0;       // States handed to ENet
    uint32_t lost = 0;       // States dropped by --loss
    uint32_t pings = 0, pongs = 0;
    uint32_t held_ticks = 0;  // send slots skipped because the host's clock was frozen or its game time had not advanced
    int clock_resets = 0;
    double offset = 0, best_rtt = 0;  // at the end
    double final_game_time = 0;
    // What the bot received (after --lag).
    uint32_t host_states = 0;     // States of the host's jet (id 0) accepted
    uint32_t relayed_states = 0;  // States of other clients accepted
    uint32_t peer_infos = 0, peer_lefts = 0;
    uint32_t dropped_rx = 0;  // packets refused: malformed, wrong channel or id, out of range, or not announced
    int host_bye = -1;        // the reason of the host's Bye, -1 if none came
    double host_age_p90 = 0;  // seconds: host time at handling minus the stamp, of the host's States
    // --script formation: each sent State against where the host really was at its time (from the host's States
    // received later) plus the offset.
    // The percentiles come from a uniform sample of at most 65536 values. States sent across a gap in the host's
    // States, or while it was silent, cannot be scored: they are counted in formation_unscored.
    uint32_t formation_n = 0, formation_unscored = 0;
    double formation_p50 = 0, formation_p95 = 0, formation_max = 0;
    double formation_offset[3] = {};  // the offset flown
};

BotResult run_bot(const BotConfig& cfg);

}  // namespace LazyWorks::SF2Patch::bot
