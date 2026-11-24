// Borderless: GraphicsEngineDX10 always creates its swap chain windowed, then calls _DoFullScreen
// (ResizeTarget + SetFullscreenState(TRUE) + ResizeBuffers) when its fullscreen flag is set
// (recon-notes.md §4.3). We turn the exclusive-mode calls into no-ops, so the swap chain stays
// windowed inside the stock monitor-sized popup, and ResizeBuffers(0,0) sizes the buffers to it.
//
// We hook the functions behind the COM vtables of whatever dxgi.dll the game loads, so this
// works the same with the system DXGI and with DXVK.
#include <windows.h>

#include <dxgi.h>

#include <MinHook.h>

#include <atomic>

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
using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                     IDXGISwapChain**);
using SetFullscreenState_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using ResizeTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);

LoadLibraryExW_t o_LoadLibraryExW = nullptr;
CreateDXGIFactory_t o_CreateDXGIFactory = nullptr;
CreateSwapChain_t o_CreateSwapChain = nullptr;
SetFullscreenState_t o_SetFullscreenState = nullptr;
ResizeTarget_t o_ResizeTarget = nullptr;

std::atomic<bool> g_dxgi_hooked{false};
std::atomic<bool> g_factory_hooked{false};
std::atomic<bool> g_swapchain_hooked{false};
CRITICAL_SECTION g_install_lock;

void* vtable_slot(void* com_object, int slot) { return (*static_cast<void***>(com_object))[slot]; }

bool create_and_enable(void* target, void* detour, void** original, const char* what) {
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        LOG("dxgi: hooking %s at %p failed: %s", what, target, MH_StatusToString(s));
        return false;
    }
    LOG("dxgi: hooked %s at %p", what, target);
    return true;
}

// --- IDXGISwapChain ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE h_SetFullscreenState(IDXGISwapChain* self, BOOL fullscreen, IDXGIOutput* target) {
    if (fullscreen) {
        LOG("dxgi: SetFullscreenState(TRUE) suppressed (borderless)");
        return S_OK;
    }
    return o_SetFullscreenState(self, fullscreen, target);
}

HRESULT STDMETHODCALLTYPE h_ResizeTarget(IDXGISwapChain* /*self*/, const DXGI_MODE_DESC* mode) {
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
        if (!desc->Windowed) {
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
    if (SUCCEEDED(hr) && out && *out) {
        if (desc && desc->OutputWindow) {
            HRESULT mwa = self->MakeWindowAssociation(desc->OutputWindow, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            LOG("dxgi: MakeWindowAssociation(NO_ALT_ENTER|NO_WINDOW_CHANGES) -> 0x%08lx", mwa);
        }
        hook_swapchain(*out);
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

// --- dxgi.dll load watch ----------------------------------------------------------------------

void try_hook_dxgi() {
    if (g_dxgi_hooked.load()) return;
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) return;

    EnterCriticalSection(&g_install_lock);
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
    LeaveCriticalSection(&g_install_lock);
}

HMODULE WINAPI h_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE h = o_LoadLibraryExW(name, file, flags);
    try_hook_dxgi();
    return h;
}

}  // namespace

bool install_dxgi_hooks() {
    InitializeCriticalSection(&g_install_lock);
    // kernel32!LoadLibraryW/A and LoadLibraryExA all end up in kernelbase!LoadLibraryExW.
    MH_STATUS s = MH_CreateHookApi(L"kernelbase", "LoadLibraryExW", reinterpret_cast<LPVOID>(&h_LoadLibraryExW),
                                   reinterpret_cast<LPVOID*>(&o_LoadLibraryExW));
    if (s != MH_OK) {
        LOG("dxgi: hooking LoadLibraryExW failed: %s", MH_StatusToString(s));
        return false;
    }
    return true;
}

}  // namespace lazysf2::hooks
