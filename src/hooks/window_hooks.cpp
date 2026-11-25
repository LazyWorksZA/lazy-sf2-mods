// Game window hooks: Borderless (drop topmost), the Alt-Tab fix, the window handle, and a subclass that applies
// display-mode changes on the window thread. See lazy-sf2-recon: recon-notes §4.1, §12.
#include <windows.h>

#include <MinHook.h>

#include <intrin.h>

#include <atomic>

#include "config.h"
#include "monitors.h"
#include "hooks/cursor_clip_logic.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {
namespace {

constexpr wchar_t kGameWindowClass[] = L"twWindowClass";

std::atomic<HWND> g_game_hwnd{nullptr};

using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU,
                                        HINSTANCE, LPVOID);
using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);

using ShowWindow_t = BOOL(WINAPI*)(HWND, int);

CreateWindowExW_t o_CreateWindowExW = nullptr;
SetWindowPos_t o_SetWindowPos = nullptr;
ShowWindow_t o_ShowWindow = nullptr;

bool is_game_class(LPCWSTR cls) {
    return cls && !IS_INTRESOURCE(cls) && wcscmp(cls, kGameWindowClass) == 0;
}

// Private message: apply the current display mode on the window thread (DXGI fullscreen changes
// send messages to the window, so they must not run on another thread while it waits).
// Registered when the game window is created (not at static-init time, which runs in DllMain).
UINT g_apply_msg = 0;
WNDPROC g_game_wndproc = nullptr;

std::atomic<bool> g_app_active{false};  // from WM_ACTIVATEAPP; GetForegroundWindow lags behind that message
SRWLOCK g_clip_lock = SRWLOCK_INIT;
bool g_clip_set = false;  // guarded by g_clip_lock: we hold the system clip, and it is g_clip_rect
RECT g_clip_rect{};

bool same_rect(const RECT& a, const RECT& b) { return memcmp(&a, &b, sizeof(RECT)) == 0; }

// Releases the clip only if it is still ours; another app's clip is left alone.
void release_clip_locked(const char* why) {
    if (!g_clip_set) return;
    g_clip_set = false;
    RECT cur{};
    if (GetClipCursor(&cur) && same_rect(cur, g_clip_rect)) {
        ClipCursor(nullptr);
        LOG("cursor: clip off (%s)", why);
    } else {
        LOG("cursor: clip no longer ours, left alone (%s)", why);
    }
}

// One line per focus change (WM_ACTIVATEAPP is rare), for Alt-Tab diagnostics.
void log_window_state(const char* when, HWND hwnd) {
    RECT r{};
    GetWindowRect(hwnd, &r);
    LOG("window: %s iconic=%d visible=%d fg=%d topmost=%d rect=(%ld,%ld)-(%ld,%ld) exclusive=%d", when, IsIconic(hwnd),
        IsWindowVisible(hwnd), GetForegroundWindow() == hwnd, (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
        r.left, r.top, r.right, r.bottom, dxgi_is_exclusive());
}

LRESULT CALLBACK game_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_apply_msg && g_apply_msg != 0) {
        dxgi_apply_display_mode();
        cursor_clip_update("display mode applied");
        return 0;
    }
    if (msg == WM_ACTIVATEAPP) g_app_active = wp != 0;
    if (msg == WM_DESTROY) {
        AcquireSRWLockExclusive(&g_clip_lock);
        release_clip_locked("window destroyed");
        ReleaseSRWLockExclusive(&g_clip_lock);
    }
    if (msg == WM_DISPLAYCHANGE) {
        LOG("window: WM_DISPLAYCHANGE %ux%u @ %u bpp", LOWORD(lp), HIWORD(lp), static_cast<unsigned>(wp));
    } else if (msg == WM_POWERBROADCAST) {
        LOG("window: WM_POWERBROADCAST 0x%lx", static_cast<unsigned long>(wp));
    }
    LRESULT r = CallWindowProcW(g_game_wndproc, hwnd, msg, wp, lp);
    if (msg == WM_ACTIVATEAPP) log_window_state(wp ? "after activate" : "after deactivate", hwnd);
    if (msg == WM_DISPLAYCHANGE) log_window_state("after display change", hwnd);
    switch (msg) {
        case WM_ACTIVATEAPP: cursor_clip_update(wp ? "activated" : "deactivated"); break;
        case WM_ACTIVATE: cursor_clip_update("WM_ACTIVATE"); break;
        case WM_SIZE: cursor_clip_update("WM_SIZE"); break;
        case WM_WINDOWPOSCHANGED: cursor_clip_update("WM_WINDOWPOSCHANGED"); break;
        case WM_DISPLAYCHANGE: cursor_clip_update("WM_DISPLAYCHANGE"); break;
        default: break;
    }
    return r;
}

// The monitor chosen in Options (Screen), or false for the stock choice.
bool chosen_monitor_rect(RECT* r) {
    const int n = config::display_monitor();
    if (n <= 0) return false;
    if (monitors::rect_of(n, r)) return true;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) LOG("window: Screen %d not found (unplugged?); using the stock monitor", n);
    return false;
}

bool called_from_exe(const void* ret) {
    HMODULE m = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(ret), &m) &&
           m == GetModuleHandleW(nullptr);
}

HWND WINAPI h_CreateWindowExW(DWORD ex_style, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w,
                              int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    const bool game = is_game_class(cls);
    const bool borderless = game && config::borderless();
    if (game) {
        LOG("window: game CreateWindowExW ex=0x%08lx style=0x%08lx rect=(%d,%d %dx%d)", ex_style, style, x, y, w,
            h);
        if (borderless) ex_style &= ~static_cast<DWORD>(WS_EX_TOPMOST);
        // Screen choice: the stock full-screen popup covers the monitor holding [Window] WindowPos;
        // put it on the chosen monitor instead (the exe's later repositioning is handled below).
        RECT r;
        if ((style & WS_POPUP) && !(style & WS_CAPTION) && chosen_monitor_rect(&r)) {
            x = r.left;
            y = r.top;
            w = r.right - r.left;
            h = r.bottom - r.top;
            LOG("window: placed on Screen %d (%d,%d %dx%d)", config::display_monitor(), x, y, w, h);
        }
    }
    HWND hwnd = o_CreateWindowExW(ex_style, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (game && hwnd) {
        g_game_hwnd = hwnd;  // also needed by mouse Cursor mode in exclusive fullscreen
        g_apply_msg = RegisterWindowMessageW(L"lazysf2.ApplyDisplayMode");
        g_game_wndproc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&game_subclass_proc)));
        LOG("window: game hwnd=%p%s, subclassed: %s", static_cast<void*>(hwnd),
            borderless ? " (WS_EX_TOPMOST removed)" : "", g_game_wndproc ? "ok" : "FAILED");
    }
    return hwnd;
}

BOOL WINAPI h_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    // The exe re-applies the monitor rect it computed at start-up (+0x5790, +0x5830); keep the window
    // on the chosen Screen. Calls from DXGI (exclusive-mode sizing) are left alone.
    RECT r;
    if (hwnd && hwnd == g_game_hwnd.load() && !(flags & SWP_NOMOVE) && called_from_exe(_ReturnAddress()) &&
        chosen_monitor_rect(&r)) {
        x = r.left;
        y = r.top;
        cx = r.right - r.left;
        cy = r.bottom - r.top;
    }
    if (hwnd && hwnd == g_game_hwnd.load() && after == HWND_TOPMOST && config::borderless()) {
        static std::atomic<int> logged{0};
        if (logged++ < 3) LOG("window: SetWindowPos HWND_TOPMOST -> HWND_NOTOPMOST (%d,%d %dx%d)", x, y, cx, cy);
        after = HWND_NOTOPMOST;
    }
    return o_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

// Alt-Tab fix: the exe calls ShowWindow(SW_MAXIMIZE) on every activation (+0x5cee), after which Windows never
// performs the restore from its own minimize (+0x5819) and the activation retry loop spins forever.
// SW_SHOWNORMAL gives the same full-monitor window without the maximized flag. See lazy-sf2-recon: recon-notes §12.
BOOL WINAPI h_ShowWindow(HWND hwnd, int cmd) {
    // Borderless: on focus loss the exe minimizes itself (+0x5819, SW_SHOWMINNOACTIVE). Keep the
    // window visible behind the Start menu / other windows instead; the game still pauses as stock.
    // (Minimizing via the taskbar or Win+D doesn't go through this call and still works.)
    if (hwnd && hwnd == g_game_hwnd.load() && cmd == SW_SHOWMINNOACTIVE && config::borderless()) {
        static std::atomic<int> logged{0};
        if (logged++ < 3) LOG("window: self-minimize on focus loss skipped (borderless)");
        return IsWindowVisible(hwnd);
    }
    if (hwnd && hwnd == g_game_hwnd.load() && cmd == SW_MAXIMIZE) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            char caller[96];
            describe_address(_ReturnAddress(), caller, sizeof(caller));
            LOG("window: ShowWindow(SW_MAXIMIZE) from %s -> SW_SHOWNORMAL (Alt-Tab fix)", caller);
        }
        cmd = SW_SHOWNORMAL;
    }
    return o_ShowWindow(hwnd, cmd);
}

bool hook_api(LPCWSTR module, LPCSTR proc, LPVOID detour, LPVOID* original) {
    MH_STATUS s = MH_CreateHookApi(module, proc, detour, original);
    if (s != MH_OK) {
        LOG("window: hook %s failed: %s", proc, MH_StatusToString(s));
        return false;
    }
    return true;
}

}  // namespace

HWND game_window() { return g_game_hwnd.load(); }

void cursor_clip_update(const char* why) {
    HWND hwnd = g_game_hwnd.load();
    if (!hwnd) return;
    RECT rc{};
    POINT tl{}, br{};
    bool have_rect = false;
    if (GetClientRect(hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
        br = {rc.right, rc.bottom};
        have_rect = ClientToScreen(hwnd, &tl) && ClientToScreen(hwnd, &br);
        rc = {tl.x, tl.y, br.x, br.y};
    }
    const ClipInputs in{!config::borderless(), mouse_cursor_mode_active(), g_app_active.load(), IsIconic(hwnd) != FALSE,
                        have_rect};
    AcquireSRWLockExclusive(&g_clip_lock);
    if (want_clip(in)) {
        RECT cur{};
        if (!g_clip_set || !same_rect(rc, g_clip_rect) || !GetClipCursor(&cur) || !same_rect(cur, rc)) {
            const bool changed = !g_clip_set || !same_rect(rc, g_clip_rect);
            if (ClipCursor(&rc)) {
                g_clip_set = true;
                g_clip_rect = rc;
                if (changed)
                    LOG("cursor: clip on (%s) (%ld,%ld)-(%ld,%ld)", why, rc.left, rc.top, rc.right, rc.bottom);
            }
        }
    } else {
        release_clip_locked(why);
    }
    ReleaseSRWLockExclusive(&g_clip_lock);
}

bool window_chosen_screen(RECT* r) { return chosen_monitor_rect(r); }

bool window_needs_move(RECT* target) {
    HWND hwnd = g_game_hwnd.load();
    if (!hwnd || !chosen_monitor_rect(target)) return false;
    // Compare monitors, not exact rects: in exclusive mode DXGI may leave the window rect slightly
    // different from the monitor rect, and a needless move would leave and re-enter exclusive mode.
    return monitors::number_of_window(hwnd) != config::display_monitor();
}


void request_display_mode_apply() {
    HWND hwnd = g_game_hwnd.load();
    if (!hwnd || !g_game_wndproc || !g_apply_msg) {
        LOG("window: cannot apply the display mode live (no game window)");
        return;
    }
    PostMessageW(hwnd, g_apply_msg, 0, 0);
}

bool install_window_hooks() {
    bool ok = hook_api(L"user32", "CreateWindowExW", reinterpret_cast<LPVOID>(&h_CreateWindowExW),
                       reinterpret_cast<LPVOID*>(&o_CreateWindowExW));
    ok &= hook_api(L"user32", "SetWindowPos", reinterpret_cast<LPVOID>(&h_SetWindowPos),
                   reinterpret_cast<LPVOID*>(&o_SetWindowPos));
    ok &= hook_api(L"user32", "ShowWindow", reinterpret_cast<LPVOID>(&h_ShowWindow),
                   reinterpret_cast<LPVOID*>(&o_ShowWindow));
    return ok;
}

}  // namespace lazysf2::hooks
