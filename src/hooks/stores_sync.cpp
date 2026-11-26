// Stores and consumables across PCs (Phase 5 T5). The owner sends its loadout table (every 10 s and when a station's
// weapon changes or its count grows, as after a rearm) and its fuel, gun rounds and countermeasure counts (every
// 2 s, unreliable). The avatar takes the owner's stores through the game's own rearm (remote event 0x16 after
// writing the entry's weapon and the station's full count), its fuel by scaling its tanks to the owner's total, and
// its guns' rounds; the countermeasure counts are T6's. Firing and jettison come through WeaponLaunch and the jet
// state's configuration bits. Game thread only. See lazy-sf2-recon: network-recon §11.3 and §11.4.
#include <windows.h>

#include <cstring>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/jet_layout.h"
#include "hooks/jet_mirror.h"
#include "hooks/world_logic.h"
#include "log.h"
#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

constexpr double kLoadoutEvery = 10.0, kConsumablesEvery = 2.0;  // game seconds
constexpr uint32_t kRvaFuelTank = 0x93abc, kRvaFixedGun = 0x94bec, kRvaStation = 0x94cdc;
constexpr int kRearmEvent = 0x16;
constexpr int kMaxSys = 256;

uintptr_t ao_base() {
    static HMODULE seen = nullptr;
    static uintptr_t base = 0;
    HMODULE m = GetModuleHandleW(L"AircraftObject.dll");
    if (m != seen) {
        seen = m;
        base = m && is_expected_build(m, jet::kAoStamp, jet::kAoSize, "AircraftObject.dll (stores)")
                   ? reinterpret_cast<uintptr_t>(m)
                   : 0;
    }
    return base;
}

struct Entry {
    int32_t weapon = -1;
    int16_t count = 0, state = 0;
};
bool read_loadout(const void* obj, Entry out[jet::kLoadoutStations]) {
    __try {
        const BYTE* o = static_cast<const BYTE*>(obj);
        for (size_t i = 0; i < jet::kLoadoutStations; ++i) {
            const BYTE* e = o + jet::kLoadout + jet::kLoadoutEntry * i;
            std::memcpy(&out[i].weapon, e, 4);
            std::memcpy(&out[i].count, e + 8, 2);
            std::memcpy(&out[i].state, e + 10, 2);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Consum {
    float fuel = 0, fuel_ext = 0;
    int guns = 0;
    int16_t rounds[4] = {};
    int chaff = 0, flares = 0;
};
bool read_consumables(void* obj, uintptr_t ao, Consum* c) {
    SysRef sys[kMaxSys];
    const int n = jet_list_systems(obj, ao, sys, kMaxSys);
    __try {
        const BYTE* o = static_cast<const BYTE*>(obj);
        int16_t ch = 0, fl = 0;
        std::memcpy(&ch, o + jet::kChaffTotal, 2);
        std::memcpy(&fl, o + jet::kFlareTotal, 2);
        c->chaff = ch;
        c->flares = fl;
        for (int i = 0; i < n; ++i) {
            if (sys[i].vt == kRvaFuelTank) {
                float f = 0;
                int16_t ext = 0;
                std::memcpy(&f, sys[i].sys + jet::kTankFuel, 4);
                std::memcpy(&ext, sys[i].sys + jet::kTankExternal, 2);
                if (f > 0) (ext ? c->fuel_ext : c->fuel) += f;
            } else if (sys[i].vt == kRvaFixedGun && c->guns < 4) {
                std::memcpy(&c->rounds[c->guns++], sys[i].sys + jet::kGunRounds, 2);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Tx {
    uint32_t epoch = 0;
    double next_loadout = 0, next_consumables = 0;
    Entry last[jet::kLoadoutStations];
    bool have = false;
    uint16_t seq = 0;
};
Tx g_tx;

bool send(const uint8_t* d, size_t n, bool reliable) {
    return host_world_active() ? host_world_send_all(d, n, reliable) : client_world_send(d, n, reliable);
}

// Sends the loadout in parts. False if a part could not be queued.
bool send_loadout(const Entry now[jet::kLoadoutStations]) {
    net::LoadoutEntry all[jet::kLoadoutStations];
    size_t k = 0;
    for (size_t i = 0; i < jet::kLoadoutStations; ++i) {
        if (now[i].weapon < 0) continue;
        net::LoadoutEntry& e = all[k++];
        e = net::LoadoutEntry{};
        e.station = static_cast<uint8_t>(i);
        e.weapon = static_cast<uint16_t>(now[i].weapon);
        char name[64];
        e.hash = weapon_name_of(static_cast<uint32_t>(now[i].weapon), name, sizeof(name)) ? net::weapon_name_hash(name) : 0;
        e.count = static_cast<uint8_t>(now[i].count < 0 ? 0 : now[i].count > 255 ? 255 : now[i].count);
        e.state = static_cast<uint8_t>(now[i].state);
    }
    const size_t parts = k == 0 ? 1 : (k + net::kLoadoutPart - 1) / net::kLoadoutPart;
    const uint16_t seq = ++g_tx.seq;
    for (size_t p = 0; p < parts; ++p) {
        net::LoadoutMsg m{};
        m.seq = seq;
        m.part = static_cast<uint8_t>(p);
        m.parts = static_cast<uint8_t>(parts);
        const size_t first = p * net::kLoadoutPart;
        m.n = static_cast<uint8_t>(k - first < net::kLoadoutPart ? k - first : net::kLoadoutPart);
        for (size_t i = 0; i < m.n; ++i) m.e[i] = all[first + i];
        uint8_t buf[sizeof(m)];
        const size_t n = net::encode(m, buf, sizeof(buf));
        if (!n || !send(buf, n, true)) return false;
    }
    return true;
}

// The station system for a station id on an avatar. SEH only (the caller's guard).
BYTE* station_sys(const SysRef* sys, int n, int station) {
    for (int i = 0; i < n; ++i) {
        if (sys[i].vt != kRvaStation) continue;
        const BYTE* data = *reinterpret_cast<const BYTE* const*>(sys[i].sys + jet::kStaData);
        int32_t id = -1;
        if (data) std::memcpy(&id, data + 0x78, 4);
        if (id == station) return sys[i].sys;
    }
    return nullptr;
}

bool has_station(const SysRef* sys, int n, int station) {
    __try {
        return station_sys(sys, n, station) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Per peer: what the last rebuild of each station asked for, so one that did not take is not retried; the most each
// avatar tank has held (its capacity, as far as we know).
struct Tried {
    int32_t weapon = -2;
    int count = -1;
    bool gave_up = false;  // logged once
};
struct TankMax {
    const BYTE* tank = nullptr;
    float max = 0;
};
struct Rx {
    uint32_t epoch = 0;
    void* avatar[5] = {};
    Tried tried[5][jet::kLoadoutStations];
    TankMax tanks[64];
    uint64_t rearms = 0, gave_up = 0, empty = 0, unknown = 0, fuel = 0;
};
Rx g_rx;

// Sets the station's full count (+0x80); with prepare, also the entry's weapon and a count of 0 so the rearm rebuilds
// it. A station not to be rebuilt gets its own count as full, so the game's rearm (which refills every station below
// full) leaves it as it is. 0 done, 1 no such station, 2 faulted.
int set_station(BYTE* avatar, const SysRef* sys, int n, int station, int32_t weapon, int count, bool prepare) {
    __try {
        BYTE* sta = station_sys(sys, n, station);
        if (!sta) return 1;
        BYTE* e = avatar + jet::kLoadout + jet::kLoadoutEntry * static_cast<size_t>(station);
        int32_t full = count;
        if (prepare) {
            const int16_t zero = 0;
            std::memcpy(e, &weapon, 4);
            std::memcpy(e + 8, &zero, 2);
        } else {
            int16_t have = 0;
            std::memcpy(&have, e + 8, 2);
            full = have;
        }
        std::memcpy(sta + jet::kStaFull, &full, 4);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

void apply_loadout(const net::LoadoutMsg& m, BYTE* avatar, uintptr_t ao) {
    const int peer = m.peer < 5 ? m.peer : 4;
    if (g_rx.avatar[peer] != avatar) {
        g_rx.avatar[peer] = avatar;
        for (Tried& t : g_rx.tried[peer]) t = Tried{};
    }
    Entry have[jet::kLoadoutStations];
    if (!read_loadout(avatar, have)) return;
    SysRef sys[kMaxSys];
    const int n = jet_list_systems(avatar, ao, sys, kMaxSys);
    bool rebuild[jet::kLoadoutStations] = {};
    int32_t want_weapon[jet::kLoadoutStations] = {};
    int want_count[jet::kLoadoutStations] = {};
    int nrebuild = 0, unknown = 0, missing = 0, gave_up = 0, empty = 0;
    for (size_t i = 0; i < m.n; ++i) {
        const net::LoadoutEntry& e = m.e[i];
        if (e.station >= jet::kLoadoutStations) continue;
        int32_t weapon = e.weapon;
        char name[64];
        if (!e.hash || !weapon_name_of(e.weapon, name, sizeof(name)) || net::weapon_name_hash(name) != e.hash)
            weapon = e.hash ? weapon_id_by_hash(e.hash) : -1;
        if (weapon < 0) {
            ++unknown;
            continue;
        }
        const Entry& h = have[e.station];
        Tried& t = g_rx.tried[peer][e.station];
        const bool same = t.weapon == weapon && t.count == e.count;
        const net::StationStep st =
            net::station_step(h.weapon, h.count, weapon, e.count, same, has_station(sys, n, e.station));
        if (st == net::StationStep::Keep) {
            t = Tried{};
        } else if (st == net::StationStep::Empty) {
            ++empty;
        } else if (st == net::StationStep::GiveUp) {
            if (!t.gave_up) ++gave_up;
            t.gave_up = true;
        } else if (st == net::StationStep::Skip) {
            ++missing;  // a fallback airframe without this station
        } else {
            rebuild[e.station] = true;
            want_weapon[e.station] = weapon;
            want_count[e.station] = e.count;
            t = Tried{weapon, e.count, false};
            ++nrebuild;
        }
    }
    int ev = -1;
    if (nrebuild) {
        for (int st = 0; st < static_cast<int>(jet::kLoadoutStations); ++st)
            set_station(avatar, sys, n, st, rebuild[st] ? want_weapon[st] : 0, rebuild[st] ? want_count[st] : 0, rebuild[st]);
        ev = aircraft_remote_event(avatar, kRearmEvent, nullptr);
        ++g_rx.rearms;
    }
    g_rx.gave_up += static_cast<uint64_t>(gave_up);
    if (unknown) ++g_rx.unknown;
    if (empty && log_milestone(++g_rx.empty))
        LOG("stores: peer %u's jet has %d station(s) emptied that its avatar still carries (the rearm cannot empty one)",
            static_cast<unsigned>(m.peer), empty);
    if (nrebuild || missing || gave_up || (unknown && log_milestone(g_rx.unknown)))
        LOG("stores: peer %u's loadout (part %u/%u): %d station(s) rebuilt%s, %d given up (the rebuild did not take), %d "
            "of an unknown weapon, %d the avatar does not have (skipped)",
            static_cast<unsigned>(m.peer), static_cast<unsigned>(m.part + 1), static_cast<unsigned>(m.parts), nrebuild,
            ev < 0 ? "" : ev == 0 ? " (rearm)" : ev == 1 ? " (rearm REFUSED: interface not the one we read)" : " (rearm FAULTED)",
            gave_up, unknown, missing);
}

// The most a tank has held so far, the capacity as far as we know (the tanks start full). SEH by the caller.
float& tank_max(const BYTE* tank, float now) {
    for (TankMax& t : g_rx.tanks)
        if (t.tank == tank) {
            if (now > t.max) t.max = now;
            return t.max;
        }
    for (TankMax& t : g_rx.tanks)
        if (!t.tank) {
            t = TankMax{tank, now};
            return t.max;
        }
    static float none;
    none = now;
    return none;
}

void apply_consumables(const net::ConsumablesMsg& m, BYTE* avatar, uintptr_t ao) {
    SysRef sys[kMaxSys];
    const int n = jet_list_systems(avatar, ao, sys, kMaxSys);
    float total[2] = {}, owner[2] = {net::fuel_from_wire(m.fuel), net::fuel_from_wire(m.fuel_ext)};
    int guns = 0;
    __try {
        for (int i = 0; i < n; ++i)
            if (sys[i].vt == kRvaFuelTank) {
                float f = 0;
                int16_t ext = 0;
                std::memcpy(&f, sys[i].sys + jet::kTankFuel, 4);
                std::memcpy(&ext, sys[i].sys + jet::kTankExternal, 2);
                tank_max(sys[i].sys, f);
                if (f > 0) total[ext ? 1 : 0] += f;
            }
        const float k[2] = {net::fuel_scale(total[0], owner[0]), net::fuel_scale(total[1], owner[1])};
        for (int i = 0; i < n; ++i) {
            if (sys[i].vt == kRvaFuelTank) {
                float f = 0;
                int16_t ext = 0;
                std::memcpy(&f, sys[i].sys + jet::kTankFuel, 4);
                std::memcpy(&ext, sys[i].sys + jet::kTankExternal, 2);
                const float kk = k[ext ? 1 : 0];
                if (kk == 1.0f) continue;
                f = f > 0 ? f * kk : 0;
                const float cap = tank_max(sys[i].sys, 0);
                if (f > cap) f = cap;
                std::memcpy(sys[i].sys + jet::kTankFuel, &f, 4);
            } else if (sys[i].vt == kRvaFixedGun && guns < m.guns) {
                const uint16_t want = m.rounds[guns++];
                const int16_t r = static_cast<int16_t>(want > 32767 ? 32767 : want);
                std::memcpy(sys[i].sys + jet::kGunRounds, &r, 2);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (log_milestone(++g_rx.fuel))
        LOG("stores: peer %u's consumables (%llu so far): fuel %.0f + %.0f kg external (the avatar had %.0f + %.0f), %u "
            "gun(s) of %d here",
            static_cast<unsigned>(m.peer), static_cast<unsigned long long>(g_rx.fuel), owner[0], owner[1], total[0],
            total[1], static_cast<unsigned>(m.guns), guns);
}

}  // namespace

void stores_tick(void* player) {
    const uintptr_t ao = ao_base();
    if (!player || !ao) return;
    const double now = sim_game_time();
    if (g_tx.epoch != sim_player_epoch()) {
        g_tx = Tx{};
        g_tx.epoch = sim_player_epoch();
    }
    Entry e[jet::kLoadoutStations];
    if (read_loadout(player, e)) {
        bool changed = !g_tx.have;
        for (size_t i = 0; i < jet::kLoadoutStations && !changed; ++i)
            changed = e[i].weapon != g_tx.last[i].weapon || e[i].count > g_tx.last[i].count;
        if (changed || now >= g_tx.next_loadout) {
            if (send_loadout(e)) {
                if (changed) {
                    int loaded = 0;
                    for (const Entry& x : e) loaded += x.weapon >= 0;
                    LOG("stores: loadout sent (%d loaded stations)%s", loaded, g_tx.have ? ", changed" : "");
                }
                std::memcpy(g_tx.last, e, sizeof(e));
                g_tx.have = true;
                g_tx.next_loadout = now + kLoadoutEvery;
            }
        } else {
            for (size_t i = 0; i < jet::kLoadoutStations; ++i) g_tx.last[i].count = e[i].count;  // used stores
        }
    }
    if (now < g_tx.next_consumables) return;
    g_tx.next_consumables = now + kConsumablesEvery;
    Consum c;
    if (!read_consumables(player, ao, &c)) return;
    net::ConsumablesMsg m{};
    m.seq = ++g_tx.seq;
    m.fuel = net::fuel_to_wire(c.fuel);
    m.fuel_ext = net::fuel_to_wire(c.fuel_ext);
    m.guns = static_cast<uint8_t>(c.guns);
    for (int i = 0; i < c.guns; ++i) m.rounds[i] = static_cast<uint16_t>(c.rounds[i] < 0 ? 0 : c.rounds[i]);
    m.chaff = static_cast<uint8_t>(c.chaff < 0 ? 0 : c.chaff > 255 ? 255 : c.chaff);
    m.flares = static_cast<uint8_t>(c.flares < 0 ? 0 : c.flares > 255 ? 255 : c.flares);
    uint8_t buf[sizeof(m)];
    const size_t n = net::encode(m, buf, sizeof(buf));
    if (n) send(buf, n, false);
}

void stores_apply(const uint8_t* data, size_t len, void* avatar) {
    const uintptr_t ao = ao_base();
    if (!avatar || !ao) return;
    if (g_rx.epoch != sim_player_epoch()) {
        g_rx = Rx{};
        g_rx.epoch = sim_player_epoch();
    }
    const uint8_t wt = net::peek_world_type(data, len);
    if (wt == static_cast<uint8_t>(net::WorldType::Loadout)) {
        net::LoadoutMsg m;
        if (net::decode(data, len, m) == net::Status::Ok) apply_loadout(m, static_cast<BYTE*>(avatar), ao);
    } else if (wt == static_cast<uint8_t>(net::WorldType::Consumables)) {
        net::ConsumablesMsg m;
        if (net::decode(data, len, m) == net::Status::Ok) apply_consumables(m, static_cast<BYTE*>(avatar), ao);
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
