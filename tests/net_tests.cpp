// Console tests for the network protocol and an ENet loopback. Not shipped, no game needed.
#include <enet/enet.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <numeric>
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
#include "acmi/net_report.h"
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
#include "hooks/world_files.h"
#include "hooks/world_logic.h"
#include "hooks/aircraft_side.h"
#include "hooks/jet_layout.h"
#include "hooks/jet_mirror.h"
#include "hooks/cursor_clip_logic.h"
#include "hooks/focus_logic.h"
#include "hooks/remote_avatar_logic.h"
#include "hooks/pilot_twist_math.h"
#include "config_parse.h"
#include "bot_client.h"
#include "bot_serve.h"
#include "net/receiver_math.h"
#include "net/remote_jet.h"
#include <miniz.h>

#include "acmi/object_types.h"
#include "bot_core.h"
#include "net/clock_sync.h"
#include "net/client_core.h"
#include "net/client_logic.h"
#include "net/host_core.h"
#include "net/host_session.h"
#include "net/protocol.h"
#include "net/stream_format.h"
#include "net/stream_record.h"
#include "net/mission_fetch.h"
#include "net/world_proto.h"
#include "net/world_stream_logic.h"
#include "net/hit_logic.h"
#include "net/handover_format.h"
#include <deque>

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
        buf[2] = 1;
        c.send(buf, n, kChannelUnreliable);  // wrong version (v1)
        enc(good_state(19, 100.5), buf);
        c.send(buf, n, kChannelReliable);  // a State on the reliable channel
    }
    c.send(buf, enc(w, buf), kChannelUnreliable);  // a Welcome from a client
    c.send(buf, enc(h, buf), kChannelUnreliable);  // a Hello on the unreliable channel
    {
        PeerInfoMsg pi{};
        c.send(buf, enc(pi, buf), kChannelReliable);  // PeerInfo only travels host to client
        PeerLeftMsg pl{};
        c.send(buf, enc(pl, buf), kChannelReliable);  // PeerLeft likewise
    }
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
    expect(range == 6 && format == 10 && state == 1 && queue_empty(core),
           "6 out of range, 10 malformed, 1 repeated Hello, and nothing else queued");

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

    {  // a v1 Hello: refused with a Bye (wrong version) and a disconnect, nothing queued for the game thread
        TestClient v;
        v.host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
        v.peer = enet_host_connect(v.host, &addr, kChannelCount, 0);
        HelloMsg v1{};
        enc(v1, buf);
        buf[2] = 1;
        for (int i = 0; i < 2000 && !v.connected; ++i) v.idle(1);
        v.send(buf, 52, kChannelReliable);
        for (int i = 0; i < 2000 && !v.disconnected; ++i) v.idle(1);
        bool bye6 = false;
        for (const auto& g : v.got) {
            ByeMsg bm;
            if (decode(g.bytes.data(), g.bytes.size(), bm) == Status::Ok && bm.reason == kByeWrongVersion) bye6 = true;
        }
        expect(v.disconnected && bye6, "a v1 Hello is refused with Bye 6 and a disconnect");
        HostEvent drain[16];
        for (int i = 0; i < 50; ++i) {
            core.poll(drain, 16);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        enet_host_destroy(v.host);
        v.host = nullptr;
    }
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
        for (int i = 0; i < 32; ++i) pong(0.040, 0.015);
        expect(std::fabs(c.best_rtt() - 0.040) < 1e-9 && std::fabs(c.offset() - (90.0 + 0.015)) < 1e-6,
               "the lucky sample leaves the window after 32 Pongs, it does not stick");
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

// A synthetic network between a client and a host whose game clock runs at `rate` of wall time (and can freeze).
// Pings leave every 0.25 s; delays are one-way constants plus uniform jitter from a fixed-seed generator.
struct ClockNet {
    double rate = 1.0, base = 500.0;
    double up = 0.15, down = 0.15, jitter = 0.010;
    double freeze_at = 1e9, freeze_len = 0.0;
    uint32_t seed = 12345;
    double rnd() {  // uniform in (-1, 1)
        seed = seed * 1664525u + 1013904223u;
        return (static_cast<double>(seed >> 8) / 8388608.0) - 1.0;
    }
    double host(double w) const {
        if (w <= freeze_at) return base + rate * w;
        if (w < freeze_at + freeze_len) return base + rate * freeze_at;
        return base + rate * (w - freeze_len);
    }
    struct Flight {
        double send, rx, host;
    };
    Flight ping(double send) {
        const double u = up + jitter * rnd(), d = down + jitter * rnd();
        return {send, send + u + d, host(send + u)};
    }
};

// Runs the client over the model from wall 10 (the Welcome) to `end` in 10 ms steps, delivering Pongs as they arrive,
// and calls on_step(wall) each step.
template <class F>
void run_clock(ClockNet& n, ClockSync& c, double end, F on_step) {
    c.on_welcome(10.0, n.host(10.0 - n.down), 0.5 * (n.up + n.down));
    std::vector<ClockNet::Flight> pending;
    double next_ping = 10.0;
    for (int i = 0; 10.0 + i * 0.01 <= end; ++i) {
        const double w = 10.0 + i * 0.01;
        if (w >= next_ping - 1e-9) {
            pending.push_back(n.ping(next_ping));
            next_ping += 0.25;
        }
        for (size_t k = 0; k < pending.size();) {
            if (pending[k].rx <= w) {
                c.on_pong(pending[k].send, pending[k].rx, pending[k].host);
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(k));
            } else {
                ++k;
            }
        }
        on_step(w);
    }
}

void clock_sync_tests() {
    std::printf("shared clock (synthetic network)\n");
    {  // symmetric 150 ms one way, +-10 ms jitter
        ClockNet n;
        ClockSync c;
        double worst = 0;
        run_clock(n, c, 26.0, [&](double w) {
            if (w >= 10.0 + 8.0) worst = (std::max)(worst, std::fabs(c.now(w) - n.host(w)));
        });
        std::printf("    symmetric: worst offset error from 8 s on %.2f ms\n", worst * 1e3);
        expect(worst < 0.010, "symmetric 150 ms +-10 ms: the offset error stays under 10 ms from 8 s on (the window is full), over 8 s");
    }
    {  // asymmetric 150 ms up, 0 down: half the asymmetry is the limit
        ClockNet n;
        n.up = 0.15;
        n.down = 0.0;
        ClockSync c;
        double lo = 1e9, hi = -1e9;
        run_clock(n, c, 14.0, [&](double w) {
            if (w >= 10.0 + 4.0) {
                const double e = c.now(w) - n.host(w);
                lo = (std::min)(lo, e);
                hi = (std::max)(hi, e);
            }
        });
        std::printf("    asymmetric 150/0: error %.1f..%.1f ms\n", lo * 1e3, hi * 1e3);
        expect(lo > 0.070 && hi < 0.080, "asymmetric 150/0 ms: the error is 75 ms +-5 ms (the limit of the method)");
    }
    for (double rate : {0.99, 1.01}) {  // a host clock that runs slow or fast
        ClockNet n;
        n.rate = rate;
        ClockSync c;
        double worst = 0;
        run_clock(n, c, 26.0, [&](double w) {
            if (w >= 10.0 + 8.0) worst = (std::max)(worst, std::fabs(c.now(w) - n.host(w)));
        });
        std::printf("    host clock at %.2f of wall: worst error over 8 s %.2f ms, skew %.4f\n", rate, worst * 1e3,
                    c.skew());
        expect(worst < 0.010, "with 1 % skew (+-10 ms jitter) the prediction error stays under 10 ms over 8 s");
        expect(std::fabs(c.skew() - (rate - 1.0)) < 0.002, "the fitted skew matches the host's rate");
    }
    {  // the skew is clamped to +-2 %
        ClockNet n;
        n.rate = 1.05;
        ClockSync c;
        run_clock(n, c, 18.0, [&](double) {});
        expect(std::fabs(c.skew()) <= ClockSync::kMaxSkew + 1e-12, "the skew is clamped to +-2 %");
    }
    {  // HostClock: monotonic, and a 50 ms correction closes in about 1 s of wall time without a step
        ClockSync c;
        HostClock h;
        c.on_welcome(10.0, 100.0, 0.0);  // now(w) = w + 90
        double wall = 10.0, prev = h.read(wall, c), max_step = 0, closed_at = -1, sent = -1;
        bool backwards = false;
        for (int i = 1; i <= 400; ++i) {
            wall = 10.0 + i * 0.01;
            if (i == 100) {  // a Pong that moves the estimate by +50 ms (no reset: under kJump)
                c.on_pong(wall - 0.002, wall, wall - 0.001 + 90.0 + 0.05);
                sent = wall;
            }
            const double v = h.read(wall, c);
            if (v < prev) backwards = true;
            max_step = (std::max)(max_step, v - prev - 0.01);
            prev = v;
            if (sent > 0 && closed_at < 0 && std::fabs(v - c.now(wall)) < 1e-4) closed_at = wall - sent;
        }
        std::printf("    slew: 50 ms closed in %.2f s, largest step beyond the elapsed time %.2f ms\n", closed_at,
                    max_step * 1e3);
        expect(!backwards, "HostClock never goes backwards");
        expect(closed_at > 0.9 && closed_at < 1.1, "a 50 ms correction closes in about 1 s of wall time");
        expect(max_step < 0.01 * 0.05 + 1e-9, "the slew adds at most 5 % of the elapsed time per read (no step)");
    }
    {  // a 1 s freeze holds, and the resume resets
        ClockNet n;
        n.freeze_at = 16.0;
        n.freeze_len = 1.0;
        ClockSync c;
        HostClock h;
        double prev = 0, held_value = 0, worst_after = 0;
        bool backwards = false, held_seen = false, held_const = true, resumed = false, hold_flags = true;
        run_clock(n, c, 24.0, [&](double w) {
            const double v = h.read(w, c);
            if (c.frozen()) {
                if (!held_seen) held_value = v;
                else if (v != held_value) held_const = false;
                held_seen = true;
                if (!(h.held() && c.on_hold(w))) hold_flags = false;
            } else if (held_seen) {
                resumed = true;
                if (w > 19.0) worst_after = (std::max)(worst_after, std::fabs(v - n.host(w)));
            }
            if (!held_seen && prev != 0 && v < prev) backwards = true;
            prev = v;
        });
        std::printf("    freeze: resets %d, worst error from 2 s after the resume %.2f ms\n", c.resets(),
                    worst_after * 1e3);
        expect(held_seen && held_const && hold_flags && !backwards, "a freeze holds the clock still, and the sender on hold");
        expect(resumed && c.resets() == 1 && !h.held(), "the resume resets the window, once");
        expect(worst_after < 0.010, "after the resume the clock matches the host again");
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
    clock_sync_tests();
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
    {  // A v3 recording (before Phase 5): its States are not read as bad samples, and the version is named.
        std::vector<uint8_t> g(48 + 2000);
        size_t m = encode_stream_header(10.0, "", g.data(), g.size());
        for (int k = 0; k < 3; ++k) {
            StateMsg hs{};
            hs.game_time = 10.0 + 0.1 * k;
            const size_t len = encode(hs, buf, sizeof(buf));
            buf[2] = 3;
            m += encode_stream_record(StreamKind::HostState, hs.game_time, buf, len, g.data() + m, g.size() - m);
        }
        std::string o3;
        LazyWorks::SF2Patch::acmi::Info i3;
        LazyWorks::SF2Patch::acmi::convert_stream(g.data(), m, opt, o3, i3);
        expect(i3.other_protocol == 3 && i3.other_version == 3 && i3.samples_in == 0 && i3.skipped_bad == 0,
               "a v3 recording: its records are counted as another protocol version, not as bad samples");
        LazyWorks::SF2Patch::acmi::ReportOptions ro3;
        LazyWorks::SF2Patch::acmi::Report r3;
        expect(!LazyWorks::SF2Patch::acmi::make_report(g.data(), m, g.data(), m, ro3, r3) &&
                   r3.error.find("protocol v3; this build reads v4") != std::string::npos,
               "a v3 recording: the report refuses it and names both versions");
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

void focus_logic_tests() {
    using LazyWorks::SF2Patch::config::BackgroundMode;
    using LazyWorks::SF2Patch::config::DisplayMode;
    using LazyWorks::SF2Patch::config::parse_choice_setting;
    namespace h = LazyWorks::SF2Patch::hooks;
    std::printf("focus logic\n");
    constexpr DisplayMode B = DisplayMode::Borderless, F = DisplayMode::Fullscreen;
    static_assert(h::keep_running(true, false, B));
    expect(h::bg_decision(false, false, B) == h::BgDecision::NoReason, "bg: Pause and no other reason -> stock pause");
    expect(h::bg_decision(false, false, F) == h::BgDecision::NoReason, "bg: Pause in Fullscreen -> stock, no Fullscreen log");
    expect(h::bg_decision(true, false, F) == h::BgDecision::Fullscreen, "bg: option in Fullscreen -> Borderless only");
    expect(h::bg_decision(false, true, F) == h::BgDecision::Fullscreen, "bg: other reason in Fullscreen -> Borderless only");
    expect(h::keep_running(false, true, B), "bg: another reason (network session) alone keeps running");
    int keeping = 0;
    for (int m = 0; m < 8; ++m) {
        const bool option = (m & 1) != 0, reason = (m & 2) != 0;
        const DisplayMode mode = (m & 4) ? B : F;
        if (h::keep_running(option, reason, mode)) {
            ++keeping;
            expect(mode == B && (option || reason), "bg: keeps running only in Borderless with a reason");
        }
    }
    expect(keeping == 3, "bg: three of eight combinations keep running");

    // GLOBALFOCUS: mode 2 only, never the primary buffer or the position-notify music stream.
    constexpr BackgroundMode S2 = BackgroundMode::KeepRunningWithSound;
    expect(h::wants_global_focus(S2, B, 0x88), "sound: plain secondary buffer gets it");
    expect(h::wants_global_focus(S2, B, 0x400b0), "sound: 3D (LOCDEFER) buffer with pitch gets it");
    expect(!h::wants_global_focus(S2, B, 0x11), "sound: primary buffer never");
    expect(!h::wants_global_focus(S2, B, 0x10188), "sound: music stream never");
    expect(!h::wants_global_focus(S2, F, 0x88), "sound: Fullscreen at creation: stock buffer");
    expect(!h::wants_global_focus(BackgroundMode::KeepRunning, B, 0x88) && !h::wants_global_focus(BackgroundMode::Pause, B, 0x88),
           "sound: modes 0 and 1 leave buffers stock");
    expect(h::needs_pacing(true, true, false) && h::needs_pacing(true, false, true), "pace: minimised or occluded");
    expect(!h::needs_pacing(true, false, false) && !h::needs_pacing(false, true, true), "pace: only while kept running");
    expect(h::pace_paced_period(6944) == h::kPaceFloorUs && h::pace_paced_period(20000) == 20000,
           "pace: paced at most 60 Hz, slower measured periods kept");

    // BackgroundMode parsing: 0, 1, 2; anything else is stock (-1).
    expect(parse_choice_setting(L"0", 2) == 0 && parse_choice_setting(L"1", 2) == 1 && parse_choice_setting(L" 2 ", 2) == 2,
           "setting: 0, 1, 2 (blanks allowed)");
    expect(parse_choice_setting(nullptr, 2) == -1 && parse_choice_setting(L"", 2) == -1, "setting: missing -> stock");
    expect(parse_choice_setting(L"3", 2) == -1 && parse_choice_setting(L"-1", 2) == -1 && parse_choice_setting(L"99", 2) == -1,
           "setting: out of range -> stock (not clamped)");
    expect(parse_choice_setting(L"1.5", 2) == -1 && parse_choice_setting(L"on", 2) == -1 && parse_choice_setting(L"1x", 2) == -1,
           "setting: not a whole number -> stock");

    // Period estimate: 1/8 weight, clamped to 30..240 Hz, hitches ignored.
    expect(h::pace_update_period(16667, 16667) == 16667, "pace: steady 60 Hz stays");
    expect(h::pace_update_period(16000, 24000) == 17000, "pace: one slow frame moves 1/8 of the way");
    expect(h::pace_update_period(16667, 250000) == 16667 && h::pace_update_period(16667, 0) == 16667 &&
               h::pace_update_period(16667, -5) == 16667,
           "pace: loading hitches and bad samples are ignored");
    expect(h::pace_update_period(h::kPaceMinPeriodUs, 1) == h::kPaceMinPeriodUs, "pace: never faster than 240 Hz");
    expect(h::pace_update_period(h::kPaceMaxPeriodUs, 99000) == h::kPaceMaxPeriodUs, "pace: never slower than 30 Hz");
    int64_t p = h::kPaceDefaultPeriodUs;
    for (int i = 0; i < 200; ++i) p = h::pace_update_period(p, 6944);  // 144 Hz monitor
    expect(p >= 6944 && p <= 6960, "pace: converges on the measured 144 Hz period");

    // Deadlines: first frame free, then one period apart; small lateness catches up, large lateness restarts.
    constexpr int64_t P = 16667;
    static_assert(h::pace_step(1000, 0, P).wait_us == 0 && h::pace_step(1000, 0, P).deadline_us == 1000);
    h::PaceStep s = h::pace_step(1000, 0, P);
    s = h::pace_step(2000, s.deadline_us, P);
    expect(s.deadline_us == 1000 + P && s.wait_us == P - 1000, "pace: waits the rest of the period");
    s = h::pace_step(s.deadline_us + 5000, s.deadline_us, P);  // woke 5 ms late
    expect(s.deadline_us == 1000 + 2 * P && s.wait_us == P - 5000, "pace: oversleep is taken from the next wait");
    const int64_t d = s.deadline_us;
    s = h::pace_step(d + P + 3000, d, P);  // a frame took a period plus 3 ms
    expect(s.deadline_us == d + P && s.wait_us == 0, "pace: late by less than a period: no wait, catches up");
    s = h::pace_step(d + 10 * P, d + P, P);  // a long stall
    expect(s.deadline_us == d + 10 * P && s.wait_us == 0, "pace: late by more than a period: schedule restarts");
    int64_t now = 1000, dl = 0;  // 0 is the "not started" deadline
    for (int i = 0; i < 60; ++i) {
        const h::PaceStep st = h::pace_step(now, dl, P);
        dl = st.deadline_us;
        now += st.wait_us + 500;  // each frame's own work takes 0.5 ms
    }
    expect(now == 1000 + 59 * P + 500, "pace: 60 frames take 59 periods (the first is free)");
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

void focus_session_tests() {
    using LazyWorks::SF2Patch::config::DisplayMode;
    namespace h = LazyWorks::SF2Patch::hooks;
    std::printf("focus session reason\n");
    constexpr DisplayMode B = DisplayMode::Borderless, F = DisplayMode::Fullscreen;
    // BackgroundMode Pause (player_option false): the session alone decides.
    static_assert(h::keep_running(false, h::session_reason(true, true), B));
    expect(h::keep_running(false, h::session_reason(true, true), B), "bg sim: session + option + Borderless -> keep running");
    expect(h::bg_decision(false, h::session_reason(false, true), B) == h::BgDecision::NoReason, "bg sim: single-player -> stock pause");
    expect(h::bg_decision(false, h::session_reason(true, false), B) == h::BgDecision::NoReason, "bg sim: LAZYSF2_BG_SIM=0 -> stock pause");
    expect(h::bg_decision(false, h::session_reason(true, true), F) == h::BgDecision::Fullscreen, "bg sim: Fullscreen -> Borderless only");
    expect(h::keep_running(true, h::session_reason(true, false), B), "bg sim: LAZYSF2_BG_SIM=0 leaves BackgroundMode in charge");
    int keeping = 0;
    for (int m = 0; m < 8; ++m) {
        const bool v = h::keep_running(false, h::session_reason((m & 1) != 0, (m & 2) != 0), (m & 4) ? B : F);
        if (v) ++keeping;
        if (v) expect(m == 1 + 2 + 4, "bg sim: only one input combination keeps running");
    }
    expect(keeping == 1, "bg sim: exactly one of 8 combinations keeps running (BackgroundMode Pause)");
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
                   ls[1].find(",Name=SA-2 Site,Type=Ground+AntiAircraft,Coalition=Enemies,Color=Red") != std::string::npos &&
                   ls[1].find("Pilot") == std::string::npos,
               "tacview world: building id 0x2000000+seq, type name, coalition");
        expect(ls[2].rfind("2000002,T=", 0) == 0 &&
                   ls[2].find(",Name=M113" + std::string(1, 92) + ",APC,Type=Ground+Heavy+Armor+Vehicle,Coalition=Allies") != std::string::npos,
               "tacview world: vehicle type and escaped name");
        expect(ls[3].find(",Name=Frigate,Type=Sea+Watercraft+Warship,Coalition=Neutrals") != std::string::npos &&
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
    // Lock lines: the formatter, then change-only writing in the core.
    {
        std::string s;
        append_lock(s, 0x10002, 1);
        append_lock(s, 0x1000005, 0);
        expect(s == "10002,LockedTarget=1,LockedTargetMode=1\n1000005,LockedTargetMode=0,LockedTarget=\n",
               "acmi format: lock and lock-lost lines");
    }
    {
        TacviewCore core{TacviewCoreConfig{}};
        auto locked = [](TacviewRec r, uint64_t target) {
            r.lock_mode = 1;
            r.lock_seq = static_cast<uint32_t>(target);
            return r;
        };
        auto frame = [&](double t, uint64_t ai_target, bool with_second) {
            std::string o;
            core.add(make_rec(1, true, AircraftSide::Friendly, "P", t, 0, 0, 1000, 0, 0, 0));
            TacviewRec ai = make_rec(2, false, AircraftSide::Enemy, "E", t, 0, 3000, 1000, 0, 0, 0);
            core.add(ai_target ? locked(ai, ai_target) : ai);
            if (with_second) core.add(make_rec(3, false, AircraftSide::Enemy, "E2", t, 0, 5000, 1000, 0, 0, 0));
            core.end_frame(t, o);
            return o;
        };
        const std::string f1 = frame(1.0, 1, false);
        expect(f1.find("10002,LockedTarget=1,LockedTargetMode=1\n") != std::string::npos,
               "tacview locks: an AI's lock on the player names id 1");
        expect(f1.rfind("10002,LockedTarget") > f1.rfind("10002,T="), "tacview locks: the lock line follows the transforms");
        expect(frame(1.1, 1, false).find("LockedTarget") == std::string::npos, "tacview locks: an unchanged lock writes nothing");
        expect(frame(1.2, 3, true).find("10002,LockedTarget=10003,LockedTargetMode=1\n") != std::string::npos,
               "tacview locks: a new target is written once described (same frame, later in the list)");
        expect(frame(1.3, 0, true).find("10002,LockedTargetMode=0,LockedTarget=\n") != std::string::npos,
               "tacview locks: a lost lock is cleared");
        expect(frame(1.4, 0, true).find("LockedTarget") == std::string::npos, "tacview locks: no lock, nothing more");
        expect(frame(1.5, 99, true).find("LockedTarget") == std::string::npos, "tacview locks: an unknown target counts as no lock");
        expect(frame(1.6, 2, true).find("LockedTarget") == std::string::npos, "tacview locks: a lock on itself is ignored");
        expect(core.stats().lock_changes == 3, "tacview locks: three lock changes counted");
    }
    {  // a missile's target
        TacviewCore core{TacviewCoreConfig{}};
        std::string out;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 1.0, 0, 0, 1000, 0, 0, 0));
        core.add(make_rec(2, false, AircraftSide::Enemy, "E", 1.0, 0, 3000, 1000, 0, 0, 0));
        TacviewRec m = make_rec(7, false, AircraftSide::Unknown, "AIM-9B", 1.0, 0, 100, 1000, 0, 0, 0);
        m.obj_class = TacviewRec::kMissile;
        m.parent_seq = 1;
        m.lock_mode = 1;
        m.lock_seq = 2;
        core.add(m);
        core.end_frame(1.0, out);
        expect(out.find("1000007,LockedTarget=10002,LockedTargetMode=1\n") != std::string::npos,
               "tacview locks: a missile's target is written on the weapon");
    }
    {  // the target leaves while the locker still names it; a frame of other series keeps the lock
        TacviewCore core{TacviewCoreConfig{}};
        auto ai = [](double t, uint64_t target) {
            TacviewRec r = make_rec(2, false, AircraftSide::Enemy, "E", t, 0, 3000, 1000, 0, 0, 0);
            r.lock_mode = target ? 1 : 0;
            r.lock_seq = static_cast<uint32_t>(target);
            return r;
        };
        std::string o1;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 1.0, 0, 0, 1000, 0, 0, 0));
        core.add(make_rec(3, false, AircraftSide::Enemy, "E2", 1.0, 0, 5000, 1000, 0, 0, 0));
        core.add(ai(1.0, 3));
        core.end_frame(1.0, o1);
        expect(o1.find("10002,LockedTarget=10003,LockedTargetMode=1\n") != std::string::npos, "tacview locks: lock written");
        std::string o2;  // a weapons-only frame: aircraft not sampled
        core.end_frame(1.05, o2, TacviewRec::kSeriesWeapon);
        expect(o2.find("LockedTarget") == std::string::npos, "tacview locks: a frame of other series keeps the lock");
        std::string o3;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 1.1, 0, 0, 1000, 0, 0, 0));
        core.add(ai(1.1, 3));
        core.end_frame(1.1, o3, TacviewRec::kSeriesAircraft);
        expect(o3.find("-10003\n") != std::string::npos && o3.find("10002,LockedTargetMode=0,LockedTarget=\n") != std::string::npos,
               "tacview locks: the target removed while still named writes a clear");
        std::string o4;
        core.add(make_rec(1, true, AircraftSide::Friendly, "P", 1.2, 0, 0, 1000, 0, 0, 0));
        core.add(ai(1.2, 3));
        core.end_frame(1.2, o4, TacviewRec::kSeriesAircraft);
        expect(o4.find("LockedTarget") == std::string::npos, "tacview locks: the clear is written once");
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
    expect(o1.find("7000003,T=") != std::string::npos && o1.find(",Name=Bridge,Type=Ground+Static+Building\n") != std::string::npos,
           "structures: a bridge is a building (the SDK has no Bridge tag)");
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

// RemoteJet (net/remote_jet.h): synthetic flights through a fake channel (one-way delay, jitter, loss, reordering),
// the receiver stepping at a jittered ~60 Hz, errors measured against the truth at the receiver's true time.
namespace {

namespace rmath = LazyWorks::SF2Patch::hooks::replay_math;
namespace jm = LazyWorks::SF2Patch::net::jet_math;
using Out = RemoteJet::Out;

// Level flight at bot::kSpeed that rolls (180 deg/s) into a sustained coordinated turn of `load` g at roll_at.
struct TurnTruth {
    double load = 4.0, roll_at = 2.0;
    double pos[3] = {}, heading = 0, bank = 0, roll_rate = 0, t0 = 0, t = 0;
    void start(const double spawn[3], double hdg, double start_t) {
        for (int i = 0; i < 3; ++i) pos[i] = spawn[i];
        heading = hdg;
        t0 = t = start_t;
    }
    void advance_to(double to) {
        while (to - t > 1e-9) {
            const double dt = (to - t) < bot::kStep ? (to - t) : bot::kStep;
            const double target = t - t0 >= roll_at ? bot::bank_for_load(load) : 0.0;
            const double d = target - bank, mx = bot::kRollRate * dt;
            const double nb = d > mx ? bank + mx : (d < -mx ? bank - mx : target);
            roll_rate = (nb - bank) / dt;
            const double hr = bot::turn_rate(0.5 * (bank + nb)), hm = heading + 0.5 * hr * dt;
            pos[0] += bot::kSpeed * std::sin(hm) * dt;
            pos[1] += bot::kSpeed * std::cos(hm) * dt;
            heading = bot::wrap_pi(heading + hr * dt);
            bank = nb;
            t += dt;
        }
    }
    bot::Flight::Sample sample() const {
        bot::Flight::Sample s{};
        for (int i = 0; i < 3; ++i) s.pos[i] = pos[i];
        s.vel[0] = static_cast<float>(bot::kSpeed * std::sin(heading));
        s.vel[1] = static_cast<float>(bot::kSpeed * std::cos(heading));
        s.ang[1] = static_cast<float>(heading);
        s.ang[2] = static_cast<float>(bank);
        s.rates[1] = static_cast<float>(bot::turn_rate(bank));
        s.rates[2] = static_cast<float>(roll_rate);
        return s;
    }
};

// A rigid body flying along its nose at bot::kSpeed, turned by body rates (pitch about x, roll about y):
//  - Loop: pulls a 6 g loop through the vertical from 2 s (pitch rate ramped in over 0.5 s) and holds it.
//  - Rolls: every 4 s, a full-stick 360 deg/s roll for exactly 1 s, started and stopped at once.
struct BodyTruth {
    enum Kind { Loop, Rolls } kind;
    explicit BodyTruth(Kind k) : kind(k) {}
    jm::Quat q{1, 0, 0, 0};
    double pos[3] = {}, t0 = 0, t = 0, rate[3] = {};
    void start(const double spawn[3], double hdg, double start_t) {
        for (int i = 0; i < 3; ++i) pos[i] = spawn[i];
        const double e[3] = {0.0, hdg, 0.0};
        q = jm::q_from_euler(e);
        t0 = t = start_t;
    }
    void rates_at(double tau, double w[3]) const {
        w[0] = w[1] = w[2] = 0.0;
        if (kind == Loop) {
            const double q_max = 6.0 * bot::kG / bot::kSpeed;
            w[0] = tau < 2.0 ? 0.0 : q_max * (std::min)(1.0, (tau - 2.0) / 0.5);
        } else {
            const double ph = std::fmod(tau, 4.0);
            w[1] = (tau > 1.0 && ph >= 2.0 && ph < 3.0) ? 2.0 * rmath::kPi : 0.0;
        }
    }
    void forward(double f[3]) const {
        f[0] = 2 * (q.x * q.y - q.z * q.w);
        f[1] = 1 - 2 * (q.x * q.x + q.z * q.z);
        f[2] = 2 * (q.y * q.z + q.x * q.w);
    }
    void advance_to(double to) {
        while (to - t > 1e-9) {
            const double dt = (to - t) < 0.001 ? (to - t) : 0.001;
            rates_at(t - t0 + 0.5 * dt, rate);
            const double r[3] = {rate[0] * dt * 0.5, rate[1] * dt * 0.5, rate[2] * dt * 0.5};
            q = jm::q_mul(q, jm::q_exp(r));  // body-frame rotation by half the step: q * exp(w dt / 2)
            double f[3];
            forward(f);
            for (int i = 0; i < 3; ++i) pos[i] += bot::kSpeed * f[i] * dt;  // along the midpoint direction
            q = jm::q_mul(q, jm::q_exp(r));
            t += dt;
        }
    }
    bot::Flight::Sample sample() const {
        bot::Flight::Sample s{};
        double f[3], e[3];
        forward(f);
        jm::q_to_euler(q, e);
        for (int i = 0; i < 3; ++i) {
            s.pos[i] = pos[i];
            s.vel[i] = static_cast<float>(bot::kSpeed * f[i]);
            s.ang[i] = static_cast<float>(e[i]);
            s.rates[i] = static_cast<float>(rate[i]);
        }
        return s;
    }
};

struct ScriptTruth {
    bot::Flight f;
    bot::Script script;
    explicit ScriptTruth(bot::Script s) : script(s) {}
    void start(const double spawn[3], double hdg, double t0) { f.start(script, spawn, hdg, t0); }
    void advance_to(double t) { f.advance_to(t); }
    bot::Flight::Sample sample() const { return f.sample(); }
};

struct JetRun {
    JetParams params;
    double duration = 60.0, delay = 0.15, jitter = 0.010, loss = 0.01, rate = 20.0;
    double heading = 0.3;
    double sender_phase = 0.0071, sender_jitter = 0.0;  // the first send time, and +- uniform jitter per send
    double dt_lo = 1.0 / 60.0, dt_hi = 1.0 / 60.0;     // receiver step length, uniform in [lo, hi]
    double clock_off = 0.0;                            // the receiver's host clock minus the true host time
    double vel_noise = 0.0;                            // +- uniform per axis on the sent velocity, m/s
    double gap_at = -1, gap_len = 0;    // seconds from the start: the source sends nothing
    double jump_at = -1, jump = 500.0;  // from then on the source is `jump` metres east
    double back_at = -1, back = 0.0;    // from then on the receiver's clock reads `back` seconds earlier
    double measure_from = 2.0;
    uint32_t seed = 1;
};

struct JetResult {
    std::vector<double> pos_err, ang_err;  // metres, degrees (rotation distance)
    int outs[5] = {};
    int snaps = 0, snaps_after_first = 0;
    double accel_excess = -1e9, shown_g = 0, true_g = 0;  // the worst ~1 s window: shown g minus true g
    uint32_t rejected = 0, restarts = 0, resyncs = 0;
    double delay_end = 0, delay_rate_max = 0;
    double rate_hz = 0, age_p50 = 0, age_p90 = 0, horizon_max = 0;
    bool angles_valid = true;  // every shown pitch in [-pi/2, pi/2], heading and roll in (-pi, pi]
    double min_move = 1e9;     // the smallest shown displacement between two written steps (frozen jet: 0)
};

double pct(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t k = static_cast<size_t>(std::ceil(q * static_cast<double>(v.size())));
    return v[k == 0 ? 0 : (k > v.size() ? v.size() - 1 : k - 1)];
}

// on_step(t_from_start, out, shown kin, truth sample) is called after each receiver step.
template <class Truth, class F>
JetResult run_jet(Truth truth, const JetRun& run, F on_step) {
    JetResult r;
    RemoteJet jet(run.params);
    const double t0 = 100.0, spawn[3] = {1000.0, 2000.0, 3000.0};
    truth.start(spawn, run.heading, t0);
    uint32_t seed = run.seed * 2654435761u + 12345u;
    auto rnd = [&]() {  // uniform in [0, 1)
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / 16777216.0;
    };
    auto sym = [&]() { return 2.0 * rnd() - 1.0; };
    struct InFlight {
        double arrival;
        StateMsg m;
    };
    std::vector<InFlight> q;
    int nominal = 0;
    auto send_time = [&](int k) { return t0 + run.sender_phase + k / run.rate; };
    double next_send = send_time(0) + run.sender_jitter * sym();
    double tr = t0, prev_delay = jet.delay();
    uint32_t seq = 0;
    double tp[2][3] = {}, tt[2] = {}, win_true = 0, win_start = t0, prev_shown[3] = {};
    int tn = 0;
    bool started = false, have_prev = false;
    while (tr <= t0 + run.duration) {
        if (next_send <= tr) {
            truth.advance_to(next_send);
            const double rel = next_send - t0;
            if (!(run.gap_at >= 0 && rel >= run.gap_at && rel < run.gap_at + run.gap_len)) {
                const bot::Flight::Sample s = truth.sample();
                StateMsg m{};
                m.seq = ++seq;
                m.game_time = next_send;
                for (int i = 0; i < 3; ++i) {
                    m.pos[i] = s.pos[i];
                    m.vel[i] = s.vel[i] + static_cast<float>(run.vel_noise * sym());
                    m.ang[i] = s.ang[i];
                    m.rates[i] = s.rates[i];
                }
                if (run.jump_at >= 0 && rel >= run.jump_at) m.pos[0] += run.jump;
                if (rnd() >= run.loss) q.push_back({next_send + run.delay + run.jitter * sym(), m});
            }
            ++nominal;
            next_send = (std::max)(next_send + 0.001, send_time(nominal) + run.sender_jitter * sym());
            continue;
        }
        truth.advance_to(tr);
        const double clock = tr + run.clock_off - (run.back_at >= 0 && tr - t0 >= run.back_at ? run.back : 0.0);
        std::stable_sort(q.begin(), q.end(), [](const InFlight& a, const InFlight& b) { return a.arrival < b.arrival; });
        size_t k = 0;
        for (; k < q.size() && q[k].arrival <= tr; ++k)
            if (!jet.push(q[k].m, clock)) ++r.rejected;
        q.erase(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(k));
        RemoteJet::Kin shown{};
        const Out o = jet.shown(clock, &shown);
        ++r.outs[static_cast<int>(o)];
        bot::Flight::Sample s = truth.sample();
        if (run.jump_at >= 0 && tr - t0 >= run.jump_at) s.pos[0] += run.jump;
        if (o == Out::Snap && r.snaps++ > 0) ++r.snaps_after_first;
        const bool written = o == Out::Snap || o == Out::Blend || o == Out::Steady;
        if (written) {
            r.angles_valid = r.angles_valid && std::fabs(shown.a[0]) <= 0.5 * rmath::kPi + 1e-9;
            for (int i = 1; i < 3; ++i) r.angles_valid = r.angles_valid && shown.a[i] <= rmath::kPi && shown.a[i] > -rmath::kPi;
            if (have_prev && started && tr - t0 >= run.measure_from) {
                double d2 = 0;
                for (int i = 0; i < 3; ++i) d2 += (shown.p[i] - prev_shown[i]) * (shown.p[i] - prev_shown[i]);
                r.min_move = (std::min)(r.min_move, std::sqrt(d2));
            }
            std::memcpy(prev_shown, shown.p, sizeof(prev_shown));
            have_prev = true;
            started = true;
        } else {
            have_prev = false;
        }
        if (written && tr - t0 >= run.measure_from) {
            double d2 = 0, ta[3];
            for (int i = 0; i < 3; ++i) {
                d2 += (shown.p[i] - s.pos[i]) * (shown.p[i] - s.pos[i]);
                ta[i] = s.ang[i];
            }
            r.pos_err.push_back(std::sqrt(d2));
            r.ang_err.push_back(jm::rotation_distance(shown.a, ta) * 180.0 / rmath::kPi);
        }
        // The truth's per-step acceleration, measured as RemoteJet measures the shown path's.
        if (tn == 2) {
            const double h1 = tt[1] - tt[0], h2 = tr - tt[1];
            double a2 = 0;
            for (int i = 0; i < 3; ++i) {
                const double a = 2.0 * ((s.pos[i] - tp[1][i]) / h2 - (tp[1][i] - tp[0][i]) / h1) / (h1 + h2);
                a2 += a * a;
            }
            win_true = (std::max)(win_true, std::sqrt(a2) / RemoteJet::kG);
            std::memcpy(tp[0], tp[1], sizeof(tp[0]));
            tt[0] = tt[1];
            tn = 1;
        }
        for (int i = 0; i < 3; ++i) tp[tn][i] = s.pos[i];
        tt[tn++] = tr;
        if (run.jump_at >= 0 && std::fabs(tr - t0 - run.jump_at) < 0.1) tn = 0;  // the jump is not an acceleration
        const double dd = std::fabs(jet.delay() - prev_delay) / (std::max)(1e-3, run.dt_lo);
        r.delay_rate_max = (std::max)(r.delay_rate_max, dd);
        prev_delay = jet.delay();
        if (tr - win_start >= 1.0) {
            const JetStats st = jet.take_stats();
            r.restarts += st.restarts;
            r.resyncs += st.resyncs;
            if (started && tr - t0 >= run.measure_from) {
                if (st.accel_g_max - win_true > r.accel_excess) {
                    r.accel_excess = st.accel_g_max - win_true;
                    r.shown_g = st.accel_g_max;
                    r.true_g = win_true;
                }
                r.rate_hz = st.rate_hz;
                r.age_p50 = st.age_p50;
                r.age_p90 = st.age_p90;
                r.horizon_max = (std::max)(r.horizon_max, st.horizon_max);
            }
            win_true = 0;
            win_start = tr;
        }
        on_step(tr - t0, o, shown, s);
        tr += run.dt_lo + (run.dt_hi - run.dt_lo) * rnd();
    }
    const JetStats st = jet.take_stats();
    r.restarts += st.restarts;
    r.resyncs += st.resyncs;
    r.delay_end = jet.delay();
    return r;
}
template <class Truth>
JetResult run_jet(Truth truth, const JetRun& run) {
    return run_jet(truth, run, [](double, Out, const RemoteJet::Kin&, const bot::Flight::Sample&) {});
}

// The worst case over a sweep of runs.
struct JetWorst {
    double med = 0, p95 = 0, max = 0, ang_med = 0, ang_p95 = 0, accel_excess = -1e9;
    int snaps_after_first = 0, coast = 0, runs = 0;
    bool angles_valid = true;
    void add(const JetResult& r) {
        med = (std::max)(med, pct(r.pos_err, 0.5));
        p95 = (std::max)(p95, pct(r.pos_err, 0.95));
        max = (std::max)(max, pct(r.pos_err, 1.0));
        ang_med = (std::max)(ang_med, pct(r.ang_err, 0.5));
        ang_p95 = (std::max)(ang_p95, pct(r.ang_err, 0.95));
        accel_excess = (std::max)(accel_excess, r.accel_excess);
        snaps_after_first = (std::max)(snaps_after_first, r.snaps_after_first);
        coast += r.outs[static_cast<int>(Out::Coast)];
        angles_valid = angles_valid && r.angles_valid;
        ++runs;
    }
};

void print_worst(const char* name, const JetWorst& w) {
    std::printf("    %-24s worst of %d: pos median %5.2f p95 %6.2f max %6.2f m | ang median %5.2f p95 %6.2f deg | "
                "snaps after join %d | accel excess %+.2f g\n",
                name, w.runs, w.med, w.p95, w.max, w.ang_med, w.ang_p95, w.snaps_after_first, w.accel_excess);
}

// 20 seeds x 4 sender phases, with sender jitter, receiver steps of 14-20 ms, velocity noise and (optionally) a
// constant clock error of up to +-clock_err.
template <class Make>
JetWorst sweep(Make make, JetRun base, double clock_err) {
    JetWorst w;
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        for (int ph = 0; ph < 4; ++ph) {
            JetRun run = base;
            run.seed = seed * 7u + static_cast<uint32_t>(ph);
            run.sender_phase = 0.0013 + 0.0119 * ph;
            run.sender_jitter = 0.004;
            run.dt_lo = 0.014;
            run.dt_hi = 0.020;
            run.vel_noise = 0.3;
            run.clock_off = clock_err * (((seed * 37u + static_cast<uint32_t>(ph) * 11u) % 21u) / 10.0 - 1.0);
            w.add(run_jet(make(), run));
        }
    }
    return w;
}

StateMsg jet_state(uint32_t seq, double t, double x, double heading) {
    StateMsg m{};
    m.seq = seq;
    m.game_time = t;
    m.pos[0] = x;
    m.vel[0] = 200.0f;
    m.ang[1] = static_cast<float>(heading);
    return m;
}

constexpr double kDeg = rmath::kPi / 180.0;

}  // namespace

void remote_jet_tests() {
    std::printf("remote jet: attitude helpers\n");
    {
        bool round = true, matrix = true;
        const double cases[][3] = {{0.3, 1.2, -0.7}, {-1.2, -3.0, 2.9}, {0.0, rmath::kPi, 0.5}, {1.5, 0.2, -3.1}};
        for (const auto& e : cases) {
            double back[3];
            jm::q_to_euler(jm::q_from_euler(e), back);
            for (int i = 0; i < 3; ++i) round = round && std::fabs(rmath::angle_diff(back[i], e[i])) < 1e-9;
            float m[9];
            rmath::build_matrix(e, m);
            const jm::Quat q = jm::q_from_euler(e);
            const double f[3] = {2 * (q.x * q.y - q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.x * q.w)};
            const double u[3] = {2 * (q.x * q.z + q.y * q.w), 2 * (q.y * q.z - q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)};
            for (int i = 0; i < 3; ++i) matrix = matrix && std::fabs(f[i] - m[3 + i]) < 1e-6 && std::fabs(u[i] - m[6 + i]) < 1e-6;
        }
        expect(round, "Euler -> quaternion -> Euler round trip");
        expect(matrix, "quaternion forward and up axes match replay_math::build_matrix");
        const double a[3] = {89.9 * kDeg, 0.0, 0.0}, b[3] = {89.9 * kDeg, rmath::kPi, rmath::kPi};
        const double c[3] = {0.0, 0.0, 0.0}, d[3] = {0.0, 0.0, 60.0 * kDeg};
        expect(std::fabs(jm::rotation_distance(a, b) - 0.2 * kDeg) < 1e-6 &&
                   std::fabs(jm::rotation_distance(c, d) - 60.0 * kDeg) < 1e-9,
               "rotation distance: the Euler flip at the vertical is 0.2 deg, a 60 deg roll is 60 deg");
    }

    std::printf("remote jet (predict to the present; 150 ms +-10 ms one way, 1 %% loss, 20 Hz)\n");
    {  // one run each, fixed phase and 60 Hz, for the stats
        JetRun run;
        const JetResult r = run_jet(ScriptTruth(bot::Script::Straight), run);
        std::printf("    stats: rx %.1f Hz, age p50 %.0f p90 %.0f ms, horizon max %.0f ms\n", r.rate_hz, r.age_p50 * 1e3,
                    r.age_p90 * 1e3, r.horizon_max * 1e3);
        expect(r.rate_hz > 18.0 && r.rate_hz < 21.0 && r.age_p50 > 0.14 && r.age_p90 < 0.18,
               "stats: rate about 20 Hz, arrival age p50/p90 about 150-180 ms");
    }

    struct Traj {
        const char* name;
        int kind;  // 0 straight, 1 orbit, 2 climb, 3 4 g turn, 4 breaks, 5 loop, 6 rolls
        double duration, heading;
    };
    const Traj trajs[] = {{"straight", 0, 60.0, 0.3},          {"orbit (3 g, across +-pi)", 1, 60.0, 3.0},
                          {"climb", 2, 60.0, 0.3},             {"roll into 4 g turn", 3, 60.0, 0.3},
                          {"breaks (6 g reversals)", 4, 90.0, 1.0}, {"6 g loop (vertical)", 5, 30.0, 0.3},
                          {"360 deg/s roll stops", 6, 30.0, 0.3}};
    auto run_sweep = [&](const Traj& t, double clock_err) {
        JetRun run;
        run.duration = t.duration;
        run.heading = t.heading;
        switch (t.kind) {
            case 0: return sweep([] { return ScriptTruth(bot::Script::Straight); }, run, clock_err);
            case 1: return sweep([] { return ScriptTruth(bot::Script::Orbit); }, run, clock_err);
            case 2: return sweep([] { return ScriptTruth(bot::Script::Climb); }, run, clock_err);
            case 3: return sweep([] { return TurnTruth{}; }, run, clock_err);
            case 4: return sweep([] { return ScriptTruth(bot::Script::Breaks); }, run, clock_err);
            case 5: return sweep([] { return BodyTruth(BodyTruth::Loop); }, run, clock_err);
            default: return sweep([] { return BodyTruth(BodyTruth::Rolls); }, run, clock_err);
        }
    };

    std::printf("  sweep: 20 seeds x 4 sender phases, sender jitter +-4 ms, receiver steps 14-20 ms, velocity noise "
                "+-0.3 m/s\n");
    JetWorst w[7];
    for (int i = 0; i < 7; ++i) {
        w[i] = run_sweep(trajs[i], 0.0);
        print_worst(trajs[i].name, w[i]);
    }
    expect(w[0].med <= 1.0 && w[0].p95 <= 3.0 && w[1].med <= 1.0 && w[1].p95 <= 3.0,
           "straight and orbit: median <= 1 m, p95 <= 3 m (worst case)");
    // The scripted climb starts and stops pitching instantly (a 7.8 g step): the capped correction spreads the
    // catch-up, so it is held to the hard-manoeuvre limit, not the owner's "gentle" one.
    expect(w[2].med <= 1.0 && w[2].p95 <= 15.0 && w[3].p95 <= 15.0 && w[4].p95 <= 15.0 && w[5].p95 <= 15.0,
           "climb, 4 g turn, breaks, loop: p95 <= 15 m (climb median <= 1 m)");
    expect(w[6].p95 <= 3.0, "roll stops (straight path): p95 <= 3 m");
    expect(w[0].ang_p95 <= 5.0 && w[1].ang_p95 <= 5.0 && w[2].ang_p95 <= 5.0 && w[3].ang_p95 <= 5.0 && w[5].ang_p95 <= 5.0,
           "attitude p95 <= 5 deg: straight, orbit, climb, 4 g turn, loop");
    expect(w[4].ang_p95 <= 20.0, "attitude p95 <= 20 deg: breaks");
    bool no_snaps = true, smooth = true, valid = true;
    for (const JetWorst& x : w) {
        no_snaps = no_snaps && x.snaps_after_first == 0 && x.coast == 0;
        smooth = smooth && x.accel_excess <= 3.0;
        valid = valid && x.angles_valid;
    }
    expect(no_snaps, "no snaps after the join and no coasting (loop and 360 deg/s roll stops included)");
    expect(smooth, "smoothness: per ~1 s window, shown acceleration <= true + 3 g");
    expect(valid, "shown pitch within +-90 deg, heading and roll within +-180 deg");

    std::printf("  the same sweep with a constant clock error of up to +-10 ms\n");
    JetWorst wc[7];
    for (int i = 0; i < 7; ++i) {
        wc[i] = run_sweep(trajs[i], 0.010);
        print_worst(trajs[i].name, wc[i]);
    }
    bool d2 = true, no_snaps_c = true, smooth_c = true;
    for (int i = 0; i < 7; ++i) {
        d2 = d2 && wc[i].med <= 3.0 && wc[i].p95 <= ((i >= 2 && i <= 5) ? 25.0 : 10.0);
        no_snaps_c = no_snaps_c && wc[i].snaps_after_first == 0;
        smooth_c = smooth_c && wc[i].accel_excess <= 3.0;
    }
    expect(d2, "with clock error, owner targets (D2): median <= 3 m; p95 <= 10 m gentle, <= 25 m in hard turns");
    expect(no_snaps_c && smooth_c, "with clock error: no snaps after the join, shown <= true + 3 g");
    expect(wc[1].ang_p95 <= 5.0 && wc[4].ang_p95 <= 20.0, "with clock error: attitude p95 orbit <= 5 deg, breaks <= 20 deg");

    {  // LAN latency (20 ms +-2 ms one way): rot_lag makes the attitude lag between States; gated loosely
        std::printf("  the sweep at 20 ms +-2 ms one way (LAN)\n");
        JetRun lan;
        lan.delay = 0.020;
        lan.jitter = 0.002;
        lan.duration = 60.0;
        lan.heading = 3.0;
        const JetWorst o = sweep([] { return ScriptTruth(bot::Script::Orbit); }, lan, 0.0);
        print_worst("LAN orbit", o);
        lan.heading = 0.3;
        const JetWorst t4 = sweep([] { return TurnTruth{}; }, lan, 0.0);
        print_worst("LAN 4 g turn", t4);
        lan.duration = 90.0;
        lan.heading = 1.0;
        const JetWorst b = sweep([] { return ScriptTruth(bot::Script::Breaks); }, lan, 0.0);
        print_worst("LAN breaks", b);
        expect(o.ang_p95 <= 5.0 && t4.ang_p95 <= 5.0 && b.ang_p95 <= 25.0 && o.p95 <= 3.0 && b.p95 <= 15.0 &&
                   o.snaps_after_first + t4.snaps_after_first + b.snaps_after_first == 0,
               "LAN: attitude p95 orbit and 4 g <= 5 deg, breaks <= 25 deg; position as at 150 ms; no snaps");
    }

    {  // Interpolate mode at the same settings (the comparison mode)
        JetRun run;
        run.params.mode = JetParams::Mode::Interpolate;
        const JetResult r = run_jet(ScriptTruth(bot::Script::Orbit), run);
        std::printf("    interpolate, orbit: pos median %.1f m, ang p95 %.2f deg; delay at the end %.0f ms, fastest "
                    "change %.1f %% per second\n",
                    pct(r.pos_err, 0.5), pct(r.ang_err, 0.95), r.delay_end * 1e3, r.delay_rate_max * 100.0);
        expect(r.delay_end >= 0.2 && r.delay_end <= 0.3, "interpolate: the delay converges to 200-300 ms");
        expect(r.delay_rate_max <= 0.05 + 1e-9, "interpolate: the delay never steps (<= 5 % per second)");
        expect(r.snaps == 1, "interpolate: no snaps after the first sample");
        JetRun loop = run;
        loop.duration = 30.0;
        const JetResult l = run_jet(BodyTruth(BodyTruth::Loop), loop);
        expect(l.snaps == 1 && l.angles_valid, "interpolate through the vertical: no snaps, valid angles");
    }

    {  // gaps of 0.5 to 2.5 s in turns: velocity only, then one capped correction on resume
        double worst = -1e9;
        int snaps = 0, coasts = 0;
        for (double gap : {0.5, 1.0, 2.0, 2.5}) {
            for (uint32_t seed = 1; seed <= 5; ++seed) {
                JetRun run;
                run.duration = 30.0;
                run.gap_at = 10.0;
                run.gap_len = gap;
                run.seed = seed;
                run.dt_lo = 0.014;
                run.dt_hi = 0.020;
                for (int k = 0; k < 2; ++k) {
                    const JetResult r = k ? run_jet(ScriptTruth(bot::Script::Breaks), run)
                                          : run_jet(ScriptTruth(bot::Script::Orbit), run);
                    worst = (std::max)(worst, r.accel_excess);
                    snaps += r.snaps_after_first;
                    coasts += r.outs[static_cast<int>(Out::Coast)];
                }
            }
        }
        std::printf("    gaps 0.5-2.5 s (orbit, breaks): worst accel excess %+.2f g, snaps %d, coasting steps %d\n",
                    worst, snaps, coasts);
        // A snap here is the attitude only (a roll reversal missed during the gap is over 90 deg); the position
        // stays within the acceleration cap, which the next check proves.
        expect(coasts == 0, "gaps up to 2.5 s: no coasting");
        expect(worst <= 6.0 + 0.5, "gaps up to 2.5 s: shown acceleration <= true + the 6 g cap (+0.5 g noise margin)");
    }

    {  // a 2 s gap: velocity only (attitude held), then a blend on resume
        JetRun run;
        run.duration = 30.0;
        run.gap_at = 10.0;
        run.gap_len = 2.0;
        bool vel_only = true, any_coast = false, seen_gap = false;
        Out resume = Out::None;
        RemoteJet::Kin prev{};
        bool have_prev = false;
        // The last State before the gap is sent at about 9.96 s and is shown from 10.11 s; 0.5 s of second-order
        // prediction later (10.47 s) the velocity and attitude must hold until the next State lands at about 12.16 s.
        const JetResult r = run_jet(ScriptTruth(bot::Script::Orbit), run,
                                    [&](double t, Out o, const RemoteJet::Kin& k, const bot::Flight::Sample&) {
            if (o == Out::Coast) any_coast = true;
            if (t > 10.7 && t < 12.1) {
                seen_gap = true;
                if (have_prev)
                    vel_only = vel_only && std::fabs(k.v[0] - prev.v[0]) + std::fabs(k.v[1] - prev.v[1]) < 1e-3 &&
                               jm::rotation_distance(k.a, prev.a) < 1e-9;
                prev = k;
                have_prev = true;
            }
            if (t >= 12.1 && resume == Out::None && o != Out::Steady) resume = o;
        });
        expect(seen_gap && vel_only && !any_coast, "2 s gap: velocity only, attitude held, never coasting");
        expect(resume == Out::Blend && r.snaps == 1, "2 s gap: a blend on resume, not a snap");
    }

    {  // a 4 s gap: Coast, then a snap on resume
        JetRun run;
        run.duration = 30.0;
        run.gap_at = 10.0;
        run.gap_len = 4.0;
        Out after = Out::None;
        bool coast_before_resume = false;
        const JetResult r = run_jet(ScriptTruth(bot::Script::Orbit), run,
                                    [&](double t, Out o, const RemoteJet::Kin&, const bot::Flight::Sample&) {
            if (t > 13.2 && t < 14.1 && o == Out::Coast) coast_before_resume = true;
            if (coast_before_resume && after == Out::None && o != Out::Coast) after = o;
        });
        expect(coast_before_resume && r.outs[static_cast<int>(Out::Coast)] > 30, "4 s gap: Coast after 3 s");
        expect(after == Out::Snap && r.snaps == 2, "4 s gap: a snap on resume");
    }

    {  // a 500 m jump in the source
        JetRun run;
        run.duration = 30.0;
        run.jump_at = 15.0;
        double snap_at = -1;
        const JetResult r = run_jet(ScriptTruth(bot::Script::Straight), run,
                                    [&](double t, Out o, const RemoteJet::Kin&, const bot::Flight::Sample&) {
            if (o == Out::Snap && t > 1.0) snap_at = t;
        });
        expect(r.snaps == 2 && snap_at > 15.1 && snap_at < 15.4, "500 m jump in the source: one snap when it arrives");
    }

    {  // reordering on the channel (jitter wider than the interval): the late States are ignored
        JetRun run;
        run.jitter = 0.04;
        const JetResult r = run_jet(ScriptTruth(bot::Script::Orbit), run);
        expect(r.rejected > 0 && r.snaps == 1 && r.restarts == 0 && pct(r.pos_err, 0.95) <= 3.0,
               "+-40 ms jitter: reordered States are dropped; no snaps or restarts, p95 <= 3 m");
    }

    {  // the receiver's clock steps back 0.3 s (a HostClock reset the caller missed): no freeze, no snap
        JetRun run;
        run.duration = 30.0;
        run.back_at = 15.0;
        run.back = 0.3;
        const JetResult r = run_jet(ScriptTruth(bot::Script::Orbit), run);
        std::printf("    clock back 0.3 s: resyncs %u, snaps %d, coasting %d, smallest step move %.2f m, p95 %.1f m\n",
                    r.resyncs, r.snaps, r.outs[static_cast<int>(Out::Coast)], r.min_move, pct(r.pos_err, 0.95));
        expect(r.resyncs >= 1 && r.snaps == 1 && r.outs[static_cast<int>(Out::Coast)] == 0 && r.min_move > 1.0,
               "receiver clock 0.3 s back: re-anchored (resync), the jet keeps moving, no snap or coast");
    }

    {  // old, duplicate, reordered, restarted
        RemoteJet j;
        RemoteJet::Kin k{};
        expect(j.shown(1.0, &k) == Out::None, "nothing received: None");
        const bool a = j.push(jet_state(1, 1.00, 0, 0), 1.15);
        const bool b = j.push(jet_state(2, 1.05, 10, 0), 1.20);
        const bool dup = j.push(jet_state(2, 1.05, 10, 0), 1.21);
        const bool same_t = j.push(jet_state(3, 1.05, 10, 0), 1.21);
        const bool c = j.push(jet_state(5, 1.20, 40, 0), 1.35);
        const bool reordered = j.push(jet_state(4, 1.15, 30, 0), 1.36);
        const bool older_t = j.push(jet_state(6, 1.10, 30, 0), 1.37);
        expect(a && b && c && !dup && !same_t && !reordered && !older_t,
               "duplicate, same-time, reordered and older-time States are refused");
        expect(j.shown(1.37, &k) == Out::Snap, "the first shown state snaps");
        JetStats st = j.take_stats();
        expect(st.dup_or_old == 4 && st.restarts == 0, "4 refusals counted, no restart");
        // sequence wrap: 0xFFFFFFFF then 0 is newer
        RemoteJet wr;
        expect(wr.push(jet_state(0xFFFFFFFFu, 1.0, 0, 0), 1.1) && wr.push(jet_state(0u, 1.05, 10, 0), 1.15) &&
                   wr.take_stats().restarts == 0,
               "sequence wrap 0xFFFFFFFF -> 0 is newer");
        // a sender restart at seq 1 with the clock running on: one restart, then the jet carries on with one blend
        const bool r1 = j.push(jet_state(1, 1.25, 50, 0), 1.40);
        const Out o1 = j.shown(1.40, &k);
        st = j.take_stats();
        expect(r1 && st.restarts == 1 && o1 == Out::Blend && st.snaps == 0, "sequence restart: reset, then one blend");
        // a mission restart: the source's time and the receiver's clock both go back 5 s, and the jet is far away
        const bool r2 = j.push(jet_state(2, -3.75, 5000, 0), -3.70);
        const Out o2 = j.shown(-3.70, &k);
        st = j.take_stats();
        expect(r2 && st.restarts == 1 && st.resyncs == 1 && o2 == Out::Snap && std::fabs(k.p[0] - 5010.0) < 1.0,
               "source time and receiver clock 5 s back: reset, re-anchor, then a snap to the new place");
        // a sequence jump back by more than kSeqRestart with time running on
        RemoteJet sj;
        sj.push(jet_state(5000, 1.0, 0, 0), 1.15);
        sj.shown(1.15, &k);
        expect(sj.push(jet_state(3000, 1.05, 10, 0), 1.20) && sj.take_stats().restarts == 1,
               "sequence back by more than kSeqRestart: restart");
        // NaN times
        RemoteJet nn;
        nn.push(jet_state(1, 1.0, 0, 0), 1.1);
        nn.shown(1.1, &k);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        expect(nn.shown(nan, &k) == Out::None && !nn.push(jet_state(2, 1.05, 10, 0), nan) &&
                   nn.shown(1.12, &k) != Out::None && std::isfinite(k.p[0]),
               "a NaN now is ignored (None / refused), and the jet carries on");
        // a 1.5 s step gap: re-anchor, not counted as a snap
        RemoteJet lg;
        lg.push(jet_state(1, 1.0, 0, 0), 1.1);
        lg.shown(1.1, &k);
        lg.push(jet_state(2, 2.5, 300, 0), 2.6);
        const Out og = lg.shown(2.6, &k);
        st = lg.take_stats();
        expect(og == Out::Snap && st.snaps == 1 && st.resyncs == 1 && st.resync_snaps == 1,
               "a 1.5 s step gap re-anchors as a resync (counted in resync_snaps, not snaps)");

        // A 360 deg/s roll with 10 States lost (0.55 s, 198 deg): the rate is held, not aliased the wrong way.
        {
            auto roll_state = [](uint32_t seq, double t) {
                StateMsg m = jet_state(seq, t, 200.0 * t, 0.0);
                m.ang[2] = static_cast<float>(rmath::wrap_pi(2.0 * rmath::kPi * t));
                return m;
            };
            RemoteJet rr;
            rr.push(roll_state(1, 1.00), 1.16);
            rr.push(roll_state(2, 1.05), 1.21);
            rr.push(roll_state(13, 1.60), 1.76);
            rr.shown(1.76, &k);
            const double r0 = k.a[2];
            rr.shown(1.78, &k);
            const double step = rmath::angle_diff(k.a[2], r0);
            std::printf("    roll held over 10 lost States: %+.1f deg in 20 ms (true +7.2)\n", step / kDeg);
            expect(step > 0.0, "360 deg/s roll after 10 lost States keeps rolling the right way");
        }

        // Heading across +-pi (rotation prediction, rate decay off so the expected value is exact).
        JetParams nodecay;
        nodecay.rot_lag = 0.0;
        RemoteJet hw(nodecay);
        const double h0 = 179.0 * kDeg, h1 = -179.0 * kDeg;
        hw.push(jet_state(1, 1.00, 0, h0), 1.0);
        hw.push(jet_state(2, 1.05, 10, h1), 1.05);
        hw.shown(1.15, &k);  // 0.1 s after the newest at +2 deg per 0.05 s: -175 deg
        expect(std::fabs(k.a[1] - (-175.0 * kDeg)) < 1e-6, "heading predicted across +-pi (179 -> -179 -> -175)");
        RemoteJet z(nodecay);
        z.push(jet_state(1, 1.00, 0, h1), 1.0);
        z.push(jet_state(2, 1.05, 10, h0), 1.05);
        z.shown(1.15, &k);
        expect(std::fabs(k.a[1] - (175.0 * kDeg)) < 1e-6, "and the other way (-179 -> 179 -> 175)");
        z.reset();
        expect(z.shown(2.0, &k) == Out::None && z.push(jet_state(1, 1.00, 0, 0), 2.0), "reset forgets everything");
    }
}

namespace {

// --- ClientCore (the client's network thread) against HostCore, a fake host game thread, or a raw ENet host ---

void cc_client_log(const char* l) { std::printf("    [client] %s\n", l); }
void cc_host_log(const char* l) { std::printf("    [core] %s\n", l); }

HelloMsg cc_hello(uint32_t theatre) {
    HelloMsg h{};
    set_str(h.callsign, "CLIENT1");
    set_str(h.aircraft, "F-4E");
    h.theatre_hash = theatre;
    return h;
}

StateMsg cc_jet(uint16_t id, uint32_t seq, double t) {
    StateMsg s{};
    s.client_id = id;
    s.seq = seq;
    s.game_time = t;
    s.pos[0] = 10.0 * seq;
    s.pos[2] = 3000.0;
    s.vel[0] = 200.0f;
    return s;
}

// The scripted host jet of FakeHost: straight and level at heading kFhHeading and 200 m/s, at (500, -300, 1200) at host
// time 1000.
constexpr double kFhHeading = 0.5;
StateMsg fh_jet(uint32_t seq, double t) {
    StateMsg s{};
    s.client_id = 0;
    s.seq = seq;
    s.game_time = t;
    const double sh = std::sin(kFhHeading), ch = std::cos(kFhHeading);
    s.pos[0] = 500.0 + 200.0 * sh * (t - 1000.0);
    s.pos[1] = -300.0 + 200.0 * ch * (t - 1000.0);
    s.pos[2] = 1200.0;
    s.vel[0] = static_cast<float>(200.0 * sh);
    s.vel[1] = static_cast<float>(200.0 * ch);
    s.ang[1] = static_cast<float>(kFhHeading);
    return s;
}

// The game thread of a host, as host.cpp runs it but without the game, on HostPeers: answers a Hello with the Welcome
// v2 (the join position on the scripted jet's wing), the PeerInfos and the announcement to the others, or refuses a
// wrong theatre with Bye 5; sends its jet as id 0 at kSendHz of host time when asked; relays every client State to the
// other clients; sends PeerLeft when a client leaves. Host time runs with the wall clock, from 1000 s.
struct FakeHost {
    HostCore core;
    HostPeers peers{core};
    SendTicker tick;
    double t0 = ClientCore::wall_now();
    uint32_t theatre = 0;
    bool answer_hello = true, send_jet = false;
    bot::Flight* flight = nullptr;  // if set, the host's jet flies this (started at host time 1000), not fh_jet's line
    bool welcomed = false, got_hello = false, got_disconnect = false;
    uint8_t slot = 0;  // the newest connection
    uint32_t gen = 0, seq = 0;
    uint32_t gens[kMaxClients] = {};
    std::vector<StateMsg> states;  // every client's
    std::vector<ByeMsg> byes;
    unsigned send_fail = 0;  // HostPeers sends the core refused

    bool start() {
        peers.set_host("HOST", "F-4E");
        core.start("127.0.0.1", 0, cc_host_log);
        for (int i = 0; i < 2000 && core.state() == HostCore::State::Starting; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        core.set_host_time(time());
        return core.state() == HostCore::State::Running;
    }
    double time() const { return 1000.0 + (ClientCore::wall_now() - t0); }
    StateMsg host_jet(uint32_t s, double t) {
        if (!flight) return fh_jet(s, t);
        flight->advance_to(t);
        const bot::Flight::Sample f = flight->sample();
        StateMsg m{};
        m.client_id = 0;
        m.seq = s;
        m.game_time = flight->time();
        for (int i = 0; i < 3; ++i) {
            m.pos[i] = f.pos[i];
            m.vel[i] = f.vel[i];
            m.ang[i] = f.ang[i];
        }
        // The game sends body rates, not Euler rates: in a banked turn most of the heading rate is about the pitch axis.
        const double hr = f.rates[1], bank = f.ang[2];
        m.rates[0] = static_cast<float>(f.rates[0] + hr * std::sin(bank));
        m.rates[1] = static_cast<float>(hr * std::cos(bank));
        m.rates[2] = f.rates[2];
        return m;
    }
    template <class M>
    void send(const M& m, bool reliable) {  // to the newest connection
        uint8_t b[128];
        core.send(slot, gen, b, encode(m, b, sizeof(b)), reliable ? kChannelReliable : kChannelUnreliable, reliable);
    }
    void bye_and_disconnect(uint8_t reason) {  // as host.cpp does: the Bye (signed by the host, id 0), then the disconnect
        ByeMsg b{};
        b.client_id = 0;
        b.reason = reason;
        send(b, true);
        core.disconnect(slot, gen);
        peers.leave(slot, gen, kPeerLeftRemoved);
        welcomed = false;
    }
    void pump() {  // one host step: drains at most 8 x 32 events, as host.cpp does
        core.set_host_time(time());
        HostEvent ev[32];
        for (int round = 0; round < 8; ++round) {
            const size_t n = core.poll(ev, 32);
            if (n == 0) break;
            for (size_t i = 0; i < n; ++i) handle(ev[i]);
        }
        peers.flush_relay();
        const double now = time();
        if (send_jet && peers.count() > 0 && tick.due(now)) {
            const StateMsg s = host_jet(++seq, now);
            uint8_t b[128];
            peers.send_state(b, encode(s, b, sizeof(b)), -1);
        }
        send_fail += peers.take_send_fail();
    }
    void handle(const HostEvent& e) {
        if (e.kind == HostEvent::Connect) {
            slot = e.slot;
            gen = e.gen;
            gens[e.slot] = e.gen;
        } else if (e.kind == HostEvent::Disconnect) {
            got_disconnect = true;
            peers.leave(e.slot, e.gen, e.reason == 1 ? kPeerLeftTimeout : kPeerLeftLeft);
            if (e.slot == slot) welcomed = false;
        } else {
            MsgType t{};
            if (peek_type(e.data, e.len, t) != Status::Ok) return;
            if (t == MsgType::Hello) {
                got_hello = true;
                HelloMsg h{};
                if (!answer_hello || decode(e.data, e.len, h) != Status::Ok) return;
                if (theatre_mismatch(theatre, h.theatre_hash)) {
                    ByeMsg b{};
                    b.reason = kByeWrongTheatre;
                    uint8_t buf[16];
                    core.send(e.slot, e.gen, buf, encode(b, buf, sizeof(buf)), kChannelReliable, true);
                    core.disconnect(e.slot, e.gen);
                    return;
                }
                const double now = time();
                const StateMsg jet = host_jet(0, now);
                WelcomeMsg w{};
                w.client_id = static_cast<uint16_t>(e.slot + 1);
                join_position(jet.pos, jet.ang[1], e.slot, w.spawn_pos);
                w.spawn_heading = jet.ang[1];
                std::memcpy(w.spawn_vel, jet.vel, sizeof(w.spawn_vel));
                w.host_game_time = now;
                w.theatre_hash = theatre;
                if (peers.welcome(e.slot, e.gen, w, h.callsign, h.aircraft) && e.slot == slot) welcomed = true;
            } else if (t == MsgType::State) {
                StateMsg s{};
                if (decode(e.data, e.len, s) != Status::Ok || !peers.welcomed(e.slot)) return;
                states.push_back(s);
                peers.relay(e.slot, e.data, e.len);
            } else if (t == MsgType::Bye) {
                ByeMsg b{};
                if (decode(e.data, e.len, b) == Status::Ok) byes.push_back(b);
                peers.leave(e.slot, e.gen, kPeerLeftLeft);
            }
        }
    }
};

// An ENet host on the test thread, for hosts HostCore cannot imitate (another protocol version, malformed packets,
// a host that stops answering).
struct RawHost {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    bool got_hello = false;
    ~RawHost() {
        if (host) enet_host_destroy(host);
    }
    bool open() {
        ENetAddress a{};
        enet_address_set_host_ip(&a, "127.0.0.1");
        a.port = 0;
        host = enet_host_create(&a, 1, kChannelCount, 0, 0);
        return host != nullptr;
    }
    uint16_t port() const { return host ? host->address.port : 0; }
    void service() {
        ENetEvent ev;
        while (enet_host_service(host, &ev, 0) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) peer = ev.peer;
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                MsgType t{};
                if (peek_type(ev.packet->data, ev.packet->dataLength, t) == Status::Ok && t == MsgType::Hello) got_hello = true;
                enet_packet_destroy(ev.packet);
            }
        }
    }
    void send_raw(const uint8_t* d, size_t n, uint8_t channel) {
        if (!peer) return;
        ENetPacket* p = enet_packet_create(d, n, channel == kChannelReliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (p && enet_peer_send(peer, channel, p) != 0) enet_packet_destroy(p);
        enet_host_flush(host);
    }
    template <class M>
    void send(const M& m, uint8_t channel) {
        uint8_t b[128];
        send_raw(b, encode(m, b, sizeof(b)), channel);
    }
    void announce(uint16_t id) {  // a PeerInfo: the client passes States from id only after it
        PeerInfoMsg p{};
        p.id = id;
        set_str(p.aircraft, "F-4E");
        send(p, kChannelReliable);
    }
    void welcome(double host_time) {
        WelcomeMsg w{};
        w.client_id = 1;
        w.spawn_pos[2] = 1000.0;
        w.host_game_time = host_time;
        send(w, kChannelReliable);
    }
};

struct CcRx {
    std::vector<ClientEvent> ev;
    void pull(ClientCore& c) {
        ClientEvent b[16];
        size_t n;
        while ((n = c.poll(b, 16)) > 0) ev.insert(ev.end(), b, b + n);
    }
    int find(ClientEvent::Kind k, size_t from = 0) const {
        for (size_t i = from; i < ev.size(); ++i)
            if (ev[i].kind == k) return static_cast<int>(i);
        return -1;
    }
    bool has(ClientEvent::Kind k) const { return find(k) >= 0; }
    size_t count(ClientEvent::Kind k) const {
        size_t n = 0;
        for (const ClientEvent& e : ev) n += e.kind == k ? 1 : 0;
        return n;
    }
};

// Runs the fake host (if any) and the client's game side until done() or the wall-clock deadline.
template <class F>
bool cc_run(FakeHost* h, RawHost* raw, ClientCore& c, CcRx& rx, double seconds, F done) {
    const double until = ClientCore::wall_now() + seconds;
    for (;;) {
        if (h) h->pump();
        if (raw) raw->service();
        rx.pull(c);
        if (done()) return true;
        if (ClientCore::wall_now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

uint8_t cc_bye_reason(const ClientEvent& e) {
    ByeMsg b{};
    return decode(e.data, e.len, b) == Status::Ok ? b.reason : 255;
}

// The Welcome's join position for a slot, worked out here from the geometry rather than with join_position: right is
// (cos h, -sin h), ahead (sin h, cos h); 150 m right per client, 50 m back, the host's altitude.
bool fh_join_ok(const WelcomeMsg& w, int slot) {
    const StateMsg jet = fh_jet(0, w.host_game_time);
    const double h = jet.ang[1], right = 150.0 * (slot + 1);
    const double want[3] = {jet.pos[0] + right * std::cos(h) - 50.0 * std::sin(h),
                            jet.pos[1] - right * std::sin(h) - 50.0 * std::cos(h), jet.pos[2]};
    bool ok = w.spawn_heading == jet.ang[1];
    for (int i = 0; i < 3; ++i) ok = ok && std::fabs(w.spawn_pos[i] - want[i]) < 1e-6 && w.spawn_vel[i] == jet.vel[i];
    return ok;
}

void host_session_logic_tests() {
    std::printf("host session: join position, theatre check, send schedule\n");
    {
        const double p[3] = {1000.0, 2000.0, 3000.0};
        double j[3];
        join_position(p, 0.0, 0, j);  // heading north (+y): right is +x
        expect(j[0] == 1150.0 && j[1] == 1950.0 && j[2] == 3000.0, "heading 0, client 1: 150 m right (+x), 50 m back (-y)");
        join_position(p, std::acos(0.0), 1, j);  // heading east (+x): right is -y
        expect(std::fabs(j[0] - 950.0) < 1e-9 && std::fabs(j[1] - 1700.0) < 1e-9 && j[2] == 3000.0,
               "heading 90 deg, client 2: 300 m right (-y), 50 m back (-x)");
        bool frame = true;
        for (double h = -3.0; h <= 3.0; h += 0.37) {
            for (int s = 0; s < kMaxClients; ++s) {
                join_position(p, h, s, j);
                const double dx = j[0] - p[0], dy = j[1] - p[1];
                const double along = dx * std::sin(h) + dy * std::cos(h), across = dx * std::cos(h) - dy * std::sin(h);
                frame = frame && std::fabs(along + 50.0) < 1e-9 && std::fabs(across - 150.0 * (s + 1)) < 1e-9;
            }
        }
        expect(frame, "at any heading: 50 m behind, 150 m right per client, in the host's heading frame");
    }
    expect(!theatre_mismatch(0, 0) && !theatre_mismatch(fnv1a("Korea"), 0) && !theatre_mismatch(0, fnv1a("Korea")) &&
               !theatre_mismatch(fnv1a("Korea"), fnv1a("Korea")) && theatre_mismatch(fnv1a("Korea"), fnv1a("GermanyCE")),
           "theatre mismatch only when both are known and differ");
    {
        auto count = [](double step, double seconds, double start) {
            SendTicker t;
            int n = 0;
            for (double now = start; now < start + seconds - 1e-9; now += step) n += t.due(now) ? 1 : 0;
            return n;
        };
        const int n60 = count(1.0 / 60, 10.0, 0.0), n30 = count(1.0 / 30, 10.0, 7.0), n144 = count(1.0 / 144, 10.0, 3.0);
        const int n15 = count(1.0 / 15, 10.0, 0.0), n20 = count(0.05, 10.0, 0.0);
        char what[200];
        std::snprintf(what, sizeof(what), "20 Hz at 60/30/144/20 fps: %d/%d/%d/%d sends in 10 s (199..201); at 15 fps every step: %d",
                      n60, n30, n144, n20, n15);
        expect(n60 >= 199 && n60 <= 201 && n30 >= 199 && n30 <= 201 && n144 >= 199 && n144 <= 201 && n20 >= 199 && n20 <= 201 &&
                   n15 == 150,
               what);
        SendTicker t;
        int burst = 0;
        expect(t.due(10.0) && !t.due(10.01), "the first step sends, the next one within 50 ms does not");
        for (double now = 15.0; now < 15.04; now += 0.01) burst += t.due(now) ? 1 : 0;
        expect(burst == 1, "after a 5 s pause: one send, no burst to catch up");
        expect(t.due(2.0) && !t.due(2.02) && t.due(2.05), "a step back in time restarts the schedule");
        t.reset();
        expect(t.due(2.051), "reset: the next step sends");
    }
    {  // Without a network thread nothing drains: unreliable sends stop short of the reserve, reliable ones fill it.
        auto core = std::make_unique<HostCore>();
        const uint8_t msg[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        size_t unrel = 0, rel = 0;
        while (unrel < 1000 && core->send(0, 1, msg, sizeof(msg), kChannelUnreliable, false)) ++unrel;
        while (rel < 1000 && core->send(0, 1, msg, sizeof(msg), kChannelReliable, true)) ++rel;
        char what[160];
        std::snprintf(what, sizeof(what), "out ring: %zu unreliable sends, then %zu reliable ones in the reserve (%zu + %zu)",
                      unrel, rel, HostCore::kOutCap - HostCore::kOutReserve, HostCore::kOutReserve);
        expect(unrel == HostCore::kOutCap - HostCore::kOutReserve && rel == HostCore::kOutReserve &&
                   !core->send(0, 1, msg, sizeof(msg), kChannelUnreliable, false),
               what);
    }
}

// Two clients on one host: each gets the host's jet (id 0) at about 20 Hz and the other's States relayed with the
// other's id, the Welcome v2 for its slot, PeerInfo for the host and the other client, and PeerLeft when the other
// leaves.
void client_core_relay_tests() {
    std::printf("client core: two clients, the host's jet and the relay\n");
    FakeHost h;
    h.theatre = fnv1a("Korea");
    if (!h.start()) return expect(false, "host core running");
    HelloMsg hello[2] = {cc_hello(fnv1a("Korea")), cc_hello(0)};
    set_str(hello[0].callsign, "ONE");
    set_str(hello[1].callsign, "TWO");
    set_str(hello[1].aircraft, "F-104G");
    std::unique_ptr<ClientCore> c[2] = {std::make_unique<ClientCore>(), std::make_unique<ClientCore>()};
    CcRx rx[2];
    auto run = [&](double seconds, auto done, auto each) {
        const double until = ClientCore::wall_now() + seconds;
        for (;;) {
            h.pump();
            for (int i = 0; i < 2; ++i) rx[i].pull(*c[i]);
            each();
            if (done()) return true;
            if (ClientCore::wall_now() >= until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto nothing = [] {};
    // One after the other, so the slots are known: ONE is client 1, TWO client 2.
    for (int i = 0; i < 2; ++i) {
        c[i]->start("127.0.0.1", h.core.port(), hello[i], cc_client_log);
        run(5.0, [&] { return rx[i].has(ClientEvent::Welcome); }, nothing);
    }
    expect(c[0]->client_id() == 1 && c[1]->client_id() == 2, "two clients welcomed as 1 and 2");
    if (c[0]->client_id() != 1 || c[1]->client_id() != 2) {
        for (auto& cc : c) cc->stop(false);
        h.core.stop(false);
        return;
    }
    for (int i = 0; i < 2; ++i) {
        WelcomeMsg w{};
        const ClientEvent& we = rx[i].ev[rx[i].find(ClientEvent::Welcome)];
        char what[160];
        std::snprintf(what, sizeof(what), "client %d: the Welcome v2 puts it %d m right and 50 m back of the host, at its "
                      "heading, velocity and altitude, with the theatre hash", i + 1, 150 * (i + 1));
        expect(decode(we.data, we.len, w) == Status::Ok && w.client_id == i + 1 && fh_join_ok(w, i) &&
                   w.theatre_hash == fnv1a("Korea") && std::fabs(w.host_game_time - 1000.0) < 30.0,
               what);
    }

    // Both send their own jet at 20 Hz for 3 s; the host sends its own.
    h.send_jet = true;
    uint32_t sent[2] = {}, refused = 0;
    double next = ClientCore::wall_now();
    const double t_start = ClientCore::wall_now();
    run(3.0, [] { return false; }, [&] {
        const double wall = ClientCore::wall_now();
        if (wall < next) return;
        next += 0.05;
        for (int i = 0; i < 2; ++i) {
            double t = 0;
            if (c[i]->clock_now(wall, &t) && !c[i]->send_state(cc_jet(0, ++sent[i], t))) ++refused;
        }
    });
    const double t_end = ClientCore::wall_now();
    h.send_jet = false;
    run(0.2, [] { return false; }, nothing);  // what is in flight arrives
    const uint32_t jet_sent = h.seq;
    for (int i = 0; i < 2; ++i) {
        const int other = 2 - i;  // the other client's id
        uint32_t host_n = 0;
        uint32_t from_other = 0, foreign = 0, last_other = 0;
        bool host_ok = true, other_ordered = true;
        for (const ClientEvent& e : rx[i].ev) {
            StateMsg s{};
            if (e.kind != ClientEvent::State || decode(e.data, e.len, s) != Status::Ok) continue;
            if (s.client_id == 0) {
                const StateMsg want = fh_jet(s.seq, s.game_time);
                host_ok = host_ok && std::fabs(s.pos[0] - want.pos[0]) < 1e-6 && std::fabs(s.pos[1] - want.pos[1]) < 1e-6;
                ++host_n;
            } else if (s.client_id == other) {
                ++from_other;
                other_ordered = other_ordered && s.seq > last_other;
                last_other = s.seq;
            } else {
                ++foreign;
            }
        }
        // The 20 Hz schedule itself is the ticker test's (L1); here: the host sent at most 20 Hz of its clock (at
        // least 8 Hz even with a starved test thread), and the client got nearly all of it, as scripted.
        const double sent_rate = static_cast<double>(jet_sent) / (t_end - t_start);
        char what[220];
        std::snprintf(what, sizeof(what), "client %d: the host's jet (id 0): %u of %u received, sent at %.1f Hz (8..21), as scripted",
                      i + 1, host_n, jet_sent, sent_rate);
        expect(host_n >= jet_sent * 8 / 10 && host_n <= h.seq && sent_rate >= 8.0 && sent_rate <= 21.0 && host_ok, what);
        const uint32_t relayed = h.peers.relayed_from(static_cast<uint8_t>(other - 1));
        std::snprintf(what, sizeof(what), "client %d: %u of client %d's %u relayed States (of %u sent), in order, with its id; %u others",
                      i + 1, from_other, other, relayed, sent[other - 1], foreign);
        expect(from_other >= relayed * 8 / 10 && from_other >= 10 && relayed <= sent[other - 1] && other_ordered && foreign == 0,
               what);
        const ClientCounters cn = c[i]->counters();
        expect(cn.dropped_format == 0 && cn.dropped_range == 0 && cn.dropped_queue == 0,
               "the client dropped nothing (its own States never come back)");
        // PeerInfo: the host first, then the other client (sent with its own Welcome, or when it joined).
        std::vector<PeerInfoMsg> infos;
        for (const ClientEvent& e : rx[i].ev) {
            PeerInfoMsg p{};
            if (e.kind == ClientEvent::PeerInfo && decode(e.data, e.len, p) == Status::Ok) infos.push_back(p);
        }
        const bool host_info = !infos.empty() && infos[0].id == 0 && std::strcmp(infos[0].callsign, "HOST") == 0 &&
                               std::strcmp(infos[0].aircraft, "F-4E") == 0;
        const bool other_info = infos.size() == 2 && infos[1].id == other &&
                                std::strcmp(infos[1].callsign, other == 1 ? "ONE" : "TWO") == 0 &&
                                std::strcmp(infos[1].aircraft, other == 1 ? "F-4E" : "F-104G") == 0;
        std::snprintf(what, sizeof(what), "client %d: PeerInfo for the host (HOST, F-4E), then for client %d", i + 1, other);
        expect(host_info && other_info && rx[i].find(ClientEvent::PeerInfo) > rx[i].find(ClientEvent::Welcome), what);
    }
    expect(refused == 0 && h.states.size() >= sent[0] / 2 + sent[1] / 2 && h.send_fail == 0,
           "the host got both clients' States and refused none of its own sends");

    // TWO leaves: ONE hears PeerLeft for client 2 (reason: left), and no more of its States.
    c[1]->bye(0);
    const size_t mark = rx[0].ev.size();
    const bool left = run(4.0, [&] { return rx[0].find(ClientEvent::PeerLeft, mark) >= 0 && rx[1].has(ClientEvent::Disconnected); },
                          nothing);
    const int pl = rx[0].find(ClientEvent::PeerLeft, mark);
    PeerLeftMsg m{};
    expect(left && pl >= 0 && decode(rx[0].ev[pl].data, rx[0].ev[pl].len, m) == Status::Ok && m.id == 2 &&
               m.reason == kPeerLeftLeft,
           "PeerLeft (id 2, reason left) reaches the other client");
    run(0.3, [] { return false; }, nothing);
    // The channels are not ordered against each other: a State relayed just before the PeerLeft may arrive just after.
    bool after = false;
    const double left_at = pl >= 0 ? rx[0].ev[pl].wall_rx : 0.0;
    for (size_t k = pl >= 0 ? static_cast<size_t>(pl) : rx[0].ev.size(); k < rx[0].ev.size(); ++k) {
        StateMsg s{};
        if (rx[0].ev[k].kind == ClientEvent::State && decode(rx[0].ev[k].data, rx[0].ev[k].len, s) == Status::Ok &&
            s.client_id == 2 && rx[0].ev[k].wall_rx > left_at + 0.1)
            after = true;
    }
    expect(!after && h.peers.count() == 1, "no State of client 2 later than 100 ms after its PeerLeft; one client left on the host");

    // The host ends the session: its Bye is signed 0, the sender (the only id the client accepts).
    h.slot = 0;
    h.gen = h.gens[0];
    h.bye_and_disconnect(3);
    run(4.0, [&] { return rx[0].has(ClientEvent::Disconnected); }, nothing);
    const int b = rx[0].find(ClientEvent::Bye);
    ByeMsg bm{};
    expect(b >= 0 && decode(rx[0].ev[b].data, rx[0].ev[b].len, bm) == Status::Ok && bm.client_id == 0 && bm.reason == 3,
           "the host's Bye carries id 0 and its reason");
    for (auto& cc : c) cc->stop(false);
    h.core.stop(false);
}

// A host hitch with three clients sending fast: their backlog is relayed newest-only, and the control messages of the
// same step (a Welcome with its PeerInfos, a PeerLeft, a Bye) all still go out.
void client_core_hitch_tests() {
    std::printf("client core: a host hitch with three clients flooding States\n");
    FakeHost h;
    if (!h.start()) return expect(false, "host core running");
    std::unique_ptr<ClientCore> c[4];
    CcRx rx[4];
    auto run = [&](double seconds, bool pump, auto done, auto each) {
        const double until = ClientCore::wall_now() + seconds;
        for (;;) {
            if (pump) h.pump();
            for (int i = 0; i < 4; ++i)
                if (c[i]) rx[i].pull(*c[i]);
            each();
            if (done()) return true;
            if (ClientCore::wall_now() >= until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto nothing = [] {};
    for (int i = 0; i < 3; ++i) {
        c[i] = std::make_unique<ClientCore>();
        HelloMsg hello = cc_hello(0);
        char cs[8];
        std::snprintf(cs, sizeof(cs), "C%d", i + 1);
        set_str(hello.callsign, cs);
        c[i]->start("127.0.0.1", h.core.port(), hello, cc_client_log);
        run(5.0, true, [&] { return rx[i].has(ClientEvent::Welcome); }, nothing);
    }
    if (c[0]->client_id() != 1 || c[1]->client_id() != 2 || c[2]->client_id() != 3) {
        expect(false, "three clients welcomed as 1, 2 and 3");
        for (auto& cc : c)
            if (cc) cc->stop(false);
        h.core.stop(false);
        return;
    }
    // Let the PeerInfos settle, then stall the host's game thread for 0.6 s while each client sends in bursts
    // (stamped with the host's own clock: the clients' estimate freezes with the stalled host). Meanwhile a fourth
    // client connects and client 3 says Bye.
    run(0.5, true, [] { return false; }, nothing);
    for (auto& r : rx) r.ev.clear();
    uint32_t sent = 0;
    bool fourth = false, bye3 = false;
    const double stall_from = ClientCore::wall_now();
    run(0.6, false, [] { return false; }, [&] {
        const double el = ClientCore::wall_now() - stall_from;
        // Bursts of 10, so a starved test thread still builds a backlog of many States per client.
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 10 && !(bye3 && i == 2) && c[i]->send_state(cc_jet(0, sent + 1, h.time())); ++k) ++sent;
        if (!fourth && el > 0.1) {
            fourth = true;
            c[3] = std::make_unique<ClientCore>();
            HelloMsg hello = cc_hello(0);
            set_str(hello.callsign, "C4");
            c[3]->start("127.0.0.1", h.core.port(), hello, cc_client_log);
        }
        if (!bye3 && el > 0.3) {
            bye3 = true;
            c[2]->bye(0);
        }
    });
    const uint32_t backlog = h.core.counters().received;
    h.pump();  // one host step: up to 256 events of the backlog
    h.slot = 0;
    h.gen = h.gens[0];
    h.bye_and_disconnect(2);  // and the same step ends client 1
    const unsigned fail_step = h.send_fail;
    const unsigned superseded = h.peers.take_superseded();
    run(4.0, true, [&] {
        return rx[3].has(ClientEvent::Welcome) && rx[0].has(ClientEvent::Bye) && rx[1].find(ClientEvent::PeerLeft) >= 0 &&
               rx[1].count(ClientEvent::PeerInfo) >= 1;
    }, nothing);
    char what[220];
    std::snprintf(what, sizeof(what), "%u States sent in the stall (%u packets at the host), %u older ones not relayed, "
                  "%u host sends refused (%u in the step after the hitch)", sent, backlog, superseded, h.send_fail, fail_step);
    expect(sent >= 150 && superseded >= 50 && h.send_fail == 0, what);
    const bool welcomed4 = rx[3].has(ClientEvent::Welcome) && c[3]->client_id() == 4;
    expect(welcomed4, "the client that connected during the hitch is welcomed as 4");
    // Client 2 hears that 3 left and that 4 joined; client 4 hears of the host and of client 2 (client 1 was ended
    // in the same step, after the Welcome: its PeerInfo and PeerLeft may both arrive).
    bool left3 = false, info4 = false;
    for (const ClientEvent& e : rx[1].ev) {
        PeerLeftMsg l{};
        PeerInfoMsg p{};
        if (e.kind == ClientEvent::PeerLeft && decode(e.data, e.len, l) == Status::Ok && l.id == 3) left3 = true;
        if (e.kind == ClientEvent::PeerInfo && decode(e.data, e.len, p) == Status::Ok && p.id == 4) info4 = true;
    }
    expect(left3 && info4, "after the hitch: PeerLeft for client 3 and PeerInfo for client 4 reach client 2");
    bool host4 = false, two4 = false;
    for (const ClientEvent& e : rx[3].ev) {
        PeerInfoMsg p{};
        if (e.kind == ClientEvent::PeerInfo && decode(e.data, e.len, p) == Status::Ok) {
            host4 = host4 || p.id == 0;
            two4 = two4 || p.id == 2;
        }
    }
    expect(host4 && two4, "client 4 gets the host's PeerInfo and client 2's");
    const int b = rx[0].find(ClientEvent::Bye);
    expect(b >= 0 && cc_bye_reason(rx[0].ev[b]) == 2, "the host's Bye of the same step reaches client 1");
    for (auto& cc : c)
        if (cc) cc->stop(false);
    h.core.stop(false);
}

void client_core_session_tests() {
    std::printf("client core: a session against HostCore\n");
    FakeHost h;
    h.theatre = fnv1a("Korea");
    if (!h.start()) return expect(false, "host core running");
    auto c = std::make_unique<ClientCore>();
    expect(c->start("127.0.0.1", h.core.port(), cc_hello(fnv1a("Korea")), cc_client_log), "client start");
    expect(!c->start("127.0.0.1", h.core.port(), cc_hello(0), cc_client_log), "a second start is refused");
    CcRx rx;
    const bool welcomed = cc_run(&h, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Welcome); });
    expect(welcomed && c->phase() == ClientCore::Phase::Welcomed && c->client_id() == 1, "connect, Hello, Welcome: client 1");
    if (!welcomed) return;
    const ClientEvent we = rx.ev[rx.find(ClientEvent::Welcome)];
    WelcomeMsg w{};
    expect(decode(we.data, we.len, w) == Status::Ok && w.client_id == 1 && fh_join_ok(w, 0) && w.theatre_hash == fnv1a("Korea"),
           "the Welcome event carries the host's message: the join position on its wing, the theatre hash");

    // Clock: synced within 1 s of the Welcome, and then on the host's time.
    double synced_at = 0, est = 0;
    cc_run(&h, nullptr, *c, rx, 3.0, [&] {
        if (c->clock_now(ClientCore::wall_now(), &est)) synced_at = ClientCore::wall_now();
        return synced_at > 0;
    });
    char what[200];
    std::snprintf(what, sizeof(what), "clock synced %.0f ms after the Welcome (limit 1000)", (synced_at - we.wall_rx) * 1000.0);
    expect(synced_at > 0 && synced_at - we.wall_rx <= 1.0, what);
    expect(rx.find(ClientEvent::PeerInfo) > rx.find(ClientEvent::Welcome), "PeerInfo for the host follows the Welcome");
    if (rx.has(ClientEvent::PeerInfo)) {
        const ClientEvent& pe = rx.ev[rx.find(ClientEvent::PeerInfo)];
        PeerInfoMsg p{};
        expect(decode(pe.data, pe.len, p) == Status::Ok && p.id == 0 && std::strcmp(p.callsign, "HOST") == 0,
               "PeerInfo: id 0, callsign HOST");
    }

    // Two seconds of traffic both ways: the host's jet at 20 Hz, the client's own States at 20 Hz in host time.
    h.send_jet = true;
    double worst_clock = 0, next_send = ClientCore::wall_now();
    uint32_t sent = 0, refused_send = 0;
    int samples = 0;
    cc_run(&h, nullptr, *c, rx, 2.0, [&] {
        const double wall = ClientCore::wall_now();
        double t = 0;
        if (c->clock_now(wall, &t)) {
            worst_clock = std::fmax(worst_clock, std::fabs(t - h.time()));
            ++samples;
            if (wall >= next_send) {
                next_send += 0.05;
                if (!c->send_state(cc_jet(9, ++sent, t))) ++refused_send;
            }
        }
        return false;
    });
    std::printf("    clock: worst error %.2f ms over %d reads; rtt %u ms\n", worst_clock * 1000.0, samples, c->rtt_ms());
    expect(samples > 0 && worst_clock < 0.05, "the client's estimate of host time stays within 50 ms on loopback");
    std::vector<StateMsg> got;
    for (const ClientEvent& e : rx.ev) {
        StateMsg s{};
        if (e.kind == ClientEvent::State && decode(e.data, e.len, s) == Status::Ok) got.push_back(s);
    }
    bool ordered = true, intact = true;
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i].client_id != 0 || got[i].pos[0] != fh_jet(got[i].seq, got[i].game_time).pos[0]) intact = false;
        if (i && got[i].seq <= got[i - 1].seq) ordered = false;
    }
    std::snprintf(what, sizeof(what), "the host's States (id 0) arrive in order and intact: %zu of %u sent", got.size(), h.seq);
    expect(got.size() >= 10 && got.size() <= h.seq && ordered && intact, what);
    bool host_ordered = true, host_ids = true;
    for (size_t i = 0; i < h.states.size(); ++i) {
        if (h.states[i].client_id != 1) host_ids = false;
        if (i && h.states[i].seq <= h.states[i - 1].seq) host_ordered = false;
    }
    std::snprintf(what, sizeof(what), "the client's States reach the host as client 1, in order: %zu of %u sent (%u refused)",
                  h.states.size(), sent, refused_send);
    expect(h.states.size() >= 10 && host_ids && host_ordered && refused_send == 0, what);

    // Another peer joins, is relayed, and leaves.
    h.send_jet = false;
    PeerInfoMsg p2{};
    p2.id = 2;
    set_str(p2.callsign, "THIRD");
    const size_t mark = rx.ev.size();
    h.send(cc_jet(2, 1, h.time()), false);  // before its PeerInfo: dropped
    cc_run(&h, nullptr, *c, rx, 0.2, [] { return false; });
    h.send(p2, true);
    cc_run(&h, nullptr, *c, rx, 3.0, [&] { return rx.find(ClientEvent::PeerInfo, mark) >= 0; });
    h.send(cc_jet(2, 2, h.time()), false);
    cc_run(&h, nullptr, *c, rx, 3.0, [&] { return rx.find(ClientEvent::State, mark) >= 0; });
    PeerLeftMsg l2{};
    l2.id = 2;
    l2.reason = 1;
    h.send(l2, true);
    cc_run(&h, nullptr, *c, rx, 3.0, [&] { return rx.find(ClientEvent::PeerLeft, mark) >= 0; });
    h.send(cc_jet(2, 3, h.time()), false);  // after its PeerLeft: dropped
    cc_run(&h, nullptr, *c, rx, 0.3, [] { return false; });
    const int pi = rx.find(ClientEvent::PeerInfo, mark), pl = rx.find(ClientEvent::PeerLeft, mark);
    std::vector<uint32_t> seq2;
    for (size_t i = mark; i < rx.ev.size(); ++i) {
        StateMsg s{};
        if (rx.ev[i].kind == ClientEvent::State && decode(rx.ev[i].data, rx.ev[i].len, s) == Status::Ok && s.client_id == 2)
            seq2.push_back(s.seq);
    }
    expect(pi >= 0 && pl > pi && seq2.size() == 1 && seq2[0] == 2,
           "PeerInfo, a relayed State (id 2) and PeerLeft for another peer arrive; its States before the PeerInfo and "
           "after the PeerLeft are dropped");
    const ClientCounters cn = c->counters();
    std::printf("    client counters: %u received, %u Pings, %u Pongs, drops %u/%u/%u/%u/%u\n", cn.received, cn.pings, cn.pongs,
                cn.dropped_format, cn.dropped_range, cn.dropped_state, cn.dropped_queue, cn.dropped_control);
    expect(cn.dropped_format == 0 && cn.dropped_range == 0 && cn.dropped_queue == 0 && cn.dropped_control == 0,
           "the client dropped nothing");

    // The client says Bye.
    c->bye(3);
    expect(!c->send_state(cc_jet(1, 1, h.time())), "no States after bye()");
    const bool ended = cc_run(&h, nullptr, *c, rx, 4.0, [&] { return rx.has(ClientEvent::Disconnected) && h.got_disconnect; });
    const int de = rx.find(ClientEvent::Disconnected);
    expect(ended && h.byes.size() == 1 && h.byes[0].reason == 3 && h.byes[0].client_id == 1,
           "client Bye (reason 3) reaches the host, then the host sees the disconnect");
    expect(de >= 0 && rx.ev[de].reason == static_cast<uint8_t>(ClientEnd::Left) &&
               de == static_cast<int>(rx.ev.size()) - 1 && rx.count(ClientEvent::Disconnected) == 1 &&
               c->phase() == ClientCore::Phase::Closed,
           "the client gets one Disconnected (we left), last; phase Closed");
    double t = 0;
    expect(!c->clock_now(ClientCore::wall_now(), &t), "no clock after the end");
    c->stop(false);
    expect(c->phase() == ClientCore::Phase::Idle, "stop: phase Idle");
    h.core.stop(false);
}

void client_core_host_bye_tests() {
    std::printf("client core: the host ends the session\n");
    {  // A Bye from the host (sent with the disconnect straight after it, as host.cpp does); the client is given a name.
        FakeHost h;
        if (!h.start()) return expect(false, "host core running");
        auto c = std::make_unique<ClientCore>();
        c->start("localhost", h.core.port(), cc_hello(0), cc_client_log);
        CcRx rx;
        const bool ok = cc_run(&h, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Welcome); });
        expect(ok, "a host name is resolved on the network thread: localhost, Welcome");
        if (ok) h.bye_and_disconnect(2);
        cc_run(&h, nullptr, *c, rx, 4.0, [&] { return rx.has(ClientEvent::Disconnected); });
        const int b = rx.find(ClientEvent::Bye), d = rx.find(ClientEvent::Disconnected);
        expect(b >= 0 && cc_bye_reason(rx.ev[b]) == 2 && d > b, "host Bye (reason 2) arrives, then Disconnected");
        expect(d >= 0 && rx.ev[d].reason == static_cast<uint8_t>(ClientEnd::HostClosed) && c->phase() == ClientCore::Phase::Closed,
               "Disconnected: the host closed; phase Closed");
        c->stop(false);
        h.core.stop(false);
    }
    {  // The host goes away (its core stops).
        FakeHost h;
        if (!h.start()) return expect(false, "host core running");
        auto c = std::make_unique<ClientCore>();
        c->start("127.0.0.1", h.core.port(), cc_hello(0), cc_client_log);
        CcRx rx;
        cc_run(&h, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Welcome); });
        h.core.stop(false);
        const double t0 = ClientCore::wall_now();
        const bool ok = cc_run(nullptr, nullptr, *c, rx, 8.0, [&] { return rx.has(ClientEvent::Disconnected); });
        char what[160];
        std::snprintf(what, sizeof(what), "host gone: Disconnected after %.2f s (limit 6)", ClientCore::wall_now() - t0);
        expect(ok && ClientCore::wall_now() - t0 <= 6.0 && c->phase() == ClientCore::Phase::Closed, what);
    }
    {  // The host stops answering without a word: the silence limit.
        RawHost raw;
        if (!raw.open()) return expect(false, "raw host");
        auto c = std::make_unique<ClientCore>();
        c->start("127.0.0.1", raw.port(), cc_hello(0), cc_client_log);
        CcRx rx;
        cc_run(nullptr, &raw, *c, rx, 5.0, [&] { return raw.got_hello; });
        raw.welcome(50.0);
        cc_run(nullptr, &raw, *c, rx, 3.0, [&] { return rx.has(ClientEvent::Welcome); });
        const double t0 = ClientCore::wall_now();
        const bool ok = cc_run(nullptr, nullptr, *c, rx, 9.0, [&] { return rx.has(ClientEvent::Disconnected); });
        const double dt = ClientCore::wall_now() - t0;
        const int d = rx.find(ClientEvent::Disconnected);
        char what[160];
        std::snprintf(what, sizeof(what), "silent host: Disconnected (reason %d) after %.2f s (4..6)", d >= 0 ? rx.ev[d].reason : -1, dt);
        expect(ok && dt >= 4.0 && dt <= 6.0 &&
                   (rx.ev[d].reason == static_cast<uint8_t>(ClientEnd::Silent) ||
                    rx.ev[d].reason == static_cast<uint8_t>(ClientEnd::HostClosed)),
               what);
    }
}

void client_core_refusal_tests() {
    std::printf("client core: refusals and bad packets\n");
    {  // Wrong theatre: HostCore's fake host refuses with Bye 5.
        FakeHost h;
        h.theatre = fnv1a("Korea");
        if (!h.start()) return expect(false, "host core running");
        auto c = std::make_unique<ClientCore>();
        c->start("127.0.0.1", h.core.port(), cc_hello(fnv1a("GermanyCE")), cc_client_log);
        CcRx rx;
        cc_run(&h, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Disconnected); });
        const int b = rx.find(ClientEvent::Bye), d = rx.find(ClientEvent::Disconnected);
        expect(h.got_hello && !rx.has(ClientEvent::Welcome) && b >= 0 && cc_bye_reason(rx.ev[b]) == kByeWrongTheatre && d > b &&
                   c->phase() == ClientCore::Phase::Closed,
               "a wrong theatre arrives as Bye 5, then Disconnected; never Welcomed");
        c->stop(false);
        h.core.stop(false);
    }
    {  // Another protocol version: a v1-framed reply becomes Bye 6, and the client leaves.
        RawHost raw;
        if (!raw.open()) return expect(false, "raw host");
        auto c = std::make_unique<ClientCore>();
        c->start("127.0.0.1", raw.port(), cc_hello(0), cc_client_log);
        CcRx rx;
        cc_run(nullptr, &raw, *c, rx, 5.0, [&] { return raw.got_hello; });
        ByeMsg v1{};
        uint8_t b1[16];
        const size_t n = encode(v1, b1, sizeof(b1));
        b1[2] = 1;  // version byte
        raw.send_raw(b1, n, kChannelReliable);
        cc_run(nullptr, &raw, *c, rx, 4.0, [&] { return rx.has(ClientEvent::Disconnected); });
        const int b = rx.find(ClientEvent::Bye), d = rx.find(ClientEvent::Disconnected);
        expect(b >= 0 && cc_bye_reason(rx.ev[b]) == kByeWrongVersion && d > b &&
                   rx.ev[d].reason == static_cast<uint8_t>(ClientEnd::Refused) && c->phase() == ClientCore::Phase::Closed,
               "a host of another version arrives as Bye 6, then Disconnected (refused)");
    }
    {  // Malformed, wrong-channel, wrong-type and out-of-range packets are dropped and counted.
        RawHost raw;
        if (!raw.open()) return expect(false, "raw host");
        auto c = std::make_unique<ClientCore>();
        c->start("127.0.0.1", raw.port(), cc_hello(0), cc_client_log);
        CcRx rx;
        cc_run(nullptr, &raw, *c, rx, 5.0, [&] { return raw.got_hello; });
        raw.send(cc_jet(0, 1, 50.0), kChannelUnreliable);  // before the Welcome: wrong phase
        raw.welcome(50.0);
        cc_run(nullptr, &raw, *c, rx, 3.0, [&] { return rx.has(ClientEvent::Welcome); });
        raw.announce(0);
        cc_run(nullptr, &raw, *c, rx, 3.0, [&] { return rx.has(ClientEvent::PeerInfo); });
        raw.send(cc_jet(3, 1, 50.0), kChannelUnreliable);  // a peer never announced: wrong phase
        raw.send(cc_jet(0, 2, 50.0), kChannelReliable);  // wrong channel
        const uint8_t junk[5] = {1, 2, 3, 4, 5};
        raw.send_raw(junk, sizeof(junk), kChannelUnreliable);  // garbage
        raw.send(cc_hello(0), kChannelReliable);  // a client-only type
        raw.send(cc_jet(1, 3, 50.0), kChannelUnreliable);  // our own id
        StateMsg wide = cc_jet(0, 4, 50.0);
        wide.pos[1] = 1e8;
        raw.send(wide, kChannelUnreliable);  // out of range
        StateMsg late = cc_jet(0, 5, 50.0 + 7200.0);
        raw.send(late, kChannelUnreliable);  // two hours from host time
        raw.send(cc_jet(0, 6, 50.0), kChannelUnreliable);  // good
        cc_run(nullptr, &raw, *c, rx, 3.0, [&] { return rx.has(ClientEvent::State); });
        cc_run(nullptr, &raw, *c, rx, 0.2, [] { return false; });
        const ClientCounters cn = c->counters();
        std::printf("    drops: %u malformed, %u out of range, %u wrong phase\n", cn.dropped_format, cn.dropped_range,
                    cn.dropped_state);
        StateMsg s{};
        const int si = rx.find(ClientEvent::State);
        expect(rx.count(ClientEvent::State) == 1 && si >= 0 && decode(rx.ev[si].data, rx.ev[si].len, s) == Status::Ok && s.seq == 6,
               "only the good State reaches the game thread");
        expect(cn.dropped_format == 4 && cn.dropped_range == 2 && cn.dropped_state == 2, "each bad packet is counted once");
    }
}

void client_core_connect_tests() {
    std::printf("client core: connecting\n");
    uint16_t dead_port = 0;
    {  // a port nothing listens on
        RawHost tmp;
        if (tmp.open()) dead_port = tmp.port();
    }
    expect(dead_port != 0, "a free port");
    {
        ClientCore c;
        HelloMsg h = cc_hello(0);
        expect(!c.start(nullptr, 7777, h, cc_client_log) && !c.start("", 7777, h, cc_client_log) &&
                   !c.start("127.0.0.1", 0, h, cc_client_log) && c.phase() == ClientCore::Phase::Idle,
               "start refuses a missing host or port 0");
    }
    {  // no answer to the connect request
        auto c = std::make_unique<ClientCore>();
        ClientLimits lim;
        lim.connect_ms = 600;
        const double t0 = ClientCore::wall_now();
        c->start("127.0.0.1", dead_port, cc_hello(0), cc_client_log, lim);
        CcRx rx;
        const bool ok = cc_run(nullptr, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Disconnected); });
        const double dt = ClientCore::wall_now() - t0;
        char what[160];
        std::snprintf(what, sizeof(what), "no answer: Disconnected (cannot connect) after %.2f s, phase Failed", dt);
        expect(ok && dt >= 0.5 && rx.ev.back().reason == static_cast<uint8_t>(ClientEnd::CannotConnect) &&
                   c->phase() == ClientCore::Phase::Failed && rx.ev.size() == 1,
               what);
    }
    {  // a name that does not resolve
        auto c = std::make_unique<ClientCore>();
        c->start("no-such-host.invalid", 7777, cc_hello(0), cc_client_log);
        CcRx rx;
        const bool ok = cc_run(nullptr, nullptr, *c, rx, 20.0, [&] { return rx.has(ClientEvent::Disconnected); });
        expect(ok && rx.ev.back().reason == static_cast<uint8_t>(ClientEnd::CannotConnect) && c->phase() == ClientCore::Phase::Failed,
               "an unknown name: Disconnected (cannot connect), phase Failed");
    }
    {  // connected, but the host never sends the Welcome
        FakeHost h;
        h.answer_hello = false;
        if (!h.start()) return expect(false, "host core running");
        auto c = std::make_unique<ClientCore>();
        ClientLimits lim;
        lim.welcome_ms = 800;
        c->start("127.0.0.1", h.core.port(), cc_hello(0), cc_client_log, lim);
        CcRx rx;
        bool awaiting = false;
        const bool ok = cc_run(&h, nullptr, *c, rx, 5.0, [&] {
            awaiting = awaiting || c->phase() == ClientCore::Phase::AwaitWelcome;
            return rx.has(ClientEvent::Disconnected);
        });
        expect(ok && awaiting && h.got_hello && rx.ev.back().reason == static_cast<uint8_t>(ClientEnd::NoWelcome) &&
                   c->phase() == ClientCore::Phase::Failed,
               "no Welcome: AwaitWelcome, then Disconnected (no Welcome), phase Failed");
        c->stop(false);
        h.core.stop(false);
    }
    for (int loader = 0; loader < 2; ++loader) {  // stop while the connect is still pending
        auto c = std::make_unique<ClientCore>();
        c->start("127.0.0.1", dead_port, cc_hello(0), cc_client_log);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const ClientCore::Phase before = c->phase();
        const double t0 = ClientCore::wall_now();
        c->stop(loader != 0);
        const double dt = ClientCore::wall_now() - t0;
        char what[160];
        std::snprintf(what, sizeof(what), "stop(%s) during the connect returns in %.0f ms", loader ? "in loader lock" : "join",
                      dt * 1000.0);
        expect(before == ClientCore::Phase::Connecting && dt < 1.5 && c->phase() == ClientCore::Phase::Idle, what);
        if (loader) std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the detached thread has finished its clean-up
    }
}

std::atomic<int> g_slow_started{0}, g_slow_done{0};

// A name lookup that takes 3 s, then answers 127.0.0.1 (stands in for a stalled DNS server).
bool cc_slow_resolve(const char*, uint32_t* ip) {
    ++g_slow_started;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    ENetAddress a{};
    enet_address_set_host_ip(&a, "127.0.0.1");
    *ip = a.host;
    ++g_slow_done;
    return true;
}

// Services the raw host for a while without the client's game side polling (a paused or loading game).
void cc_idle_raw(RawHost& raw, double seconds) {
    const double until = ClientCore::wall_now() + seconds;
    while (ClientCore::wall_now() < until) {
        raw.service();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// A raw host that has welcomed the client, with the Welcome already polled.
bool cc_welcomed_raw(RawHost& raw, ClientCore& c, CcRx& rx) {
    if (!raw.open()) return false;
    c.start("127.0.0.1", raw.port(), cc_hello(0), cc_client_log);
    cc_run(nullptr, &raw, c, rx, 5.0, [&] { return raw.got_hello; });
    raw.welcome(50.0);
    return cc_run(nullptr, &raw, c, rx, 3.0, [&] { return rx.has(ClientEvent::Welcome); });
}

void client_core_lookup_tests() {
    std::printf("client core: slow name lookups\n");
    ClientLimits slow;
    slow.resolve = cc_slow_resolve;
    for (int loader = 0; loader < 2; ++loader) {
        auto c = std::make_unique<ClientCore>();
        c->start("slow.example", 7777, cc_hello(0), cc_client_log, slow);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const double t0 = ClientCore::wall_now();
        c->stop(loader != 0);
        const double dt = ClientCore::wall_now() - t0;
        char what[160];
        std::snprintf(what, sizeof(what), "stop(%s) during a stalled lookup returns in %.0f ms", loader ? "in loader lock" : "join",
                      dt * 1000.0);
        expect(dt < 0.5 && c->phase() == ClientCore::Phase::Idle, what);
    }
    {
        auto c = std::make_unique<ClientCore>();
        ClientLimits lim = slow;
        lim.connect_ms = 500;
        const double t0 = ClientCore::wall_now();
        c->start("slow.example", 7777, cc_hello(0), cc_client_log, lim);
        CcRx rx;
        const bool ok = cc_run(nullptr, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Disconnected); });
        const double dt = ClientCore::wall_now() - t0;
        char what[160];
        std::snprintf(what, sizeof(what), "a lookup over the connect limit: Disconnected (cannot connect) after %.2f s", dt);
        expect(ok && dt >= 0.4 && dt < 2.5 && rx.ev.back().reason == static_cast<uint8_t>(ClientEnd::CannotConnect) &&
                   c->phase() == ClientCore::Phase::Failed,
               what);
    }
    // The abandoned lookups end by themselves and free their shared block (they never touch the ClientCore).
    const double until = ClientCore::wall_now() + 8.0;
    while (g_slow_done.load() < g_slow_started.load() && ClientCore::wall_now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(g_slow_started.load() == 3 && g_slow_done.load() == 3, "the three abandoned lookups finished, after their ClientCores were gone");
}

void client_core_flood_tests() {
    std::printf("client core: floods while the game does not poll\n");
    {  // States from two sources, then a Bye with the connection kept: quotas hold, the Bye and the end still arrive.
        RawHost raw;
        auto c = std::make_unique<ClientCore>();
        CcRx rx;
        if (!cc_welcomed_raw(raw, *c, rx)) return expect(false, "welcomed by the raw host");
        raw.announce(0);
        raw.announce(2);
        cc_run(nullptr, &raw, *c, rx, 3.0, [&] { return rx.count(ClientEvent::PeerInfo) == 2; });
        const int per = 150;
        uint32_t seq = 0;
        for (int i = 0; i < per; ++i) {
            raw.send(cc_jet(0, ++seq, 50.0), kChannelUnreliable);
            raw.send(cc_jet(2, ++seq, 50.0), kChannelUnreliable);
            if (i % 25 == 24) raw.service();
        }
        ByeMsg b{};
        b.reason = 2;
        raw.send(b, kChannelReliable);
        cc_idle_raw(raw, 3.0);  // the client leaves about 1 s after the Bye, since the host does not disconnect
        rx.ev.clear();
        ClientEvent one[1];
        while (c->poll(one, 1) == 1) rx.ev.push_back(one[0]);  // one at a time: Disconnected must still come last
        size_t n0 = 0, n2 = 0;
        for (const ClientEvent& e : rx.ev) {
            StateMsg s{};
            if (e.kind == ClientEvent::State && decode(e.data, e.len, s) == Status::Ok) (s.client_id == 0 ? n0 : n2) += 1;
        }
        const ClientCounters cn = c->counters();
        const int bi = rx.find(ClientEvent::Bye), di = rx.find(ClientEvent::Disconnected);
        char what[220];
        std::snprintf(what, sizeof(what), "per-source quota: %zu and %zu States queued (quota %zu), %u refused", n0, n2,
                      ClientCore::kStateQuota, cn.dropped_queue);
        expect(n0 == ClientCore::kStateQuota && n2 == ClientCore::kStateQuota && cn.dropped_queue >= 100 &&
                   cn.dropped_queue + 2 * ClientCore::kStateQuota <= 2u * per,
               what);
        expect(bi >= 0 && cc_bye_reason(rx.ev[bi]) == 2 && di == static_cast<int>(rx.ev.size()) - 1 && rx.count(ClientEvent::Disconnected) == 1 &&
                   rx.ev[di].reason == static_cast<uint8_t>(ClientEnd::HostClosed) && c->phase() == ClientCore::Phase::Closed,
               "the Bye survives the flood; the client leaves after the host's Bye by itself; Disconnected last, once");
        expect(!c->start("127.0.0.1", raw.port(), cc_hello(0), cc_client_log), "start after an end without stop() is refused");
    }
    {  // Control events beyond the whole ring: the session ends (QueueFull), and everything queued is still delivered.
        RawHost raw;
        auto c = std::make_unique<ClientCore>();
        CcRx rx;
        if (!cc_welcomed_raw(raw, *c, rx)) return expect(false, "welcomed by the raw host");
        PeerInfoMsg pi{};
        pi.id = 2;
        PeerLeftMsg pl{};
        pl.id = 2;
        for (int i = 0; i < 300; ++i) {
            raw.send(pi, kChannelReliable);
            raw.send(pl, kChannelReliable);
            if (i % 20 == 19) raw.service();
        }
        const double until = ClientCore::wall_now() + 10.0;
        while (c->phase() == ClientCore::Phase::Welcomed && ClientCore::wall_now() < until) cc_idle_raw(raw, 0.01);
        rx.ev.clear();
        ClientEvent b7[7];
        size_t n;
        while ((n = c->poll(b7, 7)) > 0) rx.ev.insert(rx.ev.end(), b7, b7 + n);
        const ClientCounters cn = c->counters();
        char what[200];
        std::snprintf(what, sizeof(what), "a control flood ends the session: %zu events delivered (ring %zu + Disconnected), %u lost",
                      rx.ev.size(), ClientCore::kInCap, cn.dropped_control);
        expect(rx.ev.size() == ClientCore::kInCap + 1 && rx.ev.back().kind == ClientEvent::Disconnected &&
                   rx.ev.back().reason == static_cast<uint8_t>(ClientEnd::QueueFull) && cn.dropped_control >= 1 &&
                   c->phase() == ClientCore::Phase::Closed,
               what);
    }
    {  // Pongs over 100 a second are dropped.
        RawHost raw;
        auto c = std::make_unique<ClientCore>();
        CcRx rx;
        if (!cc_welcomed_raw(raw, *c, rx)) return expect(false, "welcomed by the raw host");
        for (int i = 0; i < 300; ++i) {
            PongMsg p{};
            p.client_send_time = ClientCore::wall_now() - 0.001;
            p.host_game_time = 50.0 + 0.001 * i;
            raw.send(p, kChannelUnreliable);
            if (i % 25 == 24) raw.service();
        }
        cc_idle_raw(raw, 0.5);
        const ClientCounters cn = c->counters();
        char what[160];
        std::snprintf(what, sizeof(what), "Pong rate limit: %u of 300 accepted, %u dropped", cn.pongs, cn.dropped_state);
        expect(cn.pongs >= 100 && cn.pongs <= 200 && cn.pongs + cn.dropped_state == 300, what);
    }
}

void client_core_id_tests() {
    std::printf("client core: ids, duplicates and re-Welcome\n");
    RawHost raw;
    auto c = std::make_unique<ClientCore>();
    CcRx rx;
    if (!cc_welcomed_raw(raw, *c, rx)) return expect(false, "welcomed by the raw host");
    rx.ev.clear();
    PeerInfoMsg pi{};
    PeerLeftMsg pl{};
    WelcomeMsg w{};
    w.host_game_time = 51.0;
    ByeMsg b{};
    pi.id = 1;
    raw.send(pi, kChannelReliable);  // our own id: malformed
    pl.id = 0;
    raw.send(pl, kChannelReliable);  // the host "leaving": malformed (it says Bye)
    pl.id = 1;
    raw.send(pl, kChannelReliable);  // our own id: malformed
    pl.id = 3;
    raw.send(pl, kChannelReliable);  // a peer never announced: wrong phase
    pi.id = 3;
    raw.send(pi, kChannelReliable);  // good
    raw.send(pi, kChannelReliable);  // a duplicate: wrong phase
    w.client_id = 2;
    raw.send(w, kChannelReliable);  // a different id: malformed
    w.client_id = 1;
    raw.send(w, kChannelReliable);  // the same id, too soon: wrong phase
    cc_run(nullptr, &raw, *c, rx, 1.2, [] { return false; });
    raw.send(w, kChannelReliable);  // the same id, later: a re-Welcome
    b.client_id = 3;
    raw.send(b, kChannelReliable);  // not signed by the host: malformed
    b.client_id = 1;
    raw.send(b, kChannelReliable);  // signed with our id (the old convention): malformed
    b.client_id = 0;
    b.reason = 1;
    raw.send(b, kChannelReliable);  // signed by the host: good
    cc_run(nullptr, &raw, *c, rx, 4.0, [&] { return rx.has(ClientEvent::Disconnected); });
    const ClientCounters cn = c->counters();
    std::printf("    drops: %u malformed, %u wrong phase\n", cn.dropped_format, cn.dropped_state);
    const bool kinds = rx.ev.size() == 4 && rx.ev[0].kind == ClientEvent::PeerInfo && rx.ev[1].kind == ClientEvent::Welcome &&
                       rx.ev[2].kind == ClientEvent::Bye && rx.ev[3].kind == ClientEvent::Disconnected;
    expect(kinds && cn.dropped_format == 6 && cn.dropped_state == 3,
           "own-id, id-0, unknown and duplicate peer messages, a foreign Welcome, a Bye not signed 0 are dropped; a later re-Welcome and the host's Bye pass");
}

void client_core_bye_stop_tests() {
    std::printf("client core: bye() and stop() at awkward moments, restart\n");
    {  // bye() before the connection exists: the client ends at once.
        uint16_t dead_port = 0;
        {
            RawHost tmp;
            if (tmp.open()) dead_port = tmp.port();
        }
        auto c = std::make_unique<ClientCore>();
        c->start("127.0.0.1", dead_port, cc_hello(0), cc_client_log);
        c->bye(0);
        CcRx rx;
        const double t0 = ClientCore::wall_now();
        const bool ok = cc_run(nullptr, nullptr, *c, rx, 3.0, [&] { return rx.has(ClientEvent::Disconnected); });
        expect(ok && ClientCore::wall_now() - t0 < 1.0 && rx.ev.back().reason == static_cast<uint8_t>(ClientEnd::Left) &&
                   c->phase() == ClientCore::Phase::Closed,
               "bye() while connecting: Disconnected (we left) at once");
    }
    {  // bye() before the Welcome: no Bye (no id yet), just the disconnect.
        FakeHost h;
        h.answer_hello = false;
        if (!h.start()) return expect(false, "host core running");
        auto c = std::make_unique<ClientCore>();
        c->start("127.0.0.1", h.core.port(), cc_hello(0), cc_client_log);
        CcRx rx;
        cc_run(&h, nullptr, *c, rx, 5.0, [&] { return c->phase() == ClientCore::Phase::AwaitWelcome && h.got_hello; });
        c->bye(0);
        const bool ok = cc_run(&h, nullptr, *c, rx, 4.0, [&] { return rx.has(ClientEvent::Disconnected) && h.got_disconnect; });
        expect(ok && h.byes.empty() && rx.ev.back().reason == static_cast<uint8_t>(ClientEnd::Left),
               "bye() before the Welcome: the host sees the disconnect and no Bye");
        c->stop(false);
        h.core.stop(false);
    }
    {  // A Bye asked for just before stop() still reaches the host; then the same core starts a new session.
        FakeHost h;
        if (!h.start()) return expect(false, "host core running");
        auto c = std::make_unique<ClientCore>();
        c->start("127.0.0.1", h.core.port(), cc_hello(0), cc_client_log);
        CcRx rx;
        cc_run(&h, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Welcome); });
        c->bye(4);
        c->stop(false);
        cc_run(&h, nullptr, *c, rx, 3.0, [&] { return !h.byes.empty() && h.got_disconnect; });
        expect(h.byes.size() == 1 && h.byes[0].reason == 4, "a Bye pending at stop() reaches the host");
        rx.ev.clear();
        h.got_disconnect = false;
        expect(c->start("127.0.0.1", h.core.port(), cc_hello(0), cc_client_log), "restart after stop()");
        const bool ok = cc_run(&h, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Welcome); });
        expect(ok && rx.ev[0].kind == ClientEvent::Welcome && c->phase() == ClientCore::Phase::Welcomed && c->client_id() == 1,
               "the second session is welcomed, with nothing left over from the first");
        c->stop(false);
        h.core.stop(false);
    }
}

void client_core_concurrency_tests() {
    std::printf("client core: a game thread of its own\n");
    FakeHost h;
    if (!h.start()) return expect(false, "host core running");
    auto c = std::make_unique<ClientCore>();
    c->start("127.0.0.1", h.core.port(), cc_hello(0), cc_client_log);
    CcRx rx;
    if (!cc_run(&h, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Welcome); })) return expect(false, "welcomed");
    h.send_jet = true;
    std::atomic<bool> quit{false};
    std::vector<uint32_t> seqs;
    uint32_t sent = 0, refused = 0, clock_reads = 0;
    std::thread game([&] {  // polls in small batches, reads the clock and sends at about 200 Hz, as a busy game would
        double next = ClientCore::wall_now();
        while (!quit.load()) {
            ClientEvent ev[3];
            const size_t n = c->poll(ev, 3);
            for (size_t i = 0; i < n; ++i) {
                StateMsg s{};
                if (ev[i].kind == ClientEvent::State && decode(ev[i].data, ev[i].len, s) == Status::Ok) seqs.push_back(s.seq);
            }
            double t = 0;
            const double wall = ClientCore::wall_now();
            if (c->clock_now(wall, &t)) {
                ++clock_reads;
                if (wall >= next) {
                    next = wall + 0.005;
                    if (!c->send_state(cc_jet(1, ++sent, t))) ++refused;
                }
            }
            std::this_thread::yield();
        }
    });
    const double until = ClientCore::wall_now() + 2.0;
    while (ClientCore::wall_now() < until) {
        h.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    quit = true;
    game.join();
    bool ordered = true;
    for (size_t i = 1; i < seqs.size(); ++i) ordered = ordered && seqs[i] > seqs[i - 1];
    bool host_ordered = true;
    for (size_t i = 1; i < h.states.size(); ++i) host_ordered = host_ordered && h.states[i].seq > h.states[i - 1].seq;
    char what[220];
    std::snprintf(what, sizeof(what), "game thread: %zu host States in order, %u clock reads; host got %zu of %u sent in order (%u refused)",
                  seqs.size(), clock_reads, h.states.size(), sent, refused);
    expect(ordered && host_ordered && seqs.size() >= 10 && h.states.size() >= 50 && c->counters().dropped_queue == 0, what);
    c->stop(false);
    h.core.stop(false);
}

void client_core_tests() {
    client_core_connect_tests();
    client_core_session_tests();
    client_core_relay_tests();
    client_core_hitch_tests();
    client_core_host_bye_tests();
    client_core_refusal_tests();
    client_core_id_tests();
    client_core_flood_tests();
    client_core_bye_stop_tests();
    client_core_lookup_tests();
    client_core_concurrency_tests();
}

// --- The bot as a second player (Task 9) and as a synthetic host (Task 10) ---

void bot_log(const char* l) { std::printf("    [bot] %s\n", l); }

double fh_dist(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

void bot_formation_logic_tests() {
    std::printf("bot: formation geometry\n");
    const double off[3] = {150.0, 50.0, 0.0};
    bool same = true;
    for (double h = -3.1; h <= 3.1; h += 0.29) {
        const double p[3] = {1000.0, -2000.0, 3000.0};
        double a[3], b[3];
        bot::formation_point(p, h, off, a);
        join_position(p, h, 0, b);
        for (int i = 0; i < 3; ++i) same = same && std::fabs(a[i] - b[i]) < 1e-9;
    }
    expect(same, "the default offset is the Welcome's join position for client 1, at any heading");
    {  // along a 6 g turn the formation velocity is the derivative of the formation point
        bot::Flight f;
        const double spawn[3] = {0.0, 0.0, 2000.0};
        f.start(bot::Script::Breaks, spawn, 0.4, 0.0);
        f.advance_to(2.0);  // well into the right turn
        const double up[3] = {150.0, 50.0, 30.0};
        auto at = [&](double t, double out[3]) {
            bot::Flight g = f;
            g.advance_to(t);
            const bot::Flight::Sample s = g.sample();
            bot::formation_point(s.pos, s.ang[1], up, out);
        };
        double p0[3], p1[3];
        at(2.0, p0);
        at(2.01, p1);
        const bot::Flight::Sample s = f.sample();
        const double v[3] = {s.vel[0], s.vel[1], s.vel[2]};
        double fv[3];
        bot::formation_velocity(v, s.ang[1], s.rates[1], up, fv);
        double worst = 0;
        for (int i = 0; i < 3; ++i) worst = (std::max)(worst, std::fabs((p1[i] - p0[i]) / 0.01 - fv[i]));
        char what[160];
        std::snprintf(what, sizeof(what), "formation velocity matches the point's motion in a turn (worst %.2f m/s; the "
                      "plain lead velocity is %.1f m/s off)", worst, std::fabs((p1[0] - p0[0]) / 0.01 - v[0]) +
                      std::fabs((p1[1] - p0[1]) / 0.01 - v[1]));
        expect(worst < 0.5, what);
    }
    {  // States carry body rates: the turn must come from the headings (heading_rate), not from rates[1]
        bot::Flight f;
        const double spawn[3] = {0.0, 0.0, 2000.0};
        f.start(bot::Script::Breaks, spawn, 0.4, 0.0);
        f.advance_to(2.0);
        const bot::Flight::Sample a = f.sample();
        f.advance_to(2.05);
        const bot::Flight::Sample b = f.sample();
        const double body_yaw = b.rates[1] * std::cos(b.ang[2]);  // what rates[1] holds in a game State
        const double hr = bot::heading_rate(a.ang[1], 2.0, b.ang[1], 2.05);
        const double slot1[3] = {150.0, 50.0, 0.0};
        const double v[3] = {b.vel[0], b.vel[1], b.vel[2]};
        double good[3], bad[3], ideal[3];
        bot::formation_velocity(v, b.ang[1], hr, slot1, good);
        bot::formation_velocity(v, b.ang[1], body_yaw, slot1, bad);
        bot::formation_velocity(v, b.ang[1], b.rates[1], slot1, ideal);
        const double eg = std::hypot(good[0] - ideal[0], good[1] - ideal[1]), eb = std::hypot(bad[0] - ideal[0], bad[1] - ideal[1]);
        char what[200];
        std::snprintf(what, sizeof(what), "the heading rate from two headings 50 ms apart: %.4f rad/s (true %.4f); velocity "
                      "off %.2f m/s, against %.1f m/s from the body yaw rate", hr, static_cast<double>(b.rates[1]), eg, eb);
        expect(eg < 0.5 && eb > 20.0, what);
        expect(bot::heading_rate(3.1, 0.0, -3.1, 0.05) > 1.6 && bot::heading_rate(3.1, 0.0, -3.1, 0.05) < 1.7 &&
                   bot::heading_rate(0.0, 0.0, 1.0, 0.5) == 0.0 && bot::heading_rate(0.0, 1.0, 1.0, 1.0) == 0.0,
               "heading_rate: the short way across +-pi; 0 for samples too far apart or at the same time");
    }
}

struct FhBotRun {
    bot::BotResult res;
    double secs = 0;
};

// Runs the bot on its own thread against the FakeHost, pumping the host (and each()) on this thread until it exits.
template <class F>
FhBotRun fh_run_bot(FakeHost& h, bot::BotConfig cfg, F each) {
    cfg.host = "127.0.0.1:" + std::to_string(h.core.port());
    if (!cfg.log) cfg.log = bot_log;
    FhBotRun out;
    std::atomic<bool> done{false};
    const double t0 = ClientCore::wall_now();
    std::thread th([&] {
        out.res = bot::run_bot(cfg);
        done = true;
    });
    while (!done) {
        h.pump();
        each();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    th.join();
    out.secs = ClientCore::wall_now() - t0;
    for (int i = 0; i < 100; ++i) {  // the Bye and the disconnect
        h.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return out;
}

// M1 (Task 8 review): the bot against a host that sends its jet, PeerInfo and, at the end, a Bye signed 0.
void bot_host_traffic_tests() {
    std::printf("bot: against a host that sends its jet, PeerInfo and Bye 0\n");
    FakeHost h;
    h.send_jet = true;
    if (!h.start()) return expect(false, "host core running");
    bot::BotConfig cfg;
    cfg.script = bot::Script::Straight;
    cfg.rate = 20.0;
    cfg.duration = 10.0;
    cfg.log_remote = true;
    double first_state = -1;
    bool bye_sent = false;
    uint32_t host_sent = 0;
    const FhBotRun r = fh_run_bot(h, cfg, [&] {
        if (first_state < 0 && !h.states.empty()) first_state = ClientCore::wall_now();
        if (!bye_sent && first_state > 0 && ClientCore::wall_now() - first_state > 2.0) {
            bye_sent = true;
            host_sent = h.seq;
            h.bye_and_disconnect(3);
        }
    });
    char what[200];
    std::printf("    bot: code %d after %.1f s; %u host States of %u sent, %u PeerInfo, %u refused, host Bye %d\n", r.res.code,
                r.secs, r.res.host_states, host_sent, r.res.peer_infos, r.res.dropped_rx, r.res.host_bye);
    expect(bye_sent && r.res.code == 4 && r.res.host_bye == 3 && r.secs < 8.0,
           "the host's Bye 3 (signed 0) is logged and the bot exits 4 (the host closed the connection), at once");
    std::snprintf(what, sizeof(what), "the host's jet arrives: %u of %u States, from its announced id 0", r.res.host_states,
                  host_sent);
    expect(host_sent > 30 && r.res.host_states >= host_sent * 8 / 10 && r.res.peer_infos == 1 && r.res.dropped_rx == 0,
           what);
    expect(r.res.sent > 0 && !h.states.empty() && h.byes.empty(), "the bot's own States arrived, and it sent no Bye after the host's");
    h.core.stop(false);
}

// Two bots on one host: each hears of the other (PeerInfo), gets its States relayed, and hears it leave (PeerLeft).
void bot_relay_tests() {
    std::printf("bot: two bots, relay and PeerLeft\n");
    FakeHost h;
    h.send_jet = true;
    if (!h.start()) return expect(false, "host core running");
    bot::BotConfig a, b;
    a.callsign = "ALPHA";
    b.callsign = "BRAVO";
    a.rate = b.rate = 20.0;
    a.duration = 2.0;
    b.duration = 4.0;
    a.script = bot::Script::Orbit;
    b.script = bot::Script::Formation;
    a.host = b.host = "127.0.0.1:" + std::to_string(h.core.port());
    a.log = b.log = bot_log;
    bot::BotResult ra, rb;
    std::atomic<int> done{0};
    std::thread ta([&] {
        ra = bot::run_bot(a);
        ++done;
    });
    std::thread tb([&] {
        rb = bot::run_bot(b);
        ++done;
    });
    while (done < 2) {
        h.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ta.join();
    tb.join();
    std::printf("    ALPHA: code %d, %u relayed, %u PeerInfo, %u PeerLeft; BRAVO: code %d, %u relayed, %u PeerInfo, %u PeerLeft\n",
                ra.code, ra.relayed_states, ra.peer_infos, ra.peer_lefts, rb.code, rb.relayed_states, rb.peer_infos,
                rb.peer_lefts);
    expect(ra.code == 0 && rb.code == 0, "both bots exit 0");
    expect(rb.peer_infos == 2 && rb.peer_lefts == 1, "BRAVO hears of the host and ALPHA, and of ALPHA leaving");
    expect(rb.relayed_states >= 20 && ra.relayed_states > 0 && rb.dropped_rx < 5,
           "each bot gets the other's States relayed (a late one after the PeerLeft may be refused)");
    h.core.stop(false);
}

// Task 9: the FakeHost flies a script, the bot flies formation on it with --lag 150. Checked from the host side: the
// bot's States against the host's true path plus the offset.
void bot_formation_tests() {
    std::printf("bot: formation on the host's jet, --lag 150\n");
    for (int k = 0; k < 2; ++k) {
        const bot::Script hs = k == 0 ? bot::Script::Breaks : bot::Script::Straight;
        const double bound = k == 0 ? 15.0 : 3.0, duration = k == 0 ? 11.0 : 4.0;
        FakeHost h;
        h.send_jet = true;
        const double spawn[3] = {500.0, -300.0, 1200.0};
        bot::Flight fly;
        fly.start(hs, spawn, kFhHeading, 1000.0);
        h.flight = &fly;
        if (!h.start()) return expect(false, "host core running");
        bot::BotConfig cfg;
        cfg.script = bot::Script::Formation;
        cfg.rate = 20.0;
        cfg.duration = duration;
        cfg.lag_ms = 150.0;
        cfg.log_remote = true;
        const FhBotRun r = fh_run_bot(h, cfg, [] {});
        bot::Flight truth;
        truth.start(hs, spawn, kFhHeading, 1000.0);
        std::vector<double> err, verr;
        const double skip = h.states.empty() ? 0.0 : h.states.front().game_time + 1.0;  // the join
        bool mono = true;
        for (size_t i = 0; i < h.states.size(); ++i) {
            const StateMsg& s = h.states[i];
            if (i > 0 && !(s.game_time > h.states[i - 1].game_time)) mono = false;
            if (s.game_time < skip || s.game_time < truth.time()) continue;
            truth.advance_to(s.game_time);
            const bot::Flight::Sample t = truth.sample();
            double want[3];
            bot::formation_point(t.pos, t.ang[1], cfg.offset, want);
            err.push_back(fh_dist(s.pos, want));
            const double tv[3] = {t.vel[0], t.vel[1], t.vel[2]};
            double wv[3];
            bot::formation_velocity(tv, t.ang[1], t.rates[1], cfg.offset, wv);  // Flight's rates[1] is the heading rate
            verr.push_back(std::hypot(s.vel[0] - wv[0], s.vel[1] - wv[1]));
        }
        const double p50 = pct(err, 0.5), p95 = pct(err, 0.95), mx = pct(err, 1.0);
        const double vbound = k == 0 ? 20.0 : 0.5;  // breaks: the turn rate is ~170 ms old at a reversal
        std::printf("    host %s: %zu bot States checked, error p50 %.2f p95 %.2f max %.2f m; the bot's own estimate p50 "
                    "%.2f p95 %.2f m (%u); best rtt %.1f ms, host age p90 %.0f ms, %u of %u host States\n",
                    bot::script_name(hs), err.size(), p50, p95, mx, r.res.formation_p50, r.res.formation_p95,
                    r.res.formation_n, r.res.best_rtt * 1000.0, r.res.host_age_p90 * 1000.0, r.res.host_states, h.seq);
        char what[200];
        expect(r.res.code == 0 && h.byes.size() == 1 && mono, "the bot flies its time, exits 0 and says Bye");
        std::snprintf(what, sizeof(what), "%s: the bot's States sit at the host's position + offset, p95 %.2f m <= %.0f m",
                      bot::script_name(hs), p95, bound);
        expect(err.size() > duration * 20 * 0.6 && p95 <= bound, what);
        std::snprintf(what, sizeof(what), "%s: the bot's velocity is the formation point's (p50 %.1f, p95 %.1f m/s <= %.1f; "
                      "the host sends body rates)", bot::script_name(hs), pct(verr, 0.5), pct(verr, 0.95), vbound);
        expect(pct(verr, 0.95) <= vbound, what);
        std::snprintf(what, sizeof(what), "the bot's own formation estimate agrees with the host's view (p95 %.2f vs %.2f m)",
                      r.res.formation_p95, p95);
        expect(r.res.formation_n > err.size() / 2 && std::fabs(r.res.formation_p95 - p95) < 0.5 * bound + 1.0, what);
        std::snprintf(what, sizeof(what), "--lag 150 doubles into the Pings' round trip: best %.1f ms (270..330)",
                      r.res.best_rtt * 1000.0);
        expect(r.res.best_rtt > 0.27 && r.res.best_rtt < 0.33, what);
        expect(r.res.host_age_p90 > 0.13 && r.res.host_age_p90 < 0.30 && r.res.host_states >= h.seq * 85 / 100,
               "the host's States arrive about 150 ms old, and nearly all of them");
        h.core.stop(false);
    }
}

// --- bot --serve, with ClientCore as the game client ---

struct ServeRun {
    bot::ServeConfig cfg;
    bot::ServeResult res;
    std::atomic<uint16_t> port{0};
    std::atomic<bool> stop{false}, done{false};
    std::thread th;
    bool start() {
        cfg.port = 0;
        cfg.port_out = &port;
        cfg.cancel = &stop;
        cfg.log = bot_log;
        th = std::thread([this] {
            res = bot::run_serve(cfg);
            done = true;
        });
        for (int i = 0; i < 3000 && port == 0 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return port != 0;
    }
    void finish() {
        stop = true;
        if (th.joinable()) th.join();
    }
    ~ServeRun() { finish(); }
};

// The game client's side: polls, keeps the Welcomes, PeerInfos and the host's States, and sends its own jet at 20 Hz
// of host time (straight and level from pos at heading h) while the clock is synced.
struct ServeClient {
    ClientCore core;
    CcRx rx;
    std::vector<WelcomeMsg> welcomes;
    std::vector<PeerInfoMsg> infos;
    std::vector<StateMsg> host;
    std::vector<double> host_wall;
    bool synced = false, hold = false, resumed = false, got_bye = false, ended = false;
    bool follow_join = true;  // move to a Welcome's join position (the game client's one-shot move)
    uint8_t bye_reason = 255;
    uint32_t seq = 0;
    double next_send = 0, pos[3] = {5000.0, 6000.0, 2500.0}, heading = 1.0, t_first = -1;
    size_t seen = 0;
    void step() {
        rx.pull(core);
        for (; seen < rx.ev.size(); ++seen) {
            const ClientEvent& e = rx.ev[seen];
            if (e.kind == ClientEvent::Welcome) {
                WelcomeMsg w{};
                if (decode(e.data, e.len, w) == Status::Ok) {
                    welcomes.push_back(w);
                    if (follow_join && welcome_has_join(w) && welcomes.size() > 1) {  // the two-step join
                        for (int i = 0; i < 3; ++i) pos[i] = w.spawn_pos[i];
                        heading = w.spawn_heading;
                        t_first = -1;
                    }
                }
            } else if (e.kind == ClientEvent::PeerInfo) {
                PeerInfoMsg p{};
                if (decode(e.data, e.len, p) == Status::Ok) infos.push_back(p);
            } else if (e.kind == ClientEvent::State) {
                StateMsg s{};
                if (decode(e.data, e.len, s) == Status::Ok && s.client_id == 0) {
                    host.push_back(s);
                    host_wall.push_back(e.wall_rx);
                }
            } else if (e.kind == ClientEvent::Bye) {
                got_bye = true;
                bye_reason = cc_bye_reason(e);
            } else if (e.kind == ClientEvent::Disconnected) {
                ended = true;
            }
        }
        double t = 0;
        const bool now_synced = core.clock_now(ClientCore::wall_now(), &t);
        if (synced && !now_synced && !welcomes.empty() && !got_bye) hold = true;
        if (hold && now_synced) resumed = true;
        synced = now_synced;
        if (!synced || t < next_send) return;
        next_send = t + 0.05;
        if (t_first < 0) t_first = t;
        StateMsg s{};
        s.seq = ++seq;
        s.game_time = t;
        const double d = 200.0 * (t - t_first);
        s.pos[0] = pos[0] + d * std::sin(heading);
        s.pos[1] = pos[1] + d * std::cos(heading);
        s.pos[2] = pos[2];
        s.vel[0] = static_cast<float>(200.0 * std::sin(heading));
        s.vel[1] = static_cast<float>(200.0 * std::cos(heading));
        s.ang[1] = static_cast<float>(heading);
        core.send_state(s);
    }
};

template <class F>
void serve_run_for(ServeClient& c, double seconds, F done) {
    for (const double until = ClientCore::wall_now() + seconds; ClientCore::wall_now() < until && !done();) {
        c.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void bot_serve_tests() {
    std::printf("bot --serve: the synthetic host, with ClientCore as the client\n");
    {  // a start position, and a 1 s pause 2 s after the jet starts
        ServeRun s;
        s.cfg.script = bot::Script::Orbit;
        s.cfg.has_start = true;
        s.cfg.start[0] = 1000.0;
        s.cfg.start[1] = 2000.0;
        s.cfg.start[2] = 3000.0;
        s.cfg.start_heading = 0.3;
        s.cfg.pause_at = 2.0;
        s.cfg.pause_for = 1.0;
        const std::wstring dir = make_temp_dir();
        s.cfg.record_dir = dir;
        if (!s.start()) return expect(false, "bot serve listening");
        ServeClient c;
        expect(c.core.start("127.0.0.1", s.port, cc_hello(0), cc_client_log), "client start");
        serve_run_for(c, 6.0, [] { return false; });
        s.finish();
        serve_run_for(c, 2.0, [&] { return c.ended; });
        c.core.stop(false);
        const bot::ServeResult& r = s.res;
        std::printf("    serve: code %d, %u client(s), %u Welcome(s), %u jet States sent, %u client States, %u paused steps; "
                    "client got %zu host States\n",
                    r.code, r.clients, r.welcomes, r.jet_states, r.client_states, r.paused_steps, c.host.size());
        expect(r.code == 0 && r.clients == 1 && r.welcomes == 1, "one client, one Welcome");
        bool join_ok = c.welcomes.size() == 1 && c.welcomes[0].client_id == 1 && welcome_has_join(c.welcomes[0]);
        if (join_ok) {
            const WelcomeMsg& w = c.welcomes[0];
            const double v = std::sqrt(w.spawn_vel[0] * w.spawn_vel[0] + w.spawn_vel[1] * w.spawn_vel[1]);
            join_ok = std::fabs(v - bot::kSpeed) < 1.0 && fh_dist(w.spawn_pos, s.cfg.start) < 3000.0 &&
                      std::fabs(w.spawn_pos[2] - 3000.0) < 1.0;
        }
        expect(join_ok, "the Welcome v2 carries a join position on the jet's wing (its speed, its altitude)");
        bool info_ok = false;
        for (const PeerInfoMsg& p : c.infos) info_ok = info_ok || (p.id == 0 && !std::strcmp(p.callsign, "BOTHOST"));
        expect(info_ok, "the client hears of the host (PeerInfo 0, BOTHOST)");
        // Rate in host time (the pause freezes it), robust to the test thread's scheduling.
        double gap_wall = 0, gap_game = 1e9;
        for (size_t i = 1; i < c.host.size(); ++i) {
            const double gw = c.host_wall[i] - c.host_wall[i - 1];
            if (gw > gap_wall) {
                gap_wall = gw;
                gap_game = c.host[i].game_time - c.host[i - 1].game_time;
            }
        }
        const double span = c.host.size() > 1 ? c.host.back().game_time - c.host.front().game_time : 0.0;
        const double hz = span > 0 ? (static_cast<double>(c.host.size()) - 1.0) / span : 0.0;
        char what[320];
        std::snprintf(what, sizeof(what), "id 0 States at 20 Hz of host time (%.2f Hz), %zu of %u sent", hz, c.host.size(),
                      r.jet_states);
        expect(hz > 18.5 && hz < 21.0 && c.host.size() >= r.jet_states * 9 / 10 && c.host.size() > 40, what);
        std::snprintf(what, sizeof(what), "the pause: no States for %.2f s of wall time while host time moved %.3f s; the "
                      "client's clock held and resumed", gap_wall, gap_game);
        expect(gap_wall > 0.8 && gap_game < 0.15 && c.hold && c.resumed && r.paused_steps > 30, what);
        expect(r.client_states > 20, "the synthetic host received the client's States");
        expect(c.got_bye && c.bye_reason == 3 && c.ended, "the end of serving: Bye 3, then the disconnect");
        const std::wstring file = dir + L"\\botserve_001.lzst";
        const std::vector<uint8_t> bytes = read_file(file);
        StreamReader rd(bytes.data(), bytes.size());
        StreamHeader hd{};
        uint32_t kinds[16] = {};
        bool shown_ok = true;
        double max_age = 0, max_dz = 0, min_age = 1e9;
        unsigned bad_len = 0, bad_id = 0;
        StreamRecord rec;
        bool session_first = false;
        if (rd.read_header(hd) == StreamStatus::Ok) {
            while (rd.next(rec) == StreamStatus::Ok) {
                if (kinds[8] == 0 && rec.kind == static_cast<uint8_t>(StreamKind::SessionInfo) &&
                    rec.payload_len == sizeof(SessionInfoPayload) && std::accumulate(kinds, kinds + 16, 0u) == 0) {
                    SessionInfoPayload si{};
                    std::memcpy(&si, rec.payload, sizeof(si));
                    session_first = si.role == static_cast<uint8_t>(SessionRole::BotServe) && si.own_id == 0 &&
                                    std::strcmp(si.callsign, "BOTHOST") == 0;
                }
                if (rec.kind < 16) ++kinds[rec.kind];
                if (rec.kind == static_cast<uint8_t>(StreamKind::RemoteShown)) {
                    RemoteShownPayload sp{};
                    bad_len += rec.payload_len == sizeof(sp) ? 0 : 1;
                    if (rec.payload_len == sizeof(sp)) std::memcpy(&sp, rec.payload, sizeof(sp));
                    bad_id += sp.id == 1 ? 0 : 1;
                    min_age = (std::min)(min_age, static_cast<double>(sp.age));
                    max_age = (std::max)(max_age, static_cast<double>(sp.age));
                    max_dz = (std::max)(max_dz, std::fabs(sp.pos[2] - 2500.0));
                }
            }
        }
        std::snprintf(what, sizeof(what), "--record: botserve_001.lzst with %u HostState, %u StateReceived, %u RemoteShown, "
                      "%u WelcomeSent, %u ClientJoined, %u ClientLeft; RemoteShown age %.3f..%.2f s, %.2f m off the client's "
                      "altitude, %u bad sizes, %u bad ids", kinds[2], kinds[1], kinds[9], kinds[3], kinds[5], kinds[6], min_age,
                      max_age, max_dz, bad_len, bad_id);
        // Ages: on loopback a few ms, but up to about half a second after the pause (the client resumes on a Pong).
        // Ages below 0: the client's clock leads by a millisecond or so on loopback, and it notices the pause only from
        // the Pongs (a few hundred ms), stamping its States ahead of the frozen host time until then.
        shown_ok = bad_len == 0 && bad_id == 0 && min_age > -0.5 && max_age < 1.0 && max_dz < 1.0;
        expect(kinds[2] > 150 && kinds[1] > 20 && kinds[9] > 10 && kinds[3] == 1 && kinds[5] == 1 && kinds[6] == 1 && shown_ok,
               what);
        expect(session_first && kinds[8] == 1, "--record: SessionInfo (bot serve, id 0, BOTHOST) is the first record");
        expect(kinds[7] > 0 && kinds[7] <= r.jet_states && kinds[7] + 2 >= r.jet_states,
               "--record: LocalState holds each State the jet sent");
        {  // the report on the file itself: what the bot showed of the client against the States it received
            LazyWorks::SF2Patch::acmi::Report rep;
            LazyWorks::SF2Patch::acmi::ReportOptions ro;
            const bool ok = LazyWorks::SF2Patch::acmi::make_report(bytes.data(), bytes.size(), bytes.data(), bytes.size(), ro, rep);
            expect(ok && rep.id == 1 && rep.compared > 10 && rep.truth_source.rfind("StateReceived", 0) == 0,
                   "--record: the report compares the shown client with its received States");
        }
        DeleteFileW(file.c_str());
        RemoveDirectoryW(dir.c_str());
    }
    {  // no start position: the jet starts 2 km ahead of the client's first State, and a second Welcome joins it
        ServeRun s;
        s.cfg.script = bot::Script::Straight;
        if (!s.start()) return expect(false, "bot serve listening");
        ServeClient c;
        expect(c.core.start("127.0.0.1", s.port, cc_hello(0), cc_client_log), "client start");
        const double p0[3] = {c.pos[0], c.pos[1], c.pos[2]}, h = c.heading;
        serve_run_for(c, 5.0, [&] { return c.welcomes.size() >= 2 && c.host.size() >= 20; });
        serve_run_for(c, 2.5, [] { return false; });  // the client is on the wing now: no more Welcomes
        s.finish();
        c.core.stop(false);
        const bot::ServeResult& r = s.res;
        expect(!c.welcomes.empty() && !welcome_has_join(c.welcomes[0]), "the first Welcome has no join position");
        // 2 km ahead of the client's first State the serve accepted: along its track (each State is 10 m on).
        const double dj[3] = {r.jet_start[0] - p0[0], r.jet_start[1] - p0[1], r.jet_start[2] - p0[2]};
        const double along = dj[0] * std::sin(h) + dj[1] * std::cos(h), across = dj[0] * std::cos(h) - dj[1] * std::sin(h);
        char what[220];
        std::snprintf(what, sizeof(what), "the jet starts 2 km ahead of the client's first State: %.1f m ahead, %.2f m "
                      "across, heading %.4f", along, across, r.jet_start_heading);
        expect(r.jet_started && along >= 1999.0 && along < 2101.0 && std::fabs(across) < 1.0 && std::fabs(dj[2]) < 1e-6 &&
                   std::fabs(r.jet_start_heading - h) < 1e-6,
               what);
        bool second = c.welcomes.size() == 2 && welcome_has_join(c.welcomes[1]);
        double off = 1e9;
        if (second) {
            const WelcomeMsg& w = c.welcomes[1];
            const double d = bot::kSpeed * (w.host_game_time - r.jet_start_time);
            const double jet[3] = {r.jet_start[0] + d * std::sin(h), r.jet_start[1] + d * std::cos(h), r.jet_start[2]};
            double j[3];
            join_position(jet, h, 0, j);
            off = fh_dist(w.spawn_pos, j);
        }
        std::snprintf(what, sizeof(what), "a second Welcome puts the client on the jet's wing (%.3f m from the join position)",
                      off);
        expect(second && off < 1.0, what);
        std::snprintf(what, sizeof(what), "the client moved there: the serve saw it on the wing and sent no more (%zu "
                      "Welcomes)", c.welcomes.size());
        expect(r.join_confirmed && !r.join_gave_up && c.welcomes.size() == 2, what);
        int infos0 = 0;
        for (const PeerInfoMsg& p : c.infos) infos0 += p.id == 0 ? 1 : 0;
        expect(infos0 == 2 && c.host.size() >= 20, "PeerInfo 0 again after the second Welcome, and the jet's States arrive");
    }
    {  // a client that does not move (or never got the join Welcome): it is re-sent about once a second
        ServeRun s;
        s.cfg.script = bot::Script::Straight;
        if (!s.start()) return expect(false, "bot serve listening");
        ServeClient c;
        c.follow_join = false;
        expect(c.core.start("127.0.0.1", s.port, cc_hello(0), cc_client_log), "client start");
        serve_run_for(c, 8.0, [&] { return c.welcomes.size() >= 4; });
        s.finish();
        c.core.stop(false);
        bool joins = c.welcomes.size() >= 4;
        for (size_t i = 1; i < c.welcomes.size(); ++i) joins = joins && welcome_has_join(c.welcomes[i]);
        char what[160];
        std::snprintf(what, sizeof(what), "the join Welcome is re-sent while the client stays away (%zu Welcomes, none "
                      "dropped by the client's 1 s guard)", c.welcomes.size());
        expect(joins && !s.res.join_confirmed && s.res.welcomes == c.welcomes.size(), what);
    }
}

void bot_two_player_tests() {
    bot_formation_logic_tests();
    bot_host_traffic_tests();
    bot_relay_tests();
    bot_formation_tests();
    bot_serve_tests();
}

void remote_avatar_tests() {
    using namespace LazyWorks::SF2Patch::hooks;
    using avatar_logic::flight_base;
    using avatar_logic::flight_member_no;
    using avatar_logic::same_type;
    std::printf("remote avatar picker\n");
    const auto F = AircraftSide::Friendly, E = AircraftSide::Enemy, U = AircraftSide::Unknown;

    size_t bl = 0;
    int lead = 0;
    expect(flight_base(L"DENVER 11", &bl, &lead) == 6 && lead == 11, "flight: \"DENVER 11\" is flight DENVER, lead 11");
    expect(flight_base(L"GUNSLINGER", &bl, &lead) == 10 && lead == -1, "flight: a bare callsign has no lead number");
    expect(flight_base(L"VF 123456", &bl, &lead) == 9 && lead == -1, "flight: a long number is part of the name");
    expect(flight_base(nullptr, &bl, &lead) == 0 && flight_base(L"12", &bl, &lead) == 0, "flight: null or digits only: none");
    expect(flight_member_no(L"DENVER 12", L"DENVER", 6) == 12 && flight_member_no(L"denver  3", L"DENVER", 6) == 3,
           "member: the callsign plus a number, any case and spacing");
    expect(flight_member_no(L"DENVERX 12", L"DENVER", 6) == -1 && flight_member_no(L"DENVER", L"DENVER", 6) == -1 &&
               flight_member_no(L"DENVER 12A", L"DENVER", 6) == -1 && flight_member_no(L"DEN 12", L"DENVER", 6) == -1 &&
               flight_member_no(nullptr, L"DENVER", 6) == -1,
           "member: another name, no number or a suffix is not a member");
    expect(same_type("F-105D", "f-105d") && !same_type("F-105D", "F-105") && !same_type("", "") &&
               !same_type(nullptr, "F-4E"),
           "type: case-insensitive, exact, never empty");

    // The live R2 case: the player DENVER 11, wingmen 12-14 just after it, plus other aircraft nearer than #2.
    const AvatarCandidate flight[] = {
        {L"DENVER 11", "F-105D", 10, F, true, false, 0},
        {L"DENVER 12", "F-105D", 11, F, false, false, 55},
        {L"DENVER 13", "F-105D", 12, F, false, false, 40},
        {L"DENVER 14", "F-105D", 13, F, false, false, 70},
        {L"COLT 21", "F-105D", 20, F, false, false, 30},
        {L"MIG 31", "MiG-21F", 30, E, false, false, 10},
    };
    constexpr size_t nf = sizeof(flight) / sizeof(flight[0]);
    AvatarRank r;
    size_t i = pick_avatar(flight, nf, "F-105D", &r);
    expect(i == 1 && r.tier == AvatarTier::Wingman && r.flight_no == 12,
           "pick: my #2 (DENVER 12), before a nearer #3, a nearer other flight and a nearer enemy");
    AvatarCandidate f2[nf];
    std::memcpy(f2, flight, sizeof(flight));
    f2[1].taken = true;
    i = pick_avatar(f2, nf, "F-105D", &r);
    expect(i == 2 && r.tier == AvatarTier::Wingman && r.flight_no == 13, "pick: #2 taken -> #3");
    f2[2].taken = f2[3].taken = true;
    i = pick_avatar(f2, nf, "f-105d", &r);
    expect(i == 4 && r.tier == AvatarTier::SameType, "pick: the whole flight taken -> friendly of the same type");
    f2[4].taken = true;
    i = pick_avatar(f2, nf, "F-105D", &r);
    expect(i == nf && r.tier == AvatarTier::None, "pick: no friendly left -> none (never an enemy, even the nearest)");
    f2[5].taken = true;
    expect(pick_avatar(f2, nf, "F-105D", &r) == nf && r.tier == AvatarTier::None, "pick: all taken -> none, never the player");

    // The remote flies another type: the player's own flight still comes first, in slot order.
    i = pick_avatar(flight, nf, "F-4E", &r);
    expect(i == 1 && r.tier == AvatarTier::Wingman && r.flight_no == 12, "pick: other type -> my #2 still (flight first)");
    i = pick_avatar(flight, nf, nullptr, &r);
    expect(i == 1 && r.tier == AvatarTier::Wingman, "pick: unknown wanted type -> my #2");
    // A dead #2 (not in the live list) gives #3.
    {
        const AvatarCandidate no2[] = {flight[0], flight[2], flight[3], flight[4], flight[5]};
        i = pick_avatar(no2, 5, "F-105D", &r);
        expect(i == 1 && r.flight_no == 13, "pick: #2 dead -> #3");
    }

    // Mixed types inside the flight, and a lead number that is not 1.
    const AvatarCandidate mixed[] = {
        {L"Enfield 21", "F-4E", 1, F, true, false, 0},
        {L"ENFIELD 22", "F-4C", 2, F, false, false, 20},
        {L"ENFIELD 23", "F-4E", 3, F, false, false, 90},
        {L"RAVEN 11", "F-4E", 4, F, false, false, 15},
    };
    i = pick_avatar(mixed, 4, "F-4E", &r);
    expect(i == 1 && r.tier == AvatarTier::Wingman && r.flight_no == 22,
           "pick: slot order: #2 of another type before #3 of the wanted type");
    i = pick_avatar(mixed, 4, "F-4C", &r);
    expect(i == 1 && r.tier == AvatarTier::Wingman && r.flight_no == 22, "pick: #2 of the wanted type");

    // No player in the list (it was destroyed) and sides unknown (masks not read): not known to be friendly, so none.
    const AvatarCandidate bare[] = {
        {L"A 1", "", 5, U, false, false, 300},
        {L"B 1", nullptr, 6, U, false, false, 100},
        {L"C 1", "F-4E", 7, U, false, true, 10},
        {L"D 1", "F-4E", 8, U, false, false, std::numeric_limits<double>::quiet_NaN()},
    };
    i = pick_avatar(bare, 4, "F-4E", &r);
    expect(i == 4 && r.tier == AvatarTier::None, "pick: unknown sides -> none (only friendly aircraft are taken)");
    AvatarCandidate bare_f[4];
    std::memcpy(bare_f, bare, sizeof(bare));
    for (AvatarCandidate& b : bare_f) b.side = F;
    i = pick_avatar(bare_f, 4, "F-4E", &r);
    expect(i == 1 && r.tier == AvatarTier::Friendly, "pick: no player, friendly -> the nearest free one, never a taken one");
    const AvatarCandidate tie[] = {
        {L"P", "X", 1, F, true, false, 0},
        {L"Q", "X", 9, F, false, false, 50},
        {L"R", "X", 3, F, false, false, 50},
    };
    expect(pick_avatar(tie, 3, "X", &r) == 2, "pick: equal distance -> the one constructed first");
    expect(pick_avatar(tie, 1, "X", &r) == 1 && pick_avatar(nullptr, 0, "X", &r) == 0,
           "pick: only the player, or empty -> none");

    // The incremental picker gives the same answer whatever the order of the list.
    {
        size_t order[nf];
        for (size_t k = 0; k < nf; ++k) order[k] = k;
        bool same = true;
        int perms = 0;
        do {
            AvatarPicker p(L"DENVER 11", "F-105D");
            for (size_t k = 0; k < nf; ++k) p.offer(order[k], flight[order[k]]);
            same = same && p.found() && p.index() == 1;
            ++perms;
        } while (std::next_permutation(order, order + nf));
        expect(same && perms == 720, "pick: the choice does not depend on the list order (720 orders)");
    }

    // A second flight with the same callsign (DENVER 21-24) is not the player's flight.
    {
        const AvatarCandidate two[] = {
            {L"DENVER 11", "F-105D", 1, F, true, false, 0},
            {L"DENVER 12", "F-105D", 2, F, false, true, 50},
            {L"DENVER 21", "F-105D", 5, F, false, false, 900},
            {L"DENVER 22", "F-105D", 6, F, false, false, 800},
            {L"COLT 11", "F-105D", 9, F, false, false, 400},
        };
        i = pick_avatar(two, 5, "F-105D", &r);
        expect(i == 4 && r.tier == AvatarTier::SameType && r.flight_no == -1,
               "pick: another flight of the same callsign is not a wingman (nearest same type instead)");
        AvatarCandidate t2[5];
        std::memcpy(t2, two, sizeof(two));
        t2[1].taken = false;
        expect(pick_avatar(t2, 5, "F-105D", &r) == 1 && r.tier == AvatarTier::Wingman, "pick: the lead's own tens digit is its flight");
    }
    // A bare player name: x1 is probably the lead's own slot, so x2 comes first, and x1 still before other flights.
    {
        const AvatarCandidate bare_lead[] = {
            {L"GUNSLINGER", "F-100D", 1, F, true, false, 0},
            {L"GUNSLINGER 11", "F-100D", 2, F, false, false, 20},
            {L"GUNSLINGER 12", "F-100D", 3, F, false, false, 60},
            {L"GUNSLINGER 13", "F-100D", 4, F, false, false, 40},
            {L"SABRE 12", "F-100D", 5, F, false, false, 10},
        };
        i = pick_avatar(bare_lead, 5, "F-100D", &r);
        expect(i == 2 && r.tier == AvatarTier::Wingman && r.flight_no == 12, "pick: bare name -> x2 before the x1 slot");
        AvatarCandidate b2[5];
        std::memcpy(b2, bare_lead, sizeof(bare_lead));
        b2[2].taken = b2[3].taken = true;
        i = pick_avatar(b2, 5, "F-100D", &r);
        expect(i == 1 && r.tier == AvatarTier::Wingman && r.flight_no == 11,
               "pick: bare name, x2 and x3 taken -> the x1 member, before a nearer other flight");
    }
    // The LiveAircraft -> candidate mapping (as take_cb uses it), on a synthetic list.
    {
        struct FakeLive {
            void* obj;
            const wchar_t* name;
            unsigned long long seq;
            bool is_player;
            AircraftSide side;
            unsigned long long created_ms;
            const char* type_name;
        };
        int o1 = 0, o2 = 0;
        const FakeLive la{&o1, L"DENVER 12", 7, false, F, 0, "F-105D"};
        const FakeLive lb{&o2, nullptr, 8, false, U, 0, nullptr};
        const double pos[3] = {30, 40, 0}, ref[3] = {0, 0, 0};
        const AvatarHandle held[2] = {{&o1, 99, 1}, {&o2, 8, 1}};  // o1 at another seq (reused address), o2 itself
        const AvatarCandidate ca = avatar_candidate(la, pos, ref, held, 2);
        const AvatarCandidate cb = avatar_candidate(lb, pos, ref, held, 2);
        expect(!ca.taken && cb.taken && std::fabs(ca.dist - 50.0) < 1e-9 && ca.seq == 7 && ca.side == F &&
                   std::wcscmp(ca.name, L"DENVER 12") == 0 && std::strcmp(ca.type_name, "F-105D") == 0,
               "candidate: fields copied, taken by pointer and seq (a reused address is not taken), distance");
        // Within 20 km of the placement first, whatever the tier: a far wingman loses to a near friendly of any type.
        const AvatarCandidate far_wing[] = {
            {L"DENVER 11", "F-105D", 1, F, true, false, 0},
            {L"DENVER 12", "F-105D", 2, F, false, false, 30000},
            {L"COLT 21", "F-4E", 3, F, false, false, 19000},
            {L"MIG 31", "MiG-21F", 4, E, false, false, 25000},
        };
        AvatarRank fr;
        expect(pick_avatar(far_wing, 4, "F-105D", &fr) == 1 && fr.tier == AvatarTier::Wingman,
               "pick: flight slot order: my #2 30 km away beats a nearer friendly of another flight");
        {
            AvatarCandidate fw1[4];
            std::memcpy(fw1, far_wing, sizeof(far_wing));
            fw1[1].taken = true;
            expect(pick_avatar(fw1, 4, "F-105D", &fr) == 2 && fr.nearby && fr.tier == AvatarTier::Friendly,
                   "pick: no free plane in the flight -> fallback: the near friendly (never the enemy)");
        }
        AvatarCandidate fw2[4];
        std::memcpy(fw2, far_wing, sizeof(far_wing));
        fw2[2].dist = 21000;
        expect(pick_avatar(fw2, 4, "F-105D", &fr) == 1 && !fr.nearby && fr.tier == AvatarTier::Wingman,
               "pick: nothing within 20 km -> the best tier among the far ones (my #2), marked not nearby");
        fw2[3].dist = 100;
        expect(pick_avatar(fw2, 4, "F-105D", &fr) == 1 && !fr.nearby && fr.tier == AvatarTier::Wingman,
               "pick: an enemy 100 m away is never taken; the far wingman is");
        fw2[1].taken = true;
        fw2[2].dist = 20000;
        expect(pick_avatar(fw2, 4, "F-105D", &fr) == 2 && fr.nearby && fr.tier == AvatarTier::Friendly,
               "pick: fallback: a friendly at exactly 20 km is near");
        fw2[2].taken = true;
        expect(pick_avatar(fw2, 4, "F-105D", &fr) == 4 && fr.tier == AvatarTier::None,
               "pick: only an enemy left -> no avatar");

        AvatarPicker pk(nullptr, "F-105D");
        expect(!pk.offer(0, cb) && pk.offer(1, ca) && pk.best().tier == AvatarTier::SameType,
               "candidate: null name and type are safe; no player -> no flight, friendly same type");
        char buf[8];
        avatar_logic::to_ascii(buf, sizeof(buf), L"A\u00e9B\x01" L"CDEFGH");
        char nb[4];
        avatar_logic::to_ascii(nb, sizeof(nb), static_cast<const char*>(nullptr));
        expect(std::strcmp(buf, "A?B?CDE") == 0 && nb[0] == 0, "to_ascii: '?' for non-printable, truncated, null safe");
    }

    std::printf("frame order check\n");
    {
        // The player first in every frame: 60 Hz frames of [player, avatar, 3 AI] 0.1 ms apart.
        FrameOrderCheck c;
        int fires = 0;
        for (int fr = 0; fr < 400; ++fr) {
            const double t0 = fr * 16.7;
            if (c.step(t0, true, false)) ++fires;
            if (c.step(t0 + 0.1, false, true)) ++fires;
            for (int k = 0; k < 3; ++k)
                if (c.step(t0 + 0.2 + 0.1 * k, false, false)) ++fires;
        }
        expect(fires == 1 && c.done() && c.frames() == FrameOrderCheck::kFrames &&
                   c.player_first() == FrameOrderCheck::kFrames && !c.avatar_first(),
               "frame order: the player first -> one verdict, no shift");
    }
    {
        // The avatar before the player, with two sub-steps a frame: the avatar is first in every frame.
        FrameOrderCheck c;
        for (int fr = 0; fr < 400 && !c.done(); ++fr) {
            const double t0 = fr * 33.3;
            for (int sub = 0; sub < 2; ++sub) {
                c.step(t0 + sub * 0.5, false, true);
                c.step(t0 + sub * 0.5 + 0.1, true, false);
            }
        }
        expect(c.done() && c.player_first() == 0 && c.avatar_first(), "frame order: the avatar first -> shift");
    }
    {
        // Frames with no avatar step (no client yet) do not count, and nothing is decided early.
        FrameOrderCheck c;
        for (int fr = 0; fr < 1000; ++fr) c.step(fr * 16.7, true, false);
        expect(!c.done() && c.frames() == 0 && !c.avatar_first(), "frame order: no avatar steps -> no frames, no verdict");
        c.reset();
        expect(!c.done() && c.frames() == 0, "frame order: reset");
    }
}

void client_logic_tests() {
    namespace cl = LazyWorks::SF2Patch::net::client_logic;
    namespace rm = LazyWorks::SF2Patch::hooks::replay_math;
    using cl::JoinAction;
    std::printf("client logic\n");

    {
        const double up[3] = {0, 0, 1000}, low[3] = {0, 0, 400};
        const float fast[3] = {0, 200, 0}, slow[3] = {30, 30, 0};
        expect(cl::airborne(up, fast) && !cl::airborne(low, fast) && !cl::airborne(up, slow),
               "airborne: above 500 m and faster than 50 m/s");
        expect(cl::connect_due(true, true, false, 10.0, 10.0) && !cl::connect_due(false, true, false, 10, 0) &&
                   !cl::connect_due(true, false, false, 10, 0) && !cl::connect_due(true, true, true, 10, 0) &&
                   !cl::connect_due(true, true, false, 9.9, 10.0),
               "connect: player known, no core, not blocked, retry time reached; airborne not needed");
        expect(cl::wait_reason(true, true, false) && cl::wait_reason(true, false, true) &&
                   !cl::wait_reason(true, false, false) &&
                   std::string(cl::wait_reason(false, true, true)) == "player not known yet",
               "connect: the waiting reason names what holds it back");
    }
    {
        expect(cl::join_action(false, true, true, false, 5000) == JoinAction::NoJoin,
               "join: a Welcome without a join position never moves");
        expect(cl::join_action(true, false, true, false, 5000) == JoinAction::Off, "join: LAZYSF2_NET_JOIN=none");
        expect(cl::join_action(true, true, true, false, 49) == JoinAction::Close &&
                   cl::join_action(true, true, true, false, 51) == JoinAction::Move,
               "join: moves only when more than 50 m away");
        expect(cl::join_action(true, true, false, false, 2000) == JoinAction::NotAirborne, "join: not when not airborne");
        expect(cl::join_action(true, true, true, true, 2999) == JoinAction::AlreadyJoined &&
                   cl::join_action(true, true, true, true, 3001) == JoinAction::Move,
               "join: after a move, again only when the join position is over 3 km away");
        // The bot's two-step join: a first Welcome without a position, the join 2 km off, then up to 8 re-sent joins
        // while the pilot flies on the wing (within a few hundred metres): exactly one move.
        int moves = 0;
        bool moved = false;
        if (cl::join_action(false, true, true, moved, 0) == JoinAction::Move) ++moves;
        if (cl::join_action(true, true, true, moved, 2000) == JoinAction::Move) {
            ++moves;
            moved = true;
        }
        for (int k = 0; k < 8; ++k)
            if (cl::join_action(true, true, true, moved, 20.0 + 50.0 * k) == JoinAction::Move) ++moves;
        expect(moves == 1, "join: one move for a two-step join with 8 re-Welcomes");
    }
    {
        WelcomeMsg w{};
        w.spawn_pos[0] = 1000;
        w.spawn_pos[1] = 2000;
        w.spawn_pos[2] = 3000;
        w.spawn_vel[1] = 200;
        w.host_game_time = 50;
        double t[3];
        cl::join_target(w, 50.15, t);
        const bool on = std::fabs(t[1] - 2030) < 1e-6 && t[0] == 1000 && t[2] == 3000;
        cl::join_target(w, 49.0, t);
        const bool back = t[1] == 2000;
        cl::join_target(w, 60.0, t);
        expect(on && back && std::fabs(t[1] - 2400) < 1e-6,
               "join target: carried on with the join velocity, never backwards, at most 2 s");
    }
    {
        WelcomeMsg w{};
        w.spawn_pos[1] = 2000;
        w.spawn_vel[1] = 200;
        w.spawn_vel[2] = 20;
        w.spawn_heading = 0.5f;
        w.host_game_time = 50;
        const cl::JoinPose a = cl::join_pose(w, nullptr, 50.5);
        const bool wel = !a.from_state && a.vel[1] == 200 && a.vel[2] == 20 && std::fabs(a.pos[1] - 2100) < 1e-6 &&
                         a.ang[1] == 0.5f && a.ang[2] == 0 && std::fabs(a.ang[0] - std::atan2(20.0, 200.0)) < 1e-6;
        StateMsg st{};
        st.game_time = 51;
        st.pos[0] = 10;
        st.vel[0] = 100;
        st.vel[2] = -5;
        st.ang[0] = 0.1f;
        st.ang[1] = 1.0f;
        st.ang[2] = 0.3f;
        const cl::JoinPose b = cl::join_pose(w, &st, 51.5);
        const bool sta = b.from_state && std::fabs(b.pos[0] - 60) < 1e-6 && b.vel[0] == 100 && b.vel[2] == -5 &&
                         b.ang[0] == 0.1f && b.ang[1] == 1.0f && b.ang[2] == 0.3f;
        st.game_time = 49;  // older than the Welcome: the Welcome wins
        const cl::JoinPose c = cl::join_pose(w, &st, 50.5);
        expect(wel && sta && !c.from_state && c.vel[1] == 200,
               "join pose: the Welcome's velocity and climb, or a newer State's velocity, angles and carried position");
    }
    {
        float v[3] = {0, 100, 7};
        cl::turn_heading(v, rm::kPi / 2);
        const bool east = std::fabs(v[0] - 100) < 1e-3 && std::fabs(v[1]) < 1e-3 && v[2] == 7;
        float u[3] = {3, 4, 0};
        cl::turn_heading(u, 1.234);
        const double h0 = std::atan2(3.0, 4.0), h1 = std::atan2(double(u[0]), double(u[1]));
        expect(east && std::fabs(std::hypot(u[0], u[1]) - 5) < 1e-4 && std::fabs(rm::wrap_pi(h1 - h0 - 1.234)) < 1e-5,
               "turn_heading: heading grows by the angle (north turned 90 deg points east), length and z kept");
        expect(std::fabs(cl::heading_like(4.0f, -1.0) - (2 * rm::kPi - 1.0)) < 1e-5 &&
                   std::fabs(cl::heading_like(1.0f, 4.0) - (4.0 - 2 * rm::kPi)) < 1e-5 &&
                   std::fabs(cl::heading_like(4.0f, 1.0) - 1.0) < 1e-6,
               "heading_like: keeps the convention of the original value ([0, 2pi) or (-pi, pi])");
    }
    {
        char h[64];
        uint16_t port = 0;
        expect(cl::parse_connect_spec("127.0.0.1:7777", h, sizeof(h), &port) && std::strcmp(h, "127.0.0.1") == 0 &&
                   port == 7777,
               "connect spec: numeric address and port");
        expect(cl::parse_connect_spec("pc-2.tail-net.ts.net:1", h, sizeof(h), &port) &&
                   std::strcmp(h, "pc-2.tail-net.ts.net") == 0 && port == 1,
               "connect spec: a name");
        char tiny[4];
        expect(!cl::parse_connect_spec(":7777", h, sizeof(h), &port) && !cl::parse_connect_spec("1.2.3.4", h, sizeof(h), &port) &&
                   !cl::parse_connect_spec("a:0", h, sizeof(h), &port) && !cl::parse_connect_spec("a:65536", h, sizeof(h), &port) &&
                   !cl::parse_connect_spec("a b:7", h, sizeof(h), &port) && !cl::parse_connect_spec("a:7x", h, sizeof(h), &port) &&
                   !cl::parse_connect_spec("a:", h, sizeof(h), &port) &&
                   !cl::parse_connect_spec("abcd:7", tiny, sizeof(tiny), &port) &&
                   !cl::parse_connect_spec(nullptr, h, sizeof(h), &port),
               "connect spec: no host, no port, port 0 or over 65535, bad characters, too long, null");
    }
    {
        // The send schedule: 20 Hz of host time from 60 Hz steps, nothing while held, no burst after the hold.
        expect(cl::may_send(true, true, false) && !cl::may_send(false, true, false) && !cl::may_send(true, false, false) &&
                   !cl::may_send(true, true, true),
               "send: welcomed, a Pong, and no hold");
        SendTicker tick;
        int sent = 0, after = 0;
        double t = 100.0;
        for (int i = 0; i < 600; ++i, t += 1.0 / 60.0)
            if (cl::may_send(true, true, false) && tick.due(t)) ++sent;
        for (int i = 0; i < 120; ++i)  // the host's clock is held: nothing is asked
            if (cl::may_send(true, true, true) && tick.due(t)) ++sent;
        t += 2.0;  // the hold ended with a reset: host time stepped on
        for (int i = 0; i < 6; ++i, t += 1.0 / 60.0)
            if (cl::may_send(true, true, false) && tick.due(t)) ++after;
        expect(sent >= 199 && sent <= 201 && after == 2, "send: 200 States in 10 s at 60 Hz steps; none held; no burst after");
    }
    {
        // ClockWatch on a driven ClockSync: synced at the first Pong, hold on a freeze, reset and synced on resume,
        // a re-Welcome is a re-seed (left to the Welcome's line), the wait for its first Pong is no hold, and an
        // offset jump is a plain reset.
        ClockSync c;
        HostClock hc;
        cl::ClockWatch wch;
        auto upd = [&](double wall) {
            hc.read(wall, c);
            return wch.update(c, hc, wall);
        };
        c.on_welcome(0.0, 100.0, 0.01);
        const unsigned e0 = upd(0.01);
        double w = 0.25;
        c.on_pong(w - 0.01, w + 0.01, 100.0 + w);
        const unsigned e1 = upd(w + 0.02);
        unsigned quiet = 0;
        for (int i = 0; i < 8; ++i) {
            w += 0.25;
            c.on_pong(w - 0.01, w + 0.01, 100.0 + w);
            quiet |= upd(w + 0.02);
        }
        const double frozen_at = 100.0 + w;
        unsigned hold = 0;
        for (int i = 0; i < 4; ++i) {
            w += 0.25;
            c.on_pong(w - 0.01, w + 0.01, frozen_at);
            hold |= upd(w + 0.02);
        }
        const bool held = wch.held();
        w += 0.25;
        c.on_pong(w - 0.01, w + 0.01, frozen_at + 0.25);
        const unsigned resume = upd(w + 0.02);
        w += 1.0;
        c.on_welcome(w, frozen_at + 2.0, 0.01);
        const unsigned rewelcome = upd(w + 0.01);
        w += 0.25;
        c.on_pong(w - 0.01, w + 0.01, frozen_at + 2.25);
        const unsigned after_pong = upd(w + 0.02);
        w += 0.25;
        c.on_pong(w - 0.01, w + 0.01, frozen_at + 2.5 + 1.0);  // the host's clock jumped a second
        const unsigned jump = upd(w + 0.02);
        unsigned stale = 0;
        for (int i = 1; i <= 16; ++i) stale |= upd(w + 0.02 + 0.25 * i);  // 4 s without a Pong
        expect(e0 == 0 && e1 == cl::ClockWatch::kSynced && quiet == 0, "clock watch: synced once at the first Pong");
        expect(hold == cl::ClockWatch::kHold && held, "clock watch: a freeze is one hold");
        expect(resume == (cl::ClockWatch::kReset | cl::ClockWatch::kSynced), "clock watch: the resume resets and syncs");
        expect(rewelcome == (cl::ClockWatch::kReset | cl::ClockWatch::kReseed) && after_pong == 0,
               "clock watch: a re-Welcome is a re-seed; its first Pong is no new sync and the wait is no hold");
        expect(jump == cl::ClockWatch::kReset, "clock watch: an offset jump is a reset, not a re-seed");
        expect(stale == cl::ClockWatch::kHold, "clock watch: no Pong for 3 s is a hold");
    }
    {
        // A host that refuses before the Welcome: the client says each reason once, until cleared.
        cl::RefusalOnce once;
        int said = 0;
        for (int k = 0; k < 20; ++k) said += once.first(2) ? 1 : 0;
        const bool other = once.first(1) && !once.first(1);
        once.clear();
        expect(said == 1 && other && once.first(2), "refusal once: each reason logged once until a Welcome or new player");
        expect(std::strstr(bye_reason_name(2), "no free friendly") && std::strstr(bye_reason_name(1), "busy") &&
                   std::strcmp(bye_reason_name(200), "unknown reason") == 0,
               "bye_reason_name: reasons in words, any number safe");
    }
    {
        // The host's refusal lines: once a minute per key, with the count of those not logged; oldest key evicted.
        RepeatLog rl;
        unsigned more = 99;
        const bool l0 = rl.should_log(7, 0.0, &more) && more == 0;
        int logged = 0;
        for (int k = 1; k <= 11; ++k) logged += rl.should_log(7, 5.0 * k, &more) ? 1 : 0;  // every 5 s up to 55 s
        const bool at60 = rl.should_log(7, 60.0, &more) && more == 11;
        const bool other = rl.should_log(8, 61.0, &more) && more == 0 && !rl.should_log(8, 62.0, &more);
        expect(l0 && logged == 0 && at60 && other, "repeat log: at most once a minute per key, with the count not logged");
        RepeatLog full;
        for (uint32_t k = 0; k < RepeatLog::kSlots; ++k) full.should_log(100 + k, 1.0 + k, &more);
        const bool evicted = full.should_log(999, 20.0, &more) && full.should_log(100, 21.0, &more) && more == 0 &&
                             !full.should_log(101 + 1, 22.0, &more);
        expect(evicted, "repeat log: a new key takes the oldest slot (that key then logs as new)");
    }
}

// An in-memory stream for the Task 12 tests.
struct MemStream {
    std::vector<uint8_t> b = std::vector<uint8_t>(64);
    MemStream() { b.resize(encode_stream_header(100.0, "Korea", b.data(), b.size())); }
    void add(StreamKind k, double t, const void* p, size_t len) {
        uint8_t r[kStreamRecordTotalMax];
        const size_t n = encode_stream_record(k, t, static_cast<const uint8_t*>(p), len, r, sizeof(r));
        b.insert(b.end(), r, r + n);
    }
    template <class M>
    void msg(StreamKind k, double t, const M& m) {
        uint8_t buf[kMaxMessage];
        add(k, t, buf, encode(m, buf, sizeof(buf)));
    }
    void session(SessionRole role, uint16_t id, const char* cs, const char* ac) {
        const SessionInfoPayload si = make_session_info(role, id, cs, ac);
        add(StreamKind::SessionInfo, 100.0, &si, sizeof(si));
    }
    void shown(uint16_t id, double t, const double p[3], const double a[3], uint8_t out) {
        RemoteShownPayload s{};
        s.id = id;
        for (int i = 0; i < 3; ++i) {
            s.pos[i] = p[i];
            s.ang[i] = static_cast<float>(a[i]);
        }
        s.age = 0.1f;
        s.out = out;
        add(StreamKind::RemoteShown, t, &s, sizeof(s));
    }
};

StateMsg state_at(uint16_t id, double t, const double p[3], const double v[3], const double a[3]) {
    StateMsg s{};
    s.client_id = id;
    s.game_time = t;
    for (int i = 0; i < 3; ++i) {
        s.pos[i] = p[i];
        s.vel[i] = static_cast<float>(v[i]);
        s.ang[i] = static_cast<float>(a[i]);
    }
    return s;
}

// Task 12, L2: the new kinds through the writer and back, a truncated tail, and unknown kinds passed over.
void stream_kinds_tests() {
    std::printf("stream: session, local, shown, peer info and avatar records\n");
    const SessionInfoPayload odd = make_session_info(SessionRole::Client, 3, "A\x01" "B", nullptr);
    expect(odd.role == 1 && odd.own_id == 3 && std::strcmp(odd.callsign, "A?B") == 0 && odd.aircraft[0] == 0,
           "make_session_info: non-printable bytes replaced, a null string left empty");
    const SessionInfoPayload longer = make_session_info(SessionRole::Host, 0, "0123456789ABCDEFGHIJ", "x");
    expect(longer.callsign[15] == 0 && std::strncmp(longer.callsign, "0123456789ABCDE", 15) == 0,
           "make_session_info: a long callsign is cut to 15 characters");

    const std::wstring dir = make_temp_dir();
    expect(!dir.empty(), "temp dir");
    if (dir.empty()) return;
    const SessionInfoPayload si = make_session_info(SessionRole::Client, 2, "VIEW", "F-105D");
    {
        auto w = std::make_unique<StreamWriter>();
        expect(w->open(dir.c_str(), 50.0, "Korea", stream_log, kStreamMaxFileBytes, L"client", &si), "open with a session");
        const double p[3] = {1, 2, 3}, v[3] = {200, 0, 0}, a[3] = {0, 1.5, 0};
        uint8_t buf[kMaxMessage];
        const StateMsg ls = state_at(2, 50.5, p, v, a);
        w->record(StreamKind::LocalState, 50.5, buf, encode(ls, buf, sizeof(buf)));
        PeerInfoMsg pi{};
        pi.id = 0;
        set_str(pi.callsign, "BOSS");
        set_str(pi.aircraft, "F-105D");
        w->record(StreamKind::PeerInfoReceived, 50.6, buf, encode(pi, buf, sizeof(buf)));
        RemoteShownPayload rs{};
        rs.id = 0;
        rs.pos[0] = 7.0;
        rs.out = 3;
        w->record(StreamKind::RemoteShown, 50.7, &rs, sizeof(rs));
        const uint8_t junk[5] = {1, 2, 3, 4, 5};
        w->record(static_cast<StreamKind>(77), 50.8, junk, sizeof(junk));  // a kind from a later version
        const AvatarEventPayload ae{0, static_cast<uint8_t>(AvatarEventType::Parked)};
        w->record(StreamKind::AvatarEvent, 50.9, &ae, sizeof(ae));
        w->close(true);
    }
    const std::vector<uint8_t> f = read_file(dir + L"\\client_001.lzst");
    std::vector<uint8_t> kinds;
    bool fields = true;
    auto read_all_kinds = [&](size_t size, StreamStatus* end) {
        kinds.clear();
        StreamReader rd(f.data(), size);
        StreamHeader h{};
        if (rd.read_header(h) != StreamStatus::Ok) return;
        fields = fields && std::strcmp(h.theatre, "Korea") == 0 && h.start_game_time == 50.0;
        StreamRecord rec;
        StreamStatus st;
        while ((st = rd.next(rec)) == StreamStatus::Ok) {
            kinds.push_back(rec.kind);
            if (rec.kind == static_cast<uint8_t>(StreamKind::SessionInfo)) {
                SessionInfoPayload got{};
                fields = fields && rec.payload_len == sizeof(got) && rec.time == 50.0;
                if (rec.payload_len == sizeof(got)) std::memcpy(&got, rec.payload, sizeof(got));
                fields = fields && got.role == 1 && got.own_id == 2 && std::strcmp(got.callsign, "VIEW") == 0 &&
                         std::strcmp(got.aircraft, "F-105D") == 0;
            } else if (rec.kind == static_cast<uint8_t>(StreamKind::PeerInfoReceived)) {
                PeerInfoMsg got{};
                fields = fields && decode(rec.payload, rec.payload_len, got) == Status::Ok && got.id == 0 &&
                         std::strcmp(got.callsign, "BOSS") == 0;
            } else if (rec.kind == static_cast<uint8_t>(StreamKind::LocalState)) {
                StateMsg got{};
                fields = fields && decode(rec.payload, rec.payload_len, got) == Status::Ok && got.client_id == 2;
            } else if (rec.kind == static_cast<uint8_t>(StreamKind::RemoteShown)) {
                RemoteShownPayload got{};
                fields = fields && rec.payload_len == sizeof(got);
                if (rec.payload_len == sizeof(got)) std::memcpy(&got, rec.payload, sizeof(got));
                fields = fields && got.pos[0] == 7.0 && got.out == 3;
            } else if (rec.kind == 77) {
                fields = fields && rec.payload_len == 5;  // passed to the caller with its length: skippable
            }
        }
        *end = st;
    };
    StreamStatus end = StreamStatus::Ok;
    read_all_kinds(f.size(), &end);
    const std::vector<uint8_t> want = {8, 7, 10, 9, 77, 11};
    expect(end == StreamStatus::End && kinds == want && fields,
           "new kinds round trip: SessionInfo first, then LocalState, PeerInfoReceived, RemoteShown, an unknown kind, "
           "AvatarEvent");
    read_all_kinds(f.size() - 2, &end);  // the tail cut inside the last record
    expect(end == StreamStatus::Truncated && kinds.size() == 5 && kinds.back() == 77,
           "a truncated tail: the records before it are kept");
    DeleteFileW((dir + L"\\client_001.lzst").c_str());
    RemoveDirectoryW(dir.c_str());
}

// Task 12, L1: the comparison report on synthetic streams with a known error.
void net_report_tests() {
    std::printf("net report\n");
    namespace ac = LazyWorks::SF2Patch::acmi;
    // The truth: a jet flying east at 200 m/s, 60 Hz, host time 100..130.
    auto straight = [](double t, double p[3], double v[3], double a[3]) {
        p[0] = 1000.0 + 200.0 * (t - 100.0);
        p[1] = 5000.0;
        p[2] = 3000.0;
        v[0] = 200.0;
        v[1] = v[2] = 0.0;
        a[0] = a[2] = 0.0;
        a[1] = 1.5707963267948966;  // east
    };
    MemStream truth;
    truth.session(SessionRole::BotServe, 0, "BOTHOST", "F-105D");
    for (int i = 0; i <= 30 * 60; ++i) {
        const double t = 100.0 + i / 60.0;
        double p[3], v[3], a[3];
        straight(t, p, v, a);
        truth.msg(StreamKind::HostState, t, state_at(0, t, p, v, a));
    }
    // The viewer: the host's jet shown 10 m ahead along its track, 10 Hz; one Snap, one Coast record (not compared).
    MemStream viewer;
    viewer.session(SessionRole::Client, 1, "VIEW", "F-105D");
    for (int i = 0; i < 300; ++i) {
        const double t = 100.05 + i * 0.1;
        double p[3], v[3], a[3];
        straight(t, p, v, a);
        p[0] += 10.0;
        if (i == 200) p[0] = -1e6;  // a coasting record holds the last shown place: never compared
        viewer.shown(0, t, p, a, i == 200 ? 4 : (i == 50 ? 1 : 3));
    }
    ac::Report r;
    ac::ReportOptions ro;
    bool ok = ac::make_report(viewer.b.data(), viewer.b.size(), truth.b.data(), truth.b.size(), ro, r);
    const ac::ReportRow& o = r.overall;
    char what[256];
    std::snprintf(what, sizeof(what),
                  "report: a constant 10 m along-track error is 10 m along (median %.3f, p95 %.3f), 0 across (max %.4f), "
                  "0 vertical (max %.4f); %zu compared",
                  o.along.median, o.along.p95, o.cross.max, o.vert.max, r.compared);
    expect(ok && r.id == 0 && std::fabs(o.along.median - 10.0) < 1e-3 && std::fabs(o.along.p95 - 10.0) < 1e-3 &&
               std::fabs(o.pos.max - 10.0) < 1e-3 && o.cross.max < 1e-3 && o.vert.max < 1e-3 && r.compared == 299,
           what);
    expect(ok && o.snaps == 1 && std::fabs(o.coast_s - 0.1) < 1e-6 && o.ang.max < 0.01 && o.accel_max_g < 0.01 &&
               r.truth_source.rfind("HostState", 0) == 0,
           "report: one snap, 0.1 s coasting, no angle error, a smooth path, truth from HostState");
    expect(r.rows.size() == 3 && r.rows[0].pos.n >= 99 && r.rows[0].pos.n <= 101 && r.rows[0].snaps == 1,
           "report: three 10 s rows");
    const std::string text = ac::format_report(r);
    expect(text.find("overall") != std::string::npos && text.find("VIEW") != std::string::npos &&
               text.find("BOTHOST") != std::string::npos,
           "report: text names both files' sessions and has an overall row");
    expect(!r.viewer_truncated && !r.truth_truncated && text.find("truncated") == std::string::npos,
           "report: complete files give no truncation warning");
    {
        MemStream cut = viewer;
        const uint8_t tail[7] = {30, 0, 0, 0, 9, 1, 2};  // a length field and part of the record
        cut.b.insert(cut.b.end(), tail, tail + sizeof(tail));
        ac::Report rc;
        const bool okc = ac::make_report(cut.b.data(), cut.b.size(), truth.b.data(), truth.b.size(), ro, rc);
        expect(okc && rc.viewer_truncated && !rc.truth_truncated && rc.compared == r.compared &&
                   ac::format_report(rc).find("warning: the viewer file ends in a truncated record") != std::string::npos,
               "report: a truncated viewer file still reports, with a warning");
    }

    // A right turn (0.1 rad/s, radius 2000 m), shown 10 m right of the track: 10 m across, 0 along.
    {
        auto turn = [](double t, double p[3], double v[3], double a[3]) {
            const double h = 0.1 * (t - 100.0), R = 2000.0;
            p[0] = -R * std::cos(h);
            p[1] = R * std::sin(h);
            p[2] = 3000.0;
            v[0] = 200.0 * std::sin(h);
            v[1] = 200.0 * std::cos(h);
            v[2] = 0.0;
            a[0] = 0.0;
            a[1] = std::remainder(h, 2.0 * 3.141592653589793);
            a[2] = 0.0;
        };
        MemStream tt, vv;
        for (int i = 0; i <= 20 * 60; ++i) {
            const double t = 100.0 + i / 60.0;
            double p[3], v[3], a[3];
            turn(t, p, v, a);
            tt.msg(StreamKind::LocalState, t, state_at(2, t, p, v, a));
        }
        for (int i = 0; i < 190; ++i) {
            const double t = 100.5 + i * 0.1;
            double p[3], v[3], a[3];
            turn(t, p, v, a);
            const double h = 0.1 * (t - 100.0);
            p[0] += 10.0 * std::cos(h);
            p[1] -= 10.0 * std::sin(h);
            vv.shown(2, t, p, a, 3);
        }
        ac::Report rt;
        ac::ReportOptions rot;
        rot.id = 2;
        const bool okt = ac::make_report(vv.b.data(), vv.b.size(), tt.b.data(), tt.b.size(), rot, rt);
        std::snprintf(what, sizeof(what), "report: in a turn, 10 m right of the track is 10 m across (median %.3f), 0 along (max %.3f)",
                      rt.overall.cross.median, rt.overall.along.max);
        expect(okt && std::fabs(rt.overall.cross.median - 10.0) < 0.01 && rt.overall.along.max < 0.01 &&
                   rt.truth_source.rfind("LocalState", 0) == 0 && rt.overall.truth_accel_max_g > 1.9 &&
                   rt.overall.truth_accel_max_g < 2.2,
               what);
    }

    // --offset: the truth is the host's jet plus (right, back, up); a formation point 10 m ahead is 10 m along.
    {
        MemStream host;  // one file: the host's own jet, and what it showed of client 1
        host.session(SessionRole::Host, 0, "HOST", "F-105D");
        for (int i = 0; i <= 10 * 60; ++i) {
            const double t = 100.0 + i / 60.0;
            double p[3], v[3], a[3];
            straight(t, p, v, a);
            host.msg(StreamKind::HostState, t, state_at(0, t, p, v, a));
        }
        for (int i = 0; i < 90; ++i) {
            const double t = 100.5 + i * 0.1;
            double p[3], v[3], a[3];
            straight(t, p, v, a);  // east: right is south (-y), back is west (-x)
            const double q[3] = {p[0] - 50.0 + 10.0, p[1] - 150.0, p[2] + 20.0};
            host.shown(1, t, q, a, 3);
        }
        ac::Report rf;
        ac::ReportOptions rof;
        rof.has_offset = true;
        rof.offset[0] = 150.0;
        rof.offset[1] = 50.0;
        rof.offset[2] = 20.0;
        const bool okf = ac::make_report(host.b.data(), host.b.size(), host.b.data(), host.b.size(), rof, rf);
        expect(okf && rf.id == 1 && std::fabs(rf.overall.along.median - 10.0) < 1e-3 && rf.overall.cross.max < 1e-3 &&
                   rf.overall.vert.max < 1e-3,
               "report --offset: the host's jet plus the formation offset is the truth");
        ac::ReportOptions none;  // without the offset the host file has no States of id 1
        ac::Report rn;
        expect(!ac::make_report(host.b.data(), host.b.size(), host.b.data(), host.b.size(), none, rn) &&
                   rn.error.find("no States of id 1") != std::string::npos,
               "report: no truth for the id is an error");
    }

    // Files from different sessions: no overlap in host time.
    {
        MemStream late;
        const double p[3] = {0, 0, 1000}, a[3] = {0, 0, 0};
        for (int i = 0; i < 10; ++i) late.shown(0, 500.0 + i * 0.1, p, a, 3);
        ac::Report rl;
        expect(!ac::make_report(late.b.data(), late.b.size(), truth.b.data(), truth.b.size(), ro, rl) &&
                   rl.error.find("do not overlap") != std::string::npos &&
                   ac::format_report(rl).find("no report") != std::string::npos,
               "report: files that do not overlap in host time say so");
        ac::ReportOptions r7;
        r7.id = 7;
        expect(!ac::make_report(viewer.b.data(), viewer.b.size(), truth.b.data(), truth.b.size(), r7, rl) &&
                   rl.error.find("id 7") != std::string::npos,
               "report: an id the viewer never showed is an error");
    }
}

// Task 12: client and bot files convert, and ghosts are written only with the flag.
void acmi_ghost_tests() {
    std::printf("acmi: client files and ghosts\n");
    namespace ac = LazyWorks::SF2Patch::acmi;
    MemStream c;
    c.session(SessionRole::Client, 1, "VIEW", "F-105D");
    PeerInfoMsg pi{};
    pi.id = 0;
    set_str(pi.callsign, "BOSS");
    set_str(pi.aircraft, "F-105D");
    c.msg(StreamKind::PeerInfoReceived, 100.0, pi);
    for (int i = 0; i < 20; ++i) {
        const double t = 100.1 + i * 0.05;
        const double p[3] = {200.0 * i, 0, 3000}, v[3] = {200, 0, 0}, a[3] = {0, 1.5707963, 0};
        const double q[3] = {200.0 * i, -150, 3000};
        c.msg(StreamKind::StateReceived, t + 0.1, state_at(0, t, p, v, a));
        c.msg(StreamKind::LocalState, t, state_at(1, t, q, v, a));
        if (i % 2 == 0) c.shown(0, t + 0.1, p, a, 3);
    }
    const double zero[3] = {0, 0, 0};
    c.shown(0, 101.5, zero, zero, 4);  // coasting
    ac::Options opt;
    opt.max_rate = 0;
    std::string plain, ghosts;
    ac::Info ip, ig;
    expect(ac::convert_stream(c.b.data(), c.b.size(), opt, plain, ip), "client stream converts");
    opt.ghosts = true;
    expect(ac::convert_stream(c.b.data(), c.b.size(), opt, ghosts, ig), "client stream converts with --ghosts");
    expect(plain.find("Strike Fighters 2 client stream") != std::string::npos &&
               plain.find("\n1,T=") != std::string::npos && plain.find("Name=F-105D,Pilot=BOSS") != std::string::npos &&
               plain.find("\n101,T=") != std::string::npos && plain.find("Name=F-105D,Pilot=VIEW") != std::string::npos,
           "client file: the host (from PeerInfoReceived) and the own jet (from SessionInfo and LocalState)");
    expect(plain.find("as seen by") == std::string::npos && plain.find("\n200,") == std::string::npos &&
               ip.ghost_samples == 0 && ip.skipped_bad == 0,
           "no ghosts without the flag");
    expect(ghosts.find("\n200,T=") != std::string::npos &&
               ghosts.find("Name=BOSS as seen by VIEW,Type=Air+FixedWing,Coalition=Neutrals,Color=Green") !=
                   std::string::npos &&
               ig.ghost_samples == 10 && ig.objects == ip.objects + 1,
           "--ghosts: the shown track is its own green object, coasting records left out");
}

}  // namespace

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

void world_stream_tests() {
    std::printf("world stream\n");
    expect(sizeof(WorldObjState) == 35 && sizeof(WorldStatesHead) == 13 && kWorldStatesBytes == 993 &&
               sizeof(WorldEventRec) == 8 && kWorldEventsBytes == 525 && kWorldStatesBytes < 1024,
           "stream: sizes (35 per object, 28 per datagram = 993 bytes; events 8 each, 525)");
    ObjPose p{{123456.789, -98765.4321, 3048.25}, {250.0f, -31.5f, 4.2f}, {0.2f, 3.1f, -1.4f}, {0.05f, -0.3f, 1.2f}};
    WorldObjState q{};
    quantise(p, &q);
    ObjPose r{};
    dequantise(q, &r);
    bool close = true;
    for (int i = 0; i < 3; ++i) {
        close = close && std::fabs(r.pos[i] - p.pos[i]) <= 0.005 && std::fabs(r.vel[i] - p.vel[i]) <= 0.025f &&
                std::fabs(r.rates[i] - p.rates[i]) <= 0.0005f;
        double da = std::fmod(std::fabs(r.ang[i] - p.ang[i]), 6.283185307);
        if (da > 3.14159265) da = 6.283185307 - da;
        close = close && da <= 0.0001;
    }
    expect(close, "stream: quantisation within half a step (5 mm, 2.5 cm/s, 0.0001 rad, 0.0005 rad/s)");
    ObjPose big{{1e9, -1e9, std::numeric_limits<double>::quiet_NaN()}, {5000, -5000, 0}, {0, 0, 0}, {100, -100, 0}};
    quantise(big, &q);
    expect(q.pos[0] == INT32_MAX && q.pos[1] == INT32_MIN && q.pos[2] == 0 && q.vel[0] == INT16_MAX &&
               q.vel[1] == INT16_MIN && q.rates[0] == INT16_MAX,
           "stream: out-of-range values clamp, NaN gives 0");
    ObjPose wrap{{0, 0, 0}, {0, 0, 0}, {3.14159265358979f, -3.14159265358979f, 7.0f}, {0, 0, 0}};
    quantise(wrap, &q);
    dequantise(q, &r);
    expect(std::fabs(std::fabs(r.ang[0]) - 3.14159265f) < 0.001f && std::fabs(r.ang[2] - (7.0f - 6.2831853f)) < 0.001f,
           "stream: angles wrap into -pi..pi");

    std::vector<WorldObjState> recs(kWorldStatesMax + 1);
    for (size_t i = 0; i < recs.size(); ++i) {
        recs[i].kind = 1;
        recs[i].a = static_cast<uint16_t>(i);
        recs[i].b = 2;
    }
    uint8_t b[1100];
    WorldStatesHead sh{};
    const WorldObjState* sr = nullptr;
    const size_t n = encode_world_states(9, 123456, recs.data(), kWorldStatesMax, b, sizeof(b));
    expect(n == 993 && peek_world_type(b, n) == 23 && decode_world_states(b, n, sh, &sr) == Status::Ok && sh.count == 28 &&
               sh.seq == 9 && sh.host_ms == 123456 && sr[27].a == 27,
           "stream: a full states message round-trips");
    expect(encode_world_states(1, 0, recs.data(), kWorldStatesMax + 1, b, sizeof(b)) == 0 &&
               encode_world_states(1, 0, recs.data(), 0, b, sizeof(b)) == 0,
           "stream: 0 or more than 28 states are refused");
    expect(decode_world_states(b, n - 1, sh, &sr) == Status::Truncated && decode_world_states(b, n + 1, sh, &sr) == Status::Oversized,
           "stream: states length must match the count");
    size_t msgs = 0, total = 0;
    world_batches(recs.data(), 60, kWorldStatesMax, [&](const WorldObjState*, size_t k) {
        ++msgs;
        total += k;
    });
    expect(msgs == 3 && total == 60 && world_states_bytes(60) == 3 * 13 + 60 * 35, "stream: 60 states make 3 messages");
    WorldEventRec ev[3] = {{kEventDestroyed, 1, 3, 1, 0}, {kEventAwake, 1, 4, 0, 0}, {kEventPlacedDamage, 3, 12, 7, 60}};
    WorldEventsHead eh{};
    const WorldEventRec* er = nullptr;
    const size_t en = encode_world_events(2, 5000, ev, 3, b, sizeof(b));
    expect(en == 13 + 24 && decode_world_events(b, en, eh, &er) == Status::Ok && eh.count == 3 && er[2].value == 60,
           "stream: events round-trip");

    expect(world_send_interval(5000) == 0.1 && world_send_interval(50000) == 0.5 && world_send_interval(200000) == 1.0 &&
               world_send_interval(std::numeric_limits<double>::quiet_NaN()) == 1.0,
           "stream: 10 Hz near (< 20 km), 2 Hz to 80 km, 1 Hz beyond");
    WorldSendClock clk;
    const uint64_t k = world_key(1, 3, 1);
    expect(clk.due(k, 10.0, 1000) && !clk.due(k, 10.05, 1000) && clk.due(k, 10.1, 1000) && !clk.due(k, 10.5, 50000) &&
               clk.due(k, 10.6, 50000) && clk.due(k, 5.0, 1000),
           "stream: the send clock follows the distance, and a clock going back resets it");

    WorldEventTracker tr;
    std::vector<WorldEventRec> out;
    WorldSeen s0;
    s0.alive = true;
    s0.asleep = true;
    tr.observe(1, 5, 0, s0, &out);
    const bool first_silent = out.empty();
    WorldSeen s1 = s0;
    s1.asleep = false;
    s1.awake = true;
    tr.observe(1, 5, 0, s1, &out);
    WorldSeen s2 = s1;
    s2.level = 3;
    tr.observe(1, 5, 0, s2, &out);
    WorldSeen s3 = s2;
    s3.alive = false;
    s3.level = 5;
    tr.observe(1, 5, 0, s3, &out);
    tr.observe(1, 5, 0, s3, &out);  // nothing more once destroyed
    WorldSeen fl;
    fl.waypoint = 2;
    tr.observe(1, 4, kFlightKey, fl, &out);  // first sighting: remembered
    fl.waypoint = 3;
    tr.observe(1, 4, kFlightKey, fl, &out);
    tr.observe(1, 4, kFlightKey, fl, &out);  // unchanged: nothing
    WorldSeen pl;
    tr.observe(3, 12, 7, pl, &out);
    pl.level = 40;
    tr.observe(3, 12, 7, pl, &out);
    expect(first_silent && out.size() == 4 && out[0].event == kEventAwake && out[1].event == kEventDestroyed &&
               out[2].event == kEventWaypoint && out[2].value == 3 && out[2].b == kFlightKey &&
               out[3].event == kEventPlacedDamage && out[3].value == 40,
           "stream: events on change only: awake, destroyed once, a flight's waypoint, placed damage delta");

    // Bytes per second, the T6 estimate: 8 objects near (10 Hz) + 17 far (1 Hz) a second.
    const size_t per_s = world_states_bytes(8) * 10 + world_states_bytes(17);
    std::printf("    estimate: 25 movers (8 near) = %zu bytes/s of states before UDP/ENet headers\n", per_s);
    expect(per_s < 4000, "stream: a typical mission's states stay under 4 KB/s");

    // Loopback: the host core's world queue reaches a welcomed client core's world ring.
    FakeHost h;
    if (!h.start()) return expect(false, "stream loopback: host core");
    auto c = std::make_unique<ClientCore>();
    c->start("127.0.0.1", h.core.port(), cc_hello(0), cc_client_log);
    CcRx rx;
    const bool welcomed = cc_run(&h, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Welcome); });
    const bool sent = h.core.send_world(h.slot, h.gen, b, en, true) &&
                      h.core.send_world(h.slot, h.gen, b, 0, false) == false;
    uint8_t sb[1100];
    const size_t sn = encode_world_states(1, 1000, recs.data(), kWorldStatesMax, sb, sizeof(sb));
    const bool sent2 = h.core.send_world(h.slot, h.gen, sb, sn, false);
    WorldPacket got[8];
    size_t ng = 0;
    bool have_e = false, have_s = false;
    cc_run(&h, nullptr, *c, rx, 3.0, [&] {
        ng = c->poll_world(got, 8);
        for (size_t i = 0; i < ng; ++i) {
            have_e = have_e || peek_world_type(got[i].data, got[i].len) == 24;
            have_s = have_s || (peek_world_type(got[i].data, got[i].len) == 23 && got[i].len == 993);
        }
        return have_e && have_s;
    });
    expect(welcomed && sent && sent2 && have_e && have_s && c->counters().world >= 2,
           "stream loopback: events (reliable) and a full states message (unreliable) arrive");
    c->stop(false);
    h.core.stop(false);
}

void world_snapshot_tests() {
    std::printf("world snapshot\n");
    expect(sizeof(WorldSnapshotMsg) == 15, "snapshot: a 15-byte head, then WorldEvents parts");
    WorldEventTracker tr;
    std::vector<WorldEventRec> ev;
    WorldSeen awake;
    awake.awake = true;
    tr.observe(1, 2, 0, awake, &ev);
    WorldSeen dead;
    dead.alive = false;
    tr.observe(1, 3, 1, dead, &ev);
    WorldSeen sleeping;
    sleeping.asleep = true;
    tr.observe(1, 3, 0, sleeping, &ev);
    WorldSeen placed;
    placed.level = 40;
    tr.observe(3, 12, 7, placed, &ev);
    WorldSeen intact;
    tr.observe(3, 12, 8, intact, &ev);
    WorldSeen flight;
    flight.waypoint = 4;
    tr.observe(1, 2, kFlightKey, flight, &ev);
    std::vector<WorldEventRec> snap;
    tr.snapshot(&snap);
    // Everything above was the mission's start: only the flight's waypoint differs from what a fresh copy has.
    expect(ev.empty() && snap.size() == 1 && snap[0].event == kEventWaypoint && snap[0].value == 4,
           "snapshot: the start state is not resent (only the flights' waypoints)");
    // The host flies alone: a kill, a building hit, a flight woken, then a client joins.
    WorldSeen killed;
    killed.alive = false;
    tr.observe(1, 2, 0, killed, &ev);
    WorldSeen hit;
    hit.level = 70;
    tr.observe(3, 12, 8, hit, &ev);
    WorldSeen woke = sleeping;
    woke.asleep = false;
    woke.awake = true;
    tr.observe(1, 3, 0, woke, &ev);
    snap.clear();
    tr.snapshot(&snap);
    bool has_kill = false, has_hit = false, has_wake = false, has_old_dead = false;
    for (const auto& r : snap) {
        has_kill = has_kill || (r.event == kEventDestroyed && r.a == 2 && r.b == 0);
        has_old_dead = has_old_dead || (r.event == kEventDestroyed && r.a == 3 && r.b == 1);
        has_hit = has_hit || (r.event == kEventPlacedDamage && r.b == 8 && r.value == 70);
        has_wake = has_wake || (r.event == kEventAwake && r.a == 3 && r.b == 0);
    }
    expect(has_kill && has_hit && has_wake && !has_old_dead,
           "snapshot: the first joiner gets every death, hit and wake-up since the start (not what was dead at the start)");
    std::vector<WorldEventRec> wps;
    tr.waypoints(&wps);
    expect(wps.size() == 1 && wps[0].value == 4, "snapshot: the periodic waypoint resend lists every flight");
    WorldSnapshotMsg h{};
    h.mission_id = 9;
    h.host_ms = 600000;
    h.records = 5;
    h.parts = 1;
    uint8_t b[32];
    WorldSnapshotMsg r{};
    expect(encode(h, b, sizeof(b)) == 15 && peek_world_type(b, 15) == 26 && decode(b, 15, r) == Status::Ok &&
               r.host_ms == 600000 && r.parts == 1,
           "snapshot: head round trip");
    std::printf("    estimate: 150 records = 3 parts, %zu bytes once per join\n",
                sizeof(WorldSnapshotMsg) + 3 * sizeof(WorldEventsHead) + 150 * sizeof(WorldEventRec));
    // The client core takes the head and its parts into the never-dropped ring, in order.
    FakeHost fh;
    if (!fh.start()) return expect(false, "snapshot loopback: host core");
    auto c = std::make_unique<ClientCore>();
    c->start("127.0.0.1", fh.core.port(), cc_hello(0), cc_client_log);
    CcRx rx;
    const bool welcomed = cc_run(&fh, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Welcome); });
    uint8_t e[600];
    const size_t en = encode_world_events(1, 600000, snap.data(), snap.size(), e, sizeof(e));
    const bool sent = fh.core.send_world(fh.slot, fh.gen, b, 15, true) && fh.core.send_world(fh.slot, fh.gen, e, en, true);
    WorldPacket got[4];
    std::vector<uint8_t> order;
    cc_run(&fh, nullptr, *c, rx, 3.0, [&] {
        const size_t n = c->poll_world(got, 4);
        for (size_t i = 0; i < n; ++i) order.push_back(peek_world_type(got[i].data, got[i].len));
        return order.size() >= 2;
    });
    expect(welcomed && sent && order.size() == 2 && order[0] == 26 && order[1] == 24,
           "snapshot loopback: the head, then its events, reach the client in order");
    c->stop(false);
    fh.core.stop(false);
}

void world_avatar_tests() {
    std::printf("world jet state\n");
    expect(sizeof(AvatarStateMsg) == 20, "jet state: 20 bytes (reliable, on change)");
    AvatarStateMsg a{};
    a.peer = 2;
    a.seq = 7;
    a.radar_mode = 3;
    a.target_kind = 1;
    a.target_a = 4;
    a.target_b = 1;
    a.config = 0x0900;
    uint8_t b[32];
    AvatarStateMsg r{};
    expect(encode(a, b, sizeof(b)) == 20 && peek_world_type(b, 20) == 25 && decode(b, 20, r) == Status::Ok &&
               r.peer == 2 && r.radar_mode == 3 && r.target_a == 4 && r.config == 0x0900 &&
               decode(b, 19, r) == Status::Truncated,
           "jet state: round trip");
    expect(radar_mode_of(1, 0, false) == 0 && radar_mode_of(2, 1, false) == 1 && radar_mode_of(3, 2, true) == 2 &&
               radar_mode_of(4, 3, true) == 3 && radar_mode_of(4, 3, false) == 1 && radar_mode_of(5, 0, false) == 0,
           "jet state: radar block to detect-interface mode (off, search, track, lock; ground map off)");
    AvatarStateMsg p = a;
    p.seq = 8;
    expect(!avatar_state_differs(a, p), "jet state: only the visible fields count as a change");
    p.config = 0x0100;
    expect(avatar_state_differs(a, p), "jet state: a configuration change is a change");
    AvatarStateMsg none{};
    expect(avatar_lock_started(none, a) && !avatar_lock_started(a, a), "jet state: the RWR hears a new lock once");
    AvatarStateMsg other = a;
    other.target_b = 2;
    expect(avatar_lock_started(a, other), "jet state: a lock on another target warns that target");
    std::printf("    estimate: a state change is 20 bytes; a few per minute (locks, gear, flaps)\n");
    AvatarStateCache cache;
    AvatarStateMsg host{}, c1{}, c2{};
    host.peer = 0;
    host.config = 0x0800;  // gear down
    c1.peer = 1;
    c1.radar_mode = 3;
    c2.peer = 2;
    cache.set(host);
    cache.set(c1);
    cache.set(c2);
    AvatarStateMsg got[5];
    const size_t n2 = cache.for_joiner(2, got);
    expect(n2 == 2 && got[0].peer == 0 && got[0].config == 0x0800 && got[1].peer == 1 && got[1].radar_mode == 3,
           "jet state: a joiner gets the host's and the other clients' current states, not its own");
    cache.forget(2);
    cache.forget(1);
    const size_t n3 = cache.for_joiner(2, got);
    expect(n3 == 1 && got[0].peer == 0, "jet state: a reconnecting client gets the current lock and gear of who is left");
}

void world_seat_rule_tests() {
    std::printf("world seats and queues\n");
    {
        SeatBook book;
        book.reset(SeatPlan{3, 0});  // two seats to give: #2 and #3
        const int a = book.claim(0xA), b = book.claim(0xB);
        expect(a == 1 && b == 2 && book.claim(0xC) == -1, "seat book: two seats, the third client gets none");
        book.release(0xA);
        expect(!book.active(0xA) && book.claim(0xA) == 1 && book.active(0xA),
               "seat book: a released seat comes back to the same tag");
        book.release(0xA);
        expect(book.claim(0xC) == 1 && book.claim(0xA) == -1,
               "seat book: with no free seat left, a new client takes the released one; its old owner then gets none");
        book.release(0xC);
        book.release(0xB);
        expect(book.claim(0xA) == 1 || book.claim(0xA) == 2, "seat book: a seat is free again after the others leave");
        expect(book.claim(0) == -1, "seat book: tag 0 never gets a seat");
    }
    {
        const SeatPlan host1{4, 0}, host2{4, 1}, pair{2, 0};
        int taken[3] = {};
        const int a1 = assign_seat(host1, taken, 0);
        taken[0] = a1;
        const int a2 = assign_seat(host1, taken, 1);
        expect(a1 == 1 && a2 == 2, "seats: the host flies #1: clients get #2, then #3");
        const int b1 = assign_seat(host2, taken, 0);
        taken[0] = b1;
        const int b2 = assign_seat(host2, taken, 1);
        expect(b1 == 0 && b2 == 2, "seats: the host flies #2: clients get #1, then #3");
        taken[0] = 1;
        expect(assign_seat(pair, taken, 1) == -1 && assign_seat(SeatPlan{}, taken, 0) == -1,
               "seats: none free in a two-ship with one client, or without a plan");
    }
    {
        HostCore core;  // not started: the seat table and the queues work without the thread
        core.set_mission(std::vector<uint8_t>(10, 1), 77, 10, SeatPlan{4, 1});
        const int s1 = core.seat_for(0xAAAA), s2 = core.seat_for(0xBBBB);
        expect(s1 == 0 && s2 == 2 && core.seat_for(0xAAAA) == 0 && core.seat_for(0) == -1 && core.mission_id() == 77,
               "seats: two clients by tag, the same tag keeps its seat, tag 0 gets none");
        core.set_mission(std::vector<uint8_t>(10, 2), 78, 10, SeatPlan{4, 0});
        expect(core.seat_for(0xBBBB) == 1 && core.seat_plan().host_member == 0,
               "seats: a new mission gives the seats out again");
        uint8_t m[100] = {};
        size_t states = 0, events = 0;
        while (core.send_world(0, 0, m, sizeof(m), false)) ++states;
        while (core.send_world(0, 0, m, sizeof(m), true)) ++events;
        expect(states == HostCore::kWorldStatesCap && events == HostCore::kWorldEventsCap,
               "queues: a full states queue leaves the events' queue free (64 states, then 256 events)");
    }
    {  // the client's events ring: never dropped; full ends the session (QueueFull)
        FakeHost h;
        if (!h.start()) return expect(false, "queues loopback: host core");
        auto c = std::make_unique<ClientCore>();
        c->start("127.0.0.1", h.core.port(), cc_hello(0), cc_client_log);
        CcRx rx;
        const bool welcomed = cc_run(&h, nullptr, *c, rx, 5.0, [&] { return rx.has(ClientEvent::Welcome); });
        WorldEventRec ev{kEventAwake, 1, 1, 0, 0};
        uint8_t b[64];
        const size_t n = encode_world_events(1, 0, &ev, 1, b, sizeof(b));
        size_t sent = 0;
        const bool ended = cc_run(&h, nullptr, *c, rx, 8.0, [&] {
            for (int k = 0; k < 32 && sent < 400; ++k)
                if (h.core.send_world(h.slot, h.gen, b, n, true)) ++sent;
            return rx.has(ClientEvent::Disconnected);
        });
        expect(welcomed && ended && rx.ev.back().reason == static_cast<uint8_t>(ClientEnd::QueueFull) &&
                   c->counters().world == ClientCore::kWorldEventsIn,
               "queues: a client that does not drain 256 world events ends with QueueFull, none silently dropped");
        c->stop(false);
        h.core.stop(false);
    }
}

void world_mission_tests() {
    std::printf("world mission transfer\n");
    // Sizes are part of the wire format (v3); a chunk stays under ENet's 1,024-byte packet limit and one datagram.
    expect(sizeof(MissionRequestMsg) == 12 && sizeof(MissionOfferMsg) == 23 && sizeof(MissionChunkHead) == 12 &&
               kMissionChunkMax == 972 && kMissionChunkMax < 1200,
           "mission: message sizes (12, 23, 12 + 960)");
    uint8_t b[kMissionChunkMax + 8];
    MissionRequestMsg rq{};
    rq.have_id = 7;
    MissionRequestMsg rq2{};
    expect(encode(rq, b, sizeof(b)) == 12 && peek_world_type(b, 12) == 20 && decode(b, 12, rq2) == Status::Ok &&
               rq2.have_id == 7 && decode(b, 11, rq2) == Status::Truncated,
           "mission: request round trip");
    MsgType mt{};
    expect(peek_type(b, 12, mt) == Status::BadType, "mission: the v2-style peek_type does not take world types");

    std::vector<uint8_t> msn(70000);
    for (size_t i = 0; i < msn.size(); ++i) msn[i] = static_cast<uint8_t>((i % 2) ? 0 : "[AircraftMission001]\n"[i / 2 % 21]);
    const std::string sidecar = "version=1\nseed=12345\ndensity=3\n";
    std::vector<uint8_t> packed;
    uint32_t id = 0, raw = 0;
    expect(pack_mission(sidecar, msn, &packed, &id, &raw) && id != 0 && raw == 16 + sidecar.size() + msn.size() &&
               packed.size() < msn.size() / 4,
           "mission: pack compresses a UTF-16 ini");
    std::string side2;
    std::vector<uint8_t> msn2;
    const char* why = "";
    expect(unpack_mission(packed, id, raw, &side2, &msn2, &why) && side2 == sidecar && msn2 == msn,
           "mission: unpack gives the same sidecar and file");
    std::vector<uint8_t> bad = packed;
    bad[bad.size() / 2] ^= 0x5a;
    expect(!unpack_mission(bad, id, raw, &side2, &msn2, &why), "mission: a corrupted package is refused (CRC)");
    expect(!unpack_mission(packed, id, raw + 1, &side2, &msn2, &why), "mission: a wrong unpacked size is refused");

    MissionOfferMsg o{};
    o.status = kOfferOk;
    o.mission_id = id;
    o.packed_bytes = static_cast<uint32_t>(packed.size());
    o.raw_bytes = raw;
    o.chunk_bytes = kMissionChunkData;
    o.chunk_count = static_cast<uint16_t>(mission_chunk_count(packed.size()));
    MissionOfferMsg o2{};
    expect(encode(o, b, sizeof(b)) == 23 && decode(b, 23, o2) == Status::Ok && o2.chunk_count == o.chunk_count,
           "mission: offer round trip");
    MissionAssembly as;
    expect(as.begin(o) && as.count() == o.chunk_count, "mission: assembly accepts the offer");
    MissionOfferMsg wrong = o;
    wrong.chunk_count++;
    MissionAssembly as2;
    expect(!as2.begin(wrong), "mission: an offer with an inconsistent chunk count is refused");
    // Last chunk first, then the rest in order, one duplicate.
    bool order_ok = true;
    for (int k = 0; k <= o.chunk_count; ++k) {
        const uint16_t i = static_cast<uint16_t>(k == 0 ? o.chunk_count - 1 : k - 1);
        const size_t n = encode_mission_chunk(id, i, packed.data(), packed.size(), b, sizeof(b));
        MissionChunkHead ch{};
        const uint8_t* pl = nullptr;
        order_ok = order_ok && n > 12 && n <= kMissionChunkMax && decode_mission_chunk(b, n, ch, &pl) == Status::Ok;
        const MissionAssembly::Add r = as.add(ch, pl);
        if (k == o.chunk_count) order_ok = order_ok && r == MissionAssembly::Add::Duplicate;
        else if (k == o.chunk_count - 1) order_ok = order_ok && r == MissionAssembly::Add::Done;
        else order_ok = order_ok && r == MissionAssembly::Add::Ok;
    }
    expect(order_ok && as.complete() && as.bytes() == packed, "mission: chunks reassemble out of order, duplicates ignored");
    expect(encode_mission_chunk(id, o.chunk_count, packed.data(), packed.size(), b, sizeof(b)) == 0,
           "mission: no chunk past the end");
    {
        const size_t n = encode_mission_chunk(id, 0, packed.data(), packed.size(), b, sizeof(b));
        MissionChunkHead ch{};
        const uint8_t* pl = nullptr;
        expect(decode_mission_chunk(b, n - 1, ch, &pl) == Status::Truncated &&
                   decode_mission_chunk(b, n + 1, ch, &pl) == Status::Oversized,
               "mission: chunk length must match its head");
        ch.mission_id = id + 1;
        MissionAssembly as3;
        as3.begin(o);
        decode_mission_chunk(b, n, ch, &pl);
        ch.mission_id = id + 1;
        expect(as3.add(ch, pl) == MissionAssembly::Add::Bad, "mission: a chunk of another mission is refused");
    }

    // Loopback: the host core answers a fetch with "none", then sends the mission once it is set.
    if (enet_initialize() != 0) {
        expect(false, "mission loopback: enet_initialize");
        return;
    }
    {
        HostCore core;
        static std::atomic<int> none_seen{0};
        none_seen = 0;
        core.start("127.0.0.1", 0, [](const char* line) {
            if (std::strstr(line, "none yet")) none_seen.fetch_add(1);
        });
        for (int i = 0; i < 2000 && core.state() != HostCore::State::Running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto wait_take = [](MissionFetch& f, std::vector<uint8_t>* g, MissionOfferMsg* o, int ms) {
            for (int i = 0; i < ms / 10; ++i) {
                if (f.take(g, o)) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };
        auto fetch = std::make_unique<MissionFetch>();
        const bool started = fetch->start("127.0.0.1", core.port(), 0x1234u, nullptr, 200);
        for (int i = 0; i < 300 && none_seen.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        expect(started && none_seen.load() >= 1 && fetch->state() == MissionFetch::State::Running,
               "mission loopback: the host says none yet, the fetch keeps asking");
        core.set_mission(packed, id, raw, SeatPlan{4, 1});  // the host flies #2
        std::vector<uint8_t> got;
        MissionOfferMsg go{};
        expect(wait_take(*fetch, &got, &go, 10000) && got == packed && go.mission_id == id && go.seat == 0 &&
                   go.host_seat == 1 && !fetch->take(&got, &go),
               "mission loopback: the fetch receives the mission once; the host (#2) gives it seat #1");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));  // a few polls: the host answers Same
        expect(core.missions_sent() == 1 && fetch->latest_id() == id && !fetch->take(&got, &go),
               "mission loopback: later polls of the same mission send nothing (Same)");
        // The host changes its mission in the menus: the poll fetches it before the next Accept.
        std::vector<uint8_t> msn_b = msn;
        msn_b[10] ^= 1;
        std::vector<uint8_t> packed_b;
        uint32_t id_b = 0, raw_b = 0;
        pack_mission(sidecar, msn_b, &packed_b, &id_b, &raw_b);
        core.set_mission(packed_b, id_b, raw_b, SeatPlan{4, 1});
        expect(wait_take(*fetch, &got, &go, 10000) && go.mission_id == id_b && got == packed_b &&
                   fetch->latest_id() == id_b && core.missions_sent() == 2,
               "mission loopback: a new host mission is fetched by the next poll");
        // Seats: released when the client's connections close, given back to the same tag while still free.
        const uint8_t seat_a = go.seat;
        fetch->stop();
        fetch.reset();
        for (int i = 0; i < 200 && core.seat_active(0x1234u); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        expect(!core.seat_active(0x1234u), "seats: a closed fetch connection releases its seat");
        auto again = std::make_unique<MissionFetch>();
        again->start("127.0.0.1", core.port(), 0x1234u, nullptr, 200);
        const bool back = wait_take(*again, &got, &go, 10000);
        expect(back && go.seat == seat_a, "seats: a restarted client with the same tag gets its old seat back");
        again->stop();
        core.stop(false);
    }
    enet_deinitialize();
}

void world_seat_tests() {
    using namespace LazyWorks::SF2Patch::hooks;
    std::printf("world seat\n");
    expect(decide_seat(1, 2).apply && decide_seat(1, 2).member == 1, "world seat: 1-2 in a two-ship");
    expect(decide_seat(3, 4).apply && decide_seat(3, 4).member == 3, "world seat: 1-4 in a four-ship");
    expect(!decide_seat(1, 1).apply && !decide_seat(2, 2).apply, "world seat: too few members is stock");
    expect(!decide_seat(0, 4).apply && !decide_seat(-1, 4).apply, "world seat: 0 or none is stock");
    expect(!decide_seat(1, 0).apply && !decide_seat(1, 49).apply, "world seat: implausible member count is stock");
    expect(parse_seat(L"1") == 1 && parse_seat(L"47") == 47, "world seat: parse 1 and 47");
    expect(waypoint_policy() == WaypointPolicy::FollowHost && apply_host_waypoint(WaypointPolicy::FollowHost, true) &&
               apply_host_waypoint(WaypointPolicy::OwnFlightChooses, false) &&
               !apply_host_waypoint(WaypointPolicy::OwnFlightChooses, true),
           "waypoints: follow the host for now; the later policy leaves only our own flight to us");
    expect(parse_seat(L"0") == -1 && parse_seat(L"48") == -1 && parse_seat(L"") == -1 && parse_seat(L"x") == -1 &&
               parse_seat(L"-1") == -1 && parse_seat(L"1 ") == -1 && parse_seat(nullptr) == -1,
           "world seat: parse refuses 0, 48, empty, junk");
}

void world_remote_tests() {
    using namespace LazyWorks::SF2Patch::hooks;
    std::printf("world remote\n");
    expect(should_mark_remote(0x01000003, 0x01000001, true), "world remote: an AI object is marked");
    expect(!should_mark_remote(0x01000001, 0x01000001, true), "world remote: the player is not marked");
    expect(!should_mark_remote(0x01000003, 0x01000001, false) && !should_mark_remote(0xffffffffu, 1, true),
           "world remote: unresolved or -1 ids are not marked");
    expect(block_damage(true, 0x40 | 0x3, 0x40), "world remote: damage to a REMOTEOBJECT object is blocked");
    expect(!block_damage(false, 0x40, 0x40), "world remote: nothing is blocked while the remote world is off");
    expect(!block_damage(true, 0x3, 0x40) && !block_damage(true, 0xffffffffu, 0),
           "world remote: a local object, or an unknown mask, is not blocked");
    expect(log_milestone(1) && log_milestone(10) && log_milestone(1000) && !log_milestone(2) && !log_milestone(11) &&
               !log_milestone(20) && !log_milestone(0),
           "world remote: log at 1, 10, 100, ...");
    RemoteCounts c;
    c.count(RemoteKind::Aircraft);
    c.count(RemoteKind::Aircraft);
    c.count(RemoteKind::Ground);
    c.count(RemoteKind::Placed);
    c.player = 1;
    expect(format_remote_counts(c) == "marked 2 aircraft, 1 ground-group members, 1 placed objects remote; player 1, "
                                      "already 0, unresolved 0, faulted 0",
           "world remote: counts line");
}

void world_files_tests() {
    using namespace LazyWorks::SF2Patch::hooks;
    std::printf("world files\n");
    ManifestEntry a;
    a.kind = ManifestEntry::Aircraft, a.a = 3, a.b = 1, a.type = "F-4E", a.side = 'F';
    a.pos[0] = 1234.5, a.pos[1] = -0.4, a.pos[2] = -1234.5;
    a.heading = -0.00001f, a.has_pose = true, a.has_id = true, a.id = 0x01000005;
    expect(format_manifest_line(a) == "A 003.01 type=F-4E side=F pos=1235,0,-1235 hdg=0.0 id=0x01000005",
           "world manifest: aircraft line, rounding, heading wrap");
    a.heading = 3.14159265f / 2;
    a.type.clear();
    expect(format_manifest_line(a) == "A 003.01 type=? side=F pos=1235,0,-1235 hdg=90.0 id=0x01000005",
           "world manifest: empty type, 90 degrees");
    expect(heading_tenths(6.2831f) == 0 && heading_tenths(-3.14159265f / 2) == 2700 &&
               heading_tenths(std::numeric_limits<float>::quiet_NaN()) == 0,
           "world manifest: heading wraps to 0..359.9");
    ManifestEntry g;
    g.kind = ManifestEntry::Ground, g.a = 40, g.b = -1, g.state = ManifestEntry::Absent;
    expect(format_manifest_line(g) == "G 040.-- absent", "world manifest: absent slot");
    ManifestEntry p;
    p.kind = ManifestEntry::Placed, p.a = 12, p.b = 7, p.state = ManifestEntry::Model;
    p.pos[0] = 10, p.pos[1] = 20, p.has_pose = true;
    expect(format_manifest_line(p) == "P 0012.0007 model pos=10,20,0", "world manifest: placed model, no heading");
    ManifestEntry u;
    u.kind = ManifestEntry::Aircraft, u.a = 0, u.b = 2, u.state = ManifestEntry::Unresolved, u.has_id = true;
    u.id = 0xffffffffu;
    expect(format_manifest_line(u) == "A 000.02 unresolved id=0xffffffff", "world manifest: unresolved member");
    ManifestEntry bad = a;
    bad.pos[1] = std::numeric_limits<double>::infinity();
    expect(format_manifest_line(bad).find("pos=bad") != std::string::npos, "world manifest: non-finite position");
    std::vector<ManifestEntry> v{p, g, a, u};
    ManifestEntry a0 = a;
    a0.b = 0;
    v.push_back(a0);
    const std::string text = format_manifest({"# head", "H seed=1"}, v);
    expect(v[0].kind == 'A' && v[0].a == 0 && v[1].a == 3 && v[1].b == 0 && v[2].b == 1 && v[3].kind == 'G' &&
               v[4].kind == 'P',
           "world manifest: sorted by kind, slot, member");
    expect(text.rfind("# head\nH seed=1\nA 000.02", 0) == 0 &&
               text.find("C aircraft=2 aircraft_absent_slots=0 aircraft_unresolved=1 ground=0 ground_absent_slots=1 "
                         "ground_unresolved=0 placed_objects=0 placed_models=1 placed_none=0 placed_unresolved=0\n") !=
                   std::string::npos,
           "world manifest: header first, counts last");

    WorldSidecar s;
    s.msn = "world_007.msn", s.seed = 4000000000u, s.has_seed = true, s.density = 3, s.mission_start = 0;
    s.theatre = "GermanyCE", s.aircraft = "F-100D", s.mission_type = 1;
    s.year = 1965, s.month = 7, s.day = 4, s.hour = 8, s.minute = 30, s.second = 5, s.weather = 2;
    WorldSidecar r;
    std::string err;
    const bool ok = parse_sidecar(format_sidecar(s), &r, &err);
    expect(ok && r.msn == s.msn && r.seed == s.seed && r.density == 3 && r.mission_start == 0 &&
               r.theatre == "GermanyCE" && r.aircraft == "F-100D" && r.mission_type == 1 && r.year == 1965 &&
               r.month == 7 && r.day == 4 && r.hour == 8 && r.minute == 30 && r.second == 5 && r.weather == 2,
           "world sidecar: round trip");
    expect(parse_sidecar("version=1\r\nseed=5\r\n\r\n# c\r\nfuture=x\r\n", &r, &err) && r.seed == 5 &&
               r.density == -1,
           "world sidecar: CRLF, comments, unknown keys, defaults");
    expect(!parse_sidecar("version=1\n", &r, &err) && err == "no seed", "world sidecar: seed required");
    expect(!parse_sidecar("seed=1\n", &r, &err), "world sidecar: version required");
    expect(!parse_sidecar("version=2\nseed=1\n", &r, &err), "world sidecar: other version refused");
    expect(!parse_sidecar("version=1\nseed=-1\n", &r, &err) && !parse_sidecar("version=1\nseed=x\n", &r, &err) &&
               !parse_sidecar("version=1\nseed=4294967296\n", &r, &err),
           "world sidecar: bad seeds refused");
    expect(!parse_sidecar("version=1\nseed=1\ndensity=4\n", &r, &err) &&
               !parse_sidecar("version=1\nseed=1\ndate=1965-07\n", &r, &err) &&
               !parse_sidecar("version=1\nseed=1\njunk\n", &r, &err),
           "world sidecar: bad values refused");

    {
        ManifestEntry pe;
        expect(parse_manifest_line("A 003.01 type=F-4E side=F pos=1235,0,-1235 hdg=90.0 id=0x01000005", &pe) &&
                   pe.kind == 'A' && pe.a == 3 && pe.b == 1 && pe.type == "F-4E" && pe.side == 'F' && pe.has_pose &&
                   pe.pos[0] == 1235 && pe.pos[2] == -1235 && std::fabs(pe.heading - 1.5707963f) < 1e-4f &&
                   pe.has_id && pe.id == 0x01000005 && pe.state == ManifestEntry::Object,
               "world manifest: parse an object line back");
        expect(parse_manifest_line("G 040.-- absent", &pe) && pe.b == -1 && pe.state == ManifestEntry::Absent &&
                   parse_manifest_line("P 0012.0007 model pos=10,20,0", &pe) && pe.state == ManifestEntry::Model &&
                   !parse_manifest_line("H seed=1", &pe) && !parse_manifest_line("C aircraft=2", &pe) &&
                   !parse_manifest_line("# x", &pe),
               "world manifest: parse absent and model lines; header and counts are not entries");
    }
    expect(world_file_number(L"world_012.msn") == 12 && world_file_number(L"WORLD_3.manifest.txt") == 3 &&
               world_file_number(L"world_.msn") == -1 && world_file_number(L"world_12") == -1 &&
               world_file_number(L"snap_012.msn") == -1,
           "world files: numbering");
    expect(world_companion_path(L"C:\\p\\world_001.MSN", L".sidecar.txt") == L"C:\\p\\world_001.sidecar.txt" &&
               world_companion_path(L"C:\\p\\x", L".sidecar.txt") == L"C:\\p\\x.sidecar.txt",
           "world files: companion path");
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

namespace LazyWorks::SF2Patch::hooks {
std::string jet_census(const void* object, uintptr_t ao);
}

void jet_census_tests() {
    std::printf("jet census\n");
    namespace jet = LazyWorks::SF2Patch::hooks::jet;
    std::vector<uint8_t> image(jet::kAoSize), obj(0x9a8), eng(0x90), gun(0x6c), comp(0xe0);
    const uintptr_t ao = reinterpret_cast<uintptr_t>(image.data());
    auto put32 = [](std::vector<uint8_t>& b, size_t off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); };
    auto put16 = [](std::vector<uint8_t>& b, size_t off, int16_t v) { std::memcpy(b.data() + off, &v, 2); };
    auto putf = [](std::vector<uint8_t>& b, size_t off, float v) { std::memcpy(b.data() + off, &v, 4); };
    auto addr = [](const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); };
    put32(image, 0x93494 + 4 * jet::kEngSetRunningSlot, static_cast<uint32_t>(ao + jet::kRvaEngSetRunning));
    put32(eng, 0, static_cast<uint32_t>(ao + 0x93494));
    put16(eng, jet::kEngRunning, 1);
    putf(eng, jet::kEngThrottle, 0.75f);
    put16(eng, jet::kEngAb, 2);
    put32(gun, 0, static_cast<uint32_t>(ao + 0x94bec));
    put16(gun, jet::kGunRounds, 640);
    float inputs[16] = {};
    inputs[3] = 0.75f;
    uint32_t systems[2] = {addr(eng.data()), addr(gun.data())};
    put32(comp, 0, static_cast<uint32_t>(ao + 0x8c374));
    put32(comp, jet::kCompSystems + jet::kListCount, 2);
    put32(comp, jet::kCompSystems + jet::kListArray, addr(systems));
    uint32_t comps[1] = {addr(comp.data())};
    put32(obj, jet::kInputs, addr(inputs));
    put16(obj, jet::kEngineCount, 1);
    put32(obj, jet::kEngines, systems[0]);
    put32(obj, jet::kComponents + jet::kListCount, 1);
    put32(obj, jet::kComponents + jet::kListArray, addr(comps));
    for (size_t i = 0; i < jet::kLoadoutStations; ++i) put32(obj, jet::kLoadout + jet::kLoadoutEntry * i, 0xffffffffu);
    put32(obj, jet::kLoadout + jet::kLoadoutEntry * 2, 17);
    put16(obj, jet::kLoadout + jet::kLoadoutEntry * 2 + 8, 2);
    put16(obj, jet::kLoadout + jet::kLoadoutEntry * 2 + 10, 1);
    putf(obj, jet::kFuelTotal, 3200.0f);
    put16(obj, jet::kFlareTotal, 30);
    const std::string c = LazyWorks::SF2Patch::hooks::jet_census(obj.data(), ao);
    auto has = [&](const char* t) { return c.find(t) != std::string::npos; };
    expect(has("[3] 0.750") && has("[0] vt +0x93494 setter ok running 1 throttle 0.750 ab 2"),
           "census: the input array and the engine (running, throttle, AB stage, slot 22 checked)");
    expect(has("loadout: [2] id 17 x2 state 1\n") && has("fuel 3200.0 kg") && has("flares 30"),
           "census: loaded stations only, fuel and countermeasure totals");
    expect(has("comp 0") && has("systems 2") && has("(jet engine)") && has("(fixed gun)") && has("rounds 640"),
           "census: components and their systems by class");
    put32(obj, jet::kComponents + jet::kListArray, 0x20);  // a bad pointer: skipped, no fault
    const std::string bad = LazyWorks::SF2Patch::hooks::jet_census(obj.data(), ao);
    expect(bad.find("components 1:") != std::string::npos && bad.find("comp 0") == std::string::npos,
           "census: an unreadable pointer is skipped");
}

void jet_controls_tests() {
    std::printf("jet controls (Phase 5 T2: engine and afterburner)\n");
    expect(kVersion == 4 && sizeof(AvatarStateMsg) == 20, "protocol v4: the jet state grew to 20 bytes");
    expect(throttle_to_wire(0.0f) == 0 && throttle_to_wire(1.0f) == 255 && throttle_to_wire(0.7f) == 179 &&
               throttle_to_wire(2.0f) == 255 && throttle_to_wire(-1.0f) == 0 &&
               throttle_to_wire(std::numeric_limits<float>::quiet_NaN()) == 0 && throttle_from_wire(255) == 1.0f &&
               std::fabs(throttle_from_wire(179) - 0.7f) < 0.003f,
           "throttle: 0..1 as 0..255, clamped, NaN as idle, back within half a step");
    expect(!throttle_moved(100, 101) && throttle_moved(100, 102) && throttle_moved(1, 0) && throttle_moved(254, 255) &&
               !throttle_moved(7, 7),
           "throttle: a one-step wobble is no change, except onto idle or full");
    AvatarStateMsg last{}, now{};
    last.throttle = 100;
    last.engines = 1;
    now = last;
    now.throttle = 150;
    expect(!avatar_state_due(last, now, 0.1) && avatar_state_due(last, now, 0.25),
           "send rule: a throttle move waits for 0.2 s after the last send (5 a second at most)");
    now.config = 0x0800;
    expect(avatar_state_due(last, now, 0.05), "send rule: any other change (gear here) goes at once, with the throttle");
    now = last;
    now.engines = 0;
    expect(avatar_state_due(last, now, 0.0), "send rule: an engine switched off goes at once");
    now = last;
    now.throttle = 101;
    expect(!avatar_state_due(last, now, 10.0) && !avatar_state_due(last, last, 10.0), "send rule: nothing without a change");
    float thr = -1;
    uint8_t eng = 0;
    bot::jet_script_at(bot::JetScript::Ab, 1.0, &thr, &eng);
    const bool idle = thr == 0.0f && eng == 0xff;
    bot::jet_script_at(bot::JetScript::Ab, 5.0, &thr, &eng);
    const bool mil = thr == 0.7f && eng == 0xff;
    bot::jet_script_at(bot::JetScript::Ab, 9.0, &thr, &eng);
    const bool ab = thr == 1.0f && eng == 0xff;
    bot::jet_script_at(bot::JetScript::Ab, 13.0, &thr, &eng);
    const bool off = eng == 0;
    bot::jet_script_at(bot::JetScript::Ab, 15.0, &thr, &eng);
    expect(idle && mil && ab && off && thr == 0.0f && eng == 0xff, "bot --jet ab: idle, Mil, full AB, engines off, again");
    std::printf("    estimate: 20 B a state, about 64 B on the wire with UDP, ENet and its ack; a moving throttle sends at\n"
                "    most 5 a second (320 B/s per player), a steady one nothing; engines and other changes once each\n");

    ServeRun s;
    s.cfg.script = bot::Script::Straight;
    s.cfg.has_start = true;
    s.cfg.start[2] = 3000.0;
    s.cfg.jet = bot::JetScript::Ab;
    if (!s.start()) return expect(false, "bot serve listening (--jet ab)");
    ServeClient c;
    expect(c.core.start("127.0.0.1", s.port, cc_hello(0), cc_client_log), "client start");
    std::vector<AvatarStateMsg> got;
    bool sent_own = false;
    WorldPacket wp[8];
    serve_run_for(c, 6.0, [&] {
        const size_t n = c.core.poll_world(wp, 8);
        for (size_t i = 0; i < n; ++i) {
            AvatarStateMsg m{};
            if (decode(wp[i].data, wp[i].len, m) == Status::Ok) got.push_back(m);
        }
        if (!sent_own && !c.welcomes.empty()) {
            AvatarStateMsg own{};
            own.throttle = 200;
            own.engines = 1;
            uint8_t b[sizeof(own)];
            sent_own = c.core.send_world(b, encode(own, b, sizeof(b)));
        }
        return false;
    });
    s.finish();
    c.core.stop(false);
    bool first_idle = !got.empty() && got[0].peer == 0 && got[0].throttle == 0 && got[0].engines == 0xff;
    bool saw_mil = false;
    for (const AvatarStateMsg& m : got) saw_mil = saw_mil || (m.throttle == 179 && m.engines == 0xff);
    char what[200];
    std::snprintf(what, sizeof(what), "loopback: the host's jet controls reach the client (%zu states: idle first, then Mil)",
                  got.size());
    expect(first_idle && saw_mil && got.size() <= 4, what);
    expect(sent_own && s.res.jet_states_rx == 1 && s.res.last_rx_throttle == 200 && s.res.last_rx_engines == 1,
           "loopback: the client's jet controls reach the host");
}

void jet_mirror_tests() {
    std::printf("jet controls on a fake aircraft (slot 22 checked and called)\n");
    namespace jet = LazyWorks::SF2Patch::hooks::jet;
    using LazyWorks::SF2Patch::hooks::JetControls;
    using LazyWorks::SF2Patch::hooks::JetWriteResult;
    // A fake AircraftObject image, executable: the engine vtable's slot 22 points at a copy of the game's setter
    // (mov ax,[esp+4]; mov [ecx+0x2c],ax; ret 4).
    uint8_t* image = static_cast<uint8_t*>(VirtualAlloc(nullptr, jet::kAoSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!image) return expect(false, "fake image");
    const uintptr_t ao = reinterpret_cast<uintptr_t>(image);
    const uint8_t setter[] = {0x66, 0x8b, 0x44, 0x24, 0x04, 0x66, 0x89, 0x41, 0x2c, 0xc2, 0x04, 0x00};
    std::memcpy(image + jet::kRvaEngSetRunning, setter, sizeof(setter));
    const uint32_t fn = static_cast<uint32_t>(ao + jet::kRvaEngSetRunning);
    std::memcpy(image + 0x93494 + 4 * jet::kEngSetRunningSlot, &fn, 4);
    FlushInstructionCache(GetCurrentProcess(), image, jet::kAoSize);
    uint32_t other_vt[32] = {};  // a class outside the image: refused
    struct Jet {
        std::vector<uint8_t> obj = std::vector<uint8_t>(0x9a8);
        std::vector<uint8_t> eng[4];
        float in[16] = {};
        void make(uintptr_t vt, int n) {
            const uint32_t ip = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(in));
            std::memcpy(obj.data() + jet::kInputs, &ip, 4);
            const int16_t c = static_cast<int16_t>(n);
            std::memcpy(obj.data() + jet::kEngineCount, &c, 2);
            for (int i = 0; i < n; ++i) {
                eng[i].assign(0x90, 0);
                const uint32_t v = static_cast<uint32_t>(vt);
                std::memcpy(eng[i].data(), &v, 4);
                const uint32_t ep = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(eng[i].data()));
                std::memcpy(obj.data() + jet::kEngines + 4 * i, &ep, 4);
            }
        }
        int16_t running(int i) const {
            int16_t r = 0;
            std::memcpy(&r, eng[i].data() + jet::kEngRunning, 2);
            return r;
        }
        void set_running(int i, int16_t r) { std::memcpy(eng[i].data() + jet::kEngRunning, &r, 2); }
    };
    Jet owner, av;
    owner.make(ao + 0x93494, 2);
    owner.set_running(0, 1);
    owner.in[3] = 0.9f;
    JetControls c;
    expect(LazyWorks::SF2Patch::hooks::jet_read_controls_at(owner.obj.data(), ao, &c) && c.engine_count == 2 &&
               c.engines == 0x01 && c.throttle == 0.9f,
           "read: throttle, two engines, the first running");
    av.make(ao + 0x93494, 3);
    for (int i = 0; i < 3; ++i) av.set_running(i, 1);
    av.in[12] = av.in[13] = av.in[14] = 1.0f;
    JetControls taken;
    expect(LazyWorks::SF2Patch::hooks::jet_takeover_at(av.obj.data(), ao, &taken) && taken.engines == 0x07 &&
               taken.engine_count == 3 && av.in[12] == 0.0f && av.in[13] == 0.0f && av.in[14] == 0.0f,
           "takeover: the AI's triggers cleared, its three running engines kept for the release");
    JetWriteResult w = LazyWorks::SF2Patch::hooks::jet_write_controls_at(av.obj.data(), ao, c, true);
    expect(w.ok && w.engines_set == 0x02 && av.running(0) == 1 && av.running(1) == 0 && av.running(2) == 1 &&
               av.in[3] == 0.9f && av.in[7] == 0.9f && av.in[12] == 0.0f && av.in[13] == 0.0f,
           "write: throttle into inputs 3..7; the owner's second engine off through slot 22; a third engine left as taken");
    owner.in[12] = 1.0f;
    JetControls g;
    const bool read_guns = LazyWorks::SF2Patch::hooks::jet_read_controls_at(owner.obj.data(), ao, &g) && g.guns == 1;
    LazyWorks::SF2Patch::hooks::jet_write_controls_at(av.obj.data(), ao, g, true);
    const bool fires = av.in[12] == 1.0f && av.in[13] == 0.0f;
    g.guns = 0;
    LazyWorks::SF2Patch::hooks::jet_write_controls_at(av.obj.data(), ao, g, true);
    expect(read_guns && fires && av.in[12] == 0.0f, "guns: the owner's trigger read, on and off on the avatar's input 12");
    int32_t failed = 1;
    std::memcpy(av.eng[1].data() + jet::kEngFire, &failed, 4);
    c.engines = 0x03;
    w = LazyWorks::SF2Patch::hooks::jet_write_controls_at(av.obj.data(), ao, c, true);
    expect(w.ok && w.engines_set == 0 && av.running(1) == 0, "write: an engine on fire or failed is not restarted");
    failed = 0;
    std::memcpy(av.eng[1].data() + jet::kEngFire, &failed, 4);
    c.engines = 0;
    LazyWorks::SF2Patch::hooks::jet_write_controls_at(av.obj.data(), ao, c, true);
    JetControls back;
    back.engines = taken.engines;
    back.engine_count = taken.engine_count;
    w = LazyWorks::SF2Patch::hooks::jet_write_controls_at(av.obj.data(), ao, back, false);
    expect(w.engines_set == 0x03 && av.running(0) == 1 && av.running(1) == 1 && av.running(2) == 1 && av.in[3] == 0.9f,
           "release: the engines as taken again, the throttle untouched");
    Jet odd;
    odd.make(reinterpret_cast<uintptr_t>(other_vt), 1);
    c.engines = 0x01;
    c.engine_count = 1;
    w = LazyWorks::SF2Patch::hooks::jet_write_controls_at(odd.obj.data(), ao, c, true);
    expect(w.ok && w.engines_refused == 0x01 && w.engines_set == 0 && odd.running(0) == 0,
           "write: an engine of a class we did not read is refused, not called");
    JetControls oc;
    expect(LazyWorks::SF2Patch::hooks::jet_read_controls_at(odd.obj.data(), ao, &oc) && oc.engines == 0x01,
           "read: such an engine is reported running, so no avatar switches it off");
    VirtualFree(image, 0, MEM_RELEASE);
}

void hit_claim_tests() {
    std::printf("gun hits across PCs (Phase 5 T3)\n");
    HitMsg h{};
    h.peer = 3;
    h.seq = 9;
    h.target_kind = 1;
    h.target_a = 4;
    h.target_b = 1;
    h.slot = 4;
    h.weapon = 12;
    h.energy = 150000;
    h.p0[1] = -300;
    h.p1[1] = 100;
    uint8_t b[64];
    HitMsg r{};
    const size_t n = encode_hit(h, WorldType::HitClaim, b, sizeof(b));
    expect(sizeof(HitMsg) == 43 && n == 43 && peek_world_type(b, n) == 27 &&
               decode_hit(b, n, WorldType::HitClaim, r) == Status::Ok && r.peer == 3 && r.weapon == 12 &&
               r.energy == 150000 && r.p0[1] == -300 && decode_hit(b, n, WorldType::HitApply, r) == Status::BadType &&
               decode_hit(b, n - 1, WorldType::HitClaim, r) == Status::Truncated,
           "hit: 43 bytes, claim and apply are told apart, size checked");
    h.slot = 7;
    const size_t n7 = encode_hit(h, WorldType::HitApply, b, sizeof(b));
    expect(n7 == 43 && decode_hit(b, n7, WorldType::HitApply, r) == Status::BadType && encode_hit(h, WorldType::AvatarState, b, sizeof(b)) == 0,
           "hit: only damage slots 4..6, only the two hit types");
    h.slot = 4;
    expect(weapon_is_gun(0xffff) && weapon_is_gun(0xffff + 40) && !weapon_is_gun(17) && !weapon_is_gun(0xfffe),
           "hit: gun rounds carry the gun id + 0xffff, stores a small id");
    expect(hit_rel_to_wire(1.234) == 123 && hit_rel_to_wire(-1.236) == -124 && hit_rel_to_wire(500.0) == 32767 &&
               hit_rel_to_wire(std::numeric_limits<double>::quiet_NaN()) == 0 && hit_rel_from_wire(-300) == -3.0,
           "hit: offsets from the target in cm, clamped to about 327 m");
    HitRate rate;
    HitLimits lim;
    expect(check_hit_claim(h, 800.0, true, rate, 10.0, lim) == HitVerdict::Ok, "check: a gun hit from 800 m is applied");
    expect(check_hit_claim(h, 800.0, false, rate, 10.0, lim) == HitVerdict::TargetGone &&
               check_hit_claim(h, 6000.0, true, rate, 10.0, lim) == HitVerdict::TooFar,
           "check: no live target, or the shooter 6 km away, is refused");
    HitMsg big = h;
    big.energy = 5000000;
    HitMsg off = h;
    off.p1[0] = 20000;  // 200 m from the target
    expect(check_hit_claim(big, 800.0, true, rate, 10.0, lim) == HitVerdict::TooMuchEnergy &&
               check_hit_claim(off, 800.0, true, rate, 10.0, lim) == HitVerdict::OffTarget,
           "check: 5 MJ, or a round 200 m from the target, is refused");
    HitRate burst;
    int ok = 0;
    for (int i = 0; i < 80; ++i) ok += check_hit_claim(h, 800.0, true, burst, 20.0, lim) == HitVerdict::Ok;
    const bool later = check_hit_claim(h, 800.0, true, burst, 20.5, lim) == HitVerdict::Ok;
    expect(ok == 60 && later, "check: at most 60 claims at once per shooter, then 60 a second");
    std::printf("    estimate: 43 B a hit, about 90 B on the wire; at most 60 a second per shooter while it hits\n"
                "    (5.4 KB/s at the cap; a 1 s burst of a 20 mm gun on a jet is typically 10..30 hits)\n");
    float thr = 0;
    uint8_t eng = 0, guns = 9;
    bot::jet_script_at(bot::JetScript::Guns, 1.0, &thr, &eng, &guns);
    const bool quiet = guns == 0 && thr == 0.7f;
    bot::jet_script_at(bot::JetScript::Guns, 3.5, &thr, &eng, &guns);
    const bool firing = guns == 1;
    bot::jet_script_at(bot::JetScript::Ab, 3.5, &thr, &eng, &guns);
    expect(quiet && firing && guns == 0, "bot --jet guns: a 1 s burst in every 4 s; ab never fires");

    ServeRun s;
    s.cfg.script = bot::Script::Straight;
    s.cfg.has_start = true;
    s.cfg.start[2] = 3000.0;
    s.cfg.jet = bot::JetScript::Guns;
    s.cfg.hit = true;
    s.cfg.hit_kind = 1;
    s.cfg.hit_a = 3;
    s.cfg.hit_b = 1;
    s.cfg.hit_every = 1.0;
    if (!s.start()) return expect(false, "bot serve listening (--jet guns --hit)");
    ServeClient c;
    expect(c.core.start("127.0.0.1", s.port, cc_hello(0), cc_client_log), "client start");
    std::vector<HitMsg> hits;
    bool saw_guns = false;
    int claims = 0;
    WorldPacket wp[8];
    serve_run_for(c, 4.5, [&] {
        const size_t k = c.core.poll_world(wp, 8);
        for (size_t i = 0; i < k; ++i) {
            HitMsg m{};
            AvatarStateMsg a{};
            if (decode_hit(wp[i].data, wp[i].len, WorldType::HitApply, m) == Status::Ok) hits.push_back(m);
            if (decode(wp[i].data, wp[i].len, a) == Status::Ok && a.guns == 1) saw_guns = true;
        }
        if (claims < 3 && !c.welcomes.empty()) {
            HitMsg cl = h;
            uint8_t cb[sizeof(cl)];
            if (c.core.send_world(cb, encode_hit(cl, WorldType::HitClaim, cb, sizeof(cb)))) ++claims;
        }
        return false;
    });
    s.finish();
    c.core.stop(false);
    expect(saw_guns, "loopback: the host's gun trigger reaches the client in its jet state");
    expect(hits.size() >= 3 && hits[0].target_a == 3 && hits[0].target_b == 1 && hits[0].shooter_kind == 1 &&
               hits[0].slot == 4 && hits[0].energy == 20000,
           "loopback: --hit sends HitApply for the client's seat, about once a second");
    expect(claims == 3 && s.res.hit_claims_rx == 3, "loopback: the client's hit claims reach the host");
}

void weapon_launch_tests() {
    std::printf("weapon launches (Phase 5 T4)\n");
    WeaponLaunchMsg w{};
    w.peer = 2;
    w.seq = 5;
    w.shooter_kind = 1;
    w.shooter_a = 3;
    w.shooter_b = 1;
    w.target_kind = 1;
    w.target_a = 7;
    w.flags = kLaunchGuided;
    w.pos[0] = -123456789;
    w.wvel[1] = 6000;
    std::memset(w.weapon, 'X', sizeof(w.weapon));  // no terminator on the wire: the decoder adds one
    w.weapon_id = 321;
    uint8_t b[96];
    WeaponLaunchMsg r{};
    const size_t n = encode(w, b, sizeof(b));
    expect(sizeof(WeaponLaunchMsg) == 80 && n == 80 && peek_world_type(b, n) == 29 && decode(b, n, r) == Status::Ok &&
               r.weapon_id == 321 &&
               r.pos[0] == -123456789 && r.wvel[1] == 6000 && r.flags == kLaunchGuided && r.weapon[23] == '\0' &&
               std::strlen(r.weapon) == 23 && decode(b, n - 1, r) == Status::Truncated,
           "launch: 80 bytes, the weapon's name always terminated, the sender's weapon id kept");
    expect(launch_role(false, true, true, true) == LaunchRole::Visual &&
               launch_role(true, true, false, false) == LaunchRole::LiveNative &&
               launch_role(true, false, true, true) == LaunchRole::LiveSpawn &&
               launch_role(true, false, true, false) == LaunchRole::Visual &&
               launch_role(true, false, false, false) == LaunchRole::Visual,
           "launch: a guided missile is live only on its target's PC (our jet, or the host's own object), else visual");
    expect(!own_launch_stays_live(true, true, true) && own_launch_stays_live(true, true, false) &&
               own_launch_stays_live(false, true, true) && own_launch_stays_live(true, false, true),
           "launch: the host's own missile at a client's jet is visual-only (that client flies the live one)");
    {
        const LaunchPlan nat_us = launch_plan(LaunchRole::LiveNative, true, true);
        const LaunchPlan fail_us = launch_plan(LaunchRole::LiveNative, true, false);
        const LaunchPlan host_own = launch_plan(LaunchRole::LiveSpawn, true, true);
        const LaunchPlan vis_g = launch_plan(LaunchRole::Visual, true, true);
        const LaunchPlan bomb_ok = launch_plan(LaunchRole::Visual, false, true);
        const LaunchPlan bomb_fail = launch_plan(LaunchRole::Visual, false, false);
        expect(!nat_us.spawn && fail_us.spawn && fail_us.live && host_own.spawn && host_own.live && vis_g.spawn &&
                   !vis_g.live && !bomb_ok.spawn && bomb_fail.spawn && !bomb_fail.live,
               "launch plan: the avatar's own fire for bombs and missiles at us; a spawn otherwise, live wherever the "
               "role is (also a missile at us the avatar could not fire)");
    }
    {
        ShotLedger led;
        HitMsg gun{};
        gun.slot = 4;
        gun.weapon = 3;
        HitMsg st{};
        st.slot = 6;
        st.weapon = static_cast<uint16_t>(0x8000 | 42);
        uint16_t seq = 0;
        auto shot = [&](HitMsg m, double t) {
            m.seq = ++seq;
            return check_hit_shot(m, led, t);
        };
        const bool no_trigger = shot(gun, 10.0) == HitVerdict::NoShot;
        led.guns(true, 10.0);
        const bool firing = shot(gun, 10.5) == HitVerdict::Ok;
        led.guns(false, 11.0);
        const bool grace = shot(gun, 13.8) == HitVerdict::Ok && shot(gun, 14.2) == HitVerdict::NoShot;
        expect(no_trigger && firing && grace,
               "shots: a gun claim needs the claimer's trigger on, or off at most 3 s (a round's time of flight)");
        const bool no_launch = shot(st, 20.0) == HitVerdict::NoShot;
        led.launch(41, 20.0);
        const bool other = shot(st, 21.0) == HitVerdict::NoShot;
        led.launch(42, 21.0);
        int taken = 0;
        for (int i = 0; i < 70; ++i) taken += shot(st, 30.0) == HitVerdict::Ok;
        led.launch(42, 40.0);
        const bool late = shot(st, 40.0 + 91.0) == HitVerdict::NoShot;
        expect(no_launch && other && taken == ShotLedger::kStoreBudget && late,
               "shots: a store claim needs a launch of that weapon, at most 64 hits each, within 90 s");
        HitMsg again = gun;
        again.seq = seq;  // the last one again
        led.guns(true, 50.0);
        expect(check_hit_shot(again, led, 50.0) == HitVerdict::Replayed, "shots: an old or repeated seq is refused");
    }
    uint16_t wire = 0;
    expect(hit_weapon_to_wire(0xffff + 7, &wire) && wire == 7 && hit_weapon_from_wire(wire) == 0xffff + 7 &&
               hit_weapon_to_wire(42, &wire) && wire == (0x8000 | 42) && hit_weapon_from_wire(wire) == 42 &&
               !hit_weapon_to_wire(0x9000, &wire),
           "hit: guns and stores told apart on the wire");
    HitMsg bomb{};
    bomb.slot = 6;
    bomb.weapon = static_cast<uint16_t>(0x8000 | 42);
    bomb.energy = 1700000000u;
    bomb.p0[0] = 25000;  // the blast 250 m from the target
    HitRate rate;
    expect(check_hit_claim(bomb, 20000.0, true, rate, 1.0, store_limits()) == HitVerdict::Ok &&
               check_hit_claim(bomb, 20000.0, true, rate, 1.0) == HitVerdict::TooMuchEnergy,
           "check: a 1.7 GJ bomb from a jet 20 km away passes the store limits, not the gun limits");
    std::printf("    estimate: 78 B a launch (about 120 B on the wire), relayed by the host to each other client; a few\n"
                "    a minute in combat. Bomb and rocket hits are claims (43 B each, one per object in the blast)\n");

    ServeRun s;
    s.cfg.script = bot::Script::Straight;
    s.cfg.has_start = true;
    s.cfg.start[2] = 3000.0;
    s.cfg.launch = true;
    s.cfg.launch_weapon = "AIM-9J";
    s.cfg.launch_guided = true;
    s.cfg.launch_kind = 1;
    s.cfg.launch_a = 3;
    s.cfg.launch_b = 1;
    s.cfg.launch_shooter_a = 3;
    s.cfg.launch_every = 1.0;
    if (!s.start()) return expect(false, "bot serve listening (--launch)");
    ServeClient c;
    expect(c.core.start("127.0.0.1", s.port, cc_hello(0), cc_client_log), "client start");
    std::vector<WeaponLaunchMsg> got;
    bool sent = false;
    WorldPacket wp[8];
    serve_run_for(c, 3.5, [&] {
        const size_t k = c.core.poll_world(wp, 8);
        for (size_t i = 0; i < k; ++i) {
            WeaponLaunchMsg m{};
            if (decode(wp[i].data, wp[i].len, m) == Status::Ok) got.push_back(m);
        }
        if (!sent && !c.welcomes.empty()) {
            WeaponLaunchMsg own{};
            std::snprintf(own.weapon, sizeof(own.weapon), "Mk82");
            own.weapon_id = 42;
            uint8_t ob[sizeof(own)];
            sent = c.core.send_world(ob, encode(own, ob, sizeof(ob)));
        }
        return false;
    });
    s.finish();
    c.core.stop(false);
    expect(got.size() >= 2 && !std::strcmp(got[0].weapon, "AIM-9J") && (got[0].flags & kLaunchGuided) &&
               got[0].target_a == 3 && got[0].target_b == 1 && got[0].shooter_a == 3 && got[0].seq == 1 &&
               got[0].pos[2] == 300000 - 200,
           "loopback: --launch releases a guided AIM-9J at the client's seat from the bot's jet, about once a second");
    expect(sent && s.res.launches_rx == 1 && s.res.last_rx_weapon == "Mk82",
           "loopback: the client's launch reaches the host with its weapon's name");
}

static void object_type_tests() {
    using namespace LazyWorks::SF2Patch::acmi;
    auto tags = [](uint8_t cls, const char* name) { return std::string(ground_type_tags(cls, name)); };
    const uint8_t V = TacviewRec::kGroundVehicle, S = TacviewRec::kGroundStatic, H = TacviewRec::kShip;
    expect(tags(V, "Foo Truck") == "Ground+Medium+Vehicle" && tags(V, "Fuel Tanker") == "Ground+Medium+Vehicle",
           "types: truck and tanker are medium vehicles, not tanks");
    expect(tags(V, "Zork Jeep") == "Ground+Light+Vehicle" && tags(V, "Staff Car") == "Ground+Light+Vehicle", "types: jeep and car");
    expect(tags(V, "Zork Main Battle Tank") == "Ground+Heavy+Armor+Vehicle+Tank" && tags(V, "Zork MBT") == "Ground+Heavy+Armor+Vehicle+Tank",
           "types: tank");
    expect(tags(V, "Zork APC") == "Ground+Heavy+Armor+Vehicle" && tags(V, "Zork Howitzer") == "Ground+Heavy+Armor+Vehicle",
           "types: APC and artillery are armor");
    expect(tags(V, "Zork AAA Gun") == "Ground+AntiAircraft" && tags(V, "SA-13 Launcher") == "Ground+AntiAircraft" &&
               tags(V, "Mosa-13") == "Ground+Vehicle",
           "types: anti-aircraft, and a lookalike is not");
    expect(tags(V, "Zork Rifle Squad") == "Ground+Light+Human+Infantry" && tags(V, "Zorkmobile") == "Ground+Vehicle",
           "types: infantry; unknown is a plain vehicle");
    expect(tags(H, "Zork Carrier") == "Sea+Watercraft+AircraftCarrier" && tags(H, "Zork Destroyer") == "Sea+Watercraft+Warship" &&
               tags(H, "Zork Submarine") == "Sea+Watercraft+Submarine" && tags(H, "Zork Barge") == "Sea+Watercraft",
           "types: ship classes");
    expect(tags(S, "Zork Hangar") == "Ground+Static+Building" && tags(S, "Zork Fuel Tank") == "Ground+Static+Container" &&
               tags(S, "Zork Radar") == "Ground+AntiAircraft" && tags(S, "Zork Flak Tower") == "Ground+AntiAircraft",
           "types: static objects");
    expect(tags(TacviewRec::kStorage, "Zork") == "Ground+Static+Container" && tags(TacviewRec::kBridge, "Fuel Bridge") == "Ground+Static+Building" &&
               tags(TacviewRec::kStructure, "Zork Storage Tank") == "Ground+Static+Container" &&
               tags(TacviewRec::kStructure, "Zork Bunker") == "Ground+Static+Building",
           "types: structures: the class decides, the name refines");
    expect(tags(V, "Zork Personnel Carrier") == "Ground+Heavy+Armor+Vehicle" && tags(V, "Zork Caravan") == "Ground+Vehicle" &&
               tags(V, "Bush Wagon") == "Ground+Vehicle" && tags(V, "Samson Mk2") == "Ground+Vehicle" &&
               tags(S, "Zork Police Pole") == "Ground+Static+Building" && tags(V, "Zork Trucks") == "Ground+Medium+Vehicle",
           "types: whole words only, armor before car");
    expect(is_storage_category("FUEL_STORAGE") && is_storage_category("Oil") && !is_storage_category("POLE") && !is_storage_category("POLICE") && !is_storage_category("BRIDGE") && !is_storage_category("BUILDING"),
           "types: storage category");
}

void stores_tests() {
    std::printf("stores, fuel and ammunition (Phase 5 T5)\n");
    LoadoutMsg lo{};
    lo.peer = 1;
    lo.seq = 4;
    lo.parts = 2;
    lo.part = 1;
    lo.n = 2;
    lo.e[1].station = 6;
    lo.e[1].weapon = 77;
    lo.e[1].hash = weapon_name_hash("Mk82");
    lo.e[1].count = 3;
    uint8_t b[128];
    LoadoutMsg lr{};
    const size_t ln = encode(lo, b, sizeof(b));
    expect(ln == 95 && peek_world_type(b, ln) == 30 && decode(b, ln, lr) == Status::Ok && lr.e[1].station == 6 &&
               lr.e[1].weapon == 77 && lr.e[1].count == 3 && lr.e[1].hash == weapon_name_hash("Mk82"),
           "loadout: 95 bytes, up to 12 stations a part, round trip");
    lo.part = 2;
    LoadoutMsg bad{};
    expect(decode(b, encode(lo, b, sizeof(b)), bad) == Status::BadType, "loadout: a part past the count is refused");
    ConsumablesMsg co{};
    co.fuel = fuel_to_wire(3999.0f);
    co.fuel_ext = fuel_to_wire(1200.0f);
    co.guns = 2;
    co.rounds[1] = 412;
    co.flares = 30;
    ConsumablesMsg cr{};
    const size_t cn = encode(co, b, sizeof(b));
    expect(cn == 23 && peek_world_type(b, cn) == 31 && decode(b, cn, cr) == Status::Ok && cr.fuel == 1000 &&
               cr.fuel_ext == 300 &&
               cr.rounds[1] == 412 && cr.flares == 30,
           "consumables: 23 bytes, internal and external fuel in 4 kg steps, round trip");
    co.guns = 5;
    expect(decode(b, encode(co, b, sizeof(b)), cr) == Status::BadType, "consumables: at most 4 guns");
    expect(weapon_name_hash("AIM-9J") == weapon_name_hash("AIM-9J") && weapon_name_hash("AIM-9J") != weapon_name_hash("AIM-9L") &&
               weapon_name_hash("") != 0 && fuel_to_wire(-5.0f) == 0 && fuel_to_wire(1e9f) == 65535 &&
               fuel_from_wire(1000) == 4000.0f,
           "loadout: name hashes tell weapons apart; fuel clamps");
    expect(station_step(17, 2, 17, 2, false) == StationStep::Keep && station_step(17, 0, 17, 2, false) == StationStep::Rebuild &&
               station_step(-1, 0, 17, 2, false) == StationStep::Rebuild && station_step(5, 2, 17, 2, false) == StationStep::Rebuild &&
               station_step(-1, 0, -1, 0, false) == StationStep::Keep,
           "loadout: rebuild for another weapon or a rearm, leave equal ones");
    expect(station_step(17, 2, 17, 1, false) == StationStep::Rebuild && station_step(17, 2, 9, 0, false) == StationStep::Empty,
           "loadout: an avatar with more stores than the owner is rebuilt to the owner's count; an emptied one is noted");
    expect(station_step(17, 0, 17, 2, true) == StationStep::GiveUp && station_step(17, 0, 17, 3, false) == StationStep::Rebuild,
           "loadout: a rebuild that did not take is given up until the owner's station changes");
    expect(station_step(-1, 0, 17, 2, false, false) == StationStep::Skip && station_step(-1, 0, -1, 0, false, false) == StationStep::Keep,
           "loadout: a station a fallback airframe does not have is skipped");
    expect(fuel_scale(2000.0f, 1000.0f) == 0.5f && fuel_scale(0.0f, 1000.0f) == 1.0f && fuel_scale(100.0f, 5000.0f) == 10.0f,
           "fuel: the avatar's tanks scale to the owner's total (bounded)");
    std::printf("    estimate: a loadout is 95 B every 10 s (about 14 B/s with headers, per player and receiver), consumables\n"
                "    23 B every 2 s unreliable (about 30 B/s); a rearm or new loadout goes at once\n");

    namespace jet = LazyWorks::SF2Patch::hooks::jet;
    std::vector<uint8_t> image(jet::kAoSize), obj(0x9a8), comp(0xe0), s1(0x90), s2(0x90);
    const uintptr_t ao = reinterpret_cast<uintptr_t>(image.data());
    auto put32 = [](std::vector<uint8_t>& v, size_t off, uint32_t x) { std::memcpy(v.data() + off, &x, 4); };
    auto addr = [](const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); };
    put32(s1, 0, static_cast<uint32_t>(ao + 0x93abc));
    put32(s2, 0, 0x1234);  // a vtable outside the image
    uint32_t systems[3] = {addr(s1.data()), 0, addr(s2.data())};
    put32(comp, jet::kCompSystems + jet::kListCount, 3);
    put32(comp, jet::kCompSystems + jet::kListArray, addr(systems));
    uint32_t comps[2] = {addr(comp.data()), 0};
    put32(obj, jet::kComponents + jet::kListCount, 2);
    put32(obj, jet::kComponents + jet::kListArray, addr(comps));
    LazyWorks::SF2Patch::hooks::SysRef refs[8];
    const int nr = LazyWorks::SF2Patch::hooks::jet_list_systems(obj.data(), ao, refs, 8);
    expect(nr == 2 && refs[0].vt == 0x93abc && refs[1].vt == 0 && refs[1].sys == s2.data(),
           "systems: walked in component order, empty slots skipped, a foreign vtable as 0");

    ServeRun s;
    s.cfg.script = bot::Script::Straight;
    s.cfg.has_start = true;
    s.cfg.start[2] = 3000.0;
    s.cfg.stores = true;
    if (!s.start()) return expect(false, "bot serve listening (--stores)");
    ServeClient c;
    expect(c.core.start("127.0.0.1", s.port, cc_hello(0), cc_client_log), "client start");
    std::vector<LoadoutMsg> los;
    std::vector<ConsumablesMsg> cos;
    int sent = 0;
    WorldPacket wp[8];
    serve_run_for(c, 4.5, [&] {
        const size_t k = c.core.poll_world(wp, 8);
        for (size_t i = 0; i < k; ++i) {
            LoadoutMsg l{};
            ConsumablesMsg q{};
            if (decode(wp[i].data, wp[i].len, l) == Status::Ok) los.push_back(l);
            if (decode(wp[i].data, wp[i].len, q) == Status::Ok) cos.push_back(q);
        }
        if (sent < 2 && !c.welcomes.empty()) {
            LoadoutMsg ol{};
            ol.parts = 1;
            ConsumablesMsg oc{};
            oc.fuel = 777;
            uint8_t ob[128];
            if (c.core.send_world(ob, encode(ol, ob, sizeof(ob)), true) &&
                c.core.send_world(ob, encode(oc, ob, sizeof(ob)), false))
                ++sent;
        }
        return false;
    });
    s.finish();
    c.core.stop(false);
    expect(los.size() == 1 && los[0].n == 1 && los[0].e[0].hash == weapon_name_hash("AIM-9J") && los[0].e[0].count == 2,
           "loopback: --stores sends the jet's loadout (reliable) with the weapon's name hash");
    expect(cos.size() >= 2 && cos[0].fuel == fuel_to_wire(4000.0f) && cos.back().fuel < cos[0].fuel && cos[0].rounds[0] == 640 &&
               cos[0].fuel_ext == fuel_to_wire(1200.0f),
           "loopback: --stores sends falling fuel and rounds every 2 s (unreliable)");
    expect(sent == 2 && s.res.loadouts_rx == 2 && s.res.consumables_rx >= 1 && s.res.last_rx_fuel == 777,
           "loopback: the client's loadout (reliable) and consumables (unreliable) reach the host");
}

void damage_tests() {
    std::printf("damage, death, ejection and countermeasures (Phase 5 T6)\n");
    DamageStateMsg d{};
    d.peer = 2;
    d.level = 3;
    d.flags = kDmgFire;
    d.components = 12;
    d.lost = 1ull << 40;
    d.attacker_kind = 1;
    d.attacker_a = 5;
    uint8_t b[96];
    DamageStateMsg r{};
    const size_t n = encode(d, b, sizeof(b));
    d.attacker_weapon = 0xffff + 3;
    d.life = 9;
    const size_t n2 = encode(d, b, sizeof(b));
    expect(n == 47 && n2 == 47 && peek_world_type(b, n2) == 32 && decode(b, n2, r) == Status::Ok && r.lost == (1ull << 40) &&
               r.attacker_a == 5 && r.flags == kDmgFire && r.attacker_weapon == 0xffff + 3 && r.life == 9,
           "damage: 47 bytes, round trip (with the kill weapon and the owner's life)");
    d.level = 6;
    expect(decode(b, encode(d, b, sizeof(b)), r) == Status::BadType, "damage: a level above 5 is refused");
    CountermeasureMsg k{};
    k.flares = 2;
    CountermeasureMsg kr{};
    const size_t kn = encode(k, b, sizeof(b));
    expect(kn == 10 && peek_world_type(b, kn) == 33 && decode(b, kn, kr) == Status::Ok && kr.flares == 2,
           "countermeasures: 10 bytes, round trip");
    expect(released(30, 28) == 2 && released(28, 30) == 0 && released(400, 0) == 255, "countermeasures: releases are falls");

    DamageStateMsg was{}, now{};
    now.level = 2;
    now.shown = 0b0110;
    now.lost = 0b0100;
    now.dead = 0b1000;
    now.flags = kDmgFire;
    DamagePlan p = damage_plan(was, now);
    expect(p.lose == 0b0100 && p.show == 0b0010 && p.kill_systems == 0b1000 && p.set_level && p.level == 2 &&
               p.fire_on && !p.destroy && !p.pilot_gone,
           "plan: new losses, damage shown on parts still on, systems, the level and fire");
    was = now;
    now.flags = kDmgPilotGone;
    p = damage_plan(was, now);
    expect(p.lose == 0 && p.show == 0 && !p.set_level && p.fire_off && p.pilot_gone, "plan: only what changed (fire out, ejected)");
    was = now;
    now.level = 5;
    now.flags |= kDmgDestroyed;
    p = damage_plan(was, now);
    const bool destroy_once = p.destroy && !p.set_level && !damage_plan(now, now).destroy;
    expect(destroy_once, "plan: destroyed once (the game's destroy, not a level write)");
    expect(!damage_differs(now, now) && damage_differs(was, now), "damage: sent only on change");
    expect(!kill_event_allowed(true, true) && kill_event_allowed(true, false) && kill_event_allowed(false, true) &&
               kill_event_allowed(false, false),
           "kills: a joined client logs no kill for a remote copy (no phantom kill entry, mission success unmoved); "
           "the host logs every kill, a client its own jet's");
    {
        int A = 0, B = 0;
        PeerDamage pd;
        DamageStateMsg hit{}, dead{}, next{};
        hit.life = dead.life = 1;
        hit.level = 2;
        dead.level = 5;
        dead.flags = kDmgDestroyed;
        next.life = 2;
        const bool hit_ok = pd.on_state(hit, &A, false).apply;
        const PeerDamage::Step kill = pd.on_state(dead, &A, false);
        const bool retake_ignored = !pd.on_state(dead, &B, false).apply && !pd.on_state(hit, &B, false).apply;
        const bool dead_now = pd.dead();
        const PeerDamage::Step again = pd.on_state(next, &B, false);
        expect(hit_ok && kill.apply && kill.plan.destroy && retake_ignored && dead_now && again.apply && !pd.dead(),
               "life: after its destruction a peer's resends destroy no retaken aircraft; a new life starts afresh");
        PeerDamage pd2;
        const PeerDamage::Step twice = pd2.on_state(dead, &A, true);
        expect(twice.apply && !twice.plan.destroy && pd2.dead(),
               "life: no second destroy for an avatar its mirrored part losses already destroyed");
        PeerDamage pd3;
        pd3.on_state(hit, nullptr, false);
        pd3.on_state(dead, nullptr, false);
        expect(pd3.dead() && !pd3.on_state(dead, &A, false).apply,
               "life: a death seen while no aircraft shows the peer still blocks a later take");
        PeerDamage pd4;
        pd4.on_state(hit, &A, false);
        const PeerDamage::Step re = pd4.on_state(hit, &B, false);
        expect(re.apply && re.plan.set_level && re.plan.level == 2, "life: a retake within a life gets the whole state again");
    }
    expect(cm_released(30, 28, true, true) == 2 && cm_released(30, 28, false, true) == 0 &&
               cm_released(30, 28, true, false) == 0 && cm_released(30, 0, true, true) == 0 && cm_released(28, 30, true, true) == 0,
           "countermeasures: a release only while alive with working dispensers, at most 8 at once (else a reset)");
    std::printf("    estimate: 47 B a damage change (a few in a fight, plus one every 10 s), 10 B a chaff/flare release\n");

    bot::DamageStep s0 = bot::damage_script_at(1.0), s1 = bot::damage_script_at(7.0), s2 = bot::damage_script_at(12.5);
    expect(s0.level == 0 && s1.level == 2 && s1.lost == 4 && s2.level == 5 && (s2.flags & kDmgDestroyed) &&
               (s2.flags & kDmgPilotGone),
           "bot --damage: hit, part lost, fire, ejection, destroyed");

    ServeRun sv;
    sv.cfg.script = bot::Script::Straight;
    sv.cfg.has_start = true;
    sv.cfg.start[2] = 3000.0;
    sv.cfg.damage = true;
    if (!sv.start()) return expect(false, "bot serve listening (--damage)");
    ServeClient c;
    expect(c.core.start("127.0.0.1", sv.port, cc_hello(0), cc_client_log), "client start");
    std::vector<DamageStateMsg> got;
    int cms = 0;
    bool sent = false;
    WorldPacket wp[8];
    serve_run_for(c, 6.8, [&] {
        const size_t k2 = c.core.poll_world(wp, 8);
        for (size_t i = 0; i < k2; ++i) {
            DamageStateMsg m{};
            CountermeasureMsg q{};
            if (decode(wp[i].data, wp[i].len, m) == Status::Ok) got.push_back(m);
            if (decode(wp[i].data, wp[i].len, q) == Status::Ok) ++cms;
        }
        if (!sent && !c.welcomes.empty()) {
            DamageStateMsg own{};
            own.level = 4;
            CountermeasureMsg oc{};
            oc.chaff = 1;
            uint8_t ob[64];
            sent = c.core.send_world(ob, encode(own, ob, sizeof(ob)), true) && c.core.send_world(ob, encode(oc, ob, sizeof(ob)), true);
        }
        return false;
    });
    sv.finish();
    c.core.stop(false);
    expect(got.size() == 3 && got[0].level == 0 && got[1].level == 1 && got[2].level == 2 && got[2].lost == 4 && cms == 2,
           "loopback: the host's damage reaches the client on each change (reliable), and its flares");
    expect(sent && sv.res.damage_rx == 1 && sv.res.last_rx_level == 4 && sv.res.cm_rx == 1,
           "loopback: the client's damage and chaff reach the host");
}

void clock_ground_tests() {
    std::printf("mission clock and parked copies (handover tasks 1 and 2)\n");
    MissionClockMsg m{};
    m.owner_ms = 100000;
    m.clock_ms = clock_to_wire(321.5f);
    m.clock2_ms = clock_to_wire(321.5f);
    uint8_t b[32];
    MissionClockMsg r{};
    const size_t n = encode(m, b, sizeof(b));
    expect(n == 20 && peek_world_type(b, n) == 34 && decode(b, n, r) == Status::Ok && r.clock_ms == 321500 &&
               clock_to_wire(-1.0f) == 0,
           "clock: 20 bytes, ms, round trip");
    const ClockFix late = clock_fix(300.0, m, 0.05);
    const ClockFix close = clock_fix(321.6, m, 0.05);
    const ClockFix slow = clock_fix(300.0, m, 5.0);
    expect(late.set && std::fabs(late.target - 321.55) < 1e-6 && !close.set && slow.set && std::fabs(slow.target - 322.5) < 1e-6,
           "clock: owner time plus half the round trip (at most 1 s); within 0.25 s left alone");
    {  // a 10 s pause on the client: its clock stops while the owner's runs; one correction, then quiet
        double local = 100.0;
        MissionClockMsg o{};
        o.clock_ms = 100000;
        int sets = 0;
        for (int i = 0; i < 12; ++i) {
            o.clock_ms += 2000;  // the owner's clock, 2 s per message
            if (i >= 5 && i <= 9) continue;  // paused: no game step, the messages are not applied
            local += 2.0;
            const ClockFix f = clock_fix(local, o, 0.02);
            if (f.set) {
                ++sets;
                local = f.target;
            }
        }
        expect(sets == 1 && std::fabs(local - (o.clock_ms / 1000.0 + 0.02)) < 1e-6,
               "clock: a 10 s pause on the client gives one correction when it runs again, then none");
    }
    std::printf("    estimate: 20 B every 2 s from the owner to each other PC (about 25 B/s with headers)\n");
    namespace hk = LazyWorks::SF2Patch::hooks;
    expect(hk::copy_on_ground(0.5f, 0.8f, 3.0f) && hk::copy_on_ground(1.7f, 0.8f, 60.0f) && !hk::copy_on_ground(5.0f, 0.8f, 3.0f) &&
               !hk::copy_on_ground(0.5f, 0.8f, 150.0f),
           "ground: a slow copy at its gear height is on the ground; a high or fast one is airborne");

    ServeRun s;
    s.cfg.script = bot::Script::Straight;
    s.cfg.has_start = true;
    s.cfg.start[2] = 3000.0;
    s.cfg.clock = true;
    s.cfg.clock_start = 600.0;
    if (!s.start()) return expect(false, "bot serve listening (--clock)");
    ServeClient c;
    expect(c.core.start("127.0.0.1", s.port, cc_hello(0), cc_client_log), "client start");
    std::vector<MissionClockMsg> got;
    WorldPacket wp[8];
    serve_run_for(c, 4.5, [&] {
        const size_t k = c.core.poll_world(wp, 8);
        for (size_t i = 0; i < k; ++i) {
            MissionClockMsg q{};
            if (decode(wp[i].data, wp[i].len, q) == Status::Ok) got.push_back(q);
        }
        return false;
    });
    s.finish();
    c.core.stop(false);
    expect(got.size() >= 2 && got[0].clock_ms >= 600000 && got[0].clock_ms < 600100 &&
               got[1].clock_ms - got[0].clock_ms > 1900 && got[1].clock_ms - got[0].clock_ms < 2100,
           "loopback: --clock sends the owner's mission clock every 2 s (unreliable), running with host time");
}

void seat_takeover_tests() {
    std::printf("seat takeover in place (handover task 3)\n");
    SeatStateMsg m{};
    m.seat = 2;
    m.flags = kSeatAlive | kSeatAirborne;
    m.host_ms = 123456;
    m.pose.pos[0] = 5000000;
    m.fuel = fuel_to_wire(2000.0f);
    m.n = 1;
    m.e[0].station = 4;
    m.e[0].count = 2;
    uint8_t b[160];
    SeatStateMsg r{};
    const size_t n = encode(m, b, sizeof(b));
    expect(n == 135 && peek_world_type(b, n) == 35 && decode(b, n, r) == Status::Ok && r.pose.pos[0] == 5000000 &&
               r.e[0].station == 4 && r.fuel == 500,
           "seat: 135 bytes, round trip");
    m.n = 13;
    expect(decode(b, encode(m, b, sizeof(b)), r) == Status::BadType, "seat: at most 12 stations");
    m.n = 1;
    SeatStateMsg parked = m;
    parked.flags = kSeatAlive;
    SeatStateMsg dead = m;
    dead.flags = kSeatAirborne;
    expect(seat_takeover_due(m, 8000.0) && !seat_takeover_due(m, 20.0) && !seat_takeover_due(parked, 8000.0) &&
               !seat_takeover_due(dead, 8000.0),
           "seat: a late joiner takes over a live, flying seat it is far from; not a parked or destroyed one, not when "
           "already there (a reconnect)");
    std::printf("    estimate: 135 B once per Welcome\n");

    ServeRun s;
    s.cfg.script = bot::Script::Straight;
    s.cfg.has_start = true;
    s.cfg.start[2] = 3000.0;
    s.cfg.seat_state = true;
    if (!s.start()) return expect(false, "bot serve listening (--seat-state)");
    ServeClient c;
    expect(c.core.start("127.0.0.1", s.port, cc_hello(0), cc_client_log), "client start");
    std::vector<SeatStateMsg> got;
    WorldPacket wp[8];
    serve_run_for(c, 2.5, [&] {
        const size_t k = c.core.poll_world(wp, 8);
        for (size_t i = 0; i < k; ++i) {
            SeatStateMsg q{};
            if (decode(wp[i].data, wp[i].len, q) == Status::Ok) got.push_back(q);
        }
        return !got.empty();
    });
    s.finish();
    c.core.stop(false);
    expect(got.size() == 1 && (got[0].flags & kSeatAirborne) && got[0].n == 1 && got[0].fuel == fuel_to_wire(2000.0f) &&
               got[0].e[0].hash == weapon_name_hash("AIM-9J"),
           "loopback: --seat-state sends the seat's state once, reliably, after the client's first State");
}

void handover_tests() {
    std::printf("authority handover (handover task 4)\n");
    HandoverSnapshot s;
    s.head.epoch = 0x00010002;
    s.head.t_ms = 500000;
    s.head.clock_ms = 812000;
    for (int i = 0; i < 80; ++i) {
        HandoverAircraft a{};
        a.pose.kind = 1;
        a.pose.a = static_cast<uint16_t>(i / 4);
        a.pose.b = static_cast<uint16_t>(i % 4);
        a.pose.pos[0] = 100000 * i;
        a.state = 0x00800800u;
        a.fuel = 700;
        a.stations = 2;
        a.station[0] = 1;
        a.count[0] = 2;
        s.aircraft.push_back(a);
    }
    for (int i = 0; i < 200; ++i) {
        HandoverVehicle v{};
        v.pose.kind = 2;
        v.pose.a = static_cast<uint16_t>(40 + i / 8);
        v.pose.b = static_cast<uint16_t>(i % 8);
        v.speed = 200;
        s.vehicles.push_back(v);
    }
    for (int i = 0; i < 30; ++i) s.flights.push_back(HandoverFlight{static_cast<uint16_t>(i), 3});
    const std::vector<uint8_t> raw = serialize(s);
    HandoverSnapshot back;
    expect(raw.size() == 22 + 80 * 60 + 200 * 40 + 30 * 3 && deserialize(raw.data(), raw.size(), &back) &&
               back.aircraft.size() == 80 && back.aircraft[79].pose.pos[0] == 7900000 && back.vehicles[5].speed == 200 &&
               back.flights[29].waypoint == 3 && back.head.clock_ms == 812000 && !deserialize(raw.data(), raw.size() - 1, &back),
           "handover: a busy mission's snapshot (80 aircraft, 200 vehicles, 30 flights) round trips; a short one is refused");
    std::vector<uint8_t> packed, out;
    uint32_t crc = 0;
    expect(pack_handover(raw, &packed, &crc) && unpack_handover(packed, crc, static_cast<uint32_t>(raw.size()), &out) &&
               out == raw && !unpack_handover(packed, crc ^ 1, static_cast<uint32_t>(raw.size()), &out),
           "handover: packed, CRC-checked, unpacked");
    std::printf("    estimate: %zu bytes raw, %zu packed, %zu chunks of 80 B (91 B messages); once per handover\n", raw.size(),
                packed.size(), (packed.size() + kHandoverChunkData - 1) / kHandoverChunkData);
    HandoverBeginMsg b{};
    b.epoch = s.head.epoch;
    b.packed = static_cast<uint32_t>(packed.size());
    b.raw = static_cast<uint32_t>(raw.size());
    b.chunks = static_cast<uint16_t>((packed.size() + kHandoverChunkData - 1) / kHandoverChunkData);
    b.crc = crc;
    b.clock_ms = s.head.clock_ms;  // ahead of the bot's (no --clock: 0)
    HandoverAssembly as;
    bool done = false;
    expect(as.begin(b), "handover: an offer starts an assembly");
    for (int pass = 0; pass < 2; ++pass)  // every chunk twice, backwards: duplicates and order do not matter
        for (int i = b.chunks - 1; i >= 0; --i) {
            HandoverChunkMsg c{};
            c.epoch = b.epoch;
            c.index = static_cast<uint16_t>(i);
            const size_t off = static_cast<size_t>(i) * kHandoverChunkData;
            c.len = static_cast<uint8_t>(packed.size() - off < kHandoverChunkData ? packed.size() - off : kHandoverChunkData);
            std::memcpy(c.data, packed.data() + off, c.len);
            done = as.add(c);
        }
    expect(done && as.bytes() == packed, "handover: chunks in any order, twice, give the packed bytes");
    HandoverBeginMsg bad = b;
    bad.chunks = static_cast<uint16_t>(b.chunks + 1);
    expect(!as.begin(bad), "handover: an offer whose chunk count does not fit its size is refused");
    uint8_t mb[128];
    HandoverChunkMsg cr{};
    HandoverChunkMsg c0{};
    c0.len = 0;
    expect(encode(b, mb, sizeof(mb)) == 26 && sizeof(HandoverChunkMsg) == 91 &&
               decode(mb, encode(c0, mb, sizeof(mb)), cr) == Status::BadType,
           "handover: messages fit the 96-byte queues; an empty chunk is refused");
    expect(take_handover(60000, 10000) && !take_handover(10000, 60000) && !take_handover(10500, 10000),
           "handover: the side whose mission clock is ahead loaded first and keeps the world");

    ServeRun sv;
    sv.cfg.script = bot::Script::Straight;
    sv.cfg.has_start = true;
    sv.cfg.start[2] = 3000.0;
    if (!sv.start()) return expect(false, "bot serve listening (handover)");
    ServeClient c;
    expect(c.core.start("127.0.0.1", sv.port, cc_hello(0), cc_client_log), "client start");
    std::deque<std::vector<uint8_t>> q;
    {
        std::vector<uint8_t> m(sizeof(b));
        encode(b, m.data(), m.size());
        q.push_back(m);
        for (uint16_t i = 0; i < b.chunks; ++i) {
            HandoverChunkMsg ch{};
            ch.epoch = b.epoch;
            ch.index = i;
            const size_t off = static_cast<size_t>(i) * kHandoverChunkData;
            ch.len = static_cast<uint8_t>(packed.size() - off < kHandoverChunkData ? packed.size() - off : kHandoverChunkData);
            std::memcpy(ch.data, packed.data() + off, ch.len);
            std::vector<uint8_t> cm(sizeof(ch));
            encode(ch, cm.data(), cm.size());
            q.push_back(cm);
        }
    }
    bool answered = false, taken = false;
    WorldPacket wp[8];
    serve_run_for(c, 6.0, [&] {
        if (!c.welcomes.empty())
            for (int i = 0; i < 16 && !q.empty(); ++i) {
                if (!c.core.send_world(q.front().data(), q.front().size(), true)) break;
                q.pop_front();
            }
        const size_t k = c.core.poll_world(wp, 8);
        for (size_t i = 0; i < k; ++i) {
            HandoverDoneMsg d{};
            if (decode(wp[i].data, wp[i].len, d) == Status::Ok) {
                answered = true;
                taken = d.taken != 0 && d.epoch == b.epoch;
            }
        }
        return answered;
    });
    sv.finish();
    c.core.stop(false);
    expect(answered && taken && sv.res.handovers == 1 && sv.res.handover_aircraft == 80 && sv.res.handover_vehicles == 200,
           "loopback: a client's world offer (paced, 16 chunks a step) reaches the host, unpacks, and is answered");
}

int main() {
    object_type_tests();
    std::printf("protocol_selftest\n");
    const char* why = nullptr;
    bool ok = protocol_selftest(&why);
    expect(ok, ok ? "round trips and rejections" : why);

    {  // protocol v2 helpers
        static_assert(fnv1a("") == 0x811C9DC5u && fnv1a("a") == 0xE40C292Cu && fnv1a("foobar") == 0xBF9CF968u);
        expect(fnv1a("") == 0x811C9DC5u && fnv1a("a") == 0xE40C292Cu && fnv1a("foobar") == 0xBF9CF968u &&
                   fnv1a(nullptr) == 0 && fnv1a("Korea") != fnv1a("korea"),
               "fnv1a of known strings");
        char f[8];
        std::memset(f, 'z', sizeof(f));
        set_str(f, "abc");
        expect(std::memcmp(f, "abc\0\0\0\0\0", 8) == 0, "set_str zero-pads the field");
        set_str(f, "0123456789");
        expect(std::memcmp(f, "0123456\0", 8) == 0, "set_str truncates to N-1 and terminates");
        set_str(f, nullptr);
        expect(std::all_of(f, f + 8, [](char ch) { return ch == 0; }), "set_str(null) clears the field");
        uint8_t b[96];
        HelloMsg v1{};
        const size_t n = encode(v1, b, sizeof(b));
        b[2] = 1;
        MsgType t{};
        expect(n == 60 && decode(b, 52, v1) == Status::BadVersion && peek_type(b, 52, t) == Status::BadVersion &&
                   decode(b, n, v1) == Status::BadVersion,
               "a v1 Hello is rejected with BadVersion");
        expect(kVersion == 4 && sizeof(WelcomeMsg) == 64 && sizeof(PeerInfoMsg) == 55 && sizeof(PeerLeftMsg) == 7,
               "v4 sizes (as v3)");
    }

    loopback();
    parse_tests();
    host_session_logic_tests();

    if (enet_initialize() == 0) {
        host_core_tests();
        client_core_tests();
        enet_deinitialize();
    } else {
        expect(false, "enet_initialize");
    }

    bot_loopback_tests();
    bot_two_player_tests();
    stream_tests();
    acmi_tests();
    stream_kinds_tests();
    net_report_tests();
    acmi_ghost_tests();
    tracker_logic_tests();
    cursor_clip_tests();
    focus_logic_tests();
    focus_session_tests();
    tacview_tests();
    terrain_tests();
    texture_tests();
    aerodrome_core_tests();
    structure_tests();
    placement_tests();
    remote_jet_tests();
    remote_avatar_tests();
    client_logic_tests();
    gameplay_row_splice_tests();
    pilot_twist_tests();
    probe_snapshot_tests();
    world_files_tests();
    world_seat_tests();
    world_remote_tests();
    world_mission_tests();
    world_stream_tests();
    world_seat_rule_tests();
    world_avatar_tests();
    world_snapshot_tests();
    pilot_twist_settings_tests();
    jet_census_tests();
    jet_controls_tests();
    jet_mirror_tests();
    hit_claim_tests();
    weapon_launch_tests();
    stores_tests();
    damage_tests();
    clock_ground_tests();
    seat_takeover_tests();
    handover_tests();

    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
