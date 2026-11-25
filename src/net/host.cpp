// Host mode (developer tool, part of the probe): remote clients drive AI aircraft as avatars. Inert unless
// LAZYSF2_HOST is set and the probe is on. Runs on the game thread; the network thread (host_core.cpp) owns ENet.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include "hooks/hooks.h"
#include "hooks/replay_math.h"
#include "log.h"
#include "net/host_core.h"
#include "net/protocol.h"
#include "net/receiver_math.h"
#include "net/stream_format.h"
#include "net/stream_record.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

using namespace replay_math;
using net::HostEvent;

constexpr DWORD kRemoteMaskRva = 0xb7b28;  // AircraftObject.dll global: the REMOTEOBJECT flag mask
constexpr size_t kOffFlags = 0xc, kOffPos = 0x38, kOffVel = 0x50, kOffRates = 0xd0, kOffMatrix = 0xdc,
                 kOffAngles = 0x100;
constexpr int kRing = 16;      // received samples kept per client
constexpr int kPending = 16;   // samples drained but not yet applied by the avatar's own step
constexpr double kInitialDelay = 0.1;
constexpr double kMaxStep = 0.25;  // longest elapsed time the delay ramp counts per step
constexpr double kSpawnAhead = 300.0, kSpawnRight = 50.0;

// Bye reasons the host sends.
enum ByeReason : uint8_t { kByeNone = 0, kByeBusy = 1, kByeNoAvatar = 2, kByeSessionEnded = 3, kByeAvatarLost = 4 };

template <typename T>
void read_at(const void* obj, size_t off, T* out, size_t n) {
    std::memcpy(out, static_cast<const char*>(obj) + off, n * sizeof(T));
}
template <typename T>
void write_at(void* obj, size_t off, const T* in, size_t n) {
    std::memcpy(static_cast<char*>(obj) + off, in, n * sizeof(T));
}
double dist3(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// Set once by host_install(), then read-only.
bool g_enabled = false;
char g_addr[48] = {};
uint16_t g_port = 0;

// The big objects live on the heap, created in host_on_modules_loaded() only when host mode is on (as globals
// they would bloat the DLL's .data). Never freed: a destructor at DLL unload could join a thread under the loader lock.
net::HostCore* g_core = nullptr;
net::StreamWriter* g_stream = nullptr;  // the recording of the session (stream_record.h): written by its own thread
double g_now = 0;            // host game time at the latest step, for records made outside a step
uint32_t g_host_seq = 0;
std::atomic<bool> g_running{false};      // the step callback is registered and the thread started
std::atomic<bool> g_has_clients{false};  // mirrors g_nclients for other threads (puppet runs on the render thread)

struct Avatar {
    bool active = false;
    void* obj = nullptr;
    unsigned long long seq = 0;
    uint32_t set_bits = 0;  // REMOTEOBJECT bits that we set (mask & ~original flags)
    char callsign[17] = {};
    char aircraft[33] = {};
    uint32_t gen = 0;  // the core's connection generation for this slot; stale commands are dropped by the core
    double spawn[3] = {};
    double spawn_heading = 0;
    float hold_vel[3] = {};  // the player's velocity at the spawn: the avatar coasts with it until the first State
    double hold_t0 = 0;

    // Receiver (see replay.cpp for the same structure).
    Samp ring[kRing];
    int ring_n = 0;
    Samp pending[kPending];
    int pending_n = 0;
    bool have_seq = false;
    uint32_t last_seq = 0;
    double newest_t = 0;
    double intervals[net::kIntervalWindow] = {};
    int iv_n = 0, iv_pos = 0;
    double delay = kInitialDelay;
    bool have_last_now = false;
    double last_now = 0;
    double off_p[3] = {}, off_a[3] = {};
    double off_t = 0;

    // The one-second window (game time).
    double win_start = 0;
    uint32_t rx = 0, flag_clears = 0;
    double err_max = 0;
};
Avatar* g_av = nullptr;  // net::kMaxClients entries (see above)
int g_nclients = 0;
uint32_t g_newest_gen[net::kMaxClients] = {};  // per slot: the newest connection generation seen in a Connect event
constexpr double kHoldTimeout = 5.0;  // game seconds from the Welcome to the first State, before the avatar is released
uint32_t g_mask = 0;         // REMOTEOBJECT mask, read once
uint32_t g_epoch = 0;        // sim_player_epoch() when the current session started
uint32_t g_checked_gen = 0;  // g_aircraft_generation at the last liveness check
double g_next_report = 0;
unsigned g_send_fail = 0;  // messages the core's out queue refused since the last report

void set_clients(int n) {
    g_nclients = n;
    g_has_clients.store(n > 0, std::memory_order_relaxed);
}

// Printable ASCII only, for log lines and for the fields we keep.
void sanitize(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = (src[i] >= 0x20 && src[i] <= 0x7e) ? src[i] : '?';
    dst[i] = '\0';
}

void core_log(const char* line) { LOG("%s", line); }

// Hands a record to the writer thread: a copy into a ring, no I/O. A no-op while no stream is open.
void rec(net::StreamKind kind, double t, const void* payload, size_t len) { g_stream->record(kind, t, payload, len); }

// The host player's own state, as a State with client_id 0, once per player step.
void record_host_state(void* player, double now) {
    if (!g_stream->is_open()) return;
    net::StateMsg s{};
    double pos[3];
    float vel[3], ang[3], rates[3];
    read_at(player, kOffPos, pos, 3);
    read_at(player, kOffVel, vel, 3);
    read_at(player, kOffAngles, ang, 3);
    read_at(player, kOffRates, rates, 3);
    s.client_id = 0;
    s.seq = ++g_host_seq;
    s.game_time = now;
    for (int i = 0; i < 3; ++i) {
        s.pos[i] = pos[i];
        s.vel[i] = vel[i];
        s.ang[i] = ang[i];
        s.rates[i] = rates[i];
    }
    uint8_t buf[96];
    const size_t n = net::encode(s, buf, sizeof(buf));
    if (n) rec(net::StreamKind::HostState, now, buf, n);
}

void flush_send_fail() {
    if (!g_send_fail) return;
    LOG("net: %u message(s) to clients could not be queued for the network thread (out queue full)", g_send_fail);
    g_send_fail = 0;
}

// Queue helpers: a refusal (the out queue is full) is counted and reported once a second.
void send_bye(int slot, uint32_t gen, uint8_t reason) {
    net::ByeMsg b{};
    b.client_id = static_cast<uint16_t>(slot + 1);
    b.reason = reason;
    uint8_t buf[16];
    const size_t n = net::encode(b, buf, sizeof(buf));
    if (!n || !g_core->send(static_cast<uint8_t>(slot), gen, buf, n, net::kChannelReliable, true)) ++g_send_fail;
}
void request_disconnect(int slot, uint32_t gen) {
    if (!g_core->disconnect(static_cast<uint8_t>(slot), gen)) ++g_send_fail;  // the 5 s silence timeout ends it then
}

struct LiveCtx {
    void* obj[net::kMaxClients];
    unsigned long long seq[net::kMaxClients];
    bool live[net::kMaxClients] = {};
};
void live_cb(const LiveAircraft* list, size_t n, void* vctx) {  // under the shared lock: no logging
    auto& c = *static_cast<LiveCtx*>(vctx);
    for (size_t i = 0; i < n; ++i) {
        for (int s = 0; s < net::kMaxClients; ++s) {
            if (c.obj[s] && list[i].obj == c.obj[s] && list[i].seq == c.seq[s]) c.live[s] = true;
        }
    }
}
// Fills live[] for every active avatar from the probe's table.
void check_live(bool live[net::kMaxClients]) {
    LiveCtx c;
    for (int s = 0; s < net::kMaxClients; ++s) {
        c.obj[s] = g_av[s].active ? g_av[s].obj : nullptr;
        c.seq[s] = g_av[s].seq;
    }
    probe_with_aircraft(live_cb, &c);
    for (int s = 0; s < net::kMaxClients; ++s) live[s] = c.live[s];
}

// Frees a slot's avatar. If the aircraft is still live its flags are restored (the AI takes it back).
void release(int slot, const char* why, bool aircraft_live, uint8_t bye_reason, net::LeaveReason leave) {
    Avatar& a = g_av[slot];
    if (!a.active) return;
    if (aircraft_live) {
        uint32_t flags;
        read_at(a.obj, kOffFlags, &flags, 1);
        flags &= ~a.set_bits;  // clear only what we set
        write_at(a.obj, kOffFlags, &flags, 1);
    }
    if (bye_reason != kByeNone) {
        send_bye(slot, a.gen, bye_reason);
        request_disconnect(slot, a.gen);
    }
    g_core->set_welcomed(static_cast<uint8_t>(slot), a.gen, false);
    {
        net::ClientLeftPayload p{};
        p.client_id = static_cast<uint16_t>(slot + 1);
        p.reason = static_cast<uint8_t>(leave);
        rec(net::StreamKind::ClientLeft, g_now, &p, sizeof(p));
    }
    LOG("net: client %d \"%s\" left (%s); avatar %p %s", slot + 1, a.callsign, why, a.obj,
        aircraft_live ? "given back to the AI" : "is gone, flags not restored");
    a = Avatar{};
    set_clients(g_nclients - 1);
    if (g_nclients == 0) {
        sim_set_run_active(false);
        g_stream->close(false);  // the writer thread drains, flushes and closes the file
        g_core->reset_host_time();  // the session is over: no Pong from a stale clock
        flush_send_fail();
        LOG("net: no clients left");
    }
}

// Releases every avatar the probe no longer lists, and (for the rest) restores nothing: they are fine.
void release_dead() {
    const uint32_t gen = g_aircraft_generation.load(std::memory_order_acquire);  // before the table read
    bool live[net::kMaxClients];
    check_live(live);
    g_checked_gen = gen;
    for (int s = 0; s < net::kMaxClients; ++s) {
        if (g_av[s].active && !live[s]) release(s, "avatar destroyed", false, kByeAvatarLost, net::LeaveReason::HostEnded);
    }
}

struct PickCtx {
    // in
    void* taken_obj[net::kMaxClients];
    unsigned long long taken_seq[net::kMaxClients];
    uint32_t mask;
    double player_pos[3];
    double spawn[3];
    float heading;
    // out
    bool found = false;
    void* obj = nullptr;
    unsigned long long seq = 0;
    wchar_t name[64] = {};
    double distance = 0;
    double from[3] = {};
    uint32_t orig_flags = 0;
    float vel[3] = {};  // in: the player's velocity
};

void pick_cb(const LiveAircraft* list, size_t n, void* vctx) {  // under the shared lock: no logging
    auto& c = *static_cast<PickCtx*>(vctx);
    size_t best = n;
    double best_d = 0;
    for (size_t i = 0; i < n; ++i) {
        if (list[i].is_player) continue;
        bool taken = false;
        for (int s = 0; s < net::kMaxClients; ++s) {
            if (c.taken_obj[s] == list[i].obj && c.taken_seq[s] == list[i].seq) taken = true;
        }
        if (taken) continue;
        double p[3];
        read_at(list[i].obj, kOffPos, p, 3);
        const double d = dist3(p, c.player_pos);
        if (best == n || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    if (best == n) return;
    c.found = true;
    c.obj = list[best].obj;
    c.seq = list[best].seq;
    c.distance = best_d;
    read_at(c.obj, kOffPos, c.from, 3);
    wcsncpy_s(c.name, list[best].name, _TRUNCATE);
    // REMOTEOBJECT on, rates zero, at the spawn: the aircraft coasts and we are the only writer.
    char* obj = static_cast<char*>(c.obj);
    read_at(obj, kOffFlags, &c.orig_flags, 1);
    const float zero[3] = {};
    write_at(obj, kOffRates, zero, 3);
    write_at(obj, kOffVel, c.vel, 3);  // in formation with the player until the first State
    const uint32_t flags = c.orig_flags | c.mask;
    write_at(obj, kOffFlags, &flags, 1);
    write_at(obj, kOffPos, c.spawn, 3);
    const double a[3] = {0.0, static_cast<double>(c.heading), 0.0};
    const float af[3] = {0.0f, c.heading, 0.0f};
    float m[9];
    build_matrix(a, m);
    write_at(obj, kOffMatrix, m, 9);
    write_at(obj, kOffAngles, af, 3);
}

void reject(int slot, uint32_t gen, uint8_t reason, const char* why) {
    LOG("net: client %d refused: %s", slot + 1, why);
    send_bye(slot, gen, reason);
    request_disconnect(slot, gen);
    if (g_nclients == 0) flush_send_fail();
}

void handle_hello(int slot, uint32_t gen, const net::HelloMsg& h, void* player, double now) {
    if (g_puppet_state.load() != 0 || replay_active()) {
        reject(slot, gen, kByeBusy, "a puppet or replay run is active");
        return;
    }
    if (!g_mask) {
        if (const HMODULE m = GetModuleHandleW(L"AircraftObject.dll")) {  // the probe checked its build stamp
            std::memcpy(&g_mask, reinterpret_cast<const char*>(m) + kRemoteMaskRva, sizeof(g_mask));
        }
        if (!g_mask) {
            reject(slot, gen, kByeNoAvatar, "REMOTEOBJECT mask not set");
            return;
        }
    }
    PickCtx c;
    for (int s = 0; s < net::kMaxClients; ++s) {
        c.taken_obj[s] = g_av[s].active ? g_av[s].obj : nullptr;
        c.taken_seq[s] = g_av[s].seq;
    }
    c.mask = g_mask;
    float pang[3];
    read_at(player, kOffPos, c.player_pos, 3);
    read_at(player, kOffAngles, pang, 3);
    read_at(player, kOffVel, c.vel, 3);
    const double hp = pang[1], sh = std::sin(hp), ch = std::cos(hp);
    const double right = kSpawnRight * slot;
    c.spawn[0] = c.player_pos[0] + kSpawnAhead * sh + right * ch;
    c.spawn[1] = c.player_pos[1] + kSpawnAhead * ch - right * sh;
    c.spawn[2] = c.player_pos[2];
    c.heading = pang[1];
    const uint32_t air_gen = g_aircraft_generation.load(std::memory_order_acquire);  // before the table read
    probe_with_aircraft(pick_cb, &c);
    if (!c.found) {
        reject(slot, gen, kByeNoAvatar, "no free AI aircraft to use as an avatar");
        return;
    }
    if (g_nclients == 0) {
        g_checked_gen = air_gen;
        sim_set_run_active(true);
        g_next_report = now + 1.0;
        g_host_seq = 0;
        g_now = now;
        // Theatre name: not read; the header field stays empty (see docs/stream-format.md).
        // open() does one CreateFileW and may wait in join_previous() for an earlier writer that is still flushing:
        // bounded (a drain of at most 1 MB), and only at the start of a session.
        g_stream->open(probe_output_dir(), now, nullptr, core_log);
    }
    Avatar& a = g_av[slot];
    a = Avatar{};
    a.active = true;
    a.gen = gen;
    std::memcpy(a.hold_vel, c.vel, sizeof(a.hold_vel));
    a.hold_t0 = now;
    a.obj = c.obj;
    a.seq = c.seq;
    a.set_bits = c.mask & ~c.orig_flags;
    sanitize(a.callsign, sizeof(a.callsign), h.callsign);
    sanitize(a.aircraft, sizeof(a.aircraft), h.aircraft);
    std::memcpy(a.spawn, c.spawn, sizeof(a.spawn));
    a.spawn_heading = c.heading;
    a.win_start = now;
    set_clients(g_nclients + 1);

    {
        net::ClientJoinedPayload p{};
        p.client_id = static_cast<uint16_t>(slot + 1);
        std::memcpy(p.callsign, a.callsign, sizeof(p.callsign) - 1);  // sanitized and NUL-terminated by sanitize()
        std::memcpy(p.aircraft, a.aircraft, sizeof(p.aircraft) - 1);
        rec(net::StreamKind::ClientJoined, now, &p, sizeof(p));
    }
    // The slot may now pass State packets, and only then does the client learn it may send them.
    g_core->set_welcomed(static_cast<uint8_t>(slot), gen, true);
    net::WelcomeMsg w{};
    w.client_id = static_cast<uint16_t>(slot + 1);
    std::memcpy(w.spawn_pos, c.spawn, sizeof(w.spawn_pos));
    w.spawn_heading = c.heading;
    w.host_game_time = now;
    uint8_t buf[64];
    const size_t n = net::encode(w, buf, sizeof(buf));
    const bool welcomed = n && g_core->send(static_cast<uint8_t>(slot), gen, buf, n, net::kChannelReliable, true);
    if (!welcomed) {
        LOG("net: client %d: the Welcome could not be queued (out queue full); client refused", slot + 1);
        release(slot, "welcome not sent", true, kByeNone, net::LeaveReason::NoWelcome);
        request_disconnect(slot, gen);
        return;
    }
    rec(net::StreamKind::WelcomeSent, now, buf, n);
    LOG("net: client %d \"%s\" (%s) joined: avatar %p \"%ls\" (was %.0f m from the player at (%.0f, %.0f, %.0f)), "
        "spawn (%.0f, %.0f, %.0f) heading %.1f deg",
        slot + 1, a.callsign, a.aircraft, c.obj, c.name, c.distance, c.from[0], c.from[1], c.from[2], c.spawn[0],
        c.spawn[1], c.spawn[2], static_cast<double>(c.heading) * 180.0 / kPi);
}

// A checked State from the network thread. Keeps only samples that are newer than the newest, in both the
// sequence number and the time, and measures the packet interval for the delay.
void accept_state(Avatar& a, const net::StateMsg& s) {
    if (a.have_seq && static_cast<int32_t>(s.seq - a.last_seq) <= 0) return;
    if (a.ring_n + a.pending_n > 0 && !(s.game_time > a.newest_t)) return;
    if (a.ring_n + a.pending_n > 0) {
        const double iv = s.game_time - a.newest_t;
        if (iv < 2.0) {  // a longer gap is a stall, not a rate
            a.intervals[a.iv_pos] = iv;
            a.iv_pos = (a.iv_pos + 1) % net::kIntervalWindow;
            if (a.iv_n < net::kIntervalWindow) ++a.iv_n;
        }
    }
    a.have_seq = true;
    a.last_seq = s.seq;
    a.newest_t = s.game_time;
    Samp p;
    p.t = s.game_time;
    for (int i = 0; i < 3; ++i) {
        p.k.p[i] = s.pos[i];
        p.k.v[i] = s.vel[i];
        p.k.a[i] = wrap_pi(s.ang[i]);
        p.k.w[i] = s.rates[i];
    }
    if (a.pending_n == kPending) {
        std::memmove(&a.pending[0], &a.pending[1], sizeof(Samp) * (kPending - 1));
        --a.pending_n;
    }
    a.pending[a.pending_n++] = p;
    ++a.rx;
}

void push_ring(Avatar& a, const Samp& s) {
    if (a.ring_n == kRing) {
        std::memmove(&a.ring[0], &a.ring[1], sizeof(Samp) * (kRing - 1));
        --a.ring_n;
    }
    a.ring[a.ring_n++] = s;
}

// Before the first State: the spawn, moved on with the player's velocity at the time of the Hello (dead reckoning in
// formation), level, with zero rates.
void hold_kin(const Avatar& a, double now, Kin* out) {
    *out = Kin{};
    const double dt = now > a.hold_t0 ? now - a.hold_t0 : 0.0;
    for (int i = 0; i < 3; ++i) {
        out->p[i] = a.spawn[i] + static_cast<double>(a.hold_vel[i]) * dt;
        out->v[i] = a.hold_vel[i];
    }
    out->a[1] = a.spawn_heading;
}

void avatar_step(int slot, double now) {
    Avatar& a = g_av[slot];
    char* obj = static_cast<char*>(a.obj);

    // A client that was welcomed but never sends a State would leave the avatar coasting for ever: give it back.
    if (!a.have_seq && now - a.hold_t0 > kHoldTimeout) {
        LOG("net: client %d \"%s\": no State within %.0f s of the Welcome; releasing the avatar and disconnecting",
            slot + 1, a.callsign, kHoldTimeout);
        const uint32_t gen = a.gen;
        release(slot, "no State after the Welcome", true, kByeNone, net::LeaveReason::Timeout);
        request_disconnect(slot, gen);
        return;
    }

    // The game must not take the avatar back: keep REMOTEOBJECT set.
    uint32_t flags;
    read_at(obj, kOffFlags, &flags, 1);
    if ((flags & a.set_bits) != a.set_bits) {
        flags |= a.set_bits;
        write_at(obj, kOffFlags, &flags, 1);
        ++a.flag_clears;
    }

    // 1. The delay: towards 1.5 x the median packet interval, by at most 5% of the elapsed game time.
    double elapsed = a.have_last_now ? now - a.last_now : 0.0;
    if (!(elapsed > 0.0)) elapsed = 0.0;
    if (elapsed > kMaxStep) elapsed = kMaxStep;
    a.last_now = now;
    a.have_last_now = true;
    if (a.iv_n > 0) a.delay = net::ramp_delay(a.delay, net::delay_target(net::median(a.intervals, a.iv_n)), elapsed);
    const double t_render = now - a.delay;

    // 2. New samples: the state shown under the old ring, so that a change can be turned into an offset. With
    // an empty ring, that is the state we have been holding at the spawn.
    bool have_before = false;
    Kin before{};
    if (a.pending_n > 0) {
        if (a.ring_n > 0) {
            have_before = sample_at(a.ring, a.ring_n, t_render, &before);
        } else {
            hold_kin(a, now, &before);
            have_before = true;
        }
        for (int i = 0; i < a.pending_n; ++i) push_ring(a, a.pending[i]);
        a.pending_n = 0;
    }

    Kin shown{};
    if (a.ring_n == 0) {  // nothing received yet: coast from the spawn with the player's velocity
        hold_kin(a, now, &shown);
    } else {
        Kin raw;
        sample_at(a.ring, a.ring_n, t_render, &raw);
        // Correction blending, as in replay.cpp: carry what we showed before minus what we would show now.
        const double w_now = blend_weight(now - a.off_t, kBlendSeconds);
        bool jumped = false;
        if (have_before) {
            for (int i = 0; i < 3; ++i) {
                if (std::fabs(before.p[i] - raw.p[i]) >= 1e-6 || std::fabs(angle_diff(before.a[i], raw.a[i])) >= 1e-9)
                    jumped = true;
            }
        }
        if (jumped) {
            for (int i = 0; i < 3; ++i) {
                a.off_p[i] = a.off_p[i] * w_now + (before.p[i] - raw.p[i]);
                a.off_a[i] = wrap_pi(a.off_a[i] * w_now + angle_diff(before.a[i], raw.a[i]));
            }
            a.off_t = now;
        }
        const double w = blend_weight(now - a.off_t, kBlendSeconds);
        shown = raw;
        double off2 = 0;
        for (int i = 0; i < 3; ++i) {
            shown.p[i] += a.off_p[i] * w;
            shown.a[i] = wrap_pi(shown.a[i] + a.off_a[i] * w);
            off2 += (a.off_p[i] * w) * (a.off_p[i] * w);
        }
        a.err_max = std::fmax(a.err_max, std::sqrt(off2));
    }

    const float v[3] = {float(shown.v[0]), float(shown.v[1]), float(shown.v[2])};
    const float ang[3] = {float(shown.a[0]), float(shown.a[1]), float(shown.a[2])};
    const float rates[3] = {float(shown.w[0]), float(shown.w[1]), float(shown.w[2])};
    float m[9];
    build_matrix(shown.a, m);
    write_at(obj, kOffPos, shown.p, 3);
    write_at(obj, kOffVel, v, 3);
    write_at(obj, kOffRates, rates, 3);
    write_at(obj, kOffMatrix, m, 9);
    write_at(obj, kOffAngles, ang, 3);
}

void handle_message(const HostEvent& e, void* player, double now) {
    const int slot = e.slot;
    net::MsgType type{};
    if (net::peek_type(e.data, e.len, type) != net::Status::Ok) return;  // the network thread checked already
    switch (type) {
        case net::MsgType::Hello: {
            net::HelloMsg h;
            // A Hello of an older connection than the newest Connect seen for this slot is a leftover: ignore it.
            if (static_cast<int32_t>(e.gen - g_newest_gen[slot]) < 0) return;
            if (g_av[slot].active || net::decode(e.data, e.len, h) != net::Status::Ok) return;
            handle_hello(slot, e.gen, h, player, now);
            break;
        }
        case net::MsgType::State: {
            net::StateMsg s;
            if (g_av[slot].active && g_av[slot].gen == e.gen && net::decode(e.data, e.len, s) == net::Status::Ok) {
                rec(net::StreamKind::StateReceived, now, e.data, e.len);
                accept_state(g_av[slot], s);
            }
            break;
        }
        case net::MsgType::Bye: {
            if (!g_av[slot].active || g_av[slot].gen != e.gen) break;
            rec(net::StreamKind::ByeReceived, now, e.data, e.len);
            bool live[net::kMaxClients];
            check_live(live);
            release(slot, "bye", live[slot], kByeNone, net::LeaveReason::Left);
            request_disconnect(slot, e.gen);
            break;
        }
        default:
            break;
    }
}

void pump(void* player, double now) {
    g_core->set_host_time(now);
    HostEvent ev[32];
    for (int round = 0; round < 8; ++round) {
        const size_t n = g_core->poll(ev, 32);
        if (n == 0) break;
        for (size_t i = 0; i < n; ++i) {
            const int slot = ev[i].slot;
            if (slot >= net::kMaxClients) continue;
            // An event of a newer connection than the avatar's: this slot's Disconnect never reached us (it was
            // lost, or the avatar was never told). Give the old avatar back before handling the new event.
            if (g_av[slot].active && static_cast<int32_t>(ev[i].gen - g_av[slot].gen) > 0) {
                bool live[net::kMaxClients];
                check_live(live);
                release(slot, "disconnected (missed)", live[slot], kByeNone, net::LeaveReason::Left);
            }
            if (ev[i].kind == HostEvent::Connect && static_cast<int32_t>(ev[i].gen - g_newest_gen[slot]) > 0)
                g_newest_gen[slot] = ev[i].gen;
            switch (ev[i].kind) {
                case HostEvent::Connect:
                    LOG("net: peer %d connected", slot + 1);
                    break;
                case HostEvent::Disconnect:
                    if (g_av[slot].active && g_av[slot].gen == ev[i].gen) {
                        bool live[net::kMaxClients];
                        check_live(live);
                        release(slot, ev[i].reason == 1 ? "timeout, no packets for 5 s" : "disconnected", live[slot],
                                kByeNone, ev[i].reason == 1 ? net::LeaveReason::Timeout : net::LeaveReason::Left);
                    } else {
                        LOG("net: peer %d gone", slot + 1);
                    }
                    break;
                case HostEvent::Message:
                    handle_message(ev[i], player, now);
                    break;
            }
        }
    }
}

void report(void* player, double now) {
    double pp[3];
    read_at(player, kOffPos, pp, 3);
    for (int s = 0; s < net::kMaxClients; ++s) {
        Avatar& a = g_av[s];
        if (!a.active) continue;
        double ap[3];
        read_at(a.obj, kOffPos, ap, 3);
        const double span = now - a.win_start;
        LOG("net: client %d \"%s\" rate %.1f Hz delay %.0f ms rtt %.0f ms err %.2f m (avatar dist %.0f m)", s + 1,
            a.callsign, span > 0 ? a.rx / span : 0.0, a.delay * 1000.0, static_cast<double>(g_core->rtt_ms(static_cast<uint8_t>(s))),
            a.err_max, dist3(pp, ap));
        if (a.flag_clears) LOG("net: client %d: the game cleared REMOTEOBJECT %u time(s); set again", s + 1, a.flag_clears);
        a.win_start = now;
        a.rx = 0;
        a.flag_clears = 0;
        a.err_max = 0;
    }
    flush_send_fail();
}

void on_step(void* aircraft, float /*dt*/, bool is_player) {
    if (!g_running.load(std::memory_order_relaxed)) return;
    // The player changed (mission end, new flight): the session is over, and the published host time is stale.
    // Check liveness first, so that only aircraft that still exist get their flags written.
    if (sim_player_epoch() != g_epoch) {
        g_epoch = sim_player_epoch();
        if (g_nclients > 0) {
            bool live[net::kMaxClients];
            check_live(live);
            for (int s = 0; s < net::kMaxClients; ++s)
                if (g_av[s].active) release(s, "the player changed", live[s], kByeSessionEnded, net::LeaveReason::HostEnded);
        }
        g_core->reset_host_time();  // no Pongs, and no States passed, until the new player's first step
    } else if (g_nclients > 0 && g_aircraft_generation.load(std::memory_order_acquire) != g_checked_gen) {
        release_dead();  // an aircraft was destroyed since the last check: is one of ours gone?
    }
    const double now = sim_game_time();
    g_now = now;
    if (is_player) {
        pump(aircraft, now);
        if (g_nclients > 0) record_host_state(aircraft, now);
        if (g_nclients > 0 && now >= g_next_report) {
            report(aircraft, now);
            g_next_report = std::floor(now) + 1.0;
        }
    }
    if (g_nclients == 0) return;
    for (int s = 0; s < net::kMaxClients; ++s) {
        if (g_av[s].active && g_av[s].obj == aircraft) avatar_step(s, now);
    }
}

}  // namespace

void host_install() {
    wchar_t w[96] = {};
    const DWORD n = GetEnvironmentVariableW(L"LAZYSF2_HOST", w, 96);
    if (n == 0) {
        LOG("net: host off");
        return;
    }
    if (!probe_output_dir()) {
        LOG("net: host off (the probe is off: LAZYSF2_PROBE_DIR is not set or not valid)");
        return;
    }
    char spec[96] = {};
    bool ascii = n < 96;
    for (DWORD i = 0; ascii && i < n; ++i) {
        if (w[i] > 0x7e || w[i] < 0x20) ascii = false;
        spec[i] = static_cast<char>(w[i]);
    }
    if (!ascii || !net::parse_host_spec(spec, g_addr, sizeof(g_addr), &g_port)) {
        LOG("net: host off (LAZYSF2_HOST must be a port, or a numeric IPv4 address and a port: 127.0.0.1:7777)");
        return;
    }
    g_enabled = true;
}

void host_on_modules_loaded() {
    static std::atomic<bool> once{false};
    if (!g_enabled || once.exchange(true)) return;
    if (!sim_register_step_callback(on_step)) {
        LOG("net: host off (no free step callback slot)");
        return;
    }
    g_core = new (std::nothrow) net::HostCore;
    g_stream = new (std::nothrow) net::StreamWriter;
    g_av = new (std::nothrow) Avatar[net::kMaxClients];
    if (!g_core || !g_stream || !g_av) {
        LOG("net: host off (out of memory)");
        return;  // the step callback stays registered but does nothing: g_running is false
    }
    g_checked_gen = g_aircraft_generation.load(std::memory_order_acquire);
    g_epoch = sim_player_epoch();
    g_running.store(true);
    // Returns at once; the thread binds and logs "net: host on udp <addr>:<port>" (or why it could not).
    if (!g_core->start(g_addr, g_port, core_log)) {
        g_running.store(false);
        LOG("net: host off (could not start the network thread)");
    }
}

void host_shutdown() {
    if (!g_running.exchange(false)) return;
    g_core->stop(true);
    // Hand any avatar back to the AI, if the aircraft is still there. DLL unload only: nothing else runs.
    bool live[net::kMaxClients] = {};
    if (g_nclients > 0) check_live(live);
    for (int s = 0; s < net::kMaxClients; ++s)
        if (g_av[s].active) release(s, "host shut down", live[s], kByeNone, net::LeaveReason::HostEnded);
    // release() above ran on the DllMain thread, not the game thread: that breaks the writer's single-producer rule,
    // but only here, at FreeLibrary, when the game has stopped stepping.
    g_stream->close(true);  // waits at most 1 s for the writer to flush, then detaches it
}

bool host_has_clients() { return g_has_clients.load(std::memory_order_relaxed); }

}  // namespace LazyWorks::SF2Patch::hooks
