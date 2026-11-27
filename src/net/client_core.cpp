#include "net/client_core.h"

#include "net/world_proto.h"
#include "net/handover_format.h"

#include <enet/enet.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "net/receiver_math.h"

namespace LazyWorks::SF2Patch::net {

namespace {

constexpr double kPingInterval = 0.25;   // seconds
constexpr uint32_t kMaxPongsPerSecond = 100;  // more are dropped
constexpr double kByeGrace = 0.05;       // after the Bye is acknowledged, so the host's game side has picked it up
constexpr double kByeDeliveryLimit = 1.0;  // seconds to wait for the Bye's acknowledgement before disconnecting anyway
constexpr double kDisconnectLimit = 1.0;   // seconds to wait for ENet's disconnect handshake
constexpr double kStopByeLimit = 0.25;     // on stop(), seconds a pending Bye may take to be delivered
constexpr size_t kMaxPacket = sizeof(ClientEvent::data);
constexpr size_t kMaxServiceLoop = 512;  // events handled per pass, so a flood cannot starve the queues

constexpr double kHostByeGrace = 1.0;     // after the host's Bye, seconds before we disconnect if it has not

/// <summary>
/// A name lookup on its own short-lived thread, which getaddrinfo can block for many seconds: the network thread
/// waits for it while still honouring stop(), and abandons it if it must. Shared by the two threads with a reference
/// count; the lookup thread touches only this block, never the ClientCore.
/// </summary>
struct Lookup {
    std::atomic<int> refs{2};
    std::atomic<int> result{0};  // 0 pending, 1 resolved (ip set), 2 failed
    uint32_t ip = 0;             // ENetAddress::host
    char name[256] = {};
    ClientLimits::ResolveFn fn = nullptr;
};
void release(Lookup* l) {
    if (l->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete l;
}
bool enet_resolve(const char* name, uint32_t* ip) {
    ENetAddress a{};
    if (enet_address_set_host(&a, name) != 0) return false;
    *ip = a.host;
    return true;
}
void run_lookup(Lookup* l) {
    const bool wsa = enet_initialize() == 0;  // its own Winsock reference: the network thread may deinitialize first
    uint32_t ip = 0;
    const bool ok = wsa && (l->fn ? l->fn : enet_resolve)(l->name, &ip);
    if (wsa) enet_deinitialize();
    l->ip = ip;
    l->result.store(ok ? 1 : 2, std::memory_order_release);
    release(l);
}

bool delivered(const ENetPeer* p) {
    return p->reliableDataInTransit == 0 && enet_list_empty(&p->sentReliableCommands) &&
           enet_list_empty(&p->outgoingSendReliableCommands) && enet_list_empty(&p->outgoingCommands);
}

const char* end_text(ClientEnd e) {
    switch (e) {
        case ClientEnd::HostClosed: return "the host closed the connection";
        case ClientEnd::Silent: return "the host went silent";
        case ClientEnd::CannotConnect: return "cannot connect";
        case ClientEnd::NoWelcome: return "no Welcome from the host";
        case ClientEnd::Left: return "we left";
        case ClientEnd::Refused: return "refused: the host speaks another protocol version";
        case ClientEnd::QueueFull: return "the event queue is full";
    }
    return "?";
}

}  // namespace

double ClientCore::wall_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool ClientCore::start(const char* host, uint16_t port, const HelloMsg& hello, LogFn log, const AuthKey& key,
                       const ClientLimits& limits) {
    if (thread_.joinable() || !host || !*host || std::strlen(host) >= sizeof(host_) || port == 0 || !key.set) return false;
    if (started_ && !finished_.load()) return false;  // a thread detached by stop(true) is still cleaning up
    std::snprintf(host_, sizeof(host_), "%s", host);
    port_ = port;
    hello_ = hello;
    key_ = key;
    limits_ = limits;
    log_ = log;
    // No other thread runs now: reset everything a previous session left.
    stop_.store(false);
    finished_.store(false);
    bye_reason_.store(-1);
    id_.store(0);
    rtt_.store(0);
    clock_ = ClockSync{};
    {
        std::lock_guard<std::mutex> lock(clock_mx_);
        clock_valid_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(in_mx_);
        in_head_ = in_n_ = 0;
        for (size_t& n : in_states_) n = 0;
        end_pending_ = end_delivered_ = false;
        in_count_.store(0);
    }
    {
        std::lock_guard<std::mutex> lock(out_mx_);
        out_head_ = out_n_ = 0;
    }
    phase_.store(Phase::Connecting, std::memory_order_release);
    started_ = true;
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        phase_.store(Phase::Failed, std::memory_order_release);
        return false;
    }
    return true;
}

void ClientCore::stop(bool in_loader_lock) {
    if (!thread_.joinable()) return;
    stop_.store(true);
    if (in_loader_lock) {
        // The thread's exit needs the loader lock, which the caller holds: wait for its cleanup, not for its exit.
        for (int i = 0; i < 1000 && !finished_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        thread_.detach();
    } else {
        thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(clock_mx_);
        clock_valid_ = false;
    }
    phase_.store(Phase::Idle, std::memory_order_release);
}

bool ClientCore::push_in(const ClientEvent& e, int source) {
    const bool control = source < 0;
    {
        std::lock_guard<std::mutex> lock(in_mx_);
        const bool over_quota = !control && in_states_[source] >= kStateQuota;
        if (!over_quota && in_n_ < (control ? kInCap : kInCap - kControlReserve)) {
            in_[(in_head_ + in_n_) % kInCap] = e;
            ++in_n_;
            if (!control) ++in_states_[source];
            in_count_.store(static_cast<uint32_t>(in_n_), std::memory_order_release);
            return true;
        }
    }
    (control ? n_control_ : n_queue_).fetch_add(1, std::memory_order_relaxed);
    return false;
}

void ClientCore::set_end(ClientEnd why, Phase phase, double wall) {
    {
        std::lock_guard<std::mutex> lock(clock_mx_);
        clock_valid_ = false;
    }
    // The phase first: a game thread that sees the Disconnected event (through in_mx_) then also sees this phase.
    // Not after stop(): a thread detached there must not overwrite the Idle that stop() set.
    if (!stop_.load()) phase_.store(phase, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(in_mx_);
        end_ = ClientEvent{};
        end_.kind = ClientEvent::Disconnected;
        end_.reason = static_cast<uint8_t>(why);
        end_.wall_rx = wall;
        end_pending_ = true;
        in_count_.store(static_cast<uint32_t>(in_n_ + 1), std::memory_order_release);
    }
}

size_t ClientCore::poll_world(WorldPacket* out, size_t max) {
    if (!out) return 0;
    std::lock_guard<std::mutex> lock(world_mx_);
    size_t n = 0;
    for (WorldRing* r : {&world_events_, &world_states_}) {
        while (n < max && r->n > 0) {
            out[n++] = r->buf[r->head];
            r->head = (r->head + 1) % r->cap;
            --r->n;
        }
    }
    return n;
}

size_t ClientCore::poll(ClientEvent* out, size_t max) {
    if (!out || in_count_.load(std::memory_order_acquire) == 0) return 0;
    std::lock_guard<std::mutex> lock(in_mx_);
    size_t n = 0;
    while (n < max && in_n_ > 0) {
        const ClientEvent& e = in_[in_head_];
        if (e.kind == ClientEvent::State) {
            uint16_t src = 0;
            std::memcpy(&src, e.data + offsetof(StateMsg, client_id), sizeof(src));
            if (src < kSources && in_states_[src] > 0) --in_states_[src];
        }
        out[n++] = e;
        in_head_ = (in_head_ + 1) % kInCap;
        --in_n_;
    }
    if (n < max && in_n_ == 0 && end_pending_ && !end_delivered_) {
        out[n++] = end_;
        end_delivered_ = true;
    }
    in_count_.store(static_cast<uint32_t>(in_n_ + (end_pending_ && !end_delivered_ ? 1 : 0)), std::memory_order_release);
    return n;
}

bool ClientCore::send_state(const StateMsg& s) {
    if (phase_.load(std::memory_order_acquire) != Phase::Welcomed || bye_reason_.load(std::memory_order_relaxed) >= 0)
        return false;
    StateMsg m = s;
    m.client_id = id_.load(std::memory_order_acquire);
    uint8_t buf[sizeof(StateMsg)];
    const size_t n = encode(m, buf, sizeof(buf));
    if (n == 0) return false;
    {
        std::lock_guard<std::mutex> lock(out_mx_);
        if (out_n_ < kOutCap) {
            OutMsg& o = out_[(out_head_ + out_n_) % kOutCap];
            o.len = static_cast<uint8_t>(n);
            o.reliable = 0;
            std::memcpy(o.data, buf, n);
            ++out_n_;
            return true;
        }
    }
    n_out_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool ClientCore::send_world(const uint8_t* data, size_t len, bool reliable) {
    if (!data || len == 0 || len > sizeof(OutMsg::data) || phase_.load(std::memory_order_acquire) != Phase::Welcomed ||
        bye_reason_.load(std::memory_order_relaxed) >= 0)
        return false;
    std::lock_guard<std::mutex> lock(out_mx_);
    if (out_n_ >= kOutCap) return false;
    OutMsg& o = out_[(out_head_ + out_n_) % kOutCap];
    o.len = static_cast<uint8_t>(len);
    o.reliable = reliable ? 1 : 0;
    std::memcpy(o.data, data, len);
    ++out_n_;
    return true;
}

void ClientCore::bye(uint8_t reason) {
    int none = -1;
    bye_reason_.compare_exchange_strong(none, reason);
}

bool ClientCore::clock_now(double wall, double* host_time) const {
    ClockSync c;
    {
        std::lock_guard<std::mutex> lock(clock_mx_);
        if (!clock_valid_) return false;
        c = clock_snap_;
    }
    if (!c.have_pong() || c.on_hold(wall)) return false;
    if (host_time) *host_time = c.now(wall);
    return true;
}

bool ClientCore::clock_snapshot(ClockSync* out) const {
    std::lock_guard<std::mutex> lock(clock_mx_);
    if (!clock_valid_ || !out) return false;
    *out = clock_snap_;
    return true;
}

ClientCounters ClientCore::counters() const {
    ClientCounters c;
    c.received = n_received_.load(std::memory_order_relaxed);
    c.dropped_format = n_format_.load(std::memory_order_relaxed);
    c.dropped_range = n_range_.load(std::memory_order_relaxed);
    c.dropped_state = n_state_.load(std::memory_order_relaxed);
    c.dropped_queue = n_queue_.load(std::memory_order_relaxed);
    c.dropped_control = n_control_.load(std::memory_order_relaxed);
    c.dropped_out = n_out_.load(std::memory_order_relaxed);
    c.pings = n_pings_.load(std::memory_order_relaxed);
    c.pongs = n_pongs_.load(std::memory_order_relaxed);
    c.world = n_world_.load(std::memory_order_relaxed);
    c.dropped_world = n_world_drop_.load(std::memory_order_relaxed);
    return c;
}

// The network thread. Everything below runs on it, and only it touches ENet and clock_.
void ClientCore::run() {
    char line[320];
    auto say = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof(line), fmt, args...);
        if (log_) log_(line);  // never under a lock
    };
    bool ended = false;
    auto end = [&](ClientEnd why, Phase phase) {
        if (ended) return;
        ended = true;
        set_end(why, phase, wall_now());
        say("net: client disconnected: %s", end_text(why));
    };
    auto fail_early = [&](const char* what) {
        say("net: client failed: %s", what);
        end(ClientEnd::CannotConnect, Phase::Failed);
        finished_.store(true);
    };

    if (enet_initialize() != 0) return fail_early("enet_initialize");
    say("net: client connecting to %s:%u", host_, static_cast<unsigned>(port_));
    ENetAddress addr{};
    // A numeric address needs no lookup. A name is looked up on a thread of its own, so that neither the game thread
    // nor stop() ever waits for the resolver.
    if (enet_address_set_host_ip(&addr, host_) != 0) {
        Lookup* l = new (std::nothrow) Lookup;
        if (!l) {
            enet_deinitialize();
            return fail_early("out of memory");
        }
        std::snprintf(l->name, sizeof(l->name), "%s", host_);
        l->fn = limits_.resolve;
        try {
            std::thread([l] { run_lookup(l); }).detach();
        } catch (...) {
            l->result.store(2);
            release(l);  // the thread's reference
        }
        const double until = wall_now() + limits_.connect_ms * 0.001;
        while (l->result.load(std::memory_order_acquire) == 0 && !stop_.load() && wall_now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const int result = l->result.load(std::memory_order_acquire);
        if (result == 1) addr.host = l->ip;
        release(l);  // ours: an abandoned lookup frees the block when it returns
        if (result != 1) {
            enet_deinitialize();
            if (stop_.load()) {
                say("net: client stopped (during the name lookup)");
                finished_.store(true);
                return;
            }
            return fail_early(result == 2 ? "the host name does not resolve" : "the name lookup took too long");
        }
    }
    addr.port = port_;
    ENetHost* host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    ENetPeer* peer = host ? enet_host_connect(host, &addr, kChannelCount, 0) : nullptr;
    if (!peer) {
        if (host) enet_host_destroy(host);
        enet_deinitialize();
        return fail_early("cannot create the ENet client");
    }
    host->maximumPacketSize = 1024;  // our messages are under 100 bytes: refuse big ones and fragment floods
    host->maximumWaitingData = 64 * 1024;

    bool connected = false, welcomed = false, bye_seen = false, refused = false;
    bool answered = false;  // the password challenge was answered (once per connection)
    bool bye_started = false, bye_sent = false, disconnect_sent = false;
    bool peer_known[kSources] = {};  // PeerInfo seen and no PeerLeft since: duplicates are dropped
    double bye_seen_at = 0, welcomed_at = 0;
    double t0 = wall_now();
    double connect_deadline = t0 + limits_.connect_ms * 0.001, welcome_deadline = 0;
    double last_rx = t0, next_ping = 0, bye_at = 0, delivered_at = 0, disconnect_at = 0, next_report = t0 + 1.0;
    uint32_t pongs_this_second = 0;
    uint16_t my_id = 0;
    ClientCounters reported{};

    auto drop = [&](std::atomic<uint32_t>& counter) { counter.fetch_add(1, std::memory_order_relaxed); };
    auto publish_clock = [&] {
        std::lock_guard<std::mutex> lock(clock_mx_);
        clock_snap_ = clock_;
        clock_valid_ = true;
    };
    auto send_now = [&](const uint8_t* d, size_t n, uint8_t channel) {
        ENetPacket* p = enet_packet_create(d, n, channel == kChannelReliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (p && enet_peer_send(peer, channel, p) != 0) enet_packet_destroy(p);
    };
    auto push_control = [&](const ClientEvent& e, const char* what) {
        if (push_in(e, -1)) return;
        // Practically unreachable (the reserve is sized for every control event): the game would miss a peer joining
        // or leaving, so the session ends instead.
        say("net: client connection ended: %s could not be queued", what);
        enet_peer_disconnect_now(peer, 0);
        end(ClientEnd::QueueFull, Phase::Closed);
    };
    auto send_bye = [&] {
        bye_started = true;
        bye_at = wall_now();
        if (!welcomed) return;  // before the Welcome we have no id to say it with: only disconnect
        ByeMsg b{};
        b.client_id = my_id;
        b.reason = static_cast<uint8_t>(bye_reason_.load());
        uint8_t out[sizeof(ByeMsg)];
        const size_t n = encode(b, out, sizeof(out));
        if (n) send_now(out, n, kChannelReliable);
        bye_sent = true;
        say("net: client Bye sent (reason %u)", static_cast<unsigned>(b.reason));
    };

    // World messages: checked for framing and size here, applied by the game thread (world_sync.cpp).
    auto handle_world = [&](uint8_t channel, uint8_t wt, const uint8_t* data, size_t len, double rx) {
        WorldStatesHead sh;
        WorldEventsHead eh;
        const WorldObjState* sr = nullptr;
        const WorldEventRec* er = nullptr;
        AvatarStateMsg am;
        WorldSnapshotMsg snm;
        HitMsg hm;
        WeaponLaunchMsg wlm;
        LoadoutMsg lom;
        DamageStateMsg dsm;
        MissionClockMsg mcm;
        SeatStateMsg ssm;
        HandoverDoneMsg hdm;
        CountermeasureMsg cmm;
        ConsumablesMsg com;
        const bool ok =
            (wt == static_cast<uint8_t>(WorldType::WorldSnapshot) && channel == kChannelReliable &&
             decode(data, len, snm) == Status::Ok) || (wt == static_cast<uint8_t>(WorldType::WorldStates) && channel == kChannelUnreliable &&
                         decode_world_states(data, len, sh, &sr) == Status::Ok) ||
                        (wt == static_cast<uint8_t>(WorldType::WorldEvents) && channel == kChannelReliable &&
                         decode_world_events(data, len, eh, &er) == Status::Ok) ||
                        (wt == static_cast<uint8_t>(WorldType::AvatarState) && channel == kChannelReliable &&
                         decode(data, len, am) == Status::Ok) ||
                        (wt == static_cast<uint8_t>(WorldType::HitApply) && channel == kChannelReliable &&
                         decode_hit(data, len, WorldType::HitApply, hm) == Status::Ok) ||
                        (wt == static_cast<uint8_t>(WorldType::WeaponLaunch) && channel == kChannelReliable &&
                         decode(data, len, wlm) == Status::Ok) ||
                        (wt == static_cast<uint8_t>(WorldType::Loadout) && channel == kChannelReliable &&
                         decode(data, len, lom) == Status::Ok) ||
                        (wt == static_cast<uint8_t>(WorldType::Consumables) && channel == kChannelUnreliable &&
                         decode(data, len, com) == Status::Ok) ||
                        (wt == static_cast<uint8_t>(WorldType::DamageState) && channel == kChannelReliable &&
                         decode(data, len, dsm) == Status::Ok) ||
                        (wt == static_cast<uint8_t>(WorldType::Countermeasure) && channel == kChannelReliable &&
                         decode(data, len, cmm) == Status::Ok) ||
                        (wt == static_cast<uint8_t>(WorldType::MissionClock) && channel == kChannelUnreliable &&
                         decode(data, len, mcm) == Status::Ok) ||
                        (wt == static_cast<uint8_t>(WorldType::SeatState) && channel == kChannelReliable &&
                         decode(data, len, ssm) == Status::Ok) ||
                        (wt == static_cast<uint8_t>(WorldType::HandoverDone) && channel == kChannelReliable &&
                         decode(data, len, hdm) == Status::Ok);
        if (!ok) return drop(n_format_);
        if (!welcomed || len > sizeof(WorldPacket::data)) return drop(n_state_);
        // Reliable world messages (events, jet states) are never dropped: they share the events ring.
        const bool event = channel == kChannelReliable;  // unreliable ones (states, consumables) may be dropped
        bool full = false;
        {
            std::lock_guard<std::mutex> lock(world_mx_);
            WorldRing& r = event ? world_events_ : world_states_;
            if (r.n >= r.cap) {
                full = true;
            } else {
                WorldPacket& p = r.buf[(r.head + r.n) % r.cap];
                p.wall_rx = rx;
                p.len = static_cast<uint16_t>(len);
                std::memcpy(p.data, data, len);
                ++r.n;
            }
        }
        if (!full) {
            n_world_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        n_world_drop_.fetch_add(1, std::memory_order_relaxed);
        if (!event) return;  // a state: the next one replaces it
        say("net: client connection ended: a world event could not be queued (the game is not draining)");
        enet_peer_disconnect_now(peer, 0);
        end(ClientEnd::QueueFull, Phase::Closed);
    };

    auto handle_packet = [&](uint8_t channel, const uint8_t* data, size_t len, double rx) {
        n_received_.fetch_add(1, std::memory_order_relaxed);
        if (const uint8_t wt = peek_world_type(data, len)) return handle_world(channel, wt, data, len, rx);
        if (len > kMaxPacket) return drop(n_format_);
        MsgType type{};
        const Status ps = peek_type(data, len, type);
        if (ps == Status::BadVersion && channel == kChannelReliable && !refused) {
            // A host of another protocol version: tell the game the way a v2 host would (Bye 6), then leave.
            refused = true;
            say("net: the host speaks another protocol version (this client is v%u); leaving", static_cast<unsigned>(kVersion));
            ByeMsg b{};
            b.client_id = 0;
            b.reason = kByeWrongVersion;
            ClientEvent e;
            e.kind = ClientEvent::Bye;
            e.wall_rx = rx;
            e.len = static_cast<uint16_t>(encode(b, e.data, sizeof(e.data)));
            push_control(e, "Bye");
            enet_peer_disconnect(peer, 0);
            disconnect_sent = true;
            disconnect_at = rx;
        }
        if (ps != Status::Ok) return drop(n_format_);
        ClientEvent e;
        e.wall_rx = rx;
        int source = -1;
        const char* what = "";
        switch (type) {
            case MsgType::Welcome: {
                WelcomeMsg m;
                if (channel != kChannelReliable || decode(data, len, m) != Status::Ok || m.client_id == 0 ||
                    m.client_id > kMaxClients || (welcomed && m.client_id != my_id))
                    return drop(n_format_);
                if (!connected || bye_seen || bye_started || refused) return drop(n_state_);
                if (welcomed && rx - welcomed_at < kReWelcomeMinGap) return drop(n_state_);
                bool ok = finite(m.host_game_time) && within(m.spawn_heading, kMaxAngle);
                for (int i = 0; i < 3; ++i)
                    ok = ok && within(m.spawn_pos[i], kMaxPosition) && within(m.spawn_vel[i], kMaxVelocity);
                if (!ok) return drop(n_range_);
                // A re-Welcome (the same id) re-seeds the clock; ClockSync counts it as a reset, so readers step.
                clock_.on_welcome(rx, m.host_game_time, 0.0005 * static_cast<double>(peer->roundTripTime));
                if (!welcomed) {
                    welcomed = true;
                    my_id = m.client_id;
                    id_.store(my_id, std::memory_order_release);
                    next_ping = rx;
                    say("net: client welcomed as client %u, host time %.3f", static_cast<unsigned>(my_id), m.host_game_time);
                }
                welcomed_at = rx;
                for (bool& k : peer_known) k = false;  // the host announces its peers again after a Welcome
                publish_clock();
                phase_.store(Phase::Welcomed, std::memory_order_release);
                e.kind = ClientEvent::Welcome;
                what = "Welcome";
                break;
            }
            case MsgType::Pong: {
                PongMsg m;
                if (channel != kChannelUnreliable || decode(data, len, m) != Status::Ok) return drop(n_format_);
                if (!welcomed || ++pongs_this_second > kMaxPongsPerSecond) return drop(n_state_);
                if (!finite(m.client_send_time) || !finite(m.host_game_time)) return drop(n_range_);
                if (!clock_.on_pong(m.client_send_time, rx, m.host_game_time)) return drop(n_range_);
                n_pongs_.fetch_add(1, std::memory_order_relaxed);
                publish_clock();
                return;  // never queued: the clock is all it is for
            }
            case MsgType::State: {
                StateMsg m;
                if (channel != kChannelUnreliable || decode(data, len, m) != Status::Ok || m.client_id > kMaxClients ||
                    (welcomed && m.client_id == my_id))
                    return drop(n_format_);
                // Only from a peer the host announced (PeerInfo) and that has not left (PeerLeft): the channels are
                // not ordered against each other, so a State may overtake the PeerInfo (that sample is lost) or arrive
                // after the PeerLeft (it must not bring the peer back).
                if (!welcomed || bye_seen || !peer_known[m.client_id]) return drop(n_state_);
                if (!state_in_range(m, clock_.now(rx))) return drop(n_range_);
                e.kind = ClientEvent::State;
                source = m.client_id;
                break;
            }
            case MsgType::PeerInfo: {
                PeerInfoMsg m;
                if (channel != kChannelReliable || decode(data, len, m) != Status::Ok || m.id > kMaxClients ||
                    (welcomed && m.id == my_id))
                    return drop(n_format_);
                if (!welcomed || bye_seen || peer_known[m.id]) return drop(n_state_);
                peer_known[m.id] = true;
                e.kind = ClientEvent::PeerInfo;
                what = "PeerInfo";
                break;
            }
            case MsgType::PeerLeft: {
                PeerLeftMsg m;
                if (channel != kChannelReliable || decode(data, len, m) != Status::Ok || m.id == 0 || m.id > kMaxClients ||
                    (welcomed && m.id == my_id))
                    return drop(n_format_);
                if (!welcomed || bye_seen || !peer_known[m.id]) return drop(n_state_);
                peer_known[m.id] = false;
                e.kind = ClientEvent::PeerLeft;
                what = "PeerLeft";
                break;
            }
            case MsgType::AuthChallenge: {
                AuthChallengeMsg m;
                if (channel != kChannelReliable || decode(data, len, m) != Status::Ok) return drop(n_format_);
                if (!connected || answered || bye_seen || refused) return drop(n_state_);
                answered = true;
                AuthAnswerMsg a;
                uint8_t out[sizeof(HelloMsg)];
                if (!auth_answer(key_, m, &a)) {
                    say("net: client could not answer the password challenge; leaving");
                    enet_peer_disconnect(peer, 0);
                    disconnect_sent = true;
                    disconnect_at = rx;
                    return;
                }
                // Reliable and in order: the host reads the answer before the Hello.
                send_now(out, encode(a, out, sizeof(out)), kChannelReliable);
                send_now(out, encode(hello_, out, sizeof(out)), kChannelReliable);
                say("net: client answered the password challenge; Hello sent");
                return;
            }
            case MsgType::Bye: {
                ByeMsg m;
                // A Bye carries its sender's id (protocol.h): from the host always 0.
                if (channel != kChannelReliable || decode(data, len, m) != Status::Ok || m.client_id != 0)
                    return drop(n_format_);
                if (!connected || bye_seen) return drop(n_state_);
                bye_seen = true;
                bye_seen_at = rx;
                say("net: client got Bye from the host (reason %u)", static_cast<unsigned>(m.reason));
                e.kind = ClientEvent::Bye;
                what = "Bye";
                break;
            }
            default:  // Hello and Ping only travel from a client to the host
                return drop(n_format_);
        }
        e.len = static_cast<uint16_t>(len);
        std::memcpy(e.data, data, len);
        if (source >= 0) {
            push_in(e, source);  // a refused State is counted; the next one will do
        } else {
            push_control(e, what);
        }
    };

    auto handle_event = [&](ENetEvent& ev) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                if (ev.peer != peer || connected) break;
                connected = true;
                const double now = wall_now();
                last_rx = now;
                welcome_deadline = now + limits_.welcome_ms * 0.001;
                // ENet's throttle drops unreliable packets when the RTT rises above its running mean, which a lagged or
                // jittery link does all the time; States and Pings are better lost to the network than to the throttle.
                // This also configures the host's side of the connection.
                enet_peer_throttle_configure(peer, 5000, 0, 0);
                // The Hello goes out with the answer to the host's password challenge (handle_packet).
                phase_.store(Phase::AwaitWelcome, std::memory_order_release);
                say("net: client connected; waiting for the host's password challenge (ENet packet throttle off for "
                    "this connection)");
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                if (ev.peer != peer) break;
                if (!connected) {
                    end(ClientEnd::CannotConnect, Phase::Failed);
                } else if (refused) {
                    end(ClientEnd::Refused, Phase::Closed);
                } else if (bye_started) {
                    end(ClientEnd::Left, Phase::Closed);
                } else {
                    end(ClientEnd::HostClosed, Phase::Closed);
                }
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                if (ev.peer == peer && !ended) {
                    const double rx = wall_now();
                    last_rx = rx;
                    handle_packet(ev.channelID, ev.packet->data, ev.packet->dataLength, rx);
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            default:
                break;
        }
    };

    auto drain_out = [&] {
        OutMsg batch[16];
        for (;;) {
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lock(out_mx_);
                while (n < 16 && out_n_ > 0) {
                    batch[n++] = out_[out_head_];
                    out_head_ = (out_head_ + 1) % kOutCap;
                    --out_n_;
                }
            }
            if (n == 0) return;
            if (!welcomed || bye_started || bye_seen || ended) {  // late States: discarded
                n_out_.fetch_add(static_cast<uint32_t>(n), std::memory_order_relaxed);
                continue;
            }
            for (size_t i = 0; i < n; ++i)
                send_now(batch[i].data, batch[i].len, batch[i].reliable ? kChannelReliable : kChannelUnreliable);
        }
    };

    while (!stop_.load(std::memory_order_relaxed) && !ended) {
        drain_out();
        ENetEvent ev;
        int r = enet_host_service(host, &ev, 2);  // waits up to 2 ms for traffic (a queued State waits at most that long)
        for (size_t guard = 0; r > 0 && guard < kMaxServiceLoop && !ended; ++guard) {
            handle_event(ev);
            if (ended) break;
            r = enet_host_service(host, &ev, 0);
        }
        if (ended) break;
        const double now = wall_now();

        if (!connected && now >= connect_deadline) {
            say("net: client: no answer to the connect request after %u ms", static_cast<unsigned>(limits_.connect_ms));
            end(ClientEnd::CannotConnect, Phase::Failed);
            break;
        }
        if (!connected && bye_reason_.load(std::memory_order_relaxed) >= 0) {  // bye() before the connection: just go
            end(ClientEnd::Left, Phase::Closed);
            break;
        }
        if (connected && !welcomed && !disconnect_sent && now >= welcome_deadline) {
            say("net: client: no Welcome %u ms after connecting", static_cast<unsigned>(limits_.welcome_ms));
            enet_peer_disconnect_now(peer, 0);
            end(ClientEnd::NoWelcome, Phase::Failed);
            break;
        }
        // Before the Welcome the host may be busy loading; the Welcome limit covers that wait.
        if (welcomed && now - last_rx >= limits_.silence_ms * 0.001) {
            say("net: client: nothing from the host for %u ms", static_cast<unsigned>(limits_.silence_ms));
            enet_peer_disconnect_now(peer, 0);
            end(ClientEnd::Silent, Phase::Closed);
            break;
        }

        if (bye_seen && !bye_started && !disconnect_sent && now - bye_seen_at >= kHostByeGrace) {
            // The host said Bye but kept the connection: leave by ourselves.
            enet_peer_disconnect(peer, 0);
            disconnect_sent = true;
            disconnect_at = now;
        }
        if (connected && !bye_started && !disconnect_sent && bye_reason_.load(std::memory_order_relaxed) >= 0) send_bye();
        if (bye_started && !disconnect_sent) {
            // ENet drops a peer's undelivered packets when the disconnect arrives, so disconnect only once the Bye is
            // acknowledged, plus a moment for the host's game side to pick it up.
            if (delivered(peer) && delivered_at == 0) delivered_at = now;
            if (!bye_sent || (delivered_at > 0 && now - delivered_at >= kByeGrace) || now - bye_at >= kByeDeliveryLimit) {
                enet_peer_disconnect(peer, 0);
                disconnect_sent = true;
                disconnect_at = now;
            }
        }
        if (disconnect_sent && now - disconnect_at >= kDisconnectLimit) {  // the handshake did not complete: just go
            enet_peer_disconnect_now(peer, 0);
            end(refused ? ClientEnd::Refused : bye_started ? ClientEnd::Left : ClientEnd::HostClosed, Phase::Closed);
            break;
        }

        if (welcomed && !bye_started && !bye_seen && !disconnect_sent && now >= next_ping) {
            PingMsg p{};
            p.client_send_time = now;
            uint8_t out[sizeof(PingMsg)];
            const size_t n = encode(p, out, sizeof(out));
            if (n) {
                send_now(out, n, kChannelUnreliable);
                n_pings_.fetch_add(1, std::memory_order_relaxed);
            }
            next_ping += kPingInterval;
            if (next_ping < now) next_ping = now + kPingInterval;  // fell behind: do not burst
        }
        drain_out();
        enet_host_flush(host);
        rtt_.store(peer->roundTripTime, std::memory_order_relaxed);

        if (now >= next_report) {
            next_report = now + 1.0;
            pongs_this_second = 0;
            const ClientCounters c = counters();
            if (c.dropped_format != reported.dropped_format || c.dropped_range != reported.dropped_range ||
                c.dropped_state != reported.dropped_state || c.dropped_queue != reported.dropped_queue ||
                c.dropped_control != reported.dropped_control || c.dropped_out != reported.dropped_out) {
                say("net: client dropped packets in the last second: %u malformed, %u out of range, %u wrong phase or rate, "
                    "%u States refused (the game is not draining), %u control events lost (queue full), "
                    "%u outgoing States (queue full, or after a Bye)",
                    c.dropped_format - reported.dropped_format, c.dropped_range - reported.dropped_range,
                    c.dropped_state - reported.dropped_state, c.dropped_queue - reported.dropped_queue,
                    c.dropped_control - reported.dropped_control, c.dropped_out - reported.dropped_out);
            }
            reported = c;
        }
    }

    if (!ended && connected) {
        // stop(): give a Bye the game asked for a short chance to arrive, then leave at once.
        if (!bye_started && welcomed && bye_reason_.load() >= 0) send_bye();
        if (bye_sent) {
            enet_host_flush(host);
            for (const double until = wall_now() + kStopByeLimit; !delivered(peer) && wall_now() < until;) {
                ENetEvent ev;
                if (enet_host_service(host, &ev, 5) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            }
        }
        enet_peer_disconnect_now(peer, 0);
    }
    enet_host_flush(host);
    enet_host_destroy(host);
    enet_deinitialize();
    {
        std::lock_guard<std::mutex> lock(clock_mx_);
        clock_valid_ = false;
    }
    say("net: client stopped");
    finished_.store(true);
}

}  // namespace LazyWorks::SF2Patch::net
