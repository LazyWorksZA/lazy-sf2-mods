#pragma once

#include <windows.h>

namespace lazysf2::hooks {

// Game window: strip WS_EX_TOPMOST at creation and keep it non-topmost afterwards.
bool install_window_hooks();
HWND game_window();

// DXGI: watch module loads for dxgi.dll, then hook CreateDXGIFactory -> CreateSwapChain ->
// SetFullscreenState/ResizeTarget so the game never enters exclusive fullscreen.
bool install_dxgi_hooks();

}  // namespace lazysf2::hooks
