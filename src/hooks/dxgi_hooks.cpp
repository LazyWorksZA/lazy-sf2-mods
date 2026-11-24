// Display mode at the DXGI layer.
//
// GraphicsEngineDX10 always creates its swap chain windowed, then calls _DoFullScreen
// (ResizeTarget + SetFullscreenState(TRUE) + ResizeTarget + ResizeBuffers) when its fullscreen flag
// is set, at start-up and every time the game regains focus (recon-notes.md §4.3). In Borderless
// mode we turn the exclusive-mode calls into no-ops, so the swap chain stays windowed inside the
// stock monitor-sized popup. In Fullscreen mode everything passes through unchanged.
//
// The mode can change while the game runs (Options menu). dxgi_apply_display_mode() then moves
// the existing swap chain into or out of exclusive mode the way the game itself does; it must run
// on the game window's thread.
//
// We hook the functions behind the COM vtables of whatever dxgi.dll the game loads, so this
// works the same with the system DXGI and with DXVK.
#include <windows.h>

#include <d3d10.h>
#include <dxgi.h>

#include <MinHook.h>

#include <intrin.h>

#include <atomic>

#include "config.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "log.h"

namespace lazysf2::hooks {
namespace {

// IDXGIFactory vtable slot
constexpr int kFactoryCreateSwapChain = 10;
// IDXGISwapChain vtable slots
constexpr int kSwapPresent = 8;
constexpr int kSwapSetFullscreenState = 10;
constexpr int kSwapResizeBuffers = 13;
constexpr int kSwapResizeTarget = 14;

using CreateDXGIFactory_t = HRESULT(WINAPI*)(REFIID, void**);
using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                     IDXGISwapChain**);
using SetFullscreenState_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using ResizeTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

CreateDXGIFactory_t o_CreateDXGIFactory = nullptr;
CreateSwapChain_t o_CreateSwapChain = nullptr;
SetFullscreenState_t o_SetFullscreenState = nullptr;
ResizeTarget_t o_ResizeTarget = nullptr;
Present_t o_Present = nullptr;
ResizeBuffers_t o_ResizeBuffers = nullptr;

std::atomic<bool> g_dxgi_hooked{false};
std::atomic<bool> g_factory_hooked{false};
std::atomic<bool> g_swapchain_hooked{false};
SRWLOCK g_install_lock = SRWLOCK_INIT;

// The game's swap chain and what it last asked for. Not reference-counted: the game keeps its
// swap chain for the whole session, and we only use it from the window thread.
IDXGISwapChain* g_swapchain = nullptr;
IDXGIFactory* g_factory = nullptr;
HWND g_output_window = nullptr;
DXGI_MODE_DESC g_target_mode = {};
bool g_have_target_mode = false;
bool g_game_wants_fullscreen = false;  // the game's own fullscreen flag, as seen through its calls

void set_alt_enter_handling(bool borderless) {
    if (!g_factory || !g_output_window) return;
    const UINT flags = borderless ? (DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES) : 0;  // 0 = stock
    HRESULT hr = g_factory->MakeWindowAssociation(g_output_window, flags);
    LOG("dxgi: MakeWindowAssociation(0x%x) -> 0x%08lx", flags, hr);
}

// --- IDXGISwapChain ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE h_SetFullscreenState(IDXGISwapChain* self, BOOL fullscreen, IDXGIOutput* target) {
    char caller[96];
    describe_address(_ReturnAddress(), caller, sizeof(caller));
    if (self == g_swapchain) g_game_wants_fullscreen = fullscreen != FALSE;
    if (fullscreen && config::borderless()) {
        LOG("dxgi: SetFullscreenState(TRUE) from %s suppressed (borderless)", caller);
        return S_OK;
    }
    HRESULT hr = o_SetFullscreenState(self, fullscreen, target);
    BOOL now = FALSE;
    self->GetFullscreenState(&now, nullptr);
    LOG("dxgi: SetFullscreenState(%d) from %s -> 0x%08lx (exclusive now %d)", fullscreen, caller, hr, now);
    return hr;
}

HRESULT STDMETHODCALLTYPE h_ResizeTarget(IDXGISwapChain* self, const DXGI_MODE_DESC* mode) {
    if (mode && self == g_swapchain && !g_have_target_mode) {
        g_target_mode = *mode;  // the mode the game wants for exclusive fullscreen
        g_have_target_mode = true;
    }
    if (!config::borderless()) {
        // Fullscreen on a chosen Screen whose size differs from the game's mode: switch that
        // monitor to its own size (DXGI scales the back buffer) instead of growing the window past it.
        RECT r;
        if (mode && self == g_swapchain && window_chosen_screen(&r)) {
            const UINT w = static_cast<UINT>(r.right - r.left), h = static_cast<UINT>(r.bottom - r.top);
            if (mode->Width != w || mode->Height != h) {
                DXGI_MODE_DESC m = *mode;
                m.Width = w;
                m.Height = h;
                m.RefreshRate = {0, 0};
                LOG("dxgi: ResizeTarget %ux%u -> %ux%u (chosen Screen)", mode->Width, mode->Height, w, h);
                return o_ResizeTarget(self, &m);
            }
        }
        return o_ResizeTarget(self, mode);
    }
    if (mode) {
        LOG("dxgi: ResizeTarget(%ux%u @ %u/%u) suppressed (borderless)", mode->Width, mode->Height,
            mode->RefreshRate.Numerator, mode->RefreshRate.Denominator);
    }
    return S_OK;  // the window already covers the monitor; never resize it or change the mode
}

// Present runs every frame: log only when its result changes (S_OK -> OCCLUDED -> S_OK, removal, ...).
const char* present_result_name(HRESULT hr) {
    switch (hr) {
        case S_OK: return "S_OK";
        case DXGI_STATUS_OCCLUDED: return "DXGI_STATUS_OCCLUDED";
        case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
        case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
        default: return "?";
    }
}

HRESULT g_last_present = S_OK;
DWORD g_last_present_tick = 0;
unsigned g_frames_since_change = 0;

HRESULT STDMETHODCALLTYPE h_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    HRESULT hr = o_Present(self, sync, flags);
    ++g_frames_since_change;
    if (self == g_swapchain && hr != g_last_present) {
        const DWORD now = GetTickCount();
        LOG("dxgi: Present -> 0x%08lx %s (was %s for %u frames, %lu ms)", hr, present_result_name(hr),
            present_result_name(g_last_present), g_frames_since_change, now - g_last_present_tick);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            ID3D10Device* device = nullptr;
            if (SUCCEEDED(self->GetDevice(__uuidof(ID3D10Device), reinterpret_cast<void**>(&device))) && device) {
                LOG("dxgi: device removed reason 0x%08lx", device->GetDeviceRemovedReason());
                device->Release();
            }
        }
        g_last_present = hr;
        g_last_present_tick = now;
        g_frames_since_change = 0;
    }
    return hr;
}

// The game renders at its own resolution (DisplayWidth/Height). After its explicit
// ResizeBuffers(2, W, H), its _DoFullScreen calls ResizeBuffers(0, 0, 0) = "size of the window".
// In stock exclusive mode ResizeTarget made the window W x H first, so that is the same thing; but
// when the window covers a monitor of another size (Borderless, or the chosen Screen), the buffers
// would follow the monitor while the game keeps rendering W x H, pushing the picture off-screen.
// So 0 x 0 becomes the game's last explicit size, and DXGI scales it to the window / display mode.
UINT g_game_buffer_w = 0, g_game_buffer_h = 0;

HRESULT STDMETHODCALLTYPE h_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    char caller[96];
    describe_address(_ReturnAddress(), caller, sizeof(caller));
    const UINT asked_w = w, asked_h = h;
    if (self == g_swapchain) {
        if (w && h) {
            g_game_buffer_w = w;
            g_game_buffer_h = h;
        } else if (g_game_buffer_w && g_game_buffer_h) {
            w = g_game_buffer_w;
            h = g_game_buffer_h;
        }
    }
    HRESULT hr = o_ResizeBuffers(self, count, w, h, fmt, flags);
    LOG("dxgi: ResizeBuffers(%u, %ux%u%s, fmt %d, flags 0x%x) from %s -> 0x%08lx", count, asked_w, asked_h,
        (w != asked_w || h != asked_h) ? " -> game resolution" : "", fmt, flags, caller, hr);
    return hr;
}

void hook_swapchain(IDXGISwapChain* sc) {
    if (g_swapchain_hooked.exchange(true)) return;
    g_last_present_tick = GetTickCount();
    create_and_enable(vtable_slot(sc, kSwapPresent), reinterpret_cast<void*>(&h_Present),
                      reinterpret_cast<void**>(&o_Present), "IDXGISwapChain::Present");
    create_and_enable(vtable_slot(sc, kSwapResizeBuffers), reinterpret_cast<void*>(&h_ResizeBuffers),
                      reinterpret_cast<void**>(&o_ResizeBuffers), "IDXGISwapChain::ResizeBuffers");
    create_and_enable(vtable_slot(sc, kSwapSetFullscreenState), reinterpret_cast<void*>(&h_SetFullscreenState),
                      reinterpret_cast<void**>(&o_SetFullscreenState), "IDXGISwapChain::SetFullscreenState");
    create_and_enable(vtable_slot(sc, kSwapResizeTarget), reinterpret_cast<void*>(&h_ResizeTarget),
                      reinterpret_cast<void**>(&o_ResizeTarget), "IDXGISwapChain::ResizeTarget");
}

// --- IDXGIFactory -----------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE h_CreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                            IDXGISwapChain** out) {
    if (desc) {
        LOG("dxgi: CreateSwapChain %ux%u fmt=%d refresh=%u/%u msaa=%u buffers=%u windowed=%d flags=0x%x hwnd=%p",
            desc->BufferDesc.Width, desc->BufferDesc.Height, desc->BufferDesc.Format,
            desc->BufferDesc.RefreshRate.Numerator, desc->BufferDesc.RefreshRate.Denominator, desc->SampleDesc.Count,
            desc->BufferCount, desc->Windowed, desc->Flags, static_cast<void*>(desc->OutputWindow));
        if (!desc->Windowed && config::borderless()) {
            LOG("dxgi: forcing Windowed=TRUE");
            desc->Windowed = TRUE;
        }
        RECT rc{};
        if (desc->OutputWindow && GetClientRect(desc->OutputWindow, &rc) &&
            (static_cast<UINT>(rc.right - rc.left) != desc->BufferDesc.Width ||
             static_cast<UINT>(rc.bottom - rc.top) != desc->BufferDesc.Height)) {
            LOG("dxgi: WARNING window client %ldx%ld differs from the game resolution; the image will be scaled",
                rc.right - rc.left, rc.bottom - rc.top);
        }
    }

    HRESULT hr = o_CreateSwapChain(self, device, desc, out);
    LOG("dxgi: CreateSwapChain -> 0x%08lx", hr);
    if (SUCCEEDED(hr) && out && *out && desc && desc->OutputWindow == game_window()) {
        g_swapchain = *out;
        g_game_buffer_w = desc->BufferDesc.Width;  // the game's resolution
        g_game_buffer_h = desc->BufferDesc.Height;
        g_factory = self;  // the game keeps its factory alive with the swap chain
        g_output_window = desc->OutputWindow;
        g_have_target_mode = false;
        hook_swapchain(*out);
        if (config::borderless()) set_alt_enter_handling(true);
    }
    return hr;
}

void hook_factory(IUnknown* obj) {
    if (g_factory_hooked.load()) return;
    IDXGIFactory* factory = nullptr;
    if (FAILED(obj->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) return;
    if (!g_factory_hooked.exchange(true)) {
        create_and_enable(vtable_slot(factory, kFactoryCreateSwapChain), reinterpret_cast<void*>(&h_CreateSwapChain),
                          reinterpret_cast<void**>(&o_CreateSwapChain), "IDXGIFactory::CreateSwapChain");
    }
    factory->Release();
}

HRESULT WINAPI h_CreateDXGIFactory(REFIID riid, void** out) {
    HRESULT hr = o_CreateDXGIFactory(riid, out);
    LOG("dxgi: CreateDXGIFactory -> 0x%08lx", hr);
    if (SUCCEEDED(hr) && out && *out) hook_factory(static_cast<IUnknown*>(*out));
    return hr;
}

}  // namespace

bool dxgi_is_exclusive() {
    BOOL exclusive = FALSE;
    if (g_swapchain) g_swapchain->GetFullscreenState(&exclusive, nullptr);
    return exclusive != FALSE;
}

// --- Live switching -----------------------------------------------------------------------------

void dxgi_apply_display_mode() {
    const bool borderless = config::borderless();
    input_apply_display_mode();  // Windows key follows the mode
    IDXGISwapChain* sc = g_swapchain;
    if (!sc || !o_SetFullscreenState || !o_ResizeTarget) {
        LOG("dxgi: live switch skipped: no swap chain yet");
        return;
    }
    BOOL exclusive = FALSE;
    sc->GetFullscreenState(&exclusive, nullptr);
    RECT target{};
    const bool move = window_needs_move(&target);
    LOG("dxgi: live switch to %s%s (exclusive now: %d, game fullscreen flag: %d)", borderless ? "Borderless" : "Fullscreen",
        move ? " on another Screen" : "", exclusive, g_game_wants_fullscreen);

    HWND hwnd = g_output_window;
    // Exclusive mode is tied to a monitor: leave it before moving, or when going borderless.
    if (exclusive && (borderless || move)) {
        LOG("dxgi: SetFullscreenState(FALSE) -> 0x%08lx", o_SetFullscreenState(sc, FALSE, nullptr));
        exclusive = FALSE;
    }
    if (move) {
        // Our SetWindowPos hook only rewrites the exe's own calls, so this one goes through as is.
        SetWindowPos(hwnd, borderless ? HWND_NOTOPMOST : HWND_TOPMOST, target.left, target.top,
                     target.right - target.left, target.bottom - target.top, SWP_NOACTIVATE | SWP_FRAMECHANGED);
        LOG("dxgi: moved the game window to (%ld,%ld)-(%ld,%ld)", target.left, target.top, target.right, target.bottom);
    }
    if (borderless) {
        set_alt_enter_handling(true);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }

    // Fullscreen: only enter exclusive mode if the game itself runs fullscreen (its stock state).
    set_alt_enter_handling(false);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (!g_game_wants_fullscreen || exclusive) return;
    // Same sequence as the game's _DoFullScreen. ResizeBuffers is left out: the game still holds its
    // back-buffer view, and the buffers already match the mode.
    DXGI_MODE_DESC mode = g_target_mode;
    if (move) {
        // On another Screen the game's own mode may not exist (e.g. 2560x1440 on a 1080p monitor):
        // use that monitor's size; DXGI scales the back buffer to it.
        mode.Width = static_cast<UINT>(target.right - target.left);
        mode.Height = static_cast<UINT>(target.bottom - target.top);
        mode.RefreshRate = {0, 0};
    }
    if (g_have_target_mode) o_ResizeTarget(sc, &mode);
    HRESULT hr = o_SetFullscreenState(sc, TRUE, nullptr);
    LOG("dxgi: SetFullscreenState(TRUE) -> 0x%08lx", hr);
    if (SUCCEEDED(hr) && g_have_target_mode) {
        mode.RefreshRate.Numerator = 0;
        mode.RefreshRate.Denominator = 0;
        o_ResizeTarget(sc, &mode);
    }
}

// --- dxgi.dll load watch ----------------------------------------------------------------------

void dxgi_on_modules_loaded() {
    if (g_dxgi_hooked.load()) return;
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) return;

    AcquireSRWLockExclusive(&g_install_lock);
    if (!g_dxgi_hooked.load()) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(dxgi, path, MAX_PATH);
        LOG("dxgi: dxgi.dll loaded from %ls", path);
        void* target = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory"));
        if (target) {
            create_and_enable(target, reinterpret_cast<void*>(&h_CreateDXGIFactory),
                              reinterpret_cast<void**>(&o_CreateDXGIFactory), "CreateDXGIFactory");
        }
        g_dxgi_hooked = true;
    }
    ReleaseSRWLockExclusive(&g_install_lock);
}

}  // namespace lazysf2::hooks
