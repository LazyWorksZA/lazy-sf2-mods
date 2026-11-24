#pragma once
// The synthetic client: connects to a host, syncs the clock, flies a script and sends States. Used by
// lazysf2-bot.exe and by the loopback tests.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "bot_core.h"

namespace lazysf2::bot {

struct BotConfig {
    std::string host = "127.0.0.1:7777";  // numeric address or name, and port
    std::string callsign = "BOT1";
    std::string aircraft = "F-4E";
    Script script = Script::Straight;
    double rate = 30.0;      // States per second
    double duration = 30.0;  // wall seconds of flying
    double loss_pct = 0.0;   // percent of States dropped before sending
    double jitter_ms = 0.0;  // each State is delayed by a random 0..jitter_ms before sending
    bool drop = false;       // exit without Bye
    uint32_t rng_seed = 12345;
    std::function<void(const char*)> log;     // may be empty
    const std::atomic<bool>* cancel = nullptr;  // set to end the run early (with a Bye)
};

struct BotResult {
    int code = 0;  // 0 ok, 1 bad arguments, 2 cannot connect, 3 no Welcome, 4 the host closed the connection or is not answering
    uint16_t client_id = 0;
    uint32_t generated = 0;  // States produced (seq numbers used)
    uint32_t sent = 0;       // States handed to ENet
    uint32_t lost = 0;       // States dropped by --loss
    uint32_t pings = 0, pongs = 0;
    uint32_t held_ticks = 0;  // send slots skipped because the host's clock was frozen or its game time had not advanced
    int clock_resets = 0;
    double offset = 0, best_rtt = 0;  // at the end
    double final_game_time = 0;
};

BotResult run_bot(const BotConfig& cfg);

}  // namespace lazysf2::bot
