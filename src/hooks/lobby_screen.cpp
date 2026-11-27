// Multiplayer lobby screens (connect, host, join): this DLL is their menu code (MenuCodeDLL in the spliced
// MenuSystem.ini), so the menu system calls InitMenuCode/ExitMenuCode here and drives our handler like a stock
// screen's. The last address is kept in Options.ini [OtherOptions] LobbyAddress, through the game's own Options.ini
// object. See lazy-sf2-recon: recon-notes §28.
#include <windows.h>

#include <cstring>
#include <cwchar>
#include <string>

#include "config.h"
#include "hooks/hook_util.h"
#include "hooks/hooks.h"
#include "hooks/lobby_logic.h"
#include "hooks/lobby_splice.h"
#include "log.h"

namespace LazyWorks::SF2Patch::hooks {
namespace {

constexpr DWORD kExeTimestamp = 0x51d22110, kExeImageSize = 0x62000;
constexpr DWORD kOptObjectRva = 0x3ce28;     // the OPT service object (shared Options.ini)
constexpr DWORD kOptVtableRva = 0x31c1c;
constexpr DWORD kIniGetStringRva = 0x11d20;  // ini GetString(section, key, default, out, size, mode)
constexpr DWORD kIniSetStringRva = 0x117d0;  // ini SetString(section, key, value)
constexpr int kOptAcquire = 0x08 / 4;        // bool Acquire(ini** out)
constexpr int kOptRelease = 0x0c / 4;        // void Release(ini** p)
constexpr int kOptReleaseSave = 0x10 / 4;    // int Release(ini** p), writing Options.ini first

constexpr DWORD kMnuTimestamp = 0x51d21fc1, kMnuImageSize = 0x41000;  // Core\MenuSystem.dll
constexpr DWORD kMnuVtableRva = 0x30274;
constexpr int kSysGetService = 0x08 / 4;      // GetService(tag, void** out)
constexpr int kSysReleaseService = 0x0c / 4;  // ReleaseService(tag, void** p)
constexpr int kMnuItemUnderPointer = 0x10 / 4;
constexpr int kMnuPostCommand = 0x24 / 4;  // PostCommand(cmd, a, b)
constexpr int kMnuGetItem = 0x50 / 4;      // int GetItem(item, type, void** iface)
constexpr int kIfaceSelection = 2, kIfaceText = 4, kIfaceEdit = 7;
constexpr int kTextSet = 0x0c / 4;  // TEXT SetText(text)
constexpr int kEditSet = 0x18 / 4;  // EDIT SetText(text)
constexpr int kEditGet = 0x1c / 4;  // EDIT GetText(buf, n)
constexpr int kCmdGoTo = 3;         // go to the item's DestinationMenu
constexpr int kCmdLeave = 0;        // leave the menus: (0,1) flies the selected mission
constexpr DWORD kMmgStamp = 0x51d21faa, kMmgSize = 0x25000;  // Core\MissionManager.dll
constexpr DWORD kMmgVtableRva = 0x1b2c4;
constexpr int kMmgSelectMission = 0x14 / 4;  // SelectMission(name): loads SingleMission, Campaign or InstantAction
constexpr int kHangarFly = 9;               // the Hangar's Fly button (HANGARSCREEN.INI MenuItem009)
constexpr DWORD kTerStamp = 0x51d21fa0, kTerSize = 0x46000;  // Core\TerrainEngine.dll
constexpr DWORD kTerVtableRva = 0x3543c;
constexpr int kTerSelect = 0x24 / 4;  // bool SelectTerrain(name): the theatre the next flight loads (the New page's choice)
constexpr int kCmdArg = 10;         // third PostCommand argument, as the stock screens pass it

constexpr wchar_t kAddressSection[] = L"OtherOptions";
constexpr wchar_t kAddressKey[] = L"LobbyAddress";

using Acquire_t = int(__fastcall*)(void* opt, void* edx, void** out);
using Release_t = void(__fastcall*)(void* opt, void* edx, void** p);
using ReleaseSave_t = int(__fastcall*)(void* opt, void* edx, void** p);
using IniGetString_t = void(__fastcall*)(void* ini, void* edx, const wchar_t* section, const wchar_t* key,
                                         const wchar_t* def, wchar_t* out, size_t n, int mode);
using IniSetString_t = int(__fastcall*)(void* ini, void* edx, const wchar_t* section, const wchar_t* key,
                                        const wchar_t* value);
using Service_t = int(__fastcall*)(void* sys, void* edx, const wchar_t* tag, void** p);
using ItemUnderPointer_t = int(__fastcall*)(void* mnu, void* edx);
using PostCommand_t = void(__fastcall*)(void* mnu, void* edx, int cmd, int a, int b);
using GetItem_t = int(__fastcall*)(void* mnu, void* edx, int item, int type, void** iface);
using SetText_t = void(__fastcall*)(void* iface, void* edx, const wchar_t* text);
using GetText_t = void(__fastcall*)(void* iface, void* edx, wchar_t* buf, size_t n);

// Menu thread only: the menu system calls InitMenuCode, the handler and ExitMenuCode on its own thread.
void* g_sys = nullptr;
void* g_mnu = nullptr;
std::wstring g_saved_address;  // what Options.ini holds (as last read or written)
std::wstring g_join_address;   // the address Join was clicked with
bool g_ready = false;          // join screen: this pilot's Ready (sent with every status)
uint32_t g_ready_mission = 0;  // ... and the host's mission it was pressed for
std::wstring g_pending_status; // a status line for the next session screen (set where no lobby screen is open)
int g_wished = -1;             // join screen: the seat asked for, until the roster answers
uint32_t g_launches = 0;       // host: Launches in this session
uint32_t g_flown = 0;          // client: the Launch it flew (or was told about), so each is taken once
uint32_t g_told = 0;           // client: the Launch it said "not ready" about
enum class Screen { Connect, Host, Join };
Screen g_screen = Screen::Connect;

/// <summary>The OPT service object if the exe is the expected build and the object is as analysed, else nullptr.</summary>
void* opt_object() {
    static int state = 0;  // 0 = unchecked, 1 = ok, -1 = not usable
    HMODULE exe = GetModuleHandleW(nullptr);
    if (state == 0) {
        state = is_expected_build(exe, kExeTimestamp, kExeImageSize, "StrikeFighters2.exe") ? 1 : -1;
        if (state == 1 && *static_cast<void**>(at_rva(exe, kOptObjectRva)) != at_rva(exe, kOptVtableRva)) {
            LOG("lobby: Options.ini service not as expected; the address is not saved");
            state = -1;
        }
    }
    return state == 1 ? at_rva(exe, kOptObjectRva) : nullptr;
}

/// <summary>Reads a string from the game's Options.ini object; false if it is not available.</summary>
bool read_option(const wchar_t* section, const wchar_t* key, wchar_t* out, size_t n) {
    out[0] = L'\0';
    void* opt = opt_object();
    if (!opt) return false;
    void* ini = nullptr;
    if (!reinterpret_cast<Acquire_t>(vtable_slot(opt, kOptAcquire))(opt, nullptr, &ini) || !ini) return false;
    HMODULE exe = GetModuleHandleW(nullptr);
    reinterpret_cast<IniGetString_t>(at_rva(exe, kIniGetStringRva))(ini, nullptr, section, key, L"", out, n, 0);
    reinterpret_cast<Release_t>(vtable_slot(opt, kOptRelease))(opt, nullptr, &ini);
    return true;
}

/// <summary>Writes a string to the game's Options.ini object, and the game writes the file.</summary>
bool write_option(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    void* opt = opt_object();
    if (!opt) return false;
    void* ini = nullptr;
    if (!reinterpret_cast<Acquire_t>(vtable_slot(opt, kOptAcquire))(opt, nullptr, &ini) || !ini) return false;
    HMODULE exe = GetModuleHandleW(nullptr);
    const int set = reinterpret_cast<IniSetString_t>(at_rva(exe, kIniSetStringRva))(ini, nullptr, section, key, value);
    reinterpret_cast<ReleaseSave_t>(vtable_slot(opt, kOptReleaseSave))(opt, nullptr, &ini);
    return set != 0;
}

void* item_iface(int item, int type) {
    void* iface = nullptr;
    if (!g_mnu) return nullptr;
    reinterpret_cast<GetItem_t>(vtable_slot(g_mnu, kMnuGetItem))(g_mnu, nullptr, item, type, &iface);
    return iface;
}

/// <summary>Sets a TEXT item's text. Never empty: an empty text drops the item's text node, and the next one would be
/// created hidden (see kLobbyBlankKey).</summary>
void set_text(int item, const wchar_t* text) {
    if (!text || !*text) text = L" ";
    if (void* t = item_iface(item, kIfaceText)) reinterpret_cast<SetText_t>(vtable_slot(t, kTextSet))(t, nullptr, text);
}

void set_status(const wchar_t* text) {
    set_text(g_screen == Screen::Connect ? lobby_item::kStatus : session_item::kStatus, text);
    LOG("lobby: status \"%ls\"", text);
}

std::wstring read_address() {
    wchar_t buf[kLobbyAddressChars + 1] = {};
    if (void* e = item_iface(lobby_item::kAddress, kIfaceEdit)) {
        reinterpret_cast<GetText_t>(vtable_slot(e, kEditGet))(e, nullptr, buf, _countof(buf));
    }
    buf[kLobbyAddressChars] = L'\0';
    std::wstring s = buf;
    const size_t first = s.find_first_not_of(L" \t");
    const size_t last = s.find_last_not_of(L" \t");
    return first == std::wstring::npos ? std::wstring() : s.substr(first, last - first + 1);
}

/// <summary>Reads the Address field, logs it, and saves it to Options.ini when it changed.</summary>
std::wstring take_address() {
    std::wstring a = read_address();
    LOG("lobby: address field reads \"%ls\"", a.c_str());
    if (a != g_saved_address) {
        if (write_option(kAddressSection, kAddressKey, a.c_str())) {
            g_saved_address = a;
            LOG("lobby: saved [%ls] %ls=%ls", kAddressSection, kAddressKey, a.c_str());
        } else {
            LOG("lobby: could not save the address");
        }
    }
    return a;
}

bool parse_int(const wchar_t* s, int* out) {
    wchar_t* end = nullptr;
    const long v = std::wcstol(s, &end, 10);
    if (!s[0] || !end || *end) return false;
    *out = static_cast<int>(v);
    return true;
}

/// <summary>Selects the Single Mission mission DLL through MMG, as the main menu's Single Mission button does.</summary>
bool select_single_mission() {
    HMODULE mm = GetModuleHandleW(L"MissionManager.dll");
    if (!g_sys || !mm || !is_expected_build(mm, kMmgStamp, kMmgSize, "MissionManager.dll")) return false;
    void* mmg = nullptr;
    reinterpret_cast<Service_t>(vtable_slot(g_sys, kSysGetService))(g_sys, nullptr, L"MMG", &mmg);
    if (!mmg) return false;
    const bool ok = *static_cast<void**>(mmg) == at_rva(mm, kMmgVtableRva);
    if (ok) {
        using Select_t = int(__fastcall*)(void*, void*, const wchar_t*);
        reinterpret_cast<Select_t>(vtable_slot(mmg, kMmgSelectMission))(mmg, nullptr, L"SingleMission");
    }
    reinterpret_cast<Service_t>(vtable_slot(g_sys, kSysReleaseService))(g_sys, nullptr, L"MMG", &mmg);
    return ok;
}

/// <summary>Makes theatre the terrain of the next flight, as the Single Mission New page's theatre list does.</summary>
bool select_terrain(const std::string& theatre) {
    HMODULE te = GetModuleHandleW(L"TerrainEngine.dll");
    if (!g_sys || !te || !is_expected_build(te, kTerStamp, kTerSize, "TerrainEngine.dll")) return false;
    void* ter = nullptr;
    reinterpret_cast<Service_t>(vtable_slot(g_sys, kSysGetService))(g_sys, nullptr, L"TER", &ter);
    if (!ter) return false;
    bool ok = *static_cast<void**>(ter) == at_rva(te, kTerVtableRva);
    if (ok) {
        const std::wstring w = widen_ansi(theatre);
        using Select_t = bool(__fastcall*)(void*, void*, const wchar_t*);
        ok = reinterpret_cast<Select_t>(vtable_slot(ter, kTerSelect))(ter, nullptr, w.c_str());
    }
    reinterpret_cast<Service_t>(vtable_slot(g_sys, kSysReleaseService))(g_sys, nullptr, L"TER", &ter);
    return ok;
}

/// <summary>Join screen, Briefing and Launch: the host's latest mission into the stock Single Mission (its theatre
/// selected first, as the New page would). False with a status line when it cannot.</summary>
bool load_host_mission() {
    std::string theatre;
    const int r = world_lobby_fetched(&theatre);
    if (r != 0) {
        LOG("lobby: Briefing: %s", r == 1 ? "no mission from the host yet" : r == 2 ? "theatre not installed" : "unusable mission");
        set_status(r == 1 ? L"The host has not shared a mission yet."
                   : r == 2 ? (L"This mission's theatre (" + widen_ansi(theatre) + L") is not installed.").c_str()
                            : L"The host's mission cannot be used.");
        return false;
    }
    if (!select_single_mission() || !select_terrain(theatre)) {
        LOG("lobby: Briefing: could not select Single Mission or theatre %s", theatre.c_str());
        set_status(L"The mission's theatre could not be selected.");
        return false;
    }
    if (!world_lobby_load()) {
        set_status(L"The host's mission could not be loaded.");
        return false;
    }
    LOG("lobby: the host's mission is loaded (theatre %s)", theatre.c_str());
    return true;
}

void post(int cmd, int a) {
    reinterpret_cast<PostCommand_t>(vtable_slot(g_mnu, kMnuPostCommand))(g_mnu, nullptr, cmd, a, kCmdArg);
}

/// <summary>The stock screens' click test: a left-button release over a button, or Esc (reported as item 1).</summary>
int clicked_item(const int* ev) {
    int item = 0;
    if (ev[0] == 3) {
        if ((ev[3] & 0xf) == 0 && ev[2] == 0) {
            item = reinterpret_cast<ItemUnderPointer_t>(vtable_slot(g_mnu, kMnuItemUnderPointer))(g_mnu, nullptr);
        }
    } else if (ev[0] == 1 && (ev[3] & 0xf) != 0 && ev[2] == 1) {
        item = lobby_item::kBack;
    }
    return item > 0 && item_iface(item, kIfaceSelection) ? item : 0;
}

/// <summary>The active pilot's callsign for the wire (printable ASCII, 15 characters).</summary>
std::string wire_callsign() {
    wchar_t callsign[64] = {};
    read_option(L"Pilot", L"Callsign", callsign, _countof(callsign));
    return lobby_callsign(callsign, "PLAYER");
}

/// <summary>The Session password field as UTF-8 (empty if none). Never logged, never stored: the session keeps what it
/// needs (the host a derived key, a client the password until it leaves), and every copy here is wiped after use.</summary>
std::string take_password() {
    wchar_t buf[kLobbyPasswordChars + 1] = {};
    if (void* e = item_iface(lobby_item::kPassword, kIfaceEdit)) {
        reinterpret_cast<GetText_t>(vtable_slot(e, kEditGet))(e, nullptr, buf, _countof(buf));
    }
    buf[kLobbyPasswordChars] = L'\0';
    char utf8[4 * kLobbyPasswordChars + 1] = {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    std::string s = n > 1 ? std::string(utf8, static_cast<size_t>(n - 1)) : std::string();
    net::auth_wipe(buf, sizeof(buf));
    net::auth_wipe(utf8, sizeof(utf8));
    return s;
}

/// <summary>Clears the Session password field (after a session started with it).</summary>
void clear_password_field() {
    if (void* e = item_iface(lobby_item::kPassword, kIfaceEdit)) {
        reinterpret_cast<SetText_t>(vtable_slot(e, kEditSet))(e, nullptr, L"");
    }
}

void start_host() {
    take_address();
    std::string password = take_password();
    if (password.empty()) {
        LOG("lobby: Host refused: no session password");
        set_status(L"Type a session password first: players join with it.");
        return;
    }
    const std::string cs = wire_callsign();
    std::string why;
    const bool ok = net_session_host(kLobbyPort, cs.c_str(), password, &why);
    net::auth_wipe(password);
    if (ok) clear_password_field();
    if (!ok) {
        LOG("lobby: Host refused: %s", why.c_str());
        set_status((L"Could not host: " + widen_ansi(why) + L".").c_str());
        return;
    }
    LOG("lobby: hosting as \"%s\" on port %u", cs.c_str(), static_cast<unsigned>(kLobbyPort));
    g_launches = 0;
    post(kCmdGoTo, lobby_item::kHost);
}

void start_join() {
    const std::wstring a = take_address();
    char host[64] = {};
    uint16_t port = 0;
    if (a.empty()) {
        LOG("lobby: Join clicked with no address");
        set_status(L"Type the host's address first.");
        return;
    }
    if (!parse_lobby_address(a, host, sizeof(host), &port)) {
        LOG("lobby: Join refused: \"%ls\" is not an address", a.c_str());
        set_status(L"That is not an address: use a name or IP, with :port if not 7777.");
        return;
    }
    std::string password = take_password();
    if (password.empty()) {
        LOG("lobby: Join refused: no session password");
        set_status(L"Type the host's session password first.");
        return;
    }
    const std::string cs = wire_callsign();
    std::string why;
    const bool ok = net_session_join(host, port, cs.c_str(), password, &why);
    net::auth_wipe(password);
    if (ok) clear_password_field();
    if (!ok) {
        LOG("lobby: Join refused: %s", why.c_str());
        set_status((L"Could not join: " + widen_ansi(why) + L".").c_str());
        return;
    }
    g_join_address = widen_ansi(host) + L":" + std::to_wstring(port);
    g_ready = false;
    g_ready_mission = 0;
    g_wished = -1;
    g_flown = g_told = 0;
    LOG("lobby: joining %s:%u as \"%s\"", host, static_cast<unsigned>(port), cs.c_str());
    post(kCmdGoTo, lobby_item::kJoin);
}

void connect_click(int item) {
    switch (item) {
        case lobby_item::kBack:
            take_address();
            LOG("lobby: Back");
            post(kCmdGoTo, item);
            break;
        case lobby_item::kHost:
            start_host();
            break;
        case lobby_item::kJoin:
            start_join();
            break;
        default:
            break;
    }
}

// --- The host and join screens (L4): roster, seats, Ready. ---

std::wstring widen(const char* s) { return widen_ansi(s); }

std::wstring seat_text(int seat) { return seat < 0 || seat == net::kNoSeat ? L"-" : L"#" + std::to_wstring(seat + 1); }

// What the screens show, from the host's own view or the client's latest roster.
struct Roster {
    bool have = false;
    int members = 0;
    int clients = 0;
    int you = -1;  // this pilot's seat
    uint8_t result = net::kLobbyOk;
    uint32_t launch_seq = 0;
    uint32_t mission_id = 0;
    uint32_t rtt_ms = 0;
    std::wstring host_callsign;
    std::wstring summary[3];
    size_t count = 0;
    net::LobbyRow rows[net::kLobbyRows] = {};
};

Roster read_roster(bool host) {
    Roster r;
    if (host) {
        net::HostCore::LobbyView v;
        if (!host_lobby_view(&v)) return r;
        r.have = true;
        r.members = v.members;
        r.clients = v.clients;
        r.you = v.host_seat;
        r.launch_seq = v.launch_seq;
        r.mission_id = v.mission_id;
        r.count = v.count;
        std::memcpy(r.rows, v.rows, sizeof(r.rows));
        for (int i = 0; i < 3; ++i) r.summary[i] = widen(v.summary[i]);
    } else {
        net::LobbyRosterMsg m;
        if (!world_lobby_roster(&m, &r.rtt_ms)) return r;
        r.have = true;
        r.members = m.members;
        r.you = m.your_seat == net::kNoSeat ? -1 : m.your_seat;
        r.result = m.result;
        r.launch_seq = m.launch_seq;
        r.mission_id = m.mission_id;
        r.count = m.count;
        std::memcpy(r.rows, m.rows, sizeof(r.rows));
        for (int i = 0; i < 3; ++i) r.summary[i] = widen(m.summary[i]);
        for (size_t i = 0; i < r.count; ++i) {
            if (r.rows[i].flags & net::kLobbyRowHost) r.host_callsign = widen(r.rows[i].callsign);
            if (r.rows[i].callsign[0] && !(r.rows[i].flags & net::kLobbyRowHost)) ++r.clients;
        }
    }
    return r;
}

bool g_refusal_shown = false;
ULONGLONG g_next_roster = 0;  // the roster is redrawn twice a second

void show_roster(const Roster& r) {
    const bool host = g_screen == Screen::Host;
    for (int i = 0; i < net::kLobbyRows; ++i) {
        std::wstring seat, cs, st;
        if (static_cast<size_t>(i) < r.count) {
            const net::LobbyRow& row = r.rows[i];
            seat = seat_text(row.seat);
            if (row.callsign[0]) {
                cs = widen(row.callsign);
                if (row.flags & net::kLobbyRowYou) cs += L" (you)";
                st = (row.flags & net::kLobbyRowHost) ? L"Host" : (row.flags & net::kLobbyRowReady) ? L"Ready" : L"Not ready";
            } else {
                cs = L"AI";
            }
        }
        set_text(session_item::row_seat(i), seat.c_str());
        set_text(session_item::row_callsign(i), cs.c_str());
        set_text(session_item::row_status(i), st.c_str());
    }
    const std::wstring players = std::to_wstring(r.clients) + (r.clients == 1 ? L" player joined" : L" players joined");
    if (host) {
        uint16_t port = 0;
        bool failed = false;
        host_session_port(&port, &failed);
        set_text(session_item::kHeader, (L"Hosting on port " + std::to_wstring(port) + L" - " + players).c_str());
        for (int i = 0; i < 3; ++i) set_text(session_item::kSummary + i, r.summary[i].c_str());
        if (r.summary[0].empty()) set_text(session_item::kSummary, L"No mission yet: Set up mission...");
    } else {
        std::wstring h = L"Host " + (r.host_callsign.empty() ? std::wstring(L"?") : r.host_callsign) + L" - " +
                         g_join_address + L" - ping " + std::to_wstring(r.rtt_ms) + L" ms";
        set_text(session_item::kHeader, h.c_str());
        for (int i = 0; i < 3; ++i) set_text(session_item::kSummary + i, r.summary[i].c_str());
        if (r.summary[0].empty()) set_text(session_item::kSummary, L"The host has not set up a mission yet.");
    }
    if (r.members == 0) {
        set_text(session_item::kYouFly, L"Seats appear once the host has set up the mission.");
    } else if (r.you < 0) {
        set_text(session_item::kYouFly, L"No seat yet: pick a free one with < Seat or Seat >.");
    } else {
        std::wstring y = L"You fly seat " + seat_text(r.you) + L" of the player flight";
        if (!host) y += L" with the host's loadout";
        set_text(session_item::kYouFly, (y + L".").c_str());
    }
}

void session_frame() {
    if (g_screen == Screen::Host) {
        uint16_t port = 0;
        bool failed = false;
        if (!host_session_port(&port, &failed) && failed) {
            LOG("lobby: the host could not open its port; closing the session");
            net_session_close();
            set_text(session_item::kHeader, L"Not hosting");
            set_status(L"Could not open UDP port 7777: is another game or program using it?");
            return;
        }
    } else if (!g_refusal_shown) {
        if (net_session_wrong_password()) {
            g_refusal_shown = true;
            LOG("lobby: the host refused the session password");
            set_status(L"Wrong session password. Leave, and type the host's password.");
        } else if (net_session_host_unknown()) {
            g_refusal_shown = true;
            LOG("lobby: the host's name did not resolve");
            set_status(L"Couldn't find the host. Leave, and check the address.");
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_roster || net_session_role() == NetRole::None) return;
    g_next_roster = now + 500;
    const Roster r = read_roster(g_screen == Screen::Host);
    if (!r.have) return;
    show_roster(r);
    if (g_screen == Screen::Join) {
        if (g_ready && r.mission_id != g_ready_mission) {  // Ready was for a mission the host has replaced
            g_ready = false;
            g_ready_mission = 0;
            world_lobby_ready(false, 0);
            LOG("lobby: the host changed the mission; Ready off");
            set_status(L"The host changed the mission: check the briefing, then press Ready again.");
        }
        // The host's Launch: a ready pilot loads the latest mission and flies; one that is not ready is told, and flies
        // as soon as it is (late join) while the host's flight runs.
        const uint32_t launch = r.launch_seq & ~net::kLaunchFlying;
        if ((r.launch_seq & net::kLaunchFlying) && launch != g_flown) {
            if (g_ready && r.you >= 0 && g_ready_mission == r.mission_id) {
                g_flown = launch;
                LOG("lobby: the host launched (Launch %u); joining the flight", launch);
                if (load_host_mission()) {
                    g_ready = false;
                    g_ready_mission = 0;
                    world_lobby_ready(false, 0);
                    set_status(L"Launching...");
                    post(kCmdLeave, 1);
                    return;
                }
            } else if (g_told != launch) {
                g_told = launch;
                set_status(L"The host has launched: press Ready to join the flight.");
            }
        }
        if (r.result == net::kLobbyContent && !g_refusal_shown) {
            g_refusal_shown = true;
            LOG("lobby: the host's game content differs from ours; not in its lobby");
            set_status(L"Your game content differs from the host's (aircraft, weapons, terrains or patches).");
        } else {
            int seat = -1;
            uint8_t res = 0;
            if (world_lobby_wish_answer(&seat, &res) && seat == g_wished) {
                g_wished = -1;
                set_status(res == net::kLobbyOk
                               ? (L"You took seat " + seat_text(seat) + L".").c_str()
                               : (L"Seat " + seat_text(seat) + L" was taken; you keep " + seat_text(r.you) + L".").c_str());
            }
        }
    }
}

/// <summary>Client: Ready for the host's current mission, if this pilot has a seat in it. Returns nullptr, or why
/// not (a status line).</summary>
const wchar_t* try_ready() {
    const Roster r = read_roster(false);
    if (!r.have || r.result == net::kLobbyContent) return L"Not in the host's lobby.";
    if (r.mission_id == 0) return L"The host has not set up a mission yet.";
    if (r.you < 0) return L"Pick a seat first.";
    g_ready = true;
    g_ready_mission = r.mission_id;
    world_lobby_ready(true, r.mission_id);
    LOG("lobby: Ready on (mission 0x%08lx, seat %d)", static_cast<unsigned long>(r.mission_id), r.you);
    return nullptr;
}

/// <summary>The next free seat after (dir 1) or before (dir -1) this pilot's, from the roster's free rows; -1 if none.</summary>
int next_free_seat(const Roster& r, int dir) {
    if (r.members <= 0) return -1;
    const int from = r.you < 0 ? (dir > 0 ? -1 : r.members) : r.you;
    for (int step = 1; step <= r.members; ++step) {
        const int s = ((from + dir * step) % r.members + r.members) % r.members;
        for (size_t i = 0; i < r.count; ++i)
            if (!r.rows[i].callsign[0] && r.rows[i].seat == s) return s;
    }
    return -1;
}

void change_seat(int dir) {
    const Roster r = read_roster(g_screen == Screen::Host);
    const int seat = next_free_seat(r, dir);
    if (seat < 0) {
        set_status(r.members == 0 ? L"No seats yet: the host has not set up the mission." : L"No other seat is free.");
        return;
    }
    LOG("lobby: seat %d asked for", seat);
    if (g_screen == Screen::Host) {
        const uint8_t res = host_lobby_seat(seat);
        set_status(res == net::kLobbyOk ? (L"You took seat " + seat_text(seat) + L".").c_str()
                                        : (L"Seat " + seat_text(seat) + L" is not free.").c_str());
    } else {
        g_wished = seat;
        g_ready = false;  // a new seat clears Ready (the host clears it too)
        g_ready_mission = 0;
        world_lobby_ready(false, 0);
        world_lobby_want_seat(seat);
        set_status((L"Asking the host for seat " + seat_text(seat) + L"...").c_str());
    }
    g_next_roster = 0;
}

void session_click(int item) {
    const bool host = g_screen == Screen::Host;
    switch (item) {
        case session_item::kLeave:
            LOG("lobby: %s", host ? "Close session" : "Leave");
            net_session_close();
            post(kCmdGoTo, item);
            break;
        case session_item::kAction:
            if (host) {
                if (!world_host_armed()) {
                    set_status(L"Set up the mission first.");
                    break;
                }
                ++g_launches;
                host_lobby_launch(g_launches | net::kLaunchFlying);
                LOG("lobby: Launch %u: the host flies; ready pilots follow, the others may join late", g_launches);
                set_status(L"Launching...");
                post(kCmdLeave, 1);
            } else {
                if (g_ready) {
                    g_ready = false;
                    g_ready_mission = 0;
                    world_lobby_ready(false, 0);
                    LOG("lobby: Ready off");
                    set_status(L"Not ready.");
                } else {
                    const wchar_t* why = try_ready();
                    set_status(why ? why : L"Ready: the host can launch the mission.");
                }
                g_next_roster = 0;
            }
            break;
        case session_item::kMissionButton:
            if (host) {
                if (!select_single_mission()) {
                    LOG("lobby: Set up mission: the mission manager is not as expected");
                    set_status(L"The Single Mission screen cannot be opened from here.");
                    break;
                }
                LOG("lobby: Set up mission: opening Single Mission");
                post(kCmdGoTo, item);  // its DestinationMenu: SingleMissionScreen
            } else if (load_host_mission()) {
                LOG("lobby: Briefing: opening the Hangar");
                post(kCmdGoTo, item);  // its DestinationMenu: HangarScreen
            }
            break;
        case session_item::kSeatPrev:
        case session_item::kSeatNext:
            change_seat(item == session_item::kSeatNext ? 1 : -1);
            break;
        default:
            break;
    }
}

/// <summary>The handler object the menu system calls (thiscall slots: 0 frame(), 1 activate(int), 2 refresh(),
/// 3 handle_event(event*); MenuSystem calls no other slot). The signatures must match: thiscall pops the arguments.</summary>
class LobbyHandler {
public:
    virtual void frame() {
        if (g_mnu && g_screen != Screen::Connect) session_frame();
    }
    // 0 when a popup or another screen opens over this one (MenuSystem +0xbde0), 1 when it is on top again (+0xbe70).
    virtual void activate(int /*on_top*/) {}
    virtual void refresh() {}
    virtual void handle_event(const int* ev) {
        if (!g_mnu || !ev) return;
        const int item = clicked_item(ev);
        if (item <= 0) return;
        if (g_screen == Screen::Connect) {
            connect_click(item);
        } else {
            session_click(item);
        }
    }
};
LobbyHandler g_handler;

/// <summary>Which lobby screen is open: only the connect screen has an EDIT field; the host and join screens share
/// their items, so the session's role tells them apart (none: the join screen's wording).</summary>
Screen identify_screen() {
    if (item_iface(lobby_item::kAddress, kIfaceEdit)) return Screen::Connect;
    return net_session_role() == NetRole::Host ? Screen::Host : Screen::Join;
}

void init_connect_screen() {
    // The connect screen is outside any session: one still open (a way out we did not see) closes here.
    if (net_session_role() != NetRole::None) {
        LOG("lobby: a session was still open on the connect screen; closing it");
        net_session_close();
    }
    wchar_t callsign[64] = {};
    read_option(L"Pilot", L"Callsign", callsign, _countof(callsign));
    set_text(lobby_item::kCallsign, callsign);
    wchar_t address[kLobbyAddressChars + 1] = {};
    const bool have_options = read_option(kAddressSection, kAddressKey, address, _countof(address));
    g_saved_address = address;
    if (void* e = item_iface(lobby_item::kAddress, kIfaceEdit)) {
        reinterpret_cast<SetText_t>(vtable_slot(e, kEditSet))(e, nullptr, address);
    }
    set_text(lobby_item::kStatus, L"Not connected.");
    LOG("lobby: connect screen opened (callsign \"%ls\", saved address \"%ls\"%s)", callsign, address,
        have_options ? "" : ", Options.ini not available");
}

void init_session_screen() {
    const bool host = g_screen == Screen::Host;
    const NetRole role = net_session_role();
    g_next_roster = 0;
    g_refusal_shown = false;
    if (role != (host ? NetRole::Host : NetRole::Client)) {
        set_text(session_item::kHeader, L"Not in a session");
        set_status(L"The session has ended. Go back to start a new one.");
    } else if (host) {
        if (g_launches) host_lobby_launch(g_launches);  // back from the flight: no longer flying, no late joins
        set_text(session_item::kHeader, L"Starting the host...");
        set_status(L"Waiting for players. Set up the mission when you are ready.");
    } else {
        set_text(session_item::kHeader, (L"Host at " + g_join_address).c_str());
        set_status(g_ready ? L"Ready." : L"Pick a seat, then Ready.");
    }
    if (!g_pending_status.empty() && role != NetRole::None) set_status(g_pending_status.c_str());
    g_pending_status.clear();
    LOG("lobby: %s screen opened", host ? "host" : "join");
}

/// <summary>Logs loose screen files our MenuSystem.ini sections override (MenuSystem.ini is looked up first).</summary>
void log_overridden_screen_files() {
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring game = (n && n < MAX_PATH) ? exe : L"";
    game = game.substr(0, game.find_last_of(L'\\') + 1);
    const std::wstring data = config::game_data_dir();
    for (const wchar_t* screen : {kLobbyScreen, kLobbyHostScreen, kLobbyJoinScreen}) {
        for (const std::wstring& dir : {game, game + L"Menu\\", data.empty() ? data : data + L"\\Menu\\"}) {
            if (dir.empty()) continue;
            const std::wstring file = dir + screen + L".ini";
            if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) {
                LOG("lobby: %ls exists; the lobby's own [%ls] is used instead", file.c_str(), screen);
            }
        }
    }
}

void pin_this_module() {
    static bool done = false;
    if (done) return;
    done = true;
    log_overridden_screen_files();  // here, not in the file-read hook: a shell call, outside the file service's lock
    HMODULE self = nullptr;
    const BOOL ok = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                       reinterpret_cast<LPCWSTR>(&pin_this_module), &self);
    LOG("lobby: mod DLL pinned as menu code: %s", ok ? "ok" : "FAILED");
}

}  // namespace

bool game_option_int(const wchar_t* section, const wchar_t* key, int* out) {
    wchar_t buf[32] = {};
    return read_option(section, key, buf, _countof(buf)) && parse_int(buf, out);
}

namespace {

int g_in_hangar = 0;  // menu thread: inside the Hangar's event handler with the lobby's way back in it
PostCommand_t g_post = nullptr;  // MNU's own PostCommand, once guard_fly has replaced its vtable slot

// The Hangar's Fly, from the lobby: back to the lobby screen instead (a client's Ready through the lobby's checks).
bool hangar_back(void* mnu, const char* how) {
    const int back = lobby_hangar_return_item();
    void* sel = nullptr;
    if (back <= 0) return false;
    reinterpret_cast<GetItem_t>(vtable_slot(mnu, kMnuGetItem))(mnu, nullptr, back, kIfaceSelection, &sel);
    if (!sel) {
        LOG("lobby: the Hangar's way back to the lobby is missing; Fly starts the flight");
        return false;
    }
    const NetRole role = net_session_role();
    if (role == NetRole::Client && !g_ready) {
        const wchar_t* why = try_ready();
        g_pending_status = why ? why : L"Ready: the host can launch the mission.";
    }
    LOG("lobby: Hangar %s (%s): back to the lobby", role == NetRole::Host ? "Done" : role == NetRole::Client ? "Ready" : "Fly after the session ended", how);
    const PostCommand_t post_cmd = g_post ? g_post : reinterpret_cast<PostCommand_t>(vtable_slot(mnu, kMnuPostCommand));
    post_cmd(mnu, nullptr, kCmdGoTo, back, kCmdArg);
    return true;
}

// The menu system's command, while the Hangar's handler runs: its fly command goes back to the lobby.
void __fastcall guarded_post(void* mnu, void* edx, int cmd, int a, int b) {
    if (g_in_hangar > 0 && cmd == kCmdLeave && a == 1 && lobby_hangar_return_item() > 0 && hangar_back(mnu, "fly command"))
        return;
    g_post(mnu, edx, cmd, a, b);
}

/// <summary>Replaces MNU's PostCommand vtable slot with guarded_post, once (MenuSystem.dll stays loaded).</summary>
void guard_fly(void* mnu) {
    static bool tried = false;
    if (tried) return;
    tried = true;
    HMODULE mnu_dll = GetModuleHandleW(L"MenuSystem.dll");
    if (!is_expected_build(mnu_dll, kMnuTimestamp, kMnuImageSize, "MenuSystem.dll")) return;
    void** slot = static_cast<void**>(at_rva(mnu_dll, kMnuVtableRva)) + kMnuPostCommand;
    DWORD old = 0;
    if (*static_cast<void**>(mnu) != at_rva(mnu_dll, kMnuVtableRva) ||
        !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        LOG("lobby: the Hangar's fly command is not guarded (menu system not as expected)");
        return;
    }
    g_post = reinterpret_cast<PostCommand_t>(*slot);
    *slot = reinterpret_cast<void*>(&guarded_post);
    VirtualProtect(slot, sizeof(void*), old, &old);
    LOG("lobby: the Hangar's fly command is guarded (MenuSystem PostCommand slot)");
}

}  // namespace

bool lobby_hangar_event(void* self, const int* ev) {
    if (!self || !ev) return false;
    void* mnu = *reinterpret_cast<void**>(static_cast<BYTE*>(self) + 4);  // the menu-code base class keeps MNU there
    HMODULE mnu_dll = GetModuleHandleW(L"MenuSystem.dll");
    if (!mnu || !mnu_dll || *static_cast<void**>(mnu) != at_rva(mnu_dll, kMnuVtableRva)) return false;
    if (ev[0] != 3 || (ev[3] & 0xf) != 0 || ev[2] != 0) return false;  // a left-button release
    const int item = reinterpret_cast<ItemUnderPointer_t>(vtable_slot(mnu, kMnuItemUnderPointer))(mnu, nullptr);
    if (item != kHangarFly) return false;
    return hangar_back(mnu, "Fly");
}

void lobby_hangar_enter(void* self) {
    void* mnu = self ? *reinterpret_cast<void**>(static_cast<BYTE*>(self) + 4) : nullptr;
    HMODULE mnu_dll = GetModuleHandleW(L"MenuSystem.dll");
    if (mnu && mnu_dll && *static_cast<void**>(mnu) == at_rva(mnu_dll, kMnuVtableRva)) guard_fly(mnu);
    ++g_in_hangar;
}

void lobby_hangar_leave() {
    if (g_in_hangar > 0) --g_in_hangar;
}

std::wstring lobby_menu_code_path() {
    static int state = 0;  // 0 = undecided, 1 = ok, -1 = not available
    static std::wstring path;
    if (state != 0) return path;
    HMODULE mnu = GetModuleHandleW(L"MenuSystem.dll");
    if (!mnu) return {};  // not loaded yet: decide on a later read
    state = -1;
    if (!is_expected_build(GetModuleHandleW(nullptr), kExeTimestamp, kExeImageSize, "StrikeFighters2.exe") ||
        !is_expected_build(mnu, kMnuTimestamp, kMnuImageSize, "MenuSystem.dll")) {
        LOG("lobby: not available (unexpected build); the Multiplayer button stays hidden");
        return path;
    }
    HMODULE self = nullptr;
    wchar_t buf[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&lobby_menu_code_path), &self) ||
        GetModuleFileNameW(self, buf, MAX_PATH) >= MAX_PATH) {
        LOG("lobby: not available (mod DLL path unknown); the Multiplayer button stays hidden");
        return path;
    }
    path = buf;
    state = 1;
    LOG("lobby: available, menu code %ls", path.c_str());
    return path;
}

}  // namespace LazyWorks::SF2Patch::hooks

using namespace LazyWorks::SF2Patch::hooks;

/// <summary>Menu-code entry point: the menu system calls it on the lobby screen's first frame.</summary>
extern "C" __declspec(dllexport) void __cdecl InitMenuCode(void* sys, void** handler) {
    if (handler) *handler = nullptr;
    if (!sys || !handler) return;
    g_sys = sys;
    g_mnu = nullptr;
    reinterpret_cast<Service_t>(vtable_slot(sys, kSysGetService))(sys, nullptr, L"MNU", &g_mnu);
    HMODULE mnu_dll = GetModuleHandleW(L"MenuSystem.dll");
    if (!g_mnu || !mnu_dll || *static_cast<void**>(g_mnu) != at_rva(mnu_dll, kMnuVtableRva)) {
        LOG("lobby: menu service not as expected; the lobby screen is inert");
        if (g_mnu) reinterpret_cast<Service_t>(vtable_slot(sys, kSysReleaseService))(sys, nullptr, L"MNU", &g_mnu);
        g_mnu = nullptr;
        return;
    }
    pin_this_module();
    g_screen = identify_screen();
    if (g_screen == Screen::Connect) {
        init_connect_screen();
    } else {
        init_session_screen();
    }
    *handler = &g_handler;
}

/// <summary>Menu-code exit: the menu system calls it when the lobby screen closes, before it frees the DLL reference.</summary>
extern "C" __declspec(dllexport) void __cdecl ExitMenuCode() {
    if (g_sys && g_mnu) reinterpret_cast<Service_t>(vtable_slot(g_sys, kSysReleaseService))(g_sys, nullptr, L"MNU", &g_mnu);
    g_mnu = nullptr;
    g_sys = nullptr;
    LOG("lobby: screen closed");
}
