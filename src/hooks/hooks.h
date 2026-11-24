#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>

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

// Object probe (developer tool): only active when LAZYSF2_PROBE_DIR is set. install_probe_hooks()
// reads it once (call from DllMain); probe_on_modules_loaded() hooks AircraftObject.dll when it loads.
void install_probe_hooks();
void probe_on_modules_loaded();

// One live aircraft object, as seen by probe_with_aircraft(). name points into the probe table and is valid
// only inside the callback.
struct LiveAircraft {
    void* obj;
    const wchar_t* name;
    unsigned long long created;  // GetTickCount64() at construction
};
// Runs callback while holding the aircraft table's lock shared: no listed object can be destroyed
// until it returns (the destructor hook needs the lock exclusively). The callback must not log or
// do file I/O. The table is empty unless the probe is on.
void probe_with_aircraft(void (*callback)(const LiveAircraft* list, size_t count, void* ctx), void* ctx);

// Puppet mode (developer tool, part of the probe): drives one AI aircraft along a level path from
// the render thread, to see whether the game lets us hold it there. Ctrl+Shift+P toggles a run of
// at most 120 s. puppet_toggle() is called by the probe's hotkey thread; puppet_on_present() from
// IDXGISwapChain::Present, and costs one relaxed atomic load while no run is requested.
void puppet_toggle();
void puppet_run_frame();
extern std::atomic<int> g_puppet_state;
inline void puppet_on_present() {
    if (g_puppet_state.load(std::memory_order_relaxed) != 0) puppet_run_frame();
}

// Missile spawn test (developer tool, part of the probe): Ctrl+Shift+M creates one missile 200 m ahead of
// the player through the game's object manager, on the next Present, and follows it for 20 s.
// missile_spawn_on_modules_loaded() hooks MissileObject's constructor (to learn weapon ids); the probe calls it.
void missile_spawn_request();
void missile_spawn_run_frame();
void missile_spawn_on_modules_loaded();
extern std::atomic<int> g_missile_state;
inline void missile_spawn_on_present() {
    if (g_missile_state.load(std::memory_order_relaxed) != 0) missile_spawn_run_frame();
}

}  // namespace lazysf2::hooks
