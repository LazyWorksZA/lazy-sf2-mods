#include "config.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <atomic>
#include <iterator>
#include <string>

#include "config_parse.h"
#include "log.h"

namespace LazyWorks::SF2Patch::config {
namespace {

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
std::atomic<DisplayMode> g_display{DisplayMode::Fullscreen};
std::atomic<int> g_monitor{0};
std::atomic<MouseMode> g_mouse{MouseMode::Cursor};
std::atomic<int> g_tacview{-1};  // -1 = not loaded yet
std::atomic<int> g_pilot_twist{-1};  // -1 = not loaded yet
std::atomic<double> g_twist_angle{kPilotTwistAngleDefault};
std::atomic<double> g_twist_lean{kPilotTwistLeanDefault};

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

// A numeric key, parsed and clamped (see parse_number_setting), logged once with the value used.
double read_number(const std::wstring& path, const wchar_t* section, const wchar_t* key, double def, double lo, double hi,
                   const char* unit) {
    wchar_t text[64] = {};
    const bool present = !path.empty() &&
                         GetPrivateProfileStringW(section, key, L"", text, static_cast<DWORD>(std::size(text)), path.c_str()) > 0;
    const NumberSetting s = parse_number_setting(present ? text : nullptr, def, lo, hi);
    if (!present)
        LOG("config: [%ls] %ls not set -> %.1f %s (default)", section, key, s.value, unit);
    else if (s.defaulted)
        LOG("config: [%ls] %ls=%ls is not a number -> %.1f %s (default)", section, key, text, s.value, unit);
    else
        LOG("config: [%ls] %ls=%ls -> %.1f %s%s", section, key, text, s.value, unit, s.clamped ? " (clamped)" : "");
    return s.value;
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

    const int twist = read_int(path, L"ControlOptions", L"PilotTwist", 0);
    g_pilot_twist = twist == 1 ? 1 : 0;
    LOG("config: [ControlOptions] PilotTwist=%d -> %s", twist, twist == 1 ? "on" : "off (stock)");
    g_twist_angle = read_number(path, L"ControlOptions", L"PilotTwistAngle", kPilotTwistAngleDefault, 0.0,
                                kPilotTwistAngleMax, "degrees");
    g_twist_lean = read_number(path, L"ControlOptions", L"PilotTwistLean", kPilotTwistLeanDefault, 0.0,
                               kPilotTwistLeanMax, "cm");
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

void set_tacview_recording(bool on) {
    ensure_loaded();
    if (g_tacview.exchange(on ? 1 : 0) != (on ? 1 : 0)) LOG("config: Tacview Recording -> %s", on ? "on" : "off");
}

int tacview_recording_peek() { return g_tacview.load(); }

int pilot_twist_peek() { return g_pilot_twist.load(); }

double pilot_twist_angle_peek() { return g_twist_angle.load(); }

double pilot_twist_lean_peek() { return g_twist_lean.load(); }

std::wstring game_data_dir() { return game_data_path(); }

MouseMode mouse_mode() {
    ensure_loaded();
    return g_mouse.load();
}

void set_mouse_mode(MouseMode mode) {
    ensure_loaded();
    g_mouse = mode;
}

bool pilot_twist() {
    ensure_loaded();
    return g_pilot_twist.load() == 1;
}

void set_pilot_twist(bool on) {
    ensure_loaded();
    if (g_pilot_twist.exchange(on ? 1 : 0) != (on ? 1 : 0)) LOG("config: Pilot Twist -> %s", on ? "on" : "off");
}

}  // namespace LazyWorks::SF2Patch::config
