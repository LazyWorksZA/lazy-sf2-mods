#include "net/mission_fetch.h"

#include <enet/enet.h>

#include <chrono>
#include <cstdio>

namespace LazyWorks::SF2Patch::net {

namespace {

using Clock = std::chrono::steady_clock;
uint64_t ms_now() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}
constexpr uint64_t kConnectMs = 5000, kRetryMs = 5000, kAskAgainMs = 3000, kSilenceMs = 10000;

}  // namespace

bool MissionFetch::start(const char* host, uint16_t port, uint32_t tag, LogFn log) {
    if (state_.load() != State::Idle || !host || !*host || port == 0 || tag == 0) return false;
    host_ = host;
    port_ = port;
    tag_ = tag;
    log_ = log;
    stop_.store(false);
    state_.store(State::Running, std::memory_order_release);
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        state_.store(State::Stopped);
        return false;
    }
    return true;
}

void MissionFetch::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}

bool MissionFetch::take(std::vector<uint8_t>* packed, MissionOfferMsg* offer) {
    if (state_.load(std::memory_order_acquire) != State::Done) return false;
    std::lock_guard<std::mutex> lock(mx_);
    if (taken_) return false;
    taken_ = true;
    *packed = std::move(packed_);
    *offer = offer_;
    return true;
}

void MissionFetch::run() {
    char line[256];
    auto say = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof(line), fmt, args...);
        if (log_) log_(line);
    };
    if (enet_initialize() != 0) {
        say("net: mission fetch failed: enet_initialize");
        state_.store(State::Stopped);
        return;
    }
    ENetAddress addr{};
    if (enet_address_set_host(&addr, host_.c_str()) != 0) {
        say("net: mission fetch failed: cannot resolve %s", host_.c_str());
        enet_deinitialize();
        state_.store(State::Stopped);
        return;
    }
    addr.port = port_;
    ENetHost* host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    if (!host) {
        say("net: mission fetch failed: enet_host_create");
        enet_deinitialize();
        state_.store(State::Stopped);
        return;
    }
    host->maximumPacketSize = 1024;  // a chunk is 972 bytes
    say("net: mission fetch from %s:%u", host_.c_str(), static_cast<unsigned>(port_));

    MissionAssembly as;
    bool done = false;
    bool logged_wait = false, refused = false, wait_none = false;
    while (!stop_.load() && !done && !refused) {
        wait_none = false;
        ENetPeer* peer = enet_host_connect(host, &addr, kChannelCount, 0);
        if (!peer) break;
        bool connected = false;
        uint64_t t0 = ms_now(), next_ask = 0, last_rx = t0;
        while (!stop_.load() && !done) {
            ENetEvent ev;
            const int r = enet_host_service(host, &ev, 10);
            const uint64_t now = ms_now();
            if (r > 0) {
                if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                    connected = true;
                    last_rx = now;
                    next_ask = now;
                } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                    break;
                } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    last_rx = now;
                    const uint8_t* d = ev.packet->data;
                    const size_t n = ev.packet->dataLength;
                    const uint8_t wt = peek_world_type(d, n);
                    if (!wt && n >= sizeof(Header) && d[0] == 'L' && d[1] == 'Z' && d[2] != kVersion) {
                        say("net: mission fetch: the host speaks protocol v%u (this client is v%u); not asking again",
                            static_cast<unsigned>(d[2]), static_cast<unsigned>(kVersion));
                        refused = true;
                    }
                    MissionOfferMsg o;
                    MissionChunkHead ch;
                    const uint8_t* payload = nullptr;
                    if (wt == static_cast<uint8_t>(WorldType::MissionOffer) && decode(d, n, o) == Status::Ok) {
                        if (o.status == kOfferOk && as.begin(o)) {
                            say("net: mission offer 0x%08x: %u bytes in %u chunks (seat %u assigned, host seat %u)",
                                static_cast<unsigned>(o.mission_id), static_cast<unsigned>(o.packed_bytes),
                                static_cast<unsigned>(o.chunk_count), static_cast<unsigned>(o.seat),
                                static_cast<unsigned>(o.host_seat));
                        } else {
                            if (!logged_wait)
                                say("net: the host has %s; asking again every 3 s",
                                    o.status == kOfferNone ? "no mission yet" : "a mission it cannot send");
                            logged_wait = true;
                            wait_none = true;  // leave, so the waiting fetch does not hold a host slot
                        }
                    } else if (wt == static_cast<uint8_t>(WorldType::MissionChunk) &&
                               decode_mission_chunk(d, n, ch, &payload) == Status::Ok) {
                        if (as.add(ch, payload) == MissionAssembly::Add::Done) {
                            std::lock_guard<std::mutex> lock(mx_);
                            packed_ = as.bytes();
                            offer_ = as.offer();
                            done = true;
                        }
                    }
                    enet_packet_destroy(ev.packet);
                    if (wait_none || refused) break;
                }
            }
            if (connected && next_ask && now >= next_ask) {
                MissionRequestMsg rq{};
                rq.tag = tag_;
                uint8_t b[sizeof(rq)];
                const size_t len = encode(rq, b, sizeof(b));
                if (ENetPacket* p = enet_packet_create(b, len, ENET_PACKET_FLAG_RELIABLE))
                    if (enet_peer_send(peer, kChannelReliable, p) != 0) enet_packet_destroy(p);
                next_ask = 0;
            }
            if ((!connected && now - t0 > kConnectMs) || (connected && now - last_rx > kSilenceMs)) break;
        }
        if (done) {
            enet_peer_disconnect_later(peer, 0);
            for (int i = 0; i < 50; ++i) {  // up to 0.5 s for a clean disconnect
                ENetEvent ev;
                if (enet_host_service(host, &ev, 10) > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE)
                    enet_packet_destroy(ev.packet);
            }
            break;
        }
        if (wait_none) {
            enet_peer_disconnect(peer, 0);
            enet_host_flush(host);
        }
        enet_peer_reset(peer);
        if (!stop_.load() && !refused) {
            if (!wait_none)
                say("net: mission fetch: %s; trying again in 5 s", connected ? "connection lost" : "no answer from the host");
            const uint64_t until = ms_now() + (wait_none ? kAskAgainMs : kRetryMs);
            while (!stop_.load() && ms_now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    enet_host_destroy(host);
    enet_deinitialize();
    if (done) {
        say("net: mission 0x%08x received (%u bytes)", static_cast<unsigned>(offer_.mission_id),
            static_cast<unsigned>(offer_.packed_bytes));
        state_.store(State::Done, std::memory_order_release);
    } else {
        state_.store(State::Stopped, std::memory_order_release);
    }
}

}  // namespace LazyWorks::SF2Patch::net
