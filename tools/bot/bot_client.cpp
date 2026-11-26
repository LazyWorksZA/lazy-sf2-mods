#include "bot_client.h"

#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <vector>

#include "net/host_core.h"
#include "net/receiver_math.h"
#include "net/remote_jet.h"

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
/// After the host's Bye, seconds before the bot leaves by itself if the host has not disconnected.
constexpr double kHostByeGrace = 1.0;
constexpr int kSources = kMaxClients + 1;  // State sources: the host (0) and clients 1..kMaxClients
constexpr size_t kMaxQueued = 4096;        // packets --lag may hold in each direction
constexpr size_t kHostHistory = 64;        // the host's States kept for the formation error (3 s at 20 Hz)

/// <summary>Nearest-rank percentile (q in [0,1]); 0 for no values.</summary>
double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(std::ceil(q * static_cast<double>(v.size())));
    i = i > 0 ? i - 1 : 0;
    return v[i < v.size() ? i : v.size() - 1];
}

double dist3(const double a[3], const double b[3]) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

replay_math::Samp to_samp(const StateMsg& s) {
    replay_math::Samp o{};
    o.t = s.game_time;
    for (int i = 0; i < 3; ++i) {
        o.k.p[i] = s.pos[i];
        o.k.v[i] = s.vel[i];
        o.k.a[i] = s.ang[i];
        o.k.w[i] = s.rates[i];
    }
    return o;
}

bool delivered(const ENetPeer* p) {
    return p->reliableDataInTransit == 0 && enet_list_empty(&p->sentReliableCommands) &&
           enet_list_empty(&p->outgoingSendReliableCommands) && enet_list_empty(&p->outgoingCommands);
}

/// <summary>A packet held by --lag.</summary>
struct Pkt {
    double at = 0;  // wall time it is released
    uint8_t channel = 0;
    bool state = false;  // outgoing State: counted in BotResult::sent once it is handed to ENet
    uint16_t len = 0;
    uint8_t data[kMaxMessage] = {};
};

/// <summary>A peer the host announced (PeerInfo) and the receiver for its States.</summary>
struct Remote {
    Remote() = default;  // not an aggregate: RemoteJet's constructor is explicit
    bool known = false;
    bool first = true;  // no State logged yet
    char callsign[16] = {};
    char aircraft[32] = {};
    RemoteJet jet;
    uint32_t rx = 0, rx_logged = 0;
};

struct Runner {
    const BotConfig& cfg;
    BotResult res;
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    bool connected = false, disconnected = false, welcomed = false;
    bool host_bye = false;
    double host_bye_at = 0;
    WelcomeMsg welcome{};
    double last_rx = 0;  // wall time of the last packet from the host (as it arrived, before --lag)
    double lag = 0;      // seconds
    ClockSync clock;
    int resets_seen = 0;
    Flight flight;
    std::mt19937 rng;
    std::multimap<double, std::vector<uint8_t>> delayed;  // --jitter: release wall time -> encoded State
    std::deque<Pkt> out_q, in_q;                          // --lag, each in release order
    Remote remotes[kSources];
    std::vector<double> host_ages;
    std::deque<replay_math::Samp> host_hist;  // the host's accepted States, oldest first

    // --script formation
    struct Sent {
        double t;
        double p[3];
    };
    bool form_on = false, form_coast_logged = false;
    Sent form_last{};
    double form_last_v[3] = {}, form_last_a[3] = {};
    std::deque<Sent> form_pending;  // sent, waiting for the host's States around their time
    std::vector<double> form_err;
    size_t form_err_logged = 0;

    explicit Runner(const BotConfig& c) : cfg(c), lag(c.lag_ms * 0.001), rng(c.rng_seed) {}
    ~Runner() {
        if (host) enet_host_destroy(host);
    }

    template <typename... A>
    void say(const char* fmt, A... args) const {
        if (!cfg.log) return;
        char line[320];
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
    /// <summary>Sends now, or after --lag.</summary>
    bool post(const uint8_t* d, size_t n, uint8_t channel, bool state) {
        if (n == 0 || n > kMaxMessage) return false;
        if (lag > 0.0) {
            if (out_q.size() >= kMaxQueued) return false;
            Pkt p;
            p.at = wall_now() + lag;
            p.channel = channel;
            p.state = state;
            p.len = static_cast<uint16_t>(n);
            std::memcpy(p.data, d, n);
            out_q.push_back(p);
            return true;
        }
        const bool ok = send(d, n, channel);
        if (ok && state) ++res.sent;
        return ok;
    }
    /// <summary>Releases what --lag holds and is due: sends, and handles received packets at their release time.</summary>
    void pump(double wall) {
        while (!out_q.empty() && out_q.front().at <= wall) {
            const Pkt& p = out_q.front();
            if (send(p.data, p.len, p.channel) && p.state) ++res.sent;
            out_q.pop_front();
        }
        while (!in_q.empty() && in_q.front().at <= wall) {
            const Pkt p = in_q.front();
            in_q.pop_front();
            process(p.channel, p.data, p.len, p.at);
        }
    }

    void refuse() { ++res.dropped_rx; }

    /// <summary>A received packet, checked as ClientCore checks it. rx is the wall time it is handled.</summary>
    void process(uint8_t channel, const uint8_t* data, size_t len, double rx) {
        MsgType type{};
        if (peek_type(data, len, type) != Status::Ok) return refuse();
        switch (type) {
            case MsgType::Welcome: {
                WelcomeMsg w{};
                if (channel != kChannelReliable || welcomed || decode(data, len, w) != Status::Ok) return refuse();
                welcome = w;
                welcomed = true;
                res.client_id = w.client_id;
                // The Welcome took half of ENet's smoothed round trip (ms) to arrive, plus --lag.
                clock.on_welcome(rx, w.host_game_time, 0.0005 * static_cast<double>(peer->roundTripTime) + lag);
                resets_seen = clock.resets();
                return;
            }
            case MsgType::Pong: {
                PongMsg p{};
                if (channel != kChannelUnreliable || decode(data, len, p) != Status::Ok) return refuse();
                if (clock.on_pong(p.client_send_time, rx, p.host_game_time)) ++res.pongs;
                return;
            }
            case MsgType::PeerInfo: {
                PeerInfoMsg m{};
                if (channel != kChannelReliable || decode(data, len, m) != Status::Ok || m.id > kMaxClients || !welcomed ||
                    m.id == res.client_id)
                    return refuse();
                Remote& r = remotes[m.id];
                if (r.known) return refuse();
                r = Remote{};
                r.known = true;
                set_str(r.callsign, m.callsign);
                set_str(r.aircraft, m.aircraft);
                ++res.peer_infos;
                // An empty or "unknown" type is unknown, not a mismatch.
                say("bot: peer %u \"%s\" (%s) announced", static_cast<unsigned>(m.id), r.callsign,
                    r.aircraft[0] ? r.aircraft : "type unknown");
                return;
            }
            case MsgType::PeerLeft: {
                PeerLeftMsg m{};
                if (channel != kChannelReliable || decode(data, len, m) != Status::Ok || m.id == 0 || m.id > kMaxClients ||
                    !welcomed || m.id == res.client_id || !remotes[m.id].known)
                    return refuse();
                Remote& r = remotes[m.id];
                say("bot: peer %u \"%s\" left (reason %u) after %u States", static_cast<unsigned>(m.id), r.callsign,
                    static_cast<unsigned>(m.reason), r.rx);
                r = Remote{};
                ++res.peer_lefts;
                return;
            }
            case MsgType::State: {
                StateMsg m{};
                if (channel != kChannelUnreliable || decode(data, len, m) != Status::Ok || m.client_id > kMaxClients ||
                    !welcomed || host_bye || m.client_id == res.client_id)
                    return refuse();
                Remote& r = remotes[m.client_id];
                // Only from a peer the host has announced and not reported gone (a late State must not bring it back).
                if (!r.known) return refuse();
                const double now = clock.now(rx);
                if (!state_in_range(m, now)) return refuse();
                if (!r.jet.push(m, now)) return;  // old or duplicate: counted in the jet's stats
                ++r.rx;
                if (m.client_id == 0) {
                    ++res.host_states;
                    host_ages.push_back(now - m.game_time);
                    if (!host_hist.empty() && !(m.game_time > host_hist.back().t)) host_hist.clear();  // a restart
                    host_hist.push_back(to_samp(m));
                    if (host_hist.size() > kHostHistory) host_hist.pop_front();
                } else {
                    ++res.relayed_states;
                }
                if (r.first) {
                    r.first = false;
                    say("bot: first State from %s %u \"%s\": seq %u, age %.0f ms",
                        m.client_id == 0 ? "the host" : "client", static_cast<unsigned>(m.client_id), r.callsign,
                        static_cast<unsigned>(m.seq), (now - m.game_time) * 1000.0);
                }
                return;
            }
            case MsgType::Bye: {
                ByeMsg b{};
                // A Bye carries its sender's id: from the host always 0.
                if (channel != kChannelReliable || decode(data, len, b) != Status::Ok || b.client_id != 0 || host_bye)
                    return refuse();
                host_bye = true;
                host_bye_at = rx;
                res.host_bye = b.reason;
                say("bot: the host said Bye (reason %u)", static_cast<unsigned>(b.reason));
                return;
            }
            default:  // Hello and Ping only travel from a client to the host
                return refuse();
        }
    }

    void handle(const ENetEvent& ev) {
        if (ev.type == ENET_EVENT_TYPE_CONNECT) {
            connected = true;
            // ENet's throttle drops unreliable packets when the RTT rises above its mean, which a lagged or jittery
            // link does all the time (as ClientCore does). This also configures the host's side.
            enet_peer_throttle_configure(peer, 5000, 0, 0);
        } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            disconnected = true;
            // Hand on what --lag still holds (a Bye before the disconnect, say), so it is not lost with the connection.
            while (!in_q.empty()) {
                const Pkt p = in_q.front();
                in_q.pop_front();
                process(p.channel, p.data, p.len, wall_now());
            }
        } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            const double rx = wall_now();
            last_rx = rx;
            const size_t len = ev.packet->dataLength;
            if (len > kMaxMessage) {
                refuse();
            } else if (lag > 0.0) {
                if (in_q.size() < kMaxQueued) {
                    Pkt p;
                    p.at = rx + lag;
                    p.channel = ev.channelID;
                    p.len = static_cast<uint16_t>(len);
                    std::memcpy(p.data, ev.packet->data, len);
                    in_q.push_back(p);
                } else {
                    refuse();
                }
            } else {
                process(ev.channelID, ev.packet->data, len, rx);
            }
            enet_packet_destroy(ev.packet);
        }
    }
    void service(int ms) {
        pump(wall_now());
        ENetEvent ev;
        int r = enet_host_service(host, &ev, static_cast<enet_uint32>(ms));
        for (int guard = 0; r > 0 && guard < 256; ++guard) {
            handle(ev);
            r = enet_host_service(host, &ev, 0);
        }
        pump(wall_now());
        if (welcomed && clock.resets() != resets_seen) {  // the clock stepped: the receivers start again
            resets_seen = clock.resets();
            for (Remote& rm : remotes) rm.jet.reset();
        }
    }

    /// <summary>--script formation: the State for host time t, on the host's predicted jet plus the offset.</summary>
    void fly_formation(double t, StateMsg* m) {
        RemoteJet::Kin k{};
        RemoteJet::Out o = RemoteJet::Out::None;
        if (remotes[0].known) o = remotes[0].jet.shown(t, &k);
        double p[3], v[3], a[3], w[3] = {};
        if (o != RemoteJet::Out::None && o != RemoteJet::Out::Coast) {
            if (!form_on) {
                form_on = true;
                say("bot: formation: on the host's jet, %.0f m right, %.0f m back, %.0f m up", cfg.offset[0], cfg.offset[1],
                    cfg.offset[2]);
            }
            form_coast_logged = false;
            formation_point(k.p, k.a[1], cfg.offset, p);
            formation_velocity(k.v, k.a[1], k.w[1], cfg.offset, v);
            for (int i = 0; i < 3; ++i) {
                a[i] = k.a[i];
                w[i] = k.w[i];
            }
        } else if (!form_on) {  // before the first host State: straight on from the Welcome's join position
            flight.advance_to(t);
            const Flight::Sample s = flight.sample();
            for (int i = 0; i < 3; ++i) {
                p[i] = s.pos[i];
                v[i] = s.vel[i];
                a[i] = s.ang[i];
                w[i] = s.rates[i];
            }
        } else {  // the host's States stopped (or the clock stepped): on along the last velocity, attitude held
            if (o == RemoteJet::Out::Coast && !form_coast_logged) {
                form_coast_logged = true;
                say("bot: formation: no State from the host for over %.0f s; flying straight on",
                    remotes[0].jet.params().coast_after);
            }
            for (int i = 0; i < 3; ++i) {
                p[i] = form_last.p[i] + form_last_v[i] * (t - form_last.t);
                v[i] = form_last_v[i];
                a[i] = form_last_a[i];
            }
        }
        for (int i = 0; i < 3; ++i) {
            m->pos[i] = p[i];
            m->vel[i] = static_cast<float>(v[i]);
            m->ang[i] = static_cast<float>(a[i]);
            m->rates[i] = static_cast<float>(w[i]);
            form_last.p[i] = p[i];
            form_last_v[i] = v[i];
            form_last_a[i] = a[i];
        }
        form_last.t = t;
        if (form_on) {
            form_pending.push_back(form_last);
            if (form_pending.size() > 1024) form_pending.pop_front();
        }
    }

    /// <summary>Scores each sent formation State once the host's States around its time have arrived.</summary>
    void resolve_formation() {
        while (!form_pending.empty() && host_hist.size() >= 2) {
            const Sent& s = form_pending.front();
            if (s.t > host_hist.back().t) break;
            if (s.t >= host_hist.front().t) {
                size_t i = 0;
                while (i + 2 < host_hist.size() && host_hist[i + 1].t < s.t) ++i;
                const replay_math::Samp& a = host_hist[i];
                const replay_math::Samp& b = host_hist[i + 1];
                if (b.t > a.t && b.t - a.t <= 0.25 && s.t <= b.t) {  // not across a gap of lost States
                    replay_math::Kin k{};
                    replay_math::interpolate(a, b, s.t, &k);
                    double want[3];
                    formation_point(k.p, k.a[1], cfg.offset, want);
                    form_err.push_back(dist3(s.p, want));
                }
            }
            form_pending.pop_front();
        }
    }

    void log_remotes() {
        for (int id = 0; id < kSources; ++id) {
            Remote& r = remotes[id];
            if (!r.known) continue;
            const JetStats st = r.jet.take_stats();
            char extra[96] = "";
            if (id == 0 && cfg.script == Script::Formation && form_err.size() > form_err_logged) {
                std::vector<double> last(form_err.begin() + static_cast<std::ptrdiff_t>(form_err_logged), form_err.end());
                std::snprintf(extra, sizeof(extra), " formation err p50 %.1f p95 %.1f max %.1f m",
                              percentile(last, 0.5), percentile(last, 0.95), percentile(last, 1.0));
            }
            say("bot: remote %d \"%s\" rx %u (%.1f Hz) age %.0f/%.0f ms horizon %.0f ms err %.2f m snaps %u coast %u "
                "accel %.1f g%s",
                id, r.callsign, r.rx - r.rx_logged, st.rate_hz, st.age_p50 * 1000.0, st.age_p90 * 1000.0,
                st.horizon_max * 1000.0, st.offset_max, st.snaps + st.resync_snaps, st.coasting_steps, st.accel_g_max, extra);
            r.rx_logged = r.rx;
        }
        form_err_logged = form_err.size();
    }
};

}  // namespace

BotResult run_bot(const BotConfig& cfg) {
    Runner r(cfg);
    BotResult& res = r.res;
    bool offset_ok = true;
    for (double o : cfg.offset) offset_ok = offset_ok && std::isfinite(o) && std::fabs(o) <= 10000.0;
    if (!std::isfinite(cfg.rate) || !std::isfinite(cfg.duration) || !std::isfinite(cfg.loss_pct) ||
        !std::isfinite(cfg.jitter_ms) || !std::isfinite(cfg.lag_ms) || !(cfg.rate > 0.0 && cfg.rate <= 240.0) ||
        !(cfg.duration >= 0.0) || cfg.loss_pct < 0 || cfg.loss_pct > 100 || cfg.jitter_ms < 0 || cfg.lag_ms < 0 ||
        cfg.lag_ms > 2000 || !offset_ok) {
        r.say("bot: bad arguments (rate 0..240 Hz, duration >= 0, loss 0..100 %%, jitter >= 0, lag 0..2000 ms, offset "
              "within 10 km)");
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
    if (cfg.lag_ms > 0) r.say("bot: --lag: every packet is held %.0f ms each way", cfg.lag_ms);
    HelloMsg hello{};
    set_str(hello.callsign, cfg.callsign.c_str());
    set_str(hello.aircraft, cfg.aircraft.c_str());
    uint8_t buf[128];
    r.post(buf, encode(hello, buf, sizeof(buf)), kChannelReliable, false);
    r.pump(wall_now());
    enet_host_flush(r.host);

    // Welcome. With --lag the Hello and the Welcome each wait their turn in the queues.
    for (double t0 = wall_now(); !r.welcomed && !r.disconnected && wall_now() - t0 < kWelcomeWait && !r.cancelled();)
        r.service(1);
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
    const bool formation = cfg.script == Script::Formation;
    const double period = 1.0 / cfg.rate;
    double next_ping = wall_now(), next_send = 0, next_log = 0, fly_start = 0, hold_since = 0, last_t = 0;
    bool flying = false, was_logged_hold = false;
    uint32_t seq = 0, last_logged_sent = 0, last_logged_host = 0, last_logged_relayed = 0;
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    auto send_state = [&](const std::vector<uint8_t>& bytes) { r.post(bytes.data(), bytes.size(), kChannelUnreliable, true); };

    for (;;) {
        r.service(0);  // count what already arrived before judging silence
        const double wall = wall_now();
        if (r.disconnected) {
            r.say("bot: the host closed the connection");
            res.code = 4;
            break;
        }
        if (r.host_bye && wall - r.host_bye_at > kHostByeGrace) {
            r.say("bot: the host said Bye but kept the connection; leaving");
            res.code = 4;
            break;
        }
        if (flying && (wall - fly_start >= cfg.duration || r.cancelled())) break;
        if (!flying && r.cancelled()) break;

        if (wall >= next_ping && !r.host_bye) {
            PingMsg p{};
            p.client_send_time = wall;
            if (r.post(buf, encode(p, buf, sizeof(buf)), kChannelUnreliable, false)) ++res.pings;
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
            r.flight.start(formation ? Script::Straight : cfg.script, r.welcome.spawn_pos,
                           static_cast<double>(r.welcome.spawn_heading), t0);
            last_t = t0;
            flying = true;
            fly_start = wall;
            next_send = wall;
            next_log = wall + 1.0;
            r.say("bot: flying '%s' at %.1f Hz from host time %.3f (clock offset %.4f s, best rtt %.1f ms)",
                  script_name(cfg.script), cfg.rate, t0, r.clock.offset(), r.clock.best_rtt() * 1000.0);
        }

        if (flying && wall >= next_send && !r.host_bye) {
            next_send += period;
            if (next_send < wall - 3.0 * period) next_send = wall + period;  // fell far behind: do not burst
            const double target = r.clock.now(wall);
            const bool held = r.clock.on_hold(wall) || target <= last_t + 1e-4;
            if (held) {
                ++res.held_ticks;
                if (!was_logged_hold && r.clock.on_hold(wall)) {
                    r.say("bot: holding (the host's game time is not advancing)");
                    was_logged_hold = true;
                }
            } else {
                if (was_logged_hold) r.say("bot: resuming, host time %.3f", target);
                was_logged_hold = false;
                StateMsg m{};
                m.client_id = res.client_id;
                m.seq = ++seq;
                if (formation) {
                    r.fly_formation(target, &m);
                    m.game_time = target;
                } else {
                    r.flight.advance_to(target);
                    const Flight::Sample s = r.flight.sample();
                    m.game_time = r.flight.time();
                    for (int i = 0; i < 3; ++i) {
                        m.pos[i] = s.pos[i];
                        m.vel[i] = s.vel[i];
                        m.ang[i] = s.ang[i];
                        m.rates[i] = s.rates[i];
                    }
                }
                last_t = m.game_time;
                // The other receivers step with the send tick (their stats need a shown path).
                for (int id = 0; id < kSources; ++id) {
                    if (!r.remotes[id].known || (formation && id == 0)) continue;
                    RemoteJet::Kin k{};
                    r.remotes[id].jet.shown(target, &k);
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
        if (formation) r.resolve_formation();

        if (flying && wall >= next_log) {
            next_log += 1.0;
            r.say("bot: t=%.0f sent %u (%u/s) lost %u rtt %u ms (best pong %.1f ms) offset %.4f s rx host %u relayed %u%s",
                  wall - fly_start, res.sent, res.sent - last_logged_sent, res.lost,
                  static_cast<unsigned>(r.peer->roundTripTime), r.clock.best_rtt() * 1000.0, r.clock.offset(),
                  res.host_states - last_logged_host, res.relayed_states - last_logged_relayed,
                  r.clock.frozen() ? " HOLD" : "");
            last_logged_sent = res.sent;
            last_logged_host = res.host_states;
            last_logged_relayed = res.relayed_states;
            if (cfg.log_remote) r.log_remotes();
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
        if (!r.host_bye) {
            // Send every State still held back by --jitter first, so the Bye does not overtake them.
            for (auto& d : r.delayed) send_state(d.second);
            r.delayed.clear();
            ByeMsg b{};
            b.client_id = res.client_id;
            b.reason = 0;
            r.post(buf, encode(b, buf, sizeof(buf)), kChannelReliable, false);
        }
        // --lag still holds the last packets (the Bye among them): let them go first.
        for (const double t0 = wall_now(); !r.disconnected && !r.out_q.empty() && wall_now() - t0 < r.lag + 1.0;)
            r.service(1);
        enet_host_flush(r.host);
        // ENet drops a peer's undelivered packets when its disconnect arrives, so wait until the Bye is acknowledged
        // (and the host had a moment to hand it on) before disconnecting.
        for (const double t0 = wall_now(); !r.disconnected && !delivered(r.peer) && wall_now() - t0 < 1.0;) r.service(5);
        for (const double t0 = wall_now(); !r.disconnected && wall_now() - t0 < 0.05;) r.service(5);
        enet_peer_disconnect(r.peer, 0);
        enet_host_flush(r.host);
        for (const double t0 = wall_now(); !r.disconnected && wall_now() - t0 < 0.5;) r.service(10);
    }
    if (formation) r.resolve_formation();
    res.host_age_p90 = percentile(r.host_ages, 0.9);
    res.formation_n = static_cast<uint32_t>(r.form_err.size());
    res.formation_p50 = percentile(r.form_err, 0.5);
    res.formation_p95 = percentile(r.form_err, 0.95);
    res.formation_max = percentile(r.form_err, 1.0);
    r.say("bot: done: %u States made, %u sent, %u lost to --loss, %u slots held, %u Pings, %u Pongs, %d clock resets, "
          "offset %.4f s, best rtt %.1f ms",
          res.generated, res.sent, res.lost, res.held_ticks, res.pings, res.pongs, res.clock_resets, res.offset,
          res.best_rtt * 1000.0);
    r.say("bot: received: %u host States (age p90 %.0f ms), %u relayed, %u PeerInfo, %u PeerLeft, %u refused%s",
          res.host_states, res.host_age_p90 * 1000.0, res.relayed_states, res.peer_infos, res.peer_lefts, res.dropped_rx,
          res.host_bye >= 0 ? ", host Bye" : "");
    if (formation)
        r.say("bot: formation error against the host's own States: %u samples, p50 %.1f m, p95 %.1f m, max %.1f m",
              res.formation_n, res.formation_p50, res.formation_p95, res.formation_max);
    return res;
}

}  // namespace LazyWorks::SF2Patch::bot
