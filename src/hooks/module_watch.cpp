// Watches DLL loads so features can hook modules as soon as they appear. All LoadLibrary
// variants end up in kernelbase!LoadLibraryExW. Handlers run on the loading thread, after the
// load (and the loader lock) has finished, and must be cheap when there is nothing new to do.
#include <windows.h>

#include <MinHook.h>

#include "config.h"
#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {
namespace {

using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadLibraryExW_t o_LoadLibraryExW = nullptr;

void notify() {
    const config::Config& c = config::get();
    if (c.borderless) dxgi_on_modules_loaded();
    if (c.mouse == config::MouseMode::Cursor) mouse_on_modules_loaded();
}

HMODULE WINAPI h_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE h = o_LoadLibraryExW(name, file, flags);
    notify();
    return h;
}

}  // namespace

bool install_module_watch() {
    MH_STATUS s = MH_CreateHookApi(L"kernelbase", "LoadLibraryExW", reinterpret_cast<LPVOID>(&h_LoadLibraryExW),
                                   reinterpret_cast<LPVOID*>(&o_LoadLibraryExW));
    if (s != MH_OK) {
        LOG("modules: hooking LoadLibraryExW failed: %s", MH_StatusToString(s));
        return false;
    }
    return true;
}

}  // namespace lazysf2::hooks
