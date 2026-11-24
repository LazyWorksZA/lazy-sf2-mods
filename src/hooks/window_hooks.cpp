// The stock game window is already a monitor-sized WS_POPUP (recon-notes.md §4.1), but it is
// created WS_EX_TOPMOST and the exe re-asserts HWND_TOPMOST later (exe +0x5790, +0x5830).
// A topmost borderless window covers everything after Alt-Tab, so in borderless mode we drop
// topmost in both places. The game window handle is recorded in every mode.
#include <windows.h>

#include <MinHook.h>

#include <atomic>

#include "config.h"
#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {
namespace {

constexpr wchar_t kGameWindowClass[] = L"twWindowClass";

std::atomic<HWND> g_game_hwnd{nullptr};

using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU,
                                        HINSTANCE, LPVOID);
using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);

CreateWindowExW_t o_CreateWindowExW = nullptr;
SetWindowPos_t o_SetWindowPos = nullptr;

bool is_game_class(LPCWSTR cls) {
    return cls && !IS_INTRESOURCE(cls) && wcscmp(cls, kGameWindowClass) == 0;
}

HWND WINAPI h_CreateWindowExW(DWORD ex_style, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int w,
                              int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    const bool game = is_game_class(cls);
    const bool borderless = game && config::borderless();
    if (game) {
        LOG("window: game CreateWindowExW ex=0x%08lx style=0x%08lx rect=(%d,%d %dx%d)", ex_style, style, x, y, w,
            h);
        if (borderless) ex_style &= ~static_cast<DWORD>(WS_EX_TOPMOST);
    }
    HWND hwnd = o_CreateWindowExW(ex_style, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (game) {
        g_game_hwnd = hwnd;  // also needed by mouse Cursor mode in exclusive fullscreen
        LOG("window: game hwnd=%p%s", static_cast<void*>(hwnd), borderless ? " (WS_EX_TOPMOST removed)" : "");
    }
    return hwnd;
}

BOOL WINAPI h_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (hwnd && hwnd == g_game_hwnd.load() && after == HWND_TOPMOST && config::borderless()) {
        static std::atomic<int> logged{0};
        if (logged++ < 3) LOG("window: SetWindowPos HWND_TOPMOST -> HWND_NOTOPMOST (%d,%d %dx%d)", x, y, cx, cy);
        after = HWND_NOTOPMOST;
    }
    return o_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
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

bool install_window_hooks() {
    bool ok = hook_api(L"user32", "CreateWindowExW", reinterpret_cast<LPVOID>(&h_CreateWindowExW),
                       reinterpret_cast<LPVOID*>(&o_CreateWindowExW));
    ok &= hook_api(L"user32", "SetWindowPos", reinterpret_cast<LPVOID>(&h_SetWindowPos),
                   reinterpret_cast<LPVOID*>(&o_SetWindowPos));
    return ok;
}

}  // namespace lazysf2::hooks
