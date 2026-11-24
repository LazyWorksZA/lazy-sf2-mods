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
constexpr int kSwapSetFullscreenState = 10;
constexpr int kSwapResizeTarget = 14;

using CreateDXGIFactory_t = HRESULT(WINAPI*)(REFIID, void**);
using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                     IDXGISwapChain**);
using SetFullscreenState_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using ResizeTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);

CreateDXGIFactory_t o_CreateDXGIFactory = nullptr;
CreateSwapChain_t o_CreateSwapChain = nullptr;
SetFullscreenState_t o_SetFullscreenState = nullptr;
ResizeTarget_t o_ResizeTarget = nullptr;

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
    if (!config::borderless()) return o_ResizeTarget(self, mode);
    if (mode) {
        LOG("dxgi: ResizeTarget(%ux%u @ %u/%u) suppressed (borderless)", mode->Width, mode->Height,
            mode->RefreshRate.Numerator, mode->RefreshRate.Denominator);
    }
    return S_OK;  // the window already covers the monitor; never resize it or change the mode
}

void hook_swapchain(IDXGISwapChain* sc) {
    if (g_swapchain_hooked.exchange(true)) return;
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
    IDXGISwapChain* sc = g_swapchain;
    if (!sc || !o_SetFullscreenState || !o_ResizeTarget) {
        LOG("dxgi: live switch skipped: no swap chain yet");
        return;
    }
    BOOL exclusive = FALSE;
    sc->GetFullscreenState(&exclusive, nullptr);
    LOG("dxgi: live switch to %s (exclusive now: %d, game fullscreen flag: %d)", borderless ? "Borderless" : "Fullscreen",
        exclusive, g_game_wants_fullscreen);

    HWND hwnd = g_output_window;
    if (borderless) {
        if (exclusive) LOG("dxgi: SetFullscreenState(FALSE) -> 0x%08lx", o_SetFullscreenState(sc, FALSE, nullptr));
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
