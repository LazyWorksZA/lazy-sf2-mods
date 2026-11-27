#include "net/mission_fetch.h"

#include "net/name_lookup.h"

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
constexpr uint64_t kConnectMs = 5000, kRetryMs = 5000, kSilenceMs = 10000;

}  // namespace

bool MissionFetch::start(const char* host, uint16_t port, uint32_t tag, const AuthClient& auth, LogFn log,
                         uint32_t poll_ms) {
    if (state_.load() != State::Idle || !host || !*host || port == 0 || tag == 0 || !auth.has_password()) return false;
    auth_ = auth;
    host_ = host;
    port_ = port;
    tag_ = tag;
    poll_ms_ = poll_ms ? poll_ms : 5000;
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
    std::lock_guard<std::mutex> lock(mx_);
    if (!fresh_) return false;
    fresh_ = false;
    *packed = packed_;
    *offer = offer_;
    return true;
}

void MissionFetch::run() {
    char line[256];
    auto say = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof(line), fmt, args...);
        if (log_) log_(line);
    };
    auto finish = [&] { state_.store(State::Stopped, std::memory_order_release); };
    if (enet_initialize() != 0) {
        say("net: mission fetch failed: enet_initialize");
        return finish();
    }
    ENetAddress addr{};
    if (enet_address_set_host_ip(&addr, host_.c_str()) != 0) {
        // A name: looked up on a thread of its own, so stop() (on the game thread) never waits for the resolver.
        uint32_t ip = 0;
        const LookupResult r = lookup_name(host_.c_str(), &ip, stop_, kLookupMs);
        if (r != LookupResult::Resolved) {
            if (r != LookupResult::Stopped) {
                say("net: mission fetch failed: cannot resolve %s%s", host_.c_str(),
                    r == LookupResult::TimedOut ? " (no answer in time)" : "");
                host_unknown_.store(true, std::memory_order_release);
            }
            enet_deinitialize();
            return finish();
        }
        addr.host = ip;
    }
    addr.port = port_;
    ENetHost* host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    if (!host) {
        say("net: mission fetch failed: enet_host_create");
        enet_deinitialize();
        return finish();
    }
    host->maximumPacketSize = 1024;  // a chunk is 972 bytes
    say("net: mission fetch from %s:%u, asking every %u ms", host_.c_str(), static_cast<unsigned>(port_),
        static_cast<unsigned>(poll_ms_));

    uint32_t have = 0;  // the id of the newest mission received
    bool logged_none = false, logged_lost = false, refused = false;
    while (!stop_.load() && !refused) {
        ENetPeer* peer = enet_host_connect(host, &addr, kChannelCount, 0);
        if (!peer) break;
        MissionAssembly as;
        bool connected = false, round_done = false;
        bool answered = false, proved = false;  // per connection: nothing from the host counts before its proof
        AuthChallengeMsg challenge{};
        AuthAnswerMsg answer{};
        AuthKey key{};
        uint64_t t0 = ms_now(), last_rx = t0;
        while (!stop_.load() && !round_done && !refused) {
            ENetEvent ev;
            const int r = enet_host_service(host, &ev, 10);
            const uint64_t now = ms_now();
            if (r > 0 && ev.type == ENET_EVENT_TYPE_CONNECT) {
                connected = true;  // the request goes out with the answer to the host's password challenge
                last_rx = now;
            } else if (r > 0 && ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                break;
            } else if (r > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
                last_rx = now;
                const uint8_t* d = ev.packet->data;
                const size_t n = ev.packet->dataLength;
                const uint8_t wt = peek_world_type(d, n);
                MissionOfferMsg o;
                MissionChunkHead ch;
                const uint8_t* payload = nullptr;
                AuthChallengeMsg chal;
                AuthProofMsg proof;
                ByeMsg bye;
                if (!wt && decode(d, n, chal) == Status::Ok) {
                    if (answered) {
                        enet_packet_destroy(ev.packet);
                        continue;
                    }
                    answered = true;
                    challenge = chal;
                    MissionRequestMsg rq{};
                    rq.have_id = have;
                    rq.tag = tag_;
                    uint8_t ab[sizeof(AuthAnswerMsg)], rb[sizeof(MissionRequestMsg)];
                    auto send = [&](const uint8_t* src, size_t len) {
                        if (ENetPacket* p = enet_packet_create(src, len, ENET_PACKET_FLAG_RELIABLE))
                            if (enet_peer_send(peer, kChannelReliable, p) != 0) enet_packet_destroy(p);
                    };
                    if (auth_.key_for(chal, &key) && auth_answer(key, chal, &answer)) {
                        // Reliable and in order: the host reads the answer before the request.
                        send(ab, encode(answer, ab, sizeof(ab)));
                        send(rb, encode(rq, rb, sizeof(rb)));
                    } else {
                        say("net: mission fetch: cannot answer the host's challenge (iterations %u); not asking again",
                            static_cast<unsigned>(chal.iterations));
                        refused = true;
                    }
                } else if (!wt && decode(d, n, proof) == Status::Ok) {
                    if (answered && !proved && auth_check_host(key, challenge, answer, proof)) {
                        proved = true;
                        auth_wipe(key);
                    } else if (!proved) {
                        say("net: mission fetch: the host could not prove the session password; not asking again");
                        wrong_password_.store(true, std::memory_order_release);
                        refused = true;
                    }
                } else if (!wt && decode(d, n, bye) == Status::Ok && bye.reason == kByeWrongPassword) {
                    say("net: mission fetch: the host refused the session password; not asking again");
                    wrong_password_.store(true, std::memory_order_release);
                    refused = true;
                } else if (!wt && n >= sizeof(Header) && d[0] == 'L' && d[1] == 'Z' && d[2] != kVersion) {
                    say("net: mission fetch: the host speaks protocol v%u (this client is v%u); not asking again",
                        static_cast<unsigned>(d[2]), static_cast<unsigned>(kVersion));
                    refused = true;
                } else if (!proved) {
                    // An offer, a chunk or anything else from a host that has not proven the password: unread.
                } else if (wt == static_cast<uint8_t>(WorldType::MissionOffer) && decode(d, n, o) == Status::Ok) {
                    latest_seat_.store(o.seat, std::memory_order_release);
                    if (o.status == kOfferOk || o.status == kOfferSame) {
                        latest_id_.store(o.mission_id, std::memory_order_release);
                        logged_none = false;
                    }
                    if (o.status == kOfferOk && as.begin(o)) {
                        say("net: mission offer 0x%08x: %u bytes in %u chunks (seat %u assigned, host seat %u)",
                            static_cast<unsigned>(o.mission_id), static_cast<unsigned>(o.packed_bytes),
                            static_cast<unsigned>(o.chunk_count), static_cast<unsigned>(o.seat),
                            static_cast<unsigned>(o.host_seat));
                    } else {
                        if (o.status != kOfferSame && !logged_none)
                            say("net: the host has %s; asking again",
                                o.status == kOfferNone ? "no mission yet" : "a mission it cannot send");
                        if (o.status != kOfferSame) logged_none = true;
                        round_done = true;
                    }
                } else if (wt == static_cast<uint8_t>(WorldType::MissionChunk) &&
                           decode_mission_chunk(d, n, ch, &payload) == Status::Ok) {
                    if (as.add(ch, payload) == MissionAssembly::Add::Done) {
                        {
                            std::lock_guard<std::mutex> lock(mx_);
                            packed_ = as.bytes();
                            offer_ = as.offer();
                            fresh_ = true;
                        }
                        have = as.offer().mission_id;
                        say("net: mission 0x%08x received (%u bytes)", static_cast<unsigned>(have),
                            static_cast<unsigned>(as.offer().packed_bytes));
                        round_done = true;
                    }
                }
                enet_packet_destroy(ev.packet);
            }
            if ((!connected && now - t0 > kConnectMs) || (connected && now - last_rx > kSilenceMs)) break;
        }
        // Leave between asks: a waiting client holds no host slot (the host keeps its seat by tag while free).
        if (connected) {
            enet_peer_disconnect(peer, 0);
            for (int i = 0; i < 20; ++i) {
                ENetEvent ev;
                const int r = enet_host_service(host, &ev, 10);
                if (r > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
                if (r > 0 && ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
            }
        }
        enet_peer_reset(peer);
        if (!round_done && !refused && !stop_.load()) {
            if (!logged_lost)
                say("net: mission fetch: %s; trying again", connected ? "connection lost" : "no answer from the host");
            logged_lost = true;
        } else if (round_done) {
            logged_lost = false;
        }
        const uint64_t until = ms_now() + (round_done ? poll_ms_ : kRetryMs);
        while (!stop_.load() && !refused && ms_now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    enet_host_destroy(host);
    enet_deinitialize();
    finish();
}

}  // namespace LazyWorks::SF2Patch::net
