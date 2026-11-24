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
    LOG("lazysf2 " __DATE__ " " __TIME__ " loaded into %ls", exe);
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
            MH_STATUS s = MH_Initialize();
            if (s != MH_OK) {
                LOG("MH_Initialize failed: %s", MH_StatusToString(s));
                return TRUE;  // keep the proxy working even if hooking is impossible
            }
            lazysf2::config::load();  // lazysf2.ini; the display mode is read later, outside DllMain
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
