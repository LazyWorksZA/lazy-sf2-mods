// What the game does when its window loses focus ([OtherOptions] BackgroundMode). Pause is stock. Keep running (in
// Borderless): the focus loss leaves the game's APP active, so the flight keeps stepping and presenting; only input is
// released. While the kept-running window is minimised, our own frame limiter in the message pump stands in for the
// vsync wait. See lazy-sf2-recon: network-recon §8.1.
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
constexpr DWORD kAppPumpRva = 0x5b20;       // APP vtable slot 0: the message pump, once per frame
constexpr DWORD kAppGlobalRva = 0x49cbc;    // APP service pointer
constexpr DWORD kSysGlobalRva = 0x49cc0;    // service registry pointer
constexpr DWORD kInpTagRva = 0x30284;       // L"INP", the tag the exe itself passes
constexpr size_t kAppActive = 0x1c, kAppBusy = 0x20;
constexpr int kSysGet = 0x08 / 4, kSysRelease = 0x0c / 4, kInpSetActive = 0x04 / 4;
constexpr UINT_PTR kReasonTimer = 0x4C5A4247;  // our timer on the game window while kept running
constexpr UINT kReasonPollMs = 250;

using FocusHandler_t = void(__cdecl*)(int on);
// __thiscall game calls through the repo's __fastcall pattern (ecx = this, edx unused).
using AppPump_t = int(__fastcall*)(void* self, void* edx);
using SysGet_t = int(__fastcall*)(void* self, void* edx, const wchar_t* tag, void** out);
using SysRelease_t = void(__fastcall*)(void* self, void* edx, const wchar_t* tag, void** p);
using InpSetActive_t = void(__fastcall*)(void* self, void* edx, int on);

HMODULE g_exe = nullptr;
FocusHandler_t o_FocusHandler = nullptr;
AppPump_t o_AppPump = nullptr;
// Window (= game) thread only; atomic so the pump's check is a plain relaxed load.
std::atomic<bool> g_kept_running{false};
bool g_pump_hooked = false;  // set once in DllMain; without it a minimised window falls back to the stock pause

/// <summary>
/// Further reasons to keep running besides the player's BackgroundMode. None on master. When net-background merges:
/// return net_session_active() and config::background_sim() (window thread; must stay cheap).
/// </summary>
bool focus_keep_running_reason() { return false; }

bool player_option() { return config::background_mode() != config::BackgroundMode::Pause; }

BgDecision decide() { return bg_decision(player_option(), focus_keep_running_reason(), config::display_mode()); }

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

// The stock pause is about to run. Buffers made with GLOBALFOCUS would keep looping through it (the stock focus path
// never tells SND), so stop the game's channels first; the game starts its looped sounds again when it resumes.
void before_stock_pause() {
    if (sound_global_focus_active() && app_active_idle()) sound_stop_all_channels("stock pause");
}

void stop_reason_timer() {
    if (HWND hwnd = game_window()) KillTimer(hwnd, kReasonTimer);
}

// Window thread, dispatched by the game's pump like a WndProc message. When no reason to keep running is left (a
// network session ended, later), or the window is minimised and cannot be paced, hand the focus loss to the stock handler, which pauses. Called directly rather than
// posted, so a click back in between can't pause a foreground game.
void CALLBACK reason_timer(HWND hwnd, UINT, UINT_PTR, DWORD) {
    if (!g_kept_running.load()) return;
    const bool unpaced = !g_pump_hooked && IsIconic(hwnd);
    if (decide() == BgDecision::KeepRunning && !unpaced) return;
    if (GetForegroundWindow() == hwnd) return;  // the player is back; the activation clears the state
    KillTimer(hwnd, kReasonTimer);
    g_kept_running = false;
    LOG("focus: %s while unfocused: stock pause re-applied", unpaced ? "minimised without frame pacing" : "no reason left");
    before_stock_pause();
    o_FocusHandler(0);
}

void log_decision_once(BgDecision d) {
    static std::atomic<bool> fullscreen{false};
    if (d == BgDecision::Fullscreen && !fullscreen.exchange(true)) {
        LOG("focus: keep running is Borderless only: stock pause in Fullscreen");
    }
}

void __cdecl h_FocusHandler(int on) {
    if (on == 0) {
        HWND hwnd = game_window();
        // WM_DESTROY also arrives here with 0; DestroyWindow has hidden the window by then.
        const bool closing = !hwnd || !IsWindowVisible(hwnd);
        const BgDecision d = decide();
        if (d == BgDecision::KeepRunning && closing) {
            if (g_kept_running.exchange(false)) stop_reason_timer();
            if (!hwnd) {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true)) LOG("focus: no game window known: stock pause");
            }
        } else if (d == BgDecision::KeepRunning) {
            // WM_KILLFOCUS and WM_ACTIVATEAPP(0) both arrive: act on the first only.
            if (g_kept_running.load()) return;
            if (app_active_idle()) {
                g_kept_running = true;
                inp_set_active(0);
                cursor_clip_update("focus lost, game kept running");
                SetTimer(hwnd, kReasonTimer, kReasonPollMs, &reason_timer);
                LOG("focus lost: game kept running (input released)");
                return;
            }
        } else {
            log_decision_once(d);
            if (g_kept_running.exchange(false)) {
                stop_reason_timer();
                LOG("focus: keep running ended (no reason left): stock pause");
            }
        }
        before_stock_pause();
    } else if (g_kept_running.exchange(false)) {
        stop_reason_timer();
        inp_set_active(1);  // the stock handler does nothing now (APP never went inactive), so re-acquire ourselves
        cursor_clip_update("focus back");
        LOG("focus back: input re-acquired (game never paused)");
    }
    // For on == 1 this returns at once (APP is still active); a click activation passes 2, which re-runs SetActive and
    // the notifications with 2, exactly as stock does after WM_ACTIVATEAPP(1).
    o_FocusHandler(on);
}

// --- Frame limiter while minimised. A minimised window's Present returns at once (no vsync wait), so the loop would
// spin. The pump runs once per frame on the game thread; sleeping before it paces the whole frame.

int64_t now_us() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<int64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(c.QuadPart) * 1000000 / freq;
}

int64_t g_period_us = kPaceDefaultPeriodUs;  // game thread only
int64_t g_last_us = 0;
int64_t g_deadline_us = 0;
bool g_pacing = false;

void pace_frame() {
    const int64_t now = now_us();
    HWND hwnd = game_window();
    if (g_kept_running.load(std::memory_order_relaxed) && hwnd && IsIconic(hwnd)) {
        if (!g_pacing) {
            g_pacing = true;
            g_deadline_us = 0;
            LOG("focus: minimised while kept running: frames paced at %.1f Hz", 1e6 / static_cast<double>(g_period_us));
        }
        const PaceStep s = pace_step(now, g_deadline_us, g_period_us);
        g_deadline_us = s.deadline_us;
        if (s.wait_us > 0) Sleep(static_cast<DWORD>((s.wait_us + 999) / 1000));
        g_last_us = 0;  // paced frames are not measured
        return;
    }
    if (g_pacing) {
        g_pacing = false;
        LOG("focus: no longer minimised: frame pacing off");
    }
    if (g_last_us) g_period_us = pace_update_period(g_period_us, now - g_last_us);
    g_last_us = now;
}

int __fastcall h_AppPump(void* self, void* edx) {
    // Mode Pause with no other reason: one call and a relaxed load per frame, then stock.
    static const bool enabled = player_option();  // first pump call: game thread, outside the loader lock
    if (enabled || g_kept_running.load(std::memory_order_relaxed)) pace_frame();
    return o_AppPump(self, edx);
}

}  // namespace

bool install_focus_hooks() {
    g_exe = GetModuleHandleW(nullptr);
    if (!is_expected_build(g_exe, kExeTimestamp, kExeImageSize, "StrikeFighters2.exe")) return false;
    // Created here, enabled with MH_ALL_HOOKS by DllMain.
    MH_STATUS s = MH_CreateHook(at_rva(g_exe, kFocusHandlerRva), reinterpret_cast<void*>(&h_FocusHandler),
                                reinterpret_cast<void**>(&o_FocusHandler));
    LOG("focus: handler hook %s", s == MH_OK ? "created" : MH_StatusToString(s));
    if (s != MH_OK) return false;
    s = MH_CreateHook(at_rva(g_exe, kAppPumpRva), reinterpret_cast<void*>(&h_AppPump),
                      reinterpret_cast<void**>(&o_AppPump));
    g_pump_hooked = s == MH_OK;
    LOG("focus: pump hook %s", g_pump_hooked ? "created" : MH_StatusToString(s));
    return g_pump_hooked;
}

}  // namespace LazyWorks::SF2Patch::hooks
