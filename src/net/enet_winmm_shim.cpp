// Satisfies ENet's winmm imports from inside our own winmm.dll.
//
// The mod IS winmm.dll (a proxy, src/proxy/). ENet's win32 time code calls timeGetTime, timeBeginPeriod and
// timeEndPeriod, which a normal build gets by linking winmm.lib: that would make this DLL import from its own
// name. So winmm.lib is not linked into the lazysf2 target, and the three import slots ENet's object code asks
// for (__imp__timeGetTime@0, __imp__timeBeginPeriod@4, __imp__timeEndPeriod@4) are defined here, each holding
// the address of a small wrapper that calls the same function in the real system winmm.dll that proxy::init()
// loaded. /ALTERNATENAME makes the linker use these when the symbols are otherwise undefined, so a build that
// does link winmm.lib (the unit tests) is unaffected.
#include <windows.h>

#include "proxy/proxy.h"

namespace {

using TimeGetTime_t = DWORD(WINAPI*)();
using TimePeriod_t = UINT(WINAPI*)(UINT);

template <typename Fn>
Fn real_fn(const char* name) {
    const HMODULE real = lazysf2::proxy::real_module();
    return real ? reinterpret_cast<Fn>(GetProcAddress(real, name)) : nullptr;
}

DWORD WINAPI shim_timeGetTime() {
    static const TimeGetTime_t fn = real_fn<TimeGetTime_t>("timeGetTime");  // proxy::init() ran long before ENet
    return fn ? fn() : static_cast<DWORD>(GetTickCount64());
}
UINT WINAPI shim_timeBeginPeriod(UINT ms) {
    static const TimePeriod_t fn = real_fn<TimePeriod_t>("timeBeginPeriod");
    return fn ? fn(ms) : 0u;  // 0 is TIMERR_NOERROR
}
UINT WINAPI shim_timeEndPeriod(UINT ms) {
    static const TimePeriod_t fn = real_fn<TimePeriod_t>("timeEndPeriod");
    return fn ? fn(ms) : 0u;
}

}  // namespace

extern "C" {
// The slots hold function addresses (constant-initialised, so there is no start-up order to depend on).
void* lazysf2_imp_timeGetTime = reinterpret_cast<void*>(&shim_timeGetTime);
void* lazysf2_imp_timeBeginPeriod = reinterpret_cast<void*>(&shim_timeBeginPeriod);
void* lazysf2_imp_timeEndPeriod = reinterpret_cast<void*>(&shim_timeEndPeriod);
}

// x86 decorates a C name with a leading underscore, so these are _lazysf2_imp_* on the right-hand side.
#pragma comment(linker, "/ALTERNATENAME:__imp__timeGetTime@0=_lazysf2_imp_timeGetTime")
#pragma comment(linker, "/ALTERNATENAME:__imp__timeBeginPeriod@4=_lazysf2_imp_timeBeginPeriod")
#pragma comment(linker, "/ALTERNATENAME:__imp__timeEndPeriod@4=_lazysf2_imp_timeEndPeriod")
