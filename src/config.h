#pragma once

namespace lazysf2::config {

enum class MouseMode {
    Stock,   // game reads relative DirectInput deltas in exclusive mode (unchanged)
    Cursor,  // game cursor follows the Windows cursor (for remote-control tools such as RustDesk)
};

struct Config {
    bool borderless = true;  // Phase 2 spike: always on until the Options menu setting exists
    MouseMode mouse = MouseMode::Stock;
};

// Reads lazysf2.ini next to our DLL. A missing file or key keeps the defaults above.
//   [Mouse]
//   Mode=Cursor      ; or Stock
void load();
const Config& get();

}  // namespace lazysf2::config
