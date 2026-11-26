// Background simulation in a network session (Borderless): a focus loss leaves the game's APP active, so the
// flight keeps stepping and presenting; only input is released. See lazy-sf2-recon: network-recon §8.1.
#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>

#include "config.h"
#include "hooks/focus_logic.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

constexpr DWORD kExeTimestamp = 0x51d22110, kExeImageSize = 0x62000;
constexpr DWORD kFocusHandlerRva = 0x16b0;  // __cdecl void(int on), called by the WndProc on (de)activation
constexpr DWORD kAppGlobalRva = 0x49cbc;    // APP service pointer
constexpr DWORD kSysGlobalRva = 0x49cc0;    // service registry pointer
constexpr DWORD kInpTagRva = 0x30284;       // L"INP", the tag the exe itself passes
constexpr size_t kAppActive = 0x1c, kAppBusy = 0x20;
constexpr int kSysGet = 0x08 / 4, kSysRelease = 0x0c / 4, kInpSetActive = 0x04 / 4;
constexpr UINT_PTR kSessionTimer = 0x4C5A4247;  // our timer on the game window while kept running
constexpr UINT kSessionPollMs = 250;

using FocusHandler_t = void(__cdecl*)(int on);
// __thiscall game calls through the repo's __fastcall pattern (ecx = this, edx unused).
using SysGet_t = int(__fastcall*)(void* self, void* edx, const wchar_t* tag, void** out);
using SysRelease_t = void(__fastcall*)(void* self, void* edx, const wchar_t* tag, void** p);
using InpSetActive_t = void(__fastcall*)(void* self, void* edx, int on);

HMODULE g_exe = nullptr;
FocusHandler_t o_FocusHandler = nullptr;
std::atomic<bool> g_kept_running{false};  // a focus loss was skipped; window thread only, atomic for clarity

void* read_ptr(DWORD rva) { return *static_cast<void**>(at_rva(g_exe, rva)); }

// APP is active and not inside SetActive: only then would the stock handler pause.
bool app_active_idle() {
    auto* app = static_cast<const BYTE*>(read_ptr(kAppGlobalRva));
    if (!app) return false;
    return *reinterpret_cast<const int*>(app + kAppActive) != 0 && *reinterpret_cast<const int*>(app + kAppBusy) == 0;
}

// What the stock handler does for input: INP slot 1 (DirectInput acquire / unacquire).
void inp_set_active(int on) {
    void* sys = read_ptr(kSysGlobalRva);
    if (!sys) return;
    const auto* tag = static_cast<const wchar_t*>(at_rva(g_exe, kInpTagRva));
    void* inp = nullptr;
    if (!reinterpret_cast<SysGet_t>(vtable_slot(sys, kSysGet))(sys, nullptr, tag, &inp) || !inp) return;
    reinterpret_cast<InpSetActive_t>(vtable_slot(inp, kInpSetActive))(inp, nullptr, on);
    reinterpret_cast<SysRelease_t>(vtable_slot(sys, kSysRelease))(sys, nullptr, tag, &inp);
}

void stop_session_timer() {
    if (HWND hwnd = game_window()) KillTimer(hwnd, kSessionTimer);
}

// Window thread (dispatched by the game's pump). The session ended while unfocused: hand the focus loss back to the
// game, which then pauses as stock.
void CALLBACK session_timer(HWND hwnd, UINT, UINT_PTR, DWORD) {
    if (!g_kept_running.load() || net_session_active()) return;
    if (GetForegroundWindow() == hwnd) return;  // the player is back; the activation clears the state
    KillTimer(hwnd, kSessionTimer);
    g_kept_running = false;
    LOG("focus: session ended while unfocused: stock pause re-applied");
    PostMessageW(hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
}

// Why a session's focus loss pauses as stock, once per reason.
void log_decision_once(BgDecision d) {
    static std::atomic<bool> fullscreen{false}, iconic{false}, off{false};
    if (d == BgDecision::Fullscreen && !fullscreen.exchange(true)) {
        LOG("focus: background simulation: Borderless only (stock pause in Fullscreen)");
    } else if (d == BgDecision::Iconic && !iconic.exchange(true)) {
        LOG("focus: lost while minimised: stock pause (session)");
    } else if (d == BgDecision::OptionOff && !off.exchange(true)) {
        LOG("focus: background simulation off (LAZYSF2_BG_SIM=0): stock pause (session)");
    }
}

void __cdecl h_FocusHandler(int on) {
    if (on == 0) {
        HWND hwnd = game_window();
        const BgDecision d = bg_decision(net_session_active(), config::background_sim(), config::display_mode(),
                                         hwnd && IsIconic(hwnd));
        if (d == BgDecision::KeepRunning) {
            // WM_KILLFOCUS and WM_ACTIVATEAPP(0) both arrive: act on the first only.
            if (g_kept_running.load()) return;
            if (app_active_idle()) {
                g_kept_running = true;
                inp_set_active(0);
                cursor_clip_update("focus lost, simulation kept running");
                if (hwnd) SetTimer(hwnd, kSessionTimer, kSessionPollMs, &session_timer);
                LOG("focus lost: simulation kept running (session)");
                return;
            }
        } else {
            log_decision_once(d);
            if (g_kept_running.exchange(false)) {
                stop_session_timer();
                LOG("focus: background simulation ended (%s): stock pause",
                    d == BgDecision::NoSession ? "session over" : "no longer allowed");
            }
        }
    } else if (g_kept_running.exchange(false)) {
        stop_session_timer();
        inp_set_active(1);  // the stock handler does nothing now (APP never went inactive), so re-acquire ourselves
        cursor_clip_update("focus back");
        LOG("focus back: input re-acquired (simulation never paused)");
    }
    o_FocusHandler(on);
}

}  // namespace

bool install_focus_hooks() {
    g_exe = GetModuleHandleW(nullptr);
    if (!is_expected_build(g_exe, kExeTimestamp, kExeImageSize, "StrikeFighters2.exe")) return false;
    // Created here, enabled with MH_ALL_HOOKS by DllMain.
    const MH_STATUS s = MH_CreateHook(at_rva(g_exe, kFocusHandlerRva), reinterpret_cast<void*>(&h_FocusHandler),
                                      reinterpret_cast<void**>(&o_FocusHandler));
    LOG("focus: handler hook %s", s == MH_OK ? "created" : MH_StatusToString(s));
    return s == MH_OK;
}

}  // namespace LazyWorks::SF2Patch::hooks
