#pragma once

#include <windows.h>

namespace lazysf2::hooks {

// Game window: strip WS_EX_TOPMOST at creation and keep it non-topmost afterwards (Borderless).
bool install_window_hooks();
HWND game_window();
// Rect of the Screen chosen in Options, or false for the stock monitor choice.
bool window_chosen_screen(RECT* r);
// True (with the chosen Screen's rect) if the game window is not on the chosen Screen.
bool window_needs_move(RECT* target);
// Asks the game window's thread to apply config::display_mode() to the running game.
void request_display_mode_apply();
// Applies config::display_mode() to the game's swap chain. Window thread only.
void dxgi_apply_display_mode();
// True if the game's swap chain is currently in exclusive fullscreen (diagnostics).
bool dxgi_is_exclusive();

// Hooks kernelbase!LoadLibraryExW and, after every load, gives each feature a chance to hook
// modules that have just appeared (dxgi.dll, dinput8.dll, Core\DirectInput.dll, ...).
bool install_module_watch();

// DXGI: CreateDXGIFactory -> CreateSwapChain -> SetFullscreenState/ResizeTarget, so the game
// never enters exclusive fullscreen. Called by the module watch.
void dxgi_on_modules_loaded();

// DirectInput: Windows key in Borderless, and mouse "Cursor" mode (the game's cursor follows the
// Windows cursor). Called by the module watch.
void input_on_modules_loaded();
// Re-applies the keyboard's and mouse's cooperative levels for the current display mode and mouse
// setting (live switches). Window thread only.
void input_apply_settings();

// Options menu "Display Mode" list (Graphics page), saved as Options.ini [GraphicsOptions]
// DisplayMode. install_menu_hooks() hooks the exe's file service (call from DllMain);
// menu_on_modules_loaded() hooks OptionsScreen.dll once it is loaded.
bool install_menu_hooks();
void menu_on_modules_loaded();

}  // namespace lazysf2::hooks
