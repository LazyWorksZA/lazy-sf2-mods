// Hits across PCs (Phase 5 T3, T4). The shooter's PC claims each hit of its player's guns and unguided stores
// (bombs, rockets) on an object it does not own (a REMOTEOBJECT copy, whose damage the remote block drops); the host
// checks the claim and applies it to its own object, with the claimer's avatar as the attacker, or forwards it to the
// PC that owns the target when that is a player's jet. On the host, hits on a client's avatar go to that client the
// same way, except a player's guided missile: the target's PC flies the live copy of it (weapon_net.cpp). Game thread only: the damage calls come from the objects' updates. See lazy-sf2-recon: network-recon §9.5.
#include <windows.h>

#include <cmath>
#include <cstring>

#include "hooks/hooks.h"
#include "hooks/world_logic.h"
#include "log.h"
#include "net/hit_logic.h"
#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

struct Stats {
    uint64_t claims = 0, claim_fail = 0, forwarded = 0, forward_fail = 0, applied = 0, refused = 0, applied_here = 0;
};
Stats g_st;
uint16_t g_seq = 0;
net::HitRate g_gun_rate[4], g_store_rate[4];  // per claimer slot (client 1..4)
net::ShotLedger g_ledger[4];                   // per claimer slot: its recent shots
net::HitLimits g_limits;
const net::HitLimits g_store_limits = net::store_limits();

float as_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
uint32_t bits_of(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
uint32_t to_u32(double v, double scale) {
    if (!(v > 0)) return 0;
    const double x = v * scale + 0.5;
    return x >= 4294967295.0 ? 0xffffffffu : static_cast<uint32_t>(x);
}
double wall_s() { return static_cast<double>(GetTickCount64()) / 1000.0; }

bool read_d3(uint32_t p, double out[3]) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(p)), 3 * sizeof(double));
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool read_f3(uint32_t p, float out[3]) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(p)), 3 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The damage call's arguments as a message, positions relative to the target at tpos. False if unreadable.
bool hit_of_call(int slot, const uint32_t* args, const double tpos[3], net::HitMsg* m) {
    *m = net::HitMsg{};
    m->slot = static_cast<uint8_t>(slot);
    if (!net::hit_weapon_to_wire(args[4], &m->weapon)) return false;
    double p0[3] = {}, p1[3] = {};
    if (!read_d3(args[0], p0)) return false;
    if (slot == 4) {
        if (!read_d3(args[1], p1)) return false;
        m->energy = to_u32(as_float(args[2]), 1.0);
        for (int i = 0; i < 3; ++i) m->p1[i] = net::hit_rel_to_wire(p1[i] - tpos[i]);
    } else {
        m->energy = to_u32(as_float(args[1]), 1.0);
        m->radius2 = to_u32(as_float(args[2]), 1.0e4);
        m->warhead = static_cast<uint8_t>(args[5]);
        if (slot == 5) {
            float v[3] = {};
            if (!read_f3(args[6], v)) return false;
            for (int i = 0; i < 3; ++i) {
                const double q = std::round(static_cast<double>(v[i]) * 20.0);
                m->p1[i] = static_cast<int16_t>(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
            }
            m->flag = static_cast<uint8_t>(args[7]);
        }
    }
    for (int i = 0; i < 3; ++i) m->p0[i] = net::hit_rel_to_wire(p0[i] - tpos[i]);
    return true;
}

// The message as a damage call on target (at tpos), with shooter as the attacker. 0 done, 1 refused, 2 faulted.
int apply_hit(void* target, const double tpos[3], const net::HitMsg& m, uint32_t shooter) {
    double p0[3], p1[3];
    float v[3];
    for (int i = 0; i < 3; ++i) {
        p0[i] = tpos[i] + net::hit_rel_from_wire(m.p0[i]);
        p1[i] = tpos[i] + net::hit_rel_from_wire(m.p1[i]);
        v[i] = static_cast<float>(m.p1[i] / 20.0);
    }
    const uint32_t ap0 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p0));
    const uint32_t weapon = net::hit_weapon_from_wire(m.weapon);
    const uint32_t energy = bits_of(static_cast<float>(m.energy));
    const uint32_t r2 = bits_of(static_cast<float>(m.radius2 / 1.0e4));
    uint32_t args[8] = {};
    if (m.slot == 4) {
        const uint32_t a[5] = {ap0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p1)), energy, shooter, weapon};
        std::memcpy(args, a, sizeof(a));
    } else {
        const uint32_t a[8] = {ap0, energy, r2, shooter, weapon, m.warhead,
                               static_cast<uint32_t>(reinterpret_cast<uintptr_t>(v)), m.flag};
        std::memcpy(args, a, sizeof(a));
    }
    return world_damage_call(target, m.slot, args);
}

double dist3(const double a[3], const double b[3]) {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

bool send_apply_to(int slot, net::HitMsg m) {
    m.peer = 0;
    m.seq = ++g_seq;
    uint8_t buf[sizeof(net::HitMsg)];
    const size_t n = net::encode_hit(m, net::WorldType::HitApply, buf, sizeof(buf));
    return n && host_world_send_to(slot, buf, n, true);
}

}  // namespace

bool hits_on_damage(void* obj, int slot, const uint32_t* args, bool remote) {
    uint16_t wire = 0;
    if (!net::hit_weapon_to_wire(args[4], &wire)) return false;
    if (remote ? !client_world_active() : !host_world_active()) return false;
    const int s = remote ? -1 : host_avatar_slot(obj);  // the host only takes hits on a client's avatar
    if (!remote && s < 0) return false;
    const bool guided = !net::weapon_is_gun(args[4]) && weapon_guided(args[4]);
    double tpos[3];
    uint32_t tid = 0;
    bool alive = false;
    if (!world_object_state(obj, tpos, &tid, &alive)) return false;
    net::HitMsg m;
    if (!world_key_of_object(tid, &m.target_kind, &m.target_a, &m.target_b)) return false;
    const net::HitMsg key = m;
    if (!hit_of_call(slot, args, tpos, &m)) return false;
    m.target_kind = key.target_kind;
    m.target_a = key.target_a;
    m.target_b = key.target_b;
    if (remote) {
        // A client: only our own player's rounds and unguided stores are claimed (others' copies are visual-only,
        // and a guided missile's live copy flies on its target's PC).
        if (args[3] != world_player_id() || guided) return false;
        m.seq = ++g_seq;
        uint8_t buf[sizeof(net::HitMsg)];
        const size_t n = net::encode_hit(m, net::WorldType::HitClaim, buf, sizeof(buf));
        const bool ok = n && client_world_send(buf, n);
        const uint64_t c = ok ? ++g_st.claims : ++g_st.claim_fail;
        if (log_milestone(c))
            LOG("hits: %s %s hit %llu on %c %03u.%02u (slot %d, %u J)%s", ok ? "claimed" : "could NOT claim",
                net::hit_is_store(m) ? "store" : "gun", static_cast<unsigned long long>(c), m.target_kind == 1 ? 'A' : 'G',
                static_cast<unsigned>(m.target_a), static_cast<unsigned>(m.target_b), slot, static_cast<unsigned>(m.energy),
                ok ? "" : " (the send queue is full)");
        return true;
    }
    // The host: a hit on a client's avatar goes to that client, who owns the jet.
    if (!world_key_of_object(args[3], &m.shooter_kind, &m.shooter_a, &m.shooter_b)) m.shooter_kind = 0;
    if (guided) {
        // A player's guided missile is flown live on the target's PC: dropped here. The host's AI missiles are
        // decided here (their copies do not fly on the client) and forwarded.
        void* sh = world_object_of_key(m.shooter_kind, m.shooter_a, m.shooter_b, nullptr);
        if (args[3] == world_player_id() || (sh && host_avatar_slot(sh) >= 0)) return true;
    }
    const bool ok = send_apply_to(s, m);
    const uint64_t c = ok ? ++g_st.forwarded : ++g_st.forward_fail;
    if (log_milestone(c))
        LOG("hits: %s hit %llu on client %d's avatar %s to its owner", net::hit_is_store(m) ? "store" : "gun",
            static_cast<unsigned long long>(c), s + 1,
            ok ? "forwarded" : "NOT forwarded (queue full)");
    return true;  // dropped here either way: only the owner applies it
}

void hits_host_claim(int claimer_slot, const uint8_t* data, size_t len) {
    net::HitMsg m;
    if (net::decode_hit(data, len, net::WorldType::HitClaim, m) != net::Status::Ok || claimer_slot < 0 ||
        claimer_slot > 3)
        return;
    void* shooter = host_avatar_object(claimer_slot);
    uint32_t target_id = 0;
    void* target = world_object_of_key(m.target_kind, m.target_a, m.target_b, &target_id);
    double tpos[3] = {}, spos[3] = {};
    uint32_t sid = 0xffffffffu, tid2 = 0;
    bool alive = false, salive = false;
    const bool have = target && world_object_state(target, tpos, &tid2, &alive);
    const bool have_shooter = shooter && world_object_state(shooter, spos, &sid, &salive);
    const double d = have && have_shooter ? dist3(tpos, spos) : 1e9;
    const bool store = net::hit_is_store(m);
    net::HitVerdict v = net::check_hit_shot(m, g_ledger[claimer_slot], wall_s());
    if (v == net::HitVerdict::Ok && !have_shooter) v = net::HitVerdict::NoShooter;
    if (v == net::HitVerdict::Ok)
        v = net::check_hit_claim(m, d, have && alive, store ? g_store_rate[claimer_slot] : g_gun_rate[claimer_slot],
                                 wall_s(), store ? g_store_limits : g_limits);
    if (v != net::HitVerdict::Ok) {
        if (log_milestone(++g_st.refused))
            LOG("hits: client %d's hit on %c %03u.%02u refused: %s (%llu refused so far)", claimer_slot + 1,
                m.target_kind == 1 ? 'A' : 'G', static_cast<unsigned>(m.target_a), static_cast<unsigned>(m.target_b),
                net::hit_verdict_name(v), static_cast<unsigned long long>(g_st.refused));
        return;
    }
    const int owner = host_avatar_slot(target);
    if (owner >= 0) {  // another client's jet: its owner applies it
        if (!world_key_of_object(sid, &m.shooter_kind, &m.shooter_a, &m.shooter_b)) m.shooter_kind = 0;
        const bool ok = send_apply_to(owner, m);
        if (log_milestone(ok ? ++g_st.forwarded : ++g_st.forward_fail))
            LOG("hits: client %d's hit on client %d %s", claimer_slot + 1, owner + 1,
                ok ? "forwarded to its owner" : "NOT forwarded (queue full)");
        return;
    }
    const int r = apply_hit(target, tpos, m, sid);
    if (r == 0 && log_milestone(++g_st.applied))
        LOG("hits: client %d's hit applied to %c %03u.%02u (slot %u, %u J, the attacker is its avatar 0x%08lx; "
            "%llu so far)",
            claimer_slot + 1, m.target_kind == 1 ? 'A' : 'G', static_cast<unsigned>(m.target_a),
            static_cast<unsigned>(m.target_b), static_cast<unsigned>(m.slot), static_cast<unsigned>(m.energy),
            static_cast<unsigned long>(sid), static_cast<unsigned long long>(g_st.applied));
    else if (r != 0)
        LOG("hits: client %d's hit NOT applied: %s", claimer_slot + 1,
            r == 1 ? "the target's damage interface is not the one we read" : "the call faulted");
}

void hits_client_apply(const uint8_t* data, size_t len) {
    net::HitMsg m;
    if (net::decode_hit(data, len, net::WorldType::HitApply, m) != net::Status::Ok) return;
    uint32_t tid = 0;
    void* target = world_object_of_key(m.target_kind, m.target_a, m.target_b, &tid);
    if (!target || tid != world_player_id()) {
        LOG("hits: a hit for %c %03u.%02u is not for our jet here; dropped", m.target_kind == 1 ? 'A' : 'G',
            static_cast<unsigned>(m.target_a), static_cast<unsigned>(m.target_b));
        return;
    }
    uint32_t sid = 0xffffffffu;
    if (m.shooter_kind) world_object_of_key(m.shooter_kind, m.shooter_a, m.shooter_b, &sid);
    double tpos[3];
    uint32_t id2 = 0;
    bool alive = false;
    if (!world_object_state(target, tpos, &id2, &alive) || !alive) return;
    const int r = apply_hit(target, tpos, m, sid);
    if (log_milestone(++g_st.applied_here) || r != 0)
        LOG("hits: hit %llu on our jet from %s%03u.%02u: %s", static_cast<unsigned long long>(g_st.applied_here),
            m.shooter_kind == 1 ? "A " : m.shooter_kind == 2 ? "G " : "unknown ", static_cast<unsigned>(m.shooter_a),
            static_cast<unsigned>(m.shooter_b),
            r == 0 ? "applied" : r == 1 ? "NOT applied (damage interface not the one we read)" : "FAULTED");
}

void hits_session_reset() {
    for (int i = 0; i < 4; ++i) hits_forget(i);
}

void hits_forget(int slot) {
    if (slot < 0 || slot > 3) return;
    g_gun_rate[slot].reset();
    g_store_rate[slot].reset();
    g_ledger[slot].reset();
}

void hits_note_guns(int slot, uint8_t guns) {
    if (slot >= 0 && slot <= 3) g_ledger[slot].guns(guns != 0, wall_s());
}

void hits_note_launch(int slot, uint16_t weapon_id) {
    if (slot >= 0 && slot <= 3) g_ledger[slot].launch(weapon_id, wall_s());
}

}  // namespace LazyWorks::SF2Patch::hooks
