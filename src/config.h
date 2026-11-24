#pragma once

namespace lazysf2::config {

enum class MouseMode {
    Stock,   // game reads relative DirectInput deltas in exclusive mode (unchanged)
    Cursor,  // game pointer sits on the Windows cursor (stock builds its own from raw deltas, which
             // never quite matches and wanders over remote-control tools)
};

// Values of Options.ini [GraphicsOptions] DisplayMode, as written by the Options menu.
enum class DisplayMode : int {
    Fullscreen = 0,  // stock exclusive fullscreen (also used when the key is missing or invalid)
    Borderless = 1,
};

struct Config {
    MouseMode mouse = MouseMode::Stock;
};

// Reads lazysf2.ini next to our DLL (safe in DllMain). A missing file or key keeps the defaults.
//   [Mouse]
//   Mode=Cursor      ; or Stock
void load();
const Config& get();

// Current display mode. Starts from the game's Options.ini (read once on first use) and changes
// when the player picks another mode in the Options menu (applied live). Must NOT be first called
// from DllMain or under the loader lock (it uses the shell to find Saved Games).
DisplayMode display_mode();
void set_display_mode(DisplayMode mode);
inline bool borderless() { return display_mode() == DisplayMode::Borderless; }

// Options.ini [GraphicsOptions] DisplayMonitor: n = Windows display number (\\.\DISPLAYn) to run
// on; 0 / missing = stock (the game picks the monitor). Read with the display mode, same rules.
int display_monitor();
void set_display_monitor(int number);

}  // namespace lazysf2::config
