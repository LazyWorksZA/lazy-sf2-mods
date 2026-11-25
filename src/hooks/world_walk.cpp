// World walker: see world_walk.h. Reads are SEH-guarded and happen on the game thread only.
#include "hooks/world_walk.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "hooks/aircraft_side.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/world_walk_logic.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

using acmi::TacviewRec;

struct Build {
    const wchar_t* module;
    const char* name;
    DWORD stamp, size;
};
// See lazy-sf2-recon: recon-notes §20 (builds).
constexpr Build kObjectManager{L"ObjectManager.dll", "ObjectManager.dll", 0x51d21fa8, 0x28000};
constexpr Build kGroundObject{L"GroundObject.dll", "GroundObject.dll", 0x51d220a7, 0x54000};
constexpr Build kGroundMobile{L"GroundMobileObject.dll", "GroundMobileObject.dll", 0x51d220a2, 0x47000};
constexpr Build kNaval{L"NavalObject.dll", "NavalObject.dll", 0x51d220cd, 0x4a000};
constexpr Build kCounterMeasure{L"CounterMeasureObject.dll", "CounterMeasureObject.dll", 0x51d2208a, 0x25000};
constexpr Build kBullet{L"BulletObject.dll", "BulletObject.dll", 0x51d22099, 0x32000};

// ObjectManager: the manager object, its group table (0x54 bytes a group) and the primary vtable that proves it.
constexpr DWORD kManagerRva = 0x234d0, kManagerVtableRva = 0x1d55c;
constexpr size_t kMgrGroupCount = 0x14, kMgrGroups = 0x18;
constexpr size_t kGroupStride = 0x54, kGrpCount = 0x30, kGrpCapacity = 0x34, kGrpSlots = 0x3c, kGrpStates = 0x50;
constexpr uint32_t kMaxGroups = 64, kMaxSlots = 65536;
constexpr uint8_t kSlotInUse = 1;

// Primary vtables (RVA in the class DLL) and the counter-measure kind table.
constexpr DWORD kGroundVtable = 0x3d8dc, kMobileVtable = 0x347a4, kNavalVtable = 0x375f4, kCmVtable = 0x1937c,
                kBulletVtable = 0x211fc;
constexpr DWORD kCmKindTableRva = 0x1e4a0, kCmKindStride = 0x132c;

// Every object: active flag and status flags.
constexpr size_t kOffActive = 0x04, kOffFlags = 0x0c;
// Ground, vehicle, ship.
constexpr size_t kOffPos = 0x38, kOffAngles = 0x100, kOffTypeRec = 0x360, kOffDamage = 0x16c;
constexpr size_t kTypeRecName = 0x04;
// Counter-measure.
constexpr size_t kOffCmPos = 0x20, kOffCmKind = 0x70, kOffCmLife = 0x78;
// Bullet: tracer flag (int), shooter object id, position, velocity.
constexpr size_t kOffBulletTracer = 0x10, kOffBulletShooter = 0x1c, kOffBulletPos = 0x38, kOffBulletVel = 0x50;

constexpr size_t kMaxGround = 4096, kMaxCm = 256, kMaxMarkers = 256, kMaxBullets = 128;

// Rates, in game seconds between walks: static 1 Hz, mobile 5 Hz, counter-measures 10 Hz, tracers 20 Hz.
enum Due : int { kDueStatic, kDueMobile, kDueCm, kDueBullet, kDueCount };
constexpr double kInterval[kDueCount] = {1.0, 0.2, 0.1, 0.05};
constexpr uint8_t kDueSeries[kDueCount] = {TacviewRec::kSeriesGroundStatic, TacviewRec::kSeriesGroundMobile,
                                           TacviewRec::kSeriesCounterMeasure, TacviewRec::kSeriesBullet};

struct Modules {
    uintptr_t manager = 0;  // ObjectManager base
    uintptr_t ground = 0, mobile = 0, naval = 0, cm = 0, bullet = 0;
};

struct Stats {
    uint32_t peak[6] = {};  // buildings, vehicles, ships, flares, chaff, tracers: most live in one walk
    uint32_t unknown_vtable = 0, ground_capped = 0, cm_capped = 0, markers = 0, markers_dropped = 0, bad_walks = 0;
    uint32_t bullet_capped = 0, bullets_seen = 0;  // tracer sightings over the cap; distinct tracers recorded
    uint32_t walks = 0;
};

struct Walk {
    bool on = false;
    Modules mods;
    uint32_t side_masks[3] = {};
    std::vector<SlotMem> ground, cm, bullet;
    uint64_t next_ground = 1, next_cm = 1, next_bullet = 1;
    double last[kDueCount] = {};
    Stats stats;
    bool logged_first = false, logged_unknown = false, logged_cap = false;
    struct Pending {
        uint64_t seq;
        uint8_t cls;
    } pending[kMaxMarkers];  // Destroyed markers of a frame that was dropped: sent with the next one
    size_t n_pending = 0;
};
Walk g;  // game thread only

uintptr_t resolve(const Build& b) {
    HMODULE m = GetModuleHandleW(b.module);
    if (!m) return 0;
    return is_expected_build(m, b.stamp, b.size, b.name) ? reinterpret_cast<uintptr_t>(m) : 0;
}

bool name_is(const wchar_t* w, const wchar_t* base) {
    size_t k = 0;
    for (; base[k]; ++k)
        if (w[k] != base[k]) return false;
    if (!w[k]) return true;
    static const wchar_t kSuffix[] = L"Group";  // the ini may spell the group with or without it
    for (size_t j = 0; kSuffix[j]; ++j)
        if (w[k + j] != kSuffix[j]) return false;
    return w[k + 5] == 0;
}

struct Groups {
    uintptr_t ground = 0, cm = 0, bullet = 0, aircraft = 0;
};

// Finds the groups by name each walk (a handful of compares). False if the manager is not what we expect.
bool find_groups(const Modules& mods, Groups* out) {
    __try {
        const uintptr_t mgr = mods.manager + kManagerRva;
        if (*reinterpret_cast<const uintptr_t*>(mgr) != mods.manager + kManagerVtableRva) return false;
        const uint32_t n = *reinterpret_cast<const uint32_t*>(mgr + kMgrGroupCount);
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(mgr + kMgrGroups);
        if (!arr || n > kMaxGroups) return false;
        *out = Groups{};
        for (uint32_t i = 0; i < n; ++i) {
            const uintptr_t grp = arr + i * kGroupStride;
            const wchar_t* nm = *reinterpret_cast<const wchar_t* const*>(grp);
            if (!nm) continue;
            if (name_is(nm, L"GroundObject")) out->ground = grp;
            else if (name_is(nm, L"CounterMeasure")) out->cm = grp;
            else if (name_is(nm, L"Bullet")) out->bullet = grp;
            else if (name_is(nm, L"Aircraft")) out->aircraft = grp;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct GroupView {
    const uintptr_t* slots = nullptr;
    const uint8_t* states = nullptr;
    uint32_t count = 0;
};
// Reloaded at every walk: the arrays are freed and re-created between flights.
bool read_group(uintptr_t grp, GroupView* v) {
    __try {
        const uint32_t count = *reinterpret_cast<const uint32_t*>(grp + kGrpCount);
        const uint32_t cap = *reinterpret_cast<const uint32_t*>(grp + kGrpCapacity);
        v->slots = *reinterpret_cast<const uintptr_t* const*>(grp + kGrpSlots);
        v->states = *reinterpret_cast<const uint8_t* const*>(grp + kGrpStates);
        v->count = count;
        return v->slots && v->states && count <= cap && cap <= kMaxSlots;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Head {
    uintptr_t obj = 0, vtable = 0;
    uint32_t flags = 0;
    uint8_t state = 0;
    bool active = false;
};
bool read_head(const GroupView& v, uint32_t i, Head* h) {
    __try {
        *h = Head{};
        h->obj = v.slots[i];
        h->state = v.states[i];
        if (h->obj) {
            const char* o = reinterpret_cast<const char*>(h->obj);
            h->vtable = *reinterpret_cast<const uintptr_t*>(o);
            h->active = *reinterpret_cast<const int*>(o + kOffActive) != 0;
            h->flags = *reinterpret_cast<const uint32_t*>(o + kOffFlags);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int read_damage(uintptr_t obj) {
    __try {
        return *reinterpret_cast<const int*>(obj + kOffDamage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Pose and type name of a ground object into r. False if unreadable.
bool read_ground(uintptr_t obj, TacviewRec* r) {
    __try {
        const char* b = reinterpret_cast<const char*>(obj);
        std::memcpy(r->pos, b + kOffPos, 3 * sizeof(double));
        std::memcpy(r->ang, b + kOffAngles, 3 * sizeof(float));
        r->name[0] = '\0';
        const char* rec = *reinterpret_cast<const char* const*>(b + kOffTypeRec);
        if (rec) {
            const wchar_t* w = reinterpret_cast<const wchar_t*>(rec + kTypeRecName);
            size_t k = 0;
            for (; k < acmi::kRecNameChars && w[k]; ++k)
                r->name[k] = (w[k] >= 0x20 && w[k] < 0x7f) ? static_cast<char>(w[k]) : '?';
            r->name[k] = '\0';
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_cm(uintptr_t obj, double pos[3], float* life, uintptr_t* kind_ptr) {
    __try {
        const char* b = reinterpret_cast<const char*>(obj);
        std::memcpy(pos, b + kOffCmPos, 3 * sizeof(double));
        *life = *reinterpret_cast<const float*>(b + kOffCmLife);
        *kind_ptr = *reinterpret_cast<const uintptr_t*>(b + kOffCmKind);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A tracer round's pose and shooter id. False if unreadable or not a tracer.
bool read_bullet(uintptr_t obj, double pos[3], float vel[3], uint32_t* shooter) {
    __try {
        const char* b = reinterpret_cast<const char*>(obj);
        if (*reinterpret_cast<const int*>(b + kOffBulletTracer) == 0) return false;
        std::memcpy(pos, b + kOffBulletPos, 3 * sizeof(double));
        std::memcpy(vel, b + kOffBulletVel, 3 * sizeof(float));
        *shooter = *reinterpret_cast<const uint32_t*>(b + kOffBulletShooter);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The object a shooter id names: group = id >> 24, slot = id & 0xffffff (a pointer is read, never kept).
bool resolve_shooter(const Modules& mods, uint32_t id, uintptr_t* group, uint32_t* slot, uintptr_t* obj) {
    __try {
        const uintptr_t mgr = mods.manager + kManagerRva;
        const uint32_t ngroups = *reinterpret_cast<const uint32_t*>(mgr + kMgrGroupCount);
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(mgr + kMgrGroups);
        const uint32_t gi = id >> 24;
        *slot = id & 0xffffff;
        if (!arr || gi >= ngroups || gi >= kMaxGroups) return false;
        *group = arr + gi * kGroupStride;
        const uint32_t count = *reinterpret_cast<const uint32_t*>(*group + kGrpCount);
        const uintptr_t* slots = *reinterpret_cast<const uintptr_t* const*>(*group + kGrpSlots);
        if (!slots || *slot >= count || *slot >= kMaxSlots) return false;
        *obj = slots[*slot];
        return *obj != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Our seq for the shooter of a bullet, in the family named by *cls; 0 if it cannot be resolved.
uint64_t shooter_seq(const Groups& gr, uint32_t id, uint8_t* cls) {
    uintptr_t group = 0, obj = 0;
    uint32_t slot = 0;
    if (!resolve_shooter(g.mods, id, &group, &slot, &obj)) return 0;
    if (group == gr.aircraft) {
        *cls = TacviewRec::kAircraft;
        return aircraft_seq_for_object(reinterpret_cast<const void*>(obj));
    }
    if (group == gr.ground && slot < g.ground.size()) {
        const SlotMem& m = g.ground[slot];
        if (m.live && m.ptr == obj) {
            *cls = m.cls;
            return m.seq;
        }
    }
    return 0;
}

void set_nan_pose(TacviewRec& r) { r.pos[0] = r.pos[1] = r.pos[2] = std::nan(""); }

void fill_common(TacviewRec& r, uint8_t cls, uint64_t seq, double t) {
    r = TacviewRec{};
    r.kind = TacviewRec::kObject;
    r.obj_class = cls;
    r.seq = seq;
    r.t = t;
}

// Grows a slot array to n entries before a walk. On failure the walker is off for the rest of the recording.
bool grow(std::vector<SlotMem>& v, size_t n) {
    if (n <= v.size()) return true;
    try {
        v.resize(n);
        return true;
    } catch (const std::bad_alloc&) {
        g.on = false;
        LOG("world: out of memory growing the slot state; ground objects, flares, chaff and tracers stop here");
        return false;
    }
}

// Ground objects, vehicles and ships. Returns the series walked (0 if the group could not be read).
uint8_t walk_ground(uintptr_t grp, uint8_t due_series, double t, TacviewRec* out, size_t cap, size_t& n) {
    GroupView v;
    if (!read_group(grp, &v)) {
        ++g.stats.bad_walks;
        return 0;
    }
    if (!grow(g.ground, v.count)) return 0;  // before the loop: the loop never allocates
    const uint32_t gcount = static_cast<uint32_t>(g.ground.size());
    uint32_t live[3] = {};
    size_t present = 0, markers = 0;
    for (uint32_t i = 0; i < v.count && i < gcount; ++i) {
        SlotMem& m = g.ground[i];
        Head h;
        const bool ok = read_head(v, i, &h);
        uint8_t cls = 0;
        if (ok && h.obj) {
            if (h.vtable == g.mods.ground + kGroundVtable) cls = TacviewRec::kGroundStatic;
            else if (h.vtable == g.mods.mobile + kMobileVtable) cls = TacviewRec::kGroundVehicle;
            else if (h.vtable == g.mods.naval + kNavalVtable) cls = TacviewRec::kShip;
            else if (h.state == kSlotInUse && h.active) ++g.stats.unknown_vtable;
        }
        const uint8_t known = cls ? cls : m.cls;  // a slot that lost its object keeps its last class
        if (!known || !(TacviewRec::series_of(known) & due_series)) continue;
        const bool is_live = ok && cls && h.state == kSlotInUse && h.active;
        const int damage = (!is_live && m.live && h.obj == m.ptr) ? read_damage(h.obj) : -1;
        const GroundStep s = ground_step(m, ok ? h.obj : 0, is_live, damage, g.next_ground);
        if (s.destroyed) {
            if (markers < kMaxMarkers && n < cap) {
                fill_common(out[n], m.cls, s.dead_seq, t);
                out[n++].destroyed = acmi::TacviewRec::kDestroyed;
                ++markers;
                ++g.stats.markers;
            } else {
                ++g.stats.markers_dropped;
            }
        }
        if (!s.present) continue;
        if (present >= kMaxGround || n >= cap) {  // over the cap: not recorded, and retried at the next walk
            m.live = false;
            ++g.stats.ground_capped;
            continue;
        }
        m.cls = cls;
        TacviewRec& r = out[n];
        fill_common(r, cls, m.seq, t);
        r.side = static_cast<uint8_t>(classify_side(h.flags, g.side_masks[0], g.side_masks[1], g.side_masks[2]));
        if (!read_ground(h.obj, &r)) set_nan_pose(r);
        ++n;
        ++present;
        ++live[cls - TacviewRec::kGroundStatic];
    }
    for (int k = 0; k < 3; ++k) {
        const uint8_t series = k == 0 ? TacviewRec::kSeriesGroundStatic : TacviewRec::kSeriesGroundMobile;
        if ((due_series & series) && live[k] > g.stats.peak[k]) g.stats.peak[k] = live[k];
    }
    return due_series & (TacviewRec::kSeriesGroundStatic | TacviewRec::kSeriesGroundMobile);
}

// Flares and chaff (pooled: slots are reused in place).
uint8_t walk_counter_measures(uintptr_t grp, double t, TacviewRec* out, size_t cap, size_t& n) {
    GroupView v;
    if (!read_group(grp, &v)) {
        ++g.stats.bad_walks;
        return 0;
    }
    if (!grow(g.cm, v.count)) return 0;
    const uint32_t gcount = static_cast<uint32_t>(g.cm.size());
    uint32_t live[2] = {};
    size_t present = 0;
    const uintptr_t kinds = g.mods.cm + kCmKindTableRva;
    for (uint32_t i = 0; i < v.count && i < gcount; ++i) {
        SlotMem& m = g.cm[i];
        Head h;
        const bool ok = read_head(v, i, &h);
        bool is_live = ok && h.obj && h.state == kSlotInUse && h.active && h.vtable == g.mods.cm + kCmVtable;
        double pos[3] = {};
        float life = 0;
        uintptr_t kind_ptr = 0;
        uint8_t cls = 0;
        if (is_live) {
            if (!read_cm(h.obj, pos, &life, &kind_ptr) || kind_ptr < kinds) {
                is_live = false;
            } else {
                const uintptr_t kind = (kind_ptr - kinds) / kCmKindStride;  // 0 flare, 1 chaff
                if (kind > 1) is_live = false;
                else cls = kind == 0 ? TacviewRec::kFlare : TacviewRec::kChaff;
            }
        }
        if (is_live && (present >= kMaxCm || n >= cap)) {
            is_live = false;
            ++g.stats.cm_capped;
        }
        const CounterMeasureStep s = counter_measure_step(m, is_live, life, g.next_cm);
        if (!s.present) continue;
        fill_common(out[n], cls, m.seq, t);
        std::memcpy(out[n].pos, pos, sizeof(pos));
        ++n;
        ++present;
        ++live[cls - TacviewRec::kFlare];
    }
    for (int k = 0; k < 2; ++k)
        if (live[k] > g.stats.peak[3 + k]) g.stats.peak[3 + k] = live[k];
    return TacviewRec::kSeriesCounterMeasure;
}

// Tracer rounds, at most kMaxBullets at a time.
uint8_t walk_bullets(uintptr_t grp, const Groups& gr, double t, TacviewRec* out, size_t cap, size_t& n) {
    GroupView v;
    if (!read_group(grp, &v)) {
        ++g.stats.bad_walks;
        return 0;
    }
    if (!grow(g.bullet, v.count)) return 0;
    const uint32_t gcount = static_cast<uint32_t>(g.bullet.size());
    size_t present = 0;
    for (uint32_t i = 0; i < v.count && i < gcount; ++i) {
        SlotMem& m = g.bullet[i];
        Head h;
        const bool ok = read_head(v, i, &h);
        bool is_live = ok && h.obj && h.state == kSlotInUse && h.active && h.vtable == g.mods.bullet + kBulletVtable;
        double pos[3] = {};
        float vel[3] = {};
        uint32_t shooter = 0;
        if (is_live && !read_bullet(h.obj, pos, vel, &shooter)) is_live = false;  // not a tracer, or unreadable
        if (is_live && (present >= kMaxBullets || n >= cap)) {  // over the cap: skipped, counted
            is_live = false;
            ++g.stats.bullet_capped;
        }
        const BulletStep s = bullet_step(m, is_live, pos, vel, t, g.next_bullet);
        if (!s.present) continue;
        TacviewRec& r = out[n];
        fill_common(r, TacviewRec::kBullet, m.seq, t);
        std::memcpy(r.pos, pos, sizeof(pos));
        angles_from_velocity(vel, r.ang);
        uint8_t pcls = TacviewRec::kAircraft;
        r.parent_seq = shooter_seq(gr, shooter, &pcls);
        r.parent_class = pcls;
        if (s.is_new) ++g.stats.bullets_seen;
        ++n;
        ++present;
    }
    if (present > g.stats.peak[5]) g.stats.peak[5] = static_cast<uint32_t>(present);
    return TacviewRec::kSeriesBullet;
}

}  // namespace

bool world_walk_start() {
    g.on = false;
    g.mods = Modules{};
    g.stats = Stats{};
    g.logged_first = g.logged_unknown = g.logged_cap = false;
    g.next_ground = g.next_cm = g.next_bullet = 1;
    g.n_pending = 0;
    for (double& l : g.last) l = -1e300;
    uint32_t f = 0, e = 0, nt = 0;
    aircraft_side_masks(&f, &e, &nt);
    g.side_masks[0] = f;
    g.side_masks[1] = e;
    g.side_masks[2] = nt;
    g.mods.manager = resolve(kObjectManager);
    g.mods.ground = resolve(kGroundObject);
    g.mods.mobile = resolve(kGroundMobile);
    g.mods.naval = resolve(kNaval);
    g.mods.cm = resolve(kCounterMeasure);
    g.mods.bullet = resolve(kBullet);
    if (!g.mods.manager) {
        LOG("world: ObjectManager.dll is missing or not the expected build; ground objects, flares and chaff are not recorded");
        return false;
    }
    try {
        g.ground.assign(kMaxGround, SlotMem{});  // grows later if a flight has more slots
        g.cm.assign(kMaxCm, SlotMem{});
        g.bullet.assign(1024, SlotMem{});
    } catch (const std::bad_alloc&) {
        LOG("world: out of memory for the slot state; ground objects, flares and chaff are not recorded");
        return false;
    }
    g.on = true;
    LOG("world: walker on (ground %s, vehicles %s, ships %s, flares and chaff %s, tracers %s)",
        g.mods.ground ? "yes" : "no", g.mods.mobile ? "yes" : "no", g.mods.naval ? "yes" : "no",
        g.mods.cm ? "yes" : "no", g.mods.bullet ? "yes" : "no");
    return true;
}

size_t world_walk_run(double t, TacviewRec* out, size_t cap, uint8_t* series_mask) {
    *series_mask = 0;
    if (!g.on) return 0;
    uint8_t due = 0;
    for (int k = 0; k < kDueCount; ++k)
        if (!(t >= g.last[k] && t - g.last[k] < kInterval[k] - 1e-9)) due |= kDueSeries[k];
    if (!due) return 0;
    Groups gr;
    if (!find_groups(g.mods, &gr)) {
        ++g.stats.bad_walks;
        return 0;
    }
    size_t n = 0;
    uint8_t mask = 0;
    const uint8_t ground_due = due & (TacviewRec::kSeriesGroundStatic | TacviewRec::kSeriesGroundMobile);
    if (ground_due && gr.ground && (g.mods.ground || g.mods.mobile || g.mods.naval))
        mask |= walk_ground(gr.ground, ground_due, t, out, cap, n);
    if ((due & TacviewRec::kSeriesCounterMeasure) && gr.cm && g.mods.cm)
        mask |= walk_counter_measures(gr.cm, t, out, cap, n);
    if ((due & TacviewRec::kSeriesBullet) && gr.bullet && g.mods.bullet)
        mask |= walk_bullets(gr.bullet, gr, t, out, cap, n);
    if ((mask & (TacviewRec::kSeriesGroundStatic | TacviewRec::kSeriesGroundMobile)) && g.n_pending) {
        size_t sent = 0;
        for (; sent < g.n_pending && n < cap; ++sent) {
            fill_common(out[n], g.pending[sent].cls, g.pending[sent].seq, t);
            out[n++].destroyed = acmi::TacviewRec::kDestroyed;
        }
        g.n_pending -= sent;  // a rare overflow of cap stays queued
        for (size_t k = 0; k < g.n_pending; ++k) g.pending[k] = g.pending[sent + k];
    }
    for (int k = 0; k < kDueCount; ++k)
        if (mask & kDueSeries[k]) g.last[k] = t;
    *series_mask = mask;
    ++g.stats.walks;
    // Outside the loops: one line each.
    if (!g.logged_first) {
        g.logged_first = true;
        LOG("world: first walk: GroundObject group %s, CounterMeasure group %s, Bullet group %s, Aircraft group %s",
            gr.ground ? "found" : "NOT found", gr.cm ? "found" : "NOT found", gr.bullet ? "found" : "NOT found",
            gr.aircraft ? "found" : "NOT found");
    }
    if (g.stats.unknown_vtable && !g.logged_unknown) {
        g.logged_unknown = true;
        LOG("world: a live object in the ground group has an unknown class (skipped)");
    }
    if ((g.stats.ground_capped || g.stats.cm_capped || g.stats.bullet_capped) && !g.logged_cap) {
        g.logged_cap = true;
        LOG("world: record cap reached (ground %zu, counter-measures %zu, tracers %zu); more objects are skipped",
            kMaxGround, kMaxCm, kMaxBullets);
    }
    return n;
}

namespace {

constexpr uint32_t kSummaryMaxObjects = 8192;

// Header of the manager: group count and table. False if the vtable is not the expected one.
bool read_manager(uintptr_t base, uint32_t* n, uintptr_t* arr) {
    __try {
        const uintptr_t mgr = base + kManagerRva;
        if (*reinterpret_cast<const uintptr_t*>(mgr) != base + kManagerVtableRva) return false;
        *n = *reinterpret_cast<const uint32_t*>(mgr + kMgrGroupCount);
        *arr = *reinterpret_cast<const uintptr_t*>(mgr + kMgrGroups);
        return *arr != 0 && *n <= kMaxGroups;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct GroupHead {
    char name[48];
    uint32_t count, capacity;
};
bool read_group_head(uintptr_t grp, GroupHead* h) {
    __try {
        const wchar_t* w = *reinterpret_cast<const wchar_t* const*>(grp);
        size_t k = 0;
        if (w)
            for (; k + 1 < sizeof(h->name) && w[k]; ++k) h->name[k] = (w[k] >= 0x20 && w[k] < 0x7f) ? static_cast<char>(w[k]) : '?';
        h->name[k] = '\0';
        h->count = *reinterpret_cast<const uint32_t*>(grp + kGrpCount);
        h->capacity = *reinterpret_cast<const uint32_t*>(grp + kGrpCapacity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void world_walk_summary(std::string& out) {
    char line[256];
    Modules mods;
    mods.manager = resolve(kObjectManager);
    mods.ground = resolve(kGroundObject);
    mods.mobile = resolve(kGroundMobile);
    mods.naval = resolve(kNaval);
    if (!mods.manager) {
        out += "objects: ObjectManager.dll is missing or not the expected build\n";
        return;
    }
    uint32_t ngroups = 0;
    uintptr_t arr = 0;
    if (!read_manager(mods.manager, &ngroups, &arr)) {
        out += "objects: the object manager is not as expected\n";
        return;
    }
    sprintf_s(line, "objects: %u groups\n", ngroups);
    out += line;
    uint32_t listed = 0;
    for (uint32_t gi = 0; gi < ngroups; ++gi) {
        const uintptr_t grp = arr + gi * kGroupStride;
        GroupHead head;
        GroupView v;
        if (!read_group_head(grp, &head) || !read_group(grp, &v)) {
            sprintf_s(line, "group %u: unreadable\n", gi);
            out += line;
            continue;
        }
        uint32_t in_use = 0, active = 0;
        std::string objects;
        for (uint32_t i = 0; i < v.count; ++i) {
            Head h;
            if (!read_head(v, i, &h) || !h.obj || h.state != kSlotInUse) continue;
            ++in_use;
            if (!h.active) continue;
            ++active;
            if (listed >= kSummaryMaxObjects) continue;
            ++listed;
            const char* cls = "?";
            bool ground = true;
            if (h.vtable == mods.ground + kGroundVtable) cls = "static";
            else if (h.vtable == mods.mobile + kMobileVtable) cls = "vehicle";
            else if (h.vtable == mods.naval + kNavalVtable) cls = "ship";
            else ground = false;
            TacviewRec r;
            if (ground && read_ground(h.obj, &r)) {
                sprintf_s(line, "  obj %u %u %s vtable=%08x pos=%.1f,%.1f,%.1f name=%s\n", gi, i, cls,
                          static_cast<unsigned>(h.vtable), r.pos[0], r.pos[1], r.pos[2], r.name);
            } else {
                sprintf_s(line, "  obj %u %u %s vtable=%08x\n", gi, i, cls, static_cast<unsigned>(h.vtable));
            }
            objects += line;
        }
        sprintf_s(line, "group %u %s count=%u capacity=%u in_use=%u active=%u\n", gi, head.name, head.count,
                  head.capacity, in_use, active);
        out += line;
        out += objects;
    }
    if (listed >= kSummaryMaxObjects) out += "objects: list cut at the cap\n";
}

void world_walk_frame_dropped(const TacviewRec* recs, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (!recs[i].destroyed) continue;
        if (g.n_pending < kMaxMarkers) g.pending[g.n_pending++] = {recs[i].seq, recs[i].obj_class};
        else ++g.stats.markers_dropped;
    }
}

void world_walk_stop() {
    if (!g.on) return;
    g.on = false;
    LOG("world: stopped: peak live %u buildings, %u vehicles, %u ships, %u flares, %u chaff, %u tracers (%u recorded "
        "in all); %u destroyed events (%u dropped); %u unknown-class sightings, %u capped ground, %u capped "
        "counter-measures, %u capped tracers, %u failed walks",
        g.stats.peak[0], g.stats.peak[1], g.stats.peak[2], g.stats.peak[3], g.stats.peak[4], g.stats.peak[5],
        g.stats.bullets_seen, g.stats.markers, g.stats.markers_dropped, g.stats.unknown_vtable, g.stats.ground_capped,
        g.stats.cm_capped, g.stats.bullet_capped, g.stats.bad_walks);
}

}  // namespace LazyWorks::SF2Patch::hooks
