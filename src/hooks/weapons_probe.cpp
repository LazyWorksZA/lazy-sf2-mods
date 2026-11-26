// Weapons probe (developer tool, part of the probe): fires a missile from the player's wingman with the game's own
// launch calls, applies damage with no weapon, and logs the player's gun stations while they fire. Every game call is
// made on the game thread at Present, through the repo's __fastcall pattern for __thiscall, inside SEH. Fixed RVAs,
// July 2013 build only. See lazy-sf2-recon: network-recon §9.
#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/weapons_probe_logic.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

std::atomic<bool> g_wprobe_busy{false};

namespace {

using namespace wprobe;

constexpr DWORD kAoStamp = 0x51d220bd, kAoSize = 0xc2000;
constexpr DWORD kMoStamp = 0x51d220d1, kMoSize = 0x46000;
constexpr DWORD kWmStamp = 0x51d21f8f, kWmSize = 0x1e000;

// AircraftObject.dll
constexpr DWORD kObjServiceRva = 0xb7b88, kMissileClassRva = 0xa7700, kMissileGroupRva = 0xa7704,
                kWeaponMgrRva = 0xb7ba0, kAoRemoteMaskRva = 0xb7b28, kAircraftVtRva = 0x9454c,
                kDamageIfaceVtRva = 0x94488, kGunPodRva = 0x50220, kGunInternalRva = 0x4f6d0,
                kInputCountRva = 0xb7f54;
// BulletObject.dll
constexpr DWORD kBoStamp = 0x51d22099, kBoSize = 0x32000, kBulletInitRva = 0x17a0;
// MissileObject.dll
constexpr DWORD kMissileVtRva = 0x2f68c, kLinkVtRva = 0x2f624, kGuidanceVtRva = 0x2f58c, kWeaponMgrVtRva = 0x2f9dc,
                kMoRemoteMaskRva = 0x40800, kMoWeaponMgrRva = 0x3e16c, kMoWldPtrRva = 0x4054c;
// WorldManager.dll: the static WLD object and its vtable.
constexpr DWORD kWldRva = 0x192b8, kWldVtRva = 0x13254;

// OBJ, WLD and interface slots.
constexpr size_t kCreateSlot = 0x30 / 4, kByIdSlot = 0x3c / 4;
constexpr size_t kWldExplosionSlot = 0x40 / 4, kWldHitSlot = 0x44 / 4;
constexpr size_t kLinkReleaseSlot = 2, kLinkShooterSlot = 3, kLinkSlot6 = 6, kGuidanceSetTargetSlot = 0;
constexpr size_t kQiSlot = 0, kBlastSlot = 0x18 / 4;
constexpr int kDamageIid = 9;

// Aircraft object.
constexpr size_t kOffActive = 0x04, kOffId = 0x08, kOffFlags = 0x0c, kOffPos = 0x38, kOffVel = 0x50,
                 kOffFlags2 = 0x168, kOffLevel = 0x16c, kOffAttacker = 0x318, kOffAttackerWeapon = 0x31c,
                 kOffWarn = 0x5f4, kOffMatrix = 0xdc, kOffAngles = 0x100;
constexpr int32_t kDestroyedLevel = 5;
// Missile object.
constexpr size_t kMOffLink = 0x14, kMOffPos = 0x20, kMOffShooter = 0x100, kMOffGuidance = 0x148,
                 kMOffData = 0x14c, kMOffWarhead = 0x150, kMOffSeeker = 0x178, kMOffTargetId = 0x198,
                 kMOffNoDamage = 0x1ac;
constexpr size_t kDescSize = 0xc8;
// Weapon data record and manager.
constexpr size_t kWOffType = 0x298, kWOffWarhead = 0x380, kWOffGuidance = 0x398, kWNameChars = 64;
constexpr size_t kMgrOffCount = 0x08, kMgrOffArray = 0x14;
// WLD queue state: hit queue count/capacity/array, immediate explosion count/capacity/array.
constexpr size_t kWldHitCount = 0x50, kWldHitCap = 0xa0, kWldHitArray = 0xa4;
constexpr size_t kWldExpCount = 0x48, kWldExpCap = 0x88, kWldExpArray = 0x8c;
// Gun stations (network-recon §9.12). Both kinds: +0x1c context (its +0x00 the aircraft, +0x14 the control block),
// +0x2c gun station data.
constexpr size_t kGOffContext = 0x1c, kGOffData = 0x2c;
// Gun pod loop (+0x50220).
constexpr size_t kPOffTrigger = 0x30, kPOffTimer = 0x40, kPOffFiring = 0x48, kPOffBurst = 0x4a, kPOffRounds = 0x4c,
                 kPOffAmmo = 0x4e;
constexpr size_t kPodTriggerByte = 0x30;
constexpr uint8_t kPodTriggerBit = 0x20;
// The aircraft's own gun loop (+0x4f6d0): fires while input[data +0x11c] > 0 and the gun's bit (data +0x78) is set in
// control +0x2f8; control byte +0x00 bit 0x20 inhibits.
constexpr size_t kIOffTimer = 0x30, kIOffFlags = 0x38, kIOffBurst = 0x3c, kIOffAmmo = 0x3e;
constexpr size_t kCtxOffControl = 0x14, kCtlOffInputs = 0x08, kCtlOffSelected = 0x2f8, kDataOffInput = 0x11c,
                 kDataOffSelectBit = 0x78;
constexpr double kForceFireSeconds = 2.0;

constexpr double kTailDistance = 2000.0;    // m behind the target
constexpr double kOvertake = 50.0;          // m/s above the target's speed
constexpr double kMinLaunchSpeed = 150.0;   // m/s
constexpr double kRocketOffset = 30.0;      // m ahead of the player's nose
constexpr double kWatchSeconds = 12.0;      // wall clock: the game time stops if the player dies
constexpr double kKineticJoules = 1.0e6;    // about twenty 20 mm rounds (½ m v², 0.1 kg at 1000 m/s)
constexpr float kFallbackMassKg = 10.f, kFallbackRadius = 10.f;
constexpr size_t kMaxPlanes = 96;
constexpr size_t kMaxGunStations = 6;

using CreateObject_t = uint32_t(__fastcall*)(void* self, void* edx, int class_id, int group_id, void* desc);
using ObjectById_t = void*(__fastcall*)(void* self, void* edx, uint32_t id);
using SetPtr_t = void(__fastcall*)(void* self, void* edx, void* p);
using SetU32_t = void(__fastcall*)(void* self, void* edx, uint32_t v);
using QI_t = int(__fastcall*)(void* self, void* edx, int iid, void** out);
using Blast_t = void(__fastcall*)(void* self, void* edx, const double* pos, float energy, float radius2,
                                  uint32_t shooter, int32_t weapon, int32_t warhead);
using GunUpdate_t = void(__fastcall*)(void* station);
using BulletInit_t = void(__fastcall*)(void* self, void* edx, const uint32_t* desc);

std::atomic<int> g_request{0};

// --- SEH helpers (no C++ objects in these functions) ---

bool safe_copy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <typename T>
bool rd(const void* base, size_t off, T* out) {
    return base && safe_copy(out, static_cast<const char*>(base) + off, sizeof(T));
}
bool wr_f32(float* p, float v) {
    __try {
        *p = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool wr_u32(void* base, size_t off, uint32_t v) {
    __try {
        *reinterpret_cast<uint32_t*>(static_cast<char*>(base) + off) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void* vslot(void* obj, size_t slot) {
    void** vt = nullptr;
    if (!rd(obj, 0, &vt) || !vt) return nullptr;
    void* f = nullptr;
    return safe_copy(&f, vt + slot, sizeof(f)) ? f : nullptr;
}
bool vtable_is(const void* obj, HMODULE m, DWORD rva) {
    const void* vt = nullptr;
    return rd(obj, 0, &vt) && vt == at_rva(m, rva);
}
DWORD vtable_rva(const void* obj, HMODULE m) {
    const char* vt = nullptr;
    if (!rd(obj, 0, &vt) || !vt) return 0;
    return static_cast<DWORD>(vt - reinterpret_cast<const char*>(m));
}

// Each returns 0, or the exception code if the game call faulted.
DWORD seh_create(CreateObject_t f, void* svc, int cls, int grp, void* desc, uint32_t* id) {
    __try {
        *id = f(svc, nullptr, cls, grp, desc);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}
void* seh_by_id(ObjectById_t f, void* svc, uint32_t id) {
    __try {
        return f(svc, nullptr, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
DWORD seh_ptr(SetPtr_t f, void* self, void* p) {
    __try {
        f(self, nullptr, p);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}
DWORD seh_u32(SetU32_t f, void* self, uint32_t v) {
    __try {
        f(self, nullptr, v);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}
DWORD seh_qi(QI_t f, void* self, int iid, void** out) {
    __try {
        f(self, nullptr, iid, out);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}
DWORD seh_blast(Blast_t f, void* self, const double* pos, float e, float r2, uint32_t shooter, int32_t weapon,
                int32_t warhead) {
    __try {
        f(self, nullptr, pos, e, r2, shooter, weapon, warhead);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

// --- Environment: verified modules, services and masks ---

struct Env {
    HMODULE ao = nullptr, mo = nullptr, wm = nullptr;
    void* obj = nullptr;
    int mcls = -1, mgrp = -1;
    void* wmgr = nullptr;
    void* wld = nullptr;
    uint32_t ao_remote = 0, mo_remote = 0;
};

bool load_env(Env& e) {
    e.ao = GetModuleHandleW(L"AircraftObject.dll");
    e.mo = GetModuleHandleW(L"MissileObject.dll");
    e.wm = GetModuleHandleW(L"WorldManager.dll");
    if (!e.ao || !e.mo || !e.wm) {
        LOG("wprobe: AircraftObject %p, MissileObject %p, WorldManager %p: not all loaded (no flight?)", e.ao, e.mo,
            e.wm);
        return false;
    }
    if (!is_expected_build(e.ao, kAoStamp, kAoSize, "AircraftObject.dll") ||
        !is_expected_build(e.mo, kMoStamp, kMoSize, "MissileObject.dll") ||
        !is_expected_build(e.wm, kWmStamp, kWmSize, "WorldManager.dll")) {
        return false;
    }
    rd(at_rva(e.ao, kObjServiceRva), 0, &e.obj);
    rd(at_rva(e.ao, kMissileClassRva), 0, &e.mcls);
    rd(at_rva(e.ao, kMissileGroupRva), 0, &e.mgrp);
    rd(at_rva(e.ao, kWeaponMgrRva), 0, &e.wmgr);
    rd(at_rva(e.ao, kAoRemoteMaskRva), 0, &e.ao_remote);
    rd(at_rva(e.mo, kMoRemoteMaskRva), 0, &e.mo_remote);
    e.wld = at_rva(e.wm, kWldRva);
    if (!e.obj || e.mcls < 0 || e.mgrp < 0 || !vslot(e.obj, kCreateSlot) || !vslot(e.obj, kByIdSlot)) {
        LOG("wprobe: object service %p, missile class %d, group %d not ready", e.obj, e.mcls, e.mgrp);
        return false;
    }
    if (!e.wmgr || !vtable_is(e.wmgr, e.mo, kWeaponMgrVtRva)) {
        void* fallback = at_rva(e.mo, kMoWeaponMgrRva);  // MissileObject's static weapon data manager
        if (vtable_is(fallback, e.mo, kWeaponMgrVtRva)) {
            e.wmgr = fallback;
        }
    }
    if (!e.wmgr || !vtable_is(e.wmgr, e.mo, kWeaponMgrVtRva)) {
        LOG("wprobe: weapon manager %p has vtable MissileObject+0x%lx, expected +0x%lx", e.wmgr,
            e.wmgr ? vtable_rva(e.wmgr, e.mo) : 0ul, static_cast<unsigned long>(kWeaponMgrVtRva));
        return false;
    }
    if (!vtable_is(e.wld, e.wm, kWldVtRva)) {
        void* p = nullptr;  // MissileObject's cached WLD pointer
        if (rd(at_rva(e.mo, kMoWldPtrRva), 0, &p) && vtable_is(p, e.wm, kWldVtRva)) e.wld = p;
    }
    if (!vtable_is(e.wld, e.wm, kWldVtRva)) {
        LOG("wprobe: WLD at WorldManager+0x%lx has vtable +0x%lx, expected +0x%lx", static_cast<unsigned long>(kWldRva),
            vtable_rva(e.wld, e.wm), static_cast<unsigned long>(kWldVtRva));
        return false;
    }
    if (!e.ao_remote || e.ao_remote != e.mo_remote) {
        LOG("wprobe: REMOTEOBJECT masks differ or unset (AircraftObject 0x%08x, MissileObject 0x%08x)", e.ao_remote,
            e.mo_remote);
        return false;
    }
    return true;
}

void* by_id(const Env& e, uint32_t id) {
    auto f = reinterpret_cast<ObjectById_t>(vslot(e.obj, kByIdSlot));
    return f ? seh_by_id(f, e.obj, id) : nullptr;
}
/// <summary>The live aircraft with this object id, or null (gone, or the id now names something else).</summary>
void* aircraft_by_id(const Env& e, uint32_t id) {
    void* p = by_id(e, id);
    uint32_t got = 0;
    if (!p || !vtable_is(p, e.ao, kAircraftVtRva) || !rd(p, kOffId, &got) || got != id) return nullptr;
    return p;
}
void* missile_by_id(const Env& e, uint32_t id) {
    void* p = by_id(e, id);
    uint32_t got = 0;
    if (!p || !vtable_is(p, e.mo, kMissileVtRva) || !rd(p, kOffId, &got) || got != id) return nullptr;
    return p;
}

// --- Weapon types ---

const unsigned char* weapon_record(const Env& e, int32_t id) {
    int32_t count = 0;
    const unsigned char* const* arr = nullptr;
    if (!rd(e.wmgr, kMgrOffCount, &count) || !rd(e.wmgr, kMgrOffArray, &arr) || !arr) return nullptr;
    if (id < 0 || id >= count) return nullptr;
    const unsigned char* rec = nullptr;
    return safe_copy(&rec, arr + id, sizeof(rec)) ? rec : nullptr;
}
int32_t weapon_count(const Env& e) {
    int32_t count = 0;
    rd(e.wmgr, kMgrOffCount, &count);
    return count;
}
bool weapon_info(const unsigned char* rec, int32_t id, WeaponInfo* w, char* name) {
    wchar_t wn[kWNameChars] = {};
    if (!rec || !safe_copy(wn, rec, sizeof(wn)) || !rd(rec, kWOffType, &w->data_type) ||
        !rd(rec, kWOffGuidance, &w->guidance)) {
        return false;
    }
    w->id = id;
    size_t k = 0;
    for (; k < kWNameChars - 1 && wn[k]; ++k) name[k] = (wn[k] >= 0x20 && wn[k] < 0x7f) ? static_cast<char>(wn[k]) : '?';
    name[k] = '\0';
    return true;
}

struct Chosen {
    WeaponInfo w{};
    char name[kWNameChars] = {};
    const char* how = "";
    float mass = 0, radius = 0;
    int32_t warhead_type = 0;
};

char g_env_weapon[kWNameChars] = {};  // LAZYSF2_PROBE_WEAPON, read once
double g_dmg_scale = 1.0;             // LAZYSF2_PROBE_DMG_SCALE, read once
bool g_env_read = false;

void read_env_once() {
    if (g_env_read) return;
    g_env_read = true;
    char buf[64] = {};
    const DWORD n = GetEnvironmentVariableA("LAZYSF2_PROBE_WEAPON", g_env_weapon, sizeof(g_env_weapon));
    if (n == 0 || n >= sizeof(g_env_weapon)) g_env_weapon[0] = '\0';
    const DWORD s = GetEnvironmentVariableA("LAZYSF2_PROBE_DMG_SCALE", buf, sizeof(buf));
    if (s > 0 && s < sizeof(buf)) {
        const double v = std::atof(buf);
        if (v > 0 && v < 1e6) g_dmg_scale = v;
    }
    LOG("wprobe: weapon %s%s%s, damage scale %.3g (LAZYSF2_PROBE_WEAPON, LAZYSF2_PROBE_DMG_SCALE)",
        g_env_weapon[0] ? "\"" : "", g_env_weapon[0] ? g_env_weapon : "from the loadouts seen at mission load",
        g_env_weapon[0] ? "\"" : "", g_dmg_scale);
}

bool find_weapon(const Env& e, const char* name, Chosen* c) {
    const int32_t n = weapon_count(e);
    for (int32_t id = 0; id < n; ++id) {
        const unsigned char* rec = weapon_record(e, id);
        wchar_t wn[kWNameChars] = {};
        if (rec && safe_copy(wn, rec, sizeof(wn)) && name_matches(wn, kWNameChars, name) &&
            weapon_info(rec, id, &c->w, c->name)) {
            return true;
        }
    }
    return false;
}

void fill_warhead(const Env& e, Chosen* c) {
    const unsigned char* rec = weapon_record(e, c->w.id);
    float mass = 0, radius = 0;
    int32_t type = 0;
    const bool ok = rd(rec, kWOffWarhead + 0x00, &type) && rd(rec, kWOffWarhead + 0x04, &mass) &&
                    rd(rec, kWOffWarhead + 0x0c, &radius);
    const bool sane = ok && mass > 0 && mass < 10000 && radius > 0 && radius < 2000;
    c->mass = sane ? mass : kFallbackMassKg;
    c->radius = sane ? radius : kFallbackRadius;
    c->warhead_type = sane ? type : 0;
    if (!sane) {
        LOG("wprobe: warhead at weapon +0x380 reads type %d mass %.3g radius %.3g: implausible, using %.0f kg, %.0f m",
            type, mass, radius, kFallbackMassKg, kFallbackRadius);
    }
}

bool choose_weapon(const Env& e, Chosen* c) {
    read_env_once();
    bool found = false;
    if (g_env_weapon[0]) {
        found = find_weapon(e, g_env_weapon, c);
        if (found) {
            c->how = "named by LAZYSF2_PROBE_WEAPON";
        } else {
            LOG("wprobe: weapon \"%s\" not in the %d weapon types; using the default", g_env_weapon, weapon_count(e));
        }
    }
    if (!found) {
        uint32_t ids[64];
        const size_t n = missile_spawn_seen_weapon_ids(ids, 64);
        WeaponInfo infos[64];
        size_t k = 0;
        char scratch[kWNameChars];
        for (size_t i = 0; i < n; ++i) {
            if (weapon_info(weapon_record(e, static_cast<int32_t>(ids[i])), static_cast<int32_t>(ids[i]), &infos[k],
                            scratch)) {
                ++k;
            }
        }
        const int pick = pick_default_weapon(infos, k);
        if (pick < 0) {
            LOG("wprobe: no guided weapon among the %zu types created at mission load", n);
            return false;
        }
        weapon_info(weapon_record(e, infos[pick].id), infos[pick].id, &c->w, c->name);
        c->how = is_air_to_air(c->w) ? "first air-to-air missile created at mission load (normally the player's)"
                                     : "first guided weapon created at mission load";
    }
    fill_warhead(e, c);
    return true;
}

/// <summary>An unguided rocket (type 1, guidance 0): LAZYSF2_PROBE_WEAPON if it names one, else the first stock one found.</summary>
bool choose_rocket(const Env& e, Chosen* c) {
    read_env_once();
    if (g_env_weapon[0] && find_weapon(e, g_env_weapon, c) && c->w.data_type == 1 && c->w.guidance == 0) {
        c->how = "named by LAZYSF2_PROBE_WEAPON";
        fill_warhead(e, c);
        return true;
    }
    static const char* const kStock[] = {"275rocket", "ZuniAP", "HydraHE", "HVAR", "S-5", "SNEB68", "MICRO2"};
    for (const char* name : kStock) {
        if (find_weapon(e, name, c) && c->w.data_type == 1 && c->w.guidance == 0) {
            c->how = g_env_weapon[0] ? "LAZYSF2_PROBE_WEAPON is not an unguided rocket: first stock rocket found"
                                     : "first stock rocket found";
            fill_warhead(e, c);
            return true;
        }
    }
    LOG("wprobe: no unguided rocket type (type 1, guidance 0) found");
    return false;
}

// --- Aircraft snapshot (game thread; objects are destroyed only on this thread, so pointers hold until we return) ---

struct Plane {
    void* obj;
    uint32_t id;
    double pos[3];
    float vel[3];
    int32_t level;
    bool alive;
    bool is_player;
    AircraftSide side;
    wchar_t name[32];
};
struct Snap {
    Plane p[kMaxPlanes];
    size_t n = 0;
    int player = -1;
};
Snap g_snap;

// Under the tracker's shared lock: copies only, no logging.
void snap_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& s = *static_cast<Snap*>(vctx);
    s.n = 0;
    s.player = -1;
    for (size_t i = 0; i < n && s.n < kMaxPlanes; ++i) {
        Plane& p = s.p[s.n];
        int32_t active = 0;
        if (!rd(list[i].obj, kOffId, &p.id) || !rd(list[i].obj, kOffActive, &active) ||
            !rd(list[i].obj, kOffPos, &p.pos) || !rd(list[i].obj, kOffVel, &p.vel) ||
            !rd(list[i].obj, kOffLevel, &p.level)) {
            continue;
        }
        p.obj = list[i].obj;
        p.alive = active != 0 && p.level < kDestroyedLevel;
        p.is_player = list[i].is_player;
        p.side = list[i].side;
        size_t k = 0;
        if (list[i].name) {
            for (; k < 31 && list[i].name[k]; ++k) p.name[k] = list[i].name[k];
        }
        p.name[k] = L'\0';
        if (p.is_player) s.player = static_cast<int>(s.n);
        ++s.n;
    }
}

const char* side_name(AircraftSide s) {
    switch (s) {
        case AircraftSide::Friendly: return "friendly";
        case AircraftSide::Enemy: return "enemy";
        case AircraftSide::Neutral: return "neutral";
        default: return "side?";
    }
}

bool take_snapshot() {
    probe_with_aircraft(snap_cb, &g_snap);
    if (g_snap.player < 0) {
        LOG("wprobe: no player aircraft tracked (%zu aircraft); nothing done", g_snap.n);
        return false;
    }
    return true;
}

size_t candidates(Candidate* c) {
    for (size_t i = 0; i < g_snap.n; ++i) {
        const Plane& p = g_snap.p[i];
        c[i] = {{p.pos[0], p.pos[1], p.pos[2]}, p.is_player, p.alive, p.side, p.name};
    }
    return g_snap.n;
}

// --- Target watch ---

struct TargetState {
    bool ok = false;
    int32_t active = 0, level = 0, weapon = 0;
    uint32_t attacker = 0, flags2 = 0, warn = 0;
    double pos[3] = {};
};
TargetState read_target(void* obj) {
    TargetState t;
    t.ok = obj && rd(obj, kOffActive, &t.active) && rd(obj, kOffLevel, &t.level) &&
           rd(obj, kOffAttacker, &t.attacker) && rd(obj, kOffAttackerWeapon, &t.weapon) &&
           rd(obj, kOffFlags2, &t.flags2) && rd(obj, kOffWarn, &t.warn) && rd(obj, kOffPos, &t.pos);
    return t;
}
void log_target(const char* tag, const TargetState& t) {
    LOG("wprobe: %s target damage level +0x16c %d, last attacker +0x318 0x%08x weapon +0x31c %d, active %d, +0x168 "
        "0x%08x, warnings +0x5f4 0x%08x (bits 2/4: %s/%s)",
        tag, t.level, t.attacker, t.weapon, t.active, t.flags2, t.warn, (t.warn & 4) ? "on" : "off",
        (t.warn & 0x10) ? "on" : "off");
}

struct Watch {
    bool on = false;
    const char* what = "";
    uint32_t target_id = 0;
    char target_name[32] = {};
    uint32_t missile_id = 0xffffffffu;  // none
    bool missile_gone = false;
    double missile_pos[3] = {};
    LARGE_INTEGER start{};
    double next_log = 1.0;
    TargetState before, last;
};
Watch g_watch;
LARGE_INTEGER g_freq{};

double seconds_since(const LARGE_INTEGER& s) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - s.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

void narrow(const wchar_t* w, char* out, size_t n) {
    size_t k = 0;
    for (; k + 1 < n && w[k]; ++k) out[k] = (w[k] >= 0x20 && w[k] < 0x7f) ? static_cast<char>(w[k]) : '?';
    out[k] = '\0';
}

void start_watch(const char* what, const Plane& target, uint32_t missile_id) {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    g_watch = Watch{};
    g_watch.on = true;
    g_watch.what = what;
    g_watch.target_id = target.id;
    narrow(target.name, g_watch.target_name, sizeof(g_watch.target_name));
    g_watch.missile_id = missile_id;
    g_watch.before = read_target(target.obj);
    g_watch.last = g_watch.before;
    log_target("before:", g_watch.before);
    QueryPerformanceCounter(&g_watch.start);
}

void end_watch(const char* why, const TargetState& now) {
    if (g_watch.missile_id != 0xffffffffu && !g_watch.missile_gone) {
        LOG("wprobe: missile 0x%08x still flying at the watch end, at (%.0f, %.0f, %.0f), %.0f m from the target",
            g_watch.missile_id, g_watch.missile_pos[0], g_watch.missile_pos[1], g_watch.missile_pos[2],
            now.ok ? distance(g_watch.missile_pos, now.pos) : -1.0);
    }
    LOG("wprobe: %s watch end (%s) after %.1f s: target %s 0x%08x level %d -> %d, attacker 0x%08x -> 0x%08x%s",
        g_watch.what, why, seconds_since(g_watch.start), g_watch.target_name, g_watch.target_id, g_watch.before.level,
        now.ok ? now.level : -1, g_watch.before.attacker, now.ok ? now.attacker : 0u,
        now.ok && now.level > g_watch.before.level ? " -> DAMAGED" : " -> no damage change");
    g_watch.on = false;
}

void watch_frame() {
    Env e;
    if (!load_env(e)) {
        end_watch("modules gone", TargetState{});
        return;
    }
    const double t = seconds_since(g_watch.start);
    void* target = aircraft_by_id(e, g_watch.target_id);
    const TargetState now = read_target(target);
    if (!now.ok) {
        LOG("wprobe: target 0x%08x gone (destroyed and removed?) at %.1f s", g_watch.target_id, t);
        end_watch("target gone", g_watch.last);
        return;
    }
    if (now.level != g_watch.last.level || now.attacker != g_watch.last.attacker || now.active != g_watch.last.active ||
        now.warn != g_watch.last.warn) {
        char tag[48];
        std::snprintf(tag, sizeof(tag), "t+%.2fs change:", t);
        log_target(tag, now);
    }
    g_watch.last = now;
    if (g_watch.missile_id != 0xffffffffu && !g_watch.missile_gone) {
        void* m = missile_by_id(e, g_watch.missile_id);
        int32_t active = 0;
        if (!m || !rd(m, kOffActive, &active) || !active) {
            g_watch.missile_gone = true;
            LOG("wprobe: missile 0x%08x ended at t+%.2fs, last at (%.0f, %.0f, %.0f), %.0f m from the target",
                g_watch.missile_id, t, g_watch.missile_pos[0], g_watch.missile_pos[1], g_watch.missile_pos[2],
                distance(g_watch.missile_pos, now.pos));
        } else {
            rd(m, kMOffPos, &g_watch.missile_pos);
        }
    }
    if (t >= g_watch.next_log) {
        const bool flying = g_watch.missile_id != 0xffffffffu && !g_watch.missile_gone;
        const double step = flying ? 0.5 : 1.0;
        g_watch.next_log = std::floor(t / step) * step + step;
        if (flying) {
            void* m = missile_by_id(e, g_watch.missile_id);
            uint32_t seeker = 0, tid = 0, nodmg = 0, flags = 0;
            rd(m, kMOffSeeker, &seeker);
            rd(m, kMOffTargetId, &tid);
            rd(m, kMOffNoDamage, &nodmg);
            rd(m, kOffFlags, &flags);
            LOG("wprobe: t=%.1fs missile at (%.0f, %.0f, %.0f) range %.0f m, its target +0x198 0x%08x, seeker +0x178 "
                "0x%08x, no-damage +0x1ac %u, flags 0x%08x | target level %d, warnings 0x%08x",
                t, g_watch.missile_pos[0], g_watch.missile_pos[1], g_watch.missile_pos[2],
                distance(g_watch.missile_pos, now.pos), tid, seeker, nodmg, flags, now.level, now.warn);
        } else {
            LOG("wprobe: t=%.0fs target level %d, attacker 0x%08x, active %d, warnings 0x%08x", t, now.level,
                now.attacker, now.active, now.warn);
        }
    }
    if (t >= kWatchSeconds) end_watch("time", now);
}

// --- Actions ---

/// <summary>
/// The 9.2 recipe: CreateObject at pose, SetShooter, SetTarget (0xffffffff: none), Release(0), link slot 6 (1), with
/// the shooter REMOTEOBJECT around the release if remote. Returns the missile id, or 0xffffffff after logging why.
/// </summary>
uint32_t launch_missile(const Env& e, const Chosen& w, void* shooter, uint32_t target_id, const LaunchPose& pose,
                        double speed, bool remote) {
    alignas(8) unsigned char desc[kDescSize] = {};
    const int16_t one = 1;
    const int32_t minus1 = -1;
    memcpy(desc + 0x00, &w.w.id, 4);
    memcpy(desc + 0x04, &one, 2);
    memcpy(desc + 0x08, &minus1, 4);
    memcpy(desc + 0x0c, &minus1, 4);
    memcpy(desc + 0x10, pose.pos, sizeof(pose.pos));
    memcpy(desc + 0x28, pose.angles, sizeof(pose.angles));
    memcpy(desc + 0x34, pose.vel, sizeof(pose.vel));
    auto create = reinterpret_cast<CreateObject_t>(vslot(e.obj, kCreateSlot));
    uint32_t id = 0xffffffffu;
    if (DWORD x = seh_create(create, e.obj, e.mcls, e.mgrp, desc, &id)) {
        LOG("wprobe: CreateObject faulted (0x%08lx); nothing more done", x);
        return 0xffffffffu;
    }
    void* m = id == 0xffffffffu ? nullptr : missile_by_id(e, id);
    void* link = m ? static_cast<char*>(m) + kMOffLink : nullptr;
    void* guid = m ? static_cast<char*>(m) + kMOffGuidance : nullptr;
    LOG("wprobe: CreateObject(class %d, group %d) -> id 0x%08x obj %p at (%.0f, %.0f, %.0f), angles p %.1f h %.1f "
        "deg, speed %.0f m/s",
        e.mcls, e.mgrp, id, m, pose.pos[0], pose.pos[1], pose.pos[2], pose.angles[0] * 57.29578f,
        pose.angles[1] * 57.29578f, speed);
    if (!m || !vtable_is(link, e.mo, kLinkVtRva) || !vtable_is(guid, e.mo, kGuidanceVtRva)) {
        LOG("wprobe: no verified missile (link vtable +0x%lx, guidance vtable +0x%lx); not launching",
            m ? vtable_rva(link, e.mo) : 0ul, m ? vtable_rva(guid, e.mo) : 0ul);
        return 0xffffffffu;
    }
    const void* data = nullptr;
    rd(m, kMOffData, &data);
    if (!data) {
        LOG("wprobe: the missile has no weapon data (Init refused the id); not launching");
        return 0xffffffffu;
    }
    DWORD x = seh_ptr(reinterpret_cast<SetPtr_t>(vslot(link, kLinkShooterSlot)), link, shooter);
    if (!x) x = seh_u32(reinterpret_cast<SetU32_t>(vslot(guid, kGuidanceSetTargetSlot)), guid, target_id);
    if (x) {
        LOG("wprobe: SetShooter/SetTarget faulted (0x%08lx); not releasing", x);
        return 0xffffffffu;
    }
    uint32_t tid = 0;
    rd(m, kMOffTargetId, &tid);
    LOG("wprobe: SetShooter(%p) and SetTarget(0x%08x) done; missile target +0x198 0x%08x%s", shooter, target_id, tid,
        tid == target_id ? "" : " (refused: target not active?)");
    uint32_t sflags = 0;
    rd(shooter, kOffFlags, &sflags);
    const bool was_remote = (sflags & e.ao_remote) != 0;
    if (remote && !was_remote) wr_u32(shooter, kOffFlags, sflags | e.ao_remote);
    x = seh_ptr(reinterpret_cast<SetPtr_t>(vslot(link, kLinkReleaseSlot)), link, nullptr);
    if (!x) x = seh_u32(reinterpret_cast<SetU32_t>(vslot(link, kLinkSlot6)), link, 1);
    if (remote && !was_remote) {  // only the release reads it: the AI gets the aircraft back at once
        uint32_t f = 0;
        if (rd(shooter, kOffFlags, &f)) wr_u32(shooter, kOffFlags, f & ~e.ao_remote);
    }
    if (x) {
        LOG("wprobe: Release faulted (0x%08lx)", x);
        return 0xffffffffu;
    }
    uint32_t mflags = 0, seeker = 0, nodmg = 0;
    const void* shooter_back = nullptr;
    rd(m, kOffFlags, &mflags);
    rd(m, kMOffSeeker, &seeker);
    rd(m, kMOffNoDamage, &nodmg);
    rd(m, kMOffShooter, &shooter_back);
    LOG("wprobe: Release(0) done%s: missile flags 0x%08x, seeker +0x178 0x%08x, no-damage +0x1ac %u (%s), shooter "
        "+0x100 %p",
        remote ? (was_remote ? " (shooter was already REMOTEOBJECT)" : " (shooter REMOTEOBJECT set and cleared around it)")
               : "",
        mflags, seeker, nodmg, nodmg ? "visual only" : "live", shooter_back);
    return id;
}

double speed_of(const float v[3]) {
    return std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
}

void do_launch(WeaponsProbeAction a) {
    Env e;
    if (!load_env(e) || !take_snapshot()) return;
    Candidate c[kMaxPlanes];
    const size_t n = candidates(c);
    const int player = g_snap.player;
    int shooter = pick_wingman(c, n, player);
    const char* shooter_how = "wingman";
    if (shooter < 0) {
        shooter = nearest_ai(c, n, c[player].pos, player);
        shooter_how = "no wingman found: nearest AI";
    }
    if (shooter < 0) {
        LOG("wprobe: no live AI aircraft to credit as the shooter; nothing done");
        return;
    }
    const bool remote = a != WeaponsProbeAction::LiveAtPlayer;
    int target = player;
    if (a == WeaponsProbeAction::RemoteAtAi) {
        target = nearest_ai(c, n, c[shooter].pos, shooter);
        if (target < 0) {
            LOG("wprobe: no other live AI aircraft to aim at; nothing done");
            return;
        }
    }
    Chosen w;
    if (!choose_weapon(e, &w)) return;
    const Plane& sp = g_snap.p[shooter];
    const Plane& tp = g_snap.p[target];
    // The missile is spawned on the target's tail, not at the shooter: a launch from the shooter's own position was
    // inside the minimum range (round 1). The shooter only gets the credit and the release rules.
    double from[3];
    tail_point(tp.pos, tp.vel, kTailDistance, from);
    double speed = speed_of(tp.vel) + kOvertake;
    if (speed < kMinLaunchSpeed) speed = kMinLaunchSpeed;
    LaunchPose pose{};
    if (!launch_pose(from, tp.pos, 0.0, speed, &pose)) {
        LOG("wprobe: no launch geometry; nothing done");
        return;
    }
    char sname[32], tname[32];
    narrow(sp.name, sname, sizeof(sname));
    narrow(tp.name, tname, sizeof(tname));
    LOG("wprobe: launch (%s): shooter %s \"%s\" 0x%08x (%s, %.0f m from the target) -> target \"%s\" 0x%08x (%s%s, "
        "%.0f m/s); spawned %.0f m behind the target on its tail, same altitude, nose on, %.0f m/s; weapon \"%s\" id %d "
        "type %d guidance %d (%s)",
        remote ? "shooter REMOTEOBJECT for the release" : "live", shooter_how, sname, sp.id, side_name(sp.side),
        distance(sp.pos, tp.pos), tname, tp.id, side_name(tp.side), tp.is_player ? ", the player" : "",
        speed_of(tp.vel), pose.range, speed, w.name, w.w.id, w.w.data_type, w.w.guidance, w.how);
    const uint32_t id = launch_missile(e, w, sp.obj, tp.id, pose, speed, remote);
    if (id == 0xffffffffu) return;
    start_watch(a == WeaponsProbeAction::LiveAtPlayer ? "live launch at the player"
                : a == WeaponsProbeAction::RemoteAtAi ? "remote launch at an AI"
                                                     : "remote launch at the player",
                tp, id);
}

void do_rocket() {
    Env e;
    if (!load_env(e) || !take_snapshot()) return;
    Chosen w;
    if (!choose_rocket(e, &w)) return;
    const Plane& pp = g_snap.p[g_snap.player];
    float m[9] = {}, ang[3] = {};
    if (!rd(pp.obj, kOffMatrix, &m) || !rd(pp.obj, kOffAngles, &ang)) {
        LOG("wprobe: could not read the player's attitude; nothing done");
        return;
    }
    const float* fwd = m + 3;  // rows right, forward, up
    LaunchPose pose{};
    for (int i = 0; i < 3; ++i) {
        pose.pos[i] = pp.pos[i] + fwd[i] * kRocketOffset;
        pose.vel[i] = pp.vel[i];
        pose.angles[i] = ang[i];
    }
    pose.range = 0;
    const double speed = speed_of(pose.vel);
    LOG("wprobe: rocket: the player 0x%08x fires \"%s\" id %d type %d guidance %d (%s) %.0f m ahead of its nose, at "
        "its velocity, no target",
        pp.id, w.name, w.w.id, w.w.data_type, w.w.guidance, w.how, kRocketOffset);
    const uint32_t id = launch_missile(e, w, pp.obj, 0xffffffffu, pose, speed, false);
    if (id == 0xffffffffu) return;
    start_watch("rocket ahead of the player", pp, id);
}

void do_damage(WeaponsProbeAction a) {
    Env e;
    if (!load_env(e) || !take_snapshot()) return;
    Candidate c[kMaxPlanes];
    const size_t n = candidates(c);
    const int player = g_snap.player;
    const int ti = nearest_ai(c, n, c[player].pos, player);
    if (ti < 0) {
        LOG("wprobe: no live AI aircraft to damage; nothing done");
        return;
    }
    const Plane& tp = g_snap.p[ti];
    const Plane& pp = g_snap.p[player];
    Chosen w;
    if (!choose_weapon(e, &w)) {
        w.w.id = -1;
        w.mass = kFallbackMassKg;
        w.radius = kFallbackRadius;
        std::snprintf(w.name, sizeof(w.name), "none");
    }
    char tname[32];
    narrow(tp.name, tname, sizeof(tname));
    const double range = distance(tp.pos, pp.pos);
    const float blast_e = static_cast<float>(w.mass * kExplosiveJoulesPerKg * g_dmg_scale);
    const char* what = a == WeaponsProbeAction::HitQueue ? "WLD+0x44 hit" : a == WeaponsProbeAction::Explosion
                                                                              ? "WLD+0x40 explosion"
                                                                              : "QI 9 blast";
    LOG("wprobe: %s on \"%s\" 0x%08x (%s, %.0f m from the player), shooter = the player 0x%08x, weapon \"%s\" id %d",
        what, tname, tp.id, side_name(tp.side), range, pp.id, w.name, w.w.id);
    start_watch(what, tp, 0xffffffffu);
    DWORD x = 0;
    if (a == WeaponsProbeAction::HitQueue) {
        int32_t count = 0, cap = 0;
        void* arr = nullptr;
        rd(e.wld, kWldHitCount, &count);
        rd(e.wld, kWldHitCap, &cap);
        rd(e.wld, kWldHitArray, &arr);
        if (!arr || count >= cap) {
            LOG("wprobe: WLD hit queue not usable (array %p, %d of %d); nothing queued", arr, count, cap);
            g_watch.on = false;
            return;
        }
        const double from[3] = {tp.pos[0], tp.pos[1], tp.pos[2] + 30.0};
        unsigned char rec[kHitRecordSize];
        const float energy = static_cast<float>(kKineticJoules * g_dmg_scale);
        build_hit_record(rec, tp.id, 1, energy, 0.f, from, tp.pos, pp.id, w.w.id);
        x = seh_ptr(reinterpret_cast<SetPtr_t>(vslot(e.wld, kWldHitSlot)), e.wld, rec);
        LOG("wprobe: queued kind 1 hit, %.3g J, segment from 30 m above to the centre (queue had %d of %d)%s", energy,
            count, cap, x ? " -- FAULTED" : "");
    } else if (a == WeaponsProbeAction::Explosion) {
        int32_t count = 0, cap = 0;
        void* arr = nullptr;
        rd(e.wld, kWldExpCount, &count);
        rd(e.wld, kWldExpCap, &cap);
        rd(e.wld, kWldExpArray, &arr);
        if (!arr || count >= cap) {
            LOG("wprobe: WLD explosion queue not usable (array %p, %d of %d); nothing queued", arr, count, cap);
            g_watch.on = false;
            return;
        }
        unsigned char rec[kExplosionRecordSize];
        build_explosion_record(rec, tp.pos, blast_e, w.radius, w.w.id, pp.id, w.warhead_type);
        x = seh_ptr(reinterpret_cast<SetPtr_t>(vslot(e.wld, kWldExplosionSlot)), e.wld, rec);
        LOG("wprobe: queued explosion at the target, %.3g J (%.1f kg), radius %.1f m, warhead type %d, no effect or "
            "sound (queue had %d of %d)%s",
            blast_e, w.mass, w.radius, w.warhead_type, count, cap, x ? " -- FAULTED" : "");
    } else {
        void* iface = nullptr;
        x = seh_qi(reinterpret_cast<QI_t>(vslot(tp.obj, kQiSlot)), tp.obj, kDamageIid, &iface);
        if (x || !iface || !vtable_is(iface, e.ao, kDamageIfaceVtRva)) {
            LOG("wprobe: QI(9) gave %p (vtable +0x%lx, expected +0x%lx)%s; not calling", iface,
                iface ? vtable_rva(iface, e.ao) : 0ul, static_cast<unsigned long>(kDamageIfaceVtRva),
                x ? " -- FAULTED" : "");
            g_watch.on = false;
            return;
        }
        const float r2 = w.radius * w.radius;
        x = seh_blast(reinterpret_cast<Blast_t>(vslot(iface, kBlastSlot)), iface, tp.pos, blast_e, r2, pp.id, w.w.id,
                      w.warhead_type);
        LOG("wprobe: damage slot 6 blast at the target centre, %.3g J, radius %.1f m (r2 %.0f)%s", blast_e, w.radius, r2,
            x ? " -- FAULTED" : "");
        log_target("right after the call:", read_target(tp.obj));
    }
    if (x) g_watch.on = false;
}

// --- Gun stations (game thread: the detours run inside the aircraft's Update) ---
// Two gun loops: the aircraft's own guns (+0x4f6d0) and gun pods (+0x50220). See lazy-sf2-recon: network-recon §9.12.

enum class GunKind : uint8_t { Internal, Pod };

struct GunStation {
    void* st = nullptr;
    GunKind kind = GunKind::Internal;
    unsigned rounds = 0, frames = 0, trig_frames = 0, firing_frames = 0;
    // Internal: the trigger input the loop reads, the selection and inhibit bits, the flag word.
    float input = 0;
    int32_t input_index = -1;
    bool selected = false, inhibit = false;
    uint32_t flags = 0;
    // Pod: byte (+0x30)+0x30.
    uint8_t pod_byte = 0;
    int16_t ammo = 0, burst = 0, left = 0;
    float timer = 0;
};
GunStation g_gun[kMaxGunStations];
SecondWindow g_gun_window;
bool g_gun_was_active = false;
unsigned g_gun_total = 0;
bool g_gun_reset = false;  // game thread
std::atomic<void*> g_player_obj{nullptr};
std::atomic<uint32_t> g_player_id{0xffffffffu};
std::atomic<uintptr_t> g_ao_base{0};  // the AircraftObject.dll image the gun hooks are on
// Counters since the last report (game thread).
unsigned g_calls_internal = 0, g_calls_internal_player = 0, g_calls_pod = 0, g_calls_pod_player = 0;
std::atomic<unsigned> g_bullets_player{0}, g_bullets_other{0};
// Force fire (game thread).
bool g_force_on = false;
double g_force_until = 0;
unsigned g_force_writes = 0, g_force_bullets = 0;

GunUpdate_t o_GunPod = nullptr;
GunUpdate_t o_GunInternal = nullptr;
BulletInit_t o_BulletInit = nullptr;

GunStation* station_slot(void* st, GunKind kind) {
    for (GunStation& s : g_gun) {
        if (s.st == st) return &s;
    }
    for (GunStation& s : g_gun) {
        if (!s.st) {
            s = GunStation{};
            s.st = st;
            s.kind = kind;
            LOG("wprobe gun: player %s gun station %p seen", kind == GunKind::Internal ? "internal" : "pod", st);
            return &s;
        }
    }
    return nullptr;
}

bool owned_by_player(void* st) {
    void* player = g_player_obj.load(std::memory_order_relaxed);
    void** ctx = nullptr;
    void* owner = nullptr;
    return player && rd(st, kGOffContext, &ctx) && ctx && rd(ctx, 0, &owner) && owner == player;
}

/// <summary>The trigger input slot the internal gun loop reads, or null; also its index and the selection bits.</summary>
float* internal_trigger(void* st, int32_t* index, bool* selected, bool* inhibit) {
    const uintptr_t ao = g_ao_base.load(std::memory_order_relaxed);
    void* ctx = nullptr;
    unsigned char* ctl = nullptr;
    void* data = nullptr;
    float* inputs = nullptr;
    int32_t idx = -1, bit = 0, count = 0;
    uint32_t sel = 0;
    uint8_t b0 = 0;
    if (!ao || !rd(st, kGOffContext, &ctx) || !rd(ctx, kCtxOffControl, &ctl) || !ctl || !rd(st, kGOffData, &data) ||
        !rd(data, kDataOffInput, &idx) || !rd(data, kDataOffSelectBit, &bit) ||
        !rd(reinterpret_cast<const void*>(ao + kInputCountRva), 0, &count) || !rd(ctl, kCtlOffSelected, &sel) ||
        !rd(ctl, 0, &b0) || !rd(ctl, kCtlOffInputs, &inputs)) {
        return nullptr;
    }
    *index = idx;
    *selected = ((sel >> (bit & 31)) & 1) != 0;
    *inhibit = (b0 & 0x20) != 0;
    if (idx < 0 || idx >= count || !inputs) return nullptr;
    return inputs + idx;
}

void __fastcall h_GunInternal(void* st) {
    ++g_calls_internal;
    const bool mine = owned_by_player(st);
    int16_t ammo = 0, burst = 0;
    if (mine) {
        ++g_calls_internal_player;
        rd(st, kIOffAmmo, &ammo);
        rd(st, kIOffBurst, &burst);
        if (g_force_on) {
            int32_t idx = -1;
            bool sel = false, inh = false;
            if (float* trig = internal_trigger(st, &idx, &sel, &inh)) {
                if (sim_game_time() < g_force_until) {
                    if (wr_f32(trig, 1.0f)) ++g_force_writes;
                } else {
                    wr_f32(trig, 0.0f);
                    g_force_on = false;
                    LOG("wprobe gun: force fire done: input[%d] held at 1.0 for %u updates, then set to 0; %u player "
                        "BulletObjects since the start",
                        idx, g_force_writes, g_bullets_player.load() + g_force_bullets);
                }
            }
        }
    }
    o_GunInternal(st);
    if (!mine) return;
    GunStation* g = station_slot(st, GunKind::Internal);
    if (!g) return;
    float* trig = internal_trigger(st, &g->input_index, &g->selected, &g->inhibit);
    g->input = 0;
    if (trig) rd(trig, 0, &g->input);
    rd(st, kIOffFlags, &g->flags);
    rd(st, kIOffAmmo, &g->ammo);
    rd(st, kIOffBurst, &g->burst);
    rd(st, kIOffTimer, &g->timer);
    g->rounds += rounds_fired(ammo, g->ammo, burst, g->burst);
    ++g->frames;
    if (g->input > 0) ++g->trig_frames;
    if (g->flags & 1) ++g->firing_frames;
}

void __fastcall h_GunPod(void* st) {
    ++g_calls_pod;
    const bool mine = owned_by_player(st);
    int16_t ammo = 0, burst = 0;
    if (mine) {
        ++g_calls_pod_player;
        rd(st, kPOffAmmo, &ammo);
        rd(st, kPOffBurst, &burst);
    }
    o_GunPod(st);
    if (!mine) return;
    GunStation* g = station_slot(st, GunKind::Pod);
    if (!g) return;
    void* trig_obj = nullptr;
    int16_t firing = 0;
    g->pod_byte = 0;
    if (rd(st, kPOffTrigger, &trig_obj) && trig_obj) rd(trig_obj, kPodTriggerByte, &g->pod_byte);
    rd(st, kPOffFiring, &firing);
    rd(st, kPOffAmmo, &g->ammo);
    rd(st, kPOffBurst, &g->burst);
    rd(st, kPOffRounds, &g->left);
    rd(st, kPOffTimer, &g->timer);
    g->rounds += rounds_fired(ammo, g->ammo, burst, g->burst);
    ++g->frames;
    if (g->pod_byte & kPodTriggerBit) ++g->trig_frames;
    if (firing) ++g->firing_frames;
}

void __fastcall h_BulletInit(void* self, void* edx, const uint32_t* desc) {
    o_BulletInit(self, edx, desc);
    uint32_t shooter = 0xffffffffu;
    rd(desc, 0x3c, &shooter);
    if (shooter == g_player_id.load(std::memory_order_relaxed)) {
        g_bullets_player.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_bullets_other.fetch_add(1, std::memory_order_relaxed);
    }
}

/// <summary>The virtual keys held now, as "0x20 0x11 ..." (at most 8); the count is returned.</summary>
int held_keys(char* out, size_t n) {
    int count = 0;
    size_t k = 0;
    out[0] = '\0';
    for (int vk = 0x08; vk <= 0xfe && count < 8; ++vk) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;  // the L/R codes are listed instead
        if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
        const int w = std::snprintf(out + k, n - k, "%s0x%02x", count ? " " : "", vk);
        if (w < 0 || static_cast<size_t>(w) >= n - k) break;
        k += static_cast<size_t>(w);
        ++count;
    }
    return count;
}

/// <summary>Once a game second (the player's step): a line while a key is held, the player fires or force fire runs.</summary>
void gun_report(double t) {
    unsigned rounds = 0, trig = 0, firing = 0;
    for (const GunStation& g : g_gun) {
        rounds += g.rounds;
        trig += g.trig_frames;
        firing += g.firing_frames;
    }
    char keys[64];
    const int nkeys = held_keys(keys, sizeof(keys));
    const unsigned bp = g_bullets_player.exchange(0), bo = g_bullets_other.exchange(0);
    if (g_force_on) g_force_bullets += bp;
    const bool active = nkeys || rounds || trig || firing || bp || g_force_on;
    if (active) {
        char line[1024];
        int k = std::snprintf(line, sizeof(line),
                              "wprobe gun: t=%.1f keys held [%s]; BulletObjects created: player %u, others %u; "
                              "rounds counted %u; gun updates for the player: internal %u of %u, pod %u of %u%s",
                              t, keys, bp, bo, rounds, g_calls_internal_player, g_calls_internal, g_calls_pod_player,
                              g_calls_pod, g_force_on ? "; FORCE FIRE on" : "");
        for (const GunStation& g : g_gun) {
            if (!g.st || k < 0 || static_cast<size_t>(k) >= sizeof(line)) continue;
            if (g.kind == GunKind::Internal) {
                k += std::snprintf(line + k, sizeof(line) - k,
                                   " | internal %p: %u rounds, input[%d] %.2f (>0 in %u/%u updates), selected %d, "
                                   "inhibit %d, +0x38 0x%x (bit0 in %u), ammo +0x3e %d, burst +0x3c %d, timer +0x30 %.3f",
                                   g.st, g.rounds, g.input_index, g.input, g.trig_frames, g.frames, g.selected ? 1 : 0,
                                   g.inhibit ? 1 : 0, g.flags, g.firing_frames, g.ammo, g.burst, g.timer);
            } else {
                k += std::snprintf(line + k, sizeof(line) - k,
                                   " | pod %p: %u rounds, byte (+0x30)+0x30 0x%02x (bit 0x20 in %u/%u), +0x48 in %u, "
                                   "ammo +0x4e %d, burst +0x4a %d, +0x4c %d, timer +0x40 %.3f",
                                   g.st, g.rounds, g.pod_byte, g.trig_frames, g.frames, g.firing_frames, g.ammo, g.burst,
                                   g.left, g.timer);
            }
        }
        LOG("%s", line);
        g_gun_total += bp;
        g_gun_was_active = true;
    } else if (g_gun_was_active) {
        LOG("wprobe gun: t=%.1f quiet again (no key, no rounds); %u player BulletObjects in that stretch", t, g_gun_total);
        g_gun_was_active = false;
        g_gun_total = 0;
    }
    for (GunStation& g : g_gun) g.rounds = g.frames = g.trig_frames = g.firing_frames = 0;
    g_calls_internal = g_calls_internal_player = g_calls_pod = g_calls_pod_player = 0;
}

void on_step(void* aircraft, float, bool is_player) {
    if (!is_player) return;
    if (aircraft != g_player_obj.load(std::memory_order_relaxed)) {
        uint32_t id = 0xffffffffu;
        rd(aircraft, kOffId, &id);
        g_player_obj.store(aircraft, std::memory_order_relaxed);
        g_player_id.store(id, std::memory_order_relaxed);
        g_gun_reset = true;
    }
    if (g_gun_reset) {  // a new player: its stations are new
        g_gun_reset = false;
        for (GunStation& s : g_gun) s = GunStation{};
        g_gun_window = SecondWindow{};
        g_gun_was_active = false;
        g_gun_total = 0;
        g_force_on = false;
    }
    const double t = sim_game_time();
    if (g_gun_window.tick(t)) gun_report(t);
}

void do_force_fire() {
    bool have_internal = false;
    for (const GunStation& g : g_gun) have_internal = have_internal || (g.st && g.kind == GunKind::Internal);
    if (!have_internal) {
        LOG("wprobe gun: force fire: no internal gun station of the player seen yet (hook %s, player %p); nothing done",
            g_ao_base.load() ? "on" : "off", g_player_obj.load());
        return;
    }
    g_force_until = sim_game_time() + kForceFireSeconds;
    g_force_on = true;
    g_force_writes = 0;
    g_force_bullets = 0;
    LOG("wprobe gun: force fire: holding the player's gun trigger input at 1.0 for %.0f s of game time", kForceFireSeconds);
}

// --- Hook installation (module watch, any thread) ---

struct ModHook {
    const wchar_t* module;
    const char* module_name;
    DWORD stamp, size, rva;
    void* detour;
    void** original;
    const char* what;
    HMODULE base;
    HMODULE bad;
};
ModHook g_hooks[] = {
    {L"AircraftObject.dll", "AircraftObject.dll", kAoStamp, kAoSize, kGunInternalRva,
     reinterpret_cast<void*>(&h_GunInternal), reinterpret_cast<void**>(&o_GunInternal),
     "AircraftObject gun update (weapons probe)", nullptr, nullptr},
    {L"AircraftObject.dll", "AircraftObject.dll", kAoStamp, kAoSize, kGunPodRva, reinterpret_cast<void*>(&h_GunPod),
     reinterpret_cast<void**>(&o_GunPod), "AircraftObject gun pod update (weapons probe)", nullptr, nullptr},
    {L"BulletObject.dll", "BulletObject.dll", kBoStamp, kBoSize, kBulletInitRva,
     reinterpret_cast<void*>(&h_BulletInit), reinterpret_cast<void**>(&o_BulletInit),
     "BulletObject Init (weapons probe)", nullptr, nullptr},
};
SRWLOCK g_install_lock = SRWLOCK_INIT;  // guards g_hooks

bool image_still_loaded(HMODULE base, const wchar_t* module) {
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(base), &h) ||
        h != base) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(base, path, MAX_PATH);
    const size_t len = wcslen(module);
    if (n >= MAX_PATH || n <= len) return false;
    return path[n - len - 1] == L'\\' &&
           CompareStringOrdinal(path + n - len, static_cast<int>(len), module, static_cast<int>(len), TRUE) ==
               CSTR_EQUAL;
}

/// <summary>Hooks h on the loaded image of its module, re-hooking a reloaded image. Under g_install_lock.</summary>
void ensure_hook(ModHook& h) {
    HMODULE m = GetModuleHandleW(h.module);
    if (!m) return;
    void* target = at_rva(m, h.rva);
    if (m == h.base && *static_cast<const BYTE*>(target) == 0xE9) return;  // still our JMP
    if (m == h.bad) return;
    if (!is_expected_build(m, h.stamp, h.size, h.module_name)) {
        h.bad = m;
        return;
    }
    if (h.base && h.base != m && image_still_loaded(h.base, h.module)) MH_RemoveHook(at_rva(h.base, h.rva));
    h.base = nullptr;
    MH_RemoveHook(target);  // a stale entry from an earlier image at this base; MH_ERROR_NOT_CREATED otherwise
    if (create_and_enable(target, h.detour, h.original, h.what)) h.base = m;
}

void do_list() {
    Env e;
    if (!load_env(e)) return;
    read_env_once();
    uint32_t ids[64];
    const size_t n = missile_spawn_seen_weapon_ids(ids, 64);
    LOG("wprobe: %d weapon types; %zu created at mission load:", weapon_count(e), n);
    for (size_t i = 0; i < n; ++i) {
        WeaponInfo w{};
        char name[kWNameChars];
        if (weapon_info(weapon_record(e, static_cast<int32_t>(ids[i])), static_cast<int32_t>(ids[i]), &w, name)) {
            LOG("wprobe:   id %d \"%s\" type %d guidance %d%s", w.id, name, w.data_type, w.guidance,
                is_air_to_air(w) ? " (air-to-air)" : "");
        }
    }
    Chosen c;
    if (choose_weapon(e, &c)) {
        LOG("wprobe: the launch keys use \"%s\" id %d (%s); warhead type %d, %.1f kg, radius %.1f m", c.name, c.w.id,
            c.how, c.warhead_type, c.mass, c.radius);
    }
    Chosen r;
    if (choose_rocket(e, &r)) LOG("wprobe: the rocket key uses \"%s\" id %d (%s)", r.name, r.w.id, r.how);
    if (take_snapshot()) {
        Candidate cand[kMaxPlanes];
        const size_t m = candidates(cand);
        const int wing = pick_wingman(cand, m, g_snap.player);
        const int near_ai = nearest_ai(cand, m, cand[g_snap.player].pos, g_snap.player);
        char a[32] = "none", b[32] = "none";
        if (wing >= 0) narrow(g_snap.p[wing].name, a, sizeof(a));
        if (near_ai >= 0) narrow(g_snap.p[near_ai].name, b, sizeof(b));
        LOG("wprobe: %zu aircraft; wingman \"%s\" %.0f m, nearest AI \"%s\" %.0f m", m, a,
            wing >= 0 ? distance(cand[wing].pos, cand[g_snap.player].pos) : 0.0, b,
            near_ai >= 0 ? distance(cand[near_ai].pos, cand[g_snap.player].pos) : 0.0);
    }
    AcquireSRWLockShared(&g_install_lock);
    for (const ModHook& h : g_hooks) LOG("wprobe gun: hook %s: %s", h.what, h.base ? "on" : "off");
    ReleaseSRWLockShared(&g_install_lock);
    LOG("wprobe gun: player %p id 0x%08x", g_player_obj.load(), g_player_id.load());
    for (const GunStation& g : g_gun) {
        if (g.st) {
            LOG("wprobe gun:   %s station %p input[%d] %.2f selected %d inhibit %d ammo %d burst %d",
                g.kind == GunKind::Internal ? "internal" : "pod", g.st, g.input_index, g.input, g.selected ? 1 : 0,
                g.inhibit ? 1 : 0, g.ammo, g.burst);
        }
    }
}

}  // namespace

void weapons_probe_request(WeaponsProbeAction a) {
    g_request.store(static_cast<int>(a));
    g_wprobe_busy.store(true);
}

void weapons_probe_run_frame() {
    const auto a = static_cast<WeaponsProbeAction>(g_request.exchange(0));
    if (a != WeaponsProbeAction::None) {
        if (a == WeaponsProbeAction::List) {
            do_list();
        } else if (a == WeaponsProbeAction::ForceFire) {
            do_force_fire();
        } else if (g_watch.on) {
            LOG("wprobe: key ignored: the last test (%s) is still being watched", g_watch.what);
        } else if (host_has_clients() || replay_active()) {
            LOG("wprobe: key ignored while a host session or replay runs");
        } else if (a == WeaponsProbeAction::LiveAtPlayer || a == WeaponsProbeAction::RemoteAtAi ||
                   a == WeaponsProbeAction::RemoteAtPlayer) {
            do_launch(a);
        } else if (a == WeaponsProbeAction::Rocket) {
            do_rocket();
        } else {
            do_damage(a);
        }
    }
    if (g_watch.on) watch_frame();
    if (!g_watch.on && g_request.load() == 0) g_wprobe_busy.store(false);
    if (g_request.load() != 0) g_wprobe_busy.store(true);  // a key pressed while we were clearing
}

void weapons_probe_start() {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    if (sim_register_step_callback(on_step)) {
        LOG("wprobe: on (Ctrl+Shift+F/G/V launch credited to the wingman, Q/E/U damage the nearest AI, N rocket, B force "
            "fire, L lists; gun log while a key is held or the player fires)");
    } else {
        LOG("wprobe: no free step callback slot; the gun log is off");
    }
}

void weapons_probe_on_modules_loaded() {
    AcquireSRWLockExclusive(&g_install_lock);
    for (ModHook& h : g_hooks) ensure_hook(h);
    const HMODULE ao = g_hooks[0].base;
    if (reinterpret_cast<uintptr_t>(ao) != g_ao_base.load()) {
        g_ao_base.store(reinterpret_cast<uintptr_t>(ao));
        g_player_obj.store(nullptr);  // the game thread clears the stations when it sees the next player
    }
    ReleaseSRWLockExclusive(&g_install_lock);
}

}  // namespace LazyWorks::SF2Patch::hooks
