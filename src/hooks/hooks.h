#pragma once

#include <windows.h>

namespace lazysf2::hooks {

// Game window: strip WS_EX_TOPMOST at creation and keep it non-topmost afterwards.
bool install_window_hooks();
HWND game_window();

// Hooks kernelbase!LoadLibraryExW and, after every load, gives each feature a chance to hook
// modules that have just appeared (dxgi.dll, dinput8.dll, Core\DirectInput.dll, ...).
bool install_module_watch();

// DXGI: CreateDXGIFactory -> CreateSwapChain -> SetFullscreenState/ResizeTarget, so the game
// never enters exclusive fullscreen. Called by the module watch.
void dxgi_on_modules_loaded();

// Mouse "Cursor" mode: the game's cursor follows the Windows cursor. Called by the module watch.
void mouse_on_modules_loaded();

}  // namespace lazysf2::hooks
