// Client mode (developer tool, part of the probe): the game joins a host, moves its jet onto the host's wing, sends
// its State, and shows the host and the other clients on AI aircraft (remote_avatar.cpp). Inert unless LAZYSF2_CONNECT
// is set and the probe is on. Game thread only, from the step callback; the network thread (client_core.cpp) owns ENet
// and the clock. The decisions are in client_logic.h. See the Phase 4 plan, sections 2.2, 2.4 and 2.7.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "hooks/hooks.h"
#include "hooks/remote_avatar.h"
#include "hooks/replay_math.h"
#include "hooks/terrain_export.h"
#include "log.h"
#include "net/client_core.h"
#include "net/client_logic.h"
#include "net/clock_sync.h"
#include "net/host_session.h"
#include "net/protocol.h"
#include "net/receiver_math.h"
#include "net/remote_jet.h"
#include "net/stream_format.h"
#include "net/stream_record.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

using namespace replay_math;
namespace cl = net::client_logic;
using net::ClientCore;
using net::ClientEvent;

constexpr size_t kOffPos = 0x38, kOffVel = 0x50, kOffRates = 0xd0, kOffMatrix = 0xdc, kOffAngles = 0x100,
                 kOffHeading2 = 0x150;
constexpr uint8_t kByeSessionEnded = 3;
constexpr int kSources = ClientCore::kSources;  // remote ids: the host (0) and clients 1..kMaxClients
constexpr double kParkAfter = 10.0;   // wall seconds without a State before a remote's aircraft goes back to the AI
constexpr double kShownEvery = 0.1;   // RemoteShown records per remote, host seconds
constexpr double kTakeEvery = 1.0;    // host seconds between tries to take an aircraft for a remote

// Set once by client_install(), then read-only.
bool g_enabled = false;
char g_host[64] = {};
uint16_t g_port = 0;
char g_callsign[16] = "PLAYER";  // LAZYSF2_CALLSIGN, printable ASCII, at most 15 characters (the wire field)
bool g_join_move = true;         // LAZYSF2_NET_JOIN=move (default) or none

// On the heap, created in client_on_modules_loaded() only when client mode is on. Never freed (as in host.cpp).
ClientCore* g_core = nullptr;
net::StreamWriter* g_stream = nullptr;
std::atomic<bool> g_running{false};   // the step callback is registered and the objects exist
std::atomic<bool> g_welcomed{false};  // client_welcomed(), for other threads

// The connection, as the game thread sees it.
enum class Link { Idle, Running };
Link g_link = Link::Idle;
bool g_leaving = false;       // we said Bye (the player changed): only the end of the connection is still wanted
bool g_disconnected = false;  // Disconnected seen; stop() once the thread has cleaned up
double g_next_try = 0;        // wall time of the next connection attempt
bool g_blocked = false;       // refused in a way a retry cannot fix (theatre, version) until the player changes
bool g_busy_logged = false;   // "not connecting while a puppet or replay runs", once per block
bool g_retry_quiet = false;   // a failed attempt was logged: later ones are not, until a Welcome or a new player
// The network thread's lines are dropped while a quiet retry has not connected (it clears this when it does).
std::atomic<bool> g_core_quiet{false};
uint32_t g_epoch = 0;
char g_own_type[33] = {};
uint32_t g_theatre_hash = 0;
char g_theatre[33] = {};

// The session: from the first Welcome of a connection to its end.
bool g_session = false;
uint16_t g_id = 0;
net::WelcomeMsg g_welcome{};
int g_welcomes = 0;           // Welcomes in this session; a re-Welcome re-seeds the clock
double g_welcome_wall = 0;    // the first Welcome's arrival
double g_reannounce_at = 0;   // wall: after a re-Welcome, remotes not announced again by then are dropped
bool g_peer0_logged = false;  // R1: the host's PeerInfo did not come (logged once)
bool g_moved = false;         // the jet was moved to a join position for this player (kept across reconnects)
unsigned g_join_logged = 0;   // the join actions already logged in this session (bit per JoinAction)

// The shared clock, read once per player step; avatars read the HostClock again at their own step.
net::ClockSync g_snap;
bool g_snap_ok = false;
// No avatar writes (and no States): no clock, the host's clock frozen, no Pong for 3 s, or never synced yet. The wait
// for the first Pong after a Welcome's re-seed is not a hold: the seeded clock drives the avatars meanwhile.
bool g_hold = true;
double g_now = 0;       // host time at the player's latest step
net::HostClock g_hclock;
cl::ClockWatch g_watch;


net::SendTicker g_tick;
uint32_t g_send_seq = 0;  // never reset, so the host's receiver never sees it restart
unsigned g_tx = 0, g_tx_fail = 0, g_tx_held = 0;  // since the last report: sent, refused by the out queue, held steps
double g_next_report = 0;  // wall
uint32_t g_checked_gen = 0;

struct Remote {
    Remote() = default;  // not an aggregate: RemoteJet's constructor is explicit
    bool known = false;      // announced by a PeerInfo
    bool announced = false;  // announced again since the latest Welcome
    char callsign[17] = {};
    char aircraft[33] = {};
    AvatarHandle h;
    net::RemoteJet jet{net::JetParams{}};  // Predict mode (plan 2.5)
    bool got_state = false;
    bool shown_state = false;  // the first shown State was logged for this aircraft
    bool coasting = false;
    bool take_failed = false;  // logged once until a take succeeds
    bool have_shown = false;
    double next_take = 0;        // host time
    double last_state_wall = 0;  // wall time of the newest accepted State
    double last_game_time = 0;   // that State's stamp
    double next_shown = 0;       // host time of the next RemoteShown record
    double shown_p[3] = {};
    float shown_a[3] = {};
    double placed[3] = {};  // where the aircraft was put when taken
    uint32_t flag_clears = 0;
};
Remote* g_rm = nullptr;  // kSources entries, indexed by id

void core_log(const char* line) { LOG("%s", line); }

/// <summary>
/// ClientCore's log (network thread). Its "connecting to" line names the address, which client_install() logged once
/// already; during a quiet retry nothing is logged until the connection is made.
/// </summary>
void client_core_log(const char* line) {
    if (std::strncmp(line, "net: client connected", 21) == 0) g_core_quiet.store(false, std::memory_order_relaxed);
    if (g_core_quiet.load(std::memory_order_relaxed) || std::strncmp(line, "net: client connecting to ", 26) == 0)
        return;
    LOG("%s", line);
}

void rec(net::StreamKind kind, double t, const void* payload, size_t len) { g_stream->record(kind, t, payload, len); }

template <class M>
void rec_msg(net::StreamKind kind, double t, const M& m) {
    if (!g_stream->is_open()) return;
    uint8_t buf[net::kMaxMessage];
    const size_t n = net::encode(m, buf, sizeof(buf));
    if (n) rec(kind, t, buf, n);
}

double deg(double r) { return r * 180.0 / kPi; }

void handles(AvatarHandle out[kSources]) {
    for (int i = 0; i < kSources; ++i) out[i] = g_rm[i].h;
}

void check_live(bool live[kSources]) {
    AvatarHandle h[kSources];
    handles(h);
    avatar_check_live(h, kSources, live);
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

/// <summary>Gives a remote's aircraft back (restoring the flags only if it is live), and with forget the whole entry.</summary>
void release_remote(int id, const char* why, bool live, bool forget) {
    Remote& r = g_rm[id];
    if (r.h.obj) {
        void* const obj = r.h.obj;
        avatar_release(r.h, live);
        LOG("net: remote %d \"%s\": avatar %p %s (%s)", id, r.callsign, obj,
            live ? "given back to the AI" : "is gone, flags not restored", why);
    }
    if (forget) {
        r = Remote{};
    } else {
        r.coasting = false;
        r.shown_state = false;
        r.have_shown = false;
    }
}

/// <summary>Ends the session on our side: every avatar back to the AI, the recording closed. Idempotent.</summary>
void end_session(const char* why) {
    bool live[kSources] = {};
    bool any = false;
    for (int i = 0; i < kSources; ++i) any = any || g_rm[i].h.obj;
    if (any) check_live(live);
    for (int i = 0; i < kSources; ++i) release_remote(i, why, live[i], true);
    if (g_session) {
        LOG("net: client %u: session over (%s); %d Welcome(s) in it", static_cast<unsigned>(g_id), why, g_welcomes);
        sim_set_run_active(false);
    }
    g_stream->close(false);  // the writer thread drains, flushes and closes the file
    g_session = false;
    g_welcomed.store(false, std::memory_order_relaxed);
    g_snap_ok = false;
    g_hold = true;
}

void release_dead() {
    const uint32_t gen = g_aircraft_generation.load(std::memory_order_acquire);  // before the table read
    bool live[kSources];
    check_live(live);
    g_checked_gen = gen;
    for (int i = 0; i < kSources; ++i)
        if (g_rm[i].h.obj && !live[i]) release_remote(i, "aircraft destroyed; another is taken at the next State", false, false);
}

void reset_jets() {
    for (int i = 0; i < kSources; ++i) g_rm[i].jet.reset();  // the next State snaps; the stats window starts again
    g_tick.reset();
}

/// <summary>Reads the shared clock for this step, and logs and acts on its changes.</summary>
void refresh_clock(double wall) {
    g_snap_ok = g_core->clock_snapshot(&g_snap);
    if (!g_snap_ok) {
        g_hold = true;
        return;
    }
    g_now = g_hclock.read(wall, g_snap);
    const unsigned ev = g_watch.update(g_snap, g_hclock, wall);
    g_hold = g_snap.on_hold(wall) || (!g_snap.have_pong() && !g_watch.synced());
    if (ev & cl::ClockWatch::kReset) {
        reset_jets();
        if (!(ev & cl::ClockWatch::kReseed))  // a Welcome's re-seed is logged with the Welcome
            LOG("net: client: clock reset (host time %.3f); the remote jets start again", g_now);
    }
    if (ev & cl::ClockWatch::kHold)
        LOG("net: client: clock hold (the host's clock is frozen, or no Pong for %.0f s); no States sent, remote jets "
            "coast",
            net::ClockSync::kStale);
    if (ev & cl::ClockWatch::kSynced)
        LOG("net: client: clock synced: offset %.4f s, skew %.0f ppm, rtt %.1f ms, host time %.3f", g_snap.offset(),
            g_snap.skew() * 1e6, g_snap.best_rtt() * 1000.0, g_now);
}

/// <summary>
/// Moves the player's jet to pos and turns it to heading: position, the heading angle (and its copy at +0x150), the
/// matrix rows and the world velocity turned about the vertical. Pitch, roll, speed, body velocity and rates stay, so
/// the flight model's body-frame state stays valid (the R3 result: lazy-sf2-recon, network-recon §8.7).
/// </summary>
void move_player(void* player, const double pos[3], float heading, double* turned) {
    double p[3];
    float vel[3], m[9], ang[3], h2;
    obj_read(player, kOffPos, p, 3);
    obj_read(player, kOffVel, vel, 3);
    obj_read(player, kOffMatrix, m, 9);
    obj_read(player, kOffAngles, ang, 3);
    obj_read(player, kOffHeading2, &h2, 1);
    const double delta = wrap_pi(static_cast<double>(heading) - static_cast<double>(ang[1]));
    *turned = delta;
    cl::turn_heading(vel, delta);
    for (int row = 0; row < 3; ++row) cl::turn_heading(m + 3 * row, delta);  // rows right, forward, up: world vectors
    const bool h2_is_copy = std::fabs(wrap_pi(static_cast<double>(h2) - static_cast<double>(ang[1]))) < 1e-3;
    ang[1] = cl::heading_like(ang[1], heading);
    if (h2_is_copy) h2 = cl::heading_like(h2, heading);
    obj_write(player, kOffPos, pos, 3);
    obj_write(player, kOffVel, vel, 3);
    obj_write(player, kOffMatrix, m, 9);
    obj_write(player, kOffAngles, ang, 3);
    if (h2_is_copy) obj_write(player, kOffHeading2, &h2, 1);
}

void join(void* player, const net::WelcomeMsg& w, double wall) {
    double pos[3];
    float vel[3];
    obj_read(player, kOffPos, pos, 3);
    obj_read(player, kOffVel, vel, 3);
    net::ClockSync c;
    const double now = g_core->clock_snapshot(&c) ? c.now(wall) : w.host_game_time;
    double target[3];
    cl::join_target(w, now, target);
    const double dist = cl::dist3(pos, target);
    const cl::JoinAction a =
        cl::join_action(net::welcome_has_join(w), g_join_move, cl::airborne(pos, vel), g_moved, dist);
    if (a == cl::JoinAction::Move) {
        double turned = 0;
        move_player(player, target, w.spawn_heading, &turned);
        g_moved = true;
        LOG("net: client: joined the host's wing: moved %.0f m to (%.0f, %.0f, %.0f), turned %.1f deg to heading %.1f "
            "deg (speed kept, %.0f m/s; the host's %.0f m/s)",
            dist, target[0], target[1], target[2], deg(turned), deg(static_cast<double>(w.spawn_heading)),
            cl::norm3(vel), cl::norm3(w.spawn_vel));
        return;
    }
    const unsigned bit = 1u << static_cast<unsigned>(a);
    if (g_join_logged & bit) return;  // a host re-sends its join: say each outcome once per session
    g_join_logged |= bit;
    if (a == cl::JoinAction::NoJoin)
        LOG("net: client: this Welcome has no join position (the host places its jet first); waiting for one");
    else
        LOG("net: client: join position %.0f m away: %s", dist, cl::join_action_name(a));
}

void handle_welcome(void* player, const ClientEvent& e, double wall) {
    net::WelcomeMsg w;
    if (net::decode(e.data, e.len, w) != net::Status::Ok) return;
    const bool first = !g_session;
    if (g_puppet_state.load() != 0 || replay_active()) {
        LOG("net: client: a puppet or replay run started; leaving the session (Bye 1)");
        g_core->bye(1);  // busy
        g_leaving = true;
        end_session("a puppet or replay run is active");
        return;
    }
    if (first && net::theatre_mismatch(g_theatre_hash, w.theatre_hash)) {
        LOG("net: client: the host flies in another theatre (its hash %08x; ours %08x, %s); leaving (Bye 5), no retry "
            "until the player changes",
            static_cast<unsigned>(w.theatre_hash), static_cast<unsigned>(g_theatre_hash), g_theatre);
        g_core->bye(net::kByeWrongTheatre);
        g_leaving = true;
        g_blocked = true;
        return;
    }
    g_welcome = w;
    ++g_welcomes;
    for (int i = 0; i < kSources; ++i) g_rm[i].announced = false;
    if (first) {
        g_session = true;
        g_retry_quiet = false;
        g_id = w.client_id;
        g_welcome_wall = wall;
        g_reannounce_at = 0;
        g_peer0_logged = false;
        g_join_logged = 0;
        g_watch.clear();
        g_hclock = net::HostClock{};
        g_tx = g_tx_fail = g_tx_held = 0;
        g_next_report = wall + 1.0;
        g_checked_gen = g_aircraft_generation.load(std::memory_order_acquire);
        g_welcomed.store(true, std::memory_order_relaxed);
        sim_set_run_active(true);
        // One CreateFileW, and a bounded wait for an earlier writer that is still flushing: only at a session start.
        g_stream->open(probe_output_dir(), w.host_game_time, g_theatre[0] ? g_theatre : nullptr, core_log,
                       net::kStreamMaxFileBytes, L"client");
        LOG("net: client %u welcomed: host time %.3f, theatre %s", static_cast<unsigned>(g_id), w.host_game_time,
            w.theatre_hash ? (w.theatre_hash == g_theatre_hash ? g_theatre : "another hash") : "unknown to the host");
    } else {
        g_reannounce_at = wall + cl::kPeerInfoWait;
        if (g_welcomes == 2)
            LOG("net: client: Welcome again%s: the clock is re-seeded and the remote jets start again (further ones are "
                "counted, not logged)",
                net::welcome_has_join(w) ? ", with a join position" : "");
    }
    rec_msg(net::StreamKind::WelcomeSent, w.host_game_time, w);
    reset_jets();
    refresh_clock(wall);  // takes the re-seeded clock now, so the States after it are timed on it
    join(player, w, wall);
}

void handle_peer_info(const ClientEvent& e) {
    net::PeerInfoMsg m;
    if (net::decode(e.data, e.len, m) != net::Status::Ok || m.id >= kSources) return;
    Remote& r = g_rm[m.id];
    char callsign[17], aircraft[33];
    avatar_logic::to_ascii(callsign, sizeof(callsign), m.callsign);
    avatar_logic::to_ascii(aircraft, sizeof(aircraft), m.aircraft);
    const bool changed = r.known && (std::strcmp(callsign, r.callsign) != 0 || std::strcmp(aircraft, r.aircraft) != 0);
    if (!r.known || changed) {
        LOG("net: peer %u \"%s\" (%s) %s%s", static_cast<unsigned>(m.id), callsign, aircraft,
            changed ? "announced again with new details" : "announced", m.id == 0 ? ": the host" : "");
        if (!avatar_logic::same_type(aircraft, g_own_type) && std::strcmp(aircraft, "unknown") != 0)
            LOG("net: peer %u: warning: it flies \"%s\" and we fly \"%s\"; both players should fly the same type",
                static_cast<unsigned>(m.id), aircraft, g_own_type[0] ? g_own_type : "(type unknown)");
    }
    r.known = true;
    r.announced = true;
    std::memcpy(r.callsign, callsign, sizeof(r.callsign));
    std::memcpy(r.aircraft, aircraft, sizeof(r.aircraft));
}

void handle_peer_left(const ClientEvent& e) {
    net::PeerLeftMsg m;
    if (net::decode(e.data, e.len, m) != net::Status::Ok || m.id >= kSources || !g_rm[m.id].known) return;
    LOG("net: peer %u \"%s\" left (reason %u)", static_cast<unsigned>(m.id), g_rm[m.id].callsign,
        static_cast<unsigned>(m.reason));
    bool live[kSources] = {};
    if (g_rm[m.id].h.obj) check_live(live);
    release_remote(m.id, "the peer left", live[m.id], true);
}

/// <summary>Takes an aircraft for a remote, placed on the State. At most once per kTakeEvery; false while none.</summary>
bool take(int id, const net::StateMsg& s) {
    Remote& r = g_rm[id];
    if (g_now < r.next_take) return false;
    r.next_take = g_now + kTakeEvery;
    const AvatarPick pick{r.aircraft, {s.pos[0], s.pos[1], s.pos[2]}, s.ang[1], {s.vel[0], s.vel[1], s.vel[2]}};
    AvatarHandle taken[kSources];
    handles(taken);
    AvatarHandle got;
    AvatarInfo info;
    char why[256];
    if (!avatar_take(pick, taken, kSources, &got, why, sizeof(why), &info)) {
        if (!r.take_failed) LOG("net: remote %d \"%s\": no avatar yet: %s", id, r.callsign, why);
        r.take_failed = true;
        return false;
    }
    r.take_failed = false;
    r.h = got;
    std::memcpy(r.placed, pick.place, sizeof(r.placed));
    r.jet.reset();  // the first State on this aircraft snaps
    r.shown_state = false;
    r.have_shown = false;
    r.coasting = false;
    LOG("net: remote %d \"%s\" (%s): avatar %p %s", id, r.callsign, r.aircraft, got.obj, why);
    if (!avatar_logic::same_type(info.type, r.aircraft) && std::strcmp(r.aircraft, "unknown") != 0)
        LOG("net: remote %d: warning: it flies \"%s\" but its avatar is \"%s\"", id, r.aircraft,
            info.type[0] ? info.type : "(type unknown)");
    return true;
}

void handle_state(const ClientEvent& e, double wall) {
    net::StateMsg s;
    if (net::decode(e.data, e.len, s) != net::Status::Ok || s.client_id >= kSources || !g_snap_ok) return;
    const int id = s.client_id;
    Remote& r = g_rm[id];
    if (!r.known) return;  // the core passes only announced ids; this is the game side's own guard
    // Its arrival in host time; never after this step's time (the HostClock may lag the raw estimate while it slews).
    double t_rx = g_snap.now(e.wall_rx);
    if (t_rx > g_now) t_rx = g_now;
    rec(net::StreamKind::StateReceived, t_rx, e.data, e.len);
    // Heard from, with or without an aircraft: the 1 Hz line and the park rule go by this.
    r.last_state_wall = wall;
    r.last_game_time = s.game_time;
    if (!r.h.obj && !take(id, s)) return;
    if (!r.jet.push(s, t_rx)) return;
    if (!r.got_state)
        LOG("net: remote %d \"%s\": first State, seq %u, age %.0f ms", id, r.callsign, static_cast<unsigned>(s.seq),
            (t_rx - s.game_time) * 1000.0);
    r.got_state = true;
}

void handle_end(const ClientEvent& e, double wall) {
    const auto why = static_cast<net::ClientEnd>(e.reason);
    if (why == net::ClientEnd::Refused) g_blocked = true;
    end_session("disconnected");
    g_disconnected = true;
    // A session we left ourselves may start again at once (a new player); anything else waits kRetrySeconds.
    g_next_try = wall + (why == net::ClientEnd::Left && !g_blocked ? 0.0 : cl::kRetrySeconds);
    if (g_blocked) {
        LOG("net: client: not connecting again until the player changes");
    } else if (!g_retry_quiet) {
        LOG("net: client: will connect again %s", why == net::ClientEnd::Left ? "once airborne"
                                                  : "every 5 s while airborne (further attempts are not logged here)");
        g_retry_quiet = why != net::ClientEnd::Left;
    }
}

void pump(void* player, double wall) {
    ClientEvent ev[16];
    for (int round = 0; round < 16; ++round) {
        const size_t n = g_core->poll(ev, 16);
        if (n == 0) break;
        for (size_t i = 0; i < n; ++i) {
            const ClientEvent& e = ev[i];
            if (e.kind == ClientEvent::Disconnected) {
                handle_end(e, wall);
                return;
            }
            if (g_leaving) continue;  // after our Bye only the end matters
            switch (e.kind) {
                case ClientEvent::Welcome: handle_welcome(player, e, wall); break;
                case ClientEvent::PeerInfo:
                    if (g_session) handle_peer_info(e);
                    break;
                case ClientEvent::PeerLeft:
                    if (g_session) handle_peer_left(e);
                    break;
                case ClientEvent::State:
                    if (g_session) handle_state(e, wall);
                    break;
                case ClientEvent::Bye: {
                    net::ByeMsg b;
                    if (net::decode(e.data, e.len, b) != net::Status::Ok) break;
                    if (g_session) rec_msg(net::StreamKind::ByeReceived, g_now, b);
                    if (b.reason == net::kByeWrongTheatre || b.reason == net::kByeWrongVersion) g_blocked = true;
                    LOG("net: client: the host said Bye (reason %u)%s", static_cast<unsigned>(b.reason),
                        g_blocked ? "; no retry until the player changes" : "");
                    end_session("the host said Bye");
                    g_leaving = true;  // the core disconnects by itself
                    break;
                }
                default: break;
            }
        }
    }
}

net::StateMsg own_state(void* player, double now, uint32_t seq) {
    net::StateMsg s{};
    double pos[3];
    float vel[3], ang[3], rates[3];
    obj_read(player, kOffPos, pos, 3);
    obj_read(player, kOffVel, vel, 3);
    obj_read(player, kOffAngles, ang, 3);
    obj_read(player, kOffRates, rates, 3);
    s.client_id = g_id;
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

void send_own(void* player) {
    if (!cl::may_send(g_session, g_snap_ok && g_snap.have_pong(), g_hold)) {
        ++g_tx_held;
        return;
    }
    if (!g_tick.due(g_now)) return;
    const net::StateMsg s = own_state(player, g_now, ++g_send_seq);
    if (!g_core->send_state(s)) {
        ++g_tx_fail;
        return;
    }
    ++g_tx;
    rec_msg(net::StreamKind::LocalState, g_now, s);
}

void report(void* player, double wall) {
    LOG("net: client %u jet: %u State(s) sent, %u refused (out queue full), %u step(s) held (no clock); rtt %.0f ms",
        static_cast<unsigned>(g_id), g_tx, g_tx_fail, g_tx_held, static_cast<double>(g_core->rtt_ms()));
    g_tx = g_tx_fail = g_tx_held = 0;
    if (!g_rm[0].known && !g_peer0_logged && wall - g_welcome_wall > cl::kPeerInfoWait) {
        g_peer0_logged = true;
        LOG("net: client: no PeerInfo from the host %.0f s after the Welcome; its jet is not shown until one comes",
            cl::kPeerInfoWait);
    }
    if (g_reannounce_at > 0 && wall >= g_reannounce_at) {
        g_reannounce_at = 0;
        for (int i = 0; i < kSources; ++i) {
            if (!g_rm[i].known || g_rm[i].announced) continue;
            LOG("net: peer %d \"%s\" was not announced again after the Welcome; dropped", i, g_rm[i].callsign);
            bool live[kSources] = {};
            if (g_rm[i].h.obj) check_live(live);
            release_remote(i, "not announced again", live[i], true);
        }
    }
    double pp[3];
    obj_read(player, kOffPos, pp, 3);
    for (int i = 0; i < kSources; ++i) {
        Remote& r = g_rm[i];
        if (!r.known || r.last_state_wall == 0) continue;
        if (!r.h.obj) {
            LOG("net: remote %d \"%s\" no avatar (last State %.0f s ago) rtt %.0f ms", i, r.callsign, wall - r.last_state_wall,
                static_cast<double>(g_core->rtt_ms()));
            continue;
        }
        double ap[3];
        obj_read(r.h.obj, kOffPos, ap, 3);
        const net::JetStats st = r.jet.take_stats();
        // The same fields as the host's per-client line (host.cpp report()).
        LOG("net: remote %d \"%s\" rate %.1f Hz delay %.0f ms rtt %.0f ms err %.2f m (avatar dist %.0f m) "
            "age %.0f/%.0f ms horizon %.0f ms snaps %u coast %u accel %.1f g",
            i, r.callsign, st.rate_hz, st.delay * 1000.0, static_cast<double>(g_core->rtt_ms()), st.offset_max,
            cl::dist3(pp, ap), std::fmax(0.0, st.age_p50) * 1000.0, std::fmax(0.0, st.age_p90) * 1000.0,
            st.horizon_max * 1000.0, st.snaps + st.resync_snaps, st.coasting_steps, st.accel_g_max);
        if (st.dup_or_old || st.restarts || st.resyncs)
            LOG("net: remote %d: %u old or duplicate State(s), %u source restart(s), %u clock resync(s)", i,
                st.dup_or_old, st.restarts, st.resyncs);
        if (r.flag_clears) LOG("net: remote %d: the game cleared REMOTEOBJECT %u time(s); set again", i, r.flag_clears);
        r.flag_clears = 0;
    }
}

void avatar_step(int id, double now, double wall) {
    Remote& r = g_rm[id];
    // Silent for long (the peer paused, or its States stopped): the aircraft goes back to the AI until they resume.
    // Wall time, so it also applies while the host's clock is on hold. In its own step, so it is live.
    if (r.got_state && wall - r.last_state_wall > kParkAfter) {
        LOG("net: remote %d \"%s\": no State for %.0f s; avatar %p given back to the AI until States resume", id,
            r.callsign, kParkAfter, r.h.obj);
        avatar_release(r.h, true);
        r.coasting = r.shown_state = r.have_shown = false;
        r.next_take = 0;
        return;
    }
    if (avatar_keep_remote(r.h)) ++r.flag_clears;
    replay_math::Kin k{};
    const net::RemoteJet::Out out = g_hold ? net::RemoteJet::Out::Coast : r.jet.shown(now, &k);
    if (out == net::RemoteJet::Out::Coast) {
        if (!r.coasting) {
            r.coasting = true;
            avatar_zero_rates(r.h);  // coast straight on, attitude held
            if (!g_hold)
                LOG("net: remote %d \"%s\": no State for over %.0f s; the avatar coasts on its velocity, rates zeroed",
                    id, r.callsign, r.jet.params().coast_after);
        }
    } else if (out != net::RemoteJet::Out::None) {
        if (r.coasting) {
            r.coasting = false;
            LOG("net: remote %d \"%s\": States again; the avatar is driven", id, r.callsign);
        }
        if (!r.shown_state) {
            r.shown_state = true;
            LOG("net: remote %d \"%s\": first State shown (%s, %.0f m from where the avatar was placed)", id, r.callsign,
                out == net::RemoteJet::Out::Snap ? "snap" : "blend", cl::dist3(r.placed, k.p));
        }
        avatar_write(r.h, k);
        for (int i = 0; i < 3; ++i) {
            r.shown_p[i] = k.p[i];
            r.shown_a[i] = static_cast<float>(k.a[i]);
        }
        r.have_shown = true;
    }
    if (out != net::RemoteJet::Out::None && r.have_shown && g_stream->is_open() && now >= r.next_shown) {
        r.next_shown = now + kShownEvery;
        net::RemoteShownPayload p{};
        p.id = static_cast<uint16_t>(id);
        std::memcpy(p.pos, r.shown_p, sizeof(p.pos));
        std::memcpy(p.ang, r.shown_a, sizeof(p.ang));
        p.age = static_cast<float>(now - r.last_game_time);
        p.out = static_cast<uint8_t>(out);  // Coast: the last shown position and angles
        rec(net::StreamKind::RemoteShown, now, &p, sizeof(p));
    }
}

/// <summary>On the player's step with no connection: start one if it is time.</summary>
void maybe_connect(void* player, double wall) {
    const bool busy = g_puppet_state.load() != 0 || replay_active();
    if (busy && !g_busy_logged) LOG("net: client: not connecting while a puppet or replay run is active");
    g_busy_logged = busy;
    double pos[3];
    float vel[3];
    obj_read(player, kOffPos, pos, 3);
    obj_read(player, kOffVel, vel, 3);
    if (!cl::connect_due(sim_player_known(), cl::airborne(pos, vel), true, busy || g_blocked, wall, g_next_try)) return;
    g_next_try = wall + cl::kRetrySeconds;
    g_own_type[0] = 0;
    probe_with_aircraft(player_type_cb, g_own_type);
    std::string theatre;  // once per attempt, not per frame
    g_theatre_hash = terrain_theatre_id(theatre) ? net::fnv1a(theatre.c_str()) : 0;
    avatar_logic::to_ascii(g_theatre, sizeof(g_theatre), theatre.c_str());
    net::HelloMsg h{};
    net::set_str(h.callsign, g_callsign);
    net::set_str(h.aircraft, g_own_type[0] ? g_own_type : "unknown");
    h.theatre_hash = g_theatre_hash;
    g_core_quiet.store(g_retry_quiet, std::memory_order_relaxed);
    if (!g_core->start(g_host, g_port, h, client_core_log)) {
        LOG("net: client: could not start the network thread; trying again in 5 s");
        return;
    }
    g_link = Link::Running;
    g_leaving = g_disconnected = false;
    g_welcomes = 0;
    if (!g_retry_quiet)
        LOG("net: client: joining as \"%s\" (%s), theatre %s", g_callsign, g_own_type[0] ? g_own_type : "type unknown",
            g_theatre_hash ? g_theatre : "unknown");
}

void on_step(void* aircraft, float /*dt*/, bool is_player) {
    if (!g_running.load(std::memory_order_relaxed)) return;
    const double wall = ClientCore::wall_now();
    // The player changed (mission end, new flight): this session is over. Give the avatars back first, so only
    // aircraft that still exist get their flags written.
    if (sim_player_epoch() != g_epoch) {
        g_epoch = sim_player_epoch();
        g_blocked = false;
        g_retry_quiet = false;
        g_moved = false;
        if (g_link == Link::Running && !g_leaving && !g_disconnected) {
            g_core->bye(kByeSessionEnded);
            g_leaving = true;
            LOG("net: client: the player changed; leaving the session (Bye 3)");
        }
        end_session("the player changed");
    } else if (g_session && g_aircraft_generation.load(std::memory_order_acquire) != g_checked_gen) {
        release_dead();
    }
    if (is_player) {
        if (g_link == Link::Running) {
            if (!g_disconnected) {
                refresh_clock(wall);
                pump(aircraft, wall);
            }
            if (g_disconnected && g_core->finished()) {
                g_core->stop(false);  // the thread has cleaned up: the join returns at once
                g_link = Link::Idle;
            }
        }
        if (g_link == Link::Idle) {
            maybe_connect(aircraft, wall);
            return;
        }
        if (!g_session || g_leaving) return;
        send_own(aircraft);
        if (wall >= g_next_report) {
            report(aircraft, wall);
            g_next_report = std::floor(wall) + 1.0;
        }
        return;
    }
    if (!g_session || g_leaving) return;
    for (int i = 0; i < kSources; ++i) {
        if (!avatar_is(g_rm[i].h, aircraft)) continue;
        const double now = g_snap_ok ? g_hclock.read(wall, g_snap) : g_now;
        avatar_step(i, now, wall);
    }
}

/// <summary>An environment variable as printable ASCII; false if unset, too long or not printable.</summary>
bool env_ascii(const wchar_t* name, char* out, size_t cap) {
    wchar_t w[96] = {};
    const DWORD n = GetEnvironmentVariableW(name, w, 96);
    if (n == 0 || n >= 96 || n >= cap) return false;
    for (DWORD i = 0; i < n; ++i) {
        if (w[i] > 0x7e || w[i] < 0x20) return false;
        out[i] = static_cast<char>(w[i]);
    }
    out[n] = 0;
    return true;
}

}  // namespace

void client_install() {
    if (!env_set(L"LAZYSF2_CONNECT")) return;
    if (env_set(L"LAZYSF2_HOST")) return;  // host_install() logged that both are off
    if (!probe_output_dir()) {
        LOG("net: client off (the probe is off: LAZYSF2_PROBE_DIR is not set or not valid)");
        return;
    }
    char spec[96] = {};
    if (!env_ascii(L"LAZYSF2_CONNECT", spec, sizeof(spec)) ||
        !cl::parse_connect_spec(spec, g_host, sizeof(g_host), &g_port)) {
        LOG("net: client off (LAZYSF2_CONNECT must be host:port, the host a numeric IPv4 address or a name: "
            "100.64.0.1:7777)");
        return;
    }
    wchar_t cs[32] = {};
    const DWORD nc = GetEnvironmentVariableW(L"LAZYSF2_CALLSIGN", cs, 32);
    if (nc > 0 && nc < 32) avatar_logic::to_ascii(g_callsign, sizeof(g_callsign), cs);
    if (!g_callsign[0]) std::snprintf(g_callsign, sizeof(g_callsign), "PLAYER");
    char join[16] = {};
    if (env_ascii(L"LAZYSF2_NET_JOIN", join, sizeof(join))) {
        if (_stricmp(join, "none") == 0) {
            g_join_move = false;
        } else if (_stricmp(join, "move") != 0) {
            LOG("net: client: LAZYSF2_NET_JOIN must be move or none; using move");
        }
    }
    g_enabled = true;
    LOG("net: client on: joins %s:%u as \"%s\" once airborne; join %s", g_host, static_cast<unsigned>(g_port),
        g_callsign, g_join_move ? "move (onto the host's wing)" : "none (fly to the host)");
}

void client_on_modules_loaded() {
    static std::atomic<bool> once{false};
    if (!g_enabled || once.exchange(true)) return;
    g_core = new (std::nothrow) ClientCore;
    g_stream = new (std::nothrow) net::StreamWriter;
    g_rm = new (std::nothrow) Remote[kSources];
    if (!g_core || !g_stream || !g_rm) {
        LOG("net: client off (out of memory)");
        return;
    }
    if (!sim_register_step_callback(on_step)) {
        LOG("net: client off (no free step callback slot)");
        return;
    }
    g_epoch = sim_player_epoch();
    g_checked_gen = g_aircraft_generation.load(std::memory_order_acquire);
    g_running.store(true);
}

void client_shutdown() {
    if (!g_running.exchange(false)) return;
    if (g_link == Link::Running && !g_leaving) g_core->bye(kByeSessionEnded);
    g_core->stop(true);  // a Bye gets a short chance to go out; waits at most about 1 s, then detaches
    // DLL unload only: the game no longer steps, so this thread may give the avatars back (as host_shutdown does).
    end_session("shut down");
    g_stream->close(true);
}

bool client_welcomed() { return g_welcomed.load(std::memory_order_relaxed); }

}  // namespace LazyWorks::SF2Patch::hooks
