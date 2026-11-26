#include "bot_serve.h"

#include "net/world_proto.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>

#include "net/host_core.h"
#include "net/host_session.h"
#include "net/receiver_math.h"
#include "net/remote_jet.h"
#include "net/stream_record.h"

namespace LazyWorks::SF2Patch::bot {

using namespace LazyWorks::SF2Patch::net;

namespace {

double wall_now() {
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}

constexpr double kServeStep = 1.0 / 60.0;          // the game-thread step, wall seconds
constexpr double kStartAhead = 2000.0;        // metres ahead of the client's first State
// The join Welcome goes out this long after the client's first State (which it sends only after receiving the first
// Welcome), and is re-sent this often until the client is on the wing: ClientCore drops one within kReWelcomeMinGap.
constexpr double kReWelcomeAfter = kReWelcomeMinGap + 0.5;
constexpr double kReWelcomeEvery = kReWelcomeMinGap + 0.25;
constexpr double kJoinNear = 300.0;     // metres from the join point: the client has moved there
constexpr double kJoinGiveUp = 10.0;    // seconds of re-sending before giving up
constexpr double kShownEvery = 0.1;           // RemoteShown records, host seconds (and every Snap)
constexpr double kHostTime0 = 100.0;          // host time at the start
constexpr uint8_t kByeBusy = 1, kByeSessionEnded = 3;  // as host.cpp
constexpr size_t kMaxQueued = 4096;           // events and States --lag may hold

// HostCore and StreamWriter take plain function pointers, called from their own threads.
std::atomic<const std::function<void(const char*)>*> g_log{nullptr};
void core_log(const char* line) {
    const std::function<void(const char*)>* f = g_log.load(std::memory_order_acquire);
    if (f && *f) (*f)(line);
}

double dist3(const double a[3], const double b[3]) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Client {
    Client() = default;  // not an aggregate: RemoteJet's constructor is explicit
    bool on = false;
    uint8_t slot = 0;
    uint32_t gen = 0;
    char callsign[16] = {};
    char aircraft[32] = {};
    uint32_t world_tag = 0;
    RemoteJet jet;
    bool got_state = false, coasting = false;
    // The two-step join (protocol.h, welcome_has_join): only when the first Welcome had no join position.
    bool join_pending = false;  // a join Welcome is still to be sent, or to be confirmed by the client's States
    double first_state_wall = -1, join_first_wall = -1, join_last_wall = -1;
    double last_pos[3] = {}, last_state_wall = 0;  // the newest accepted State's position, and when
    double last_game_time = 0, next_shown = 0;
    uint32_t rx = 0;
    double shown[3] = {}, shown_a[3] = {};  // the last position and angles written (Coast keeps them)
    bool have_shown = false;
};

struct Serve {
    const ServeConfig& cfg;
    ServeResult res;
    HostCore core;
    HostPeers peers{core};
    SendTicker tick;
    StreamWriter rec;
    Flight flight;
    Client c;
    double lag = 0, host_t = kHostTime0;
    uint32_t seq = 0, rec_seq = 0;
    struct Ev {
        double at;
        HostEvent e;
    };
    struct Out {
        double at;
        uint8_t len;
        uint8_t data[kMaxMessage];
    };
    std::deque<Ev> in_q;
    std::deque<Out> out_q;

    explicit Serve(const ServeConfig& s) : cfg(s), lag(s.lag_ms * 0.001) {}

    // The synthetic world stream (T6): from the client's Welcome on, every aircraft of cfg.world flies straight on at
    // 150 m/s at its altitude, sent at 10 Hz of host time; cfg.world_events fire once at their times.
    double world_t0 = -1, world_next = 0;
    uint32_t world_seq = 0, world_eseq = 0;
    size_t world_ev_done = 0;
    bool world_logged = false;
    void world_tick() {
        if (!c.on) {
            world_t0 = -1;
            world_ev_done = 0;
            return;
        }
        if (world_t0 < 0) {
            world_t0 = host_t;
            world_next = host_t;
        }
        const uint32_t ms = static_cast<uint32_t>(host_t * 1000.0);
        uint8_t buf[kWorldMaxMessage];
        while (world_ev_done < cfg.world_events.size() && host_t - world_t0 >= cfg.world_events[world_ev_done].at) {
            const auto& e = cfg.world_events[world_ev_done++];
            const WorldEventRec r{e.event, e.kind, e.a, e.b, 0};
            const size_t n = encode_world_events(++world_eseq, ms, &r, 1, buf, sizeof(buf));
            if (n && core.send_world(c.slot, c.gen, buf, n, true)) ++res.world_events;
            say("bot serve: world event %u for %u %u.%u at +%.1f s", static_cast<unsigned>(e.event),
                static_cast<unsigned>(e.kind), static_cast<unsigned>(e.a), static_cast<unsigned>(e.b), host_t - world_t0);
        }
        if (cfg.world.empty() || host_t < world_next) return;
        world_next = host_t + 0.1;
        const double dt = host_t - world_t0;
        std::vector<WorldObjState> st;
        for (const auto& o : cfg.world) {
            ObjPose p{};
            const double sh = std::sin(o.heading), ch = std::cos(o.heading);
            p.vel[0] = static_cast<float>(150.0 * sh);
            p.vel[1] = static_cast<float>(150.0 * ch);
            p.pos[0] = o.pos[0] + p.vel[0] * dt;
            p.pos[1] = o.pos[1] + p.vel[1] * dt;
            p.pos[2] = o.pos[2];
            p.ang[1] = o.heading;
            WorldObjState q{};
            q.kind = o.kind;
            q.a = o.a;
            q.b = o.b;
            quantise(p, &q);
            st.push_back(q);
        }
        for (size_t i = 0; i < st.size(); i += kWorldStatesMax) {
            const size_t k = st.size() - i < kWorldStatesMax ? st.size() - i : kWorldStatesMax;
            const size_t n = encode_world_states(++world_seq, ms, st.data() + i, k, buf, sizeof(buf));
            if (n && core.send_world(c.slot, c.gen, buf, n, false)) ++res.world_msgs;
        }
        if (!world_logged) {
            world_logged = true;
            say("bot serve: world stream on: %zu aircraft at 10 Hz (%zu bytes a second)", st.size(),
                ((st.size() + kWorldStatesMax - 1) / kWorldStatesMax * sizeof(WorldStatesHead) +
                 st.size() * sizeof(WorldObjState)) * 10);
        }
    }

    // --jet: the host's AvatarState (peer 0) from the client's Welcome on, sent on change as the mod's owner rule does.
    double jet_t0 = -1, jet_sent_at = 0;
    bool jet_have = false;
    AvatarStateMsg jet_last{};
    uint16_t jet_seq = 0;
    void jet_tick() {
        if (!c.on) {
            jet_t0 = -1;
            jet_have = false;
            return;
        }
        if (jet_t0 < 0) jet_t0 = host_t;
        float thr = 0;
        AvatarStateMsg m{};
        jet_script_at(cfg.jet, host_t - jet_t0, &thr, &m.engines, &m.guns);
        m.throttle = throttle_to_wire(thr);
        m.engine_count = 8;  // the script switches all of them
        if (jet_have && !avatar_state_due(jet_last, m, host_t - jet_sent_at)) return;
        m.peer = 0;
        m.seq = ++jet_seq;
        uint8_t buf[sizeof(AvatarStateMsg)];
        const size_t n = encode(m, buf, sizeof(buf));
        if (!n || !core.send_world(c.slot, c.gen, buf, n, true)) return;  // tried again next step
        ++res.jet_states_sent;
        if (!jet_have || m.engines != jet_last.engines || m.guns != jet_last.guns ||
            throttle_moved(jet_last.throttle, m.throttle))
            say("bot serve: jet controls: throttle %u/255, engines 0x%02x, guns %u at +%.1f s",
                static_cast<unsigned>(m.throttle), static_cast<unsigned>(m.engines), static_cast<unsigned>(m.guns),
                host_t - jet_t0);
        jet_last = m;
        jet_have = true;
        jet_sent_at = host_t;
    }

    // --hit: a scripted HitApply on the client's jet.
    double hit_t0 = -1, hit_next = 0;
    uint16_t hit_seq = 0;
    void hit_tick() {
        if (!c.on) {
            hit_t0 = -1;
            return;
        }
        if (hit_t0 < 0) {
            hit_t0 = host_t;
            hit_next = host_t + cfg.hit_every;
        }
        if (host_t < hit_next) return;
        hit_next += cfg.hit_every;
        HitMsg m{};
        m.seq = ++hit_seq;
        m.target_kind = cfg.hit_kind;
        m.target_a = cfg.hit_a;
        m.target_b = cfg.hit_b;
        m.shooter_kind = 1;
        m.shooter_a = cfg.hit_a;
        m.shooter_b = static_cast<uint16_t>(cfg.host_member);
        m.slot = 4;
        m.energy = 20000;
        const int16_t from[3] = {0, -300, 0}, to[3] = {0, 100, 0};  // 3 m behind to 1 m ahead of the jet's centre
        std::memcpy(m.p0, from, sizeof(from));
        std::memcpy(m.p1, to, sizeof(to));
        uint8_t buf[sizeof(HitMsg)];
        const size_t n = encode_hit(m, WorldType::HitApply, buf, sizeof(buf));
        if (n && core.send_world(c.slot, c.gen, buf, n, true)) ++res.hits_sent;
        say("bot serve: scripted gun hit %u on %c %u.%u (20 kJ kinetic)", static_cast<unsigned>(res.hits_sent),
            cfg.hit_kind == 1 ? 'A' : 'G', static_cast<unsigned>(cfg.hit_a), static_cast<unsigned>(cfg.hit_b));
    }

    // --stores: the jet's loadout and consumables.
    double stores_t0 = -1, stores_next_lo = 0, stores_next_co = 0;
    uint16_t stores_seq = 0;
    void stores_tick() {
        if (!c.on) {
            stores_t0 = -1;
            return;
        }
        if (stores_t0 < 0) stores_t0 = stores_next_lo = stores_next_co = host_t;
        const double t = host_t - stores_t0;
        if (host_t >= stores_next_lo) {
            stores_next_lo += 10.0;
            LoadoutMsg m{};
            m.seq = ++stores_seq;
            m.parts = 1;
            m.n = 1;
            m.e[0].station = 1;
            m.e[0].weapon = 0xffff;  // no id the client knows: it finds the weapon by its name's hash
            m.e[0].hash = weapon_name_hash(cfg.stores_weapon.c_str());
            m.e[0].count = 2;
            m.e[0].state = 1;
            uint8_t buf[sizeof(m)];
            const size_t n = encode(m, buf, sizeof(buf));
            if (n && core.send_world(c.slot, c.gen, buf, n, true)) ++res.loadouts_sent;
        }
        if (host_t >= stores_next_co) {
            stores_next_co += 2.0;
            ConsumablesMsg m{};
            m.seq = ++stores_seq;
            const double fuel = 4000.0 - 2.0 * t, rounds = 640.0 - 10.0 * t;
            m.fuel = fuel_to_wire(static_cast<float>(fuel > 0 ? fuel : 0));
            m.guns = 1;
            m.rounds[0] = static_cast<uint16_t>(rounds > 0 ? rounds : 0);
            m.chaff = m.flares = 30;
            uint8_t buf[sizeof(m)];
            const size_t n = encode(m, buf, sizeof(buf));
            if (n && core.send_world(c.slot, c.gen, buf, n, false)) ++res.consumables_sent;
        }
    }

    // --launch: a scripted release from the bot's jet.
    double launch_t0 = -1, launch_next = 0;
    uint16_t launch_seq = 0;
    void launch_tick() {
        if (!c.on || !res.jet_started) {
            launch_t0 = -1;
            return;
        }
        if (launch_t0 < 0) {
            launch_t0 = host_t;
            launch_next = host_t + cfg.launch_every;
        }
        if (host_t < launch_next) return;
        launch_next += cfg.launch_every;
        const Flight::Sample f = flight.sample();
        WeaponLaunchMsg m{};
        m.seq = ++launch_seq;
        m.shooter_kind = 1;
        m.shooter_a = cfg.launch_shooter_a;
        m.shooter_b = static_cast<uint16_t>(cfg.host_member);
        m.target_kind = cfg.launch_kind;
        m.target_a = cfg.launch_a;
        m.target_b = cfg.launch_b;
        m.station = 0;
        m.flags = cfg.launch_guided ? kLaunchGuided : 0;
        m.host_ms = static_cast<uint32_t>(host_t * 1000.0);
        for (int i = 0; i < 3; ++i) {
            m.pos[i] = static_cast<int32_t>(std::lround(f.pos[i] * 100.0));
            m.wvel[i] = static_cast<int16_t>(std::lround(f.vel[i] * 20.0));
        }
        m.pos[2] -= 200;  // 2 m below the jet, as from a pylon
        m.ang[1] = static_cast<int16_t>(std::lround(std::remainder(f.ang[1], 2.0 * kPi) / (2.0 * kPi) * 65536.0));
        m.bvel[1] = static_cast<int16_t>(std::lround(bot::kSpeed * 20.0));  // forward at the jet's speed
        std::snprintf(m.weapon, sizeof(m.weapon), "%s", cfg.launch_weapon.c_str());
        uint8_t buf[sizeof(WeaponLaunchMsg)];
        const size_t n = encode(m, buf, sizeof(buf));
        if (n && core.send_world(c.slot, c.gen, buf, n, true)) ++res.launches_sent;
        say("bot serve: scripted launch %u: %s%s at %s%u.%u", static_cast<unsigned>(res.launches_sent), m.weapon,
            cfg.launch_guided ? " (guided)" : "", m.target_kind == 1 ? "A " : m.target_kind == 2 ? "G " : "none ",
            static_cast<unsigned>(m.target_a), static_cast<unsigned>(m.target_b));
    }

    template <typename... A>
    void say(const char* fmt, A... args) const {
        char line[320];
        std::snprintf(line, sizeof(line), fmt, args...);
        core_log(line);
    }
    template <class M>
    void record(StreamKind k, const M& m) {
        if (!rec.is_open()) return;
        uint8_t b[kMaxMessage];
        const size_t n = encode(m, b, sizeof(b));
        if (n) rec.record(k, host_t, b, n);
    }

    WelcomeMsg make_welcome(uint8_t slot) {
        WelcomeMsg w{};
        w.client_id = static_cast<uint16_t>(slot + 1);
        w.host_game_time = host_t;
        w.theatre_hash = cfg.theatre_hash;
        w.mission_id = core.mission_id();
        const int seat = core.seat_for(c.world_tag);
        w.seat = seat >= 0 ? static_cast<uint8_t>(seat) : kNoSeat;
        w.host_seat = cfg.mission.empty() ? kNoSeat : static_cast<uint8_t>(cfg.host_member);
        if (res.jet_started) {  // the join position on the jet's wing; otherwise none (welcome_has_join is false)
            const Flight::Sample s = flight.sample();
            join_position(s.pos, s.ang[1], slot, w.spawn_pos);
            w.spawn_heading = s.ang[1];
            std::memcpy(w.spawn_vel, s.vel, sizeof(w.spawn_vel));
        }
        return w;
    }
    void send_welcome() {
        const WelcomeMsg w = make_welcome(c.slot);
        if (!peers.welcome(c.slot, c.gen, w, c.callsign, c.aircraft)) {
            say("bot serve: client %u: the Welcome could not be queued", static_cast<unsigned>(c.slot + 1));
            return;
        }
        // (A failed re-Welcome would also block the client's States in HostPeers until the next try succeeds.)
        ++res.welcomes;
        if (!welcome_has_join(w)) c.join_pending = true;
        record(StreamKind::WelcomeSent, w);
        if (welcome_has_join(w))
            say("bot serve: Welcome to client %u: join (%.0f, %.0f, %.0f) heading %.1f deg, host time %.3f",
                static_cast<unsigned>(c.slot + 1), w.spawn_pos[0], w.spawn_pos[1], w.spawn_pos[2],
                static_cast<double>(w.spawn_heading) * 180.0 / kPi, w.host_game_time);
        else
            say("bot serve: Welcome to client %u without a join position (waiting for its first State), host time %.3f",
                static_cast<unsigned>(c.slot + 1), w.host_game_time);
    }

    /// <summary>
    /// The second step of the join: the Welcome with the join position, timed from the client's first State, re-sent
    /// until its States show it near the join point, for at most kJoinGiveUp.
    /// </summary>
    void join_step(double wall) {
        if (c.join_first_wall < 0) {
            if (wall - c.first_state_wall < kReWelcomeAfter) return;
            c.join_first_wall = c.join_last_wall = wall;
            send_welcome();
            return;
        }
        const Flight::Sample s = flight.sample();
        double want[3];
        join_position(s.pos, s.ang[1], c.slot, want);
        const double d = dist3(c.last_pos, want);
        if (d < kJoinNear && c.last_state_wall > c.join_first_wall) {  // a State sent after the move
            c.join_pending = false;
            res.join_confirmed = true;
            say("bot serve: client %u is on the wing (%.0f m from the join point)", static_cast<unsigned>(c.slot + 1), d);
        } else if (wall - c.join_first_wall > kJoinGiveUp) {
            c.join_pending = false;
            res.join_gave_up = true;
            say("bot serve: client %u is still %.0f m from the join point %.0f s after the join Welcome; not sending it again",
                static_cast<unsigned>(c.slot + 1), d, kJoinGiveUp);
        } else if (wall - c.join_last_wall >= kReWelcomeEvery) {
            c.join_last_wall = wall;
            say("bot serve: client %u is %.0f m from the join point; sending the join Welcome again",
                static_cast<unsigned>(c.slot + 1), d);
            send_welcome();
        }
    }

    void start_jet(const double pos[3], double heading) {
        flight.start(cfg.script, pos, heading, host_t);
        res.jet_started = true;
        for (int i = 0; i < 3; ++i) res.jet_start[i] = pos[i];
        res.jet_start_heading = heading;
        res.jet_start_time = host_t;
        tick.reset();
        say("bot serve: jet flying '%s' from (%.0f, %.0f, %.0f) heading %.1f deg, host time %.3f", script_name(cfg.script),
            pos[0], pos[1], pos[2], heading * 180.0 / kPi, host_t);
    }

    void leave(const char* why, uint8_t reason) {
        if (!c.on) return;
        peers.leave(c.slot, c.gen, reason, false);
        if (rec.is_open()) {
            ClientLeftPayload p{};
            p.client_id = static_cast<uint16_t>(c.slot + 1);
            p.reason = reason;
            rec.record(StreamKind::ClientLeft, host_t, &p, sizeof(p));
        }
        say("bot serve: client %u \"%s\" left (%s) after %u States", static_cast<unsigned>(c.slot + 1), c.callsign, why, c.rx);
        c = Client{};
    }

    void handle(const HostEvent& e, double wall) {
        if (e.kind == HostEvent::Connect) return;
        if (e.kind == HostEvent::Disconnect) {
            if (c.on && e.slot == c.slot && e.gen == c.gen)
                leave(e.reason == 1 ? "timed out" : "disconnected", e.reason == 1 ? kPeerLeftTimeout : kPeerLeftLeft);
            return;
        }
        if (peek_world_type(e.data, e.len) == static_cast<uint8_t>(WorldType::AvatarState)) {
            AvatarStateMsg m{};
            if (!c.on || e.slot != c.slot || e.gen != c.gen || decode(e.data, e.len, m) != Status::Ok) return;
            const bool first = res.jet_states_rx == 0;
            if (first || m.engines != res.last_rx_engines || m.guns != res.last_rx_guns ||
                throttle_moved(res.last_rx_throttle, m.throttle))
                say("bot serve: client %u jet: throttle %u/255, engines 0x%02x, guns %u, radar mode %u, config 0x%04x",
                    static_cast<unsigned>(m.peer), static_cast<unsigned>(m.throttle), static_cast<unsigned>(m.engines),
                    static_cast<unsigned>(m.guns), static_cast<unsigned>(m.radar_mode), static_cast<unsigned>(m.config));
            ++res.jet_states_rx;
            res.last_rx_throttle = m.throttle;
            res.last_rx_engines = m.engines;
            res.last_rx_guns = m.guns;
            return;
        }
        if (peek_world_type(e.data, e.len) == static_cast<uint8_t>(WorldType::Loadout)) {
            LoadoutMsg m{};
            if (!c.on || e.slot != c.slot || e.gen != c.gen || decode(e.data, e.len, m) != Status::Ok) return;
            if (res.loadouts_rx++ == 0 || m.part == 0)
                say("bot serve: client %u loadout (part %u/%u): %u stations%s", static_cast<unsigned>(m.peer),
                    static_cast<unsigned>(m.part + 1), static_cast<unsigned>(m.parts), static_cast<unsigned>(m.n),
                    m.n ? "" : " (none loaded)");
            return;
        }
        if (peek_world_type(e.data, e.len) == static_cast<uint8_t>(WorldType::Consumables)) {
            ConsumablesMsg m{};
            if (!c.on || e.slot != c.slot || e.gen != c.gen || decode(e.data, e.len, m) != Status::Ok) return;
            if (res.consumables_rx++ % 15 == 0)
                say("bot serve: client %u consumables: fuel %.0f kg, %u gun(s), first %u rounds, chaff %u, flares %u",
                    static_cast<unsigned>(m.peer), fuel_from_wire(m.fuel), static_cast<unsigned>(m.guns),
                    static_cast<unsigned>(m.rounds[0]), static_cast<unsigned>(m.chaff), static_cast<unsigned>(m.flares));
            res.last_rx_fuel = m.fuel;
            return;
        }
        if (peek_world_type(e.data, e.len) == static_cast<uint8_t>(WorldType::WeaponLaunch)) {
            WeaponLaunchMsg m{};
            if (!c.on || e.slot != c.slot || e.gen != c.gen || decode(e.data, e.len, m) != Status::Ok) return;
            ++res.launches_rx;
            res.last_rx_weapon = m.weapon;
            say("bot serve: client %u launch %u: %s%s at %s%u.%u from station %u, pos (%.0f, %.0f, %.0f)",
                static_cast<unsigned>(m.peer), static_cast<unsigned>(m.seq), m.weapon,
                (m.flags & kLaunchGuided) ? " (guided)" : "",
                m.target_kind == 1 ? "A " : m.target_kind == 2 ? "G " : "none ", static_cast<unsigned>(m.target_a),
                static_cast<unsigned>(m.target_b), static_cast<unsigned>(m.station), m.pos[0] / 100.0,
                m.pos[1] / 100.0, m.pos[2] / 100.0);
            return;
        }
        if (peek_world_type(e.data, e.len) == static_cast<uint8_t>(WorldType::HitClaim)) {
            HitMsg m{};
            if (!c.on || e.slot != c.slot || e.gen != c.gen ||
                decode_hit(e.data, e.len, WorldType::HitClaim, m) != Status::Ok)
                return;
            ++res.hit_claims_rx;
            if (res.hit_claims_rx == 1 || res.hit_claims_rx % 50 == 0)
                say("bot serve: client %u hit claim %u: %c %u.%u slot %u weapon 0x%04x, %u J", static_cast<unsigned>(m.peer),
                    res.hit_claims_rx, m.target_kind == 1 ? 'A' : 'G', static_cast<unsigned>(m.target_a),
                    static_cast<unsigned>(m.target_b), static_cast<unsigned>(m.slot), static_cast<unsigned>(m.weapon),
                    static_cast<unsigned>(m.energy));
            return;
        }
        MsgType t{};
        if (peek_type(e.data, e.len, t) != Status::Ok) return;
        if (t == MsgType::Hello) {
            HelloMsg h{};
            if (decode(e.data, e.len, h) != Status::Ok) return;
            uint8_t reason = 0;
            if (c.on && !(e.slot == c.slot && e.gen == c.gen)) reason = kByeBusy;
            else if (theatre_mismatch(cfg.theatre_hash, h.theatre_hash)) reason = kByeWrongTheatre;
            if (reason) {
                ByeMsg b{};
                b.reason = reason;
                uint8_t buf[16];
                core.send(e.slot, e.gen, buf, encode(b, buf, sizeof(buf)), kChannelReliable, true);
                core.disconnect(e.slot, e.gen);
                ++res.refused;
                say("bot serve: client %u \"%s\" refused (Bye %u: %s)", static_cast<unsigned>(e.slot + 1), h.callsign,
                    static_cast<unsigned>(reason), reason == kByeBusy ? "one client only" : "another theatre");
                return;
            }
            if (c.on) return;  // a repeated Hello
            c = Client{};
            c.on = true;
            c.slot = e.slot;
            c.gen = e.gen;
            c.world_tag = h.world_tag;
            set_str(c.callsign, h.callsign);
            set_str(c.aircraft, h.aircraft);
            ++res.clients;
            if (rec.is_open()) {
                ClientJoinedPayload p{};
                p.client_id = static_cast<uint16_t>(c.slot + 1);
                std::memcpy(p.callsign, c.callsign, sizeof(p.callsign));
                std::memcpy(p.aircraft, c.aircraft, sizeof(p.aircraft));
                rec.record(StreamKind::ClientJoined, host_t, &p, sizeof(p));
            }
            say("bot serve: client %u \"%s\" (%s) connected", static_cast<unsigned>(c.slot + 1), c.callsign,
                c.aircraft[0] ? c.aircraft : "type unknown");
            send_welcome();
        } else if (t == MsgType::State) {
            StateMsg s{};
            if (!c.on || e.slot != c.slot || e.gen != c.gen || decode(e.data, e.len, s) != Status::Ok) return;
            if (!c.jet.push(s, host_t)) return;  // old or repeated: not recorded either
            if (rec.is_open()) rec.record(StreamKind::StateReceived, host_t, e.data, e.len);
            for (int i = 0; i < 3; ++i) c.last_pos[i] = s.pos[i];
            c.last_state_wall = wall;
            ++c.rx;
            ++res.client_states;
            c.last_game_time = s.game_time;
            if (!c.got_state) {
                c.got_state = true;
                c.first_state_wall = wall;
                say("bot serve: client %u first State at (%.0f, %.0f, %.0f) heading %.1f deg", static_cast<unsigned>(c.slot + 1),
                    s.pos[0], s.pos[1], s.pos[2], static_cast<double>(s.ang[1]) * 180.0 / kPi);
                if (!res.jet_started) {
                    const double h = s.ang[1];
                    const double p[3] = {s.pos[0] + kStartAhead * std::sin(h), s.pos[1] + kStartAhead * std::cos(h), s.pos[2]};
                    start_jet(p, h);
                }
            }
        } else if (t == MsgType::Bye) {
            ByeMsg b{};
            if (c.on && e.slot == c.slot && e.gen == c.gen && decode(e.data, e.len, b) == Status::Ok) {
                say("bot serve: client %u said Bye (reason %u)", static_cast<unsigned>(c.slot + 1), static_cast<unsigned>(b.reason));
                leave("Bye", kPeerLeftLeft);
            }
        }
    }

    /// <summary>One game-thread step at host time host_t (not called while paused).</summary>
    void step(double wall) {
        if (res.jet_started) {
            flight.advance_to(host_t);
            const Flight::Sample s = flight.sample();
            StateMsg m{};
            m.client_id = 0;
            m.game_time = flight.time();
            for (int i = 0; i < 3; ++i) {
                m.pos[i] = s.pos[i];
                m.vel[i] = s.vel[i];
                m.ang[i] = s.ang[i];
                m.rates[i] = s.rates[i];
            }
            if (rec.is_open()) {
                StateMsg r = m;
                r.seq = ++rec_seq;
                record(StreamKind::HostState, r);
            }
            if (c.on && peers.count() > 0 && tick.due(host_t)) {
                m.seq = ++seq;
                Out o{};
                o.at = wall + lag;
                o.len = static_cast<uint8_t>(encode(m, o.data, sizeof(o.data)));
                if (lag > 0.0) {
                    if (out_q.size() < kMaxQueued) out_q.push_back(o);
                } else if (o.len) {
                    peers.send_state(o.data, o.len, -1);
                }
                if (o.len) record(StreamKind::LocalState, m);  // at the step, before any --lag
                ++res.jet_states;
            }
        }
        if (c.on && res.jet_started && c.join_pending && c.first_state_wall >= 0) join_step(wall);
        if (c.on && c.got_state) {
            RemoteJet::Kin k{};
            const RemoteJet::Out o = c.jet.shown(host_t, &k);
            if (o == RemoteJet::Out::Coast) {
                if (!c.coasting)
                    say("bot serve: client %u: no State for over %.0f s; coasting", static_cast<unsigned>(c.slot + 1),
                        c.jet.params().coast_after);
                c.coasting = true;
            } else if (o != RemoteJet::Out::None) {
                if (c.coasting) say("bot serve: client %u: States again", static_cast<unsigned>(c.slot + 1));
                c.coasting = false;
                for (int i = 0; i < 3; ++i) {
                    c.shown[i] = k.p[i];
                    c.shown_a[i] = k.a[i];
                }
                c.have_shown = true;
            }
            if (o != RemoteJet::Out::None && c.have_shown && rec.is_open() &&
                (host_t >= c.next_shown || o == RemoteJet::Out::Snap)) {
                c.next_shown = host_t + kShownEvery;
                RemoteShownPayload p{};
                p.id = static_cast<uint16_t>(c.slot + 1);
                for (int i = 0; i < 3; ++i) {
                    p.pos[i] = c.shown[i];
                    p.ang[i] = static_cast<float>(c.shown_a[i]);
                }
                p.age = static_cast<float>(host_t - c.last_game_time);
                p.out = static_cast<uint8_t>(o);  // Coast: the last shown position and angles
                rec.record(StreamKind::RemoteShown, host_t, &p, sizeof(p));
            }
        }
    }

    void report(double elapsed, uint32_t sent_last_second, bool paused) {
        say("bot serve: t=%.0f host time %.3f, jet %u State(s) sent in the last second%s", elapsed, host_t, sent_last_second,
            paused ? " PAUSED" : (res.jet_started ? "" : " (jet not started)"));
        if (!c.on) return;
        const JetStats st = c.jet.take_stats();
        double dist = 0;
        if (res.jet_started && c.have_shown) {
            const Flight::Sample s = flight.sample();
            dist = dist3(s.pos, c.shown);
        }
        // The same fields as the mod host's per-client line; "dist" is the client's shown jet from the bot's own.
        say("bot serve: client %u \"%s\" rate %.1f Hz delay %.0f ms rtt %.0f ms err %.2f m (dist %.0f m) "
            "age %.0f/%.0f ms horizon %.0f ms snaps %u coast %u accel %.1f g",
            static_cast<unsigned>(c.slot + 1), c.callsign, st.rate_hz, st.delay * 1000.0,
            static_cast<double>(core.rtt_ms(c.slot)), st.offset_max, dist, std::fmax(0.0, st.age_p50) * 1000.0,
            std::fmax(0.0, st.age_p90) * 1000.0, st.horizon_max * 1000.0, st.snaps + st.resync_snaps, st.coasting_steps,
            st.accel_g_max);
    }
};

bool finite3(const double v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

}  // namespace

ServeResult run_serve(const ServeConfig& cfg) {
    ServeResult res;
    g_log.store(&cfg.log, std::memory_order_release);
    struct ClearLog {
        ~ClearLog() { g_log.store(nullptr, std::memory_order_release); }
    } clear_log;
    char addr[48] = {};
    uint16_t dummy = 0;
    const std::string spec = cfg.bind + ":1";
    const bool bind_ok = parse_host_spec(spec.c_str(), addr, sizeof(addr), &dummy);
    if (cfg.script == Script::Formation || !std::isfinite(cfg.lag_ms) || cfg.lag_ms < 0 || cfg.lag_ms > 2000 ||
        !std::isfinite(cfg.pause_at) || !std::isfinite(cfg.pause_for) || cfg.pause_at < 0 || cfg.pause_for < 0 ||
        !std::isfinite(cfg.duration) || cfg.duration < 0 || !finite3(cfg.start) || !std::isfinite(cfg.start_heading) ||
        !bind_ok) {
        core_log("bot serve: bad arguments (script not formation, lag 0..2000 ms, pause and duration >= 0, a numeric "
                 "IPv4 bind address)");
        res.code = 1;
        return res;
    }
    auto s = std::make_unique<Serve>(cfg);  // large (rings): not on the stack
    Serve& sv = *s;
    sv.peers.set_host(cfg.callsign.c_str(), cfg.aircraft.c_str());
    sv.core.set_host_time(sv.host_t);
    if (!cfg.mission.empty()) {
        sv.core.set_mission(cfg.mission, cfg.mission_id, cfg.mission_raw, SeatPlan{cfg.seat_members, cfg.host_member});
        sv.say("bot serve: serving mission 0x%08x (%zu bytes packed)", static_cast<unsigned>(cfg.mission_id),
               cfg.mission.size());
    }
    if (!sv.core.start(addr, cfg.port, core_log)) {
        res.code = 2;
        return res;
    }
    for (int i = 0; i < 3000 && sv.core.state() == HostCore::State::Starting; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (sv.core.state() != HostCore::State::Running) {
        sv.say("bot serve: cannot listen on %s:%u", addr, static_cast<unsigned>(cfg.port));
        sv.core.stop(false);
        res.code = 2;
        return res;
    }
    if (cfg.port_out) cfg.port_out->store(sv.core.port(), std::memory_order_release);
    if (!cfg.record_dir.empty()) {
        const SessionInfoPayload si =
            make_session_info(SessionRole::BotServe, 0, cfg.callsign.c_str(), cfg.aircraft.c_str());
        sv.rec.open(cfg.record_dir.c_str(), sv.host_t, nullptr, core_log, kStreamMaxFileBytes, L"botserve", &si);
    }
    if (cfg.has_start) sv.start_jet(cfg.start, cfg.start_heading);
    sv.say("bot serve: serving '%s' as \"%s\" (%s) on %s:%u%s%s", script_name(cfg.script), cfg.callsign.c_str(),
           cfg.aircraft.c_str(), addr, static_cast<unsigned>(sv.core.port()),
           cfg.has_start ? "" : "; the jet starts 2 km ahead of the client's first State",
           cfg.lag_ms > 0 ? "; --lag delays handling and the jet's States, not the Pongs" : "");

    const double t0 = wall_now();
    double last = t0, next_step = t0, next_report = t0 + 1.0, jet_wall0 = -1;
    bool paused = false;
    uint32_t sent_reported = 0;
    for (;;) {
        const double wall = wall_now();
        if (cfg.cancel && cfg.cancel->load(std::memory_order_relaxed)) break;
        if (cfg.duration > 0 && wall - t0 >= cfg.duration) break;
        if (sv.res.jet_started && jet_wall0 < 0) jet_wall0 = wall;
        const bool pause_now = cfg.pause_for > 0 && jet_wall0 >= 0 && wall - jet_wall0 >= cfg.pause_at &&
                               wall - jet_wall0 < cfg.pause_at + cfg.pause_for;
        if (pause_now != paused) {
            paused = pause_now;
            sv.say(paused ? "bot serve: paused: host time frozen at %.3f" : "bot serve: resumed at host time %.3f", sv.host_t);
        }
        if (!paused) sv.host_t += wall - last;
        last = wall;
        sv.core.set_host_time(sv.host_t);

        HostEvent ev[32];
        for (int round = 0; round < 8; ++round) {
            const size_t n = sv.core.poll(ev, 32);
            if (n == 0) break;
            for (size_t i = 0; i < n; ++i) {
                if (sv.lag > 0.0) {
                    if (sv.in_q.size() < kMaxQueued) sv.in_q.push_back({wall + sv.lag, ev[i]});
                } else {
                    sv.handle(ev[i], wall);
                }
            }
        }
        while (!sv.in_q.empty() && sv.in_q.front().at <= wall) {
            const HostEvent e = sv.in_q.front().e;
            sv.in_q.pop_front();
            sv.handle(e, wall);
        }
        while (!sv.out_q.empty() && sv.out_q.front().at <= wall) {
            const Serve::Out& o = sv.out_q.front();
            if (o.len && sv.c.on) sv.peers.send_state(o.data, o.len, -1);
            sv.out_q.pop_front();
        }

        if (!cfg.world.empty() || !cfg.world_events.empty()) sv.world_tick();
        if (cfg.jet != JetScript::None) sv.jet_tick();
        if (cfg.hit) sv.hit_tick();
        if (cfg.launch) sv.launch_tick();
        if (cfg.stores) sv.stores_tick();
        if (wall >= next_step) {
            next_step += kServeStep;
            if (next_step < wall - 0.25) next_step = wall + kServeStep;  // fell far behind: do not burst
            if (paused) {
                ++sv.res.paused_steps;
            } else {
                sv.step(wall);
            }
        }
        if (wall >= next_report) {
            next_report += 1.0;
            if (next_report < wall) next_report = wall + 1.0;
            sv.report(wall - t0, sv.res.jet_states - sent_reported, paused);
            sent_reported = sv.res.jet_states;
            const unsigned fail = sv.peers.take_send_fail();
            if (fail) sv.say("bot serve: %u message(s) not queued (out queue full)", fail);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (sv.c.on) {  // as the mod host ends a session: Bye 3, then the disconnect
        ByeMsg b{};
        b.reason = kByeSessionEnded;
        uint8_t buf[16];
        sv.core.send(sv.c.slot, sv.c.gen, buf, encode(b, buf, sizeof(buf)), kChannelReliable, true);
        sv.core.disconnect(sv.c.slot, sv.c.gen);
        sv.leave("the bot stopped serving", kPeerLeftRemoved);
        for (const double until = wall_now() + 0.5; wall_now() < until;) {
            HostEvent ev[32];
            sv.core.set_host_time(sv.host_t);
            sv.core.poll(ev, 32);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    sv.res.missions_sent = sv.core.missions_sent();
    sv.core.stop(false);
    // Waits at most 1 s for the writer, then detaches it (StreamWriter has no joining close): a final flush slower than
    // that would outlive the Serve that owns its ring. Accepted for a developer tool.
    sv.rec.close(true);
    sv.res.host_time_end = sv.host_t;
    sv.say("bot serve: done: %u client(s), %u Welcome(s), %u refused, %u jet States sent, %u client States, %u paused steps",
           sv.res.clients, sv.res.welcomes, sv.res.refused, sv.res.jet_states, sv.res.client_states, sv.res.paused_steps);
    return sv.res;
}

}  // namespace LazyWorks::SF2Patch::bot
