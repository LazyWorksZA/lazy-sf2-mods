#include "config.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <atomic>
#include <string>

#include "log.h"

namespace lazysf2::config {
namespace {

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
std::atomic<DisplayMode> g_display{DisplayMode::Fullscreen};
std::atomic<int> g_monitor{0};
std::atomic<MouseMode> g_mouse{MouseMode::Cursor};
std::atomic<int> g_tacview{-1};  // -1 = not loaded yet

// Saved Games\ThirdWire\<exe name>, e.g. ...\StrikeFighters2 (the game's own per-user folder), or empty.
std::wstring game_data_path() {
    PWSTR saved = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved))) return {};
    std::wstring path = saved;
    CoTaskMemFree(saved);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name = exe;
    name = name.substr(name.find_last_of(L'\\') + 1);
    if (size_t dot = name.find_last_of(L'.'); dot != std::wstring::npos) name.resize(dot);

    return path + L"\\ThirdWire\\" + name;
}

// ...\StrikeFighters2\Options.ini
std::wstring options_ini_path() {
    std::wstring dir = game_data_path();
    return dir.empty() ? dir : dir + L"\\Options.ini";
}

int read_int(const std::wstring& path, const wchar_t* section, const wchar_t* key, int def) {
    return path.empty() ? def : static_cast<int>(GetPrivateProfileIntW(section, key, def, path.c_str()));
}

BOOL CALLBACK load(PINIT_ONCE, PVOID, PVOID*) {
    const std::wstring path = options_ini_path();

    const int v = read_int(path, L"GraphicsOptions", L"DisplayMode", -1);
    g_display = (v == static_cast<int>(DisplayMode::Borderless)) ? DisplayMode::Borderless : DisplayMode::Fullscreen;
    LOG("config: %ls [GraphicsOptions] DisplayMode=%d -> %s", log::log_path(path).c_str(), v,
        g_display.load() == DisplayMode::Borderless ? "Borderless" : "Fullscreen (stock)");

    const int m = read_int(path, L"GraphicsOptions", L"DisplayMonitor", 0);
    g_monitor = m > 0 ? m : 0;
    LOG("config: [GraphicsOptions] DisplayMonitor=%d%s", m, m > 0 ? "" : " (stock monitor choice)");

    const int mouse = read_int(path, L"ControlOptions", L"MouseMode", static_cast<int>(MouseMode::Cursor));
    g_mouse = mouse == static_cast<int>(MouseMode::Stock) ? MouseMode::Stock : MouseMode::Cursor;
    LOG("config: [ControlOptions] MouseMode=%d -> %s", mouse,
        g_mouse.load() == MouseMode::Cursor ? "match the Windows cursor" : "stock");

    g_tacview = read_int(path, L"OtherOptions", L"TacviewRecording", 0) == 1 ? 1 : 0;
    return TRUE;
}

void ensure_loaded() { InitOnceExecuteOnce(&g_once, &load, nullptr, nullptr); }

}  // namespace

DisplayMode display_mode() {
    ensure_loaded();
    return g_display.load();
}

void set_display_mode(DisplayMode mode) {
    ensure_loaded();  // so a later first load can't overwrite it
    g_display = mode;
}

int display_monitor() {
    ensure_loaded();
    return g_monitor.load();
}

void set_display_monitor(int number) {
    ensure_loaded();
    g_monitor = number > 0 ? number : 0;
}

bool tacview_recording() {
    ensure_loaded();
    return g_tacview.load() == 1;
}

void set_tacview_recording(bool on) {
    ensure_loaded();
    if (g_tacview.exchange(on ? 1 : 0) != (on ? 1 : 0)) LOG("config: Tacview Recording -> %s", on ? "on" : "off");
}

int tacview_recording_peek() { return g_tacview.load(); }

std::wstring game_data_dir() { return game_data_path(); }

MouseMode mouse_mode() {
    ensure_loaded();
    return g_mouse.load();
}

void set_mouse_mode(MouseMode mode) {
    ensure_loaded();
    g_mouse = mode;
}

}  // namespace lazysf2::config
