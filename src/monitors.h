#pragma once
// Monitors as Windows numbers them (\\.\DISPLAYn, the numbers shown in Settings > Display).
#include <windows.h>

#include <vector>

namespace lazysf2::monitors {

struct Monitor {
    int number = 0;  // n in \\.\DISPLAYn
    RECT rect{};     // in real pixels (the process is per-monitor DPI aware)
    bool primary = false;
};

// All active monitors, sorted by number.
std::vector<Monitor> list();

// Rect of monitor `number`; false if there is no such monitor (e.g. unplugged).
bool rect_of(int number, RECT* out);

// Number of the monitor containing most of `hwnd`, or 0.
int number_of_window(HWND hwnd);

}  // namespace lazysf2::monitors
