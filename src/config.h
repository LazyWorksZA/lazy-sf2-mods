#pragma once

namespace lazysf2::config {

enum class MouseMode {
    Stock,   // game reads relative DirectInput deltas in exclusive mode (unchanged)
    Cursor,  // game cursor follows the Windows cursor (for remote-control tools such as RustDesk)
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

// Display mode from the game's Options.ini, read once on first use. Must NOT be called from
// DllMain (it uses the shell to find Saved Games).
DisplayMode display_mode();
inline bool borderless() { return display_mode() == DisplayMode::Borderless; }

}  // namespace lazysf2::config
