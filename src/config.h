#pragma once

// Settings, all stored in the player's own Saved Games\ThirdWire\<exe>\Options.ini by the game's
// Options menu (rows added by the mod). They are read once on first use and can then change while
// the game runs. None of these may be first called from DllMain or under the loader lock (finding
// Saved Games uses the shell); hooks check them when they are called, not when they are installed.
#include <string>

namespace lazysf2::config {

// Options.ini [ControlOptions] MouseMode (Options > Controls > Mouse Pointer).
enum class MouseMode : int {
    Stock = 0,   // the game builds its own pointer from raw DirectInput deltas, exclusive mouse
    Cursor = 1,  // the game pointer sits on the Windows cursor (default, also when the key is missing:
                 // the stock pointer never quite matches Windows' and wanders over remote desktop)
};

// Options.ini [GraphicsOptions] DisplayMode (Options > Graphics > Display Mode).
enum class DisplayMode : int {
    Fullscreen = 0,  // stock exclusive fullscreen (also used when the key is missing or invalid)
    Borderless = 1,
};

DisplayMode display_mode();
void set_display_mode(DisplayMode mode);
inline bool borderless() { return display_mode() == DisplayMode::Borderless; }

// Options.ini [GraphicsOptions] DisplayMonitor: n = Windows display number (\\.\DISPLAYn) to run
// on; 0 / missing = stock (the game picks the monitor).
int display_monitor();
void set_display_monitor(int number);

MouseMode mouse_mode();
void set_mouse_mode(MouseMode mode);

// Options.ini [OtherOptions] TacviewRecording (0 or 1, default 0 = off), loaded with the other settings.
void set_tacview_recording(bool on);  // the Options page, on Exit
// For the module watch (it can run under the loader lock): reads the loaded value only, never loads anything.
// -1 = not loaded yet (treat as off), 0 = off, 1 = on.
int tacview_recording_peek();

// The game's per-user folder, Saved Games\ThirdWire\<exe name> (where Options.ini lives), or empty if it cannot be
// found. A shell call: game thread at a safe moment only, never from the module watch or under a lock.
std::wstring game_data_dir();

}  // namespace lazysf2::config
