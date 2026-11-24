#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

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
    unsigned long long seq;  // construction sequence number: unique, increasing, never reused. Identifies the object
                             // together with obj (an address can be reused).
    bool is_player;          // the player's aircraft: the first constructed since the table was last empty. At most
                             // one entry; none once the player is destroyed (until the table empties and refills).
                             // Every "which one is the player" decision uses this flag, nothing else.
};
// Runs callback while holding the aircraft table's lock shared: no listed object can be destroyed
// until it returns (the destructor hook needs the lock exclusively). The callback must not log or
// do file I/O. The table is empty unless the probe is on.
void probe_with_aircraft(void (*callback)(const LiveAircraft* list, size_t count, void* ctx), void* ctx);
// Incremented (release) each time a tracked aircraft is destroyed or its address is reused by a new one.
// A cached aircraft pointer is only trusted while this still has the value it had at the last validation.
extern std::atomic<uint32_t> g_aircraft_generation;
// The probe's output directory (LAZYSF2_PROBE_DIR, validated), or nullptr when the probe is off.
const wchar_t* probe_output_dir();

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

// Simulation step (developer tool, part of the probe): hooks the aircraft's own Update(dt), which runs
// once per simulation step for every aircraft on the game's main thread, and not while the game is
// paused. sim_on_modules_loaded() installs the hook (the probe calls it); nothing is hooked with the probe off.
// The game clock adds dt on each step of the player's aircraft (LiveAircraft::is_player: the first aircraft constructed since the probe table was last empty).
// sim_game_time() and sim_step_count() may be read from the game thread only (inside a step callback).
void sim_on_modules_loaded();
// True once the player was seen and has since been destroyed. Stays true until a new player is chosen (the table
// empties and refills). Game thread; updated on any aircraft's step.
bool sim_player_lost();
double sim_game_time();
uint64_t sim_step_count();
// "After an aircraft's Update" callback: called on the game thread, after the original returns and after the
// clock is updated, for every aircraft. is_player is true for the player's aircraft. The aircraft is valid
// for the duration of the call. Keep it cheap and return early for aircraft you don't care about. A fixed set
// of 4 slots; register once at install (module watch time). Returns false if the set is full.
using SimStepCallback = void (*)(void* aircraft, float dt, bool is_player);
bool sim_register_step_callback(SimStepCallback cb);
// While a developer run (recording, replay) is active, the hook logs one clock line per second of game time.
void sim_set_run_active(bool active);

// Recorder (developer tool, part of the probe): Ctrl+Shift+R records the player's aircraft state on every
// player step (game time), for at most 180 s, and writes <probe dir>\rec_NNN.bin when it stops.
// recorder_toggle() is called by the probe's hotkey thread and only sets a request; the game thread acts on it.
// File format (little endian, no padding): "LZRC", uint32 version (1), uint32 count, then count RecSample.
#pragma pack(push, 1)
struct RecSample {
    double t;         // game time (sim_game_time()), seconds
    double pos[3];    // world metres (x east, y north, z up)
    float vel[3];     // world m/s
    float ang[3];     // pitch, heading, roll, radians
    float rate[3];    // angular rates
};
#pragma pack(pop)
static_assert(sizeof(RecSample) == 68, "RecSample is part of the rec_NNN.bin file format");
void recorder_on_modules_loaded();
void recorder_toggle();
// The latest finished recording (game thread only), or nullptr with *count = 0 while none exists or one is
// being recorded. Valid until the next recording starts.
const RecSample* recorder_latest(uint32_t* count);

// Replay (developer tool, part of the probe): Ctrl+Shift+T replays the latest recording onto the nearest AI aircraft
// with network-style smoothing (simulated feed, interpolation delay, dead reckoning, correction blending);
// Ctrl+Shift+Y cycles the feed rate 60/10/5/2 Hz. The hotkey thread only sets requests; the game thread acts.
// See replay.cpp and replay_math.h.
void replay_on_modules_loaded();
void replay_toggle();
void replay_cycle_rate();
// True while a replay runs. The recorder refuses to start then.
bool replay_active();

}  // namespace lazysf2::hooks
