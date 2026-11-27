// Shared mission world, tasks T1/T2 (developer tool): saves the generated single mission to a .MSN file in the probe
// folder and runs from it (LAZYSF2_WORLD_SAVE=1), or runs a saved one instead of the generated mission
// (LAZYSF2_WORLD_LOAD=<file>), with the same RND seed and forced start options; after the mission's one creation pass
// it writes a manifest of every object, keyed by mission slot or placed record, for diffing two launches.
// Fixed RVAs, July 2013 build only. See lazy-sf2-recon: network-recon §10.1, §10.3 and §10.10.
#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <MinHook.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hooks/aircraft_side.h"
#include "hooks/hook_util.h"
#include "hooks/game_files.h"
#include "hooks/hooks.h"
#include "hooks/jet_mirror.h"
#include "hooks/replay_math.h"
#include "hooks/world_files.h"
#include "hooks/world_logic.h"
#include "log.h"
#include "net/mission_fetch.h"
#include "net/world_proto.h"
#include "net/world_stream_logic.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

constexpr DWORD kExeStamp = 0x51d22110, kExeSize = 0x62000;
constexpr DWORD kSmStamp = 0x51d220e5, kSmSize = 0xc1000;
constexpr DWORD kSmsStamp = 0x51d220ee, kSmsSize = 0x31000;
constexpr DWORD kOmStamp = 0x51d21fa8, kOmSize = 0x28000;

// Exe: the service registry (recon-notes §18.1) and RND (network-recon §10.1.1).
constexpr DWORD kRvaSys = 0x414b4, kRvaSysVtable = 0x32330;
constexpr size_t kSlotGet = 0x08, kSlotRelease = 0x0c;
constexpr DWORD kRvaRnd = 0x3cf10, kRvaRndVtable = 0x31c6c, kRvaRndSeed = 0x7c50;
constexpr size_t kRndIndex = 0x4004, kRndSeedValue = 0x4008;

// SingleMission: the MSN object and its slots (Load 0, Save 1, Generate 4), the start call, the ini int getter and
// the Ground Object Density global.
constexpr DWORD kRvaMsn = 0xa67c0, kRvaMsnVtable = 0x8b924;
constexpr DWORD kRvaLoad = 0x4a980, kRvaSave = 0x45c60, kRvaGenerate = 0x9230;
constexpr DWORD kRvaStart = 0x31a20, kRvaIniGetInt = 0x14ae0, kRvaDensity = 0xb5d1c;
constexpr DWORD kRvaSlot32 = 0x85e0;  // MSN slot 32: writes the pilot into the player's seat, then runs the start
// The flight list (node +0x00 next, +0x08 record) and the flight record's member count and slot index.
constexpr size_t kOffFlightHead = 0x2c10, kFlightNodeNext = 0x00, kFlightNodeRec = 0x08;
constexpr size_t kFlightMembers = 0xe8, kFlightSlot = 0x82;
// Mission object fields.
// The settings block: +0x04 type, +0x08 aircraft, +0x210 map, +0x418 time (h, m, s), +0x428 date (y, m, d).
constexpr size_t kOffHeader = 0x24d0;
constexpr size_t kHdrType = 0x04, kHdrAircraft = 0x08, kHdrMap = 0x210, kHdrTime = 0x418, kHdrDate = 0x428;
constexpr size_t kOffStartMode = 0x74, kOffPlayerObj = 0x1c, kOffPlayerFlight = 0x2b24, kOffPlayerMember = 0x2b28;
constexpr size_t kOffWeather = 0x2b48, kOffFlightCount = 0x2c18;
constexpr size_t kOffSlotCount = 0x2c88, kOffSlots = 0x2c8c, kOffFirstGroundSlot = 0x2c54, kSlotBytes = 0xe8;
constexpr size_t kSlotFormation = 0x00, kSlotMemberCount = 0x06, kSlotMembers = 0x24;
constexpr int kMaxMembers = 48, kMaxSlots = 4096;
// Target areas and placed records (recon-notes §26).
constexpr size_t kOffTargets = 0x2e30, kOffAreaCount = 0x690, kOffAreaArray = 0x694, kAreaBytes = 0x118;
constexpr size_t kAreaActive = 0x02, kAreaPos = 0x98, kAreaPlacedCount = 0xec, kAreaPlaced = 0xf8;
constexpr size_t kPlacedOffset = 0x2c, kPlacedIndex = 0x54, kPlacedModel = 0x5c, kPlacedId = 0x60, kPlacedObj = 0x64;
constexpr int kMaxAreas = 8192, kMaxPlaced = 4096;

// SingleMissionScreen: the New page's event handler; the settings block it passes to Generate.
constexpr DWORD kRvaNewEvent = 0x4300;
// The Load page's handler (thiscall(page, event), ret 4): on Accept/Fly it builds page +0x220 = the mission folder
// (page +0x18, from MMG slot 8) joined with the chosen name and ".MSN", and calls MSN slot 0 Load with it.
constexpr DWORD kRvaLoadEvent = 0x25e0;
constexpr size_t kLoadPagePath = 0x220;
constexpr size_t kPageSettings = 0x88;
constexpr int kEventAccept = 0x65, kEventFly = 0x67;

// ObjectManager group table (recon-notes §20); objects: flags, position, angles, type records.
constexpr DWORD kManagerRva = 0x234d0, kManagerVtableRva = 0x1d55c;
constexpr size_t kMgrGroupCount = 0x14, kMgrGroups = 0x18, kGroupStride = 0x54, kGrpCount = 0x30, kGrpSlots = 0x3c;
constexpr uint32_t kMaxGroups = 64, kMaxGroupSlots = 65536;
constexpr size_t kObjFlags = 0x0c, kObjPos = 0x38, kObjAngles = 0x100;
constexpr size_t kAircraftData = 0x380, kGroundTypeRec = 0x360, kTypeName = 0x04;

enum class Mode { Off, Save, Load };
// Set in world_install() from the variables, or by a lobby session (world_session_begin/end, menus only).
Mode g_mode = Mode::Off;
std::wstring g_session_dir;       // lobby session: where the world files go when the probe is off
char g_lobby_callsign[16] = {};   // a joined lobby session: this pilot's callsign for the roster
uint32_t g_lobby_hash = 0;        // ... and its content hash (0: no lobby)
std::wstring g_load_path;         // LAZYSF2_WORLD_LOAD, full path
bool g_join = false;              // LAZYSF2_WORLD_JOIN=1 (with LAZYSF2_CONNECT): fetch the host's mission in menus
bool g_seat_from_env = false;     // LAZYSF2_WORLD_SEAT was given: it wins over the host's seat
uint32_t g_tag = 0;               // join: this launch's world tag (the host keys our seat by it)
uint32_t g_fetched_id = 0;        // join: the mission id behind g_load_path
uint32_t g_flying_id = 0;         // join: the mission id of the flight under way (0: not the host's)
int g_host_seat = 0;              // join: the member the host flies
net::MissionFetch* g_fetch = nullptr;  // freed at a lobby session's end only (no join at DLL unload)
std::atomic<bool> g_world_host{false};  // a mission was published: this host streams its world to clients
uint32_t g_published_id = 0;  // the mission this host serves (game thread)
bool g_remote = false;            // LAZYSF2_WORLD_REMOTE=1
int g_seat = -1;                  // LAZYSF2_WORLD_SEAT: the player's member index, or -1 (stock)
bool g_seed_from_env = false;
uint32_t g_env_seed = 0;

// The probe folder (the variables), else the lobby session's folder; empty outside both.
std::wstring world_files_dir() {
    if (const wchar_t* p = probe_output_dir()) return p;
    return g_session_dir;
}

// What an Accept armed for the next start call. Menu and flight start run on the game's main thread, but the start
// hook and the getter read it without assuming that.
struct Armed {
    bool on = false;
    const void* msn = nullptr;
    uint32_t seed = 0;
    int number = 0;              // NNN of the manifest
    WorldSidecar sidecar;        // Save: what was written; Load: what is forced
    std::wstring msn_path, sidecar_path;
};
SRWLOCK g_lock = SRWLOCK_INIT;
Armed g_armed;  // guarded by g_lock

// The ini getter's override, active only on the thread running the armed start call.
std::atomic<DWORD> g_start_thread{0};
int g_force_density = -1, g_force_start = -1;     // Load: forced values (-1 = leave)
int g_seen_density = -1, g_seen_start = -1;       // what the getter returned during the start call

// Hooks, per image.
// The detours return what the originals return: the start returns 1 in eax, and MSN slot 32 (+0x85e0) passes that
// on to FlightEngine, which drops the flight without it. The event handler's eax is passed on for the same reason.
using NewEvent_t = int(__fastcall*)(void* page, void* edx, int ev);
using Start_t = int(__fastcall*)(void* msn);
using IniGetInt_t = int(__fastcall*)(void* ini, void* edx, const wchar_t* section, const wchar_t* key, int def);
NewEvent_t o_NewEvent = nullptr;
Start_t o_Start = nullptr;
IniGetInt_t o_IniGetInt = nullptr;
SRWLOCK g_hook_lock = SRWLOCK_INIT;
HMODULE g_sms = nullptr, g_sms_bad = nullptr, g_sm = nullptr, g_sm_bad = nullptr;  // guarded by g_hook_lock
using Slot32_t = int(__fastcall*)(void* msn);  // returns the start's eax, which FlightEngine checks
Slot32_t o_Slot32 = nullptr;

// Game calls (thiscall, called as fastcall with a dummy edx).
using Load_t = bool(__fastcall*)(void* msn, void* edx, const wchar_t* path);
using Save_t = void(__fastcall*)(void* msn, void* edx, const wchar_t* path);
using Generate_t = void(__fastcall*)(void* msn, void* edx, void* settings);
using Seed_t = void(__fastcall*)(void* rnd, void* edx, int seed);
using Get_t = int(__fastcall*)(void*, void*, const wchar_t*, void**);
using Release_t = void(__fastcall*)(void*, void*, const wchar_t*, void**);

// ---- guarded game access (no C++ objects in these functions: SEH) ----

// The registered MSN object if it is SingleMission's own, with Load/Save/Generate where we expect them. 0 ok,
// 1 none or another module's, 2 unexpected memory.
int get_single_msn(HMODULE exe, HMODULE sm, void** out) {
    *out = nullptr;
    __try {
        void* sys = at_rva(exe, kRvaSys);
        void** vt = *static_cast<void***>(sys);
        if (vt != at_rva(exe, kRvaSysVtable)) return 2;
        void* msn = nullptr;
        if (!reinterpret_cast<Get_t>(vt[kSlotGet / sizeof(void*)])(sys, nullptr, L"MSN", &msn) || !msn) return 1;
        const bool ours = msn == at_rva(sm, kRvaMsn);
        // Release zeroes the pointer it is given, so compare first.
        reinterpret_cast<Release_t>(vt[kSlotRelease / sizeof(void*)])(sys, nullptr, L"MSN", &msn);
        if (!ours) return 1;
        msn = at_rva(sm, kRvaMsn);
        void** mvt = *static_cast<void***>(msn);
        if (mvt != at_rva(sm, kRvaMsnVtable)) return 2;
        if (mvt[0] != at_rva(sm, kRvaLoad) || mvt[1] != at_rva(sm, kRvaSave) || mvt[4] != at_rva(sm, kRvaGenerate))
            return 2;
        *out = at_rva(sm, kRvaMsn);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

// The RND object, if it is what we expect.
void* get_rnd(HMODULE exe) {
    __try {
        void* rnd = at_rva(exe, kRvaRnd);
        void** vt = *static_cast<void***>(rnd);
        if (vt != at_rva(exe, kRvaRndVtable) || vt[0] != at_rva(exe, kRvaRndSeed)) return nullptr;
        return rnd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool call_seed(void* rnd, uint32_t seed) {
    __try {
        reinterpret_cast<Seed_t>((*static_cast<void***>(rnd))[0])(rnd, nullptr, static_cast<int>(seed));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_rnd(void* rnd, int* index, int* seed) {
    __try {
        std::memcpy(index, static_cast<const BYTE*>(rnd) + kRndIndex, sizeof(*index));
        std::memcpy(seed, static_cast<const BYTE*>(rnd) + kRndSeedValue, sizeof(*seed));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 0 ok, 1 the call faulted.
int call_save(void* msn, const wchar_t* path) {
    __try {
        reinterpret_cast<Save_t>((*static_cast<void***>(msn))[1])(msn, nullptr, path);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// 0 loaded, 1 Load returned false, 2 the call faulted.
int call_load(void* msn, const wchar_t* path) {
    __try {
        return reinterpret_cast<Load_t>((*static_cast<void***>(msn))[0])(msn, nullptr, path) ? 0 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

bool call_generate(void* msn, void* settings) {
    __try {
        reinterpret_cast<Generate_t>((*static_cast<void***>(msn))[4])(msn, nullptr, settings);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_density(HMODULE sm, int v) {
    __try {
        *static_cast<int*>(at_rva(sm, kRvaDensity)) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void narrow(const wchar_t* w, char* out, size_t cap) {
    size_t k = 0;
    for (; k + 1 < cap && w[k]; ++k) out[k] = (w[k] >= 0x20 && w[k] < 0x7f) ? static_cast<char>(w[k]) : '?';
    out[k] = '\0';
}

struct HeaderRaw {
    char map[64], aircraft[64];
    int type, time[3], date[3], weather, start_mode, player_flight, player_member, flights;
    uint32_t player_obj;
};
bool read_header(const void* msn, HeaderRaw* h) {
    __try {
        const BYTE* m = static_cast<const BYTE*>(msn);
        const BYTE* hdr = m + kOffHeader;
        narrow(reinterpret_cast<const wchar_t*>(hdr + kHdrMap), h->map, sizeof(h->map));
        narrow(reinterpret_cast<const wchar_t*>(hdr + kHdrAircraft), h->aircraft, sizeof(h->aircraft));
        std::memcpy(&h->type, hdr + kHdrType, sizeof(int));
        std::memcpy(h->time, hdr + kHdrTime, sizeof(h->time));
        std::memcpy(h->date, hdr + kHdrDate, sizeof(h->date));
        std::memcpy(&h->weather, m + kOffWeather, sizeof(int));
        std::memcpy(&h->start_mode, m + kOffStartMode, sizeof(int));
        std::memcpy(&h->player_flight, m + kOffPlayerFlight, sizeof(int));
        std::memcpy(&h->player_member, m + kOffPlayerMember, sizeof(int));
        std::memcpy(&h->flights, m + kOffFlightCount, sizeof(int));
        std::memcpy(&h->player_obj, m + kOffPlayerObj, sizeof(uint32_t));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct SlotTable {
    const BYTE* arr;
    int count;
    int first_ground;
};
bool read_slot_table(const void* msn, SlotTable* t) {
    __try {
        const BYTE* m = static_cast<const BYTE*>(msn);
        std::memcpy(&t->count, m + kOffSlotCount, sizeof(int));
        std::memcpy(&t->arr, m + kOffSlots, sizeof(t->arr));
        int16_t fg = 0;
        std::memcpy(&fg, m + kOffFirstGroundSlot, sizeof(fg));
        t->first_ground = fg;
        return t->count >= 0 && t->count <= kMaxSlots && (t->count == 0 || t->arr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct SlotRaw {
    uint32_t formation;
    int members;
    uint32_t ids[kMaxMembers];
};
bool read_slot(const SlotTable& t, int i, SlotRaw* s) {
    __try {
        const BYTE* r = t.arr + static_cast<size_t>(i) * kSlotBytes;
        std::memcpy(&s->formation, r + kSlotFormation, sizeof(uint32_t));
        int16_t n = 0;
        std::memcpy(&n, r + kSlotMemberCount, sizeof(n));
        s->members = n < 0 ? 0 : (n > kMaxMembers ? kMaxMembers : n);
        std::memcpy(s->ids, r + kSlotMembers, sizeof(uint32_t) * static_cast<size_t>(s->members));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Om {
    uintptr_t mgr = 0, aircraft_grp = 0, ground_grp = 0;
};
bool group_named(const wchar_t* w, const wchar_t* base) {
    size_t k = 0;
    for (; base[k]; ++k)
        if (w[k] != base[k]) return false;
    return !w[k] || std::wcscmp(w + k, L"Group") == 0;
}
bool read_om(HMODULE om, Om* o) {
    __try {
        const uintptr_t mgr = reinterpret_cast<uintptr_t>(at_rva(om, kManagerRva));
        if (*reinterpret_cast<const uintptr_t*>(mgr) != reinterpret_cast<uintptr_t>(at_rva(om, kManagerVtableRva)))
            return false;
        const uint32_t n = *reinterpret_cast<const uint32_t*>(mgr + kMgrGroupCount);
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(mgr + kMgrGroups);
        if (!arr || n > kMaxGroups) return false;
        o->mgr = mgr;
        for (uint32_t i = 0; i < n; ++i) {
            const uintptr_t grp = arr + i * kGroupStride;
            const wchar_t* nm = *reinterpret_cast<const wchar_t* const*>(grp);
            if (!nm) continue;
            if (group_named(nm, L"Aircraft")) o->aircraft_grp = grp;
            else if (group_named(nm, L"GroundObject")) o->ground_grp = grp;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct ObjRaw {
    char type[64];
    uint32_t flags;
    double pos[3];
    float ang[3];
};
// The object an id names (group = id >> 24, slot = id & 0xffffff), read but never kept. False if it names none.
bool read_object(const Om& o, uint32_t id, ObjRaw* r) {
    __try {
        if (id == 0xffffffffu || !o.mgr) return false;
        const uint32_t ngroups = *reinterpret_cast<const uint32_t*>(o.mgr + kMgrGroupCount);
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(o.mgr + kMgrGroups);
        const uint32_t gi = id >> 24, slot = id & 0xffffff;
        if (!arr || gi >= ngroups || gi >= kMaxGroups) return false;
        const uintptr_t grp = arr + gi * kGroupStride;
        const uint32_t count = *reinterpret_cast<const uint32_t*>(grp + kGrpCount);
        const uintptr_t* slots = *reinterpret_cast<const uintptr_t* const*>(grp + kGrpSlots);
        if (!slots || slot >= count || slot >= kMaxGroupSlots) return false;
        const BYTE* obj = reinterpret_cast<const BYTE*>(slots[slot]);
        if (!obj) return false;
        std::memcpy(&r->flags, obj + kObjFlags, sizeof(uint32_t));
        std::memcpy(r->pos, obj + kObjPos, sizeof(r->pos));
        std::memcpy(r->ang, obj + kObjAngles, sizeof(r->ang));
        r->type[0] = '\0';
        const BYTE* rec = nullptr;
        if (grp == o.aircraft_grp) std::memcpy(&rec, obj + kAircraftData, sizeof(rec));
        else if (grp == o.ground_grp) std::memcpy(&rec, obj + kGroundTypeRec, sizeof(rec));
        if (rec) narrow(reinterpret_cast<const wchar_t*>(rec + kTypeName), r->type, sizeof(r->type));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct AreaRaw {
    int16_t active;
    double pos[2];
    int n_placed;
    const BYTE* const* placed;
};
bool read_areas_head(const void* msn, const BYTE** arr, int* count) {
    __try {
        const BYTE* owner = static_cast<const BYTE*>(msn) + kOffTargets;
        std::memcpy(count, owner + kOffAreaCount, sizeof(int));
        std::memcpy(arr, owner + kOffAreaArray, sizeof(*arr));
        return *count >= 0 && *count <= kMaxAreas && (*count == 0 || *arr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool read_area(const BYTE* arr, int i, AreaRaw* a) {
    __try {
        const BYTE* rec = arr + static_cast<size_t>(i) * kAreaBytes;
        std::memcpy(&a->active, rec + kAreaActive, sizeof(a->active));
        std::memcpy(a->pos, rec + kAreaPos, sizeof(a->pos));
        std::memcpy(&a->n_placed, rec + kAreaPlacedCount, sizeof(int));
        std::memcpy(&a->placed, rec + kAreaPlaced, sizeof(a->placed));
        return a->n_placed >= 0 && a->n_placed <= kMaxPlaced && (a->n_placed == 0 || a->placed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct PlacedRaw {
    int index;
    float off[2];
    const void* model;
    uint32_t id;
    const void* obj;
};
bool read_placed(const AreaRaw& a, int j, PlacedRaw* p) {
    __try {
        const BYTE* rec = a.placed[j];
        if (!rec) return false;
        std::memcpy(&p->index, rec + kPlacedIndex, sizeof(int));
        std::memcpy(p->off, rec + kPlacedOffset, sizeof(p->off));
        std::memcpy(&p->model, rec + kPlacedModel, sizeof(p->model));
        std::memcpy(&p->id, rec + kPlacedId, sizeof(uint32_t));
        std::memcpy(&p->obj, rec + kPlacedObj, sizeof(p->obj));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- files ----

bool write_text(const std::wstring& path, const std::string& text, bool create_new) {
    if (path.size() >= MAX_PATH) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, create_new ? CREATE_NEW : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size();
    CloseHandle(f);
    return ok;
}

bool read_text(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < 65536;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out->empty() || (ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) &&
                              got == out->size());
    }
    CloseHandle(f);
    return ok;
}

bool read_bytes(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart <= net::kMissionMaxRaw;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size();
    }
    CloseHandle(f);
    return ok;
}

bool write_bytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    if (path.size() >= MAX_PATH) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size();
    CloseHandle(f);
    return ok;
}

bool file_exists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The next free NNN over the world_* files in the probe folder.
int next_number(const std::wstring& dir) {
    int best = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\world_*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const int n = world_file_number(fd.cFileName);
            if (n > best) best = n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best + 1;
}

std::wstring numbered(const std::wstring& dir, int n, const wchar_t* suffix) {
    wchar_t name[64];
    swprintf_s(name, L"\\world_%03d%ls", n, suffix);
    return dir + name;
}

std::string file_name_of(const std::wstring& path) {
    const size_t s = path.find_last_of(L'\\');
    const std::wstring w = s == std::wstring::npos ? path : path.substr(s + 1);
    std::string out;
    for (wchar_t c : w) out += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
    return out;
}

void fill_sidecar_from_header(const HeaderRaw& h, WorldSidecar* s) {
    s->theatre = h.map;
    s->aircraft = h.aircraft;
    s->mission_type = h.type;
    s->year = h.date[0], s->month = h.date[1], s->day = h.date[2];
    s->hour = h.time[0], s->minute = h.time[1], s->second = h.time[2];
    s->weather = h.weather;
}

uint32_t pick_seed() {
    if (g_seed_from_env) return g_env_seed;
    return (GetTickCount() ^ (GetCurrentProcessId() << 16)) & 0x7fffffff;
}

// ---- the flows ----

struct Ctx {
    HMODULE exe, sm;
    void* msn;
    void* rnd;
};

bool resolve(Ctx* c, const char* when) {
    c->exe = GetModuleHandleW(nullptr);
    c->sm = GetModuleHandleW(L"SingleMission.dll");
    static bool exe_checked = false, exe_ok = false;
    if (!exe_checked) {
        exe_checked = true;
        exe_ok = is_expected_build(c->exe, kExeStamp, kExeSize, "StrikeFighters2.exe");
    }
    if (!exe_ok || !c->sm) {
        LOG("world: %s: refused (%s)", when, !exe_ok ? "unexpected exe build" : "SingleMission.dll not loaded");
        return false;
    }
    if (!is_expected_build(c->sm, kSmStamp, kSmSize, "SingleMission.dll")) {
        LOG("world: %s: refused (unexpected SingleMission.dll build)", when);
        return false;
    }
    const int r = get_single_msn(c->exe, c->sm, &c->msn);
    if (r != 0) {
        LOG("world: %s: refused (%s)", when,
            r == 1 ? "the running mission is not SingleMission's (campaign or instant action?)"
                   : "SingleMission's MSN object or its vtable is not as expected");
        return false;
    }
    c->rnd = get_rnd(c->exe);
    if (!c->rnd) {
        LOG("world: %s: refused (the RND service is not as expected)", when);
        return false;
    }
    return true;
}

// After a failed Load the mission is cleared: generate it again from the page's settings so the player can still fly.
void regenerate(const Ctx& c, void* page) {
    if (!page) {  // the lobby's briefing load has no New page to generate from
        LOG("world: ERR the mission could not be loaded and there is no page to generate one; do not fly it");
        return;
    }
    void* settings = static_cast<BYTE*>(page) + kPageSettings;
    if (call_generate(c.msn, settings)) LOG("world: generated the mission again from the New page's settings");
    else LOG("world: ERR regenerating the mission faulted; do not fly this mission");
}

// The Load page's chosen file as an absolute path (a relative one is under the game folder).
bool read_page_path(const void* page, wchar_t* out, size_t cap) {
    __try {
        wcsncpy_s(out, cap, reinterpret_cast<const wchar_t*>(static_cast<const BYTE*>(page) + kLoadPagePath), _TRUNCATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool load_page_path(const void* page, wchar_t* out, size_t cap) {
    if (!read_page_path(page, out, cap) || !out[0]) return false;
    const bool absolute = (out[1] == L':') || (out[0] == L'\\' && out[1] == L'\\');
    if (absolute) return true;
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = n && n < MAX_PATH ? wcsrchr(exe, L'\\') : nullptr;
    if (!slash) return false;
    slash[1] = L'\0';
    std::wstring full = std::wstring(exe) + (out[0] == L'.' && out[1] == L'\\' ? out + 2 : out);
    if (full.size() >= cap) return false;
    wcsncpy_s(out, cap, full.c_str(), _TRUNCATE);
    return true;
}

void publish_mission(const Armed& a);  // below, with the start

// source: null for the generated mission (written by MSN Save); else the Load page's file, copied byte for byte (loose
// or from a cat, through the exe's file manager) so hand-made keys survive.
void accept_save(const Ctx& c, void* page, const wchar_t* source) {
    const std::wstring dir = world_files_dir();
    if (dir.empty()) {
        LOG("world: save refused (no folder for the world files)");
        return;
    }
    const int n = next_number(dir);
    const std::wstring msn_path = numbered(dir, n, L".msn");
    const std::wstring side_path = world_companion_path(msn_path, L".sidecar.txt");
    if (side_path.size() >= MAX_PATH) {
        LOG("world: save refused (probe folder path too long)");
        return;
    }
    if (source) {
        std::vector<uint8_t> bytes;
        if (!read_game_file(source, bytes) || bytes.empty() || bytes.size() > net::kMissionMaxRaw ||
            !write_bytes(msn_path, bytes)) {
            LOG("world: ERR could not copy the Load page's mission %ls to %ls; flying it unshared", source,
                msn_path.c_str());
            return;
        }
    } else if (call_save(c.msn, msn_path.c_str()) != 0) {
        LOG("world: ERR MSN Save faulted for %ls", msn_path.c_str());
        return;
    }
    if (!file_exists(msn_path)) {
        LOG("world: ERR no file at %ls; mission left as it was", msn_path.c_str());
        return;
    }
    const uint32_t seed = pick_seed();
    if (!call_seed(c.rnd, seed)) {
        LOG("world: ERR RND Seed faulted; mission left as generated");
        return;
    }
    const int lr = call_load(c.msn, msn_path.c_str());
    if (lr != 0) {
        LOG("world: ERR MSN Load of %ls %s", msn_path.c_str(), lr == 1 ? "returned false" : "faulted");
        if (source) {
            if (call_load(c.msn, source) == 0) LOG("world: loaded the Load page's mission again from %ls", source);
            else LOG("world: ERR loading %ls again failed; do not fly this mission", source);
        } else {
            regenerate(c, page);
        }
        return;
    }
    HeaderRaw h{};
    WorldSidecar s;
    s.msn = file_name_of(msn_path);
    s.seed = seed;
    s.has_seed = true;
    if (read_header(c.msn, &h)) fill_sidecar_from_header(h, &s);
    if (!write_text(side_path, format_sidecar(s), false)) LOG("world: ERR could not write %ls", side_path.c_str());
    LOG("world: saved the %s mission to %ls (theatre %s, %s, %04d-%02d-%02d %02d:%02d, weather %d), seed %lu, "
        "reloaded it from the file; sidecar %ls",
        source ? "Load page's" : "generated", msn_path.c_str(), s.theatre.c_str(), s.aircraft.c_str(), s.year, s.month, s.day, s.hour, s.minute,
        s.weather, static_cast<unsigned long>(seed), side_path.c_str());
    AcquireSRWLockExclusive(&g_lock);
    g_armed = Armed{true, c.msn, seed, n, s, msn_path, side_path};
    ReleaseSRWLockExclusive(&g_lock);
    // A lobby session shares the mission now, in the menus, so the clients can read its briefing before the flight: the
    // two values the start reads (density, mission start) come from the same Options.ini it reads them from. The start
    // records them again and publishes the same mission.
    if (net_session_role() == NetRole::Host) {
        Armed early{true, c.msn, seed, n, s, msn_path, side_path};
        if (game_option_int(L"GraphicsOptions", L"GroundObjectDensity", &early.sidecar.density) &&
            game_option_int(L"GameplayOptions", L"MissionStart", &early.sidecar.mission_start) &&
            write_text(side_path, format_sidecar(early.sidecar), false)) {
            publish_mission(early);
            LOG("world: shared at Accept (density %d, mission start %d)", early.sidecar.density, early.sidecar.mission_start);
        } else {
            LOG("world: not shared at Accept (Options.ini density or mission start not readable); shared at the start");
        }
    }
}

// Join, lobby session: the seat the host's book gives this pilot now (it may have changed since the mission arrived;
// the host repeats it in every roster and offer). Menu thread.
void apply_lobby_seat() {
    if (!g_join || g_seat_from_env || !g_fetch || !g_lobby_hash) return;
    net::LobbyRosterMsg r{};
    uint32_t rtt = 0;
    int seat = g_fetch->roster(&r, &rtt) ? r.your_seat : g_fetch->latest_seat();
    if (seat == net::kNoSeat) seat = g_fetch->latest_seat();
    if (seat == net::kNoSeat || seat == g_seat) return;
    LOG("world: join: the lobby seat is member %d (was %d)", seat, g_seat);
    g_seat = seat;
}

// Join: a mission fetched from the host becomes the file to load (written to the probe folder as world_NNN.msn and
// its sidecar, so Load reads it by path as for LAZYSF2_WORLD_LOAD). Menu thread.
void take_fetched_mission() {
    if (!g_fetch) return;
    std::vector<uint8_t> packed, msn;
    net::MissionOfferMsg offer{};
    if (!g_fetch->take(&packed, &offer)) {
        // Nothing new: the file we hold is still good only if it is the host's current mission.
        const uint32_t latest = g_fetch->latest_id();
        if (!g_load_path.empty() && latest != 0 && latest != g_fetched_id) {
            LOG("world: join: the host changed its mission (0x%08lx) and it has not arrived yet; this Accept flies the "
                "generated mission", static_cast<unsigned long>(latest));
            g_load_path.clear();
            g_fetched_id = 0;
        } else if (g_load_path.empty()) {
            LOG("world: join: no mission from the host yet; this Accept flies the generated mission");
        }
        return;
    }
    std::string sidecar;
    const char* why = "";
    if (!net::unpack_mission(packed, offer.mission_id, offer.raw_bytes, &sidecar, &msn, &why)) {
        LOG("world: join: the host's mission 0x%08lx is unusable (%s)", static_cast<unsigned long>(offer.mission_id), why);
        return;
    }
    const std::wstring dir = world_files_dir();
    if (dir.empty()) {
        LOG("world: join: no folder for the world files; the host's mission is not kept");
        return;
    }
    const int n = next_number(dir);
    const std::wstring msn_path = numbered(dir, n, L".msn");
    const std::wstring side_path = world_companion_path(msn_path, L".sidecar.txt");
    if (!write_bytes(msn_path, msn) || !write_text(side_path, sidecar, true)) {
        LOG("world: ERR join: could not write %ls or its sidecar", msn_path.c_str());
        return;
    }
    if (offer.seat == net::kNoSeat && !g_seat_from_env) {
        LOG("world: join: the host has no free seat for us in its flight; flying the generated mission");
        return;
    }
    g_load_path = msn_path;
    g_fetched_id = offer.mission_id;
    g_host_seat = offer.host_seat == net::kNoSeat ? 0 : offer.host_seat;
    if (!g_seat_from_env) g_seat = offer.seat;
    LOG("world: join: the host's mission 0x%08lx is %ls (%zu bytes); we fly member %d (%s), the host member %d",
        static_cast<unsigned long>(offer.mission_id), msn_path.c_str(), msn.size(), g_seat,
        g_seat_from_env ? "LAZYSF2_WORLD_SEAT" : "assigned by the host", g_host_seat);
    apply_lobby_seat();
}

// The New page's settings theatre (+0x210, a wide string of MAX_PATH), read or written. SEH only.
bool page_theatre(void* page, char* out, size_t cap) {
    __try {
        narrow(reinterpret_cast<const wchar_t*>(static_cast<const BYTE*>(page) + kPageSettings + kHdrMap), out, cap);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Whether this PC has the theatre: its ini in the game's or the user's Terrains folder (loose, or in a cat).
bool theatre_installed(const std::string& theatre) {
    if (theatre.empty()) return false;
    const std::wstring t = widen_ansi(theatre);
    std::vector<uint8_t> bytes;
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring dir(exe);
        dir.resize(dir.find_last_of(L'\\'));
        const std::wstring game = dir + L"\\Terrains\\" + t;
        const DWORD a = GetFileAttributesW(game.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return true;
        if (read_game_file(game + L"\\" + t + L".ini", bytes)) return true;  // also finds a stock theatre in a cat
    }
    PWSTR saved = nullptr;
    bool ok = false;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved)) && saved) {
        const std::wstring user = std::wstring(saved) + L"\\ThirdWire\\StrikeFighters2\\Terrains\\" + t;
        const DWORD a = GetFileAttributesW(user.c_str());
        ok = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) || read_game_file(user + L"\\" + t + L".ini", bytes);
    }
    CoTaskMemFree(saved);
    return ok;
}

bool g_join_no_theatre = false;  // join: the host's mission is in a theatre this PC does not have

void accept_load(const Ctx& c, void* page, bool new_page) {
    if (g_join) take_fetched_mission();
    g_join_no_theatre = false;
    if (g_load_path.empty()) return;
    const std::wstring side_path = world_companion_path(g_load_path, L".sidecar.txt");
    std::string text, err;
    WorldSidecar s;
    if (!file_exists(g_load_path)) {
        LOG("world: load refused (no file %ls); flying the generated mission", g_load_path.c_str());
        return;
    }
    if (!read_text(side_path, &text) || !parse_sidecar(text, &s, &err)) {
        LOG("world: load refused (sidecar %ls: %s); flying the generated mission", side_path.c_str(),
            err.empty() ? "unreadable" : err.c_str());
        return;
    }
    if (s.density < 0) {
        LOG("world: load refused (sidecar has no density: the saving launch never started that mission); flying "
            "the generated mission");
        return;
    }
    // The flight's terrain is the theatre selected on the New page (the menu's own choice; neither the page's
    // settings field, a new Generate nor MSN Load changes it: live check 2026-11-27), so a mission in another theatre
    // would fly on the wrong terrain. Refuse it and say what to do.
    char have[64] = {};
    if (new_page && !s.theatre.empty() && page_theatre(page, have, sizeof(have)) && s.theatre != have) {
        const bool installed = theatre_installed(s.theatre);
        if (g_join && !installed) g_join_no_theatre = true;
        LOG("world: load REFUSED: the mission is in theatre %s, the New page is on %s. %s; flying the generated mission%s",
            s.theatre.c_str(), have,
            installed ? "Select that theatre on the New page and Accept again" : "That theatre is not installed on this PC",
            g_join ? (installed ? " (the host's session will refuse it)" : ", and leaving the host's session when it welcomes us") : "");
        return;
    }
    if (!call_seed(c.rnd, s.seed)) {
        LOG("world: ERR RND Seed faulted; flying the generated mission");
        return;
    }
    const int lr = call_load(c.msn, g_load_path.c_str());
    if (lr != 0) {
        LOG("world: ERR MSN Load of %ls %s", g_load_path.c_str(), lr == 1 ? "returned false" : "faulted");
        regenerate(c, page);
        return;
    }
    HeaderRaw h{};
    if (read_header(c.msn, &h) && s.theatre != h.map)
        LOG("world: WARNING the loaded mission's theatre is %s, the sidecar says %s", h.map, s.theatre.c_str());
    const std::wstring dir = world_files_dir();
    const int n = next_number(dir);
    LOG("world: loaded %ls instead of the generated mission (theatre %s, %s, %04d-%02d-%02d %02d:%02d, weather %d), "
        "seed %lu; at the start: density %d, mission start %d",
        g_load_path.c_str(), h.map, h.aircraft, h.date[0], h.date[1], h.date[2], h.time[0], h.time[1], h.weather,
        static_cast<unsigned long>(s.seed), s.density, s.mission_start);
    AcquireSRWLockExclusive(&g_lock);
    g_armed = Armed{true, c.msn, s.seed, n, s, g_load_path, side_path};
    ReleaseSRWLockExclusive(&g_lock);
}

int __fastcall h_NewEvent(void* page, void* edx, int ev) {
    const int r = o_NewEvent(page, edx, ev);
    if (g_mode == Mode::Off || (ev != kEventAccept && ev != kEventFly)) return r;  // Off: a lobby session has ended
    Ctx c{};
    if (!resolve(&c, "New page Accept")) return r;
    if (g_mode == Mode::Save) accept_save(c, page, nullptr);
    else if (g_mode == Mode::Load) accept_load(c, page, true);
    return r;
}

// The Load page: a host shares the mission it chose there (a stock, saved or hand-made .MSN) as it shares a generated
// one; a client in a world join flies the host's mission instead, as on the New page.
NewEvent_t o_LoadEvent = nullptr;
int __fastcall h_LoadEvent(void* page, void* edx, int ev) {
    const int r = o_LoadEvent(page, edx, ev);
    if (g_mode == Mode::Off || (ev != kEventAccept && ev != kEventFly)) return r;
    Ctx c{};
    if (!resolve(&c, "Load page Accept")) return r;
    if (g_mode == Mode::Save) {
        wchar_t src[MAX_PATH] = {};
        if (!load_page_path(page, src, MAX_PATH)) {
            LOG("world: Load page Accept: refused (the chosen file's path is not readable)");
            return r;
        }
        accept_save(c, page, src);
    } else if (g_mode == Mode::Load) {
        accept_load(c, page, false);
    }
    return r;
}

int __fastcall h_IniGetInt(void* ini, void* edx, const wchar_t* section, const wchar_t* key, int def) {
    const DWORD t = g_start_thread.load(std::memory_order_acquire);
    if (t == 0 || t != GetCurrentThreadId() || !section || !key) return o_IniGetInt(ini, edx, section, key, def);
    int* seen = nullptr;
    int force = -1;
    if (std::wcscmp(section, L"GraphicsOptions") == 0 && std::wcscmp(key, L"GroundObjectDensity") == 0) {
        seen = &g_seen_density;
        force = g_force_density;
    } else if (std::wcscmp(section, L"GameplayOptions") == 0 && std::wcscmp(key, L"MissionStart") == 0) {
        seen = &g_seen_start;
        force = g_force_start;
    }
    const int r = force >= 0 ? force : o_IniGetInt(ini, edx, section, key, def);
    if (seen) *seen = r;
    return r;
}

void write_manifest(const Ctx& c, const Armed& a, int rnd_index, int rnd_seed) {
    std::vector<ManifestEntry> v;
    Om om;
    HMODULE omm = GetModuleHandleW(L"ObjectManager.dll");
    if (!omm || !is_expected_build(omm, kOmStamp, kOmSize, "ObjectManager.dll") || !read_om(omm, &om)) {
        LOG("world: ERR ObjectManager not as expected; no manifest");
        return;
    }
    uint32_t mf = 0, me = 0, mn = 0;
    aircraft_side_masks(&mf, &me, &mn);
    auto fill = [&](ManifestEntry& e, uint32_t id) {
        e.has_id = true;
        e.id = id;
        ObjRaw r{};
        if (!read_object(om, id, &r)) {
            e.state = ManifestEntry::Unresolved;
            return;
        }
        e.type = r.type;
        switch (classify_side(r.flags, mf, me, mn)) {
            case AircraftSide::Friendly: e.side = 'F'; break;
            case AircraftSide::Enemy: e.side = 'E'; break;
            case AircraftSide::Neutral: e.side = 'N'; break;
            default: e.side = '?'; break;
        }
        std::memcpy(e.pos, r.pos, sizeof(e.pos));
        e.heading = r.ang[1];
        e.has_pose = true;
    };
    SlotTable t{};
    std::string player_key = "none";
    HeaderRaw h{};
    const bool have_header = read_header(c.msn, &h);
    if (!read_slot_table(c.msn, &t)) {
        LOG("world: ERR the mission's slot table is not as expected; no manifest");
        return;
    }
    int empty_formations = 0;  // a formation with no member count: would hide its members, so it is counted
    for (int i = 0; i < t.count; ++i) {
        SlotRaw s{};
        if (!read_slot(t, i, &s)) {
            LOG("world: ERR slot %d unreadable; no manifest", i);
            return;
        }
        const char kind = i < t.first_ground ? ManifestEntry::Aircraft : ManifestEntry::Ground;
        if (s.formation == 0xffffffffu) {
            ManifestEntry e;
            e.kind = kind, e.a = i, e.b = -1, e.state = ManifestEntry::Absent;
            v.push_back(e);
            continue;
        }
        if (s.members == 0) ++empty_formations;
        for (int m = 0; m < s.members; ++m) {
            ManifestEntry e;
            e.kind = kind, e.a = i, e.b = m;
            fill(e, s.ids[m]);
            if (have_header && s.ids[m] == h.player_obj) {
                char k[32];
                std::snprintf(k, sizeof(k), "%c %03d.%02d", kind, i, m);
                player_key = k;
            }
            v.push_back(e);
        }
    }
    const BYTE* areas = nullptr;
    int n_areas = 0, populated = 0;
    if (!read_areas_head(c.msn, &areas, &n_areas)) {
        LOG("world: ERR the target areas are not as expected; manifest without placed records");
        n_areas = 0;
    }
    for (int i = 0; i < n_areas; ++i) {
        AreaRaw ar{};
        if (!read_area(areas, i, &ar)) {
            LOG("world: ERR area %d unreadable; placed records stop there", i);
            break;
        }
        if (ar.active != 1) continue;
        ++populated;
        for (int j = 0; j < ar.n_placed; ++j) {
            PlacedRaw p{};
            ManifestEntry e;
            e.kind = ManifestEntry::Placed, e.a = i, e.b = j;
            if (!read_placed(ar, j, &p)) {
                e.state = ManifestEntry::Unresolved;
                v.push_back(e);
                continue;
            }
            e.b = p.index;
            if (p.obj && p.id != 0xffffffffu) {
                fill(e, p.id);
            } else {
                e.state = p.model ? ManifestEntry::Model : ManifestEntry::None;
                // The game sums area position and offset in float (SingleMission +0x3a2c0).
                e.pos[0] = static_cast<double>(p.off[0] + static_cast<float>(ar.pos[0]));
                e.pos[1] = static_cast<double>(p.off[1] + static_cast<float>(ar.pos[1]));
                e.has_pose = true;
            }
            v.push_back(e);
        }
    }
    std::vector<std::string> header;
    char b[512];
    SYSTEMTIME st;
    GetLocalTime(&st);
    header.push_back("# Lazy SF2 Patch world manifest (developer tool). Lines starting with # differ per run.");
    std::snprintf(b, sizeof(b), "# mode=%s file=%s written=%04d-%02d-%02d %02d:%02d:%02d",
                  g_mode == Mode::Save ? "save" : "load", file_name_of(a.msn_path).c_str(), st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond);
    header.push_back(b);
    std::snprintf(b, sizeof(b), "H theatre=%s aircraft=%s mission_type=%d date=%04d-%02d-%02d time=%02d:%02d:%02d "
                  "weather=%d", h.map, h.aircraft, h.type, h.date[0], h.date[1], h.date[2], h.time[0], h.time[1],
                  h.time[2], h.weather);
    header.push_back(b);
    std::snprintf(b, sizeof(b), "H seed=%lu density=%d mission_start=%d start_mode=%d",
                  static_cast<unsigned long>(a.seed), g_seen_density, g_seen_start, h.start_mode);
    header.push_back(b);
    std::snprintf(b, sizeof(b), "H rnd_index_after=%d rnd_seed_after=%d", rnd_index, rnd_seed);
    header.push_back(b);
    std::snprintf(b, sizeof(b), "H player=%s flight=%d member=%d", player_key.c_str(), h.player_flight,
                  h.player_member);
    header.push_back(b);
    std::snprintf(b, sizeof(b), "H slots=%d first_ground_slot=%d flights=%d empty_formations=%d areas=%d populated=%d",
                  t.count, t.first_ground, h.flights, empty_formations, n_areas, populated);
    header.push_back(b);
    const std::string text = format_manifest(header, v);
    const std::wstring dir = world_files_dir();
    if (dir.empty()) {
        LOG("world: no folder for the world files (the session has ended); no manifest");
        return;
    }
    const std::wstring path = numbered(dir, a.number, L".manifest.txt");
    if (!write_text(path, text, true)) {
        LOG("world: ERR could not write %ls (exists, or path too long)", path.c_str());
        return;
    }
    LOG("world: manifest %ls: %s; %zu lines, player %s, %d formations without members", path.c_str(),
        format_manifest_counts(v).c_str() + 2, v.size(), player_key.c_str(), empty_formations);
}

bool own_flight(int* members, int* member);  // the player flight's size and our member (seat section)

// Host: the saved file and its final sidecar, packed, for clients that ask (T5).
void publish_mission(const Armed& a) {
    std::vector<uint8_t> msn, packed;
    uint32_t id = 0, raw = 0;
    if (!read_bytes(a.msn_path, &msn)) {
        LOG("world: ERR could not read %ls back to serve it", a.msn_path.c_str());
        return;
    }
    if (!net::pack_mission(format_sidecar(a.sidecar), msn, &packed, &id, &raw)) {
        LOG("world: the mission is too big to serve (%zu bytes)", msn.size());
        return;
    }
    int members = 0, host_member = 0;
    own_flight(&members, &host_member);
    host_publish_mission(packed.data(), packed.size(), id, raw, members, host_member);
    g_published_id = id;
    // The lobby's summary of it (three short ASCII lines).
    char l1[40], l2[40], l3[40];
    const WorldSidecar& s = a.sidecar;
    std::snprintf(l1, sizeof(l1), "Theatre: %s", s.theatre.c_str());
    std::snprintf(l2, sizeof(l2), "%04d-%02d-%02d %02d:%02d, %s", s.year, s.month, s.day, s.hour, s.minute, s.aircraft.c_str());
    std::snprintf(l3, sizeof(l3), "Player flight: %d aircraft", members);
    const char* const lines[3] = {l1, l2, l3};
    host_lobby_info(lines);
    g_world_host.store(true, std::memory_order_release);
}

int __fastcall h_Start(void* msn) {
    Armed a;
    AcquireSRWLockExclusive(&g_lock);
    if (g_mode == Mode::Off) {
        g_armed = Armed{};  // armed by a session that has ended since: the stock mission runs
    } else if (g_armed.on && g_armed.msn == msn) {
        a = g_armed;
        g_armed = Armed{};
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!a.on) {
        LOG("world: a single mission starts without an armed world flow (Load page, or no New-page Accept); no "
            "manifest");
        return o_Start(msn);
    }
    Ctx c{};
    if (!resolve(&c, "mission start") || c.msn != msn) return o_Start(msn);
    if (g_join) {
        g_flying_id = g_load_path == a.msn_path ? g_fetched_id : 0;
    }
    const bool seeded = call_seed(c.rnd, a.seed);
    // -1 makes the start read the density through the getter, where it is recorded (Save) or forced (Load).
    const bool density_reset = write_density(c.sm, -1);
    g_force_density = g_mode == Mode::Load ? a.sidecar.density : -1;
    g_force_start = g_mode == Mode::Load ? a.sidecar.mission_start : -1;
    g_seen_density = g_seen_start = -1;
    LOG("world: mission start: RND seeded with %lu%s%s; %s", static_cast<unsigned long>(a.seed),
        seeded ? "" : " FAILED", density_reset ? "" : ", density reset FAILED",
        g_mode == Mode::Load ? "forcing the sidecar's density and mission start" : "recording density and start");
    g_start_thread.store(GetCurrentThreadId(), std::memory_order_release);
    const int result = o_Start(msn);
    g_start_thread.store(0, std::memory_order_release);
    int idx = -1, sd = -1;
    read_rnd(c.rnd, &idx, &sd);
    LOG("world: mission objects created (start returned %d): density %d, mission start %d (-1 = not read), RND seed "
        "%d index %d",
        result, g_seen_density, g_seen_start, sd, idx);
    if (g_mode == Mode::Save) {
        a.sidecar.density = g_seen_density;
        a.sidecar.mission_start = g_seen_start;
        if (!write_text(a.sidecar_path, format_sidecar(a.sidecar), false))
            LOG("world: ERR could not rewrite %ls", a.sidecar_path.c_str());
        else
            LOG("world: sidecar %ls now holds density %d and mission start %d", a.sidecar_path.c_str(),
                g_seen_density, g_seen_start);
        publish_mission(a);
    }
    write_manifest(c, a, idx, sd);
    return result;
}

void remote_before_start();
void remote_after_start(void* msn);

// ---- seat (T3) ----

struct PlayerFlight {
    int index;      // +0x2b24
    int member;     // +0x2b28
    int members;    // flight record +0xe8
    int slot;       // flight record +0x82
};
// The player flight's record, found by walking the flight list (bounded by its count). SEH only.
bool read_player_flight(const void* msn, PlayerFlight* f) {
    __try {
        const BYTE* m = static_cast<const BYTE*>(msn);
        int count = 0;
        std::memcpy(&f->index, m + kOffPlayerFlight, sizeof(int));
        std::memcpy(&f->member, m + kOffPlayerMember, sizeof(int));
        std::memcpy(&count, m + kOffFlightCount, sizeof(int));
        if (count < 1 || count > kMaxSlots || f->index < 0 || f->index >= count) return false;
        const BYTE* node = *reinterpret_cast<const BYTE* const*>(m + kOffFlightHead);
        for (int i = 0; i < f->index && node; ++i) node = *reinterpret_cast<const BYTE* const*>(node + kFlightNodeNext);
        if (!node) return false;
        const BYTE* rec = *reinterpret_cast<const BYTE* const*>(node + kFlightNodeRec);
        if (!rec) return false;
        int16_t members = 0, slot = 0;
        std::memcpy(&members, rec + kFlightMembers, sizeof(members));
        std::memcpy(&slot, rec + kFlightSlot, sizeof(slot));
        f->members = members;
        f->slot = slot;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool own_flight(int* members, int* member) {
    PlayerFlight f{};
    HMODULE sm = GetModuleHandleW(L"SingleMission.dll");
    if (!sm || !read_player_flight(at_rva(sm, kRvaMsn), &f)) return false;
    *members = f.members;
    *member = f.member;
    return true;
}

bool write_player_member(void* msn, int member) {
    __try {
        std::memcpy(static_cast<BYTE*>(msn) + kOffPlayerMember, &member, sizeof(int));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The object id of member m of mission slot s, and the player object (+0x1c). SEH only.
bool read_member_id(const SlotTable& t, int s, int m, uint32_t* id) {
    if (s < 0 || s >= t.count || m < 0 || m >= kMaxMembers) return false;
    __try {
        std::memcpy(id, t.arr + static_cast<size_t>(s) * kSlotBytes + kSlotMembers + 4 * static_cast<size_t>(m),
                    sizeof(uint32_t));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool read_player_obj(const void* msn, uint32_t* id) {
    __try {
        std::memcpy(id, static_cast<const BYTE*>(msn) + kOffPlayerObj, sizeof(uint32_t));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Before the start: the player takes member g_seat of the player flight. Load and Generate both reset +0x2b28 to 0,
// so this is the last moment; slot 32 then writes the pilot into that member. See lazy-sf2-recon: network-recon §10.2.
bool apply_seat(void* msn) {
    PlayerFlight f{};
    if (!read_player_flight(msn, &f)) {
        LOG("world: seat: refused (the player flight could not be read); stock seat");
        return false;
    }
    const SeatDecision d = g_join && g_seat == 0 ? SeatDecision{true, 0, "ok"} : decide_seat(g_seat, f.members);
    if (!d.apply) {
        LOG("world: seat: refused (%s: flight %d has %d members, seat %d asked); stock seat", d.why, f.index,
            f.members, g_seat);
        return false;
    }
    if (!write_player_member(msn, d.member)) {
        LOG("world: ERR seat: writing the member index faulted; stock seat");
        return false;
    }
    LOG("world: seat: the player takes member %d (x-%d) of flight %d (%d members) instead of member %d", d.member,
        d.member + 1, f.index, f.members, f.member);
    return true;
}

void check_seat(void* msn) {
    PlayerFlight f{};
    SlotTable t{};
    uint32_t member_id = 0, player_id = 0;
    if (!read_player_flight(msn, &f) || !read_slot_table(msn, &t) || !read_member_id(t, f.slot, f.member, &member_id) ||
        !read_player_obj(msn, &player_id)) {
        LOG("world: seat check: the mission could not be read after the start");
        return;
    }
    LOG("world: seat check: member index %d, player object 0x%08lx, member %d of slot %d is 0x%08lx: %s", f.member,
        static_cast<unsigned long>(player_id), f.member, f.slot, static_cast<unsigned long>(member_id),
        player_id == member_id && f.member == g_seat ? "ok" : "MISMATCH");
    LOG("world: seat: note that the probe's aircraft tracker still takes the first aircraft built (member 0) as the "
        "player");
}

void* g_remote_deferred = nullptr;  // the MSN whose mark pass waits for the host's session (game thread)

// Hosting a lobby: the seat the host flies is the book's for this flight's size, taken as the flight starts.
void host_lobby_seat(void* msn) {
    if (g_join || g_seat_from_env) return;
    PlayerFlight f{};
    if (!read_player_flight(msn, &f)) return;
    const int s = host_lobby_seat_for(f.members);
    if (s == -2) return;
    const int seat = s > 0 ? s : -1;  // member 0 is the mission's own seat: stock
    LOG("world: the lobby gives the host member %d of this %d-member flight%s", s < 0 ? 0 : s, f.members,
        seat == g_seat ? "" : " (changed)");
    g_seat = seat;
}

int __fastcall h_Slot32(void* msn) {
    HMODULE sm = GetModuleHandleW(L"SingleMission.dll");
    if (sm && msn == at_rva(sm, kRvaMsn) && g_mode == Mode::Save) host_lobby_seat(msn);
    if ((g_seat < 0 && !g_remote) || !sm || msn != at_rva(sm, kRvaMsn)) return o_Slot32(msn);
    if (g_remote) remote_before_start();
    const bool seated = g_seat >= 0 && apply_seat(msn);
    const int r = o_Slot32(msn);
    if (seated) check_seat(msn);
    if (g_remote) {
        if (g_join && !client_welcomed()) {
            // We loaded first: our world stays local (we own it) until the host's session; then it is offered to the
            // host and marked remote (handover task 4).
            g_remote_deferred = msn;
            LOG("world: remote: we loaded the mission before the host's session: our AI runs the world until then");
        } else {
            remote_after_start(msn);
        }
    }
    return r;
}

// ---- mass remote and the damage block (T4) ----

// AircraftObject's and GroundObject's REMOTEOBJECT masks (the object manager hands out one bit per flag name, so
// vehicles and ships in the GroundObject group use the same bit). See lazy-sf2-recon: network-recon §10.4 and §10.13.
constexpr DWORD kAoStamp = 0x51d220bd, kAoSize = 0xc2000, kRvaAoRemoteMask = 0xb7b28;
constexpr DWORD kGoStamp = 0x51d220a7, kGoSize = 0x54000, kRvaGoRemoteMask = 0x4d620;
constexpr DWORD kGmStamp = 0x51d220a2, kGmSize = 0x47000;
constexpr DWORD kNvStamp = 0x51d220cd, kNvSize = 0x4a000;
constexpr size_t kDamageIface = 0x1c;  // QI 9, the damage interface, inside every aircraft and ground object

std::atomic<bool> g_remote_on{false};          // set after the marking pass, cleared at the next start
std::atomic<uint32_t> g_mask_air{0}, g_mask_ground{0};
std::atomic<uint64_t> g_blocked{0};
HMODULE g_ao = nullptr, g_ao_bad = nullptr, g_go = nullptr, g_go_bad = nullptr, g_gm = nullptr, g_gm_bad = nullptr,
        g_nv = nullptr, g_nv_bad = nullptr;  // guarded by g_hook_lock

bool read_mask(const wchar_t* module, DWORD stamp, DWORD size, const char* name, DWORD rva, uint32_t* out) {
    HMODULE m = GetModuleHandleW(module);
    if (!m || !is_expected_build(m, stamp, size, name)) return false;
    __try {
        std::memcpy(out, at_rva(m, rva), sizeof(uint32_t));
        return *out != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The object an id names, and which group it is in. SEH only.
bool resolve_obj(const Om& o, uint32_t id, BYTE** obj, int* group_kind) {
    __try {
        if (id == 0xffffffffu || !o.mgr) return false;
        const uint32_t ngroups = *reinterpret_cast<const uint32_t*>(o.mgr + kMgrGroupCount);
        const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(o.mgr + kMgrGroups);
        const uint32_t gi = id >> 24, slot = id & 0xffffff;
        if (!arr || gi >= ngroups || gi >= kMaxGroups) return false;
        const uintptr_t grp = arr + gi * kGroupStride;
        const uint32_t count = *reinterpret_cast<const uint32_t*>(grp + kGrpCount);
        const uintptr_t* slots = *reinterpret_cast<const uintptr_t* const*>(grp + kGrpSlots);
        if (!slots || slot >= count || slot >= kMaxGroupSlots) return false;
        *obj = reinterpret_cast<BYTE*>(slots[slot]);
        *group_kind = grp == o.aircraft_grp ? 1 : grp == o.ground_grp ? 2 : 0;
        return *obj != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 0 set now, 1 already set, 2 faulted.
int set_flag(BYTE* obj, uint32_t mask) {
    __try {
        uint32_t f = 0;
        std::memcpy(&f, obj + kObjFlags, sizeof(f));
        if (f & mask) return 1;
        f |= mask;
        std::memcpy(obj + kObjFlags, &f, sizeof(f));
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

// The player's aircraft by its flag (+0x168 bit 1), besides the mission's player id. SEH only.
bool has_player_flag(const BYTE* obj) {
    __try {
        uint32_t f = 0;
        std::memcpy(&f, obj + 0x168, 4);
        return (f & 1u) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void mark_one(const Om& om, uint32_t id, uint32_t player, RemoteKind kind, RemoteCounts* c) {
    BYTE* obj = nullptr;
    int group = 0;
    const bool resolved = resolve_obj(om, id, &obj, &group);
    // Never the player's jet: by the mission's player id, the aircraft tracker's player, or its player flag.
    if (resolved && (id == player || (group == 1 && (obj == sim_player_object() || has_player_flag(obj))))) {
        ++c->player;
        return;
    }
    if (!should_mark_remote(id, player, resolved) || group == 0) {
        ++c->unresolved;
        return;
    }
    const uint32_t mask = group == 1 ? g_mask_air.load() : g_mask_ground.load();
    const int r = set_flag(obj, mask);
    if (r == 0) c->count(kind);
    else if (r == 1) ++c->already;
    else ++c->faulted;
}

void remote_before_start() {
    g_remote_deferred = nullptr;
    const bool was = g_remote_on.exchange(false);
    const uint64_t n = g_blocked.exchange(0);
    if (was) LOG("world: remote: the last mission blocked %llu local damage calls to remote objects", n);
}

// After the start: every object of the mission except the player's is made REMOTEOBJECT, and from now on local
// damage to REMOTEOBJECT objects is dropped (with NET null the game would apply it; network-recon §10.4).
void remote_after_start(void* msn) {
    uint32_t air = 0, ground = 0;
    if (!read_mask(L"AircraftObject.dll", kAoStamp, kAoSize, "AircraftObject.dll", kRvaAoRemoteMask, &air) ||
        !read_mask(L"GroundObject.dll", kGoStamp, kGoSize, "GroundObject.dll", kRvaGoRemoteMask, &ground)) {
        LOG("world: remote: refused (REMOTEOBJECT masks unreadable or not set)");
        return;
    }
    if (air != ground) LOG("world: remote: note the aircraft and ground REMOTEOBJECT masks differ (0x%08lx, 0x%08lx)",
                           static_cast<unsigned long>(air), static_cast<unsigned long>(ground));
    g_mask_air = air;
    g_mask_ground = ground;
    Om om;
    HMODULE omm = GetModuleHandleW(L"ObjectManager.dll");
    SlotTable t{};
    uint32_t player = 0xffffffffu;
    if (!omm || !is_expected_build(omm, kOmStamp, kOmSize, "ObjectManager.dll") || !read_om(omm, &om) ||
        !read_slot_table(msn, &t) || !read_player_obj(msn, &player)) {
        LOG("world: remote: refused (object manager or slot table not as expected)");
        return;
    }
    RemoteCounts c;
    for (int i = 0; i < t.count; ++i) {
        SlotRaw s{};
        if (!read_slot(t, i, &s)) break;
        if (s.formation == 0xffffffffu) continue;
        for (int m = 0; m < s.members; ++m)
            mark_one(om, s.ids[m], player, i < t.first_ground ? RemoteKind::Aircraft : RemoteKind::Ground, &c);
    }
    const BYTE* areas = nullptr;
    int n_areas = 0;
    if (read_areas_head(msn, &areas, &n_areas)) {
        for (int i = 0; i < n_areas; ++i) {
            AreaRaw ar{};
            if (!read_area(areas, i, &ar)) break;
            if (ar.active != 1) continue;
            for (int j = 0; j < ar.n_placed; ++j) {
                PlacedRaw p{};
                if (read_placed(ar, j, &p) && p.obj && p.id != 0xffffffffu)
                    mark_one(om, p.id, player, RemoteKind::Placed, &c);
            }
        }
    }
    g_remote_on = true;
    LOG("world: remote: %s (mask 0x%08lx); local damage to REMOTEOBJECT objects is now blocked",
        format_remote_counts(c).c_str(), static_cast<unsigned long>(air));
}

// Damage interface slots 4 (kinetic, 5 args), 5 (directed, 8) and 6 (blast, 6), per class DLL; the shooter id is the
// fourth argument in each. Index: class * 3 + (slot - 4); classes aircraft, ground, vehicle, ship.
void* g_dmg_orig[12] = {};
const char* const kDmgClass[4] = {"aircraft", "ground object", "vehicle", "ship"};

// args are the call's arguments (5, 8 or 6 of them); the shooter id is args[3]. A gun hit the hit claims take (a
// claim sent for a remote target, or a hit forwarded to the PC that owns the target) is dropped here too.
bool damage_blocked(void* iface, int k, const uint32_t* args) {
    const uint32_t shooter = args[3];
    BYTE* obj = static_cast<BYTE*>(iface) - kDamageIface;
    bool remote = false;
    if (g_remote_on.load(std::memory_order_acquire)) {
        const uint32_t mask =
            k < 3 ? g_mask_air.load(std::memory_order_relaxed) : g_mask_ground.load(std::memory_order_relaxed);
        uint32_t flags = 0;
        __try {
            std::memcpy(&flags, obj + kObjFlags, sizeof(flags));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        remote = block_damage(true, flags, mask);
    }
    const bool taken = hits_on_damage(obj, 4 + k % 3, args, remote);
    if (!remote) return taken;
    const uint64_t n = g_blocked.fetch_add(1) + 1;
    if (log_milestone(n))
        LOG("world: remote: blocked %s damage (slot %d) to %s %p from shooter 0x%08lx (%llu blocked so far)",
            k % 3 == 0 ? "kinetic" : k % 3 == 1 ? "directed" : "blast", 4 + k % 3, kDmgClass[k / 3], obj,
            static_cast<unsigned long>(shooter), n);
    return true;
}

using Dmg5_t = int(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using Dmg8_t = int(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t);
using Dmg6_t = int(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

// The originals are void; eax is passed through anyway. A blocked call returns 0 and pops the same arguments.
template <int K>
int __fastcall h_dmg5(void* i, void* e, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t f) {
    const uint32_t args[5] = {a, b, c, d, f};
    if (damage_blocked(i, K, args)) return 0;
    return reinterpret_cast<Dmg5_t>(g_dmg_orig[K])(i, e, a, b, c, d, f);
}
template <int K>
int __fastcall h_dmg8(void* i, void* e, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t f, uint32_t g,
                      uint32_t h, uint32_t j) {
    const uint32_t args[8] = {a, b, c, d, f, g, h, j};
    if (damage_blocked(i, K, args)) return 0;
    return reinterpret_cast<Dmg8_t>(g_dmg_orig[K])(i, e, a, b, c, d, f, g, h, j);
}
template <int K>
int __fastcall h_dmg6(void* i, void* e, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t f, uint32_t g) {
    const uint32_t args[6] = {a, b, c, d, f, g};
    if (damage_blocked(i, K, args)) return 0;
    return reinterpret_cast<Dmg6_t>(g_dmg_orig[K])(i, e, a, b, c, d, f, g);
}


bool read_flags(const BYTE* obj, uint32_t* f) {
    __try {
        std::memcpy(f, obj + kObjFlags, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- the world stream (T6) ----

// Flag masks for wake-ups (AircraftObject +0x63e70 and GroundObject +0x16ee0 ask the object manager for them by name).
constexpr DWORD kRvaAoAwakeMask = 0xb7b14, kRvaAoAsleepMask = 0xb7b18;
constexpr DWORD kRvaGmVtable = 0x347a4, kRvaNvVtable = 0x375f4;  // vehicles and ships move; static sites do not
constexpr DWORD kRvaAoDestroy = 0x41030, kRvaGoDestroy = 0x159b0;  // primary slot 29 in AircraftObject / GroundObject
constexpr size_t kSlotDestroy = 29;
constexpr size_t kObjVel = 0x50, kObjRates = 0xd0, kObjMatrix = 0xdc, kObjDamage = 0x16c, kPlacedDamage = 0x04;
constexpr int kDestroyedLevel = 5;
constexpr double kEventEvery = 1.0;  // seconds of host time between event walks

// AircraftFormation: each flight's formation object holds its AI at +0x24 (AircraftFormationAIClass, vtable +0x171ac),
// whose current waypoint is +0x18 (count +0x14); slot 10 (+0x7d00, thiscall(int), ret 4) sets it, clamped, and slot 11
// reads it. See lazy-sf2-recon: network-recon §10.17.
constexpr DWORD kAfStamp = 0x51d22040, kAfSize = 0x23000;
constexpr DWORD kRvaAfAiVtable = 0x171ac, kRvaAfSetWaypoint = 0x7d00;
constexpr size_t kFormationAi = 0x24, kAiWaypoint = 0x18, kAiWaypointCount = 0x14, kAiSetWaypointSlot = 10;

// The formation AI of a flight's formation object, if it is what we expect. SEH only.
BYTE* formation_ai(const BYTE* formation, uintptr_t af) {
    if (!formation || !af) return nullptr;
    __try {
        BYTE* ai = *reinterpret_cast<BYTE* const*>(formation + kFormationAi);
        if (!ai || *reinterpret_cast<const uintptr_t*>(ai) != af + kRvaAfAiVtable) return nullptr;
        return ai;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
bool read_waypoint(const BYTE* ai, int* wp, int* count) {
    __try {
        std::memcpy(wp, ai + kAiWaypoint, 4);
        std::memcpy(count, ai + kAiWaypointCount, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Sets the flight's current waypoint through the AI's own slot 10 (it clamps and recomputes its leg). 0 ok, 2 faulted,
// 1 the slot is not the function we read.
int set_waypoint(BYTE* ai, uintptr_t af, int wp) {
    uintptr_t fn = 0;
    __try {
        fn = reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(ai))[kAiSetWaypointSlot]);
        if (fn != af + kRvaAfSetWaypoint) return 1;
        reinterpret_cast<void(__fastcall*)(void*, void*, int)>(fn)(ai, nullptr, wp);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

struct ObjFull {
    BYTE* obj = nullptr;
    int group = 0;  // 1 aircraft, 2 ground group
    bool mover = false;
    uint32_t flags = 0;
    int level = 0;
    net::ObjPose pose{};
};

// Reads what the stream needs from one object. SEH only.
bool read_full(const Om& om, uint32_t id, uintptr_t gm, uintptr_t nv, ObjFull* f) {
    if (!resolve_obj(om, id, &f->obj, &f->group) || f->group == 0) return false;
    __try {
        const BYTE* o = f->obj;
        std::memcpy(&f->flags, o + kObjFlags, 4);
        std::memcpy(&f->level, o + kObjDamage, 4);
        std::memcpy(f->pose.pos, o + kObjPos, sizeof(f->pose.pos));
        std::memcpy(f->pose.ang, o + kObjAngles, sizeof(f->pose.ang));
        if (f->group == 1) {
            f->mover = true;
            std::memcpy(f->pose.vel, o + kObjVel, sizeof(f->pose.vel));
            std::memcpy(f->pose.rates, o + kObjRates, sizeof(f->pose.rates));
        } else {
            // Vehicles and ships: position and angles only; their velocity field is not known yet (network-recon §10.4).
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(o);
            f->mover = (gm && vt == gm + kRvaGmVtable) || (nv && vt == nv + kRvaNvVtable);
            f->pose.vel[0] = f->pose.vel[1] = f->pose.vel[2] = 0;
            f->pose.rates[0] = f->pose.rates[1] = f->pose.rates[2] = 0;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_i16(const void* p, int16_t* v) {
    __try {
        std::memcpy(v, p, 2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct StreamMasks {
    uint32_t awake = 0, asleep = 0, remote = 0;
};
constexpr DWORD kRvaGoAwakeMask = 0x4d60c, kRvaGoAsleepMask = 0x4d610;
// The flag masks per class: [0] aircraft (AircraftObject), [1] the GroundObject group (GroundObject's; vehicles and
// ships get the same bits from the object manager by name, which the caller checks once).
bool read_stream_masks(StreamMasks m[2]) {
    return read_mask(L"AircraftObject.dll", kAoStamp, kAoSize, "AircraftObject.dll", kRvaAoAwakeMask, &m[0].awake) &&
           read_mask(L"AircraftObject.dll", kAoStamp, kAoSize, "AircraftObject.dll", kRvaAoAsleepMask, &m[0].asleep) &&
           read_mask(L"AircraftObject.dll", kAoStamp, kAoSize, "AircraftObject.dll", kRvaAoRemoteMask, &m[0].remote) &&
           read_mask(L"GroundObject.dll", kGoStamp, kGoSize, "GroundObject.dll", kRvaGoAwakeMask, &m[1].awake) &&
           read_mask(L"GroundObject.dll", kGoStamp, kGoSize, "GroundObject.dll", kRvaGoAsleepMask, &m[1].asleep) &&
           read_mask(L"GroundObject.dll", kGoStamp, kGoSize, "GroundObject.dll", kRvaGoRemoteMask, &m[1].remote);
}

uintptr_t module_if(const wchar_t* name, DWORD stamp, DWORD size, const char* label) {
    HMODULE m = GetModuleHandleW(name);
    return m && is_expected_build(m, stamp, size, label) ? reinterpret_cast<uintptr_t>(m) : 0;
}

// The aircraft that is a member of the player flight (a peer's seat, as the host assigned it). Game thread.
void* seat_object(int member) {
    HMODULE sm = GetModuleHandleW(L"SingleMission.dll");
    HMODULE omm = GetModuleHandleW(L"ObjectManager.dll");
    if (!sm || !omm || member < 0) return nullptr;
    void* msn = at_rva(sm, kRvaMsn);
    PlayerFlight f{};
    SlotTable t{};
    uint32_t id = 0;
    Om om;
    BYTE* obj = nullptr;
    int group = 0;
    if (!read_player_flight(msn, &f) || member >= f.members || !read_slot_table(msn, &t) ||
        !read_member_id(t, f.slot, member, &id) || !read_om(omm, &om) || !resolve_obj(om, id, &obj, &group) ||
        group != 1)
        return nullptr;
    return obj;
}

struct HostStream {
    const void* msn = nullptr;
    const BYTE* slots = nullptr;  // the slot array of the mission being streamed: a new one starts over
    uint32_t epoch = 0, mission = 0;  // sim_player_epoch() and the published mission it was reset for
    bool have_masks = false;      // read once per reset (the build checks are not repeated every frame)
    StreamMasks masks[2];
    uintptr_t gm = 0, nv = 0, af = 0;
    net::WorldSendClock clock;
    net::WorldEventTracker tracker;
    uint32_t seq = 0, eseq = 0;
    double next_events = 0, next_report = 0, next_waypoints = 0;
    uint64_t states = 0, state_bytes = 0, events = 0, msgs = 0, send_fail = 0;
    bool logged_start = false;
};
HostStream* g_hs = nullptr;  // game thread


}  // namespace

bool world_host_streaming() { return g_world_host.load(std::memory_order_acquire); }

void* world_member_object(int member) {
    if ((!g_world_host.load(std::memory_order_acquire) && !g_join) || member == net::kNoSeat) return nullptr;
    return seat_object(member);
}

bool world_host_snapshot(int slot, uint32_t mission_id, WorldSendToFn send) {
    if (!g_world_host.load(std::memory_order_acquire) || !send) return true;
    std::vector<net::WorldEventRec> ev;
    if (g_hs && g_hs->mission == g_published_id) g_hs->tracker.snapshot(&ev);
    const size_t cap = net::kSnapshotMaxParts * net::kWorldEventsMax;
    if (ev.size() > cap) {
        LOG("world: snapshot for client %d: %zu events, cut to %zu (the events ring)", slot + 1, ev.size(), cap);
        ev.resize(cap);
    }
    const uint32_t ms = static_cast<uint32_t>(sim_game_time() * 1000.0);
    net::WorldSnapshotMsg head{};
    head.mission_id = mission_id;
    head.host_ms = ms;
    head.records = static_cast<uint16_t>(ev.size());
    head.parts = static_cast<uint8_t>((ev.size() + net::kWorldEventsMax - 1) / net::kWorldEventsMax);
    uint8_t buf[net::kWorldMaxMessage];
    bool ok = send(slot, buf, net::encode(head, buf, sizeof(buf)), true);
    size_t bytes = sizeof(head);
    uint32_t seq = 0;
    net::world_batches(ev.data(), ev.size(), net::kWorldEventsMax, [&](const net::WorldEventRec* r, size_t n) {
        const size_t len = net::encode_world_events(++seq, ms, r, n, buf, sizeof(buf));
        ok = ok && len && send(slot, buf, len, true);
        bytes += len;
    });
    LOG("world: snapshot for client %d: %zu events in %u parts (%zu bytes)%s", slot + 1, ev.size(),
        static_cast<unsigned>(head.parts), bytes, ok ? "" : "; NOT all queued");
    return ok;
}

void world_host_tick(double now, const double (*players)[3], int n_players, WorldSendFn send) {
    if (!g_world_host.load(std::memory_order_acquire) || !send) return;
    if (!g_hs) g_hs = new (std::nothrow) HostStream;
    if (!g_hs) return;
    HostStream& hs = *g_hs;
    HMODULE sm = GetModuleHandleW(L"SingleMission.dll");
    HMODULE omm = GetModuleHandleW(L"ObjectManager.dll");
    if (!sm || !omm) return;
    void* msn = at_rva(sm, kRvaMsn);
    SlotTable t{};
    Om om;
    uint32_t player = 0xffffffffu;
    if (!read_slot_table(msn, &t) || !read_om(omm, &om) || !read_player_obj(msn, &player)) return;
    // A new flight (player epoch), a new published mission or a new slot array: everything starts over.
    if (hs.msn != msn || hs.slots != t.arr || hs.epoch != sim_player_epoch() || hs.mission != g_published_id) {
        hs.clock.clear();
        hs.tracker.clear();
        hs.msn = msn;
        hs.slots = t.arr;
        hs.epoch = sim_player_epoch();
        hs.mission = g_published_id;
        hs.next_events = now;
        hs.logged_start = false;
        hs.have_masks = read_stream_masks(hs.masks);
        if (hs.have_masks && (hs.masks[0].awake != hs.masks[1].awake || hs.masks[0].asleep != hs.masks[1].asleep ||
                              hs.masks[0].remote != hs.masks[1].remote))
            LOG("world: stream: note the aircraft and ground flag masks differ; each class uses its own");
        hs.gm = module_if(L"GroundMobileObject.dll", kGmStamp, kGmSize, "GroundMobileObject.dll");
        hs.nv = module_if(L"NavalObject.dll", kNvStamp, kNvSize, "NavalObject.dll");
        hs.af = module_if(L"AircraftFormation.dll", kAfStamp, kAfSize, "AircraftFormation.dll");
    }
    const bool have_masks = hs.have_masks;
    const uintptr_t gm = hs.gm, nv = hs.nv;
    const bool event_walk = now >= hs.next_events || now < hs.next_events - 2 * kEventEvery;
    if (event_walk) hs.next_events = now + kEventEvery;
    std::vector<net::WorldObjState> out;
    std::vector<net::WorldEventRec> ev;
    int movers = 0, statics = 0;
    for (int i = 0; i < t.count; ++i) {
        SlotRaw s{};
        if (!read_slot(t, i, &s)) break;
        if (s.formation == 0xffffffffu) continue;
        const uint8_t kind = static_cast<uint8_t>(i < t.first_ground ? net::KeyKind::Aircraft : net::KeyKind::Ground);
        if (event_walk && kind == static_cast<uint8_t>(net::KeyKind::Aircraft)) {  // the flight's current waypoint
            BYTE* fo = nullptr;
            int fg = 0;
            int wp = -1, wn = 0;
            const BYTE* ai = resolve_obj(om, s.formation, &fo, &fg) ? formation_ai(fo, hs.af) : nullptr;
            if (ai && read_waypoint(ai, &wp, &wn) && wp >= 0 && wp < 32767) {
                net::WorldSeen seen;
                seen.waypoint = static_cast<int16_t>(wp);
                hs.tracker.observe(kind, static_cast<uint16_t>(i), net::kFlightKey, seen, &ev);
            }
        }
        for (int m = 0; m < s.members; ++m) {
            ObjFull f;
            const bool ok = read_full(om, s.ids[m], gm, nv, &f);
            if (event_walk) {
                net::WorldSeen seen;
                seen.alive = ok && f.level < kDestroyedLevel;
                const StreamMasks& mk = hs.masks[f.group == 2 ? 1 : 0];
                seen.awake = ok && have_masks && (f.flags & mk.awake);
                seen.asleep = ok && have_masks && (f.flags & mk.asleep);
                seen.level = static_cast<int16_t>(ok ? f.level : 0);
                hs.tracker.observe(kind, static_cast<uint16_t>(i), static_cast<uint16_t>(m), seen, &ev);
            }
            if (!ok || f.level >= kDestroyedLevel) continue;
            if (!f.mover) {
                ++statics;
                continue;
            }
            ++movers;
            // The host's own jet and the clients' avatars travel as Phase 4 States, not here.
            if (s.ids[m] == player || host_drives_object(f.obj)) continue;
            double d = 1e30;
            for (int p = 0; p < n_players; ++p) {
                const double dx = players[p][0] - f.pose.pos[0], dy = players[p][1] - f.pose.pos[1],
                             dz = players[p][2] - f.pose.pos[2];
                const double dd = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dd < d) d = dd;
            }
            if (!hs.clock.due(net::world_key(kind, static_cast<uint16_t>(i), static_cast<uint16_t>(m)), now, d)) continue;
            net::WorldObjState q{};
            q.kind = kind;
            q.a = static_cast<uint16_t>(i);
            q.b = static_cast<uint16_t>(m);
            net::quantise(f.pose, &q);
            out.push_back(q);
        }
    }
    if (event_walk && now >= hs.next_waypoints) {  // every 10 s: the clients' formations cannot drift for long
        hs.next_waypoints = now + 10.0;
        hs.tracker.waypoints(&ev);
    }
    if (event_walk) {
        const BYTE* areas = nullptr;
        int n_areas = 0;
        if (read_areas_head(msn, &areas, &n_areas)) {
            for (int i = 0; i < n_areas; ++i) {
                AreaRaw ar{};
                if (!read_area(areas, i, &ar)) break;
                if (ar.active != 1) continue;
                for (int j = 0; j < ar.n_placed; ++j) {
                    PlacedRaw p{};
                    int16_t dmg = 0;
                    if (!read_placed(ar, j, &p) || !read_i16(ar.placed[j] + kPlacedDamage, &dmg)) continue;
                    net::WorldSeen seen;
                    seen.level = dmg;
                    hs.tracker.observe(static_cast<uint8_t>(net::KeyKind::Placed), static_cast<uint16_t>(i),
                                       static_cast<uint16_t>(p.index), seen, &ev);
                }
            }
        }
    }
    const uint32_t ms = static_cast<uint32_t>(now * 1000.0);
    uint8_t buf[net::kWorldMaxMessage];
    net::world_batches(out.data(), out.size(), net::kWorldStatesMax, [&](const net::WorldObjState* r, size_t n) {
        const size_t len = net::encode_world_states(++hs.seq, ms, r, n, buf, sizeof(buf));
        if (len && send(buf, len, false)) {
            hs.state_bytes += len;
            ++hs.msgs;
        } else {
            ++hs.send_fail;
        }
    });
    net::world_batches(ev.data(), ev.size(), net::kWorldEventsMax, [&](const net::WorldEventRec* r, size_t n) {
        const size_t len = net::encode_world_events(++hs.eseq, ms, r, n, buf, sizeof(buf));
        if (!len || !send(buf, len, true)) ++hs.send_fail;
    });
    for (const net::WorldEventRec& e : ev)
        if (e.event == net::kEventDestroyed || e.event == net::kEventAwake || e.event == net::kEventAsleep)
            LOG("world: stream: event %s %c %03u.%02u", e.event == net::kEventDestroyed ? "destroyed"
                                                        : e.event == net::kEventAwake   ? "awake"
                                                                                        : "asleep",
                e.kind == 1 ? 'A' : 'G', static_cast<unsigned>(e.a), static_cast<unsigned>(e.b));
    hs.states += out.size();
    hs.events += ev.size();
    if (!hs.logged_start) {
        hs.logged_start = true;
        hs.next_report = now + 10.0;
        LOG("world: stream on: %d moving and %d static mission objects; masks %s", movers, statics,
            have_masks ? "read" : "NOT read (no wake-up events)");
    } else if (now >= hs.next_report) {
        hs.next_report = now + 10.0;
        LOG("world: stream: %llu states (%llu bytes in %llu messages), %llu events, %llu send failures so far",
            hs.states, hs.state_bytes, hs.msgs, hs.events, hs.send_fail);
    }
}

namespace {

struct ClientStream {
    const BYTE* slots = nullptr;
    uint32_t epoch = 0, mission = 0;  // reset on a new flight or mission
    bool have_masks = false;
    StreamMasks masks[2];
    std::unordered_map<uint64_t, uint32_t> last_ms;  // per key: the newest state applied
    std::unordered_map<uint64_t, bool> destroyed;    // keys the host destroyed: late states are not written on the wreck
    int skip_parts = 0;  // WorldEvents parts of a snapshot for another mission: not applied
    uintptr_t af = 0;
    uint64_t applied = 0, stale = 0, missing = 0, not_remote = 0, events = 0;
    double next_report = 0;
    bool logged_first = false, logged_refresh = false;
};
ClientStream* g_cs = nullptr;

bool write_pose(BYTE* obj, int group, const net::ObjPose& p) {
    __try {
        std::memcpy(obj + kObjPos, p.pos, sizeof(p.pos));
        std::memcpy(obj + kObjAngles, p.ang, sizeof(p.ang));
        if (group == 1) {
            const double a[3] = {p.ang[0], p.ang[1], p.ang[2]};
            float m[9];
            replay_math::build_matrix(a, m);
            std::memcpy(obj + kObjVel, p.vel, sizeof(p.vel));
            std::memcpy(obj + kObjRates, p.rates, sizeof(p.rates));
            std::memcpy(obj + kObjMatrix, m, sizeof(m));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool set_flags(BYTE* obj, uint32_t set, uint32_t clear) {
    __try {
        uint32_t f = 0;
        std::memcpy(&f, obj + kObjFlags, 4);
        f = (f & ~clear) | set;
        std::memcpy(obj + kObjFlags, &f, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Destroys a remote copy through its own primary slot 29, as the game's remote "destroyed" event does
// (network-recon §9.7), after checking the slot against the class DLL. 0 done, 1 refused, 2 faulted.
int destroy_copy(BYTE* obj) {
    uintptr_t fn = 0;
    __try {
        fn = reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(obj))[kSlotDestroy]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
    const uintptr_t ao = module_if(L"AircraftObject.dll", kAoStamp, kAoSize, "AircraftObject.dll");
    const uintptr_t go = module_if(L"GroundObject.dll", kGoStamp, kGoSize, "GroundObject.dll");
    if (!((ao && fn == ao + kRvaAoDestroy) || (go && fn == go + kRvaGoDestroy))) return 1;
    __try {
        reinterpret_cast<int(__fastcall*)(void*)>(fn)(obj);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

// Placed record b of area a (by its +0x54 index), or null.
BYTE* find_placed(const void* msn, int a, int b) {
    const BYTE* areas = nullptr;
    int n_areas = 0;
    AreaRaw ar{};
    if (!read_areas_head(msn, &areas, &n_areas) || a < 0 || a >= n_areas || !read_area(areas, a, &ar)) return nullptr;
    for (int j = 0; j < ar.n_placed; ++j) {
        PlacedRaw p{};
        if (read_placed(ar, j, &p) && p.index == b) return const_cast<BYTE*>(ar.placed[j]);
    }
    return nullptr;
}

bool write_i16(BYTE* p, int16_t v) {
    __try {
        std::memcpy(p, &v, 2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void world_client_apply(const uint8_t* data, size_t len, double host_now) {
    if (!g_join || g_flying_id == 0) return;
    if (!g_cs) g_cs = new (std::nothrow) ClientStream;
    if (!g_cs) return;
    ClientStream& cs = *g_cs;
    HMODULE sm = GetModuleHandleW(L"SingleMission.dll");
    HMODULE omm = GetModuleHandleW(L"ObjectManager.dll");
    if (!sm || !omm) return;
    void* msn = at_rva(sm, kRvaMsn);
    SlotTable t{};
    Om om;
    uint32_t player = 0xffffffffu;
    if (!read_slot_table(msn, &t) || !read_om(omm, &om) || !read_player_obj(msn, &player)) return;
    if (cs.slots != t.arr || cs.epoch != sim_player_epoch() || cs.mission != g_flying_id) {
        cs.slots = t.arr;
        cs.epoch = sim_player_epoch();
        cs.mission = g_flying_id;
        cs.last_ms.clear();
        cs.destroyed.clear();
        cs.have_masks = read_stream_masks(cs.masks);
        cs.af = module_if(L"AircraftFormation.dll", kAfStamp, kAfSize, "AircraftFormation.dll");
    }
    if (!cs.have_masks) return;
    auto object_of = [&](uint8_t kind, uint16_t a, uint16_t b, BYTE** obj, int* group) {
        uint32_t id = 0;
        if (kind != 1 && kind != 2) return false;
        if (a >= t.count || (kind == 1) != (a < t.first_ground)) return false;
        return read_member_id(t, a, b, &id) && id != player && resolve_obj(om, id, obj, group) && *group != 0;
    };
    const uint8_t wt = net::peek_world_type(data, len);
    if (wt == static_cast<uint8_t>(net::WorldType::WorldSnapshot)) {
        net::WorldSnapshotMsg sn;
        if (net::decode(data, len, sn) != net::Status::Ok) return;
        LOG("world: snapshot from the host: mission 0x%08lx, %u events in %u parts at host time %.1f s (ours %.1f s)%s",
            static_cast<unsigned long>(sn.mission_id), static_cast<unsigned>(sn.records),
            static_cast<unsigned>(sn.parts), sn.host_ms / 1000.0, sim_game_time(),
            sn.mission_id == g_flying_id ? "" : "; ANOTHER mission: its parts are not applied");
        if (g_cs && sn.mission_id != g_flying_id) g_cs->skip_parts = sn.parts;
        return;
    }
    if (wt == static_cast<uint8_t>(net::WorldType::HitApply)) {
        hits_client_apply(data, len);
        return;
    }
    if (wt == static_cast<uint8_t>(net::WorldType::WeaponLaunch)) {
        weapons_apply_launch(data, len, host_now);
        return;
    }
    if (wt == static_cast<uint8_t>(net::WorldType::MissionClock)) {
        mission_clock_apply(data, len, host_now);
        return;
    }
    if (wt == static_cast<uint8_t>(net::WorldType::HandoverDone)) {
        handover_client_message(data, len);
        return;
    }
    if (wt == static_cast<uint8_t>(net::WorldType::SeatState)) {
        seat_state_apply(data, len, host_now);
        return;
    }
    if (wt == static_cast<uint8_t>(net::WorldType::Loadout) || wt == static_cast<uint8_t>(net::WorldType::Consumables) ||
        wt == static_cast<uint8_t>(net::WorldType::DamageState) || wt == static_cast<uint8_t>(net::WorldType::Countermeasure)) {
        uint16_t peer = 0;
        if (len >= sizeof(net::Header) + 2) std::memcpy(&peer, data + sizeof(net::Header), 2);
        if (wt <= static_cast<uint8_t>(net::WorldType::Consumables)) stores_apply(data, len, client_avatar_object(peer));
        else damage_apply(data, len, client_avatar_object(peer));
        return;
    }
    if (wt == static_cast<uint8_t>(net::WorldType::AvatarState)) {
        net::AvatarStateMsg am;
        if (net::decode(data, len, am) == net::Status::Ok) world_avatar_apply(data, len, client_avatar_object(am.peer));
        return;
    }
    if (wt == static_cast<uint8_t>(net::WorldType::WorldStates)) {
        net::WorldStatesHead h;
        const net::WorldObjState* r = nullptr;
        if (net::decode_world_states(data, len, h, &r) != net::Status::Ok) return;
        double age = host_now - h.host_ms / 1000.0;
        if (!(age > 0)) age = 0;
        if (age > 1.0) age = 1.0;
        for (size_t i = 0; i < h.count; ++i) {
            net::WorldObjState q;
            std::memcpy(&q, &r[i], sizeof(q));
            BYTE* obj = nullptr;
            int group = 0;
            if (!object_of(q.kind, q.a, q.b, &obj, &group)) {
                ++cs.missing;
                continue;
            }
            if (client_drives_object(obj) || cs.destroyed.count(net::world_key(q.kind, q.a, q.b))) continue;
            uint32_t flags = 0;
            if (!read_flags(obj, &flags) || !(flags & cs.masks[group == 2 ? 1 : 0].remote)) {
                ++cs.not_remote;
                continue;
            }
            uint32_t& last = cs.last_ms[net::world_key(q.kind, q.a, q.b)];
            if (h.host_ms < last) {
                ++cs.stale;
                continue;
            }
            last = h.host_ms;
            net::ObjPose p;
            net::dequantise(q, &p);
            for (int k = 0; k < 3; ++k) p.pos[k] += p.vel[k] * age;  // to the host's present
            if (write_pose(obj, group, p)) ++cs.applied;
        }
        if (!cs.logged_first) {
            cs.logged_first = true;
            cs.next_report = host_now + 10.0;
            LOG("world: stream: first states from the host (%u objects, %.0f ms old)", static_cast<unsigned>(h.count),
                age * 1000.0);
        }
    } else if (wt == static_cast<uint8_t>(net::WorldType::WorldEvents)) {
        net::WorldEventsHead h;
        const net::WorldEventRec* r = nullptr;
        if (net::decode_world_events(data, len, h, &r) != net::Status::Ok) return;
        if (cs.skip_parts > 0) {
            --cs.skip_parts;
            return;
        }
        for (size_t i = 0; i < h.count; ++i) {
            net::WorldEventRec e;
            std::memcpy(&e, &r[i], sizeof(e));
            ++cs.events;
            if (e.event == net::kEventWaypoint && e.kind == static_cast<uint8_t>(net::KeyKind::Aircraft) &&
                e.b == net::kFlightKey) {
                PlayerFlight pf{};
                const bool own = read_player_flight(msn, &pf) && pf.slot == e.a;
                if (!apply_host_waypoint(waypoint_policy(), own)) continue;
                SlotRaw sr{};
                BYTE* fo = nullptr;
                int fg = 0, cur = -1, count = 0;
                BYTE* ai = e.a < t.count && read_slot(t, e.a, &sr) && sr.formation != 0xffffffffu &&
                                   resolve_obj(om, sr.formation, &fo, &fg)
                               ? formation_ai(fo, cs.af)
                               : nullptr;
                if (!ai || !read_waypoint(ai, &cur, &count)) {
                    LOG("world: stream: waypoint %d for flight slot %u: the formation is not readable here", e.value,
                        static_cast<unsigned>(e.a));
                    continue;
                }
                if (cur == e.value) continue;
                const int r3 = set_waypoint(ai, cs.af, e.value);
                LOG("world: stream: flight slot %u%s follows the host to waypoint %d (was %d of %d): %s",
                    static_cast<unsigned>(e.a), own ? " (ours)" : "", e.value, cur, count,
                    r3 == 0 ? "set" : r3 == 1 ? "NOT set (unexpected slot)" : "FAULTED");
                continue;
            }
            if (e.event == net::kEventPlacedDamage && e.kind == static_cast<uint8_t>(net::KeyKind::Placed)) {
                if (BYTE* rec = find_placed(msn, e.a, e.b)) write_i16(rec + kPlacedDamage, e.value);
                if (!cs.logged_refresh) {
                    cs.logged_refresh = true;
                    LOG("world: stream: placed damage is written to the record; its model is not refreshed yet");
                }
                continue;
            }
            BYTE* obj = nullptr;
            int group = 0;
            if (!object_of(e.kind, e.a, e.b, &obj, &group)) continue;
            const StreamMasks& mk = cs.masks[group == 2 ? 1 : 0];
            if (e.event == net::kEventAwake) {
                set_flags(obj, mk.awake, mk.asleep);
            } else if (e.event == net::kEventAsleep) {
                set_flags(obj, mk.asleep, mk.awake);
            } else if (e.event == net::kEventDestroyed) {
                cs.destroyed[net::world_key(e.kind, e.a, e.b)] = true;
                const int r2 = destroy_copy(obj);
                LOG("world: stream: %c %03u.%02u destroyed on the host: %s", e.kind == 1 ? 'A' : 'G',
                    static_cast<unsigned>(e.a), static_cast<unsigned>(e.b),
                    r2 == 0 ? "destroyed here" : r2 == 1 ? "not destroyed (its class is not checked)" : "FAULTED");
            }
        }
    }
    if (cs.logged_first && host_now >= cs.next_report) {
        cs.next_report = host_now + 10.0;
        LOG("world: stream: %llu states applied, %llu stale, %llu without an object, %llu not remote, %llu events",
            cs.applied, cs.stale, cs.missing, cs.not_remote, cs.events);
    }
}

namespace {

// ---- a player's jet state (T7): radar lock and configuration ----

// AircraftObject: the radar block (state +0x970, stage +0x974, target +0x988), the state word +0x5f0, and the detect
// interface at +0x24 (vtable +0x94374): slot 20 sets the mode, 21 the target by id, 22 is the RWR input
// (shooter id, stage, frequency, CW). See lazy-sf2-recon: recon-notes §27 and network-recon §10.19.
constexpr size_t kObjRadarState = 0x970, kObjRadarStage = 0x974, kObjRadarTarget = 0x988, kObjStateWord = 0x5f0;
constexpr size_t kObjId = 0x08, kObjDetect = 0x24, kObjData = 0x380, kDataTrackFreq = 0x920, kDataCw = 0x92a;
constexpr DWORD kRvaAoDetectVtable = 0x94374, kRvaAoSetMode = 0x40ca0, kRvaAoSetTarget = 0x40cd0, kRvaAoRwr = 0x41dc0;
constexpr size_t kDetectSetMode = 20, kDetectSetTarget = 21, kDetectRwr = 22;

struct OwnJet {
    int state = 0, stage = 0;
    uint32_t target_id = 0xffffffffu;
    uint32_t word = 0;
};
bool read_own_jet(const BYTE* obj, OwnJet* j) {
    __try {
        std::memcpy(&j->state, obj + kObjRadarState, 4);
        std::memcpy(&j->stage, obj + kObjRadarStage, 4);
        const BYTE* t = *reinterpret_cast<const BYTE* const*>(obj + kObjRadarTarget);
        j->target_id = t ? *reinterpret_cast<const uint32_t*>(t + kObjId) : 0xffffffffu;
        std::memcpy(&j->word, obj + kObjStateWord, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The stable key (aircraft slot and member) of an object id, from the mission's slot table.
bool key_of_id(const SlotTable& t, uint32_t id, uint16_t* a, uint16_t* b) {
    if (id == 0xffffffffu) return false;
    for (int i = 0; i < t.count && i < t.first_ground; ++i) {
        SlotRaw s{};
        if (!read_slot(t, i, &s)) return false;
        for (int m = 0; m < s.members; ++m)
            if (s.ids[m] == id) {
                *a = static_cast<uint16_t>(i);
                *b = static_cast<uint16_t>(m);
                return true;
            }
    }
    return false;
}

// 0 done, 1 an interface or slot is not the one we read, 2 faulted.
int detect_call(BYTE* obj, uintptr_t ao, size_t slot, DWORD rva, int arg) {
    __try {
        BYTE* iface = obj + kObjDetect;
        void** vt = *reinterpret_cast<void***>(iface);
        if (reinterpret_cast<uintptr_t>(vt) != ao + kRvaAoDetectVtable ||
            reinterpret_cast<uintptr_t>(vt[slot]) != ao + rva)
            return 1;
        reinterpret_cast<void(__fastcall*)(void*, void*, int)>(vt[slot])(iface, nullptr, arg);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

// The target's RWR hears the shooter's lock (what the game's own radar update does once per lock, skipped for a
// REMOTEOBJECT shooter). 0 done, 1 refused, 2 faulted.
int rwr_lock(BYTE* target, const BYTE* shooter, uintptr_t ao) {
    __try {
        const uint32_t shooter_id = *reinterpret_cast<const uint32_t*>(shooter + kObjId);
        const BYTE* data = *reinterpret_cast<const BYTE* const*>(shooter + kObjData);
        if (!data) return 1;
        float freq = 0;
        int16_t cw = 0;
        std::memcpy(&freq, data + kDataTrackFreq, 4);
        std::memcpy(&cw, data + kDataCw, 2);
        BYTE* iface = target + kObjDetect;
        void** vt = *reinterpret_cast<void***>(iface);
        if (reinterpret_cast<uintptr_t>(vt) != ao + kRvaAoDetectVtable ||
            reinterpret_cast<uintptr_t>(vt[kDetectRwr]) != ao + kRvaAoRwr)
            return 1;
        reinterpret_cast<void(__fastcall*)(void*, void*, uint32_t, int, float, int)>(vt[kDetectRwr])(
            iface, nullptr, shooter_id, 3, freq, cw);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

bool write_config(BYTE* obj, uint16_t config) {
    __try {
        uint32_t w = 0;
        std::memcpy(&w, obj + kObjStateWord, 4);
        w = (w & ~static_cast<uint32_t>(net::kAvatarConfigMask)) | config;
        std::memcpy(obj + kObjStateWord, &w, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct AvatarTx {
    net::AvatarStateMsg last{};
    bool have = false;
    uint16_t seq = 0;
    uint32_t epoch = 0;
    double sent_at = 0;  // game time of the last send
};
AvatarTx g_atx;  // game thread
net::AvatarStateMsg g_arx[1 + 4] = {};  // per peer: the last state applied (host 0, clients 1..4)
bool g_arx_have[1 + 4] = {};            // ... and whether one came this flight (its controls are kept on the avatar)
uint32_t g_arx_epoch = 0;
uintptr_t g_arx_ao = 0;  // AircraftObject, checked once per player epoch

bool target_alive(const BYTE* obj) {
    __try {
        int level = 0;
        std::memcpy(&level, obj + kObjDamage, 4);
        return level < kDestroyedLevel;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void world_avatar_tick(void* player, uint16_t own_peer, AvatarSendFn send) {
    if (!player || !send) return;
    HMODULE sm = GetModuleHandleW(L"SingleMission.dll");
    if (!sm) return;
    SlotTable t{};
    OwnJet j;
    if (!read_slot_table(at_rva(sm, kRvaMsn), &t) || !read_own_jet(static_cast<const BYTE*>(player), &j)) return;
    if (g_atx.epoch != sim_player_epoch()) g_atx = AvatarTx{{}, false, 0, sim_player_epoch(), 0};
    net::AvatarStateMsg m{};
    m.peer = own_peer;
    uint16_t a = 0, b = 0;
    const bool keyed = key_of_id(t, j.target_id, &a, &b);
    m.radar_mode = net::radar_mode_of(j.state, j.stage, keyed);
    if (keyed && m.radar_mode >= 2) {
        m.target_kind = static_cast<uint8_t>(net::KeyKind::Aircraft);
        m.target_a = a;
        m.target_b = b;
    }
    m.config = static_cast<uint16_t>(j.word & net::kAvatarConfigMask);
    JetControls jc;
    if (jet_read_controls(player, &jc)) {
        m.throttle = net::throttle_to_wire(jc.throttle);
        m.engines = jc.engines;
        m.engine_count = jc.engine_count;
        m.guns = jc.guns;
    } else if (g_atx.have) {
        m.throttle = g_atx.last.throttle;
        m.engines = g_atx.last.engines;
        m.engine_count = g_atx.last.engine_count;
    }  // else engine count 0: no avatar's engines are switched
    const double now = sim_game_time();
    if (g_atx.have && !net::avatar_state_due(g_atx.last, m, now - g_atx.sent_at)) return;
    m.seq = ++g_atx.seq;
    uint8_t buf[sizeof(m)];
    const size_t n = net::encode(m, buf, sizeof(buf));
    if (!n || !send(buf, n)) return;  // tried again next step
    if (!g_atx.have || m.radar_mode != g_atx.last.radar_mode || m.target_a != g_atx.last.target_a ||
        m.target_b != g_atx.last.target_b || m.engines != g_atx.last.engines || m.guns != g_atx.last.guns)
        LOG("world: jet state: radar mode %u, target %s%03u.%02u, config 0x%04x, throttle %u/255, engines 0x%02x, guns %u "
            "(sent)",
            static_cast<unsigned>(m.radar_mode), m.target_kind ? "A " : "none ", static_cast<unsigned>(m.target_a),
            static_cast<unsigned>(m.target_b), static_cast<unsigned>(m.config), static_cast<unsigned>(m.throttle),
            static_cast<unsigned>(m.engines), static_cast<unsigned>(m.guns));
    g_atx.last = m;
    g_atx.have = true;
    g_atx.sent_at = now;
}

void world_avatar_forget(int peer) {
    if (peer >= 0 && peer < 5) {
        g_arx[peer] = net::AvatarStateMsg{};
        g_arx_have[peer] = false;
    }
}

void world_avatar_session_reset() {
    g_atx = AvatarTx{{}, false, 0, sim_player_epoch(), 0};  // our state goes out again
    for (auto& p : g_arx) p = net::AvatarStateMsg{};
    for (bool& h : g_arx_have) h = false;
    hits_session_reset();
}

namespace {
JetControls controls_of(const net::AvatarStateMsg& m) {
    JetControls c;
    c.throttle = net::throttle_from_wire(m.throttle);
    c.engines = m.engines;
    c.engine_count = m.engine_count > 8 ? 8 : m.engine_count;
    c.guns = m.guns & 3;
    return c;
}
}  // namespace

void world_avatar_refresh(int peer, void* avatar) {
    if (peer < 0 || peer >= 5 || !avatar || !g_arx_have[peer] || g_arx_epoch != sim_player_epoch()) return;
    jet_write_controls(avatar, controls_of(g_arx[peer]));
}

void world_avatar_apply(const uint8_t* data, size_t len, void* avatar) {
    net::AvatarStateMsg m;
    if (net::decode(data, len, m) != net::Status::Ok || m.peer > 4 || !avatar) return;
    if (g_arx_epoch != sim_player_epoch()) {  // a new flight: nothing applied yet
        g_arx_epoch = sim_player_epoch();
        for (auto& p : g_arx) p = net::AvatarStateMsg{};
        for (bool& h : g_arx_have) h = false;
        g_arx_ao = module_if(L"AircraftObject.dll", kAoStamp, kAoSize, "AircraftObject.dll");
    }
    net::AvatarStateMsg& prev = g_arx[m.peer];
    HMODULE sm = GetModuleHandleW(L"SingleMission.dll");
    HMODULE omm = GetModuleHandleW(L"ObjectManager.dll");
    const uintptr_t ao = g_arx_ao;
    if (!sm || !omm || !ao) return;
    BYTE* av = static_cast<BYTE*>(avatar);
    write_config(av, m.config);
    const JetWriteResult jw = jet_write_controls(av, controls_of(m));
    if (!g_arx_have[m.peer] || m.engines != prev.engines || m.guns != prev.guns || jw.engines_set ||
        jw.engines_refused)
        LOG("world: jet state of peer %u: throttle %u/255, engines 0x%02x, guns %u%s (%u switched%s)",
            static_cast<unsigned>(m.peer), static_cast<unsigned>(m.throttle), static_cast<unsigned>(m.engines),
            static_cast<unsigned>(m.guns),
            jw.ok ? "" : ", NOT written (input array not readable)", static_cast<unsigned>(std::popcount(jw.engines_set)),
            jw.engines_refused ? ", some engines' class not the one we read" : "");
    const int rm = detect_call(av, ao, kDetectSetMode, kRvaAoSetMode, m.radar_mode);
    SlotTable t{};
    Om om;
    uint32_t tid = 0xffffffffu;
    BYTE* tobj = nullptr;
    int tg = 0;
    const bool have_target = m.target_kind == static_cast<uint8_t>(net::KeyKind::Aircraft) &&
                             read_slot_table(at_rva(sm, kRvaMsn), &t) && read_om(omm, &om) &&
                             read_member_id(t, m.target_a, m.target_b, &tid) && resolve_obj(om, tid, &tobj, &tg) &&
                             tg == 1;
    int rt = 0, rw = -1;
    if (have_target) rt = detect_call(av, ao, kDetectSetTarget, kRvaAoSetTarget, static_cast<int>(tid));
    if (have_target && net::avatar_lock_started(prev, m) && target_alive(tobj)) rw = rwr_lock(tobj, av, ao);
    if (m.radar_mode != prev.radar_mode || m.target_a != prev.target_a || m.target_b != prev.target_b)
        LOG("world: jet state of peer %u: radar mode %u%s, target %s (%s%s)", static_cast<unsigned>(m.peer),
            static_cast<unsigned>(m.radar_mode), rm == 0 ? "" : rm == 1 ? " NOT set (unexpected slot)" : " FAULTED",
            have_target ? "set" : m.target_kind ? "not found here" : "none",
            rw < 0 ? "no RWR call" : rw == 0 ? "the target's RWR warned" : rw == 1 ? "RWR refused" : "RWR FAULTED",
            rt == 0 ? "" : ", target slot unexpected");
    prev = m;
    g_arx_have[m.peer] = true;
}

// ---- keys and damage for the hit claims (Phase 5 T3, hit_claims.cpp) ----

namespace {

// The running mission's slot table and object manager. Game thread.
bool world_tables(SlotTable* t, Om* om, uint32_t* player) {
    HMODULE sm = GetModuleHandleW(L"SingleMission.dll");
    HMODULE omm = GetModuleHandleW(L"ObjectManager.dll");
    if (!sm || !omm) return false;
    const void* msn = at_rva(sm, kRvaMsn);
    return read_slot_table(msn, t) && read_om(omm, om) && (!player || read_player_obj(msn, player));
}

// The damage interface functions of slots 4..6 per class module (the ones the damage block hooks; network-recon §9.5).
struct DmgModule {
    const wchar_t* name;
    DWORD stamp, size;
    DWORD rva[3];
};
const DmgModule kDmgModules[4] = {
    {L"AircraftObject.dll", kAoStamp, kAoSize, {0x69210, 0x693d0, 0x692f0}},
    {L"GroundObject.dll", kGoStamp, kGoSize, {0x18e60, 0x14780, 0x146a0}},
    {L"GroundMobileObject.dll", kGmStamp, kGmSize, {0x11500, 0xbf70, 0xbe90}},
    {L"NavalObject.dll", kNvStamp, kNvSize, {0x35d0, 0x3700, 0x3660}},
};
uintptr_t g_dmg_base[4] = {};
HMODULE g_dmg_checked[4] = {};

// The function slot `slot` (4..6) of the object's damage interface must be one we read; the hook's trampoline is
// returned when it is hooked, so an applied hit does not run through the damage block and the claims again.
// Game thread.
void* checked_damage_fn(void** vt, int slot) {
    void* f = vt[slot];
    for (int i = 0; i < 4; ++i) {
        HMODULE m = GetModuleHandleW(kDmgModules[i].name);
        if (m != g_dmg_checked[i]) {
            g_dmg_checked[i] = m;
            g_dmg_base[i] = m && is_expected_build(m, kDmgModules[i].stamp, kDmgModules[i].size, "damage interface")
                                ? reinterpret_cast<uintptr_t>(m)
                                : 0;
        }
        if (g_dmg_base[i] && reinterpret_cast<uintptr_t>(f) == g_dmg_base[i] + kDmgModules[i].rva[slot - 4]) {
            void* tramp = g_dmg_orig[i * 3 + (slot - 4)];
            return tramp ? tramp : f;
        }
    }
    return nullptr;
}

}  // namespace

bool world_remote_on() { return g_remote_on.load(std::memory_order_acquire); }

bool world_remote_deferred() { return g_remote_deferred != nullptr; }

int world_destroy_object(void* obj) { return obj ? destroy_copy(static_cast<BYTE*>(obj)) : 1; }

int world_log_kill(void* obj, uint32_t killer, uint32_t weapon) {
    if (!obj) return 1;
    struct K {
        const wchar_t* name;
        DWORD stamp, size, rva;
    };
    static const K kKill[4] = {{L"AircraftObject.dll", kAoStamp, kAoSize, 0x42ac0},
                               {L"GroundObject.dll", kGoStamp, kGoSize, 0x15aa0},
                               {L"GroundMobileObject.dll", kGmStamp, kGmSize, 0xd290},
                               {L"NavalObject.dll", kNvStamp, kNvSize, 0x12620}};
    constexpr size_t kSlotKill = 30;
    uintptr_t fn = 0;
    __try {
        fn = reinterpret_cast<uintptr_t>((*static_cast<void***>(obj))[kSlotKill]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
    bool known = false;
    for (const K& k : kKill) {
        const uintptr_t base = module_if(k.name, k.stamp, k.size, "kill event");
        known = known || (base && fn == base + k.rva);
    }
    if (!known) return 1;
    __try {
        BYTE* o = static_cast<BYTE*>(obj);
        std::memcpy(o + 0x318, &killer, 4);
        std::memcpy(o + 0x31c, &weapon, 4);
        reinterpret_cast<void(__fastcall*)(void*)>(fn)(obj);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

void world_remote_now() {
    void* msn = g_remote_deferred;
    g_remote_deferred = nullptr;
    if (msn) remote_after_start(msn);
}

namespace {
bool world_tables(SlotTable* t, Om* om, uint32_t* player);  // below, with the keys
// Clears REMOTEOBJECT on one aircraft or ground object being handed back to its AI, with the fix-ups of
// network-recon §12.1.3: an aircraft on the ground loses the airborne bit the remote state forced, its ripple timer
// and rounds (+0x8f4, +0x8e6) and its trigger inputs are zeroed. SEH only. 1 cleared, 0 not remote, -1 faulted.
int hand_back_one(BYTE* obj, uint32_t mask, bool aircraft) {
    __try {
        uint32_t f = 0;
        std::memcpy(&f, obj + kObjFlags, 4);
        if (!(f & mask)) return 0;
        if (aircraft) {
            uint32_t state = 0;
            float height = 0, gear = 0, v[3] = {};
            std::memcpy(&state, obj + 0x5f0, 4);
            std::memcpy(&height, obj + 0x620, 4);
            std::memcpy(&gear, obj + 0x2fc, 4);
            std::memcpy(v, obj + kObjVel, sizeof(v));
            const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (copy_on_ground(height, gear, speed)) state &= ~0x800000u;
            std::memcpy(obj + 0x5f0, &state, 4);
            const float zero = 0;
            const int16_t zs = 0;
            std::memcpy(obj + 0x8f4, &zero, 4);
            std::memcpy(obj + 0x8e6, &zs, 2);
            float* in = *reinterpret_cast<float**>(obj + 0x5f8);
            if (in) in[12] = in[13] = in[14] = 0.0f;
        }
        f &= ~mask;
        std::memcpy(obj + kObjFlags, &f, 4);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
}  // namespace

int world_set_remote_all(bool remote) {
    HMODULE sm = GetModuleHandleW(L"SingleMission.dll");
    if (!sm) return -1;
    void* msn = at_rva(sm, kRvaMsn);
    if (remote) {
        remote_after_start(msn);
        return g_remote_on.load() ? 1 : -1;
    }
    uint32_t air = 0, ground = 0;
    if (!read_mask(L"AircraftObject.dll", kAoStamp, kAoSize, "AircraftObject.dll", kRvaAoRemoteMask, &air) ||
        !read_mask(L"GroundObject.dll", kGoStamp, kGoSize, "GroundObject.dll", kRvaGoRemoteMask, &ground))
        return -1;
    SlotTable t{};
    Om om;
    uint32_t player = 0xffffffffu;
    if (!world_tables(&t, &om, &player)) return -1;
    g_remote_on = false;  // our world is local again: its damage applies here
    int n = 0;
    for (int i = 0; i < t.count; ++i) {
        SlotRaw s{};
        if (!read_slot(t, i, &s)) break;
        for (int m = 0; m < s.members; ++m) {
            BYTE* obj = nullptr;
            int group = 0;
            if (s.ids[m] == player || !resolve_obj(om, s.ids[m], &obj, &group) || group == 0) continue;
            if (host_drives_object(obj) || client_drives_object(obj)) continue;  // players' avatars stay remote
            n += hand_back_one(obj, group == 1 ? air : ground, group == 1) == 1;
        }
    }
    const BYTE* areas = nullptr;
    int n_areas = 0;
    if (read_areas_head(msn, &areas, &n_areas)) {
        for (int i = 0; i < n_areas; ++i) {
            AreaRaw ar{};
            if (!read_area(areas, i, &ar)) break;
            for (int j = 0; j < ar.n_placed; ++j) {
                PlacedRaw p{};
                if (read_placed(ar, j, &p) && p.obj) n += hand_back_one(static_cast<BYTE*>(const_cast<void*>(p.obj)), ground, false) == 1;
            }
        }
    }
    return n;
}

void world_for_each_object(void (*cb)(uint8_t kind, uint16_t a, uint16_t b, void* obj, void* ctx), void* ctx) {
    SlotTable t{};
    Om om;
    uint32_t player = 0xffffffffu;
    if (!cb || !world_tables(&t, &om, &player)) return;
    for (int i = 0; i < t.count; ++i) {
        SlotRaw s{};
        if (!read_slot(t, i, &s)) break;
        for (int m = 0; m < s.members; ++m) {
            BYTE* obj = nullptr;
            int group = 0;
            if (s.ids[m] == player || !resolve_obj(om, s.ids[m], &obj, &group) || group == 0) continue;
            if (host_drives_object(obj) || client_drives_object(obj)) continue;
            cb(static_cast<uint8_t>(i < t.first_ground ? 1 : 2), static_cast<uint16_t>(i), static_cast<uint16_t>(m), obj, ctx);
        }
    }
}

int world_flight_count() {
    SlotTable t{};
    Om om;
    return world_tables(&t, &om, nullptr) ? t.first_ground : 0;
}

bool world_flight_waypoint(int slot, int* wp, bool set, int value) {
    SlotTable t{};
    Om om;
    if (!world_tables(&t, &om, nullptr) || slot < 0 || slot >= t.first_ground) return false;
    const uintptr_t af = module_if(L"AircraftFormation.dll", kAfStamp, kAfSize, "AircraftFormation.dll");
    SlotRaw sr{};
    BYTE* fo = nullptr;
    int fg = 0, count = 0;
    if (!af || !read_slot(t, slot, &sr) || sr.formation == 0xffffffffu || !resolve_obj(om, sr.formation, &fo, &fg))
        return false;
    BYTE* ai = formation_ai(fo, af);
    if (!ai || !read_waypoint(ai, wp, &count)) return false;
    if (set && value != *wp) return set_waypoint(ai, af, value) == 0;
    return true;
}

namespace {
constexpr DWORD kRvaMsnSetClock = 0xa160;  // MSN slot 19: thiscall(float), writes +0x10
constexpr size_t kMsnClock = 0x10, kMsnClock2 = 0x14, kMsnSetClockSlot = 19;
uintptr_t sm_base_checked() {
    static HMODULE seen = nullptr;
    static uintptr_t base = 0;
    HMODULE m = GetModuleHandleW(L"SingleMission.dll");
    if (m != seen) {
        seen = m;
        base = m && is_expected_build(m, kSmStamp, kSmSize, "SingleMission.dll (clock)") ? reinterpret_cast<uintptr_t>(m) : 0;
    }
    return base;
}
}  // namespace

bool world_mission_clock(float* t10, float* t14) {
    const uintptr_t sm = sm_base_checked();
    if (!sm) return false;
    __try {
        const BYTE* msn = reinterpret_cast<const BYTE*>(sm + kRvaMsn);
        if (*reinterpret_cast<const uintptr_t*>(msn) != sm + kRvaMsnVtable) return false;
        std::memcpy(t10, msn + kMsnClock, 4);
        std::memcpy(t14, msn + kMsnClock2, 4);
        return std::isfinite(*t10) && std::isfinite(*t14);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int world_set_mission_clock(float t10, float t14) {
    const uintptr_t sm = sm_base_checked();
    if (!sm || !std::isfinite(t10) || !std::isfinite(t14)) return 1;
    __try {
        BYTE* msn = reinterpret_cast<BYTE*>(sm + kRvaMsn);
        void** vt = *reinterpret_cast<void***>(msn);
        if (reinterpret_cast<uintptr_t>(vt) != sm + kRvaMsnVtable ||
            reinterpret_cast<uintptr_t>(vt[kMsnSetClockSlot]) != sm + kRvaMsnSetClock)
            return 1;
        reinterpret_cast<void(__fastcall*)(void*, void*, float)>(vt[kMsnSetClockSlot])(msn, nullptr, t10);
        std::memcpy(msn + kMsnClock2, &t14, 4);  // the second clock has no setter
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}
uint32_t world_remote_mask() { return g_mask_air.load(std::memory_order_relaxed); }
uint32_t world_remote_mask_ground() { return g_mask_ground.load(std::memory_order_relaxed); }

bool world_key_of_object(uint32_t id, uint8_t* kind, uint16_t* a, uint16_t* b) {
    SlotTable t{};
    Om om;
    if (id == 0xffffffffu || !world_tables(&t, &om, nullptr)) return false;
    for (int i = 0; i < t.count; ++i) {
        SlotRaw s{};
        if (!read_slot(t, i, &s)) return false;
        for (int m = 0; m < s.members; ++m)
            if (s.ids[m] == id) {
                *kind = static_cast<uint8_t>(i < t.first_ground ? net::KeyKind::Aircraft : net::KeyKind::Ground);
                *a = static_cast<uint16_t>(i);
                *b = static_cast<uint16_t>(m);
                return true;
            }
    }
    return false;
}

void* world_object_of_key(uint8_t kind, uint16_t a, uint16_t b, uint32_t* id_out) {
    SlotTable t{};
    Om om;
    if ((kind != 1 && kind != 2) || !world_tables(&t, &om, nullptr)) return nullptr;
    if (a >= t.count || (kind == 1) != (a < t.first_ground)) return nullptr;
    SlotRaw s{};
    uint32_t id = 0;
    BYTE* obj = nullptr;
    int group = 0;
    if (!read_slot(t, a, &s) || b >= s.members || !read_member_id(t, a, b, &id) || !resolve_obj(om, id, &obj, &group) ||
        group == 0)
        return nullptr;
    if (id_out) *id_out = id;
    return obj;
}

void* world_player_object() {
    SlotTable t{};
    Om om;
    uint32_t player = 0xffffffffu;
    BYTE* obj = nullptr;
    int group = 0;
    return world_tables(&t, &om, &player) && resolve_obj(om, player, &obj, &group) && group == 1 ? obj : nullptr;
}

uint32_t world_player_id() {
    SlotTable t{};
    Om om;
    uint32_t player = 0xffffffffu;
    return world_tables(&t, &om, &player) ? player : 0xffffffffu;
}

bool world_object_state(const void* obj, double pos[3], uint32_t* id, bool* alive) {
    __try {
        const BYTE* o = static_cast<const BYTE*>(obj);
        std::memcpy(pos, o + kObjPos, 3 * sizeof(double));
        std::memcpy(id, o + kObjId, 4);
        int level = 0;
        std::memcpy(&level, o + kObjDamage, 4);
        *alive = level < kDestroyedLevel;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int world_damage_call(void* obj, int slot, const uint32_t* args) {
    if (!obj || slot < 4 || slot > 6) return 1;
    __try {
        BYTE* iface = static_cast<BYTE*>(obj) + kDamageIface;
        void* f = checked_damage_fn(*reinterpret_cast<void***>(iface), slot);
        if (!f) return 1;
        if (slot == 4)
            reinterpret_cast<Dmg5_t>(f)(iface, nullptr, args[0], args[1], args[2], args[3], args[4]);
        else if (slot == 5)
            reinterpret_cast<Dmg8_t>(f)(iface, nullptr, args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                                        args[7]);
        else
            reinterpret_cast<Dmg6_t>(f)(iface, nullptr, args[0], args[1], args[2], args[3], args[4], args[5]);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

namespace {

// ---- hook installation, per image ----

struct HookDef {
    DWORD rva;
    void* detour;
    void** original;
    const char* what;
    unsigned char prologue[9];  // the first bytes of the stock function (prologue_len of them)
    size_t prologue_len;
};

// Self-check before hooking: the function must start with the bytes the decompile was read from. Logs them once.
bool prologue_ok(HMODULE m, const HookDef& d) {
    const BYTE* f = static_cast<const BYTE*>(at_rva(m, d.rva));
    char hex[3 * sizeof(d.prologue) + 1] = {};
    for (size_t i = 0; i < d.prologue_len; ++i) std::snprintf(hex + 3 * i, 4, "%02x ", f[i]);
    const bool ok = std::memcmp(f, d.prologue, d.prologue_len) == 0;
    LOG("%s: prologue at +0x%lx is %s%s", d.what, static_cast<unsigned long>(d.rva), hex,
        ok ? "(as expected)" : "(NOT as expected: not hooked)");
    return ok;
}

// Hooks a freshly loaded image once. The DLLs can be unloaded and loaded again, so each new image is re-hooked.
void hook_image(const wchar_t* name, const char* label, DWORD stamp, DWORD size, const HookDef* defs, size_t n,
                HMODULE& current, HMODULE& bad) {
    HMODULE m = GetModuleHandleW(name);
    if (!m || m == bad) return;
    if (m == current && *static_cast<const BYTE*>(at_rva(m, defs[0].rva)) == 0xE9) return;
    AcquireSRWLockExclusive(&g_hook_lock);
    if (!(m == current && *static_cast<const BYTE*>(at_rva(m, defs[0].rva)) == 0xE9) && m != bad) {
        if (!is_expected_build(m, stamp, size, label)) {
            bad = m;
            LOG("world: %s not hooked; the world tooling stays off for it", label);
        } else {
            // Entries left by an earlier image at this base: removing them restores identical bytes (same build).
            for (size_t i = 0; i < n; ++i) (void)MH_RemoveHook(at_rva(m, defs[i].rva));
            bool all = true;
            for (size_t i = 0; i < n; ++i) all = prologue_ok(m, defs[i]) && all;
            if (all) {
                for (size_t i = 0; i < n; ++i)
                    create_and_enable(at_rva(m, defs[i].rva), defs[i].detour, defs[i].original, defs[i].what);
            } else {
                bad = m;
                LOG("world: %s not hooked (prologue check); the world tooling stays off for it", label);
            }
            current = m;
        }
    }
    ReleaseSRWLockExclusive(&g_hook_lock);
}

}  // namespace

void install_save_load();

void world_install() {
    if (!probe_output_dir()) {
        if (env_set(L"LAZYSF2_WORLD_SAVE") || env_set(L"LAZYSF2_WORLD_LOAD") || env_set(L"LAZYSF2_WORLD_SEAT") ||
            env_set(L"LAZYSF2_WORLD_REMOTE"))
            LOG("world: off (LAZYSF2_WORLD_* need LAZYSF2_PROBE_DIR)");
        return;
    }
    wchar_t seat[16] = {};
    if (GetEnvironmentVariableW(L"LAZYSF2_WORLD_SEAT", seat, 16) > 0) {
        g_seat = parse_seat(seat);
        g_seat_from_env = g_seat >= 0;
        if (g_seat < 0)
            LOG("world: LAZYSF2_WORLD_SEAT is not a member index 1..47; stock seat");
        else
            LOG("world: seat mode: the player takes member %d (x-%d) of the player flight in single missions", g_seat,
                g_seat + 1);
    }
    if (env_set(L"LAZYSF2_WORLD_REMOTE")) {
        g_remote = true;
        LOG("world: remote mode: after each single mission start every object but the player's is made REMOTEOBJECT, "
            "and local damage to REMOTEOBJECT objects is blocked");
    }
    install_save_load();
    if (env_set(L"LAZYSF2_WORLD_JOIN")) {
        char host[64] = {};
        uint16_t port = 0;
        if (g_mode != Mode::Off) {
            LOG("world: join off (LAZYSF2_WORLD_SAVE or LAZYSF2_WORLD_LOAD is set too)");
        } else if (!client_connect_target(host, sizeof(host), &port)) {
            LOG("world: join off (it needs client mode: LAZYSF2_CONNECT)");
        } else {
            g_join = true;
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            g_tag = (static_cast<uint32_t>(qpc.QuadPart) ^ static_cast<uint32_t>(qpc.QuadPart >> 32) ^
                     (GetCurrentProcessId() << 12)) | 1u;  // never 0
            g_mode = Mode::Load;  // the fetched file is loaded at the New page's Accept, as for LAZYSF2_WORLD_LOAD
            if (g_seat < 0) g_seat = 1;  // until the host suggests one
            g_remote = true;  // the host owns the world: every other object is a remote copy (T4), driven by the stream
            LOG("world: join mode: the host's mission is fetched from %s:%u in menus and flown at the next Accept",
                host, static_cast<unsigned>(port));
        }
    }
}

void install_save_load() {
    const bool save = env_set(L"LAZYSF2_WORLD_SAVE");
    wchar_t load[MAX_PATH] = {};
    const DWORD ln = GetEnvironmentVariableW(L"LAZYSF2_WORLD_LOAD", load, MAX_PATH);
    if (!save && ln == 0) return;
    if (save && ln > 0) {
        LOG("world: off (LAZYSF2_WORLD_SAVE and LAZYSF2_WORLD_LOAD are both set)");
        return;
    }
    wchar_t seed[16] = {};
    if (GetEnvironmentVariableW(L"LAZYSF2_WORLD_SEED", seed, 16) > 0) {
        wchar_t* end = nullptr;
        const unsigned long v = std::wcstoul(seed, &end, 10);
        if (end && !*end && v <= 0x7fffffffUL) {
            g_seed_from_env = true;
            g_env_seed = static_cast<uint32_t>(v);
        } else {
            LOG("world: LAZYSF2_WORLD_SEED is not a number 0..2147483647; a random seed is used");
        }
    }
    if (save) {
        g_mode = Mode::Save;
        LOG("world: save mode: each New-page Accept saves the mission to %ls\\world_NNN.msn and runs from it (seed %s)",
            probe_output_dir(), g_seed_from_env ? "from LAZYSF2_WORLD_SEED" : "random");
        return;
    }
    wchar_t full[MAX_PATH] = {};
    const DWORD fn = ln < MAX_PATH ? GetFullPathNameW(load, MAX_PATH, full, nullptr) : 0;
    if (fn == 0 || fn >= MAX_PATH) {
        LOG("world: off (LAZYSF2_WORLD_LOAD is not a usable path)");
        return;
    }
    g_load_path = full;
    g_mode = Mode::Load;
    LOG("world: load mode: each New-page Accept loads %ls with its sidecar's seed and options", full);
}

namespace {
void fetch_log(const char* line) { LOG("%s", line); }

void start_fetch() {
    char host[64] = {};
    uint16_t port = 0;
    if (!g_fetch) g_fetch = new (std::nothrow) net::MissionFetch;
    if (g_fetch && g_lobby_hash) g_fetch->enable_lobby(g_lobby_callsign, g_lobby_hash);  // a lobby session: 1 s rounds
    net::AuthClient auth;
    if (!g_fetch || !client_connect_target(host, sizeof(host), &port) || !client_auth(&auth) ||
        !g_fetch->start(host, port, g_tag, auth, fetch_log, g_lobby_hash ? 1000 : 5000))
        LOG("world: join: could not start the mission fetch");
}

uint32_t new_tag() {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    return (static_cast<uint32_t>(qpc.QuadPart) ^ static_cast<uint32_t>(qpc.QuadPart >> 32) ^
            (GetCurrentProcessId() << 12)) | 1u;  // never 0
}
}  // namespace

bool world_session_begin(bool host, const std::wstring& dir, const char* callsign, uint32_t content_hash) {
    if (g_mode != Mode::Off || g_join || g_remote || g_seat >= 0) {
        LOG("world: a session is already configured (LAZYSF2_WORLD_* or an earlier session); not changed");
        return false;
    }
    g_session_dir = dir;
    g_seed_from_env = false;
    g_load_path.clear();
    g_fetched_id = g_flying_id = 0;
    g_host_seat = 0;
    if (host) {
        g_mode = Mode::Save;
        LOG("world: session: each New- or Load-page Accept saves the mission to %ls and serves it", dir.c_str());
    } else {
        g_join = true;
        g_tag = new_tag();
        std::snprintf(g_lobby_callsign, sizeof(g_lobby_callsign), "%s", callsign ? callsign : "");
        g_lobby_hash = content_hash;
        g_mode = Mode::Load;
        g_seat = 1;  // until the host suggests one
        g_remote = true;
        start_fetch();
        LOG("world: session: the host's mission is fetched in menus and flown at the next Accept");
    }
    world_on_modules_loaded();  // hooks the images already loaded for what this mode needs
    return true;
}

void world_session_end() {
    if (net::MissionFetch* f = g_fetch) {
        g_fetch = nullptr;
        // Off the menu thread: the fetch tells the host this pilot left (the host drops its row at once), then stops
        // and joins its thread, which never waits for the resolver. A fetch cannot be restarted, so the next session
        // makes a new one.
        try {
            std::thread([f] {
                f->leave(600);
                delete f;
            }).detach();
        } catch (...) {
            delete f;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g_armed = Armed{};  // an Accept in the session that was never flown: its seed and files do not reach a later mission
    ReleaseSRWLockExclusive(&g_lock);
    g_remote_deferred = nullptr;
    g_world_host = false;
    g_published_id = 0;
    g_mode = Mode::Off;
    g_join = false;
    g_remote = false;
    g_remote_on = false;  // our world is local again (as after a handover takeover)
    g_seat = -1;
    g_tag = 0;
    g_load_path.clear();
    g_fetched_id = g_flying_id = 0;
    g_session_dir.clear();
    g_lobby_hash = 0;
    world_set_own_seat(-1);
    LOG("world: session ended; Single Mission is stock again");
}

uint32_t world_client_tag() { return g_join ? g_tag : 0; }

bool world_fetch_wrong_password() { return g_fetch && g_fetch->wrong_password(); }

bool world_lobby_roster(net::LobbyRosterMsg* out, uint32_t* rtt_ms) { return g_fetch && g_fetch->roster(out, rtt_ms); }
void world_lobby_ready(bool ready, uint32_t mission_id) {
    if (g_fetch) g_fetch->set_ready(ready, mission_id);
}
bool world_lobby_wish_answer(int* seat, uint8_t* result) { return g_fetch && g_fetch->wish_answer(seat, result); }
void world_lobby_want_seat(int seat) {
    if (g_fetch) g_fetch->want_seat(seat);
}

bool world_host_armed() {
    AcquireSRWLockShared(&g_lock);
    const bool armed = g_mode == Mode::Save && g_armed.on;
    ReleaseSRWLockShared(&g_lock);
    return armed;
}

int world_lobby_fetched(std::string* theatre) {
    if (!g_join) return 3;
    take_fetched_mission();
    if (g_load_path.empty()) return 1;
    std::string text, err;
    WorldSidecar s;
    if (!read_text(world_companion_path(g_load_path, L".sidecar.txt"), &text) || !parse_sidecar(text, &s, &err) ||
        s.theatre.empty())
        return 3;
    *theatre = s.theatre;
    return theatre_installed(s.theatre) ? 0 : 2;
}

uint32_t world_lobby_fetched_id() { return g_load_path.empty() ? 0 : g_fetched_id; }

bool world_lobby_load() {
    Ctx c{};
    if (!g_join || g_load_path.empty() || !resolve(&c, "lobby briefing")) return false;
    accept_load(c, nullptr, false);  // the theatre is selected already (no New page here)
    apply_lobby_seat();               // the seat this pilot picked in the lobby, after any new offer's
    AcquireSRWLockExclusive(&g_lock);
    const bool armed = g_armed.on && g_armed.msn == c.msn && g_armed.msn_path == g_load_path;
    ReleaseSRWLockExclusive(&g_lock);
    return armed;
}

void world_set_own_seat(int member) {
    if (g_join || g_seat_from_env) return;  // a joined client's seat comes from the host's mission offer
    const int seat = member > 0 ? member : -1;  // member 0 is the mission's own player seat: stock
    if (seat == g_seat) return;
    g_seat = seat;
    LOG("world: this PC's pilot flies member %d of the player flight%s", member < 0 ? 0 : member,
        seat < 0 ? " (the mission's own seat)" : "");
    if (seat >= 0) world_on_modules_loaded();  // the seat hook (SingleMission slot 32)
}
bool world_fetch_host_unknown() { return g_fetch && g_fetch->host_unknown(); }

bool world_client_welcome(uint32_t mission_id, uint8_t seat, uint8_t host_seat) {
    if (!g_join) return true;
    if (g_join_no_theatre) {
        LOG("world: join: the host's mission is in a theatre this PC does not have; leaving (Bye 8). Install the "
            "theatre to join this mission");
        return false;
    }
    if (g_flying_id == 0 || mission_id != g_flying_id) {
        LOG("world: join: the host flies mission 0x%08lx and we fly %s0x%08lx; leaving (Bye 8)",
            static_cast<unsigned long>(mission_id), g_flying_id ? "" : "a generated mission, not ",
            static_cast<unsigned long>(g_flying_id));
        return false;
    }
    if (seat != net::kNoSeat && static_cast<int>(seat) != g_seat) {
        if (g_lobby_hash) {  // the lobby's seats are fixed at Launch: another seat would put two pilots in one jet
            LOG("world: join: the host's Welcome gives seat %u and we fly %d; leaving (Bye 8)", static_cast<unsigned>(seat),
                g_seat);
            return false;
        }
        LOG("world: join: WARNING the host's Welcome gives seat %u, we fly %d", static_cast<unsigned>(seat), g_seat);
    }
    if (host_seat != net::kNoSeat) g_host_seat = host_seat;
    LOG("world: join: welcomed into mission 0x%08lx; we fly member %d, the host member %d",
        static_cast<unsigned long>(mission_id), g_seat, g_host_seat);
    return true;
}

void world_client_session_end() {
    if (!g_join) return;
    // The fetch keeps polling the host: the next Accept flies the host's current mission, or none if it changed
    // and has not arrived yet.
    LOG("world: join: the session ended; the next Accept flies the host's current mission");
}

void world_on_modules_loaded() {
    if (g_join && !g_fetch) start_fetch();
    if (g_mode == Mode::Off && g_seat < 0 && !g_remote) return;
    static const HookDef kSms[] = {
        {kRvaNewEvent, reinterpret_cast<void*>(&h_NewEvent), reinterpret_cast<void**>(&o_NewEvent),
         "world: SingleMissionScreen New page events", {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08}, 6},
        {kRvaLoadEvent, reinterpret_cast<void*>(&h_LoadEvent), reinterpret_cast<void**>(&o_LoadEvent),
         "world: SingleMissionScreen Load page events", {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x81, 0xec, 0x10}, 9},
    };
    static const HookDef kSmAll[] = {
        {kRvaSlot32, reinterpret_cast<void*>(&h_Slot32), reinterpret_cast<void**>(&o_Slot32),
         "world: SingleMission MSN slot 32", {0x55, 0x8b, 0xec, 0x81, 0xec, 0xe4, 0x03, 0x00, 0x00}, 9},
        {kRvaStart, reinterpret_cast<void*>(&h_Start), reinterpret_cast<void**>(&o_Start),
         "world: SingleMission mission start", {0x55, 0x8b, 0xec, 0x81, 0xec, 0x34, 0x02, 0x00, 0x00}, 9},
        {kRvaIniGetInt, reinterpret_cast<void*>(&h_IniGetInt), reinterpret_cast<void**>(&o_IniGetInt),
         "world: SingleMission ini int getter", {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x53}, 7},
    };
    // Only what the switches need: slot 32 for the seat and the remote pass; the start and the getter for save/load.
    const bool slot32 = g_seat >= 0 || g_remote;
    const HookDef* sm_defs = slot32 ? kSmAll : kSmAll + 1;
    const size_t sm_n = (slot32 ? 1 : 0) + (g_mode != Mode::Off ? 2 : 0);
    if (g_mode != Mode::Off)
        hook_image(L"SingleMissionScreen.dll", "SingleMissionScreen.dll", kSmsStamp, kSmsSize, kSms, 2, g_sms,
                   g_sms_bad);
    if (sm_n) hook_image(L"SingleMission.dll", "SingleMission.dll", kSmStamp, kSmSize, sm_defs, sm_n, g_sm, g_sm_bad);
    // The damage hooks: on a joined client (the remote block and the hit claims), and on a host that serves its
    // mission (gun hits on the clients' avatars go to their owners).
    if (!g_remote && !(g_mode == Mode::Save && host_configured())) return;
#define LZ_DMG(K, ARGS, RVA, ...)                                                                         \
    {RVA, reinterpret_cast<void*>(&h_dmg##ARGS<K>), &g_dmg_orig[K], "world: damage block " #K, {__VA_ARGS__}, 9}
    static const HookDef kAo[] = {
        LZ_DMG(0, 5, 0x69210, 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x83, 0xec, 0x40),
        LZ_DMG(1, 8, 0x693d0, 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x83, 0xec, 0x4c),
        LZ_DMG(2, 6, 0x692f0, 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x83, 0xec, 0x40),
        {0x42ac0, reinterpret_cast<void*>(&h_aircraft_kill_event), &g_o_aircraft_kill_event,
         "world: aircraft kill event (remote copies on a client)", {0x55, 0x8b, 0xec, 0x83, 0xec, 0x38, 0x56, 0x8b, 0xf1}, 9},
        {0x40d40, reinterpret_cast<void*>(&h_incoming_missile), &g_o_incoming_missile,
         "world: incoming-missile notice (avatars' auto-dispense)", {0x55, 0x8b, 0xec, 0x83, 0xec, 0x14, 0x56, 0x57, 0x8b}, 9},
        {0x48dd0, reinterpret_cast<void*>(&h_ground_contact), &g_o_ground_contact,
         "world: ground contact (remote copies on the ground)", {0x55, 0x8b, 0xec, 0x83, 0xec, 0x20, 0x56, 0x8b, 0xf1}, 9},
    };
    static const HookDef kGo[] = {
        LZ_DMG(3, 5, 0x18e60, 0x55, 0x8b, 0xec, 0x83, 0xec, 0x1c, 0x8b, 0x45, 0x08),
        LZ_DMG(4, 8, 0x14780, 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x83, 0xec, 0x4c),
        LZ_DMG(5, 6, 0x146a0, 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x83, 0xec, 0x40),
        {0x15aa0, reinterpret_cast<void*>(&h_ground_kill_event<0>), &g_o_ground_kill_event[0],
         "world: ground object kill event (remote copies on a client)", {0x55, 0x8b, 0xec, 0x83, 0xec, 0x38, 0x56, 0x8b, 0xf1}, 9},
    };
    static const HookDef kGm[] = {
        LZ_DMG(6, 5, 0x11500, 0x55, 0x8b, 0xec, 0x83, 0xec, 0x1c, 0x8b, 0x45, 0x08),
        LZ_DMG(7, 8, 0xbf70, 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x83, 0xec, 0x4c),
        LZ_DMG(8, 6, 0xbe90, 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x83, 0xec, 0x40),
        {0xd290, reinterpret_cast<void*>(&h_ground_kill_event<1>), &g_o_ground_kill_event[1],
         "world: vehicle kill event (remote copies on a client)", {0x55, 0x8b, 0xec, 0x83, 0xec, 0x38, 0x56, 0x8b, 0xf1}, 9},
    };
    static const HookDef kNv[] = {
        LZ_DMG(9, 5, 0x35d0, 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x14, 0xd9, 0x45, 0x10),
        LZ_DMG(10, 8, 0x3700, 0x55, 0x8b, 0xec, 0xd9, 0x45, 0x24, 0x8b, 0x45, 0x20),
        LZ_DMG(11, 6, 0x3660, 0x55, 0x8b, 0xec, 0x8b, 0x45, 0x1c, 0xd9, 0x45, 0x10),
        {0x12620, reinterpret_cast<void*>(&h_ground_kill_event<2>), &g_o_ground_kill_event[2],
         "world: ship kill event (remote copies on a client)", {0x55, 0x8b, 0xec, 0x83, 0xec, 0x38, 0x56, 0x8b, 0xf1}, 9},
    };
#undef LZ_DMG
    hook_image(L"AircraftObject.dll", "AircraftObject.dll", kAoStamp, kAoSize, kAo, 6, g_ao, g_ao_bad);
    hook_image(L"GroundObject.dll", "GroundObject.dll", kGoStamp, kGoSize, kGo, 4, g_go, g_go_bad);
    hook_image(L"GroundMobileObject.dll", "GroundMobileObject.dll", kGmStamp, kGmSize, kGm, 4, g_gm, g_gm_bad);
    hook_image(L"NavalObject.dll", "NavalObject.dll", kNvStamp, kNvSize, kNv, 4, g_nv, g_nv_bad);
}

}  // namespace LazyWorks::SF2Patch::hooks
