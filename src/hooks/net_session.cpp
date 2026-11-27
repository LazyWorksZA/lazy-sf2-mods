// Network sessions started and stopped at run time by the lobby (menus only): the host or client, the shared world,
// and the trackers they need. LAZYSF2_HOST/CONNECT and the LAZYSF2_WORLD_* variables still configure a launch and win
// over the lobby. World files go to %LOCALAPPDATA%\LazySF2Patch\Session (the probe folder while the probe is on) and are
// deleted when the session closes.
#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <string>

#include "hooks/hooks.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

std::atomic<NetRole> g_role{NetRole::None};

// A real folder (not a file, not a junction or link): files are only written and deleted in those.
bool plain_dir(const std::wstring& dir) {
    const DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

// %LOCALAPPDATA%\LazySF2Patch\Session, created; empty if that fails or either folder is a reparse point.
std::wstring session_dir() {
    PWSTR local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
        dir = std::wstring(local) + L"\\LazySF2Patch";
        CreateDirectoryW(dir.c_str(), nullptr);
        if (plain_dir(dir)) {
            dir += L"\\Session";
            CreateDirectoryW(dir.c_str(), nullptr);
        }
        if (!plain_dir(dir) || dir.size() < 8 || dir.compare(dir.size() - 8, 8, L"\\Session") != 0) {
            LOG("session: %ls is not a plain folder; no session files", dir.c_str());
            dir.clear();
        }
    }
    CoTaskMemFree(local);
    return dir;
}

// Deletes the session's world files (world_NNN.*) in dir; nothing else.
void clear_world_files(const std::wstring& dir) {
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\world_*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (DeleteFileW((dir + L"\\" + fd.cFileName).c_str())) ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (n) LOG("session: deleted %d world file(s) in %ls", n, dir.c_str());
}

std::wstring g_dir;  // menu thread

// The world files' folder for this session: none while the probe is on (the probe folder is used and kept).
bool prepare_dir(std::string* why) {
    g_dir.clear();
    if (probe_output_dir()) return true;
    g_dir = session_dir();
    if (g_dir.empty()) {
        *why = "no folder for the session files";
        return false;
    }
    clear_world_files(g_dir);
    return true;
}

// The trackers (aircraft, weapons) follow the session: installed at its start, removed at its end unless the probe
// or Tacview Recording still needs them. Menus only.
void apply_trackers() {
    aircraft_track_reevaluate();
    if (g_role.load() != NetRole::None) handover_on_modules_loaded();  // registers its step callback once
}

}  // namespace

NetRole net_session_role() { return g_role.load(); }
bool net_session_wrong_password() { return g_role.load() == NetRole::Client && world_fetch_wrong_password(); }
bool net_session_host_unknown() { return g_role.load() == NetRole::Client && world_fetch_host_unknown(); }
bool net_session_wanted() { return g_role.load() != NetRole::None; }

// The variables configured host or client mode for this launch: the lobby stays out of it.
static bool env_session(std::string* why) {
    char host[64];
    uint16_t port = 0;
    if (host_configured() || client_connect_target(host, sizeof(host), &port)) {
        *why = "LAZYSF2_HOST or LAZYSF2_CONNECT is set for this launch";
        return true;
    }
    return false;
}

bool net_session_host(uint16_t port, const char* callsign, const std::string& password, std::string* why) {
    if (g_role.load() != NetRole::None) {
        *why = "a session is already open";
        return false;
    }
    if (env_session(why)) return false;
    if (!prepare_dir(why)) return false;
    const char* reason = "";
    if (!host_session_start("0.0.0.0", port, callsign, password, &reason)) {
        *why = reason;
        return false;
    }
    if (!world_session_begin(true, g_dir)) {
        host_session_stop();
        *why = "the shared world is already configured for this launch";
        return false;
    }
    g_role = NetRole::Host;
    apply_trackers();
    LOG("session: hosting on port %u", static_cast<unsigned>(port));
    return true;
}

bool net_session_join(const char* host, uint16_t port, const char* callsign, const std::string& password,
                      std::string* why) {
    if (g_role.load() != NetRole::None) {
        *why = "a session is already open";
        return false;
    }
    if (env_session(why)) return false;
    if (!prepare_dir(why)) return false;
    const char* reason = "";
    if (!client_session_start(host, port, callsign, password, &reason)) {
        *why = reason;
        return false;
    }
    if (!world_session_begin(false, g_dir)) {  // after the client: the mission fetch asks it for the host's address
        client_session_stop();
        *why = "the shared world is already configured for this launch";
        return false;
    }
    g_role = NetRole::Client;
    apply_trackers();
    LOG("session: joining %s:%u", host, static_cast<unsigned>(port));
    return true;
}

void net_session_close() {
    const NetRole role = g_role.exchange(NetRole::None);
    if (role == NetRole::None) return;
    if (role == NetRole::Host) host_session_stop();
    else client_session_stop();
    world_session_end();
    clear_world_files(g_dir);
    g_dir.clear();
    apply_trackers();
    LOG("session: closed (%s)", role == NetRole::Host ? "host" : "client");
}

}  // namespace LazyWorks::SF2Patch::hooks
