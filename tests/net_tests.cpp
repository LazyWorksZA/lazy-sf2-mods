// Console tests for the network protocol and an ENet loopback. Not shipped, no game needed.
#include <enet/enet.h>

#include <algorithm>
#include <atomic>
#include <map>
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
#include "acmi/airfields.h"
#include "tacview_addon_lua.h"
#include "acmi/ini.h"
#include "acmi/acmi_zip.h"
#include "acmi/tacview_core.h"
#include "acmi/tacview_terrain.h"
#include "acmi/tacview_writer.h"
#include "acmi/terrain_texture.h"
#include "hooks/options_gameplay_splice.h"
#include "hooks/weapon_class.h"
#include "hooks/probe_snap_logic.h"
#include "hooks/world_walk_logic.h"
#include "hooks/aircraft_side.h"
#include "hooks/cursor_clip_logic.h"
#include "hooks/pilot_twist_math.h"
#include "config_parse.h"
#include "bot_client.h"
#include <miniz.h>

#include "bot_core.h"
#include "net/host_core.h"
#include "net/protocol.h"
#include "net/stream_format.h"
#include "net/stream_record.h"

using namespace LazyWorks::SF2Patch::net;

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
    bool throttle_off = false;
    void send(const uint8_t* d, size_t n, uint8_t channel) {
        if (connected && !throttle_off) {  // a starved machine inflates the RTT, and ENet's throttle would then drop unreliable States
            enet_peer_throttle_configure(peer, 5000, 0, 0);
            throttle_off = true;
        }
        ENetPacket* p = enet_packet_create(d, n, channel == kChannelReliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (p && enet_peer_send(peer, channel, p) != 0) enet_packet_destroy(p);
        enet_host_flush(host);
    }
    void idle(int ms) {  // service the client for ms milliseconds of wall time (the host's timers run on the wall clock too)
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        do {
            service(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < until);
    }
};

// Polls the core for up to ms, until want events have arrived; the client is serviced meanwhile.
std::vector<HostEvent> collect(HostCore& core, TestClient& c, size_t want, int ms) {
    std::vector<HostEvent> out;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (out.size() < want && std::chrono::steady_clock::now() < until) {
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
    {  // wait for the Bye itself rather than for a fixed time: the network thread may be starved
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (b.got.empty() && !b.disconnected && std::chrono::steady_clock::now() < until) b.idle(1);
        b.idle(100);
    }
    {
        ByeMsg got_bye{};
        const bool one = b.got.size() == 1;
        std::printf("  Bye wait: %zu received, disconnected %d\n", b.got.size(), b.disconnected ? 1 : 0);
        expect(!b.disconnected && one && b.got[0].channel == kChannelReliable &&
                   decode(b.got[0].bytes.data(), b.got[0].bytes.size(), got_bye) == Status::Ok && got_bye.reason == 9,
               "the current-generation Bye is delivered, with its content (reason 9)");
    }

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

namespace bot = LazyWorks::SF2Patch::bot;

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
    {  // hosts that go quiet: no listener (code 3 at 4 s), a connected host without a Welcome (code 3 at 15 s), a Welcome then
       // silence (code 4 at 4 s), and the real core, whose own 5 s timer kicks a client that never gets welcomed (code 4)
        using Clk = std::chrono::steady_clock;
        struct Run {
            int code = -1;
            double secs = 0;
            bool welcomed = false;
        };
        auto timed = [](const std::string& host) {
            bot::BotConfig c;
            c.host = host;
            c.duration = 20.0;
            Run r;
            const auto t0 = Clk::now();
            r.code = bot::run_bot(c).code;
            r.secs = std::chrono::duration<double>(Clk::now() - t0).count();
            return r;
        };
        // A bare ENet server that never answers a Ping, and sends the Welcome to the first Hello only if asked to.
        auto raw_host = [&](bool send_welcome) {
            Run r;
            if (enet_initialize() != 0) return r;
            ENetAddress sa{};
            enet_address_set_host(&sa, "127.0.0.1");
            sa.port = 0;
            ENetHost* srv = enet_host_create(&sa, 2, kChannelCount, 0, 0);
            if (srv) {
                std::atomic<bool> done{false};
                Run res;
                const std::string host = "127.0.0.1:" + std::to_string(srv->address.port);
                std::thread th([&] {
                    res = timed(host);
                    done = true;
                });
                while (!done) {
                    ENetEvent ev;
                    while (enet_host_service(srv, &ev, 1) > 0) {
                        if (ev.type != ENET_EVENT_TYPE_RECEIVE) continue;
                        MsgType t{};
                        if (send_welcome && !res.welcomed && peek_type(ev.packet->data, ev.packet->dataLength, t) == Status::Ok &&
                            t == MsgType::Hello) {
                            WelcomeMsg w{};
                            w.client_id = 1;
                            w.host_game_time = 1000.0;
                            uint8_t wb[64];
                            ENetPacket* p = enet_packet_create(wb, encode(w, wb, sizeof(wb)), ENET_PACKET_FLAG_RELIABLE);
                            if (p && enet_peer_send(ev.peer, kChannelReliable, p) != 0) enet_packet_destroy(p);
                            enet_host_flush(srv);
                            r.welcomed = true;
                        }
                        enet_packet_destroy(ev.packet);
                    }
                }
                th.join();
                r.code = res.code;
                r.secs = res.secs;
                enet_host_destroy(srv);
            }
            enet_deinitialize();
            return r;
        };
        const Run none = timed("127.0.0.1:9");  // nothing listens there
        const Run no_welcome = raw_host(false);
        const Run silent = raw_host(true);
        Run kicked;
        {
            HostCore core;  // accepts the connection, but no game thread ever sends the Welcome
            core.start("127.0.0.1", 0, [](const char*) {});
            for (int i = 0; i < 2000 && core.state() == HostCore::State::Starting; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (core.state() == HostCore::State::Running) kicked = timed("127.0.0.1:" + std::to_string(core.port()));
            core.stop(false);
        }
        std::printf("  quiet hosts: no listener %d/%.1f s; no Welcome %d/%.1f s; silent after Welcome %d/%.1f s; kicked by the core "
                    "%d/%.1f s\n",
                    none.code, none.secs, no_welcome.code, no_welcome.secs, silent.code, silent.secs, kicked.code, kicked.secs);
        expect(none.code == 3 && none.secs > 3.5 && none.secs < 9.0, "no answer to connect: exit code 3 after about 4 s");
        expect(no_welcome.code == 3 && no_welcome.secs > 14.0 && no_welcome.secs < 22.0,
               "connected but no Welcome: exit code 3 after about 15 s");
        expect(silent.welcomed && silent.code == 4 && silent.secs > 3.5 && silent.secs < 9.0,
               "host silent after the Welcome: exit code 4 after about 4 s");
        expect(kicked.code == 4 && kicked.secs > 3.5 && kicked.secs < 9.0,
               "the core drops a client that is never welcomed: the bot exits 4");
    }
    {
        const BotSession r = run_clean_session(bot::Script::Climb, 20.0, 1.0, 0.0, 0.0, 0.0);
        check_session(r, 20.0, 0.8, 1.2, "climb, 20 Hz, 1 s");
        expect(r.got_bye && !r.states.empty() && r.states.back().ang[0] > 0.3f && r.states.back().vel[2] > 50.0f,
               "climb: the stream pitches up and climbs");
    }
}

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

    LazyWorks::SF2Patch::acmi::Options opt;
    std::string out;
    LazyWorks::SF2Patch::acmi::Info info;
    expect(LazyWorks::SF2Patch::acmi::convert_stream(f.data(), full, opt, out, info), "converts");
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

    LazyWorks::SF2Patch::acmi::Options all;
    all.max_rate = 0;
    std::string out_all;
    LazyWorks::SF2Patch::acmi::convert_stream(f.data(), full, all, out_all, info);
    int c = 0;
    for (size_t p = out_all.find("\n1,T="); p != std::string::npos; p = out_all.find("\n1,T=", p + 1)) ++c;
    expect(c == 100, "--max-rate 0 keeps every sample");

    // Reference point: lat/lon are offsets from it, and x is scaled by cos(lat).
    LazyWorks::SF2Patch::acmi::Options ro;
    ro.ref_lat = 60;
    ro.ref_lon = 10;
    std::string out_ref;
    LazyWorks::SF2Patch::acmi::convert_stream(f.data(), full, ro, out_ref, info);
    expect(out_ref.find("ReferenceLongitude=10,ReferenceLatitude=60") != std::string::npos &&
               out_ref.find("\n1,T=0.017966223|0.017966223|") != std::string::npos,
           "reference point and cos(lat) scaling");

    // Truncated tail: cut in the middle of a record. What was read is converted, the status says Truncated.
    std::string out_t;
    LazyWorks::SF2Patch::acmi::Info it;
    expect(LazyWorks::SF2Patch::acmi::convert_stream(f.data(), full - 20, opt, out_t, it) && it.end == StreamStatus::Truncated &&
               out_t.find("\n1,T=") != std::string::npos,
           "truncated tail: converted, reported");
    // A corrupt length stops reading, the earlier records stay.
    {
        std::vector<uint8_t> g(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(full));
        const size_t fourth = starts[3];  // from the real record sizes: the 4th record
        g[fourth + 3] = 0xff;  // top byte of its length
        std::string o2;
        LazyWorks::SF2Patch::acmi::Info i2;
        expect(LazyWorks::SF2Patch::acmi::convert_stream(g.data(), g.size(), opt, o2, i2) && i2.end == StreamStatus::BadLength &&
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
        LazyWorks::SF2Patch::acmi::Info i4;
        LazyWorks::SF2Patch::acmi::Options all2;
        all2.max_rate = 0;
        expect(LazyWorks::SF2Patch::acmi::convert_stream(g.data(), m, all2, o4, i4), "rejoin stream converts");
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
    LazyWorks::SF2Patch::acmi::Info i3;
    expect(!LazyWorks::SF2Patch::acmi::convert_stream(f.data(), 20, opt, o3, i3) && o3.empty(), "short header: rejected");
    std::vector<uint8_t> bad(f.begin(), f.begin() + 60);
    bad[0] = 'X';
    expect(!LazyWorks::SF2Patch::acmi::convert_stream(bad.data(), bad.size(), opt, o3, i3), "bad magic: rejected");
}

void cursor_clip_tests() {
    using LazyWorks::SF2Patch::hooks::ClipInputs;
    using LazyWorks::SF2Patch::hooks::want_clip;
    std::printf("cursor clip\n");
    const ClipInputs on{true, true, true, false, true};
    expect(want_clip(on), "clip: fullscreen + match cursor + foreground -> clip");
    expect(!want_clip({false, true, true, false, true}), "clip: borderless -> never");
    expect(!want_clip({true, false, true, false, true}), "clip: stock mouse -> never");
    expect(!want_clip({true, true, false, false, true}), "clip: not foreground -> release");
    expect(!want_clip({true, true, true, true, true}), "clip: minimized -> release");
    expect(!want_clip({true, true, true, false, false}), "clip: no rect -> release");
    int clipping = 0;
    for (int m = 0; m < 32; ++m) {
        const bool v = want_clip({(m & 1) != 0, (m & 2) != 0, (m & 4) != 0, (m & 8) != 0, (m & 16) != 0});
        if (v) ++clipping;
        if (v) expect(m == 1 + 2 + 4 + 16, "clip: only one input combination clips");
    }
    expect(clipping == 1, "clip: exactly one of 32 combinations clips");
}

void tracker_logic_tests() {
    using LazyWorks::SF2Patch::hooks::AircraftSide;
    using LazyWorks::SF2Patch::hooks::classify_side;
    using LazyWorks::SF2Patch::hooks::tracking_wanted;
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
    using LazyWorks::SF2Patch::hooks::LoadInput;
    using LazyWorks::SF2Patch::hooks::LoadKind;
    using LazyWorks::SF2Patch::hooks::decide_on_image;
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
    using LazyWorks::SF2Patch::hooks::OptionsExitAction;
    using LazyWorks::SF2Patch::hooks::decide_on_options_exit;
    expect(decide_on_options_exit(true, false, true) == OptionsExitAction::Install, "options exit: wanted, DLL loaded, not hooked -> install");
    expect(decide_on_options_exit(false, false, true) == OptionsExitAction::Nothing, "options exit: wanted but no DLL yet -> nothing (the load installs)");
    expect(decide_on_options_exit(true, true, true) == OptionsExitAction::Nothing, "options exit: wanted and hooked -> nothing");
    expect(decide_on_options_exit(true, true, false) == OptionsExitAction::Remove, "options exit: hooked, no longer wanted -> remove");
    expect(decide_on_options_exit(false, true, false) == OptionsExitAction::Remove, "options exit: hooked, DLL state unknown, not wanted -> remove");
    expect(decide_on_options_exit(true, false, false) == OptionsExitAction::Nothing, "options exit: not wanted, not hooked -> nothing");
}

LazyWorks::SF2Patch::acmi::TacviewRec make_rec(uint64_t seq, bool player, LazyWorks::SF2Patch::hooks::AircraftSide side, const char* name,
                                   double t, double x, double y, double z, float pitch, float heading, float roll) {
    LazyWorks::SF2Patch::acmi::TacviewRec r;
    r.kind = LazyWorks::SF2Patch::acmi::TacviewRec::kObject;
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

// The one entry of a .zip.acmi, read through miniz; empty (and entry_name empty) when it is not a one-entry zip.
std::string read_zip_entry(const std::wstring& p, std::string& entry_name) {
    std::string r;
    entry_name.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return r;
    mz_zip_archive z{};
    if (mz_zip_reader_init_cfile(&z, f, 0, 0)) {
        mz_zip_archive_file_stat st{};
        if (mz_zip_reader_get_num_files(&z) == 1 && mz_zip_reader_file_stat(&z, 0, &st)) {
            size_t n = 0;
            void* heap = mz_zip_reader_extract_to_heap(&z, 0, &n, 0);
            if (heap) {
                r.assign(static_cast<const char*>(heap), n);
                mz_free(heap);
                entry_name = st.m_filename;
            }
        }
        mz_zip_reader_end(&z);
    }
    std::fclose(f);
    return r;
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

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

void terrain_tests() {
    using namespace LazyWorks::SF2Patch::acmi;
    std::printf("tacview terrain\n");
    expect(terrain_id(L"DESERT") == "LazySF2Patch.DESERT", "terrain id: plain name");
    expect(terrain_id(std::wstring(L"Angola v1/") + wchar_t(0xe9) + L",x") == "LazySF2Patch.Angola_v1___x", "terrain id: other characters become _");
    expect(terrain_id(L"A-b_9") == "LazySF2Patch.A-b_9", "terrain id: dash and underscore kept");
    expect(terrain_id(L"").empty(), "terrain id: empty in, empty out");
    expect(terrain_id(std::wstring(100, L'X')).size() == 48, "terrain id: capped at 48 chars");

    const uint16_t g[6] = {1, 2, 3, 0x0104, 5, 0xabcd};  // 3x2, south row first
    std::vector<uint8_t> raw;
    terrain_raw_north_first(g, 3, 2, raw);
    const uint8_t want[12] = {0x04, 0x01, 5, 0, 0xcd, 0xab, 1, 0, 2, 0, 3, 0};
    expect(raw.size() == 12 && std::memcmp(raw.data(), want, 12) == 0, "terrain raw: north row first, little-endian");

    expect(terrain_file_stem("LazySF2Patch.T") == "LazySF2Patch-T" && terrain_file_stem("LazySF2Patch.A_b-1") == "LazySF2Patch-A_b-1" &&
               terrain_file_stem("").empty(),
           "terrain file stem: dots become dashes");

    const std::string xml = terrain_xml("LazySF2Patch.T", "LazySF2Patch-T.raw", 2000, 2000, 500.0, -25.0, -4.5, -4.5);
    const auto m = make_mapping(-4.5);
    const std::string east = num(-25.0 + 1999 * 500.0 * m.lon_scale, 9);
    const std::string north = num(-4.5 + 1999 * 500.0 * m.lat_scale, 9);
    auto has = [&](const std::string& piece) { return xml.find(piece) != std::string::npos; };
    auto corner = [&](const char* tag, const std::string& lon, const std::string& lat) {
        return has(std::string("<") + tag + ">\n\t\t\t\t<Longitude>" + lon + "</Longitude>\n\t\t\t\t<Latitude>" + lat +
                   "</Latitude>");
    };
    expect(has("<CustomHeightmap Layer=\"Real World\" Id=\"LazySF2Patch.T\">") && has("<File>LazySF2Patch-T.raw</File>") &&
               has("<Width>2000</Width>") && has("<Height>2000</Height>") && has("<BigEndian>0</BigEndian>") &&
               has("<AltitudeFactor>1</AltitudeFactor>") && has("<Projection>Quad</Projection>"),
           "terrain xml: structure");
    expect(corner("BottomLeft", "-25", "-4.5") && corner("BottomRight", east, "-4.5") && corner("TopRight", east, north) &&
               corner("TopLeft", "-25", north),
           "terrain xml: corners");
    expect(std::atof(east.c_str()) > -16.0 && std::atof(east.c_str()) < -15.9 && std::atof(north.c_str()) > 4.4 &&
               std::atof(north.c_str()) < 4.5,
           "terrain xml: 1000 km square sits over the Atlantic");

    TacviewCoreConfig cfg;
    cfg.theater = "LazySF2Patch.DESERT";
    cfg.title = "Strike Fighters 2 - Desert";
    std::string h1, h2;
    TacviewCore{cfg}.header(h1, "2026-11-24T20:15:00Z");
    expect(h1 == "FileType=text/acmi/tacview\nFileVersion=2.2\n0,ReferenceTime=2026-11-24T20:15:00Z,"
                 "ReferenceLongitude=0,ReferenceLatitude=0,Title=Strike Fighters 2 - Desert,DataSource=Strike Fighters 2,DataRecorder=Lazy SF2 Patch,Theater=LazySF2Patch.DESERT,LazySF2PatchTheater=LazySF2Patch.DESERT\n",
           "header: Theater= and LazySF2PatchTheater= after the title");
    TacviewCore{TacviewCoreConfig{}}.header(h2, "2026-11-24T20:15:00Z");
    expect(h2.find("Theater") == std::string::npos && h2.find("Title=Strike Fighters 2,DataSource=") != std::string::npos,
           "header: no Theater= by default");
}

void addon_embed_test() {
    const std::string lua = kTacviewAddonLua;
    expect(!lua.empty() && lua.rfind("-- Lazy SF2 Patch terrain switch", 0) == 0, "tacview add-on: embedded text is non-empty and has its header line");
    expect(lua.find("LazySF2PatchTheater") != std::string::npos && lua.find("SetTitle(\"Lazy SF2 Patch\")") != std::string::npos,
           "tacview add-on: embedded text reads LazySF2PatchTheater and sets the add-on title");
}

void tacview_tests() {
    addon_embed_test();
    using namespace LazyWorks::SF2Patch::acmi;
    using LazyWorks::SF2Patch::hooks::AircraftSide;
    std::printf("tacview recorder\n");

    // Header, ids, properties, sides, angles.
    {
        TacviewCore core{TacviewCoreConfig{}};
        std::string out;
        core.header(out, "2026-11-24T20:15:00Z");
        expect(out == "FileType=text/acmi/tacview\nFileVersion=2.2\n0,ReferenceTime=2026-11-24T20:15:00Z,"
                      "ReferenceLongitude=0,ReferenceLatitude=0,Title=Strike Fighters 2,DataSource=Strike Fighters 2,DataRecorder=Lazy SF2 Patch\n",
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

    // Aircraft type name: Name=type, Pilot=callsign (the player too); unknown type keeps the old output.
    {
        TacviewCore core{TacviewCoreConfig{}};
        std::string out;
        core.header(out, "2026-11-25T00:00:00Z");
        auto with_type = [](TacviewRec r, const char* type) {
            strncpy_s(r.type, sizeof(r.type), type, _TRUNCATE);
            return r;
        };
        core.add(with_type(make_rec(1, true, AircraftSide::Friendly, "GUNSLINGER", 1.0, 1, 2, 3, 0, 0, 0), "F-100D"));
        core.add(with_type(make_rec(2, false, AircraftSide::Enemy, "CRAB 12", 1.0, 1, 2, 3, 0, 0, 0), "MiG-17,F"));
        core.add(make_rec(3, false, AircraftSide::Enemy, "MiG-21", 1.0, 1, 2, 3, 0, 0, 0));  // type unknown
        core.add(with_type(make_rec(4, false, AircraftSide::Friendly, "", 1.0, 1, 2, 3, 0, 0, 0), "A-4E"));  // empty callsign
        core.end_frame(1.0, out);
        const auto ls = lines_of(out);
        expect(ls.size() >= 7 && ls[4].find(",Name=F-100D,Pilot=GUNSLINGER,Type=Air+FixedWing,") != std::string::npos,
               "tacview type: player with a known type gets Name=type, Pilot=callsign");
        expect(ls[4].rfind("1,T=", 0) == 0, "tacview type: the player keeps id 1");
        expect(ls[5].find(",Name=MiG-17\\,F,Pilot=CRAB 12,") != std::string::npos,
               "tacview type: the type name is escaped like any property");
        expect(ls[6].find(",Name=MiG-21,Pilot=MiG-21,Type=Air+FixedWing,") != std::string::npos,
               "tacview type: unknown type keeps Name=callsign, Pilot=callsign");
        expect(ls[7].find("Name=A-4E,") != std::string::npos && ls[7].find("Pilot") == std::string::npos,
               "tacview type: an empty callsign writes no Pilot");
    }
    {  // a player whose type is unknown keeps Pilot=Player (unchanged output)
        TacviewCore core{TacviewCoreConfig{}};
        std::string out;
        core.add(make_rec(1, true, AircraftSide::Friendly, "GUNSLINGER", 1.0, 1, 2, 3, 0, 0, 0));
        core.end_frame(1.0, out);
        expect(out.find("Name=GUNSLINGER,Pilot=Player,") != std::string::npos, "tacview type: unknown type, player keeps Pilot=Player");
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
        b.add(make_rec(1, true, AircraftSide::Friendly, "P", last_t + 0.5, 1, 0, 0, 0, 0, 0));  // moved: a delta line
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
            c3.add(make_rec(1, true, AircraftSide::Friendly, "P", t, i, 0, 100, 0, 0, 0));
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

    // Delta encoding: only the changed T components are written, every '|' is kept, a re-created id is complete, and
    // a decoder that reapplies the deltas reproduces the full values.
    {
        TacviewCore core{TacviewCoreConfig{}};
        std::string o1, o2, o3;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 10.0, 1000, 2000, 3000, 0.1f, 1.0f, 0.2f));
        core.end_frame(10.0, o1);
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 10.2, 2000, 2000, 3000, 0.1f, 1.0f, 0.2f));
        core.end_frame(10.2, o2);
        expect(o2 == "#0.200\n1,T=0.017966223||||||2000||\n", "tacview delta: a longitude-only change writes lon and U, nine fields");
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 10.4, 2000, 2000, 3000, 0.1f, 1.0f, 0.2f));
        core.end_frame(10.4, o3);
        expect(o3.empty() && core.stats().unchanged == 1, "tacview delta: an unchanged sample writes no line");

        std::string o4, o5;
        core.end_frame(10.6, o4);  // the player is gone
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 10.8, 2000, 2000, 3000, 0.1f, 1.0f, 0.2f));
        core.end_frame(10.8, o5);
        const auto l5 = lines_of(o5);
        expect(o4.find("-1\n") != std::string::npos && l5.size() == 2 && l5[1].rfind("1,T=0.017966223|0.017966223|3000|", 0) == 0 &&
                   l5[1].find("||") == std::string::npos && l5[1].find(",Name=") != std::string::npos,
               "tacview delta: a re-created id is written in full, with its properties");

        // Round trip over several objects, with a removal and a re-creation.
        TacviewCore rc{TacviewCoreConfig{}};
        std::map<uint32_t, std::vector<std::string>> state;
        bool round_ok = true;
        int checked = 0;
        for (int i = 0; i < 60; ++i) {
            const double t = 100.0 + i * 0.2;
            std::vector<std::pair<uint32_t, TacviewRec>> in;
            in.push_back({1, make_rec(1, true, AircraftSide::Friendly, "P", t, i * 37.5, 500 + i % 7, 3000 + i, 0.01f * i, 0.05f * i, 0)});
            if (i % 3) in.push_back({0x10002, make_rec(2, false, AircraftSide::Enemy, "E", t, 90000, -i * 11.25, 1200, 0, 1.5f, (i / 5) * 0.1f)});
            if (i < 20 || i >= 40) in.push_back({0x10003, make_rec(3, false, AircraftSide::Neutral, "N", t, 5, 6, 7, 0, 0, 0)});
            for (auto& e : in) rc.add(e.second);
            std::string out;
            rc.end_frame(t, out);
            for (const std::string& l : lines_of(out)) {
                if (!l.empty() && l[0] == '-') {
                    state.erase(static_cast<uint32_t>(std::strtoul(l.c_str() + 1, nullptr, 16)));
                    continue;
                }
                const size_t tp = l.find(",T=");
                if (tp == std::string::npos) continue;
                const uint32_t id = static_cast<uint32_t>(std::strtoul(l.c_str(), nullptr, 16));
                const size_t end = l.find(',', tp + 3);
                const std::string body = l.substr(tp + 3, end == std::string::npos ? std::string::npos : end - tp - 3);
                std::vector<std::string> f;
                size_t a = 0;
                for (;;) {
                    const size_t b = body.find('|', a);
                    f.push_back(body.substr(a, b == std::string::npos ? std::string::npos : b - a));
                    if (b == std::string::npos) break;
                    a = b + 1;
                }
                if (f.size() != 9) {
                    round_ok = false;
                    continue;
                }
                auto& st = state[id];
                if (st.empty()) st.assign(9, "");
                for (int k = 0; k < 9; ++k)
                    if (!f[k].empty()) st[k] = f[k];
            }
            for (auto& e : in) {
                TransformText full;
                format_transform(full, e.second.pos, e.second.ang, Mapping{1.0 / 111320.0, 1.0 / 111320.0});
                auto it = state.find(e.first);
                if (it == state.end()) {
                    round_ok = false;
                    continue;
                }
                for (int k = 0; k < 9; ++k) round_ok = round_ok && it->second[k] == full.f[k];
                ++checked;
            }
        }
        expect(round_ok && checked > 100, "tacview delta: a decoder reapplying the deltas reproduces the full values");
    }

    // NaN sample of an existing object; end_frame after an empty frame.
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

    // A finished object: it leaves the frame (removal), and a destroyed marker adds the Destroyed event before it.
    {
        auto marker = [](uint64_t seq, uint8_t cls, double t) {
            TacviewRec r = make_rec(seq, false, AircraftSide::Unknown, "", t, 0, 0, 0, 0, 0, 0);
            r.obj_class = cls;
            r.destroyed = 1;
            return r;
        };
        auto weapon = [](uint64_t seq, double t) {
            TacviewRec r = make_rec(seq, false, AircraftSide::Friendly, "AIM-9B", t, 0, 0, 500, 0, 0, 0);
            r.obj_class = TacviewRec::kMissile;
            return r;
        };
        TacviewCore core{TacviewCoreConfig{}};
        std::string o1, o2, o3, o4;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 1.0, 0, 0, 100, 0, 0, 0));
        core.add(make_rec(2, false, AircraftSide::Enemy, "E", 1.0, 0, 0, 100, 0, 0, 0));
        core.add(weapon(1, 1.0));
        core.end_frame(1.0, o1);
        // Aircraft 2 and the weapon are no longer listed (inactive); only aircraft 2 gets a marker.
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 1.1, 1, 0, 100, 0, 0, 0));
        core.add(marker(2, TacviewRec::kAircraft, 1.1));
        core.end_frame(1.1, o2);
        const size_t ev = o2.find("0,Event=Destroyed|10002|\n");
        const size_t rm = o2.find("-10002\n");
        expect(ev != std::string::npos && rm != std::string::npos && ev < rm,
               "tacview destroyed: the event is written before the removal");
        expect(count_of(o2, "-1000001\n") == 1 && count_of(o2, "Event=") == 1,
               "tacview destroyed: an unlisted weapon is removed with no event");
        expect(core.live_objects() == 1, "tacview destroyed: only the player is left");
        // The marker again (the aircraft stays inactive for the rest of the flight), and one for an unknown object.
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 1.2, 2, 0, 100, 0, 0, 0));
        core.add(marker(2, TacviewRec::kAircraft, 1.2));
        core.add(marker(77, TacviewRec::kAircraft, 1.2));
        core.end_frame(1.2, o3);
        expect(o3.find("Event=") == std::string::npos && o3.find("-1") == std::string::npos,
               "tacview destroyed: a repeated or unknown marker writes nothing");
        // A marker never keeps the object alive: the player's marker removes id 1 too.
        core.add(marker(1, TacviewRec::kAircraft, 1.3));
        core.end_frame(1.3, o4);
        expect(o4.find("0,Event=Destroyed|1|\n") != std::string::npos && o4.find("-1\n") != std::string::npos &&
                   core.live_objects() == 0 && core.stats().destroyed_events == 2,
               "tacview destroyed: the player's id is 1, and the event count is kept");
        std::string ev_line;
        append_event_destroyed(ev_line, 0x2000a1f);
        expect(ev_line == "0,Event=Destroyed|2000a1f|\n", "acmi format: Destroyed event line");
    }

    // An unchanged ground pose is not written again; a change is.
    {
        using LazyWorks::SF2Patch::hooks::AircraftSide;
        TacviewCore core{TacviewCoreConfig{}};
        auto site = [](double t, double x) {
            TacviewRec r = make_rec(1, false, AircraftSide::Enemy, "SA-2 Site", t, x, 200, 5, 0.0f, 1.0f, 0.0f);
            r.obj_class = TacviewRec::kGroundStatic;
            return r;
        };
        std::string o1, o2, o3, o4;
        core.add(site(1.0, 100));
        core.end_frame(1.0, o1, TacviewRec::kSeriesGroundStatic);
        core.add(site(2.0, 100));
        core.end_frame(2.0, o2, TacviewRec::kSeriesGroundStatic);
        core.add(site(3.0, 101));
        core.end_frame(3.0, o3, TacviewRec::kSeriesGroundStatic);
        core.add(site(4.0, 101));
        core.end_frame(4.0, o4, TacviewRec::kSeriesGroundStatic);
        expect(count_of(o1, "2000001,T=") == 1 && count_of(o2, "2000001,T=") == 0 && count_of(o3, "2000001,T=") == 1 &&
                   count_of(o4, "2000001,T=") == 0 && core.live_objects() == 1 && core.stats().unchanged == 2,
               "tacview world: an unchanged static is written once, a change is written again, and it stays alive");
        std::string o5;
        core.end_frame(5.0, o5, TacviewRec::kSeriesGroundStatic);
        expect(o5.find("-2000001") != std::string::npos, "tacview world: a skipped static is still removed when it is gone");
    }

    // Explosions: for a detonated weapon, a destroyed aircraft and a destroyed ground object; not for a plain removal.
    {
        using LazyWorks::SF2Patch::hooks::AircraftSide;
        TacviewCore core{TacviewCoreConfig{}};
        auto obj = [](uint8_t cls, uint64_t seq, double t, double x) {
            TacviewRec r = make_rec(seq, false, AircraftSide::Friendly, "X", t, x, 50, 300, 0, 0, 0);
            r.obj_class = cls;
            return r;
        };
        auto marker = [](uint8_t cls, uint64_t seq, double t, uint8_t kind) {
            TacviewRec r = make_rec(seq, false, AircraftSide::Unknown, "", t, 0, 0, 0, 0, 0, 0);
            r.obj_class = cls;
            r.destroyed = kind;
            return r;
        };
        std::string o1, o2, o3, o4, o5;
        core.add(obj(TacviewRec::kBomb, 1, 1.0, 100));
        core.add(obj(TacviewRec::kRocket, 2, 1.0, 110));
        core.add(obj(TacviewRec::kMissile, 3, 1.0, 120));
        core.add(obj(TacviewRec::kBullet, 1, 1.0, 130));
        core.add(obj(TacviewRec::kGroundStatic, 1, 1.0, 140));
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 1.0, 150, 50, 300, 0, 0, 0));
        core.end_frame(1.0, o1, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon | TacviewRec::kSeriesGroundStatic);
        // The bomb and the rocket detonate (no event); the bullet is just gone; the building and the player die.
        core.add(marker(TacviewRec::kBomb, 1, 2.0, TacviewRec::kDetonated));
        core.add(marker(TacviewRec::kRocket, 2, 2.0, TacviewRec::kDetonated));
        core.add(marker(TacviewRec::kGroundStatic, 1, 2.0, TacviewRec::kDestroyed));
        core.add(marker(TacviewRec::kAircraft, 1, 2.0, TacviewRec::kDestroyed));
        core.add(obj(TacviewRec::kMissile, 3, 2.0, 121));
        core.end_frame(2.0, o2, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon | TacviewRec::kSeriesGroundStatic);
        const auto ls = lines_of(o2);
        auto line_with = [&](const std::string& piece) {
            for (const std::string& l : ls)
                if (l.find(piece) != std::string::npos) return l;
            return std::string();
        };
        expect(count_of(o2, "Type=Misc+Explosion") == 4 && count_of(o2, "Event=Destroyed") == 2,
               "explosion: one per detonated weapon, destroyed aircraft and destroyed ground object; events only for kills");
        const std::string bomb = line_with("5000001,T=");
        expect(bomb.find(",Type=Misc+Explosion,Radius=30") != std::string::npos, "explosion: bomb radius 30");
        expect(bomb.find("T=0.000898311|0.000449156|300") != std::string::npos,
               "explosion: at the bomb's last written position, position only");
        expect(line_with("5000002,T=").find("Radius=8") != std::string::npos, "explosion: rocket radius 8");
        expect(line_with("5000003,T=").find("Radius=25") != std::string::npos, "explosion: ground radius 25");
        expect(line_with("5000004,T=").find("Radius=20") != std::string::npos, "explosion: aircraft radius 20");
        expect(count_of(o2, "|Pitch") == 0 && bomb.find("Name=") == std::string::npos, "explosion: no name, no angles");
        expect(core.stats().explosions == 4, "explosion: counted");
        // Not due at 3.0; due at 4.0 (2 s after 2.0). A missile was removed with no marker: no explosion.
        core.add(obj(TacviewRec::kMissile, 3, 3.0, 122));
        core.end_frame(3.0, o3, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon | TacviewRec::kSeriesGroundStatic);
        expect(o3.find("-5000001") == std::string::npos, "explosion: still there after 1 s");
        core.add(obj(TacviewRec::kMissile, 3, 4.0, 123));
        core.end_frame(4.0, o4, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon | TacviewRec::kSeriesGroundStatic);
        expect(count_of(o4, "-500000") == 4 && count_of(o4, "-5000001" + std::string(1, 10)) == 1,
               "explosion: removed 2 s later");
        core.end_frame(5.0, o5, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon | TacviewRec::kSeriesGroundStatic);  // the missile vanishes with no reason: a removal and no explosion
        expect(o5.find("-1000003") != std::string::npos && o5.find("Explosion") == std::string::npos,
               "explosion: a plain removal writes none");
        // A frame listing every series removes explosions still pending.
        TacviewCore c2{TacviewCoreConfig{}};
        std::string p1, p2, p3;
        c2.add(obj(TacviewRec::kBomb, 1, 1.0, 100));
        c2.end_frame(1.0, p1, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon | TacviewRec::kSeriesGroundStatic);
        c2.add(marker(TacviewRec::kBomb, 1, 2.0, TacviewRec::kDetonated));
        c2.end_frame(2.0, p2, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon | TacviewRec::kSeriesGroundStatic);
        c2.end_frame(2.5, p3);
        expect(p3.find("-5000001") != std::string::npos, "explosion: the last frame removes pending explosions");
        // A core that continues a file after a crash exit numbers its explosions after the lost core's.
        TacviewCore c3{TacviewCoreConfig{}};
        c3.seed_explosions(c2.next_explosion());
        std::string q1, q2;
        c3.add(obj(TacviewRec::kBomb, 2, 3.0, 100));
        c3.end_frame(3.0, q1, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon | TacviewRec::kSeriesGroundStatic);
        c3.add(marker(TacviewRec::kBomb, 2, 3.5, TacviewRec::kDetonated));
        c3.end_frame(3.5, q2, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon | TacviewRec::kSeriesGroundStatic);
        expect(q2.find("5000002,T=") != std::string::npos && q2.find("5000001,T=") == std::string::npos,
               "explosion: a seeded core does not reuse the lost core's ids");
    }

    // The world walker's identity rules, over synthetic slot snapshots.
    {
        using namespace LazyWorks::SF2Patch::hooks;
        uint64_t next = 1;
        SlotMem m;
        GroundStep s = ground_step(m, 0x1000, true, -1, next);
        expect(s.present && s.is_new && m.seq == 1 && next == 2, "ground identity: a first sighting gets seq 1");
        s = ground_step(m, 0x1000, true, -1, next);
        expect(s.present && !s.is_new && m.seq == 1 && next == 2, "ground identity: the same pointer keeps its seq");
        s = ground_step(m, 0x2000, true, -1, next);
        expect(s.present && s.is_new && m.seq == 2, "ground identity: a new pointer in the slot gets a new seq");
        s = ground_step(m, 0x2000, false, 5, next);
        expect(!s.present && s.destroyed && s.dead_seq == 2 && !m.live,
               "ground identity: live to inactive at damage 5 writes the destroyed event");
        s = ground_step(m, 0x2000, false, 5, next);
        expect(!s.present && !s.destroyed, "ground identity: the event is written once");
        s = ground_step(m, 0x2000, true, -1, next);
        expect(s.present && s.is_new && m.seq == 3, "ground identity: inactive to live again gets a new seq");
        s = ground_step(m, 0x2000, false, 3, next);
        expect(!s.present && !s.destroyed, "ground identity: inactive at a lower damage level is only a removal");
        s = ground_step(m, 0x2000, true, -1, next);
        s = ground_step(m, 0x3000, false, 5, next);
        expect(!s.destroyed && !m.live, "ground identity: a replaced object gives no event");

        uint64_t cm_next = 1;
        SlotMem c;
        CounterMeasureStep cs = counter_measure_step(c, true, 3.5f, cm_next);
        expect(cs.present && cs.is_new && c.seq == 1, "counter-measure: first sighting");
        cs = counter_measure_step(c, true, 3.1f, cm_next);
        expect(cs.present && !cs.is_new && c.seq == 1, "counter-measure: life going down keeps the seq");
        cs = counter_measure_step(c, true, 3.8f, cm_next);
        expect(cs.present && cs.is_new && c.seq == 2, "counter-measure: life going up is a new instance in the same object");
        cs = counter_measure_step(c, false, 0.0f, cm_next);
        expect(!cs.present && !c.live, "counter-measure: inactive is not present");
        cs = counter_measure_step(c, true, 0.5f, cm_next);
        expect(cs.present && cs.is_new && c.seq == 3, "counter-measure: live again after a gap is new, even with less life");
    }

    // New classes in the core: ids, types, no rate limit, and series that are only removed in their own frames.
    {
        TacviewCore core{TacviewCoreConfig{}};
        std::string out;
        auto rec = [](uint8_t cls, uint64_t seq, AircraftSide side, const char* name, double t) {
            TacviewRec r = make_rec(seq, false, side, name, t, 100, 200, 5, 0.1f, 1.0f, 0.0f);
            r.obj_class = cls;
            return r;
        };
        core.add(rec(TacviewRec::kGroundStatic, 1, AircraftSide::Enemy, "SA-2 Site", 10.0));
        core.add(rec(TacviewRec::kGroundVehicle, 2, AircraftSide::Friendly, "M113,APC", 10.0));
        core.add(rec(TacviewRec::kShip, 3, AircraftSide::Neutral, "Frigate", 10.0));
        core.add(rec(TacviewRec::kFlare, 1, AircraftSide::Friendly, "", 10.0));
        core.add(rec(TacviewRec::kChaff, 2, AircraftSide::Unknown, "", 10.0));
        core.end_frame(10.0, out, TacviewRec::kSeriesGroundStatic | TacviewRec::kSeriesGroundMobile |
                                      TacviewRec::kSeriesCounterMeasure);
        const auto ls = lines_of(out);
        expect(ls.size() == 6, "tacview world: five objects");
        expect(ls[1].rfind("2000001,T=", 0) == 0 &&
                   ls[1].find(",Name=SA-2 Site,Type=Ground+Static+Building,Coalition=Enemies,Color=Red") != std::string::npos &&
                   ls[1].find("Pilot") == std::string::npos,
               "tacview world: building id 0x2000000+seq, type name, coalition");
        expect(ls[2].rfind("2000002,T=", 0) == 0 &&
                   ls[2].find(",Name=M113" + std::string(1, 92) + ",APC,Type=Ground+Vehicle,Coalition=Allies") != std::string::npos,
               "tacview world: vehicle type and escaped name");
        expect(ls[3].find(",Name=Frigate,Type=Sea+Watercraft,Coalition=Neutrals") != std::string::npos &&
                   ls[3].rfind("2000003,T=", 0) == 0,
               "tacview world: ship type");
        expect(ls[4].rfind("3000001,T=", 0) == 0 && ls[4].find("Type=Misc+Decoy+Flare") != std::string::npos &&
                   ls[4].find("Name=") == std::string::npos && ls[4].find("Coalition") == std::string::npos,
               "tacview world: flare id 0x3000000+seq, no name, no coalition");
        expect(ls[5].rfind("3000002,T=", 0) == 0 && ls[5].find("Type=Misc+Decoy+Chaff") != std::string::npos,
               "tacview world: chaff type");
        // Same seq in different families are different objects; there is no rate limit in the core for these.
        std::string o2;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 10.01, 0, 0, 100, 0, 0, 0));
        TacviewRec moved = rec(TacviewRec::kGroundStatic, 1, AircraftSide::Enemy, "SA-2 Site", 10.01);
        moved.pos[0] += 1;  // an unchanged pose would not be written again
        core.add(moved);
        core.end_frame(10.01, o2, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesGroundStatic);
        expect(o2.find("2000001,T=") != std::string::npos && o2.find("10001") == std::string::npos &&
                   o2.find("-2000002") == std::string::npos && o2.find("-3000001") == std::string::npos,
               "tacview world: the same seq in two families; unlisted series are not removed");
        // An aircraft frame (aircraft and weapons only) does not remove the ground objects.
        std::string o3;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 10.1, 0, 0, 100, 0, 0, 0));
        core.end_frame(10.1, o3, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon);
        expect(o3.find("-") == std::string::npos && core.live_objects() == 6, "tacview world: an aircraft frame keeps the rest");
        // A ground frame that lacks the ship removes it.
        std::string o4;
        core.add(rec(TacviewRec::kGroundVehicle, 2, AircraftSide::Friendly, "M113,APC", 11.0));
        core.end_frame(11.0, o4, TacviewRec::kSeriesGroundStatic | TacviewRec::kSeriesGroundMobile);
        expect(count_of(o4, "-2000003" + std::string(1, 10)) == 1 && count_of(o4, "-2000001" + std::string(1, 10)) == 1 &&
                   o4.find("-3000001") == std::string::npos,
               "tacview world: objects missing from a frame of their series are removed");
        // A destroyed building: event, then removal.
        std::string o5;
        TacviewRec d = rec(TacviewRec::kGroundVehicle, 2, AircraftSide::Unknown, "", 12.0);
        d.destroyed = 1;
        core.add(d);
        core.end_frame(12.0, o5, TacviewRec::kSeriesGroundStatic | TacviewRec::kSeriesGroundMobile);
        expect(o5.find("0,Event=Destroyed|2000002|") != std::string::npos && o5.find("-2000002") != std::string::npos,
               "tacview world: a destroyed vehicle gets the event and the removal");
    }

    // Tracer rounds: identity by prediction, angles from the velocity, the core's bullet type and parent.
    {
        using namespace LazyWorks::SF2Patch::hooks;
        uint64_t next = 1;
        SlotMem m;
        const float vel[3] = {800.0f, 0.0f, 0.0f};
        double pos[3] = {0, 0, 100};
        BulletStep s = bullet_step(m, true, pos, vel, 10.0, next);
        expect(s.present && s.is_new && m.seq == 1, "bullet identity: first sighting");
        pos[0] = 40.0;  // 0.05 s at 800 m/s
        s = bullet_step(m, true, pos, vel, 10.05, next);
        expect(s.present && !s.is_new && m.seq == 1, "bullet identity: on the predicted path keeps the seq");
        pos[0] = 40.0 + 100.0 + 250.0;  // gravity and drag are small: 350 m off is another round
        s = bullet_step(m, true, pos, vel, 10.10, next);
        expect(s.present && s.is_new && m.seq == 2, "bullet identity: more than 300 m from the prediction is a new round");
        pos[0] = 0;
        s = bullet_step(m, false, pos, vel, 10.15, next);
        expect(!s.present && !m.live, "bullet identity: inactive is not present");
        s = bullet_step(m, true, pos, vel, 10.20, next);
        expect(s.is_new && m.seq == 3, "bullet identity: live again after a gap is new");
        const double nanpos[3] = {std::nan(""), 0, 0};
        s = bullet_step(m, true, nanpos, vel, 10.25, next);
        s = bullet_step(m, true, pos, vel, 10.30, next);
        expect(s.is_new, "bullet identity: a non-finite position counts as a jump");

        float ang[3];
        const float north_up[3] = {0.0f, 100.0f, 100.0f};
        angles_from_velocity(north_up, ang);
        expect(std::fabs(ang[0] - 0.785398f) < 1e-4f && std::fabs(ang[1]) < 1e-5f && ang[2] == 0.0f,
               "bullet angles: north and climbing 45 degrees");
        const float east_down[3] = {50.0f, 0.0f, -50.0f};
        angles_from_velocity(east_down, ang);
        expect(std::fabs(ang[0] + 0.785398f) < 1e-4f && std::fabs(ang[1] - 1.570796f) < 1e-4f,
               "bullet angles: heading east, diving 45 degrees");
    }
    {
        using LazyWorks::SF2Patch::hooks::AircraftSide;
        TacviewCore core{TacviewCoreConfig{}};
        std::string out;
        auto bullet = [](uint64_t seq, uint64_t parent, uint8_t parent_cls, double t) {
            TacviewRec r = make_rec(seq, false, AircraftSide::Unknown, "", t, 10, 20, 30, 0.1f, 1.0f, 0.0f);
            r.obj_class = TacviewRec::kBullet;
            r.parent_seq = parent;
            r.parent_class = parent_cls;
            return r;
        };
        TacviewRec site = make_rec(7, false, AircraftSide::Enemy, "ZSU-23", 5.0, 0, 0, 0, 0, 0, 0);
        site.obj_class = TacviewRec::kGroundStatic;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 5.0, 0, 0, 100, 0, 0, 0));
        core.add(site);
        core.add(bullet(1, 1, TacviewRec::kAircraft, 5.0));
        core.add(bullet(2, 7, TacviewRec::kGroundStatic, 5.0));
        core.add(bullet(3, 0, TacviewRec::kAircraft, 5.0));
        core.add(bullet(4, 9, TacviewRec::kGroundStatic, 5.0));
        core.end_frame(5.0, out);
        const auto ls = lines_of(out);
        expect(ls.size() == 7, "tacview bullets: header-less frame with six objects");
        expect(ls[3].rfind("4000001,T=", 0) == 0 &&
                   ls[3].find(",Type=Projectile+Bullet,Coalition=Allies,Color=Blue,Parent=1") != std::string::npos &&
                   ls[3].find("Name=") == std::string::npos,
               "tacview bullets: id 0x4000000+seq, type, the player as parent, side from the parent");
        expect(ls[4].find(",Type=Projectile+Bullet,Coalition=Enemies,Color=Red,Parent=2000007") != std::string::npos,
               "tacview bullets: a ground object parent has an id in the ground range");
        expect(ls[5].find("Parent") == std::string::npos, "tacview bullets: no Parent when it could not be resolved");
        expect(ls[6].find("Parent=2000009") != std::string::npos, "tacview bullets: an unseen ground parent still gets an id");
        // Bullets are removed only by a frame of their series.
        std::string o2;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 5.1, 1, 0, 100, 0, 0, 0));
        core.end_frame(5.1, o2, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon);
        expect(o2.find("-4000001") == std::string::npos, "tacview bullets: an aircraft frame does not remove them");
        std::string o3;
        core.add(bullet(1, 1, TacviewRec::kAircraft, 5.2));
        core.end_frame(5.2, o3, TacviewRec::kSeriesBullet);
        expect(o3.find("-4000002") != std::string::npos && o3.find("-4000001") == std::string::npos &&
                   o3.find("-4000003") != std::string::npos,
               "tacview bullets: a bullet frame removes the rounds that are gone");
    }

    // Weapon type classification and the ring slot size.
    {
        using LazyWorks::SF2Patch::hooks::WeaponClass;
        using LazyWorks::SF2Patch::hooks::classify_weapon;
        expect(classify_weapon(0, 0) == WeaponClass::Bomb && classify_weapon(0, 4) == WeaponClass::Bomb &&
                   classify_weapon(1, 4) == WeaponClass::Bomb && classify_weapon(1, 5) == WeaponClass::Bomb &&
                   classify_weapon(1, 0) == WeaponClass::Rocket && classify_weapon(1, 10) == WeaponClass::Missile &&
                   classify_weapon(1, 13) == WeaponClass::Missile && classify_weapon(7, 0) == WeaponClass::Missile &&
                   classify_weapon(7, 9) == WeaponClass::Missile && classify_weapon(-5, 0) == WeaponClass::Missile,
               "weapon class: type 0 bomb, type 1 guidance 0 rocket, guided bombs bombs, other guided a missile");
        expect(sizeof(TacviewRec) == 168, "tacview: ring slot is 168 bytes");
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
        expect(zip_queue_wait_idle(10000), "tacview writer: the compressor finishes");
        std::string entry1;
        const std::string c1 = read_zip_entry(dir + L"\\SF2 2026-11-24 10-11-12.zip.acmi", entry1);
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
               "tacview writer: the file is named SF2 <date time>.zip.acmi and starts with the header");
        expect(entry1 == "SF2 2026-11-24 10-11-12.acmi" && !exists(dir + L"\\SF2 2026-11-24 10-11-12.acmi"),
               "tacview writer: one entry named <name>.acmi, and the plain file is gone");
        expect(tv_logged.find("tacview: packed the recording as ") != std::string::npos, "tacview writer: the compressor logs the packed file");
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
        zip_queue_wait_idle(10000);
        expect(exists(dir + L"\\SF2 2026-11-24 10-11-12 (2).zip.acmi"),
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
        zip_queue_wait_idle(10000);
        std::string entry3;
        const std::string c3 = read_zip_entry(dir + L"\\SF2 2026-11-24 10-11-40.zip.acmi", entry3);
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

        // close(true) (shutdown) never zips: the plain file stays. A stale temp file is deleted at the next start.
        {
            tv_logged.clear();
            SYSTEMTIME st5 = st;
            st5.wSecond = 50;
            const std::wstring stale = dir + L"\\old.zip.acmi.tmp";
            FILE* sf = nullptr;
            _wfopen_s(&sf, stale.c_str(), L"wb");
            if (sf) std::fclose(sf);
            TacviewWriter w5(64);
            expect(w5.open(dir, st5, "2026-11-24T20:15:00Z", cfg, tv_log, nullptr), "tacview shutdown: opens");
            while (w5.begin_frame(1) == false) Sleep(5);
            w5.slot(0) = make_rec(1, true, AircraftSide::Friendly, "P", 0.0, 1, 0, 100, 0, 0, 0);
            w5.commit_frame(1, 0.0);
            for (int i = 0; i < 200 && tv_logged.find("recording to") == std::string::npos; ++i) Sleep(10);
            expect(tv_logged.find("deleted 1 unfinished") != std::string::npos && !exists(stale),
                   "tacview zip: a stale .zip.acmi.tmp is deleted when a recording starts");
            w5.close(true, "shutdown test");
            for (int i = 0; i < 200 && !w5.idle(); ++i) Sleep(10);
            zip_queue_wait_idle(5000);
            expect(w5.idle() && exists(dir + L"\\SF2 2026-11-24 10-11-50.acmi") &&
                       !exists(dir + L"\\SF2 2026-11-24 10-11-50.zip.acmi"),
                   "tacview zip: close(true) skips the zip and keeps the plain file");
        }

        // Two recordings closing back to back: the writer is free at once, the compressor queue packs both.
        {
            tv_logged.clear();
            TacviewWriter w6(64);
            bool all_ok = true;
            for (unsigned k = 0; k < 2; ++k) {
                SYSTEMTIME s6 = st;
                s6.wSecond = static_cast<WORD>(20 + k);
                all_ok = all_ok && w6.open(dir, s6, "2026-11-24T20:15:00Z", cfg, tv_log, nullptr);
                for (int i = 0; i < 50; ++i) {
                    while (!w6.begin_frame(1)) Sleep(1);
                    w6.slot(0) = make_rec(1, true, AircraftSide::Friendly, "P", i * 0.1, i, 0, 100, 0, 0, 0);
                    w6.commit_frame(1, i * 0.1);
                }
                w6.close(false, "back to back");
                for (int i = 0; i < 500 && !w6.idle(); ++i) Sleep(2);
                all_ok = all_ok && w6.idle();
            }
            expect(all_ok, "tacview zip: the writer is idle again without waiting for the compressor");
            expect(zip_queue_wait_idle(10000), "tacview zip: the queue drains");
            std::string e1, e2;
            const std::string z1 = read_zip_entry(dir + L"\\SF2 2026-11-24 10-11-20.zip.acmi", e1);
            const std::string z2 = read_zip_entry(dir + L"\\SF2 2026-11-24 10-11-21.zip.acmi", e2);
            expect(!z1.empty() && !z2.empty() && e1 == "SF2 2026-11-24 10-11-20.acmi" && e2 == "SF2 2026-11-24 10-11-21.acmi" &&
                       !exists(dir + L"\\SF2 2026-11-24 10-11-20.acmi") && !exists(dir + L"\\SF2 2026-11-24 10-11-21.acmi") &&
                       count_of(tv_logged, "packed the recording as") == 2,
                   "tacview zip: two back-to-back recordings are both packed and logged");
        }

        // A zip that fails midway (the read stops after 1000 bytes): the plain file stays and the temp file is removed.
        {
            const std::wstring plain = dir + L"\\zt4.acmi";
            std::string big(300000, 'x');
            FILE* pf = nullptr;
            _wfopen_s(&pf, plain.c_str(), L"wb");
            if (pf) {
                std::fwrite(big.data(), 1, big.size(), pf);
                std::fclose(pf);
            }
            zip_test_fail_after(1000);
            std::wstring zp;
            uint64_t zb = 0;
            std::string err;
            const bool ok = compress_acmi(plain, zp, zb, err);
            zip_test_fail_after(0);
            expect(!ok && !err.empty() && exists(plain) && !exists(dir + L"\\zt4.zip.acmi") && !exists(dir + L"\\zt4.zip.acmi.tmp"),
                   "tacview zip: a mid-stream failure keeps the plain file and removes the temp file");
        }

        // The zip step on its own: streamed (a file larger than the read chunk), exact bytes, plain file gone.
        {
            std::string big;
            for (int i = 0; i < 120000; ++i) big += "#" + std::to_string(i) + "\n1,T=" + std::to_string(i * 7 % 1000) + "||\n";
            const std::wstring plain = dir + L"\\zt1.acmi";
            FILE* pf = nullptr;
            _wfopen_s(&pf, plain.c_str(), L"wb");
            if (pf) {
                std::fwrite(big.data(), 1, big.size(), pf);
                std::fclose(pf);
            }
            std::wstring zp;
            uint64_t zb = 0;
            std::string err;
            const bool ok = compress_acmi(plain, zp, zb, err);
            std::string en;
            const std::string back = ok ? read_zip_entry(zp, en) : std::string();
            expect(ok && zp == dir + L"\\zt1.zip.acmi" && en == "zt1.acmi" && back == big && !exists(plain) && zb > 0 && zb < big.size(),
                   "tacview zip: one entry named like the plain file, exact bytes, smaller, plain file deleted");

            // Failure: the temp name is taken by a folder. The plain file stays, an error is reported.
            const std::wstring plain2 = dir + L"\\zt2.acmi";
            _wfopen_s(&pf, plain2.c_str(), L"wb");
            if (pf) {
                std::fwrite("abc\n", 1, 4, pf);
                std::fclose(pf);
            }
            CreateDirectoryW((dir + L"\\zt2.zip.acmi.tmp").c_str(), nullptr);
            std::wstring zp2;
            expect(!compress_acmi(plain2, zp2, zb, err) && !err.empty() && exists(plain2) && !exists(dir + L"\\zt2.zip.acmi"),
                   "tacview zip: an unwritable target keeps the plain file and reports why");
            RemoveDirectoryW((dir + L"\\zt2.zip.acmi.tmp").c_str());

            // An existing zip with the target's name is never overwritten.
            const std::wstring plain3 = dir + L"\\zt3.acmi";
            _wfopen_s(&pf, plain3.c_str(), L"wb");
            if (pf) {
                std::fwrite("abc\n", 1, 4, pf);
                std::fclose(pf);
            }
            _wfopen_s(&pf, (dir + L"\\zt3.zip.acmi").c_str(), L"wb");
            if (pf) std::fclose(pf);
            expect(!compress_acmi(plain3, zp2, zb, err) && exists(plain3) && !exists(dir + L"\\zt3.zip.acmi.tmp"),
                   "tacview zip: an existing target keeps the plain file, no temp file left");
        }

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

// The Gameplay row splice, on a miniature of the stock OptionsScreen.ini shape.
void gameplay_row_splice_tests() {
    std::printf("gameplay_row_splice\n");
    const std::wstring ini =
        L"[OptionsScreen]\r\nMenuItem009=GameplayOptionsSubMenu\r\nMenuItem014=OtherOptionsSubMenu\r\n\r\n"
        L"[OtherOptionsButton]\r\nMenuItemType=TEXTBUTTON\r\nTextPosition=\r\nActive=FALSE\r\n\r\n"
        L"[GameplayOptionsSubMenu]\r\nMenuItemType=SUBMENU\r\nMenuItem001=MissionStartList\r\n"
        L"MenuItem016=GameplayTitleText\r\nMenuItem032=FuelUsageText\r\n"
        L"Position=550,180\r\nWidth=450\r\nHeight=460\r\nHasBackground=FALSE\r\nActive=FALSE\r\n\r\n"
        L"[MissionStartList]\r\nMenuItemType=LIST\r\nPosition=200,0\r\n\r\n"
        L"[OtherOptionsSubMenu]\r\nMenuItemType=SUBMENU\r\nMenuItem001=OtherTitleText\r\nMenuItem002=SectionVITitleText\r\n"
        L"Position=565,130\r\nWidth=362\r\nHeight=480\r\n";
    std::wstring t = ini;
    const int id = LazyWorks::SF2Patch::hooks::splice_gameplay_row(t);
    expect(id == 33, "row takes the next free Gameplay MenuItem ID");
    auto has = [&](const wchar_t* needle) { return t.find(needle) != std::wstring::npos; };
    expect(has(L"MenuItem032=FuelUsageText\r\nMenuItem033=TacviewRecordingList\r\nMenuItem034=TacviewRecordingText\r\n"
               L"Position=550,180\r\n"),
           "row items appended after the last Gameplay item, ids unique");
    expect(has(L"[TacviewRecordingList]\r\nMenuItemType=LIST\r\nPosition=200,400\r\nWidth=240\r\nHeight=22\r\n"
               L"ListFieldSetting=MediumListField\r\n") &&
               has(L"InfoTextStringID=TXT_LAZY_TACVIEW_INFO\r\n"),
           "list at 200,400, styled like the top rows");
    expect(has(L"[TacviewRecordingText]\r\nMenuItemType=TEXT\r\nTextSetting=MediumTextLeft\r\n"
               L"TextStringID=TXT_LAZY_TACVIEW_RECORDING\r\nTextPosition=0,403\r\n"),
           "text at 0,403");
    expect(has(L"[OtherOptionsSubMenu]\r\nMenuItemType=SUBMENU\r\nMenuItem001=OtherTitleText\r\nMenuItem002="
               L"SectionVITitleText\r\nPosition=565,130\r\n") &&
               has(L"[OtherOptionsButton]\r\nMenuItemType=TEXTBUTTON\r\nTextPosition=\r\nActive=FALSE\r\n") &&
               has(L"Position=550,180\r\nWidth=450\r\nHeight=460\r\n"),
           "Other tab, Other page and Gameplay geometry stay stock");
    const std::wstring once = t;
    expect(LazyWorks::SF2Patch::hooks::splice_gameplay_row(t) == 0 && t == once, "second splice is refused, text unchanged");
    std::wstring bad = L"[OptionsScreen]\r\n";
    expect(LazyWorks::SF2Patch::hooks::splice_gameplay_row(bad) == 0 && bad == L"[OptionsScreen]\r\n", "unexpected file left unchanged");
    std::wstring empty_page = L"[GameplayOptionsSubMenu]\r\nMenuItemType=SUBMENU\r\nPosition=550,180\r\n";
    const std::wstring empty_copy = empty_page;
    expect(LazyWorks::SF2Patch::hooks::splice_gameplay_row(empty_page) == 0 && empty_page == empty_copy,
           "page without MenuItem lines left unchanged");
}

namespace {
using LazyWorks::SF2Patch::acmi::Image;

// Synthetic images: a 2x2 picture with red top-left, green top-right, blue bottom-left, white bottom-right.
std::vector<uint8_t> make_tga(int type, int bpp, bool top_origin, bool rle) {
    const uint8_t px[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};  // as viewed, row by row
    std::vector<uint8_t> f = {0, 0, static_cast<uint8_t>(type), 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0,
                              static_cast<uint8_t>(bpp), static_cast<uint8_t>(top_origin ? 0x20 : 0)};
    const int order_top[4] = {0, 1, 2, 3}, order_bottom[4] = {2, 3, 0, 1};
    const int* order = top_origin ? order_top : order_bottom;
    auto put = [&](int i) {
        f.push_back(px[i][2]);
        f.push_back(px[i][1]);
        f.push_back(px[i][0]);
        if (bpp == 32) f.push_back(200);
    };
    if (!rle) {
        for (int i = 0; i < 4; ++i) put(order[i]);
    } else {
        f.push_back(0x80 | 0);  // a run of 1
        put(order[0]);
        f.push_back(0x02);  // 3 raw pixels
        for (int i = 1; i < 4; ++i) put(order[i]);
    }
    return f;
}

std::vector<uint8_t> make_bmp(int bpp, bool bottom_up, int w = 3, int h = 2) {
    // Pixel (x, y) as viewed has the value 10*y + x in the red channel (or index), green 7, blue 9.
    const int stride = ((w * bpp + 31) / 32) * 4;
    const int pal = bpp == 8 ? 256 * 4 : 0;
    const uint32_t off = 14 + 40 + pal;
    std::vector<uint8_t> f(off + static_cast<size_t>(stride) * h, 0xee);  // padding bytes are junk
    std::fill(f.begin(), f.begin() + off, uint8_t{0});
    auto w16 = [&](size_t p, uint16_t v) { f[p] = v & 0xff; f[p + 1] = v >> 8; };
    auto w32 = [&](size_t p, uint32_t v) { for (int i = 0; i < 4; ++i) f[p + i] = static_cast<uint8_t>(v >> (8 * i)); };
    f[0] = 'B';
    f[1] = 'M';
    w32(2, static_cast<uint32_t>(f.size()));
    w32(10, off);
    w32(14, 40);
    w32(18, static_cast<uint32_t>(w));
    w32(22, static_cast<uint32_t>(bottom_up ? h : -h));
    w16(26, 1);
    w16(28, static_cast<uint16_t>(bpp));
    for (int i = 0; i < pal / 4; ++i) {  // palette entry i: B=9, G=7, R=i
        f[54 + i * 4] = 9;
        f[54 + i * 4 + 1] = 7;
        f[54 + i * 4 + 2] = static_cast<uint8_t>(i);
        f[54 + i * 4 + 3] = 0;
    }
    for (int y = 0; y < h; ++y) {
        const int row = bottom_up ? h - 1 - y : y;
        uint8_t* p = &f[off + static_cast<size_t>(stride) * row];
        for (int x = 0; x < w; ++x) {
            const uint8_t v = static_cast<uint8_t>(10 * y + x);
            if (bpp == 8) {
                p[x] = v;
            } else {
                const int b = bpp / 8;
                p[x * b] = 9;
                p[x * b + 1] = 7;
                p[x * b + 2] = v;
                if (b == 4) p[x * b + 3] = 0;
            }
        }
    }
    return f;
}

Image solid(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    Image im;
    im.w = w;
    im.h = h;
    for (int i = 0; i < w * h; ++i) {
        im.px.push_back(r);
        im.px.push_back(g);
        im.px.push_back(b);
        im.px.push_back(255);
    }
    return im;
}

std::vector<uint8_t> utf16le(const std::wstring& s, bool bom = true) {
    std::vector<uint8_t> v;
    if (bom) {
        v.push_back(0xff);
        v.push_back(0xfe);
    }
    for (wchar_t c : s) {
        v.push_back(static_cast<uint8_t>(c & 0xff));
        v.push_back(static_cast<uint8_t>(c >> 8));
    }
    return v;
}

std::vector<uint8_t> ansi(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

void texture_tests() {
    using namespace LazyWorks::SF2Patch::acmi;
    std::printf("tacview texture\n");
    const auto pix = [](const Image& im, int x, int y, int c) { return im.px[(static_cast<size_t>(y) * im.w + x) * 4 + c]; };

    // TGA: both origins, uncompressed and RLE, 24 and 32 bpp. The picture as viewed is always the same.
    bool tga_ok = true;
    for (int bpp : {24, 32}) {
        for (bool top : {false, true}) {
            for (bool rle : {false, true}) {
                const auto f = make_tga(rle ? 10 : 2, bpp, top, rle);
                Image im;
                tga_ok &= decode_image(f.data(), f.size(), im) && im.w == 2 && im.h == 2 && pix(im, 0, 0, 0) == 255 &&
                          pix(im, 0, 0, 1) == 0 && pix(im, 1, 0, 1) == 255 && pix(im, 0, 1, 2) == 255 &&
                          pix(im, 1, 1, 0) == 255 && pix(im, 1, 1, 2) == 255 && pix(im, 0, 0, 3) == (bpp == 32 ? 200 : 255);
            }
        }
    }
    expect(tga_ok, "tga: uncompressed and RLE, 24 and 32 bpp, both origins decode top-down");
    {
        auto f = make_tga(2, 24, false, false);
        f[17] |= 0x10;  // right to left
        Image im;
        expect(decode_image(f.data(), f.size(), im) && pix(im, 1, 0, 0) == 255 && pix(im, 0, 0, 1) == 255,
               "tga: right-to-left origin bit");
    }
    {
        auto f = make_tga(2, 24, false, false);
        Image im;
        f.pop_back();
        expect(!decode_image(f.data(), f.size(), im), "tga: truncated pixel data rejected");
        auto r = make_tga(10, 24, true, true);
        r.resize(r.size() - 3);
        expect(!decode_image(r.data(), r.size(), im), "tga: truncated RLE rejected");
        auto big = make_tga(2, 24, true, false);
        big[12] = 0x01;
        big[13] = 0x20;  // width 8193
        expect(!decode_image(big.data(), big.size(), im), "tga: too wide rejected");
        auto z = make_tga(2, 24, true, false);
        z[12] = z[13] = 0;
        expect(!decode_image(z.data(), z.size(), im), "tga: zero width rejected");
        auto t = make_tga(3, 24, true, false);
        expect(!decode_image(t.data(), t.size(), im), "tga: unsupported type rejected");
        auto rl = make_tga(10, 24, true, true);
        rl[18] = 0x7f;  // a raw packet of 128 pixels in a 4-pixel image
        expect(!decode_image(rl.data(), rl.size(), im), "tga: RLE packet past the image rejected");
        expect(!decode_image(nullptr, 0, im) && !decode_image(f.data(), 5, im), "tga: empty and tiny input rejected");
    }

    // BMP: palettised, 24 and 32 bpp, both orientations; width 3 gives row padding.
    bool bmp_ok = true;
    for (int bpp : {8, 24, 32}) {
        for (bool up : {true, false}) {
            const auto f = make_bmp(bpp, up);
            Image im;
            bmp_ok &= decode_image(f.data(), f.size(), im) && im.w == 3 && im.h == 2 && pix(im, 0, 0, 0) == 0 &&
                      pix(im, 2, 0, 0) == 2 && pix(im, 0, 1, 0) == 10 && pix(im, 2, 1, 0) == 12 && pix(im, 1, 1, 1) == 7 &&
                      pix(im, 1, 1, 2) == 9 && pix(im, 1, 1, 3) == 255;
        }
    }
    expect(bmp_ok, "bmp: 8, 24 and 32 bpp, bottom-up and top-down, row padding");
    {
        auto f = make_bmp(24, true);
        Image im;
        f.resize(f.size() - 1);
        expect(!decode_image(f.data(), f.size(), im), "bmp: truncated pixel data rejected");
        auto c = make_bmp(24, true);
        c[30] = 1;  // RLE-compressed
        expect(!decode_image(c.data(), c.size(), im), "bmp: compressed rejected");
        auto b4 = make_bmp(24, true);
        b4[28] = 4;
        expect(!decode_image(b4.data(), b4.size(), im), "bmp: 4 bpp rejected");
        auto o = make_bmp(24, true);
        o[10] = o[11] = o[12] = 0xff;  // pixel offset far outside
        expect(!decode_image(o.data(), o.size(), im), "bmp: pixel offset outside the file rejected");
        auto hu = make_bmp(24, true);
        hu[20] = 0xff;
        hu[21] = 0x7f;  // width ~2 billion
        expect(!decode_image(hu.data(), hu.size(), im), "bmp: huge width rejected");
    }

    // rotate_cw on [a b; c d]
    {
        Image im = solid(2, 2, 0, 0, 0);
        const uint8_t v[4] = {1, 2, 3, 4};  // a b c d
        for (int i = 0; i < 4; ++i) im.px[i * 4] = v[i];
        Image r1, r2, r3, r0, r4;
        rotate_cw(im, 1, r1);
        rotate_cw(im, 2, r2);
        rotate_cw(im, 3, r3);
        rotate_cw(im, 0, r0);
        rotate_cw(im, 4, r4);
        auto at = [&](const Image& i, int x, int y) { return pix(i, x, y, 0); };
        expect(at(r1, 0, 0) == 3 && at(r1, 1, 0) == 1 && at(r1, 0, 1) == 4 && at(r1, 1, 1) == 2, "rotate_cw: one turn is [c a; d b]");
        expect(at(r2, 0, 0) == 4 && at(r2, 1, 1) == 1, "rotate_cw: two turns");
        expect(at(r3, 0, 0) == 2 && at(r3, 1, 0) == 4 && at(r3, 0, 1) == 1 && at(r3, 1, 1) == 3, "rotate_cw: three turns is [b d; a c]");
        expect(at(r0, 0, 0) == 1 && at(r4, 1, 1) == 4, "rotate_cw: zero and four turns leave it");
        Image wide = solid(3, 1, 0, 0, 0), rw;
        rotate_cw(wide, 1, rw);
        expect(rw.w == 1 && rw.h == 3, "rotate_cw: a non-square image swaps its sides");
    }

    // box_downsample
    {
        Image im = solid(4, 4, 0, 0, 0), out;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) im.px[(y * 4 + x) * 4] = static_cast<uint8_t>(x < 2 ? (y < 2 ? 10 : 20) : (y < 2 ? 30 : 40));
        box_downsample(im, 2, out);
        expect(out.w == 2 && out.h == 2 && pix(out, 0, 0, 0) == 10 && pix(out, 1, 0, 0) == 30 && pix(out, 0, 1, 0) == 20 &&
                   pix(out, 1, 1, 0) == 40,
               "box_downsample: 4x4 to 2x2");
        box_downsample(im, 1, out);
        expect(pix(out, 0, 0, 0) == 25, "box_downsample: the mean");
        box_downsample(im, 8, out);
        expect(out.w == 8 && pix(out, 0, 0, 0) == 10 && pix(out, 7, 7, 0) == 40, "box_downsample: a small source repeats pixels");
        Image none;
        box_downsample(none, 4, out);
        expect(out.w == 0 && out.px.empty(), "box_downsample: empty in, empty out");
    }

    // compose: 2x2 tiles of 2x2 pixels. Row 0 of the game's order is south.
    {
        std::vector<Image> tiles(3);
        tiles[1] = solid(2, 2, 100, 0, 0);
        tiles[2] = solid(2, 2, 0, 0, 0);
        tiles[2].px[0] = 200;  // top-left pixel red-ish: tile "2" has a marker at its top-left
        const std::vector<RGB> colours = {{9, 9, 9}, {1, 1, 1}, {2, 2, 2}, {3, 3, 3}};
        const uint16_t index[4] = {1, 2, 0, 3};  // (0,0)=1 (1,0)=2 (0,1)=0 (1,1)=3 in (tx, ty)
        const uint8_t rot[4] = {0, 1, 0, 0};     // tile (1,0) turned once
        RGBImage out;
        compose(2, 2, 2, index, rot, tiles, colours, out);
        const auto g = [&](int x, int y, int c) { return out.px[(static_cast<size_t>(y) * out.w + x) * 3 + c]; };
        expect(out.w == 4 && out.h == 4, "compose: size");
        expect(g(0, 2, 0) == 100 && g(1, 3, 0) == 100, "compose: tile (0,0) is at the bottom-left");
        expect(g(0, 0, 0) == 9 && g(3, 0, 0) == 3, "compose: tile (0,1) top-left and (1,1) top-right; no image uses its colour");
        // tile (1,0): bottom-right block (columns 2-3, rows 2-3), turned once clockwise: the top-left marker moves to the top-right.
        expect(g(3, 2, 0) == 200 && g(2, 2, 0) == 0 && g(2, 3, 0) == 0 && g(3, 3, 0) == 0, "compose: rotation r turns the tile clockwise");
        const uint16_t bad[4] = {50, 50, 50, 50};
        compose(2, 2, 2, bad, rot, tiles, colours, out);
        expect(g(0, 0, 0) == 128, "compose: an index out of range gives grey");
        compose(0, 2, 2, index, rot, tiles, colours, out);
        expect(out.px.empty(), "compose: no tiles, no image");
    }

    // paint_runway: 100 m pixels, a 100 x 100 image covering 10 km square.
    {
        RGBImage img;
        img.w = img.h = 100;
        img.px.assign(100 * 100 * 3, 200);
        // Centre (5000, 5000): row 50, column 50. Heading 90: along east-west.
        paint_runway(img, 100.0, 5000, 5000, kPi / 2, 1000, 90);
        const auto painted = [&](int x, int y) { return img.px[(static_cast<size_t>(y) * 100 + x) * 3] == 64; };
        int count = 0;
        for (size_t i = 0; i < img.px.size(); i += 3) count += img.px[i] == 64;
        expect(painted(50, 49) && painted(50, 50), "paint_runway: the centre is painted");
        expect(painted(46, 49) && painted(53, 49) && !painted(40, 49) && !painted(60, 49), "paint_runway: heading 90 is a horizontal stripe");
        expect(!painted(50, 45) && !painted(50, 55), "paint_runway: not vertical");
        expect(count >= 20 && count <= 24, "paint_runway: a 90 m runway is painted 2 pixels wide at 100 m per pixel");
        RGBImage v;
        v.w = v.h = 100;
        v.px.assign(100 * 100 * 3, 200);
        paint_runway(v, 100.0, 5000, 5000, 0.0, 1000, 90);  // heading 0: north-south
        expect(v.px[(46 * 100 + 49) * 3] == 64 && v.px[(54 * 100 + 49) * 3] == 64 && v.px[(50 * 100 + 40) * 3] == 200,
               "paint_runway: heading 0 is a vertical stripe");
        RGBImage n;
        n.w = n.h = 100;
        n.px.assign(100 * 100 * 3, 200);
        paint_runway(n, 100.0, 5000, 9500, 0.0, 2000, 90);  // runs off the north edge: row 0 is north
        expect(n.px[(0 * 100 + 49) * 3] == 64 && n.px[(99 * 100 + 49) * 3] == 200, "paint_runway: row 0 is north, clipped at the edge");
        paint_runway(n, 100.0, 1e9, 1e9, 0.0, 10, 10);
        paint_runway(n, 100.0, std::nan(""), 0, 0.0, 10, 10);
        expect(true, "paint_runway: far away and non-finite input do nothing");
    }

    // ini
    {
        const auto u16 = utf16le(L"[TargetArea001]\r\nName=Ramstein Airbase\r\nPosition=1000.5,2000\r\n; comment\r\n[Other]\r\nKey = Value  ; note\r\n");
        const Ini a = parse_ini(u16.data(), u16.size());
        const auto s1 = a.get("targetarea001", "NAME"), s2 = a.get("OTHER", "key");
        expect(s1 && *s1 == "Ramstein Airbase" && s2 && *s2 == "Value", "ini: UTF-16LE with a BOM, case-insensitive, trimmed, comments cut");
        const auto an = ansi(std::string("[A]\r\nName=Zweibr") + "ü" + "cken\r\n");
        const Ini b = parse_ini(an.data(), an.size());
        const auto s3 = b.get("a", "name");
        expect(s3 && *s3 == "Zweibr\xc3\xbc" "cken", "ini: ANSI text read as Latin-1 into UTF-8");
        const auto eu = utf16le(std::wstring(L"[A]\r\nName=Zweibr") + wchar_t(0xfc) + L"cken\r\n");
        const Ini c = parse_ini(eu.data(), eu.size());
        expect(c.get("A", "Name") && *c.get("A", "Name") == "Zweibr\xc3\xbc" "cken", "ini: UTF-16 non-ASCII becomes UTF-8");
        expect(a.get("nope", "x") == nullptr && a.get("other", "x") == nullptr, "ini: missing section or key is null");
        double v[2];
        expect(parse_numbers("-1066.00, 2392.5", v, 2) && v[0] == -1066.0 && v[1] == 2392.5 && !parse_numbers("1", v, 2) &&
                   !parse_numbers("x,2", v, 2),
               "ini: number lists");
    }

    // names: cp1252 and UTF-8 sources reach ACMI text as UTF-8
    {
        using LazyWorks::SF2Patch::acmi::append_escaped;
        const auto by = [](std::initializer_list<int> v) {
            std::string s;
            for (int c : v) s += static_cast<char>(c);
            return s;
        };
        const auto nm = [&](const std::string& cp1252) {
            const auto b = ansi(by({'[', 'A', ']', 13, 10, 'N', 'a', 'm', 'e', '='}) + cp1252 + by({13, 10}));
            const Ini i = parse_ini(b.data(), b.size());
            const std::string* v = i.get("a", "name");
            return v ? *v : std::string("<none>");
        };
        expect(nm(by({'M', 0xf6, 'n', 'c', 'h', 'h', 'o', 'f'})) == by({'M', 0xc3, 0xb6, 'n', 'c', 'h', 'h', 'o', 'f'}),
               "names: cp1252 o-umlaut becomes UTF-8");
        expect(nm(by({0xc6, 'r', 0xf8})) == by({0xc3, 0x86, 'r', 0xc3, 0xb8}), "names: cp1252 AE and o-slash become UTF-8");
        expect(nm(by({'Z', 0xfc, 'r', 'i', 'c', 'h'})) == by({'Z', 0xc3, 0xbc, 'r', 'i', 'c', 'h'}),
               "names: cp1252 u-umlaut becomes UTF-8");
        expect(nm(by({'E', 0x80})) == by({'E', 0xe2, 0x82, 0xac}), "names: cp1252 0x80 is the euro sign");
        expect(nm(by({'Z', 0xc3, 0xbc, 'r'})) == by({'Z', 0xc3, 0xbc, 'r'}), "names: UTF-8 without a BOM is not encoded twice");
        std::string o;
        append_escaped(o, by({'B', 0xc3, 0xbc, 'c', 'k', ',', ' ', 0xc3, 0x84}).c_str());
        expect(o == by({'B', 0xc3, 0xbc, 'c', 'k', 92, ',', ' ', 0xc3, 0x84}),
               "names: valid UTF-8 passes through the ACMI escape, comma escaped");
        o.clear();
        append_escaped(o, by({'a', 0xfc, 'b', 0xc3, 'c', 0xc0, 0x80, 0xed, 0xa0, 0x80, 10}).c_str());
        expect(o == "a?b?c??????", "names: invalid bytes and control characters become '?'");
        o.clear();
        append_escaped(o, by({'a', 0xc3}).c_str());
        expect(o == "a?", "names: a truncated UTF-8 sequence becomes '?'");
        o.clear();
        append_escaped(o, by({0xf0, 0x9f, 0x98, 0x80, 'x'}).c_str());
        expect(o == by({0xf0, 0x9f, 0x98, 0x80, 'x'}), "names: a 4-byte UTF-8 sequence passes through");
        o.clear();
        append_escaped(o, by({0xf0, 0x9f, 0x98}).c_str());
        expect(o == "???", "names: a truncated 4-byte sequence becomes '?' per byte");
        o.clear();
        LazyWorks::SF2Patch::acmi::append_utf8_cp(o, 0x1f600);
        LazyWorks::SF2Patch::acmi::append_utf8_cp(o, 0xd800);
        LazyWorks::SF2Patch::acmi::append_utf8_cp(o, 0xfc);
        expect(o == by({0xf0, 0x9f, 0x98, 0x80, '?', 0xc3, 0xbc}), "names: append_utf8_cp (what copy_name uses) writes 4-byte, rejects surrogates");
        expect(nm("Plain Name") == "Plain Name", "names: an ASCII-only ini is unchanged");
    }

    // airfields
    {
        const auto targets = utf16le(
            L"[TargetArea001]\r\nName=Ramstein Airbase\r\nPosition=100000,200000\r\nAirfieldDataFile=af_one.ini\r\n"
            L"Target[001].Type=Runway1\r\nTarget[001].Offset=0,0\r\nTarget[001].Heading=90\r\n"
            L"Target[002].Type=Tower1\r\nTarget[002].Heading=45\r\n\r\n"
            L"[TargetArea002]\r\nName=USS Somebody\r\nPosition=5,5\r\nAirfieldDataFile=carrier.ini\r\nCarrierStation=1\r\n\r\n"
            L"[TargetArea003]\r\nName=Far Away\r\nPosition=5,5\r\nAirfieldDataFile=off.ini\r\nOffMapAirBase=TRUE\r\n\r\n"
            L"[TargetArea004]\r\nName=Town\r\nPosition=5,5\r\n\r\n"
            L"[TargetArea005]\r\nName=Missing File\r\nPosition=300,400\r\nAirfieldDataFile=gone.ini\r\n"
            L"Target[001].Type=Runway2\r\nTarget[001].Heading=30\r\n");
        const Ini t = parse_ini(targets.data(), targets.size());
        int loads = 0;
        const AirfieldLoader loader = [&](const std::string& name) -> std::vector<uint8_t> {
            ++loads;
            if (name != "af_one.ini") return {};
            return ansi("[Runway001]\r\nHeading=0\r\nOffset=0,1000\r\nLength=2500\r\nWidth=90\r\n"
                        "[Runway002]\r\nHeading=-30\r\nOffset=100,0\r\nLength=1200\r\nWidth=60\r\n");
        };
        const auto af = airfields_from(t, loader);
        expect(af.size() == 2 && af[0].name == "Ramstein Airbase" && af[1].name == "Missing File", "airfields: carriers, off-map bases and non-airfields skipped");
        expect(loads == 2, "airfields: only airfields' inis are loaded");
        const double deg = kPi / 180;
        const Airfield& a = af[0];
        expect(a.x == 100000 && a.y == 200000 && a.runways.size() == 2, "airfields: position and two runways");
        // A = 90 degrees, offset (0, 1000): centre = position + (1000, 0).
        expect(std::fabs(a.runways[0].x - 101000) < 1e-6 && std::fabs(a.runways[0].y - 200000) < 1e-6, "airfields: the runway offset is turned by the area heading");
        expect(std::fabs(a.runways[0].heading - 90 * deg) < 1e-9 && a.runways[0].length == 2500 && a.runways[0].width == 90, "airfields: heading is area plus ini, size as given");
        // Offset (100, 0) at A = 90: (x cosA + y sinA, -x sinA + y cosA) = (0, -100).
        expect(std::fabs(a.runways[1].x - 100000) < 1e-6 && std::fabs(a.runways[1].y - 199900) < 1e-6 &&
                   std::fabs(a.runways[1].heading - 60 * deg) < 1e-9,
               "airfields: the second runway");
        expect(std::fabs(a.heading - 90 * deg) < 1e-9, "airfields: the airfield heading is the first runway's");
        expect(af[1].runways.empty() && std::fabs(af[1].heading - 30 * deg) < 1e-9 && af[1].x == 300, "airfields: an unreadable ini keeps the airfield, with the area heading");
        // height_at: a 3x2 grid, south row first, 500 m apart.
        const uint16_t grid[6] = {0, 100, 200, 1000, 1100, 1200};
        expect(height_at(grid, 3, 2, 500.0, 0, 0) == 0 && height_at(grid, 3, 2, 500.0, 1000, 500) == 1200, "height_at: the corner samples");
        expect(std::fabs(height_at(grid, 3, 2, 500.0, 250, 250) - 550) < 1e-9 && std::fabs(height_at(grid, 3, 2, 500.0, 750, 0) - 150) < 1e-9,
               "height_at: interpolates between the four nearest samples");
        expect(height_at(grid, 3, 2, 500.0, -1, 0) == 0 && height_at(grid, 3, 2, 500.0, 1001, 0) == 0 && height_at(nullptr, 3, 2, 500.0, 0, 0) == 0,
               "height_at: outside the grid, or no grid, is 0");
        const Ini none = parse_ini(nullptr, 0);
        expect(airfields_from(none, loader).empty(), "airfields: an empty ini has none");
    }

    // texture xml
    {
        const std::string xml = texture_xml("LazySF2Patch.T", "LazySF2Patch-T.jpg", 4000, 4000, 1000000.0, 1000000.0, -25.0, -4.5, -4.5);
        const auto m = make_mapping(-4.5);
        const std::string east = num(-25.0 + 1000000.0 * m.lon_scale, 9), north = num(-4.5 + 1000000.0 * m.lat_scale, 9);
        auto has = [&](const std::string& piece) { return xml.find(piece) != std::string::npos; };
        auto corner = [&](const char* tag, const std::string& lon, const std::string& lat) {
            return has(std::string("<") + tag + ">\n\t\t\t\t<Longitude>" + lon + "</Longitude>\n\t\t\t\t<Latitude>" + lat + "</Latitude>");
        };
        expect(has("<CustomTextureList>") && has("<CustomTexture Layer=\"Real World\" Id=\"LazySF2Patch.T\">") &&
                   has("<File>LazySF2Patch-T.jpg</File>") && has("<Visible>true</Visible>") && has("<Projection>Quad</Projection>"),
               "texture xml: structure");
        expect(corner("BottomLeft", "-25", "-4.5") && corner("BottomRight", east, "-4.5") && corner("TopRight", east, north) &&
                   corner("TopLeft", "-25", north),
               "texture xml: corners are the outer edges of the extent");
    }

    // runway patches
    {
        auto rwy = [](double x, double y, double heading, double len, double wid) {
            Runway r;
            r.x = x; r.y = y; r.heading = heading; r.length = len; r.width = wid;
            return r;
        };
        auto rgb = [](const RGBImage& im, int col, int row) { return &im.px[(static_cast<size_t>(row) * im.w + col) * 3]; };
        auto is = [](const uint8_t* p, int r, int g, int b) { return p[0] == r && p[1] == g && p[2] == b; };

        // patch_tiles: snapped outward to whole tiles, clamped to the field
        Airfield a;
        a.x = 5100; a.y = 5100;
        a.runways.push_back(rwy(5100, 5100, 0.0, 100, 20));
        PatchTiles pt;
        expect(patch_tiles(a, 2000.0, 10, 10, pt), "patch_tiles: an airfield with a runway has a patch");
        expect(pt.tx0 == 2 && pt.ty0 == 2 && pt.nx == 2 && pt.ny == 2 && pt.x0 == 4000 && pt.y0 == 4000 && pt.w_m == 4000 && pt.h_m == 4000,
               "patch_tiles: the 2000 m square 4100..6100 snaps outward to tiles 2 and 3");
        a.runways[0] = rwy(5100, 5100, 0.0, 6000, 20);
        expect(patch_tiles(a, 2000.0, 10, 10, pt) && pt.tx0 == 0 && pt.nx == 5 && pt.ny == 5,
               "patch_tiles: a long runway widens the patch (about 6400 m, then outward)");
        Airfield corner;
        corner.x = 500; corner.y = 19500;
        corner.runways.push_back(rwy(500, 19500, 0.0, 100, 20));
        expect(patch_tiles(corner, 2000.0, 10, 10, pt) && pt.tx0 == 0 && pt.nx == 1 && pt.ty0 == 9 && pt.ny == 1 && pt.x0 == 0 && pt.y0 == 18000,
               "patch_tiles: clamped to the tile field");
        Airfield outside;
        outside.x = -9000; outside.y = 5000;
        outside.runways.push_back(rwy(-9000, 5000, 0.0, 100, 20));
        expect(!patch_tiles(outside, 2000.0, 10, 10, pt), "patch_tiles: nothing inside the field, no patch");
        Airfield none;
        none.x = 1; none.y = 1;
        expect(!patch_tiles(none, 2000.0, 10, 10, pt), "patch_tiles: no runways, no patch");

        // compose_patch: the 2 x 2 tiles (1,1)..(2,2) of a 3 x 3 field at native size, one of them turned
        std::vector<Image> tiles(3);
        tiles[1] = solid(4, 4, 100, 0, 0);
        tiles[2] = solid(4, 4, 0, 100, 0);
        tiles[2].px[0] = 200;  // a marker at the top-left of texture 2
        const std::vector<RGB> colours = {{9, 9, 9}, {1, 1, 1}, {2, 2, 2}};
        const uint16_t field[9] = {0, 0, 0, 0, 1, 2, 0, 2, 0};  // row 0 south
        const uint8_t frot[9] = {0, 0, 0, 0, 0, 1, 0, 0, 0};
        PatchTiles reg;
        reg.tx0 = 1; reg.ty0 = 1; reg.nx = 2; reg.ny = 2;
        RGBImage patch;
        compose_patch(reg, 3, 4, field, frot, tiles, colours, patch);
        expect(patch.w == 8 && patch.h == 8, "compose_patch: native size, 2 x 2 tiles of 4 px");
        expect(rgb(patch, 0, 4)[0] == 100 && rgb(patch, 3, 7)[0] == 100, "compose_patch: tile (1,1) is bottom-left");
        expect(rgb(patch, 0, 0)[0] == 200 && rgb(patch, 1, 0)[0] == 0 && rgb(patch, 1, 0)[1] == 100, "compose_patch: tile (1,2) is top-left, unturned");
        expect(rgb(patch, 4, 0)[0] == 9 && rgb(patch, 7, 3)[0] == 9, "compose_patch: tile (2,2) has no image: its colour");
        // tile (2,1) is bottom-right, turned once clockwise: the marker moves from the top-left to the top-right
        expect(rgb(patch, 7, 4)[0] == 200 && rgb(patch, 4, 4)[0] == 0 && rgb(patch, 4, 7)[0] == 0, "compose_patch: a rotated tile");
        expect(rgb(patch, 4, 4)[1] == 100, "compose_patch: the rotated tile keeps its colour");

        // draw_runways: an opaque patch, 8 m per pixel (a 2000 m square), heading 0, 3000 m x 90 m runway
        Airfield r0;
        r0.x = 1000; r0.y = 1000;
        r0.runways.push_back(rwy(1000, 1000, 0.0, 1200, 90));
        RGBImage im;
        im.w = im.h = 250;
        im.px.assign(static_cast<size_t>(250) * 250 * 3, 77);
        draw_runways(im, r0, 0, 0, 2000, 2000);
        expect(is(rgb(im, 0, 0), 77, 77, 77) && is(rgb(im, 249, 249), 77, 77, 77), "draw_runways: the ground is left alone");
        const int cx = 125, cy = 125;
        auto asphalt = [&](int c, int r) { return is(rgb(im, c, r), 58, 58, 60); };
        expect(asphalt(cx - 3, cy - 60) && asphalt(cx + 3, cy - 60) && is(rgb(im, cx - 12, cy), 77, 77, 77) && is(rgb(im, cx + 12, cy), 77, 77, 77),
               "draw_runways: a vertical stripe about 90 m wide");
        expect(is(rgb(im, cx, cy - 80), 77, 77, 77) && is(rgb(im, cx, cy + 80), 77, 77, 77) && asphalt(cx + 2, cy - 70),
               "draw_runways: the stripe is as long as the runway");
        int edges = 0;
        for (int c = 0; c < im.w; ++c)
            if (is(rgb(im, c, cy - 40), 200, 200, 200)) ++edges;
        expect(edges == 2, "draw_runways: one edge-line pixel along each long side");
        int centre = 0, dashed = 0;
        for (int r = cy - 70; r < cy + 70; ++r) {
            bool any = false;
            for (int c = cx - 1; c <= cx; ++c) any = any || is(rgb(im, c, r), 230, 230, 230);
            (any ? centre : dashed)++;
        }
        expect(centre > 40 && dashed > 20, "draw_runways: the centre line is dashed");
        Airfield e;
        e.x = 1000; e.y = 1000;
        e.runways.push_back(rwy(1000, 1000, kPi / 2, 1000, 20));
        RGBImage ew;
        ew.w = ew.h = 250;
        ew.px.assign(static_cast<size_t>(250) * 250 * 3, 77);
        draw_runways(ew, e, 0, 0, 2000, 2000);
        expect(is(rgb(ew, cx - 40, cy - 1), 58, 58, 60) || is(rgb(ew, cx - 40, cy), 58, 58, 60),
               "draw_runways: heading 90 is a horizontal stripe (a narrow runway is drawn at least 3 px)");
        expect(is(rgb(ew, cx, cy - 10), 77, 77, 77) && is(rgb(ew, cx, cy + 10), 77, 77, 77) && is(rgb(ew, cx - 70, cy), 77, 77, 77),
               "draw_runways: heading 90 stays narrow and short");
        RGBImage nonsquare;
        nonsquare.w = 200; nonsquare.h = 100;
        nonsquare.px.assign(static_cast<size_t>(200) * 100 * 3, 77);
        Airfield ns;
        ns.runways.push_back(rwy(2000, 500, 0.0, 600, 60));
        draw_runways(nonsquare, ns, 0, 0, 4000, 1000);  // 20 m per px across, 10 m per px up
        expect(is(rgb(nonsquare, 100, 50), 58, 58, 60) && is(rgb(nonsquare, 100, 10), 77, 77, 77) && is(rgb(nonsquare, 100, 90), 77, 77, 77),
               "draw_runways: a rectangular patch has its own scale on each axis");

        // the size cap and the tile pixel budget
        int fw = 0, fh = 0;
        fit_patch_size(1024, 1024, fw, fh);
        expect(fw == 1024 && fh == 1024, "fit_patch_size: a patch that fits is unchanged");
        fit_patch_size(4096, 3072, fw, fh);
        expect(fw == 2048 && fh == 1536, "fit_patch_size: shrunk to 2048 on the long side, same shape");
        RGBImage big, shrunk;
        big.w = 4; big.h = 2;
        big.px = {10, 10, 10, 30, 30, 30, 50, 50, 50, 70, 70, 70, 10, 10, 10, 30, 30, 30, 50, 50, 50, 70, 70, 70};
        resize_box_rgb(big, 2, 1, shrunk);
        expect(shrunk.w == 2 && shrunk.h == 1 && shrunk.px[0] == 20 && shrunk.px[3] == 60, "resize_box_rgb: box mean");
        resize_box_rgb(big, 8, 8, shrunk);
        expect(shrunk.px.empty(), "resize_box_rgb: never enlarges");
        expect(patch_tile_px(1024, 4, kPatchCacheBytes) == 512, "patch_tile_px: capped at 512");
        expect(patch_tile_px(256, 4, kPatchCacheBytes) == 256, "patch_tile_px: a smaller native size is kept");
        // 64 MB holds 64 tiles of 512 px as RGBA (1 MiB each), 256 of 256 px
        expect(patch_tile_px(1024, 64, kPatchCacheBytes) == 512 && patch_tile_px(1024, 65, kPatchCacheBytes) == 256,
               "patch_tile_px: halved when the set exceeds the budget");
        expect(patch_tile_px(1024, 257, kPatchCacheBytes) == 128 && patch_tile_px(1024, 100000, kPatchCacheBytes) == 64,
               "patch_tile_px: halved again, never below 64");
        expect(patch_tile_px(32, 100000, kPatchCacheBytes) == 32, "patch_tile_px: a tiny native size is not enlarged");
        expect(patch_tile_px(512, 64, kPatchCacheBytes) * patch_tile_px(512, 64, kPatchCacheBytes) * 4u * 64u <= kPatchCacheBytes,
               "patch_tile_px: the bytes held fit the budget");
        expect(patch_compose_px(512, 3, 2) == 512 && patch_compose_px(512, 10, 10) == 204 && patch_compose_px(512, 5000, 1) == 1,
               "patch_compose_px: a big patch is composed from smaller tiles, never below 1 px");
        {
            std::vector<Image> cache(4);
            cache[0] = solid(512, 512, 10, 20, 30);
            cache[1] = solid(256, 256, 40, 50, 60);
            cache[3] = solid(512, 512, 70, 80, 90);  // [2] stays empty: a texture that did not decode
            const int sz = unify_tile_size(cache, 512);
            expect(sz == 256 && cache[0].w == 256 && cache[0].h == 256 && cache[3].w == 256 && cache[2].w == 0 && cache[1].w == 256,
                   "unify_tile_size: all tiles brought to the smallest size, an empty one stays empty");
            expect(cache[0].px[0] == 10 && cache[3].px[1] == 80 && cache[1].px[2] == 60, "unify_tile_size: the colours are kept");
            std::vector<Image> empty_cache(2);
            expect(unify_tile_size(empty_cache, 128) == 128, "unify_tile_size: nothing decoded gives the cap");
            std::vector<Image> capped(1);
            capped[0] = solid(64, 64, 1, 2, 3);
            expect(unify_tile_size(capped, 32) == 32 && capped[0].w == 32, "unify_tile_size: a tile above the cap is brought down to it");
        }

        const std::vector<PatchEntry> extra = {{"LazySF2Patch.T.AF001", "LazySF2Patch-T-AF001.jpg", 1000, 2000, 3000, 3000},
                                               {"LazySF2Patch.T.AF002", "LazySF2Patch-T-AF002.jpg", 5000, 6000, 2000, 2000}};
        const std::string xml = texture_xml("LazySF2Patch.T", "LazySF2Patch-T.jpg", 4000, 4000, 1000000.0, 1000000.0, -25.0, -4.5, -4.5, extra);
        const auto m = make_mapping(-4.5);
        auto has = [&](const std::string& piece) { return xml.find(piece) != std::string::npos; };
        const size_t base = xml.find("Id=\"LazySF2Patch.T\""), p1 = xml.find("Id=\"LazySF2Patch.T.AF001\""), p2 = xml.find("Id=\"LazySF2Patch.T.AF002\"");
        expect(base != std::string::npos && base < p1 && p1 < p2, "texture xml: base entry first, then the patches in order");
        expect(has("<File>LazySF2Patch-T-AF001.jpg</File>") && has("<File>LazySF2Patch-T-AF002.jpg</File>"), "texture xml: patch files");
        const std::string bl_lon = num(-25.0 + 1000 * m.lon_scale, 9), bl_lat = num(-4.5 + 2000 * m.lat_scale, 9);
        const std::string tr_lon = num(-25.0 + 4000 * m.lon_scale, 9), tr_lat = num(-4.5 + 5000 * m.lat_scale, 9);
        const std::string first = xml.substr(p1, xml.find("</CustomTexture>", p1) - p1);
        const size_t i_bl = first.find("<BottomLeft>"), i_br = first.find("<BottomRight>"), i_tr = first.find("<TopRight>"), i_tl = first.find("<TopLeft>");
        expect(i_bl < i_br && i_br < i_tr && i_tr < i_tl, "texture xml: patch corner order");
        expect(first.find("<Longitude>" + bl_lon + "</Longitude>\n\t\t\t\t<Latitude>" + bl_lat + "</Latitude>", i_bl) < i_br &&
                   first.find("<Longitude>" + tr_lon + "</Longitude>\n\t\t\t\t<Latitude>" + tr_lat + "</Latitude>", i_tr) < i_tl,
               "texture xml: patch corners from the world square");
        expect(has("<Visible>true</Visible>") && has("<Projection>Quad</Projection>"), "texture xml: patch structure");
    }

    // The base texture in bands of tile rows (north first), runways painted per band, is the image composed whole.
    {
        std::vector<Image> tiles(3);
        for (int k = 0; k < 3; ++k) {
            tiles[k].w = tiles[k].h = 4;
            tiles[k].px.resize(64);
            for (size_t i = 0; i < 64; ++i) tiles[k].px[i] = static_cast<uint8_t>(k * 50 + i);
        }
        const std::vector<RGB> colours = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
        const uint16_t field[15] = {0, 1, 2, 2, 1, 0, 1, 1, 2, 0, 0, 2, 1, 2, 0};  // 3 wide, 5 high, row 0 south
        const uint8_t frot[15] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2};
        const double mpp = 2000.0 / 4;
        RGBImage whole;
        compose(3, 5, 4, field, frot, tiles, colours, whole);
        paint_runway(whole, mpp, 3000, 4100, 0.3, 3000, 600);  // crosses the band edge at 4000 m
        std::vector<uint8_t> banded;
        for (int top = 5; top > 0; top -= 2) {
            PatchTiles pt;
            pt.ty0 = (std::max)(0, top - 2);
            pt.nx = 3;
            pt.ny = top - pt.ty0;
            RGBImage band;
            compose_patch(pt, 3, 4, field, frot, tiles, colours, band);
            paint_runway(band, mpp, 3000, 4100 - pt.ty0 * 2000.0, 0.3, 3000, 600);
            banded.insert(banded.end(), band.px.begin(), band.px.end());
        }
        expect(!whole.px.empty() && banded == whole.px,
               "base texture bands: the same pixels as the whole image, runway across a band edge");
    }

    // The size of an existing texture, from its header.
    {
        const std::vector<uint8_t> j = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x04, 0x4a, 0x46, 0xff, 0xc4, 0x00, 0x03, 0x00,
                                        0xff, 0xc0, 0x00, 0x11, 0x08, 0x1f, 0x40, 0x0f, 0xa0, 0x03};
        int w = 0, h = 0;
        expect(jpeg_size(j.data(), j.size(), w, h) && w == 4000 && h == 8000, "jpeg size: from the frame header, after other segments");
        std::vector<uint8_t> prog = j;
        prog[14] = 0xc2;
        expect(jpeg_size(prog.data(), prog.size(), w, h) && w == 4000, "jpeg size: progressive");
        std::vector<uint8_t> scan = j;
        scan[14] = 0xda;
        expect(!jpeg_size(scan.data(), scan.size(), w, h), "jpeg size: a scan before any frame header");
        std::vector<uint8_t> dht = j;
        dht[14] = 0xc4;  // a Huffman table, not a frame header
        expect(!jpeg_size(dht.data(), dht.size(), w, h), "jpeg size: DHT is not a frame header");
        expect(!jpeg_size(j.data(), 18, w, h), "jpeg size: cut short");
        const uint8_t png[4] = {0x89, 'P', 'N', 'G'};
        expect(!jpeg_size(png, 4, w, h) && !jpeg_size(nullptr, 0, w, h), "jpeg size: not a JPEG");
    }

    // The files of the export before the rename: exact shapes only.
    {
        expect(is_pre_rename_file("LazySF2-GermanyCE.jpg", ".jpg") && is_pre_rename_file("lazysf2-GermanyCE-AF001.JPG", ".jpg") &&
                   is_pre_rename_file("LazySF2-GermanyCE.xml", ".xml") && is_pre_rename_file("LazySF2-GermanyCE.raw", ".raw"),
               "pre-rename: LazySF2- names with the extension, any case");
        expect(!is_pre_rename_file("LazySF2Patch-GermanyCE.jpg", ".jpg") && !is_pre_rename_file("LazySF2Patch-GermanyCE.xml", ".xml"),
               "pre-rename: LazySF2Patch- names are never matched");
        expect(!is_pre_rename_file("LazySF2-.jpg", ".jpg") && !is_pre_rename_file("LazySF2-GermanyCE.jpg", ".xml") &&
                   !is_pre_rename_file("LazySF2-GermanyCE.jpg.bak", ".jpg") && !is_pre_rename_file("MyLazySF2-x.jpg", ".jpg") &&
                   !is_pre_rename_file("LazySF2-a\\b.jpg", ".jpg") && !is_pre_rename_file("LazySF2-a/b.jpg", ".jpg") &&
                   !is_pre_rename_file("LazySF2.GermanyCE.jpg", ".jpg"),
               "pre-rename: nothing else (empty stem, other extension, suffix, prefix, path, dotted)");
    }
}
}  // namespace

void aerodrome_core_tests() {
    using namespace LazyWorks::SF2Patch::acmi;
    using LazyWorks::SF2Patch::hooks::AircraftSide;
    std::printf("tacview aerodromes\n");
    TacviewCoreConfig cfg;
    cfg.ref_lat = -4.5;
    cfg.ref_lon = -25.0;
    TacviewCore core{cfg};
    std::string out;
    auto aero = [](uint64_t seq, const char* name, double x, double y, double z, float heading) {
        TacviewRec r = make_rec(seq, false, AircraftSide::Friendly, name, 5.0, x, y, z, 0.0f, heading, 0.0f);
        r.obj_class = TacviewRec::kAerodrome;
        return r;
    };
    core.add(aero(1, "Ramstein, Airbase", 100000, 200000, 250, static_cast<float>(kPi / 2)));
    core.add(aero(2, "Sembach", 1000, 2000, 0, 0.0f));
    core.end_frame(5.0, out, TacviewRec::kSeriesAerodrome);
    auto ls = lines_of(out);
    const auto m = make_mapping(-4.5);
    expect(ls.size() == 3 && ls[0] == "#0.000", "aerodromes: one frame with both");
    expect(ls[1].rfind("6000001,T=" + num(100000 * m.lon_scale, 9) + "|" + num(200000 * m.lat_scale, 9) + "|250|", 0) == 0,
           "aerodromes: id 0x6000000+seq, position from the world metres, altitude");
    expect(ls[1].find("|250|0|0|90|100000|200000|90,Name=") != std::string::npos,
           "aerodromes: the heading is written");
    expect(ls[1].find(",Name=Ramstein" + std::string(1, 92) + ", Airbase,Type=Ground+Static+Aerodrome") != std::string::npos &&
               ls[1].find("Coalition") == std::string::npos && ls[1].find("Color") == std::string::npos,
           "aerodromes: name escaped, type Ground+Static+Aerodrome, no coalition");
    expect(ls[2].rfind("6000002,T=", 0) == 0, "aerodromes: the second id");

    // Written once: frames of other series neither repeat nor remove them.
    std::string o2;
    core.add(make_rec(1, true, AircraftSide::Friendly, "P", 6.0, 0, 0, 100, 0, 0, 0));
    core.end_frame(6.0, o2, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon);
    expect(o2.find("6000001") == std::string::npos && o2.find("-6000001") == std::string::npos, "aerodromes: other frames leave them alone");
    std::string o3;
    core.end_frame(7.0, o3, TacviewRec::kSeriesGroundStatic | TacviewRec::kSeriesGroundMobile | TacviewRec::kSeriesBullet);
    expect(o3.find("6000001") == std::string::npos, "aerodromes: a ground-only frame leaves them alone too");
    // The last frame lists every series: it removes them.
    std::string o4;
    core.end_frame(8.0, o4);
    expect(o4.find("-6000001\n") != std::string::npos && o4.find("-6000002\n") != std::string::npos,
           "aerodromes: the final frame removes them");
    expect(core.live_objects() == 0, "aerodromes: none left");
    static_assert(TacviewRec::series_of(TacviewRec::kAerodrome) == TacviewRec::kSeriesAerodrome, "series");
    static_assert(TacviewRec::kSeriesAerodrome != TacviewRec::kSeriesBullet && (TacviewRec::kSeriesAerodrome & 0x3f) == 0, "series bit is its own");
}

void structure_tests() {
    using namespace LazyWorks::SF2Patch::acmi;
    using LazyWorks::SF2Patch::hooks::AircraftSide;
    std::printf("tacview structures\n");
    const double deg = kPi / 180;
    const auto targets = utf16le(
        L"[TargetArea001]\r\nName=Base\r\nPosition=100000,200000\r\nAirfieldDataFile=af.ini\r\n"
        L"Target[001].Type=Runway1\r\nTarget[001].Offset=0,0\r\nTarget[001].Heading=90\r\n"
        L"Target[002].Type=Hangar1\r\nTarget[002].Offset=0,1000\r\nTarget[002].Heading=0\r\n"
        L"Target[003].Type=AAA\r\nTarget[003].Offset=5,5\r\nTarget[003].Heading=0\r\n"
        L"Target[004].Type=Unlisted1\r\nTarget[004].Offset=100,0\r\nTarget[004].Heading=-90\r\n\r\n"
        L"[TargetArea002]\r\nName=USS Somebody\r\nPosition=5,5\r\nCarrierStation=TRUE\r\n"
        L"Target[001].Type=Hangar1\r\nTarget[001].Offset=0,0\r\n\r\n"
        L"[TargetArea003]\r\nName=Far Away\r\nPosition=5,5\r\nOffMapAirBase=1\r\n"
        L"Target[001].Type=Hangar1\r\nTarget[001].Offset=0,0\r\n\r\n"
        L"[TargetArea004]\r\nName=River\r\nPosition=5000,6000\r\nRotateToRunwayHeading=0\r\n"
        L"Target[001].Type=Runway2\r\nTarget[001].Heading=90\r\n"
        L"Target[002].Type=vBridge1\r\nTarget[002].Offset=10,20\r\nTarget[002].Heading=45\r\n\r\n"
        L"[TargetArea005]\r\nName=Village\r\nPosition=1000,1000\r\n"
        L"Target[001].Type=hangar1\r\nTarget[001].Offset=-100,50\r\nTarget[001].Heading=30\r\n");
    const auto types = ansi(
        "[TargetType001]\r\nName=Hangar1\r\nFullName=Hangar\r\nTargetType=HANGAR\r\nUseGroundObject=FALSE\r\n"
        "[TargetType002]\r\nName=AAA\r\nFullName=AAA Site\r\nTargetType=AAA\r\nUseGroundObject=TRUE\r\n"
        "[TargetType003]\r\nName=Runway1\r\nFullName=Runway\r\nTargetType=MEDIUM_RUNWAY\r\nUseGroundObject=FALSE\r\n"
        "[TargetType004]\r\nName=vBridge1\r\nFullName=Paul Doumer Bridge\r\nTargetType=BRIDGE\r\nUseGroundObject=FALSE\r\n");
    const Ini t = parse_ini(targets.data(), targets.size());
    const Ini ty = parse_ini(types.data(), types.size());
    const auto placed = structures_from(t, ty);
    const auto& s = placed.items;
    expect(s.size() == 4, "structures: runways, live types, carriers and off-map bases left out");
    if (s.size() == 4) {
        // A = 90 (the runway's heading): offset (0, 1000) turns to (+1000, 0), the heading to 90.
        expect(s[0].name == "Hangar" && std::fabs(s[0].x - 101000) < 1e-6 && std::fabs(s[0].y - 200000) < 1e-6 &&
                   std::fabs(s[0].heading - 90 * deg) < 1e-9 && !s[0].bridge,
               "structures: an airfield's object is turned by the area heading, named by FullName");
        // Offset (100, 0) at A = 90: (0, -100); heading 90 - 90 = 0.
        expect(s[1].name == "Unlisted1" && std::fabs(s[1].x - 100000) < 1e-6 && std::fabs(s[1].y - 199900) < 1e-6 &&
                   std::fabs(s[1].heading) < 1e-9,
               "structures: a type the types ini does not list keeps its type id");
        expect(s[2].bridge && s[2].name == "Paul Doumer Bridge" && std::fabs(s[2].x - 5010) < 1e-6 && std::fabs(s[2].y - 6020) < 1e-6 &&
                   std::fabs(s[2].heading - 45 * deg) < 1e-9,
               "structures: RotateToRunwayHeading=0 keeps offsets and headings as given; a BRIDGE type is a bridge");
        expect(s[3].name == "Hangar" && std::fabs(s[3].x - 900) < 1e-6 && std::fabs(s[3].y - 1050) < 1e-6 &&
                   std::fabs(s[3].heading - 30 * deg) < 1e-9,
               "structures: an area without a runway has heading 0; type names match without case");
    }
    const Ini none = parse_ini(nullptr, 0);
    const auto bare = structures_from(t, none).items;
    expect(bare.size() == 5 && bare[1].name == "AAA" && !bare[3].bridge && bare[3].name == "vBridge1",
           "structures: without a types ini, only runways (by name) are left out");
    expect(structures_from(none, ty).items.empty() && structures_from(none, ty).areas.empty(), "structures: an empty targets ini has none");

    const auto theatre = ansi("[Terrain]\r\nTargetFile=desert_targets.ini\r\nTargetTypeFile=desert_types.ini\r\n");
    const Ini th = parse_ini(theatre.data(), theatre.size());
    expect(types_file_name(th, "x_TARGETS.INI") == "desert_types.ini", "types file: the theatre ini names it");
    expect(types_file_name(none, "GermanyCE_targets.INI") == "GermanyCE_TYPES.INI" && types_file_name(none, "foo.ini").empty(),
           "types file: else from the targets ini's name, or none");

    // The core: dedupe against live ground objects, the line format, written once, removed only by the final frame.
    TacviewCoreConfig cfg;
    cfg.ref_lat = -4.5;
    cfg.ref_lon = -25.0;
    TacviewCore core{cfg};
    auto structure = [](uint64_t seq, const char* name, double x, double y, float heading, bool bridge) {
        TacviewRec r = make_rec(seq, false, AircraftSide::Unknown, name, 2.0, x, y, 0, 0.0f, heading, 0.0f);
        r.obj_class = bridge ? TacviewRec::kBridge : TacviewRec::kStructure;
        return r;
    };
    std::string out;
    TacviewRec gun = make_rec(7, false, AircraftSide::Enemy, "AAA", 1.0, 1000, 1000, 0, 0, 0, 0);
    gun.obj_class = TacviewRec::kGroundStatic;
    TacviewRec truck = make_rec(8, false, AircraftSide::Enemy, "Truck", 1.0, 3000, 3000, 0, 0, 0, 0);
    truck.obj_class = TacviewRec::kGroundVehicle;
    core.add(gun);
    core.add(truck);
    core.end_frame(1.0, out, TacviewRec::kSeriesGroundStatic | TacviewRec::kSeriesGroundMobile);
    std::string o1;
    core.add(structure(1, "Hangar", 1010, 1000, 0.0f, false));          // 10 m from the gun: the game's own object
    core.add(structure(2, "Hangar", 1020, 1000, static_cast<float>(kPi / 2), false));  // 20 m: kept
    core.add(structure(3, "Bridge", 5000, 5000, 0.0f, true));
    core.add(structure(4, "Shed", 3005, 3000, 0.0f, false));            // next to a vehicle: kept
    core.end_frame(2.0, o1, 0);
    const auto m = make_mapping(-4.5);
    const std::string want2 = "7000002,T=" + num(1020 * m.lon_scale, 9) + "|" + num(1000 * m.lat_scale, 9) +
                              "|0|0|0|90|1020|1000|90,Name=Hangar,Type=Ground+Static+Building";
    const auto l1 = lines_of(o1);
    expect(l1.size() == 4 && l1[0] == "#1.000" && l1[1] == want2, "structures: one line each, id 0x7000000+seq, Ground+Static+Building, no coalition");
    expect(o1.find("7000001,") == std::string::npos, "structures: one within 15 m of a live ground object is skipped");
    expect(o1.find("7000003,T=") != std::string::npos && o1.find(",Name=Bridge,Type=Ground+Static+Bridge\n") != std::string::npos,
           "structures: a bridge is Ground+Static+Bridge");
    expect(o1.find("7000004,T=") != std::string::npos, "structures: vehicles do not hide them");
    // The next chunk: a repeat writes nothing; a new one is added; the earlier ones stay.
    std::string o2;
    core.add(structure(2, "Hangar", 1020, 1000, static_cast<float>(kPi / 2), false));
    core.add(structure(5, "Barn", 8000, 8000, 0.0f, false));
    core.end_frame(3.0, o2, 0);
    expect(count_of(o2, "7000002") == 0 && count_of(o2, "\n7000005,T=") == 1 && o2.find("-700000") == std::string::npos,
           "structures: written once; a later chunk does not remove the earlier ones");
    expect(core.stats().structures == 4 && core.stats().structures_skipped == 1, "structures: the counts");
    // Other series' frames leave them alone, even when the gun goes.
    std::string o3;
    core.add(make_rec(1, true, AircraftSide::Friendly, "P", 4.0, 0, 0, 100, 0, 0, 0));
    core.end_frame(4.0, o3, TacviewRec::kSeriesAircraft | TacviewRec::kSeriesWeapon);
    core.end_frame(5.0, o3, TacviewRec::kSeriesGroundStatic | TacviewRec::kSeriesGroundMobile | TacviewRec::kSeriesAerodrome);
    expect(o3.find("-2000007\n") != std::string::npos && o3.find("-700000") == std::string::npos,
           "structures: frames of other series never remove them");
    std::string o4;
    core.end_frame(6.0, o4);
    expect(count_of(o4, "\n-700000") + (o4.rfind("-700000", 0) == 0 ? 1 : 0) == 4 && core.live_objects() == 0,
           "structures: the final frame removes all of them");
    static_assert(TacviewRec::series_of(TacviewRec::kStructure) == TacviewRec::kSeriesStructure &&
                      TacviewRec::series_of(TacviewRec::kBridge) == TacviewRec::kSeriesStructure,
                  "series");
    static_assert((TacviewRec::kSeriesStructure & 0x7f) == 0 && (TacviewRec::kSeriesAll & TacviewRec::kSeriesStructure) != 0,
                  "the structure series bit is its own, and the final frame names it");

    // Areas: radius, index, and the game's rotation rule (airfield areas only, targets after a runway only).
    expect(placed.areas.size() == 3 && s.size() == 4 && s[0].area == 0 && s[1].area == 0 && s[2].area == 1 && s[3].area == 2,
           "structures: each belongs to its area; carriers and off-map bases have no area");
    expect(placed.areas.size() == 3 && std::fabs(placed.areas[0].radius - 1000) < 1e-9 &&
               std::fabs(placed.areas[2].radius - std::hypot(100.0, 50.0)) < 1e-9 && placed.areas[1].x == 5000,
           "structures: an area's radius reaches its farthest placed object");
    {
        const auto t2b = utf16le(
            L"[TargetArea001]\r\nName=Strip\r\nPosition=0,0\r\nRadius=5000\r\nAirfieldDataFile=x.ini\r\n"
            L"Target[003].Type=Hangar1\r\nTarget[003].Offset=0,100\r\nTarget[003].Heading=0\r\n"
            L"Target[001].Type=Hangar1\r\nTarget[001].Offset=0,100\r\nTarget[001].Heading=0\r\n"
            L"Target[002].Type=RwyMod\r\nTarget[002].Heading=90\r\n\r\n"
            L"[TargetArea002]\r\nName=Town\r\nPosition=1000,0\r\n"
            L"Target[001].Type=Runway1\r\nTarget[001].Heading=90\r\n"
            L"Target[002].Type=Hangar1\r\nTarget[002].Offset=0,100\r\nTarget[002].Heading=0\r\n");
        const auto ty2b = ansi("[TargetType001]\r\nName=Hangar1\r\nFullName=Hangar\r\nTargetType=HANGAR\r\n"
                               "[TargetType002]\r\nName=RwyMod\r\nTargetType=SMALL_RUNWAY\r\n"
                               "[TargetType003]\r\nName=Runway1\r\nTargetType=MEDIUM_RUNWAY\r\n");
        const Ini t2 = parse_ini(t2b.data(), t2b.size());
        const Ini ty2 = parse_ini(ty2b.data(), ty2b.size());
        const auto p2 = structures_from(t2, ty2);
        const auto& i2 = p2.items;
        expect(i2.size() == 3 && p2.areas.size() == 2 && p2.areas[0].radius == 5000, "structures: a runway by category is left out; Radius kept when larger");
        if (i2.size() == 3) {
            expect(std::fabs(i2[0].x) < 1e-9 && std::fabs(i2[0].y - 100) < 1e-9 && std::fabs(i2[0].heading) < 1e-12,
                   "structures: a target before the runway is not turned (targets by number, not file order)");
            expect(std::fabs(i2[1].x - 100) < 1e-6 && std::fabs(i2[1].y) < 1e-6 && std::fabs(i2[1].heading - 90 * deg) < 1e-9,
                   "structures: a target after it is turned by its heading");
            expect(std::fabs(i2[2].x - 1000) < 1e-9 && std::fabs(i2[2].y - 100) < 1e-9 && std::fabs(i2[2].heading) < 1e-12,
                   "structures: an area without an airfield is never turned");
        }
        const AirfieldLoader none_loader = [](const std::string&) { return std::vector<uint8_t>{}; };
        const auto af2 = airfields_from(t2, none_loader, &ty2);
        const auto af3 = airfields_from(t2, none_loader);
        expect(af2.size() == 1 && std::fabs(af2[0].heading - 90 * deg) < 1e-9 && af3.size() == 1 && std::fabs(af3[0].heading) < 1e-12,
               "airfields: the runway by category with the types ini, by name without it");
    }

    // Picking areas: the game's own set, else near the player or holding a live ground object.
    {
        const TargetArea areas[3] = {{0, 0, 1000}, {50000, 0, 1000}, {100000, 0, 500}};
        uint8_t on[3] = {9, 9, 9};
        const double active[1][2] = {{50000.5, 0}};
        AreaPick pk;
        pk.use_active = true;
        pk.active = active;
        pk.n_active = 1;
        pk.have_player = true;  // ignored with the game's set
        expect(pick_areas(areas, 3, pk, on) == 1 && on[0] == 0 && on[1] == 1 && on[2] == 0, "pick: exactly the game's populated areas");
        pk.n_active = 0;
        expect(pick_areas(areas, 3, pk, on) == 0, "pick: an empty game set picks none");
        const double ground[1][2] = {{100000, 500}};
        AreaPick fb;
        fb.have_player = true;
        fb.player[0] = 0;
        fb.player[1] = 31000;  // exactly radius + range
        fb.range = 30000;
        fb.ground = ground;
        fb.n_ground = 1;
        expect(pick_areas(areas, 3, fb, on) == 2 && on[0] == 1 && on[1] == 0 && on[2] == 1,
               "pick: fallback by the player's reach and by a live ground object inside the radius");
        fb.player[1] = 31001;
        fb.n_ground = 0;
        expect(pick_areas(areas, 3, fb, on) == 0, "pick: beyond the reach, no ground objects: none");
    }

    expect(utf8_prefix("abc", 2) == 2 && utf8_prefix("a\xc3\xbc", 2) == 1 && utf8_prefix("a\xc3\xbc", 3) == 3 &&
               utf8_prefix("\xe2\x82\xac" "x", 2) == 0,
           "utf8_prefix: never cuts a character");
    expect(TacviewWriter::ring_fits(0, 0, 64, 63) && !TacviewWriter::ring_fits(0, 0, 64, 64) &&
               TacviewWriter::ring_fits(10, 0, 64, 53) && !TacviewWriter::ring_fits(10, 0, 64, 54) &&
               TacviewWriter::ring_fits(5, 0xfffffffeu, 64, 56) && !TacviewWriter::ring_fits(5, 0xfffffffeu, 64, 57),
           "tacview writer: ring_fits at the boundary, and across the index wrap");

    // The 15 m boundary, markers and repeats of a skipped one.
    {
        TacviewCore c2{cfg};
        std::string ob;
        TacviewRec g2 = make_rec(1, false, AircraftSide::Enemy, "AAA", 1.0, 0, 0, 0, 0, 0, 0);
        g2.obj_class = TacviewRec::kGroundStatic;
        c2.add(g2);
        c2.end_frame(1.0, ob, TacviewRec::kSeriesGroundStatic);
        c2.add(structure(1, "A", 15.0, 0, 0.0f, false));
        c2.add(structure(2, "B", 15.001, 0, 0.0f, false));
        TacviewRec dead = structure(3, "C", 9000, 0, 0.0f, false);
        dead.destroyed = TacviewRec::kDestroyed;
        c2.add(dead);
        std::string oc;
        c2.end_frame(2.0, oc, 0);
        c2.add(structure(1, "A", 15.0, 0, 0.0f, false));
        c2.end_frame(3.0, oc, 0);
        expect(oc.find("7000001,") == std::string::npos && oc.find("7000002,T=") != std::string::npos &&
                   oc.find("7000003") == std::string::npos && c2.stats().structures == 1,
               "structures: 15 m is still the game's object, just over it is not; a marker or a repeat writes nothing");
    }

    // A destroyed structure: the event, an explosion, its removal; only once.
    {
        TacviewCore c3{cfg};
        std::string o;
        c3.add(structure(4, "Fuel Tank", 2000, 3000, 0.0f, false));
        c3.add(structure(5, "Bridge", 2500, 3000, 0.0f, true));
        c3.end_frame(1.0, o, 0);
        std::string od;
        TacviewRec boom = structure(4, "", 0, 0, 0.0f, false);
        boom.destroyed = TacviewRec::kDestroyed;
        boom.t = 2.0;
        c3.add(boom);
        TacviewRec bridge_boom = structure(5, "", 0, 0, 0.0f, false);
        bridge_boom.obj_class = TacviewRec::kStructure;  // the marker's class need not say bridge: one family
        bridge_boom.destroyed = TacviewRec::kDestroyed;
        c3.add(bridge_boom);
        c3.end_frame(2.0, od, 0);
        const size_t ev = od.find("0,Event=Destroyed|7000004|"), rm = od.find("-7000004\n");
        expect(ev != std::string::npos && rm != std::string::npos && ev < rm && od.find("Type=Misc+Explosion") != std::string::npos &&
                   od.find("0,Event=Destroyed|7000005|") != std::string::npos && od.find("-7000005\n") != std::string::npos,
               "structures: a destroyed one gets its Destroyed event, an explosion and its removal");
        std::string oe;
        c3.add(boom);
        c3.end_frame(3.0, oe, 0);
        std::string of;
        c3.end_frame(9.0, of);
        expect(oe.find("7000004") == std::string::npos && of.find("7000004") == std::string::npos && of.find("7000005") == std::string::npos &&
                   c3.stats().structures_destroyed == 2 && c3.live_objects() == 0,
               "structures: destroyed once; the final frame does not remove it again");
    }
    {
        const double pts[4][2] = {{0, 0}, {10, 0}, {20, 0}, {10, 0.5}};
        const uint8_t taken[4] = {0, 1, 0, 0};
        expect(find_near(pts, 4, 10.4, 0, 2.0, nullptr) == 1 && find_near(pts, 4, 10.1, 0.4, 2.0, nullptr) == 3 &&
                   find_near(pts, 4, 10.4, 0, 2.0, taken) == 3 && find_near(pts, 4, 0.5, 0, 2.0, taken) == 0 &&
                   find_near(pts, 4, 50, 0, 2.0, nullptr) == 4 && find_near(pts, 0, 0, 0, 2.0, nullptr) == 0,
               "find_near: the nearest within the tolerance, never a taken one; n when none");
        // The destroy hotkey: the nearest standing structure at any distance; none when all are skipped.
        const uint8_t all[4] = {1, 1, 1, 1};
        expect(find_near(pts, 4, 1e6, 0, HUGE_VAL, nullptr) == 2 && find_near(pts, 4, 1e6, 0, HUGE_VAL, taken) == 2 &&
                   find_near(pts, 4, 9.0, 0.6, HUGE_VAL, taken) == 3 && find_near(pts, 4, 1e6, 0, HUGE_VAL, all) == 4,
               "find_near: an infinite tolerance takes the nearest free point at any distance");
    }

    TacviewWriter w(64);
    expect(!w.can_take(1), "tacview writer: can_take is false while nothing is open");
}

void placement_tests() {
    using namespace LazyWorks::SF2Patch::acmi;
    const Placement none = place_theatre(false, 10.817, 51.111, 1000000.0, 1000000.0);
    expect(none.ref_lon == -25.0 && none.ref_lat == -4.5 && none.scale_lat == -4.5, "placement: no geo is the default frame");

    const Placement p = place_theatre(true, 10.817, 51.111, 1000000.0, 1000000.0);
    const Mapping m = make_mapping(51.111);
    expect(p.scale_lat == 51.111, "placement: scale latitude is the ini latitude");
    expect(std::fabs(m.lon_scale - 1.0 / 69895.4) < 3e-9,  // 111320 * cos(51.111 deg) = 69895.4 m
           "placement: lon_scale uses cos(51.111)");
    expect(std::fabs(p.ref_lon + 500000.0 * m.lon_scale - 10.817) < 1e-6 &&
               std::fabs(p.ref_lat + 500000.0 * m.lat_scale - 51.111) < 1e-6,
           "placement: the square's centre maps back to the geo point");
    expect(p.ref_lon < 10.817 && p.ref_lat < 51.111, "placement: the corner is south-west of the centre");

    expect(geo_is_set(10.817, 51.111), "geo: a real point is set");
    expect(!geo_is_set(-96.8, 30.2) && !geo_is_set(-96.8 + 1e-7, 30.2 - 1e-7), "geo: the game default pair is not set");
    expect(!geo_is_set(static_cast<double>(-96.8f), static_cast<double>(30.2f)), "geo: the float-widened game default is not set");
    expect(geo_is_set(-96.8 + 1e-3, 30.2), "geo: near the default but not float precision is set");
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    expect(!geo_is_set(nan, 10) && !geo_is_set(10, nan) && !geo_is_set(inf, 10) && !geo_is_set(10, -inf),
           "geo: NaN and infinity are not set");
    expect(!geo_is_set(181, 10) && !geo_is_set(10, 86) && !geo_is_set(10, -86), "geo: out of range is not set");
    const Placement edge = place_theatre(true, 179.9, 85.0, 1000000.0, 1000000.0);
    expect(edge.ref_lon >= -180.0 && edge.ref_lon <= 180.0 && edge.ref_lat >= -89.0, "placement: the corner is clamped");

    const std::string txml = texture_xml("LazySF2Patch.T", "LazySF2Patch-T.jpg", 8, 8, 1000.0, 1000.0, p.ref_lon, p.ref_lat,
                                         p.scale_lat);
    expect(txml.find("<Longitude>" + num(p.ref_lon + 1000.0 * m.lon_scale, 9) + "</Longitude>") != std::string::npos &&
               txml.find("<Longitude>" + num(p.ref_lon + 1000.0 * make_mapping(p.ref_lat).lon_scale, 9) + "</Longitude>") ==
                   std::string::npos,
           "placement: texture_xml uses the scale latitude, not the corner latitude");

    const std::string xml = terrain_xml("LazySF2Patch.T", "LazySF2Patch-T.raw", 2, 2, 1000.0, p.ref_lon, p.ref_lat, p.scale_lat);
    expect(xml.find(num(p.ref_lon + 1000.0 * m.lon_scale, 9)) != std::string::npos,
           "placement: terrain_xml uses the scale latitude");
}

void pilot_twist_tests() {
    namespace pt = LazyWorks::SF2Patch::hooks::pilot_twist;
    std::printf("pilot twist\n");
    auto close_to = [](double a, double b, double eps) { return std::fabs(a - b) <= eps; };
    const float d = pt::kDeg;

    // Curve: zero up to the start angle, full from the full angle, smooth and signed in between.
    expect(pt::target_weight(0.0f) == 0.0f && pt::target_weight(59.0f * d) == 0.0f, "twist: no effect below 60 degrees");
    expect(pt::target_weight(150.0f * d) == 1.0f && pt::target_weight(170.0f * d) == 1.0f, "twist: full from 150 degrees");
    expect(close_to(pt::target_weight(105.0f * d), 0.5, 1e-5), "twist: half way at the midpoint (smoothstep)");
    expect(pt::target_weight(-120.0f * d) == -pt::target_weight(120.0f * d) && pt::target_weight(-120.0f * d) < 0,
           "twist: signed like the yaw");
    float prev = 0.0f;
    bool monotonic = true;
    for (int deg = 0; deg <= 180; ++deg) {
        const float w = pt::target_weight(deg * d);
        if (w < prev - 1e-6f || w < 0.0f || w > 1.0f) monotonic = false;
        prev = w;
    }
    expect(monotonic, "twist: the curve rises monotonically within [0, 1]");
    // Past +-180 degrees the side holds: the yaw is unwrapped against last frame's.
    float c = pt::continuous_yaw(1e9f, 170.0f * d);
    expect(close_to(c, 170.0f * d, 1e-5), "side: the first frame takes the wrapped yaw");
    c = pt::continuous_yaw(c, -175.0f * d);
    expect(close_to(c, 185.0f * d, 1e-4) && pt::target_weight(c) == 1.0f, "side: crossing 180 keeps the right side");
    c = pt::continuous_yaw(c, 175.0f * d);
    expect(close_to(c, 175.0f * d, 1e-4) && pt::target_weight(c) == 1.0f, "side: and back again");
    expect(close_to(pt::continuous_yaw(400.0f * d, 10.0f * d), 10.0f * d, 1e-5), "side: re-anchored inside the start angle");
    expect(pt::lean_body(pt::target_weight(pt::continuous_yaw(170.0f * d, -170.0f * d)), {0, 0, 0}).x > 0,
           "side: the lean does not flip across 180");
    expect(pt::target_weight(std::numeric_limits<float>::quiet_NaN()) == 0.0f, "twist: NaN yaw has no effect");

    // Filter: a first-order lag of kFilterSeconds, never past the target, snaps after a long gap.
    expect(pt::filter_step(0.0f, 1.0f, 0.0f) == 0.0f, "filter: no time, no change");
    expect(close_to(pt::filter_step(0.0f, 1.0f, pt::kFilterSeconds), 1.0 - std::exp(-1.0), 1e-5),
           "filter: 63% after one time constant");
    expect(pt::filter_step(0.2f, 1.0f, 1.0f) == 1.0f, "filter: a long gap jumps to the target");
    float w = 0.0f;
    bool bounded = true;
    for (int i = 0; i < 60; ++i) {  // one second at 60 fps
        w = pt::filter_step(w, 1.0f, 1.0f / 60.0f);
        if (w > 1.0f) bounded = false;
    }
    expect(bounded && w > 0.99f, "filter: converges in a second without overshoot");
    expect(pt::filter_step(std::numeric_limits<float>::quiet_NaN(), 0.5f, 0.01f) == 0.5f, "filter: recovers from NaN");

    // Twist: adds up to kMaxTwist, within the widened seat limit and +-180 degrees.
    const float lo = -145.0f * d, hi = 145.0f * d;
    auto twisted_yaw = [](float yaw, float wt, float mn, float mx, bool lim) { return yaw + pt::twist_delta(yaw, wt, mn, mx, lim); };
    expect(twisted_yaw(30.0f * d, 0.0f, lo, hi, true) == 30.0f * d, "twist: none at zero weight");
    expect(close_to(twisted_yaw(100.0f * d, 0.5f, lo, hi, true), 100.0f * d + 0.5f * pt::kMaxTwist, 1e-5),
           "twist: weight times the maximum");
    expect(close_to(pt::kMaxTwist, 30.0f * d, 1e-6), "twist: at most 30 degrees extra");
    expect(close_to(twisted_yaw(145.0f * d, 1.0f, lo, hi, true), 175.0f * d, 1e-4), "twist: the seat MaxYaw plus 30 at most");
    expect(close_to(twisted_yaw(-145.0f * d, -1.0f, lo, hi, true), -175.0f * d, 1e-4), "twist: the seat MinYaw minus 30 at most");
    expect(close_to(twisted_yaw(120.0f * d, 1.0f, -130.0f * d, 130.0f * d, true), 150.0f * d, 1e-4),
           "twist: within the seat limit plus the twist");
    expect(close_to(twisted_yaw(110.0f * d, 1.0f, -100.0f * d, 100.0f * d, true), 130.0f * d, 1e-4),
           "twist: stops at the narrow seat limit plus the twist");
    expect(close_to(twisted_yaw(140.0f * d, 1.0f, -100.0f * d, 100.0f * d, true), 140.0f * d, 1e-4),
           "twist: never pulls back a yaw already past the widened limit");
    expect(close_to(twisted_yaw(140.0f * d, 1.0f, 0.0f, 0.0f, false), 170.0f * d, 1e-4), "twist: unlimited view, full twist");
    expect(close_to(twisted_yaw(175.0f * d, 1.0f, 0.0f, 0.0f, false), 180.0f * d, 1e-4), "twist: unlimited view, +-180 only");
    // No compounding: the game smooths its stored yaw (+0xdc) toward the head target every frame; the hook writes the
    // twisted yaw only around the camera build and restores it. The stored yaw must settle on the target and the
    // rendered one on target + twist. Without the restore the stored yaw would run off past the target.
    {
        pt::TwistState st;
        const float target = 120.0f * d;
        float stored = 0.0f, rendered = 0.0f;
        for (int frame = 0; frame < 600; ++frame) {
            stored += (target - stored) * 0.2f;  // the game's smoothing reads the value left from last frame
            const float head = stored;
            const float written = pt::camera_yaw(st, head, 1.0f / 60.0f, lo, hi, true);
            rendered = written;              // the camera is built from it
            stored = pt::restore_yaw(written, written, head);
        }
        expect(close_to(stored, target, 1e-4), "twist: the stored yaw settles on the target (no compounding)");
        expect(close_to(rendered, target + pt::target_weight(target) * pt::kMaxTwist, 1e-4),
               "twist: the rendered yaw is the target plus the twist");
    }
    expect(pt::restore_yaw(0.5f, 0.7f, 0.3f) == 0.5f, "twist: a change the game made during the camera build stands");
    expect(pt::restore_yaw(0.7f, 0.7f, 0.3f) == 0.3f, "twist: otherwise the head yaw is put back");
    {
        pt::TwistState st;
        const float first = pt::camera_yaw(st, 120.0f * d, 0.0f, lo, hi, true);
        expect(close_to(first, 120.0f * d + pt::target_weight(120.0f * d) * pt::kMaxTwist, 1e-4), "twist: a fresh state starts at its target");
    }

    // Lean: toward the look side and up, in body axes; our lean shrinks so that it plus the game's tracker offset
    // stays within the caps.
    expect(close_to(pt::kLeanSide, 0.04, 1e-6) && close_to(pt::kLeanUp, 0.02, 1e-6), "lean: 4 cm sideways, 2 cm up by default");
    {
        const pt::Vec3 six = pt::lean_body(1.0f, {0, 0, 0}, 0.06f);
        expect(close_to(six.x, 0.06, 1e-6) && close_to(six.z, 0.03, 1e-6), "lean: 6 cm sideways gives 3 cm up (half)");
        const pt::Vec3 off = pt::lean_body(1.0f, {0, 0, 0}, 0.0f);
        expect(off.x == 0.0 && off.z == 0.0, "lean: 0 cm is no lean");
        expect(close_to(pt::lean_body(1.0f, {0.03, 0.0, 0.0}, 0.06f).x, 0.03, 1e-6),
               "lean: the 6 cm cap with the tracker offset holds for a configured lean");
        expect(pt::lean_body(1.0f, {0, 0, 0}, 0.5f).x == 0.0, "lean: a side past the cap is refused");
        expect(close_to(pt::twist_delta(100.0f * d, 1.0f, -100.0f * d, 100.0f * d, true, 45.0f * d), 45.0f * d, 1e-5) &&
                   pt::twist_delta(100.0f * d, 1.0f, -100.0f * d, 100.0f * d, true, 0.0f) == 0.0f,
               "twist: a configured angle (45, 0)");
    }
    const pt::Vec3 zero{0, 0, 0};
    const pt::Vec3 full = pt::lean_body(1.0f, zero);
    expect(close_to(full.x, pt::kLeanSide, 1e-6) && full.y == 0.0 && close_to(full.z, pt::kLeanUp, 1e-6), "lean: full right and up");
    const pt::Vec3 left = pt::lean_body(-0.5f, zero);
    expect(close_to(left.x, -0.5 * pt::kLeanSide, 1e-6) && close_to(left.z, 0.5 * pt::kLeanUp, 1e-6), "lean: left is negative x, still up");
    const pt::Vec3 none = pt::lean_body(0.0f, zero);
    expect(none.x == 0.0 && none.z == 0.0, "lean: none at zero weight");
    const pt::Vec3 tracked = pt::lean_body(1.0f, {0.05, 0.0, 0.02});
    expect(close_to(tracked.x + 0.05, pt::kCapSide, 1e-6) && close_to(tracked.z + 0.02, pt::kCapUp, 1e-6),
           "lean: the sum with the tracker offset is capped");
    const pt::Vec3 big = pt::lean_body(1.0f, {0.08, 0.0, 0.04});
    expect(big.x == 0.0 && big.z == 0.0, "lean: no room left when the tracker offset is already past the cap");
    const pt::Vec3 opposite = pt::lean_body(1.0f, {-0.03, 0.0, -0.03});
    expect(close_to(opposite.x, pt::kLeanSide, 1e-6) && close_to(opposite.z, pt::kLeanUp, 1e-6),
           "lean: a tracker offset the other way leaves the lean whole");
    const pt::Vec3 left_tracked = pt::lean_body(-1.0f, {-0.05, 0.0, 0.0});
    expect(close_to(left_tracked.x - 0.05, -pt::kCapSide, 1e-6), "lean: the cap holds to the left too");
    expect(close_to(pt::lean_body(1.0f, {0.03, 0.0, 0.015}).x, pt::kCapSide - 0.03, 1e-6),
           "lean: with the stock 3 cm tracker range at full the lean is trimmed to the 6 cm cap");
    expect(close_to(pt::lean_body(1.0f, {0.02, 0.0, 0.0}).x, pt::kLeanSide, 1e-6),
           "lean: a 2 cm tracker offset still leaves room for the whole 4 cm lean");
    {
        const float mlo[3] = {0.03f, 0.0f, 0.03f}, mhi[3] = {0.03f, 0.0f, 0.03f};  // stock seat limits as the seat returns them
        const pt::Vec3 n = pt::native_camera_offset(-16383, 16383, mlo, mhi);
        expect(close_to(n.x, 0.03, 1e-6) && n.y == 0.0 && close_to(n.z, 0.03, 1e-6), "native: full tracker x and z give the seat limits");
        expect(pt::native_camera_offset(0, 0, mlo, mhi).x == 0.0, "native: a centred tracker adds nothing");
        const float m[9] = {0, -1, 0, 1, 0, 0, 0, 0, 1};
        const pt::Vec3 wv = pt::body_to_world(m, {0.01, 0.02, 0.03});
        const pt::Vec3 back = pt::world_to_body(m, wv);
        expect(close_to(back.x, 0.01, 1e-7) && close_to(back.y, 0.02, 1e-7) && close_to(back.z, 0.03, 1e-7),
               "native: world_to_body undoes body_to_world");
    }

    // Body to world: rows are the body axes in world coordinates.
    const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const pt::Vec3 wi = pt::body_to_world(ident, {0.06, 0.0, 0.03});
    expect(wi.x == 0.06 && wi.y == 0.0 && close_to(wi.z, 0.03, 1e-9), "lean: identity orientation leaves the offset alone");
    const float heading_east[9] = {0, -1, 0, 1, 0, 0, 0, 0, 1};  // nose east (+x), right wing south (-y)
    const pt::Vec3 we = pt::body_to_world(heading_east, {0.06, 0.0, 0.0});
    expect(close_to(we.x, 0.0, 1e-9) && close_to(we.y, -0.06, 1e-9), "lean: right of an east-heading jet is south");
    const float inverted[9] = {-1, 0, 0, 0, 1, 0, 0, 0, -1};  // rolled 180 degrees
    expect(close_to(pt::body_to_world(inverted, {0.0, 0.0, 0.03}).z, -0.03, 1e-9), "lean: up follows the airframe, not the world");
    // Developer head hold: off, 120 left, 160 left, 120 right, 160 right, then off again.
    int step = 0;
    const int want_steps[] = {1, 2, 3, 4, 0, 1};
    bool cycle_ok = true;
    for (int i : want_steps) {
        step = pt::next_hold(step);
        if (step != i) cycle_ok = false;
    }
    expect(cycle_ok, "head hold: cycles off, 120 L, 160 L, 120 R, 160 R, off");
    expect(pt::next_hold(-3) == 0 && pt::next_hold(99) == 0 && !pt::hold_active(-1) && !pt::hold_active(5) &&
               pt::hold_yaw(7) == 0.0f && std::string(pt::hold_name(9)) == "off",
           "head hold: out of range is off");
    expect(!pt::hold_active(0) && pt::hold_yaw(0) == 0.0f && std::string(pt::hold_name(0)) == "off", "head hold: step 0 is off");
    expect(close_to(pt::hold_yaw(1), 120.0f * d, 1e-6) && close_to(pt::hold_yaw(2), 160.0f * d, 1e-6) &&
               close_to(pt::hold_yaw(3), -120.0f * d, 1e-6) && close_to(pt::hold_yaw(4), -160.0f * d, 1e-6),
           "head hold: left is positive yaw (seen live), right negative, 120 and 160 degrees");
    expect(pt::target_weight(pt::hold_yaw(2)) > 0 && pt::target_weight(pt::hold_yaw(4)) < 0,
           "head hold: the twist follows the held side");
    expect(pt::tracker_axis_from_angle(0.0f) == 0 && pt::tracker_axis_from_angle(pt::kPi) == 16383 &&
               pt::tracker_axis_from_angle(-pt::kPi) == -16383 && pt::tracker_axis_from_angle(4.0f) == 16383 &&
               pt::tracker_axis_from_angle(std::numeric_limits<float>::quiet_NaN()) == 0,
           "head hold: tracker axis range +-16383 for +-180 degrees, clamped, NaN 0");
    expect(pt::tracker_axis_from_angle(pt::hold_yaw(1)) == 10922 && pt::tracker_axis_from_angle(pt::hold_yaw(4)) == -14563,
           "head hold: 120 left and 160 right as tracker axis values");
    // A head yaw the hold could give but the seat never would (past the limit widened by the twist) gets no twist:
    // why the hold must go through the game's own clamp (the head target), as real head tracking does.
    expect(pt::twist_delta(160.0f * d, 1.0f, -100.0f * d, 100.0f * d, true) == 0.0f,
           "head hold: a yaw past the widened seat limit gets no extra twist");
    expect(close_to(pt::twist_delta(100.0f * d, 1.0f, -100.0f * d, 100.0f * d, true), pt::kMaxTwist, 1e-6),
           "head hold: at the seat limit the full twist applies");
}

void pilot_twist_settings_tests() {
    using LazyWorks::SF2Patch::config::parse_number_setting;
    std::printf("pilot twist settings\n");
    auto is = [](const LazyWorks::SF2Patch::config::NumberSetting& s, double v, bool def, bool clamp) {
        return std::fabs(s.value - v) < 1e-9 && s.defaulted == def && s.clamped == clamp;
    };
    expect(is(parse_number_setting(nullptr, 30, 0, 45), 30, true, false), "settings: missing gives the default");
    expect(is(parse_number_setting(L"", 30, 0, 45), 30, true, false) && is(parse_number_setting(L"  ", 30, 0, 45), 30, true, false),
           "settings: empty gives the default");
    expect(is(parse_number_setting(L"25", 30, 0, 45), 25, false, false) && is(parse_number_setting(L" 12.5 ", 30, 0, 45), 12.5, false, false),
           "settings: a number, blanks allowed, decimals");
    expect(is(parse_number_setting(L"60", 30, 0, 45), 45, false, true) && is(parse_number_setting(L"-3", 4, 0, 6), 0, false, true),
           "settings: clamped to the range");
    expect(is(parse_number_setting(L"abc", 4, 0, 6), 4, true, false) && is(parse_number_setting(L"5cm", 4, 0, 6), 4, true, false) &&
               is(parse_number_setting(L"nan", 4, 0, 6), 4, true, false) && is(parse_number_setting(L"inf", 4, 0, 6), 4, true, false),
           "settings: not a number gives the default");
    expect(is(parse_number_setting(L"0", 4, 0, 6), 0, false, false) && is(parse_number_setting(L"6", 4, 0, 6), 6, false, false),
           "settings: the limits themselves are kept");
}

void probe_snapshot_tests() {
    using namespace LazyWorks::SF2Patch::hooks;
    bool a = true;
    expect(probe_snapshot_number(L"snap012_auto.txt", &a) == 12 && a, "probe snap: auto index file");
    expect(probe_snapshot_number(L"snap012_auto_world.txt", &a) == 12 && a, "probe snap: auto world file");
    expect(probe_snapshot_number(L"snap1000_auto_0a1b2c3d_f0010.bin", &a) == 1000 && a, "probe snap: auto dump");
    expect(probe_snapshot_number(L"snap012_0a1b2c3d.bin", &a) == 12 && !a, "probe snap: manual dump is not auto");
    expect(probe_snapshot_number(L"snap012.txt", &a) == 12 && !a, "probe snap: manual index is not auto");
    expect(probe_snapshot_number(L"snap012_automatic.txt", &a) == 12 && !a, "probe snap: _automatic is not auto");
    const wchar_t* const kNotOurs[] = {L"snap012_auto.bak",       L"snap012_auto_keep.txt",       L"snap012_auto.txt.old",
                                       L"snap012_auto_world.txt.bak", L"snap012_auto_0a1b2c3.bin",  L"snap012_auto_0a1b2c3d.bin.old",
                                       L"snap012_auto_0A1B2C3D.bin",  L"snap012_auto_0a1b2c3d_f001.bin", L"snap012_auto_0a1b2c3d_f00100.bin",
                                       L"snap012_auto_0a1b2c3d_g0010.bin", L"snap012_auto_.bin", L"snap012_auto"};
    for (const wchar_t* nm : kNotOurs) {
        a = true;
        expect(probe_snapshot_number(nm, &a) == 12 && !a, "probe snap: only the exact shapes we write are auto");
    }
    expect(probe_snapshot_number(L"snap012_auto_0a1b2c3d.bin", &a) == 12 && a, "probe snap: auto aircraft dump");
    expect(probe_snapshot_number(L"snap12_auto.txt", &a) == -1 && !a, "probe snap: too few digits");
    expect(probe_snapshot_number(L"rec_001.bin", &a) == -1 && !a, "probe snap: other files are not ours");
    expect(probe_snapshot_number(L"xsnap012_auto.txt", &a) == -1, "probe snap: prefix must start the name");
    const std::vector<long> del = probe_auto_numbers_to_delete({3, 9, 3, 1, 7, 5, 8, 2, 9}, 5);
    expect(del.size() == 2 && del[0] == 2 && del[1] == 1, "probe snap: keeps the newest 5 flights");
    expect(probe_auto_numbers_to_delete({4, 5}, 5).empty(), "probe snap: fewer than 5 deletes nothing");
    expect(probe_auto_numbers_to_delete({}, 5).empty(), "probe snap: empty");
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
    cursor_clip_tests();
    tacview_tests();
    terrain_tests();
    texture_tests();
    aerodrome_core_tests();
    structure_tests();
    placement_tests();
    gameplay_row_splice_tests();
    pilot_twist_tests();
    probe_snapshot_tests();
    pilot_twist_settings_tests();

    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
