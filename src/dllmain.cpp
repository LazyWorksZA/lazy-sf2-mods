// lazysf2: Strike Fighters 2 borderless fullscreen mod, loaded as a winmm.dll proxy.
//
// Set the environment variable LAZYSF2_DISABLE=1 to load as a plain pass-through proxy
// (no hooks), for comparing against stock behaviour.
#include <windows.h>

#include <MinHook.h>

#include "config.h"
#include "hooks/hooks.h"
#include "log.h"
#include "proxy/proxy.h"

namespace {

bool hooks_disabled() {
    wchar_t v[8] = {};
    return GetEnvironmentVariableW(L"LAZYSF2_DISABLE", v, 8) > 0 && v[0] == L'1';
}

void log_host() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    LOG("lazysf2 " __DATE__ " " __TIME__ " loaded into %ls", lazysf2::log::log_path(exe).c_str());
}

// The exe is DPI-unaware. At display scaling other than 100% Windows gives it a virtual desktop
// (e.g. 2048x1152 for 2560x1440 at 125%), and a scaling change while it runs leaves the game in
// part of the screen. Declaring the process per-monitor DPI aware before the game creates its
// window makes it always see real pixels. Applies in every display mode (stock is broken there).
void make_dpi_aware() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using SetCtx_t = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto set_ctx = user32 ? reinterpret_cast<SetCtx_t>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")) : nullptr;
    if (set_ctx && set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        LOG("dpi: process is per-monitor DPI aware (v2)");
    } else if (SetProcessDPIAware()) {
        LOG("dpi: process is system DPI aware (fallback, error %lu)", GetLastError());
    } else {
        LOG("dpi: could not make the process DPI aware (error %lu)", GetLastError());
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            lazysf2::log::open();
            log_host();
            if (!lazysf2::proxy::init()) return FALSE;

            if (hooks_disabled()) {
                LOG("LAZYSF2_DISABLE=1: hooks not installed");
                return TRUE;
            }
            make_dpi_aware();  // before the game creates any window
            MH_STATUS s = MH_Initialize();
            if (s != MH_OK) {
                LOG("MH_Initialize failed: %s", MH_StatusToString(s));
                return TRUE;  // keep the proxy working even if hooking is impossible
            }
            // Settings come from Options.ini and are read later, on first use outside DllMain.
            bool ok = lazysf2::hooks::install_window_hooks();
            ok &= lazysf2::hooks::install_menu_hooks();
            ok &= lazysf2::hooks::install_module_watch();
            s = MH_EnableHook(MH_ALL_HOOKS);
            LOG("hooks installed: %s, enable: %s", ok ? "ok" : "PARTIAL", MH_StatusToString(s));
            break;
        }
        case DLL_PROCESS_DETACH:
            if (reserved == nullptr) {  // FreeLibrary, not process exit
                MH_DisableHook(MH_ALL_HOOKS);
                MH_Uninitialize();
            }
            lazysf2::proxy::shutdown();
            LOG("lazysf2 unloading");
            lazysf2::log::close();
            break;
        default:
            break;
    }
    return TRUE;
}
