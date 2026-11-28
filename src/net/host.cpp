// Host mode (developer tool, part of the probe): remote clients drive AI aircraft as avatars. Inert unless
// LAZYSF2_HOST is set and the probe is on. Runs on the game thread; the network thread (host_core.cpp) owns ENet.
// The avatars themselves (taking, writing, giving back) are in hooks/remote_avatar.cpp, the smoothing in RemoteJet.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "hooks/hooks.h"
#include "hooks/remote_avatar.h"
#include "hooks/replay_math.h"
#include "hooks/terrain_export.h"
#include "log.h"
#include "net/host_core.h"
#include "net/host_session.h"
#include "net/protocol.h"
#include "net/receiver_math.h"
#include "net/remote_jet.h"
#include "net/stream_format.h"
#include "net/stream_record.h"
#include "net/world_proto.h"
#include "net/world_stream_logic.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

using namespace replay_math;
using net::HostEvent;

constexpr size_t kOffPos = 0x38, kOffVel = 0x50, kOffRates = 0xd0, kOffAngles = 0x100;
constexpr double kShownEvery = 0.1;  // RemoteShown records per client, host seconds (and every Snap)

// Bye reasons the host sends.
enum ByeReason : uint8_t { kByeNone = 0, kByeBusy = 1, kByeNoAvatar = 2, kByeSessionEnded = 3, kByeAvatarLost = 4 };

using avatar_logic::dist3;

// Set by host_install() (LAZYSF2_HOST, for the process) or by a lobby session (host_session_start/stop, menus only:
// no step runs then, and the network thread copies the address at its start).
std::atomic<bool> g_enabled{false};
bool g_from_env = false;
uint32_t g_lobby_hash = 0;  // a lobby session: the content hash (0: no lobby, the variables' flows)
net::AuthKey g_key;  // the session's key (PBKDF2 of LAZYSF2_PASSWORD or the lobby's password over a fresh salt)
char g_addr[48] = {};
uint16_t g_port = 0;
char g_callsign[17] = "HOST";  // LAZYSF2_CALLSIGN, printable ASCII; the wire field keeps 15 characters

// The big objects live on the heap, created in host_on_modules_loaded() only when host mode is on (as globals
// they would bloat the DLL's .data). Never freed: a destructor at DLL unload could join a thread under the loader lock.
net::HostCore* g_core = nullptr;
// The published mission (T5), kept here because it can come before the core exists.
std::mutex g_mission_mx;
std::vector<uint8_t> g_mission;  // under g_mission_mx
uint32_t g_mission_id = 0, g_mission_raw = 0;
net::SeatPlan g_seat_plan{};
std::atomic<bool> g_core_started{false};
net::HostPeers* g_peers = nullptr;  // who is welcomed, and the fan-out of States, PeerInfos and PeerLefts
net::StreamWriter* g_stream = nullptr;  // the recording of the session (stream_record.h): written by its own thread
double g_now = 0;            // host game time at the latest step, for records made outside a step
uint32_t g_host_seq = 0;  // the recording's host States, one per step
uint32_t g_send_seq = 0;  // the host States sent to the clients (20 Hz); never reset, so a receiver never sees it restart
net::SendTicker g_tick;   // when the next host State is due, in host time
unsigned g_tx_states = 0, g_relayed = 0;  // since the last report: host States queued, client States relayed (per recipient)
std::atomic<bool> g_running{false};      // the step callback is registered and the thread started
std::atomic<bool> g_has_clients{false};  // mirrors g_nclients for other threads (puppet runs on the render thread)

struct Avatar {
    bool active = false;
    AvatarHandle h;
    char callsign[17] = {};
    char aircraft[33] = {};
    uint32_t gen = 0;  // the core's connection generation for this slot; stale commands are dropped by the core
    double spawn[3] = {};
    double spawn_heading = 0;
    float hold_vel[3] = {};  // the player's velocity at the spawn: the avatar coasts with it until the first State
    double hold_t0 = 0;
    net::RemoteJet jet{net::JetParams{}};  // the receiver: Predict mode (plan 2.5)
    bool got_state = false;  // the jet accepted a State
    bool shown_state = false;  // the avatar has shown one (the join snap was logged)
    bool coasting = false;   // the jet said Coast: we stopped writing and REMOTEOBJECT coasts the aircraft
    // No State for kParkAfter while the peer stays connected (a paused client): the aircraft goes back to the AI and
    // h is empty; the next State takes one again with the same picker. The session stays up.
    bool parked = false;
    bool retake_failed = false;  // logged once per park
    double last_state_t = 0;     // host time of the newest accepted State
    double last_game_time = 0;   // that State's stamp
    double next_shown = 0;       // host time of the next RemoteShown record
    bool have_shown = false;
    double shown_p[3] = {};
    float shown_a[3] = {};
    double next_retake = 0;
    uint32_t flag_clears = 0;  // in the current one-second window
    int seat = -1;  // shared world: the member of the host's flight this client flies (the host assigns it)
};
Avatar* g_av = nullptr;  // net::kMaxClients entries (see above)
int g_nclients = 0;
net::AvatarStateCache g_avatar_states;  // everyone's latest jet state, for joiners (T7/T8)
// Each client's latest Loadout parts and DamageState, as received (peer set), for joiners: they resend only every 10 s.
struct JoinCache {
    uint8_t loadout[4][sizeof(net::LoadoutMsg)];
    uint16_t loadout_len[4];
    uint8_t damage[sizeof(net::DamageStateMsg)];
    uint16_t damage_len;
};
JoinCache g_join_cache[net::kMaxClients] = {};

void join_cache_note(int slot, const uint8_t* d, size_t n) {
    if (slot < 0 || slot >= net::kMaxClients) return;
    JoinCache& c = g_join_cache[slot];
    const uint8_t wt = net::peek_world_type(d, n);
    if (wt == static_cast<uint8_t>(net::WorldType::Loadout)) {
        net::LoadoutMsg m;
        if (net::decode(d, n, m) != net::Status::Ok || m.part >= 4 || n > sizeof(c.loadout[0])) return;
        if (m.part == 0)
            for (uint16_t& l : c.loadout_len) l = 0;  // a new loadout starts with part 0
        std::memcpy(c.loadout[m.part], d, n);
        c.loadout_len[m.part] = static_cast<uint16_t>(n);
    } else if (wt == static_cast<uint8_t>(net::WorldType::DamageState) && n <= sizeof(c.damage)) {
        std::memcpy(c.damage, d, n);
        c.damage_len = static_cast<uint16_t>(n);
    }
}
bool g_world_behind[net::kMaxClients] = {};  // a client whose world events could not be queued (ended after the tick)
uint32_t g_newest_gen[net::kMaxClients] = {};  // per slot: the newest connection generation seen in a Connect event
constexpr double kHoldTimeout = 5.0;  // game seconds from the Welcome to the first State, before the avatar is released
constexpr double kParkAfter = 10.0;   // game seconds without a State before a connected client's avatar is given back
uint32_t g_epoch = 0;        // sim_player_epoch() when the current session started
uint32_t g_checked_gen = 0;  // g_aircraft_generation at the last liveness check
double g_next_report = 0;
unsigned g_send_fail = 0;  // messages the core's out queue refused since the last report
// The frame-order check (R4), once per player. If the avatars step before the player, sim_game_time() at their step
// is the previous frame's: they are then shown at that time plus the player's latest dt.
FrameOrderCheck g_order;
bool g_order_shift = false;
float g_player_dt = 0;
double g_qpc_ms = 0;  // milliseconds per QPC tick

void set_clients(int n) {
    g_nclients = n;
    g_has_clients.store(n > 0, std::memory_order_relaxed);
}

void core_log(const char* line) { LOG("%s", line); }

// Hands a record to the writer thread: a copy into a ring, no I/O. A no-op while no stream is open.
void rec(net::StreamKind kind, double t, const void* payload, size_t len) { g_stream->record(kind, t, payload, len); }

void rec_avatar_event(int slot, net::AvatarEventType ev, double t) {
    const net::AvatarEventPayload p{static_cast<uint16_t>(slot + 1), static_cast<uint8_t>(ev)};
    rec(net::StreamKind::AvatarEvent, t, &p, sizeof(p));
}

// The host player's own state, as a State with client_id 0 (the source id of the host's jet).
net::StateMsg host_state(void* player, double now, uint32_t seq) {
    net::StateMsg s{};
    double pos[3];
    float vel[3], ang[3], rates[3];
    obj_read(player, kOffPos, pos, 3);
    obj_read(player, kOffVel, vel, 3);
    obj_read(player, kOffAngles, ang, 3);
    obj_read(player, kOffRates, rates, 3);
    s.client_id = 0;
    s.seq = seq;
    s.game_time = now;
    for (int i = 0; i < 3; ++i) {
        s.pos[i] = pos[i];
        s.vel[i] = vel[i];
        s.ang[i] = ang[i];
        s.rates[i] = rates[i];
    }
    return s;
}

// Once per player step, for the recording.
void record_host_state(void* player, double now) {
    if (!g_stream->is_open()) return;
    const net::StateMsg s = host_state(player, now, ++g_host_seq);
    uint8_t buf[net::kMaxMessage];
    const size_t n = net::encode(s, buf, sizeof(buf));
    if (n) rec(net::StreamKind::HostState, now, buf, n);
}

// At kSendHz of host time, to every welcomed client.
void send_host_state(void* player, double now) {
    const net::StateMsg s = host_state(player, now, ++g_send_seq);
    uint8_t buf[net::kMaxMessage];
    const size_t n = net::encode(s, buf, sizeof(buf));
    if (n) g_tx_states += static_cast<unsigned>(g_peers->send_state(buf, n, -1));
}

void flush_send_fail() {
    g_send_fail += g_peers->take_send_fail();
    if (!g_send_fail) return;
    LOG("net: %u message(s) to clients could not be queued for the network thread (out queue full)", g_send_fail);
    g_send_fail = 0;
}

// Queue helpers: a refusal (the out queue is full) is counted and reported once a second.
void send_bye(int slot, uint32_t gen, uint8_t reason) {
    net::ByeMsg b{};
    b.client_id = 0;  // the sender: the host (protocol.h)
    b.reason = reason;
    uint8_t buf[16];
    const size_t n = net::encode(b, buf, sizeof(buf));
    if (!n || !g_core->send(static_cast<uint8_t>(slot), gen, buf, n, net::kChannelReliable, true)) ++g_send_fail;
}
void request_disconnect(int slot, uint32_t gen) {
    if (!g_core->disconnect(static_cast<uint8_t>(slot), gen)) ++g_send_fail;  // the 5 s silence timeout ends it then
}

// The handles of the active avatars (empty for free slots), for the liveness check and the picker.
void handles(AvatarHandle out[net::kMaxClients]) {
    for (int s = 0; s < net::kMaxClients; ++s) out[s] = g_av[s].active ? g_av[s].h : AvatarHandle{};
}

// Fills live[] for every active avatar from the probe's table.
void check_live(bool live[net::kMaxClients]) {
    AvatarHandle h[net::kMaxClients];
    handles(h);
    avatar_check_live(h, net::kMaxClients, live);
}

// Frees a slot's avatar. If the aircraft is still live its flags are restored (the AI takes it back).
// announce false: no PeerLeft to the others (the host's session end, when every client gets a Bye of its own).
void release(int slot, const char* why, bool aircraft_live, uint8_t bye_reason, net::LeaveReason leave, bool announce = true) {
    Avatar& a = g_av[slot];
    if (!a.active) return;
    g_avatar_states.forget(slot + 1);
    g_join_cache[slot] = JoinCache{};
    world_avatar_forget(slot + 1);
    hits_forget(slot);
    handover_host_forget(slot);
    void* const obj = a.h.obj;
    avatar_release(a.h, aircraft_live);
    if (bye_reason != kByeNone) {
        send_bye(slot, a.gen, bye_reason);
        request_disconnect(slot, a.gen);
    }
    // The others hear of it (PeerLeft), and the slot's States are blocked.
    g_peers->leave(static_cast<uint8_t>(slot), a.gen,
                   leave == net::LeaveReason::Left      ? net::kPeerLeftLeft
                   : leave == net::LeaveReason::Timeout ? net::kPeerLeftTimeout
                                                        : net::kPeerLeftRemoved,
                   announce);
    g_core->set_welcomed(static_cast<uint8_t>(slot), a.gen, false);  // also when it was never welcomed
    {
        net::ClientLeftPayload p{};
        p.client_id = static_cast<uint16_t>(slot + 1);
        p.reason = static_cast<uint8_t>(leave);
        rec(net::StreamKind::ClientLeft, g_now, &p, sizeof(p));
    }
    LOG("net: client %d \"%s\" left (%s); avatar %p %s", slot + 1, a.callsign, why, obj,
        !obj ? "was already given back (no States)"
             : (aircraft_live ? "given back to the AI" : "is gone, flags not restored"));
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
        if (g_av[s].active && g_av[s].h.obj && !live[s]) release(s, "avatar destroyed", false, kByeAvatarLost, net::LeaveReason::HostEnded);
    }
}

net::RepeatLog g_refusals;  // a client retrying after a refusal is logged at most once a minute per callsign and reason

void reject(int slot, uint32_t gen, uint8_t reason, const char* why, const char* callsign) {
    unsigned more = 0;
    const uint32_t key = net::fnv1a(callsign) ^ (static_cast<uint32_t>(reason) * 0x9E3779B1u);
    if (g_refusals.should_log(key, static_cast<double>(GetTickCount64()) * 0.001, &more)) {
        if (more)
            LOG("net: client %d \"%s\" refused: %s (refused %u more time(s) in the last minute; logged at most once a "
                "minute)",
                slot + 1, callsign, why, more);
        else
            LOG("net: client %d \"%s\" refused: %s", slot + 1, callsign, why);
    }
    send_bye(slot, gen, reason);
    request_disconnect(slot, gen);
    if (g_nclients == 0) flush_send_fail();
}

// The player's aircraft type, from the probe's table (copied under its lock; nothing logs there).
void player_type_cb(const LiveAircraft* list, size_t count, void* ctx) {
    for (size_t i = 0; i < count; ++i) {
        if (list[i].is_player && list[i].type_name) {
            strncpy_s(static_cast<char*>(ctx), 33, list[i].type_name, _TRUNCATE);
            return;
        }
    }
}

bool world_send_to(int s, const uint8_t* data, size_t len, bool reliable);  // below, with the world tick

void handle_hello(int slot, uint32_t gen, const net::HelloMsg& h, void* player, double now) {
    char callsign[17], aircraft[33];
    avatar_logic::to_ascii(callsign, sizeof(callsign), h.callsign);
    avatar_logic::to_ascii(aircraft, sizeof(aircraft), h.aircraft);
    if (g_puppet_state.load() != 0 || replay_active()) {
        reject(slot, gen, kByeBusy, "a puppet or replay run is active", callsign);
        return;
    }
    std::string theatre;
    const uint32_t theatre_hash = terrain_theatre_id(theatre) ? net::fnv1a(theatre.c_str()) : 0;  // 0: unknown
    if (net::theatre_mismatch(theatre_hash, h.theatre_hash)) {
        char why[192];
        std::snprintf(why, sizeof(why), "it flies in another theatre (its hash %08x; the host is in %s, hash %08x)",
                      static_cast<unsigned>(h.theatre_hash), theatre.c_str(), static_cast<unsigned>(theatre_hash));
        reject(slot, gen, net::kByeWrongTheatre, why, callsign);
        return;
    }
    double ppos[3];
    float pang[3], pvel[3];
    obj_read(player, kOffPos, ppos, 3);
    obj_read(player, kOffAngles, pang, 3);
    obj_read(player, kOffVel, pvel, 3);
    // The join position on the host's wing (plan 2.7): the client moves its jet there, and its avatar holds there.
    double join[3];
    net::join_position(ppos, pang[1], slot, join);
    // Shared world: the client's seat, by the tag its mission fetch carried (never by slot number).
    const int seat = world_host_streaming() ? g_core->seat_for(h.world_tag) : -1;
    if (world_host_streaming() && seat < 0) {
        char why[160] = "it did not fetch the mission";
        if (h.world_tag) {
            char row[100];
            g_core->lobby_describe(h.world_tag, row, sizeof(row));
            std::snprintf(why, sizeof(why), "no free seat in the host's flight (%s)", row);
        }
        reject(slot, gen, kByeNoAvatar, why, callsign);
        return;
    }
    // Shared world: the seat aircraft's current state, before the take, for the client's takeover in place (task 3);
    // the avatar stays where the seat aircraft is instead of moving to the wing.
    void* const seat_obj = seat >= 0 ? world_member_object(seat) : nullptr;
    net::SeatStateMsg seat_state{};
    const bool have_seat_state = seat_obj && seat_state_capture(seat, seat_obj, &seat_state);
    if (have_seat_state && (seat_state.flags & net::kSeatAirborne)) {
        net::ObjPose sp{};
        net::dequantise(seat_state.pose, &sp);
        for (int i = 0; i < 3; ++i) {
            join[i] = sp.pos[i];
            pvel[i] = sp.vel[i];
        }
        pang[1] = sp.ang[1];
    }
    const AvatarPick pick{aircraft, {join[0], join[1], join[2]}, pang[1], {pvel[0], pvel[1], pvel[2]}, seat_obj};
    AvatarHandle taken[net::kMaxClients];
    handles(taken);
    const uint32_t air_gen = g_aircraft_generation.load(std::memory_order_acquire);  // before the table read
    AvatarHandle got;
    AvatarInfo info;
    char why[192];
    if (!avatar_take(pick, taken, net::kMaxClients, &got, why, sizeof(why), &info)) {
        reject(slot, gen, kByeNoAvatar, why, callsign);
        return;
    }
    if (g_nclients == 0) {
        g_checked_gen = air_gen;
        sim_set_run_active(true);
        g_next_report = now + 1.0;
        g_host_seq = 0;
        g_tick.reset();
        g_tx_states = g_relayed = 0;
        g_peers->take_superseded();
        g_now = now;
        char own_type[33] = {};
        probe_with_aircraft(player_type_cb, own_type);
        const net::SessionInfoPayload si =
            net::make_session_info(net::SessionRole::Host, 0, g_callsign, own_type[0] ? own_type : "unknown");
        // open() does one CreateFileW and may wait in join_previous() for an earlier writer that is still flushing:
        // bounded (a drain of at most 1 MB), and only at the start of a session.
        g_stream->open(probe_output_dir(), now, theatre_hash ? theatre.c_str() : nullptr, core_log,
                       net::kStreamMaxFileBytes, L"stream", &si);
    }
    Avatar& a = g_av[slot];
    a = Avatar{};  // a fresh RemoteJet: the Welcome resets the receiver
    a.active = true;
    a.gen = gen;
    std::memcpy(a.hold_vel, pick.vel, sizeof(a.hold_vel));
    a.hold_t0 = now;
    a.h = got;
    std::memcpy(a.callsign, callsign, sizeof(a.callsign));
    std::memcpy(a.aircraft, aircraft, sizeof(a.aircraft));
    std::memcpy(a.spawn, pick.place, sizeof(a.spawn));
    a.spawn_heading = pick.heading;
    a.seat = seat;
    set_clients(g_nclients + 1);

    {
        net::ClientJoinedPayload p{};
        p.client_id = static_cast<uint16_t>(slot + 1);
        std::memcpy(p.callsign, a.callsign, sizeof(p.callsign) - 1);  // printable and NUL-terminated by to_ascii()
        std::memcpy(p.aircraft, a.aircraft, sizeof(p.aircraft) - 1);
        rec(net::StreamKind::ClientJoined, now, &p, sizeof(p));
    }
    net::WelcomeMsg w{};
    w.client_id = static_cast<uint16_t>(slot + 1);
    std::memcpy(w.spawn_pos, a.spawn, sizeof(w.spawn_pos));
    w.spawn_heading = pick.heading;
    w.host_game_time = now;
    std::memcpy(w.spawn_vel, pick.vel, sizeof(w.spawn_vel));
    w.theatre_hash = theatre_hash;
    w.mission_id = world_host_streaming() ? g_core->mission_id() : 0;
    w.seat = seat >= 0 ? static_cast<uint8_t>(seat) : net::kNoSeat;
    w.host_seat = world_host_streaming() ? static_cast<uint8_t>(g_core->seat_plan().host_member) : net::kNoSeat;
    if (seat >= 0)
        LOG("net: client %d flies seat %d of the host's flight (the host flies %d), mission 0x%08lx", slot + 1, seat,
            static_cast<int>(w.host_seat), static_cast<unsigned long>(w.mission_id));
    char host_type[33] = {};
    probe_with_aircraft(player_type_cb, host_type);
    g_peers->set_host(g_callsign, host_type);
    // Lets the slot's States through, then sends the Welcome, the PeerInfos (the host, the other clients) and this
    // client's PeerInfo to the others.
    if (!g_peers->welcome(static_cast<uint8_t>(slot), gen, w, a.callsign, a.aircraft)) {
        LOG("net: client %d: the Welcome could not be queued (out queue full); client refused", slot + 1);
        release(slot, "welcome not sent", true, kByeNone, net::LeaveReason::NoWelcome);
        request_disconnect(slot, gen);
        return;
    }
    uint8_t buf[net::kMaxMessage];
    const size_t n = net::encode(w, buf, sizeof(buf));
    if (n) rec(net::StreamKind::WelcomeSent, now, buf, n);
    // A late joiner (or a reconnect) gets the world's events so far, after its Welcome on the same channel (T8).
    if (world_host_streaming()) {
        world_host_snapshot(slot, w.mission_id, world_send_to);
        // Then everyone's current jet state (the host's and the other clients'), as if it had just changed.
        net::AvatarStateMsg st[net::AvatarStateCache::kPeers];
        const size_t ns = g_avatar_states.for_joiner(slot + 1, st);
        for (size_t i = 0; i < ns; ++i) {
            uint8_t ab[sizeof(net::AvatarStateMsg)];
            world_send_to(slot, ab, net::encode(st[i], ab, sizeof(ab)), true);
        }
        g_avatar_states.forget(slot + 1);  // its own comes from it again
        // The other clients' loadouts and damage as last received, and ours again (to everyone) at the next tick.
        int nl = 0, nd = 0;
        for (int s = 0; s < net::kMaxClients; ++s) {
            if (s == slot || !g_av[s].active) continue;
            const JoinCache& c = g_join_cache[s];
            for (int p = 0; p < 4; ++p)
                if (c.loadout_len[p] && world_send_to(slot, c.loadout[p], c.loadout_len[p], true)) ++nl;
            if (c.damage_len && world_send_to(slot, c.damage, c.damage_len, true)) ++nd;
        }
        world_avatar_resend();
        stores_resend();
        damage_resend();
        LOG("net: client %d: sent it %zu jet state(s), %d loadout part(s) and %d damage state(s) of the others; ours go "
            "out again now", slot + 1, ns, nl, nd);
        if (have_seat_state) seat_state_send(slot, seat_state);
    }
    LOG("net: client %d \"%s\" (%s) joined: avatar %p %s, spawn (%.0f, %.0f, %.0f) heading %.1f deg, theatre %s; "
        "host \"%s\" (%s)",
        slot + 1, a.callsign, a.aircraft, got.obj, why, a.spawn[0], a.spawn[1], a.spawn[2],
        static_cast<double>(pick.heading) * 180.0 / kPi, theatre_hash ? theatre.c_str() : "unknown", g_callsign,
        host_type[0] ? host_type : "type unknown");
    if (!avatar_logic::same_type(info.type, a.aircraft)) {
        LOG("net: client %d: warning: the client flies \"%s\" but its avatar is \"%s\"; both players should fly the "
            "same type",
            slot + 1, a.aircraft, info.type[0] ? info.type : "(type unknown)");
    }
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

// RemoteShown at kShownEvery, and at every Snap. t is the time the avatar was shown for.
void record_shown(int slot, net::RemoteJet::Out out, double t) {
    Avatar& a = g_av[slot];
    if (!a.have_shown || !g_stream->is_open() || (t < a.next_shown && out != net::RemoteJet::Out::Snap)) return;
    a.next_shown = t + kShownEvery;
    net::RemoteShownPayload p{};
    p.id = static_cast<uint16_t>(slot + 1);
    std::memcpy(p.pos, a.shown_p, sizeof(p.pos));
    std::memcpy(p.ang, a.shown_a, sizeof(p.ang));
    p.age = static_cast<float>(t - a.last_game_time);
    p.out = static_cast<uint8_t>(out);
    rec(net::StreamKind::RemoteShown, t, &p, sizeof(p));
}

void avatar_step(int slot, double now) {
    Avatar& a = g_av[slot];

    // A client that was welcomed but never sends a State would leave the avatar coasting for ever: give it back.
    if (!a.got_state && now - a.hold_t0 > kHoldTimeout) {
        LOG("net: client %d \"%s\": no State within %.0f s of the Welcome; releasing the avatar and disconnecting",
            slot + 1, a.callsign, kHoldTimeout);
        const uint32_t gen = a.gen;
        release(slot, "no State after the Welcome", true, kByeNone, net::LeaveReason::Timeout);
        request_disconnect(slot, gen);
        return;
    }

    // A connected client that has sent nothing for a long time (paused, or in the background): give the aircraft
    // back to the AI until its States resume. In its own step, so it is live.
    if (a.got_state && now - a.last_state_t > kParkAfter) {
        LOG("net: client %d \"%s\": no State for %.0f s; avatar %p given back to the AI until States resume",
            slot + 1, a.callsign, kParkAfter, a.h.obj);
        avatar_release(a.h, true);
        a.parked = true;
        a.retake_failed = false;
        a.coasting = false;
        a.have_shown = false;
        a.next_retake = 0;
        rec_avatar_event(slot, net::AvatarEventType::Parked, now);
        return;
    }

    // The game must not take the avatar back: keep REMOTEOBJECT set (also while coasting).
    if (avatar_keep_remote(a.h)) ++a.flag_clears;
    world_avatar_refresh(slot + 1, a.h.obj);

    const double t = g_order_shift ? now + static_cast<double>(g_player_dt) : now;
    Kin k{};
    const net::RemoteJet::Out out = a.jet.shown(t, &k);
    if (out == net::RemoteJet::Out::Coast) {
        if (!a.coasting) {
            a.coasting = true;
            avatar_zero_rates(a.h);  // coast straight on, attitude held, not rolling at the last State's rates
            LOG("net: client %d \"%s\": no State for over %.0f s; the avatar coasts on its velocity, rates zeroed", slot + 1,
                a.callsign, a.jet.params().coast_after);
        }
        record_shown(slot, out, t);  // the last shown position: REMOTEOBJECT coasts it natively
        return;
    }
    if (out == net::RemoteJet::Out::None) {  // nothing received yet: coast from the spawn with the player's velocity
        hold_kin(a, now, &k);
    } else {
        if (a.coasting) {
            a.coasting = false;
            LOG("net: client %d \"%s\": States again; the avatar is driven", slot + 1, a.callsign);
        }
        if (!a.shown_state) {
            a.shown_state = true;
            Kin held;
            hold_kin(a, now, &held);
            LOG("net: client %d \"%s\": first State shown (%s, %.0f m from the hold position)", slot + 1, a.callsign,
                out == net::RemoteJet::Out::Snap ? "snap" : "blend", dist3(held.p, k.p));
        }
    }
    avatar_write(a.h, k);
    if (out != net::RemoteJet::Out::None) {
        for (int i = 0; i < 3; ++i) {
            a.shown_p[i] = k.p[i];
            a.shown_a[i] = static_cast<float>(k.a[i]);
        }
        a.have_shown = true;
        record_shown(slot, out, t);
    }
}

// A parked slot's States resumed: take an aircraft again (same picker), placed on the State, and start the receiver
// afresh so the State snaps. At most one try a second; false while it has no aircraft.
bool retake(int slot, const net::StateMsg& s, double now) {
    Avatar& a = g_av[slot];
    if (now < a.next_retake) return false;
    a.next_retake = now + 1.0;
    const AvatarPick pick{a.aircraft, {s.pos[0], s.pos[1], s.pos[2]}, s.ang[1], {s.vel[0], s.vel[1], s.vel[2]}};
    AvatarHandle taken[net::kMaxClients];
    handles(taken);
    AvatarHandle got;
    char why[192];
    if (!avatar_take(pick, taken, net::kMaxClients, &got, why, sizeof(why))) {
        if (!a.retake_failed) LOG("net: client %d \"%s\": States again, but no avatar yet: %s", slot + 1, a.callsign, why);
        a.retake_failed = true;
        return false;
    }
    a.h = got;
    a.parked = false;
    a.jet.reset();
    a.shown_state = false;
    a.have_shown = false;
    rec_avatar_event(slot, net::AvatarEventType::Taken, now);
    LOG("net: client %d \"%s\": States again; avatar %p %s", slot + 1, a.callsign, got.obj, why);
    return true;
}

void handle_message(const HostEvent& e, void* player, double now) {
    const int slot = e.slot;
    if (net::peek_world_type(e.data, e.len) == static_cast<uint8_t>(net::WorldType::AvatarState)) {
        // A client's jet state (T7): onto its avatar here, and on to the other clients.
        if (slot < net::kMaxClients && g_av[slot].active && g_av[slot].gen == e.gen) {
            net::AvatarStateMsg m;
            if (net::decode(e.data, e.len, m) == net::Status::Ok) {
                g_avatar_states.set(m);
                hits_note_guns(slot, m.guns);
            }
            if (!g_av[slot].parked) world_avatar_apply(e.data, e.len, g_av[slot].h.obj);  // parked: the AI flies it
            for (int s = 0; s < net::kMaxClients; ++s)
                if (s != slot && g_av[s].active && !g_core->send_world(static_cast<uint8_t>(s), g_av[s].gen, e.data, e.len, true))
                    g_world_behind[s] = true;
        }
        return;
    }
    if (net::peek_world_type(e.data, e.len) == static_cast<uint8_t>(net::WorldType::JetAnim)) {
        // A client's jet inputs: onto its avatar here (not while parked: the AI flies it), and on to the other clients.
        if (slot < net::kMaxClients && g_av[slot].active && g_av[slot].gen == e.gen) {
            world_anim_apply(e.data, e.len, g_av[slot].parked ? nullptr : g_av[slot].h.obj);
            for (int s = 0; s < net::kMaxClients; ++s)
                if (s != slot && g_av[s].active) g_core->send_world(static_cast<uint8_t>(s), g_av[s].gen, e.data, e.len, false);
        }
        return;
    }
    {
        const uint8_t wt = net::peek_world_type(e.data, e.len);
        if (wt == static_cast<uint8_t>(net::WorldType::Loadout) || wt == static_cast<uint8_t>(net::WorldType::Consumables) ||
            wt == static_cast<uint8_t>(net::WorldType::DamageState) || wt == static_cast<uint8_t>(net::WorldType::Countermeasure)) {
            // A client's stores or consumables: onto its avatar here, and on to the other clients.
            if (slot < net::kMaxClients && g_av[slot].active && g_av[slot].gen == e.gen) {
                const bool reliable = wt != static_cast<uint8_t>(net::WorldType::Consumables);
                join_cache_note(slot, e.data, e.len);
                if (!g_av[slot].parked) {
                    if (wt <= static_cast<uint8_t>(net::WorldType::Consumables)) stores_apply(e.data, e.len, g_av[slot].h.obj);
                    else damage_apply(e.data, e.len, g_av[slot].h.obj);
                }
                for (int s = 0; s < net::kMaxClients; ++s)
                    if (s != slot && g_av[s].active &&
                        !g_core->send_world(static_cast<uint8_t>(s), g_av[s].gen, e.data, e.len, reliable) && reliable)
                        g_world_behind[s] = true;
            }
            return;
        }
    }
    if (net::peek_world_type(e.data, e.len) == static_cast<uint8_t>(net::WorldType::WeaponLaunch)) {
        // A client's weapon release: replayed here, and on to the other clients.
        if (slot < net::kMaxClients && g_av[slot].active && g_av[slot].gen == e.gen) {
            net::WeaponLaunchMsg wl;
            if (net::decode(e.data, e.len, wl) == net::Status::Ok) hits_note_launch(slot, wl.weapon_id);
            weapons_apply_launch(e.data, e.len, now);
            for (int s = 0; s < net::kMaxClients; ++s)
                if (s != slot && g_av[s].active && !g_core->send_world(static_cast<uint8_t>(s), g_av[s].gen, e.data, e.len, true))
                    g_world_behind[s] = true;
        }
        return;
    }
    if (net::peek_world_type(e.data, e.len) == static_cast<uint8_t>(net::WorldType::HandoverBegin) ||
        net::peek_world_type(e.data, e.len) == static_cast<uint8_t>(net::WorldType::HandoverChunk)) {
        if (slot < net::kMaxClients && g_av[slot].active && g_av[slot].gen == e.gen)
            handover_host_message(slot, e.data, e.len, g_core->rtt_ms(static_cast<uint8_t>(slot)) / 1000.0);
        return;
    }
    if (net::peek_world_type(e.data, e.len) == static_cast<uint8_t>(net::WorldType::HitClaim)) {
        if (slot < net::kMaxClients && g_av[slot].active && g_av[slot].gen == e.gen) hits_host_claim(slot, e.data, e.len);
        return;
    }
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
                // Relayed as it came (D10), but only the newest per slot and step: see the end of pump().
                g_peers->relay(static_cast<uint8_t>(slot), e.data, e.len);
                if (g_av[slot].parked && !retake(slot, s, now)) break;
                // now is the drain time (this player step), which stands for the arrival time: up to a frame later,
                // so the ages in the 1 Hz line include up to a frame of queue wait. Predict shows at the step's now.
                if (g_av[slot].jet.push(s, now)) {
                    g_av[slot].got_state = true;
                    g_av[slot].last_state_t = now;
                    g_av[slot].last_game_time = s.game_time;
                }
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

// Connections without a Hello (mission fetch rounds, about one a second per waiting pilot) and their ends: logged
// once a minute with a count, not each.
void quiet_peer_log(int slot, bool connected) {
    static ULONGLONG last = 0;
    static unsigned conns = 0, gone = 0;
    (connected ? conns : gone) += 1;
    const ULONGLONG now = GetTickCount64();
    if (last && now - last < 60000) return;
    LOG("net: peer %d %s (%u connections and %u ends without a Hello since the last line; logged once a minute)",
        slot + 1, connected ? "connected" : "gone", conns, gone);
    last = now;
    conns = gone = 0;
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
                    quiet_peer_log(slot, true);
                    break;
                case HostEvent::Disconnect:
                    if (g_av[slot].active && g_av[slot].gen == ev[i].gen) {
                        bool live[net::kMaxClients];
                        check_live(live);
                        release(slot, ev[i].reason == 1 ? "timeout, no packets for 5 s" : "disconnected", live[slot],
                                kByeNone, ev[i].reason == 1 ? net::LeaveReason::Timeout : net::LeaveReason::Left);
                    } else {
                        quiet_peer_log(slot, false);
                    }
                    break;
                case HostEvent::Message:
                    handle_message(ev[i], player, now);
                    break;
            }
        }
    }
    // One State per source and step at most, whatever backlog a hitch left: the receivers only want the newest.
    g_relayed += static_cast<unsigned>(g_peers->flush_relay());
}

void report(void* player, double /*now*/) {
    double pp[3];
    obj_read(player, kOffPos, pp, 3);
    LOG("net: host jet: %u State(s) sent to %d client(s), %u relayed (%u older ones not relayed)", g_tx_states,
        g_peers->count(), g_relayed, g_peers->take_superseded());
    g_tx_states = g_relayed = 0;
    for (int s = 0; s < net::kMaxClients; ++s) {
        Avatar& a = g_av[s];
        if (!a.active) continue;
        if (!a.h.obj) {
            LOG("net: client %d \"%s\" no avatar (no State for %.0f s) rtt %.0f ms", s + 1, a.callsign,
                g_now - a.last_state_t, static_cast<double>(g_core->rtt_ms(static_cast<uint8_t>(s))));
            continue;
        }
        double ap[3];
        obj_read(a.h.obj, kOffPos, ap, 3);
        const net::JetStats st = a.jet.take_stats();
        // "err" is the largest correction offset (the shown path minus the new prediction); every Snap is counted.
        // Ages are host time at the drain minus the State's stamp. They print as at least 0: on loopback the client's
        // estimate of host time can lead the host by about a millisecond.
        LOG("net: client %d \"%s\" rate %.1f Hz delay %.0f ms rtt %.0f ms err %.2f m (avatar dist %.0f m) "
            "age %.0f/%.0f ms horizon %.0f ms snaps %u coast %u accel %.1f g",
            s + 1, a.callsign, st.rate_hz, st.delay * 1000.0,
            static_cast<double>(g_core->rtt_ms(static_cast<uint8_t>(s))), st.offset_max, dist3(pp, ap),
            std::fmax(0.0, st.age_p50) * 1000.0, std::fmax(0.0, st.age_p90) * 1000.0, st.horizon_max * 1000.0, st.snaps + st.resync_snaps,
            st.coasting_steps, st.accel_g_max);
        if (st.dup_or_old || st.restarts || st.resyncs) {
            LOG("net: client %d: %u old or duplicate State(s), %u source restart(s), %u clock resync(s)", s + 1,
                st.dup_or_old, st.restarts, st.resyncs);
        }
        if (a.flag_clears) LOG("net: client %d: the game cleared REMOTEOBJECT %u time(s); set again", s + 1, a.flag_clears);
        a.flag_clears = 0;
    }
    flush_send_fail();
}

// The frame-order check, fed with every aircraft step while a session runs, until it has a verdict.
void check_order(void* aircraft, bool is_player) {
    if (g_order.done()) return;
    bool is_avatar = false;
    for (int s = 0; s < net::kMaxClients; ++s) is_avatar = is_avatar || (g_av[s].active && avatar_is(g_av[s].h, aircraft));
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!g_order.step(static_cast<double>(q.QuadPart) * g_qpc_ms, is_player, is_avatar)) return;
    g_order_shift = g_order.avatar_first();
    LOG("net: frame order: the player stepped before the avatars in %d of %d frames; %s", g_order.player_first(),
        g_order.frames(),
        g_order_shift ? "avatars are shown at the game time plus the player's step" : "the game time is current");
}

bool world_send_to(int s, const uint8_t* data, size_t len, bool reliable) {
    if (s < 0 || s >= net::kMaxClients || !g_av[s].active) return false;
    if (g_core->send_world(static_cast<uint8_t>(s), g_av[s].gen, data, len, reliable)) return true;
    if (reliable) g_world_behind[s] = true;
    return false;
}

bool world_send(const uint8_t* data, size_t len, bool reliable) {
    if (g_nclients == 0) return true;  // flying alone: the tracker still runs, nobody to send to
    bool any = false;
    for (int s = 0; s < net::kMaxClients; ++s) {
        if (!g_av[s].active) continue;
        if (g_core->send_world(static_cast<uint8_t>(s), g_av[s].gen, data, len, reliable)) any = true;
        else if (reliable) g_world_behind[s] = true;  // an event lost would make its world diverge
    }
    return any;
}

void world_tick(void* player, double now) {
    double pl[1 + net::kMaxClients][3];
    int np = 0;
    obj_read(player, kOffPos, pl[np++], 3);
    for (int s = 0; s < net::kMaxClients; ++s)
        if (g_av[s].active && g_av[s].h.obj) obj_read(g_av[s].h.obj, kOffPos, pl[np++], 3);
    world_host_tick(now, pl, np, world_send);
    world_avatar_tick(player, 0, [](const uint8_t* d, size_t n) {
        net::AvatarStateMsg m;
        if (net::decode(d, n, m) == net::Status::Ok) g_avatar_states.set(m);
        return g_nclients == 0 || world_send(d, n, true);  // alone: kept for the first joiner
    });
    if (g_nclients > 0) {
        world_anim_tick(player, 0, [](const uint8_t* d, size_t n) { return world_send(d, n, false); });
        stores_tick(player);
        damage_tick(player);
        mission_clock_tick();
    }
    for (int s = 0; s < net::kMaxClients; ++s) {
        if (!g_world_behind[s]) continue;
        g_world_behind[s] = false;
        if (!g_av[s].active) continue;
        bool live[net::kMaxClients];
        check_live(live);
        const uint32_t gen = g_av[s].gen;
        release(s, "its world events could not be queued", live[s], net::kByeWorldBehind, net::LeaveReason::HostEnded);
        request_disconnect(s, gen);
    }
}

void on_step(void* aircraft, float dt, bool is_player) {
    if (!g_running.load(std::memory_order_relaxed)) return;
    // The player changed (mission end, new flight): the session is over, and the published host time is stale.
    // Check liveness first, so that only aircraft that still exist get their flags written.
    if (sim_player_epoch() != g_epoch) {
        g_epoch = sim_player_epoch();
        if (g_nclients > 0) {
            bool live[net::kMaxClients];
            check_live(live);
            for (int s = 0; s < net::kMaxClients; ++s)
                if (g_av[s].active) release(s, "the player changed", live[s], kByeSessionEnded, net::LeaveReason::HostEnded, false);
        }
        g_core->reset_host_time();  // no Pongs, and no States passed, until the new player's first step
        g_order.reset();  // check the order again for the new player
        g_order_shift = false;
    } else if (g_nclients > 0 && g_aircraft_generation.load(std::memory_order_acquire) != g_checked_gen) {
        release_dead();  // an aircraft was destroyed since the last check: is one of ours gone?
    }
    const double now = sim_game_time();
    g_now = now;
    if (is_player) {
        g_player_dt = dt;
        pump(aircraft, now);
        if (g_nclients > 0) record_host_state(aircraft, now);
        if (g_nclients > 0 && g_tick.due(now)) send_host_state(aircraft, now);
        // From the mission's start, clients or not: the event tracker must see every death for a later snapshot.
        if (world_host_streaming()) world_tick(aircraft, now);
        if (g_nclients > 0 && now >= g_next_report) {
            report(aircraft, now);
            g_next_report = std::floor(now) + 1.0;
        }
    }
    if (g_nclients == 0) return;
    check_order(aircraft, is_player);
    for (int s = 0; s < net::kMaxClients; ++s) {
        if (g_av[s].active && avatar_is(g_av[s].h, aircraft)) avatar_step(s, now);
    }
}

}  // namespace

// LAZYSF2_PASSWORD as UTF-8 (the developer flows' session password), or empty.
static std::string env_password() {
    wchar_t w[128] = {};
    const DWORD n = GetEnvironmentVariableW(L"LAZYSF2_PASSWORD", w, 128);
    if (n == 0 || n >= 128) return {};
    char u[512] = {};
    const int m = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), u, sizeof(u) - 1, nullptr, nullptr);
    std::string s = m > 0 ? std::string(u, static_cast<size_t>(m)) : std::string();
    net::auth_wipe(w, sizeof(w));
    net::auth_wipe(u, sizeof(u));
    return s;
}

void host_install() {
    wchar_t w[96] = {};
    const DWORD n = GetEnvironmentVariableW(L"LAZYSF2_HOST", w, 96);
    if (n == 0) {
        LOG("net: host off");
        return;
    }
    if (env_set(L"LAZYSF2_CONNECT")) {
        LOG("net: host and client off (LAZYSF2_HOST and LAZYSF2_CONNECT are both set; set one)");
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
    wchar_t cs[32] = {};
    const DWORD nc = GetEnvironmentVariableW(L"LAZYSF2_CALLSIGN", cs, 32);
    if (nc > 0 && nc < 32) avatar_logic::to_ascii(g_callsign, sizeof(g_callsign), cs);
    if (!g_callsign[0]) std::snprintf(g_callsign, sizeof(g_callsign), "HOST");
    std::string password = env_password();
    const bool keyed = net::make_host_key(password, &g_key);
    net::auth_wipe(password);
    if (!keyed) {
        LOG("net: host off (a session password is required: set LAZYSF2_PASSWORD)");
        return;
    }
    g_from_env = true;
    g_enabled = true;
}

namespace {

// Creates the per-session objects (a fresh core each session: its queues and mission start empty) and registers
// the step callback (once; it does nothing while g_running is false).
bool make_objects() {
    if (!sim_register_step_callback(on_step)) {
        LOG("net: host off (no free step callback slot)");
        return false;
    }
    if (!g_stream) g_stream = new (std::nothrow) net::StreamWriter;
    if (!g_av) g_av = new (std::nothrow) Avatar[net::kMaxClients];
    delete g_peers;
    delete g_core;
    g_core = new (std::nothrow) net::HostCore;
    g_peers = g_core ? new (std::nothrow) net::HostPeers(*g_core) : nullptr;
    if (!g_core || !g_peers || !g_stream || !g_av) {
        LOG("net: host off (out of memory)");
        return false;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_ms = 1000.0 / static_cast<double>(f.QuadPart);
    return true;
}

bool start_running() {
    if (!make_objects()) return false;
    for (int s = 0; s < net::kMaxClients; ++s) g_av[s] = Avatar{};
    set_clients(0);
    g_avatar_states.clear();
    for (auto& c : g_join_cache) c = JoinCache{};
    std::memset(g_world_behind, 0, sizeof(g_world_behind));
    std::memset(g_newest_gen, 0, sizeof(g_newest_gen));
    g_order.reset();
    g_order_shift = false;
    g_checked_gen = g_aircraft_generation.load(std::memory_order_acquire);
    g_epoch = sim_player_epoch();
    g_running.store(true);
    if (g_lobby_hash) {
        g_core->enable_lobby(g_lobby_hash);
        g_core->lobby_host(g_callsign, -1);
    }
    // Returns at once; the thread binds and logs "net: host on udp <addr>:<port>" (or why it could not).
    if (!g_core->start(g_addr, g_port, core_log, g_key)) {
        g_running.store(false);
        LOG("net: host off (could not start the network thread)");
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mission_mx);
    g_core_started.store(true);
    if (!g_mission.empty()) g_core->set_mission(g_mission, g_mission_id, g_mission_raw, g_seat_plan);
    return true;
}

}  // namespace

void host_on_modules_loaded() {
    static std::atomic<bool> once{false};
    if (!g_from_env || once.exchange(true)) return;
    start_running();
}

bool host_configured() { return g_enabled.load(); }

bool host_lobby_inventories(std::vector<std::vector<net::InventoryEntry>>* complete, std::vector<std::string>* waiting) {
    if (!g_running.load() || !g_core || !g_lobby_hash) return false;
    g_core->lobby_inventories(complete, waiting);
    return true;
}

bool host_lobby_view(net::HostCore::LobbyView* out) {
    if (!g_running.load() || !g_core || !g_lobby_hash) return false;
    *out = g_core->lobby_view();
    return true;
}

uint8_t host_lobby_seat(int want_seat) {
    if (!g_running.load() || !g_core || !g_lobby_hash) return net::kLobbyFull;
    const uint8_t r = g_core->lobby_host(g_callsign, want_seat);
    world_set_own_seat(g_core->lobby_view().host_seat);
    return r;
}

void host_lobby_info(const char* const lines[3]) {
    if (g_running.load() && g_core && g_lobby_hash) g_core->set_lobby_info(lines);
}

int host_lobby_seat_for(int members) {
    if (!g_running.load() || !g_core || !g_lobby_hash) return -2;
    return g_core->lobby_host_seat_for(members);
}

void host_lobby_launch(uint32_t launch_seq) {
    if (g_running.load() && g_core && g_lobby_hash) g_core->set_lobby_launch(launch_seq);
}

bool host_session_start(const char* addr, uint16_t port, const char* callsign, const std::string& password,
                        uint32_t content_hash, const char** why) {
    if (g_from_env) {
        *why = "LAZYSF2_HOST is set for this launch";
        return false;
    }
    if (g_enabled.load() || g_running.load()) {
        *why = "a host session is already running";
        return false;
    }
    if (!net::make_host_key(password, &g_key)) {
        *why = "a session password is required";
        return false;
    }
    std::snprintf(g_addr, sizeof(g_addr), "%s", addr);
    g_port = port;
    g_lobby_hash = content_hash;
    std::snprintf(g_callsign, sizeof(g_callsign), "%s", callsign && *callsign ? callsign : "HOST");
    g_enabled = true;
    if (!start_running()) {
        g_enabled = false;
        *why = "the network thread could not start";
        return false;
    }
    LOG("net: host session started on %s:%u as \"%s\"", g_addr, static_cast<unsigned>(g_port), g_callsign);
    return true;
}

bool host_session_port(uint16_t* port, bool* failed) {
    *port = 0;
    *failed = false;
    if (!g_running.load() || !g_core) return false;
    *failed = g_core->state() == net::HostCore::State::Failed;
    *port = g_core->port();
    return *port != 0;
}

void host_session_stop() {
    if (g_from_env || !g_running.exchange(false)) return;
    g_core->stop(false);  // joins the network thread: it disconnects every peer and stops ENet
    // Menus: no step runs, and the aircraft of the last flight are gone (check_live finds none), so the avatars are
    // only forgotten.
    bool live[net::kMaxClients] = {};
    if (g_nclients > 0) check_live(live);
    for (int s = 0; s < net::kMaxClients; ++s)
        if (g_av[s].active) release(s, "the host closed the session", live[s], kByeNone, net::LeaveReason::HostEnded, false);
    set_clients(0);
    sim_set_run_active(false);
    g_stream->close(false);  // the writer drains on its own; the next open waits for it
    {
        std::lock_guard<std::mutex> lock(g_mission_mx);
        g_core_started.store(false);
        g_mission.clear();
        g_mission_id = g_mission_raw = 0;
        g_seat_plan = net::SeatPlan{};
    }
    g_enabled = false;
    g_lobby_hash = 0;
    net::auth_wipe(g_key);
    LOG("net: host session closed");
}

bool host_drives_object(const void* obj) {
    if (!g_av || !obj) return false;
    for (int s = 0; s < net::kMaxClients; ++s)
        if (g_av[s].active && g_av[s].h.obj == obj) return true;
    return false;
}

void host_publish_mission(const uint8_t* packed, size_t n, uint32_t id, uint32_t raw, int members, int host_member) {
    if (!g_enabled) return;
    std::lock_guard<std::mutex> lock(g_mission_mx);
    g_mission.assign(packed, packed + n);
    g_mission_id = id;
    g_mission_raw = raw;
    g_seat_plan = net::SeatPlan{members, host_member};
    if (g_core_started.load()) g_core->set_mission(g_mission, id, raw, g_seat_plan);
    if (g_core_started.load() && g_lobby_hash) world_set_own_seat(g_core->lobby_view().host_seat);  // the lobby's seat
    LOG("net: host serves mission 0x%08lx (%zu bytes packed, %lu unpacked); seats: %d members, the host flies %d",
        static_cast<unsigned long>(id), n, static_cast<unsigned long>(raw), members, host_member);
}

void host_shutdown() {
    if (!g_running.exchange(false)) return;
    g_core->stop(true);
    // Hand any avatar back to the AI, if the aircraft is still there. DLL unload only: nothing else runs.
    bool live[net::kMaxClients] = {};
    if (g_nclients > 0) check_live(live);
    for (int s = 0; s < net::kMaxClients; ++s)
        if (g_av[s].active) release(s, "host shut down", live[s], kByeNone, net::LeaveReason::HostEnded, false);
    // release() above ran on the DllMain thread, not the game thread: that breaks the writer's single-producer rule,
    // but only here, at FreeLibrary, when the game has stopped stepping.
    g_stream->close(true);  // waits at most 1 s for the writer to flush, then detaches it
}

bool host_has_clients() { return g_has_clients.load(std::memory_order_relaxed); }

int host_avatar_slot(const void* obj) {
    if (!g_av || !obj) return -1;
    for (int s = 0; s < net::kMaxClients; ++s)
        if (g_av[s].active && !g_av[s].parked && g_av[s].h.obj == obj) return s;
    return -1;
}

void* host_avatar_object(int slot) {
    if (!g_av || slot < 0 || slot >= net::kMaxClients || !g_av[slot].active || g_av[slot].parked) return nullptr;
    return g_av[slot].h.obj;
}

bool host_world_send_to(int slot, const uint8_t* data, size_t len, bool reliable) {
    return g_running.load(std::memory_order_relaxed) && world_send_to(slot, data, len, reliable);
}

bool host_world_send_all(const uint8_t* data, size_t len, bool reliable) {
    return g_running.load(std::memory_order_relaxed) && world_send(data, len, reliable);
}

double net_host_now() { return client_welcomed() ? client_host_now() : sim_game_time(); }

bool host_world_active() { return g_running.load(std::memory_order_relaxed) && g_nclients > 0 && world_host_streaming(); }

}  // namespace LazyWorks::SF2Patch::hooks
