// Weapon launches across PCs (Phase 5 T4). The player's own releases (MissileObject Release, through the weapon
// tracker's hook) go out as WeaponLaunch; every other PC replays them on the shooter's avatar. A guided missile does
// damage only on the PC that owns its target: our own jet (the avatar's own fire path launches it live), or on the
// host an object the host owns (spawned here and made live, the avatar credited); everywhere else the copy is
// visual-only. Bombs and rockets are visual-only copies; their hits come from the shooter's claims (hit_claims.cpp).
// Game thread only. Fixed RVAs, July 2013 build only. See lazy-sf2-recon: network-recon §9.2, §9.4 and §11.7.
#include <windows.h>

#include <cmath>
#include <cstring>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/jet_layout.h"
#include "hooks/world_logic.h"
#include "log.h"
#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

constexpr DWORD kMoStamp = 0x51d220d1, kMoSize = 0x46000;
// MissileObject: the weapon data manager (count +0x08, records +0x14; a record starts with its UTF-16 name, the
// seeker's GuidanceType short at +0x398), the REMOTEOBJECT mask, the link interface (+0x14: slot 2 Release, slot 3
// SetShooter) and the guidance interface (+0x148: slot 0 SetTarget).
constexpr DWORD kRvaWeaponDb = 0x3e16c, kRvaMoRemoteMask = 0x40800, kRvaLinkVtable = 0x2f624;
constexpr DWORD kRvaRelease = 0x50e0, kRvaSetShooter = 0xa220, kRvaSetTarget = 0x53e0;
constexpr size_t kDbCount = 0x08, kDbRecords = 0x14, kRecGuidance = 0x398;
constexpr size_t kMisFlags = 0x0c, kMisLink = 0x14, kMisPos = 0x20, kMisWvel = 0x38, kMisBvel = 0xa0, kMisAng = 0xe8;
constexpr size_t kMisShooter = 0x100, kMisData = 0x14c, kMisSeeker = 0x154, kMisGuidance = 0x148,
                 kMisTarget = 0x198, kMisNoDamage = 0x1ac;
// AircraftObject: the OBJ service and the MissileObject class and group ids (globals), the remote interface (+0x28,
// vtable +0x94344, slot 1 = receive an event: 2 fires one store from a station), the station class.
constexpr DWORD kRvaObjService = 0xb7b88, kRvaMissileClass = 0xa7700, kRvaMissileGroup = 0xa7704;
constexpr DWORD kRvaRemoteVtable = 0x94344, kRvaRemoteEvent = 0x4cdb0, kRvaStationVtable = 0x94cdc;
constexpr size_t kObjId = 0x08, kObjRemote = 0x28, kStaStationId = 0x78;
constexpr size_t kCreateSlot = 0x30 / 4, kByIdSlot = 0x3c / 4;
constexpr size_t kDescSize = 0xc8;
constexpr double kPendingSeconds = 3.0;  // a native fire's release is expected within this (game time)

uintptr_t checked_base(const wchar_t* name, DWORD stamp, DWORD size, HMODULE* seen, uintptr_t* base) {
    HMODULE m = GetModuleHandleW(name);
    if (m != *seen) {
        *seen = m;
        *base = m && is_expected_build(m, stamp, size, "weapon launches") ? reinterpret_cast<uintptr_t>(m) : 0;
    }
    return *base;
}
uintptr_t mo_base() {
    static HMODULE seen = nullptr;
    static uintptr_t base = 0;
    return checked_base(L"MissileObject.dll", kMoStamp, kMoSize, &seen, &base);
}
uintptr_t ao_base() {
    static HMODULE seen = nullptr;
    static uintptr_t base = 0;
    return checked_base(L"AircraftObject.dll", jet::kAoStamp, jet::kAoSize, &seen, &base);
}

// The weapon data record of an id, or null. SEH only.
const BYTE* weapon_record(uintptr_t mo, uint32_t id) {
    const BYTE* db = reinterpret_cast<const BYTE*>(mo + kRvaWeaponDb);
    int32_t n = 0;
    std::memcpy(&n, db + kDbCount, 4);
    const BYTE* const* recs = *reinterpret_cast<const BYTE* const* const*>(db + kDbRecords);
    if (!recs || n <= 0 || id >= static_cast<uint32_t>(n) || n > 4096) return nullptr;
    return recs[id];
}

int weapon_id_by_name(const char* name) {
    const uintptr_t mo = mo_base();
    if (!mo || !name || !name[0]) return -1;
    __try {
        const BYTE* db = reinterpret_cast<const BYTE*>(mo + kRvaWeaponDb);
        int32_t n = 0;
        std::memcpy(&n, db + kDbCount, 4);
        const BYTE* const* recs = *reinterpret_cast<const BYTE* const* const*>(db + kDbRecords);
        if (!recs || n <= 0 || n > 4096) return -1;
        for (int32_t i = 0; i < n; ++i) {
            const wchar_t* w = reinterpret_cast<const wchar_t*>(recs[i]);
            if (!w) continue;
            size_t k = 0;
            while (name[k] && w[k] == static_cast<wchar_t>(static_cast<unsigned char>(name[k]))) ++k;
            if (!name[k] && !w[k]) return i;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

int16_t ang_to_wire(float a) {
    if (!std::isfinite(a)) return 0;
    const double turns = std::remainder(static_cast<double>(a), 2.0 * 3.14159265358979323846) / (2.0 * 3.14159265358979323846);
    long v = std::lround(turns * 65536.0);
    if (v > 32767) v -= 65536;
    if (v < -32768) v += 65536;
    return static_cast<int16_t>(v);
}
float ang_from_wire(int16_t q) { return static_cast<float>(q / 65536.0 * 2.0 * 3.14159265358979323846); }
int16_t vel_to_wire(float v) {
    if (!std::isfinite(v)) return 0;
    const double q = std::round(static_cast<double>(v) * 20.0);
    return static_cast<int16_t>(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
}
int32_t pos_to_wire(double p) {
    if (!std::isfinite(p)) return 0;
    const double q = std::round(p * 100.0);
    return static_cast<int32_t>(q > 2147483647.0 ? 2147483647.0 : q < -2147483648.0 ? -2147483648.0 : q);
}

struct Released {
    uint32_t shooter_id = 0xffffffffu, target_id = 0xffffffffu, weapon_id = 0xffffffffu;
    bool guided = false;
    double pos[3] = {};
    float ang[3] = {}, bvel[3] = {}, wvel[3] = {};
    char name[24] = {};
};
bool read_released(const BYTE* m, Released* r) {
    __try {
        const BYTE* shooter = *reinterpret_cast<const BYTE* const*>(m + kMisShooter);
        const BYTE* data = *reinterpret_cast<const BYTE* const*>(m + kMisData);
        if (!shooter || !data) return false;
        std::memcpy(&r->shooter_id, shooter + kObjId, 4);
        std::memcpy(&r->target_id, m + kMisTarget, 4);
        const int16_t* seeker = *reinterpret_cast<const int16_t* const*>(m + kMisSeeker);
        r->guided = seeker && *seeker != 0;
        std::memcpy(r->pos, m + kMisPos, sizeof(r->pos));
        std::memcpy(r->ang, m + kMisAng, sizeof(r->ang));
        std::memcpy(r->bvel, m + kMisBvel, sizeof(r->bvel));
        std::memcpy(r->wvel, m + kMisWvel, sizeof(r->wvel));
        const wchar_t* w = reinterpret_cast<const wchar_t*>(data);
        size_t k = 0;
        for (; k + 1 < sizeof(r->name) && w[k]; ++k) r->name[k] = (w[k] >= 0x20 && w[k] < 0x7f) ? static_cast<char>(w[k]) : '?';
        r->name[k] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool set_int(BYTE* p, int32_t v) {
    __try {
        std::memcpy(p, &v, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The missile's interface slot, if it is the function we read. SEH only.
void* missile_fn(BYTE* iface, size_t slot, uintptr_t mo, DWORD rva) {
    void** vt = *reinterpret_cast<void***>(iface);
    if (!vt) return nullptr;
    return reinterpret_cast<uintptr_t>(vt[slot]) == mo + rva ? vt[slot] : nullptr;
}

// Guidance slot 0 on a missile. 0 done, 1 refused, 2 faulted.
int set_target(BYTE* m, uint32_t target, uintptr_t mo) {
    __try {
        BYTE* g = m + kMisGuidance;
        void* f = missile_fn(g, 0, mo, kRvaSetTarget);
        if (!f) return 1;
        reinterpret_cast<void(__fastcall*)(void*, void*, uint32_t)>(f)(g, nullptr, target);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

// A launch the avatar's own fire path is about to make (native fire): the release hook gives it our target again,
// since the game's reliability roll may have dropped it.
struct Pending {
    void* avatar = nullptr;
    uint32_t target = 0xffffffffu;
    double until = 0;
};
Pending g_pending[8];
bool g_in_spawn = false;  // our own spawn's Release: not a native fire, not ours to send
uint16_t g_launch_seq = 0;
uint64_t g_sent = 0, g_send_fail = 0, g_replayed = 0, g_unknown = 0;

// The avatar's station system for a station id, or null. SEH only.
BYTE* station_of(BYTE* obj, int station, uintptr_t ao) {
    int32_t ncomp = 0;
    std::memcpy(&ncomp, obj + jet::kComponents + jet::kListCount, 4);
    BYTE* const* comps = *reinterpret_cast<BYTE* const* const*>(obj + jet::kComponents + jet::kListArray);
    for (int i = 0; comps && i < ncomp && i < 64; ++i) {
        BYTE* c = comps[i];
        if (!c) continue;
        int32_t nsys = 0;
        std::memcpy(&nsys, c + jet::kCompSystems + jet::kListCount, 4);
        BYTE* const* sys = *reinterpret_cast<BYTE* const* const*>(c + jet::kCompSystems + jet::kListArray);
        for (int j = 0; sys && j < nsys && j < 64; ++j) {
            BYTE* s = sys[j];
            if (!s || *reinterpret_cast<uintptr_t*>(s) != ao + kRvaStationVtable) continue;
            const BYTE* data = *reinterpret_cast<const BYTE* const*>(s + jet::kStaData);
            int32_t id = -1;
            if (data) std::memcpy(&id, data + kStaStationId, 4);
            if (id == station) return s;
        }
    }
    return nullptr;
}

// The avatar fires one store from a station through its own remote event 2, with the station's target set first.
// 0 done, 1 refused (no such station, empty, another weapon, an unexpected interface), 2 faulted.
int native_fire(BYTE* avatar, int station, uint32_t weapon, uint32_t target, uintptr_t ao) {
    if (station < 0 || station >= static_cast<int>(jet::kLoadoutStations)) return 1;
    __try {
        const BYTE* e = avatar + jet::kLoadout + jet::kLoadoutEntry * static_cast<size_t>(station);
        int32_t id = -1;
        int16_t count = 0;
        std::memcpy(&id, e, 4);
        std::memcpy(&count, e + 8, 2);
        if (id < 0 || static_cast<uint32_t>(id) != weapon || count <= 0) return 1;
        BYTE* sta = station_of(avatar, station, ao);
        BYTE* iface = avatar + kObjRemote;
        void** vt = *reinterpret_cast<void***>(iface);
        if (!sta || reinterpret_cast<uintptr_t>(vt) != ao + kRvaRemoteVtable ||
            reinterpret_cast<uintptr_t>(vt[1]) != ao + kRvaRemoteEvent)
            return 1;
        std::memcpy(sta + jet::kStaTarget, &target, 4);
        int16_t payload[2] = {static_cast<int16_t>(station), 0};
        reinterpret_cast<void(__fastcall*)(void*, void*, int, void*)>(vt[1])(iface, nullptr, 2, payload);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

struct Spawn {
    uint32_t weapon;
    double pos[3];
    float ang[3], bvel[3];
    uint32_t target;
    void* shooter;
    bool live;
};

// The §9.2 recipe: CreateObject, SetTarget before the release, SetShooter, Release(0); for a live copy the no-damage
// flag and REMOTEOBJECT are cleared after it. 0 done, 1 refused, 2 faulted.
int spawn_missile(const Spawn& s, uintptr_t ao, uintptr_t mo) {
    __try {
        void* service = *reinterpret_cast<void* const*>(ao + kRvaObjService);
        int32_t cls = -1, grp = -1;
        std::memcpy(&cls, reinterpret_cast<const void*>(ao + kRvaMissileClass), 4);
        std::memcpy(&grp, reinterpret_cast<const void*>(ao + kRvaMissileGroup), 4);
        if (!service || cls < 0 || grp < 0) return 1;
        void** svt = *static_cast<void***>(service);
        alignas(8) unsigned char desc[kDescSize] = {};
        const int16_t one = 1, boost = 0;  // a release: the motor starts its boost
        const int32_t minus1 = -1;
        std::memcpy(desc + 0x00, &s.weapon, 4);
        std::memcpy(desc + 0x04, &one, 2);
        std::memcpy(desc + 0x06, &boost, 2);
        std::memcpy(desc + 0x08, &minus1, 4);
        std::memcpy(desc + 0x0c, &minus1, 4);
        std::memcpy(desc + 0x10, s.pos, sizeof(s.pos));
        std::memcpy(desc + 0x28, s.ang, sizeof(s.ang));
        std::memcpy(desc + 0x34, s.bvel, sizeof(s.bvel));
        const uint32_t id = reinterpret_cast<uint32_t(__fastcall*)(void*, void*, int, int, void*)>(svt[kCreateSlot])(
            service, nullptr, cls, grp, desc);
        if (id == 0xffffffffu) return 1;
        BYTE* m = static_cast<BYTE*>(
            reinterpret_cast<void*(__fastcall*)(void*, void*, uint32_t)>(svt[kByIdSlot])(service, nullptr, id));
        if (!m) return 1;
        BYTE* link = m + kMisLink;
        if (*reinterpret_cast<uintptr_t*>(link) != mo + kRvaLinkVtable) return 1;
        void* set_shooter = missile_fn(link, 3, mo, kRvaSetShooter);
        void* release = missile_fn(link, 2, mo, kRvaRelease);
        if (!set_shooter || !release) return 1;
        if (s.target != 0xffffffffu) set_target(m, s.target, mo);
        reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(set_shooter)(link, nullptr, s.shooter);
        g_in_spawn = true;
        reinterpret_cast<void(__fastcall*)(void*, void*, int*)>(release)(link, nullptr, nullptr);
        g_in_spawn = false;
        if (s.live) {
            uint32_t mask = 0, flags = 0;
            std::memcpy(&mask, reinterpret_cast<const void*>(mo + kRvaMoRemoteMask), 4);
            std::memcpy(&flags, m + kMisFlags, 4);
            flags &= ~mask;
            std::memcpy(m + kMisFlags, &flags, 4);
            const int32_t zero = 0;
            std::memcpy(m + kMisNoDamage, &zero, 4);
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_in_spawn = false;
        return 2;
    }
}

// The shooter's selected station (it moves on only after Release returns), or -1.
int16_t selected_station(const void* obj) {
    int16_t s = -1;
    if (!obj) return s;
    __try {
        std::memcpy(&s, static_cast<const BYTE*>(obj) + jet::kSelectedStation, 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s = -1;
    }
    return s;
}

}  // namespace

void weapons_forget_avatar(const void* avatar) {
    for (Pending& p : g_pending)
        if (p.avatar == avatar) p = Pending{};
}

bool weapon_guided(uint32_t id) {
    const uintptr_t mo = mo_base();
    if (!mo) return false;
    __try {
        const BYTE* r = weapon_record(mo, id);
        if (!r) return false;
        int16_t g = 0;
        std::memcpy(&g, r + kRecGuidance, 2);
        return g != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void weapons_on_release(void* missile) {
    if (g_in_spawn || !missile) return;
    const bool host = host_world_active(), client = client_world_active();
    if (!host && !client) return;
    const uintptr_t mo = mo_base();
    Released r;
    if (!mo || !read_released(static_cast<const BYTE*>(missile), &r)) return;
    const double now = sim_game_time();
    if (r.shooter_id != world_player_id()) {
        // A native fire we asked an avatar for: give the missile our target again (the reliability roll).
        for (Pending& p : g_pending) {
            if (!p.avatar || now > p.until) continue;
            uint32_t aid = 0;
            double ap[3];
            bool alive = false;
            if (!world_object_state(p.avatar, ap, &aid, &alive) || aid != r.shooter_id) continue;
            if (r.guided && p.target != 0xffffffffu && r.target_id != p.target)
                set_target(static_cast<BYTE*>(missile), p.target, mo);
            p = Pending{};
            break;
        }
        return;
    }
    net::WeaponLaunchMsg m{};
    if (!world_key_of_object(r.shooter_id, &m.shooter_kind, &m.shooter_a, &m.shooter_b)) return;
    void* target = nullptr;
    if (r.target_id != 0xffffffffu && (r.target_id & 0x40000000u)) {
        m.flags |= net::kLaunchPlacedTarget;
    } else if (r.target_id != 0xffffffffu && world_key_of_object(r.target_id, &m.target_kind, &m.target_a, &m.target_b)) {
        target = world_object_of_key(m.target_kind, m.target_a, m.target_b, nullptr);
    }
    if (r.guided) m.flags |= net::kLaunchGuided;
    const int16_t station = selected_station(world_object_of_key(m.shooter_kind, m.shooter_a, m.shooter_b, nullptr));
    m.station = station >= 0 && station < 32 ? static_cast<uint8_t>(station) : 0xff;
    m.host_ms = static_cast<uint32_t>(net_host_now() * 1000.0);
    for (int i = 0; i < 3; ++i) {
        m.pos[i] = pos_to_wire(r.pos[i]);
        m.ang[i] = ang_to_wire(r.ang[i]);
        m.bvel[i] = vel_to_wire(r.bvel[i]);
        m.wvel[i] = vel_to_wire(r.wvel[i]);
    }
    std::memcpy(m.weapon, r.name, sizeof(m.weapon));
    const int wid = weapon_id_by_name(r.name);  // our own id: the host matches our store-hit claims to it
    m.weapon_id = wid >= 0 ? static_cast<uint16_t>(wid) : 0xffff;
    m.weapon[sizeof(m.weapon) - 1] = '\0';
    m.seq = ++g_launch_seq;
    // Our own missile does damage only where its target is owned (on the host: not at a client's jet).
    const bool at_client = host && target && host_avatar_slot(target) >= 0;
    if (!net::own_launch_stays_live(r.guided, host, at_client)) set_int(static_cast<BYTE*>(missile) + kMisNoDamage, 1);
    uint8_t buf[sizeof(m)];
    const size_t n = net::encode(m, buf, sizeof(buf));
    const bool ok = n && (host ? host_world_send_all(buf, n) : client_world_send(buf, n));
    const uint64_t c = ok ? ++g_sent : ++g_send_fail;
    if (log_milestone(c) || r.guided)
        LOG("weapons: launch %u %s: %s%s at %s%03u.%02u from station %d%s", static_cast<unsigned>(m.seq),
            ok ? "sent" : "NOT sent (queue full)", r.name, r.guided ? " (guided)" : "",
            m.target_kind == 1 ? "A " : m.target_kind == 2 ? "G " : (m.flags & net::kLaunchPlacedTarget) ? "placed " : "none ",
            static_cast<unsigned>(m.target_a), static_cast<unsigned>(m.target_b), static_cast<int>(station),
            at_client && r.guided ? "; ours is visual-only, the target's PC flies the live one" : "");
}

void weapons_apply_launch(const uint8_t* data, size_t len, double host_now) {
    net::WeaponLaunchMsg m;
    if (net::decode(data, len, m) != net::Status::Ok) return;
    const uintptr_t ao = ao_base(), mo = mo_base();
    if (!ao || !mo) return;
    uint32_t sid = 0;
    BYTE* shooter = static_cast<BYTE*>(world_object_of_key(m.shooter_kind, m.shooter_a, m.shooter_b, &sid));
    const uint32_t player = world_player_id();
    if (!shooter || sid == player) return;
    const int weapon = weapon_id_by_name(m.weapon);
    if (weapon < 0) {
        if (log_milestone(++g_unknown))
            LOG("weapons: launch of \"%s\" from peer %u: no such weapon here; not shown", m.weapon,
                static_cast<unsigned>(m.peer));
        return;
    }
    uint32_t tid = 0xffffffffu;
    void* target = m.target_kind ? world_object_of_key(m.target_kind, m.target_a, m.target_b, &tid) : nullptr;
    if (!target) tid = 0xffffffffu;
    const bool guided = (m.flags & net::kLaunchGuided) != 0;
    const bool host = host_world_active();
    const net::LaunchRole role =
        net::launch_role(guided, tid != 0xffffffffu && tid == player, host, target && host_avatar_slot(target) < 0);
    // Also for a live spawn: a guided store from a REMOTEOBJECT shooter only leaves the rail, so the avatar shows it.
    const int nat = native_fire(shooter, m.station, static_cast<uint32_t>(weapon), tid, ao);
    if (nat == 0 && role == net::LaunchRole::LiveNative) {
        for (Pending& p : g_pending)
            if (!p.avatar || sim_game_time() > p.until) {
                p = Pending{shooter, tid, sim_game_time() + kPendingSeconds};
                break;
            }
    }
    // The avatar's own fire path launches unguided stores and guided missiles at our jet; for the others (or when it
    // could not fire: an empty or other station) the recipe makes the copy, live where the role is.
    const net::LaunchPlan plan = net::launch_plan(role, guided, nat == 0);
    int sp = -1;
    if (plan.spawn) {
        double age = host_now - m.host_ms / 1000.0;
        if (!(age > 0)) age = 0;
        if (age > 1.0) age = 1.0;
        Spawn s{};
        s.weapon = static_cast<uint32_t>(weapon);
        for (int i = 0; i < 3; ++i) {
            s.pos[i] = m.pos[i] / 100.0 + m.wvel[i] / 20.0 * age;
            s.ang[i] = ang_from_wire(m.ang[i]);
            s.bvel[i] = static_cast<float>(m.bvel[i] / 20.0);
        }
        s.target = guided ? tid : 0xffffffffu;
        s.shooter = shooter;
        s.live = plan.live;
        sp = spawn_missile(s, ao, mo);
    }
    ++g_replayed;
    if (log_milestone(g_replayed) || guided || nat == 2 || sp > 0)
        LOG("weapons: launch %u of peer %u (%s%s at %s%03u.%02u): %s%s%s", static_cast<unsigned>(m.seq),
            static_cast<unsigned>(m.peer), m.weapon, guided ? ", guided" : "",
            m.target_kind == 1 ? "A " : m.target_kind == 2 ? "G " : "none ", static_cast<unsigned>(m.target_a),
            static_cast<unsigned>(m.target_b),
            role == net::LaunchRole::LiveNative  ? "live here (it is aimed at us)"
            : role == net::LaunchRole::LiveSpawn ? "live here (the host owns the target)"
                                                 : "visual-only copy",
            nat == 0 ? "; the avatar fired its own store" : nat == 2 ? "; the avatar's fire FAULTED" : "; the avatar's store was not fired",
            sp < 0 ? "" : sp == 0 ? "; spawned" : sp == 1 ? "; spawn refused" : "; spawn FAULTED");
}

}  // namespace LazyWorks::SF2Patch::hooks
