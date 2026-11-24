// DirectInput hooks: keyboard Windows-key handling and mouse "Cursor" mode.
//
// Keyboard: the game opens the system keyboard with DISCL_NONEXCLUSIVE | DISCL_FOREGROUND |
// DISCL_NOWINKEY (0x16, DirectInput.dll +0x42a0), which disables the Windows key while the game
// has focus. In Borderless mode we drop DISCL_NOWINKEY so the Start menu works; Fullscreen stays
// stock. input_apply_display_mode() re-applies it after a live display-mode switch.
//
// Mouse "Cursor" mode (lazysf2.ini [Mouse] Mode=Cursor):
// Stock behaviour (DirectInput.dll, July 2013): the system mouse is opened buffered, exclusive +
// foreground. Every frame the read at DirectInput.dll+0x51b0 adds the DIMOFS_X/Y deltas from
// GetDeviceData into the game's own cursor position (this[8], this[9]), clamps it to the screen and
// dispatches it. Remote-control tools (RustDesk, ...) inject absolute mouse positions, which
// DirectInput turns into bogus deltas, so the game cursor drifts on its own.
//
// Cursor mode:
//   * the mouse device is opened non-exclusive (foreground), so Windows keeps moving its cursor;
//   * GetDeviceData drops the X/Y delta records for the mouse (buttons and wheel pass through);
//   * just before each read we set the game's cursor position to the Windows cursor, in client
//     coordinates of the game window, and the read clamps and dispatches it as usual.
//
// The read hook targets a fixed RVA, so it is only installed when DirectInput.dll is the exact
// build we analysed (checked by PE timestamp and image size).
#include <windows.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <MinHook.h>

#include <atomic>

#include "config.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {
namespace {

// Core\DirectInput.dll, July 2013 build.
constexpr DWORD kDirectInputTimestamp = 0x51d21fcc;
constexpr DWORD kDirectInputImageSize = 0x30000;
constexpr DWORD kMouseReadRva = 0x51b0;  // __fastcall int read(MouseState* this)
// MouseState layout (int-sized fields): [0] device, [4..7] range left/top/right/bottom (inclusive;
// set by the game through DirectInput.dll+0x2840, so it is a virtual space, not screen pixels),
// [8] x, [9] y, [10] wheel.
constexpr int kStateDevice = 0;
constexpr int kStateLeft = 4;
constexpr int kStateTop = 5;
constexpr int kStateRight = 6;
constexpr int kStateBottom = 7;
constexpr int kStateX = 8;
constexpr int kStateY = 9;

// Defined here to avoid linking dxguid.lib.
constexpr GUID kGuidSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kGuidSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// vtable slots (identical for the A and W interfaces)
constexpr int kDiCreateDevice = 3;
constexpr int kDevAcquire = 7;
constexpr int kDevUnacquire = 8;
constexpr int kDevGetDeviceData = 10;
constexpr int kDevSetCooperativeLevel = 13;

// Cursor mode is active only once its fixed-RVA read hook is installed; until then (or if the
// DirectInput.dll build is not the one we analysed) the mouse stays completely stock.
std::atomic<bool> g_cursor_active{false};
bool cursor_mode() { return g_cursor_active.load(); }

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFGUID, IUnknown**, LPUNKNOWN);
using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, DWORD, DIDEVICEOBJECTDATA*, DWORD*, DWORD);
using SetCooperativeLevel_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, HWND, DWORD);
using MouseRead_t = int(__fastcall*)(int* state);

DirectInput8Create_t o_DirectInput8Create = nullptr;
CreateDevice_t o_CreateDevice = nullptr;
GetDeviceData_t o_GetDeviceData = nullptr;
SetCooperativeLevel_t o_SetCooperativeLevel = nullptr;
MouseRead_t o_MouseRead = nullptr;

// Every system-mouse device created (the game creates more than one; only one is read).
constexpr int kMaxMice = 8;
std::atomic<IUnknown*> g_mice[kMaxMice] = {};

bool is_mouse(IUnknown* dev) {
    if (!dev) return false;
    for (auto& m : g_mice)
        if (m.load() == dev) return true;
    return false;
}

// Device pointers get reused (a released mouse's address can come back as a keyboard), so a
// pointer must be forgotten as a mouse when it is created as something else.
void remove_mouse(IUnknown* dev) {
    for (auto& m : g_mice) {
        IUnknown* expected = dev;
        m.compare_exchange_strong(expected, nullptr);
    }
}

void add_mouse(IUnknown* dev) {
    for (auto& m : g_mice) {
        IUnknown* expected = nullptr;
        if (m.load() == dev || m.compare_exchange_strong(expected, dev)) return;
    }
    LOG("mouse: more than %d mouse devices; ignoring %p", kMaxMice, static_cast<void*>(dev));
}

std::atomic<bool> g_dinput_hooked{false};
std::atomic<bool> g_device_hooked{false};
std::atomic<bool> g_di_hooked{false};
SRWLOCK g_lock = SRWLOCK_INIT;

// System keyboards. The game creates more than one; the one it configures with
// SetCooperativeLevel (g_keyboard) is the one in use, and the one re-applied on a live switch.
std::atomic<IUnknown*> g_keyboard_created{nullptr};  // most recent CreateDevice(GUID_SysKeyboard)
std::atomic<IUnknown*> g_keyboard{nullptr};
HWND g_keyboard_hwnd = nullptr;
DWORD g_keyboard_flags = 0;  // as requested by the game

DWORD keyboard_flags_for_mode(DWORD requested) {
    return config::borderless() ? (requested & ~static_cast<DWORD>(DISCL_NOWINKEY)) : requested;
}

// --- IDirectInputDevice8 ---------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE h_SetCooperativeLevel(IUnknown* self, HWND hwnd, DWORD flags) {
    if (cursor_mode() && is_mouse(self) && (flags & DISCL_EXCLUSIVE)) {
        DWORD patched = (flags & ~static_cast<DWORD>(DISCL_EXCLUSIVE)) | DISCL_NONEXCLUSIVE;
        LOG("mouse: SetCooperativeLevel 0x%lx -> 0x%lx (non-exclusive)", flags, patched);
        flags = patched;
    } else if (self && (self == g_keyboard_created.load() || self == g_keyboard.load())) {
        g_keyboard = self;
        g_keyboard_hwnd = hwnd;
        g_keyboard_flags = flags;
        const DWORD patched = keyboard_flags_for_mode(flags);
        LOG("input: keyboard SetCooperativeLevel 0x%lx -> 0x%lx%s", flags, patched,
            patched != flags ? " (Windows key allowed: borderless)" : "");
        flags = patched;
    }
    return o_SetCooperativeLevel(self, hwnd, flags);
}

HRESULT STDMETHODCALLTYPE h_GetDeviceData(IUnknown* self, DWORD cb, DIDEVICEOBJECTDATA* data, DWORD* inout,
                                          DWORD flags) {
    HRESULT hr = o_GetDeviceData(self, cb, data, inout, flags);
    if (!cursor_mode() || !is_mouse(self) || FAILED(hr) || !data || !inout || cb < sizeof(DWORD) * 2) return hr;

    // Drop the relative X/Y records; keep everything else in order.
    auto* bytes = reinterpret_cast<BYTE*>(data);
    DWORD kept = 0;
    for (DWORD i = 0; i < *inout; ++i) {
        auto* rec = reinterpret_cast<DIDEVICEOBJECTDATA*>(bytes + i * cb);
        if (rec->dwOfs == DIMOFS_X || rec->dwOfs == DIMOFS_Y) continue;
        if (kept != i) memmove(bytes + kept * cb, rec, cb);
        ++kept;
    }
    *inout = kept;
    return hr;
}

void hook_device(IUnknown* dev) {
    if (g_device_hooked.exchange(true)) return;
    // Keyboard, mouse and joystick devices share these vtables; the detours check which one it is.
    create_and_enable(vtable_slot(dev, kDevSetCooperativeLevel), reinterpret_cast<void*>(&h_SetCooperativeLevel),
                      reinterpret_cast<void**>(&o_SetCooperativeLevel), "IDirectInputDevice8::SetCooperativeLevel");
    create_and_enable(vtable_slot(dev, kDevGetDeviceData), reinterpret_cast<void*>(&h_GetDeviceData),
                      reinterpret_cast<void**>(&o_GetDeviceData), "IDirectInputDevice8::GetDeviceData");
}

// --- IDirectInput8 -----------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE h_CreateDevice(IUnknown* self, REFGUID guid, IUnknown** out, LPUNKNOWN outer) {
    HRESULT hr = o_CreateDevice(self, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        IUnknown* dev = *out;
        if (dev == g_keyboard.load()) g_keyboard = nullptr;  // that keyboard was released; address reused
        if (dev == g_keyboard_created.load()) g_keyboard_created = nullptr;
        remove_mouse(dev);
        if (IsEqualGUID(guid, kGuidSysMouse)) {
            LOG("input: system mouse device %p", static_cast<void*>(dev));
            if (cursor_mode()) add_mouse(dev);
            hook_device(dev);
        } else if (IsEqualGUID(guid, kGuidSysKeyboard)) {
            LOG("input: system keyboard device %p", static_cast<void*>(dev));
            g_keyboard_created = dev;
            hook_device(dev);
        }
    }
    return hr;
}

HRESULT WINAPI h_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    HRESULT hr = o_DirectInput8Create(inst, version, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out && !g_di_hooked.exchange(true)) {
        create_and_enable(vtable_slot(*out, kDiCreateDevice), reinterpret_cast<void*>(&h_CreateDevice),
                          reinterpret_cast<void**>(&o_CreateDevice), "IDirectInput8::CreateDevice");
    }
    return hr;
}

// --- Hide the Windows cursor over the game (the game draws its own) ---------------------------

WNDPROC g_orig_wndproc = nullptr;

LRESULT CALLBACK cursor_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);
        return TRUE;
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
}

void hide_windows_cursor(HWND hwnd) {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    g_orig_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&cursor_wndproc)));
    LOG("mouse: game window subclassed to hide the Windows cursor (%s)", g_orig_wndproc ? "ok" : "FAILED");
}

// --- DirectInput.dll per-frame mouse read ------------------------------------------------------

// The menu system lays screens out in a 1024x768 virtual space (MenuSystem.ini Width/Height) and
// sets the cursor range to that space minus the cursor image, e.g. (0,0)-(1007,751). The game's
// own read clamps x/y to the range afterwards.
constexpr int kMenuVirtualWidth = 1024;
constexpr int kMenuVirtualHeight = 768;

// Maps a client-area pixel onto the game's cursor space. `virtual_extent` is the size of the space
// the client area represents; values past the range are clamped by the game.
int map_axis(LONG pixel, LONG client_extent, int lo, int virtual_extent) {
    if (client_extent <= 0 || virtual_extent <= 0) return lo;
    if (pixel < 0) pixel = 0;
    if (pixel > client_extent - 1) pixel = client_extent - 1;
    return lo + static_cast<int>(static_cast<long long>(pixel) * virtual_extent / client_extent);
}

// Size of the space the whole client area stands for: the 1024x768 menu space when the range fits
// inside it, otherwise the range itself (e.g. if flight uses screen-sized ranges).
int virtual_extent(int lo, int hi, int menu_extent) {
    return (lo == 0 && hi < menu_extent) ? menu_extent : hi - lo + 1;
}

int __fastcall h_MouseRead(int* state) {
    HWND hwnd = game_window();
    if (state && hwnd && is_mouse(reinterpret_cast<IUnknown*>(state[kStateDevice]))) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) LOG("mouse: game reads device %p; following the Windows cursor",
                                        reinterpret_cast<void*>(state[kStateDevice]));
        hide_windows_cursor(hwnd);

        static int last_range[4] = {};
        if (memcmp(last_range, state + kStateLeft, sizeof(last_range)) != 0) {
            memcpy(last_range, state + kStateLeft, sizeof(last_range));
            LOG("mouse: game cursor range (%d,%d)-(%d,%d)", last_range[0], last_range[1], last_range[2], last_range[3]);
        }

        POINT p;
        RECT rc;
        if (GetCursorPos(&p) && ScreenToClient(hwnd, &p) && GetClientRect(hwnd, &rc)) {
            const int vw = virtual_extent(state[kStateLeft], state[kStateRight], kMenuVirtualWidth);
            const int vh = virtual_extent(state[kStateTop], state[kStateBottom], kMenuVirtualHeight);
            state[kStateX] = map_axis(p.x, rc.right - rc.left, state[kStateLeft], vw);
            state[kStateY] = map_axis(p.y, rc.bottom - rc.top, state[kStateTop], vh);
        }
    }
    return o_MouseRead(state);
}

}  // namespace

void input_on_modules_loaded() {
    if (g_dinput_hooked.load()) return;
    HMODULE dinput8 = GetModuleHandleW(L"dinput8.dll");
    HMODULE game_di = GetModuleHandleW(L"DirectInput.dll");  // Core\DirectInput.dll (the game's)
    if (!dinput8 || !game_di) return;

    AcquireSRWLockExclusive(&g_lock);
    if (!g_dinput_hooked.exchange(true)) {
        // Mouse Cursor mode needs the fixed-RVA read hook; without it, leave the mouse stock.
        if (config::get().mouse == config::MouseMode::Cursor) {
            if (is_expected_build(game_di, kDirectInputTimestamp, kDirectInputImageSize, "DirectInput.dll") &&
                create_and_enable(at_rva(game_di, kMouseReadRva), reinterpret_cast<void*>(&h_MouseRead),
                                  reinterpret_cast<void**>(&o_MouseRead), "DirectInput.dll+0x51b0 (mouse read)")) {
                g_cursor_active = true;
            } else {
                LOG("mouse: Cursor mode unavailable; mouse stays stock");
            }
        }
        // Device hooks (keyboard Windows key, and mouse Cursor mode when enabled).
        if (void* create = reinterpret_cast<void*>(GetProcAddress(dinput8, "DirectInput8Create"))) {
            create_and_enable(create, reinterpret_cast<void*>(&h_DirectInput8Create),
                              reinterpret_cast<void**>(&o_DirectInput8Create), "DirectInput8Create");
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void input_apply_display_mode() {
    IUnknown* kb = g_keyboard.load();
    if (!kb || !g_keyboard_hwnd || !o_SetCooperativeLevel) return;
    using Simple_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*);
    // The cooperative level can only change while unacquired; the keyboard was acquired (in use).
    reinterpret_cast<Simple_t>(vtable_slot(kb, kDevUnacquire))(kb);
    const DWORD flags = keyboard_flags_for_mode(g_keyboard_flags);
    HRESULT hr = o_SetCooperativeLevel(kb, g_keyboard_hwnd, flags);
    HRESULT acq = reinterpret_cast<Simple_t>(vtable_slot(kb, kDevAcquire))(kb);
    LOG("input: keyboard cooperative level 0x%lx -> 0x%08lx, Acquire -> 0x%08lx", flags, hr, acq);
}

}  // namespace lazysf2::hooks
