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
constexpr size_t kOffAreaActive = 0x02, kOffAreaPos = 0x98;
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
int read_areas(HMODULE sm, const BYTE* msn, double (*out)[2], size_t cap, size_t* n) {
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
                out[k][0] = pos[0];
                out[k][1] = pos[1];
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

MissionAreas mission_active_areas(double (*out)[2], size_t cap, size_t* n) {
    *n = 0;
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
        if (read_areas(sm, static_cast<const BYTE*>(msn), out, cap, n) == 0) {
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

}  // namespace LazyWorks::SF2Patch::hooks
