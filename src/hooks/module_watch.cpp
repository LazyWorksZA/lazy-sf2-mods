// Watches DLL loads so features can hook modules as soon as they appear. All LoadLibrary
// variants end up in kernelbase!LoadLibraryExW. Handlers run on the loading thread, after the
// load (and the loader lock) has finished, and must be cheap when there is nothing new to do.
#include <windows.h>

#include <MinHook.h>

#include "config.h"
#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadLibraryExW_t o_LoadLibraryExW = nullptr;

// May run under the loader lock: handlers must not call the shell, COM or anything that can block, and must not
// read settings (config::display_mode()); the aircraft tracker only peeks at the Tacview setting.
void notify() {
    dxgi_on_modules_loaded();  // behaves as stock unless the display mode is Borderless
    input_on_modules_loaded();  // keyboard Windows key always; mouse only in Cursor mode
    sound_on_modules_loaded();  // DirectSoundCreate8 pass-through; buffers are changed only with BackgroundMode 2
    pilot_twist_on_modules_loaded();  // hooks created disabled; enabled only while Pilot Twist is on
    menu_on_modules_loaded();  // the Display Mode option is offered in every mode
    aircraft_track_on_modules_loaded();  // hooks only if the probe or Tacview Recording needs it
    weapon_track_on_modules_loaded();  // MissileObject.dll: same rule as the aircraft tracker
    tacview_recorder_on_modules_loaded();  // registers a step callback once; nothing runs unless tracking is on
    probe_on_modules_loaded();  // developer probe; does nothing unless LAZYSF2_PROBE_DIR is set
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

}  // namespace LazyWorks::SF2Patch::hooks
