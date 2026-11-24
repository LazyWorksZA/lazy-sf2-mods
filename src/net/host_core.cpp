#include "net/host_core.h"

#include <enet/enet.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "net/receiver_math.h"

namespace lazysf2::net {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

constexpr uint32_t kMaxPingsPerSecond = 100;  // per peer; more are dropped
constexpr size_t kMaxPacket = sizeof(HostEvent::data);
constexpr size_t kMaxServiceLoop = 512;  // events handled per pass, so a flood cannot starve the queues

}  // namespace

bool parse_host_spec(const char* spec, char* addr, size_t cap, uint16_t* port) {
    if (!spec || !addr || !port || cap < 16) return false;
    const char* colon = std::strrchr(spec, ':');
    const char* port_text = colon ? colon + 1 : spec;
    char host[16] = "127.0.0.1";
    if (colon) {
        const size_t n = static_cast<size_t>(colon - spec);
        if (n < 7 || n > 15) return false;  // "1.2.3.4" .. "255.255.255.255"
        int dots = 0, run = 0;
        for (size_t i = 0; i < n; ++i) {
            const char c = spec[i];
            if (c == '.') {
                if (run == 0) return false;
                ++dots;
                run = 0;
            } else if (c >= '0' && c <= '9') {
                if (++run > 3) return false;
            } else {
                return false;  // names are not resolved: a numeric address only
            }
            host[i] = c;
        }
        if (dots != 3 || run == 0) return false;
        host[n] = 0;
    }
    if (!*port_text) return false;
    unsigned long v = 0;
    for (const char* p = port_text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + static_cast<unsigned long>(*p - '0');
        if (v > 65535) return false;
    }
    if (v == 0) return false;
    std::snprintf(addr, cap, "%s", host);
    *port = static_cast<uint16_t>(v);
    return true;
}

bool HostCore::start(const char* address, uint16_t port, LogFn log) {
    if (thread_.joinable() || !address) return false;
    std::snprintf(addr_, sizeof(addr_), "%s", address);
    req_port_ = port;
    log_ = log;
    stop_.store(false);
    finished_.store(false);
    port_.store(0);
    state_.store(State::Starting, std::memory_order_release);
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        state_.store(State::Failed, std::memory_order_release);
        return false;
    }
    return true;
}

void HostCore::stop(bool in_loader_lock) {
    if (!thread_.joinable()) return;
    stop_.store(true);
    if (in_loader_lock) {
        // The thread's exit needs the loader lock, which the caller holds: wait for its cleanup, not for its exit.
        for (int i = 0; i < 1000 && !finished_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        thread_.detach();
    } else {
        thread_.join();
    }
    state_.store(State::Idle, std::memory_order_release);
}

bool HostCore::push_in(const HostEvent& e, bool control) {
    {
        std::lock_guard<std::mutex> lock(in_mx_);
        if (in_n_ < (control ? kInCap : kInCap - kControlReserve)) {
            in_[(in_head_ + in_n_) % kInCap] = e;
            ++in_n_;
            in_count_.store(static_cast<uint32_t>(in_n_), std::memory_order_release);
            return true;
        }
    }
    (control ? n_control_ : n_queue_).fetch_add(1, std::memory_order_relaxed);
    return false;
}

size_t HostCore::poll(HostEvent* out, size_t max) {
    if (in_count_.load(std::memory_order_acquire) == 0) return 0;
    std::lock_guard<std::mutex> lock(in_mx_);
    size_t n = 0;
    while (n < max && in_n_ > 0) {
        out[n++] = in_[in_head_];
        in_head_ = (in_head_ + 1) % kInCap;
        --in_n_;
    }
    in_count_.store(static_cast<uint32_t>(in_n_), std::memory_order_release);
    return n;
}

bool HostCore::send(uint8_t slot, uint32_t gen, const uint8_t* data, size_t len, uint8_t channel, bool reliable) {
    if (slot >= kMaxClients || !data || len == 0 || len > sizeof(OutMsg::data) || channel >= kChannelCount) return false;
    std::lock_guard<std::mutex> lock(out_mx_);
    if (out_n_ >= kOutCap) return false;
    OutMsg& m = out_[(out_head_ + out_n_) % kOutCap];
    m.op = OutMsg::Send;
    m.slot = slot;
    m.gen = gen;
    m.channel = channel;
    m.reliable = reliable ? 1 : 0;
    m.len = static_cast<uint8_t>(len);
    std::memcpy(m.data, data, len);
    ++out_n_;
    return true;
}

bool HostCore::disconnect(uint8_t slot, uint32_t gen) {
    if (slot >= kMaxClients) return false;
    std::lock_guard<std::mutex> lock(out_mx_);
    if (out_n_ >= kOutCap) return false;
    OutMsg& m = out_[(out_head_ + out_n_) % kOutCap];
    m.op = OutMsg::Disconnect;
    m.slot = slot;
    m.gen = gen;
    m.len = 0;
    ++out_n_;
    return true;
}

void HostCore::set_welcomed(uint8_t slot, uint32_t gen, bool welcomed) {
    if (slot >= kMaxClients || gen == 0) return;
    uint64_t cur = slot_state_[slot].load(std::memory_order_acquire);
    const uint64_t want = (static_cast<uint64_t>(gen) << 1) | (welcomed ? 1u : 0u);
    // Only while the slot still belongs to this generation; the check and the store are one atomic step.
    while (static_cast<uint32_t>(cur >> 1) == gen && cur != 0) {
        if (slot_state_[slot].compare_exchange_weak(cur, want, std::memory_order_acq_rel)) return;
    }
}

uint32_t HostCore::rtt_ms(uint8_t slot) const {
    return slot < kMaxClients ? rtt_[slot].load(std::memory_order_relaxed) : 0;
}

HostCounters HostCore::counters() const {
    HostCounters c;
    c.received = n_received_.load(std::memory_order_relaxed);
    c.dropped_format = n_format_.load(std::memory_order_relaxed);
    c.dropped_range = n_range_.load(std::memory_order_relaxed);
    c.dropped_state = n_state_.load(std::memory_order_relaxed);
    c.dropped_queue = n_queue_.load(std::memory_order_relaxed);
    c.dropped_control = n_control_.load(std::memory_order_relaxed);
    return c;
}

// The network thread. Everything below runs on it, and only it touches ENet.
void HostCore::run() {
    char line[200];
    auto say = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof(line), fmt, args...);
        if (log_) log_(line);  // never under a lock
    };

    if (enet_initialize() != 0) {
        say("net: host failed: enet_initialize");
        state_.store(State::Failed, std::memory_order_release);
        finished_.store(true);
        return;
    }
    ENetAddress addr{};
    ENetHost* host = nullptr;
    if (enet_address_set_host_ip(&addr, addr_) == 0) {
        addr.port = req_port_;
        host = enet_host_create(&addr, kMaxClients, kChannelCount, 0, 0);
    }
    if (!host) {
        say("net: host failed: cannot bind udp %s:%u (bad address, or the port is in use)", addr_,
            static_cast<unsigned>(req_port_));
        enet_deinitialize();
        state_.store(State::Failed, std::memory_order_release);
        finished_.store(true);
        return;
    }
    host->maximumPacketSize = 1024;  // our messages are under 100 bytes: refuse big ones and fragment floods
    host->maximumWaitingData = 64 * 1024;
    port_.store(host->address.port, std::memory_order_release);
    state_.store(State::Running, std::memory_order_release);
    say("net: host on udp %s:%u", addr_, static_cast<unsigned>(host->address.port));

    ENetPeer* peers[kMaxClients] = {};
    bool hello_seen[kMaxClients] = {};
    bool bye_seen[kMaxClients] = {};
    uint64_t last_rx[kMaxClients] = {};
    uint32_t pings[kMaxClients] = {};
    uint32_t gens[kMaxClients] = {};
    uint32_t next_gen = 1;
    uint64_t next_report = now_ms() + 1000;
    HostCounters reported{};

    auto free_slot = [&](int slot) {
        peers[slot] = nullptr;
        gens[slot] = 0;
        slot_state_[slot].store(0, std::memory_order_release);  // commands for this connection are void from now on
        hello_seen[slot] = false;
        bye_seen[slot] = false;
        rtt_[slot].store(0, std::memory_order_relaxed);
    };
    auto drop = [&](std::atomic<uint32_t>& counter) { counter.fetch_add(1, std::memory_order_relaxed); };

    auto handle_packet = [&](int slot, uint8_t channel, const uint8_t* data, size_t len) {
        n_received_.fetch_add(1, std::memory_order_relaxed);
        if (len > kMaxPacket) return drop(n_format_);
        MsgType type{};
        if (peek_type(data, len, type) != Status::Ok) return drop(n_format_);
        const uint16_t id = static_cast<uint16_t>(slot + 1);
        switch (type) {
            case MsgType::Hello: {
                HelloMsg m;
                if (channel != kChannelReliable || decode(data, len, m) != Status::Ok) return drop(n_format_);
                if (hello_seen[slot]) return drop(n_state_);
                hello_seen[slot] = true;
                break;
            }
            case MsgType::Ping: {
                PingMsg m;
                if (channel != kChannelUnreliable || decode(data, len, m) != Status::Ok) return drop(n_format_);
                if (!finite(m.client_send_time)) return drop(n_range_);
                if (++pings[slot] > kMaxPingsPerSecond) return drop(n_state_);
                // Answered here, with the host game time the game thread last published (it does not advance
                // while the game is paused, which is the truth for a clock the clients stamp States in).
                PongMsg pong{};
                pong.client_send_time = m.client_send_time;
                if (!host_time_valid_.load(std::memory_order_acquire)) return drop(n_state_);  // no time to give yet
                pong.host_game_time = host_time_.load(std::memory_order_relaxed);
                uint8_t out[sizeof(PongMsg)];
                const size_t n = encode(pong, out, sizeof(out));
                if (n && peers[slot]) {
                    if (ENetPacket* p = enet_packet_create(out, n, 0)) {
                        if (enet_peer_send(peers[slot], kChannelUnreliable, p) != 0) enet_packet_destroy(p);
                    }
                }
                return;  // never queued: the game thread has nothing to do with it
            }
            case MsgType::State: {
                StateMsg m;
                if (channel != kChannelUnreliable || decode(data, len, m) != Status::Ok || m.client_id != id)
                    return drop(n_format_);
                if (!(slot_state_[slot].load(std::memory_order_acquire) & 1) ||
                    !host_time_valid_.load(std::memory_order_acquire))
                    return drop(n_state_);
                if (!state_in_range(m, host_time_.load(std::memory_order_relaxed))) return drop(n_range_);
                break;
            }
            case MsgType::Bye: {
                ByeMsg m;
                if (channel != kChannelReliable || decode(data, len, m) != Status::Ok || m.client_id != id)
                    return drop(n_format_);
                if (bye_seen[slot]) return drop(n_state_);
                bye_seen[slot] = true;
                break;
            }
            default:  // Welcome and Pong only travel from the host to a client
                return drop(n_format_);
        }
        HostEvent e;
        e.kind = HostEvent::Message;
        e.slot = static_cast<uint8_t>(slot);
        e.gen = gens[slot];
        e.channel = channel;
        e.len = static_cast<uint16_t>(len);
        std::memcpy(e.data, data, len);
        // Control events (Connect, Disconnect) may use the reserve; messages may not. A Hello or Bye that cannot be
        // queued would leave a connection the game never hears about, so that connection is ended.
        if (!push_in(e, false) && type != MsgType::State && peers[slot]) enet_peer_disconnect(peers[slot], 0);
    };

    auto handle_event = [&](ENetEvent& ev) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                int slot = -1;
                for (int i = 0; i < kMaxClients && slot < 0; ++i)
                    if (!peers[i]) slot = i;
                if (slot < 0) {
                    enet_peer_disconnect_now(ev.peer, 0);
                    break;
                }
                peers[slot] = ev.peer;
                gens[slot] = next_gen++;
                if (next_gen == 0) next_gen = 1;
                slot_state_[slot].store(static_cast<uint64_t>(gens[slot]) << 1, std::memory_order_release);
                ev.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(slot + 1));
                hello_seen[slot] = false;
                bye_seen[slot] = false;
                last_rx[slot] = now_ms();
                pings[slot] = 0;
                HostEvent e;
                e.kind = HostEvent::Connect;
                e.slot = static_cast<uint8_t>(slot);
                e.gen = gens[slot];
                push_in(e, true);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {  // the peer left, or ENet's own timeout fired
                const uintptr_t tag = reinterpret_cast<uintptr_t>(ev.peer->data);
                ev.peer->data = nullptr;
                if (tag >= 1 && tag <= kMaxClients && peers[tag - 1] == ev.peer) {
                    const int slot = static_cast<int>(tag) - 1;
                    HostEvent e;
                    e.kind = HostEvent::Disconnect;
                    e.slot = static_cast<uint8_t>(slot);
                    e.gen = gens[slot];
                    free_slot(slot);
                    push_in(e, true);
                }
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                const uintptr_t tag = reinterpret_cast<uintptr_t>(ev.peer->data);
                if (tag >= 1 && tag <= kMaxClients && peers[tag - 1] == ev.peer) {
                    const int slot = static_cast<int>(tag) - 1;
                    last_rx[slot] = now_ms();
                    handle_packet(slot, ev.channelID, ev.packet->data, ev.packet->dataLength);
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            default:
                break;
        }
    };

    auto drain_out = [&] {
        OutMsg batch[32];
        for (;;) {
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lock(out_mx_);
                while (n < 32 && out_n_ > 0) {
                    batch[n++] = out_[out_head_];
                    out_head_ = (out_head_ + 1) % kOutCap;
                    --out_n_;
                }
            }
            if (n == 0) return;
            for (size_t i = 0; i < n; ++i) {
                const OutMsg& m = batch[i];
                ENetPeer* peer = peers[m.slot];
                if (!peer || m.gen != gens[m.slot]) continue;  // a command for a connection that is gone
                if (m.op == OutMsg::Disconnect) {
                    enet_peer_disconnect(peer, 0);
                } else if (ENetPacket* p =
                               enet_packet_create(m.data, m.len, m.reliable ? ENET_PACKET_FLAG_RELIABLE : 0)) {
                    if (enet_peer_send(peer, m.channel, p) != 0) enet_packet_destroy(p);
                }
            }
        }
    };

    while (!stop_.load(std::memory_order_relaxed)) {
        drain_out();
        ENetEvent ev;
        int r = enet_host_service(host, &ev, 5);  // waits up to 5 ms for traffic, then returns
        for (size_t guard = 0; r > 0 && guard < kMaxServiceLoop; ++guard) {
            handle_event(ev);
            r = enet_host_service(host, &ev, 0);
        }
        drain_out();
        enet_host_flush(host);

        const uint64_t now = now_ms();
        for (int i = 0; i < kMaxClients; ++i) {
            if (!peers[i]) continue;
            rtt_[i].store(peers[i]->roundTripTime, std::memory_order_relaxed);
            if (now - last_rx[i] >= kTimeoutMs) {
                enet_peer_disconnect_now(peers[i], 0);
                HostEvent e;
                e.kind = HostEvent::Disconnect;
                e.slot = static_cast<uint8_t>(i);
                e.gen = gens[i];
                e.reason = 1;
                free_slot(i);
                push_in(e, true);
            }
        }
        if (now >= next_report) {
            next_report = now + 1000;
            for (uint32_t& p : pings) p = 0;
            const HostCounters c = counters();
            if (c.dropped_format != reported.dropped_format || c.dropped_range != reported.dropped_range ||
                c.dropped_state != reported.dropped_state || c.dropped_queue != reported.dropped_queue ||
                c.dropped_control != reported.dropped_control) {
                say("net: dropped packets in the last second: %u malformed, %u out of range, %u wrong phase or rate, "
                    "%u States while the game is not draining (queue nearly full), %u control events (queue full)",
                    c.dropped_format - reported.dropped_format, c.dropped_range - reported.dropped_range,
                    c.dropped_state - reported.dropped_state, c.dropped_queue - reported.dropped_queue,
                    c.dropped_control - reported.dropped_control);
                reported = c;
            }
        }
    }

    for (int i = 0; i < kMaxClients; ++i)
        if (peers[i]) enet_peer_disconnect_now(peers[i], 0);
    enet_host_flush(host);
    enet_host_destroy(host);
    enet_deinitialize();
    say("net: host stopped");
    finished_.store(true);
}

}  // namespace lazysf2::net
