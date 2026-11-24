// Missile spawn test (developer tool, part of the object probe; see probe_hooks.cpp). Inert unless
// the probe is on: the MissileObject constructor hook is only installed then, and the hotkey
// (Ctrl+Shift+M) lives in the probe's hotkey thread, which only exists then.
//
// The constructor hook learns which weapon data ids the game uses. On the hotkey, the next
// Present (the game's main thread) asks the game's object manager to create one missile of the
// most recently seen id, 200 m ahead of the player, and then follows it for up to 20 s, to see
// whether it flies on its own. Nothing here destroys the missile.
//
// Calls into the game are __thiscall; the function pointers use the repo's __fastcall(self, edx,
// ...) pattern. The callee cleans the stack arguments, as __fastcall does for the ones on the stack.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {

// 0 idle, 1 spawn requested (hotkey), 2 following a spawned missile.
std::atomic<int> g_missile_state{0};

namespace {

enum State { kIdle = 0, kRequested = 1, kFollowing = 2 };

constexpr DWORD kAircraftTimestamp = 0x51d220bd, kAircraftImageSize = 0xc2000;
constexpr DWORD kMissileTimestamp = 0x51d220d1, kMissileImageSize = 0x46000;
constexpr DWORD kMissileCtorRva = 0x4fb0;  // void* __thiscall ctor(this, const uint32_t* desc)  (ret 4)
constexpr DWORD kObjServiceRva = 0xb7b88;  // AircraftObject.dll global: the OBJ interface pointer
constexpr DWORD kClassIdRva = 0xa7700;     // AircraftObject.dll global: MissileObject class id (int)
constexpr DWORD kGroupIdRva = 0xa7704;     // AircraftObject.dll global: MissileGroup group id (int)
constexpr size_t kCreateSlot = 0x30 / 4, kByIdSlot = 0x3c / 4;  // OBJ vtable slots
constexpr size_t kDescSize = 0xc8;
constexpr size_t kPosOffset = 0x38, kVelOffset = 0x50, kHeadingOffset = 0x104;  // aircraft
constexpr size_t kMissilePosOffset = 0x20;                                      // missile object
constexpr double kFollowSeconds = 20.0;
constexpr size_t kMaxIds = 16;

using MissileCtor_t = void*(__fastcall*)(void* self, void* edx, const uint32_t* desc);
using CreateObject_t = uint32_t(__fastcall*)(void* self, void* edx, int class_id, int group_id, void* desc);
using ObjectById_t = void*(__fastcall*)(void* self, void* edx, uint32_t id);
MissileCtor_t o_MissileCtor = nullptr;

// Weapon ids seen by the constructor hook (any thread), most recent in g_last.
struct SeenId {
    uint32_t id;
    unsigned count;
};
SRWLOCK g_ids_lock = SRWLOCK_INIT;
SeenId g_ids[kMaxIds];      // guarded by g_ids_lock
size_t g_id_count = 0;      // guarded by g_ids_lock
bool g_have_last = false;   // guarded by g_ids_lock
uint32_t g_last = 0;        // guarded by g_ids_lock
std::atomic<bool> g_hook_tried{false};

void* __fastcall h_MissileCtor(void* self, void* edx, const uint32_t* desc) {
    void* r = o_MissileCtor(self, edx, desc);
    if (!desc) return r;
    const uint32_t id = desc[0];
    bool is_new = false;
    AcquireSRWLockExclusive(&g_ids_lock);
    size_t slot = g_id_count;
    for (size_t i = 0; i < g_id_count; ++i) {
        if (g_ids[i].id == id) {
            slot = i;
            break;
        }
    }
    if (slot == g_id_count) {
        is_new = true;
        if (g_id_count < kMaxIds) {
            ++g_id_count;
        } else {  // full: drop the oldest
            memmove(&g_ids[0], &g_ids[1], (kMaxIds - 1) * sizeof(SeenId));
            slot = kMaxIds - 1;
        }
        g_ids[slot] = {id, 0};
    }
    ++g_ids[slot].count;
    g_last = id;
    g_have_last = true;
    ReleaseSRWLockExclusive(&g_ids_lock);
    if (is_new) LOG("spawn: saw missile weapon id %u (this %p)", id, self);
    return r;
}

struct Vec3d {
    double x, y, z;
};

// Present thread only.
struct Follow {
    uint32_t id = 0;
    void* service = nullptr;
    LARGE_INTEGER start{};
    double next_log = 1.0;
    Vec3d last_pos{};
    double last_t = 0;
};
Follow g_follow;
LARGE_INTEGER g_freq{};

struct PlayerCtx {
    bool found = false;
    Vec3d pos{};
    float vel[3] = {};
    float heading = 0;
};

// Runs under the aircraft table's shared lock: reads only, no logging.
void player_cb(const LiveAircraft* list, size_t n, void* vctx) {
    auto& c = *static_cast<PlayerCtx*>(vctx);
    if (n == 0) return;
    size_t player = 0;
    for (size_t i = 1; i < n; ++i) {
        if (list[i].created < list[player].created) player = i;
    }
    const char* o = static_cast<const char*>(list[player].obj);
    memcpy(&c.pos, o + kPosOffset, sizeof(c.pos));
    memcpy(c.vel, o + kVelOffset, sizeof(c.vel));
    memcpy(&c.heading, o + kHeadingOffset, sizeof(c.heading));
    c.found = true;
}

// Reads an interface function pointer from vtable slot, or null.
void* vslot(void* obj, size_t slot) {
    void** vt = *static_cast<void***>(obj);
    return vt ? vt[slot] : nullptr;
}

double seconds_since(const LARGE_INTEGER& start) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

void do_spawn() {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    PlayerCtx pc;
    probe_with_aircraft(player_cb, &pc);
    if (!pc.found) {
        LOG("spawn: no player aircraft tracked; not spawning");
        g_missile_state.store(kIdle);
        return;
    }
    uint32_t weapon = 0;
    bool have = false;
    AcquireSRWLockShared(&g_ids_lock);
    have = g_have_last;
    weapon = g_last;
    ReleaseSRWLockShared(&g_ids_lock);
    if (!have) {
        LOG("spawn: no missile weapon id seen yet");
        g_missile_state.store(kIdle);
        return;
    }
    const HMODULE am = GetModuleHandleW(L"AircraftObject.dll");
    if (!am || !is_expected_build(am, kAircraftTimestamp, kAircraftImageSize, "AircraftObject.dll")) {
        LOG("spawn: AircraftObject.dll missing or unexpected build; not spawning");
        g_missile_state.store(kIdle);
        return;
    }
    void* service = nullptr;
    int class_id = 0, group_id = 0;
    memcpy(&service, static_cast<const char*>(at_rva(am, kObjServiceRva)), sizeof(service));
    memcpy(&class_id, at_rva(am, kClassIdRva), sizeof(class_id));
    memcpy(&group_id, at_rva(am, kGroupIdRva), sizeof(group_id));
    if (!service || class_id < 0 || group_id < 0) {  // ids are indices: 0 is valid, -1 means unset
        LOG("spawn: object service %p, class %d, group %d not ready; not spawning", service, class_id, group_id);
        g_missile_state.store(kIdle);
        return;
    }
    auto create = reinterpret_cast<CreateObject_t>(vslot(service, kCreateSlot));
    auto by_id = reinterpret_cast<ObjectById_t>(vslot(service, kByIdSlot));
    if (!create || !by_id) {
        LOG("spawn: object service %p has no CreateObject/ObjectById; not spawning", service);
        g_missile_state.store(kIdle);
        return;
    }

    const double h = pc.heading, sh = std::sin(h), ch = std::cos(h);
    const double speed =
        std::sqrt(double(pc.vel[0]) * pc.vel[0] + double(pc.vel[1]) * pc.vel[1] + double(pc.vel[2]) * pc.vel[2]);
    const double mspeed = speed + 50.0;
    const Vec3d pos{pc.pos.x + 200.0 * sh, pc.pos.y + 200.0 * ch, pc.pos.z + 20.0};
    const float orient[3] = {0.f, pc.heading, 0.f};
    const float vel[3] = {float(mspeed * sh), float(mspeed * ch), 0.f};

    alignas(8) unsigned char desc[kDescSize] = {};
    const int16_t one = 1;
    const int32_t minus1 = -1;
    memcpy(desc + 0x00, &weapon, 4);
    memcpy(desc + 0x04, &one, 2);
    memcpy(desc + 0x08, &minus1, 4);
    memcpy(desc + 0x0c, &minus1, 4);
    memcpy(desc + 0x10, &pos, sizeof(pos));
    memcpy(desc + 0x28, orient, sizeof(orient));
    memcpy(desc + 0x34, vel, sizeof(vel));

    const uint32_t id = create(service, nullptr, class_id, group_id, desc);
    void* obj = id == 0xffffffffu ? nullptr : by_id(service, nullptr, id);
    LOG("spawn: CreateObject(class %d, group %d, weapon %u) -> id 0x%08x obj %p at (%.0f, %.0f, %.0f) speed %.1f",
        class_id, group_id, weapon, id, obj, pos.x, pos.y, pos.z, mspeed);
    if (!obj) {
        LOG("spawn: CreateObject did not give a live object; not following");
        g_missile_state.store(kIdle);
        return;
    }
    g_follow = Follow{};
    g_follow.id = id;
    g_follow.service = service;
    g_follow.last_pos = pos;
    QueryPerformanceCounter(&g_follow.start);
    g_missile_state.store(kFollowing);
}

void follow_frame() {
    const double t = seconds_since(g_follow.start);
    auto by_id = reinterpret_cast<ObjectById_t>(vslot(g_follow.service, kByIdSlot));
    void* obj = by_id ? by_id(g_follow.service, nullptr, g_follow.id) : nullptr;
    if (!obj) {
        LOG("spawn: missile 0x%08x gone after %.1f s", g_follow.id, t);
        g_follow = Follow{};
        g_missile_state.store(kIdle);
        return;
    }
    if (t >= kFollowSeconds) {
        LOG("spawn: still alive after 20 s");
        g_follow = Follow{};
        g_missile_state.store(kIdle);
        return;
    }
    if (t >= g_follow.next_log) {
        Vec3d p;
        memcpy(&p, static_cast<const char*>(obj) + kMissilePosOffset, sizeof(p));
        const double dx = p.x - g_follow.last_pos.x, dy = p.y - g_follow.last_pos.y, dz = p.z - g_follow.last_pos.z;
        LOG("spawn: t=%.0fs pos (%.0f, %.0f, %.0f) moved %.1f m in the last second", t, p.x, p.y, p.z,
            std::sqrt(dx * dx + dy * dy + dz * dz));
        g_follow.last_pos = p;
        g_follow.next_log = std::floor(t) + 1.0;
    }
}

}  // namespace

void missile_spawn_request() {
    int s = kIdle;
    g_missile_state.compare_exchange_strong(s, kRequested);  // ignored unless idle
}

void missile_spawn_run_frame() {
    const int s = g_missile_state.load();
    if (s == kRequested) {
        do_spawn();
    } else if (s == kFollowing) {
        follow_frame();
    }
}

void missile_spawn_on_modules_loaded() {
    if (g_hook_tried.load()) return;
    HMODULE m = GetModuleHandleW(L"MissileObject.dll");
    if (!m || g_hook_tried.exchange(true)) return;
    if (!is_expected_build(m, kMissileTimestamp, kMissileImageSize, "MissileObject.dll")) return;
    create_and_enable(at_rva(m, kMissileCtorRva), reinterpret_cast<void*>(&h_MissileCtor),
                      reinterpret_cast<void**>(&o_MissileCtor), "MissileObject ctor");
}

}  // namespace lazysf2::hooks
