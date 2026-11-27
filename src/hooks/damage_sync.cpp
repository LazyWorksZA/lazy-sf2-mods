// Damage, death, ejection and countermeasures across PCs (Phase 5 T6). The owner polls its jet 4 times a second
// (damage level, fire, pilot gone, parts lost or showing damage, systems destroyed, the last attacker) and sends
// what changed; the avatar mirrors it through the game's own remote-aircraft events (3 part lost, 0xc part shows
// damage, 4 system destroyed, 8 level, 0xb destroy). Chaff and flare releases go out as they happen and are queued on
// the avatar's dispenser, which releases them itself. Two hooks keep copies honest: an avatar's incoming-missile
// notice no longer leaves the auto-dispense bits set, and on a joined client a remote copy's death makes no kill
// entry (only the owner's and the host's records count). Game thread. See lazy-sf2-recon: network-recon §11.4, §11.5.
#include <windows.h>

#include <bit>
#include <cstring>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/jet_layout.h"
#include "hooks/jet_mirror.h"
#include "hooks/world_logic.h"
#include "log.h"
#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::hooks {

void* g_o_aircraft_kill_event = nullptr;
void* g_o_incoming_missile = nullptr;

namespace {

constexpr double kPollEvery = 0.25, kResendEvery = 10.0;  // game seconds
constexpr uint32_t kRvaDispenser = 0x9335c;
constexpr size_t kObjAttacker = 0x318, kObjAttackerWeapon = 0x31c, kCompCount = 0x330, kRequestCmBits = 0x6;  // +0x5f4: 2 flares, 4 chaff
constexpr uint32_t kFlagFire = 0x10, kFlagKillLogged = 0x80000000u, kStatePilotGone = 0x2000000u;
constexpr int kMaxSys = 256;

uintptr_t ao_base() {
    static HMODULE seen = nullptr;
    static uintptr_t base = 0;
    HMODULE m = GetModuleHandleW(L"AircraftObject.dll");
    if (m != seen) {
        seen = m;
        base = m && is_expected_build(m, jet::kAoStamp, jet::kAoSize, "AircraftObject.dll (damage)")
                   ? reinterpret_cast<uintptr_t>(m)
                   : 0;
    }
    return base;
}

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

// The owner's view of its jet.
bool read_damage(void* obj, uintptr_t ao, net::DamageStateMsg* m) {
    BYTE* o = static_cast<BYTE*>(obj);
    int32_t level = 0, ncomp = 0;
    uint32_t flags = 0, state = 0, attacker = 0xffffffffu;
    BYTE* const* comps = nullptr;
    if (!rd(o + jet::kDamage, &level) || !rd(o + jet::kFlags, &flags) || !rd(o + jet::kStateWord, &state) ||
        !rd(o + kCompCount, &ncomp) || !rd(o + jet::kComponents + jet::kListArray, &comps))
        return false;
    rd(o + kObjAttacker, &attacker);
    rd(o + kObjAttackerWeapon, &m->attacker_weapon);
    m->life = static_cast<uint16_t>(sim_player_epoch());
    m->level = static_cast<uint8_t>(level < 0 ? 0 : level > 5 ? 5 : level);
    m->flags = static_cast<uint8_t>(((flags & kFlagFire) ? net::kDmgFire : 0) |
                                    ((state & kStatePilotGone) ? net::kDmgPilotGone : 0) |
                                    (level >= 5 ? net::kDmgDestroyed : 0));
    m->components = static_cast<uint8_t>(ncomp < 0 ? 0 : ncomp > 64 ? 64 : ncomp);
    for (int i = 0; comps && i < m->components; ++i) {
        BYTE* c = nullptr;
        int32_t attached = 1, shows = 0;
        if (!rd(comps + i, &c) || !c) continue;
        rd(c + jet::kCompAttached, &attached);
        rd(c + jet::kCompShowsDamage, &shows);
        if (!attached) m->lost |= 1ull << i;
        if (shows) m->shown |= 1ull << i;
    }
    SysRef sys[kMaxSys];
    const int n = jet_list_systems(obj, ao, sys, kMaxSys);
    m->systems = static_cast<uint8_t>(n > 64 ? 64 : n);
    for (int i = 0; i < m->systems; ++i) {
        int32_t working = 1;
        if (rd(sys[i].sys + jet::kSysWorking, &working) && !working) m->dead |= 1ull << i;
    }
    if (!world_key_of_object(attacker, &m->attacker_kind, &m->attacker_a, &m->attacker_b)) m->attacker_kind = 0;
    return true;
}

struct Tx {
    uint32_t epoch = 0;
    double next_poll = 0, next_resend = 0;
    net::DamageStateMsg last{};
    bool have = false;
    int chaff = -1, flares = -1;
    uint16_t seq = 0;
};
Tx g_tx;

bool send(const uint8_t* d, size_t n) { return host_world_active() ? host_world_send_all(d, n, true) : client_world_send(d, n, true); }

struct Rx {
    uint32_t epoch = 0;
    net::PeerDamage peer[5];
    uint64_t events = 0, cm = 0, kills_blocked = 0, notices = 0, dead_ignored = 0;
};
Rx g_rx;

int event(void* avatar, int type, int a = 0, int b = 0, int c = 0) {
    int payload[4] = {a, b, c, 0};
    return aircraft_remote_event(avatar, type, payload);
}

void apply_damage(const net::DamageStateMsg& m, BYTE* av, uintptr_t ao) {
    const int peer = m.peer < 5 ? m.peer : 4;
    int32_t level = 0;
    const bool already = av && rd(av + jet::kDamage, &level) && level >= 5;
    const bool was_dead = g_rx.peer[peer].dead();
    const net::PeerDamage::Step st = g_rx.peer[peer].on_state(m, av, already);
    if (!st.apply) {
        if (was_dead && av && log_milestone(++g_rx.dead_ignored))
            LOG("damage: peer %u's jet was destroyed; its later states are not applied (%llu so far)",
                static_cast<unsigned>(m.peer), static_cast<unsigned long long>(g_rx.dead_ignored));
        return;
    }
    const net::DamagePlan& p = st.plan;
    if (m.attacker_kind) {
        uint32_t id = 0xffffffffu;
        if (world_object_of_key(m.attacker_kind, m.attacker_a, m.attacker_b, &id)) {
            wr(av + kObjAttacker, id);
            wr(av + kObjAttackerWeapon, m.attacker_weapon);
        }
    }
    int sent = 0, refused = 0;
    auto ev = [&](int type, int a = 0, int b = 0, int c = 0) { (event(av, type, a, b, c) == 0 ? sent : refused)++; };
    for (int i = 0; i < 64; ++i)
        if (p.lose >> i & 1) ev(3, i, 1);
    for (int i = 0; i < 64; ++i)
        if (p.show >> i & 1) ev(0xc, i);
    if (p.kill_systems) {
        SysRef sys[kMaxSys];
        const int n = jet_list_systems(av, ao, sys, kMaxSys);
        for (int i = 0; i < n && i < 64; ++i)
            if (p.kill_systems >> i & 1) ev(4, sys[i].comp, sys[i].index, 1);
    }
    if (p.set_level) ev(8, p.level);
    uint32_t flags = 0, state = 0;
    if ((p.fire_on || p.fire_off) && rd(av + jet::kFlags, &flags))
        wr(av + jet::kFlags, p.fire_on ? flags | kFlagFire : flags & ~kFlagFire);
    if (p.pilot_gone && rd(av + jet::kStateWord, &state)) wr(av + jet::kStateWord, state | kStatePilotGone);
    if (p.destroy) ev(0xb);
    g_rx.events += static_cast<uint64_t>(sent);
    if (p.destroy || p.pilot_gone || refused || p.lose || log_milestone(g_rx.events))
        LOG("damage: peer %u's jet: level %u%s%s%s, %d part(s) lost, %d event(s) applied%s", static_cast<unsigned>(m.peer),
            static_cast<unsigned>(m.level), (m.flags & net::kDmgFire) ? ", on fire" : "",
            p.pilot_gone ? ", the pilot is gone (ejected or killed)" : "", p.destroy ? ", DESTROYED (no kill entry here)" : "",
            std::popcount(p.lose), sent, refused ? "; some REFUSED (not the interface we read)" : "");
}

void apply_cm(const net::CountermeasureMsg& m, BYTE* av, uintptr_t ao) {
    SysRef sys[kMaxSys];
    const int n = jet_list_systems(av, ao, sys, kMaxSys);
    for (int i = 0; i < n; ++i) {
        if (sys[i].vt != kRvaDispenser) continue;
        int16_t chaff = 0, flares = 0;
        if (!rd(sys[i].sys + jet::kDispChaffDue, &chaff) || !rd(sys[i].sys + jet::kDispFlaresDue, &flares)) return;
        wr(sys[i].sys + jet::kDispChaffDue, static_cast<int16_t>(chaff + m.chaff));
        wr(sys[i].sys + jet::kDispFlaresDue, static_cast<int16_t>(flares + m.flares));
        if (log_milestone(++g_rx.cm))
            LOG("countermeasures: peer %u released %u chaff, %u flares; queued on its avatar (%llu so far)",
                static_cast<unsigned>(m.peer), static_cast<unsigned>(m.chaff), static_cast<unsigned>(m.flares),
                static_cast<unsigned long long>(g_rx.cm));
        return;
    }
    if (log_milestone(++g_rx.cm)) LOG("countermeasures: peer %u's avatar has no dispenser here", static_cast<unsigned>(m.peer));
}

// Every dispenser of the jet still works (a destroyed one drops its counts without releasing anything).
bool dispensers_work(void* obj, uintptr_t ao) {
    SysRef sys[kMaxSys];
    const int n = jet_list_systems(obj, ao, sys, kMaxSys);
    int found = 0;
    for (int i = 0; i < n; ++i) {
        if (sys[i].vt != kRvaDispenser) continue;
        int32_t working = 0;
        if (!rd(sys[i].sys + jet::kSysWorking, &working) || !working) return false;
        ++found;
    }
    return found > 0;
}

bool is_remote_copy(void* obj) {
    uint32_t flags = 0;
    return rd(static_cast<BYTE*>(obj) + 0x0c, &flags) && (flags & world_remote_mask());
}

}  // namespace

void damage_tick(void* player) {
    const uintptr_t ao = ao_base();
    if (!player || !ao) return;
    const double now = sim_game_time();
    if (g_tx.epoch != sim_player_epoch()) {
        g_tx = Tx{};
        g_tx.epoch = sim_player_epoch();
    }
    // Countermeasures every step: a release goes out at once.
    int16_t chaff = 0, flares = 0;
    BYTE* o = static_cast<BYTE*>(player);
    if (rd(o + jet::kChaffTotal, &chaff) && rd(o + jet::kFlareTotal, &flares)) {
        if (g_tx.chaff >= 0 && (chaff < g_tx.chaff || flares < g_tx.flares)) {
            int32_t level = 0;
            const bool alive = rd(o + jet::kDamage, &level) && level < 5;
            net::CountermeasureMsg m{};
            const bool ok = dispensers_work(player, ao);
            m.chaff = net::cm_released(g_tx.chaff, chaff, alive, ok);
            m.flares = net::cm_released(g_tx.flares, flares, alive, ok);
            if (m.chaff || m.flares) {
                m.seq = ++g_tx.seq;
                uint8_t buf[sizeof(m)];
                const size_t n = net::encode(m, buf, sizeof(buf));
                if (n) send(buf, n);
            }
        }
        g_tx.chaff = chaff;
        g_tx.flares = flares;
    }
    if (now < g_tx.next_poll) return;
    g_tx.next_poll = now + kPollEvery;
    net::DamageStateMsg m{};
    if (!read_damage(player, ao, &m)) return;
    if (g_tx.have && !net::damage_differs(m, g_tx.last) && now < g_tx.next_resend) return;
    m.seq = ++g_tx.seq;
    uint8_t buf[sizeof(m)];
    const size_t n = net::encode(m, buf, sizeof(buf));
    if (!n || !send(buf, n)) return;
    if (!g_tx.have || net::damage_differs(m, g_tx.last))
        LOG("damage: our jet: level %u, flags 0x%02x, %d of %u part(s) lost, %d system(s) destroyed (sent)",
            static_cast<unsigned>(m.level), static_cast<unsigned>(m.flags), std::popcount(m.lost),
            static_cast<unsigned>(m.components), std::popcount(m.dead));
    g_tx.last = m;
    g_tx.have = true;
    g_tx.next_resend = now + kResendEvery;
}

void damage_apply(const uint8_t* data, size_t len, void* avatar) {
    const uintptr_t ao = ao_base();
    if (!ao) return;
    if (g_rx.epoch != sim_player_epoch()) {
        g_rx = Rx{};
        g_rx.epoch = sim_player_epoch();
    }
    const uint8_t wt = net::peek_world_type(data, len);
    if (wt == static_cast<uint8_t>(net::WorldType::DamageState)) {
        net::DamageStateMsg m;
        if (net::decode(data, len, m) == net::Status::Ok) apply_damage(m, static_cast<BYTE*>(avatar), ao);
    } else if (avatar && wt == static_cast<uint8_t>(net::WorldType::Countermeasure)) {
        net::CountermeasureMsg m;
        if (net::decode(data, len, m) == net::Status::Ok) apply_cm(m, static_cast<BYTE*>(avatar), ao);
    }
}

bool damage_peer_dead(int peer) {
    if (g_rx.epoch != sim_player_epoch()) return false;
    return peer >= 0 && peer < 5 && g_rx.peer[peer].dead();
}

// The ground classes' kill event (GroundObject +0x15aa0, GroundMobileObject +0xd290, NavalObject +0x12620, each
// __fastcall(obj), the same as the aircraft's): on a joined client a remote copy logs no kill.
void* g_o_ground_kill_event[3] = {};
template <int K>
void __fastcall h_ground_kill_event(void* obj) {
    uint32_t flags = 0;
    const bool logged = rd(static_cast<BYTE*>(obj) + jet::kFlags, &flags) && (flags & kFlagKillLogged);
    uint32_t rf = 0;
    const bool remote = rd(static_cast<BYTE*>(obj) + 0x0c, &rf) && (rf & world_remote_mask_ground());
    const net::KillHook k = net::kill_hook_action(logged, world_remote_on(), remote);
    if (k == net::KillHook::Repeat) return;
    if (k == net::KillHook::Block) {
        if (rd(static_cast<BYTE*>(obj) + jet::kFlags, &flags)) wr(static_cast<BYTE*>(obj) + jet::kFlags, flags | kFlagKillLogged);
        if (log_milestone(++g_rx.kills_blocked))
            LOG("damage: a remote ground copy's kill event was not logged here (%llu so far): the host keeps the kill record",
                static_cast<unsigned long long>(g_rx.kills_blocked));
        return;
    }
    reinterpret_cast<void(__fastcall*)(void*)>(g_o_ground_kill_event[K])(obj);
}
template void __fastcall h_ground_kill_event<0>(void*);
template void __fastcall h_ground_kill_event<1>(void*);
template void __fastcall h_ground_kill_event<2>(void*);

// Aircraft primary slot 30 (+0x42ac0, __fastcall(obj)): logs the kill (mission event 1) once.
void __fastcall h_aircraft_kill_event(void* obj) {
    uint32_t flags = 0;
    // The game asks every frame for a dead aircraft (+0x40a40, +0x687f0); once logged (or blocked) it is a no-op.
    const bool read = rd(static_cast<BYTE*>(obj) + jet::kFlags, &flags);
    const net::KillHook k = net::kill_hook_action(read && (flags & kFlagKillLogged), world_remote_on(), is_remote_copy(obj));
    if (k == net::KillHook::Repeat) return;
    if (k == net::KillHook::Block) {
        wr(static_cast<BYTE*>(obj) + jet::kFlags, flags | kFlagKillLogged);
        if (log_milestone(++g_rx.kills_blocked))
            LOG("damage: a remote copy's kill event was not logged here (%llu so far): the host keeps the kill record",
                static_cast<unsigned long long>(g_rx.kills_blocked));
        return;
    }
    reinterpret_cast<void(__fastcall*)(void*)>(g_o_aircraft_kill_event)(obj);
}

// IID-10 slot 10 (+0x40d40, thiscall(iface = obj + 0x20, missile id, stage)): an incoming missile. On an avatar the
// auto-dispense bits it sets are cleared at once, so a copy never pops chaff or flares by itself.
void __fastcall h_incoming_missile(void* iface, void* edx, int missile, int stage) {
    reinterpret_cast<void(__fastcall*)(void*, void*, int, int)>(g_o_incoming_missile)(iface, edx, missile, stage);
    BYTE* obj = static_cast<BYTE*>(iface) - 0x20;
    // Avatars on either side, and on a joined client every remote copy (the host's AI are puppets there too).
    if (!host_drives_object(obj) && !client_drives_object(obj) && !(world_remote_on() && is_remote_copy(obj))) return;
    uint32_t req = 0;
    if (rd(obj + jet::kRequestWord, &req) && (req & kRequestCmBits)) {
        wr(obj + jet::kRequestWord, req & ~kRequestCmBits);
        if (log_milestone(++g_rx.notices))
            LOG("countermeasures: an avatar's automatic chaff/flare request was cleared (%llu so far)",
                static_cast<unsigned long long>(g_rx.notices));
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
