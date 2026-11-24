#include "monitors.h"

#include <algorithm>
#include <cwchar>

namespace lazysf2::monitors {
namespace {

int display_number(const wchar_t* device) {
    // "\\.\DISPLAY12" -> 12
    const wchar_t* p = wcsstr(device, L"DISPLAY");
    return p ? _wtoi(p + 7) : 0;
}

BOOL CALLBACK collect(HMONITOR m, HDC, LPRECT, LPARAM data) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(m, &info)) {
        auto* out = reinterpret_cast<std::vector<Monitor>*>(data);
        out->push_back({display_number(info.szDevice), info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    }
    return TRUE;
}

}  // namespace

std::vector<Monitor> list() {
    std::vector<Monitor> all;
    EnumDisplayMonitors(nullptr, nullptr, &collect, reinterpret_cast<LPARAM>(&all));
    std::sort(all.begin(), all.end(), [](const Monitor& a, const Monitor& b) { return a.number < b.number; });
    return all;
}

bool rect_of(int number, RECT* out) {
    if (number <= 0) return false;
    for (const Monitor& m : list()) {
        if (m.number == number) {
            *out = m.rect;
            return true;
        }
    }
    return false;
}

int number_of_window(HWND hwnd) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    HMONITOR m = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    return (m && GetMonitorInfoW(m, &info)) ? display_number(info.szDevice) : 0;
}

}  // namespace lazysf2::monitors
