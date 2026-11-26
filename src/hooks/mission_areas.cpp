#include "hooks/mission_areas.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "hooks/hook_util.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {

namespace {

// StrikeFighters2.exe: the service registry. See lazy-sf2-recon: recon-notes §18.1.
constexpr DWORD kExeTimestamp = 0x51d22110, kExeImageSize = 0x62000;
constexpr DWORD kRvaSys = 0x414b4, kRvaSysVtable = 0x32330;
constexpr size_t kSlotGet = 0x08, kSlotRelease = 0x0c;
// SingleMission.dll: its MSN object and target areas. See lazy-sf2-recon: recon-notes §20.6 and §21.5.
constexpr DWORD kSmTimestamp = 0x51d220e5, kSmImageSize = 0xc1000;
constexpr DWORD kRvaMsn = 0xa67c0, kRvaMsnVtable = 0x8b924;
constexpr size_t kOffTargets = 0x2e30, kOffAreaCount = 0x690, kOffAreaArray = 0x694, kAreaBytes = 0x118;
constexpr size_t kOffAreaActive = 0x02, kOffAreaPos = 0x98, kOffAreaPlacedCount = 0xec, kOffAreaPlaced = 0xf8;
// A placed-object record (0x80 bytes): damage 0..100 (100 destroyed), offset from the area (already turned), the plain
// model it is drawn with, and the live object made instead. See lazy-sf2-recon: recon-notes §20.6.
constexpr size_t kOffPlacedDamage = 0x04, kOffPlacedOffset = 0x2c, kOffPlacedModel = 0x5c, kOffPlacedObject = 0x64;
constexpr int16_t kDamageDestroyed = 100;
constexpr int kMaxPlaced = 4096;
constexpr int kMaxAreas = 8192;
constexpr double kMaxCoord = 1e7;

using Get_t = int(__fastcall*)(void*, void*, const wchar_t*, void**);
using Release_t = void(__fastcall*)(void*, void*, const wchar_t*, void**);

std::atomic<unsigned> g_logged{0};
void log_once(unsigned bit, const char* msg) {
    if (!(g_logged.fetch_or(1u << bit) & (1u << bit))) LOG("%s", msg);
}

// 0 the MSN object is in *obj (a reference is held: release it), 1 none registered, 2 unexpected memory. SEH only.
int get_msn(HMODULE exe, void** obj, void** sys_out) {
    __try {
        void* sys = at_rva(exe, kRvaSys);
        void** vt = *static_cast<void***>(sys);
        if (vt != at_rva(exe, kRvaSysVtable)) return 2;
        *obj = nullptr;
        const auto get = reinterpret_cast<Get_t>(vt[kSlotGet / sizeof(void*)]);
        if (!get(sys, nullptr, L"MSN", obj) || !*obj) return 1;
        *sys_out = sys;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

void release_msn(void* sys, void** obj) {
    __try {
        void** vt = *static_cast<void***>(sys);
        reinterpret_cast<Release_t>(vt[kSlotRelease / sizeof(void*)])(sys, nullptr, L"MSN", obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 0 ok, 2 unexpected memory. SEH only.
// What mission_active_areas read last: mission_unchanged() compares against it. Game thread only.
struct Stamp {
    HMODULE sm = nullptr;
    const void* msn = nullptr;
    const BYTE* arr = nullptr;
    int count = -1;
};
Stamp g_stamp;

// The area array and count of an MSN object; false when unreadable or not SingleMission's. SEH only.
bool read_owner(HMODULE sm, const BYTE* msn, const BYTE** arr, int* count) {
    __try {
        if (*reinterpret_cast<void* const*>(msn) != at_rva(sm, kRvaMsnVtable)) return false;
        std::memcpy(count, msn + kOffTargets + kOffAreaCount, sizeof(*count));
        std::memcpy(arr, msn + kOffTargets + kOffAreaArray, sizeof(*arr));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int read_areas(HMODULE sm, const BYTE* msn, GameArea* out, size_t cap, size_t* n) {
    __try {
        if (*reinterpret_cast<void* const*>(msn) != at_rva(sm, kRvaMsnVtable)) return 2;
        const BYTE* owner = msn + kOffTargets;
        int count = 0;
        std::memcpy(&count, owner + kOffAreaCount, sizeof(count));
        const BYTE* arr = nullptr;
        std::memcpy(&arr, owner + kOffAreaArray, sizeof(arr));
        if (count < 0 || count > kMaxAreas || (count > 0 && !arr)) return 2;
        size_t k = 0;
        for (int i = 0; i < count; ++i) {
            const BYTE* rec = arr + static_cast<size_t>(i) * kAreaBytes;
            int16_t active = 0;
            double pos[2];
            std::memcpy(&active, rec + kOffAreaActive, sizeof(active));
            std::memcpy(pos, rec + kOffAreaPos, sizeof(pos));
            if (active != 0 && active != 1) return 2;  // the flag is 0 or 1: anything else means another layout
            if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || std::fabs(pos[0]) > kMaxCoord ||
                std::fabs(pos[1]) > kMaxCoord)
                return 2;
            if (active && k < cap) {
                int placed_n = 0;
                const void* const* placed = nullptr;
                std::memcpy(&placed_n, rec + kOffAreaPlacedCount, sizeof(placed_n));
                std::memcpy(&placed, rec + kOffAreaPlaced, sizeof(placed));
                if (placed_n < 0 || placed_n > kMaxPlaced || (placed_n > 0 && !placed)) return 2;
                out[k].x = pos[0];
                out[k].y = pos[1];
                out[k].placed = placed;
                out[k].n_placed = placed_n;
                ++k;
            }
        }
        *n = k;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

}  // namespace

MissionAreas mission_active_areas(GameArea* out, size_t cap, size_t* n) {
    *n = 0;
    g_stamp = Stamp{};
    static bool exe_checked = false, exe_ok = false;
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe_checked) {
        exe_checked = true;
        exe_ok = is_expected_build(exe, kExeTimestamp, kExeImageSize, "StrikeFighters2.exe");
    }
    if (!exe_ok) return MissionAreas::Unavailable;
    HMODULE sm = GetModuleHandleW(L"SingleMission.dll");
    if (!sm) return MissionAreas::NotSingle;
    static HMODULE sm_checked = nullptr;
    static bool sm_ok = false;
    if (sm != sm_checked) {
        sm_checked = sm;
        sm_ok = is_expected_build(sm, kSmTimestamp, kSmImageSize, "SingleMission.dll");
    }
    if (!sm_ok) return MissionAreas::Unavailable;
    void* msn = nullptr;
    void* sys = nullptr;
    const int g = get_msn(exe, &msn, &sys);
    if (g == 2) {
        log_once(0, "tacview structures: the service registry is not as expected; no mission area set");
        return MissionAreas::Unavailable;
    }
    if (g == 1) return MissionAreas::NotSingle;
    // The registry holds one MSN at a time: the running mission's. Only SingleMission's own object is read.
    MissionAreas result = MissionAreas::NotSingle;
    if (msn == at_rva(sm, kRvaMsn)) {
        if (read_areas(sm, static_cast<const BYTE*>(msn), out, cap, n) == 0 &&
            read_owner(sm, static_cast<const BYTE*>(msn), &g_stamp.arr, &g_stamp.count)) {
            g_stamp.sm = sm;
            g_stamp.msn = msn;
            result = MissionAreas::Read;
        } else {
            *n = 0;
            log_once(1, "tacview structures: SingleMission's target areas are not as expected; no mission area set");
            result = MissionAreas::Unavailable;
        }
    }
    release_msn(sys, &msn);
    return result;
}

bool mission_unchanged() {
    if (!g_stamp.msn || GetModuleHandleW(L"SingleMission.dll") != g_stamp.sm) return false;
    void* msn = nullptr;
    void* sys = nullptr;
    if (get_msn(GetModuleHandleW(nullptr), &msn, &sys) != 0) return false;
    const BYTE* arr = nullptr;
    int count = -1;
    const bool same = msn == g_stamp.msn && read_owner(g_stamp.sm, static_cast<const BYTE*>(msn), &arr, &count) &&
                      arr == g_stamp.arr && count == g_stamp.count;
    release_msn(sys, &msn);
    return same;
}

bool mission_placed(const GameArea& a, int i, PlacedView* out) {
    if (i < 0 || i >= a.n_placed) return false;
    __try {
        const BYTE* rec = static_cast<const BYTE*>(a.placed[i]);
        if (!rec) return false;
        float off[2];
        const void* model = nullptr;
        const void* object = nullptr;
        std::memcpy(off, rec + kOffPlacedOffset, sizeof(off));
        std::memcpy(&model, rec + kOffPlacedModel, sizeof(model));
        std::memcpy(&object, rec + kOffPlacedObject, sizeof(object));
        // The game adds the offset to the area position in float (SingleMission +0x3a2c0).
        out->x = static_cast<double>(off[0] + static_cast<float>(a.x));
        out->y = static_cast<double>(off[1] + static_cast<float>(a.y));
        out->model = model != nullptr;
        out->object = object != nullptr;
        out->record = rec;
        return std::isfinite(out->x) && std::isfinite(out->y);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

size_t mission_poll_destroyed(const void* const* records, size_t n, uint8_t* dead, uint32_t* out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n && k < cap; ++i) {
        if (dead[i]) continue;
        int16_t damage = 0;
        __try {
            std::memcpy(&damage, static_cast<const BYTE*>(records[i]) + kOffPlacedDamage, sizeof(damage));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            damage = 0;
        }
        if (damage < kDamageDestroyed) continue;
        dead[i] = 1;
        out[k++] = static_cast<uint32_t>(i);
    }
    return k;
}

int mission_placed_damage(const void* record) {
    int16_t damage = -1;
    __try {
        std::memcpy(&damage, static_cast<const BYTE*>(record) + kOffPlacedDamage, sizeof(damage));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return damage >= 0 && damage <= kDamageDestroyed ? damage : -1;
}

bool mission_set_destroyed(const void* record, int* before) {
    *before = mission_placed_damage(record);
    if (*before < 0 || *before >= kDamageDestroyed) return false;
    __try {
        *reinterpret_cast<volatile int16_t*>(const_cast<BYTE*>(static_cast<const BYTE*>(record)) + kOffPlacedDamage) =
            kDamageDestroyed;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace LazyWorks::SF2Patch::hooks
