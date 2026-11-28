// The generator filter (gen_filter.h): detours on SingleMission.dll's availability functions (they answer "not
// available" for a type outside the shared set) and on the ground picker (an unshared result becomes "none"). The
// type records are read from ObjectManager's class table. See lazy-sf2-recon: recon-notes §28.7d, §20.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "hooks/gen_filter.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

constexpr DWORD kSmStamp = 0x51d220e5, kSmSize = 0xc1000;
constexpr DWORD kAvail1 = 0x64ec0, kAvail2 = 0x65280, kAvail3 = 0x653e0, kGroundPick = 0x23d10;
// Only bytes without relocations (the functions load a global's address right after the frame).
constexpr BYTE kPro1[] = {0x55, 0x8b, 0xec, 0x8b, 0x0d};
constexpr BYTE kPro23[] = {0x55, 0x8b, 0xec, 0x51, 0x8b, 0x0d};
constexpr BYTE kProPick[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x14, 0x53, 0x8b, 0xd9, 0x8b, 0x83, 0xf8, 0x28, 0x00, 0x00, 0x56};
constexpr size_t kAircraftSub = 0x6ec, kGroundSub = 0xb24;  // what the pickers pass the availability functions
constexpr size_t kRecName = 0x04, kGroundCategory = 0xbe4;
constexpr DWORD kOmStamp = 0x51d21fa8, kOmSize = 0x28000, kOmManager = 0x234d0, kOmVtable = 0x1d55c;

using Avail1_t = int(__cdecl*)(int sub, int nation, int* date);
using Avail2_t = int(__cdecl*)(wchar_t* sub, int nation, int* date);
using Avail3_t = unsigned(__cdecl*)(wchar_t* sub, int nation);
using Pick_t = int(__fastcall*)(void* self, void* edx, int category, int nation, int fallback, wchar_t* name);
Avail1_t o_avail1 = nullptr;
Avail2_t o_avail2 = nullptr;
Avail3_t o_avail3 = nullptr;
Pick_t o_pick = nullptr;
HMODULE g_base = nullptr;

SRWLOCK g_lock = SRWLOCK_INIT;
bool g_active = false;
std::vector<uintptr_t> g_blocked_subs;     // sorted: record + kAircraftSub / kGroundSub of the unshared types
std::vector<int> g_blocked_ground;         // sorted: ground record indices of the unshared types
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
// The ground picker may fall back to the closest-year candidate even when every weight is 0: an unshared result is
// turned into "none".
int __fastcall h_pick(void* self, void* edx, int category, int nation, int fallback, wchar_t* name) {
    const int r = o_pick(self, edx, category, nation, fallback, name);
    if (r < 0) return r;
    AcquireSRWLockShared(&g_lock);
    const bool b = g_active && std::binary_search(g_blocked_ground.begin(), g_blocked_ground.end(), r);
    const bool log = b && !g_logged_pick;
    if (log) g_logged_pick = true;
    ReleaseSRWLockShared(&g_lock);
    if (log) LOG("gen filter: the ground picker's fallback chose an unshared type (category %d); none is placed", category);
    return b ? -1 : r;
}

struct Rec {
    uintptr_t ptr;
    short category;
    wchar_t name[64];
};
constexpr int kMaxRecs = 20000;

// SEH only: the records of the class whose name starts with prefix, into out[kMaxRecs]. -1 when anything is not as
// analysed (names printable, ground categories small).
int read_class(const wchar_t* prefix, bool ground, Rec* out) {
    HMODULE om = GetModuleHandleW(L"ObjectManager.dll");
    if (!om) return -1;
    const BYTE* mgr = static_cast<const BYTE*>(at_rva(om, kOmManager));
    const size_t plen = wcslen(prefix);
    __try {
        if (*reinterpret_cast<void* const*>(mgr) != at_rva(om, kOmVtable)) return -1;
        const uint32_t n = *reinterpret_cast<const uint32_t*>(mgr + 0x08);
        const BYTE* arr = *reinterpret_cast<const BYTE* const*>(mgr + 0x0c);
        if (!arr || n == 0 || n > 64) return -1;
        void* cls = nullptr;
        for (uint32_t i = 0; i < n && !cls; ++i) {
            const wchar_t* name = *reinterpret_cast<const wchar_t* const*>(arr + 0x10 * i + 0x08);
            if (name && _wcsnicmp(name, prefix, plen) == 0 && (name[plen] == 0 || name[plen] == L'.'))
                cls = *reinterpret_cast<void* const*>(arr + 0x10 * i);
        }
        if (!cls) return -1;
        using Count_t = int(__fastcall*)(void*, void*);
        using Get_t = void(__fastcall*)(void*, void*, int, void**);
        const int count = reinterpret_cast<Count_t>(vtable_slot(cls, 0))(cls, nullptr);
        if (count <= 0 || count > kMaxRecs) return -1;
        const int get_slot = ground ? 0x10 / 4 : 0x14 / 4;  // as SingleMission calls them (§28.7d)
        for (int i = 0; i < count; ++i) {
            void* rec = nullptr;
            reinterpret_cast<Get_t>(vtable_slot(cls, get_slot))(cls, nullptr, i, &rec);
            if (!rec) return -1;
            const BYTE* r = static_cast<const BYTE*>(rec);
            Rec& o = out[i];
            o.ptr = reinterpret_cast<uintptr_t>(rec);
            o.category = ground ? *reinterpret_cast<const short*>(r + kGroundCategory) : 0;
            if (o.category < 0 || o.category > 255) return -1;
            const wchar_t* type = reinterpret_cast<const wchar_t*>(r + kRecName);
            int k = 0;
            for (; k < 63 && type[k]; ++k) {
                if (type[k] < 0x20 || type[k] > 0x7e) return -1;
                o.name[k] = type[k];
            }
            o.name[k] = 0;
            if (k == 0) return -1;
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

std::string narrow_name(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s += static_cast<char>(*w);
    return s;
}

}  // namespace

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
    std::vector<Rec> recs(kMaxRecs);
    std::vector<uintptr_t> subs;
    std::vector<int> ground_idx;
    size_t aircraft_kept = 0, ground_kept = 0;
    const int na = read_class(L"AircraftObject", false, recs.data());
    if (na < 0) {
        *why = "the aircraft type records are not as expected";
        return false;
    }
    for (int i = 0; i < na; ++i) {
        if (shared_has(s.aircraft, narrow_name(recs[i].name)))
            ++aircraft_kept;
        else
            subs.push_back(recs[i].ptr + kAircraftSub);
    }
    const int ng = read_class(L"GroundObject", true, recs.data());
    if (ng < 0) {
        *why = "the ground object type records are not as expected";
        return false;
    }
    for (int i = 0; i < ng; ++i) {
        if (shared_has(s.ground, narrow_name(recs[i].name))) {
            ++ground_kept;
        } else {
            subs.push_back(recs[i].ptr + kGroundSub);
            ground_idx.push_back(i);
        }
    }
    std::sort(subs.begin(), subs.end());
    AcquireSRWLockExclusive(&g_lock);
    g_blocked_subs = std::move(subs);
    g_blocked_ground = std::move(ground_idx);
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
    if (!is_expected_build(m, kSmStamp, kSmSize, "SingleMission.dll") || std::memcmp(t1, kPro1, sizeof(kPro1)) != 0 ||
        std::memcmp(t2, kPro23, sizeof(kPro23)) != 0 || std::memcmp(t3, kPro23, sizeof(kPro23)) != 0 ||
        std::memcmp(tp, kProPick, sizeof(kProPick)) != 0) {
        bad = m;
        LOG("gen filter: SingleMission.dll not hooked (build or prologues); shared-content generation unavailable");
        return;
    }
    bool ok = true;
    for (DWORD rva : {kAvail1, kAvail2, kAvail3, kGroundPick}) (void)MH_RemoveHook(at_rva(m, rva));
    ok &= create_and_enable(at_rva(m, kAvail1), reinterpret_cast<void*>(&h_avail1), reinterpret_cast<void**>(&o_avail1),
                            "SingleMission availability 1");
    ok &= create_and_enable(at_rva(m, kAvail2), reinterpret_cast<void*>(&h_avail2), reinterpret_cast<void**>(&o_avail2),
                            "SingleMission availability 2");
    ok &= create_and_enable(at_rva(m, kAvail3), reinterpret_cast<void*>(&h_avail3), reinterpret_cast<void**>(&o_avail3),
                            "SingleMission availability 3");
    ok &= create_and_enable(at_rva(m, kGroundPick), reinterpret_cast<void*>(&h_pick), reinterpret_cast<void**>(&o_pick),
                            "SingleMission ground picker");
    if (ok) g_base = m;
}

}  // namespace LazyWorks::SF2Patch::hooks
