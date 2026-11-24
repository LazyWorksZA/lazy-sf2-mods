// Console tests for the network protocol and an ENet loopback. Not shipped, no game needed.
#include <enet/enet.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "net/host_core.h"
#include "net/protocol.h"

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
    expect(post_flood.dropped_queue > pre_flood.dropped_queue && states == (512 - 64) / kMaxClients,
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
        expect(a_states == kQuota && mid.dropped_quota[xslot] - pre.dropped_quota[xslot] == 800 - kQuota,
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
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<HostEvent> gone;
    for (int i = 0; i < 9000 && gone.empty(); ++i) {
        d.service(0);
        HostEvent e2[4];
        const size_t n = core.poll(e2, 4);
        gone.insert(gone.end(), e2, e2 + n);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  Disconnect after %.1f s, reason %d\n", secs, gone.empty() ? -1 : gone[0].reason);
    expect(!gone.empty() && gone[0].kind == HostEvent::Disconnect && gone[0].reason == 1 && secs >= 4.5 && secs < 8.0,
           "a silent client times out after about 5 s, by the core's timer (reason 1)");

    core.stop(false);
    expect(core.state() == HostCore::State::Idle, "stopped cleanly");
}

}  // namespace

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

    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
