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

// May run under the loader lock (DLLs loading other DLLs during start-up), so handlers must not
// call into the shell, COM or anything else that can block. In particular they must not call
// config::display_mode(); hooks that depend on it check it when they are called instead.
void notify() {
    dxgi_on_modules_loaded();  // behaves as stock unless the display mode is Borderless
    if (config::get().mouse == config::MouseMode::Cursor) mouse_on_modules_loaded();
    menu_on_modules_loaded();  // the Display Mode option is offered in every mode
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
