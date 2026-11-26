#pragma once
// The bot as a synthetic host (lazysf2-bot --serve): the real HostCore and HostPeers, with a game-thread loop that
// flies a scripted jet on wall time as its host time, so the game can be tested as a client on one machine.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
    std::vector<uint8_t> mission;  // non-empty: a packed mission (net/world_proto.h) served to clients that ask
    uint32_t mission_id = 0, mission_raw = 0;
    int seat_members = 4, host_member = 0;  // the seat plan for --mission (--seats N,HOST)
    /// <summary>A synthetic world stream (T6) for a welcomed client: aircraft that fly straight at 150 m/s from their
    /// manifest position and heading, sent at 10 Hz as WorldStates, and scripted events.</summary>
    struct WorldObj {
        uint8_t kind = 1;
        uint16_t a = 0, b = 0;
        double pos[3] = {};
        float heading = 0;
    };
    struct WorldEv {
        double at = 0;  // seconds after the client's Welcome
        uint8_t event = 0;
        uint8_t kind = 1;
        uint16_t a = 0, b = 0;
    };
    std::vector<WorldObj> world;
    std::vector<WorldEv> world_events;
    /// <summary>The jet's controls (throttle, engines) sent as the host's AvatarState (peer 0) to a welcomed client,
    /// on change, from the client's Welcome on.</summary>
    JetScript jet = JetScript::None;
    /// <summary>--hit: a scripted gun hit (HitApply, 20 kJ kinetic) on the key every `every` seconds of host time
    /// from the client's Welcome on, as if the bot's jet (shooter key: its seat in the same slot) had hit it.</summary>
    bool hit = false;
    uint8_t hit_kind = 1;
    uint16_t hit_a = 0, hit_b = 0;
    double hit_every = 0;
    /// <summary>--launch: a scripted weapon release (WeaponLaunch from the bot's jet, its seat in the --hit slot or
    /// slot 0) every `every` seconds: the weapon's name, guided or not, and a target key (kind 0: none).</summary>
    bool launch = false;
    std::string launch_weapon;
    bool launch_guided = false;
    uint8_t launch_kind = 0;
    uint16_t launch_a = 0, launch_b = 0, launch_shooter_a = 0;
    double launch_every = 0;
    /// <summary>--stores: the jet's loadout (one station of `stores_weapon`, by name hash, count 2, every 10 s) and
    /// consumables (fuel from 4,000 kg falling 2 kg/s, a gun's 640 rounds falling 10/s, 30 chaff, 30 flares; every 2 s).</summary>
    bool stores = false;
    std::string stores_weapon = "AIM-9J";
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
    uint32_t missions_sent = 0;  // missions sent in full (T5)
    uint32_t world_msgs = 0, world_events = 0;  // T6 messages queued
    uint32_t jet_states_sent = 0;  // the jet's AvatarStates queued (--jet)
    uint32_t jet_states_rx = 0;    // the client's AvatarStates received
    uint8_t last_rx_throttle = 0, last_rx_engines = 0, last_rx_guns = 0;
    uint32_t hit_claims_rx = 0;  // the client's HitClaims received
    uint32_t hits_sent = 0;      // --hit HitApplies queued
    uint32_t launches_sent = 0;  // --launch WeaponLaunches queued
    uint32_t launches_rx = 0;    // the client's WeaponLaunches received
    std::string last_rx_weapon;
    uint32_t loadouts_sent = 0, consumables_sent = 0;
    uint32_t loadouts_rx = 0, consumables_rx = 0;  // the client's
    uint16_t last_rx_fuel = 0;
};

/// <summary>
/// Serves until cfg.duration or cfg.cancel. Without a start position the first Welcome carries no join position
/// (net::welcome_has_join is false); once the client's first State is in, the jet starts 2 km ahead of it, and a
/// second Welcome puts the client on the jet's wing.
/// </summary>
ServeResult run_serve(const ServeConfig& cfg);

}  // namespace LazyWorks::SF2Patch::bot
