// "Keep running with sound" (BackgroundMode 2): new secondary DirectSound buffers get DSBCAPS_GLOBALFOCUS, so they
// keep playing while the game window is unfocused. A COM vtable hook on IDirectSound8::CreateSoundBuffer, reached
// through dsound!DirectSoundCreate8; no system DLL is replaced. See lazy-sf2-recon: network-recon §8.1.4.
#include <windows.h>

#include <mmreg.h>
#include <dsound.h>

#include <MinHook.h>

#include <atomic>

#include "config.h"
#include "hooks/focus_logic.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

// Core\DirectSound.dll (the game's SND service), July 2013.
constexpr DWORD kSndTimestamp = 0x51d21fc6, kSndImageSize = 0x2a000;
constexpr DWORD kSndManagerRva = 0x256a8;  // the sound manager object (InitDLL registers it as "SND")
constexpr DWORD kSndVtableRva = 0x1ee04;   // its vtable
constexpr int kSndStopAllChannels = 0x54 / 4;  // stops every playing sound channel (the flight unload uses it)
constexpr int kDsCreateSoundBuffer = 3;

using DirectSoundCreate8_t = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
using CreateSoundBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSound8*, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN);
using SndMethod_t = void(__fastcall*)(void* self, void* edx);

DirectSoundCreate8_t o_DirectSoundCreate8 = nullptr;
CreateSoundBuffer_t o_CreateSoundBuffer = nullptr;
std::atomic<bool> g_create_hooked{false};      // DirectSoundCreate8 detour placed (module watch)
std::atomic<bool> g_buffer_hooked{false};      // CreateSoundBuffer detour placed (mode 2 only)
std::atomic<long> g_global_focus_buffers{0};   // buffers created with the flag added by us (never decremented)
SRWLOCK g_lock = SRWLOCK_INIT;

HRESULT STDMETHODCALLTYPE h_CreateSoundBuffer(IDirectSound8* self, LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out,
                                             LPUNKNOWN outer) {
    if (desc && desc->dwSize >= sizeof(DSBUFFERDESC1) && (desc->dwFlags & DSBCAPS_GLOBALFOCUS) == 0 &&
        wants_global_focus(config::background_mode(), desc->dwFlags)) {
        DSBUFFERDESC copy = {};
        memcpy(&copy, desc, desc->dwSize < sizeof(copy) ? desc->dwSize : sizeof(copy));
        copy.dwFlags |= DSBCAPS_GLOBALFOCUS;
        const HRESULT hr = o_CreateSoundBuffer(self, &copy, out, outer);
        if (SUCCEEDED(hr) && g_global_focus_buffers.fetch_add(1) == 0) {
            LOG("sound: first sound buffer created with GLOBALFOCUS (flags 0x%lx): it plays without focus",
                copy.dwFlags);
        }
        return hr;
    }
    return o_CreateSoundBuffer(self, desc, out, outer);
}

// The game's own call, from DirectSound.dll's InitDLL (game thread, after the load: never under the loader lock).
HRESULT WINAPI h_DirectSoundCreate8(LPCGUID guid, LPDIRECTSOUND8* out, LPUNKNOWN outer) {
    const HRESULT hr = o_DirectSoundCreate8(guid, out, outer);
    if (FAILED(hr) || !out || !*out) return hr;
    const config::BackgroundMode mode = config::background_mode();
    if (mode != config::BackgroundMode::KeepRunningWithSound) return hr;
    if (!g_buffer_hooked.exchange(true)) {
        create_and_enable(vtable_slot(*out, kDsCreateSoundBuffer), reinterpret_cast<void*>(&h_CreateSoundBuffer),
                          reinterpret_cast<void**>(&o_CreateSoundBuffer), "IDirectSound8::CreateSoundBuffer");
        LOG("sound: BackgroundMode 2: sound buffers made from now on keep playing without focus (music stream "
            "excluded); a change of BackgroundMode applies after a restart");
    }
    return hr;
}

}  // namespace

void sound_on_modules_loaded() {
    if (g_create_hooked.load()) return;
    HMODULE dsound = GetModuleHandleW(L"dsound.dll");
    if (!dsound) return;
    // Module watch (maybe under the loader lock): no settings are read here.
    AcquireSRWLockExclusive(&g_lock);
    if (!g_create_hooked.exchange(true)) {
        if (void* create = reinterpret_cast<void*>(GetProcAddress(dsound, "DirectSoundCreate8"))) {
            create_and_enable(create, reinterpret_cast<void*>(&h_DirectSoundCreate8),
                              reinterpret_cast<void**>(&o_DirectSoundCreate8), "DirectSoundCreate8");
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool sound_global_focus_active() { return g_global_focus_buffers.load() > 0; }

void sound_stop_all_channels(const char* why) {
    HMODULE snd = GetModuleHandleW(L"DirectSound.dll");  // Core\DirectSound.dll (the game's)
    static std::atomic<bool> checked{false}, ok{false}, logged{false};
    if (!snd) return;
    if (!checked.exchange(true)) ok = is_expected_build(snd, kSndTimestamp, kSndImageSize, "DirectSound.dll");
    if (!ok.load()) return;
    void* mgr = at_rva(snd, kSndManagerRva);
    if (*static_cast<void**>(mgr) != at_rva(snd, kSndVtableRva)) return;  // not constructed (or already destroyed)
    // Takes the manager's own lock and does nothing before its init; the game thread, as stock calls it.
    reinterpret_cast<SndMethod_t>(vtable_slot(mgr, kSndStopAllChannels))(mgr, nullptr);
    if (!logged.exchange(true)) LOG("sound: stopped the game's sound channels (%s); later ones are not logged", why);
}

}  // namespace LazyWorks::SF2Patch::hooks
