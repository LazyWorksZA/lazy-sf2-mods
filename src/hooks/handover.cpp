// Authority handover (handover task 4). One world owner at a time, the host by default. A client that loads the
// mission before the host's session runs the world with its own AI (its mark pass waits); when the host's session
// starts, it offers its world to the host as a snapshot (aircraft poses, state, throttle, fuel and stores; vehicles'
// poses, speeds and headings; flights' waypoints; the mission clock) and marks its world remote. The host takes the
// offer only if the client's mission clock is ahead of its own (the client loaded first), writes it into its world and
// answers. The probe's Ctrl+Shift+C hands the local world over to "itself" for the solo test: capture and mark remote,
// then rewind to the capture and hand every copy back to its AI. Weapons in flight stay with their launcher (their
// later hits are blocked or claimed as before). Game thread. See lazy-sf2-recon: network-recon §12.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

#include "hooks/hooks.h"
#include "hooks/jet_layout.h"
#include "hooks/world_logic.h"
#include "log.h"
#include "net/handover_format.h"
#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

constexpr size_t kObjPos = 0x38, kObjVel = 0x50, kObjRates = 0xd0, kObjAngles = 0x100, kObjHeading = 0x150,
                 kObjSpeed = 0x154, kObjAi = 0x4b8;
constexpr size_t kAiTask = 0x08, kAiManoeuvre = 0x0c, kAiWaypoint = 0x20;
constexpr uint32_t kStateKeep = 0x0080ff00u;  // airborne, and the configuration bits (gear, flaps, lights...)
constexpr int kChunksPerStep = 16;          // the client core's out queue holds 64

template <typename T>
bool rd(const void* p, T* v) {
    __try {
        std::memcpy(v, p, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <typename T>
bool wr(void* p, const T& v) {
    __try {
        std::memcpy(p, &v, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int16_t ang16(float a) {
    if (!std::isfinite(a)) return 0;
    const double t = std::remainder(static_cast<double>(a), 2.0 * 3.14159265358979323846) / (2.0 * 3.14159265358979323846);
    long v = std::lround(t * 65536.0);
    if (v > 32767) v -= 65536;
    return static_cast<int16_t>(v < -32768 ? -32768 : v);
}
float ang_back(int16_t q) { return static_cast<float>(q / 65536.0 * 2.0 * 3.14159265358979323846); }

bool read_pose(const BYTE* o, net::WorldObjState* q) {
    net::ObjPose p{};
    if (!rd(o + kObjPos, &p.pos) || !rd(o + kObjVel, &p.vel) || !rd(o + kObjAngles, &p.ang) || !rd(o + kObjRates, &p.rates))
        return false;
    net::quantise(p, q);
    return true;
}

struct Build {
    net::HandoverSnapshot* s;
};
void collect(uint8_t kind, uint16_t a, uint16_t b, void* obj, void* ctx) {
    net::HandoverSnapshot& s = *static_cast<Build*>(ctx)->s;
    const BYTE* o = static_cast<const BYTE*>(obj);
    int32_t level = 0;
    if (!rd(o + jet::kDamage, &level)) return;
    if (level >= 5) {
        s.dead.push_back(net::HandoverDead{kind, a, b});
        return;
    }
    if (kind == 1) {
        net::HandoverAircraft x{};
        if (!read_pose(o, &x.pose)) return;
        x.pose.kind = kind;
        x.pose.a = a;
        x.pose.b = b;
        rd(o + jet::kStateWord, &x.state);
        x.damage = static_cast<uint8_t>(level < 0 ? 0 : level);
        const float* in = nullptr;
        float thr = 0;
        if (rd(o + jet::kInputs, &in) && in && rd(in + jet::kInputThrottle, &thr)) x.throttle = net::throttle_to_wire(thr);
        float internal = 0, external = 0;
        if (stores_read_fuel(obj, &internal, &external)) x.fuel = net::fuel_to_wire(internal + external);
        net::LoadoutEntry e[12];
        const size_t n = stores_read_entries(obj, e, 12);
        for (size_t i = 0; i < n && x.stations < 8; ++i) {
            x.station[x.stations] = e[i].station;
            x.count[x.stations++] = e[i].count;
        }
        s.aircraft.push_back(x);
    } else {
        net::HandoverVehicle x{};
        if (!read_pose(o, &x.pose)) return;
        x.pose.kind = kind;
        x.pose.a = a;
        x.pose.b = b;
        float speed = 0, heading = 0;
        rd(o + kObjSpeed, &speed);
        rd(o + kObjHeading, &heading);
        const double q = std::round(static_cast<double>(speed) * 20.0);
        x.speed = static_cast<int16_t>(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
        x.heading = ang16(heading);
        x.damage = static_cast<uint8_t>(level < 0 ? 0 : level);
        s.vehicles.push_back(x);
    }
}

net::HandoverSnapshot build_snapshot(uint32_t epoch) {
    net::HandoverSnapshot s;
    s.head.epoch = epoch;
    s.head.t_ms = static_cast<uint32_t>(sim_game_time() * 1000.0);
    float t10 = 0, t14 = 0;
    if (world_mission_clock(&t10, &t14)) {
        s.head.clock_ms = net::clock_to_wire(t10);
        s.head.clock2_ms = net::clock_to_wire(t14);
    }
    Build b{&s};
    world_for_each_object(collect, &b);
    const int flights = world_flight_count();
    for (int i = 0; i < flights; ++i) {
        int wp = -1;
        if (world_flight_waypoint(i, &wp, false, 0) && wp >= 0 && wp < 256)
            s.flights.push_back(net::HandoverFlight{static_cast<uint16_t>(i), static_cast<uint8_t>(wp)});
    }
    return s;
}

struct Applied {
    int aircraft = 0, vehicles = 0, flights = 0, missing = 0, destroyed = 0;
};

// Writes a snapshot into our world, `age` seconds after it was taken. The objects keep their REMOTEOBJECT state.
Applied apply_snapshot(const net::HandoverSnapshot& s, double age) {
    Applied r;
    if (!(age > 0)) age = 0;
    if (age > 2.0) age = 2.0;
    const uint32_t player = world_player_id();
    for (const net::HandoverAircraft& x : s.aircraft) {
        uint32_t id = 0xffffffffu;
        BYTE* o = static_cast<BYTE*>(world_object_of_key(x.pose.kind, x.pose.a, x.pose.b, &id));
        // Never our own jet (the offerer ran our seat with its AI) nor a player's avatar; nor one destroyed here.
        int32_t here = 0;
        if (!o || id == player || host_drives_object(o) || client_drives_object(o) || !rd(o + jet::kDamage, &here) ||
            here >= 5) {
            ++r.missing;
            continue;
        }
        net::ObjPose p;
        net::dequantise(x.pose, &p);
        for (int i = 0; i < 3; ++i) p.pos[i] += p.vel[i] * age;
        wr(o + kObjPos, p.pos);
        wr(o + kObjVel, p.vel);
        wr(o + kObjAngles, p.ang);
        wr(o + kObjRates, p.rates);
        uint32_t state = 0;
        if (rd(o + jet::kStateWord, &state)) wr(o + jet::kStateWord, (state & ~kStateKeep) | (x.state & kStateKeep));
        int32_t level = 0;
        if (rd(o + jet::kDamage, &level) && x.damage > level) wr(o + jet::kDamage, static_cast<int32_t>(x.damage));
        float* in = nullptr;
        if (rd(o + jet::kInputs, &in) && in) wr(in + jet::kInputThrottle, net::throttle_from_wire(x.throttle));
        float internal = 0, external = 0;
        if (stores_read_fuel(o, &internal, &external) && internal + external > 0) {
            const float want = net::fuel_from_wire(x.fuel), k = want / (internal + external);
            stores_set_fuel(o, internal * k, external * k);
        }
        if (x.stations) {
            net::LoadoutEntry have[12];
            const size_t nh = stores_read_entries(o, have, 12);
            net::LoadoutEntry want[8];
            size_t nw = 0;
            for (uint8_t i = 0; i < x.stations && i < 8; ++i)
                for (size_t j = 0; j < nh; ++j)
                    if (have[j].station == x.station[i]) {
                        want[nw] = have[j];
                        want[nw++].count = x.count[i];
                    }
            stores_apply_entries(o, want, nw, "handover stores");
        }
        ++r.aircraft;
    }
    for (const net::HandoverVehicle& x : s.vehicles) {
        BYTE* o = static_cast<BYTE*>(world_object_of_key(x.pose.kind, x.pose.a, x.pose.b, nullptr));
        int32_t here = 0;
        if (!o || !rd(o + jet::kDamage, &here) || here >= 5) {
            ++r.missing;
            continue;
        }
        net::ObjPose p;
        net::dequantise(x.pose, &p);
        for (int i = 0; i < 3; ++i) p.pos[i] += p.vel[i] * age;
        wr(o + kObjPos, p.pos);
        wr(o + kObjVel, p.vel);
        wr(o + kObjAngles, p.ang);
        wr(o + kObjSpeed, static_cast<float>(x.speed / 20.0));
        wr(o + kObjHeading, ang_back(x.heading));
        ++r.vehicles;
    }
    for (const net::HandoverDead& d : s.dead) {
        uint32_t id = 0xffffffffu;
        BYTE* o = static_cast<BYTE*>(world_object_of_key(d.kind, d.a, d.b, &id));
        int32_t here = 0;
        if (!o || id == player || host_drives_object(o) || client_drives_object(o) || !rd(o + jet::kDamage, &here) ||
            here >= 5)
            continue;
        if (world_destroy_object(o) == 0) ++r.destroyed;
    }
    for (const net::HandoverFlight& f : s.flights) {
        int wp = -1;
        if (world_flight_waypoint(f.slot, &wp, true, f.waypoint)) ++r.flights;
    }
    const float t10 = static_cast<float>(s.head.clock_ms / 1000.0 + age), t14 = static_cast<float>(s.head.clock2_ms / 1000.0 + age);
    if (s.head.clock_ms) world_set_mission_clock(t10, t14);
    return r;
}

// ---- the client's offer and the host's answer ----

std::deque<std::vector<uint8_t>> g_out;  // the offer's messages not yet queued (paced: the out queue holds 64)
uint32_t g_epoch_seq = 0;
uint32_t g_owned_epoch = 0xffffffffu;  // the player epoch in which we ran the world and offered it

uint32_t new_epoch() { return (sim_player_epoch() << 16) | (++g_epoch_seq & 0xffff); }

bool queue_offer(const net::HandoverSnapshot& s) {
    const std::vector<uint8_t> raw = net::serialize(s);
    std::vector<uint8_t> packed;
    uint32_t crc = 0;
    if (!net::pack_handover(raw, &packed, &crc)) return false;
    net::HandoverBeginMsg b{};
    b.epoch = s.head.epoch;
    b.packed = static_cast<uint32_t>(packed.size());
    b.raw = static_cast<uint32_t>(raw.size());
    b.chunks = static_cast<uint16_t>((packed.size() + net::kHandoverChunkData - 1) / net::kHandoverChunkData);
    b.crc = crc;
    b.clock_ms = s.head.clock_ms;
    std::vector<uint8_t> m(sizeof(b));
    net::encode(b, m.data(), m.size());
    g_out.push_back(std::move(m));
    for (uint16_t i = 0; i < b.chunks; ++i) {
        net::HandoverChunkMsg c{};
        c.epoch = b.epoch;
        c.index = i;
        const size_t off = static_cast<size_t>(i) * net::kHandoverChunkData;
        c.len = static_cast<uint8_t>(packed.size() - off < net::kHandoverChunkData ? packed.size() - off : net::kHandoverChunkData);
        std::memcpy(c.data, packed.data() + off, c.len);
        std::vector<uint8_t> cm(sizeof(c));
        net::encode(c, cm.data(), cm.size());
        g_out.push_back(std::move(cm));
    }
    LOG("handover: our world offered to the host: %zu aircraft, %zu vehicles, %zu flights, clock %.1f s; %zu bytes, %u "
        "bytes packed in %u chunks",
        s.aircraft.size(), s.vehicles.size(), s.flights.size(), s.head.clock_ms / 1000.0, raw.size(),
        static_cast<unsigned>(b.packed), static_cast<unsigned>(b.chunks));
    return true;
}

net::HandoverAssembly g_in[4];  // the host: per client slot
net::OfferArbiter g_arbiter;
uint32_t g_arbiter_epoch = 0xffffffffu;
double wall_s() { return static_cast<double>(GetTickCount64()) / 1000.0; }

void answer(int slot, uint32_t epoch, bool taken) {
    net::HandoverDoneMsg d{};
    d.epoch = epoch;
    d.taken = taken ? 1 : 0;
    uint8_t buf[sizeof(d)];
    const size_t n = net::encode(d, buf, sizeof(buf));
    if (n) host_world_send_to(slot, buf, n, true);
}

// ---- the solo probe (Ctrl+Shift+C) ----

std::atomic<bool> g_probe_request{false};
net::HandoverSnapshot g_probe_snap;
bool g_probe_marked = false;
double g_probe_log_at = -1;
uint32_t g_probe_epoch = 0;

void log_ai(const char* when) {
    struct Ctx {
        int n = 0;
        const char* when;
    } c{0, when};
    world_for_each_object(
        [](uint8_t kind, uint16_t a, uint16_t b, void* obj, void* v) {
            Ctx& x = *static_cast<Ctx*>(v);
            if (kind != 1 || x.n >= 4) return;
            const BYTE* ai = static_cast<const BYTE*>(obj) + kObjAi;
            int32_t task = -1, man = -1, wp = -1;
            rd(ai + kAiTask, &task);
            rd(ai + kAiManoeuvre, &man);
            rd(ai + kAiWaypoint, &wp);
            int fwp = -1;
            world_flight_waypoint(a, &fwp, false, 0);
            LOG("handover probe %s: A %03u.%02u task %d manoeuvre %d waypoint %d (its flight's %d)", x.when,
                static_cast<unsigned>(a), static_cast<unsigned>(b), task, man, wp, fwp);
            ++x.n;
        },
        &c);
}

void probe_step() {
    if (g_probe_epoch != sim_player_epoch()) {
        g_probe_epoch = sim_player_epoch();
        g_probe_marked = false;
        g_probe_log_at = -1;
    }
    if (g_probe_log_at >= 0 && sim_game_time() >= g_probe_log_at) {
        g_probe_log_at = -1;
        log_ai("10 s after the hand-back");
    }
    if (!g_probe_request.exchange(false)) return;
    if (!g_probe_marked) {
        g_probe_snap = build_snapshot(new_epoch());
        const int r = world_set_remote_all(true);
        g_probe_marked = r > 0;
        LOG("handover probe: captured %zu aircraft, %zu vehicles, %zu flights at clock %.1f s; world marked remote: %s",
            g_probe_snap.aircraft.size(), g_probe_snap.vehicles.size(), g_probe_snap.flights.size(),
            g_probe_snap.head.clock_ms / 1000.0, g_probe_marked ? "yes" : "NO");
        return;
    }
    const Applied a = apply_snapshot(g_probe_snap, 0.0);
    const int n = world_set_remote_all(false);
    g_probe_marked = false;
    LOG("handover probe: rewound to the capture (%d aircraft, %d vehicles, %d flights, %d missing) and handed %d "
        "object(s) back to their AI",
        a.aircraft, a.vehicles, a.flights, a.missing, n);
    log_ai("at the hand-back");
    g_probe_log_at = sim_game_time() + 10.0;
}

void step_cb(void*, float, bool is_player) {
    if (!is_player) return;
    probe_step();
    if (g_arbiter_epoch != sim_player_epoch()) {
        g_arbiter_epoch = sim_player_epoch();
        g_arbiter.reset();
        for (auto& a : g_in) a.reset();
    }
    const int late = g_arbiter.on_tick(wall_s());
    if (late >= 0) {
        LOG("handover: client %d's offer did not complete within %.0f s; dropped", late + 1, net::OfferArbiter::kTimeout);
        const uint32_t epoch = g_in[late].offer().epoch;
        g_in[late].reset();
        answer(late, epoch, false);
    }
    // The client's paced offer: a few messages a step, in order.
    for (int i = 0; i < kChunksPerStep && !g_out.empty(); ++i) {
        if (!client_world_send(g_out.front().data(), g_out.front().size(), true)) break;
        g_out.pop_front();
    }
}

}  // namespace

void handover_probe_request() { g_probe_request.store(true); }

void handover_on_modules_loaded() {
    static bool once = false;
    if (once) return;
    once = true;
    if (!sim_register_step_callback(step_cb)) LOG("handover: no free step callback slot; no handover");
}

bool handover_we_owned_world() { return g_owned_epoch == sim_player_epoch(); }

void handover_session_end() {
    if (!g_out.empty()) LOG("handover: the session ended with %zu offer message(s) unsent; dropped", g_out.size());
    g_out.clear();
}

void handover_host_forget(int slot) {
    if (slot < 0 || slot > 3) return;
    if (g_in[slot].active()) LOG("handover: client %d left during its offer; dropped", slot + 1);
    g_in[slot].reset();
    g_arbiter.on_disconnect(slot);
}

void handover_client_welcomed() {
    if (!world_remote_deferred()) return;
    // We ran the world: offer it to the host, then follow (our copies coast until the host's stream drives them).
    g_owned_epoch = sim_player_epoch();
    g_out.clear();
    if (!queue_offer(build_snapshot(new_epoch()))) LOG("handover: our world could not be packed; not offered");
    world_remote_now();
}

void handover_host_message(int slot, const uint8_t* data, size_t len, double rtt_s) {
    if (slot < 0 || slot > 3) return;
    const uint8_t wt = net::peek_world_type(data, len);
    if (wt == static_cast<uint8_t>(net::WorldType::HandoverBegin)) {
        net::HandoverBeginMsg b;
        if (net::decode(data, len, b) != net::Status::Ok) return;
        float t10 = 0, t14 = 0;
        const uint32_t own = world_mission_clock(&t10, &t14) ? net::clock_to_wire(t10) : 0;
        const net::OfferArbiter::Verdict v = g_arbiter.on_begin(slot, b.clock_ms, own, wall_s());
        const bool ok = v == net::OfferArbiter::Verdict::Take && g_in[slot].begin(b);
        if (!ok) {
            if (v == net::OfferArbiter::Verdict::Take) g_arbiter.on_complete(slot, false);
            LOG("handover: client %d offers its world (clock %.1f s, ours %.1f s): %s", slot + 1, b.clock_ms / 1000.0,
                own / 1000.0,
                v == net::OfferArbiter::Verdict::RefuseOwner  ? "refused, we loaded first and own the world"
                : v == net::OfferArbiter::Verdict::RefuseBusy ? "refused, another client's offer is being taken"
                : v == net::OfferArbiter::Verdict::RefuseDone ? "refused, a world was taken over already this flight"
                                                              : "refused (bad offer)");
            g_in[slot].reset();
            answer(slot, b.epoch, false);
            return;
        }
        LOG("handover: client %d loaded first (clock %.1f s, ours %.1f s): taking its world, %u chunks", slot + 1,
            b.clock_ms / 1000.0, own / 1000.0, static_cast<unsigned>(b.chunks));
        return;
    }
    if (wt != static_cast<uint8_t>(net::WorldType::HandoverChunk)) return;
    net::HandoverChunkMsg c;
    if (net::decode(data, len, c) != net::Status::Ok || !g_in[slot].active()) return;
    if (!g_in[slot].add(c)) return;
    const net::HandoverBeginMsg b = g_in[slot].offer();
    std::vector<uint8_t> raw;
    net::HandoverSnapshot s;
    const bool ok = net::unpack_handover(g_in[slot].bytes(), b.crc, b.raw, &raw) &&
                    net::deserialize(raw.data(), raw.size(), &s);
    g_in[slot].reset();
    if (!ok) {
        g_arbiter.on_complete(slot, false);
        LOG("handover: client %d's world did not unpack (CRC or sizes); refused", slot + 1);
        answer(slot, b.epoch, false);
        return;
    }
    const Applied a = apply_snapshot(s, rtt_s * 0.5);
    g_arbiter.on_complete(slot, true);
    LOG("handover: client %d's world taken over: %d aircraft, %d vehicles, %d flights written, %d destroyed as there "
        "(%d not found or destroyed here), the mission clock set to %.1f s; we own the world and stream it",
        slot + 1, a.aircraft, a.vehicles, a.flights, a.destroyed, a.missing, s.head.clock_ms / 1000.0);
    answer(slot, b.epoch, true);
}

void handover_client_message(const uint8_t* data, size_t len) {
    net::HandoverDoneMsg d;
    if (net::decode(data, len, d) != net::Status::Ok) return;
    LOG("handover: the host %s our world (epoch 0x%08lx)",
        d.taken ? "took over" : "refused (it loaded first and owns the world)", static_cast<unsigned long>(d.epoch));
}

}  // namespace LazyWorks::SF2Patch::hooks
