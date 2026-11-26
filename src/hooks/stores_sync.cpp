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
    float fuel = 0;
    int guns = 0;
    int16_t rounds[4] = {};
    int chaff = 0, flares = 0;
};
bool read_consumables(void* obj, uintptr_t ao, Consum* c) {
    SysRef sys[kMaxSys];
    const int n = jet_list_systems(obj, ao, sys, kMaxSys);
    __try {
        const BYTE* o = static_cast<const BYTE*>(obj);
        std::memcpy(&c->fuel, o + jet::kFuelTotal, 4);
        int16_t ch = 0, fl = 0;
        std::memcpy(&ch, o + jet::kChaffTotal, 2);
        std::memcpy(&fl, o + jet::kFlareTotal, 2);
        c->chaff = ch;
        c->flares = fl;
        for (int i = 0; i < n && c->guns < 4; ++i)
            if (sys[i].vt == kRvaFixedGun) std::memcpy(&c->rounds[c->guns++], sys[i].sys + jet::kGunRounds, 2);
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

// Prepares the station for the game's rearm: the entry's weapon, the station's full count, the entry's count 0.
// 0 prepared, 1 no such station, 2 faulted.
int prepare_station(BYTE* avatar, const SysRef* sys, int n, int station, int32_t weapon, int count) {
    __try {
        BYTE* sta = station_sys(sys, n, station);
        if (!sta) return 1;
        BYTE* e = avatar + jet::kLoadout + jet::kLoadoutEntry * static_cast<size_t>(station);
        const int16_t zero = 0;
        const int32_t full = count;
        std::memcpy(e, &weapon, 4);
        std::memcpy(e + 8, &zero, 2);
        std::memcpy(sta + jet::kStaFull, &full, 4);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

struct Rx {
    uint32_t epoch = 0;
    uint16_t logged_seq[5] = {};
    uint64_t rearms = 0, mismatches = 0, unknown = 0, fuel = 0;
};
Rx g_rx;

void apply_loadout(const net::LoadoutMsg& m, BYTE* avatar, uintptr_t ao) {
    Entry have[jet::kLoadoutStations];
    if (!read_loadout(avatar, have)) return;
    SysRef sys[kMaxSys];
    const int n = jet_list_systems(avatar, ao, sys, kMaxSys);
    int rebuild = 0, more = 0, unknown = 0, missing = 0;
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
        const net::StationAction a = net::station_action(h.weapon, h.count, weapon, e.count);
        if (a == net::StationAction::More) ++more;
        if (a != net::StationAction::Rebuild) continue;
        const int r = prepare_station(avatar, sys, n, e.station, weapon, e.count);
        if (r == 0) ++rebuild;
        else ++missing;
    }
    int ev = -1;
    if (rebuild) {
        ev = aircraft_remote_event(avatar, kRearmEvent, nullptr);
        ++g_rx.rearms;
    }
    if (more) ++g_rx.mismatches;
    if (unknown) ++g_rx.unknown;
    const uint16_t peer = m.peer < 5 ? m.peer : 4;
    if (rebuild || missing || unknown || (more && log_milestone(g_rx.mismatches))) {
        if (g_rx.logged_seq[peer] == m.seq && !rebuild) return;
        g_rx.logged_seq[peer] = m.seq;
        LOG("stores: peer %u's loadout (part %u/%u, %u stations): %d rebuilt%s, %d with more stores here than the "
            "owner's, %d of an unknown weapon, %d without a station here",
            static_cast<unsigned>(m.peer), static_cast<unsigned>(m.part + 1), static_cast<unsigned>(m.parts),
            static_cast<unsigned>(m.n), rebuild,
            ev < 0 ? "" : ev == 0 ? " (rearm)" : ev == 1 ? " (rearm REFUSED: interface not the one we read)" : " (rearm FAULTED)",
            more, unknown, missing);
    }
}

void apply_consumables(const net::ConsumablesMsg& m, BYTE* avatar, uintptr_t ao) {
    SysRef sys[kMaxSys];
    const int n = jet_list_systems(avatar, ao, sys, kMaxSys);
    float total = 0;
    int guns = 0;
    __try {
        for (int i = 0; i < n; ++i)
            if (sys[i].vt == kRvaFuelTank) {
                float f = 0;
                std::memcpy(&f, sys[i].sys + jet::kTankFuel, 4);
                if (f > 0) total += f;
            }
        const float k = net::fuel_scale(total, net::fuel_from_wire(m.fuel));
        for (int i = 0; i < n; ++i) {
            if (sys[i].vt == kRvaFuelTank && k != 1.0f) {
                float f = 0;
                std::memcpy(&f, sys[i].sys + jet::kTankFuel, 4);
                f = f > 0 ? f * k : 0;
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
        LOG("stores: peer %u's consumables (%llu so far): fuel %.0f kg (the avatar had %.0f), %u gun(s) of %d here",
            static_cast<unsigned>(m.peer), static_cast<unsigned long long>(g_rx.fuel), net::fuel_from_wire(m.fuel),
            total, static_cast<unsigned>(m.guns), guns);
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
