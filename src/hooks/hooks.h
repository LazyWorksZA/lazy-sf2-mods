#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hooks/aircraft_side.h"

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
// reads it once (call from DllMain); probe_on_modules_loaded() starts the developer features (recorder, replay,
// host, hotkeys) once the aircraft tracker is active.
void install_probe_hooks();
void probe_on_modules_loaded();

// Aircraft tracker (src/hooks/aircraft_track.cpp), the production base of the probe and of the Tacview recorder.
// aircraft_track_on_modules_loaded() runs on every module load. Each time a (new) AircraftObject.dll image
// appears it installs the constructor, destructor and Update hooks, but only if a client needs them: the probe is
// on, or the Tacview Recording setting is on (config peek; it never loads anything). When neither applies nothing is
// hooked. It also re-hooks after the DLL is unloaded and loaded again.
void aircraft_track_on_modules_loaded();
// Called on the game thread when an Options page is left (menus, no live aircraft): installs the hooks if the
// setting or the probe now needs them, and removes them if nothing does. A change of the setting applies here, or
// at the next AircraftObject.dll load, never mid-flight.
void aircraft_track_reevaluate();
// True while the tracking hooks are installed on the loaded AircraftObject.dll.
bool aircraft_track_active();
// Why the hooks are installed: the probe, the setting, or both. Meaningful only while active.
bool aircraft_track_for_probe();
bool aircraft_track_for_tacview();
// Tacview recorder (src/hooks/tacview_recorder.cpp, player-facing): with Options > Other > Tacview Recording On it
// writes each flight's aircraft to Saved Games\ThirdWire\<exe>\Tacview\SF2 <date time>.acmi from a writer thread.
// tacview_recorder_on_modules_loaded() registers the sim step callback (once; it only ever runs while the aircraft
// tracker is hooked, which needs the setting or the probe). tacview_recorder_menu_moment() (game thread, an Options
// page was left) closes a recording of a flight that has ended. tacview_recorder_shutdown() (DllMain detach, both
// FreeLibrary and process exit) closes the file; at process exit it writes what the dead writer left in its ring.
void tacview_recorder_on_modules_loaded();
void tacview_recorder_menu_moment();
void tacview_recorder_shutdown();
// Hooks of the sim step (sim_hooks.cpp); the tracker calls these, after its own build check.
bool sim_install(HMODULE aircraft_dll);
void sim_remove(HMODULE aircraft_dll);

// One live aircraft object, as seen by probe_with_aircraft(). name points into the tracker table and is valid
// only inside the callback.
struct LiveAircraft {
    void* obj;
    const wchar_t* name;     // the constructor's name argument, at most 63 characters
    unsigned long long seq;  // construction sequence number: unique, increasing, never reused. Identifies the object
                             // together with obj (an address can be reused).
    bool is_player;          // the player's aircraft: the first constructed since the table was last empty. At most
                             // one entry; none once the player is destroyed (until the table empties and refills).
                             // Every "which one is the player" decision uses this flag, nothing else.
    AircraftSide side;       // the object's FRIENDLY/ENEMY/NEUTRAL flag bit (see aircraft_side.h), read from the object's flag word when sampled; Unknown if the masks are not known
    unsigned long long created_ms;  // GetTickCount64() at construction: coarse, only for diagnostics, never identity
    // There is no aircraft type name field: no cheap, certain source in the object was found.
};
// Runs callback while holding the aircraft table's lock shared: no listed object can be destroyed
// until it returns (the destructor hook needs the lock exclusively). The callback must not log or
// do file I/O. The table is empty unless tracking is active.
void probe_with_aircraft(void (*callback)(const LiveAircraft* list, size_t count, void* ctx), void* ctx);
// Incremented (release) each time a tracked aircraft is destroyed or its address is reused by a new one, and when
// the tracker re-hooks a reloaded AircraftObject.dll.
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

// Simulation step: hooks the aircraft's own Update(dt), which runs once per simulation step for every aircraft on
// the game's main thread, and not while the game is paused. The aircraft tracker installs the hook; nothing is
// hooked unless a client needs tracking.
// The game clock adds dt on each step of the player's aircraft (LiveAircraft::is_player: the first aircraft constructed since the tracker table was last empty).
// sim_game_time() and sim_step_count() may be read from the game thread only (inside a step callback).
// Game thread; updated on any aircraft's step. The player is known while sim has a cached one.
bool sim_player_known();
// Incremented on every player loss and every change of player. A run stores it at its start and ends when it differs.
uint32_t sim_player_epoch();
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

// Host mode (developer tool, part of the probe; src/net/host.cpp): LAZYSF2_HOST=port or address:port makes the mod
// listen for remote clients over UDP (ENet) and drive one AI aircraft per client as its avatar. Inert unless
// LAZYSF2_HOST is set and the probe is on; the address defaults to 127.0.0.1.
// host_install() reads the variable once (call from DllMain, after install_probe_hooks()) and logs "net: host off"
// or that it is pending. host_on_modules_loaded() starts the network thread and registers the step callback (the
// probe calls it). host_shutdown() stops the thread and gives every avatar back to the AI (DLL unload only).
void host_install();
void host_on_modules_loaded();
void host_shutdown();
// True while any client holds an avatar. Puppet and replay refuse to start then, and the host refuses clients
// while one of them runs.
bool host_has_clients();

}  // namespace lazysf2::hooks
