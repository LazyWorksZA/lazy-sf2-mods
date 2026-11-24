#include "config.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <atomic>
#include <string>

#include "log.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace lazysf2::config {
namespace {

Config g_config;
INIT_ONCE g_display_once = INIT_ONCE_STATIC_INIT;
std::atomic<DisplayMode> g_display{DisplayMode::Fullscreen};
std::atomic<int> g_monitor{0};

// Saved Games\ThirdWire\<exe name>\Options.ini, e.g. ...\StrikeFighters2\Options.ini.
std::wstring options_ini_path() {
    PWSTR saved = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved))) return {};
    std::wstring path = saved;
    CoTaskMemFree(saved);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name = exe;
    name = name.substr(name.find_last_of(L'\\') + 1);
    if (size_t dot = name.find_last_of(L'.'); dot != std::wstring::npos) name.resize(dot);

    return path + L"\\ThirdWire\\" + name + L"\\Options.ini";
}

BOOL CALLBACK load_display_mode(PINIT_ONCE, PVOID, PVOID*) {
    const std::wstring path = options_ini_path();
    const int v = path.empty() ? -1 : static_cast<int>(GetPrivateProfileIntW(L"GraphicsOptions", L"DisplayMode", -1, path.c_str()));
    g_display = (v == static_cast<int>(DisplayMode::Borderless)) ? DisplayMode::Borderless : DisplayMode::Fullscreen;
    LOG("config: %ls [GraphicsOptions] DisplayMode=%d -> %s", path.c_str(), v,
        g_display.load() == DisplayMode::Borderless ? "Borderless" : "Fullscreen (stock)");
    const int m = path.empty() ? 0 : static_cast<int>(GetPrivateProfileIntW(L"GraphicsOptions", L"DisplayMonitor", 0, path.c_str()));
    g_monitor = m > 0 ? m : 0;
    LOG("config: [GraphicsOptions] DisplayMonitor=%d%s", m, m > 0 ? "" : " (stock monitor choice)");
    return TRUE;
}

}  // namespace

void load() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    wchar_t* slash = (n && n < MAX_PATH) ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"lazysf2.ini");

    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"Mouse", L"Mode", L"Stock", mode, 32, path);
    if (_wcsicmp(mode, L"Cursor") == 0) {
        g_config.mouse = MouseMode::Cursor;
    } else if (_wcsicmp(mode, L"Stock") != 0) {
        LOG("config: unknown [Mouse] Mode=%ls, using Stock", mode);
    }
    LOG("config: mouse=%s", g_config.mouse == MouseMode::Cursor ? "Cursor" : "Stock");
}

const Config& get() { return g_config; }

DisplayMode display_mode() {
    InitOnceExecuteOnce(&g_display_once, &load_display_mode, nullptr, nullptr);
    return g_display.load();
}

void set_display_mode(DisplayMode mode) {
    InitOnceExecuteOnce(&g_display_once, &load_display_mode, nullptr, nullptr);  // don't let a later load overwrite it
    g_display = mode;
}

int display_monitor() {
    InitOnceExecuteOnce(&g_display_once, &load_display_mode, nullptr, nullptr);
    return g_monitor.load();
}

void set_display_monitor(int number) {
    InitOnceExecuteOnce(&g_display_once, &load_display_mode, nullptr, nullptr);
    g_monitor = number > 0 ? number : 0;
}

}  // namespace lazysf2::config
