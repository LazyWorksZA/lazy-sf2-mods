#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "hooks/aircraft_side.h"
#include "net/auth.h"
#include "net/host_core.h"
#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>Strips WS_EX_TOPMOST from the game window at creation and keeps it non-topmost (Borderless).</summary>
bool install_window_hooks();
HWND game_window();
/// <summary>Rect of the Screen chosen in Options; false means the stock monitor choice.</summary>
bool window_chosen_screen(RECT* r);
/// <summary>True (with the chosen Screen's rect) if the game window is not on the chosen Screen.</summary>
bool window_needs_move(RECT* target);
/// <summary>Asks the game window's thread to apply config::display_mode() to the running game.</summary>
void request_display_mode_apply();
/// <summary>Applies config::display_mode() to the game's swap chain. Window thread only.</summary>
void dxgi_apply_display_mode();
/// <summary>True if the swap chain is in exclusive fullscreen (diagnostics).</summary>
bool dxgi_is_exclusive();
/// <summary>The game swap chain's last Present result (S_OK before any; D3D9 fallback: always S_OK). Any thread.</summary>
long dxgi_last_present_result();
/// <summary>
/// Confines the cursor to the game's client area in Fullscreen with "Match Windows cursor" (while the game is the
/// active app) and releases it otherwise, only ever releasing a clip this mod set. Any thread; logs on change.
/// </summary>
void cursor_clip_update(const char* why);
/// <summary>As cursor_clip_update, for periodic calls: also requires the foreground window to belong to this process.</summary>
void cursor_clip_poll(const char* why);
/// <summary>DLL detach: lets go of a clip we hold. Lock-free, safe at process exit.</summary>
void cursor_clip_release_at_exit();
/// <summary>True while "Match Windows cursor" is active (chosen in Options and the mouse read hook is in place).</summary>
bool mouse_cursor_mode_active();

/// <summary>
/// Hooks the exe's focus handler and message pump ([OtherOptions] BackgroundMode, or a network session): in Borderless a
/// focus loss can keep the game running (input is still released), paced by our frame limiter while minimised; otherwise stock. Call from
/// DllMain.
/// </summary>
bool install_focus_hooks();

/// <summary>
/// BackgroundMode 2: hooks dsound!DirectSoundCreate8, then IDirectSound8::CreateSoundBuffer, so new secondary buffers
/// get DSBCAPS_GLOBALFOCUS. Module watch callback; reads no settings.
/// </summary>
void sound_on_modules_loaded();
/// <summary>True once a sound buffer was created with the GLOBALFOCUS flag we added.</summary>
bool sound_global_focus_active();
/// <summary>Stops every playing channel of the game's SND service (stamp-checked). Game thread only.</summary>
void sound_stop_all_channels(const char* why);

/// <summary>
/// Hooks kernelbase!LoadLibraryExW. After every load each feature gets a chance to hook the modules that
/// just appeared.
/// </summary>
bool install_module_watch();

/// <summary>Hooks the game's DXGI swap chain so it never enters exclusive fullscreen. Module watch callback.</summary>
void dxgi_on_modules_loaded();

/// <summary>Hooks DirectInput (Windows key in Borderless, mouse Cursor mode). Module watch callback.</summary>
void input_on_modules_loaded();
/// <summary>Re-applies keyboard and mouse cooperative levels after a live setting change. Window thread only.</summary>
void input_apply_settings();

/// <summary>
/// Pilot Twist (pilot_twist.cpp): creates the ViewManager.dll inside-view hooks once it is loaded, and enables them
/// only while the setting is on (no detour runs while it is off). Module watch callback.
/// </summary>
void pilot_twist_on_modules_loaded();
/// <summary>
/// Enables or disables the view hooks to match config::pilot_twist(). Game thread, never under the loader lock: at
/// game window creation and when an Options page is left. Idempotent.
/// </summary>
void pilot_twist_apply_setting();
/// <summary>
/// Developer (probe hotkey thread, Ctrl+Shift+W): turns the twist on or off for this session without touching the
/// setting; the view hooks follow at once.
/// </summary>
void pilot_twist_dev_toggle();
/// <summary>
/// Developer (probe hotkey thread, Ctrl+Shift+H): steps the head hold off, 120 left, 160 left, 120 right, 160 right,
/// off. The held yaw (pitch 0) is written to the view's head target angles before each update, so the game clamps
/// and smooths it as it does a hat or tracker head movement.
/// </summary>
void pilot_twist_dev_cycle_hold();

/// <summary>
/// Hooks the exe's file service so the Options menu can offer Display Mode (Options.ini [GraphicsOptions]
/// DisplayMode). Call from DllMain.
/// </summary>
bool install_menu_hooks();
/// <summary>Hooks OptionsScreen.dll once it is loaded.</summary>
void menu_on_modules_loaded();
/// <summary>
/// This DLL's absolute path, for the lobby screen's MenuCodeDLL, or empty while the lobby can't run (MenuSystem.dll not
/// loaded yet, an unexpected build, an unknown path). Decides once and logs it. Menu thread.
/// </summary>
std::wstring lobby_menu_code_path();
/// <summary>menu_hooks.cpp: the Hangar's hidden back-to-lobby item number in this lobby session's Hangar (0: none).</summary>
int lobby_hangar_return_item();
/// <summary>lobby_flow.cpp: hooks HangarScreen.dll's event handler (Fly in a lobby session goes back to the lobby).</summary>
void lobby_flow_on_modules_loaded();
/// <summary>lobby_screen.cpp: an int from the game's Options.ini object; false if it is not there.</summary>
bool game_option_int(const wchar_t* section, const wchar_t* key, int* out);
/// <summary>lobby_screen.cpp: the Hangar's handler (self) got an event in a lobby session; true if the lobby took it
/// (Fly: back to the lobby).</summary>
bool lobby_hangar_event(void* self, const int* ev);

/// <summary>Reads LAZYSF2_PROBE_DIR once. The probe (developer tool) is active only when it is set. Call from DllMain.</summary>
void install_probe_hooks();
/// <summary>DLL detach: waits at most 1 s for an automatic snapshot's writer thread, then leaves it. Not under any lock of ours.</summary>
void probe_shutdown();
/// <summary>Starts the developer features (recorder, replay, host, hotkeys) once the aircraft tracker is active.</summary>
void probe_on_modules_loaded();

/// <summary>
/// Aircraft tracker (aircraft_track.cpp): runs on every module load and, whenever an AircraftObject.dll image
/// appears, installs the constructor, destructor and Update hooks if a client needs them (the probe, or the
/// Tacview Recording setting). Re-hooks after the DLL is reloaded.
/// </summary>
void aircraft_track_on_modules_loaded();
/// <summary>
/// Game thread, when an Options page is left: installs or removes the hooks as the setting and probe
/// require. A setting change never applies mid-flight.
/// </summary>
void aircraft_track_reevaluate();
/// <summary>True while the tracking hooks are installed.</summary>
bool aircraft_track_active();
/// <summary>Why the hooks are installed: the probe, the setting, or both. Meaningful only while active.</summary>
bool aircraft_track_for_probe();
bool aircraft_track_for_tacview();

/// <summary>
/// Tacview recorder (tacview_recorder.cpp): with Options > Other > Tacview Recording On, writes each flight to
/// Saved Games\ThirdWire\&lt;exe&gt;\Tacview\SF2 &lt;date time&gt;.acmi from a writer thread (packed to .zip.acmi afterwards). Registers the sim step
/// callback once.
/// </summary>
void tacview_recorder_on_modules_loaded();
/// <summary>Game thread, an Options page was left: closes the recording of a flight that has ended.</summary>
void tacview_recorder_menu_moment();
/// <summary>
/// Called by the tracker (any thread, after releasing its lock) when the aircraft table has just become
/// empty. On the recording's game thread it closes the recording at once (final frame, log line); on any other thread it sets a flag that
/// the next step or menu moment acts on.
/// </summary>
void tacview_recorder_table_empty();
/// <summary>DllMain detach: closes the file. At process exit it also writes what the dead writer left in its ring.</summary>
void tacview_recorder_shutdown(bool process_exit);
/// <summary>
/// Developer (probe hotkey thread, Ctrl+Shift+X): asks the recorder to set the damage of the nearest structure it
/// watches (drawn, not destroyed) to destroyed at the next player step, so the destruction poll records it.
/// </summary>
void tacview_destroy_nearest_request();
/// <summary>Sim step hooks (sim_hooks.cpp); the tracker calls these after its own build check.</summary>
bool sim_install(HMODULE aircraft_dll);
void sim_remove(HMODULE aircraft_dll);

/// <summary>One live aircraft object, as seen by probe_with_aircraft(). Pointers are valid only inside the callback.</summary>
struct LiveAircraft {
    void* obj;
    const wchar_t* name;     // the constructor's name argument, at most 63 characters
    unsigned long long seq;  // construction sequence number: unique, increasing, never reused (an address can be)
    bool is_player;          // the first aircraft constructed since the table was last empty; at most one entry, and
                             // none once the player is destroyed. The only "which is the player" test to use.
    AircraftSide side;       // FRIENDLY/ENEMY/NEUTRAL flag bit read when sampled; Unknown if the masks are not known
    unsigned long long created_ms;  // GetTickCount64() at construction: diagnostics only, never identity
    const char* type_name;   // aircraft type as ASCII (e.g. "F-100D"), at most 31 characters; empty when unknown
};
/// <summary>
/// The tracker's sequence number of the live aircraft at obj, or 0 if none or tracking is off. Takes the
/// table lock shared; the caller must not hold the weapon table's lock.
/// </summary>
unsigned long long aircraft_seq_for_object(const void* obj);
/// <summary>The FRIENDLY, ENEMY and NEUTRAL flag masks of the hooked AircraftObject.dll (0 for one that cannot be read); false when tracking is off.</summary>
bool aircraft_side_masks(uint32_t* friendly, uint32_t* enemy, uint32_t* neutral);
/// <summary>
/// Runs callback while holding the aircraft table lock shared, so no listed object is destroyed until it
/// returns. The callback must not log or do file I/O. The table is empty unless tracking is active.
/// </summary>
void probe_with_aircraft(void (*callback)(const LiveAircraft* list, size_t count, void* ctx), void* ctx);
/// <summary>One live weapon (missile, bomb or rocket in flight), as seen by probe_with_weapons().</summary>
struct LiveWeapon {
    void* obj;
    const char* name;                // weapon type name as ASCII, at most 63 characters (may be empty)
    unsigned long long seq;          // the weapon tracker's own sequence number: unique, increasing, never reused
    unsigned char weapon_class;      // WeaponClass
    unsigned long long parent_seq;   // the shooting aircraft's seq (LiveAircraft::seq), or 0 if unknown
    AircraftSide side;               // Unknown if neither flag bit is set or masks are unknown
};
/// <summary>
/// Weapon tracker (weapon_track.cpp): follows weapons from release, under the same enable rule and calling
/// pattern as the aircraft tracker (module watch; reevaluate is called by aircraft_track_reevaluate()).
/// </summary>
void weapon_track_on_modules_loaded();
void weapon_track_reevaluate();
bool weapon_track_active();
/// <summary>A new flight begins: forgets (and logs, if non-zero) the count of weapons skipped because the table was full.</summary>
void weapon_track_new_flight();
/// <summary>
/// Clears the weapon table when the flight is over; returns the live weapons dropped. Takes only the weapon
/// lock; call with no other tracker lock held.
/// </summary>
size_t weapon_track_drop_all();
/// <summary>
/// Runs callback while holding the weapon table lock shared; the callback must not log or do file I/O. Empty
/// unless the weapon hooks are installed. Do not call while holding the aircraft table lock exclusively.
/// </summary>
void probe_with_weapons(void (*callback)(const LiveWeapon* list, size_t count, void* ctx), void* ctx);
/// <summary>
/// Incremented (release) when a tracked aircraft is destroyed, its address is reused, or the tracker re-hooks
/// a reloaded AircraftObject.dll. A cached aircraft pointer is trusted only while this still has the value it
/// had at the last validation.
/// </summary>
extern std::atomic<uint32_t> g_aircraft_generation;
/// <summary>The probe's output directory (LAZYSF2_PROBE_DIR, validated), or nullptr when the probe is off.</summary>
const wchar_t* probe_output_dir();

/// <summary>
/// Puppet mode (developer tool): Ctrl+Shift+P drives one AI aircraft along a level path from the render
/// thread for at most 120 s. puppet_toggle() is called by the hotkey thread; puppet_on_present() from
/// IDXGISwapChain::Present and costs one relaxed atomic load while idle.
/// </summary>
void puppet_toggle();
void puppet_run_frame();
extern std::atomic<int> g_puppet_state;
inline void puppet_on_present() {
    if (g_puppet_state.load(std::memory_order_relaxed) != 0) puppet_run_frame();
}

/// <summary>
/// Missile spawn test (developer tool): Ctrl+Shift+M creates one missile 200 m ahead of the player on the next
/// Present and follows it for 20 s. missile_spawn_on_modules_loaded() hooks MissileObject's constructor to learn
/// weapon ids.
/// </summary>
void missile_spawn_request();
void missile_spawn_run_frame();
void missile_spawn_on_modules_loaded();
extern std::atomic<int> g_missile_state;
inline void missile_spawn_on_present() {
    if (g_missile_state.load(std::memory_order_relaxed) != 0) missile_spawn_run_frame();
}

// Simulation step (sim_hooks.cpp): hooks the aircraft's Update(dt), which runs once per simulation step on the
// game thread and not while paused. The aircraft tracker installs it. The game clock advances on each step of
// the player's aircraft. The readers below are valid on the game thread only (inside a step callback).

/// <summary>True while sim has a cached player aircraft.</summary>
bool sim_player_known();
/// <summary>The cached player aircraft, or null (game thread).</summary>
void* sim_player_object();
/// <summary>Incremented on every player loss and change of player. A run stores it at its start and ends when it differs.</summary>
uint32_t sim_player_epoch();
double sim_game_time();
uint64_t sim_step_count();
/// <summary>
/// Called on the game thread after an aircraft's Update returns and the clock is updated, for every aircraft.
/// The aircraft is valid only for the call. Keep it cheap.
/// </summary>
using SimStepCallback = void (*)(void* aircraft, float dt, bool is_player);
/// <summary>Registers a step callback (8 slots; register once at install time). False if the set is full.</summary>
bool sim_register_step_callback(SimStepCallback cb);
/// <summary>While a developer run (recording, replay) is active, logs one clock line per second of game time.</summary>
void sim_set_run_active(bool active);

/// <summary>
/// Recorder (developer tool): Ctrl+Shift+R records the player's aircraft state on every player step for at most
/// 180 s and writes &lt;probe dir&gt;\rec_NNN.bin. recorder_toggle() is called by the hotkey thread and only sets a
/// request; the game thread acts on it.
/// File format (little endian, no padding): "LZRC", uint32 version (1), uint32 count, then count RecSample.
/// </summary>
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
/// <summary>
/// The latest finished recording (game thread only), or nullptr with *count = 0 while none exists or one is
/// being recorded. Valid until the next recording starts.
/// </summary>
const RecSample* recorder_latest(uint32_t* count);

/// <summary>
/// Replay (developer tool): Ctrl+Shift+T replays the latest recording onto the nearest AI aircraft with
/// network-style smoothing; Ctrl+Shift+Y cycles the feed rate 60/10/5/2 Hz. The hotkey thread only sets
/// requests; the game thread acts. See replay.cpp and replay_math.h.
/// </summary>
void replay_on_modules_loaded();
void replay_toggle();
void replay_cycle_rate();
/// <summary>True while a replay runs. The recorder refuses to start then.</summary>
bool replay_active();

/// <summary>
/// Player jump (developer tool, player_jump.cpp): Ctrl+Shift+J moves the player 1 km ahead and turns it 90 degrees
/// (position, angles, matrix, world velocity); Ctrl+Shift+K moves it only. Then logs its state for 5 s. The hotkey
/// thread only sets the request; the player's next step acts on it.
/// </summary>
void player_jump_on_modules_loaded();
void player_jump_request(bool full);
/// <summary>
/// Lock probe (developer tool, lock_probe.cpp): Ctrl+Shift+A places an AI 3 km ahead of the player, Ctrl+Shift+O
/// locks it with the player's radar, Ctrl+Shift+I makes it lock the player. Requests: 1 place, 2 player lock, 3 AI lock.
/// </summary>
void lock_probe_on_modules_loaded();
void lock_probe_request(int what);
/// <summary>Step order (developer tool, step_order.cpp): logs once per player whether it steps before the other aircraft in a frame.</summary>
void step_order_on_modules_loaded();

/// <summary>
/// Host mode (developer tool; src/net/host.cpp): LAZYSF2_HOST=port or address:port (default 127.0.0.1) makes the
/// mod listen for clients over UDP (ENet) and drive one AI aircraft per client as its avatar. The variable needs the
/// probe; a lobby session (host_session_start) does not. host_install() reads the variable once (DllMain, after
/// install_probe_hooks());
/// host_on_modules_loaded() starts the network thread and registers the step callback; host_shutdown() stops the
/// thread and returns every avatar to the AI (DLL unload only).
/// </summary>
void host_install();
void host_on_modules_loaded();
void host_shutdown();
/// <summary>True while any client holds an avatar. Puppet and replay refuse to start then, and the host refuses clients while one runs.</summary>
bool host_has_clients();

/// <summary>
/// Client mode (developer tool; src/net/client.cpp): LAZYSF2_CONNECT=host:port makes the mod join a host once the
/// player is airborne, move the own jet onto the host's wing (LAZYSF2_NET_JOIN=move, the default, or none), send its
/// State at 20 Hz of host time and show the host and the other clients on AI aircraft. LAZYSF2_CALLSIGN names the
/// player (default PLAYER). The variables need the probe (with LAZYSF2_HOST also set, both stay off); a lobby session
/// (client_session_start) does not.
/// client_install() reads the variables (DllMain, after host_install()); client_on_modules_loaded() registers the
/// step callback; client_shutdown() ends the connection and gives the avatars back (DLL unload only).
/// </summary>
void client_install();
void client_on_modules_loaded();
void client_shutdown();
/// <summary>True from the client's Welcome until its session ends. Any thread.</summary>
bool client_welcomed();
/// <summary>
/// Shared mission world (world_sync.cpp): the variables need the probe; a lobby session (world_session_begin) does not.
/// LAZYSF2_WORLD_SAVE=1
/// saves each Single Mission accepted on the New page to &lt;probe dir&gt;\world_NNN.msn (plus world_NNN.sidecar.txt:
/// seed and start options) and runs from it; LAZYSF2_WORLD_LOAD=&lt;file&gt; runs that file instead, with the
/// sidecar's seed and options forced. Both write world_NNN.manifest.txt after the mission's objects are created.
/// LAZYSF2_WORLD_SEED=n fixes the seed for a save. LAZYSF2_WORLD_SEAT=n puts the player on member n (n = 1 is "x-2") of
/// the player flight in every single mission. LAZYSF2_WORLD_REMOTE=1 makes every mission object but the player's
/// REMOTEOBJECT after the start and blocks local damage to REMOTEOBJECT objects (the client's world).
/// LAZYSF2_WORLD_JOIN=1 (client mode) fetches the host's mission in menus and flies it at the next Accept; a host
/// with LAZYSF2_WORLD_SAVE serves the mission it flies. world_install() reads the variables (DllMain, after
/// install_probe_hooks()); world_on_modules_loaded() hooks SingleMissionScreen.dll and SingleMission.dll images.
/// </summary>
void world_install();
void world_on_modules_loaded();

/// <summary>
/// Lobby sessions (net_session.cpp; menus only, the menu thread): start a host or a client and the shared world at run
/// time, without the probe, and close them again. The variables (LAZYSF2_HOST, LAZYSF2_CONNECT, LAZYSF2_WORLD_*) win:
/// with them set, a start is refused with the reason in why.
/// </summary>
enum class NetRole { None, Host, Client };
bool net_session_host(uint16_t port, const char* callsign, const std::string& password, std::string* why);
bool net_session_join(const char* host, uint16_t port, const char* callsign, const std::string& password,
                      std::string* why);
/// <summary>A joined session's mission fetch was refused for the password. Menus.</summary>
bool net_session_wrong_password();
/// <summary>world_sync.cpp: the mission fetch was refused for the password.</summary>
bool world_fetch_wrong_password();
/// <summary>world_sync.cpp: the mission fetch could not find the host (its name did not resolve).</summary>
bool world_fetch_host_unknown();
/// <summary>A joined session cannot find its host. Menus.</summary>
bool net_session_host_unknown();
void net_session_close();
NetRole net_session_role();
/// <summary>A lobby session is open: the aircraft and weapon trackers are wanted. Any thread.</summary>
bool net_session_wanted();
/// <summary>host.cpp: host mode is on (the variable or a lobby session). Any thread.</summary>
bool host_configured();
/// <summary>host.cpp, menus only: starts or stops host mode for a lobby session; why: a reason literal.</summary>
bool host_session_start(const char* addr, uint16_t port, const char* callsign, const std::string& password,
                        uint32_t content_hash, const char** why);
/// <summary>host.cpp, lobby sessions: the roster as the host sees it; the host's own seat wish (-1: keep; also applied
/// to its flight through world_set_own_seat); the mission summary lines and Launch count sent to the clients.</summary>
bool host_lobby_view(net::HostCore::LobbyView* out);
uint8_t host_lobby_seat(int want_seat);
void host_lobby_info(const char* const lines[3], uint32_t launch_seq);
/// <summary>content_hash.cpp: this install's content (game cats, the player's aircraft, weapons, ground objects and
/// terrains folders), cached. Menu thread.</summary>
uint32_t content_hash();
/// <summary>world_sync.cpp: the member of the player flight this PC's player flies (the host's lobby seat; -1: stock).</summary>
void world_set_own_seat(int member);
/// <summary>world_sync.cpp, a joined lobby session: the latest roster (and round trip), Ready, a seat wish.</summary>
bool world_lobby_roster(net::LobbyRosterMsg* out, uint32_t* rtt_ms);
void world_lobby_ready(bool ready);
void world_lobby_want_seat(int seat);
void host_session_stop();
/// <summary>host.cpp: the bound port once the network thread runs (false before); failed: it could not bind.</summary>
bool host_session_port(uint16_t* port, bool* failed);
/// <summary>client.cpp, menus only: starts or stops client mode for a lobby session (it connects when a flight starts).</summary>
bool client_session_start(const char* host, uint16_t port, const char* callsign, const std::string& password,
                          const char** why);
/// <summary>client.cpp: the configured client's session password (for the mission fetch).</summary>
bool client_auth(net::AuthClient* out);
void client_session_stop();
/// <summary>world_sync.cpp, menus only: the shared world for a lobby session (host: save and serve each Accept;
/// client: fetch the host's mission and fly it). dir holds the world files unless the probe is on.</summary>
bool world_session_begin(bool host, const std::wstring& dir, const char* callsign = nullptr, uint32_t content_hash = 0);
void world_session_end();

/// <summary>
/// Host mode: the mission package (net/world_proto.h) to serve to clients that ask for it (T5). Any thread; kept
/// until replaced, and handed to the network core once it runs. No-op without LAZYSF2_HOST.
/// </summary>
/// members and host_member describe the host's own flight, for the seat rule (net/world_proto.h assign_seat).
void host_publish_mission(const uint8_t* packed, size_t n, uint32_t id, uint32_t raw, int members, int host_member);
/// <summary>Sends one world message to every welcomed client (reliable: WorldEvents). True if any took it.</summary>
using WorldSendFn = bool (*)(const uint8_t* data, size_t len, bool reliable);
/// <summary>True once this host serves a mission (T5): it then streams its world to its clients (T6).</summary>
bool world_host_streaming();
/// <summary>
/// Host, game thread, the player's step, with clients: sends the moving mission objects' states (10/2/1 Hz by distance to
/// the nearest of players[0..n_players)) and, once a second, the events (destroyed, awake, asleep, damage). T6.
/// </summary>
void world_host_tick(double now, const double (*players)[3], int n_players, WorldSendFn send);
/// <summary>Sends one world message to one client slot (reliable: on the events queue).</summary>
using WorldSendToFn = bool (*)(int slot, const uint8_t* data, size_t len, bool reliable);
/// <summary>
/// Host, game thread, right after a client's Welcome in a shared world: sends it the late-join snapshot (T8), a
/// WorldSnapshot head and the current events in WorldEvents messages. False if a part could not be queued.
/// </summary>
bool world_host_snapshot(int slot, uint32_t mission_id, WorldSendToFn send);
/// <summary>Client in a world join, game thread: applies one WorldStates or WorldEvents message to the remote copies.</summary>
void world_client_apply(const uint8_t* data, size_t len, double host_now);
/// <summary>
/// The aircraft that is member `member` of the player flight, for a peer's avatar in a shared world (its seat, from
/// the host: the Welcome or PeerInfo). Null outside a shared world, for kNoSeat, or when that member does not exist.
/// </summary>
void* world_member_object(int member);
/// <summary>Client in a world join: the tag its fetch and its Hello carry (the host's seat key); 0 when not joining.</summary>
uint32_t world_client_tag();
/// <summary>
/// Client, game thread, on a Welcome: false when it flies another mission than the host's (the caller leaves with
/// kByeWrongMission). Takes the host's seat numbers. True outside a world join.
/// </summary>
bool world_client_welcome(uint32_t mission_id, uint8_t seat, uint8_t host_seat);
/// <summary>Client, game thread: the session ended; the next Accept flies only a mission fetched again.</summary>
void world_client_session_end();
/// <summary>True if the host's or the client's Phase 4 code drives this aircraft as an avatar (game thread).</summary>
bool host_drives_object(const void* obj);
bool client_drives_object(const void* obj);
/// <summary>Sends one AvatarState of the local player's jet (to the host, or from the host to every client).</summary>
using AvatarSendFn = bool (*)(const uint8_t* data, size_t len);
/// <summary>
/// Game thread, the player's step, in a shared world: sends the player's radar mode and lock target (by stable key)
/// and its configuration bits when they change (T7).
/// </summary>
void world_avatar_tick(void* player, uint16_t own_peer, AvatarSendFn send);
/// <summary>Game thread: applies a peer's AvatarState to the aircraft that is its avatar here (config, radar, RWR,
/// throttle and engines).</summary>
void world_avatar_apply(const uint8_t* data, size_t len, void* avatar);
/// <summary>From the avatar's own step: writes that peer's last controls (throttle, engines) again, so an avatar taken
/// or retaken after the state came still shows them. Does nothing before the peer's first AvatarState this flight.</summary>
void world_avatar_refresh(int peer, void* avatar);
/// <summary>A peer left: its next state is applied as new (the RWR hears its lock again).</summary>
void world_avatar_forget(int peer);
/// <summary>A session starts or ends: our own state is sent again in full, and every peer's applied as new.</summary>
void world_avatar_session_reset();
/// <summary>Client: the aircraft that shows peer id here (its Phase 4 avatar), or null.</summary>
void* client_avatar_object(int peer);

// ---- Phase 5: keys, damage and gun hits across PCs (game thread) ----
/// <summary>The mission key (slot and member, aircraft or ground) of an object id; false if it has none.</summary>
bool world_key_of_object(uint32_t id, uint8_t* kind, uint16_t* a, uint16_t* b);
/// <summary>The object a key names here (and its id), or null.</summary>
void* world_object_of_key(uint8_t kind, uint16_t a, uint16_t b, uint32_t* id);
/// <summary>The local player's object id, or 0xffffffff.</summary>
uint32_t world_player_id();
/// <summary>An object's position, id and whether its damage level is below destroyed. SEH-guarded.</summary>
bool world_object_state(const void* obj, double pos[3], uint32_t* id, bool* alive);
/// <summary>Calls the object's damage interface slot 4, 5 or 6 with the call's own arguments, after checking the slot
/// is a function we read. 0 done, 1 refused, 2 faulted.</summary>
int world_damage_call(void* obj, int slot, const uint32_t* args);
/// <summary>
/// From the damage hooks (hit_claims.cpp): a gun hit on a remote copy by our player becomes a claim to the host
/// (remote), and on the host a gun hit on a client's avatar goes to that client. True if taken (the call is dropped).
/// </summary>
bool hits_on_damage(void* obj, int slot, const uint32_t* args, bool remote);
/// <summary>Host: a client's claim (checked, then applied here or forwarded to the target's owner).</summary>
void hits_host_claim(int slot, const uint8_t* data, size_t len);
/// <summary>Client: a hit on our own jet from the host.</summary>
void hits_client_apply(const uint8_t* data, size_t len);
/// <summary>A session starts or ends: the claim rate limits and shot records start again.</summary>
void hits_session_reset();
/// <summary>Host: a client slot was released (its limits and shots start again); its guns trigger (AvatarState) and
/// its launches (WeaponLaunch, its own weapon id), which its claims must match.</summary>
void hits_forget(int slot);
void hits_note_guns(int slot, uint8_t guns);
void hits_note_launch(int slot, uint16_t weapon_id);
/// <summary>Host (host.cpp): the client slot whose avatar obj is, or -1; that slot's avatar; a reliable or unreliable
/// world message for one client; and whether a shared-world session with clients is running.</summary>
int host_avatar_slot(const void* obj);
void* host_avatar_object(int slot);
bool host_world_send_to(int slot, const uint8_t* data, size_t len, bool reliable);
bool host_world_active();
/// <summary>Client (client.cpp): a reliable world message for the host, and whether a shared-world session runs.</summary>
bool client_world_send(const uint8_t* data, size_t len, bool reliable = true);
bool client_world_active();
/// <summary>Host: a reliable world message for every client (true alone or if any queued it).</summary>
bool host_world_send_all(const uint8_t* data, size_t len, bool reliable = true);
/// <summary>Stores and consumables (stores_sync.cpp), game thread: the player's step sends its loadout (every 10 s
/// and on change) and consumables (every 2 s); a peer's message goes onto the avatar that shows that peer.</summary>
void stores_tick(void* player);
void stores_apply(const uint8_t* data, size_t len, void* avatar);
/// <summary>Seat takeover helpers: an aircraft's loaded stations as wire entries; the same entries rebuilt on a local
/// (not REMOTEOBJECT) jet through the game's rearm called directly; its internal and external fuel, read and set.</summary>
size_t stores_read_entries(void* obj, net::LoadoutEntry* out, size_t max);
void stores_apply_entries(void* obj, const net::LoadoutEntry* e, size_t n, const char* what);
bool stores_read_fuel(void* obj, float* internal, float* external);
void stores_set_fuel(void* obj, float internal, float external);
/// <summary>Seat takeover (seat_takeover.cpp): the host sends a client its seat aircraft's state after the Welcome;
/// the client's jet takes it over in place. world_player_object: our player's aircraft, or null.</summary>
bool seat_state_capture(int seat, void* obj, net::SeatStateMsg* out);
void seat_state_send(int slot, const net::SeatStateMsg& m);
void seat_state_apply(const uint8_t* data, size_t len, double host_now);
void* world_player_object();
/// <summary>Client: writes the player's pose (position, velocity, angles; the matrix, heading copy and speed follow),
/// as the join move does.</summary>
void client_place_player(void* player, const double pos[3], const float vel[3], const float ang[3]);
/// <summary>Damage, death, ejection and countermeasures (damage_sync.cpp), game thread: the player's step sends its
/// jet's damage (polled 4 times a second, on change) and its chaff/flare releases; a peer's goes onto its avatar.</summary>
void damage_tick(void* player);
void damage_apply(const uint8_t* data, size_t len, void* avatar);
/// <summary>The damage hooks' detours (installed with the other AircraftObject hooks in world_sync.cpp).</summary>
void __fastcall h_aircraft_kill_event(void* obj);
void __fastcall h_incoming_missile(void* iface, void* edx, int missile, int stage);
extern void* g_o_aircraft_kill_event;
extern void* g_o_incoming_missile;
/// <summary>A joined client's world is remote (the T4 block is on); the aircraft REMOTEOBJECT mask (0 before).</summary>
/// <summary>The mission clock (handover task 1, mission_clock.cpp): the host sends it every 2 s; a client sets its own
/// through the mission's setter when it is off. world_*: MSN +0x10/+0x14, read, and set through MSN slot 19 (checked;
/// 0 done, 1 refused, 2 faulted).</summary>
void mission_clock_tick();
/// <summary>Client: ENet's smoothed round trip to the host in seconds (0 outside a session).</summary>
double client_rtt_s();
void mission_clock_apply(const uint8_t* data, size_t len, double host_now);
bool world_mission_clock(float* t10, float* t14);
int world_set_mission_clock(float t10, float t14);
/// <summary>The ground-contact step's detour (remote_ground.cpp, handover task 2).</summary>
void __fastcall h_ground_contact(void* obj);
extern void* g_o_ground_contact;
/// <summary>Authority handover (handover.cpp, task 4): the probe's solo hand-back (Ctrl+Shift+C); the step callback;
/// the client's offer when the host's session starts (if it loaded first); the host's handling of an offer; the
/// client's handling of the answer.</summary>
void handover_probe_request();
/// <summary>From Present (game thread): runs a pending probe press even when no player step runs.</summary>
void handover_on_present();
void handover_on_modules_loaded();
void handover_client_welcomed();
void handover_host_message(int slot, const uint8_t* data, size_t len, double rtt_s);
void handover_client_message(const uint8_t* data, size_t len);
/// <summary>We ran the world in this flight and offered it (no seat takeover then); the client's session ended (the
/// unsent offer is dropped); the host's client slot was released (its offer is dropped).</summary>
bool handover_we_owned_world();
void handover_session_end();
void handover_host_forget(int slot);
/// <summary>world_sync.cpp: destroys an aircraft or ground object through its own slot 29 (checked; no kill entry).
/// 0 done, 1 refused, 2 faulted.</summary>
int world_destroy_object(void* obj);
/// <summary>Logs a kill as the game does (the object's class kill event: aircraft slot 30 and its ground, vehicle and
/// ship equivalents; slot-checked), with killer and weapon written first (+0x318/+0x31c). On the host the hooks keep
/// it, so it lands on the host's single record. 0 done, 1 refused, 2 faulted.</summary>
int world_log_kill(void* obj, uint32_t killer, uint32_t weapon);
/// <summary>world_sync.cpp: a joined client's mark pass waits (it loaded first and runs the world); run it now; mark
/// or hand back the whole world (fix-ups included; players' avatars stay remote; the count, -1 on failure); every
/// live slot member but the player and the avatars; the flights (aircraft slots) and a flight's waypoint, read or set.</summary>
bool world_remote_deferred();
void world_remote_now();
int world_set_remote_all(bool remote);
void world_for_each_object(void (*cb)(uint8_t kind, uint16_t a, uint16_t b, void* obj, void* ctx), void* ctx);
int world_flight_count();
bool world_flight_waypoint(int slot, int* wp, bool set, int value);
bool world_remote_on();
uint32_t world_remote_mask();
uint32_t world_remote_mask_ground();
/// <summary>The peer's jet was destroyed in its current life (no aircraft is taken for it until a new one).</summary>
bool damage_peer_dead(int peer);
template <int K>
void __fastcall h_ground_kill_event(void* obj);
extern void* g_o_ground_kill_event[3];
/// <summary>The host's game time: the game time on the host, the client's estimate of it on a client.</summary>
double net_host_now();
double client_host_now();
/// <summary>Weapon launches (weapon_net.cpp): the weapon id's seeker (GuidanceType != 0); a released weapon (from
/// the weapon tracker's Release hook, in flight only); a peer's launch to replay here (host_now in host time).</summary>
bool weapon_guided(uint32_t weapon_id);
void weapons_on_release(void* missile);
void weapons_apply_launch(const uint8_t* data, size_t len, double host_now);
/// <summary>The weapon type's name for an id here; the id whose name has this weapon_name_hash (-1 none); an
/// aircraft's remote interface event (slot 1, the game's own remote-aircraft protocol: 2 fire a station, 0x16
/// rearm, ...; network-recon §11): 0 done, 1 refused (not the interface we read), 2 faulted.</summary>
bool weapon_name_of(uint32_t id, char* out, size_t cap);
int weapon_id_by_hash(uint16_t hash);
int aircraft_remote_event(void* obj, int type, void* payload);
/// <summary>An avatar was given back: no native fire on it is awaited any more.</summary>
void weapons_forget_avatar(const void* avatar);
/// <summary>Client mode: the host address and port from LAZYSF2_CONNECT; false when client mode is off.</summary>
bool client_connect_target(char* host, size_t cap, uint16_t* port);
/// <summary>True if the environment variable is set to a non-empty value (an empty one counts as unset).</summary>
inline bool env_set(const wchar_t* name) {
    wchar_t b[2];
    return GetEnvironmentVariableW(name, b, 2) > 0;
}
/// <summary>A network session is running: the host has clients, or the client is welcomed. Any thread.</summary>
inline bool net_session_active() { return host_has_clients() || client_welcomed(); }

}  // namespace LazyWorks::SF2Patch::hooks
