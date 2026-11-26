#pragma once

// Settings, all stored in the player's own Saved Games\ThirdWire\<exe>\Options.ini by the game's
// Options menu (rows added by the mod). They are read once on first use and can then change while
// the game runs. None of these may be first called from DllMain or under the loader lock (finding
// Saved Games uses the shell); hooks check them when they are called, not when they are installed.
#include <string>

namespace LazyWorks::SF2Patch::config {

/// <summary>Options.ini [ControlOptions] MouseMode (Options &gt; Controls &gt; Mouse Pointer).</summary>
enum class MouseMode : int {
    Stock = 0,   // the game builds its own pointer from raw DirectInput deltas, exclusive mouse
    Cursor = 1,  // the game pointer sits on the Windows cursor (default, also when the key is missing:
                 // the stock pointer never quite matches Windows' and wanders over remote desktop)
};

/// <summary>Options.ini [GraphicsOptions] DisplayMode (Options &gt; Graphics &gt; Display Mode).</summary>
enum class DisplayMode : int {
    Fullscreen = 0,  // stock exclusive fullscreen (also used when the key is missing or invalid)
    Borderless = 1,
};

DisplayMode display_mode();
void set_display_mode(DisplayMode mode);
inline bool borderless() { return display_mode() == DisplayMode::Borderless; }

/// <summary>
/// Options.ini [GraphicsOptions] DisplayMonitor: n = Windows display number (\\.\DISPLAYn) to run
/// on; 0 / missing = stock (the game picks the monitor).
/// </summary>
int display_monitor();
void set_display_monitor(int number);

MouseMode mouse_mode();
void set_mouse_mode(MouseMode mode);

/// <summary>Options.ini [ControlOptions] PilotTwist (Options &gt; Controls &gt; Pilot Twist): 1 = on, else off (default).</summary>
bool pilot_twist();
void set_pilot_twist(bool on);
/// <summary>For the module watch: the loaded value only, never loads. -1 = not loaded yet, 0 = off, 1 = on.</summary>
int pilot_twist_peek();

// Options.ini [ControlOptions] PilotTwistAngle and PilotTwistLean: hand-edited for now (no Options row yet), read
// with the other settings. Missing or invalid values give the defaults; values out of range are clamped.
constexpr double kPilotTwistAngleDefault = 30.0, kPilotTwistAngleMax = 45.0;  // degrees of extra view yaw
constexpr double kPilotTwistLeanDefault = 4.0, kPilotTwistLeanMax = 6.0;      // cm sideways
/// <summary>The extra view yaw in degrees. Never loads: the default until the settings are loaded (any thread).</summary>
double pilot_twist_angle_peek();
/// <summary>The sideways lean in cm. Never loads: the default until the settings are loaded (any thread).</summary>
double pilot_twist_lean_peek();

/// <summary>Options.ini [OtherOptions] TacviewRecording (0 or 1, default 0 = off), loaded with the other settings.</summary>
void set_tacview_recording(bool on);  // the Options page, on Exit
/// <summary>
/// For the module watch (it can run under the loader lock): reads the loaded value only, never loads anything.
/// -1 = not loaded yet (treat as off), 0 = off, 1 = on.
/// </summary>
int tacview_recording_peek();

/// <summary>
/// The game's per-user folder, Saved Games\ThirdWire\&lt;exe name&gt; (where Options.ini lives), or empty if it cannot be
/// found. A shell call: game thread at a safe moment only, never from the module watch or under a lock.
/// </summary>
std::wstring game_data_dir();

/// <summary>
/// LAZYSF2_BG_SIM (environment, read once): keep simulating while unfocused during a network session, Borderless
/// only. Default on; "0" turns it off. Single-player always keeps the stock pause.
/// </summary>
bool background_sim();

}  // namespace LazyWorks::SF2Patch::config
