// The generator filter (gen_filter.h): detours on SingleMission.dll's availability functions (they answer "not
// available" for a type outside the shared set) and on the ground picker (an unshared result becomes "none"). The
// type records are read from ObjectManager's class table. See lazy-sf2-recon: recon-notes §28.7d, §20.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "hooks/gen_filter.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

constexpr DWORD kSmStamp = 0x51d220e5, kSmSize = 0xc1000;
constexpr DWORD kAvail1 = 0x64ec0, kAvail2 = 0x65280, kAvail3 = 0x653e0, kGroundPick = 0x23d10, kGroundPick2 = 0x23b80;
// Only bytes without relocations (the functions load a global's address right after the frame).
constexpr BYTE kPro1[] = {0x55, 0x8b, 0xec, 0x8b, 0x0d};
constexpr BYTE kPro23[] = {0x55, 0x8b, 0xec, 0x51, 0x8b, 0x0d};
constexpr BYTE kProPick[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x14, 0x53, 0x8b, 0xd9, 0x8b, 0x83, 0xf8, 0x28, 0x00, 0x00, 0x56};
constexpr BYTE kProPick2[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x18, 0x8b, 0x45, 0x10, 0x53, 0x56, 0x57, 0x8b, 0xf9, 0x33, 0xdb};
constexpr size_t kAircraftSub = 0x6ec, kGroundSub = 0xb24;  // what the pickers pass the availability functions
constexpr size_t kRecName = 0x04, kGroundCategory = 0xbe4;
constexpr size_t kAircraftPath = 0x204, kGroundPath = 0x184;  // the type's data file / folder (260 wide characters)
constexpr DWORD kOmStamp = 0x51d21fa8, kOmSize = 0x28000;

using Avail1_t = int(__cdecl*)(int sub, int nation, int* date);
using Avail2_t = int(__cdecl*)(wchar_t* sub, int nation, int* date);
using Avail3_t = unsigned(__cdecl*)(wchar_t* sub, int nation);
using Pick_t = int(__fastcall*)(void* self, void* edx, int category, int nation, int fallback, wchar_t* name);
using Pick2_t = int(__fastcall*)(void* self, void* edx, int category, int mode, int side, int fallback);
Pick2_t o_pick2 = nullptr;
Avail1_t o_avail1 = nullptr;
Avail2_t o_avail2 = nullptr;
Avail3_t o_avail3 = nullptr;
Pick_t o_pick = nullptr;
HMODULE g_base = nullptr;

SRWLOCK g_lock = SRWLOCK_INIT;
bool g_active = false;
std::vector<uintptr_t> g_blocked_subs;     // sorted: record + kAircraftSub / kGroundSub of the unshared types
std::vector<int> g_blocked_ground;         // sorted: ground record indices of the unshared types
std::vector<uintptr_t> g_blocked_ground_recs;  // ... and their records (hidden from the ground pickers)
thread_local int g_hidden = 0;             // nesting of the ground pickers (only the outer one hides)
bool g_logged_pick = false;

bool blocked_sub(uintptr_t sub) {
    AcquireSRWLockShared(&g_lock);
    const bool b = g_active && std::binary_search(g_blocked_subs.begin(), g_blocked_subs.end(), sub);
    ReleaseSRWLockShared(&g_lock);
    return b;
}

int __cdecl h_avail1(int sub, int nation, int* date) {
    return blocked_sub(static_cast<uintptr_t>(sub)) ? 0 : o_avail1(sub, nation, date);
}
int __cdecl h_avail2(wchar_t* sub, int nation, int* date) {
    return blocked_sub(reinterpret_cast<uintptr_t>(sub)) ? 0 : o_avail2(sub, nation, date);
}
unsigned __cdecl h_avail3(wchar_t* sub, int nation) {
    return blocked_sub(reinterpret_cast<uintptr_t>(sub)) ? 0u : o_avail3(sub, nation);
}
// SEH only: swaps the categories of the given records with `with` (a value no picker asks for), keeping the old ones.
bool swap_categories(const uintptr_t* recs, short* saved, size_t n, bool hide) {
    __try {
        for (size_t i = 0; i < n; ++i) {
            short* c = reinterpret_cast<short*>(recs[i] + kGroundCategory);
            if (hide) {
                saved[i] = *c;
                *c = 0x7fff;
            } else {
                *c = saved[i];
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The ground pickers (+0x23d10, +0x23b80) loop over the records of a category and, when no candidate has a weight,
// fall back to the one with the closest service year. While they run, the unshared records' categories are hidden,
// so both the weighted pick and the fallback see the shared records only, the same on every PC. Menu thread
// (generate) or game thread (start); nothing else reads the categories then. See lazy-sf2-recon: recon-notes §28.7d.
struct HideUnshared {
    std::vector<uintptr_t> recs;  // a copy: the lock is not held while the picker runs (it calls the detours above)
    std::vector<short> saved;
    bool on = false;
    HideUnshared() {
        AcquireSRWLockShared(&g_lock);
        const bool outer = g_active && g_hidden == 0;
        if (outer) recs = g_blocked_ground_recs;
        ReleaseSRWLockShared(&g_lock);
        ++g_hidden;
        if (!recs.empty()) {
            saved.assign(recs.size(), 0);
            on = swap_categories(recs.data(), saved.data(), recs.size(), true);
        }
    }
    ~HideUnshared() {
        if (on) swap_categories(recs.data(), saved.data(), recs.size(), false);
        --g_hidden;
    }
};

int __fastcall h_pick2(void* self, void* edx, int category, int mode, int side, int fallback) {
    int r;
    {
        HideUnshared hide;
        r = o_pick2(self, edx, category, mode, side, fallback);
    }
    return r;
}

int __fastcall h_pick(void* self, void* edx, int category, int nation, int fallback, wchar_t* name) {
    int r;
    {
        HideUnshared hide;
        r = o_pick(self, edx, category, nation, fallback, name);
    }
    if (r < 0) return r;
    AcquireSRWLockShared(&g_lock);
    const bool b = g_active && std::binary_search(g_blocked_ground.begin(), g_blocked_ground.end(), r);
    const bool log = b && !g_logged_pick;
    if (log) g_logged_pick = true;
    ReleaseSRWLockShared(&g_lock);
    if (log) LOG("gen filter: the ground picker still chose an unshared type (category %d); none is placed", category);
    return b ? -1 : r;
}

// One type record, copied (names and folders): the menu thread walks, a worker may hash later.
struct Rec {
    uintptr_t ptr;
    short category;
    wchar_t name[64];
    wchar_t path[260];
};
constexpr int kMaxRecs = 20000;
// The calls below must match SingleMission's own: count() at slot 0, get(index, &record) at the spec's slot.
static_assert(kAircraftTypes.count_slot == 0 && kAircraftTypes.get_slot == 5 && kGroundTypes.count_slot == 0 &&
              kGroundTypes.get_slot == 4);

// SEH only, plain reads (no calls): one record's fields into o. False when they are not as analysed (names
// printable, ground categories small).
bool copy_record(const void* rec, bool ground, Rec* o) {
    __try {
        const BYTE* r = static_cast<const BYTE*>(rec);
        o->ptr = reinterpret_cast<uintptr_t>(rec);
        o->category = ground ? *reinterpret_cast<const short*>(r + kGroundCategory) : 0;
        if (o->category < 0 || o->category > 255) return false;
        const wchar_t* type = reinterpret_cast<const wchar_t*>(r + kRecName);
        int k = 0;
        for (; k < 63 && type[k]; ++k) {
            if (type[k] < 0x20 || type[k] > 0x7e) return false;
            o->name[k] = type[k];
        }
        o->name[k] = 0;
        if (k == 0) return false;
        const wchar_t* path = reinterpret_cast<const wchar_t*>(r + (ground ? kGroundPath : kAircraftPath));
        int q = 0;
        for (; q < 259 && path[q]; ++q) {
            if (path[q] < 0x20) return false;
            o->path[q] = path[q];
        }
        o->path[q] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH only, plain reads: whether the word at p equals want.
bool word_is(const void* p, const void* want) {
    __try {
        return *static_cast<void* const*>(p) == want;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool type_manager_verified(const TypeManagerSpec& s, void** out) {
    *out = nullptr;
    HMODULE m = GetModuleHandleW(s.module);
    if (!m || !is_expected_build(m, s.stamp, s.size, s.module_name)) return false;
    void* obj = at_rva(m, s.object);
    void* vt = at_rva(m, s.vtable);
    // The object is the type manager the class factory hands out (factory slot +0x10 returns factory + 4), its vtable
    // is the analysed one, and the two slots we call are the analysed functions: nothing unverified is ever called.
    if (!word_is(obj, vt) || !word_is(static_cast<BYTE*>(vt) + 4 * s.count_slot, at_rva(m, s.count_fn)) ||
        !word_is(static_cast<BYTE*>(vt) + 4 * s.get_slot, at_rva(m, s.get_fn)))
        return false;
    *out = obj;
    return true;
}

namespace {

// Menu thread only (the get slot writes the manager's "current" field, as the game's own loops do): the records of
// one type manager, verified before any call. -1 when not as analysed.
int read_class(bool ground, Rec* out) {
    const TypeManagerSpec& spec = ground ? kGroundTypes : kAircraftTypes;
    void* mgr = nullptr;
    if (!type_manager_verified(spec, &mgr)) return -1;
    using Count_t = int(__fastcall*)(void* self, void* edx);                       // thiscall, no arguments
    using Get_t = int(__fastcall*)(void* self, void* edx, int index, void** rec);  // thiscall (index, &rec)
    const int count = reinterpret_cast<Count_t>(vtable_slot(mgr, spec.count_slot))(mgr, nullptr);
    if (count <= 0 || count > kMaxRecs) return -1;
    for (int i = 0; i < count; ++i) {
        void* rec = nullptr;
        if (!reinterpret_cast<Get_t>(vtable_slot(mgr, spec.get_slot))(mgr, nullptr, i, &rec) || !rec) return -1;
        if (!copy_record(rec, ground, &out[i])) return -1;
    }
    return count;
}

std::string narrow_name(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s += static_cast<char>(*w);
    return s;
}

// A record's folder: its data path, without a file name when it names an .ini.
std::wstring record_dir(const wchar_t* path) {
    std::wstring p(path);
    if (p.size() > 4 && _wcsicmp(p.c_str() + p.size() - 4, L".ini") == 0) {
        const size_t slash = p.find_last_of(L"\\/");
        p.resize(slash == std::wstring::npos ? 0 : slash);
    }
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

}  // namespace

bool gen_walk_types(std::vector<TypeRecord>* out, std::string* why) {
    out->clear();
    std::vector<Rec> recs(kMaxRecs);
    for (const bool ground : {false, true}) {
        const int n = read_class(ground, recs.data());
        if (n < 0) {
            *why = ground ? "the ground object type records are not as expected" : "the aircraft type records are not as expected";
            out->clear();
            return false;
        }
        for (int i = 0; i < n; ++i) {
            TypeRecord r;
            r.kind = ground ? 'G' : 'A';
            r.name = narrow_name(recs[i].name);
            r.dir = record_dir(recs[i].path);
            r.ptr = recs[i].ptr;
            r.category = recs[i].category;
            out->push_back(std::move(r));
        }
    }
    return true;
}

std::vector<ContentEntry> gen_hash_types(const std::vector<TypeRecord>& types) {
    std::vector<ContentEntry> out;
    out.reserve(types.size());
    for (const TypeRecord& r : types) {
        if (!content_name_ok(r.name)) continue;
        out.push_back({r.kind, r.name, content_hash_in(r.kind, r.dir, r.name)});
    }
    return out;
}

namespace {
struct InventoryJob {
    std::mutex mx;
    bool done = false;
    std::vector<ContentEntry> entries;
    std::string why;
};
std::shared_ptr<InventoryJob> g_job;  // menu thread sets it once; the worker keeps its own reference
}  // namespace

void gen_inventory_start() {
    if (g_job) return;
    g_job = std::make_shared<InventoryJob>();
    std::shared_ptr<InventoryJob> job = g_job;
    // The record walk calls the type managers: here, on the menu thread. The worker only reads files (the file
    // manager is safe from any thread).
    std::vector<TypeRecord> types;
    std::string walk_why;
    if (!gen_walk_types(&types, &walk_why)) {
        LOG("gen filter: this PC's inventory could not be built (%s)", walk_why.c_str());
        std::lock_guard<std::mutex> lock(job->mx);
        job->why = walk_why;
        job->done = true;
        return;
    }
    try {
        std::thread([job, types = std::move(types)] {
            const ULONGLONG t0 = GetTickCount64();
            std::string why;
            std::vector<ContentEntry> e = gen_hash_types(types);
            size_t aircraft = 0, missing = 0;
            for (const auto& x : e) {
                aircraft += x.kind == 'A';
                missing += x.hash == 0;
            }
            if (e.empty())
                LOG("gen filter: this PC's inventory could not be built (%s)", why.c_str());
            else
                LOG("gen filter: this PC's inventory: %zu aircraft and %zu ground object types (%zu without their files) in "
                    "%llu ms",
                    aircraft, e.size() - aircraft, missing, static_cast<unsigned long long>(GetTickCount64() - t0));
            std::lock_guard<std::mutex> lock(job->mx);
            job->entries = std::move(e);
            job->why = why;
            job->done = true;
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(g_job->mx);
        g_job->why = "no thread";
        g_job->done = true;
    }
}

bool gen_inventory(std::vector<ContentEntry>* out, std::string* why) {
    std::shared_ptr<InventoryJob> job = g_job;
    if (!job) return false;
    std::lock_guard<std::mutex> lock(job->mx);
    if (!job->done) return false;
    *out = job->entries;
    if (why) *why = job->why;
    return true;
}

bool gen_filter_apply(const SharedTypes& s, std::string* why) {
    gen_filter_clear();
    if (!g_base) {
        *why = "SingleMission.dll is not hooked";
        return false;
    }
    if (!is_expected_build(GetModuleHandleW(L"ObjectManager.dll"), kOmStamp, kOmSize, "ObjectManager.dll")) {
        *why = "ObjectManager.dll is not the expected build";
        return false;
    }
    std::vector<TypeRecord> types;
    if (!gen_walk_types(&types, why)) return false;
    std::vector<uintptr_t> subs, ground_recs;
    std::vector<int> ground_idx;
    size_t aircraft_kept = 0, ground_kept = 0;
    int na = 0, ng = 0;
    for (const TypeRecord& r : types) {
        if (r.kind == 'A') {
            ++na;
            if (shared_has(s.aircraft, r.name))
                ++aircraft_kept;
            else
                subs.push_back(r.ptr + kAircraftSub);
        } else {
            const int i = ng++;
            if (shared_has(s.ground, r.name)) {
                ++ground_kept;
            } else {
                subs.push_back(r.ptr + kGroundSub);
                ground_idx.push_back(i);
                ground_recs.push_back(r.ptr);
            }
        }
    }
    std::sort(subs.begin(), subs.end());
    AcquireSRWLockExclusive(&g_lock);
    g_blocked_subs = std::move(subs);
    g_blocked_ground = std::move(ground_idx);
    g_blocked_ground_recs = std::move(ground_recs);
    g_active = true;
    g_logged_pick = false;
    ReleaseSRWLockExclusive(&g_lock);
    LOG("gen filter: on: %zu of %d aircraft types and %zu of %d ground object types are shared and may be picked",
        aircraft_kept, na, ground_kept, ng);
    return true;
}

void gen_filter_clear() {
    AcquireSRWLockExclusive(&g_lock);
    const bool was = g_active;
    g_active = false;
    g_blocked_subs.clear();
    g_blocked_ground.clear();
    g_blocked_ground_recs.clear();
    ReleaseSRWLockExclusive(&g_lock);
    if (was) LOG("gen filter: off (stock picking)");
}

bool gen_filter_active() {
    AcquireSRWLockShared(&g_lock);
    const bool a = g_active;
    ReleaseSRWLockShared(&g_lock);
    return a;
}

void gen_filter_on_modules_loaded() {
    HMODULE m = GetModuleHandleW(L"SingleMission.dll");
    if (!m) return;
    const auto* t1 = static_cast<const BYTE*>(at_rva(m, kAvail1));
    if (m == g_base && *t1 == 0xE9) return;  // hooked
    static HMODULE bad = nullptr;
    if (m == bad) return;
    // A new image (the game frees the mission DLL between flights): its own hooks; the earlier image's entries are
    // removed (MH_RemoveHook at the same base restores stock bytes, which this image has too).
    g_base = nullptr;
    const auto* t2 = static_cast<const BYTE*>(at_rva(m, kAvail2));
    const auto* t3 = static_cast<const BYTE*>(at_rva(m, kAvail3));
    const auto* tp = static_cast<const BYTE*>(at_rva(m, kGroundPick));
    const auto* tp2 = static_cast<const BYTE*>(at_rva(m, kGroundPick2));
    if (!is_expected_build(m, kSmStamp, kSmSize, "SingleMission.dll") || std::memcmp(t1, kPro1, sizeof(kPro1)) != 0 ||
        std::memcmp(t2, kPro23, sizeof(kPro23)) != 0 || std::memcmp(t3, kPro23, sizeof(kPro23)) != 0 ||
        std::memcmp(tp, kProPick, sizeof(kProPick)) != 0 || std::memcmp(tp2, kProPick2, sizeof(kProPick2)) != 0) {
        bad = m;
        LOG("gen filter: SingleMission.dll not hooked (build or prologues); shared-content generation unavailable");
        return;
    }
    bool ok = true;
    for (DWORD rva : {kAvail1, kAvail2, kAvail3, kGroundPick, kGroundPick2}) (void)MH_RemoveHook(at_rva(m, rva));
    ok &= create_and_enable(at_rva(m, kAvail1), reinterpret_cast<void*>(&h_avail1), reinterpret_cast<void**>(&o_avail1),
                            "SingleMission availability 1");
    ok &= create_and_enable(at_rva(m, kAvail2), reinterpret_cast<void*>(&h_avail2), reinterpret_cast<void**>(&o_avail2),
                            "SingleMission availability 2");
    ok &= create_and_enable(at_rva(m, kAvail3), reinterpret_cast<void*>(&h_avail3), reinterpret_cast<void**>(&o_avail3),
                            "SingleMission availability 3");
    ok &= create_and_enable(at_rva(m, kGroundPick), reinterpret_cast<void*>(&h_pick), reinterpret_cast<void**>(&o_pick),
                            "SingleMission ground picker");
    ok &= create_and_enable(at_rva(m, kGroundPick2), reinterpret_cast<void*>(&h_pick2), reinterpret_cast<void**>(&o_pick2),
                            "SingleMission ground picker 2");
    if (ok) g_base = m;
}

}  // namespace LazyWorks::SF2Patch::hooks
