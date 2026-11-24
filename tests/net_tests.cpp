// Console tests for the network protocol and an ENet loopback. Not shipped, no game needed.
#include <enet/enet.h>

#include <atomic>
#include <memory>
#include <chrono>
#include <cmath>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "acmi/acmi_convert.h"
#include "acmi/acmi_format.h"
#include "acmi/tacview_core.h"
#include "acmi/tacview_writer.h"
#include "hooks/weapon_class.h"
#include "hooks/aircraft_side.h"
#include "bot_client.h"
#include "bot_core.h"
#include "net/host_core.h"
#include "net/protocol.h"
#include "net/stream_format.h"
#include "net/stream_record.h"

using namespace lazysf2::net;

namespace {

int g_fail = 0;

void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool send_msg(ENetPeer* peer, const uint8_t* data, size_t n, uint8_t channel, bool reliable) {
    ENetPacket* p = enet_packet_create(data, n, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!p) return false;
    return enet_peer_send(peer, channel, p) == 0;
}

void loopback() {
    std::printf("loopback\n");
    const bool inited = enet_initialize() == 0;
    expect(inited, "enet_initialize");
    if (!inited) return;

    // Cleans up on every exit path.
    struct Cleanup {
        ENetHost* host = nullptr;
        ENetHost* client = nullptr;
        ~Cleanup() {
            if (client) enet_host_destroy(client);
            if (host) enet_host_destroy(host);
            enet_deinitialize();
        }
    } cleanup;

    ENetAddress addr{};
    enet_address_set_host(&addr, "127.0.0.1");
    addr.port = 0;  // let the OS pick a free port
    ENetHost* host = cleanup.host = enet_host_create(&addr, 4, kChannelCount, 0, 0);
    ENetHost* client = cleanup.client = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    expect(host && client, "host and client created");
    if (!host || !client) return;
    addr.port = host->address.port;
    expect(addr.port != 0, "host bound to an actual port");
    if (addr.port == 0) return;

    ENetPeer* cpeer = enet_host_connect(client, &addr, kChannelCount, 0);
    expect(cpeer != nullptr, "connect started");
    if (!cpeer) return;

    ENetPeer* hpeer = nullptr;
    bool got_hello = false, got_welcome = false, host_connected = false, client_connected = false;
    uint32_t states_sent = 0, states_got = 0, states_bad = 0, last_seq = 0;
    bool seq_ordered = true;

    for (int iter = 0; iter < 2000 && (!got_welcome || states_got < states_sent || states_sent < 100); ++iter) {
        ENetEvent ev;
        while (enet_host_service(host, &ev, 0) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                host_connected = true;
                hpeer = ev.peer;
            } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                if (ev.channelID == kChannelReliable) {
                    HelloMsg h{};
                    if (decode(ev.packet->data, ev.packet->dataLength, h) == Status::Ok &&
                        std::strcmp(h.callsign, "Bot") == 0 && std::strcmp(h.aircraft, "F-4E") == 0) {
                        got_hello = true;
                        WelcomeMsg w{};
                        w.client_id = 1;
                        w.spawn_pos[0] = 10;
                        w.spawn_heading = 0.5f;
                        w.host_game_time = 77.0;
                        uint8_t buf[64];
                        size_t n = encode(w, buf, sizeof(buf));
                        send_msg(ev.peer, buf, n, kChannelReliable, true);
                    }
                } else if (ev.channelID == kChannelUnreliable) {
                    StateMsg s{};
                    if (decode(ev.packet->data, ev.packet->dataLength, s) == Status::Ok && s.client_id == 1) {
                        if (states_got && s.seq <= last_seq) seq_ordered = false;
                        last_seq = s.seq;
                        ++states_got;
                    } else {
                        ++states_bad;
                    }
                }
                enet_packet_destroy(ev.packet);
            }
        }
        while (enet_host_service(client, &ev, 0) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                client_connected = true;
                HelloMsg h{};
                set_str(h.callsign, "Bot");
                set_str(h.aircraft, "F-4E");
                uint8_t buf[64];
                size_t n = encode(h, buf, sizeof(buf));
                send_msg(ev.peer, buf, n, kChannelReliable, true);
            } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                WelcomeMsg w{};
                if (ev.channelID == kChannelReliable &&
                    decode(ev.packet->data, ev.packet->dataLength, w) == Status::Ok && w.client_id == 1 &&
                    w.host_game_time == 77.0)
                    got_welcome = true;
                enet_packet_destroy(ev.packet);
            }
        }
        // After the welcome, send one State per iteration until 100 are out.
        if (got_welcome && states_sent < 100) {
            StateMsg s{};
            s.client_id = 1;
            s.seq = ++states_sent;
            s.game_time = 78.0 + 0.01 * s.seq;
            s.pos[0] = s.seq;
            uint8_t buf[96];
            size_t n = encode(s, buf, sizeof(buf));
            send_msg(cpeer, buf, n, kChannelUnreliable, false);
        }
        enet_host_flush(client);
    }

    expect(host_connected && client_connected, "connected");
    expect(got_hello, "host got Hello");
    expect(got_welcome, "client got Welcome");
    expect(states_sent == 100, "100 State packets sent");
    // Loopback is lossless in practice, but unreliable delivery is not guaranteed; require all here.
    std::printf("  states received %u of %u (bad %u)\n", states_got, states_sent, states_bad);
    expect(states_got == 100 && states_bad == 0, "100 State packets received intact");
    expect(seq_ordered, "State sequence numbers increase");
    (void)hpeer;

    enet_peer_disconnect_now(cpeer, 0);
    enet_host_flush(client);
}

// --- The host's network core (the part of host mode that needs no game) ---------------------------

void parse_tests() {
    std::printf("parse_host_spec\n");
    char a[48];
    uint16_t port = 0;
    expect(parse_host_spec("7777", a, sizeof(a), &port) && port == 7777 && !std::strcmp(a, "127.0.0.1"),
           "a bare port defaults to 127.0.0.1");
    expect(parse_host_spec("0.0.0.0:65535", a, sizeof(a), &port) && port == 65535 && !std::strcmp(a, "0.0.0.0"),
           "address and port");
    const char* bad[] = {"",          "0",           "65536",       "-1",           "12a",
                         ":7777",     "127.0.0.1:",  "localhost:7777", "1.2.3:5",    "1.2.3.4.5:5",
                         "1111.2.3.4:5", "1..2.3:5", "127.0.0.1:0", "::1",          "127.0.0.1:99999999999999"};
    bool all_bad = true;
    for (const char* b : bad) {
        if (parse_host_spec(b, a, sizeof(a), &port)) {
            std::printf("    accepted '%s'\n", b);
            all_bad = false;
        }
    }
    expect(all_bad, "malformed specs are rejected");
}

// A client, and helpers that stand in for the game thread.
struct TestClient {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    bool connected = false, disconnected = false;
    struct Got {
        uint8_t channel;
        std::vector<uint8_t> bytes;
    };
    std::vector<Got> got;
    ~TestClient() {
        if (host) enet_host_destroy(host);
    }
    void service(int ms) {
        ENetEvent ev;
        while (enet_host_service(host, &ev, ms) > 0) {
            ms = 0;
            if (ev.type == ENET_EVENT_TYPE_CONNECT) connected = true;
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) disconnected = true;
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                got.push_back(
                    {ev.channelID, std::vector<uint8_t>(ev.packet->data, ev.packet->data + ev.packet->dataLength)});
                enet_packet_destroy(ev.packet);
            }
        }
    }
    void send(const uint8_t* d, size_t n, uint8_t channel) {
        ENetPacket* p = enet_packet_create(d, n, channel == kChannelReliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (p && enet_peer_send(peer, channel, p) != 0) enet_packet_destroy(p);
        enet_host_flush(host);
    }
    void idle(int ms) {  // service the client for about ms milliseconds
        for (int i = 0; i < ms; ++i) {
            service(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

// Polls the core for up to ms, until want events have arrived; the client is serviced meanwhile.
std::vector<HostEvent> collect(HostCore& core, TestClient& c, size_t want, int ms) {
    std::vector<HostEvent> out;
    for (int t = 0; t < ms && out.size() < want; ++t) {
        c.service(0);
        HostEvent ev[16];
        const size_t n = core.poll(ev, 16);
        out.insert(out.end(), ev, ev + n);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return out;
}

bool queue_empty(HostCore& core) {
    HostEvent e[4];
    return core.poll(e, 4) == 0;
}

StateMsg good_state(uint32_t seq, double t) {
    StateMsg s{};
    s.client_id = 1;
    s.seq = seq;
    s.game_time = t;
    s.pos[0] = 100.0 + seq;
    s.vel[1] = 200.0f;
    s.ang[1] = 1.0f;
    s.rates[2] = 0.1f;
    return s;
}
template <typename M>
size_t enc(const M& m, uint8_t* out) {
    return encode(m, out, 128);
}

void host_core_tests() {
    std::printf("host core\n");
    HostCore core;
    expect(core.start("127.0.0.1", 0, [](const char* l) { std::printf("    [core] %s\n", l); }), "start");
    for (int i = 0; i < 2000 && core.state() == HostCore::State::Starting; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(core.state() == HostCore::State::Running && core.port() != 0, "the network thread bound a free port");
    if (core.state() != HostCore::State::Running) return;

    TestClient c;
    c.host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    expect(c.host != nullptr, "client host");
    if (!c.host) return;
    ENetAddress addr{};
    enet_address_set_host_ip(&addr, "127.0.0.1");
    addr.port = core.port();
    c.peer = enet_host_connect(c.host, &addr, kChannelCount, 0);
    auto ev = collect(core, c, 1, 2000);
    expect(ev.size() == 1 && ev[0].kind == HostEvent::Connect && ev[0].slot == 0 && ev[0].gen != 0,
           "Connect event for slot 0, with a connection generation");
    const uint32_t gen0 = ev.empty() ? 0 : ev[0].gen;

    uint8_t buf[128];
    {  // no host time published yet: a Ping gets no Pong (a stale clock would be worse than none)
        PingMsg early_ping{};
        early_ping.client_send_time = 1.0;
        c.send(buf, enc(early_ping, buf), kChannelUnreliable);
        c.idle(100);
        expect(c.got.empty() && core.counters().dropped_state == 1, "no Pong before the host time is published");
        core.set_host_time(100.0);
    }
    HelloMsg h{};
    set_str(h.callsign, "BOT1");
    set_str(h.aircraft, "F-4E");
    c.send(buf, enc(h, buf), kChannelReliable);
    // A State before the Welcome (slot not welcomed yet) must not get through.
    c.send(buf, enc(good_state(1, 100.5), buf), kChannelUnreliable);
    ev = collect(core, c, 1, 2000);
    HelloMsg hd{};
    expect(ev.size() == 1 && ev[0].kind == HostEvent::Message && ev[0].channel == kChannelReliable &&
               decode(ev[0].data, ev[0].len, hd) == Status::Ok && !std::strcmp(hd.callsign, "BOT1"),
           "Hello arrives intact");
    c.idle(100);  // let the early State arrive (and be dropped)
    expect(core.counters().dropped_state == 2 && queue_empty(core), "a State before the Welcome is dropped");

    // Welcome, then Ping/Pong, as the game thread would do.
    WelcomeMsg w{};
    w.client_id = 1;
    w.spawn_pos[0] = 5.0;
    w.host_game_time = 100.0;
    core.set_welcomed(0, gen0, true);
    expect(core.send(0, gen0, buf, enc(w, buf), kChannelReliable, true), "Welcome queued");
    PingMsg ping{};
    ping.client_send_time = 12.5;
    c.send(buf, enc(ping, buf), kChannelUnreliable);
    // The network thread answers the Ping itself, with the host time published by set_host_time (100.0).
    for (int i = 0; i < 500 && c.got.size() < 2; ++i) c.idle(1);
    expect(queue_empty(core), "a Ping is answered on the network thread and never queued for the game thread");
    bool got_welcome = false, got_pong = false;
    for (auto& g : c.got) {
        WelcomeMsg wd;
        PongMsg qd;
        if (decode(g.bytes.data(), g.bytes.size(), wd) == Status::Ok && wd.client_id == 1 && wd.spawn_pos[0] == 5.0)
            got_welcome = true;
        if (decode(g.bytes.data(), g.bytes.size(), qd) == Status::Ok && qd.client_send_time == 12.5 &&
            qd.host_game_time == 100.0)
            got_pong = true;
    }
    expect(got_welcome && got_pong, "client got Welcome and Pong");

    // Good States pass.
    for (uint32_t i = 1; i <= 3; ++i) c.send(buf, enc(good_state(i, 100.0 + 0.1 * i), buf), kChannelUnreliable);
    ev = collect(core, c, 3, 2000);
    bool states_ok = ev.size() == 3;
    for (size_t i = 0; states_ok && i < ev.size(); ++i) {
        StateMsg d;
        states_ok = decode(ev[i].data, ev[i].len, d) == Status::Ok && d.seq == i + 1 && d.pos[0] == 101.0 + i;
    }
    expect(states_ok, "3 good States arrive in order and intact");

    // Bad packets: every one must be dropped, none may reach the game thread.
    const HostCounters before = core.counters();
    const double inf = std::numeric_limits<double>::infinity();
    const float nanf = std::numeric_limits<float>::quiet_NaN();
    auto send_state = [&](const StateMsg& m) { c.send(buf, enc(m, buf), kChannelUnreliable); };
    StateMsg s = good_state(10, 100.5);
    s.pos[2] = 1.0e7;
    send_state(s);  // position out of range
    s = good_state(11, 100.5);
    s.vel[0] = 2000.0f;
    send_state(s);  // velocity out of range
    s = good_state(12, 100.5);
    s.ang[0] = nanf;
    send_state(s);  // NaN angle
    s = good_state(13, 100.5);
    s.rates[1] = static_cast<float>(inf);
    send_state(s);  // infinite rate
    send_state(good_state(14, 100.0 + 3601.0));  // time more than an hour from the host's
    s = good_state(15, 100.5);
    s.client_id = 2;
    send_state(s);  // someone else's client id
    {
        const size_t n = enc(good_state(16, 100.5), buf);
        c.send(buf, n - 1, kChannelUnreliable);  // truncated
        buf[n] = 0;
        c.send(buf, n + 1, kChannelUnreliable);  // oversized
        enc(good_state(17, 100.5), buf);
        buf[0] = 'X';
        c.send(buf, n, kChannelUnreliable);  // wrong magic
        enc(good_state(18, 100.5), buf);
        buf[2] = 2;
        c.send(buf, n, kChannelUnreliable);  // wrong version
        enc(good_state(19, 100.5), buf);
        c.send(buf, n, kChannelReliable);  // a State on the reliable channel
    }
    c.send(buf, enc(w, buf), kChannelUnreliable);  // a Welcome from a client
    c.send(buf, enc(h, buf), kChannelUnreliable);  // a Hello on the unreliable channel
    c.send(buf, enc(h, buf), kChannelReliable);    // a second Hello
    PingMsg nanping{};
    nanping.client_send_time = std::numeric_limits<double>::quiet_NaN();
    c.send(buf, enc(nanping, buf), kChannelUnreliable);  // NaN in a Ping
    // And a good one after them all, which must still get through.
    c.send(buf, enc(good_state(20, 101.0), buf), kChannelUnreliable);
    ev = collect(core, c, 1, 3000);
    StateMsg last{};
    expect(ev.size() == 1 && decode(ev[0].data, ev[0].len, last) == Status::Ok && last.seq == 20,
           "only the good State after the bad ones reaches the game thread");
    c.idle(200);
    const HostCounters after = core.counters();
    const uint32_t range = after.dropped_range - before.dropped_range;
    const uint32_t format = after.dropped_format - before.dropped_format;
    const uint32_t state = after.dropped_state - before.dropped_state;
    std::printf("  dropped: %u out of range, %u malformed, %u wrong phase\n", range, format, state);
    expect(range == 6 && format == 8 && state == 1 && queue_empty(core),
           "6 out of range, 8 malformed, 1 repeated Hello, and nothing else queued");

    // Bye: the game thread frees the slot and asks for the disconnect.
    ByeMsg bye{};
    bye.client_id = 1;
    c.send(buf, enc(bye, buf), kChannelReliable);
    ev = collect(core, c, 1, 2000);
    expect(ev.size() == 1 && ev[0].kind == HostEvent::Message && ev[0].channel == kChannelReliable, "Bye arrives");
    const uint32_t state_before_bye2 = core.counters().dropped_state;
    c.send(buf, enc(bye, buf), kChannelReliable);  // a repeated Bye
    c.idle(100);
    expect(core.counters().dropped_state == state_before_bye2 + 1 && queue_empty(core),
           "a repeated Bye is dropped as wrong phase and not queued");
    expect(core.disconnect(0, gen0), "disconnect queued");
    for (int i = 0; i < 2000 && !c.disconnected; ++i) c.idle(1);
    ev = collect(core, c, 1, 2000);
    expect(c.disconnected && ev.size() == 1 && ev[0].kind == HostEvent::Disconnect && ev[0].slot == 0,
           "disconnect: the client sees it, and the game thread gets a Disconnect event");

    // A command for a connection that is gone must not reach the client that took its slot.
    TestClient b;
    b.host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    b.peer = enet_host_connect(b.host, &addr, kChannelCount, 0);
    ev = collect(core, b, 1, 2000);
    expect(ev.size() == 1 && ev[0].kind == HostEvent::Connect && ev[0].slot == 0 && ev[0].gen > gen0,
           "second client takes slot 0 with a new generation");
    const uint32_t gen_b = ev.empty() ? 0 : ev[0].gen;
    ByeMsg stale{};
    stale.client_id = 1;
    stale.reason = 9;
    core.send(0, gen0, buf, enc(stale, buf), kChannelReliable, true);  // meant for the first client
    core.disconnect(0, gen0);
    b.idle(300);
    expect(!b.disconnected && b.got.empty(), "stale-generation Bye and disconnect do not reach the new client");
    expect(core.send(0, gen_b, buf, enc(stale, buf), kChannelReliable, true), "a current-generation Bye is queued");
    b.idle(300);
    expect(!b.disconnected && b.got.size() == 1, "the current-generation Bye is delivered");

    // Control events survive a flood of States while the game does not drain the queue.
    b.send(buf, enc(h, buf), kChannelReliable);
    core.set_welcomed(0, gen_b, true);
    b.idle(100);
    const HostCounters pre_flood = core.counters();
    uint32_t sent = 0;
    for (int round = 0; round < 40; ++round) {  // 40 x 20 = 800 States, more than the 512-entry ring holds
        for (int k = 0; k < 20; ++k) {
            StateMsg fs = good_state(++sent, 100.5);
            fs.client_id = 1;
            b.send(buf, enc(fs, buf), kChannelUnreliable);
        }
        b.idle(5);
    }
    b.send(buf, enc(bye, buf), kChannelReliable);
    b.idle(100);
    enet_peer_disconnect(b.peer, 0);
    b.idle(300);
    std::vector<HostEvent> all;
    for (;;) {  // the game thread wakes up and drains everything
        HostEvent e3[64];
        const size_t n = core.poll(e3, 64);
        if (n == 0) break;
        all.insert(all.end(), e3, e3 + n);
    }
    size_t states = 0, byes = 0, hellos = 0, discs = 0;
    for (const HostEvent& e : all) {
        if (e.kind == HostEvent::Disconnect) ++discs;
        if (e.kind != HostEvent::Message) continue;
        MsgType t{};
        peek_type(e.data, e.len, t);
        if (t == MsgType::State) ++states;
        if (t == MsgType::Bye) ++byes;
        if (t == MsgType::Hello) ++hellos;
    }
    const HostCounters post_flood = core.counters();
    std::printf("  flood: %u sent, %zu States queued, %u refused; Hello %zu, Bye %zu, Disconnect %zu\n", sent, states,
                post_flood.dropped_queue - pre_flood.dropped_queue, hellos, byes, discs);
    const size_t flood_quota = (512 - 64) / kMaxClients;
    const uint32_t flood_refused = post_flood.dropped_queue - pre_flood.dropped_queue;
    // The quota is exact; how many were refused depends on how many States ENet delivered.
    expect(states == flood_quota && flood_refused >= 1 && flood_refused <= sent - flood_quota,
           "one client's States are held to its quota");
    expect(hellos == 1 && byes == 1 && discs == 1 && post_flood.dropped_control == 0,
           "after the flood: Hello, Bye and Disconnect all arrive, no control event lost");

    // One client's flood cannot crowd out another client's join: Client A floods States and repeats Byes while the game
    // does not drain; client B then connects, sends its Hello and leaves. B's Connect, Hello and Disconnect all arrive,
    // B is not kicked, and A's States are held to its quota.
    {
        constexpr size_t kQuota = (512 - 64) / kMaxClients;  // HostCore's per-slot State quota
        TestClient x;
        x.host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
        x.peer = enet_host_connect(x.host, &addr, kChannelCount, 0);
        ev = collect(core, x, 1, 2000);
        expect(ev.size() == 1 && ev[0].kind == HostEvent::Connect, "flooding client A connects");
        const uint8_t xslot = ev.empty() ? 0 : ev[0].slot;
        core.set_welcomed(xslot, ev.empty() ? 0 : ev[0].gen, true);
        ByeMsg xb{};
        xb.client_id = static_cast<uint16_t>(xslot + 1);
        const HostCounters pre = core.counters();
        uint32_t xs = 100000;
        for (int round = 0; round < 40; ++round) {  // 800 States
            for (int k = 0; k < 20; ++k) {
                StateMsg fs = good_state(++xs, 100.5);
                fs.client_id = xb.client_id;
                x.send(buf, enc(fs, buf), kChannelUnreliable);
            }
            x.idle(5);
        }
        for (int k = 0; k < 500; ++k) {  // and 500 Byes on the reliable channel
            x.send(buf, enc(xb, buf), kChannelReliable);
            if (k % 25 == 0) x.idle(2);
        }
        x.idle(200);
        const HostCounters mid = core.counters();

        TestClient y;
        y.host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
        y.peer = enet_host_connect(y.host, &addr, kChannelCount, 0);
        for (int i = 0; i < 2000 && !y.connected; ++i) {
            x.service(0);
            y.service(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        y.send(buf, enc(h, buf), kChannelReliable);  // B's Hello
        for (int i = 0; i < 200; ++i) {
            x.service(0);
            y.service(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool y_kicked = y.disconnected;
        enet_peer_disconnect(y.peer, 0);
        enet_peer_disconnect(x.peer, 0);
        for (int i = 0; i < 300; ++i) {
            x.service(0);
            y.service(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        size_t connects = 0, disconnects = 0, a_states = 0, bye_count = 0, hello_count = 0;
        for (;;) {
            HostEvent e4[64];
            const size_t n = core.poll(e4, 64);
            if (n == 0) break;
            for (size_t i = 0; i < n; ++i) {
                if (e4[i].kind == HostEvent::Connect) ++connects;
                if (e4[i].kind == HostEvent::Disconnect) ++disconnects;
                if (e4[i].kind != HostEvent::Message) continue;
                MsgType t{};
                peek_type(e4[i].data, e4[i].len, t);
                if (t == MsgType::State && e4[i].slot == xslot) ++a_states;
                if (t == MsgType::Bye) ++bye_count;
                if (t == MsgType::Hello) ++hello_count;
            }
        }
        const HostCounters post = core.counters();
        std::printf("  flood: A's States queued %zu (quota %zu), refused over quota %u; Bye %zu; B: Connect %zu, "
                    "Hello %zu, kicked %d; Disconnects %zu; wrong-phase drops %u; control lost %u\n",
                    a_states, kQuota, mid.dropped_quota[xslot] - pre.dropped_quota[xslot], bye_count, connects, hello_count,
                    y_kicked ? 1 : 0, disconnects, mid.dropped_state - pre.dropped_state, post.dropped_control);
        const size_t a_refused = mid.dropped_quota[xslot] - pre.dropped_quota[xslot];
        expect(a_states == kQuota && a_refused >= 1 && a_refused <= 800 - kQuota,
               "A's States are held to its quota, and the rest are counted as refused for A's slot");
        expect(connects == 1 && hello_count == 1 && !y_kicked, "B's Connect and Hello arrive and B is not kicked");
        expect(bye_count == 1 && mid.dropped_state - pre.dropped_state == 499 && disconnects == 2 && post.dropped_control == 0,
               "one Bye queued, 499 repeats dropped, both Disconnects arrive, no control event lost");
    }

    // Silence: a client that is serviced (so ENet itself is happy) but sends no application packets is dropped by
    // the core after 5 s, with reason 1.
    TestClient d;
    d.host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    d.peer = enet_host_connect(d.host, &addr, kChannelCount, 0);
    ev = collect(core, d, 1, 2000);
    expect(ev.size() == 1 && ev[0].kind == HostEvent::Connect, "third client connects (slot reused)");
    std::printf("  waiting for the 5 s silence timeout...\n");
    const auto t0 = std::chrono::steady_clock::now();  // timed from the Connect event collected just above
    std::vector<HostEvent> gone;
    for (int i = 0; i < 14000 && gone.empty(); ++i) {
        d.service(0);
        HostEvent e2[4];
        const size_t n = core.poll(e2, 4);
        gone.insert(gone.end(), e2, e2 + n);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  Disconnect after %.1f s, reason %d\n", secs, gone.empty() ? -1 : gone[0].reason);
    expect(!gone.empty() && gone[0].kind == HostEvent::Disconnect && gone[0].reason == 1 && secs >= 4.5 && secs < 12.0,
           "a silent client times out after about 5 s, by the core's timer (reason 1)");

    core.stop(false);
    expect(core.state() == HostCore::State::Idle, "stopped cleanly");
}

}  // namespace

// --- The synthetic client (tools/bot) -------------------------------------------------------------

namespace bot = lazysf2::bot;

void bot_flight_tests() {
    std::printf("bot flight scripts\n");
    const double spawn[3] = {500.0, -300.0, 1200.0};
    const double g = bot::kG, V = bot::kSpeed;

    {  // straight: nothing changes but the position
        bot::Flight f;
        f.start(bot::Script::Straight, spawn, 3.1, 0.0);
        f.advance_to(10.0);
        const auto s = f.sample();
        const double dist = std::hypot(s.pos[0] - spawn[0], s.pos[1] - spawn[1]);
        expect(std::fabs(dist - 2200.0) < 1e-6 && s.pos[2] == spawn[2] && s.ang[2] == 0.0f && s.rates[1] == 0.0f,
               "straight: 2200 m in 10 s, level, no turn");
        expect(std::fabs(s.vel[0] - static_cast<float>(V * std::sin(3.1))) < 1e-3 && s.vel[1] < 0,
               "straight: velocity follows the heading (3.1 rad is almost due south)");
    }
    {  // orbit: constant 3 g turn, right
        bot::Flight f;
        f.start(bot::Script::Orbit, spawn, 3.1, 0.0);
        f.advance_to(1.0);
        auto s = f.sample();
        const double bank = std::acos(1.0 / 3.0), w = g * std::tan(bank) / V, R = V / w;
        expect(std::fabs(s.ang[2] - bank) < 1e-5 && std::fabs(s.rates[1] - w) < 1e-5 && s.rates[1] > 0,
               "orbit: bank acos(1/3), turn rate g*tan(bank)/V, right turn positive");
        const double h = s.ang[1];
        const double cx = s.pos[0] + R * std::cos(h), cy = s.pos[1] - R * std::sin(h);  // centre is to the right
        double worst = 0, max_ang = 0;
        for (double t = 1.0; t < 40.0; t += 0.0137) {
            f.advance_to(t);
            s = f.sample();
            worst = std::fmax(worst, std::fabs(std::hypot(s.pos[0] - cx, s.pos[1] - cy) - R));
            max_ang = std::fmax(max_ang, std::fabs(s.ang[1]));
            if (std::fabs(std::hypot(s.vel[0], s.vel[1]) - V) > 1e-3) worst = 1e9;
        }
        std::printf("    radius %.0f m, worst deviation from the circle over 39 s: %.4f m\n", R, worst);
        expect(worst < 0.5, "orbit: the path is a circle of radius V/w, and the speed stays 220 m/s");
        expect(max_ang <= bot::kPi, "orbit: heading stays in (-pi, pi] across the wrap");
    }
    {  // breaks: alternating direction every 8 s, roll at 180 deg/s
        bot::Flight f;
        f.start(bot::Script::Breaks, spawn, 0.0, 0.0);
        const double bank6 = std::acos(1.0 / 6.0);
        bool alt = true, rate_ok = true;
        double max_roll_rate = 0, prev_hdg = 0;
        double seg_dh[5] = {};
        for (double t = 0.01; t <= 40.0; t += 0.01) {
            f.advance_to(t);
            const auto s = f.sample();
            max_roll_rate = std::fmax(max_roll_rate, std::fabs(static_cast<double>(s.rates[2])));
            rate_ok = rate_ok && std::fabs(s.rates[1] - g * std::tan(s.ang[2]) / V) < 1e-4;
            double dh = s.ang[1] - prev_hdg;
            if (dh > bot::kPi) dh -= 2 * bot::kPi;
            if (dh < -bot::kPi) dh += 2 * bot::kPi;
            prev_hdg = s.ang[1];
            const int seg = static_cast<int>(std::floor((t - 0.005) / bot::kBreakPeriod));
            if (seg >= 0 && seg < 5) seg_dh[seg] += dh;
            const double ph = std::fmod(t, bot::kBreakPeriod);
            if (ph > 3.9 && ph < 4.1) {
                const double want = (seg % 2 == 0 ? 1.0 : -1.0) * bank6;
                if (std::fabs(s.ang[2] - want) > 1e-4) alt = false;
            }
        }
        std::printf("    heading change per 8 s segment (rad): %.2f %.2f %.2f %.2f %.2f, max roll rate %.3f rad/s\n",
                    seg_dh[0], seg_dh[1], seg_dh[2], seg_dh[3], seg_dh[4], max_roll_rate);
        expect(alt, "breaks: bank is +acos(1/6), -acos(1/6), +, -, + in successive 8 s segments");
        expect(seg_dh[0] > 1.5 && seg_dh[1] < -1.5 && seg_dh[2] > 1.5 && seg_dh[3] < -1.5 && seg_dh[4] > 1.5,
               "breaks: the heading turns right, left, right, left, right");
        expect(max_roll_rate <= bot::kRollRate + 1e-3 && max_roll_rate > 0.9 * bot::kRollRate,
               "breaks: roll rate is limited to 180 deg/s, and reaches it");
        expect(rate_ok, "breaks: the heading rate is g*tan(bank)/V all the time (coordinated)");
    }
    {  // climb: 30 deg for 10 s, then level
        bot::Flight f;
        f.start(bot::Script::Climb, spawn, 0.0, 5.0);
        f.advance_to(5.0 + 5.0);
        auto s = f.sample();
        expect(std::fabs(s.ang[0] - bot::kClimbPitch) < 1e-5 && std::fabs(s.vel[2] - V * std::sin(bot::kClimbPitch)) < 1e-3,
               "climb: 30 degrees nose-up, vz = V sin(30 deg)");
        f.advance_to(5.0 + 12.0);
        s = f.sample();
        const double gain = s.pos[2] - spawn[2];
        std::printf("    altitude gained %.0f m\n", gain);
        expect(std::fabs(s.ang[0]) < 1e-6 && std::fabs(s.vel[2]) < 1e-3 && gain > 900 && gain < 1200,
               "climb: level again after 10 s, having gained about 1000 m");
    }
}

void bot_clock_tests() {
    std::printf("bot clock sync\n");
    {
        bot::ClockSync c;
        expect(!c.on_pong(1.0, 1.1, 50.0) && c.pongs() == 0, "a Pong before the Welcome is ignored");
        c.on_welcome(10.0, 100.0, 0.01);
        expect(std::fabs(c.now(10.0) - 100.01) < 1e-9, "the Welcome seeds the offset (host time + half the RTT)");
        expect(!c.on_pong(10.0, 9.0, 100.0), "a Pong that arrives before it was sent is ignored");
    }
    {  // windowed lowest RTT
        bot::ClockSync c;
        c.on_welcome(10.0, 100.0, 0.0);
        double wall = 10.0;
        auto pong = [&](double rtt, double off_error) {  // the host time is the true one plus an RTT-asymmetry error
            wall += 0.25;
            c.on_pong(wall - rtt, wall, (wall - 0.5 * rtt) + 90.0 + off_error);
        };
        pong(0.050, 0.020);
        pong(0.004, 0.001);  // the best sample
        for (int i = 0; i < 10; ++i) pong(0.040, 0.015);
        expect(std::fabs(c.best_rtt() - 0.004) < 1e-9 && std::fabs(c.offset() - (90.0 + 0.001)) < 1e-6,
               "the offset comes from the lowest-RTT sample in the window");
        for (int i = 0; i < 16; ++i) pong(0.040, 0.015);
        expect(std::fabs(c.best_rtt() - 0.040) < 1e-9 && std::fabs(c.offset() - (90.0 + 0.015)) < 1e-6,
               "the lucky sample leaves the window after 16 Pongs, it does not stick");
    }
    {  // freeze and resume
        bot::ClockSync c;
        c.on_welcome(10.0, 100.0, 0.0);
        double wall = 10.0, host = 100.0;
        auto pong = [&](bool advance) {
            wall += 0.25;
            if (advance) host += 0.25;
            c.on_pong(wall - 0.002, wall, host);
        };
        pong(true);
        pong(true);
        expect(!c.frozen() && c.resets() == 0, "a running host is not frozen");
        const double before = host;
        pong(false);
        expect(c.frozen() && c.on_hold(wall), "a Pong with no host advance while wall time passed freezes the clock");
        expect(std::fabs(c.now(wall + 5.0) - before) < 1e-9, "the frozen clock does not extrapolate");
        pong(false);
        pong(false);
        expect(c.frozen(), "it stays frozen");
        pong(true);
        expect(!c.frozen() && c.resets() == 1, "host time advancing again resumes it, with the window cleared");
        expect(std::fabs(c.now(wall) - host) < 0.01, "after the resume the clock matches the host again");
    }
    {  // a pause shorter than a ping interval shows up as an offset jump: reset, no hold needed
        bot::ClockSync c;
        c.on_welcome(10.0, 100.0, 0.0);
        double wall = 10.0;
        for (int i = 0; i < 4; ++i) {
            wall += 0.25;
            c.on_pong(wall - 0.002, wall, wall - 0.001 + 90.0);
        }
        wall += 0.25;
        c.on_pong(wall - 0.002, wall, wall - 0.001 + 90.0 - 0.8);  // the host lost 0.8 s
        expect(c.resets() == 1 && std::fabs(c.offset() - (90.0 - 0.8)) < 1e-6, "an offset jump resets the window");
    }
    {  // silence: no Pong for 3 s holds, too
        bot::ClockSync c;
        c.on_welcome(10.0, 100.0, 0.0);
        c.on_pong(10.25 - 0.002, 10.25, 100.25);
        expect(!c.on_hold(12.0) && c.on_hold(13.5), "no Pong for 3 s puts the sender on hold");
    }
    {  // silence from the start: no Pong ever, the Welcome's clock goes stale, too
        bot::ClockSync c;
        c.on_welcome(10.0, 100.0, 0.0);
        expect(!c.on_hold(12.0) && c.on_hold(13.5), "no Pong since the Welcome for 3 s puts the sender on hold");
    }
}

struct BotSession {
    bot::BotResult res;
    std::vector<StateMsg> states;
    std::vector<double> host_at_rx;
    HostCounters counters;
    bool got_hello = false, got_bye = false;
    uint8_t bye_reason = 255;
};

// Runs the bot on its own thread against a HostCore, and plays the game thread: publishes a host clock, answers
// Hello with Welcome, and collects what the host would hand to the game. pause_at/pause_len freeze the host clock
// for a while, as a paused game does.
BotSession run_bot_session(bot::Script script, double rate, double duration, double pause_at, double pause_len,
                           double loss, bool drop = false, double jitter_ms = 0.0) {
    BotSession out;
    HostCore core;
    core.start("127.0.0.1", 0, [](const char* l) { std::printf("    [core] %s\n", l); });
    for (int i = 0; i < 2000 && core.state() == HostCore::State::Starting; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (core.state() != HostCore::State::Running) {
        expect(false, "host core running");
        return out;
    }
    using Clk = std::chrono::steady_clock;
    const auto start = Clk::now();
    auto secs = [&] { return std::chrono::duration<double>(Clk::now() - start).count(); };
    double host_time = 1000.0, last = 0;
    core.set_host_time(host_time);

    bot::BotConfig cfg;
    cfg.host = "127.0.0.1:" + std::to_string(core.port());
    cfg.script = script;
    cfg.rate = rate;
    cfg.duration = duration;
    cfg.loss_pct = loss;
    cfg.jitter_ms = jitter_ms;
    cfg.drop = drop;
    cfg.callsign = "BOTTEST";
    cfg.log = [](const char* l) { std::printf("    [bot] %s\n", l); };
    std::atomic<bool> done{false};
    std::thread th([&] {
        out.res = bot::run_bot(cfg);
        done = true;
    });

    const double spawn[3] = {500.0, -300.0, 1200.0};
    double finished_at = 0;
    double first_state_at = -1;  // the pause is scheduled from the first State received, not from the test start
    while (finished_at == 0 || secs() - finished_at < 0.4) {
        const double now = secs();
        const bool paused = pause_len > 0 && first_state_at >= 0 && now - first_state_at >= pause_at &&
                            now - first_state_at < pause_at + pause_len;
        if (!paused) host_time += now - last;
        last = now;
        core.set_host_time(host_time);
        HostEvent ev[16];
        const size_t n = core.poll(ev, 16);
        for (size_t i = 0; i < n; ++i) {
            const HostEvent& e = ev[i];
            if (e.kind != HostEvent::Message) continue;
            MsgType t{};
            if (peek_type(e.data, e.len, t) != Status::Ok) continue;
            if (t == MsgType::Hello) {
                out.got_hello = true;
                core.set_welcomed(e.slot, e.gen, true);
                WelcomeMsg w{};
                w.client_id = static_cast<uint16_t>(e.slot + 1);
                for (int k = 0; k < 3; ++k) w.spawn_pos[k] = spawn[k];
                w.spawn_heading = 0.5f;
                w.host_game_time = host_time;
                uint8_t b[64];
                core.send(e.slot, e.gen, b, encode(w, b, sizeof(b)), kChannelReliable, true);
            } else if (t == MsgType::State) {
                StateMsg s{};
                if (decode(e.data, e.len, s) == Status::Ok) {
                    if (first_state_at < 0) first_state_at = now;
                    out.states.push_back(s);
                    out.host_at_rx.push_back(host_time);
                }
            } else if (t == MsgType::Bye) {
                ByeMsg b{};
                if (decode(e.data, e.len, b) == Status::Ok) {
                    out.got_bye = true;
                    out.bye_reason = b.reason;
                }
            }
        }
        if (done && finished_at == 0) finished_at = secs();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    th.join();
    out.counters = core.counters();
    core.stop(false);
    return out;
}

// A session is "clean" when the bot never reset its clock and held at most the one slot it always holds at take-off
// (the first tick has no host time ahead of the flight clock yet): only then is the sent rate exact.
bool session_clean(const BotSession& r) { return r.res.held_ticks <= 1 && r.res.clock_resets == 0; }

// Runs a session that is expected to be clean, and retries it up to 3 times when a scheduling hiccup on a loaded
// machine made the bot hold or reset its clock (the result of the last attempt is what gets checked).
template <class... A>
BotSession run_clean_session(A... args) {
    BotSession r;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        r = run_bot_session(args...);
        if (session_clean(r)) break;
        std::printf("    (attempt %d held %u slots and reset the clock %d times: %s)\n", attempt, r.res.held_ticks,
                    r.res.clock_resets, attempt < 3 ? "retrying" : "giving up");
    }
    return r;
}

void check_session(const BotSession& r, double rate, double lo_seconds, double hi_seconds, const char* label) {
    char what[160];
    std::printf("  session: %s\n", label);
    std::printf("    bot: %u made, %u sent, %u lost, %u held slots, %u Pongs, %d clock resets; host got %zu States\n",
                r.res.generated, r.res.sent, r.res.lost, r.res.held_ticks, r.res.pongs, r.res.clock_resets, r.states.size());
    expect(r.got_hello && r.res.code == 0 && r.res.client_id == 1, "Hello sent, Welcome received, bot exited 0");
    // Loopback loses nothing: what the bot handed to ENet is what the host got.
    std::snprintf(what, sizeof(what), "every State the bot sent arrived: sent %u, got %zu", r.res.sent, r.states.size());
    expect(r.states.size() == r.res.sent, what);
    std::snprintf(what, sizeof(what), "States arrive at the requested rate: %.0f..%.0f expected, got %zu", rate * lo_seconds,
                  rate * hi_seconds, r.states.size());
    expect(static_cast<double>(r.states.size()) >= rate * lo_seconds &&
               static_cast<double>(r.states.size()) <= rate * hi_seconds + 1,
           what);
    expect(r.counters.dropped_range == 0 && r.counters.dropped_format == 0 && r.counters.dropped_state == 0 &&
               r.counters.dropped_queue == 0,
           "the host dropped nothing: zero range, format, phase or queue drops");
    // Between consecutive States the position change must match the mean velocity times dt.
    double worst = 0, worst_dt = 0, max_ahead = -1e9;
    bool mono = true, headings_ok = true;
    for (size_t i = 0; i < r.states.size(); ++i) {
        const StateMsg& s = r.states[i];
        max_ahead = std::fmax(max_ahead, s.game_time - r.host_at_rx[i]);
        if (std::fabs(static_cast<double>(s.ang[1])) > bot::kPi) headings_ok = false;
        if (i == 0) continue;
        const StateMsg& p = r.states[i - 1];
        const double dt = s.game_time - p.game_time;
        if (!(dt > 0)) mono = false;
        if (dt > 0.25) continue;  // across a hold the path is still exact, but the trapezoid is coarser
        for (int k = 0; k < 3; ++k) {
            const double e = std::fabs((s.pos[k] - p.pos[k]) - 0.5 * (static_cast<double>(s.vel[k]) + p.vel[k]) * dt);
            if (e > worst) {
                worst = e;
                worst_dt = dt;
            }
        }
    }
    std::printf("    worst |dpos - mean(vel)*dt| %.4f m (dt %.3f s); no State was more than %.3f s ahead of the host clock\n",
                worst, worst_dt, max_ahead);
    expect(worst < 0.05, "consecutive States are consistent: |dpos - vel*dt| < 5 cm");
    expect(mono, "State game times strictly increase");
    expect(headings_ok, "headings stay in (-pi, pi]");
    expect(max_ahead < 0.6, "no State is stamped far beyond the host's game time");
}

void bot_loopback_tests() {
    bot_flight_tests();
    bot_clock_tests();
    {  // warm-up: the first session after a build pays for cold caches and thread start-up; it is not measured
        const BotSession w = run_bot_session(bot::Script::Straight, 20.0, 0.3, 0, 0, 0);
        std::printf("  warm-up session: %zu States\n", w.states.size());
    }
    std::printf("bot against the host core (loopback)\n");
    {
        const BotSession r = run_clean_session(bot::Script::Breaks, 50.0, 3.0, 0.0, 0.0, 0.0);
        check_session(r, 50.0, 2.7, 3.2, "breaks, 50 Hz, 3 s");
        expect(r.got_bye && r.bye_reason == 0, "the host got Bye");
        bool coordinated = !r.states.empty();
        for (const StateMsg& s : r.states)
            if (s.ang[2] * s.rates[1] < 0) coordinated = false;
        expect(coordinated && !r.states.empty() && r.states.back().ang[2] > 1.0f && r.states.back().rates[1] > 0.2f,
               "received stream: roll right-wing-down with a positive heading rate in the first (right) break");
    }
    {
        // The host clock freezes for 1 s, 1.5 s after the first State arrived. The bot notices within a ping interval, holds, and after the resume
        // waits until the host catches up with what it already sent. About 4 s - 1 s - 0.7 s of flying at 30 Hz, less 10% loss.
        const BotSession r = run_bot_session(bot::Script::Orbit, 30.0, 4.0, 1.5, 1.0, 10.0);
        check_session(r, 30.0, 1.0, 3.1, "orbit, 30 Hz, 10% loss, host paused for 1 s");
        expect(r.res.lost > 0 && r.res.held_ticks > 0 && r.res.clock_resets >= 1,
               "the bot lost some States on purpose, held during the pause, and re-synced after it");
    }
    {
        const BotSession r = run_clean_session(bot::Script::Straight, 20.0, 1.0, 0.0, 0.0, 0.0, true);
        check_session(r, 20.0, 0.8, 1.2, "straight, 20 Hz, --drop");
        expect(!r.got_bye, "with --drop the bot leaves without a Bye");
    }
    {
        // Jitter holds States back; every one must have been handed to ENet before the Bye (none left in the queue).
        const BotSession r = run_bot_session(bot::Script::Straight, 20.0, 1.0, 0, 0, 0, false, 400.0);
        expect(r.got_bye && r.res.generated > 0 && r.res.sent == r.res.generated - r.res.lost,
               "jitter: the delayed States are flushed before the Bye");
    }
    {  // argument checks that return before any network use
        auto code = [](double rate, double dur, double loss, double jit, const char* host) {
            bot::BotConfig c;
            c.rate = rate;
            c.duration = dur;
            c.loss_pct = loss;
            c.jitter_ms = jit;
            c.host = host;
            return bot::run_bot(c).code;
        };
        const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        expect(code(30, 5, 0, 0, "127.0.0.1:7777abc") == 1 && code(30, 5, 0, 0, "127.0.0.1:") == 1 &&
                   code(nan, 5, 0, 0, "127.0.0.1:7777") == 1 && code(30, inf, 0, 0, "127.0.0.1:7777") == 1 &&
                   code(30, 5, nan, 0, "127.0.0.1:7777") == 1 && code(30, 5, 0, inf, "127.0.0.1:7777") == 1,
               "the bot rejects non-finite numbers and a port with trailing characters");
    }
    {
        const BotSession r = run_clean_session(bot::Script::Climb, 20.0, 1.0, 0.0, 0.0, 0.0);
        check_session(r, 20.0, 0.8, 1.2, "climb, 20 Hz, 1 s");
        expect(r.got_bye && !r.states.empty() && r.states.back().ang[0] > 0.3f && r.states.back().vel[2] > 50.0f,
               "climb: the stream pitches up and climbs");
    }
}

// --- Stream recording -------------------------------------------------------------------------------

void stream_log(const char* line) { std::printf("    log: %s\n", line); }

std::vector<uint8_t> read_file(const std::wstring& path) {
    std::vector<uint8_t> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return out;
    uint8_t tmp[4096];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) out.insert(out.end(), tmp, tmp + n);
    std::fclose(f);
    return out;
}

std::wstring make_temp_dir() {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    for (unsigned n = GetTickCount() ^ GetCurrentProcessId();; ++n) {
        wchar_t name[MAX_PATH];
        swprintf_s(name, L"%slzst_test_%08x", base, n);
        if (CreateDirectoryW(name, nullptr)) return name;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return L"";
    }
}

void remove_dir(const std::wstring& dir) {
    for (unsigned n = 1; n < 20; ++n) {
        wchar_t f[16];
        swprintf_s(f, L"stream_%03u.lzst", n);
        DeleteFileW((dir + L"\\" + f).c_str());
    }
    RemoveDirectoryW(dir.c_str());
}

StateMsg make_state(uint16_t id, uint32_t seq, double t) {
    StateMsg s{};
    s.client_id = id;
    s.seq = seq;
    s.game_time = t;
    s.pos[0] = 100.0 + seq;
    s.pos[2] = 1500.0;
    s.vel[1] = 200.0f;
    s.ang[1] = 0.5f;
    return s;
}

void stream_tests() {
    std::printf("stream format and writer\n");
    const std::wstring dir = make_temp_dir();
    expect(!dir.empty(), "temp dir");
    if (dir.empty()) return;

    // Write through the writer: every kind, with timestamps.
    auto w = std::make_unique<StreamWriter>();
    expect(w->open(dir.c_str(), 12.5, "Europe", stream_log), "open");
    expect(w->is_open(), "is_open");
    uint8_t buf[96];
    ClientJoinedPayload jp{};
    jp.client_id = 1;
    set_str(jp.callsign, "Bot");
    set_str(jp.aircraft, "F-4E");
    w->record(StreamKind::ClientJoined, 12.5, &jp, sizeof(jp));
    WelcomeMsg wm{};
    wm.client_id = 1;
    wm.host_game_time = 12.5;
    w->record(StreamKind::WelcomeSent, 12.5, buf, encode(wm, buf, sizeof(buf)));
    const int kStates = 3000;  // more than a flush buffer's worth, so the writer flushes more than once
    for (int i = 1; i <= kStates; ++i) {
        const StateMsg hs = make_state(0, static_cast<uint32_t>(i), 13.0 + 0.01 * i);
        w->record(StreamKind::HostState, 13.0 + 0.01 * i, buf, encode(hs, buf, sizeof(buf)));
        const StateMsg cs = make_state(1, static_cast<uint32_t>(i), 13.0 + 0.01 * i - 0.05);
        w->record(StreamKind::StateReceived, 13.0 + 0.01 * i, buf, encode(cs, buf, sizeof(buf)));
        if (i % 500 == 0) Sleep(30);  // let the writer keep up: nothing may be dropped
    }
    ByeMsg bm{};
    bm.client_id = 1;
    w->record(StreamKind::ByeReceived, 99.0, buf, encode(bm, buf, sizeof(buf)));
    ClientLeftPayload lp{1, static_cast<uint8_t>(LeaveReason::Left)};
    w->record(StreamKind::ClientLeft, 99.0, &lp, sizeof(lp));
    expect(w->dropped() == 0, "no records dropped");
    w->close(false);
    w.reset();  // the destructor waits for the writer thread (up to 1 s) and detaches it

    const std::vector<uint8_t> file = read_file(dir + L"\\stream_001.lzst");
    expect(file.size() > sizeof(StreamHeader), "stream_001.lzst written");

    StreamReader r(file.data(), file.size());
    StreamHeader h{};
    expect(r.read_header(h) == StreamStatus::Ok, "header parses");
    expect(std::memcmp(h.magic, "LZST", 4) == 0 && h.version == 1 && h.header_len == 48, "magic, version, header length");
    expect(h.start_game_time == 12.5 && std::strcmp(h.theatre, "Europe") == 0, "start time and theatre round-trip");
    int counts[8] = {};
    int bad = 0;
    bool times_ok = true, states_ok = true;
    double last_time = 0;
    StreamRecord rec;
    StreamStatus st;
    while ((st = r.next(rec)) == StreamStatus::Ok) {
        if (rec.kind < 8) ++counts[rec.kind];
        if (rec.time < last_time) times_ok = false;  // written in host-time order
        last_time = rec.time;
        if (rec.kind == 1 || rec.kind == 2) {
            StateMsg s{};
            if (decode(rec.payload, rec.payload_len, s) != Status::Ok) {
                ++bad;
            } else if (s.client_id != (rec.kind == 1 ? 1 : 0) || s.pos[0] != 100.0 + s.seq ||
                       std::fabs(rec.time - (13.0 + 0.01 * s.seq)) > 1e-9) {
                states_ok = false;
            }
        } else if (rec.kind == 3) {
            WelcomeMsg m{};
            if (decode(rec.payload, rec.payload_len, m) != Status::Ok || m.host_game_time != 12.5) ++bad;
        } else if (rec.kind == 4) {
            ByeMsg m{};
            if (decode(rec.payload, rec.payload_len, m) != Status::Ok || m.client_id != 1 || rec.time != 99.0) ++bad;
        } else if (rec.kind == 5) {
            ClientJoinedPayload p;
            if (rec.payload_len != sizeof(p)) {
                ++bad;
            } else {
                std::memcpy(&p, rec.payload, sizeof(p));
                if (p.client_id != 1 || std::strcmp(p.callsign, "Bot") != 0 || std::strcmp(p.aircraft, "F-4E") != 0) ++bad;
            }
        } else if (rec.kind == 6) {
            ClientLeftPayload p;
            if (rec.payload_len != sizeof(p)) {
                ++bad;
            } else {
                std::memcpy(&p, rec.payload, sizeof(p));
                if (p.client_id != 1 || p.reason != 0 || rec.time != 99.0) ++bad;
            }
        }
    }
    expect(st == StreamStatus::End, "the file ends at a record boundary");
    expect(counts[1] == kStates && counts[2] == kStates && counts[3] == 1 && counts[4] == 1 && counts[5] == 1 &&
               counts[6] == 1,
           "every kind round-trips with its count");
    expect(bad == 0 && states_ok, "payloads and timestamps round-trip");
    expect(times_ok, "timestamps are non-decreasing");

    // The next stream gets the next number, and a size cap stops it cleanly.
    {
        auto w2 = std::make_unique<StreamWriter>();
        expect(w2->open(dir.c_str(), 1.0, nullptr, stream_log, 1000), "second open (1000 byte cap)");
        for (int i = 1; i <= 100; ++i) {
            const StateMsg s = make_state(1, static_cast<uint32_t>(i), i);
            w2->record(StreamKind::StateReceived, i, buf, encode(s, buf, sizeof(buf)));
            if (i % 10 == 0) Sleep(30);
        }
        w2->close(false);
        w2.reset();
        const std::vector<uint8_t> f2 = read_file(dir + L"\\stream_002.lzst");
        expect(!f2.empty() && f2.size() <= 1000, "the cap holds");
        StreamReader r2(f2.data(), f2.size());
        StreamHeader h2{};
        int n = 0;
        StreamRecord rc;
        const bool ok2 = r2.read_header(h2) == StreamStatus::Ok && h2.theatre[0] == '\0';
        while (r2.next(rc) == StreamStatus::Ok) ++n;
        expect(ok2 && n > 0 && n < 100, "a capped file is still a valid stream with a prefix of the records");
    }

    // Reader defences, on the good file.
    {
        // Every truncation point near the start: the heap copy is exactly the prefix, so an over-read would be
        // a crash or garbage under a debugger; here we check that the statuses are right and only whole records come out.
        bool prefix_ok = true;
        for (size_t cut = 0; cut < 400 && cut < file.size(); ++cut) {
            std::vector<uint8_t> part(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(cut));
            StreamReader tr(part.data(), part.size());
            StreamHeader th{};
            const StreamStatus hs = tr.read_header(th);
            if (cut < sizeof(StreamHeader)) {
                if (hs != StreamStatus::Truncated) prefix_ok = false;
                continue;
            }
            if (hs != StreamStatus::Ok) {
                prefix_ok = false;
                continue;
            }
            StreamRecord tc;
            StreamStatus ts;
            while ((ts = tr.next(tc)) == StreamStatus::Ok) {
            }
            if (ts != StreamStatus::End && ts != StreamStatus::Truncated) prefix_ok = false;
        }
        expect(prefix_ok, "every truncation ends as Truncated or End");
    }
    {
        StreamHeader hh{};
        std::vector<uint8_t> bad_magic = file;
        bad_magic[0] = 'X';
        StreamReader a(bad_magic.data(), bad_magic.size());
        expect(a.read_header(hh) == StreamStatus::BadMagic, "bad magic rejected");
        std::vector<uint8_t> bad_ver = file;
        bad_ver[4] = 2;
        StreamReader b(bad_ver.data(), bad_ver.size());
        expect(b.read_header(hh) == StreamStatus::BadVersion, "bad version rejected");
        std::vector<uint8_t> bad_hlen = file;
        bad_hlen[6] = 8;
        StreamReader c(bad_hlen.data(), bad_hlen.size());
        expect(c.read_header(hh) == StreamStatus::BadHeader, "short header_len rejected");
        std::vector<uint8_t> big_hlen(file.begin(), file.begin() + 100);
        big_hlen[6] = 0xff;
        big_hlen[7] = 0xff;
        StreamReader d(big_hlen.data(), big_hlen.size());  // 0xffff claimed, 100 bytes present
        expect(d.read_header(hh) == StreamStatus::Truncated, "header_len beyond the data is Truncated, not an over-read");

        // A record length that is too small, too large or wild stops the reader, and it stays stopped.
        const uint32_t lens[] = {0, 8, kStreamRecordMax + 1, 0x7fffffffu, 0xffffffffu};
        bool all_bad = true, sticky = true;
        for (const uint32_t len : lens) {
            std::vector<uint8_t> f = file;
            std::memcpy(&f[sizeof(StreamHeader)], &len, 4);
            StreamReader e(f.data(), f.size());
            StreamHeader eh{};
            StreamRecord er;
            if (e.read_header(eh) != StreamStatus::Ok || e.next(er) != StreamStatus::BadLength) all_bad = false;
            if (e.next(er) != StreamStatus::BadLength) sticky = false;
        }
        expect(all_bad, "record lengths below 9 and above 105 are rejected");
        expect(sticky, "a failed reader keeps failing");
        // A valid length that claims more than the file holds.
        std::vector<uint8_t> f(file.begin(), file.begin() + sizeof(StreamHeader) + 4 + 20);
        const uint32_t claim = 100;
        std::memcpy(&f[sizeof(StreamHeader)], &claim, 4);
        StreamReader g(f.data(), f.size());
        StreamHeader gh{};
        StreamRecord gr;
        expect(g.read_header(gh) == StreamStatus::Ok && g.next(gr) == StreamStatus::Truncated,
               "a record longer than the data is Truncated");
        StreamReader hdrless(file.data(), file.size());
        expect(hdrless.next(gr) == StreamStatus::BadHeader, "next() before read_header() fails");
        StreamReader empty(nullptr, 0);
        expect(empty.read_header(gh) == StreamStatus::Truncated, "an empty file is Truncated");
        // Unknown kinds are passed through, for forward compatibility.
        uint8_t rbuf[kStreamRecordTotalMax];
        std::vector<uint8_t> u(file.begin(), file.begin() + sizeof(StreamHeader));
        const uint8_t pl[2] = {1, 2};
        const size_t rn = encode_stream_record(static_cast<StreamKind>(200), 5.0, pl, 2, rbuf, sizeof(rbuf));
        u.insert(u.end(), rbuf, rbuf + rn);
        StreamReader ur(u.data(), u.size());
        expect(ur.read_header(gh) == StreamStatus::Ok && ur.next(gr) == StreamStatus::Ok && gr.kind == 200 &&
                   gr.payload_len == 2 && ur.next(gr) == StreamStatus::End,
               "an unknown kind is returned, not an error");
        uint8_t big[kStreamMaxPayload + 1] = {};
        expect(encode_stream_record(StreamKind::HostState, 0, big, sizeof(big), rbuf, sizeof(rbuf)) == 0,
               "an oversized payload is refused by the encoder");
    }

    // A file that cannot be created (the folder is missing) fails cleanly.
    {
        StreamWriter wbad;
        expect(!wbad.open((dir + L"\\missing").c_str(), 0, nullptr, stream_log), "open in a missing folder fails");
        wbad.record(StreamKind::HostState, 0, buf, 10);  // a no-op, not a crash
        expect(!wbad.is_open(), "and stays closed");
    }
    // A full ring drops and counts instead of blocking.
    {
        auto w3 = std::make_unique<StreamWriter>();
        expect(w3->open(dir.c_str(), 0, nullptr, stream_log), "third open");
        const StateMsg s = make_state(0, 1, 1);
        const size_t n = encode(s, buf, sizeof(buf));
        for (int i = 0; i < 200000; ++i) w3->record(StreamKind::HostState, i, buf, n);
        w3->close(false);
        const uint32_t dropped = w3->dropped();
        w3.reset();
        std::printf("    200000 records in a tight loop: %u dropped\n", dropped);
        const std::vector<uint8_t> f3 = read_file(dir + L"\\stream_003.lzst");
        StreamReader r3(f3.data(), f3.size());
        StreamHeader h3{};
        StreamRecord rc3;
        uint32_t n3 = 0;
        const bool ok3 = r3.read_header(h3) == StreamStatus::Ok;
        StreamStatus end3 = StreamStatus::Ok;
        while ((end3 = r3.next(rc3)) == StreamStatus::Ok) ++n3;
        expect(ok3 && end3 == StreamStatus::End && n3 + dropped == 200000, "written + dropped = recorded; the file is valid");
    }
    remove_dir(dir);
}

// ---- ACMI export ----
void acmi_tests() {
    std::printf("acmi export\n");
    std::vector<uint8_t> f(48 + 100000);
    size_t n = encode_stream_header(10.0, "", f.data(), f.size());
    auto add = [&](StreamKind k, double t, const void* p, size_t len) {
        n += encode_stream_record(k, t, static_cast<const uint8_t*>(p), len, f.data() + n, f.size() - n);
    };
    uint8_t buf[96];
    ClientJoinedPayload jp{};
    jp.client_id = 2;
    set_str(jp.callsign, "Bo,t");
    set_str(jp.aircraft, "F-4E");
    std::vector<size_t> starts;  // offset of each record in the file, for the corrupt-length test
    starts.push_back(n);
    add(StreamKind::ClientJoined, 10.0, &jp, sizeof(jp));
    // 100 Hz host samples for 1 s, flying east (heading +90 deg), pitch 0.1, roll 0.2 rad. The client's
    // game_time stamps run 50 ms before the arrival time, so the two kinds interleave out of order.
    for (int i = 0; i < 100; ++i) {
        StateMsg hs{};
        hs.client_id = 0;
        hs.game_time = 10.0 + 0.01 * i;
        hs.pos[0] = 1000.0 + i;
        hs.pos[1] = 2000.0;
        hs.pos[2] = 3000.0;
        hs.ang[0] = 0.1f;
        hs.ang[1] = 1.5707964f;
        hs.ang[2] = 0.2f;
        starts.push_back(n);
        add(StreamKind::HostState, hs.game_time, buf, encode(hs, buf, sizeof(buf)));
        StateMsg cs{};
        cs.client_id = 2;
        cs.game_time = 10.0 + 0.01 * i - 0.05;
        cs.pos[0] = -500.0;
        cs.pos[1] = 111320.0;  // 1 degree north
        cs.pos[2] = 100.0;
        cs.ang[1] = -1.5707964f;  // west: 270 deg
        if (i == 50) cs.pos[0] = std::numeric_limits<double>::quiet_NaN();
        starts.push_back(n);
        add(StreamKind::StateReceived, 10.0 + 0.01 * i, buf, encode(cs, buf, sizeof(buf)));
    }
    ClientLeftPayload lp{2, 0};
    add(StreamKind::ClientLeft, 11.0, &lp, sizeof(lp));
    const size_t full = n;
    char* dump = nullptr;
    size_t dump_len = 0;
    if (_dupenv_s(&dump, &dump_len, "LAZYSF2_ACMI_TEST_DUMP") == 0 && dump) {  // for trying lazysf2-acmi by hand
        FILE* df = nullptr;
        if (fopen_s(&df, dump, "wb") == 0 && df) {
            std::fwrite(f.data(), 1, full, df);
            std::fclose(df);
        }
        std::free(dump);
    }

    lazysf2::acmi::Options opt;
    std::string out;
    lazysf2::acmi::Info info;
    expect(lazysf2::acmi::convert_stream(f.data(), full, opt, out, info), "converts");
    expect(info.end == StreamStatus::End, "reads to the end");
    expect(out.rfind("FileType=text/acmi/tacview\nFileVersion=2.2\n0,ReferenceTime=", 0) == 0, "header and global object");
    expect(out.find("ReferenceLongitude=0,ReferenceLatitude=0") != std::string::npos, "reference point");
    auto line_of = [&](const std::string& prefix) {
        const size_t p = out.find("\n" + prefix);
        if (p == std::string::npos) return std::string();
        const size_t e = out.find('\n', p + 1);
        return out.substr(p + 1, e - p - 1);
    };
    const std::string host1 = line_of("1,T=");
    // lon = 1000 / 111320, lat = 2000 / 111320 (offsets from the reference), alt 3000, roll 11.46, pitch 5.73,
    // yaw 90 (east), U 1000, V 2000, heading 90.
    expect(host1 == "1,T=0.008983112|0.017966223|3000|11.46|5.73|90|1000|2000|90,Pilot=Host,Type=Air+FixedWing,Coalition=Allies,Color=Blue",
           "host line: transform and properties (east = 90)");
    const std::string cl = line_of("102,T=");
    expect(cl.find(",T=-0.004491556|1|100|0|0|270|-500|111320|270,") != std::string::npos &&
               cl.find("Name=F-4E,Pilot=Bo\\,t,Type=Air+FixedWing,Coalition=Enemies,Color=Red") != std::string::npos,
           "client line: id 0x100+2, 1 degree north, west = 270, escaped callsign");
    // Frames: monotonic.
    double last = -1;
    bool mono = true;
    int frames = 0, host_lines = 0, nan_lines = 0;
    for (size_t p = 0; p < out.size();) {
        const size_t e = out.find('\n', p);
        const std::string l = out.substr(p, e - p);
        if (!l.empty() && l[0] == '#') {
            const double t = std::atof(l.c_str() + 1);
            if (t <= last) mono = false;
            last = t;
            ++frames;
        } else if (l.rfind("1,T=", 0) == 0) {
            ++host_lines;
        }
        if (l.find("nan") != std::string::npos || l.find("inf") != std::string::npos) ++nan_lines;
        p = e + 1;
    }
    expect(mono && frames > 0, "frames strictly increase");
    expect(host_lines >= 29 && host_lines <= 31, "30 Hz default rate limit (100 Hz in gives about 30 lines, not 25)");
    expect(nan_lines == 0 && info.skipped_bad == 1, "the non-finite sample is skipped");
    const size_t rm = out.find("\n-102\n");
    expect(rm != std::string::npos && out.find("\n102,", rm) == std::string::npos, "client removed on leave, nothing after");
    expect(out.find("\n#1.000\n-102\n") != std::string::npos, "removal is at the leave time");

    lazysf2::acmi::Options all;
    all.max_rate = 0;
    std::string out_all;
    lazysf2::acmi::convert_stream(f.data(), full, all, out_all, info);
    int c = 0;
    for (size_t p = out_all.find("\n1,T="); p != std::string::npos; p = out_all.find("\n1,T=", p + 1)) ++c;
    expect(c == 100, "--max-rate 0 keeps every sample");

    // Reference point: lat/lon are offsets from it, and x is scaled by cos(lat).
    lazysf2::acmi::Options ro;
    ro.ref_lat = 60;
    ro.ref_lon = 10;
    std::string out_ref;
    lazysf2::acmi::convert_stream(f.data(), full, ro, out_ref, info);
    expect(out_ref.find("ReferenceLongitude=10,ReferenceLatitude=60") != std::string::npos &&
               out_ref.find("\n1,T=0.017966223|0.017966223|") != std::string::npos,
           "reference point and cos(lat) scaling");

    // Truncated tail: cut in the middle of a record. What was read is converted, the status says Truncated.
    std::string out_t;
    lazysf2::acmi::Info it;
    expect(lazysf2::acmi::convert_stream(f.data(), full - 20, opt, out_t, it) && it.end == StreamStatus::Truncated &&
               out_t.find("\n1,T=") != std::string::npos,
           "truncated tail: converted, reported");
    // A corrupt length stops reading, the earlier records stay.
    {
        std::vector<uint8_t> g(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(full));
        const size_t fourth = starts[3];  // from the real record sizes: the 4th record
        g[fourth + 3] = 0xff;  // top byte of its length
        std::string o2;
        lazysf2::acmi::Info i2;
        expect(lazysf2::acmi::convert_stream(g.data(), g.size(), opt, o2, i2) && i2.end == StreamStatus::BadLength &&
                   o2.find("\n1,T=") != std::string::npos,
               "corrupt length: stops, keeps the earlier records");
    }
    {  // A client joins again while its object is still shown: the old one is removed first. Headings that print as 360 or -0 are 0.
        std::vector<uint8_t> g(48 + 4000);
        size_t m = encode_stream_header(10.0, "", g.data(), g.size());
        auto put = [&](StreamKind k, double t, const void* p, size_t len) {
            m += encode_stream_record(k, t, static_cast<const uint8_t*>(p), len, g.data() + m, g.size() - m);
        };
        put(StreamKind::ClientJoined, 10.0, &jp, sizeof(jp));
        const float angs[3] = {-0.0f, -1e-5f, 3.14159265f};  // -0, a hair below 0 (359.9994 deg), and 180 deg
        for (int k = 0; k < 3; ++k) {
            StateMsg cs{};
            cs.client_id = 2;
            cs.game_time = 10.1 + 0.1 * k;
            cs.ang[1] = angs[k];
            put(StreamKind::StateReceived, cs.game_time, buf, encode(cs, buf, sizeof(buf)));
        }
        put(StreamKind::ClientJoined, 10.5, &jp, sizeof(jp));  // again, no ClientLeft in between
        std::string o4;
        lazysf2::acmi::Info i4;
        lazysf2::acmi::Options all2;
        all2.max_rate = 0;
        expect(lazysf2::acmi::convert_stream(g.data(), m, all2, o4, i4), "rejoin stream converts");
        expect(o4.find("\n#0.500\n-102\n") != std::string::npos, "a rejoin removes the object that is still shown");
        int zero_heading = 0, bad360 = 0, neg0 = 0;
        for (size_t p = o4.find("\n102,T="); p != std::string::npos; p = o4.find("\n102,T=", p + 1)) {
            const std::string l = o4.substr(p + 1, o4.find('\n', p + 1) - p - 1);
            if (l.find("|360|") != std::string::npos) ++bad360;
            if (l.find("|-0|") != std::string::npos) ++neg0;
            if (l.find(",T=0|0|0|0|0|0|0|0|0") != std::string::npos) ++zero_heading;
        }
        expect(bad360 == 0 && neg0 == 0 && zero_heading >= 2, "a heading of -0 or one that rounds to 360 is written as 0");
    }
    std::string o3;
    lazysf2::acmi::Info i3;
    expect(!lazysf2::acmi::convert_stream(f.data(), 20, opt, o3, i3) && o3.empty(), "short header: rejected");
    std::vector<uint8_t> bad(f.begin(), f.begin() + 60);
    bad[0] = 'X';
    expect(!lazysf2::acmi::convert_stream(bad.data(), bad.size(), opt, o3, i3), "bad magic: rejected");
}

void tracker_logic_tests() {
    using lazysf2::hooks::AircraftSide;
    using lazysf2::hooks::classify_side;
    using lazysf2::hooks::tracking_wanted;
    std::printf("tracker logic\n");
    // The enable rule: hooks only if the probe is on or Tacview Recording is on.
    expect(!tracking_wanted(false, false), "enable rule: neither client -> nothing hooked");
    expect(tracking_wanted(true, false), "enable rule: probe -> hooked");
    expect(tracking_wanted(false, true), "enable rule: Tacview Recording -> hooked");
    expect(tracking_wanted(true, true), "enable rule: both -> hooked");
    // Side classification against three single-bit masks.
    const uint32_t fr = 0x100, en = 0x200, ne = 0x400;
    expect(classify_side(0x1 | fr, fr, en, ne) == AircraftSide::Friendly, "side: friendly bit");
    expect(classify_side(0x1 | en | 0x8000, fr, en, ne) == AircraftSide::Enemy, "side: enemy bit");
    expect(classify_side(ne, fr, en, ne) == AircraftSide::Neutral, "side: neutral bit");
    expect(classify_side(0xfffff0ff, fr, en, ne) == AircraftSide::Unknown, "side: no side bit -> unknown");
    expect(classify_side(0xffffffff, 0, 0, 0) == AircraftSide::Unknown, "side: masks not filled in yet -> unknown");
    expect(classify_side(en, 0, en, ne) == AircraftSide::Enemy, "side: a zero mask never matches, others still do");

    // The decision when a new AircraftObject.dll image appears.
    using lazysf2::hooks::LoadInput;
    using lazysf2::hooks::LoadKind;
    using lazysf2::hooks::decide_on_image;
    auto dec = [](bool hooked, bool build, bool wanted, bool old) {
        return decide_on_image(LoadInput{hooked, build, wanted, old});
    };
    expect(dec(true, true, true, true).kind == LoadKind::Keep && !dec(true, true, true, true).drop_old,
           "image: already hooked and patched -> keep");
    expect(dec(true, true, false, true).kind == LoadKind::Keep, "image: hooked image is kept even if no longer wanted");
    expect(dec(false, false, true, true).kind == LoadKind::BadBuild && !dec(false, false, true, true).drop_old,
           "image: unexpected build -> nothing touched");
    expect(dec(false, true, false, false).kind == LoadKind::Skip && !dec(false, true, false, false).drop_old,
           "image: not wanted, no old hooks -> skip");
    expect(dec(false, true, false, true).kind == LoadKind::Skip && dec(false, true, false, true).drop_old,
           "image: not wanted, old hooks -> skip and drop the old ones");
    expect(dec(false, true, true, false).kind == LoadKind::Install && !dec(false, true, true, false).drop_old,
           "image: wanted, first image -> install");
    expect(dec(false, true, true, true).kind == LoadKind::Install && dec(false, true, true, true).drop_old,
           "image: wanted, reload (hooked image replaced or unpatched) -> drop old, install");
    // The decision when an Options page is left.
    using lazysf2::hooks::OptionsExitAction;
    using lazysf2::hooks::decide_on_options_exit;
    expect(decide_on_options_exit(true, false, true) == OptionsExitAction::Install, "options exit: wanted, DLL loaded, not hooked -> install");
    expect(decide_on_options_exit(false, false, true) == OptionsExitAction::Nothing, "options exit: wanted but no DLL yet -> nothing (the load installs)");
    expect(decide_on_options_exit(true, true, true) == OptionsExitAction::Nothing, "options exit: wanted and hooked -> nothing");
    expect(decide_on_options_exit(true, true, false) == OptionsExitAction::Remove, "options exit: hooked, no longer wanted -> remove");
    expect(decide_on_options_exit(false, true, false) == OptionsExitAction::Remove, "options exit: hooked, DLL state unknown, not wanted -> remove");
    expect(decide_on_options_exit(true, false, false) == OptionsExitAction::Nothing, "options exit: not wanted, not hooked -> nothing");
}

// ---- Tacview recorder core and writer (no game) ----
lazysf2::acmi::TacviewRec make_rec(uint64_t seq, bool player, lazysf2::hooks::AircraftSide side, const char* name,
                                   double t, double x, double y, double z, float pitch, float heading, float roll) {
    lazysf2::acmi::TacviewRec r;
    r.kind = lazysf2::acmi::TacviewRec::kObject;
    r.seq = seq;
    r.is_player = player ? 1 : 0;
    r.side = static_cast<uint8_t>(side);
    r.t = t;
    r.pos[0] = x;
    r.pos[1] = y;
    r.pos[2] = z;
    r.ang[0] = pitch;
    r.ang[1] = heading;
    r.ang[2] = roll;
    strncpy_s(r.name, sizeof(r.name), name, _TRUNCATE);
    return r;
}

size_t count_of(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n;
    return n;
}

std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> v;
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) b = s.size();
        v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}

std::string tv_logged;
void tv_log(const char* l) {
    tv_logged += l;
    tv_logged += '\n';
}

std::string read_whole(const std::wstring& p) {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return s;
    char b[4096];
    size_t n;
    while ((n = std::fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}

void tacview_tests() {
    using namespace lazysf2::acmi;
    using lazysf2::hooks::AircraftSide;
    std::printf("tacview recorder\n");

    // Header, ids, properties, sides, angles.
    {
        TacviewCore core{TacviewCoreConfig{}};
        std::string out;
        core.header(out, "2026-11-24T20:15:00Z");
        expect(out == "FileType=text/acmi/tacview\nFileVersion=2.2\n0,ReferenceTime=2026-11-24T20:15:00Z,"
                      "ReferenceLongitude=0,ReferenceLatitude=0,Title=Strike Fighters 2\n",
               "tacview: header");
        core.add(make_rec(1, true, AircraftSide::Friendly, "F-4E,lead", 100.0, 1000, 2000, 3000, 0.1f, -1.5707964f, 0.2f));
        core.add(make_rec(2, false, AircraftSide::Enemy, "MiG-21", 100.0, 5, 6, 7, 0, 0, 0));
        core.add(make_rec(3, false, AircraftSide::Neutral, "Cessna", 100.0, 5, 6, 7, 0, 0, 0));
        core.add(make_rec(4, false, AircraftSide::Unknown, "Mystery", 100.0, 5, 6, 7, 0, 0, 0));
        core.end_frame(100.0, out);
        const auto ls = lines_of(out);
        expect(ls.size() == 8 && ls[3] == "#0.000", "tacview: frame 0 starts at #0.000");
        expect(ls[4].rfind("1,T=", 0) == 0 &&
                   ls[4].find("|11.46|5.73|270|1000|2000|270,Name=F-4E\\,lead,Pilot=Player,"
                              "Type=Air+FixedWing,Coalition=Allies,Color=Blue") != std::string::npos,
               "tacview: player id 1, Pilot=Player, escaped name, Allies/Blue, angles in degrees");
        expect(ls[5].rfind("10002,T=", 0) == 0 &&
                   ls[5].find(",Name=MiG-21,Pilot=MiG-21,Type=Air+FixedWing,Coalition=Enemies,Color=Red") !=
                       std::string::npos,
               "tacview: other id 0x10000+seq, Pilot=name, Enemies/Red");
        expect(ls[6].find("Coalition=Neutrals,Color=Grey") != std::string::npos, "tacview: neutral is Neutrals/Grey");
        expect(ls[7].rfind("10004,T=", 0) == 0 && ls[7].find("Coalition") == std::string::npos &&
                   ls[7].find("Color") == std::string::npos,
               "tacview: unknown side leaves coalition and colour unset");
        // Lat/lon from the flat mapping at reference 0/0: 1000 m east is 1000/111320 degrees, 2000 m north 2000/111320.
        expect(ls[4].find("T=0.008983112|0.017966223|3000|") != std::string::npos, "tacview: local lon/lat mapping");
    }

    // 10 Hz limit, monotonic frames, removal, non-finite input, seeded continuation.
    {
        TacviewCore core{TacviewCoreConfig{}};
        std::string out;
        core.header(out, "2026-11-24T20:15:00Z");
        // 60 Hz for 2 s; object 2 is there until t = 51.0 and then gone; one frame whose time goes back.
        for (int i = 0; i <= 120; ++i) {
            const double t = 50.0 + i / 60.0;
            core.add(make_rec(1, true, AircraftSide::Friendly, "P", t, i, 0, 100, 0, 0, 0));
            if (t < 51.0) core.add(make_rec(2, false, AircraftSide::Enemy, "E", t, 0, i, 100, 0, 0, 0));
            core.end_frame(i == 90 ? t - 5.0 : t, out);
        }
        const size_t player_lines = count_of(out, "\n1,T=");
        expect(player_lines >= 20 && player_lines <= 21, "tacview: player limited to 10 Hz over 2 s");
        expect(count_of(out, "\n-10002\n") == 1, "tacview: removal written once, when the object leaves");
        expect(count_of(out, "10002,T=") >= 9 && count_of(out, "10002,T=") <= 11,
               "tacview: other object also limited to 10 Hz");
        double prev = -1;
        bool mono = true, any = false;
        for (const std::string& l : lines_of(out)) {
            if (l.empty() || l[0] != '#') continue;
            any = true;
            const double v = std::atof(l.c_str() + 1);
            if (v < prev) mono = false;
            prev = v;
        }
        expect(any && mono, "tacview: frame times never go backwards (clamped)");
        expect(core.stats().rate_limited > 100 && core.live_objects() == 1, "tacview: rate-limited count, one object left");

        // Non-finite input never reaches the file.
        std::string o2;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 60.0, std::nan(""), 0, 0, 0, 0, 0));
        core.end_frame(60.0, o2);
        expect(core.stats().bad_samples == 1 && o2.find("nan") == std::string::npos, "tacview: NaN sample skipped");

        // A fresh core seeded with the origin continues without going back and without a header.
        const double last_t = core.origin() + static_cast<double>(core.last_ms()) / 1000.0;
        TacviewCore b{TacviewCoreConfig{}};
        b.seed_time(core.origin(), core.last_ms());
        std::string o3, o4;
        b.add(make_rec(1, true, AircraftSide::Friendly, "P", last_t - 1.0, 0, 0, 0, 0, 0, 0));  // earlier than the last frame
        b.end_frame(last_t - 1.0, o3);
        expect(o3.rfind("1,T=", 0) == 0, "tacview: a seeded core clamps an earlier frame to the last one (no # line)");
        b.add(make_rec(1, true, AircraftSide::Friendly, "P", last_t + 0.5, 0, 0, 0, 0, 0, 0));
        b.end_frame(last_t + 0.5, o4);
        expect(o4.rfind("#", 0) == 0 && std::atof(o4.c_str() + 1) > static_cast<double>(core.last_ms()) / 1000.0 + 0.49 &&
                   o4.find("FileType") == std::string::npos,
               "tacview: a seeded core continues the time line");
    }

    // Weapons: type by class, name, parent, side from the parent, 20 Hz, its own id series, removal.
    {
        TacviewCore core{TacviewCoreConfig{}};
        std::string out;
        core.header(out, "2026-11-24T20:15:00Z");
        auto weapon = [](uint64_t seq, uint8_t cls, uint64_t parent, AircraftSide side, const char* name, double t, double x) {
            TacviewRec r = make_rec(seq, false, side, name, t, x, 0, 500, 0.1f, 0.5f, 0);
            r.obj_class = cls;
            r.parent_seq = parent;
            return r;
        };
        // Aircraft seq 1 (the player, friendly) and seq 2 (enemy); weapons have their own seq series.
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 10.0, 0, 0, 100, 0, 0, 0));
        core.add(make_rec(2, false, AircraftSide::Enemy, "E", 10.0, 0, 0, 100, 0, 0, 0));
        core.add(weapon(1, TacviewRec::kMissile, 1, AircraftSide::Unknown, "AIM-9B", 10.0, 10));
        core.add(weapon(2, TacviewRec::kBomb, 2, AircraftSide::Unknown, "Mk82", 10.0, 20));
        core.add(weapon(3, TacviewRec::kRocket, 0, AircraftSide::Enemy, "Zuni", 10.0, 30));
        core.end_frame(10.0, out);
        const auto ls = lines_of(out);
        expect(ls.size() == 9, "tacview weapons: header, frame and five objects");
        expect(ls[6].rfind("1000001,T=", 0) == 0 &&
                   ls[6].find(",Name=AIM-9B,Type=Weapon+Missile,Coalition=Allies,Color=Blue,Parent=1") != std::string::npos &&
                   ls[6].find("Pilot") == std::string::npos,
               "tacview weapons: missile id, name, type, the player parent as 1, coalition from the parent");
        expect(ls[7].rfind("1000002,T=", 0) == 0 &&
                   ls[7].find(",Name=Mk82,Type=Weapon+Bomb,Coalition=Enemies,Color=Red,Parent=10002") != std::string::npos,
               "tacview weapons: bomb type, parent id 0x10000+seq, enemy coalition from the parent");
        expect(ls[8].find(",Name=Zuni,Type=Weapon+Rocket,Coalition=Enemies,Color=Red") != std::string::npos &&
                   ls[8].find("Parent") == std::string::npos,
               "tacview weapons: rocket type, own side, no parent when parent_seq is 0");
        // An unknown class value is written as a missile (never as an aircraft); an unseen parent still gets an id.
        {
            TacviewCore c2{TacviewCoreConfig{}};
            std::string o2;
            c2.add(weapon(9, 77, 5, AircraftSide::Unknown, "X", 1.0, 0));
            c2.end_frame(1.0, o2);
            expect(o2.find(",Name=X,Type=Weapon+Missile,Parent=10005") != std::string::npos,
                   "tacview weapons: unknown class is a missile; unseen parent falls back to its aircraft id");
        }

        // 20 Hz for a weapon, 10 Hz for an aircraft, over 2 s of 100 Hz frames; then the weapon is destroyed.
        TacviewCore c3{TacviewCoreConfig{}};
        std::string o3;
        for (int i = 0; i <= 200; ++i) {
            const double t = 50.0 + i / 100.0;
            c3.add(make_rec(1, true, AircraftSide::Friendly, "P", t, 0, 0, 100, 0, 0, 0));
            if (i < 200) c3.add(weapon(1, TacviewRec::kMissile, 1, AircraftSide::Unknown, "AIM-9B", t, i));
            c3.end_frame(t, o3);
        }
        const size_t w_lines = count_of(o3, "\n1000001,T=");
        expect(w_lines >= 39 && w_lines <= 41, "tacview weapons: a weapon is written at 20 Hz");
        const size_t a_lines = count_of(o3, "\n1,T=");
        expect(a_lines >= 20 && a_lines <= 21, "tacview weapons: the aircraft stays at 10 Hz");
        expect(count_of(o3, "\n-1000001\n") == 1, "tacview weapons: removal line when the weapon is gone");
        expect(c3.live_objects() == 1, "tacview weapons: only the aircraft is left");
        // Weapon seq 1 and aircraft seq 1 are different objects with different ids.
        expect(count_of(o3, "\n10001,T=") == 0, "tacview weapons: no id collision with aircraft");
    }

    // Reviewer's two core cases: a NaN sample of an existing object, and end_frame after an empty frame.
    {
        TacviewCore core{TacviewCoreConfig{}};
        std::string out;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 5.0, 0, 0, 100, 0, 0, 0));
        core.add(make_rec(2, false, AircraftSide::Enemy, "E", 5.0, 0, 0, 100, 0, 0, 0));
        core.end_frame(5.0, out);
        std::string o2;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 6.0, 1, 0, 100, 0, 0, 0));
        core.add(make_rec(2, false, AircraftSide::Enemy, "E", 6.0, std::nan(""), 0, 100, 0, 0, 0));
        core.end_frame(6.0, o2);
        expect(o2.find("-") == std::string::npos && core.live_objects() == 2 && core.stats().bad_samples == 1,
               "tacview: an existing object with a NaN record gets no removal line");
        std::string o3;
        core.end_frame(7.0, o3);  // an empty frame: nothing added, so both objects are gone
        expect(o3.find("-1\n") != std::string::npos && o3.find("-10002\n") != std::string::npos &&
                   o3.find("#2.000") != std::string::npos && core.live_objects() == 0,
               "tacview: end_frame with no objects writes the removal lines");
        std::string o4;
        core.end_frame(8.0, o4);
        expect(o4.empty(), "tacview: a second empty frame writes nothing");
    }

    // Weapon type classification and the ring slot size.
    {
        using lazysf2::hooks::WeaponClass;
        using lazysf2::hooks::classify_weapon_type;
        expect(classify_weapon_type(0) == WeaponClass::Bomb && classify_weapon_type(1) == WeaponClass::Missile &&
                   classify_weapon_type(7) == WeaponClass::Missile && classify_weapon_type(2) == WeaponClass::Rocket &&
                   classify_weapon_type(-5) == WeaponClass::Missile,
               "weapon class: 0 bomb, 1 and 7 missile, 2 rocket, anything else a missile");
        expect(sizeof(TacviewRec) == 136, "tacview: ring slot is 136 bytes");
    }

    // The writer, into a temp folder: folder creation, the file name, the name clash, content, drop and cap.
    {
        wchar_t base[MAX_PATH];
        GetTempPathW(MAX_PATH, base);
        const std::wstring root = std::wstring(base) + L"lzsf2_tv_" + std::to_wstring(GetCurrentProcessId());
        const std::wstring dir = root + L"\\Tacview";  // the writer creates this one (root must exist)
        CreateDirectoryW(root.c_str(), nullptr);
        SYSTEMTIME st{};
        st.wYear = 2026;
        st.wMonth = 11;
        st.wDay = 24;
        st.wHour = 10;
        st.wMinute = 11;
        st.wSecond = 12;
        tv_logged.clear();
        TacviewWriter w(64);
        expect(w.ready(), "tacview writer: ring allocated");
        TacviewCoreConfig cfg;
        expect(w.open(dir, st, "2026-11-24T20:15:00Z", cfg, tv_log, nullptr), "tacview writer: opens");
        for (int i = 0; i < 30; ++i) {
            if (!w.begin_frame(2)) {  // the thread is still draining the small ring
                Sleep(5);
                --i;
                continue;
            }
            w.slot(0) = make_rec(1, true, AircraftSide::Friendly, "P", i * 0.1, i, 0, 100, 0, 0, 0);
            w.slot(1) = make_rec(2, false, AircraftSide::Enemy, "E", i * 0.1, 0, i, 100, 0, 0, 0);
            w.commit_frame(i < 20 ? 2 : 1, i * 0.1);
        }
        const uint32_t dropped_before = w.dropped_frames();
        expect(!w.begin_frame(100) && w.dropped_frames() == dropped_before + 1,
               "tacview writer: a frame that cannot fit is dropped and counted");
        w.close_and_join("test");
        const std::string c1 = read_whole(dir + L"\\SF2 2026-11-24 10-11-12.acmi");
        {
            char* dump = nullptr;
            size_t dump_len = 0;
            FILE* df = nullptr;
            if (_dupenv_s(&dump, &dump_len, "LAZYSF2_TACVIEW_TEST_DUMP") == 0 && dump && _wfopen_s(&df, std::wstring(dump, dump + std::strlen(dump)).c_str(), L"wb") == 0 && df) {
                std::fwrite(c1.data(), 1, c1.size(), df);  // for looking at the output by hand
                std::fclose(df);
            }
            std::free(dump);
        }
        expect(c1.rfind("FileType=text/acmi/tacview\nFileVersion=2.2\n0,ReferenceTime=2026-11-24T20:15:00Z", 0) == 0,
               "tacview writer: the file is named SF2 <date time>.acmi and starts with the header");
        expect(count_of(c1, "\n1,T=") == 30 && count_of(c1, "\n10002,T=") == 20 && count_of(c1, "\n-10002\n") == 1,
               "tacview writer: all frames and the removal reach the file");
        expect(tv_logged.find("tacview: recording to ") != std::string::npos &&
                   count_of(tv_logged, "tacview: stopped (test)") == 1 && tv_logged.find("50 transforms") != std::string::npos,
               "tacview writer: one start line and one stop line with the counts");

        // Same second again: " (2)".
        tv_logged.clear();
        TacviewWriter w2(64);
        expect(w2.open(dir, st, "2026-11-24T20:15:00Z", cfg, tv_log, nullptr), "tacview writer: second open");
        w2.close_and_join("test");
        expect(GetFileAttributesW((dir + L"\\SF2 2026-11-24 10-11-12 (2).acmi").c_str()) != INVALID_FILE_ATTRIBUTES,
               "tacview writer: a name clash appends (2)");

        // Size cap: stops, keeps a whole-line prefix, accepting() turns false.
        tv_logged.clear();
        SYSTEMTIME st3 = st;
        st3.wSecond = 40;
        TacviewWriter w3(64);
        expect(w3.open(dir, st3, "2026-11-24T20:15:00Z", cfg, tv_log, nullptr, 600), "tacview writer: opens with a small cap");
        for (int i = 0; i < 40 && w3.accepting(); ++i) {
            if (!w3.begin_frame(1)) {
                Sleep(5);
                continue;
            }
            w3.slot(0) = make_rec(1, true, AircraftSide::Friendly, "P", i * 0.1, i, 0, 100, 0, 0, 0);
            w3.commit_frame(1, i * 0.1);
            Sleep(2);
        }
        for (int i = 0; i < 100 && w3.accepting(); ++i) Sleep(10);
        expect(!w3.accepting(), "tacview writer: the size cap stops the recording");
        w3.close_and_join("cap test");
        const std::string c3 = read_whole(dir + L"\\SF2 2026-11-24 10-11-40.acmi");
        expect(!c3.empty() && c3.size() <= 600 && c3.back() == '\n' && count_of(tv_logged, "byte cap") == 1,
               "tacview writer: capped file is a whole-line prefix, logged once");

        // A folder that cannot be created: the failure is logged once and accepting() is false.
        tv_logged.clear();
        TacviewWriter w4(64);
        expect(w4.open(root + L"\\no\\such\\parent\\Tacview", st, "2026-11-24T20:15:00Z", cfg, tv_log, nullptr),
               "tacview writer: open returns (the thread reports the failure)");
        for (int i = 0; i < 100 && w4.accepting(); ++i) Sleep(10);
        expect(!w4.accepting() && count_of(tv_logged, "not recording") == 1,
               "tacview writer: folder failure logged once, recording off");
        w4.close_and_join("test");

        // Clean up.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(dir.c_str());
        RemoveDirectoryW(root.c_str());
    }

    // The shared formatter: escaping and heading normalisation.
    {
        std::string s;
        append_escaped(s, "a,b\nc\xE9");
        expect(s == "a\\,b?c?", "acmi format: comma escaped, control and non-ASCII bytes replaced");
        expect(norm_deg(-90.0) == 270.0 && norm_deg(360.0) == 0.0 && norm_deg(359.999) == 0.0,
               "acmi format: heading normalised");
    }
}

int main() {
    std::printf("protocol_selftest\n");
    const char* why = nullptr;
    bool ok = protocol_selftest(&why);
    expect(ok, ok ? "round trips and rejections" : why);

    loopback();
    parse_tests();

    if (enet_initialize() == 0) {
        host_core_tests();
        enet_deinitialize();
    } else {
        expect(false, "enet_initialize");
    }

    bot_loopback_tests();
    stream_tests();
    acmi_tests();
    tracker_logic_tests();
    tacview_tests();

    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
