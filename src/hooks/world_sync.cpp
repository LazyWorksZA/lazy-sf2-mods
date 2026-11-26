// Shared mission world, tasks T1/T2 (developer tool): saves the generated single mission to a .MSN file in the probe
// folder and runs from it (LAZYSF2_WORLD_SAVE=1), or runs a saved one instead of the generated mission
// (LAZYSF2_WORLD_LOAD=<file>), with the same RND seed and forced start options; after the mission's one creation pass
// it writes a manifest of every object, keyed by mission slot or placed record, for diffing two launches.
// Fixed RVAs, July 2013 build only. See lazy-sf2-recon: network-recon §10.1, §10.3 and §10.10.
#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "hooks/aircraft_side.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/world_files.h"
#include "log.h"

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
constexpr size_t kPageSettings = 0x88;
constexpr int kEventAccept = 0x65, kEventFly = 0x67;

// ObjectManager group table (recon-notes §20); objects: flags, position, angles, type records.
constexpr DWORD kManagerRva = 0x234d0, kManagerVtableRva = 0x1d55c;
constexpr size_t kMgrGroupCount = 0x14, kMgrGroups = 0x18, kGroupStride = 0x54, kGrpCount = 0x30, kGrpSlots = 0x3c;
constexpr uint32_t kMaxGroups = 64, kMaxGroupSlots = 65536;
constexpr size_t kObjFlags = 0x0c, kObjPos = 0x38, kObjAngles = 0x100;
constexpr size_t kAircraftData = 0x380, kGroundTypeRec = 0x360, kTypeName = 0x04;

enum class Mode { Off, Save, Load };
Mode g_mode = Mode::Off;          // set once in world_install()
std::wstring g_load_path;         // LAZYSF2_WORLD_LOAD, full path
bool g_seed_from_env = false;
uint32_t g_env_seed = 0;

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
    void* settings = static_cast<BYTE*>(page) + kPageSettings;
    if (call_generate(c.msn, settings)) LOG("world: generated the mission again from the New page's settings");
    else LOG("world: ERR regenerating the mission faulted; do not fly this mission");
}

void accept_save(const Ctx& c, void* page) {
    const std::wstring dir = probe_output_dir();
    const int n = next_number(dir);
    const std::wstring msn_path = numbered(dir, n, L".msn");
    const std::wstring side_path = world_companion_path(msn_path, L".sidecar.txt");
    if (side_path.size() >= MAX_PATH) {
        LOG("world: save refused (probe folder path too long)");
        return;
    }
    if (call_save(c.msn, msn_path.c_str()) != 0) {
        LOG("world: ERR MSN Save faulted for %ls", msn_path.c_str());
        return;
    }
    if (!file_exists(msn_path)) {
        LOG("world: ERR MSN Save wrote no file at %ls; mission left as generated", msn_path.c_str());
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
        regenerate(c, page);
        return;
    }
    HeaderRaw h{};
    WorldSidecar s;
    s.msn = file_name_of(msn_path);
    s.seed = seed;
    s.has_seed = true;
    if (read_header(c.msn, &h)) fill_sidecar_from_header(h, &s);
    if (!write_text(side_path, format_sidecar(s), false)) LOG("world: ERR could not write %ls", side_path.c_str());
    LOG("world: saved the generated mission to %ls (theatre %s, %s, %04d-%02d-%02d %02d:%02d, weather %d), seed %lu, "
        "reloaded it from the file; sidecar %ls",
        msn_path.c_str(), s.theatre.c_str(), s.aircraft.c_str(), s.year, s.month, s.day, s.hour, s.minute,
        s.weather, static_cast<unsigned long>(seed), side_path.c_str());
    AcquireSRWLockExclusive(&g_lock);
    g_armed = Armed{true, c.msn, seed, n, s, msn_path, side_path};
    ReleaseSRWLockExclusive(&g_lock);
}

void accept_load(const Ctx& c, void* page) {
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
    const std::wstring dir = probe_output_dir();
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
    if (ev != kEventAccept && ev != kEventFly) return r;
    Ctx c{};
    if (!resolve(&c, "New page Accept")) return r;
    if (g_mode == Mode::Save) accept_save(c, page);
    else if (g_mode == Mode::Load) accept_load(c, page);
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
    const std::wstring path = numbered(probe_output_dir(), a.number, L".manifest.txt");
    if (!write_text(path, text, true)) {
        LOG("world: ERR could not write %ls (exists, or path too long)", path.c_str());
        return;
    }
    LOG("world: manifest %ls: %s; %zu lines, player %s, %d formations without members", path.c_str(),
        format_manifest_counts(v).c_str() + 2, v.size(), player_key.c_str(), empty_formations);
}

int __fastcall h_Start(void* msn) {
    Armed a;
    AcquireSRWLockExclusive(&g_lock);
    if (g_armed.on && g_armed.msn == msn) {
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
    }
    write_manifest(c, a, idx, sd);
    return result;
}

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

void world_install() {
    const bool save = env_set(L"LAZYSF2_WORLD_SAVE");
    wchar_t load[MAX_PATH] = {};
    const DWORD ln = GetEnvironmentVariableW(L"LAZYSF2_WORLD_LOAD", load, MAX_PATH);
    if (!save && ln == 0) return;
    if (!probe_output_dir()) {
        LOG("world: off (LAZYSF2_WORLD_SAVE/LOAD need LAZYSF2_PROBE_DIR)");
        return;
    }
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

void world_on_modules_loaded() {
    if (g_mode == Mode::Off) return;
    static const HookDef kSms[] = {
        {kRvaNewEvent, reinterpret_cast<void*>(&h_NewEvent), reinterpret_cast<void**>(&o_NewEvent),
         "world: SingleMissionScreen New page events", {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08}, 6},
    };
    static const HookDef kSm[] = {
        {kRvaStart, reinterpret_cast<void*>(&h_Start), reinterpret_cast<void**>(&o_Start),
         "world: SingleMission mission start", {0x55, 0x8b, 0xec, 0x81, 0xec, 0x34, 0x02, 0x00, 0x00}, 9},
        {kRvaIniGetInt, reinterpret_cast<void*>(&h_IniGetInt), reinterpret_cast<void**>(&o_IniGetInt),
         "world: SingleMission ini int getter", {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x53}, 7},
    };
    hook_image(L"SingleMissionScreen.dll", "SingleMissionScreen.dll", kSmsStamp, kSmsSize, kSms, 1, g_sms, g_sms_bad);
    hook_image(L"SingleMission.dll", "SingleMission.dll", kSmStamp, kSmSize, kSm, 2, g_sm, g_sm_bad);
}

}  // namespace LazyWorks::SF2Patch::hooks
