// The lobby's way through the stock screens (L5, L6): while the Hangar carries the lobby's way back (it was opened in a
// session, even if the session has ended since), its Fly goes back to the lobby (Done for the host, Ready for a client)
// instead of starting the flight. Hooks HangarScreen.dll's event handler each
// time the screen's DLL is loaded (the menu system frees it when the screen closes). See lazy-sf2-recon: §28.5.
#include <windows.h>

#include <cstring>

#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/lobby_splice.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

constexpr DWORD kHangarStamp = 0x51d220f0, kHangarSize = 0x23000;
constexpr DWORD kHangarEventRva = 0x1060;  // HangarScreenClass handler slot 3: HandleEvent(event*)
constexpr BYTE kPrologue[] = {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x56, 0x50, 0x8b, 0xf1};

using Event_t = void(__fastcall*)(void* self, void* edx, const int* ev);
Event_t o_event = nullptr;
HMODULE g_base = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;

void __fastcall h_event(void* self, void* edx, const int* ev) {
    if (lobby_hangar_return_item() > 0) {
        if (lobby_hangar_event(self, ev)) return;
        // Any other way the handler flies (a key) is caught at the menu system's command (lobby_screen.cpp).
        lobby_hangar_enter(self);
        o_event(self, edx, ev);
        lobby_hangar_leave();
        return;
    }
    o_event(self, edx, ev);
}

// MenuSystem's screen open (MNU +0xd9e0, thiscall (name, remember)): every way to another screen ends here. See
// lazy-sf2-recon: recon-notes §28.1.
constexpr DWORD kMnuStamp = 0x51d21fc1, kMnuSize = 0x41000;
constexpr DWORD kMnuOpenRva = 0xd9e0;
constexpr DWORD kMnuCurrentName = 0x430;  // the open screen's name (wide, MAX_PATH)
constexpr BYTE kOpenPrologue[] = {0x55, 0x8b, 0xec, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68};
using Open_t = int(__fastcall*)(void* mnu, void* edx, const wchar_t* name, int remember);
Open_t o_open = nullptr;

// The open screen's name as MNU holds it (for the log only: after a flight the exe opens the Debrief another way, so
// this may still name the screen before the flight). SEH only.
void current_name(const void* mnu, wchar_t* out, size_t n) {
    out[0] = L'\0';
    __try {
        wcsncpy_s(out, n, reinterpret_cast<const wchar_t*>(static_cast<const BYTE*>(mnu) + kMnuCurrentName), _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = L'\0';
    }
}

// The session Debrief's Accept: its screen code sends it to the screen the flight came from (Single Mission) or the
// main menu, never through a command we see; the open itself is turned to the lobby screen. Whether the Debrief is
// the open screen is our own record (its ini was read, no other screen's since).
int __fastcall h_open(void* mnu, void* edx, const wchar_t* name, int remember) {
    const bool debrief = lobby_debrief_return_item(nullptr) > 0;
    if (name && (debrief || net_session_role() != NetRole::None)) {
        wchar_t cur[64];
        current_name(mnu, cur, _countof(cur));
        LOG("lobby: screen open %ls (MNU's current %ls%s)", name, cur, debrief ? "; the session Debrief is open" : "");
    }
    if (name && debrief && (_wcsicmp(name, L"SingleMissionScreen") == 0 || _wcsicmp(name, L"MainScreen") == 0)) {
        const wchar_t* lobby = net_session_role() == NetRole::Host ? kLobbyHostScreen : kLobbyJoinScreen;
        LOG("lobby: Debrief Accept: %ls opens instead of %ls", lobby, name);
        lobby_debrief_closed();
        return o_open(mnu, edx, lobby, remember);
    }
    return o_open(mnu, edx, name, remember);
}

void hook_screen_open() {
    static bool done = false;
    HMODULE m = GetModuleHandleW(L"MenuSystem.dll");
    if (done || !m) return;
    done = true;  // MenuSystem.dll stays loaded: hooked once
    const auto* target = static_cast<const BYTE*>(at_rva(m, kMnuOpenRva));
    if (!is_expected_build(m, kMnuStamp, kMnuSize, "MenuSystem.dll") ||
        std::memcmp(target, kOpenPrologue, sizeof(kOpenPrologue)) != 0) {
        LOG("lobby: MenuSystem screen open not hooked (build or prologue); the debrief's Accept stays stock");
        return;
    }
    create_and_enable(at_rva(m, kMnuOpenRva), reinterpret_cast<void*>(&h_open), reinterpret_cast<void**>(&o_open),
                      "MenuSystem screen open");
}

bool still_hangar(HMODULE base) {
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(base), &h) ||
        h != base)
        return false;
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(base, path, MAX_PATH);
    static constexpr wchar_t kName[] = L"\\HangarScreen.dll";
    constexpr DWORD kLen = static_cast<DWORD>(_countof(kName) - 1);
    return n >= kLen && n < MAX_PATH &&
           CompareStringOrdinal(path + n - kLen, static_cast<int>(kLen), kName, static_cast<int>(kLen), TRUE) == CSTR_EQUAL;
}

}  // namespace

void lobby_flow_on_modules_loaded() {
    if (!lobby_menu_code_path().empty()) hook_screen_open();  // only where the lobby can exist at all
    HMODULE m = GetModuleHandleW(L"HangarScreen.dll");
    if (!m) return;
    auto* target = static_cast<const BYTE*>(at_rva(m, kHangarEventRva));
    if (m == g_base && *target == 0xE9) return;  // hooked (MinHook writes a JMP rel32)
    AcquireSRWLockExclusive(&g_lock);
    if (!(m == g_base && *target == 0xE9)) {
        static HMODULE bad = nullptr;
        if (m != bad) {
            if (g_base && g_base != m && still_hangar(g_base)) (void)MH_RemoveHook(at_rva(g_base, kHangarEventRva));
            g_base = nullptr;
            if (!is_expected_build(m, kHangarStamp, kHangarSize, "HangarScreen.dll") ||
                std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
                bad = m;
                LOG("lobby: HangarScreen.dll not hooked (build or prologue); in a session its Fly starts the flight");
            } else {
                (void)MH_RemoveHook(at_rva(m, kHangarEventRva));  // an entry left by an earlier image at this base
                if (create_and_enable(at_rva(m, kHangarEventRva), reinterpret_cast<void*>(&h_event),
                                      reinterpret_cast<void**>(&o_event), "HangarScreen event handler"))
                    g_base = m;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace LazyWorks::SF2Patch::hooks
