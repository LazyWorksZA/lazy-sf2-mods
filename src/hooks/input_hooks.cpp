// DirectInput hooks: Windows key in Borderless, and the "Mouse Pointer" (Cursor) setting. Settings are read when
// the hooks run, so Options can change them live. See lazy-sf2-recon: recon-notes §10.
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
constexpr DWORD kMouseReadRva = 0x51b0;  // per-frame mouse read
// MouseState layout (int-sized fields): [0] device, [4..7] range left/top/right/bottom (inclusive, a virtual
// space, not screen pixels), [8] x, [9] y, [10] wheel.
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
constexpr int kDevRelease = 2;
constexpr int kDevAcquire = 7;
constexpr int kDevUnacquire = 8;
constexpr int kDevGetDeviceData = 10;
constexpr int kDevSetCooperativeLevel = 13;

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFGUID, IUnknown**, LPUNKNOWN);
using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, DWORD, DIDEVICEOBJECTDATA*, DWORD*, DWORD);
using SetCooperativeLevel_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, HWND, DWORD);
using MouseRead_t = int(__fastcall*)(int* state);
using Simple_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*);

DirectInput8Create_t o_DirectInput8Create = nullptr;
CreateDevice_t o_CreateDevice = nullptr;
GetDeviceData_t o_GetDeviceData = nullptr;
SetCooperativeLevel_t o_SetCooperativeLevel = nullptr;
using Release_t = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
Release_t o_Release = nullptr;
MouseRead_t o_MouseRead = nullptr;

std::atomic<bool> g_dinput_hooked{false};
std::atomic<bool> g_device_hooked{false};
std::atomic<bool> g_di_hooked{false};
std::atomic<bool> g_read_hooked{false};  // the fixed-RVA read hook is in place
SRWLOCK g_lock = SRWLOCK_INIT;

// "Match the Windows cursor" is active: chosen in Options and technically possible.
bool cursor_mode() { return g_read_hooked.load() && config::mouse_mode() == config::MouseMode::Cursor; }

// --- Device tracking. The game creates several keyboards and mice; the one it passes to SetCooperativeLevel
// is in use. Device pointers get reused, so a pointer is forgotten whenever it is created again.

struct Device {
    std::atomic<IUnknown*> created{nullptr};  // most recent CreateDevice for this kind
    std::atomic<IUnknown*> used{nullptr};     // the one the game configured
    HWND hwnd = nullptr;
    DWORD flags = 0;                          // cooperative level as the game asked for it
};
Device g_keyboard, g_mouse;

bool is(Device& d, IUnknown* dev) { return dev && (dev == d.created.load() || dev == d.used.load()); }

void forget(Device& d, IUnknown* dev) {
    IUnknown* expected = dev;
    d.created.compare_exchange_strong(expected, nullptr);
    expected = dev;
    d.used.compare_exchange_strong(expected, nullptr);
}

void forget(IUnknown* dev) {
    forget(g_keyboard, dev);
    forget(g_mouse, dev);
}

DWORD keyboard_flags(DWORD requested) {
    return config::borderless() ? (requested & ~static_cast<DWORD>(DISCL_NOWINKEY)) : requested;
}

DWORD mouse_flags(DWORD requested) {
    return cursor_mode() && (requested & DISCL_EXCLUSIVE)
               ? (requested & ~static_cast<DWORD>(DISCL_EXCLUSIVE)) | DISCL_NONEXCLUSIVE
               : requested;
}

// Records the configured device and returns the flags to really use.
DWORD configure(Device& d, IUnknown* self, HWND hwnd, DWORD flags, DWORD patched, const char* what) {
    d.used = self;
    d.hwnd = hwnd;
    d.flags = flags;
    LOG("input: %s SetCooperativeLevel 0x%lx -> 0x%lx", what, flags, patched);
    return patched;
}

HRESULT STDMETHODCALLTYPE h_SetCooperativeLevel(IUnknown* self, HWND hwnd, DWORD flags) {
    if (is(g_keyboard, self)) {
        flags = configure(g_keyboard, self, hwnd, flags, keyboard_flags(flags), "keyboard");
    } else if (is(g_mouse, self)) {
        flags = configure(g_mouse, self, hwnd, flags, mouse_flags(flags), "mouse");
    }
    return o_SetCooperativeLevel(self, hwnd, flags);
}

HRESULT STDMETHODCALLTYPE h_GetDeviceData(IUnknown* self, DWORD cb, DIDEVICEOBJECTDATA* data, DWORD* inout,
                                          DWORD flags) {
    HRESULT hr = o_GetDeviceData(self, cb, data, inout, flags);
    if (FAILED(hr) || !data || !inout || cb < sizeof(DWORD) * 2 || !is(g_mouse, self) || !cursor_mode()) return hr;

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

// When the game's last reference to a device goes, drop our pointer (no locks: Release can run on
// any thread). After the original returns 0 `self` is only compared, never used.
ULONG STDMETHODCALLTYPE h_Release(IUnknown* self) {
    const ULONG refs = o_Release(self);
    if (refs == 0 && (is(g_keyboard, self) || is(g_mouse, self))) {
        forget(self);
        LOG("input: game released a device; forgetting it");
    }
    return refs;
}

void hook_device(IUnknown* dev) {
    if (g_device_hooked.exchange(true)) return;
    // Keyboard, mouse and joystick devices share these vtables; the detours check which one it is.
    create_and_enable(vtable_slot(dev, kDevSetCooperativeLevel), reinterpret_cast<void*>(&h_SetCooperativeLevel),
                      reinterpret_cast<void**>(&o_SetCooperativeLevel), "IDirectInputDevice8::SetCooperativeLevel");
    create_and_enable(vtable_slot(dev, kDevGetDeviceData), reinterpret_cast<void*>(&h_GetDeviceData),
                      reinterpret_cast<void**>(&o_GetDeviceData), "IDirectInputDevice8::GetDeviceData");
    create_and_enable(vtable_slot(dev, kDevRelease), reinterpret_cast<void*>(&h_Release),
                      reinterpret_cast<void**>(&o_Release), "IDirectInputDevice8::Release");
}

HRESULT STDMETHODCALLTYPE h_CreateDevice(IUnknown* self, REFGUID guid, IUnknown** out, LPUNKNOWN outer) {
    HRESULT hr = o_CreateDevice(self, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        IUnknown* dev = *out;
        forget(dev);  // address reuse
        if (IsEqualGUID(guid, kGuidSysMouse)) {
            LOG("input: system mouse device %p", static_cast<void*>(dev));
            g_mouse.created = dev;
            hook_device(dev);
        } else if (IsEqualGUID(guid, kGuidSysKeyboard)) {
            LOG("input: system keyboard device %p", static_cast<void*>(dev));
            g_keyboard.created = dev;
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

WNDPROC g_orig_wndproc = nullptr;

LRESULT CALLBACK cursor_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && cursor_mode()) {
        SetCursor(nullptr);
        return TRUE;
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
}

void subclass_for_cursor(HWND hwnd) {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    g_orig_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&cursor_wndproc)));
    LOG("mouse: game window subclassed for the cursor setting (%s)", g_orig_wndproc ? "ok" : "FAILED");
}

// The menu lays screens out in a 1024x768 virtual space and sets the cursor range to that space minus the
// cursor image; the game clamps x/y to the range afterwards.
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
    if (state && hwnd && is(g_mouse, reinterpret_cast<IUnknown*>(state[kStateDevice]))) {
        subclass_for_cursor(hwnd);
        if (cursor_mode()) {
            static int last_range[4] = {};
            if (memcmp(last_range, state + kStateLeft, sizeof(last_range)) != 0) {
                memcpy(last_range, state + kStateLeft, sizeof(last_range));
                LOG("mouse: game cursor range (%d,%d)-(%d,%d)", last_range[0], last_range[1], last_range[2],
                    last_range[3]);
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
    }
    return o_MouseRead(state);
}

// Re-applies a device's cooperative level; only possible while unacquired.
void reapply(Device& d, DWORD flags, const char* what) {
    IUnknown* dev = d.used.load();
    if (!dev || !d.hwnd || !o_SetCooperativeLevel) return;
    reinterpret_cast<Simple_t>(vtable_slot(dev, kDevUnacquire))(dev);
    HRESULT hr = o_SetCooperativeLevel(dev, d.hwnd, flags);
    HRESULT acq = reinterpret_cast<Simple_t>(vtable_slot(dev, kDevAcquire))(dev);
    LOG("input: %s cooperative level 0x%lx -> 0x%08lx, Acquire -> 0x%08lx", what, flags, hr, acq);
}

}  // namespace

void input_on_modules_loaded() {
    if (g_dinput_hooked.load()) return;
    HMODULE dinput8 = GetModuleHandleW(L"dinput8.dll");
    HMODULE game_di = GetModuleHandleW(L"DirectInput.dll");  // Core\DirectInput.dll (the game's)
    if (!dinput8 || !game_di) return;

    // Runs under the module watch (maybe under the loader lock): no settings are read here.
    AcquireSRWLockExclusive(&g_lock);
    if (!g_dinput_hooked.exchange(true)) {
        if (is_expected_build(game_di, kDirectInputTimestamp, kDirectInputImageSize, "DirectInput.dll") &&
            create_and_enable(at_rva(game_di, kMouseReadRva), reinterpret_cast<void*>(&h_MouseRead),
                              reinterpret_cast<void**>(&o_MouseRead), "DirectInput.dll+0x51b0 (mouse read)")) {
            g_read_hooked = true;
        } else {
            LOG("mouse: pointer matching unavailable; the mouse stays stock");
        }
        if (void* create = reinterpret_cast<void*>(GetProcAddress(dinput8, "DirectInput8Create"))) {
            create_and_enable(create, reinterpret_cast<void*>(&h_DirectInput8Create),
                              reinterpret_cast<void**>(&o_DirectInput8Create), "DirectInput8Create");
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void input_apply_settings() {
    reapply(g_keyboard, keyboard_flags(g_keyboard.flags), "keyboard");
    reapply(g_mouse, mouse_flags(g_mouse.flags), "mouse");
}

}  // namespace lazysf2::hooks
