#include "bot_client.h"

#include <enet/enet.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

namespace LazyWorks::SF2Patch::bot {

using namespace LazyWorks::SF2Patch::net;

namespace {

double wall_now() {
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}

/// Seconds without anything from the host before the bot gives up: no ENet connection (exit code 3), or silence after the Welcome (4).
constexpr double kSilenceLimit = 4.0;
/// Seconds a connected host may take to send the Welcome (its game thread may be busy loading) before exit code 3.
constexpr double kWelcomeWait = 15.0;

struct Runner {
    const BotConfig& cfg;
    BotResult res;
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    bool connected = false, disconnected = false, welcomed = false;
    WelcomeMsg welcome{};
    double last_rx = 0;  // wall time of the last packet from the host
    ClockSync clock;
    Flight flight;
    std::mt19937 rng;
    std::multimap<double, std::vector<uint8_t>> delayed;  // release wall time -> encoded State

    explicit Runner(const BotConfig& c) : cfg(c), rng(c.rng_seed) {}
    ~Runner() {
        if (host) enet_host_destroy(host);
    }

    template <typename... A>
    void say(const char* fmt, A... args) const {
        if (!cfg.log) return;
        char line[256];
        std::snprintf(line, sizeof(line), fmt, args...);
        cfg.log(line);
    }
    bool cancelled() const { return cfg.cancel && cfg.cancel->load(std::memory_order_relaxed); }

    bool send(const uint8_t* d, size_t n, uint8_t channel) {
        ENetPacket* p = enet_packet_create(d, n, channel == kChannelReliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (!p) return false;
        if (enet_peer_send(peer, channel, p) != 0) {
            enet_packet_destroy(p);
            return false;
        }
        return true;
    }

    void handle(const ENetEvent& ev) {
        if (ev.type == ENET_EVENT_TYPE_CONNECT) {
            connected = true;
        } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            disconnected = true;
        } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            const double rx = wall_now();
            last_rx = rx;
            MsgType type{};
            if (peek_type(ev.packet->data, ev.packet->dataLength, type) == Status::Ok) {
                if (type == MsgType::Welcome && ev.channelID == kChannelReliable && !welcomed) {
                    WelcomeMsg w{};
                    if (decode(ev.packet->data, ev.packet->dataLength, w) == Status::Ok) {
                        welcome = w;
                        welcomed = true;
                        res.client_id = w.client_id;
                        // The connect RTT: ENet's smoothed value for the peer (ms), as half a trip.
                        clock.on_welcome(rx, w.host_game_time, 0.0005 * static_cast<double>(peer->roundTripTime));
                    }
                } else if (type == MsgType::Pong && ev.channelID == kChannelUnreliable) {
                    PongMsg p{};
                    if (decode(ev.packet->data, ev.packet->dataLength, p) == Status::Ok &&
                        clock.on_pong(p.client_send_time, rx, p.host_game_time))
                        ++res.pongs;
                }
            }
            enet_packet_destroy(ev.packet);
        }
    }
    void service(int ms) {
        ENetEvent ev;
        int r = enet_host_service(host, &ev, static_cast<enet_uint32>(ms));
        for (int guard = 0; r > 0 && guard < 256; ++guard) {
            handle(ev);
            r = enet_host_service(host, &ev, 0);
        }
    }
};

}  // namespace

BotResult run_bot(const BotConfig& cfg) {
    Runner r(cfg);
    BotResult& res = r.res;
    if (!std::isfinite(cfg.rate) || !std::isfinite(cfg.duration) || !std::isfinite(cfg.loss_pct) ||
        !std::isfinite(cfg.jitter_ms) || !(cfg.rate > 0.0 && cfg.rate <= 240.0) || !(cfg.duration >= 0.0) ||
        cfg.loss_pct < 0 || cfg.loss_pct > 100 || cfg.jitter_ms < 0) {
        r.say("bot: bad arguments (rate 0..240 Hz, duration >= 0, loss 0..100 %%, jitter >= 0)");
        res.code = 1;
        return res;
    }
    // host "address:port"
    const size_t colon = cfg.host.rfind(':');
    char* port_end = nullptr;
    const long port =
        colon == std::string::npos ? 0 : std::strtol(cfg.host.c_str() + colon + 1, &port_end, 10);
    if (colon == std::string::npos || colon == 0 || port < 1 || port > 65535 || port_end == cfg.host.c_str() + colon + 1 ||
        *port_end != '\0') {
        r.say("bot: --host must be address:port");
        res.code = 1;
        return res;
    }
    if (enet_initialize() != 0) {
        r.say("bot: enet_initialize failed");
        res.code = 2;
        return res;
    }
    struct Deinit {
        ~Deinit() { enet_deinitialize(); }
    } deinit;

    // The ENet host must be destroyed before enet_deinitialize, and Runner (declared first) outlives Deinit.
    struct HostGuard {
        Runner& r;
        ~HostGuard() {
            if (r.host) {
                enet_host_destroy(r.host);
                r.host = nullptr;
            }
        }
    } guard{r};

    ENetAddress addr{};
    const std::string name = cfg.host.substr(0, colon);
    if (enet_address_set_host(&addr, name.c_str()) != 0) {
        r.say("bot: cannot resolve %s", name.c_str());
        res.code = 2;
        return res;
    }
    addr.port = static_cast<enet_uint16>(port);
    r.host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    if (r.host) r.peer = enet_host_connect(r.host, &addr, kChannelCount, 0);
    if (!r.peer) {
        r.say("bot: cannot create the ENet client");
        res.code = 2;
        return res;
    }

    // Connect.
    for (double t0 = wall_now(); !r.connected && !r.disconnected && wall_now() - t0 < kSilenceLimit && !r.cancelled();)
        r.service(10);
    if (!r.connected) {
        if (r.disconnected || r.cancelled()) {
            r.say("bot: cannot connect to %s", cfg.host.c_str());
            res.code = 2;
        } else {
            r.say("bot: no answer from %s to the connect request after %.0f s", cfg.host.c_str(), kSilenceLimit);
            res.code = 3;
        }
        return res;
    }
    r.last_rx = wall_now();
    HelloMsg hello{};
    set_str(hello.callsign, cfg.callsign.c_str());
    set_str(hello.aircraft, cfg.aircraft.c_str());
    uint8_t buf[128];
    r.send(buf, encode(hello, buf, sizeof(buf)), kChannelReliable);
    enet_host_flush(r.host);

    // Welcome.
    for (double t0 = wall_now(); !r.welcomed && !r.disconnected && wall_now() - t0 < kWelcomeWait && !r.cancelled();)
        r.service(10);
    if (!r.welcomed) {
        r.say("bot: no Welcome from the host (%s)", r.disconnected ? "it closed the connection" : "timed out");
        res.code = r.disconnected ? 4 : 3;
        enet_peer_disconnect_now(r.peer, 0);
        enet_host_flush(r.host);
        return res;
    }
    r.say("bot: welcome: client %u, spawn %.0f %.0f %.0f heading %.3f, host time %.3f", static_cast<unsigned>(res.client_id),
          r.welcome.spawn_pos[0], r.welcome.spawn_pos[1], r.welcome.spawn_pos[2],
          static_cast<double>(r.welcome.spawn_heading), r.welcome.host_game_time);

    // Clock sync, then flight.
    const double period = 1.0 / cfg.rate;
    double next_ping = wall_now(), next_send = 0, next_log = 0, fly_start = 0, hold_since = 0;
    bool flying = false, was_logged_hold = false;
    uint32_t seq = 0, last_logged_sent = 0;
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    auto send_state = [&](const std::vector<uint8_t>& bytes) {
        if (r.send(bytes.data(), bytes.size(), kChannelUnreliable)) ++res.sent;
    };

    for (;;) {
        r.service(0);  // count what already arrived before judging silence
        const double wall = wall_now();
        if (r.disconnected) {
            r.say("bot: the host closed the connection");
            res.code = 4;
            break;
        }
        if (flying && (wall - fly_start >= cfg.duration || r.cancelled())) break;
        if (!flying && r.cancelled()) break;

        if (wall >= next_ping) {
            PingMsg p{};
            p.client_send_time = wall;
            if (r.send(buf, encode(p, buf, sizeof(buf)), kChannelUnreliable)) ++res.pings;
            next_ping = wall + 0.25;
        }

        if (wall - r.last_rx > kSilenceLimit) {
            r.say("bot: the host went silent: nothing received for %.0f s", kSilenceLimit);
            res.code = 4;
            break;
        }
        // The host answers Pings but its clock held for more than 4 s on end.
        if (flying) {
            if (!r.clock.on_hold(wall)) {
                hold_since = 0;
            } else if (hold_since == 0) {
                hold_since = wall;
            } else if (wall - hold_since > 4.0) {
                r.say("bot: host not answering: held for more than 4 s");
                res.code = 4;
                break;
            }
        }

        if (!flying && r.clock.have_pong()) {
            const double t0 = r.clock.now(wall);
            r.flight.start(cfg.script, r.welcome.spawn_pos, static_cast<double>(r.welcome.spawn_heading), t0);
            flying = true;
            fly_start = wall;
            next_send = wall;
            next_log = wall + 1.0;
            r.say("bot: flying '%s' at %.1f Hz from host time %.3f (clock offset %.4f s, best rtt %.1f ms)",
                  cfg.script == Script::Straight ? "straight"
                  : cfg.script == Script::Orbit  ? "orbit"
                  : cfg.script == Script::Breaks ? "breaks"
                                                 : "climb",
                  cfg.rate, t0, r.clock.offset(), r.clock.best_rtt() * 1000.0);
        }

        if (flying && wall >= next_send) {
            next_send += period;
            if (next_send < wall - 3.0 * period) next_send = wall + period;  // fell far behind: do not burst
            const double target = r.clock.now(wall);
            const bool held = r.clock.on_hold(wall) || target <= r.flight.time() + 1e-4;
            if (held) {
                ++res.held_ticks;
                if (!was_logged_hold && r.clock.on_hold(wall)) {
                    r.say("bot: holding (the host's game time is not advancing)");
                    was_logged_hold = true;
                }
            } else {
                if (was_logged_hold) r.say("bot: resuming, host time %.3f", target);
                was_logged_hold = false;
                r.flight.advance_to(target);
                const Flight::Sample s = r.flight.sample();
                StateMsg m{};
                m.client_id = res.client_id;
                m.seq = ++seq;
                m.game_time = r.flight.time();
                for (int i = 0; i < 3; ++i) {
                    m.pos[i] = s.pos[i];
                    m.vel[i] = s.vel[i];
                    m.ang[i] = s.ang[i];
                    m.rates[i] = s.rates[i];
                }
                ++res.generated;
                res.final_game_time = m.game_time;
                std::vector<uint8_t> bytes(sizeof(StateMsg));
                bytes.resize(encode(m, bytes.data(), bytes.size()));
                if (cfg.loss_pct > 0.0 && uni(r.rng) * 100.0 < cfg.loss_pct) {
                    ++res.lost;
                } else if (cfg.jitter_ms > 0.0) {
                    r.delayed.emplace(wall + uni(r.rng) * cfg.jitter_ms * 0.001, std::move(bytes));
                } else {
                    send_state(bytes);
                }
            }
        }
        while (!r.delayed.empty() && r.delayed.begin()->first <= wall) {
            send_state(r.delayed.begin()->second);
            r.delayed.erase(r.delayed.begin());
        }

        if (flying && wall >= next_log) {
            next_log += 1.0;
            r.say("bot: t=%.0f sent %u (%u/s) lost %u rtt %u ms (best pong %.1f ms) offset %.4f s%s",
                  wall - fly_start, res.sent, res.sent - last_logged_sent, res.lost,
                  static_cast<unsigned>(r.peer->roundTripTime), r.clock.best_rtt() * 1000.0, r.clock.offset(),
                  r.clock.frozen() ? " HOLD" : "");
            last_logged_sent = res.sent;
        }
        enet_host_flush(r.host);
        r.service(1);
    }

    res.offset = r.clock.offset();
    res.best_rtt = r.clock.best_rtt();
    res.clock_resets = r.clock.resets();

    if (cfg.drop) {
        r.say("bot: --drop: leaving without Bye");
    } else if (!r.disconnected) {
        // Send every State still held back by --jitter first, so the Bye does not overtake them.
        for (auto& d : r.delayed) send_state(d.second);
        r.delayed.clear();
        ByeMsg b{};
        b.client_id = res.client_id;
        b.reason = 0;
        r.send(buf, encode(b, buf, sizeof(buf)), kChannelReliable);
        enet_host_flush(r.host);
        // ENet drops a peer's undelivered packets when its disconnect arrives, so wait until the Bye is acknowledged
        // (and the host had a moment to hand it on) before disconnecting.
        for (double t0 = wall_now(); !r.disconnected && r.peer->reliableDataInTransit > 0 && wall_now() - t0 < 1.0;)
            r.service(5);
        for (double t0 = wall_now(); !r.disconnected && wall_now() - t0 < 0.05;) r.service(5);
        enet_peer_disconnect(r.peer, 0);
        enet_host_flush(r.host);
        for (double t0 = wall_now(); !r.disconnected && wall_now() - t0 < 0.5;) r.service(10);
    }
    r.say("bot: done: %u States made, %u sent, %u lost to --loss, %u slots held, %u Pings, %u Pongs, %d clock resets, "
          "offset %.4f s, best rtt %.1f ms",
          res.generated, res.sent, res.lost, res.held_ticks, res.pings, res.pongs, res.clock_resets, res.offset,
          res.best_rtt * 1000.0);
    return res;
}

}  // namespace LazyWorks::SF2Patch::bot
