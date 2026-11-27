// Seat takeover in place (handover task 3). After each Welcome the host sends the client the current state of the
// aircraft in its seat (the AI flew it until then); a client joining late or reconnecting takes that state over:
// its jet moves to the seat's pose and velocity, and gets its fuel and stores, instead of starting where the mission
// put it. Game thread. See lazy-sf2-recon: network-recon §12.6.
#include <windows.h>

#include <cmath>
#include <cstring>

#include "hooks/hooks.h"
#include "hooks/jet_layout.h"
#include "log.h"
#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

constexpr size_t kObjId = 0x08, kObjPos = 0x38, kObjVel = 0x50, kObjRates = 0xd0, kObjAngles = 0x100;
constexpr uint32_t kStateAirborne = 0x800000u;
uint32_t g_applied_epoch = 0xffffffffu;  // one takeover per flight (a re-Welcome repeats the state)

struct Pose {
    double pos[3];
    float vel[3], ang[3], rates[3];
    int32_t damage;
    uint32_t state;
};
bool read_pose(const void* obj, Pose* p) {
    __try {
        const BYTE* o = static_cast<const BYTE*>(obj);
        std::memcpy(p->pos, o + kObjPos, sizeof(p->pos));
        std::memcpy(p->vel, o + kObjVel, sizeof(p->vel));
        std::memcpy(p->ang, o + kObjAngles, sizeof(p->ang));
        std::memcpy(p->rates, o + kObjRates, sizeof(p->rates));
        std::memcpy(&p->damage, o + jet::kDamage, 4);
        std::memcpy(&p->state, o + jet::kStateWord, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool seat_state_capture(int seat, void* obj, net::SeatStateMsg* out) {
    if (!obj || seat < 0 || seat > 255) return false;
    Pose p;
    if (!read_pose(obj, &p)) return false;
    net::SeatStateMsg& m = *out;
    m = net::SeatStateMsg{};
    m.seat = static_cast<uint8_t>(seat);
    uint32_t id = 0;
    double ignored[3];
    bool alive = false;
    if (world_object_state(obj, ignored, &id, &alive) && alive) m.flags |= net::kSeatAlive;
    if (p.state & kStateAirborne) m.flags |= net::kSeatAirborne;
    m.host_ms = static_cast<uint32_t>(sim_game_time() * 1000.0);
    net::ObjPose op{};
    for (int i = 0; i < 3; ++i) {
        op.pos[i] = p.pos[i];
        op.vel[i] = p.vel[i];
        op.ang[i] = p.ang[i];
        op.rates[i] = p.rates[i];
    }
    net::quantise(op, &m.pose);
    world_key_of_object(id, &m.pose.kind, &m.pose.a, &m.pose.b);
    float internal = 0, external = 0;
    if (stores_read_fuel(obj, &internal, &external)) {
        m.fuel = net::fuel_to_wire(internal);
        m.fuel_ext = net::fuel_to_wire(external);
    }
    m.damage = static_cast<uint8_t>(p.damage < 0 ? 0 : p.damage > 5 ? 5 : p.damage);
    m.n = static_cast<uint8_t>(stores_read_entries(obj, m.e, 12));
    return true;
}

void seat_state_send(int slot, const net::SeatStateMsg& m) {
    uint8_t buf[sizeof(m)];
    const size_t n = net::encode(m, buf, sizeof(buf));
    const bool ok = n && host_world_send_to(slot, buf, n, true);
    LOG("seat: client %d's seat %u: state %s (%s, %u stations, fuel %.0f + %.0f kg)", slot + 1,
        static_cast<unsigned>(m.seat), ok ? "sent" : "NOT sent (queue full)",
        (m.flags & net::kSeatAirborne) ? "airborne" : "on the ground", static_cast<unsigned>(m.n),
        net::fuel_from_wire(m.fuel), net::fuel_from_wire(m.fuel_ext));
}

void seat_state_apply(const uint8_t* data, size_t len, double host_now) {
    net::SeatStateMsg m;
    if (net::decode(data, len, m) != net::Status::Ok) return;
    void* player = world_player_object();
    if (!player || g_applied_epoch == sim_player_epoch()) return;
    Pose own;
    if (!read_pose(player, &own)) return;
    net::ObjPose p{};
    net::dequantise(m.pose, &p);
    double age = host_now - m.host_ms / 1000.0;
    if (!(age > 0)) age = 0;
    if (age > 2.0) age = 2.0;
    for (int i = 0; i < 3; ++i) p.pos[i] += p.vel[i] * age;
    const double dx = p.pos[0] - own.pos[0], dy = p.pos[1] - own.pos[1], dz = p.pos[2] - own.pos[2];
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    g_applied_epoch = sim_player_epoch();
    if (!net::seat_takeover_due(m, dist)) {
        LOG("seat: our seat's state from the host: %s, %.0f m away: no takeover",
            !(m.flags & net::kSeatAlive) ? "destroyed" : !(m.flags & net::kSeatAirborne) ? "on the ground" : "airborne",
            dist);
        return;
    }
    client_place_player(player, p.pos, p.vel, p.ang);
    stores_set_fuel(player, net::fuel_from_wire(m.fuel), net::fuel_from_wire(m.fuel_ext));
    stores_apply_entries(player, m.e, m.n, "our seat's stores (takeover)");
    LOG("seat: took over our seat in place: moved %.0f m to (%.0f, %.0f, %.0f), speed %.0f m/s, fuel %.0f + %.0f kg, "
        "%u stations; the host's damage level there is %u",
        dist, p.pos[0], p.pos[1], p.pos[2],
        std::sqrt(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2]), net::fuel_from_wire(m.fuel),
        net::fuel_from_wire(m.fuel_ext), static_cast<unsigned>(m.n), static_cast<unsigned>(m.damage));
}

}  // namespace LazyWorks::SF2Patch::hooks
